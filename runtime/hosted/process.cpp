#include "process.hpp"

#include <cerrno>
#include <pthread.h>
#include <unistd.h>

extern "C" int64_t lang_hosted_fork_v1() noexcept {
  // Deliberately libc's fork(3), not syscall(SYS_fork)/SYS_clone.  glibc runs
  // its internal and user atfork handlers, including the hosted lock hooks
  // registered below.
  const pid_t pid = ::fork();
  return pid < 0 ? -static_cast<int64_t>(errno) : static_cast<int64_t>(pid);
}

namespace {
// Register at constructor time, before user main can register its own
// handlers. POSIX calls prepare handlers in reverse registration order and
// parent/child handlers in registration order. Our prepare uses an existing
// language launch guard or acquires launch, then environment and allocator.
// User child handlers run after our locks have been reset.
__attribute__((constructor(101), used)) void register_hosted_fork_hooks()
    noexcept {
  pthread_atfork(&lang_hosted_process_atfork_prepare_v1,
                 &lang_hosted_process_atfork_parent_v1,
                 &lang_hosted_process_atfork_child_v1);
}
} // namespace
