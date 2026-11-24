#ifndef LANG_RUNTIME_NATIVE_OS_HPP
#define LANG_RUNTIME_NATIVE_OS_HPP

#include <cstdint>

// Private native OS primitive ABI: lang-native-os/1.
//
// These entry points back the Woozy standard-library `os` module on Linux
// x86_64. They are runtime internals, not a user-visible raw syscall API:
// source programs use the typed `os` declarations, and lowering translates
// these results into language values or `throws os.Error`. There is no global
// errno, no libc dependency, and no retry policy hidden in this layer.
//
// Raw error ABI
// -------------
// Every int64_t-returning function reports either a non-negative success
// result (byte count, descriptor, offset, pid) or a negative Linux errno in
// [-4095, -1] (for example -9 = -EBADF, -22 = -EINVAL, -36 = -ENAMETOOLONG).
// Callers must test `result < 0`; they must not treat only -1 as failure.
// No wrapper sets errno, retries, or maps errors to another convention.
//
// Borrowed buffers
// ----------------
// data/path/byte_length describe caller-owned memory borrowed for the duration
// of one call. The runtime never retains these pointers. Path functions copy
// at most `length` bytes into private stack storage and read no byte beyond
// that range; the source need not be NUL-terminated. A NUL inside the provided
// range is rejected with -EINVAL, and a length of 4096 or more (Linux PATH_MAX
// including its terminator) with -ENAMETOOLONG. A null path pointer with a
// nonzero length yields -EFAULT. A zero-length borrowed path is the empty
// path and is passed to the kernel unchanged.
//
// For read/write, `count == 0` returns 0 without touching the buffer. Kernel
// I/O may modify read buffers and observe write buffers only during the call;
// callers must keep that memory valid and obey read(2)/write(2) aliasing rules.
// Descriptors and offsets are plain Linux values and carry no ownership beyond
// what the typed language wrapper declares.
//
// This ABI is implemented only for Linux x86_64; there is no other-platform
// fallback here.

extern "C" {

// One read(2) / write(2) syscall, never restarted. EINTR is returned as -4;
// partial transfers are reported as the positive count transferred.
int64_t lang_os_read_v1(int32_t fd, void *data, uint64_t count) noexcept;
int64_t lang_os_write_v1(int32_t fd, const void *data, uint64_t count) noexcept;

// close(2). If close reports EINTR (-4), descriptor state is platform-defined;
// the caller decides whether retrying is safe.
int64_t lang_os_close_v1(int32_t fd) noexcept;

// openat(AT_FDCWD, path, flags, mode). The path is borrowed for this call only.
int64_t lang_os_open_v1(const char *data, uint64_t byte_length,
                        int32_t flags, uint32_t mode) noexcept;

// Descriptor-relative operations. The name is one directory-entry component:
// it must be non-empty, contain no NUL or '/', and must not be "." or "..".
// The runtime copies at most `length` bytes into private stack storage. Errors
// still use the raw negative-errno convention above.
int64_t lang_os_openat_v1(int32_t fd, const char *name, uint64_t length,
                          int32_t flags, uint32_t mode) noexcept;
int64_t lang_os_mkdirat_v1(int32_t fd, const char *name, uint64_t length,
                           uint32_t mode) noexcept;
// `kind` is 0 for a non-directory entry or 1 for a directory entry. It is not
// a platform flag; any other value returns -EINVAL.
int64_t lang_os_unlinkat_v1(int32_t fd, const char *name, uint64_t length,
                            int32_t kind) noexcept;

// lseek(2); `offset` is a signed 64-bit file offset.
int64_t lang_os_seek_v1(int32_t fd, int64_t offset, int32_t whence) noexcept;

// Path operations. Each borrows `path`/`length` for the duration of the call.
int64_t lang_os_unlink_v1(const char *path, uint64_t length) noexcept;
int64_t lang_os_mkdir_v1(const char *path, uint64_t length,
                         uint32_t mode) noexcept;
int64_t lang_os_rmdir_v1(const char *path, uint64_t length) noexcept;

// getpid(2). Returns the positive process id; Linux reports no errors here.
int64_t lang_os_getpid_v1() noexcept;

// exit_group(2) with the low 8 bits of `status`. This is terminal: it never
// returns, performs no language cleanup, and flushes no userspace buffers.
[[noreturn]] void lang_os_exit_v1(int32_t status) noexcept;

} // extern "C"

#endif
