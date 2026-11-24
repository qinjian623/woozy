// Host-side acceptance harness for the descriptor-relative runtime ABI.
//
// The new entry points are called directly through runtime/native/os.hpp and
// runtime/native/system.hpp. Host libc is used only to create and inspect the
// temporary directory tree; it is not a fallback implementation of any
// lang_os_* / lang_sys_* function.
#include "../../runtime/native/os.hpp"
#include "../../runtime/native/system.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int failures = 0;

void fail(const char *what, int line, long long actual, long long expected) {
  std::fprintf(stderr, "FAIL line %d: %s: actual %lld expected %lld\n", line,
               what, actual, expected);
  ++failures;
}

void check(const char *what, int line, int64_t actual, int64_t expected) {
  if (actual == expected) return;
  fail(what, line, static_cast<long long>(actual),
       static_cast<long long>(expected));
}

void check_true(const char *what, int line, bool value) {
  if (value) return;
  fail(what, line, 0, 1);
}

#define CHECK(actual, expected) \
  check(#actual, __LINE__, static_cast<int64_t>(actual), expected)
#define CHECK_TRUE(value) check_true(#value, __LINE__, (value))

struct Directory {
  int fd = -1;

  explicit Directory(const char *path) {
    fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK_TRUE(fd >= 0);
  }
  ~Directory() {
    if (fd >= 0) close(fd);
  }
  Directory(const Directory &) = delete;
  Directory &operator=(const Directory &) = delete;
};

void path_validation(int parent) {
  const char nul_component[] = {'a', '\0', 'b'};
  const char empty[] = "";
  CHECK(lang_os_openat_v1(parent, empty, 0, O_RDONLY, 0), -EINVAL);
  CHECK(lang_os_openat_v1(parent, ".", 1, O_RDONLY, 0), -EINVAL);
  CHECK(lang_os_openat_v1(parent, "..", 2, O_RDONLY, 0), -EINVAL);
  CHECK(lang_os_openat_v1(parent, "a/b", 3, O_RDONLY, 0), -EINVAL);
  CHECK(lang_os_openat_v1(parent, nul_component, sizeof nul_component,
                          O_RDONLY, 0),
        -EINVAL);
  CHECK(lang_os_openat_v1(parent, nullptr, 1, O_RDONLY, 0), -EFAULT);
  char long_name[4096];
  std::memset(long_name, 'a', sizeof long_name);
  CHECK(lang_os_openat_v1(parent, long_name, sizeof long_name, O_RDONLY, 0),
        -ENAMETOOLONG);

  CHECK(lang_os_mkdirat_v1(parent, "a/b", 3, 0700), -EINVAL);
  CHECK(lang_os_mkdirat_v1(parent, "", 0, 0700), -EINVAL);
  CHECK(lang_os_unlinkat_v1(parent, "a/b", 3, 0), -EINVAL);
  LangFileStat status{};
  CHECK(lang_sys_fstatat_v1(parent, "a/b", 3, 0, &status), -EINVAL);
  CHECK(lang_sys_fstatat_v1(parent, "name", 4, 2, &status), -EINVAL);
  CHECK(lang_os_unlinkat_v1(parent, "name", 4, 2), -EINVAL);
}

void create_file(int parent, const char *name, size_t length) {
  const int64_t fd = lang_os_openat_v1(
      parent, name, length, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  CHECK_TRUE(fd >= 0);
  if (fd >= 0) CHECK(lang_os_close_v1(static_cast<int32_t>(fd)), 0);
}

void directory_descendant_after_rename(int parent) {
  // The descriptor must keep naming the renamed directory, not a path that was
  // later recreated with the same spelling.
  CHECK(lang_os_mkdirat_v1(parent, "pin", 3, 0700), 0);
  const int64_t pinned =
      lang_os_openat_v1(parent, "pin", 3,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
  CHECK_TRUE(pinned >= 0);
  if (pinned < 0) return;

  CHECK_TRUE(renameat(parent, "pin", parent, "moved") == 0);
  CHECK(lang_os_mkdirat_v1(parent, "pin", 3, 0700), 0);
  CHECK(lang_os_mkdirat_v1(static_cast<int32_t>(pinned), "inside", 6, 0700), 0);

  LangFileStat status{};
  CHECK(lang_sys_fstatat_v1(static_cast<int32_t>(pinned), "inside", 6, 1,
                            &status),
        0);
  CHECK_TRUE(S_ISDIR(status.mode));

  // The recreated "pin" is a different directory. Its descriptor must not see
  // the child created through the descriptor that outlived the rename.
  const int64_t recreated =
      lang_os_openat_v1(parent, "pin", 3,
                        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
  CHECK_TRUE(recreated >= 0);
  if (recreated >= 0) {
    CHECK(lang_sys_fstatat_v1(static_cast<int32_t>(recreated), "inside", 6, 1,
                              &status),
          -ENOENT);
    CHECK(lang_os_close_v1(static_cast<int32_t>(recreated)), 0);
  }

  CHECK(lang_sys_fstatat_v1(parent, "moved", 5, 1, &status), 0);
  CHECK_TRUE(S_ISDIR(status.mode));
  CHECK_TRUE(renameat(parent, "pin", parent, "other") == 0);
  CHECK(lang_sys_fstatat_v1(parent, "moved", 5, 1, &status), 0);
  CHECK_TRUE(S_ISDIR(status.mode));

  // Completing the file is not enough: only the renamed directory may contain
  // the new child.
  CHECK_TRUE(renameat(parent, "other", parent, "pin") == 0);
  CHECK(lang_sys_fstatat_v1(parent, "pin", 3, 1, &status), 0);
  CHECK_TRUE(S_ISDIR(status.mode));

  CHECK(lang_os_close_v1(static_cast<int32_t>(pinned)), 0);
}

void symlink_and_entry_semantics(int parent) {
  CHECK(lang_os_mkdirat_v1(parent, "real", 4, 0700), 0);
  CHECK_TRUE(symlinkat("real", parent, "link") == 0);

  LangFileStat status{};
  CHECK(lang_sys_fstatat_v1(parent, "link", 4, 1, &status), 0);
  CHECK_TRUE(S_ISLNK(status.mode));
  CHECK(lang_sys_fstatat_v1(parent, "link", 4, 0, &status), 0);
  CHECK_TRUE(S_ISDIR(status.mode));
  CHECK(lang_os_openat_v1(parent, "link", 4, O_RDONLY | O_NOFOLLOW | O_CLOEXEC,
                          0),
        -ELOOP);
  CHECK(lang_os_openat_v1(parent, "link", 4,
                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0),
        -ENOTDIR);

  create_file(parent, "target", 6);
  CHECK_TRUE(symlinkat("target", parent, "drop") == 0);
  CHECK(lang_os_unlinkat_v1(parent, "drop", 4, 0), 0);
  CHECK(lang_sys_fstatat_v1(parent, "drop", 4, 0, &status), -ENOENT);
  CHECK(lang_sys_fstatat_v1(parent, "target", 6, 1, &status), 0);
  CHECK_TRUE(S_ISREG(status.mode));

  CHECK(lang_os_mkdirat_v1(parent, "sub", 3, 0700), 0);
  CHECK(lang_os_unlinkat_v1(parent, "sub", 3, 0), -EISDIR);
  CHECK(lang_os_unlinkat_v1(parent, "sub", 3, 1), 0);
  CHECK(lang_sys_fstatat_v1(parent, "sub", 3, 0, &status), -ENOENT);
  CHECK(lang_os_unlinkat_v1(parent, "target", 6, 1), -ENOTDIR);
}

void invalid_utf8_name(int parent) {
  const char name[] = {'b', 'a', 'd', static_cast<char>(0xff)};
  const size_t length = sizeof name;
  const int64_t fd = lang_os_openat_v1(
      parent, name, length, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  CHECK_TRUE(fd >= 0);
  if (fd >= 0) {
    const char byte = 'x';
    CHECK(lang_os_write_v1(static_cast<int32_t>(fd), &byte, 1), 1);
    CHECK(lang_os_close_v1(static_cast<int32_t>(fd)), 0);
  }
  LangFileStat status{};
  CHECK(lang_sys_fstatat_v1(parent, name, length, 1, &status), 0);
  CHECK_TRUE(S_ISREG(status.mode));
  CHECK_TRUE(status.size == 1);
  CHECK(lang_os_unlinkat_v1(parent, name, length, 0), 0);
}

} // namespace

int main() {
  char pattern[] = "/tmp/woozy-fs-at-XXXXXX";
  char *root = mkdtemp(pattern);
  if (root == nullptr) {
    std::fprintf(stderr, "mkdtemp failed\n");
    return 2;
  }

  Directory parent(root);
  if (parent.fd < 0) return 2;

  path_validation(parent.fd);
  create_file(parent.fd, "alpha", 5);
  directory_descendant_after_rename(parent.fd);
  symlink_and_entry_semantics(parent.fd);
  invalid_utf8_name(parent.fd);

  // Remove the entries created by the runtime calls. Host libc cleanup is only
  // for the temporary tree and does not affect any checked result above.
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  return 0;
}
