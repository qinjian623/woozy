#include "../native/system.hpp"

#include "../native/darwin.hpp"
#include "../native/gc_program.hpp"
#include "posix.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/attr.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <time.h>
#include <unistd.h>

namespace {
namespace darwin = lang::native::darwin;

LangTimespec timestamp(const timespec &time) noexcept {
  return {time.tv_sec, time.tv_nsec};
}

void fill_stat(LangFileStat &out, const struct stat &value) noexcept {
  out = {static_cast<uint32_t>(value.st_dev), value.st_ino, value.st_nlink,
         value.st_mode, value.st_uid, value.st_gid, 0,
         static_cast<uint32_t>(value.st_rdev), value.st_size, value.st_blksize,
         value.st_blocks, timestamp(value.st_atimespec),
         timestamp(value.st_mtimespec), timestamp(value.st_ctimespec)};
}

int64_t path_stat(const char *path, uint64_t length, LangFileStat *out,
                  bool follow) noexcept {
  const darwin::Path name(path, length);
  if (name.status != 0) return name.status;
  if (!out) return -14;
  struct stat value;
  const int status = follow ? stat(name.bytes, &value) : lstat(name.bytes, &value);
  if (status < 0) return darwin::error(errno);
  fill_stat(*out, value);
  return 0;
}

// Attribute order is fixed by getattrlistbulk with FSOPT_PACK_INVAL_ATTRS.
struct DirRecord {
  uint32_t length;
  attribute_set_t returned;
  uint32_t error;
  attrreference_t name;
  fsobj_type_t type;
  uint64_t inode;
};

static_assert(sizeof(DirRecord) == 48 && offsetof(DirRecord, name) == 28 &&
              offsetof(DirRecord, inode) == 40);

uint8_t dir_type(fsobj_type_t type) noexcept {
  switch (type) {
    case VREG: return DT_REG;
    case VDIR: return DT_DIR;
    case VBLK: return DT_BLK;
    case VCHR: return DT_CHR;
    case VLNK: return DT_LNK;
    case VSOCK: return DT_SOCK;
    case VFIFO: return DT_FIFO;
    default: return DT_UNKNOWN;
  }
}

// Woozy's reader consumes the existing linux_dirent64 wire format.
int64_t convert_directory(char *buffer, uint64_t capacity, int count) noexcept {
  constexpr size_t kNameOffset = 19;
  constexpr size_t kLengthOffset = 16;
  constexpr size_t kTypeOffset = 18;
  uint64_t consumed = 0;
  uint64_t written = 0;
  for (int index = 0; index < count; ++index) {
    if (capacity - consumed < sizeof(DirRecord)) return -5;
    const char *position = buffer + consumed;
    DirRecord entry;
    std::memcpy(&entry, position, sizeof(entry));
    if (entry.length < sizeof(entry) || entry.length > capacity - consumed) return -5;
    if ((entry.returned.commonattr & ATTR_CMN_ERROR) && entry.error != 0)
      return darwin::error(static_cast<int>(entry.error));
    if ((entry.returned.commonattr & ATTR_CMN_NAME) == 0) return -5;

    const int64_t name_offset = static_cast<int64_t>(offsetof(DirRecord, name)) +
                                entry.name.attr_dataoffset;
    const uint32_t name_length = entry.name.attr_length;
    if (name_offset < static_cast<int64_t>(sizeof(entry)) ||
        name_offset >= entry.length || name_length == 0 ||
        name_length > entry.length - name_offset) return -5;
    const char *name = position + name_offset;
    if (std::memchr(name, 0, name_length) != name + name_length - 1) return -5;

    const size_t size = (kNameOffset + name_length + 7) & ~size_t{7};
    if (size > entry.length || size > UINT16_MAX) return -5;
    const auto record_length = static_cast<uint16_t>(size);
    char *out = buffer + written;
    const uint64_t inode = (entry.returned.commonattr & ATTR_CMN_FILEID) ? entry.inode : 0;
    const uint8_t type = (entry.returned.commonattr & ATTR_CMN_OBJTYPE)
                             ? dir_type(entry.type) : DT_UNKNOWN;
    // The native records are larger, so in-place compaction preserves unread data.
    std::memmove(out + kNameOffset, name, name_length);
    std::memcpy(out, &inode, sizeof(inode));
    // shortcut: getattrlistbulk has no per-entry seek cookie, extend the ABI if callers need one.
    std::memset(out + sizeof(inode), 0, kLengthOffset - sizeof(inode));
    std::memcpy(out + kLengthOffset, &record_length, sizeof(record_length));
    out[kTypeOffset] = static_cast<char>(type);
    std::memset(out + kNameOffset + name_length, 0, size - kNameOffset - name_length);
    consumed += entry.length;
    written += size;
  }
  return static_cast<int64_t>(written);
}
} // namespace

extern "C" {

int64_t lang_sys_stat_v1(const char *path, uint64_t length, LangFileStat *out) noexcept {
  return path_stat(path, length, out, true);
}

int64_t lang_sys_lstat_v1(const char *path, uint64_t length, LangFileStat *out) noexcept {
  return path_stat(path, length, out, false);
}

int64_t lang_sys_fstat_v1(int32_t fd, LangFileStat *out) noexcept {
  if (!out) return -14;
  struct stat value;
  if (fstat(fd, &value) < 0) return darwin::error(errno);
  fill_stat(*out, value);
  return 0;
}

int64_t lang_sys_rename_v1(const char *old_path, uint64_t old_length,
                           const char *new_path, uint64_t new_length) noexcept {
  const darwin::Path from(old_path, old_length);
  if (from.status != 0) return from.status;
  const darwin::Path to(new_path, new_length);
  if (to.status != 0) return to.status;
  return darwin::result(rename(from.bytes, to.bytes));
}

int64_t lang_sys_chmod_v1(const char *path, uint64_t length, uint32_t mode) noexcept {
  const darwin::Path name(path, length);
  if (name.status != 0) return name.status;
  return darwin::result(chmod(name.bytes, static_cast<mode_t>(mode)));
}

int64_t lang_sys_fchmod_v1(int32_t fd, uint32_t mode) noexcept {
  return darwin::result(fchmod(fd, static_cast<mode_t>(mode)));
}

int64_t lang_sys_chdir_v1(const char *path, uint64_t length) noexcept {
  const darwin::Path name(path, length);
  if (name.status != 0) return name.status;
  return darwin::result(chdir(name.bytes));
}

int64_t lang_sys_getcwd_v1(char *buffer, uint64_t capacity) noexcept {
  if (capacity == 0 || capacity > INT64_MAX) return -22;
  if (!buffer) return -14;
  if (!getcwd(buffer, capacity)) return darwin::error(errno);
  return static_cast<int64_t>(std::strlen(buffer));
}

int64_t lang_sys_getdents_v1(int32_t fd, void *buffer, uint64_t capacity) noexcept {
  if (capacity == 0) return 0;
  if (capacity > INT64_MAX) return -22;
  if (!buffer) return -14;
  struct attrlist attrs {};
  attrs.bitmapcount = ATTR_BIT_MAP_COUNT;
  attrs.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR |
                    ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID;
  const int count = getattrlistbulk(fd, &attrs, buffer, capacity, FSOPT_PACK_INVAL_ATTRS);
  if (count < 0) return errno == ERANGE ? -22 : darwin::error(errno);
  return convert_directory(static_cast<char *>(buffer), capacity, count);
}

int64_t lang_sys_readlink_v1(const char *path, uint64_t length, void *buffer,
                             uint64_t capacity) noexcept {
  const darwin::Path name(path, length);
  if (name.status != 0) return name.status;
  if (capacity == 0) return 0;
  if (capacity > INT64_MAX) return -22;
  if (!buffer) return -14;
  return darwin::result(readlink(name.bytes, static_cast<char *>(buffer), capacity));
}

int64_t lang_sys_symlink_v1(const char *target, uint64_t target_length,
                            const char *link_path, uint64_t link_length) noexcept {
  const darwin::Path from(target, target_length);
  if (from.status != 0) return from.status;
  const darwin::Path to(link_path, link_length);
  if (to.status != 0) return to.status;
  return darwin::result(symlink(from.bytes, to.bytes));
}

int64_t lang_sys_link_v1(const char *old_path, uint64_t old_length,
                         const char *new_path, uint64_t new_length) noexcept {
  const darwin::Path from(old_path, old_length);
  if (from.status != 0) return from.status;
  const darwin::Path to(new_path, new_length);
  if (to.status != 0) return to.status;
  return darwin::result(link(from.bytes, to.bytes));
}

int64_t lang_sys_truncate_v1(const char *path, uint64_t length, int64_t size) noexcept {
  const darwin::Path name(path, length);
  if (name.status != 0) return name.status;
  return darwin::result(truncate(name.bytes, size));
}

int64_t lang_sys_ftruncate_v1(int32_t fd, int64_t size) noexcept {
  return darwin::result(ftruncate(fd, size));
}

int64_t lang_sys_flock_v1(int32_t fd, int32_t operation) noexcept {
  return darwin::result(flock(fd, operation));
}

int64_t lang_sys_fsync_v1(int32_t fd) noexcept { return darwin::result(fsync(fd)); }

int64_t lang_sys_fdatasync_v1(int32_t fd) noexcept {
  // fsync also synchronizes file metadata on Darwin.
  return darwin::result(fsync(fd));
}

int64_t lang_sys_dup_v1(int32_t fd) noexcept { return darwin::result(dup(fd)); }

int64_t lang_sys_dup2_v1(int32_t fd, int32_t new_fd) noexcept {
  return darwin::result(dup2(fd, new_fd));
}

int64_t lang_sys_pipe_v1(int32_t descriptors[2], uint32_t flags) noexcept {
  if (!descriptors) return -14;
  if (flags & ~static_cast<uint32_t>(darwin::kCloexec | darwin::kNonblock)) return -22;
  const darwin::LaunchGuard guard;
  int fds[2];
  if (pipe(fds) < 0) return darwin::error(errno);
  for (const int fd : fds) {
    if (((flags & darwin::kCloexec) && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) ||
        ((flags & darwin::kNonblock) && fcntl(fd, F_SETFL, O_NONBLOCK) < 0)) {
      const int64_t status = darwin::error(errno);
      close(fds[0]);
      close(fds[1]);
      return status;
    }
  }
  descriptors[0] = fds[0];
  descriptors[1] = fds[1];
  return 0;
}

int64_t lang_sys_mkfifo_v1(const char *path, uint64_t length, uint32_t mode) noexcept {
  const darwin::Path name(path, length);
  if (name.status != 0) return name.status;
  return darwin::result(mkfifo(name.bytes, static_cast<mode_t>(mode & 07777)));
}

int64_t lang_sys_access_v1(const char *path, uint64_t length, int32_t mode) noexcept {
  const darwin::Path name(path, length);
  if (name.status != 0) return name.status;
  return darwin::result(access(name.bytes, mode));
}

int64_t lang_sys_getuid_v1() noexcept { return getuid(); }
int64_t lang_sys_geteuid_v1() noexcept { return geteuid(); }
int64_t lang_sys_getgid_v1() noexcept { return getgid(); }
int64_t lang_sys_getegid_v1() noexcept { return getegid(); }
int64_t lang_sys_getppid_v1() noexcept { return getppid(); }

int64_t lang_sys_gettid_v1() noexcept {
  const int64_t id = darwin::thread_id();
  if (id == 0) return -5;
  return id <= INT32_MAX ? id : -75;
}

int64_t lang_sys_setpgid_v1(int32_t pid, int32_t pgid) noexcept {
  return darwin::result(setpgid(pid, pgid));
}

int64_t lang_sys_getpgid_v1(int32_t pid) noexcept {
  return darwin::result(getpgid(pid));
}

int64_t lang_sys_setsid_v1() noexcept { return darwin::result(setsid()); }

int64_t lang_sys_kill_v1(int32_t pid, int32_t signal) noexcept {
  const auto &signals = darwin::linux_signal_to_darwin;
  if (signal < 0 || static_cast<size_t>(signal) >= sizeof(signals) / sizeof(signals[0]) ||
      (signal != 0 && signals[signal] == 0)) return -22;
  return darwin::result(kill(pid, signals[signal]));
}

int64_t lang_sys_clock_gettime_v1(int32_t clock_id, LangTimespec *out) noexcept {
  if (!out) return -14;
  if (clock_id != 0 && clock_id != 1) return -22;
  const clockid_t clock = clock_id == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC;
  timespec sample;
  if (clock_gettime(clock, &sample) < 0) return darwin::error(errno);
  *out = timestamp(sample);
  return 0;
}

int64_t lang_sys_nanosleep_v1(const LangTimespec *request, LangTimespec *remaining) noexcept {
  if (!request) return -14;
  const timespec interval{request->seconds, request->nanoseconds};
  timespec rest;
  int64_t status;
  {
    lang::native::gc_program::SafeGuard safe;
    status = darwin::result(nanosleep(&interval, remaining ? &rest : nullptr));
  }
  if (status == -4 && remaining) *remaining = timestamp(rest);
  return status;
}

int64_t lang_sys_getrandom_v1(void *buffer, uint64_t count, uint32_t flags) noexcept {
  if (count == 0) return 0;
  if (count > INT64_MAX || (flags & ~3u)) return -22;
  if (!buffer) return -14;
  if (flags != 0) return -95;
  const auto size = std::min<uint64_t>(count, 256);
  if (getentropy(buffer, size) < 0) return darwin::error(errno);
  return static_cast<int64_t>(size);
}

} // extern "C"
