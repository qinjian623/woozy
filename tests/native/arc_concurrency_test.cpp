// Independent host-side concurrency regression for the native Vec ARC runtime.
//
// The runtime sources are compiled with the freestanding flags used by
// lang_native_runtime; only this harness uses the host C++ library and
// std::thread. To isolate reference counters from allocator behavior, every test creates
// its Vec before starting worker threads and performs the final release only
// after all workers have joined. Worker threads touch only the atomic ref and
// iteration counters and never allocate.
//
// This harness installs a thread-safe observer. It does not install a racy
// hook: concurrent observer callbacks are allowed here, but all recorded state
// is atomic. The weak observer is still present, so the runtime slow paths are
// exercised exactly as with an external observer.

#include "../../runtime/native/vec.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static_assert(sizeof(LangVec) == 80, "LangVec capacity ABI must stay 80 bytes");
static_assert(offsetof(LangVec, refs) == 0, "refs must stay first");
static_assert(offsetof(LangVec, iterations) == 32,
              "iterations offset changed");

namespace {

constexpr uint64_t kMaxU64 = ~uint64_t{0};
constexpr unsigned kMaxTrackedValues = 32;

std::atomic<bool> retain_on_release{false};
std::atomic<uint64_t> observed_create{0};
std::atomic<uint64_t> observed_retain{0};
std::atomic<uint64_t> observed_release{0};
std::atomic<uint64_t> observed_element_drop{0};
std::atomic<uint64_t> observed_final_free{0};
std::atomic<uint64_t> observed_begin{0};
std::atomic<uint64_t> observed_end{0};
std::atomic<unsigned> observed_index_seen[kMaxTrackedValues] = {};
std::atomic<unsigned> observed_bad_index{0};

std::atomic<uint64_t> dropped_total{0};
std::atomic<unsigned> dropped_bad_value{0};
std::atomic<unsigned> dropped_value_count[kMaxTrackedValues] = {};
std::atomic<unsigned> drop_sequence{0};
std::atomic<int> drop_order[kMaxTrackedValues] = {};

void reset_observer_and_drops() {
  observed_create.store(0, std::memory_order_relaxed);
  observed_retain.store(0, std::memory_order_relaxed);
  observed_release.store(0, std::memory_order_relaxed);
  observed_element_drop.store(0, std::memory_order_relaxed);
  observed_final_free.store(0, std::memory_order_relaxed);
  observed_begin.store(0, std::memory_order_relaxed);
  observed_end.store(0, std::memory_order_relaxed);
  observed_bad_index.store(0, std::memory_order_relaxed);
  for (auto &slot : observed_index_seen)
    slot.store(0, std::memory_order_relaxed);
  dropped_total.store(0, std::memory_order_relaxed);
  dropped_bad_value.store(0, std::memory_order_relaxed);
  for (auto &slot : dropped_value_count)
    slot.store(0, std::memory_order_relaxed);
  drop_sequence.store(0, std::memory_order_relaxed);
  for (auto &slot : drop_order)
    slot.store(0, std::memory_order_relaxed);
}

void drop_int(void *pointer) {
  const int value = *static_cast<int *>(pointer);
  if (value >= 0 && static_cast<unsigned>(value) < kMaxTrackedValues)
    dropped_value_count[value].fetch_add(1, std::memory_order_relaxed);
  else
    dropped_bad_value.fetch_add(1, std::memory_order_relaxed);
  const unsigned slot = drop_sequence.fetch_add(1, std::memory_order_relaxed);
  if (slot < kMaxTrackedValues)
    drop_order[slot].store(value, std::memory_order_relaxed);
  dropped_total.fetch_add(1, std::memory_order_relaxed);
}

[[noreturn]] void panic_retain_overflow(LangVec *v) {
  v->refs = kMaxU64;
  lang_vec_retain_v1(v);
  _exit(0);
}

[[noreturn]] void panic_release_underflow(LangVec *v) {
  v->refs = 0;
  lang_vec_release_v1(v);
  _exit(0);
}

[[noreturn]] void panic_final_release_active_iteration(LangVec *v) {
  v->refs = 1;
  v->iterations = 1;
  lang_vec_release_v1(v);
  _exit(0);
}

[[noreturn]] void panic_begin_overflow(LangVec *v) {
  v->iterations = kMaxU64;
  lang_vec_begin_v1(v);
  _exit(0);
}

[[noreturn]] void panic_end_underflow(LangVec *v) {
  v->iterations = 0;
  lang_vec_end_v1(v);
  _exit(0);
}

[[noreturn]] void panic_check_resize_active(LangVec *v) {
  v->iterations = 1;
  lang_vec_check_resize_v1(v);
  _exit(0);
}

int fail(const char *message) {
  std::fprintf(stderr, "arc_concurrency_test: %s\n", message);
  return 1;
}

struct PanicCase {
  const char *name;
  void (*run)(LangVec *);
  const char *record;
};

int expect_panic(LangVec *v, const PanicCase &test) {
  int fds[2];
  if (pipe(fds) != 0) return fail("pipe failed");
  const pid_t pid = fork();
  if (pid < 0) return fail("fork failed");
  if (pid == 0) {
    close(fds[0]);
    if (dup2(fds[1], 2) < 0) _exit(97);
    close(fds[1]);
    test.run(v);
    _exit(0);
  }

  close(fds[1]);
  char record[1024];
  size_t used = 0;
  for (;;) {
    const ssize_t n = read(fds[0], record + used, sizeof(record) - 1 - used);
    if (n > 0) {
      used += static_cast<size_t>(n);
      if (used == sizeof(record) - 1) break;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;
  }
  close(fds[0]);
  record[used] = '\0';

  int status = 0;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR) return fail("waitpid failed");

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 101) {
    std::fprintf(stderr, "arc_concurrency_test: %s did not exit 101\n",
                 test.name);
    return 1;
  }
  if (!std::strstr(record, "LANG_NATIVE_PANIC:v1:") ||
      !std::strstr(record, test.record)) {
    std::fprintf(stderr, "arc_concurrency_test: %s panic record: %s\n",
                 test.name, record);
    return 1;
  }
  return 0;
}

int test_panic_regressions() {
  reset_observer_and_drops();
  LangVec *v = lang_vec_new_v1(2, sizeof(int), nullptr, drop_int);
  if (!v) return fail("panic fixture allocation failed");

  const PanicCase cases[] = {
      {"retain-overflow", panic_retain_overflow, "reference-count-overflow"},
      {"release-underflow", panic_release_underflow,
       "reference-count-overflow"},
      {"final-release-active-iteration",
       panic_final_release_active_iteration, "active-iteration"},
      {"begin-overflow", panic_begin_overflow, "active-iteration"},
      {"end-underflow", panic_end_underflow, "active-iteration"},
      {"check-resize-active", panic_check_resize_active, "active-iteration"},
  };
  for (const auto &test : cases)
    if (expect_panic(v, test)) return 1;

  // The parent copy is untouched by the forked panics and still releases
  // normally, including the single final free.
  if (__atomic_load_n(&v->refs, __ATOMIC_RELAXED) != 1)
    return fail("panic fixture parent refcount changed");
  lang_vec_release_v1(v);
  if (observed_final_free.load(std::memory_order_relaxed) != 1)
    return fail("panic fixture did not free exactly once");
  if (observed_element_drop.load(std::memory_order_relaxed) != 0)
    return fail("panic fixture dropped uninitialized elements");
  return 0;
}

int test_cleanup_regression() {
  reset_observer_and_drops();
  LangVec *v = lang_vec_new_v1(4, sizeof(int), nullptr, drop_int);
  if (!v) return fail("cleanup fixture allocation failed");
  const int values[] = {10, 20};
  for (int value : values) lang_vec_init_v1(v, &value);

  lang_vec_retain_v1(v);
  lang_vec_release_v1(v);
  if (dropped_total.load(std::memory_order_relaxed) != 0)
    return fail("retain/release dropped an element");
  if (observed_final_free.load(std::memory_order_relaxed) != 0)
    return fail("retain/release freed the object");
  if (__atomic_load_n(&v->refs, __ATOMIC_RELAXED) != 1)
    return fail("cleanup retain/release lost a count");

  lang_vec_release_v1(v);
  if (dropped_total.load(std::memory_order_relaxed) != 2)
    return fail("partial Vec must drop only its initialized prefix");
  if (drop_order[0].load(std::memory_order_relaxed) != 20 ||
      drop_order[1].load(std::memory_order_relaxed) != 10)
    return fail("partial Vec cleanup order is not reverse index");
  if (observed_element_drop.load(std::memory_order_relaxed) != 2)
    return fail("element observer count mismatch");
  if (observed_index_seen[0].load(std::memory_order_relaxed) != 1 ||
      observed_index_seen[1].load(std::memory_order_relaxed) != 1)
    return fail("element observer saw the wrong initialized indices");
  if (observed_final_free.load(std::memory_order_relaxed) != 1)
    return fail("final free observer count mismatch");
  if (observed_create.load(std::memory_order_relaxed) != 1)
    return fail("create observer count mismatch");
  return 0;
}

int test_retain_release_stress() {
  reset_observer_and_drops();
  constexpr uint64_t kLength = 8;
  LangVec *v = lang_vec_new_v1(kLength, sizeof(int), nullptr, drop_int);
  if (!v) return fail("retain stress allocation failed");
  for (uint64_t i = 0; i < kLength; ++i) {
    const int value = static_cast<int>(i);
    lang_vec_init_v1(v, &value);
  }

  const unsigned hardware = std::thread::hardware_concurrency();
  const unsigned thread_count = hardware ? (hardware < 16 ? hardware : 16) : 8;
  constexpr uint64_t kLoops = 100000;
  constexpr uint64_t kHeld = 64;

  std::atomic<unsigned> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> held_ready_gate{false};
  std::atomic<unsigned> held_ready{0};
  std::atomic<unsigned> worker_error{0};

  std::thread workers[16];
  for (unsigned t = 0; t < thread_count; ++t) {
    workers[t] = std::thread([&] {
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();

      for (uint64_t i = 0; i < kLoops; ++i) {
        lang_vec_retain_v1(v);
        lang_vec_release_v1(v);
      }

      // Hold references simultaneously so a lost increment is observable in
      // the main thread before any of these workers release them.
      for (uint64_t i = 0; i < kHeld; ++i) lang_vec_retain_v1(v);
      held_ready.fetch_add(1, std::memory_order_acq_rel);
      while (!held_ready_gate.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (uint64_t i = 0; i < kHeld; ++i) lang_vec_release_v1(v);
    });
  }

  while (ready.load(std::memory_order_acquire) != thread_count)
    std::this_thread::yield();
  go.store(true, std::memory_order_release);
  while (held_ready.load(std::memory_order_acquire) != thread_count)
    std::this_thread::yield();

  const uint64_t expected_held = 1 + thread_count * kHeld;
  const uint64_t held_refs = __atomic_load_n(&v->refs, __ATOMIC_RELAXED);
  if (held_refs != expected_held) {
    std::fprintf(stderr,
                 "arc_concurrency_test: held refcount %llu, expected %llu\n",
                 static_cast<unsigned long long>(held_refs),
                 static_cast<unsigned long long>(expected_held));
    worker_error.store(1, std::memory_order_relaxed);
  }
  held_ready_gate.store(true, std::memory_order_release);

  for (unsigned t = 0; t < thread_count; ++t) workers[t].join();

  if (worker_error.load(std::memory_order_relaxed))
    return 1;
  if (__atomic_load_n(&v->refs, __ATOMIC_RELAXED) != 1)
    return fail("retain/release stress lost a count");
  if (dropped_total.load(std::memory_order_relaxed) != 0)
    return fail("retain/release stress destroyed elements early");
  if (observed_final_free.load(std::memory_order_relaxed) != 0)
    return fail("retain/release stress freed the object early");

  const uint64_t expected_events = thread_count * (kLoops + kHeld);
  if (observed_retain.load(std::memory_order_relaxed) != expected_events)
    return fail("retain observer count mismatch");
  if (observed_release.load(std::memory_order_relaxed) != expected_events)
    return fail("release observer count mismatch");

  // The single owner releases only after all workers are gone, so cleanup and
  // deallocation happen exactly once and cannot race the allocator.
  lang_vec_release_v1(v);
  if (observed_final_free.load(std::memory_order_relaxed) != 1)
    return fail("retain stress did not free exactly once");
  if (observed_element_drop.load(std::memory_order_relaxed) != kLength)
    return fail("retain stress element observer count mismatch");
  if (dropped_total.load(std::memory_order_relaxed) != kLength)
    return fail("retain stress dropped the wrong number of elements");
  if (dropped_bad_value.load(std::memory_order_relaxed) != 0)
    return fail("retain stress dropped an out-of-range value");
  for (uint64_t i = 0; i < kLength; ++i)
    if (dropped_value_count[i].load(std::memory_order_relaxed) != 1)
      return fail("retain stress lost an element destructor");
  for (uint64_t i = 0; i < kLength; ++i)
    if (observed_index_seen[i].load(std::memory_order_relaxed) != 1)
      return fail("retain stress observed an element index the wrong number of times");
  for (uint64_t i = 0; i < kLength; ++i)
    if (drop_order[i].load(std::memory_order_relaxed) !=
        static_cast<int>(kLength - 1 - i))
      return fail("retain stress cleanup order is not reverse index");
  return 0;
}

int test_iteration_stress() {
  reset_observer_and_drops();
  constexpr uint64_t kLength = 4;
  LangVec *v = lang_vec_new_v1(kLength, sizeof(int), nullptr, drop_int);
  if (!v) return fail("iteration stress allocation failed");
  for (uint64_t i = 0; i < kLength; ++i) {
    const int value = static_cast<int>(i);
    lang_vec_init_v1(v, &value);
  }

  const unsigned hardware = std::thread::hardware_concurrency();
  const unsigned thread_count = hardware ? (hardware < 16 ? hardware : 16) : 8;
  constexpr uint64_t kLoops = 100000;

  std::atomic<unsigned> ready{0};
  std::atomic<bool> go{false};
  std::atomic<unsigned> worker_error{0};
  std::thread workers[16];
  for (unsigned t = 0; t < thread_count; ++t) {
    workers[t] = std::thread([&] {
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (uint64_t i = 0; i < kLoops; ++i) {
        const uint64_t length = lang_vec_begin_v1(v);
        if (length != kLength)
          worker_error.store(1, std::memory_order_relaxed);
        lang_vec_end_v1(v);
      }
    });
  }

  while (ready.load(std::memory_order_acquire) != thread_count)
    std::this_thread::yield();
  go.store(true, std::memory_order_release);
  for (unsigned t = 0; t < thread_count; ++t) workers[t].join();

  if (worker_error.load(std::memory_order_relaxed))
    return fail("begin returned a changed length");
  if (__atomic_load_n(&v->iterations, __ATOMIC_RELAXED) != 0)
    return fail("concurrent begin/end did not settle to zero");
  const uint64_t expected_events = thread_count * kLoops;
  if (observed_begin.load(std::memory_order_relaxed) != expected_events)
    return fail("begin observer count mismatch");
  if (observed_end.load(std::memory_order_relaxed) != expected_events)
    return fail("end observer count mismatch");

  // With all iterations complete, resize preflight succeeds.
  lang_vec_check_resize_v1(v);

  lang_vec_release_v1(v);
  if (observed_final_free.load(std::memory_order_relaxed) != 1)
    return fail("iteration stress did not free exactly once");
  if (dropped_total.load(std::memory_order_relaxed) != kLength)
    return fail("iteration stress lost element cleanup");
  return 0;
}

} // namespace

extern "C" void lang_vec_observe_v1(uint32_t event, const void *object,
                                     uint64_t detail) {
  if (event == 3 && retain_on_release.exchange(false))
    lang_vec_retain_v1(const_cast<LangVec *>(static_cast<const LangVec *>(object)));
  switch (event) {
  case 1:
    observed_create.fetch_add(1, std::memory_order_relaxed);
    break;
  case 2:
    observed_retain.fetch_add(1, std::memory_order_relaxed);
    break;
  case 3:
    observed_release.fetch_add(1, std::memory_order_relaxed);
    break;
  case 4:
    observed_element_drop.fetch_add(1, std::memory_order_relaxed);
    if (detail < kMaxTrackedValues)
      observed_index_seen[detail].fetch_add(1, std::memory_order_relaxed);
    else
      observed_bad_index.fetch_add(1, std::memory_order_relaxed);
    break;
  case 5:
    observed_final_free.fetch_add(1, std::memory_order_relaxed);
    break;
  case 6:
    observed_begin.fetch_add(1, std::memory_order_relaxed);
    break;
  case 7:
    observed_end.fetch_add(1, std::memory_order_relaxed);
    break;
  default:
    observed_bad_index.fetch_add(1, std::memory_order_relaxed);
    break;
  }
}

int main() {
  // A release observer can retain while the caller still owns its reference.
  reset_observer_and_drops();
  auto *observed = lang_vec_new_v1(1, sizeof(int), nullptr, drop_int);
  const int zero = 0;
  lang_vec_init_v1(observed, &zero);
  retain_on_release.store(true);
  lang_vec_release_v1(observed);
  if (dropped_total.load() || __atomic_load_n(&observed->refs, __ATOMIC_RELAXED) != 1)
    return fail("release observer could not retain the live object");
  lang_vec_release_v1(observed);
  if (dropped_total.load() != 1) return fail("observer-retained value cleanup");
  if (test_panic_regressions()) return 1;
  if (test_cleanup_regression()) return 1;
  if (test_retain_release_stress()) return 1;
  if (test_iteration_stress()) return 1;

  if (observed_bad_index.load(std::memory_order_relaxed) != 0)
    return fail("observer saw an invalid event or index");
  std::puts("arc-concurrency-ok");
  return 0;
}
