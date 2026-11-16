// Focused verification for the single-registry-lock critical hooks.
//
// This translation unit includes the runtime implementation so it can read the
// private ticket-lock acquisition counter. That proves a no-park enter/leave
// takes exactly one registry lock instead of inferring it from timing. The
// remaining cases exercise the observable protocol: the collector-exempt
// leave, a pending-stop park/wake from the outermost leave, unregister after
// the wakeup, and the absence of leaked node references.

#include "gc_world.hpp"
#include "alloc.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>

#include "../../../runtime/native/gc_world.cpp"

extern "C" uint64_t gc_test_alloc_live() noexcept;

namespace {

[[noreturn]] void fail_at(const char *file, int line, const char *what) {
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
  std::fflush(stderr);
  std::_Exit(1);
}

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition))                                                          \
      fail_at(__FILE__, __LINE__, #condition);                                 \
  } while (0)

void sleep_ms(int milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

bool wait_for(const std::function<bool()> &predicate, int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    sleep_ms(1);
  }
  return predicate();
}

// g_lock_next is incremented once per registry_lock() acquisition.
uint32_t registry_acquisitions() noexcept {
  return __atomic_load_n(&g_lock_next, __ATOMIC_RELAXED);
}

// A no-park enter/leave pair is exactly one acquisition per hook. Nested hooks
// remain one each, and the depth is still visible to can_collect.
void test_single_lock_per_hook() {
  CHECK(lang_gc_world_register_v1());

  const uint32_t before_enter = registry_acquisitions();
  lang_gc_world_enter_critical_v1();
  CHECK(registry_acquisitions() - before_enter == 1);
  CHECK(!lang_gc_world_can_collect_v1());
  const uint32_t before_leave = registry_acquisitions();
  lang_gc_world_leave_critical_v1();
  CHECK(registry_acquisitions() - before_leave == 1);
  CHECK(lang_gc_world_can_collect_v1());

  const uint32_t before_nested = registry_acquisitions();
  lang_gc_world_enter_critical_v1();
  lang_gc_world_enter_critical_v1();
  CHECK(registry_acquisitions() - before_nested == 2);
  CHECK(!lang_gc_world_can_collect_v1());
  const uint32_t before_nested_leaves = registry_acquisitions();
  lang_gc_world_leave_critical_v1();
  lang_gc_world_leave_critical_v1();
  CHECK(registry_acquisitions() - before_nested_leaves == 2);
  CHECK(lang_gc_world_can_collect_v1());

  CHECK(lang_gc_world_unregister_v1());
  CHECK(gc_test_alloc_live() == 0);
}

// The active collector is exempt from its own request: enter/leave stay
// ACTIVE and the leave must not park even though the request word is set.
void test_collector_critical_leave_does_not_park() {
  CHECK(lang_gc_world_register_v1());
  CHECK(lang_gc_world_stop_v1(0) == 0);
  CHECK(lang_gc_world_poll_required_v1());

  const uint32_t before = registry_acquisitions();
  lang_gc_world_enter_critical_v1();
  CHECK(registry_acquisitions() - before == 1);
  lang_gc_world_leave_critical_v1();
  CHECK(registry_acquisitions() - before == 2);

  // Returning from the leave proves it did not wait for a resume it must not
  // consume. The stop is still owned and can only be released by resume.
  CHECK(lang_gc_world_poll_required_v1());
  lang_gc_world_resume_v1();
  CHECK(!lang_gc_world_poll_required_v1());
  CHECK(lang_gc_world_unregister_v1());
  CHECK(gc_test_alloc_live() == 0);
}

// The outermost leave publishes PARKED, notifies outside the lock, and holds
// its temporary node reference until resume wakes the futex wait. The worker
// then unregisters, which can free the node only if that reference was
// released after the wait.
void test_pending_stop_parks_in_leave_and_wakes() {
  CHECK(lang_gc_world_register_v1()); // collector

  std::atomic<bool> in_critical{false};
  std::atomic<bool> leaving{false};
  std::atomic<bool> left{false};

  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_critical_v1();
    in_critical.store(true, std::memory_order_release);
    while (!lang_gc_world_poll_required_v1())
      std::this_thread::yield();
    leaving.store(true, std::memory_order_release);
    lang_gc_world_leave_critical_v1(); // parks here until resume
    left.store(true, std::memory_order_release);
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(wait_for([&] { return in_critical.load(std::memory_order_acquire); },
                 5000));
  CHECK(lang_gc_world_stop_v1(5000000000LL) == 0);
  CHECK(wait_for([&] { return leaving.load(std::memory_order_acquire); }, 5000));
  sleep_ms(50);
  CHECK(!left.load(std::memory_order_acquire)); // still inside the leave wait

  lang_gc_world_resume_v1();
  CHECK(wait_for([&] { return left.load(std::memory_order_acquire); }, 5000));
  worker.join();
  CHECK(lang_gc_world_unregister_v1());
  CHECK(gc_test_alloc_live() == 0);
}

// Many no-park pairs must not accumulate temporary node references.
void test_repeated_pairs_do_not_leak_references() {
  CHECK(lang_gc_world_register_v1());
  CHECK(gc_test_alloc_live() == 1);
  for (int iteration = 0; iteration < 100000; ++iteration) {
    lang_gc_world_enter_critical_v1();
    lang_gc_world_leave_critical_v1();
  }
  CHECK(gc_test_alloc_live() == 1);
  CHECK(lang_gc_world_unregister_v1());
  CHECK(gc_test_alloc_live() == 0);
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  test_single_lock_per_hook();
  test_collector_critical_leave_does_not_park();
  test_pending_stop_parks_in_leave_and_wakes();
  test_repeated_pairs_do_not_leak_references();

  std::printf("GC_WORLD_CRITICAL_SINGLE_LOCK_OK\n");
  return 0;
}
