#ifndef LANG_RUNTIME_NATIVE_SYSTEM_HPP
#define LANG_RUNTIME_NATIVE_SYSTEM_HPP

#include <cstdint>

// Private native system primitive ABI: lang-native-system/1.
//
// These entry points back the filesystem, time, random, signal, and process
// parts of the Woozy standard library on Linux x86_64. They are runtime
// internals, not a user-visible raw syscall API: source programs use the typed
// `os`/`system` declarations and lowering translates failures into
// `throws os.Error`.
//
// Error ABI
// ---------
// Every int64_t-returning function reports either a documented non-negative
// success value or a raw negative Linux errno in [-4095, -1]. Callers must test
// `result < 0`; they must not treat only -1 as failure. No wrapper sets a
// global errno, retries EINTR, or maps an error to another convention, except
// where a count/zero rule documented below returns 0 without a kernel call.
//
// Borrowed buffers and paths
// --------------------------
// Path arguments are (pointer, byte_length) pairs borrowed for one call. The
// runtime copies at most `length` bytes into private stack storage and reads no
// byte beyond that range; the source need not be NUL-terminated. An embedded
// NUL is rejected with -EINVAL, a length of 4096 or more with -ENAMETOOLONG,
// and a null pointer with a nonzero length with -EFAULT. A zero length with a
// null pointer is the empty path and is passed to the kernel unchanged.
//
// Output buffers (getcwd, getdents, readlink, getrandom, pipe) are borrowed and
// must stay valid for the call. The runtime never retains these pointers.
//
// LangFileStat layout
// -------------------
// LangFileStat is a stable 120-byte value with this exact layout:
//   device       0   uint64_t
//   inode        8   uint64_t
//   links       16   uint64_t
//   mode        24   uint32_t
//   uid         28   uint32_t
//   gid         32   uint32_t
//   padding     36   uint32_t
//   rdev        40   uint64_t
//   size        48   int64_t
//   block_size  56   int64_t
//   blocks      64   int64_t
//   accessed    72   LangTimespec { seconds 72, nanoseconds 80 }
//   modified    88   LangTimespec { seconds 88, nanoseconds 96 }
//   changed    104   LangTimespec { seconds 104, nanoseconds 112 }
// It is filled from the real Linux x86_64 kernel `stat` structure on success;
// the kernel structure is 144 bytes and is converted field by field.
//
// LangTimespec is the Linux kernel timespec layout: two signed 64-bit fields,
// `seconds` at offset 0 and `nanoseconds` at offset 8, for a total size of 16.
//
// This ABI is implemented only for Linux x86_64; there is no other-platform
// fallback here. fork/exec and networking are deliberately not part of it.

struct LangTimespec {
  int64_t seconds;
  int64_t nanoseconds;
};

struct LangFileStat {
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
  LangTimespec accessed;
  LangTimespec modified;
  LangTimespec changed;
};

static_assert(sizeof(LangTimespec) == 16, "LangTimespec must be two int64_t");
static_assert(__builtin_offsetof(LangTimespec, seconds) == 0,
              "LangTimespec.seconds offset");
static_assert(__builtin_offsetof(LangTimespec, nanoseconds) == 8,
              "LangTimespec.nanoseconds offset");

static_assert(sizeof(LangFileStat) == 120, "LangFileStat must be 120 bytes");
static_assert(__builtin_offsetof(LangFileStat, device) == 0,
              "LangFileStat.device offset");
static_assert(__builtin_offsetof(LangFileStat, inode) == 8,
              "LangFileStat.inode offset");
static_assert(__builtin_offsetof(LangFileStat, links) == 16,
              "LangFileStat.links offset");
static_assert(__builtin_offsetof(LangFileStat, mode) == 24,
              "LangFileStat.mode offset");
static_assert(__builtin_offsetof(LangFileStat, uid) == 28,
              "LangFileStat.uid offset");
static_assert(__builtin_offsetof(LangFileStat, gid) == 32,
              "LangFileStat.gid offset");
static_assert(__builtin_offsetof(LangFileStat, padding) == 36,
              "LangFileStat.padding offset");
static_assert(__builtin_offsetof(LangFileStat, rdev) == 40,
              "LangFileStat.rdev offset");
static_assert(__builtin_offsetof(LangFileStat, size) == 48,
              "LangFileStat.size offset");
static_assert(__builtin_offsetof(LangFileStat, block_size) == 56,
              "LangFileStat.block_size offset");
static_assert(__builtin_offsetof(LangFileStat, blocks) == 64,
              "LangFileStat.blocks offset");
static_assert(__builtin_offsetof(LangFileStat, accessed) == 72,
              "LangFileStat.accessed offset");
static_assert(__builtin_offsetof(LangFileStat, modified) == 88,
              "LangFileStat.modified offset");
static_assert(__builtin_offsetof(LangFileStat, changed) == 104,
              "LangFileStat.changed offset");

extern "C" {

// stat(2)/lstat(2)/fstat(2). The complete LangFileStat is written only on
// success; `out` is untouched on failure and must be non-null (-EFAULT).
int64_t lang_sys_stat_v1(const char *path, uint64_t length,
                         LangFileStat *out) noexcept;
int64_t lang_sys_lstat_v1(const char *path, uint64_t length,
                          LangFileStat *out) noexcept;
int64_t lang_sys_fstat_v1(int32_t fd, LangFileStat *out) noexcept;

// rename(2). Both paths are validated before the kernel call.
int64_t lang_sys_rename_v1(const char *old_path, uint64_t old_length,
                           const char *new_path,
                           uint64_t new_length) noexcept;

// chmod(2)/fchmod(2). `mode` is the Linux permission value (the kernel decides
// which bits are meaningful).
int64_t lang_sys_chmod_v1(const char *path, uint64_t length,
                          uint32_t mode) noexcept;
int64_t lang_sys_fchmod_v1(int32_t fd, uint32_t mode) noexcept;

// chdir(2).
int64_t lang_sys_chdir_v1(const char *path, uint64_t length) noexcept;

// getcwd(2). On success returns the path length excluding the terminating NUL;
// the kernel writes a NUL-terminated path into `buffer`. A zero `capacity`
// returns -EINVAL without touching the buffer; a null buffer with nonzero
// capacity returns -EFAULT.
int64_t lang_sys_getcwd_v1(char *buffer, uint64_t capacity) noexcept;

// getdents64(2). Returns the raw byte count and writes the kernel's
// `linux_dirent64` records into `buffer`; parsing is the caller's job.
// `capacity == 0` returns 0 without touching the buffer, even for a null
// pointer. A null pointer with nonzero capacity returns -EFAULT.
int64_t lang_sys_getdents_v1(int32_t fd, void *buffer,
                             uint64_t capacity) noexcept;

// readlink(2). Returns the number of bytes placed in `buffer` (no terminating
// NUL is added by the kernel). `capacity == 0` returns 0 without touching the
// buffer, even for a null pointer. A null pointer with nonzero capacity
// returns -EFAULT. The path is validated before the zero-capacity rule, so an
// invalid path still reports its path error.
int64_t lang_sys_readlink_v1(const char *path, uint64_t length, void *buffer,
                             uint64_t capacity) noexcept;

// symlink(2)/link(2). Both paths are validated before the kernel call.
int64_t lang_sys_symlink_v1(const char *target, uint64_t target_length,
                            const char *link_path,
                            uint64_t link_length) noexcept;
int64_t lang_sys_link_v1(const char *old_path, uint64_t old_length,
                         const char *new_path, uint64_t new_length) noexcept;

// truncate(2)/ftruncate(2).
int64_t lang_sys_truncate_v1(const char *path, uint64_t length,
                             int64_t size) noexcept;
int64_t lang_sys_ftruncate_v1(int32_t fd, int64_t size) noexcept;

// fsync(2)/fdatasync(2). EINTR is returned unchanged.
int64_t lang_sys_fsync_v1(int32_t fd) noexcept;
int64_t lang_sys_flock_v1(int32_t fd, int32_t operation) noexcept;
int64_t lang_sys_fdatasync_v1(int32_t fd) noexcept;

// dup(2)/dup2(2). dup returns the new descriptor; dup2 returns `new_fd`.
int64_t lang_sys_dup_v1(int32_t fd) noexcept;
int64_t lang_sys_dup2_v1(int32_t fd, int32_t new_fd) noexcept;

// pipe2(2). `descriptors` must be non-null (-EFAULT). Both descriptors are
// written only when pipe2 succeeds; on failure the array is untouched.
int64_t lang_sys_pipe_v1(int32_t descriptors[2], uint32_t flags) noexcept;

// mkfifo(2) via mknodat(AT_FDCWD, path, S_IFIFO | (mode & 07777), 0).
int64_t lang_sys_mkfifo_v1(const char *path, uint64_t length,
                            uint32_t mode) noexcept;

// access(2). `mode` is the Linux R_OK/W_OK/X_OK/F_OK mask.
int64_t lang_sys_access_v1(const char *path, uint64_t length,
                           int32_t mode) noexcept;

// Identity and thread/process-group reads. Linux reports no errors for these
// identity calls; they return the positive id directly.
int64_t lang_sys_getuid_v1() noexcept;
int64_t lang_sys_geteuid_v1() noexcept;
int64_t lang_sys_getgid_v1() noexcept;
int64_t lang_sys_getegid_v1() noexcept;
int64_t lang_sys_getppid_v1() noexcept;
int64_t lang_sys_gettid_v1() noexcept;

// setpgid(2)/getpgid(2)/setsid(2). setsid returns the new session id (the
// caller's pid) on success. No EINTR retry is performed.
int64_t lang_sys_setpgid_v1(int32_t pid, int32_t pgid) noexcept;
int64_t lang_sys_getpgid_v1(int32_t pid) noexcept;
int64_t lang_sys_setsid_v1() noexcept;

// kill(2). A signal value of 0 performs only the existence/permission check.
int64_t lang_sys_kill_v1(int32_t pid, int32_t signal) noexcept;

// clock_gettime(2). `out` must be non-null (-EFAULT). The complete timespec is
// written only on success; the kernel validates `clock_id` and the nanosecond
// range and returns -EINVAL/-EFAULT as appropriate.
int64_t lang_sys_clock_gettime_v1(int32_t clock_id,
                                  LangTimespec *out) noexcept;

// nanosleep(2). The request is copied to kernel layout before the call. On
// -EINTR, the remaining time reported by the kernel is written to `remaining`
// when it is non-null; on success or other errors `remaining` is untouched.
// A null request returns -EFAULT. The kernel validates the nanosecond range
// (0 <= nanoseconds < 1000000000) and returns -EINVAL. There is no retry.
int64_t lang_sys_nanosleep_v1(const LangTimespec *request,
                              LangTimespec *remaining) noexcept;

// getrandom(2). Returns the number of bytes written, which may be less than
// `count` when interrupted. `count == 0` returns 0 without touching the buffer,
// even for a null pointer. A null pointer with nonzero count returns -EFAULT.
// A count greater than INT64_MAX returns -EINVAL before the kernel call; the
// kernel performs its own flag and upper-bound validation.
int64_t lang_sys_getrandom_v1(void *buffer, uint64_t count,
                              uint32_t flags) noexcept;

} // extern "C"

#endif // LANG_RUNTIME_NATIVE_SYSTEM_HPP

struct LangVec;
extern "C" {
int64_t lang_sys_getcwd_vec_v1(LangVec *, uint64_t) noexcept;
int64_t lang_sys_getdents_vec_v1(int32_t, LangVec *, uint64_t) noexcept;
int64_t lang_sys_readlink_vec_v1(const char *, uint64_t, LangVec *, uint64_t) noexcept;
// Current executable's absolute path as bytes, without a trailing NUL.
// Returns -ERANGE when the borrowed buffer is too small.
int64_t lang_sys_executable_path_vec_v1(LangVec *, uint64_t) noexcept;
int64_t lang_sys_getrandom_vec_v1(LangVec *, uint64_t, uint32_t) noexcept;
int64_t lang_sys_nanosleep_vec_v1(const LangTimespec *, LangVec *) noexcept;
}
