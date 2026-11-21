#include "../../runtime/darwin/posix.hpp"
#include "../../runtime/native/os.hpp"
#include "../../runtime/native/system.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr int32_t kCreate = 64;
constexpr int32_t kExclusive = 128;
constexpr int32_t kNonblock = 2048;
constexpr int32_t kDirectory = 65536;
constexpr int32_t kNoFollow = 131072;
constexpr int32_t kCloexec = 524288;

void test_directory(uint64_t inode) {
  std::set<std::string> created{std::string(250, 'x')};
  for (int i = 0; i < 90; ++i) created.insert("entry-" + std::to_string(i));
  for (const auto &name : created) {
    const auto fd = lang_os_open_v1(name.data(), name.size(), 1 | kCreate | kExclusive, 0600);
    assert(fd >= 0 && lang_os_close_v1(static_cast<int32_t>(fd)) == 0);
  }
  auto expected = created;
  expected.insert("sample");
  expected.insert("symlink");
  const auto fd = static_cast<int32_t>(lang_os_open_v1(".", 1, kDirectory | kNoFollow, 0));
  assert(fd >= 0);
  std::array<char, 512> records;
  assert(lang_sys_getdents_v1(fd, records.data(), records.size()) > 0);
  assert(lang_os_seek_v1(fd, 0, SEEK_SET) == 0);
  int batches = 0;
  for (;;) {
    const int64_t count = lang_sys_getdents_v1(fd, records.data(), records.size());
    assert(count >= 0 && count <= static_cast<int64_t>(records.size()));
    if (count == 0) break;
    ++batches;
    for (int offset = 0; offset < count;) {
      assert(count - offset >= 20);
      uint16_t length;
      std::memcpy(&length, records.data() + offset + 16, sizeof(length));
      assert(length >= 20 && length <= count - offset);
      const char *const name = records.data() + offset + 19;
      assert(std::memchr(name, '\0', length - 19));
      if (std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0)
        assert(expected.erase(name) == 1);
      if (std::strcmp(name, "sample") == 0) {
        uint64_t file_id = 0;
        std::memcpy(&file_id, records.data() + offset, sizeof(file_id));
        assert(file_id == inode && records[offset + 18] == DT_REG);
      }
      if (std::strcmp(name, "symlink") == 0) assert(records[offset + 18] == DT_LNK);
      offset += length;
    }
  }
  assert(expected.empty() && batches > 1);
  assert(lang_sys_getdents_v1(fd, nullptr, 0) == 0);
  assert(lang_sys_getdents_v1(fd, nullptr, 1) == -14);
  assert(lang_os_close_v1(fd) == 0);
  for (const auto &name : created) assert(lang_os_unlink_v1(name.data(), name.size()) == 0);
}

void test_pipe() {
  int32_t fds[2]{-1, -1};
  assert(lang_sys_pipe_v1(fds, 0x80000000u) == -22);
  assert(fds[0] == -1 && fds[1] == -1);
  assert(lang_sys_pipe_v1(fds, kCloexec | kNonblock) == 0);
  for (const int fd : fds) {
    const int descriptor = fcntl(fd, F_GETFD);
    const int mode = fcntl(fd, F_GETFL);
    assert(descriptor >= 0 && (descriptor & FD_CLOEXEC));
    assert(mode >= 0 && (mode & O_NONBLOCK));
  }
  char byte = 'x';
  assert(lang_os_read_v1(fds[0], &byte, 1) == -11);
  assert(lang_os_write_v1(fds[1], &byte, 1) == 1);
  byte = 0;
  assert(lang_os_read_v1(fds[0], &byte, 1) == 1 && byte == 'x');
  assert(lang_os_close_v1(fds[0]) == 0 && lang_os_close_v1(fds[1]) == 0);
}

} // namespace

int main() {
  const int current = open(".", O_RDONLY | O_DIRECTORY);
  char directory[] = "/tmp/woozy-darwin-files.XXXXXXXX";
  assert(current >= 0 && mkdtemp(directory));
  assert(lang_sys_chdir_v1(directory, std::strlen(directory)) == 0);
  char where[4096];
  assert(lang_sys_getcwd_v1(where, sizeof(where)) > 0);
  assert(std::strstr(where, "/woozy-darwin-files."));
  assert(lang_os_open_v1("missing", 7, 0, 0) == -2);
  assert(lang_os_open_v1("x\0y", 3, 0, 0) == -22);
  assert(lang_os_open_v1(nullptr, 2, 0, 0) == -14);
  assert(lang_os_open_v1("x", 4096, 0, 0) == -36);
  assert(lang_os_read_v1(-1, nullptr, 0) == 0);
  assert(lang_os_write_v1(-1, nullptr, 0) == 0);

  const auto fd = static_cast<int32_t>(lang_os_open_v1("sample", 6, 2 | kCreate | kExclusive | kCloexec, 0600));
  assert(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
  assert(lang_os_write_v1(fd, "data", 4) == 4);
  assert(lang_os_seek_v1(fd, 0, SEEK_SET) == 0);
  char bytes[4]{};
  assert(lang_os_read_v1(fd, bytes, sizeof(bytes)) == 4);
  assert(std::memcmp(bytes, "data", sizeof(bytes)) == 0);
  LangFileStat info{};
  assert(lang_sys_fstat_v1(-1, &info) == -9 && info.inode == 0);
  assert(lang_sys_fstat_v1(fd, &info) == 0 && info.size == 4);
  assert(lang_sys_flock_v1(fd, 2 | 4) == 0 && lang_sys_flock_v1(fd, 8) == 0);
  assert(lang_os_close_v1(fd) == 0);
  assert(lang_sys_symlink_v1("sample", 6, "symlink", 7) == 0);
  assert(lang_os_open_v1("symlink", 7, kNoFollow, 0) == -40);
  char path[12]{};
  assert(lang_sys_readlink_v1("symlink", 7, path, sizeof(path)) == 6);
  assert(std::strcmp(path, "sample") == 0);
  LangFileStat target{};
  assert(lang_sys_lstat_v1("symlink", 7, &target) == 0 && S_ISLNK(target.mode));
  assert(lang_sys_stat_v1("symlink", 7, &target) == 0 && target.inode == info.inode);
  test_directory(info.inode);

  test_pipe();
  pid_t child;
  {
    lang::native::darwin::LaunchGuard guard;
    child = fork();
  }
  assert(child >= 0);
  if (child == 0) {
    test_pipe();
    lang_os_exit_v1(42);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 42);
  assert(lang_sys_kill_v1(getpid(), 0) == 0 && lang_sys_kill_v1(getpid(), 16) == -22);
  assert(lang_sys_gettid_v1() > 0);
  LangTimespec time{};
  assert(lang_sys_clock_gettime_v1(1, &time) == 0 && time.seconds > 0);
  const LangTimespec bad_sleep{0, 1000000000};
  assert(lang_sys_nanosleep_v1(&bad_sleep, &time) == -22);
  std::array<char, 512> random{};
  assert(lang_sys_getrandom_v1(random.data(), random.size(), 0) == 256);
  assert(lang_sys_getrandom_v1(random.data(), random.size(), 1) == -95);

  assert(lang_os_unlink_v1("symlink", 7) == 0 && lang_os_unlink_v1("sample", 6) == 0);
  assert(fchdir(current) == 0 && close(current) == 0);
  assert(lang_os_rmdir_v1(directory, std::strlen(directory)) == 0);
  std::puts("Darwin OS runtime passed");
}
