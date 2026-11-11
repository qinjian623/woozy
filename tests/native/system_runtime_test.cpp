// Host-side acceptance harness for runtime/native/system.cpp.
//
// The runtime object under test is freestanding and must not reference libc or
// libstdc++. This harness uses the host libc only for temporary-directory
// setup, expected-value inspection, forks, signals, and diagnostics. It does
// not provide any fallback implementation of a lang_sys_* entry point.
//
// Build (exact commands are in the task report):
//   <cxx> -std=c++20 -O2 -ffreestanding -fno-exceptions -fno-rtti
//         -fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset
//         -fno-builtin-memmove (-fno-tree-loop-distribute-patterns for GCC)
//         -c runtime/native/system.cpp -o system.o
//   <cxx> -std=c++20 -O2 tests/native/system_runtime_test.cpp system.o
//         -o system_test
//   ./system_test
//
// All generated files live in one mkdtemp directory under /tmp.

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "../../runtime/native/linux.hpp"
#include "../../runtime/native/system.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int failures = 0;

void report_failure(const char *expression, int line, long long actual,
                    long long expected) {
  std::fprintf(stderr, "FAIL line %d: %s: actual %lld expected %lld\n", line,
               expression, actual, expected);
  ++failures;
}

bool check_result(const char *expression, int line, int64_t actual,
                  int64_t expected) {
  if (actual == expected) return true;
  report_failure(expression, line, static_cast<long long>(actual),
                 static_cast<long long>(expected));
  return false;
}

bool check_true(const char *expression, int line, bool value) {
  if (value) return true;
  report_failure(expression, line, value ? 1 : 0, 1);
  return false;
}

#define CHECK_RESULT(expression, expected) \
  check_result(#expression, __LINE__, (expression), static_cast<int64_t>(expected))
#define CHECK_NAMED_RESULT(name, expression, expected) \
  check_result((name), __LINE__, (expression), static_cast<int64_t>(expected))
#define CHECK_TRUE(expression) check_true(#expression, __LINE__, (expression))

int make_path(char *buffer, size_t capacity, const char *directory,
              const char *name) {
  const int length = std::snprintf(buffer, capacity, "%s/%s", directory, name);
  if (length <= 0 || static_cast<size_t>(length) >= capacity) return -1;
  return length;
}

bool host_write_all(int fd, const void *data, size_t size) {
  const unsigned char *bytes = static_cast<const unsigned char *>(data);
  size_t done = 0;
  while (done < size) {
    const ssize_t written = write(fd, bytes + done, size - done);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (written == 0) return false;
    done += static_cast<size_t>(written);
  }
  return true;
}

bool host_read_exact(int fd, void *data, size_t size) {
  unsigned char *bytes = static_cast<unsigned char *>(data);
  size_t done = 0;
  while (done < size) {
    const ssize_t received = read(fd, bytes + done, size - done);
    if (received < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (received == 0) return false;
    done += static_cast<size_t>(received);
  }
  return true;
}

bool create_file(const char *path, const void *data, size_t size,
                 mode_t mode) {
  const int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, mode);
  if (fd < 0) return false;
  const bool written = host_write_all(fd, data, size);
  const bool closed = close(fd) == 0;
  return written && closed;
}

void remove_if_present(const char *path) {
  struct stat status = {};
  if (lstat(path, &status) != 0) return;
  if (S_ISDIR(status.st_mode))
    rmdir(path);
  else
    unlink(path);
}

void check_u64_field(const char *label, const char *field, uint64_t actual,
                     uint64_t expected) {
  if (actual == expected) return;
  std::fprintf(stderr, "FAIL %s: %s: actual %llu expected %llu\n", label,
               field, static_cast<unsigned long long>(actual),
               static_cast<unsigned long long>(expected));
  ++failures;
}

void check_i64_field(const char *label, const char *field, int64_t actual,
                     int64_t expected) {
  if (actual == expected) return;
  std::fprintf(stderr, "FAIL %s: %s: actual %lld expected %lld\n", label,
               field, static_cast<long long>(actual),
               static_cast<long long>(expected));
  ++failures;
}

void check_stat_fields(const LangFileStat &actual, const struct stat &expected,
                       const char *label) {
  check_u64_field(label, "device", actual.device,
                  static_cast<uint64_t>(expected.st_dev));
  check_u64_field(label, "inode", actual.inode,
                  static_cast<uint64_t>(expected.st_ino));
  check_u64_field(label, "links", actual.links,
                  static_cast<uint64_t>(expected.st_nlink));
  check_u64_field(label, "mode", actual.mode,
                  static_cast<uint64_t>(expected.st_mode));
  check_u64_field(label, "uid", actual.uid,
                  static_cast<uint64_t>(expected.st_uid));
  check_u64_field(label, "gid", actual.gid,
                  static_cast<uint64_t>(expected.st_gid));
  check_u64_field(label, "padding", actual.padding, 0);
  check_u64_field(label, "rdev", actual.rdev,
                  static_cast<uint64_t>(expected.st_rdev));
  check_i64_field(label, "size", actual.size,
                  static_cast<int64_t>(expected.st_size));
  check_i64_field(label, "block_size", actual.block_size,
                  static_cast<int64_t>(expected.st_blksize));
  check_i64_field(label, "blocks", actual.blocks,
                  static_cast<int64_t>(expected.st_blocks));
  check_i64_field(label, "accessed.seconds", actual.accessed.seconds,
                  static_cast<int64_t>(expected.st_atim.tv_sec));
  check_i64_field(label, "accessed.nanoseconds", actual.accessed.nanoseconds,
                  static_cast<int64_t>(expected.st_atim.tv_nsec));
  check_i64_field(label, "modified.seconds", actual.modified.seconds,
                  static_cast<int64_t>(expected.st_mtim.tv_sec));
  check_i64_field(label, "modified.nanoseconds", actual.modified.nanoseconds,
                  static_cast<int64_t>(expected.st_mtim.tv_nsec));
  check_i64_field(label, "changed.seconds", actual.changed.seconds,
                  static_cast<int64_t>(expected.st_ctim.tv_sec));
  check_i64_field(label, "changed.nanoseconds", actual.changed.nanoseconds,
                  static_cast<int64_t>(expected.st_ctim.tv_nsec));
}

template <typename Call>
void check_single_path_errors(const char *label, Call call) {
  const char embedded_nul[] = {'a', 'b', 'c', '\0', 'd', 'e'};
  CHECK_NAMED_RESULT(label, call(embedded_nul, sizeof embedded_nul), -EINVAL);

  char oversized[4096];
  std::memset(oversized, 'x', sizeof oversized);
  CHECK_NAMED_RESULT(label, call(oversized, sizeof oversized),
                     -ENAMETOOLONG);

  CHECK_NAMED_RESULT(label, call(nullptr, 4), -EFAULT);
}

template <typename Call>
void check_two_path_errors(const char *label, Call call) {
  const char good[] = "woozy-path-probe";
  const uint64_t good_length = sizeof good - 1;
  const char embedded_nul[] = {'a', 'b', 'c', '\0', 'd', 'e'};

  CHECK_NAMED_RESULT(
      label, call(embedded_nul, sizeof embedded_nul, good, good_length),
      -EINVAL);
  CHECK_NAMED_RESULT(
      label, call(good, good_length, embedded_nul, sizeof embedded_nul),
      -EINVAL);

  char oversized[4096];
  std::memset(oversized, 'x', sizeof oversized);
  CHECK_NAMED_RESULT(label, call(oversized, sizeof oversized, good, good_length),
                     -ENAMETOOLONG);
  CHECK_NAMED_RESULT(label, call(good, good_length, oversized, sizeof oversized),
                     -ENAMETOOLONG);

  CHECK_NAMED_RESULT(label, call(nullptr, 4, good, good_length), -EFAULT);
  CHECK_NAMED_RESULT(label, call(good, good_length, nullptr, 4), -EFAULT);
}

void test_linux_substrate() {
  namespace linux = lang::native::linux_x86_64;

  char destination[linux::kPathMax];
  const char source[] = "substrate/path";
  const uint64_t source_length = sizeof source - 1;
  CHECK_RESULT(linux::copy_path(source, source_length, destination), 0);
  CHECK_TRUE(std::memcmp(destination, source,
                         static_cast<size_t>(source_length)) == 0);
  CHECK_RESULT(destination[source_length], '\0');

  const char embedded_nul[] = {'a', 'b', '\0', 'c'};
  CHECK_RESULT(linux::copy_path(embedded_nul, sizeof embedded_nul,
                                destination),
               -EINVAL);

  char oversized[linux::kPathMax];
  std::memset(oversized, 'x', sizeof oversized);
  CHECK_RESULT(linux::copy_path(oversized, sizeof oversized, destination),
               -ENAMETOOLONG);
  CHECK_RESULT(linux::copy_path(nullptr, 4, destination), -EFAULT);
  CHECK_RESULT(linux::copy_path(nullptr, 0, destination), 0);
  CHECK_RESULT(destination[0], '\0');

  // mmap takes six arguments, so this exercises the r10, r8, and r9 register
  // paths in syscall6, not just the first three arguments.
  const int64_t mapped = linux::syscall6(9, 0, 4096, 3, 0x22, -1, 0);
  CHECK_TRUE(mapped > 0);
  if (mapped > 0)
    CHECK_RESULT(linux::syscall6(11, mapped, 4096, 0, 0, 0, 0), 0);

  CHECK_RESULT(linux::syscall6(39, 0, 0, 0, 0, 0, 0),
               static_cast<int64_t>(getpid()));
  CHECK_RESULT(linux::syscall6(3, -1, 0, 0, 0, 0, 0), -EBADF);
}

void test_stat(const char *directory) {
  char path[512];
  const int path_length = make_path(path, sizeof path, directory, "stat-target.bin");
  CHECK_TRUE(path_length > 0);
  if (path_length <= 0) return;

  const unsigned char payload[] = {0x00, 0x11, 0x7f, 0x80, 0xfe, 0x41};
  CHECK_TRUE(create_file(path, payload, sizeof payload, 0640));
  const uint64_t length = static_cast<uint64_t>(path_length);

  struct stat host_status = {};
  CHECK_TRUE(lstat(path, &host_status) == 0);

  LangFileStat status = {};
  CHECK_RESULT(lang_sys_stat_v1(path, length, &status), 0);
  check_stat_fields(status, host_status, "stat");

  const int fd = open(path, O_RDONLY);
  CHECK_TRUE(fd >= 0);
  if (fd >= 0) {
    struct stat host_fd_status = {};
    CHECK_TRUE(fstat(fd, &host_fd_status) == 0);
    LangFileStat fd_status = {};
    CHECK_RESULT(lang_sys_fstat_v1(fd, &fd_status), 0);
    check_stat_fields(fd_status, host_fd_status, "fstat");
    CHECK_RESULT(lang_sys_fstat_v1(fd, nullptr), -EFAULT);
    close(fd);
  }

  char link_path[512];
  const int link_length =
      make_path(link_path, sizeof link_path, directory, "stat-link");
  CHECK_TRUE(link_length > 0);
  if (link_length > 0) {
    CHECK_RESULT(lang_sys_symlink_v1(path, length, link_path,
                                     static_cast<uint64_t>(link_length)),
                 0);

    struct stat host_link_status = {};
    CHECK_TRUE(lstat(link_path, &host_link_status) == 0);
    LangFileStat link_status = {};
    CHECK_RESULT(lang_sys_lstat_v1(link_path,
                                   static_cast<uint64_t>(link_length),
                                   &link_status),
                 0);
    check_stat_fields(link_status, host_link_status, "lstat");

    LangFileStat followed_status = {};
    CHECK_RESULT(lang_sys_stat_v1(link_path,
                                  static_cast<uint64_t>(link_length),
                                  &followed_status),
                 0);
    check_stat_fields(followed_status, host_status, "stat-follow");
  }

  CHECK_RESULT(lang_sys_stat_v1(path, length, nullptr), -EFAULT);
  CHECK_RESULT(lang_sys_lstat_v1(path, length, nullptr), -EFAULT);

  // Failure leaves the output untouched.
  LangFileStat missing_status;
  missing_status.device = 0x1234567890abcdefULL;
  missing_status.inode = 0xfedcba0987654321ULL;
  char missing[512];
  const int missing_length =
      make_path(missing, sizeof missing, directory, "stat-missing.bin");
  CHECK_TRUE(missing_length > 0);
  if (missing_length > 0) {
    CHECK_RESULT(lang_sys_stat_v1(missing,
                                  static_cast<uint64_t>(missing_length),
                                  &missing_status),
                 -ENOENT);
    check_u64_field("stat-failure", "device", missing_status.device,
                    0x1234567890abcdefULL);
    check_u64_field("stat-failure", "inode", missing_status.inode,
                    0xfedcba0987654321ULL);
  }

  CHECK_RESULT(lang_sys_fstat_v1(-1, &status), -EBADF);

  remove_if_present(link_path);
  remove_if_present(path);
}

void test_rename(const char *directory) {
  char old_path[512];
  char new_path[512];
  const int old_length =
      make_path(old_path, sizeof old_path, directory, "rename-old.bin");
  const int new_length =
      make_path(new_path, sizeof new_path, directory, "rename-new.bin");
  CHECK_TRUE(old_length > 0 && new_length > 0);
  if (old_length <= 0 || new_length <= 0) return;

  const char payload[] = "rename-payload";
  CHECK_TRUE(create_file(old_path, payload, sizeof payload - 1, 0600));

  const uint64_t old_size = static_cast<uint64_t>(old_length);
  const uint64_t new_size = static_cast<uint64_t>(new_length);
  CHECK_RESULT(lang_sys_rename_v1(old_path, old_size, new_path, new_size), 0);
  CHECK_TRUE(access(old_path, F_OK) != 0);
  struct stat status = {};
  CHECK_TRUE(lstat(new_path, &status) == 0);
  CHECK_TRUE(S_ISREG(status.st_mode));

  CHECK_RESULT(lang_sys_rename_v1(old_path, old_size, new_path, new_size),
               -ENOENT);

  char missing_new[512];
  const int missing_new_length =
      make_path(missing_new, sizeof missing_new, directory, "rename-none/x");
  CHECK_TRUE(missing_new_length > 0);
  if (missing_new_length > 0) {
    CHECK_RESULT(lang_sys_rename_v1(new_path, new_size, missing_new,
                                    static_cast<uint64_t>(missing_new_length)),
                 -ENOENT);
  }

  remove_if_present(new_path);
}

void test_chmod(const char *directory) {
  char path[512];
  const int path_length =
      make_path(path, sizeof path, directory, "chmod-target.bin");
  CHECK_TRUE(path_length > 0);
  if (path_length <= 0) return;

  const char payload[] = "chmod-payload";
  CHECK_TRUE(create_file(path, payload, sizeof payload - 1, 0600));
  const uint64_t length = static_cast<uint64_t>(path_length);

  CHECK_RESULT(lang_sys_chmod_v1(path, length, 0640), 0);
  struct stat status = {};
  CHECK_TRUE(lstat(path, &status) == 0);
  CHECK_RESULT(status.st_mode & 07777, 0640);

  const int fd = open(path, O_RDONLY);
  CHECK_TRUE(fd >= 0);
  if (fd >= 0) {
    CHECK_RESULT(lang_sys_fchmod_v1(fd, 0604), 0);
    CHECK_TRUE(fstat(fd, &status) == 0);
    CHECK_RESULT(status.st_mode & 07777, 0604);
    close(fd);
  }

  CHECK_RESULT(lang_sys_fchmod_v1(-1, 0600), -EBADF);

  remove_if_present(path);
}

void test_chdir_getcwd(const char *directory) {
  char saved[4096];
  CHECK_TRUE(getcwd(saved, sizeof saved) != nullptr);
  if (getcwd(saved, sizeof saved) == nullptr) return;

  CHECK_RESULT(lang_sys_chdir_v1(directory,
                                 static_cast<uint64_t>(std::strlen(directory))),
               0);

  char current[4096];
  const int64_t current_length = lang_sys_getcwd_v1(current, sizeof current);
  CHECK_TRUE(current_length >= 0);
  if (current_length >= 0) {
    CHECK_RESULT(current_length,
                 static_cast<int64_t>(std::strlen(directory)));
    CHECK_TRUE(std::strlen(directory) == static_cast<size_t>(current_length));
    CHECK_TRUE(std::memcmp(current, directory,
                           static_cast<size_t>(current_length)) == 0);
    CHECK_TRUE(current[current_length] == '\0');
  }

  char exact[4096];
  const uint64_t exact_capacity =
      static_cast<uint64_t>(std::strlen(directory)) + 1;
  CHECK_RESULT(lang_sys_getcwd_v1(exact, exact_capacity),
               static_cast<int64_t>(exact_capacity - 1));
  CHECK_TRUE(std::memcmp(exact, directory,
                         static_cast<size_t>(exact_capacity - 1)) == 0);

  CHECK_RESULT(lang_sys_getcwd_v1(current, 1), -ERANGE);
  CHECK_RESULT(lang_sys_getcwd_v1(current, 0), -EINVAL);
  CHECK_RESULT(lang_sys_getcwd_v1(nullptr, 0), -EINVAL);
  CHECK_RESULT(lang_sys_getcwd_v1(nullptr, 16), -EFAULT);
  CHECK_RESULT(lang_sys_getcwd_v1(current, UINT64_MAX), -EINVAL);

  char missing[512];
  const int missing_length =
      make_path(missing, sizeof missing, directory, "cwd-missing");
  CHECK_TRUE(missing_length > 0);
  if (missing_length > 0) {
    CHECK_RESULT(lang_sys_chdir_v1(missing,
                                   static_cast<uint64_t>(missing_length)),
                 -ENOENT);
  }

  CHECK_RESULT(lang_sys_chdir_v1(saved, static_cast<uint64_t>(std::strlen(saved))),
               0);
}

void check_dirent_record(const unsigned char *buffer, int64_t available,
                         int64_t offset, const char *label, bool *saw_dot,
                         bool *saw_dotdot, bool *saw_alpha, bool *saw_beta,
                         bool *saw_gamma) {
  if (available - offset < 19) {
    std::fprintf(stderr, "FAIL %s: truncated dirent header at %lld\n", label,
                 static_cast<long long>(offset));
    ++failures;
    return;
  }

  uint64_t inode = 0;
  int64_t next_offset = 0;
  uint16_t record_length = 0;
  unsigned char type = 0;
  std::memcpy(&inode, buffer + offset, sizeof inode);
  std::memcpy(&next_offset, buffer + offset + 8, sizeof next_offset);
  std::memcpy(&record_length, buffer + offset + 16, sizeof record_length);
  std::memcpy(&type, buffer + offset + 18, sizeof type);
  (void)inode;
  (void)next_offset;
  (void)type;

  if (record_length < 19 || (record_length % 8) != 0 ||
      offset + static_cast<int64_t>(record_length) > available) {
    std::fprintf(stderr,
                 "FAIL %s: invalid record_length %u at offset %lld\n", label,
                 static_cast<unsigned>(record_length),
                 static_cast<long long>(offset));
    ++failures;
    return;
  }

  const char *name = reinterpret_cast<const char *>(buffer + offset + 19);
  const size_t name_capacity = static_cast<size_t>(record_length) - 19;
  size_t name_length = 0;
  while (name_length < name_capacity && name[name_length] != '\0')
    ++name_length;

  if (name_length >= name_capacity) {
    std::fprintf(stderr, "FAIL %s: unterminated name at offset %lld\n", label,
                 static_cast<long long>(offset));
    ++failures;
    return;
  }

  if (std::strcmp(name, ".") == 0) *saw_dot = true;
  if (std::strcmp(name, "..") == 0) *saw_dotdot = true;
  if (std::strcmp(name, "alpha") == 0) *saw_alpha = true;
  if (std::strcmp(name, "beta") == 0) *saw_beta = true;
  if (std::strcmp(name, "gamma") == 0) *saw_gamma = true;
}

void test_getdents(const char *directory) {
  char subdirectory[512];
  const int subdirectory_length =
      make_path(subdirectory, sizeof subdirectory, directory, "getdents-dir");
  CHECK_TRUE(subdirectory_length > 0);
  if (subdirectory_length <= 0) return;

  CHECK_TRUE(mkdir(subdirectory, 0700) == 0);

  const char *names[] = {"alpha", "beta", "gamma"};
  char child[512];
  for (const char *name : names) {
    const int child_length =
        make_path(child, sizeof child, subdirectory, name);
    CHECK_TRUE(child_length > 0);
    if (child_length > 0) CHECK_TRUE(create_file(child, "", 0, 0600));
  }

  const int fd = open(subdirectory, O_RDONLY | O_DIRECTORY);
  CHECK_TRUE(fd >= 0);
  if (fd >= 0) {
    unsigned char buffer[4096];
    const int64_t count = lang_sys_getdents_v1(fd, buffer, sizeof buffer);
    CHECK_TRUE(count > 0);
    if (count > 0) {
      bool saw_dot = false;
      bool saw_dotdot = false;
      bool saw_alpha = false;
      bool saw_beta = false;
      bool saw_gamma = false;
      int64_t offset = 0;
      while (offset < count) {
        check_dirent_record(buffer, count, offset, "getdents", &saw_dot,
                            &saw_dotdot, &saw_alpha, &saw_beta, &saw_gamma);
        uint16_t record_length = 0;
        if (count - offset < 19) break;
        std::memcpy(&record_length, buffer + offset + 16,
                    sizeof record_length);
        if (record_length < 19 || offset + record_length > count) break;
        offset += record_length;
      }
      CHECK_RESULT(offset, count);
      CHECK_TRUE(saw_dot);
      CHECK_TRUE(saw_dotdot);
      CHECK_TRUE(saw_alpha);
      CHECK_TRUE(saw_beta);
      CHECK_TRUE(saw_gamma);
    }

    CHECK_RESULT(lang_sys_getdents_v1(fd, buffer, sizeof buffer), 0);

    unsigned char sentinel[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_RESULT(lang_sys_getdents_v1(fd, sentinel, 0), 0);
    for (int index = 0; index < 8; ++index)
      CHECK_RESULT(sentinel[index], index + 1);

    CHECK_RESULT(lang_sys_getdents_v1(fd, nullptr, 0), 0);
    CHECK_RESULT(lang_sys_getdents_v1(fd, nullptr, sizeof buffer), -EFAULT);
    CHECK_RESULT(lang_sys_getdents_v1(fd, buffer, UINT64_MAX), -EINVAL);
    close(fd);
  }

  CHECK_RESULT(lang_sys_getdents_v1(-1, nullptr, 1), -EFAULT);
  unsigned char invalid_fd_buffer[16] = {};
  CHECK_RESULT(lang_sys_getdents_v1(-1, invalid_fd_buffer,
                                    sizeof invalid_fd_buffer),
               -EBADF);
  CHECK_RESULT(lang_sys_getdents_v1(-1, invalid_fd_buffer, 0), 0);

  for (const char *name : names) {
    if (make_path(child, sizeof child, subdirectory, name) > 0)
      remove_if_present(child);
  }
  remove_if_present(subdirectory);
}

void test_readlink_symlink(const char *directory) {
  char target[512];
  char link_path[512];
  const int target_length =
      make_path(target, sizeof target, directory, "readlink-target.bin");
  const int link_length =
      make_path(link_path, sizeof link_path, directory, "readlink-link");
  CHECK_TRUE(target_length > 0 && link_length > 0);
  if (target_length <= 0 || link_length <= 0) return;

  const char payload[] = "readlink-payload";
  CHECK_TRUE(create_file(target, payload, sizeof payload - 1, 0600));

  const uint64_t target_size = static_cast<uint64_t>(target_length);
  const uint64_t link_size = static_cast<uint64_t>(link_length);
  CHECK_RESULT(lang_sys_symlink_v1(target, target_size, link_path, link_size),
               0);

  char buffer[512];
  const int64_t count = lang_sys_readlink_v1(link_path, link_size, buffer,
                                             sizeof buffer);
  CHECK_RESULT(count, target_length);
  if (count >= 0) {
    CHECK_TRUE(std::memcmp(buffer, target, static_cast<size_t>(count)) == 0);
  }

  char short_buffer[8] = {};
  const int64_t short_count =
      lang_sys_readlink_v1(link_path, link_size, short_buffer, 5);
  CHECK_RESULT(short_count, 5);
  CHECK_TRUE(std::memcmp(short_buffer, target, 5) == 0);

  char untouched[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK_RESULT(lang_sys_readlink_v1(link_path, link_size, untouched, 0), 0);
  for (int index = 0; index < 8; ++index)
    CHECK_RESULT(untouched[index], index + 1);

  CHECK_RESULT(lang_sys_readlink_v1(link_path, link_size, nullptr, 0), 0);
  CHECK_RESULT(lang_sys_readlink_v1(link_path, link_size, nullptr, 16),
               -EFAULT);
  CHECK_RESULT(lang_sys_readlink_v1(link_path, link_size, buffer, UINT64_MAX),
               -EINVAL);

  char missing[512];
  const int missing_length =
      make_path(missing, sizeof missing, directory, "readlink-missing");
  CHECK_TRUE(missing_length > 0);
  if (missing_length > 0) {
    CHECK_RESULT(lang_sys_readlink_v1(missing,
                                      static_cast<uint64_t>(missing_length),
                                      buffer, sizeof buffer),
                 -ENOENT);
  }

  CHECK_RESULT(lang_sys_readlink_v1(target, target_size, buffer, sizeof buffer),
               -EINVAL);

  remove_if_present(link_path);
  remove_if_present(target);
}

void test_link(const char *directory) {
  char source[512];
  char hard[512];
  const int source_length =
      make_path(source, sizeof source, directory, "link-source.bin");
  const int hard_length =
      make_path(hard, sizeof hard, directory, "link-hard.bin");
  CHECK_TRUE(source_length > 0 && hard_length > 0);
  if (source_length <= 0 || hard_length <= 0) return;

  const char payload[] = "link-payload";
  CHECK_TRUE(create_file(source, payload, sizeof payload - 1, 0600));

  const uint64_t source_size = static_cast<uint64_t>(source_length);
  const uint64_t hard_size = static_cast<uint64_t>(hard_length);
  CHECK_RESULT(lang_sys_link_v1(source, source_size, hard, hard_size), 0);

  struct stat source_status = {};
  struct stat hard_status = {};
  CHECK_TRUE(lstat(source, &source_status) == 0);
  CHECK_TRUE(lstat(hard, &hard_status) == 0);
  CHECK_RESULT(source_status.st_ino, hard_status.st_ino);
  CHECK_RESULT(source_status.st_nlink, 2);
  CHECK_RESULT(hard_status.st_nlink, 2);

  CHECK_RESULT(lang_sys_link_v1(source, source_size, hard, hard_size),
               -EEXIST);

  char missing[512];
  const int missing_length =
      make_path(missing, sizeof missing, directory, "link-missing.bin");
  CHECK_TRUE(missing_length > 0);
  if (missing_length > 0) {
    CHECK_RESULT(lang_sys_link_v1(missing,
                                  static_cast<uint64_t>(missing_length),
                                  hard, hard_size),
                 -ENOENT);
  }

  remove_if_present(hard);
  remove_if_present(source);
}

void test_truncate(const char *directory) {
  char path[512];
  const int path_length =
      make_path(path, sizeof path, directory, "truncate-target.bin");
  CHECK_TRUE(path_length > 0);
  if (path_length <= 0) return;

  const char payload[] = "truncate-payload";
  CHECK_TRUE(create_file(path, payload, sizeof payload - 1, 0600));
  const uint64_t length = static_cast<uint64_t>(path_length);

  struct stat status = {};
  CHECK_RESULT(lang_sys_truncate_v1(path, length, 12345), 0);
  CHECK_TRUE(lstat(path, &status) == 0);
  CHECK_RESULT(status.st_size, 12345);

  CHECK_RESULT(lang_sys_truncate_v1(path, length, 0), 0);
  CHECK_TRUE(lstat(path, &status) == 0);
  CHECK_RESULT(status.st_size, 0);

  CHECK_RESULT(lang_sys_truncate_v1(path, length, -1), -EINVAL);

  char missing[512];
  const int missing_length =
      make_path(missing, sizeof missing, directory, "truncate-missing.bin");
  CHECK_TRUE(missing_length > 0);
  if (missing_length > 0) {
    CHECK_RESULT(lang_sys_truncate_v1(missing,
                                      static_cast<uint64_t>(missing_length),
                                      0),
                 -ENOENT);
  }

  const int fd = open(path, O_RDWR);
  CHECK_TRUE(fd >= 0);
  if (fd >= 0) {
    CHECK_RESULT(lang_sys_ftruncate_v1(fd, 7), 0);
    CHECK_TRUE(fstat(fd, &status) == 0);
    CHECK_RESULT(status.st_size, 7);
    CHECK_RESULT(lang_sys_ftruncate_v1(fd, -1), -EINVAL);
    close(fd);
  }
  CHECK_RESULT(lang_sys_ftruncate_v1(-1, 0), -EBADF);

  remove_if_present(path);
}

void test_sync(const char *directory) {
  char path[512];
  const int path_length =
      make_path(path, sizeof path, directory, "sync-target.bin");
  CHECK_TRUE(path_length > 0);
  if (path_length <= 0) return;

  const int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
  CHECK_TRUE(fd >= 0);
  if (fd >= 0) {
    const char payload[] = "sync-payload";
    CHECK_TRUE(host_write_all(fd, payload, sizeof payload - 1));
    CHECK_RESULT(lang_sys_fdatasync_v1(fd), 0);
    CHECK_RESULT(lang_sys_fsync_v1(fd), 0);
    close(fd);
  }

  CHECK_RESULT(lang_sys_fdatasync_v1(-1), -EBADF);
  CHECK_RESULT(lang_sys_fsync_v1(-1), -EBADF);

  remove_if_present(path);
}

void test_dup_pipe(const char *directory) {
  char path[512];
  const int path_length =
      make_path(path, sizeof path, directory, "dup-target.bin");
  CHECK_TRUE(path_length > 0);
  if (path_length <= 0) return;

  const char payload[] = "duplicate-byte-payload";
  CHECK_TRUE(create_file(path, payload, sizeof payload - 1, 0600));

  const int fd = open(path, O_RDONLY);
  CHECK_TRUE(fd >= 0);
  if (fd >= 0) {
    const int64_t duplicate = lang_sys_dup_v1(fd);
    CHECK_TRUE(duplicate >= 0);
    if (duplicate >= 0) {
      char first[sizeof payload - 1] = {};
      CHECK_TRUE(host_read_exact(fd, first, sizeof first));
      CHECK_TRUE(std::memcmp(first, payload, sizeof first) == 0);

      CHECK_RESULT(lseek(fd, 0, SEEK_SET), 0);
      char second[sizeof payload - 1] = {};
      CHECK_TRUE(host_read_exact(static_cast<int>(duplicate), second,
                                 sizeof second));
      CHECK_TRUE(std::memcmp(second, payload, sizeof second) == 0);

      const int free_fd = fcntl(fd, F_DUPFD, 0);
      CHECK_TRUE(free_fd >= 0);
      if (free_fd >= 0) {
        close(free_fd);
        const int64_t duplicated_to = lang_sys_dup2_v1(fd, free_fd);
        CHECK_RESULT(duplicated_to, free_fd);
        if (duplicated_to == free_fd) {
          CHECK_RESULT(lseek(fd, 0, SEEK_SET), 0);
          char third[sizeof payload - 1] = {};
          CHECK_TRUE(host_read_exact(free_fd, third, sizeof third));
          CHECK_TRUE(std::memcmp(third, payload, sizeof third) == 0);
          CHECK_RESULT(lang_sys_dup2_v1(fd, fd),
                       static_cast<int64_t>(fd));
          close(free_fd);
        } else {
          close(free_fd);
        }
      }
      close(static_cast<int>(duplicate));
    }
    close(fd);
  }

  CHECK_RESULT(lang_sys_dup_v1(-1), -EBADF);
  CHECK_RESULT(lang_sys_dup2_v1(-1, 0), -EBADF);
  CHECK_RESULT(lang_sys_dup2_v1(0, -1), -EBADF);

  int32_t pipe_descriptors[2] = {-1, -1};
  CHECK_RESULT(lang_sys_pipe_v1(pipe_descriptors, O_CLOEXEC), 0);
  CHECK_TRUE(pipe_descriptors[0] >= 0);
  CHECK_TRUE(pipe_descriptors[1] >= 0);
  if (pipe_descriptors[0] >= 0 && pipe_descriptors[1] >= 0) {
    const int read_flags = fcntl(pipe_descriptors[0], F_GETFD);
    const int write_flags = fcntl(pipe_descriptors[1], F_GETFD);
    CHECK_TRUE(read_flags >= 0 && (read_flags & FD_CLOEXEC) != 0);
    CHECK_TRUE(write_flags >= 0 && (write_flags & FD_CLOEXEC) != 0);

    const unsigned char pipe_payload[] = {0x00, 0xff, 0x42, 0x7f, 0x80};
    CHECK_TRUE(host_write_all(pipe_descriptors[1], pipe_payload,
                              sizeof pipe_payload));
    unsigned char pipe_readback[sizeof pipe_payload] = {};
    CHECK_TRUE(host_read_exact(pipe_descriptors[0], pipe_readback,
                               sizeof pipe_readback));
    CHECK_TRUE(std::memcmp(pipe_readback, pipe_payload,
                           sizeof pipe_payload) == 0);

    close(pipe_descriptors[0]);
    close(pipe_descriptors[1]);
  }

  int32_t sentinel[2] = {-7, -9};
  CHECK_RESULT(lang_sys_pipe_v1(sentinel, 0x80000000u), -EINVAL);
  CHECK_RESULT(sentinel[0], -7);
  CHECK_RESULT(sentinel[1], -9);
  CHECK_RESULT(lang_sys_pipe_v1(nullptr, 0), -EFAULT);

  remove_if_present(path);
}

void test_mkfifo_access(const char *directory) {
  char fifo_path[512];
  char access_path[512];
  const int fifo_length =
      make_path(fifo_path, sizeof fifo_path, directory, "fifo-target");
  const int access_length =
      make_path(access_path, sizeof access_path, directory, "access-target.bin");
  CHECK_TRUE(fifo_length > 0 && access_length > 0);
  if (fifo_length <= 0 || access_length <= 0) return;

  const mode_t previous_umask = umask(0);
  CHECK_RESULT(lang_sys_mkfifo_v1(fifo_path,
                                  static_cast<uint64_t>(fifo_length), 0600),
               0);
  umask(previous_umask);

  struct stat fifo_status = {};
  CHECK_TRUE(lstat(fifo_path, &fifo_status) == 0);
  CHECK_TRUE(S_ISFIFO(fifo_status.st_mode));
  CHECK_RESULT(fifo_status.st_mode & 07777, 0600);
  CHECK_RESULT(lang_sys_mkfifo_v1(fifo_path,
                                  static_cast<uint64_t>(fifo_length), 0600),
               -EEXIST);

  const char payload[] = "access-payload";
  CHECK_TRUE(create_file(access_path, payload, sizeof payload - 1, 0600));
  CHECK_RESULT(lang_sys_chmod_v1(access_path,
                                 static_cast<uint64_t>(access_length), 0600),
               0);
  CHECK_RESULT(lang_sys_access_v1(access_path,
                                  static_cast<uint64_t>(access_length), F_OK),
               0);
  CHECK_RESULT(lang_sys_access_v1(access_path,
                                  static_cast<uint64_t>(access_length), R_OK),
               0);
  CHECK_RESULT(lang_sys_access_v1(access_path,
                                  static_cast<uint64_t>(access_length), 8),
               -EINVAL);

  char missing[512];
  const int missing_length =
      make_path(missing, sizeof missing, directory, "access-missing.bin");
  CHECK_TRUE(missing_length > 0);
  if (missing_length > 0) {
    CHECK_RESULT(lang_sys_access_v1(missing,
                                    static_cast<uint64_t>(missing_length),
                                    F_OK),
                 -ENOENT);
  }

  remove_if_present(fifo_path);
  remove_if_present(access_path);
}

void test_ids_and_process_groups() {
  CHECK_RESULT(lang_sys_getuid_v1(), static_cast<int64_t>(getuid()));
  CHECK_RESULT(lang_sys_geteuid_v1(), static_cast<int64_t>(geteuid()));
  CHECK_RESULT(lang_sys_getgid_v1(), static_cast<int64_t>(getgid()));
  CHECK_RESULT(lang_sys_getegid_v1(), static_cast<int64_t>(getegid()));
  CHECK_RESULT(lang_sys_getppid_v1(), static_cast<int64_t>(getppid()));
  CHECK_RESULT(lang_sys_gettid_v1(),
               static_cast<int64_t>(syscall(SYS_gettid)));
  CHECK_RESULT(lang_sys_getpgid_v1(0), static_cast<int64_t>(getpgid(0)));
  CHECK_RESULT(lang_sys_getpgid_v1(getpid()),
               static_cast<int64_t>(getpgid(getpid())));

  CHECK_RESULT(lang_sys_kill_v1(getpid(), 0), 0);
  CHECK_RESULT(lang_sys_kill_v1(getpid(), 0x7fffffff), -EINVAL);

  // Isolated child: a forked process is not a process-group leader, so
  // setpgid(0, 0) succeeds and makes its pid the new process-group id.
  const pid_t group_child = fork();
  CHECK_TRUE(group_child >= 0);
  if (group_child == 0) {
    if (lang_sys_setpgid_v1(0, 0) != 0) _exit(10);
    if (lang_sys_getpgid_v1(0) != static_cast<int64_t>(getpid())) _exit(11);
    if (lang_sys_kill_v1(static_cast<int32_t>(getpid()), 0) != 0) _exit(12);
    _exit(0);
  }
  if (group_child > 0) {
    int status = 0;
    CHECK_TRUE(waitpid(group_child, &status, 0) == group_child);
    CHECK_TRUE(WIFEXITED(status));
    if (WIFEXITED(status)) CHECK_RESULT(WEXITSTATUS(status), 0);
  }

  // Isolated child: setsid must succeed for a process that is not already a
  // process-group leader. Note that Linux then rejects a following setpgid on
  // the session leader with EPERM, so this child only reads the new group and
  // sends itself signal 0.
  const pid_t session_child = fork();
  CHECK_TRUE(session_child >= 0);
  if (session_child == 0) {
    const int64_t session = lang_sys_setsid_v1();
    if (session != static_cast<int64_t>(getpid())) _exit(20);
    if (lang_sys_getpgid_v1(0) != static_cast<int64_t>(getpid())) _exit(21);
    if (lang_sys_kill_v1(static_cast<int32_t>(getpid()), 0) != 0) _exit(22);
    _exit(0);
  }
  if (session_child > 0) {
    int status = 0;
    CHECK_TRUE(waitpid(session_child, &status, 0) == session_child);
    CHECK_TRUE(WIFEXITED(status));
    if (WIFEXITED(status)) CHECK_RESULT(WEXITSTATUS(status), 0);
  }
}

void on_sigusr1_exit(int) { _exit(0); }

void test_kill_signal() {
  int ready[2] = {-1, -1};
  CHECK_TRUE(pipe(ready) == 0);
  if (ready[0] < 0) return;

  const pid_t child = fork();
  CHECK_TRUE(child >= 0);
  if (child < 0) {
    close(ready[0]);
    close(ready[1]);
    return;
  }

  if (child == 0) {
    close(ready[0]);
    struct sigaction action = {};
    action.sa_handler = on_sigusr1_exit;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGUSR1, &action, nullptr) != 0) _exit(20);

    const char byte = 'R';
    if (write(ready[1], &byte, 1) != 1) _exit(21);
    close(ready[1]);
    for (;;) pause();
  }

  close(ready[1]);
  char byte = 0;
  const bool child_ready = read(ready[0], &byte, 1) == 1;
  close(ready[0]);
  CHECK_TRUE(child_ready);

  if (child_ready) {
    CHECK_RESULT(lang_sys_kill_v1(child, SIGUSR1), 0);
  } else {
    lang_sys_kill_v1(child, SIGKILL);
  }

  int status = 0;
  CHECK_TRUE(waitpid(child, &status, 0) == child);
  if (child_ready) {
    CHECK_TRUE(WIFEXITED(status));
    if (WIFEXITED(status)) CHECK_RESULT(WEXITSTATUS(status), 0);
  }
}

int64_t timespec_to_nanoseconds(const LangTimespec &value) {
  return value.seconds * INT64_C(1000000000) + value.nanoseconds;
}

int64_t host_timespec_to_nanoseconds(const struct timespec &value) {
  return static_cast<int64_t>(value.tv_sec) * INT64_C(1000000000) +
         static_cast<int64_t>(value.tv_nsec);
}

void test_clock_gettime() {
  struct timespec host_before = {};
  struct timespec host_after = {};
  CHECK_TRUE(clock_gettime(CLOCK_MONOTONIC, &host_before) == 0);
  LangTimespec monotonic = {};
  CHECK_RESULT(lang_sys_clock_gettime_v1(CLOCK_MONOTONIC, &monotonic), 0);
  CHECK_TRUE(clock_gettime(CLOCK_MONOTONIC, &host_after) == 0);

  CHECK_TRUE(monotonic.nanoseconds >= 0);
  CHECK_TRUE(monotonic.nanoseconds < INT64_C(1000000000));
  const int64_t monotonic_ns = timespec_to_nanoseconds(monotonic);
  CHECK_TRUE(monotonic_ns >= host_timespec_to_nanoseconds(host_before));
  CHECK_TRUE(monotonic_ns <= host_timespec_to_nanoseconds(host_after));

  LangTimespec realtime = {};
  CHECK_RESULT(lang_sys_clock_gettime_v1(CLOCK_REALTIME, &realtime), 0);
  CHECK_TRUE(realtime.nanoseconds >= 0);
  CHECK_TRUE(realtime.nanoseconds < INT64_C(1000000000));
  CHECK_TRUE(realtime.seconds > INT64_C(1600000000));

  LangTimespec first = {};
  LangTimespec second = {};
  CHECK_RESULT(lang_sys_clock_gettime_v1(CLOCK_MONOTONIC, &first), 0);
  volatile uint64_t spin = 0;
  for (uint64_t index = 0; index < 10000000; ++index) spin += index;
  (void)spin;
  CHECK_RESULT(lang_sys_clock_gettime_v1(CLOCK_MONOTONIC, &second), 0);
  CHECK_TRUE(timespec_to_nanoseconds(second) >
             timespec_to_nanoseconds(first));

  LangTimespec failed = {};
  failed.seconds = 1234;
  failed.nanoseconds = 5678;
  CHECK_RESULT(lang_sys_clock_gettime_v1(0x7fffffff, &failed), -EINVAL);
  CHECK_RESULT(failed.seconds, 1234);
  CHECK_RESULT(failed.nanoseconds, 5678);
  CHECK_RESULT(lang_sys_clock_gettime_v1(CLOCK_MONOTONIC, nullptr), -EFAULT);
}

volatile sig_atomic_t alarm_seen = 0;

void on_alarm(int) { alarm_seen = 1; }

void test_nanosleep() {
  LangTimespec request = {};
  LangTimespec remaining = {};
  request.seconds = 0;
  request.nanoseconds = INT64_C(5000000);

  struct timespec before = {};
  struct timespec after = {};
  CHECK_TRUE(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
  CHECK_RESULT(lang_sys_nanosleep_v1(&request, &remaining), 0);
  CHECK_TRUE(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
  CHECK_TRUE(host_timespec_to_nanoseconds(after) -
                 host_timespec_to_nanoseconds(before) >=
             INT64_C(3000000));

  remaining.seconds = 111;
  remaining.nanoseconds = 222;
  CHECK_RESULT(lang_sys_nanosleep_v1(&request, &remaining), 0);
  CHECK_RESULT(remaining.seconds, 111);
  CHECK_RESULT(remaining.nanoseconds, 222);

  request.nanoseconds = 0;
  CHECK_RESULT(lang_sys_nanosleep_v1(&request, nullptr), 0);

  LangTimespec invalid = {};
  invalid.seconds = 0;
  invalid.nanoseconds = INT64_C(1000000000);
  CHECK_RESULT(lang_sys_nanosleep_v1(&invalid, &remaining), -EINVAL);
  invalid.nanoseconds = -1;
  CHECK_RESULT(lang_sys_nanosleep_v1(&invalid, &remaining), -EINVAL);
  CHECK_RESULT(lang_sys_nanosleep_v1(nullptr, &remaining), -EFAULT);

  struct sigaction action = {};
  struct sigaction previous_action = {};
  action.sa_handler = on_alarm;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0; // no SA_RESTART
  CHECK_TRUE(sigaction(SIGALRM, &action, &previous_action) == 0);

  struct itimerval timer = {};
  timer.it_value.tv_usec = 20000;
  timer.it_interval.tv_usec = 20000;
  CHECK_TRUE(setitimer(ITIMER_REAL, &timer, nullptr) == 0);

  alarm_seen = 0;
  LangTimespec long_request = {};
  long_request.seconds = 3;
  long_request.nanoseconds = 0;
  LangTimespec interrupted_remaining = {};
  interrupted_remaining.seconds = -1;
  interrupted_remaining.nanoseconds = -1;

  CHECK_TRUE(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
  const int64_t interrupted =
      lang_sys_nanosleep_v1(&long_request, &interrupted_remaining);
  CHECK_TRUE(clock_gettime(CLOCK_MONOTONIC, &after) == 0);

  struct itimerval disabled = {};
  setitimer(ITIMER_REAL, &disabled, nullptr);

  CHECK_RESULT(interrupted, -EINTR);
  CHECK_TRUE(alarm_seen != 0);
  CHECK_TRUE(interrupted_remaining.seconds >= 0);
  CHECK_TRUE(interrupted_remaining.seconds <= 3);
  CHECK_TRUE(interrupted_remaining.nanoseconds >= 0);
  CHECK_TRUE(interrupted_remaining.nanoseconds < INT64_C(1000000000));
  const int64_t remaining_ns =
      timespec_to_nanoseconds(interrupted_remaining);
  CHECK_TRUE(remaining_ns > 0);
  CHECK_TRUE(remaining_ns < INT64_C(3000000000));
  CHECK_TRUE(host_timespec_to_nanoseconds(after) -
                 host_timespec_to_nanoseconds(before) <
             INT64_C(1000000000));

  // Block SIGALRM before restoring the old disposition so a pending timer
  // signal cannot escape the harness.
  sigset_t alarm_set = {};
  sigset_t previous_mask = {};
  sigemptyset(&alarm_set);
  sigaddset(&alarm_set, SIGALRM);
  sigprocmask(SIG_BLOCK, &alarm_set, &previous_mask);
  struct timespec zero_timeout = {};
  while (sigtimedwait(&alarm_set, nullptr, &zero_timeout) == SIGALRM) {
  }
  sigaction(SIGALRM, &previous_action, nullptr);
  sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
}

void test_getrandom() {
  unsigned char buffer[64] = {};
  const int64_t count = lang_sys_getrandom_v1(buffer, sizeof buffer, 0);
  CHECK_TRUE(count > 0);
  CHECK_TRUE(count <= static_cast<int64_t>(sizeof buffer));

  unsigned char nonblocking[16] = {};
  const int64_t nonblocking_count =
      lang_sys_getrandom_v1(nonblocking, sizeof nonblocking, 1);
  CHECK_TRUE(nonblocking_count > 0);
  CHECK_TRUE(nonblocking_count <= static_cast<int64_t>(sizeof nonblocking));

  CHECK_RESULT(lang_sys_getrandom_v1(nullptr, 0, 0), 0);
  CHECK_RESULT(lang_sys_getrandom_v1(nullptr, 0, 8), 0);
  CHECK_RESULT(lang_sys_getrandom_v1(nullptr, 16, 0), -EFAULT);
  CHECK_RESULT(lang_sys_getrandom_v1(buffer, UINT64_MAX, 0), -EINVAL);
  CHECK_RESULT(lang_sys_getrandom_v1(buffer, sizeof buffer, 8), -EINVAL);
}

void test_path_validation() {
  check_single_path_errors("stat", [](const char *path, uint64_t length) {
    LangFileStat status = {};
    return lang_sys_stat_v1(path, length, &status);
  });
  check_single_path_errors("lstat", [](const char *path, uint64_t length) {
    LangFileStat status = {};
    return lang_sys_lstat_v1(path, length, &status);
  });
  check_single_path_errors("chmod", [](const char *path, uint64_t length) {
    return lang_sys_chmod_v1(path, length, 0600);
  });
  check_single_path_errors("chdir", [](const char *path, uint64_t length) {
    return lang_sys_chdir_v1(path, length);
  });
  check_single_path_errors("truncate", [](const char *path, uint64_t length) {
    return lang_sys_truncate_v1(path, length, 0);
  });
  check_single_path_errors("mkfifo", [](const char *path, uint64_t length) {
    return lang_sys_mkfifo_v1(path, length, 0600);
  });
  check_single_path_errors("access", [](const char *path, uint64_t length) {
    return lang_sys_access_v1(path, length, F_OK);
  });
  check_single_path_errors("readlink", [](const char *path, uint64_t length) {
    char buffer[8] = {};
    return lang_sys_readlink_v1(path, length, buffer, sizeof buffer);
  });

  check_two_path_errors("rename",
                        [](const char *old_path, uint64_t old_length,
                           const char *new_path, uint64_t new_length) {
                          return lang_sys_rename_v1(old_path, old_length,
                                                    new_path, new_length);
                        });
  check_two_path_errors("symlink",
                        [](const char *target, uint64_t target_length,
                           const char *link_path, uint64_t link_length) {
                          return lang_sys_symlink_v1(target, target_length,
                                                     link_path, link_length);
                        });
  check_two_path_errors("link",
                        [](const char *old_path, uint64_t old_length,
                           const char *new_path, uint64_t new_length) {
                          return lang_sys_link_v1(old_path, old_length,
                                                  new_path, new_length);
                        });

  // A zero-length path with a null pointer is accepted by the path helper and
  // passed to the kernel as the empty path.
  LangFileStat status = {};
  CHECK_RESULT(lang_sys_stat_v1(nullptr, 0, &status), -ENOENT);
  CHECK_RESULT(lang_sys_lstat_v1(nullptr, 0, &status), -ENOENT);
  CHECK_RESULT(lang_sys_chdir_v1(nullptr, 0), -ENOENT);
  CHECK_RESULT(lang_sys_access_v1(nullptr, 0, F_OK), -ENOENT);
}

void cleanup_directory(const char *directory) {
  const char *files[] = {
      "stat-target.bin", "stat-link",       "rename-old.bin",
      "rename-new.bin",  "chmod-target.bin", "readlink-target.bin",
      "readlink-link",   "link-source.bin",  "link-hard.bin",
      "truncate-target.bin", "sync-target.bin", "dup-target.bin",
      "fifo-target",     "access-target.bin"};
  char path[512];
  for (const char *file : files) {
    if (make_path(path, sizeof path, directory, file) > 0)
      remove_if_present(path);
  }

  const char *children[] = {"alpha", "beta", "gamma"};
  char subdirectory[512];
  if (make_path(subdirectory, sizeof subdirectory, directory, "getdents-dir") >
      0) {
    for (const char *child : children) {
      if (make_path(path, sizeof path, subdirectory, child) > 0)
        remove_if_present(path);
    }
    remove_if_present(subdirectory);
  }
}

} // namespace

int main() {
  char directory_template[] = "/tmp/woozy-system-runtime-XXXXXX";
  char *directory = mkdtemp(directory_template);
  if (directory == nullptr) {
    std::fprintf(stderr, "FAIL: mkdtemp: %s\n", std::strerror(errno));
    return 1;
  }

  CHECK_RESULT(sizeof(LangTimespec), 16);
  CHECK_RESULT(sizeof(LangFileStat), 120);

  test_linux_substrate();
  test_stat(directory);
  test_rename(directory);
  test_chmod(directory);
  test_chdir_getcwd(directory);
  test_getdents(directory);
  test_readlink_symlink(directory);
  test_link(directory);
  test_truncate(directory);
  test_sync(directory);
  test_dup_pipe(directory);
  test_mkfifo_access(directory);
  test_ids_and_process_groups();
  test_kill_signal();
  test_clock_gettime();
  test_nanosleep();
  test_getrandom();
  test_path_validation();

  cleanup_directory(directory);
  if (rmdir(directory) != 0)
    std::fprintf(stderr, "note: could not remove %s: %s\n", directory,
                 std::strerror(errno));

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("system runtime checks passed\n");
  return 0;
}
