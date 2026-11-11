#ifndef LANG_RUNTIME_NATIVE_NETWORK_HPP
#define LANG_RUNTIME_NATIVE_NETWORK_HPP

#include <cstdint>

#include "vec.hpp"

// Private native network primitive ABI: lang-native-network/1.
//
// These entry points back the Woozy standard-library socket module on Linux
// x86_64. They are runtime internals, not a user-visible raw socket API:
// source programs use typed Address values and `throws os.Error`; lowering
// translates failures from these functions.
//
// LangNetAddress is the frozen 24-byte private address value:
//   family    @0  int32_t
//   port      @4  uint16_t   host byte order
//   flags     @6  uint16_t   bit 0 = abstract Unix namespace
//   scope     @8  uint32_t   host byte order (IPv6 scope id)
//   reserved  @12 uint32_t   must be zero on input
//   data      @16 LangVec*
//
// `data` is an owned Vec[u8] on output and a borrowed Vec[u8] for the duration
// of an input call. Its element stride is 1, its initialized count equals its
// length, and copy/drop callbacks are null. The payload is:
//   * IPv4 (family 2):  exactly 4 bytes in network byte order.
//   * IPv6 (family 10): exactly 16 bytes in network byte order.
//   * Unix (family 1): pathname bytes excluding the terminating NUL, or the
//     abstract name bytes excluding the leading NUL when flags bit 0 is set.
//     Length 0 is an unnamed Unix address.
// Input validation rejects a non-zero reserved field, unknown flag bits, an
// unsupported family, non-byte stride, partially initialized data, non-null
// copy/drop callbacks, length mismatch, a missing payload for non-empty data,
// a pathname containing an embedded NUL, and Unix names longer than 107 bytes
// (the 108-byte kernel sun_path reserves one byte for the leading abstract NUL
// or the pathname terminator).
//
// Output addresses are allocated as a fresh Vec owned by the caller. The
// address value is written only when the corresponding API succeeds; kernel
// address buffers are local 128-byte arrays whose returned length is checked
// before decoding. An allocation failure is a language Panic, not an errno.
// When recvfrom receives no kernel source address (for example on a connected
// stream or from an unbound Unix sender), its nullable `out` is initialized as
// an unspecified family-0 address with an empty data Vec. Input validation
// still accepts only families 1/2/10.
//
// Error ABI
// ---------
// Every int64_t-returning function reports either a documented non-negative
// success value or a raw negative Linux errno in [-4095, -1]. Callers test
// `result < 0`; there is no global errno, no EINTR retry, and no host
// inet/getaddrinfo path.
//
// send/sendto add MSG_NOSIGNAL to the caller's flags by default so a broken
// peer cannot deliver a process-killing SIGPIPE; all other caller flag bits
// are preserved. recv/send with count 0 deliberately reach the kernel so
// zero-length datagrams keep their datagram semantics; a null data pointer is
// accepted only when count is 0. Counts greater than INT64_MAX return -EINVAL.
//
// inet_pton accepts only family 2/10 and returns 0 on success. It parses
// exactly `length` bytes (no NUL assumption), rejects bracketed/zone forms,
// and returns -EINVAL for malformed text or -EAFNOSUPPORT for other families.
// inet_ntop emits canonical lowercase IPv6 with the longest zero run
// compressed (first run on ties). IPv4-mapped IPv6 addresses (::ffff:0:0/96)
// are emitted as `::ffff:d.d.d.d`; all other IPv6 values use hexadecimal
// groups. The IPv6 scope is a separate field and is not appended as `%scope`.
// inet_ntop returns the byte count excluding the terminating NUL, -ENOSPC when
// the capacity cannot hold the text plus NUL, and writes no partial output.
//
// DNS/hostname lookup is not part of this ABI.

struct LangNetAddress {
  int32_t family;
  uint16_t port;
  uint16_t flags;
  uint32_t scope;
  uint32_t reserved;
  LangVec *data;
};

struct LangPollFd {
  int32_t fd;
  int16_t events;
  int16_t revents;
};

static_assert(sizeof(LangNetAddress) == 24, "LangNetAddress must be 24 bytes");
static_assert(__builtin_offsetof(LangNetAddress, family) == 0,
              "LangNetAddress.family offset");
static_assert(__builtin_offsetof(LangNetAddress, port) == 4,
              "LangNetAddress.port offset");
static_assert(__builtin_offsetof(LangNetAddress, flags) == 6,
              "LangNetAddress.flags offset");
static_assert(__builtin_offsetof(LangNetAddress, scope) == 8,
              "LangNetAddress.scope offset");
static_assert(__builtin_offsetof(LangNetAddress, reserved) == 12,
              "LangNetAddress.reserved offset");
static_assert(__builtin_offsetof(LangNetAddress, data) == 16,
              "LangNetAddress.data offset");

static_assert(sizeof(LangPollFd) == 8, "LangPollFd must be 8 bytes");
static_assert(__builtin_offsetof(LangPollFd, fd) == 0,
              "LangPollFd.fd offset");
static_assert(__builtin_offsetof(LangPollFd, events) == 4,
              "LangPollFd.events offset");
static_assert(__builtin_offsetof(LangPollFd, revents) == 6,
              "LangPollFd.revents offset");

extern "C" {

// socket(2). Returns the new descriptor.
int64_t lang_net_socket_v1(int32_t family, int32_t type,
                           int32_t protocol) noexcept;

// bind(2)/connect(2). The address is borrowed for this call only.
int64_t lang_net_bind_v1(int32_t fd, const LangNetAddress *address) noexcept;
int64_t lang_net_connect_v1(int32_t fd,
                            const LangNetAddress *address) noexcept;

// listen(2).
int64_t lang_net_listen_v1(int32_t fd, int32_t backlog) noexcept;

// accept4(2). `out` may be null when the peer address is unwanted; when it is
// non-null the peer address is decoded into a fresh owned Vec only on success.
// `flags` is the kernel accept4 flag set (SOCK_CLOEXEC/SOCK_NONBLOCK).
int64_t lang_net_accept_v1(int32_t fd, LangNetAddress *out,
                           uint32_t flags) noexcept;

// send(2)/recv(2). send adds MSG_NOSIGNAL internally. A zero count still calls
// the kernel, so a zero-length datagram is transmitted or received.
int64_t lang_net_send_v1(int32_t fd, const void *data, uint64_t count,
                         int32_t flags) noexcept;
int64_t lang_net_recv_v1(int32_t fd, void *data, uint64_t count,
                         int32_t flags) noexcept;

// sendto(2)/recvfrom(2). sendto adds MSG_NOSIGNAL internally. `out` may be
// null when the datagram source address is unwanted.
int64_t lang_net_sendto_v1(int32_t fd, const void *data, uint64_t count,
                           int32_t flags,
                           const LangNetAddress *address) noexcept;
int64_t lang_net_recvfrom_v1(int32_t fd, void *data, uint64_t count,
                             int32_t flags, LangNetAddress *out) noexcept;

// shutdown(2).
int64_t lang_net_shutdown_v1(int32_t fd, int32_t how) noexcept;

// getsockname(2)/getpeername(2). `out` must be non-null and is populated only
// on success.
int64_t lang_net_getsockname_v1(int32_t fd, LangNetAddress *out) noexcept;
int64_t lang_net_getpeername_v1(int32_t fd, LangNetAddress *out) noexcept;

// setsockopt(2)/getsockopt(2) integer helpers. getsockopt_int writes `out` only
// on success and requires the kernel to report a complete 4-byte value.
int64_t lang_net_setsockopt_int_v1(int32_t fd, int32_t level, int32_t option,
                                   int32_t value) noexcept;
int64_t lang_net_getsockopt_int_v1(int32_t fd, int32_t level, int32_t option,
                                   int32_t *out) noexcept;

// setsockopt(2)/getsockopt(2) byte helpers. getsockopt_bytes receives the
// buffer capacity in *inout_length and writes the actual length back only on
// success. A null data pointer is accepted only for a zero length.
int64_t lang_net_setsockopt_bytes_v1(int32_t fd, int32_t level,
                                     int32_t option, const void *data,
                                     uint64_t length) noexcept;
int64_t lang_net_getsockopt_bytes_v1(int32_t fd, int32_t level,
                                     int32_t option, void *data,
                                     uint64_t *inout_length) noexcept;

// socketpair(2). Both descriptors are written only on success.
int64_t lang_net_socketpair_v1(int32_t family, int32_t type,
                               int32_t protocol,
                               int32_t descriptors[2]) noexcept;

// poll(2). LangPollFd has the exact kernel pollfd layout. A zero count with a
// null rows pointer is a valid timeout-only poll. `count` must fit the
// kernel's unsigned int nfds argument.
int64_t lang_net_poll_v1(LangPollFd *rows, uint64_t count,
                         int32_t timeout_ms) noexcept;

// IP literal conversion. DNS/hostname resolution is deliberately absent.
int64_t lang_net_inet_pton_v1(int32_t family, const char *text,
                              uint64_t length,
                              LangNetAddress *out) noexcept;
int64_t lang_net_inet_ntop_v1(const LangNetAddress *address, char *out,
                              uint64_t capacity) noexcept;

} // extern "C"

#endif // LANG_RUNTIME_NATIVE_NETWORK_HPP
