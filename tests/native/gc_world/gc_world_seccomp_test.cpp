// Seccomp fault-injection checks for lang-gc-world/1.
//
// Every mode is run as its own process by run_all.sh under `timeout`. The
// filters are installed on the calling thread only (no TSYNC), so the worker
// can reach its futex wait before the main thread loses FUTEX_WAKE.

#include "gc_world.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace {

void sleep_ms(int milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

bool install_filter(const sock_filter *filter, unsigned short count) {
  sock_fprog program{count, const_cast<sock_filter *>(filter)};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return false;
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

bool deny_gettid() {
  const sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 186, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  return install_filter(filter, 4);
}

bool deny_futex_wake() {
  const sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 202, 0, 3),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[1])),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 129, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  return install_filter(filter, 6);
}

int fast_path_mode() {
  if (!deny_gettid())
    return 80;
  for (int iteration = 0; iteration < 100000; ++iteration) {
    if (lang_gc_world_request_pending_v1() != 0)
      return 83;
    if (lang_gc_world_poll_required_v1() != 0)
      return 83;
    // No request: this must return without gettid or futex.
    lang_gc_world_poll_v1();
  }
  std::puts("FAST_OK");
  return 0;
}

int gettid_mode() {
  if (!deny_gettid())
    return 80;
  // Must Panic with gc-world:gettid-failed:0, not SIGILL.
  (void)lang_gc_world_register_v1();
  return 84;
}

int wake_mode() {
  if (!lang_gc_world_register_v1())
    return 85;
  std::atomic<bool> ready{false};
  std::atomic<bool> leave{false};
  std::atomic<bool> left{false};

  std::thread worker([&] {
    if (!lang_gc_world_register_v1())
      std::_Exit(85);
    ready.store(true, std::memory_order_release);
    while (!leave.load(std::memory_order_acquire))
      lang_gc_world_poll_v1();
    if (!lang_gc_world_unregister_v1())
      std::_Exit(85);
    left.store(true, std::memory_order_release);
  });

  while (!ready.load(std::memory_order_acquire))
    sleep_ms(1);
  if (lang_gc_world_stop_v1(1000000000LL) != 0)
    return 85;
  // Ensure the worker has reached its futex wait before FUTEX_WAKE is denied.
  sleep_ms(100);
  if (!deny_futex_wake())
    return 81;
  leave.store(true, std::memory_order_release);
  std::puts("STOP_OK");
  std::fflush(stdout);

  // resume must not return when it cannot wake the parked worker.
  lang_gc_world_resume_v1();
  std::puts("RESUME_RETURNED");
  std::fflush(stdout);
  for (int attempt = 0; attempt < 100 && !left.load(); ++attempt)
    sleep_ms(1);
  if (!left.load())
    std::_Exit(86);
  worker.join();
  (void)lang_gc_world_unregister_v1();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 82;
  if (std::strcmp(argv[1], "fast") == 0)
    return fast_path_mode();
  if (std::strcmp(argv[1], "gettid") == 0)
    return gettid_mode();
  if (std::strcmp(argv[1], "wake") == 0)
    return wake_mode();
  return 82;
}
