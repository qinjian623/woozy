#include "../../../runtime/native/gc_runtime.hpp"
#include "../../../runtime/native/thread.hpp"

#include <atomic>
#include <cstdio>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace {
void *(*entry)(void *);
void *argument;
std::atomic<bool> runner_returned{false};
unsigned joins, conds, mutexes;

void *run(void *) {
  void *result = entry(argument);
  runner_returned.store(true, std::memory_order_release);
  return result;
}
void outside_kernel() {
  LangGcRuntimeStats stats{};
  lang_gc_runtime_stats_v1(&stats);
  if (stats.busy || stats.phase == LANG_GC_RUNTIME_PHASE_KERNEL) {
    const char text[] = "THREAD_CLEANUP_IN_KERNEL\n";
    write(2, text, sizeof(text) - 1);
    _exit(91);
  }
}
__attribute__((destructor)) void report() {
  char text[80];
  const int size = std::snprintf(text, sizeof(text),
      "THREAD_CLEANUP:%u:%u:%u\n", joins, conds, mutexes);
  write(2, text, static_cast<size_t>(size));
}
} // namespace

extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                                     void *(*)(void *), void *);
extern "C" int __wrap_pthread_create(pthread_t *thread,
    const pthread_attr_t *attributes, void *(*start)(void *), void *context) {
  entry = start;
  argument = context;
  return __real_pthread_create(thread, attributes, run, nullptr);
}
extern "C" int64_t __real_lang_thread_result_v1(LangHandle *) noexcept;
extern "C" int64_t __wrap_lang_thread_result_v1(LangHandle *handle) noexcept {
  const int64_t value = __real_lang_thread_result_v1(handle);
  // One runner only: ensure its own ARC reference is gone before making the
  // closure cycle, so its final Thread reference is deterministically GC-owned.
  while (!runner_returned.load(std::memory_order_acquire)) sched_yield();
  return value;
}
extern "C" int __real_pthread_join(pthread_t, void **);
extern "C" int __wrap_pthread_join(pthread_t thread, void **value) {
  outside_kernel();
  ++joins;
  return __real_pthread_join(thread, value);
}
extern "C" int __real_pthread_cond_destroy(pthread_cond_t *);
extern "C" int __wrap_pthread_cond_destroy(pthread_cond_t *cond) {
  outside_kernel();
  ++conds;
  return __real_pthread_cond_destroy(cond);
}
extern "C" int __real_pthread_mutex_destroy(pthread_mutex_t *);
extern "C" int __wrap_pthread_mutex_destroy(pthread_mutex_t *mutex) {
  outside_kernel();
  ++mutexes;
  return __real_pthread_mutex_destroy(mutex);
}
