// Fast-path seccomp check: with no world request and no pressure, the runtime
// controller poll must not issue gettid, any lock, or any allocation.

#include "gc_runtime.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>

namespace {

bool deny_gettid() {
  const sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 186, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog program{4, const_cast<sock_filter *>(filter)};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return false;
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

} // namespace

int main() {
  if (!deny_gettid())
    return 80;
  for (int iteration = 0; iteration < 100000; ++iteration)
    lang_gc_runtime_poll_v1();
  std::puts("FAST_OK");
  return 0;
}
