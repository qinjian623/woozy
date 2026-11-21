#include "../native/network.hpp"

#include "../native/gc_program.hpp"
#include "posix.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstddef>
#include <fcntl.h>
#include <libproc.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" uint64_t lang_darwin_async_last_watch_v1() noexcept
    __attribute__((weak_import));
extern "C" void lang_darwin_async_notify_listener_shutdown_v1(int32_t, uint64_t) noexcept
    __attribute__((weak_import));

namespace {
namespace darwin = lang::native::darwin;
using lang::native::gc_program::SafeGuard;

constexpr int32_t kUnix = 1;
constexpr int32_t kInet = 2;
constexpr int32_t kInet6 = 10;
constexpr uint32_t kSocketFlags = darwin::kNonblock | darwin::kCloexec;
constexpr size_t kPathOffset = offsetof(sockaddr_un, sun_path);
constexpr int32_t kTrunc = 32;

struct SocketAddress {
  sockaddr_storage value{};
  socklen_t length = 0;

  sockaddr *get() noexcept { return reinterpret_cast<sockaddr *>(&value); }

  template <class T> void set(const T &address, socklen_t size) noexcept {
    static_assert(sizeof(T) <= sizeof(value));
    length = size;
    std::memcpy(&value, &address, sizeof(address));
  }

  template <class T> T as() const noexcept {
    static_assert(sizeof(T) <= sizeof(value));
    T address;
    std::memcpy(&address, &value, sizeof(address));
    return address;
  }
};

int platform_family(int32_t family) noexcept {
  switch (family) {
    case kUnix: return AF_UNIX;
    case kInet: return AF_INET;
    case kInet6: return AF_INET6;
    default: return -1;
  }
}

int64_t address_bytes(const LangNetAddress *in, const LangVec **out) noexcept {
  if (!in) return -14;
  if (in->reserved || (in->flags & ~uint16_t{1}) || (in->flags && in->family != kUnix))
    return -22;
  const LangVec *bytes = in->data;
  if (!bytes || bytes->stride != 1 || bytes->initialized != bytes->length ||
      bytes->copy || bytes->drop || (bytes->length && !bytes->data)) return -22;
  if (in->family == kInet && bytes->length != 4) return -22;
  if (in->family == kInet6 && bytes->length != 16) return -22;
  if (platform_family(in->family) < 0) return -97;
  *out = bytes;
  return 0;
}

int64_t encode(const LangNetAddress *in, SocketAddress &out) noexcept {
  const LangVec *bytes = nullptr;
  const int64_t status = address_bytes(in, &bytes);
  if (status < 0) return status;
  if (in->family == kInet) {
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(in->port);
    std::memcpy(&address.sin_addr, bytes->data, sizeof(address.sin_addr));
    out.set(address, sizeof(address));
    return 0;
  }
  if (in->family == kInet6) {
    sockaddr_in6 address{};
    address.sin6_len = sizeof(address);
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(in->port);
    address.sin6_scope_id = in->scope;
    std::memcpy(&address.sin6_addr, bytes->data, sizeof(address.sin6_addr));
    out.set(address, sizeof(address));
    return 0;
  }
  if (in->flags) return -95;  // Darwin has no abstract Unix socket namespace.
  sockaddr_un address{};
  if (bytes->length >= sizeof(address.sun_path)) return -36;
  if (bytes->length) {
    if (std::memchr(bytes->data, 0, bytes->length)) return -22;
    std::memcpy(address.sun_path, bytes->data, bytes->length);
  }
  address.sun_len = static_cast<uint8_t>(kPathOffset + bytes->length);
  address.sun_family = AF_UNIX;
  out.set(address, address.sun_len);
  return 0;
}

void write_address(LangNetAddress &out, int32_t family, uint16_t port,
                   uint32_t scope, const void *raw, size_t count) noexcept {
  LangVec *bytes = lang_vec_new_v1(count, 1, nullptr, nullptr);
  const auto *data = static_cast<const uint8_t *>(raw);
  for (size_t i = 0; i < count; ++i)
    lang_vec_init_v1(bytes, data + i);
  out = {family, port, 0, scope, 0, bytes};
}

int64_t decode(const SocketAddress &in, LangNetAddress &out) noexcept {
  if (in.length == 0) {
    write_address(out, 0, 0, 0, nullptr, 0);
    return 0;
  }
  if (in.length < kPathOffset || in.length > sizeof(in.value)) return -22;
  switch (in.value.ss_family) {
    case AF_UNIX: {
      if (in.length > sizeof(sockaddr_un)) return -22;
      const auto address = in.as<sockaddr_un>();
      const size_t capacity = in.length - kPathOffset;
      const size_t size = strnlen(address.sun_path, capacity);
      if (size >= sizeof(address.sun_path)) return -36;
      write_address(out, kUnix, 0, 0, address.sun_path, size);
      return 0;
    }
    case AF_INET: {
      if (in.length < sizeof(sockaddr_in)) return -22;
      const auto address = in.as<sockaddr_in>();
      write_address(out, kInet, ntohs(address.sin_port), 0,
                    &address.sin_addr, sizeof(address.sin_addr));
      return 0;
    }
    case AF_INET6: {
      if (in.length < sizeof(sockaddr_in6)) return -22;
      const auto address = in.as<sockaddr_in6>();
      write_address(out, kInet6, ntohs(address.sin6_port), address.sin6_scope_id,
                    &address.sin6_addr, sizeof(address.sin6_addr));
      return 0;
    }
    default:
      return -97;
  }
}

int socket_type(int32_t type) noexcept {
  const int32_t kind = type & ~kSocketFlags;
  switch (kind) {
    case 1: return SOCK_STREAM;
    case 2: return SOCK_DGRAM;
    default: return -1;
  }
}

int64_t configure_fd(int fd, uint32_t flags) noexcept {
  if ((flags & darwin::kCloexec) && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    return darwin::error(errno);
  const int status = fcntl(fd, F_GETFL);
  if (status < 0) return darwin::error(errno);
  const int updated = (flags & darwin::kNonblock) ? status | O_NONBLOCK : status & ~O_NONBLOCK;
  return status == updated ? 0 : darwin::result(fcntl(fd, F_SETFL, updated));
}

int64_t configured_accept(int fd, SocketAddress &address, bool want_address,
                          uint32_t flags) noexcept {
  address.length = sizeof(address.value);
  const int peer = accept(fd, want_address ? address.get() : nullptr,
                          want_address ? &address.length : nullptr);
  if (peer < 0) return darwin::error(errno);
  const int64_t configured = configure_fd(peer, flags);
  if (configured < 0) {
    close(peer);
    return configured;
  }
  return peer;
}

int64_t message_flags(int32_t flags, bool sending) noexcept {
  constexpr int32_t kPeek = 2;
  constexpr int32_t kDontWait = 64;
  constexpr int32_t kWaitAll = 256;
  constexpr int32_t kNoSignal = 0x4000;
  if (flags & ~(kPeek | kTrunc | kDontWait | kWaitAll | kNoSignal)) return -22;
  int out = sending || (flags & kNoSignal) ? MSG_NOSIGNAL : 0;
  if (flags & kPeek) out |= MSG_PEEK;
  if (flags & kDontWait) out |= MSG_DONTWAIT;
  if (flags & kWaitAll) out |= MSG_WAITALL;
  return out;
}

int64_t send_data(int fd, const void *data, uint64_t count, int32_t flags,
                  const sockaddr *address, socklen_t length) noexcept {
  if (count > INT64_MAX) return -22;
  if (count && !data) return -14;
  const int64_t translated = message_flags(flags, true);
  if (translated < 0) return translated;
  SafeGuard safe;
  return darwin::result(sendto(fd, data, count, static_cast<int>(translated), address, length));
}

int64_t receive_data(int fd, void *data, uint64_t count, int flags,
                     SocketAddress *address) noexcept {
  SafeGuard safe;
  return darwin::result(recvfrom(fd, data, count, flags,
                                 address ? address->get() : nullptr,
                                 address ? &address->length : nullptr));
}

template <class T>
int64_t read_option(int fd, int level, int option, T &value) noexcept {
  socklen_t length = sizeof(value);
  if (getsockopt(fd, level, option, &value, &length) < 0) return darwin::error(errno);
  return length == sizeof(value) ? 0 : -22;
}

int64_t receive_truncated(int fd, void *data, uint64_t count, int flags,
                          SocketAddress *address) noexcept {
  int type;
  const int64_t type_status = read_option(fd, SOL_SOCKET, SO_TYPE, type);
  if (type_status < 0) return type_status;
  if (type == SOCK_STREAM) {
    SocketAddress local;
    local.length = sizeof(local.value);
    if (getsockname(fd, local.get(), &local.length) < 0) return darwin::error(errno);
    if (local.value.ss_family == AF_UNIX || count == 0)
      return receive_data(fd, data, count, flags, address);
    // Linux discards the bytes it reads from a TCP stream with MSG_TRUNC.
    void *scratch = lang_alloc_v1(count, 1);
    if (!scratch) return -12;
    const auto result = receive_data(fd, scratch, count, flags, address);
    lang_free_v1(scratch);
    return result;
  }

  // Darwin reports only copied bytes with MSG_TRUNC. Read into one additional
  // iovec to measure the entire datagram in the same receive operation.
  int receive_buffer_size;
  const auto buffer_status = read_option(fd, SOL_SOCKET, SO_RCVBUF, receive_buffer_size);
  if (buffer_status < 0) return buffer_status;
  const auto limit = static_cast<uint64_t>(std::max(65536, receive_buffer_size));
  const uint64_t extra = count < limit ? limit - count : 0;
  void *scratch = extra ? lang_alloc_v1(extra, 1) : nullptr;
  if (extra && !scratch) return -12;
  iovec buffers[]{{data, count}, {scratch, extra}};
  msghdr message{};
  if (address) {
    message.msg_name = address->get();
    message.msg_namelen = address->length;
  }
  message.msg_iov = buffers;
  message.msg_iovlen = extra ? 2 : 1;
  int64_t result;
  {
    SafeGuard safe;
    result = darwin::result(recvmsg(fd, &message, flags));
  }
  lang_free_v1(scratch);
  if (result < 0) return result;
  if (address) address->length = message.msg_namelen;
  // shortcut: an oversized datagram beyond the receive-buffer bound reports EMSGSIZE, extend sizing if Darwin permits larger packets.
  return (message.msg_flags & MSG_TRUNC) ? -90 : result;
}

enum class OptionKind { Integer, Boolean, Linger, Timeval };

struct SocketOption {
  int level;
  int name;
  OptionKind kind = OptionKind::Integer;
};

bool socket_option(int32_t level, int32_t name, SocketOption &out) noexcept {
  switch (level) {
    case 1:
      switch (name) {
        case 2: out = {SOL_SOCKET, SO_REUSEADDR, OptionKind::Boolean}; return true;
        case 3: out = {SOL_SOCKET, SO_TYPE}; return true;
        case 4: out = {SOL_SOCKET, SO_ERROR}; return true;
        case 7: out = {SOL_SOCKET, SO_SNDBUF}; return true;
        case 8: out = {SOL_SOCKET, SO_RCVBUF}; return true;
        case 9: out = {SOL_SOCKET, SO_KEEPALIVE, OptionKind::Boolean}; return true;
        case 13: out = {SOL_SOCKET, SO_LINGER, OptionKind::Linger}; return true;
        case 20: out = {SOL_SOCKET, SO_RCVTIMEO, OptionKind::Timeval}; return true;
        case 21: out = {SOL_SOCKET, SO_SNDTIMEO, OptionKind::Timeval}; return true;
        default: return false;
      }
    case 6:
      if (name != 1) return false;
      out = {IPPROTO_TCP, TCP_NODELAY, OptionKind::Boolean};
      return true;
    case 41:
      if (name != 26) return false;
      out = {IPPROTO_IPV6, IPV6_V6ONLY, OptionKind::Boolean};
      return true;
    default:
      return false;
  }
}

// The existing byte interface uses two 64-bit fields for Linux timeval.
struct WoozyTimeval {
  int64_t seconds;
  int64_t microseconds;
};

int64_t read_option_int(int fd, const SocketOption &option, int32_t &value) noexcept {
  int32_t local;
  const auto result = read_option(fd, option.level, option.name, local);
  if (result < 0) return result;
  if (option.level == SOL_SOCKET && option.name == SO_ERROR && local != 0)
    local = static_cast<int32_t>(-darwin::error(local));
  if (option.kind == OptionKind::Boolean)
    local = local != 0;
  value = local;
  return 0;
}

template <class T>
void write_option_bytes(const T &value, void *data, uint64_t &length) noexcept {
  length = std::min<uint64_t>(length, sizeof(value));
  if (length) std::memcpy(data, &value, length);
}

int64_t socket_name(int fd, LangNetAddress *out, bool peer) noexcept {
  if (!out) return -14;
  SocketAddress address;
  address.length = sizeof(address.value);
  const int status = peer ? getpeername(fd, address.get(), &address.length)
                          : getsockname(fd, address.get(), &address.length);
  if (status < 0) return darwin::error(errno);
  return decode(address, *out);
}

#define LANG_NATIVE_NO_STACK_PROTECTOR __attribute__((no_stack_protector))
#include "../native/ip_text.inc"
#undef LANG_NATIVE_NO_STACK_PROTECTOR

} // namespace

extern "C" {

int64_t lang_net_socket_v1(int32_t family, int32_t type, int32_t protocol) noexcept {
  const int domain = platform_family(family);
  if (domain < 0) return -97;
  const int kind = socket_type(type);
  if (kind < 0) return -22;
  const darwin::LaunchGuard guard;
  const int fd = socket(domain, kind, protocol);
  if (fd < 0) return darwin::error(errno);
  const int64_t configured = configure_fd(fd, type & kSocketFlags);
  if (configured < 0) {
    close(fd);
    return configured;
  }
  return fd;
}

int64_t lang_net_bind_v1(int32_t fd, const LangNetAddress *in) noexcept {
  SocketAddress address;
  const int64_t status = encode(in, address);
  if (status < 0) return status;
  return darwin::result(bind(fd, address.get(), address.length));
}

int64_t lang_net_connect_v1(int32_t fd, const LangNetAddress *in) noexcept {
  SocketAddress address;
  const int64_t status = encode(in, address);
  if (status < 0) return status;
  SafeGuard safe;
  return darwin::result(connect(fd, address.get(), address.length));
}

int64_t lang_net_listen_v1(int32_t fd, int32_t backlog) noexcept {
  return darwin::result(listen(fd, backlog));
}

int64_t lang_net_accept_v1(int32_t fd, LangNetAddress *out, uint32_t flags) noexcept {
  if (flags & ~kSocketFlags) return -22;
  SocketAddress address;
  int64_t accepted;
  if (flags & darwin::kCloexec) {
    const darwin::LaunchGuard guard;
    const int status = fcntl(fd, F_GETFL);
    if (status < 0) return darwin::error(errno);
    // shortcut: CLOEXEC requires a nonblocking listener on Darwin, use atomic accept4 if Darwin exposes it.
    if (!(status & O_NONBLOCK)) return -95;
    accepted = configured_accept(fd, address, out != nullptr, flags);
  } else {
    SafeGuard safe;
    accepted = configured_accept(fd, address, out != nullptr, flags);
  }
  if (accepted < 0 || !out) return accepted;
  const int64_t decoded = decode(address, *out);
  if (decoded < 0) {
    close(static_cast<int>(accepted));
    return decoded;
  }
  return accepted;
}

int64_t lang_net_send_v1(int32_t fd, const void *data, uint64_t count, int32_t flags) noexcept {
  return send_data(fd, data, count, flags, nullptr, 0);
}

int64_t lang_net_sendto_v1(int32_t fd, const void *data, uint64_t count, int32_t flags,
                           const LangNetAddress *in) noexcept {
  SocketAddress address;
  const int64_t status = encode(in, address);
  if (status < 0) return status;
  return send_data(fd, data, count, flags, address.get(), address.length);
}

int64_t lang_net_recvfrom_v1(int32_t fd, void *data, uint64_t count, int32_t flags,
                             LangNetAddress *out) noexcept {
  if (count > INT64_MAX) return -22;
  if (count && !data) return -14;
  const int64_t translated = message_flags(flags, false);
  if (translated < 0) return translated;
  SocketAddress address;
  address.length = sizeof(address.value);
  const int effective_flags = static_cast<int>(translated);
  const auto received = (flags & kTrunc)
      ? receive_truncated(fd, data, count, effective_flags, out ? &address : nullptr)
      : receive_data(fd, data, count, effective_flags, out ? &address : nullptr);
  if (received < 0 || !out) return received;
  const int64_t decoded = decode(address, *out);
  return decoded < 0 ? decoded : received;
}

int64_t lang_net_recv_v1(int32_t fd, void *data, uint64_t count, int32_t flags) noexcept {
  return lang_net_recvfrom_v1(fd, data, count, flags, nullptr);
}

int64_t lang_net_shutdown_v1(int32_t fd, int32_t how) noexcept {
  static_assert(SHUT_RD == 0 && SHUT_WR == 1 && SHUT_RDWR == 2);
  uint64_t last_watch = 0;
  if ((how == SHUT_RD || how == SHUT_RDWR) && lang_darwin_async_last_watch_v1)
    last_watch = lang_darwin_async_last_watch_v1();
  if (shutdown(fd, how) == 0) return 0;
  const int status = errno;
  // Darwin rejects shutdown on listeners and kqueue reports no EOF. Wake
  // the registered reader to check its state without changing the errno.
  if (status == ENOTCONN && last_watch && lang_darwin_async_notify_listener_shutdown_v1) {
    // Darwin's getsockopt does not expose SO_ACCEPTCONN. Its libproc socket
    // information reports the flag for both TCP and Unix listeners.
    socket_fdinfo info;
    const int size = static_cast<int>(sizeof(info));
    if (proc_pidfdinfo(getpid(), fd, PROC_PIDFDSOCKETINFO, &info, size) == size &&
        (info.psi.soi_options & SO_ACCEPTCONN))
      lang_darwin_async_notify_listener_shutdown_v1(fd, last_watch);
  }
  return darwin::error(status);
}

int64_t lang_net_getsockname_v1(int32_t fd, LangNetAddress *out) noexcept {
  return socket_name(fd, out, false);
}

int64_t lang_net_getpeername_v1(int32_t fd, LangNetAddress *out) noexcept {
  return socket_name(fd, out, true);
}

int64_t lang_net_setsockopt_int_v1(int32_t fd, int32_t level, int32_t name,
                                   int32_t value) noexcept {
  SocketOption option;
  if (!socket_option(level, name, option)) return -92;
  if (option.kind == OptionKind::Linger || option.kind == OptionKind::Timeval) return -22;
  return darwin::result(setsockopt(fd, option.level, option.name, &value, sizeof(value)));
}

int64_t lang_net_getsockopt_int_v1(int32_t fd, int32_t level, int32_t name,
                                   int32_t *out) noexcept {
  if (!out) return -14;
  SocketOption option;
  if (!socket_option(level, name, option)) return -92;
  if (option.kind == OptionKind::Linger || option.kind == OptionKind::Timeval) return -22;
  return read_option_int(fd, option, *out);
}

int64_t lang_net_setsockopt_bytes_v1(int32_t fd, int32_t level, int32_t name,
                                     const void *data, uint64_t length) noexcept {
  if (length > UINT32_MAX) return -22;
  if (length && !data) return -14;
  SocketOption option;
  if (!socket_option(level, name, option)) return -92;
  if (option.kind != OptionKind::Timeval)
    return darwin::result(setsockopt(fd, option.level, option.name, data,
                                    static_cast<socklen_t>(length)));
  if (length < sizeof(WoozyTimeval)) return -22;
  WoozyTimeval input;
  std::memcpy(&input, data, sizeof(input));
  if (input.microseconds < 0 || input.microseconds >= 1000000) return -33;
  const timeval time{static_cast<time_t>(input.seconds),
                     static_cast<suseconds_t>(input.microseconds)};
  return darwin::result(setsockopt(fd, option.level, option.name, &time, sizeof(time)));
}

int64_t lang_net_getsockopt_bytes_v1(int32_t fd, int32_t level, int32_t name,
                                     void *data, uint64_t *inout_length) noexcept {
  if (!inout_length) return -14;
  const auto length = *inout_length;
  if (length > INT32_MAX) return -22;
  if (length && !data) return -14;
  SocketOption option;
  if (!socket_option(level, name, option)) return -92;
  if (option.kind == OptionKind::Integer || option.kind == OptionKind::Boolean) {
    int32_t value;
    const auto status = read_option_int(fd, option, value);
    if (status < 0) return status;
    write_option_bytes(value, data, *inout_length);
  } else if (option.kind == OptionKind::Linger) {
    static_assert(sizeof(linger) == 2 * sizeof(int32_t) &&
                  offsetof(linger, l_linger) == sizeof(int32_t));
    linger value;
    const auto status = read_option(fd, option.level, option.name, value);
    if (status < 0) return status;
    write_option_bytes(value, data, *inout_length);
  } else {
    timeval value;
    const auto status = read_option(fd, option.level, option.name, value);
    if (status < 0) return status;
    write_option_bytes(WoozyTimeval{value.tv_sec, value.tv_usec}, data, *inout_length);
  }
  return 0;
}

int64_t lang_net_socketpair_v1(int32_t family, int32_t type, int32_t protocol,
                               int32_t descriptors[2]) noexcept {
  if (!descriptors) return -14;
  if (family != kUnix) return -97;
  const int kind = socket_type(type);
  if (kind < 0) return -22;
  const darwin::LaunchGuard guard;
  int fds[2];
  if (socketpair(AF_UNIX, kind, protocol, fds) < 0) return darwin::error(errno);
  for (const int fd : fds) {
    const int64_t configured = configure_fd(fd, type & kSocketFlags);
    if (configured < 0) {
      close(fds[0]);
      close(fds[1]);
      return configured;
    }
  }
  descriptors[0] = fds[0];
  descriptors[1] = fds[1];
  return 0;
}

int64_t lang_net_poll_v1(LangPollFd *rows, uint64_t count, int32_t timeout_ms) noexcept {
  static_assert(sizeof(LangPollFd) == sizeof(pollfd) && alignof(LangPollFd) == alignof(pollfd));
  static_assert(offsetof(LangPollFd, events) == offsetof(pollfd, events) &&
                offsetof(LangPollFd, revents) == offsetof(pollfd, revents));
  static_assert(POLLIN == 1 && POLLPRI == 2 && POLLOUT == 4 &&
                POLLERR == 8 && POLLHUP == 16 && POLLNVAL == 32);
  if (count > UINT32_MAX) return -22;
  if (count && !rows) return -14;
  constexpr int kEvents = POLLIN | POLLPRI | POLLOUT | POLLERR | POLLHUP | POLLNVAL;
  for (uint64_t i = 0; i < count; ++i) {
    if (rows[i].fd >= 0 && (rows[i].events & ~kEvents)) return -22;
  }
  SafeGuard safe;
  const int timeout = timeout_ms < 0 ? -1 : timeout_ms;
  return darwin::result(poll(reinterpret_cast<pollfd *>(rows), static_cast<nfds_t>(count), timeout));
}

int64_t lang_net_inet_pton_v1(int32_t family, const char *text, uint64_t length,
                              LangNetAddress *out) noexcept {
  if (!out) return -14;
  if (family != kInet && family != kInet6) return -97;
  if (!text && length) return -14;
  uint8_t bytes[16];
  int status;
  if (family == kInet) status = parse_ipv4(text, length, bytes);
  else status = parse_ipv6(text, length, bytes);
  if (status != 0) return -22;
  write_address(*out, family, 0, 0, bytes, family == kInet ? 4 : 16);
  return 0;
}

int64_t lang_net_inet_ntop_v1(const LangNetAddress *address, char *out, uint64_t capacity) noexcept {
  if (!address) return -14;
  if (address->family != kInet && address->family != kInet6) return -97;
  const LangVec *data = nullptr;
  const auto status = address_bytes(address, &data);
  if (status < 0) return status;
  if (!out) return -14;
  const auto *bytes = static_cast<const uint8_t *>(data->data);
  char text[64];
  const uint64_t length = address->family == kInet ? format_ipv4(bytes, text)
                                                   : format_ipv6(bytes, text);
  if (length >= capacity) return -28;
  std::memcpy(out, text, length);
  out[length] = '\0';
  return static_cast<int64_t>(length);
}

} // extern "C"
