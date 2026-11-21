#include "../../runtime/native/process.hpp"
#include "../../runtime/native/gc_program.hpp"
#include "../../runtime/native/thread.hpp"
#include <cassert>
#include <cstdio>
#include <signal.h>
#include <time.h>
#include <unistd.h>
constexpr uint64_t kUser = uint64_t{1} << 9;  // Woozy uses Linux SIGUSR1 number.
static void check_info(const LangSignalInfo &info) {
  assert(info.number == 10);
  assert(!info.code && !info.pid && !info.uid && !info.status && !info.value);
}
static int64_t ordinary_waiter(void *argument) noexcept {
  auto *ready = *static_cast<uint32_t **>(argument);
  __atomic_store_n(ready, 1u, __ATOMIC_RELEASE);
  LangSignalInfo info;
  if (lang_signal_wait_v1(&info, kUser, -1) != 0) return 1;
  check_info(info);
  return 0;
}
int main() {
  assert(lang_gc_program_init_v1() == 0);
  uint64_t old, previous, pending;
  LangSignalInfo info;
  assert(lang_signal_wait_v1(&info, kUser, 0) == -22);
  assert(lang_signal_mask_v1(&old, 0, kUser) == 0);
  assert(lang_signal_mask_v1(&previous, 1, uint64_t{1} << 63) == -95);
  assert(lang_signal_wait_v1(&info, uint64_t{1} << 17, 0) == -95);
  assert(kill(getpid(), SIGUSR1) == 0);
  assert(lang_signal_pending_v1(&pending) == 0 && (pending & kUser) != 0);
  assert(lang_signal_wait_v1(&info, kUser, 0) == 0);
  check_info(info);
  assert(lang_signal_pending_v1(&pending) == 0 && (pending & kUser) == 0);
  assert(lang_signal_wait_v1(&info, kUser, 0) == -11);
  const auto start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  assert(lang_signal_wait_v1(&info, kUser, 2000000) == -11);
  assert(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start >= 2000000);
  assert(lang_signal_wait_v1(&info, kUser, -2) == -22);

  auto parent = getpid();
  auto child = lang_process_fork_v1();
  assert(child >= 0);
  if (!child) {
    usleep(20000);
    _exit(kill(parent, SIGUSR1) == 0 ? 0 : 90);
  }
  assert(lang_signal_wait_v1(&info, kUser, 1000000000) == 0);
  check_info(info);
  int32_t status;
  assert(lang_process_waitpid_v1(child, 0, &status) == child && status == 0);

  uint32_t ready = 0;
  auto *ready_pointer = &ready;
  auto *worker = lang_thread_spawn_v1(ordinary_waiter, &ready_pointer, sizeof(ready_pointer), nullptr, nullptr);
  assert(lang_thread_error_v1(worker) == 0);
  while (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) usleep(1000);
  usleep(20000);
  child = lang_process_fork_v1();
  assert(child >= 0);
  if (!child) {
    if (kill(getpid(), SIGUSR1) != 0 || lang_signal_wait_v1(&info, kUser, 10000000) != 0) _exit(91);
    check_info(info);
    _exit(0);
  }
  assert(lang_process_waitpid_v1(child, 0, &status) == child && status == 0);
  assert(kill(getpid(), SIGUSR1) == 0);
  assert(lang_thread_wait_v1(worker) == 0 && lang_thread_result_v1(worker) == 0);
  lang_handle_release_v1(worker);
  assert(lang_signal_mask_v1(&previous, 2, old) == 0 && (previous & kUser) != 0);
  assert(lang_signal_disposition_v1(12, 1) == 0);
  assert(kill(getpid(), SIGUSR2) == 0);
  assert(lang_signal_disposition_v1(12, 0) == 0);
  assert(lang_signal_disposition_v1(9, 1) == -22);
  assert(lang_gc_program_finish_v1() == 0);
  std::puts("Darwin signals: mask, pending, timed and blocking wait, fork and disposition passed");
}
