#include "tls_source.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <mutex>
#include <pthread.h>
#include <unordered_map>

namespace {

struct ErrorStorage {
  std::atomic<uint64_t> refs{1};
  LangTlsError error{};
};

struct CountStorage {
  std::atomic<uint64_t> refs{1};
  uint64_t value{0};
};

struct ByteStorage {
  std::atomic<uint64_t> refs{1};
  uint8_t *data{nullptr};
  uint64_t capacity{0};
  uint64_t length{0};
  int32_t failed{0};
};

uint64_t add_reference(std::atomic<uint64_t> &refs) noexcept {
  return refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

bool drop_reference(std::atomic<uint64_t> &refs) noexcept {
  return refs.fetch_sub(1, std::memory_order_acq_rel) == 1;
}

ErrorStorage *storage(WtsError *value) noexcept {
  return reinterpret_cast<ErrorStorage *>(value);
}

const ErrorStorage *storage(const WtsError *value) noexcept {
  return reinterpret_cast<const ErrorStorage *>(value);
}

CountStorage *storage(WtsCount *value) noexcept {
  return reinterpret_cast<CountStorage *>(value);
}

const CountStorage *storage(const WtsCount *value) noexcept {
  return reinterpret_cast<const CountStorage *>(value);
}

ByteStorage *storage(WtsBytes *value) noexcept {
  return reinterpret_cast<ByteStorage *>(value);
}

const ByteStorage *storage(const WtsBytes *value) noexcept {
  return reinterpret_cast<const ByteStorage *>(value);
}

LangTlsError *error_out(WtsError *value) noexcept {
  return value == nullptr ? nullptr : &storage(value)->error;
}

} // namespace

// The public opaque tags intentionally have no C++ layout.  The wrappers keep
// their own private layout and only expose pointers through this C ABI.
struct WtsStream {
  std::atomic<uint64_t> refs{1};
  LangTlsStream *low{nullptr};
  int32_t fd{-1};
  uint64_t token{0};
};

namespace {
// The public token is a borrowed ID; it must never be interpreted as an address.
struct TokenMutex {
  std::atomic_flag held = ATOMIC_FLAG_INIT;
  void lock() noexcept {
    while (held.test_and_set(std::memory_order_acquire)) {
#if defined(__aarch64__)
      __asm__ volatile("yield" ::: "memory");
#else
      __asm__ volatile("pause" ::: "memory");
#endif
    }
  }
  void unlock() noexcept { held.clear(std::memory_order_release); }
};
TokenMutex token_mutex;
std::unordered_map<uint64_t, WtsStream *> streams;
uint64_t next_token = 1;
pthread_once_t token_once = PTHREAD_ONCE_INIT;
bool token_hooks_ready = false;

void token_fork_prepare() { token_mutex.lock(); }
void token_fork_done() { token_mutex.unlock(); }
void token_initialize() {
  token_hooks_ready = pthread_atfork(token_fork_prepare, token_fork_done,
                                   token_fork_done) == 0;
}
bool token_ready() noexcept {
  return pthread_once(&token_once, token_initialize) == 0 && token_hooks_ready;
}

bool register_stream(WtsStream *stream) noexcept {
  if (!token_ready()) return false;
  try {
    std::lock_guard<TokenMutex> lock(token_mutex);
    if (next_token == 0) return false; // Never recycle an expired ID.
    streams.emplace(next_token, stream);
    stream->token = next_token++;
    return true;
  } catch (...) {
    return false;
  }
}

WtsStream *acquire_stream(uint64_t token) noexcept {
  if (!token_ready()) return nullptr;
  std::lock_guard<TokenMutex> lock(token_mutex);
  const auto found = streams.find(token);
  if (found == streams.end()) return nullptr;
  add_reference(found->second->refs);
  return found->second;
}

int32_t invalid_token(WtsCount *count, WtsError *error) noexcept {
  if (count) storage(count)->value = 0;
  if (error) {
    storage(error)->error = LangTlsError{};
    storage(error)->error.category = LANG_TLS_ERROR_INVALID_ARGUMENT;
  }
  return LANG_TLS_STATUS_ERROR;
}
} // namespace

extern "C" {

WtsError *wts_error_new(void) noexcept {
  auto *value = new (std::nothrow) ErrorStorage();
  return reinterpret_cast<WtsError *>(value);
}

void wts_error_retain(WtsError *value) noexcept {
  if (value != nullptr) add_reference(storage(value)->refs);
}

void wts_error_release(WtsError *value) noexcept {
  if (value == nullptr) return;
  auto *impl = storage(value);
  if (!drop_reference(impl->refs)) return;
  delete impl;
}

void wts_error_reset(WtsError *value) noexcept {
  if (value == nullptr) return;
  std::memset(&storage(value)->error, 0, sizeof(LangTlsError));
}

int32_t wts_error_is_null(const WtsError *value) noexcept {
  return value == nullptr ? 1 : 0;
}

int32_t wts_error_category(const WtsError *value) noexcept {
  return value == nullptr ? 0 : storage(value)->error.category;
}

int32_t wts_error_system_errno(const WtsError *value) noexcept {
  return value == nullptr ? 0 : storage(value)->error.system_errno;
}

int32_t wts_error_ssl_error(const WtsError *value) noexcept {
  return value == nullptr ? 0 : storage(value)->error.ssl_error;
}

int64_t wts_error_verify_result(const WtsError *value) noexcept {
  return value == nullptr ? 0 : storage(value)->error.verify_result;
}

uint64_t wts_error_openssl_error(const WtsError *value) noexcept {
  return value == nullptr ? 0 : storage(value)->error.openssl_error;
}

uint64_t wts_error_openssl_error_count(const WtsError *value) noexcept {
  return value == nullptr ? 0 : storage(value)->error.openssl_error_count;
}

WtsCount *wts_count_new(void) noexcept {
  auto *value = new (std::nothrow) CountStorage();
  return reinterpret_cast<WtsCount *>(value);
}

void wts_count_retain(WtsCount *value) noexcept {
  if (value != nullptr) add_reference(storage(value)->refs);
}

void wts_count_release(WtsCount *value) noexcept {
  if (value == nullptr) return;
  auto *impl = storage(value);
  if (!drop_reference(impl->refs)) return;
  delete impl;
}

uint64_t wts_count_value(const WtsCount *value) noexcept {
  return value == nullptr ? 0 : storage(value)->value;
}

void wts_count_set(WtsCount *value, uint64_t count) noexcept {
  if (value != nullptr) storage(value)->value = count;
}

int32_t wts_count_is_null(const WtsCount *value) noexcept {
  return value == nullptr ? 1 : 0;
}

WtsBytes *wts_bytes_new(uint64_t capacity) noexcept {
  auto *value = new (std::nothrow) ByteStorage();
  if (value == nullptr) return nullptr;
  if (capacity != 0) {
    if (capacity > static_cast<uint64_t>(SIZE_MAX)) {
      value->failed = 1;
      return reinterpret_cast<WtsBytes *>(value);
    }
    value->data = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(capacity)));
    if (value->data == nullptr) {
      value->failed = 1;
      return reinterpret_cast<WtsBytes *>(value);
    }
    value->capacity = capacity;
  }
  return reinterpret_cast<WtsBytes *>(value);
}

void wts_bytes_retain(WtsBytes *value) noexcept {
  if (value != nullptr) add_reference(storage(value)->refs);
}

void wts_bytes_release(WtsBytes *value) noexcept {
  if (value == nullptr) return;
  auto *impl = storage(value);
  if (!drop_reference(impl->refs)) return;
  std::free(impl->data);
  delete impl;
}

uint64_t wts_bytes_capacity(const WtsBytes *value) noexcept {
  return value == nullptr ? 0 : storage(value)->capacity;
}

uint64_t wts_bytes_length(const WtsBytes *value) noexcept {
  return value == nullptr ? 0 : storage(value)->length;
}

void wts_bytes_set_length(WtsBytes *value, uint64_t length) noexcept {
  if (value == nullptr) return;
  auto *impl = storage(value);
  if (length > impl->capacity) {
    impl->failed = 1;
    impl->length = impl->capacity;
    return;
  }
  impl->length = length;
}

int32_t wts_bytes_failed(const WtsBytes *value) noexcept {
  return value == nullptr ? 1 : storage(value)->failed;
}

uint8_t *wts_bytes_data_mut(WtsBytes *value) noexcept {
  return value == nullptr ? nullptr : storage(value)->data;
}

const uint8_t *wts_bytes_data(const WtsBytes *value) noexcept {
  return value == nullptr ? nullptr : storage(value)->data;
}

int32_t wts_bytes_resize(WtsBytes *value, uint64_t capacity) noexcept {
  if (value == nullptr) return 1;
  auto *impl = storage(value);
  if (capacity > static_cast<uint64_t>(SIZE_MAX)) {
    impl->failed = 1;
    return 1;
  }
  if (capacity == impl->capacity) return impl->failed;
  if (capacity == 0) {
    std::free(impl->data);
    impl->data = nullptr;
    impl->capacity = 0;
    if (impl->length > 0) impl->length = 0;
    return impl->failed;
  }
  void *next = std::realloc(impl->data, static_cast<size_t>(capacity));
  if (next == nullptr) {
    impl->failed = 1;
    return 1;
  }
  impl->data = static_cast<uint8_t *>(next);
  impl->capacity = capacity;
  if (impl->length > capacity) impl->length = capacity;
  return impl->failed;
}

WtsContext *wts_context_client_default(WtsError *error) noexcept {
  return reinterpret_cast<WtsContext *>(
      lang_tls_context_client_default_v1(error_out(error)));
}

WtsContext *wts_context_client(WtsBytes *ca_file, WtsError *error) noexcept {
  return reinterpret_cast<WtsContext *>(lang_tls_context_client_v1(
      ca_file == nullptr ? nullptr : storage(ca_file)->data,
      ca_file == nullptr ? 0 : storage(ca_file)->length, error_out(error)));
}

WtsContext *wts_context_server(WtsBytes *certificate_chain,
                               WtsBytes *private_key,
                               WtsError *error) noexcept {
  return reinterpret_cast<WtsContext *>(lang_tls_context_server_v1(
      certificate_chain == nullptr ? nullptr : storage(certificate_chain)->data,
      certificate_chain == nullptr ? 0 : storage(certificate_chain)->length,
      private_key == nullptr ? nullptr : storage(private_key)->data,
      private_key == nullptr ? 0 : storage(private_key)->length,
      error_out(error)));
}

void wts_context_retain(WtsContext *context) noexcept {
  lang_tls_context_retain_v1(reinterpret_cast<LangTlsContext *>(context));
}

void wts_context_release(WtsContext *context) noexcept {
  lang_tls_context_release_v1(reinterpret_cast<LangTlsContext *>(context));
}

int32_t wts_context_is_null(const WtsContext *context) noexcept {
  return context == nullptr ? 1 : 0;
}

WtsStream *wts_stream_client(WtsContext *context, int32_t fd, int32_t name_kind,
                             WtsBytes *name, WtsError *error) noexcept {
  LangTlsStream *low = lang_tls_stream_client_v1(
      reinterpret_cast<LangTlsContext *>(context), fd,
      static_cast<LangTlsNameKind>(name_kind),
      name == nullptr ? nullptr : storage(name)->data,
      name == nullptr ? 0 : storage(name)->length, error_out(error));
  if (low == nullptr) return nullptr;
  auto *stream = new (std::nothrow) WtsStream();
  if (stream == nullptr) {
    lang_tls_stream_release_v1(low);
    return nullptr;
  }
  stream->low = low;
  stream->fd = fd;
  if (!register_stream(stream)) {
    delete stream;
    lang_tls_stream_release_v1(low);
    if (error) storage(error)->error.category = LANG_TLS_ERROR_INTERNAL;
    return nullptr;
  }
  return reinterpret_cast<WtsStream *>(stream);
}

WtsStream *wts_stream_server(WtsContext *context, int32_t fd,
                             WtsError *error) noexcept {
  LangTlsStream *low = lang_tls_stream_server_v1(
      reinterpret_cast<LangTlsContext *>(context), fd, error_out(error));
  if (low == nullptr) return nullptr;
  auto *stream = new (std::nothrow) WtsStream();
  if (stream == nullptr) {
    lang_tls_stream_release_v1(low);
    return nullptr;
  }
  stream->low = low;
  stream->fd = fd;
  if (!register_stream(stream)) {
    delete stream;
    lang_tls_stream_release_v1(low);
    if (error) storage(error)->error.category = LANG_TLS_ERROR_INTERNAL;
    return nullptr;
  }
  return reinterpret_cast<WtsStream *>(stream);
}

void wts_stream_retain(WtsStream *stream) noexcept {
  if (stream != nullptr) add_reference(stream->refs);
}

void wts_stream_release(WtsStream *stream) noexcept {
  if (stream == nullptr) return;
  {
    std::lock_guard<TokenMutex> lock(token_mutex);
    if (!drop_reference(stream->refs)) return;
    streams.erase(stream->token);
  }
  LangTlsStream *low = stream->low;
  stream->low = nullptr;
  delete stream;
  lang_tls_stream_release_v1(low);
}

int32_t wts_stream_is_null(const WtsStream *stream) noexcept {
  return stream == nullptr ? 1 : 0;
}

int32_t wts_stream_descriptor(const WtsStream *stream) noexcept {
  return stream == nullptr ? -1 : stream->fd;
}

uint64_t wts_stream_token(const WtsStream *stream) noexcept {
  return stream == nullptr ? 0 : stream->token;
}

int32_t wts_handshake(WtsStream *stream, WtsError *error) noexcept {
  if (stream == nullptr) return LANG_TLS_STATUS_ERROR;
  return static_cast<int32_t>(
      lang_tls_handshake_v1(stream->low, error_out(error)));
}

int32_t wts_read(WtsStream *stream, uint8_t *data, uint64_t capacity,
                 WtsCount *count, WtsError *error) noexcept {
  uint64_t ignored = 0;
  if (stream == nullptr) return LANG_TLS_STATUS_ERROR;
  return static_cast<int32_t>(lang_tls_read_v1(
      stream->low, data, capacity,
      count == nullptr ? &ignored : &storage(count)->value, error_out(error)));
}

int32_t wts_write(WtsStream *stream, const uint8_t *data, uint64_t length,
                  WtsCount *count, WtsError *error) noexcept {
  uint64_t ignored = 0;
  if (stream == nullptr) return LANG_TLS_STATUS_ERROR;
  return static_cast<int32_t>(lang_tls_write_v1(
      stream->low, data, length,
      count == nullptr ? &ignored : &storage(count)->value, error_out(error)));
}

int32_t wts_shutdown(WtsStream *stream, WtsError *error) noexcept {
  if (stream == nullptr) return LANG_TLS_STATUS_ERROR;
  return static_cast<int32_t>(
      lang_tls_shutdown_v1(stream->low, error_out(error)));
}

int32_t wts_transport_fd(uint64_t token) noexcept {
  auto *stream = acquire_stream(token);
  if (stream == nullptr) return -1;
  const auto fd = stream->fd;
  wts_stream_release(stream);
  return fd;
}

int32_t wts_transport_read(uint64_t token, uint8_t *data, uint64_t capacity,
                           WtsCount *count, WtsError *error) noexcept {
  auto *stream = acquire_stream(token);
  if (stream == nullptr) return invalid_token(count, error);
  const auto result = wts_read(stream, data, capacity, count, error);
  wts_stream_release(stream);
  return result;
}

int32_t wts_transport_write(uint64_t token, const uint8_t *data,
                            uint64_t length, WtsCount *count,
                            WtsError *error) noexcept {
  auto *stream = acquire_stream(token);
  if (stream == nullptr) return invalid_token(count, error);
  const auto result = wts_write(stream, data, length, count, error);
  wts_stream_release(stream);
  return result;
}

} // extern "C"
