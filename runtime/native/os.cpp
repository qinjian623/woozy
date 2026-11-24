#include "os.hpp"

#include "gc_program.hpp"
#include "linux.hpp"

#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) && defined(__linux__)

#if defined(__GNUC__) || defined(__clang__)
#define LANG_OS_NO_STACK_PROTECTOR __attribute__((no_stack_protector))
#else
#define LANG_OS_NO_STACK_PROTECTOR
#endif

namespace {

namespace linux_abi = lang::native::linux_x86_64;

// Local Linux x86_64 syscall numbers and the openat sentinel. Keeping these
// here avoids including kernel or libc headers in the freestanding runtime.
constexpr long kSysRead = 0;
constexpr long kSysWrite = 1;
constexpr long kSysClose = 3;
constexpr long kSysLseek = 8;
constexpr long kSysGetpid = 39;
constexpr long kSysMkdir = 83;
constexpr long kSysRmdir = 84;
constexpr long kSysUnlink = 87;
constexpr long kSysOpenat = 257;
constexpr long kSysMkdirat = 258;
constexpr long kSysUnlinkat = 263;
constexpr long kAtFdcwd = -100; // AT_FDCWD
constexpr long kAtRemoveDir = 0x200;

constexpr uint64_t kMaxPathBytes = 4096;
constexpr int64_t kErrFault = -14;       // EFAULT
constexpr int64_t kErrInval = -22;       // EINVAL
constexpr int64_t kErrNameTooLong = -36; // ENAMETOOLONG

static_assert(sizeof(long) == sizeof(int64_t),
              "Linux x86_64 syscall results must be 64-bit");

// The syscall instruction clobbers rcx and r11 and can observe or modify any
// caller memory through the kernel. Every wrapper keeps the memory clobber so
// the compiler cannot move buffer accesses across the call.
inline int64_t syscall0(long number) noexcept {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number)
                   : "rcx", "r11", "memory", "cc");
  return result;
}

inline int64_t syscall1(long number, long arg1) noexcept {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(arg1)
                   : "rcx", "r11", "memory", "cc");
  return result;
}

inline int64_t syscall2(long number, long arg1, long arg2) noexcept {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(arg1), "S"(arg2)
                   : "rcx", "r11", "memory", "cc");
  return result;
}

inline int64_t syscall3(long number, long arg1, long arg2, long arg3) noexcept {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
                   : "rcx", "r11", "memory", "cc");
  return result;
}

// x86_64 passes the fourth syscall argument in r10, which has no named
// constraint operand other than a register variable.
inline int64_t syscall4(long number, long arg1, long arg2, long arg3,
                        long arg4) noexcept {
  register long r10 __asm__("r10") = arg4;
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10)
                   : "rcx", "r11", "memory", "cc");
  return result;
}

// Copies exactly `length` borrowed bytes into caller-supplied stack storage,
// adding the terminating NUL at buffer[length]. The length check runs first so
// the terminator always fits, and no byte at or beyond `length` is read.
LANG_OS_NO_STACK_PROTECTOR
int64_t prepare_path(const char *data, uint64_t length,
                     char (&buffer)[kMaxPathBytes]) noexcept {
  if (length >= kMaxPathBytes) return kErrNameTooLong;
  if (length != 0 && data == nullptr) return kErrFault;

  for (uint64_t index = 0; index < length; ++index) {
    const char byte = data[index];
    if (byte == '\0') return kErrInval;
    buffer[index] = byte;
  }
  buffer[length] = '\0';
  return 0;
}

} // namespace

extern "C" {

int64_t lang_os_read_v1(int32_t fd, void *data, uint64_t count) noexcept {
  // The specified zero-count rule is unconditional: no descriptor check and no
  // buffer access.
  if (count == 0) return 0;
  // A byte buffer has no owning elements. Every graph write/validation is done
  // before this call; only the raw kernel transfer is SAFE.
  lang::native::gc_program::SafeGuard safe;
  return syscall3(kSysRead, static_cast<long>(fd),
                  static_cast<long>(reinterpret_cast<intptr_t>(data)),
                  static_cast<long>(count));
}

int64_t lang_os_write_v1(int32_t fd, const void *data,
                         uint64_t count) noexcept {
  if (count == 0) return 0;
  lang::native::gc_program::SafeGuard safe;
  return syscall3(kSysWrite, static_cast<long>(fd),
                  static_cast<long>(reinterpret_cast<intptr_t>(data)),
                  static_cast<long>(count));
}

int64_t lang_os_close_v1(int32_t fd) noexcept {
  return syscall1(kSysClose, static_cast<long>(fd));
}

LANG_OS_NO_STACK_PROTECTOR
int64_t lang_os_open_v1(const char *data, uint64_t byte_length, int32_t flags,
                        uint32_t mode) noexcept {
  char path[kMaxPathBytes];
  const int64_t prepared = prepare_path(data, byte_length, path);
  if (prepared < 0) return prepared;
  return syscall4(kSysOpenat, kAtFdcwd,
                  static_cast<long>(reinterpret_cast<intptr_t>(path)),
                  static_cast<long>(flags), static_cast<long>(mode));
}

LANG_OS_NO_STACK_PROTECTOR
int64_t lang_os_openat_v1(int32_t fd, const char *name, uint64_t length,
                          int32_t flags, uint32_t mode) noexcept {
  char path[linux_abi::kPathMax];
  const int64_t prepared = linux_abi::copy_component(name, length, path);
  if (prepared < 0) return prepared;
  return syscall4(kSysOpenat, static_cast<long>(fd),
                  static_cast<long>(reinterpret_cast<intptr_t>(path)),
                  static_cast<long>(flags), static_cast<long>(mode));
}

int64_t lang_os_seek_v1(int32_t fd, int64_t offset, int32_t whence) noexcept {
  return syscall3(kSysLseek, static_cast<long>(fd),
                  static_cast<long>(offset), static_cast<long>(whence));
}

LANG_OS_NO_STACK_PROTECTOR
int64_t lang_os_unlink_v1(const char *path_data,
                          uint64_t length) noexcept {
  char path[kMaxPathBytes];
  const int64_t prepared = prepare_path(path_data, length, path);
  if (prepared < 0) return prepared;
  return syscall1(kSysUnlink,
                  static_cast<long>(reinterpret_cast<intptr_t>(path)));
}

LANG_OS_NO_STACK_PROTECTOR
int64_t lang_os_mkdir_v1(const char *path_data, uint64_t length,
                         uint32_t mode) noexcept {
  char path[kMaxPathBytes];
  const int64_t prepared = prepare_path(path_data, length, path);
  if (prepared < 0) return prepared;
  return syscall2(kSysMkdir,
                  static_cast<long>(reinterpret_cast<intptr_t>(path)),
                  static_cast<long>(mode));
}

LANG_OS_NO_STACK_PROTECTOR
int64_t lang_os_mkdirat_v1(int32_t fd, const char *name, uint64_t length,
                           uint32_t mode) noexcept {
  char path[linux_abi::kPathMax];
  const int64_t prepared = linux_abi::copy_component(name, length, path);
  if (prepared < 0) return prepared;
  return syscall3(kSysMkdirat, static_cast<long>(fd),
                  static_cast<long>(reinterpret_cast<intptr_t>(path)),
                  static_cast<long>(mode));
}

LANG_OS_NO_STACK_PROTECTOR
int64_t lang_os_unlinkat_v1(int32_t fd, const char *name, uint64_t length,
                            int32_t kind) noexcept {
  if (kind != 0 && kind != 1) return kErrInval;
  char path[linux_abi::kPathMax];
  const int64_t prepared = linux_abi::copy_component(name, length, path);
  if (prepared < 0) return prepared;
  const long flags = kind == 1 ? kAtRemoveDir : 0;
  return syscall3(kSysUnlinkat, static_cast<long>(fd),
                  static_cast<long>(reinterpret_cast<intptr_t>(path)), flags);
}

LANG_OS_NO_STACK_PROTECTOR
int64_t lang_os_rmdir_v1(const char *path_data, uint64_t length) noexcept {
  char path[kMaxPathBytes];
  const int64_t prepared = prepare_path(path_data, length, path);
  if (prepared < 0) return prepared;
  return syscall1(kSysRmdir,
                  static_cast<long>(reinterpret_cast<intptr_t>(path)));
}

int64_t lang_os_getpid_v1() noexcept { return syscall0(kSysGetpid); }

[[noreturn]] void lang_os_exit_v1(int32_t status) noexcept {
  // 231 = exit_group, 60 = exit. The second call is unreachable on Linux but
  // mirrors the terminal fallback used by the other runtime entry points.
  __asm__ volatile("movl $231, %%eax\n\t"
                   "syscall\n\t"
                   "movl $60, %%eax\n\t"
                   "syscall\n\t"
                   "hlt"
                   :
                   : "D"(static_cast<long>(status))
                   : "rax", "rcx", "r11", "memory", "cc");
  __builtin_unreachable();
}

} // extern "C"

#undef LANG_OS_NO_STACK_PROTECTOR

#endif // Linux x86_64
