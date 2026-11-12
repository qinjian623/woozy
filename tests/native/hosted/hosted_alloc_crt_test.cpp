// Only the allocator is strongly referenced by this fixture. Weak hook
// addresses must not cause process/fork archive members to be extracted.
#include "../../../runtime/native/alloc.hpp"

#include <cerrno>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

extern "C" void lang_hosted_process_atfork_prepare_v1() noexcept
    __attribute__((weak));
extern "C" void lang_hosted_process_atfork_child_v1() noexcept
    __attribute__((weak));

namespace {
bool allocate_and_free() {
  auto *memory = static_cast<unsigned char *>(lang_alloc_v1(256, 16));
  if (!memory) return false;
  memory[0] = 19;
  memory[255] = 71;
  const bool ok = memory[0] == 19 && memory[255] == 71;
  lang_free_v1(memory);
  return ok;
}
} // namespace

int main() {
  if (!lang_hosted_process_atfork_prepare_v1 ||
      !lang_hosted_process_atfork_child_v1) {
    std::fputs("hosted_alloc_crt_test: missing hosted atfork hook\n", stderr);
    return 1;
  }
  if (!allocate_and_free()) return 2;
  const pid_t child = fork();
  if (child < 0) return 3;
  if (!child) {
    alarm(5);
    _exit(allocate_and_free() ? 0 : 4);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0)
    if (errno != EINTR) return 5;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !allocate_and_free())
    return 6;
  std::puts("passed: allocator-only CRT extracted atfork hooks; child allocation works");
  return 0;
}
