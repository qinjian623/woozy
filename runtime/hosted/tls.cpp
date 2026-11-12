#include "tls.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <openssl/sslerr.h>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>

#if !defined(MSG_NOSIGNAL)
#error "hosted TLS requires MSG_NOSIGNAL for SIGPIPE-safe writes"
#endif

enum class Role : int32_t { client = 1, server = 2 };

// The public types are opaque, but the implementation owns these definitions.
// Keeping the C tags here (instead of in the header) freezes only the pointer
// ABI and avoids exposing std::atomic to C callers.
struct LangTlsContext {
  std::atomic<uint64_t> refs;
  SSL_CTX *ssl_ctx;
  Role role;

  LangTlsContext(SSL_CTX *context, Role context_role) noexcept
      : refs(1), ssl_ctx(context), role(context_role) {}
};

struct LangTlsStream {
  std::atomic<uint64_t> refs;
  LangTlsContext *context;
  SSL *ssl;

  LangTlsStream(LangTlsContext *owner, SSL *session) noexcept
      : refs(1), context(owner), ssl(session) {}
};

namespace {

enum class Operation { handshake, read, write, shutdown };

constexpr uint64_t kMaxHostnameBytes = 255;
constexpr int kMaxBioIndex = BIO_TYPE_MASK;
void clear_error(LangTlsError *error) noexcept {
  if (error != nullptr) std::memset(error, 0, sizeof(*error));
  ERR_clear_error();
}

void record_error(LangTlsError *error, LangTlsErrorCategory category,
                  int system_errno, int ssl_error, SSL *ssl) noexcept {
  const unsigned long last = ERR_peek_last_error();
  uint64_t count = 0;
  while (ERR_get_error() != 0) ++count;

  if (error == nullptr) return;
  error->category = static_cast<int32_t>(category);
  error->system_errno = system_errno;
  error->ssl_error = ssl_error;
  error->reserved = 0;
  error->verify_result =
      ssl == nullptr ? X509_V_OK : static_cast<int64_t>(SSL_get_verify_result(ssl));
  error->openssl_error = static_cast<uint64_t>(last);
  error->openssl_error_count = count;
}

void invalid_argument(LangTlsError *error) noexcept {
  record_error(error, LANG_TLS_ERROR_INVALID_ARGUMENT, 0, 0, nullptr);
}

bool copy_text(const uint8_t *data, uint64_t length, uint64_t maximum,
               std::string *out) {
  if (data == nullptr || length == 0 || length > maximum) return false;
  if (length > static_cast<uint64_t>(SIZE_MAX)) return false;
  for (uint64_t index = 0; index < length; ++index) {
    if (data[index] == 0) return false;
  }
  out->assign(reinterpret_cast<const char *>(data),
              static_cast<size_t>(length));
  return true;
}

bool copy_path(const uint8_t *data, uint64_t length, std::string *out) {
  if (length > static_cast<uint64_t>(SIZE_MAX) - 1) return false;
  return copy_text(data, length, static_cast<uint64_t>(SIZE_MAX) - 1, out);
}

int bio_descriptor(BIO *bio) noexcept {
  return static_cast<int>(
      reinterpret_cast<intptr_t>(BIO_get_data(bio)));
}

int bio_create(BIO *bio) noexcept {
  if (bio == nullptr) return 0;
  BIO_set_init(bio, 1);
  BIO_set_data(bio, nullptr);
  BIO_set_shutdown(bio, BIO_NOCLOSE);
  return 1;
}

int bio_destroy(BIO *bio) noexcept {
  if (bio == nullptr) return 0;
  BIO_set_data(bio, nullptr);
  BIO_set_init(bio, 0);
  BIO_set_shutdown(bio, BIO_NOCLOSE);
  return 1;
}

int bio_write(BIO *bio, const char *data, int length) noexcept {
  if (bio == nullptr || length < 0) {
    errno = EINVAL;
    return -1;
  }
  BIO_clear_retry_flags(bio);
  if (length == 0) return 0;

  const int fd = bio_descriptor(bio);
  for (;;) {
    const ssize_t written = ::send(fd, data, static_cast<size_t>(length),
                                   MSG_NOSIGNAL);
    if (written >= 0) return static_cast<int>(written);
    if (errno == EINTR) {
      BIO_set_retry_write(bio);
      return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      BIO_set_retry_write(bio);
      return -1;
    }
    return -1;
  }
}

int bio_read(BIO *bio, char *data, int length) noexcept {
  if (bio == nullptr || length < 0) {
    errno = EINVAL;
    return -1;
  }
  BIO_clear_retry_flags(bio);
  if (length == 0) return 0;

  const int fd = bio_descriptor(bio);
  for (;;) {
    const ssize_t received =
        ::recv(fd, data, static_cast<size_t>(length), 0);
    if (received > 0) return static_cast<int>(received);
    if (received == 0) {
      BIO_set_flags(bio, BIO_FLAGS_IN_EOF);
      return 0;
    }
    if (errno == EINTR) {
      BIO_set_retry_read(bio);
      return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      BIO_set_retry_read(bio);
      return -1;
    }
    return -1;
  }
}

long bio_ctrl(BIO *bio, int command, long number, void *pointer) noexcept {
  switch (command) {
  case BIO_CTRL_FLUSH:
    return 1;
  case BIO_CTRL_EOF:
    return BIO_test_flags(bio, BIO_FLAGS_IN_EOF) ? 1 : 0;
  case BIO_CTRL_PENDING:
  case BIO_CTRL_WPENDING:
    return 0;
  case BIO_CTRL_GET_CLOSE:
    return BIO_get_shutdown(bio);
  case BIO_CTRL_SET_CLOSE:
    BIO_set_shutdown(bio, static_cast<int>(number));
    return 1;
  case BIO_C_GET_FD: {
    const int fd = bio_descriptor(bio);
    if (pointer != nullptr) *static_cast<int *>(pointer) = fd;
    return fd;
  }
  case BIO_CTRL_DUP:
    return 1;
  default:
    return 0;
  }
}

BIO_METHOD *create_bio_method() noexcept {
  const int index = BIO_get_new_index();
  if (index < BIO_TYPE_START || index > kMaxBioIndex) return nullptr;
  BIO_METHOD *method =
      BIO_meth_new(index | BIO_TYPE_SOURCE_SINK, "woozy-hosted-tls-socket");
  if (method == nullptr) return nullptr;

  const bool ok =
      BIO_meth_set_write(method, bio_write) == 1 &&
      BIO_meth_set_read(method, bio_read) == 1 &&
      BIO_meth_set_ctrl(method, bio_ctrl) == 1 &&
      BIO_meth_set_create(method, bio_create) == 1 &&
      BIO_meth_set_destroy(method, bio_destroy) == 1;
  if (!ok) {
    BIO_meth_free(method);
    return nullptr;
  }
  return method;
}

BIO_METHOD *socket_bio_method() noexcept {
  // BIO_get_new_index() consumes a process-wide index (OpenSSL exposes only
  // 127 custom indices).  Allocate one method for the process instead of one
  // per stream, otherwise the 128th stream would fail.  The tiny method object
  // intentionally has process lifetime; it is not an OpenSSL global cleanup.
  static BIO_METHOD *const method = create_bio_method();
  return method;
}

bool parse_ip(const std::string &name, unsigned char address[16],
              int *length) noexcept {
  if (::inet_pton(AF_INET, name.c_str(), address) == 1) {
    *length = 4;
    return true;
  }
  if (::inet_pton(AF_INET6, name.c_str(), address) == 1) {
    *length = 16;
    return true;
  }
  return false;
}

LangTlsStatus classify_ssl_failure(Operation operation, SSL *ssl, int result,
                                   int saved_errno,
                                   LangTlsError *error) noexcept {
  const int ssl_error = SSL_get_error(ssl, result);
  const int verify = SSL_get_verify_result(ssl);

  if (verify != X509_V_OK) {
    record_error(error, LANG_TLS_ERROR_CERTIFICATE,
                 ssl_error == SSL_ERROR_SYSCALL ? saved_errno : 0, ssl_error,
                 ssl);
    return LANG_TLS_STATUS_ERROR;
  }

  switch (ssl_error) {
  case SSL_ERROR_WANT_READ:
    return LANG_TLS_STATUS_WANT_READ;
  case SSL_ERROR_WANT_WRITE:
    return LANG_TLS_STATUS_WANT_WRITE;
  case SSL_ERROR_ZERO_RETURN:
    if (operation == Operation::read) return LANG_TLS_STATUS_EOF;
    record_error(error,
                 operation == Operation::shutdown ? LANG_TLS_ERROR_SHUTDOWN
                                                  : LANG_TLS_ERROR_PROTOCOL,
                 0, ssl_error, ssl);
    return LANG_TLS_STATUS_ERROR;
  case SSL_ERROR_SYSCALL:
    if (operation == Operation::read && result == 0 && saved_errno == 0) {
      record_error(error, LANG_TLS_ERROR_UNEXPECTED_EOF, 0, ssl_error, ssl);
    } else {
      record_error(error, LANG_TLS_ERROR_IO, saved_errno, ssl_error, ssl);
    }
    return LANG_TLS_STATUS_ERROR;
  case SSL_ERROR_SSL: {
    const unsigned long last = ERR_peek_last_error();
    if (operation == Operation::read && last != 0 &&
        ERR_GET_REASON(last) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
      record_error(error, LANG_TLS_ERROR_UNEXPECTED_EOF, 0, ssl_error, ssl);
    } else if (operation == Operation::handshake) {
      record_error(error, LANG_TLS_ERROR_HANDSHAKE, 0, ssl_error, ssl);
    } else if (operation == Operation::shutdown) {
      record_error(error, LANG_TLS_ERROR_SHUTDOWN, 0, ssl_error, ssl);
    } else {
      record_error(error, LANG_TLS_ERROR_PROTOCOL, 0, ssl_error, ssl);
    }
    return LANG_TLS_STATUS_ERROR;
  }
  case SSL_ERROR_WANT_X509_LOOKUP:
  case SSL_ERROR_WANT_ASYNC:
  case SSL_ERROR_WANT_CONNECT:
  case SSL_ERROR_WANT_ACCEPT:
  default:
    record_error(error, LANG_TLS_ERROR_INTERNAL, 0, ssl_error, ssl);
    return LANG_TLS_STATUS_ERROR;
  }
}

LangTlsContext *make_context(Role role, const uint8_t *first_path,
                             uint64_t first_length, const uint8_t *second_path,
                             uint64_t second_length, bool default_trust,
                             LangTlsError *error) {
  clear_error(error);

  std::string first;
  std::string second;
  if (role == Role::client && !default_trust) {
    if (!copy_path(first_path, first_length, &first)) {
      invalid_argument(error);
      return nullptr;
    }
  } else if (role == Role::server) {
    if (!copy_path(first_path, first_length, &first) ||
        !copy_path(second_path, second_length, &second)) {
      invalid_argument(error);
      return nullptr;
    }
  }

  SSL_CTX *raw = SSL_CTX_new(role == Role::client ? TLS_client_method()
                                                  : TLS_server_method());
  if (raw == nullptr) {
    record_error(error, LANG_TLS_ERROR_CONTEXT, errno, 0, nullptr);
    return nullptr;
  }

  const auto fail = [&](LangTlsErrorCategory category) -> LangTlsContext * {
    record_error(error, category, errno, 0, nullptr);
    SSL_CTX_free(raw);
    return nullptr;
  };

  if (SSL_CTX_set_min_proto_version(raw, TLS1_2_VERSION) != 1)
    return fail(LANG_TLS_ERROR_CONTEXT);

  const long modes =
      SSL_CTX_set_mode(raw, SSL_MODE_ENABLE_PARTIAL_WRITE |
                                SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if ((modes & SSL_MODE_ENABLE_PARTIAL_WRITE) == 0)
    return fail(LANG_TLS_ERROR_CONTEXT);

  if (role == Role::client) {
    SSL_CTX_set_verify(raw, SSL_VERIFY_PEER, nullptr);
    if (default_trust) {
      if (SSL_CTX_set_default_verify_paths(raw) != 1)
        return fail(LANG_TLS_ERROR_CONTEXT);
    } else {
      if (SSL_CTX_load_verify_locations(raw, first.c_str(), nullptr) != 1)
        return fail(LANG_TLS_ERROR_CERTIFICATE);
    }
  } else {
    SSL_CTX_set_verify(raw, SSL_VERIFY_NONE, nullptr);
    if (SSL_CTX_use_certificate_chain_file(raw, first.c_str()) != 1)
      return fail(LANG_TLS_ERROR_CERTIFICATE);
    if (SSL_CTX_use_PrivateKey_file(raw, second.c_str(),
                                    SSL_FILETYPE_PEM) != 1)
      return fail(LANG_TLS_ERROR_CERTIFICATE);
    if (SSL_CTX_check_private_key(raw) != 1)
      return fail(LANG_TLS_ERROR_CERTIFICATE);
  }

  auto *context =
      new (std::nothrow) LangTlsContext(raw, role);
  if (context == nullptr) {
    record_error(error, LANG_TLS_ERROR_INTERNAL, 0, 0, nullptr);
    SSL_CTX_free(raw);
    return nullptr;
  }
  return reinterpret_cast<LangTlsContext *>(context);
}

LangTlsStream *make_client_stream(LangTlsContext *context, int32_t fd,
                                  LangTlsNameKind name_kind,
                                  const uint8_t *name, uint64_t name_length,
                                  LangTlsError *error) {
  clear_error(error);
  if (context == nullptr || fd < 0 || name == nullptr || name_length == 0 ||
      name_length > kMaxHostnameBytes) {
    invalid_argument(error);
    return nullptr;
  }

  std::string text;
  if (!copy_text(name, name_length, kMaxHostnameBytes, &text)) {
    invalid_argument(error);
    return nullptr;
  }
  if (name_kind != LANG_TLS_NAME_AUTO && name_kind != LANG_TLS_NAME_DNS &&
      name_kind != LANG_TLS_NAME_IP) {
    invalid_argument(error);
    return nullptr;
  }
  // Bracketed authority syntax is a URL-layer concern, not a DNS/IP identity.
  if ((!text.empty() && text.front() == '[') ||
      (!text.empty() && text.back() == ']')) {
    invalid_argument(error);
    return nullptr;
  }

  auto *impl = reinterpret_cast<LangTlsContext *>(context);
  if (impl->role != Role::client) {
    invalid_argument(error);
    return nullptr;
  }

  unsigned char address[16] = {};
  int address_length = 0;
  const bool ip_literal = parse_ip(text, address, &address_length);
  if (name_kind == LANG_TLS_NAME_IP && !ip_literal) {
    invalid_argument(error);
    return nullptr;
  }
  if (name_kind == LANG_TLS_NAME_DNS && ip_literal) {
    // IP identities must use IP verification, never DNS matching plus SNI.
    invalid_argument(error);
    return nullptr;
  }
  if (!ip_literal &&
      (text.find(':') != std::string::npos ||
       text.find('%') != std::string::npos)) {
    // Reject authority/port/zone syntax instead of sending it as SNI.
    invalid_argument(error);
    return nullptr;
  }
  const bool use_ip = ip_literal;

  SSL *ssl = SSL_new(impl->ssl_ctx);
  if (ssl == nullptr) {
    record_error(error, LANG_TLS_ERROR_CONTEXT, errno, 0, nullptr);
    return nullptr;
  }

  const auto fail = [&](LangTlsErrorCategory category) -> LangTlsStream * {
    record_error(error, category, errno, 0, ssl);
    SSL_free(ssl);
    return nullptr;
  };

  SSL_set_connect_state(ssl);

  if (use_ip) {
    X509_VERIFY_PARAM *parameter = SSL_get0_param(ssl);
    if (parameter == nullptr ||
        X509_VERIFY_PARAM_set1_ip(parameter, address,
                                  static_cast<size_t>(address_length)) != 1)
      return fail(LANG_TLS_ERROR_CONTEXT);
  } else {
    if (SSL_set1_host(ssl, text.c_str()) != 1)
      return fail(LANG_TLS_ERROR_CONTEXT);
    if (SSL_set_tlsext_host_name(ssl, text.c_str()) != 1)
      return fail(LANG_TLS_ERROR_CONTEXT);
  }

  BIO_METHOD *method = socket_bio_method();
  if (method == nullptr) return fail(LANG_TLS_ERROR_INTERNAL);
  BIO *bio = BIO_new(method);
  if (bio == nullptr) return fail(LANG_TLS_ERROR_INTERNAL);
  BIO_set_data(bio, reinterpret_cast<void *>(static_cast<intptr_t>(fd)));
  BIO_set_init(bio, 1);
  BIO_set_shutdown(bio, BIO_NOCLOSE);

  SSL_set_bio(ssl, bio, bio);

  auto *stream = new (std::nothrow) LangTlsStream(context, ssl);
  if (stream == nullptr) {
    record_error(error, LANG_TLS_ERROR_INTERNAL, 0, 0, ssl);
    SSL_free(ssl); // also frees bio
    return nullptr;
  }

  lang_tls_context_retain_v1(context);
  return reinterpret_cast<LangTlsStream *>(stream);
}

LangTlsStream *make_server_stream(LangTlsContext *context, int32_t fd,
                                  LangTlsError *error) {
  clear_error(error);
  if (context == nullptr || fd < 0) {
    invalid_argument(error);
    return nullptr;
  }

  auto *impl = reinterpret_cast<LangTlsContext *>(context);
  if (impl->role != Role::server) {
    invalid_argument(error);
    return nullptr;
  }

  SSL *ssl = SSL_new(impl->ssl_ctx);
  if (ssl == nullptr) {
    record_error(error, LANG_TLS_ERROR_CONTEXT, errno, 0, nullptr);
    return nullptr;
  }
  SSL_set_accept_state(ssl);

  const auto fail = [&](LangTlsErrorCategory category) -> LangTlsStream * {
    record_error(error, category, errno, 0, ssl);
    SSL_free(ssl);
    return nullptr;
  };

  BIO_METHOD *method = socket_bio_method();
  if (method == nullptr) return fail(LANG_TLS_ERROR_INTERNAL);
  BIO *bio = BIO_new(method);
  if (bio == nullptr) return fail(LANG_TLS_ERROR_INTERNAL);
  BIO_set_data(bio, reinterpret_cast<void *>(static_cast<intptr_t>(fd)));
  BIO_set_init(bio, 1);
  BIO_set_shutdown(bio, BIO_NOCLOSE);

  SSL_set_bio(ssl, bio, bio);

  auto *stream = new (std::nothrow) LangTlsStream(context, ssl);
  if (stream == nullptr) {
    record_error(error, LANG_TLS_ERROR_INTERNAL, 0, 0, ssl);
    SSL_free(ssl);
    return nullptr;
  }

  lang_tls_context_retain_v1(context);
  return reinterpret_cast<LangTlsStream *>(stream);
}

LangTlsStatus handshake_impl(LangTlsStream *stream,
                             LangTlsError *error) noexcept {
  clear_error(error);
  if (stream == nullptr) {
    invalid_argument(error);
    return LANG_TLS_STATUS_ERROR;
  }
  auto *impl = reinterpret_cast<LangTlsStream *>(stream);
  if (SSL_is_init_finished(impl->ssl) == 1) return LANG_TLS_STATUS_OK;

  errno = 0;
  const int result = SSL_is_server(impl->ssl) == 1
                         ? SSL_accept(impl->ssl)
                         : SSL_connect(impl->ssl);
  if (result == 1) return LANG_TLS_STATUS_OK;
  return classify_ssl_failure(Operation::handshake, impl->ssl, result, errno,
                              error);
}

LangTlsStatus read_impl(LangTlsStream *stream, void *data, uint64_t capacity,
                        uint64_t *out_count, LangTlsError *error) noexcept {
  clear_error(error);
  if (out_count != nullptr) *out_count = 0;
  if (stream == nullptr || out_count == nullptr ||
      (capacity != 0 && data == nullptr) ||
      capacity > static_cast<uint64_t>(SIZE_MAX)) {
    invalid_argument(error);
    return LANG_TLS_STATUS_ERROR;
  }
  if (capacity == 0) return LANG_TLS_STATUS_OK;

  auto *impl = reinterpret_cast<LangTlsStream *>(stream);
  size_t received = 0;
  errno = 0;
  const int result = SSL_read_ex(impl->ssl, data, static_cast<size_t>(capacity),
                                 &received);
  if (result == 1) {
    if (received == 0) {
      if ((SSL_get_shutdown(impl->ssl) & SSL_RECEIVED_SHUTDOWN) != 0)
        return LANG_TLS_STATUS_EOF;
      record_error(error, LANG_TLS_ERROR_PROTOCOL, 0, SSL_ERROR_NONE,
                   impl->ssl);
      return LANG_TLS_STATUS_ERROR;
    }
    *out_count = static_cast<uint64_t>(received);
    return LANG_TLS_STATUS_OK;
  }
  return classify_ssl_failure(Operation::read, impl->ssl, result, errno, error);
}

LangTlsStatus write_impl(LangTlsStream *stream, const void *data,
                         uint64_t length, uint64_t *out_count,
                         LangTlsError *error) noexcept {
  clear_error(error);
  if (out_count != nullptr) *out_count = 0;
  if (stream == nullptr || out_count == nullptr ||
      (length != 0 && data == nullptr) ||
      length > static_cast<uint64_t>(SIZE_MAX)) {
    invalid_argument(error);
    return LANG_TLS_STATUS_ERROR;
  }
  if (length == 0) return LANG_TLS_STATUS_OK;

  auto *impl = reinterpret_cast<LangTlsStream *>(stream);
  size_t written = 0;
  errno = 0;
  const int result = SSL_write_ex(impl->ssl, data, static_cast<size_t>(length),
                                  &written);
  if (result == 1) {
    *out_count = static_cast<uint64_t>(written);
    return LANG_TLS_STATUS_OK;
  }
  return classify_ssl_failure(Operation::write, impl->ssl, result, errno,
                              error);
}

LangTlsStatus shutdown_impl(LangTlsStream *stream,
                            LangTlsError *error) noexcept {
  clear_error(error);
  if (stream == nullptr) {
    invalid_argument(error);
    return LANG_TLS_STATUS_ERROR;
  }
  auto *impl = reinterpret_cast<LangTlsStream *>(stream);
  errno = 0;
  const int result = SSL_shutdown(impl->ssl);
  if (result == 1) return LANG_TLS_STATUS_OK;
  if (result == 0) {
    // Our close_notify has been sent; the peer's close_notify is still owed.
    return LANG_TLS_STATUS_WANT_READ;
  }
  return classify_ssl_failure(Operation::shutdown, impl->ssl, result, errno,
                              error);
}

} // namespace

extern "C" {

LangTlsContext *lang_tls_context_client_default_v1(
    LangTlsError *error) noexcept {
  try {
    return make_context(Role::client, nullptr, 0, nullptr, 0, true, error);
  } catch (...) {
    record_error(error, LANG_TLS_ERROR_INTERNAL, 0, 0, nullptr);
    return nullptr;
  }
}

LangTlsContext *lang_tls_context_client_v1(const uint8_t *ca_file,
                                           uint64_t ca_file_length,
                                           LangTlsError *error) noexcept {
  try {
    return make_context(Role::client, ca_file, ca_file_length, nullptr, 0,
                        false, error);
  } catch (...) {
    record_error(error, LANG_TLS_ERROR_INTERNAL, 0, 0, nullptr);
    return nullptr;
  }
}

LangTlsContext *lang_tls_context_server_v1(
    const uint8_t *certificate_chain_file, uint64_t certificate_chain_length,
    const uint8_t *private_key_file, uint64_t private_key_length,
    LangTlsError *error) noexcept {
  try {
    return make_context(Role::server, certificate_chain_file,
                        certificate_chain_length, private_key_file,
                        private_key_length, false, error);
  } catch (...) {
    record_error(error, LANG_TLS_ERROR_INTERNAL, 0, 0, nullptr);
    return nullptr;
  }
}

void lang_tls_context_retain_v1(LangTlsContext *context) noexcept {
  if (context == nullptr) return;
  reinterpret_cast<LangTlsContext *>(context)->refs.fetch_add(
      1, std::memory_order_relaxed);
}

void lang_tls_context_release_v1(LangTlsContext *context) noexcept {
  if (context == nullptr) return;
  auto *impl = reinterpret_cast<LangTlsContext *>(context);
  if (impl->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
  SSL_CTX_free(impl->ssl_ctx);
  delete impl;
}

LangTlsStream *lang_tls_stream_client_v1(LangTlsContext *context, int32_t fd,
                                         LangTlsNameKind name_kind,
                                         const uint8_t *name,
                                         uint64_t name_length,
                                         LangTlsError *error) noexcept {
  try {
    return make_client_stream(context, fd, name_kind, name, name_length,
                              error);
  } catch (...) {
    record_error(error, LANG_TLS_ERROR_INTERNAL, 0, 0, nullptr);
    return nullptr;
  }
}

LangTlsStream *lang_tls_stream_server_v1(LangTlsContext *context, int32_t fd,
                                         LangTlsError *error) noexcept {
  try {
    return make_server_stream(context, fd, error);
  } catch (...) {
    record_error(error, LANG_TLS_ERROR_INTERNAL, 0, 0, nullptr);
    return nullptr;
  }
}

void lang_tls_stream_retain_v1(LangTlsStream *stream) noexcept {
  if (stream == nullptr) return;
  reinterpret_cast<LangTlsStream *>(stream)->refs.fetch_add(
      1, std::memory_order_relaxed);
}

void lang_tls_stream_release_v1(LangTlsStream *stream) noexcept {
  if (stream == nullptr) return;
  auto *impl = reinterpret_cast<LangTlsStream *>(stream);
  if (impl->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;

  LangTlsContext *context = impl->context;
  SSL *ssl = impl->ssl;
  delete impl;
  SSL_free(ssl); // Frees the BIO; its destroy callback never closes the fd.
  lang_tls_context_release_v1(context);
}

LangTlsStatus lang_tls_handshake_v1(LangTlsStream *stream,
                                    LangTlsError *error) noexcept {
  return handshake_impl(stream, error);
}

LangTlsStatus lang_tls_read_v1(LangTlsStream *stream, void *data,
                               uint64_t capacity, uint64_t *out_count,
                               LangTlsError *error) noexcept {
  return read_impl(stream, data, capacity, out_count, error);
}

LangTlsStatus lang_tls_write_v1(LangTlsStream *stream, const void *data,
                                uint64_t length, uint64_t *out_count,
                                LangTlsError *error) noexcept {
  return write_impl(stream, data, length, out_count, error);
}

LangTlsStatus lang_tls_shutdown_v1(LangTlsStream *stream,
                                   LangTlsError *error) noexcept {
  return shutdown_impl(stream, error);
}

} // extern "C"
