// Host-only fork-failure hook test.
//
// A seccomp filter denies fork/vfork/clone/clone3, so lang_process_fork_v1 must
// unwind its parent-side allocator and Weak hooks. The following Weak calls
// would deadlock or fail if the Weak lock were still held.

#include "process.hpp"
#include "vec.hpp"
#include "weak.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" uint64_t lang_weak_objects_allocations() noexcept;
extern "C" uint64_t lang_weak_objects_frees() noexcept;
extern "C" uint64_t lang_weak_objects_live_allocations() noexcept;
extern "C" uint64_t lang_weak_objects_bad_frees() noexcept;
extern "C" uint64_t lang_weak_objects_control_allocations() noexcept;
extern "C" void lang_weak_objects_reset_stats() noexcept;

namespace {

#if !defined(__NR_clone3)
constexpr long kClone3 = 435;
#else
constexpr long kClone3 = __NR_clone3;
#endif
#if !defined(__NR_fork)
constexpr long kFork = 57;
#else
constexpr long kFork = __NR_fork;
#endif
#if !defined(__NR_vfork)
constexpr long kVfork = 58;
#else
constexpr long kVfork = __NR_vfork;
#endif
constexpr long kClone = __NR_clone;

void deny_fork_syscalls() noexcept {
  sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(kFork), 0, 1),
      BPF_STMT(BPF_RET | BPF_K,
               SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(kVfork), 0,
               1),
      BPF_STMT(BPF_RET | BPF_K,
               SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(kClone), 0, 1),
      BPF_STMT(BPF_RET | BPF_K,
               SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<uint32_t>(kClone3), 0,
               1),
      BPF_STMT(BPF_RET | BPF_K,
               SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog program{};
  program.len = static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0]));
  program.filter = filter;
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
      prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
    std::perror("weak_objects_fork_failure: seccomp");
    _exit(2);
  }
}

} // namespace

int main() {
  lang_weak_objects_reset_stats();
  LangVec *vec = lang_vec_new_v1(1, sizeof(uint64_t), nullptr, nullptr);
  if (!vec) return 10;
  uint64_t value = 0xfeedfaceULL;
  lang_vec_init_v1(vec, &value);
  LangWeakControl *weak =
      lang_weak_downgrade_v1(vec, &vec->weak_control);
  if (!weak) return 11;

  deny_fork_syscalls();
  const int64_t pid = lang_process_fork_v1();
  if (pid == 0) _exit(12); // must not have forked
  if (pid > 0) {
    int32_t status = 0;
    lang_process_waitpid_v1(static_cast<int32_t>(pid), 0, &status);
    std::fprintf(stderr,
                 "weak_objects_fork_failure: fork unexpectedly succeeded\n");
    return 13;
  }

  // A released parent Weak lock is required for these calls to complete.
  if (!lang_weak_alive_v1(weak)) return 14;
  void *upgraded = lang_weak_upgrade_v1(weak);
  if (upgraded != vec) return 15;
  lang_vec_release_v1(static_cast<LangVec *>(upgraded));
  lang_vec_release_v1(vec);
  if (lang_weak_alive_v1(weak)) return 16;
  if (lang_weak_upgrade_v1(weak) != nullptr) return 17;
  lang_weak_release_v1(weak);

  if (lang_weak_objects_allocations() != lang_weak_objects_frees()) return 18;
  if (lang_weak_objects_live_allocations() != 0) return 19;
  if (lang_weak_objects_bad_frees() != 0) return 20;
  if (!lang_weak_objects_control_allocations()) return 21;

  std::fprintf(stderr, "WEAK_OBJECTS_FORK_FAIL_PASS\n");
  return 0;
}
