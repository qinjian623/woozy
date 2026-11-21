#include "network.hpp"

#include "gc_program.hpp"

#include "linux.hpp"

#include <cstdint>

#if defined(__x86_64__) && defined(__linux__)

// GCC at -O0 cannot see that kernel syscalls write through address buffers and
// reports a false -Wmaybe-uninitialized. The buffers are intentionally left
// uninitialised so a failing call never observes stale caller-visible values.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

namespace {

namespace linux_abi = lang::native::linux_x86_64;

// Linux x86_64 syscall numbers used by this translation unit.
constexpr long kSysPoll = 7;
constexpr long kSysSocket = 41;
constexpr long kSysConnect = 42;
constexpr long kSysSendto = 44;
constexpr long kSysRecvfrom = 45;
constexpr long kSysShutdown = 48;
constexpr long kSysBind = 49;
constexpr long kSysListen = 50;
constexpr long kSysGetsockname = 51;
constexpr long kSysGetpeername = 52;
constexpr long kSysSocketpair = 53;
constexpr long kSysSetsockopt = 54;
constexpr long kSysGetsockopt = 55;
constexpr long kSysAccept4 = 288;

constexpr int32_t kAfUnix = 1;
constexpr int32_t kAfInet = 2;
constexpr int32_t kAfInet6 = 10;
constexpr uint16_t kAddressFlagAbstractUnix = 1;
constexpr int32_t kMsgNoSignal = 0x4000;

constexpr uint64_t kMaxSignedCount = 0x7fffffffffffffffULL;
constexpr uint64_t kMaxUnsigned32 = 0xffffffffULL;
constexpr uint64_t kMaxSigned32 = 0x7fffffffULL;
constexpr uint64_t kSockaddrStorageBytes = 128;
constexpr uint64_t kUnixHeaderBytes = 2;
constexpr uint64_t kUnixPathMax = 108;
constexpr uint64_t kUnixDataMax = 107; // sun_path[108] minus NUL/abstract marker
constexpr uint64_t kInetAddressBytes = 4;
constexpr uint64_t kInet6AddressBytes = 16;
constexpr uint64_t kSockaddrInBytes = 16;
constexpr uint64_t kSockaddrIn6Bytes = 28;

constexpr int64_t kErrFault = -14;        // EFAULT
constexpr int64_t kErrInval = -22;        // EINVAL
constexpr int64_t kErrNoSpace = -28;      // ENOSPC
constexpr int64_t kErrNameTooLong = -36;  // ENAMETOOLONG
constexpr int64_t kErrAfNoSupport = -97;  // EAFNOSUPPORT

static_assert(sizeof(long) == 8, "Linux x86_64 long must be 64-bit");
static_assert(sizeof(void *) == 8, "Linux x86_64 pointers must be 64-bit");
static_assert(kUnixPathMax == kUnixDataMax + 1,
              "Unix data excludes one terminator or abstract NUL");
static_assert(kSockaddrStorageBytes >= kSockaddrIn6Bytes,
              "sockaddr storage must hold IPv6");
static_assert(kSockaddrStorageBytes >= kUnixHeaderBytes + kUnixPathMax,
              "sockaddr storage must hold Unix path");

inline long to_long(const void *pointer) noexcept {
  return static_cast<long>(reinterpret_cast<intptr_t>(pointer));
}

inline int64_t sys1(long number, long arg1) noexcept {
  return linux_abi::syscall6(number, arg1, 0, 0, 0, 0, 0);
}

inline int64_t sys2(long number, long arg1, long arg2) noexcept {
  return linux_abi::syscall6(number, arg1, arg2, 0, 0, 0, 0);
}

inline int64_t sys3(long number, long arg1, long arg2, long arg3) noexcept {
  return linux_abi::syscall6(number, arg1, arg2, arg3, 0, 0, 0);
}

inline int64_t sys4(long number, long arg1, long arg2, long arg3,
                    long arg4) noexcept {
  return linux_abi::syscall6(number, arg1, arg2, arg3, arg4, 0, 0);
}

inline int64_t sys5(long number, long arg1, long arg2, long arg3, long arg4,
                    long arg5) noexcept {
  return linux_abi::syscall6(number, arg1, arg2, arg3, arg4, arg5, 0);
}

inline int64_t sys6(long number, long arg1, long arg2, long arg3, long arg4,
                    long arg5, long arg6) noexcept {
  return linux_abi::syscall6(number, arg1, arg2, arg3, arg4, arg5, arg6);
}

inline void copy_bytes(uint8_t *destination, const uint8_t *source,
                       uint64_t count) noexcept {
  for (uint64_t index = 0; index < count; ++index)
    destination[index] = source[index];
}

// Linux x86_64 is little-endian. Address families, scope ids, and flow info are
// host-order fields; ports are network-order (big-endian).
inline void store_u16_native(uint8_t *destination, uint16_t value) noexcept {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
}

inline uint16_t load_u16_native(const uint8_t *source) noexcept {
  return static_cast<uint16_t>(source[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(source[1]) << 8);
}

inline void store_u16_be(uint8_t *destination, uint16_t value) noexcept {
  destination[0] = static_cast<uint8_t>(value >> 8);
  destination[1] = static_cast<uint8_t>(value);
}

inline uint16_t load_u16_be(const uint8_t *source) noexcept {
  return static_cast<uint16_t>(static_cast<uint16_t>(source[0]) << 8) |
         static_cast<uint16_t>(source[1]);
}

inline void store_u32_native(uint8_t *destination, uint32_t value) noexcept {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

inline uint32_t load_u32_native(const uint8_t *source) noexcept {
  return static_cast<uint32_t>(source[0]) |
         (static_cast<uint32_t>(source[1]) << 8) |
         (static_cast<uint32_t>(source[2]) << 16) |
         (static_cast<uint32_t>(source[3]) << 24);
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t validate_address(const LangNetAddress *address,
                         uint64_t *data_length) noexcept {
  if (address == nullptr) return kErrFault;
  if (address->reserved != 0) return kErrInval;
  if ((address->flags & static_cast<uint16_t>(~kAddressFlagAbstractUnix)) != 0)
    return kErrInval;
  if (address->family != kAfUnix &&
      (address->flags & kAddressFlagAbstractUnix) != 0)
    return kErrInval;

  LangVec *data = address->data;
  if (data == nullptr) return kErrInval;
  if (data->stride != 1) return kErrInval;
  if (data->initialized != data->length) return kErrInval;
  if (data->copy != nullptr || data->drop != nullptr) return kErrInval;
  if (data->length != 0 && data->data == nullptr) return kErrInval;

  const uint64_t length = data->length;
  switch (address->family) {
  case kAfInet:
    if (length != kInetAddressBytes) return kErrInval;
    break;
  case kAfInet6:
    if (length != kInet6AddressBytes) return kErrInval;
    break;
  case kAfUnix:
    if (length > kUnixDataMax) return kErrNameTooLong;
    if (length != 0 &&
        (address->flags & kAddressFlagAbstractUnix) == 0) {
      const uint8_t *bytes = static_cast<const uint8_t *>(data->data);
      for (uint64_t index = 0; index < length; ++index) {
        if (bytes[index] == 0) return kErrInval;
      }
    }
    break;
  default:
    return kErrAfNoSupport;
  }

  if (data_length != nullptr) *data_length = length;
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t build_kernel_address(const LangNetAddress *address,
                             uint8_t storage[kSockaddrStorageBytes],
                             uint32_t *storage_length) noexcept {
  uint64_t length = 0;
  const int64_t validated = validate_address(address, &length);
  if (validated < 0) return validated;

  const uint8_t *bytes = length == 0
                             ? nullptr
                             : static_cast<const uint8_t *>(address->data->data);

  store_u16_native(storage, static_cast<uint16_t>(address->family));
  switch (address->family) {
  case kAfInet:
    store_u16_be(storage + 2, address->port);
    copy_bytes(storage + 4, bytes, kInetAddressBytes);
    for (uint64_t index = 8; index < kSockaddrInBytes; ++index)
      storage[index] = 0;
    *storage_length = static_cast<uint32_t>(kSockaddrInBytes);
    return 0;
  case kAfInet6:
    store_u16_be(storage + 2, address->port);
    store_u32_native(storage + 4, 0); // flowinfo
    copy_bytes(storage + 8, bytes, kInet6AddressBytes);
    store_u32_native(storage + 24, address->scope);
    *storage_length = static_cast<uint32_t>(kSockaddrIn6Bytes);
    return 0;
  case kAfUnix:
    if (length == 0) {
      // Unnamed Unix address: only the family field is significant.
      *storage_length = static_cast<uint32_t>(kUnixHeaderBytes);
      return 0;
    }
    if ((address->flags & kAddressFlagAbstractUnix) != 0) {
      // Abstract namespace: leading NUL plus the name bytes.
      storage[kUnixHeaderBytes] = 0;
      copy_bytes(storage + kUnixHeaderBytes + 1, bytes, length);
      *storage_length = static_cast<uint32_t>(kUnixHeaderBytes + 1 + length);
    } else {
      copy_bytes(storage + kUnixHeaderBytes, bytes, length);
      storage[kUnixHeaderBytes + length] = 0;
      *storage_length = static_cast<uint32_t>(kUnixHeaderBytes + length + 1);
    }
    return 0;
  default:
    return kErrAfNoSupport;
  }
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t write_address_out(LangNetAddress *out, int32_t family, uint16_t port,
                          uint16_t flags, uint32_t scope, const uint8_t *bytes,
                          uint64_t length) noexcept {
  LangVec *data = lang_vec_new_v1(length, 1, nullptr, nullptr);
  for (uint64_t index = 0; index < length; ++index)
    lang_vec_init_v1(data, bytes + index);

  out->family = family;
  out->port = port;
  out->flags = flags;
  out->scope = scope;
  out->reserved = 0;
  out->data = data;
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t write_unspecified_address(LangNetAddress *out) noexcept {
  return write_address_out(out, 0, 0, 0, 0, nullptr, 0);
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t decode_kernel_address(const uint8_t *storage, uint32_t storage_length,
                              LangNetAddress *out) noexcept {
  if (storage_length > kSockaddrStorageBytes) return kErrInval;
  // A connected stream socket may return no address at all from recvfrom.
  if (storage_length < 2) return write_unspecified_address(out);

  const uint16_t family = load_u16_native(storage);
  switch (family) {
  case kAfInet: {
    if (storage_length < kSockaddrInBytes) return kErrInval;
    const uint16_t port = load_u16_be(storage + 2);
    return write_address_out(out, kAfInet, port, 0, 0, storage + 4,
                             kInetAddressBytes);
  }
  case kAfInet6: {
    if (storage_length < kSockaddrIn6Bytes) return kErrInval;
    const uint16_t port = load_u16_be(storage + 2);
    const uint32_t scope = load_u32_native(storage + 24);
    return write_address_out(out, kAfInet6, port, 0, scope, storage + 8,
                             kInet6AddressBytes);
  }
  case kAfUnix: {
    if (storage_length < kUnixHeaderBytes) return kErrInval;
    const uint64_t path_bytes = storage_length - kUnixHeaderBytes;
    if (path_bytes > kUnixPathMax) return kErrInval;
    if (path_bytes == 0) {
      return write_address_out(out, kAfUnix, 0, 0, 0, nullptr, 0);
    }
    if (storage[kUnixHeaderBytes] == 0) {
      // Abstract name, excluding the leading NUL.
      const uint64_t name_bytes = path_bytes - 1;
      return write_address_out(out, kAfUnix, 0, kAddressFlagAbstractUnix, 0,
                               storage + kUnixHeaderBytes + 1, name_bytes);
    }
    uint64_t name_bytes = 0;
    while (name_bytes < path_bytes &&
           storage[kUnixHeaderBytes + name_bytes] != 0)
      ++name_bytes;
    if (name_bytes > kUnixDataMax) return kErrNameTooLong;
    return write_address_out(out, kAfUnix, 0, 0, 0,
                             storage + kUnixHeaderBytes, name_bytes);
  }
  default:
    return kErrAfNoSupport;
  }
}

#include "ip_text.inc"

} // namespace

extern "C" {

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_socket_v1(int32_t family, int32_t type,
                           int32_t protocol) noexcept {
  return sys3(kSysSocket, static_cast<long>(family), static_cast<long>(type),
              static_cast<long>(protocol));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_bind_v1(int32_t fd, const LangNetAddress *address) noexcept {
  alignas(8) uint8_t storage[kSockaddrStorageBytes];
  uint32_t length = 0;
  const int64_t built = build_kernel_address(address, storage, &length);
  if (built < 0) return built;
  return sys3(kSysBind, static_cast<long>(fd), to_long(storage),
              static_cast<long>(length));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_connect_v1(int32_t fd,
                            const LangNetAddress *address) noexcept {
  alignas(8) uint8_t storage[kSockaddrStorageBytes];
  uint32_t length = 0;
  const int64_t built = build_kernel_address(address, storage, &length);
  if (built < 0) return built;
  lang::native::gc_program::SafeGuard safe;
  return sys3(kSysConnect, static_cast<long>(fd), to_long(storage),
              static_cast<long>(length));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_listen_v1(int32_t fd, int32_t backlog) noexcept {
  return sys2(kSysListen, static_cast<long>(fd), static_cast<long>(backlog));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_accept_v1(int32_t fd, LangNetAddress *out,
                           uint32_t flags) noexcept {
  alignas(8) uint8_t storage[kSockaddrStorageBytes];
  uint32_t length = kSockaddrStorageBytes;
  int64_t accepted;
  {
    lang::native::gc_program::SafeGuard safe;
    accepted = out == nullptr
                   ? sys4(kSysAccept4, static_cast<long>(fd), 0, 0,
                          static_cast<long>(flags))
                   : sys4(kSysAccept4, static_cast<long>(fd), to_long(storage),
                          to_long(&length), static_cast<long>(flags));
  }
  if (accepted < 0) return accepted;
  if (out != nullptr) {
    const int64_t decoded = decode_kernel_address(storage, length, out);
    if (decoded < 0) {
      sys1(3, accepted); // close the descriptor if the peer address was invalid
      return decoded;
    }
  }
  return accepted;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_send_v1(int32_t fd, const void *data, uint64_t count,
                         int32_t flags) noexcept {
  if (count > kMaxSignedCount) return kErrInval;
  if (count != 0 && data == nullptr) return kErrFault;
  const int32_t effective_flags = flags | kMsgNoSignal;
  lang::native::gc_program::SafeGuard safe;
  return sys6(kSysSendto, static_cast<long>(fd), to_long(data),
              static_cast<long>(count), static_cast<long>(effective_flags), 0,
              0);
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_recv_v1(int32_t fd, void *data, uint64_t count,
                         int32_t flags) noexcept {
  if (count > kMaxSignedCount) return kErrInval;
  if (count != 0 && data == nullptr) return kErrFault;
  lang::native::gc_program::SafeGuard safe;
  return sys6(kSysRecvfrom, static_cast<long>(fd), to_long(data),
              static_cast<long>(count), static_cast<long>(flags), 0, 0);
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_sendto_v1(int32_t fd, const void *data, uint64_t count,
                           int32_t flags,
                           const LangNetAddress *address) noexcept {
  alignas(8) uint8_t storage[kSockaddrStorageBytes];
  uint32_t length = 0;
  const int64_t built = build_kernel_address(address, storage, &length);
  if (built < 0) return built;
  if (count > kMaxSignedCount) return kErrInval;
  if (count != 0 && data == nullptr) return kErrFault;
  const int32_t effective_flags = flags | kMsgNoSignal;
  lang::native::gc_program::SafeGuard safe;
  return sys6(kSysSendto, static_cast<long>(fd), to_long(data),
              static_cast<long>(count), static_cast<long>(effective_flags),
              to_long(storage), static_cast<long>(length));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_recvfrom_v1(int32_t fd, void *data, uint64_t count,
                             int32_t flags, LangNetAddress *out) noexcept {
  if (count > kMaxSignedCount) return kErrInval;
  if (count != 0 && data == nullptr) return kErrFault;

  alignas(8) uint8_t storage[kSockaddrStorageBytes];
  uint32_t length = kSockaddrStorageBytes;
  int64_t received;
  {
    lang::native::gc_program::SafeGuard safe;
    received = out == nullptr
                   ? sys6(kSysRecvfrom, static_cast<long>(fd), to_long(data),
                          static_cast<long>(count), static_cast<long>(flags), 0,
                          0)
                   : sys6(kSysRecvfrom, static_cast<long>(fd), to_long(data),
                          static_cast<long>(count), static_cast<long>(flags),
                          to_long(storage), to_long(&length));
  }
  if (received < 0) return received;
  if (out != nullptr) {
    const int64_t decoded = decode_kernel_address(storage, length, out);
    if (decoded < 0) return decoded;
  }
  return received;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_shutdown_v1(int32_t fd, int32_t how) noexcept {
  return sys2(kSysShutdown, static_cast<long>(fd), static_cast<long>(how));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_getsockname_v1(int32_t fd, LangNetAddress *out) noexcept {
  if (out == nullptr) return kErrFault;
  alignas(8) uint8_t storage[kSockaddrStorageBytes];
  uint32_t length = kSockaddrStorageBytes;
  const int64_t result = sys3(kSysGetsockname, static_cast<long>(fd),
                              to_long(storage), to_long(&length));
  if (result < 0) return result;
  return decode_kernel_address(storage, length, out);
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_getpeername_v1(int32_t fd, LangNetAddress *out) noexcept {
  if (out == nullptr) return kErrFault;
  alignas(8) uint8_t storage[kSockaddrStorageBytes];
  uint32_t length = kSockaddrStorageBytes;
  const int64_t result = sys3(kSysGetpeername, static_cast<long>(fd),
                              to_long(storage), to_long(&length));
  if (result < 0) return result;
  return decode_kernel_address(storage, length, out);
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_setsockopt_int_v1(int32_t fd, int32_t level, int32_t option,
                                   int32_t value) noexcept {
  int32_t local = value;
  return sys5(kSysSetsockopt, static_cast<long>(fd), static_cast<long>(level),
              static_cast<long>(option), to_long(&local),
              static_cast<long>(sizeof(local)));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_getsockopt_int_v1(int32_t fd, int32_t level, int32_t option,
                                   int32_t *out) noexcept {
  if (out == nullptr) return kErrFault;
  int32_t local = 0;
  int32_t length = static_cast<int32_t>(sizeof(local));
  const int64_t result = sys5(kSysGetsockopt, static_cast<long>(fd),
                              static_cast<long>(level),
                              static_cast<long>(option), to_long(&local),
                              to_long(&length));
  if (result < 0) return result;
  if (length != static_cast<int32_t>(sizeof(local))) return kErrInval;
  *out = local;
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_setsockopt_bytes_v1(int32_t fd, int32_t level,
                                     int32_t option, const void *data,
                                     uint64_t length) noexcept {
  if (length > kMaxUnsigned32) return kErrInval;
  if (length != 0 && data == nullptr) return kErrFault;
  return sys5(kSysSetsockopt, static_cast<long>(fd), static_cast<long>(level),
              static_cast<long>(option), to_long(data),
              static_cast<long>(length));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_getsockopt_bytes_v1(int32_t fd, int32_t level,
                                     int32_t option, void *data,
                                     uint64_t *inout_length) noexcept {
  if (inout_length == nullptr) return kErrFault;
  const uint64_t requested = *inout_length;
  if (requested > kMaxSigned32) return kErrInval;
  if (requested != 0 && data == nullptr) return kErrFault;

  int32_t length = static_cast<int32_t>(requested);
  const int64_t result = sys5(kSysGetsockopt, static_cast<long>(fd),
                              static_cast<long>(level),
                              static_cast<long>(option), to_long(data),
                              to_long(&length));
  if (result < 0) return result;
  if (length < 0) return kErrInval;
  *inout_length = static_cast<uint64_t>(length);
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_socketpair_v1(int32_t family, int32_t type, int32_t protocol,
                               int32_t descriptors[2]) noexcept {
  if (descriptors == nullptr) return kErrFault;
  int32_t local[2];
  const int64_t result =
      sys4(kSysSocketpair, static_cast<long>(family), static_cast<long>(type),
           static_cast<long>(protocol), to_long(local));
  if (result < 0) return result;
  descriptors[0] = local[0];
  descriptors[1] = local[1];
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_poll_v1(LangPollFd *rows, uint64_t count,
                         int32_t timeout_ms) noexcept {
  if (count > kMaxUnsigned32) return kErrInval;
  if (count != 0 && rows == nullptr) return kErrFault;
  lang::native::gc_program::SafeGuard safe;
  return sys3(kSysPoll, to_long(rows), static_cast<long>(count),
              static_cast<long>(timeout_ms));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_inet_pton_v1(int32_t family, const char *text,
                              uint64_t length,
                              LangNetAddress *out) noexcept {
  if (out == nullptr) return kErrFault;
  if (family != kAfInet && family != kAfInet6) return kErrAfNoSupport;
  if (text == nullptr) return length == 0 ? kErrInval : kErrFault;
  if (length == 0) return kErrInval;

  if (family == kAfInet) {
    uint8_t bytes[kInetAddressBytes];
    if (parse_ipv4(text, length, bytes) != 0) return kErrInval;
    return write_address_out(out, kAfInet, 0, 0, 0, bytes,
                             kInetAddressBytes);
  }

  uint8_t bytes[kInet6AddressBytes];
  if (parse_ipv6(text, length, bytes) != 0) return kErrInval;
  return write_address_out(out, kAfInet6, 0, 0, 0, bytes,
                           kInet6AddressBytes);
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_net_inet_ntop_v1(const LangNetAddress *address, char *out,
                              uint64_t capacity) noexcept {
  if (address == nullptr) return kErrFault;
  if (address->family != kAfInet && address->family != kAfInet6)
    return kErrAfNoSupport;
  uint64_t length = 0;
  const int64_t validated = validate_address(address, &length);
  if (validated < 0) return validated;
  if (out == nullptr) return kErrFault;
  if (capacity == 0) return kErrNoSpace;

  const uint8_t *bytes = static_cast<const uint8_t *>(address->data->data);
  char text[64];
  const uint64_t text_length =
      address->family == kAfInet ? format_ipv4(bytes, text)
                                 : format_ipv6(bytes, text);
  if (text_length + 1 > capacity) return kErrNoSpace;
  copy_bytes(reinterpret_cast<uint8_t *>(out),
             reinterpret_cast<const uint8_t *>(text), text_length);
  out[text_length] = '\0';
  return static_cast<int64_t>(text_length);
}

} // extern "C"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif // Linux x86_64
