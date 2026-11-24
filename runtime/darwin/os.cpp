#include "../native/os.hpp"

#include "../native/gc_program.hpp"
#include "posix.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace darwin = lang::native::darwin;

int open_flags(int32_t flags) noexcept {
  constexpr int32_t kAccessMask = 3;
  constexpr int32_t kCreate = 64;
  constexpr int32_t kExclusive = 128;
  constexpr int32_t kTruncate = 512;
  constexpr int32_t kAppend = 1024;
  constexpr int32_t kDirectory = 65536;
  constexpr int32_t kNoFollow = 131072;
  constexpr int32_t kKnownFlags = kAccessMask | kCreate | kExclusive |
      kTruncate | kAppend | kDirectory | kNoFollow | darwin::kNonblock |
      darwin::kCloexec;
  const int32_t access = flags & kAccessMask;
  if ((flags & ~kKnownFlags) != 0 || access == kAccessMask) return -1;
  int out = access == 0 ? O_RDONLY : access == 1 ? O_WRONLY : O_RDWR;
  if (flags & kCreate) out |= O_CREAT;
  if (flags & kExclusive) out |= O_EXCL;
  if (flags & kTruncate) out |= O_TRUNC;
  if (flags & kAppend) out |= O_APPEND;
  if (flags & kDirectory) out |= O_DIRECTORY;
  if (flags & kNoFollow) out |= O_NOFOLLOW;
  if (flags & darwin::kNonblock) out |= O_NONBLOCK;
  if (flags & darwin::kCloexec) out |= O_CLOEXEC;
  return out;
}
} // namespace

extern "C" {

int64_t lang_os_read_v1(int32_t fd, void *data, uint64_t count) noexcept {
  if (count == 0) return 0;
  if (count > INT64_MAX) return -22;
  lang::native::gc_program::SafeGuard safe;
  return darwin::result(read(fd, data, count));
}

int64_t lang_os_write_v1(int32_t fd, const void *data, uint64_t count) noexcept {
  if (count == 0) return 0;
  if (count > INT64_MAX) return -22;
  lang::native::gc_program::SafeGuard safe;
  return darwin::result(write(fd, data, count));
}

int64_t lang_os_close_v1(int32_t fd) noexcept {
  return darwin::result(close(fd));
}

int64_t lang_os_open_v1(const char *data, uint64_t length, int32_t flags,
                        uint32_t mode) noexcept {
  const darwin::Path path(data, length);
  if (path.status != 0) return path.status;
  const int translated = open_flags(flags);
  if (translated < 0) return -22;
  return darwin::result(open(path.bytes, translated, static_cast<mode_t>(mode)));
}

int64_t lang_os_openat_v1(int32_t fd, const char *name, uint64_t length,
                          int32_t flags, uint32_t mode) noexcept {
  const darwin::Component path(name, length);
  if (path.status != 0) return path.status;
  const int translated = open_flags(flags);
  if (translated < 0) return -22;
  return darwin::result(
      openat(fd, path.bytes, translated, static_cast<mode_t>(mode)));
}

int64_t lang_os_seek_v1(int32_t fd, int64_t offset, int32_t whence) noexcept {
  return darwin::result(lseek(fd, offset, whence));
}

int64_t lang_os_unlink_v1(const char *data, uint64_t length) noexcept {
  const darwin::Path path(data, length);
  if (path.status != 0) return path.status;
  return darwin::result(unlink(path.bytes));
}

int64_t lang_os_unlinkat_v1(int32_t fd, const char *name, uint64_t length,
                            int32_t kind) noexcept {
  if (kind != 0 && kind != 1) return -22;
  const darwin::Component path(name, length);
  if (path.status != 0) return path.status;
  const int flags = kind == 1 ? AT_REMOVEDIR : 0;
  return darwin::result(unlinkat(fd, path.bytes, flags));
}

int64_t lang_os_mkdir_v1(const char *data, uint64_t length, uint32_t mode) noexcept {
  const darwin::Path path(data, length);
  if (path.status != 0) return path.status;
  return darwin::result(mkdir(path.bytes, static_cast<mode_t>(mode)));
}

int64_t lang_os_mkdirat_v1(int32_t fd, const char *name, uint64_t length,
                           uint32_t mode) noexcept {
  const darwin::Component path(name, length);
  if (path.status != 0) return path.status;
  return darwin::result(
      mkdirat(fd, path.bytes, static_cast<mode_t>(mode)));
}

int64_t lang_os_rmdir_v1(const char *data, uint64_t length) noexcept {
  const darwin::Path path(data, length);
  if (path.status != 0) return path.status;
  return darwin::result(rmdir(path.bytes));
}

int64_t lang_os_getpid_v1() noexcept { return getpid(); }

[[noreturn]] void lang_os_exit_v1(int32_t status) noexcept { _exit(status); }

} // extern "C"
