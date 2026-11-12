// Literal reproduction of the two independent ASan counterexamples from the
// external async/GC review (runtime_lifetime_asan.cpp, asan-registry.log and
// asan-cancel-waiter.log), pointed at this working tree's executor.
//
//   ./review_counterexamples           # registry: new -> release -> shutdown
//   ./review_counterexamples second    # cancel a suspended waiter, then let
//                                      # the child complete
//
// Both modes are expected to exit 0. Before the lifetime fix the first mode
// reported a heap-use-after-free in lang_async_shutdown_v1 (the released
// future stayed in the registry) and the second reported one in complete()
// (the cancelled and released waiter was still in the child's waiter list).
//
// This file is intentionally independent of the hosted test harness: it only
// needs an allocator, a free and the panic entry point.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" void *lang_alloc_v1(uint64_t n, uint64_t) noexcept {
  return std::malloc(n);
}
extern "C" void lang_free_v1(void *p) noexcept { std::free(p); }
extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t, uint64_t,
                                                  uint64_t) noexcept {
  std::abort();
}

#include "../../runtime/native/async.cpp"

int main(int argc, char **argv) {
  if (argc == 1) {
    void *task = lang_async_task_new_v1();
    lang_async_task_release_v1(task);
    lang_async_shutdown_v1();
    if (g_all != nullptr) {
      std::fprintf(stderr, "registry residue after release\n");
      return 1;
    }
    std::printf("registry counterexample: no use-after-free\n");
    return 0;
  }
  auto *task = static_cast<Future *>(lang_async_task_new_v1());
  auto *timer = static_cast<Future *>(lang_async_sleep_v1(100));
  g_current = task;
  (void)lang_async_await_v1(timer);
  g_current = nullptr;
  task->flags |= kStarted | kSuspended;
  lang_async_cancel_v1(task);
  complete(task, kCancelled);
  lang_async_task_release_v1(task);
  complete(timer, kDone);
  if (timer->waiters != nullptr) {
    std::fprintf(stderr, "released waiter left in the target list\n");
    return 1;
  }
  lang_async_task_release_v1(timer);
  lang_async_shutdown_v1();
  if (g_all != nullptr) {
    std::fprintf(stderr, "registry residue after cancel\n");
    return 1;
  }
  std::printf("cancel-waiter counterexample: no use-after-free\n");
  return 0;
}
