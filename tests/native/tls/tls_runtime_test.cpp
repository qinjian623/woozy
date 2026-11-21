#include "../../../runtime/hosted/tls.hpp"

#include <openssl/x509_vfy.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kLargePayload = 4ULL * 1024ULL * 1024ULL;
constexpr int kPollMs = 15000;

[[noreturn]] void fail(const std::string &message) {
  throw std::runtime_error(message);
}

void need(bool condition, const std::string &message) {
  if (!condition) fail(message);
}

const char *category_name(int32_t category) noexcept {
  switch (category) {
  case LANG_TLS_ERROR_NONE:
    return "none";
  case LANG_TLS_ERROR_INVALID_ARGUMENT:
    return "invalid-argument";
  case LANG_TLS_ERROR_CONTEXT:
    return "context";
  case LANG_TLS_ERROR_CERTIFICATE:
    return "certificate";
  case LANG_TLS_ERROR_HANDSHAKE:
    return "handshake";
  case LANG_TLS_ERROR_PROTOCOL:
    return "protocol";
  case LANG_TLS_ERROR_IO:
    return "io";
  case LANG_TLS_ERROR_UNEXPECTED_EOF:
    return "unexpected-eof";
  case LANG_TLS_ERROR_SHUTDOWN:
    return "shutdown";
  case LANG_TLS_ERROR_INTERNAL:
    return "internal";
  default:
    return "unknown";
  }
}

std::string describe(const LangTlsError &error) {
  return std::string("category=") + category_name(error.category) +
         " errno=" + std::to_string(error.system_errno) +
         " ssl_error=" + std::to_string(error.ssl_error) +
         " verify=" + std::to_string(error.verify_result) +
         " openssl=" + std::to_string(error.openssl_error) +
         " errors=" + std::to_string(error.openssl_error_count);
}

void require_category(const LangTlsError &error, int32_t category,
                      const std::string &context) {
  need(error.category == category,
       context + " expected category " + category_name(category) + ", got " +
           describe(error));
}

void require_ok(const LangTlsError &error, const std::string &context) {
  require_category(error, LANG_TLS_ERROR_NONE, context);
}

void require_error_status(LangTlsStatus status, const LangTlsError &error,
                          const std::string &context) {
  need(status == LANG_TLS_STATUS_ERROR,
       context + " expected ERROR, got status " + std::to_string(status) +
           " (" + describe(error) + ")");
}

struct ContextRef {
  LangTlsContext *value = nullptr;

  ContextRef() = default;
  explicit ContextRef(LangTlsContext *context) noexcept : value(context) {}
  ContextRef(const ContextRef &) = delete;
  ContextRef &operator=(const ContextRef &) = delete;
  ~ContextRef() { lang_tls_context_release_v1(value); }

  LangTlsContext *get() const noexcept { return value; }
  void retain() noexcept { lang_tls_context_retain_v1(value); }
  void drop_one() noexcept { lang_tls_context_release_v1(value); }
  void release() noexcept {
    drop_one();
    value = nullptr;
  }
  void abandon() noexcept { value = nullptr; }
};

struct StreamRef {
  LangTlsStream *value = nullptr;

  StreamRef() = default;
  explicit StreamRef(LangTlsStream *stream) noexcept : value(stream) {}
  StreamRef(const StreamRef &) = delete;
  StreamRef &operator=(const StreamRef &) = delete;
  ~StreamRef() { lang_tls_stream_release_v1(value); }

  LangTlsStream *get() const noexcept { return value; }
  void retain() noexcept { lang_tls_stream_retain_v1(value); }
  void drop_one() noexcept { lang_tls_stream_release_v1(value); }
  void release() noexcept {
    drop_one();
    value = nullptr;
  }
  void abandon() noexcept { value = nullptr; }
};

struct FdRef {
  int value = -1;

  FdRef() = default;
  explicit FdRef(int fd) noexcept : value(fd) {}
  FdRef(const FdRef &) = delete;
  FdRef &operator=(const FdRef &) = delete;
  ~FdRef() {
    if (value >= 0) ::close(value);
  }

  int get() const noexcept { return value; }
  int release() noexcept {
    const int fd = value;
    value = -1;
    return fd;
  }
  void close_now() noexcept {
    if (value >= 0) ::close(value);
    value = -1;
  }
};

struct Gate {
  std::mutex mutex;
  std::condition_variable condition;
  bool open = false;

  void set() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      open = true;
    }
    condition.notify_all();
  }

  void wait(const std::string &context) {
    std::unique_lock<std::mutex> lock(mutex);
    if (!condition.wait_for(lock, std::chrono::seconds(20),
                            [this] { return open; }))
      fail(context + ": gate timeout");
  }
};

struct Arguments {
  std::map<std::string, std::string> values;

  bool has(const std::string &key) const { return values.count(key) != 0; }

  std::string get(const std::string &key, const std::string &fallback = {}) const {
    const auto found = values.find(key);
    return found == values.end() ? fallback : found->second;
  }

  std::string required(const std::string &key) const {
    const auto found = values.find(key);
    if (found == values.end()) fail("missing --" + key);
    return found->second;
  }

  uint64_t number(const std::string &key, uint64_t fallback) const {
    if (!has(key)) return fallback;
    try {
      return std::stoull(get(key));
    } catch (...) {
      fail("invalid numeric --" + key);
    }
  }
};

#if defined(__APPLE__)
int close_on_exec(int fd) noexcept {
  if (fd < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0) return fd;
  const int saved = errno;
  ::close(fd);
  errno = saved;
  return -1;
}
#endif

int open_socket(const addrinfo &entry) noexcept {
#if defined(__APPLE__)
  return close_on_exec(::socket(entry.ai_family, entry.ai_socktype, entry.ai_protocol));
#else
  return ::socket(entry.ai_family, entry.ai_socktype | SOCK_CLOEXEC, entry.ai_protocol);
#endif
}

int open_listener(const std::string &host, uint16_t *port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
  addrinfo *results = nullptr;
  const int resolved = ::getaddrinfo(host.c_str(), "0", &hints, &results);
  need(resolved == 0, "getaddrinfo(listener) failed: " +
                          std::string(gai_strerror(resolved)));

  int listener = -1;
  for (addrinfo *entry = results; entry != nullptr; entry = entry->ai_next) {
    listener = open_socket(*entry);
    if (listener < 0) continue;
    const int enabled = 1;
    (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                       sizeof(enabled));
    if (::bind(listener, entry->ai_addr, entry->ai_addrlen) == 0 &&
        ::listen(listener, 4) == 0) {
      sockaddr_storage address{};
      socklen_t length = sizeof(address);
      need(::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                         &length) == 0,
           "getsockname failed");
      if (address.ss_family == AF_INET)
        *port = ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port);
      else
        *port = ntohs(reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port);
      break;
    }
    ::close(listener);
    listener = -1;
  }
  ::freeaddrinfo(results);
  need(listener >= 0, "could not bind listener on " + host);
  return listener;
}

int connect_to(const std::string &host, uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  addrinfo *results = nullptr;
  const std::string service = std::to_string(port);
  const int resolved =
      ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
  need(resolved == 0, "getaddrinfo(connect) failed: " +
                          std::string(gai_strerror(resolved)));

  int fd = -1;
  for (addrinfo *entry = results; entry != nullptr; entry = entry->ai_next) {
    fd = open_socket(*entry);
    if (fd < 0) continue;
    if (::connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(results);
  need(fd >= 0, "could not connect to " + host + ":" + service);
  return fd;
}

int accept_with_timeout(int listener, int timeout_ms) {
  pollfd row{listener, POLLIN, 0};
  int ready = 0;
  do {
    ready = ::poll(&row, 1, timeout_ms);
  } while (ready < 0 && errno == EINTR);
  need(ready > 0, "accept poll timed out");
  need((row.revents & POLLNVAL) == 0, "accept poll reported POLLNVAL");
#if defined(__APPLE__)
  const int fd = close_on_exec(::accept(listener, nullptr, nullptr));
  need(fd >= 0, std::string("accept or fcntl(FD_CLOEXEC) failed: ") + std::strerror(errno));
#else
  const int fd = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
  need(fd >= 0, std::string("accept4 failed: ") + std::strerror(errno));
#endif
  return fd;
}

void set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  need(flags >= 0, "fcntl(F_GETFL) failed");
  need(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "fcntl(F_SETFL) failed");
}

void set_buffer_size(int fd, int option) {
  const int size = 16 * 1024;
  (void)::setsockopt(fd, SOL_SOCKET, option, &size, sizeof(size));
}

short poll_events(LangTlsStatus status) {
  switch (status) {
  case LANG_TLS_STATUS_WANT_READ:
    return POLLIN;
  case LANG_TLS_STATUS_WANT_WRITE:
    return POLLOUT;
  default:
    fail("poll requested for non-retry status " + std::to_string(status));
  }
}

void wait_for_fd(int fd, short events, const std::string &context) {
  pollfd row{fd, events, 0};
  int ready = 0;
  do {
    ready = ::poll(&row, 1, kPollMs);
  } while (ready < 0 && errno == EINTR);
  need(ready > 0, context + ": poll timeout");
  need((row.revents & POLLNVAL) == 0,
       context + ": poll reported POLLNVAL");
}

struct RetryCounts {
  bool want_read = false;
  bool want_write = false;
};

void drive_handshake(LangTlsStream *stream, int fd, RetryCounts *counts) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(25);
  LangTlsError error{};
  for (;;) {
    const LangTlsStatus status = lang_tls_handshake_v1(stream, &error);
    if (status == LANG_TLS_STATUS_OK) {
      require_ok(error, "handshake");
      return;
    }
    if (status == LANG_TLS_STATUS_ERROR) {
      require_error_status(status, error, "handshake");
      fail("handshake failed: " + describe(error));
    }
    if (counts != nullptr) {
      if (status == LANG_TLS_STATUS_WANT_READ) counts->want_read = true;
      if (status == LANG_TLS_STATUS_WANT_WRITE) counts->want_write = true;
    }
    need(std::chrono::steady_clock::now() < deadline,
         "handshake retry deadline exceeded");
    wait_for_fd(fd, poll_events(status), "handshake");
  }
}

void drive_shutdown(LangTlsStream *stream, int fd, std::string *last_status) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(25);
  LangTlsError error{};
  for (;;) {
    const LangTlsStatus status = lang_tls_shutdown_v1(stream, &error);
    if (status == LANG_TLS_STATUS_OK) {
      require_ok(error, "shutdown");
      if (last_status != nullptr) *last_status = "ok";
      return;
    }
    if (status == LANG_TLS_STATUS_ERROR) {
      require_error_status(status, error, "shutdown");
      fail("shutdown failed: " + describe(error));
    }
    if (last_status != nullptr) *last_status = std::to_string(status);
    need(std::chrono::steady_clock::now() < deadline,
         "shutdown retry deadline exceeded");
    wait_for_fd(fd, poll_events(status), "shutdown");
  }
}

void write_all(LangTlsStream *stream, int fd, const uint8_t *data,
               uint64_t length, RetryCounts *counts, bool *saw_partial,
               Gate *release_on_want_write = nullptr) {
  uint64_t offset = 0;
  LangTlsError error{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(40);
  while (offset < length) {
    uint64_t written = 0;
    const LangTlsStatus status =
        lang_tls_write_v1(stream, data + offset, length - offset, &written,
                          &error);
    if (status == LANG_TLS_STATUS_OK) {
      require_ok(error, "write");
      need(written > 0, "write returned OK without progress");
      if (saw_partial != nullptr && written < length - offset)
        *saw_partial = true;
      offset += written;
      continue;
    }
    if (status == LANG_TLS_STATUS_ERROR) {
      require_error_status(status, error, "write");
      fail("write failed: " + describe(error));
    }
    if (counts != nullptr) {
      if (status == LANG_TLS_STATUS_WANT_READ) counts->want_read = true;
      if (status == LANG_TLS_STATUS_WANT_WRITE) {
        counts->want_write = true;
        if (release_on_want_write != nullptr) release_on_want_write->set();
      }
    }
    need(std::chrono::steady_clock::now() < deadline,
         "write retry deadline exceeded");
    wait_for_fd(fd, poll_events(status), "write");
  }
}

void read_exact(LangTlsStream *stream, int fd, uint8_t *data, uint64_t length,
                RetryCounts *counts) {
  uint64_t offset = 0;
  LangTlsError error{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(40);
  while (offset < length) {
    uint64_t received = 0;
    const LangTlsStatus status =
        lang_tls_read_v1(stream, data + offset, length - offset, &received,
                         &error);
    if (status == LANG_TLS_STATUS_OK) {
      require_ok(error, "read");
      need(received > 0, "read returned OK without progress");
      offset += received;
      continue;
    }
    if (status == LANG_TLS_STATUS_EOF)
      fail("read hit clean EOF before the expected payload was complete");
    if (status == LANG_TLS_STATUS_ERROR) {
      require_error_status(status, error, "read");
      fail("read failed: " + describe(error));
    }
    if (counts != nullptr) {
      if (status == LANG_TLS_STATUS_WANT_READ) counts->want_read = true;
      if (status == LANG_TLS_STATUS_WANT_WRITE) counts->want_write = true;
    }
    need(std::chrono::steady_clock::now() < deadline,
         "read retry deadline exceeded");
    wait_for_fd(fd, poll_events(status), "read");
  }
}

struct EndRead {
  uint64_t total = 0;
  LangTlsStatus status = LANG_TLS_STATUS_ERROR;
  LangTlsError error{};
};

uint8_t payload_byte(uint64_t index) noexcept {
  return static_cast<uint8_t>((index * 37ULL + 11ULL) & 0xffULL);
}

EndRead read_until_end(LangTlsStream *stream, int fd) {
  EndRead outcome{};
  std::vector<uint8_t> buffer(64 * 1024);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  for (;;) {
    uint64_t received = 0;
    const LangTlsStatus status =
        lang_tls_read_v1(stream, buffer.data(), buffer.size(), &received,
                         &outcome.error);
    if (status == LANG_TLS_STATUS_OK) {
      require_ok(outcome.error, "read-to-end");
      need(received > 0, "read-to-end returned OK without progress");
      for (uint64_t index = 0; index < received; ++index) {
        need(buffer[index] == payload_byte(outcome.total + index),
             "payload mismatch at byte " +
                 std::to_string(outcome.total + index));
      }
      outcome.total += received;
      continue;
    }
    if (status == LANG_TLS_STATUS_EOF) {
      outcome.status = status;
      return outcome;
    }
    if (status == LANG_TLS_STATUS_ERROR) {
      outcome.status = status;
      return outcome;
    }
    need(std::chrono::steady_clock::now() < deadline,
         "read-to-end retry deadline exceeded");
    wait_for_fd(fd, poll_events(status), "read-to-end");
  }
}

uint8_t request_byte(uint64_t index) noexcept {
  return static_cast<uint8_t>((index * 73ULL + 29ULL) & 0xffULL);
}

void fill_pattern(std::vector<uint8_t> *buffer, uint8_t (*generator)(uint64_t)) {
  for (uint64_t index = 0; index < buffer->size(); ++index)
    (*buffer)[index] = generator(index);
}

ContextRef make_server_context(const std::string &certificate,
                               const std::string &key) {
  LangTlsError error{};
  auto *context = lang_tls_context_server_v1(
      reinterpret_cast<const uint8_t *>(certificate.data()),
      certificate.size(), reinterpret_cast<const uint8_t *>(key.data()),
      key.size(), &error);
  need(context != nullptr, "server context creation failed: " + describe(error));
  require_ok(error, "server context creation");
  return ContextRef(context);
}

ContextRef make_client_context(const std::string &ca_file) {
  LangTlsError error{};
  auto *context = lang_tls_context_client_v1(
      reinterpret_cast<const uint8_t *>(ca_file.data()), ca_file.size(),
      &error);
  need(context != nullptr, "client context creation failed: " + describe(error));
  require_ok(error, "client context creation");
  return ContextRef(context);
}

StreamRef make_client_stream(LangTlsContext *context, int fd,
                             const std::string &name,
                             LangTlsNameKind kind = LANG_TLS_NAME_AUTO) {
  LangTlsError error{};
  auto *stream = lang_tls_stream_client_v1(
      context, fd, kind, reinterpret_cast<const uint8_t *>(name.data()),
      name.size(), &error);
  need(stream != nullptr, "client stream creation failed: " + describe(error));
  require_ok(error, "client stream creation");
  return StreamRef(stream);
}

StreamRef make_server_stream(LangTlsContext *context, int fd) {
  LangTlsError error{};
  auto *stream = lang_tls_stream_server_v1(context, fd, &error);
  need(stream != nullptr, "server stream creation failed: " + describe(error));
  require_ok(error, "server stream creation");
  return StreamRef(stream);
}

void scenario_self(const Arguments &arguments) {
  const std::string certificate = arguments.required("cert");
  const std::string key = arguments.required("key");
  const std::string ca = arguments.required("ca");

  ContextRef server_context = make_server_context(certificate, key);
  ContextRef client_context = make_client_context(ca);
  client_context.retain();

  uint16_t port = 0;
  FdRef listener(open_listener("127.0.0.1", &port));
  std::vector<uint8_t> request(kLargePayload);
  std::vector<uint8_t> response(kLargePayload);
  fill_pattern(&request, request_byte);
  fill_pattern(&response, payload_byte);

  std::exception_ptr server_error;
  std::thread server([&] {
    try {
      FdRef fd(accept_with_timeout(listener.get(), kPollMs));
      StreamRef stream = make_server_stream(server_context.get(), fd.get());
      RetryCounts counts;
      drive_handshake(stream.get(), fd.get(), &counts);

      std::vector<uint8_t> incoming(request.size());
      read_exact(stream.get(), fd.get(), incoming.data(), incoming.size(),
                 nullptr);
      need(incoming == request, "server request payload mismatch");
      write_all(stream.get(), fd.get(), response.data(), response.size(),
                nullptr, nullptr);

      LangTlsError error{};
      for (;;) {
        uint8_t byte = 0;
        uint64_t received = 0;
        const LangTlsStatus status =
            lang_tls_read_v1(stream.get(), &byte, 1, &received, &error);
        if (status == LANG_TLS_STATUS_EOF) {
          require_ok(error, "server clean EOF");
          break;
        }
        if (status == LANG_TLS_STATUS_ERROR) {
          require_error_status(status, error, "server read-before-EOF");
          fail("server expected clean EOF: " + describe(error));
        }
        if (status == LANG_TLS_STATUS_OK) {
          need(received == 1, "unexpected zero-length server read");
          fail("server received unexpected extra byte");
        }
        wait_for_fd(fd.get(), poll_events(status), "server final read");
      }
      drive_shutdown(stream.get(), fd.get(), nullptr);
      stream.release();
      need(::fcntl(fd.get(), F_GETFD) >= 0,
           "server stream release closed the borrowed fd");
    } catch (...) {
      server_error = std::current_exception();
    }
  });

  try {
    FdRef fd(connect_to("127.0.0.1", port));
    StreamRef stream =
        make_client_stream(client_context.get(), fd.get(), "localhost");
    // Copying and dropping the stream must not destroy the live object.
    stream.retain();
    stream.drop_one();
    // The stream owns the only remaining context reference.
    client_context.drop_one();
    client_context.drop_one();
    client_context.abandon();

    RetryCounts counts;
    drive_handshake(stream.get(), fd.get(), &counts);

    uint64_t count = 99;
    LangTlsError error{};
    need(lang_tls_write_v1(stream.get(), nullptr, 0, &count, &error) ==
                 LANG_TLS_STATUS_OK &&
             count == 0,
         "zero-length write failed");
    require_ok(error, "zero-length write");
    count = 99;
    need(lang_tls_read_v1(stream.get(), nullptr, 0, &count, &error) ==
                 LANG_TLS_STATUS_OK &&
             count == 0,
         "zero-length read failed");
    require_ok(error, "zero-length read");

    bool saw_partial = false;
    write_all(stream.get(), fd.get(), request.data(), request.size(), nullptr,
              &saw_partial);

    std::vector<uint8_t> incoming(response.size());
    read_exact(stream.get(), fd.get(), incoming.data(), incoming.size(),
               nullptr);
    need(incoming == response, "client response payload mismatch");

    drive_shutdown(stream.get(), fd.get(), nullptr);
    stream.release();
    need(::fcntl(fd.get(), F_GETFD) >= 0,
         "client stream release closed the borrowed fd");
  } catch (...) {
    server.join();
    if (server_error) std::rethrow_exception(server_error);
    throw;
  }

  server.join();
  if (server_error) std::rethrow_exception(server_error);
  std::cout << "PASS self: 4 MiB binary both directions, clean close_notify, "
               "ARC and borrowed fd\n";
}

void scenario_nonblocking(const Arguments &arguments) {
  const std::string certificate = arguments.required("cert");
  const std::string key = arguments.required("key");
  const std::string ca = arguments.required("ca");

  ContextRef server_context = make_server_context(certificate, key);
  ContextRef client_context = make_client_context(ca);

  uint16_t port = 0;
  FdRef listener(open_listener("127.0.0.1", &port));
  std::vector<uint8_t> request(kLargePayload);
  std::vector<uint8_t> response(kLargePayload);
  fill_pattern(&request, request_byte);
  fill_pattern(&response, payload_byte);

  Gate start_read;
  Gate request_done;
  Gate send_response;
  std::exception_ptr server_error;
  std::thread server([&] {
    try {
      FdRef fd(accept_with_timeout(listener.get(), kPollMs));
      set_buffer_size(fd.get(), SO_RCVBUF);
      StreamRef stream = make_server_stream(server_context.get(), fd.get());
      drive_handshake(stream.get(), fd.get(), nullptr);

      start_read.wait("server start-read");
      std::vector<uint8_t> incoming(request.size());
      read_exact(stream.get(), fd.get(), incoming.data(), incoming.size(),
                 nullptr);
      need(incoming == request, "nonblocking server request mismatch");
      request_done.set();

      send_response.wait("server send-response");
      write_all(stream.get(), fd.get(), response.data(), response.size(),
                nullptr, nullptr);

      LangTlsError error{};
      for (;;) {
        uint8_t byte = 0;
        uint64_t received = 0;
        const LangTlsStatus status =
            lang_tls_read_v1(stream.get(), &byte, 1, &received, &error);
        if (status == LANG_TLS_STATUS_EOF) break;
        if (status == LANG_TLS_STATUS_ERROR)
          fail("server expected clean EOF after response: " +
               describe(error));
        if (status == LANG_TLS_STATUS_OK)
          fail("unexpected extra byte after nonblocking response");
        wait_for_fd(fd.get(), poll_events(status), "server final read");
      }
      drive_shutdown(stream.get(), fd.get(), nullptr);
    } catch (...) {
      server_error = std::current_exception();
    }
  });

  std::exception_ptr client_error;
  RetryCounts counts;
  bool saw_partial = false;
  try {
    FdRef fd(connect_to("127.0.0.1", port));
    set_nonblocking(fd.get());
    set_buffer_size(fd.get(), SO_SNDBUF);
    StreamRef stream =
        make_client_stream(client_context.get(), fd.get(), "localhost");

    drive_handshake(stream.get(), fd.get(), &counts);
    need(counts.want_read || counts.want_write,
         "nonblocking handshake never returned WANT_READ/WANT_WRITE");

    write_all(stream.get(), fd.get(), request.data(), request.size(), &counts,
              &saw_partial, &start_read);
    start_read.set();
    need(counts.want_write,
         "nonblocking write never returned WANT_WRITE while the peer was gated");

    request_done.wait("client request-done");
    uint8_t probe = 0;
    uint64_t received = 1;
    LangTlsError error{};
    const LangTlsStatus status =
        lang_tls_read_v1(stream.get(), &probe, 1, &received, &error);
    need(status == LANG_TLS_STATUS_WANT_READ,
         "nonblocking read before server response was not WANT_READ");
    require_ok(error, "nonblocking WANT_READ");
    counts.want_read = true;
    send_response.set();

    std::vector<uint8_t> incoming(response.size());
    read_exact(stream.get(), fd.get(), incoming.data(), incoming.size(),
               &counts);
    need(incoming == response, "nonblocking client response mismatch");
    drive_shutdown(stream.get(), fd.get(), nullptr);

    need(counts.want_read, "nonblocking run never observed WANT_READ");
    need(counts.want_write, "nonblocking run never observed WANT_WRITE");
    std::cout << "PASS nonblocking: WANT_READ/WANT_WRITE + poll, partial write="
              << (saw_partial ? "yes" : "no") << "\n";
  } catch (...) {
    client_error = std::current_exception();
  }

  start_read.set();
  request_done.set();
  send_response.set();
  server.join();
  if (client_error) std::rethrow_exception(client_error);
  if (server_error) std::rethrow_exception(server_error);
}

int drain_stopped_alarm(const sigset_t &alarms) noexcept {
#if defined(__APPLE__)
  // The timer has stopped and no other thread consumes this blocked signal.
  sigset_t pending;
  if (::sigpending(&pending) != 0) return errno;
  if (sigismember(&pending, SIGALRM) != 1) return 0;
  int signal = 0;
  const int status = ::sigwait(&alarms, &signal);
  return status ? status : (signal == SIGALRM ? 0 : EINVAL);
#else
  timespec no_wait{};
  while (::sigtimedwait(&alarms, nullptr, &no_wait) == SIGALRM) {
  }
  return 0;
#endif
}

void scenario_eintr(const Arguments &arguments) {
  const std::string certificate = arguments.required("cert");
  const std::string key = arguments.required("key");
  const std::string ca = arguments.required("ca");

  ContextRef server_context = make_server_context(certificate, key);
  ContextRef client_context = make_client_context(ca);

  uint16_t port = 0;
  FdRef listener(open_listener("127.0.0.1", &port));
  Gate send_data;
  std::exception_ptr server_error;

  sigset_t alarm_set;
  sigemptyset(&alarm_set);
  sigaddset(&alarm_set, SIGALRM);
  sigset_t old_mask;
  need(::pthread_sigmask(SIG_BLOCK, &alarm_set, &old_mask) == 0,
       "pthread_sigmask(block) failed");

  struct sigaction action {};
  action.sa_handler = [](int) {};
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0; // No SA_RESTART: recv must see EINTR.
  struct sigaction old_action {};
  need(::sigaction(SIGALRM, &action, &old_action) == 0,
       "sigaction(SIGALRM) failed");

  // The server thread inherits a blocked SIGALRM, so interval timers can only
  // interrupt the main thread's blocking recv.
  std::thread server([&] {
    try {
      FdRef fd(accept_with_timeout(listener.get(), kPollMs));
      StreamRef stream = make_server_stream(server_context.get(), fd.get());
      drive_handshake(stream.get(), fd.get(), nullptr);
      send_data.wait("eintr send-data");
      static constexpr char kMessage[] = "after-EINTR";
      write_all(stream.get(), fd.get(),
                reinterpret_cast<const uint8_t *>(kMessage),
                sizeof(kMessage) - 1, nullptr, nullptr);
      LangTlsError error{};
      for (;;) {
        uint8_t byte = 0;
        uint64_t received = 0;
        const LangTlsStatus status =
            lang_tls_read_v1(stream.get(), &byte, 1, &received, &error);
        if (status == LANG_TLS_STATUS_EOF) break;
        if (status == LANG_TLS_STATUS_ERROR)
          fail("EINTR server expected clean EOF: " + describe(error));
        if (status == LANG_TLS_STATUS_OK)
          fail("EINTR server received unexpected data");
        wait_for_fd(fd.get(), poll_events(status), "EINTR server final read");
      }
      drive_shutdown(stream.get(), fd.get(), nullptr);
    } catch (...) {
      server_error = std::current_exception();
    }
  });

  try {
    FdRef fd(connect_to("127.0.0.1", port));
    StreamRef stream =
        make_client_stream(client_context.get(), fd.get(), "localhost");
    drive_handshake(stream.get(), fd.get(), nullptr);

    need(::pthread_sigmask(SIG_UNBLOCK, &alarm_set, nullptr) == 0,
         "pthread_sigmask(unblock) failed");

    itimerval timer{};
    timer.it_value.tv_usec = 1000;
    timer.it_interval.tv_usec = 1000;
    need(::setitimer(ITIMER_REAL, &timer, nullptr) == 0,
         "setitimer failed");

    uint8_t byte = 0;
    uint64_t received = 0;
    LangTlsError error{};
    const LangTlsStatus status =
        lang_tls_read_v1(stream.get(), &byte, 1, &received, &error);

    need(::pthread_sigmask(SIG_BLOCK, &alarm_set, nullptr) == 0,
         "pthread_sigmask(block after read) failed");
    itimerval stopped{};
    need(::setitimer(ITIMER_REAL, &stopped, nullptr) == 0,
         "setitimer(stop) failed");
    need(drain_stopped_alarm(alarm_set) == 0, "draining pending SIGALRM failed");
    need(::sigaction(SIGALRM, &old_action, nullptr) == 0,
         "sigaction(SIGALRM restore) failed");
    need(::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr) == 0,
         "pthread_sigmask(restore) failed");

    need(status == LANG_TLS_STATUS_WANT_READ,
         "EINTR read did not return WANT_READ: status=" +
             std::to_string(status) + " " + describe(error));
    require_ok(error, "EINTR read");
    need(received == 0, "EINTR read returned bytes");

    send_data.set();
    uint8_t message[sizeof("after-EINTR") - 1] = {};
    read_exact(stream.get(), fd.get(), message, sizeof(message), nullptr);
    need(std::memcmp(message, "after-EINTR", sizeof(message)) == 0,
         "EINTR retry payload mismatch");
    drive_shutdown(stream.get(), fd.get(), nullptr);
  } catch (...) {
    itimerval stopped{};
    (void)::pthread_sigmask(SIG_BLOCK, &alarm_set, nullptr);
    (void)::setitimer(ITIMER_REAL, &stopped, nullptr);
    (void)drain_stopped_alarm(alarm_set);
    (void)::sigaction(SIGALRM, &old_action, nullptr);
    (void)::pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
    send_data.set();
    server.join();
    if (server_error) std::rethrow_exception(server_error);
    throw;
  }
  server.join();
  if (server_error) std::rethrow_exception(server_error);
  std::cout << "PASS EINTR: interrupted recv returned WANT_READ, not EOF\n";
}

void scenario_sigpipe(const Arguments &arguments) {
  const std::string certificate = arguments.required("cert");
  const std::string key = arguments.required("key");
  const std::string ca = arguments.required("ca");

  ContextRef server_context = make_server_context(certificate, key);
  ContextRef client_context = make_client_context(ca);

  uint16_t port = 0;
  FdRef listener(open_listener("127.0.0.1", &port));
  Gate server_closed;
  std::exception_ptr server_error;
  std::thread server([&] {
    try {
      FdRef fd(accept_with_timeout(listener.get(), kPollMs));
      StreamRef stream = make_server_stream(server_context.get(), fd.get());
      drive_handshake(stream.get(), fd.get(), nullptr);
      stream.release();
      linger reset{};
      reset.l_onoff = 1;
      reset.l_linger = 0;
      (void)::setsockopt(fd.get(), SOL_SOCKET, SO_LINGER, &reset,
                         sizeof(reset));
      fd.close_now(); // RST without a TLS close_notify
      server_closed.set();
    } catch (...) {
      server_error = std::current_exception();
      server_closed.set();
    }
  });

  try {
    FdRef fd(connect_to("127.0.0.1", port));
    StreamRef stream =
        make_client_stream(client_context.get(), fd.get(), "localhost");
    drive_handshake(stream.get(), fd.get(), nullptr);
    server_closed.wait("sigpipe server close");

    struct sigaction current {};
    need(::sigaction(SIGPIPE, nullptr, &current) == 0,
         "sigaction(SIGPIPE, GET) failed");
    if (current.sa_handler != SIG_DFL) {
      struct sigaction defaults {};
      defaults.sa_handler = SIG_DFL;
      sigemptyset(&defaults.sa_mask);
      need(::sigaction(SIGPIPE, &defaults, nullptr) == 0,
           "could not restore default SIGPIPE");
    }

    std::vector<uint8_t> chunk(64 * 1024, 0x5a);
    LangTlsError error{};
    bool saw_failure = false;
    for (int attempt = 0; attempt < 64; ++attempt) {
      uint64_t written = 0;
      const LangTlsStatus status =
          lang_tls_write_v1(stream.get(), chunk.data(), chunk.size(), &written,
                            &error);
      if (status == LANG_TLS_STATUS_OK) {
        need(written > 0, "SIGPIPE test write made no progress");
        continue;
      }
      if (status == LANG_TLS_STATUS_ERROR) {
        need(error.category == LANG_TLS_ERROR_IO ||
                 error.category == LANG_TLS_ERROR_PROTOCOL ||
                 error.category == LANG_TLS_ERROR_UNEXPECTED_EOF,
             "SIGPIPE write failure had unexpected category: " +
                 describe(error));
        need(error.system_errno != 0 || error.openssl_error != 0,
             "SIGPIPE write failure had no diagnostic: " + describe(error));
        saw_failure = true;
        break;
      }
      wait_for_fd(fd.get(), poll_events(status), "SIGPIPE write");
    }
    need(saw_failure, "peer reset was not observed as a write failure");
    struct sigaction after {};
    need(::sigaction(SIGPIPE, nullptr, &after) == 0,
         "sigaction(SIGPIPE, GET after) failed");
    need(after.sa_handler == SIG_DFL,
         "hosted TLS changed the process-wide SIGPIPE disposition");
    std::cout << "PASS SIGPIPE: default disposition survived peer reset; " +
                     describe(error) + "\n";
  } catch (...) {
    server_closed.set();
    server.join();
    if (server_error) std::rethrow_exception(server_error);
    throw;
  }
  server.join();
  if (server_error) std::rethrow_exception(server_error);
}

void scenario_validation(const Arguments &arguments) {
  const std::string certificate = arguments.required("cert");
  const std::string key = arguments.required("key");
  const std::string ca = arguments.required("ca");
  const std::string wrong_key = arguments.required("wrong-key");

  LangTlsError error{};
  {
    auto *context = lang_tls_context_client_default_v1(&error);
    need(context != nullptr,
         "default client context failed: " + describe(error));
    require_ok(error, "default client context");
    lang_tls_context_release_v1(context);
  }

  const uint8_t embedded_nul[] = {'/', 't', 'm', 'p', '/', 0, 'x'};
  error = {};
  need(lang_tls_context_client_v1(embedded_nul, sizeof(embedded_nul), &error) ==
           nullptr,
       "client context accepted embedded NUL");
  require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT,
                   "client embedded NUL");
  error = {};
  need(lang_tls_context_server_v1(embedded_nul, sizeof(embedded_nul),
                                  reinterpret_cast<const uint8_t *>(key.data()),
                                  key.size(), &error) == nullptr,
       "server context accepted embedded NUL");
  require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT,
                   "server embedded NUL");
  error = {};
  need(lang_tls_context_client_v1(
           reinterpret_cast<const uint8_t *>("/definitely/missing/ca.pem"),
           std::strlen("/definitely/missing/ca.pem"), &error) == nullptr,
       "missing CA file unexpectedly succeeded");
  require_category(error, LANG_TLS_ERROR_CERTIFICATE, "missing CA");

  ContextRef server_context = make_server_context(certificate, key);
  ContextRef client_context = make_client_context(ca);
  error = {};
  need(lang_tls_context_server_v1(
           reinterpret_cast<const uint8_t *>(certificate.data()),
           certificate.size(),
           reinterpret_cast<const uint8_t *>(wrong_key.data()), wrong_key.size(),
           &error) == nullptr,
       "mismatched private key unexpectedly succeeded");
  require_category(error, LANG_TLS_ERROR_CERTIFICATE, "mismatched key");

  int pair[2] = {-1, -1};
#if defined(__APPLE__)
  need(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair failed");
#else
  need(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0,
       "socketpair failed");
#endif
  FdRef left(pair[0]);
  FdRef right(pair[1]);
#if defined(__APPLE__)
  need(::fcntl(left.get(), F_SETFD, FD_CLOEXEC) == 0 &&
           ::fcntl(right.get(), F_SETFD, FD_CLOEXEC) == 0,
       "socketpair fcntl(FD_CLOEXEC) failed");
#endif

  error = {};
  need(lang_tls_stream_client_v1(client_context.get(), left.get(),
                                 LANG_TLS_NAME_AUTO, nullptr, 0,
                                 &error) == nullptr,
       "client stream accepted a missing hostname");
  require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT, "missing hostname");

  const uint8_t nul_host[] = {'l', 'o', 0, 'h'};
  error = {};
  need(lang_tls_stream_client_v1(client_context.get(), left.get(),
                                 LANG_TLS_NAME_DNS, nul_host,
                                 sizeof(nul_host), &error) == nullptr,
       "client stream accepted embedded NUL hostname");
  require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT,
                   "embedded NUL hostname");

  const uint8_t bracket_host[] = {'[', ':', ':', '1', ']'};
  error = {};
  need(lang_tls_stream_client_v1(client_context.get(), left.get(),
                                 LANG_TLS_NAME_AUTO, bracket_host,
                                 sizeof(bracket_host), &error) == nullptr,
       "client stream accepted bracketed hostname");
  require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT,
                   "bracketed hostname");

  const uint8_t not_ip[] = {'n', 'o', 't', '-', 'i', 'p'};
  error = {};
  need(lang_tls_stream_client_v1(client_context.get(), left.get(),
                                 LANG_TLS_NAME_IP, not_ip, sizeof(not_ip),
                                 &error) == nullptr,
       "explicit IP kind accepted a DNS name");
  require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT, "invalid IP");

  const uint8_t ipv4_literal[] = {'1', '2', '7', '.', '0', '.', '0', '.', '1'};
  error = {};
  need(lang_tls_stream_client_v1(client_context.get(), left.get(),
                                 LANG_TLS_NAME_DNS, ipv4_literal,
                                 sizeof(ipv4_literal), &error) == nullptr,
       "explicit DNS kind accepted an IPv4 literal");
  require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT,
                   "DNS kind with IPv4 literal");

  const uint8_t zoned_ip[] = {'f', 'e', '8', '0', ':', ':', '1', '%', 'x'};
  for (const auto kind : {LANG_TLS_NAME_AUTO, LANG_TLS_NAME_IP}) {
    error = {};
    need(lang_tls_stream_client_v1(client_context.get(), left.get(), kind,
                                   zoned_ip, sizeof(zoned_ip), &error) == nullptr,
         "client stream accepted an IPv6 zone suffix");
    require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT, "IPv6 zone suffix");
  }

  const uint8_t host_port[] = {'l', 'o', 'c', 'a', 'l', 'h', 'o', 's',
                               't', ':', '4', '4', '3'};
  error = {};
  need(lang_tls_stream_client_v1(client_context.get(), left.get(),
                                 LANG_TLS_NAME_DNS, host_port,
                                 sizeof(host_port), &error) == nullptr,
       "DNS kind accepted authority/port syntax");
  require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT, "host:port");

  const uint8_t localhost[] = {'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't'};
  error = {};
  need(lang_tls_stream_client_v1(server_context.get(), left.get(),
                                 LANG_TLS_NAME_DNS, localhost,
                                 sizeof(localhost), &error) == nullptr,
       "client stream accepted a server context");
  require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT, "role mismatch");

  error = {};
  need(lang_tls_stream_server_v1(client_context.get(), left.get(), &error) ==
           nullptr,
       "server stream accepted a client context");
  require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT,
                   "server role mismatch");

  error = {};
  need(lang_tls_stream_client_v1(client_context.get(), -1, LANG_TLS_NAME_DNS,
                                 localhost, sizeof(localhost), &error) ==
           nullptr,
       "client stream accepted fd -1");
  require_category(error, LANG_TLS_ERROR_INVALID_ARGUMENT, "bad fd");

  LangTlsError ignored{};
  lang_tls_context_retain_v1(nullptr);
  lang_tls_context_release_v1(nullptr);
  lang_tls_stream_retain_v1(nullptr);
  lang_tls_stream_release_v1(nullptr);
  need(lang_tls_handshake_v1(nullptr, &ignored) == LANG_TLS_STATUS_ERROR,
       "null handshake did not fail");
  require_category(ignored, LANG_TLS_ERROR_INVALID_ARGUMENT, "null handshake");
  uint64_t count = 7;
  ignored = {};
  need(lang_tls_read_v1(nullptr, nullptr, 0, &count, &ignored) ==
           LANG_TLS_STATUS_ERROR,
       "null read did not fail");
  require_category(ignored, LANG_TLS_ERROR_INVALID_ARGUMENT, "null read");
  ignored = {};
  need(lang_tls_write_v1(nullptr, nullptr, 0, &count, &ignored) ==
           LANG_TLS_STATUS_ERROR,
       "null write did not fail");
  require_category(ignored, LANG_TLS_ERROR_INVALID_ARGUMENT, "null write");
  ignored = {};
  need(lang_tls_shutdown_v1(nullptr, &ignored) == LANG_TLS_STATUS_ERROR,
       "null shutdown did not fail");
  require_category(ignored, LANG_TLS_ERROR_INVALID_ARGUMENT, "null shutdown");

  // BIO_get_new_index() has a process-wide budget.  Creating many streams must
  // reuse one BIO method rather than exhausting OpenSSL's custom type indices.
  for (int iteration = 0; iteration < 300; ++iteration) {
    error = {};
    LangTlsStream *client = lang_tls_stream_client_v1(
        client_context.get(), left.get(), LANG_TLS_NAME_DNS, localhost,
        sizeof(localhost), &error);
    need(client != nullptr, "client stream iteration " +
                                std::to_string(iteration) + " failed: " +
                                describe(error));
    lang_tls_stream_release_v1(client);
    error = {};
    LangTlsStream *server =
        lang_tls_stream_server_v1(server_context.get(), right.get(), &error);
    need(server != nullptr, "server stream iteration " +
                                std::to_string(iteration) + " failed: " +
                                describe(error));
    lang_tls_stream_release_v1(server);
  }

  StreamRef server_stream = make_server_stream(server_context.get(), right.get());
  StreamRef client_stream =
      make_client_stream(client_context.get(), left.get(), "localhost");
  std::exception_ptr handshake_error;
  std::thread server_handshake([&] {
    try {
      drive_handshake(server_stream.get(), right.get(), nullptr);
    } catch (...) {
      handshake_error = std::current_exception();
    }
  });
  try {
    drive_handshake(client_stream.get(), left.get(), nullptr);
  } catch (...) {
    server_handshake.join();
    throw;
  }
  server_handshake.join();
  if (handshake_error) std::rethrow_exception(handshake_error);

  count = 123;
  ignored = {};
  need(lang_tls_read_v1(client_stream.get(), nullptr, 1, &count, &ignored) ==
           LANG_TLS_STATUS_ERROR &&
           count == 0,
       "null read buffer with capacity was accepted");
  require_category(ignored, LANG_TLS_ERROR_INVALID_ARGUMENT,
                   "null read buffer");
  count = 123;
  ignored = {};
  need(lang_tls_write_v1(client_stream.get(), nullptr, 1, &count, &ignored) ==
           LANG_TLS_STATUS_ERROR &&
           count == 0,
       "null write buffer with length was accepted");
  require_category(ignored, LANG_TLS_ERROR_INVALID_ARGUMENT,
                   "null write buffer");
  ignored = {};
  need(lang_tls_read_v1(client_stream.get(), nullptr, 0, nullptr, &ignored) ==
           LANG_TLS_STATUS_ERROR,
       "null out_count was accepted");
  require_category(ignored, LANG_TLS_ERROR_INVALID_ARGUMENT, "null out_count");

  std::cout << "PASS validation: null/NUL/length/role/path/context errors\n";
}

void scenario_client_python(const Arguments &arguments) {
  const std::string host = arguments.get("host", "127.0.0.1");
  const std::string ca = arguments.required("ca");
  const std::string name = arguments.required("name");
  const std::string expect = arguments.required("expect");
  const std::string kind_text = arguments.get("name-kind", "auto");
  const uint16_t port =
      static_cast<uint16_t>(arguments.number("port", 0));
  need(port != 0, "--port is required");
  const uint64_t payload_size = arguments.number("payload-size", 4096);

  LangTlsNameKind kind = LANG_TLS_NAME_AUTO;
  if (kind_text == "dns") kind = LANG_TLS_NAME_DNS;
  else if (kind_text == "ip") kind = LANG_TLS_NAME_IP;
  else need(kind_text == "auto", "invalid --name-kind");

  LangTlsError context_error{};
  LangTlsContext *raw_context = nullptr;
  if (ca == "default") {
    raw_context = lang_tls_context_client_default_v1(&context_error);
  } else {
    raw_context = lang_tls_context_client_v1(
        reinterpret_cast<const uint8_t *>(ca.data()), ca.size(),
        &context_error);
  }
  need(raw_context != nullptr,
       "client-python context creation failed: " + describe(context_error));
  require_ok(context_error, "client-python context creation");
  ContextRef context(raw_context);
  FdRef fd(connect_to(host, port));
  StreamRef stream = make_client_stream(context.get(), fd.get(), name, kind);

  if (expect == "certificate") {
    LangTlsError error{};
    const LangTlsStatus status = lang_tls_handshake_v1(stream.get(), &error);
    require_error_status(status, error, "certificate rejection");
    require_category(error, LANG_TLS_ERROR_CERTIFICATE, "certificate rejection");
    need(error.verify_result != X509_V_OK,
         "certificate failure did not carry X509 verify result: " +
             describe(error));
    if (arguments.has("expect-verify")) {
      const int64_t expected =
          static_cast<int64_t>(arguments.number("expect-verify", 0));
      need(error.verify_result == expected,
           "expected verify result " + std::to_string(expected) + ", got " +
               describe(error));
    }
    std::cout << "PASS client-python certificate rejection: " +
                     describe(error) + "\n";
    return;
  }

  need(expect == "ok" || expect == "unexpected-eof",
       "invalid --expect");
  drive_handshake(stream.get(), fd.get(), nullptr);
  const EndRead outcome = read_until_end(stream.get(), fd.get());
  need(outcome.total == payload_size,
       "received " + std::to_string(outcome.total) + " of " +
           std::to_string(payload_size) + " expected bytes");
  if (expect == "ok") {
    need(outcome.status == LANG_TLS_STATUS_EOF,
         "clean peer did not produce EOF: status=" +
             std::to_string(outcome.status) + " " + describe(outcome.error));
    require_ok(outcome.error, "clean EOF");
    drive_shutdown(stream.get(), fd.get(), nullptr);
    std::cout << "PASS client-python clean EOF after " +
                     std::to_string(outcome.total) + " binary bytes\n";
  } else {
    need(outcome.status == LANG_TLS_STATUS_ERROR,
         "truncating peer produced status " + std::to_string(outcome.status) +
             ", not ERROR");
    require_category(outcome.error, LANG_TLS_ERROR_UNEXPECTED_EOF,
                     "truncating peer");
    std::cout << "PASS client-python truncation: " + describe(outcome.error) +
                     "\n";
  }
}

void scenario_server_once(const Arguments &arguments) {
  const std::string certificate = arguments.required("cert");
  const std::string key = arguments.required("key");
  const std::string host = arguments.get("host", "127.0.0.1");
  const std::string port_file = arguments.required("port-file");

  ContextRef context = make_server_context(certificate, key);
  uint16_t port = 0;
  FdRef listener(open_listener(host, &port));
  {
    std::ofstream output(port_file);
    need(output.good(), "could not write port file");
    output << port << "\n";
  }

  FdRef fd(accept_with_timeout(listener.get(), kPollMs));
  StreamRef stream = make_server_stream(context.get(), fd.get());
  drive_handshake(stream.get(), fd.get(), nullptr);

  std::string request;
  LangTlsError error{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (request.find('\n') == std::string::npos) {
    char buffer[64];
    uint64_t received = 0;
    const LangTlsStatus status =
        lang_tls_read_v1(stream.get(), buffer, sizeof(buffer), &received,
                         &error);
    if (status == LANG_TLS_STATUS_OK) {
      request.append(buffer, static_cast<size_t>(received));
      continue;
    }
    if (status == LANG_TLS_STATUS_ERROR)
      fail("server-once read failed: " + describe(error));
    need(std::chrono::steady_clock::now() < deadline,
         "server-once read timeout");
    wait_for_fd(fd.get(), poll_events(status), "server-once read");
  }
  need(request.starts_with("ping"),
       "server-once expected ping, got " + request);
  static constexpr char kReply[] = "pong\n";
  write_all(stream.get(), fd.get(),
            reinterpret_cast<const uint8_t *>(kReply), sizeof(kReply) - 1,
            nullptr, nullptr);
  drive_shutdown(stream.get(), fd.get(), nullptr);
  std::cout << "PASS server-once: independent client exchanged ping/pong\n";
}

void usage() {
  std::cerr
      << "usage: tls_runtime_test --scenario <self|nonblocking|eintr|sigpipe|"
         "validation|client-python|server-once> [--key value]...\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    Arguments arguments;
    std::string scenario;
    for (int index = 1; index < argc; ++index) {
      const std::string key = argv[index];
      if (key == "--scenario") {
        need(index + 1 < argc, "--scenario needs a value");
        scenario = argv[++index];
      } else if (key.starts_with("--")) {
        need(index + 1 < argc, key + " needs a value");
        arguments.values.emplace(key.substr(2), argv[++index]);
      } else {
        scenario = key;
      }
    }
    need(!scenario.empty(), "missing scenario");

    if (scenario == "self")
      scenario_self(arguments);
    else if (scenario == "nonblocking")
      scenario_nonblocking(arguments);
    else if (scenario == "eintr")
      scenario_eintr(arguments);
    else if (scenario == "sigpipe")
      scenario_sigpipe(arguments);
    else if (scenario == "validation")
      scenario_validation(arguments);
    else if (scenario == "client-python")
      scenario_client_python(arguments);
    else if (scenario == "server-once")
      scenario_server_once(arguments);
    else {
      usage();
      return 2;
    }
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "FAIL: " << exception.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "FAIL: unknown exception\n";
    return 1;
  }
}
