#include "system.hpp"

#include "gc_program.hpp"

#include "linux.hpp"

#include <cstdint>

#if defined(__x86_64__) && defined(__linux__)

// GCC at -O0 cannot see that the kernel writes through the output buffers
// passed to syscall6 and reports a false -Wmaybe-uninitialized at the call.
// The buffers are intentionally left uninitialised so that a failing syscall
// never causes the runtime to read or write stale caller-visible values.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

namespace {

namespace linux_abi = lang::native::linux_x86_64;

// Linux x86_64 syscall numbers used by this translation unit. They are kept
// local so the runtime does not include kernel or libc headers.
constexpr long kSysStat = 4;
constexpr long kSysFstat = 5;
constexpr long kSysLstat = 6;
constexpr long kSysAccess = 21;
constexpr long kSysDup = 32;
constexpr long kSysDup2 = 33;
constexpr long kSysNanosleep = 35;
constexpr long kSysKill = 62;
constexpr long kSysFsync = 74;
constexpr long kSysFdatasync = 75;
constexpr long kSysTruncate = 76;
constexpr long kSysFtruncate = 77;
constexpr long kSysGetcwd = 79;
constexpr long kSysChdir = 80;
constexpr long kSysRename = 82;
constexpr long kSysLink = 86;
constexpr long kSysSymlink = 88;
constexpr long kSysReadlink = 89;
constexpr long kSysChmod = 90;
constexpr long kSysFchmod = 91;
constexpr long kSysGetuid = 102;
constexpr long kSysGetgid = 104;
constexpr long kSysGeteuid = 107;
constexpr long kSysGetegid = 108;
constexpr long kSysSetpgid = 109;
constexpr long kSysGetppid = 110;
constexpr long kSysSetsid = 112;
constexpr long kSysGetpgid = 121;
constexpr long kSysGettid = 186;
constexpr long kSysGetdents64 = 217;
constexpr long kSysClockGettime = 228;
constexpr long kSysMknodat = 259;
constexpr long kSysPipe2 = 293;
constexpr long kSysGetrandom = 318;

constexpr long kAtFdcwd = -100; // AT_FDCWD
constexpr uint32_t kFileTypeFifo = 0010000u;
constexpr uint32_t kModePermissionMask = 07777u;
constexpr uint64_t kMaxSignedCount = 0x7fffffffffffffffULL;

// Linux x86_64 kernel `struct stat`. It is 144 bytes and is deliberately
// declared here rather than obtained from host headers: the runtime is
// freestanding and the kernel ABI is part of this platform contract.
struct KernelStat {
  uint64_t device;
  uint64_t inode;
  uint64_t links;
  uint32_t mode;
  uint32_t uid;
  uint32_t gid;
  uint32_t padding;
  uint64_t rdev;
  int64_t size;
  int64_t block_size;
  int64_t blocks;
  int64_t accessed_seconds;
  int64_t accessed_nanoseconds;
  int64_t modified_seconds;
  int64_t modified_nanoseconds;
  int64_t changed_seconds;
  int64_t changed_nanoseconds;
  int64_t unused[3];
};

static_assert(sizeof(KernelStat) == 144, "Linux x86_64 kernel stat size");
static_assert(__builtin_offsetof(KernelStat, device) == 0,
              "kernel stat device offset");
static_assert(__builtin_offsetof(KernelStat, inode) == 8,
              "kernel stat inode offset");
static_assert(__builtin_offsetof(KernelStat, links) == 16,
              "kernel stat links offset");
static_assert(__builtin_offsetof(KernelStat, mode) == 24,
              "kernel stat mode offset");
static_assert(__builtin_offsetof(KernelStat, uid) == 28,
              "kernel stat uid offset");
static_assert(__builtin_offsetof(KernelStat, gid) == 32,
              "kernel stat gid offset");
static_assert(__builtin_offsetof(KernelStat, padding) == 36,
              "kernel stat padding offset");
static_assert(__builtin_offsetof(KernelStat, rdev) == 40,
              "kernel stat rdev offset");
static_assert(__builtin_offsetof(KernelStat, size) == 48,
              "kernel stat size offset");
static_assert(__builtin_offsetof(KernelStat, block_size) == 56,
              "kernel stat block_size offset");
static_assert(__builtin_offsetof(KernelStat, blocks) == 64,
              "kernel stat blocks offset");
static_assert(__builtin_offsetof(KernelStat, accessed_seconds) == 72,
              "kernel stat atime offset");
static_assert(__builtin_offsetof(KernelStat, accessed_nanoseconds) == 80,
              "kernel stat atime_nsec offset");
static_assert(__builtin_offsetof(KernelStat, modified_seconds) == 88,
              "kernel stat mtime offset");
static_assert(__builtin_offsetof(KernelStat, modified_nanoseconds) == 96,
              "kernel stat mtime_nsec offset");
static_assert(__builtin_offsetof(KernelStat, changed_seconds) == 104,
              "kernel stat ctime offset");
static_assert(__builtin_offsetof(KernelStat, changed_nanoseconds) == 112,
              "kernel stat ctime_nsec offset");
static_assert(__builtin_offsetof(KernelStat, unused) == 120,
              "kernel stat unused offset");

// Linux x86_64 kernel `struct timespec`, the same two-int64 layout as
// LangTimespec but kept separate so the kernel buffer is explicit.
struct KernelTimespec {
  int64_t seconds;
  int64_t nanoseconds;
};

static_assert(sizeof(KernelTimespec) == 16, "kernel timespec size");
static_assert(__builtin_offsetof(KernelTimespec, seconds) == 0,
              "kernel timespec seconds offset");
static_assert(__builtin_offsetof(KernelTimespec, nanoseconds) == 8,
              "kernel timespec nanoseconds offset");

inline long to_long(const void *pointer) noexcept {
  return static_cast<long>(reinterpret_cast<intptr_t>(pointer));
}

inline int64_t sys0(long number) noexcept {
  return linux_abi::syscall6(number, 0, 0, 0, 0, 0, 0);
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

// Converts a successfully filled kernel stat into the stable public layout.
LANG_NATIVE_NO_STACK_PROTECTOR
inline void fill_file_stat(LangFileStat *out, const KernelStat &value) noexcept {
  out->device = value.device;
  out->inode = value.inode;
  out->links = value.links;
  out->mode = value.mode;
  out->uid = value.uid;
  out->gid = value.gid;
  out->padding = 0;
  out->rdev = value.rdev;
  out->size = value.size;
  out->block_size = value.block_size;
  out->blocks = value.blocks;
  out->accessed.seconds = value.accessed_seconds;
  out->accessed.nanoseconds = value.accessed_nanoseconds;
  out->modified.seconds = value.modified_seconds;
  out->modified.nanoseconds = value.modified_nanoseconds;
  out->changed.seconds = value.changed_seconds;
  out->changed.nanoseconds = value.changed_nanoseconds;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t stat_common(long number, const char *path, uint64_t length,
                    LangFileStat *out) noexcept {
  char prepared[linux_abi::kPathMax];
  const int64_t prepared_result = linux_abi::copy_path(path, length, prepared);
  if (prepared_result < 0) return prepared_result;
  if (out == nullptr) return linux_abi::kErrorFault;

  KernelStat kernel;
  const int64_t result = sys2(number, to_long(prepared), to_long(&kernel));
  if (result < 0) return result;

  fill_file_stat(out, kernel);
  return 0;
}

} // namespace

extern "C" {

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_stat_v1(const char *path, uint64_t length,
                         LangFileStat *out) noexcept {
  return stat_common(kSysStat, path, length, out);
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_lstat_v1(const char *path, uint64_t length,
                          LangFileStat *out) noexcept {
  return stat_common(kSysLstat, path, length, out);
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_fstat_v1(int32_t fd, LangFileStat *out) noexcept {
  if (out == nullptr) return linux_abi::kErrorFault;

  KernelStat kernel;
  const int64_t result = sys2(kSysFstat, static_cast<long>(fd),
                              to_long(&kernel));
  if (result < 0) return result;

  fill_file_stat(out, kernel);
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_rename_v1(const char *old_path, uint64_t old_length,
                           const char *new_path,
                           uint64_t new_length) noexcept {
  char old_prepared[linux_abi::kPathMax];
  char new_prepared[linux_abi::kPathMax];

  int64_t prepared = linux_abi::copy_path(old_path, old_length, old_prepared);
  if (prepared < 0) return prepared;
  prepared = linux_abi::copy_path(new_path, new_length, new_prepared);
  if (prepared < 0) return prepared;

  return sys2(kSysRename, to_long(old_prepared), to_long(new_prepared));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_chmod_v1(const char *path, uint64_t length,
                          uint32_t mode) noexcept {
  char prepared[linux_abi::kPathMax];
  const int64_t prepared_result = linux_abi::copy_path(path, length, prepared);
  if (prepared_result < 0) return prepared_result;

  return sys2(kSysChmod, to_long(prepared), static_cast<long>(mode));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_fchmod_v1(int32_t fd, uint32_t mode) noexcept {
  return sys2(kSysFchmod, static_cast<long>(fd), static_cast<long>(mode));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_chdir_v1(const char *path, uint64_t length) noexcept {
  char prepared[linux_abi::kPathMax];
  const int64_t prepared_result = linux_abi::copy_path(path, length, prepared);
  if (prepared_result < 0) return prepared_result;

  return sys1(kSysChdir, to_long(prepared));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_getcwd_v1(char *buffer, uint64_t capacity) noexcept {
  // The kernel cannot produce a path into zero capacity. Check this before the
  // null-pointer rule so a zero-capacity call is deterministic and never
  // touches the buffer.
  if (capacity == 0) return linux_abi::kErrorInval;
  if (capacity > kMaxSignedCount) return linux_abi::kErrorInval;
  if (buffer == nullptr) return linux_abi::kErrorFault;

  const int64_t result =
      sys2(kSysGetcwd, to_long(buffer), static_cast<long>(capacity));
  if (result < 0) return result;

  // Linux reports the length including the terminating NUL. The native ABI
  // reports the byte count excluding it.
  return result == 0 ? 0 : result - 1;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_getdents_v1(int32_t fd, void *buffer,
                             uint64_t capacity) noexcept {
  // A zero count is a successful no-op and must not touch the buffer.
  if (capacity == 0) return 0;
  if (capacity > kMaxSignedCount) return linux_abi::kErrorInval;
  if (buffer == nullptr) return linux_abi::kErrorFault;

  return sys3(kSysGetdents64, static_cast<long>(fd), to_long(buffer),
              static_cast<long>(capacity));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_readlink_v1(const char *path, uint64_t length, void *buffer,
                             uint64_t capacity) noexcept {
  char prepared[linux_abi::kPathMax];
  const int64_t prepared_result = linux_abi::copy_path(path, length, prepared);
  if (prepared_result < 0) return prepared_result;

  // A zero count is a successful no-op and must not touch the buffer, even
  // when the pointer is null.
  if (capacity == 0) return 0;
  if (capacity > kMaxSignedCount) return linux_abi::kErrorInval;
  if (buffer == nullptr) return linux_abi::kErrorFault;

  return sys3(kSysReadlink, to_long(prepared), to_long(buffer),
              static_cast<long>(capacity));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_symlink_v1(const char *target, uint64_t target_length,
                            const char *link_path,
                            uint64_t link_length) noexcept {
  char target_prepared[linux_abi::kPathMax];
  char link_prepared[linux_abi::kPathMax];

  int64_t prepared =
      linux_abi::copy_path(target, target_length, target_prepared);
  if (prepared < 0) return prepared;
  prepared = linux_abi::copy_path(link_path, link_length, link_prepared);
  if (prepared < 0) return prepared;

  return sys2(kSysSymlink, to_long(target_prepared), to_long(link_prepared));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_link_v1(const char *old_path, uint64_t old_length,
                         const char *new_path,
                         uint64_t new_length) noexcept {
  char old_prepared[linux_abi::kPathMax];
  char new_prepared[linux_abi::kPathMax];

  int64_t prepared = linux_abi::copy_path(old_path, old_length, old_prepared);
  if (prepared < 0) return prepared;
  prepared = linux_abi::copy_path(new_path, new_length, new_prepared);
  if (prepared < 0) return prepared;

  return sys2(kSysLink, to_long(old_prepared), to_long(new_prepared));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_truncate_v1(const char *path, uint64_t length,
                             int64_t size) noexcept {
  char prepared[linux_abi::kPathMax];
  const int64_t prepared_result = linux_abi::copy_path(path, length, prepared);
  if (prepared_result < 0) return prepared_result;

  return sys2(kSysTruncate, to_long(prepared), static_cast<long>(size));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_ftruncate_v1(int32_t fd, int64_t size) noexcept {
  return sys2(kSysFtruncate, static_cast<long>(fd), static_cast<long>(size));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_flock_v1(int32_t fd, int32_t operation) noexcept {
  // Linux x86_64 __NR_flock, verified against asm/unistd_64.h.
  return sys2(73, static_cast<long>(fd), static_cast<long>(operation));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_fsync_v1(int32_t fd) noexcept {
  return sys1(kSysFsync, static_cast<long>(fd));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_fdatasync_v1(int32_t fd) noexcept {
  return sys1(kSysFdatasync, static_cast<long>(fd));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_dup_v1(int32_t fd) noexcept {
  return sys1(kSysDup, static_cast<long>(fd));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_dup2_v1(int32_t fd, int32_t new_fd) noexcept {
  return sys2(kSysDup2, static_cast<long>(fd),
              static_cast<long>(new_fd));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_pipe_v1(int32_t descriptors[2], uint32_t flags) noexcept {
  if (descriptors == nullptr) return linux_abi::kErrorFault;

  // pipe2 can fail after allocating a descriptor; keep the caller's array
  // untouched until both descriptors are known to be valid.
  int32_t kernel_descriptors[2];
  const int64_t result =
      sys2(kSysPipe2, to_long(kernel_descriptors), static_cast<long>(flags));
  if (result < 0) return result;

  descriptors[0] = kernel_descriptors[0];
  descriptors[1] = kernel_descriptors[1];
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_mkfifo_v1(const char *path, uint64_t length,
                           uint32_t mode) noexcept {
  char prepared[linux_abi::kPathMax];
  const int64_t prepared_result = linux_abi::copy_path(path, length, prepared);
  if (prepared_result < 0) return prepared_result;

  const uint32_t fifo_mode =
      (mode & kModePermissionMask) | kFileTypeFifo;
  return sys4(kSysMknodat, kAtFdcwd, to_long(prepared),
              static_cast<long>(fifo_mode), 0);
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_access_v1(const char *path, uint64_t length,
                           int32_t mode) noexcept {
  char prepared[linux_abi::kPathMax];
  const int64_t prepared_result = linux_abi::copy_path(path, length, prepared);
  if (prepared_result < 0) return prepared_result;

  return sys2(kSysAccess, to_long(prepared), static_cast<long>(mode));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_getuid_v1() noexcept { return sys0(kSysGetuid); }

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_geteuid_v1() noexcept { return sys0(kSysGeteuid); }

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_getgid_v1() noexcept { return sys0(kSysGetgid); }

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_getegid_v1() noexcept { return sys0(kSysGetegid); }

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_getppid_v1() noexcept { return sys0(kSysGetppid); }

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_gettid_v1() noexcept { return sys0(kSysGettid); }

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_setpgid_v1(int32_t pid, int32_t pgid) noexcept {
  return sys2(kSysSetpgid, static_cast<long>(pid),
              static_cast<long>(pgid));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_getpgid_v1(int32_t pid) noexcept {
  return sys1(kSysGetpgid, static_cast<long>(pid));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_setsid_v1() noexcept { return sys0(kSysSetsid); }

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_kill_v1(int32_t pid, int32_t signal) noexcept {
  return sys2(kSysKill, static_cast<long>(pid),
              static_cast<long>(signal));
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_clock_gettime_v1(int32_t clock_id,
                                  LangTimespec *out) noexcept {
  if (out == nullptr) return linux_abi::kErrorFault;

  KernelTimespec kernel_result;
  const int64_t result =
      sys2(kSysClockGettime, static_cast<long>(clock_id),
           to_long(&kernel_result));
  if (result < 0) return result;

  out->seconds = kernel_result.seconds;
  out->nanoseconds = kernel_result.nanoseconds;
  return 0;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_nanosleep_v1(const LangTimespec *request,
                              LangTimespec *remaining) noexcept {
  if (request == nullptr) return linux_abi::kErrorFault;

  KernelTimespec kernel_request;
  kernel_request.seconds = request->seconds;
  kernel_request.nanoseconds = request->nanoseconds;

  KernelTimespec kernel_remaining;
  int64_t result;
  {
    // Sleeping holds no runtime lock and does not touch the graph. The
    // caller-visible remaining value is written after leaving SAFE.
    lang::native::gc_program::SafeGuard safe;
    result = sys2(kSysNanosleep, to_long(&kernel_request),
                  remaining == nullptr ? 0 : to_long(&kernel_remaining));
  }

  // The kernel reports the leftover interval only for an interrupted sleep.
  // There is deliberately no retry loop here.
  if (result == linux_abi::kErrorInterrupted && remaining != nullptr) {
    remaining->seconds = kernel_remaining.seconds;
    remaining->nanoseconds = kernel_remaining.nanoseconds;
  }
  return result;
}

LANG_NATIVE_NO_STACK_PROTECTOR
int64_t lang_sys_getrandom_v1(void *buffer, uint64_t count,
                              uint32_t flags) noexcept {
  // A zero count is a successful no-op and must not touch the buffer, even
  // when the pointer is null.
  if (count == 0) return 0;
  if (count > kMaxSignedCount) return linux_abi::kErrorInval;
  if (buffer == nullptr) return linux_abi::kErrorFault;

  return sys3(kSysGetrandom, to_long(buffer), static_cast<long>(count),
              static_cast<long>(flags));
}

} // extern "C"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#endif // Linux x86_64
