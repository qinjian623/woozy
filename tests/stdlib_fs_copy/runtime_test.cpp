// Direct host-side checks for the two descriptor-relative symbolic-link
// operations added by the tree-copy work. Host libc is used only to create and
// inspect the temporary fixture; the checked functions are the freestanding
// runtime entry points under test.
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

void check(const char *what, long long actual, long long expected, int line) {
  if (actual == expected) return;
  std::fprintf(stderr, "FAIL line %d: %s: actual %lld expected %lld\n", line,
               what, actual, expected);
  ++failures;
}
#define CHECK(actual, expected) check(#actual, (long long)(actual), (expected), __LINE__)

struct Directory {
  int fd = -1;
  explicit Directory(const char *path) : fd(open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {}
  ~Directory() { if (fd >= 0) close(fd); }
  Directory(const Directory &) = delete;
  Directory &operator=(const Directory &) = delete;
};

void invalid_components(int parent) {
  const char empty[] = "";
  const char dot[] = ".";
  const char dotdot[] = "..";
  const char slash[] = "a/b";
  const char nul[] = {'a', '\0', 'b'};
  char long_name[4096];
  std::memset(long_name, 'x', sizeof long_name);

  char buffer[16] = {};
  CHECK(lang_sys_readlinkat_v1(parent, empty, 0, buffer, sizeof buffer), -EINVAL);
  CHECK(lang_sys_readlinkat_v1(parent, dot, 1, buffer, sizeof buffer), -EINVAL);
  CHECK(lang_sys_readlinkat_v1(parent, dotdot, 2, buffer, sizeof buffer), -EINVAL);
  CHECK(lang_sys_readlinkat_v1(parent, slash, sizeof slash - 1, buffer, sizeof buffer), -EINVAL);
  CHECK(lang_sys_readlinkat_v1(parent, nul, sizeof nul, buffer, sizeof buffer), -EINVAL);
  CHECK(lang_sys_readlinkat_v1(parent, long_name, sizeof long_name, buffer, sizeof buffer), -ENAMETOOLONG);
  CHECK(lang_sys_readlinkat_v1(parent, nullptr, 1, buffer, sizeof buffer), -EFAULT);
  CHECK(lang_sys_symlinkat_v1(parent, "t", 1, empty, 0), -EINVAL);
  CHECK(lang_sys_symlinkat_v1(parent, "t", 1, slash, sizeof slash - 1), -EINVAL);
  CHECK(lang_sys_symlinkat_v1(parent, "t", 1, nullptr, 1), -EFAULT);
  CHECK(lang_sys_symlinkat_v1(parent, "t", 1, long_name, sizeof long_name), -ENAMETOOLONG);
  const char nul_target[] = {'t', '\0'};
  CHECK(lang_sys_symlinkat_v1(parent, nul_target, sizeof nul_target, "n", 1), -EINVAL);
}

void valid_symlinks(int parent) {
  const char raw_target[] = {'r', 'a', 'w', static_cast<char>(0xff)};
  const char raw_name[] = {'l', 'i', 'n', 'k', static_cast<char>(0xfe)};
  const size_t target_length = sizeof raw_target;
  const size_t name_length = sizeof raw_name;

  CHECK(lang_sys_symlinkat_v1(parent, raw_target, target_length, raw_name, name_length), 0);

  LangFileStat status{};
  CHECK(lang_sys_fstatat_v1(parent, raw_name, name_length, 1, &status), 0);
  CHECK(S_ISLNK(status.mode) ? 1 : 0, 1);

  char buffer[64] = {};
  CHECK(lang_sys_readlinkat_v1(parent, raw_name, name_length, buffer, sizeof buffer),
        static_cast<long long>(target_length));
  CHECK(std::memcmp(buffer, raw_target, target_length) == 0 ? 1 : 0, 1);

  // A zero-capacity request is a no-op after name validation, even with a null
  // buffer; a non-zero buffer request with a null pointer is EFAULT.
  CHECK(lang_sys_readlinkat_v1(parent, raw_name, name_length, nullptr, 0), 0);
  CHECK(lang_sys_readlinkat_v1(parent, raw_name, name_length, nullptr, 4), -EFAULT);
  // A short buffer reports the truncated byte count, never a NUL terminator.
  std::memset(buffer, 0, sizeof buffer);
  CHECK(lang_sys_readlinkat_v1(parent, raw_name, name_length, buffer, 2), 2);
  CHECK(std::memcmp(buffer, raw_target, 2) == 0 ? 1 : 0, 1);
  CHECK(buffer[2] == 0 ? 1 : 0, 1);

  // Creating the same raw name again is EEXIST; the original link remains.
  CHECK(lang_sys_symlinkat_v1(parent, "other", 5, raw_name, name_length), -EEXIST);
  CHECK(lang_sys_readlinkat_v1(parent, raw_name, name_length, buffer, sizeof buffer),
        static_cast<long long>(target_length));

  // A dangling target is stored verbatim and never resolved.
  const char dangling[] = "missing-target";
  CHECK(lang_sys_symlinkat_v1(parent, dangling, sizeof dangling - 1, "dangling", 8), 0);
  CHECK(lang_sys_readlinkat_v1(parent, "dangling", 8, buffer, sizeof buffer),
        static_cast<long long>(sizeof dangling - 1));
  CHECK(std::memcmp(buffer, dangling, sizeof dangling - 1) == 0 ? 1 : 0, 1);

  // readlinkat rejects non-links and missing entries; it never follows a link.
  int file = openat(parent, "plain", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  CHECK(file >= 0 ? 0 : -1, 0);
  if (file >= 0) close(file);
  CHECK(lang_sys_readlinkat_v1(parent, "plain", 5, buffer, sizeof buffer), -EINVAL);
  CHECK(lang_sys_readlinkat_v1(parent, "missing", 7, buffer, sizeof buffer), -ENOENT);

  // A long target is rejected by the borrowed-target path bound.
  char long_target[4096];
  std::memset(long_target, 'y', sizeof long_target);
  CHECK(lang_sys_symlinkat_v1(parent, long_target, sizeof long_target, "long", 4), -ENAMETOOLONG);
}

} // namespace

int main() {
  char pattern[] = "/tmp/woozy-fs-linkat-XXXXXX";
  char *root = mkdtemp(pattern);
  if (!root) {
    std::fprintf(stderr, "mkdtemp failed\n");
    return 2;
  }
  {
    Directory parent(root);
    if (parent.fd < 0) return 2;
    invalid_components(parent.fd);
    valid_symlinks(parent.fd);
  }
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  return 0;
}
