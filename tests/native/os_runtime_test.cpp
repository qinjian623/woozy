// Host-side acceptance harness for runtime/native/os.cpp.
//
// The runtime object under test is freestanding and must not reference libc or
// libstdc++; this harness uses the host libc only for temporary-directory
// setup, host-side inspection, forks, signals, and diagnostics. It provides no
// fallback or wrapper implementation of any lang_os_* entry point.
//
// Build (see the test report for the exact commands used):
//   <cxx> -std=c++20 -O2 -fno-exceptions -fno-rtti -fno-sanitize=all
//         -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memmove
//         (-fno-tree-loop-distribute-patterns is added for GCC)
//         -c runtime/native/os.cpp -o os.o
//   <cxx> -std=c++20 -O2 tests/native/os_runtime_test.cpp os.o -o os_test
//   ./os_test

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "../../runtime/native/os.hpp"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
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
  report_failure(expression, line, 0, 1);
  return false;
}

#define CHECK_RESULT(expression, expected) \
  check_result(#expression, __LINE__, (expression), static_cast<int64_t>(expected))
#define CHECK_TRUE(expression) check_true(#expression, __LINE__, (expression))

int make_path(char *buffer, size_t capacity, const char *directory,
              const char *name) {
  const int length = std::snprintf(buffer, capacity, "%s/%s", directory, name);
  if (length <= 0 || static_cast<size_t>(length) >= capacity) return -1;
  return length;
}

void test_file_round_trip(const char *directory) {
  char path[512];
  const int path_length = make_path(path, sizeof path, directory, "round-trip.bin");
  CHECK_TRUE(path_length > 0);
  if (path_length <= 0) return;

  const unsigned char payload[] = {0x00, 0x01, 0x7f, 0x80,
                                   0xff, 0x41, 0x42, 0x43};
  const int64_t fd = lang_os_open_v1(
      path, static_cast<uint64_t>(path_length), O_CREAT | O_RDWR | O_EXCL, 0600);
  CHECK_TRUE(fd >= 0);
  if (fd < 0) return;
  const int32_t descriptor = static_cast<int32_t>(fd);

  CHECK_RESULT(lang_os_write_v1(descriptor, payload, sizeof payload),
               static_cast<int64_t>(sizeof payload));

  // Zero-count calls must return 0 without dereferencing the buffer, even for
  // a null or obviously invalid pointer.
  CHECK_RESULT(lang_os_read_v1(descriptor, nullptr, 0), 0);
  CHECK_RESULT(lang_os_write_v1(
                   descriptor,
                   reinterpret_cast<const void *>(static_cast<uintptr_t>(1)), 0),
               0);

  CHECK_RESULT(lang_os_seek_v1(descriptor, 0, SEEK_SET), 0);

  unsigned char readback[sizeof payload] = {};
  CHECK_RESULT(lang_os_read_v1(descriptor, readback, sizeof payload),
               static_cast<int64_t>(sizeof payload));
  CHECK_TRUE(std::memcmp(readback, payload, sizeof payload) == 0);

  CHECK_RESULT(lang_os_seek_v1(descriptor, 2, SEEK_SET), 2);
  unsigned char partial[3] = {};
  CHECK_RESULT(lang_os_read_v1(descriptor, partial, sizeof partial), 3);
  CHECK_TRUE(std::memcmp(partial, payload + 2, sizeof partial) == 0);

  CHECK_RESULT(lang_os_seek_v1(descriptor, 0, SEEK_END),
               static_cast<int64_t>(sizeof payload));

  // A 64-bit offset beyond the 32-bit range must survive the register ABI.
  const int64_t wide_offset = INT64_C(0x100000000);
  CHECK_RESULT(lang_os_seek_v1(descriptor, wide_offset, SEEK_SET),
               wide_offset);
  CHECK_RESULT(lang_os_seek_v1(descriptor, 0, SEEK_SET), 0);

  CHECK_RESULT(lang_os_close_v1(descriptor), 0);
  CHECK_RESULT(lang_os_close_v1(-1), -EBADF);
  CHECK_RESULT(lang_os_seek_v1(-1, 0, SEEK_SET), -EBADF);

  unsigned char byte = 0;
  CHECK_RESULT(lang_os_read_v1(-1, &byte, 1), -EBADF);
  CHECK_RESULT(lang_os_write_v1(-1, payload, sizeof payload), -EBADF);

  CHECK_RESULT(lang_os_unlink_v1(path, static_cast<uint64_t>(path_length)), 0);
  CHECK_RESULT(lang_os_open_v1(path, static_cast<uint64_t>(path_length),
                               O_RDONLY, 0),
               -ENOENT);
  CHECK_RESULT(lang_os_unlink_v1(path, static_cast<uint64_t>(path_length)),
               -ENOENT);
}

void test_missing_paths(const char *directory) {
  char missing[512];
  const int length =
      make_path(missing, sizeof missing, directory, "does-not-exist.bin");
  CHECK_TRUE(length > 0);
  if (length <= 0) return;

  CHECK_RESULT(lang_os_open_v1(missing, static_cast<uint64_t>(length), O_RDONLY,
                               0),
               -ENOENT);
  CHECK_RESULT(lang_os_unlink_v1(missing, static_cast<uint64_t>(length)),
               -ENOENT);
  CHECK_RESULT(lang_os_rmdir_v1(missing, static_cast<uint64_t>(length)),
               -ENOENT);

  // A zero-length borrowed path reads no bytes; the kernel sees "".
  CHECK_RESULT(lang_os_open_v1(nullptr, 0, O_RDONLY, 0), -ENOENT);
}

void test_path_validation(const char *directory) {
  const char embedded_nul[] = {'a', 'b', 'c', '\0', 'd', 'e', 'f'};
  CHECK_RESULT(lang_os_open_v1(embedded_nul, sizeof embedded_nul, O_RDONLY, 0),
               -EINVAL);
  CHECK_RESULT(lang_os_unlink_v1(embedded_nul, sizeof embedded_nul), -EINVAL);
  CHECK_RESULT(lang_os_mkdir_v1(embedded_nul, sizeof embedded_nul, 0700),
               -EINVAL);
  CHECK_RESULT(lang_os_rmdir_v1(embedded_nul, sizeof embedded_nul), -EINVAL);

  char oversized[4096];
  std::memset(oversized, 'a', sizeof oversized);
  CHECK_RESULT(lang_os_open_v1(oversized, sizeof oversized, O_RDONLY, 0),
               -ENAMETOOLONG);
  CHECK_RESULT(lang_os_unlink_v1(oversized, sizeof oversized), -ENAMETOOLONG);
  CHECK_RESULT(lang_os_mkdir_v1(oversized, sizeof oversized, 0700),
               -ENAMETOOLONG);
  CHECK_RESULT(lang_os_rmdir_v1(oversized, sizeof oversized), -ENAMETOOLONG);

  // The bytes after the supplied length are readable garbage, not a NUL. A
  // runtime that reads past `length` would use a different name.
  char bounded[512];
  const int bounded_length =
      make_path(bounded, sizeof bounded, directory, "bounded-name.bin");
  CHECK_TRUE(bounded_length > 0);
  if (bounded_length <= 0) return;
  std::memset(bounded + bounded_length, 'Z',
              sizeof bounded - static_cast<size_t>(bounded_length));

  const int64_t created = lang_os_open_v1(
      bounded, static_cast<uint64_t>(bounded_length),
      O_CREAT | O_RDWR | O_EXCL, 0600);
  CHECK_TRUE(created >= 0);
  if (created >= 0) CHECK_RESULT(lang_os_close_v1(static_cast<int32_t>(created)), 0);

  char intended[512];
  std::memcpy(intended, bounded, static_cast<size_t>(bounded_length));
  intended[bounded_length] = '\0';
  struct stat status = {};
  CHECK_TRUE(stat(intended, &status) == 0 && S_ISREG(status.st_mode));
  CHECK_RESULT(lang_os_unlink_v1(bounded, static_cast<uint64_t>(bounded_length)),
               0);
}

void test_relative_openat(const char *directory) {
  // Relative paths prove that openat really receives AT_FDCWD rather than an
  // arbitrary directory descriptor.
  const int saved_directory = open(".", O_RDONLY);
  CHECK_TRUE(saved_directory >= 0);
  if (saved_directory < 0) return;
  if (chdir(directory) != 0) {
    CHECK_TRUE(false);
    close(saved_directory);
    return;
  }

  const char name[] = "relative.bin";
  const uint64_t length = sizeof name - 1;
  const int64_t fd = lang_os_open_v1(name, length, O_CREAT | O_RDWR | O_EXCL,
                                     0600);
  CHECK_TRUE(fd >= 0);
  if (fd >= 0) {
    const char byte = 'R';
    CHECK_RESULT(lang_os_write_v1(static_cast<int32_t>(fd), &byte, 1), 1);
    CHECK_RESULT(lang_os_seek_v1(static_cast<int32_t>(fd), 0, SEEK_SET), 0);
    char readback = 0;
    CHECK_RESULT(lang_os_read_v1(static_cast<int32_t>(fd), &readback, 1), 1);
    CHECK_TRUE(readback == 'R');
    CHECK_RESULT(lang_os_close_v1(static_cast<int32_t>(fd)), 0);
    CHECK_RESULT(lang_os_unlink_v1(name, length), 0);
  }

  CHECK_TRUE(fchdir(saved_directory) == 0);
  close(saved_directory);
}

void test_mkdir_rmdir(const char *directory) {
  char subdirectory[512];
  const int length =
      make_path(subdirectory, sizeof subdirectory, directory, "subdirectory");
  CHECK_TRUE(length > 0);
  if (length <= 0) return;

  CHECK_RESULT(lang_os_mkdir_v1(subdirectory, static_cast<uint64_t>(length),
                                0700),
               0);
  struct stat status = {};
  CHECK_TRUE(stat(subdirectory, &status) == 0 && S_ISDIR(status.st_mode));
  CHECK_RESULT(lang_os_mkdir_v1(subdirectory, static_cast<uint64_t>(length),
                                0700),
               -EEXIST);
  CHECK_RESULT(lang_os_rmdir_v1(subdirectory, static_cast<uint64_t>(length)), 0);
  CHECK_RESULT(lang_os_rmdir_v1(subdirectory, static_cast<uint64_t>(length)),
               -ENOENT);
}

void test_getpid() {
  const int64_t pid = lang_os_getpid_v1();
  CHECK_TRUE(pid > 0);
  CHECK_RESULT(pid, static_cast<int64_t>(getpid()));
}

void test_exit_status() {
  const pid_t child = fork();
  CHECK_TRUE(child >= 0);
  if (child < 0) return;
  if (child == 0) {
    lang_os_exit_v1(42);
    _exit(120); // Must be unreachable.
  }

  int status = 0;
  CHECK_TRUE(waitpid(child, &status, 0) == child);
  CHECK_TRUE(WIFEXITED(status));
  if (WIFEXITED(status)) CHECK_RESULT(WEXITSTATUS(status), 42);
}

volatile sig_atomic_t alarm_count = 0;

void on_alarm(int) {
  // If the runtime retried an interrupted read instead of returning -EINTR,
  // the repeating timer would keep waking it. Bail out after a bounded number
  // so the harness cannot hang.
  alarm_count = static_cast<sig_atomic_t>(alarm_count + 1);
  if (alarm_count > 128) _exit(121);
}

void test_eintr_is_returned_unchanged() {
  int pipe_fds[2] = {-1, -1};
  if (!CHECK_TRUE(pipe(pipe_fds) == 0)) return;

  const pid_t child = fork();
  CHECK_TRUE(child >= 0);
  if (child < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return;
  }

  if (child == 0) {
    close(pipe_fds[1]);

    struct sigaction action = {};
    action.sa_handler = on_alarm;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, nullptr) != 0) _exit(124);

    struct itimerval timer = {};
    timer.it_value.tv_usec = 20000;
    timer.it_interval.tv_usec = 20000;
    if (setitimer(ITIMER_REAL, &timer, nullptr) != 0) _exit(125);

    unsigned char byte = 0;
    const int64_t result = lang_os_read_v1(pipe_fds[0], &byte, 1);
    _exit(result == -EINTR ? 0 : 126);
  }

  // Keep the write end open so the child's blocking read cannot observe EOF.
  close(pipe_fds[0]);
  int status = 0;
  CHECK_TRUE(waitpid(child, &status, 0) == child);
  CHECK_TRUE(WIFEXITED(status));
  if (WIFEXITED(status)) CHECK_RESULT(WEXITSTATUS(status), 0);
  close(pipe_fds[1]);
}

void remove_if_present(const char *directory, const char *name) {
  char path[512];
  const int length = make_path(path, sizeof path, directory, name);
  if (length <= 0) return;
  struct stat status = {};
  if (stat(path, &status) != 0) return;
  if (S_ISDIR(status.st_mode))
    lang_os_rmdir_v1(path, static_cast<uint64_t>(length));
  else
    lang_os_unlink_v1(path, static_cast<uint64_t>(length));
}

} // namespace

int main() {
  char directory_template[] = "/tmp/woozy-os-runtime-XXXXXX";
  char *directory = mkdtemp(directory_template);
  if (!directory) {
    std::fprintf(stderr, "FAIL: mkdtemp: %s\n", std::strerror(errno));
    return 1;
  }

  test_file_round_trip(directory);
  test_missing_paths(directory);
  test_relative_openat(directory);
  test_path_validation(directory);
  test_mkdir_rmdir(directory);
  test_getpid();
  test_exit_status();
  test_eintr_is_returned_unchanged();

  // Best-effort cleanup if an earlier check failed before its own unlink.
  remove_if_present(directory, "round-trip.bin");
  remove_if_present(directory, "bounded-name.bin");
  remove_if_present(directory, "relative.bin");
  remove_if_present(directory, "subdirectory");
  if (rmdir(directory) != 0)
    std::fprintf(stderr, "note: could not remove %s: %s\n", directory,
                 std::strerror(errno));

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("os runtime checks passed\n");
  return 0;
}
