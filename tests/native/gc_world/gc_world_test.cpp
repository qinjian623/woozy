// Hosted Linux x86_64 concurrency suite for lang-gc-world/1.
//
// The suite is deliberately self-contained: run_all.sh builds it at O0 and O2
// with the real runtime allocator and once more with ASAN/UBSAN and a libc-heap
// allocation shim. Every test runs in this one process (misuse panic cases run
// in gc_world_misuse_test). The external script applies a per-process timeout.

#include "gc_world.hpp"
#include "alloc.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" uint64_t gc_test_alloc_live() noexcept;
extern "C" uint64_t gc_test_alloc_total() noexcept;

namespace {

using Clock = std::chrono::steady_clock;

std::atomic<uint64_t> g_counter{0};

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
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (Clock::now() < deadline) {
    if (predicate())
      return true;
    sleep_ms(1);
  }
  return predicate();
}

int64_t elapsed_ms(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                               start)
      .count();
}

void expect_live(uint64_t expected) {
  CHECK(gc_test_alloc_live() == expected);
}

// Bit 63 of lang_gc_safepoint_word_v1 mirrors the world request for the
// one-load compiler fast path.
bool safepoint_request() {
  return (__atomic_load_n(&lang_gc_safepoint_word_v1, __ATOMIC_ACQUIRE) &
          LANG_GC_SAFEPOINT_REQUEST) != 0;
}

// A mutator that keeps the graph counter moving and polls at every loop
// iteration. It registers itself and leaves only after stop_workers is set and
// the world is running again.
struct MutatorGroup {
  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::vector<std::thread> threads;

  void start(int count) {
    for (int index = 0; index < count; ++index) {
      threads.emplace_back([this] {
        CHECK(lang_gc_world_register_v1());
        g_counter.fetch_add(1, std::memory_order_relaxed);
        ready.fetch_add(1, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
          g_counter.fetch_add(1, std::memory_order_relaxed);
          lang_gc_world_poll_v1();
        }
        CHECK(lang_gc_world_unregister_v1());
      });
    }
    CHECK(wait_for([this, count] {
            return ready.load(std::memory_order_acquire) == count;
          },
          5000));
  }

  void finish() {
    stop.store(true, std::memory_order_release);
    for (auto &thread : threads)
      thread.join();
    threads.clear();
  }
};

void test_basic_stop_and_resume() {
  expect_live(0);
  CHECK(lang_gc_world_register_v1());
  // A foreign callback that calls register again gets false. Per the contract
  // it must not unregister, so the language thread's registration remains.
  CHECK(!lang_gc_world_register_v1());
  expect_live(1);

  lang_gc_world_enter_safe_v1();
  lang_gc_world_enter_safe_v1();
  lang_gc_world_leave_safe_v1();
  lang_gc_world_leave_safe_v1();

  MutatorGroup group;
  group.start(3);
  sleep_ms(30);

  // -1 exercises the infinite futex wait path.
  CHECK(lang_gc_world_stop_v1(-1) == 0);
  // The advisory request mirror is set for the whole requested interval.
  CHECK(lang_gc_world_poll_required_v1());
  CHECK(safepoint_request());
  const uint64_t stopped_counter = g_counter.load(std::memory_order_relaxed);
  sleep_ms(60);
  CHECK(g_counter.load(std::memory_order_relaxed) == stopped_counter);

  // Self-reentry asks for a second stop while this collector owns one.
  CHECK(lang_gc_world_stop_v1(0) == -16);
  // The collector is exempt from its own request.
  lang_gc_world_poll_v1();

  lang_gc_world_resume_v1();
  CHECK(!lang_gc_world_poll_required_v1());
  CHECK(!safepoint_request());
  CHECK(wait_for([stopped_counter] {
          return g_counter.load(std::memory_order_relaxed) > stopped_counter;
        },
        2000));

  group.finish();
  CHECK(lang_gc_world_unregister_v1());
  CHECK(!lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_critical_poll_defers_stop_and_nested_leave() {
  // The driver is intentionally unregistered. Only the collector and the
  // mutator are registry members.
  std::atomic<bool> collector_ready{false};
  std::atomic<bool> in_critical{false};
  std::atomic<bool> half_write{false};
  std::atomic<bool> poll_deferred{false};
  std::atomic<bool> nested_entered{false};
  std::atomic<bool> inner_left{false};
  std::atomic<bool> outer_left{false};
  std::atomic<bool> graph_complete{false};
  std::atomic<bool> stop_started{false};
  std::atomic<bool> stop_succeeded{false};
  std::atomic<bool> release_outer{false};
  std::atomic<bool> resume_now{false};
  std::atomic<bool> worker_done{false};

  std::thread collector([&] {
    CHECK(lang_gc_world_register_v1());
    collector_ready.store(true, std::memory_order_release);
    while (!stop_started.load(std::memory_order_acquire))
      sleep_ms(1);
    CHECK(lang_gc_world_stop_v1(5000000000LL) == 0);
    stop_succeeded.store(true, std::memory_order_release);
    while (!resume_now.load(std::memory_order_acquire))
      sleep_ms(1);
    lang_gc_world_resume_v1();
    CHECK(lang_gc_world_unregister_v1());
  });

  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_critical_v1();
    half_write.store(true, std::memory_order_release);
    in_critical.store(true, std::memory_order_release);

    while (!lang_gc_world_poll_required_v1())
      std::this_thread::yield();
    // Repeated registration must not park an already-critical thread.
    CHECK(!lang_gc_world_register_v1());
    // The request is active but the critical region must defer PARK.
    lang_gc_world_poll_v1();
    poll_deferred.store(true, std::memory_order_release);

    lang_gc_world_enter_critical_v1();
    nested_entered.store(true, std::memory_order_release);
    // Polling at nested depth is still deferred.
    lang_gc_world_poll_v1();
    // An inner leave must not become a safepoint.
    lang_gc_world_leave_critical_v1();
    inner_left.store(true, std::memory_order_release);

    while (!release_outer.load(std::memory_order_acquire))
      sleep_ms(1);
    // Outer leave is the safepoint; it parks until resume.
    lang_gc_world_leave_critical_v1();
    outer_left.store(true, std::memory_order_release);
    graph_complete.store(true, std::memory_order_release);
    CHECK(lang_gc_world_unregister_v1());
    worker_done.store(true, std::memory_order_release);
  });

  CHECK(wait_for([&] { return collector_ready.load(); }, 5000));
  CHECK(wait_for([&] { return in_critical.load(); }, 5000));
  stop_started.store(true, std::memory_order_release);

  CHECK(wait_for([&] {
          return poll_deferred.load() && nested_entered.load() &&
                 inner_left.load();
        },
        5000));
  sleep_ms(50);
  // The graph is mid-write and the mutator is still critical.
  CHECK(half_write.load());
  CHECK(!graph_complete.load());
  CHECK(!stop_succeeded.load());

  release_outer.store(true, std::memory_order_release);
  CHECK(wait_for([&] { return stop_succeeded.load(); }, 5000));
  sleep_ms(50);
  // The outermost leave parked inside leave_critical and cannot return yet.
  CHECK(!outer_left.load());

  resume_now.store(true, std::memory_order_release);
  CHECK(wait_for([&] { return outer_left.load() && worker_done.load(); }, 5000));
  worker.join();
  collector.join();
  CHECK(graph_complete.load());
  expect_live(0);
}

void test_critical_stop_busy_and_can_collect() {
  CHECK(lang_gc_world_register_v1());
  CHECK(lang_gc_world_can_collect_v1());

  lang_gc_world_enter_critical_v1();
  CHECK(!lang_gc_world_can_collect_v1());
  CHECK(lang_gc_world_stop_v1(0) == -16);
  CHECK(!lang_gc_world_poll_required_v1());
  lang_gc_world_enter_critical_v1();
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_world_leave_critical_v1();
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_world_leave_critical_v1();
  CHECK(lang_gc_world_can_collect_v1());

  // A successful stop makes the collector non-collectible but must not be
  // released or consumed by can_collect.
  CHECK(lang_gc_world_stop_v1(0) == 0);
  CHECK(!lang_gc_world_can_collect_v1());
  CHECK(lang_gc_world_stop_v1(0) == -16);
  lang_gc_world_resume_v1();
  CHECK(lang_gc_world_can_collect_v1());

  lang_gc_world_enter_safe_v1();
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_world_leave_safe_v1();
  CHECK(lang_gc_world_can_collect_v1());

  CHECK(lang_gc_world_unregister_v1());
  // Unregistered is false and does not silently register the thread.
  CHECK(!lang_gc_world_can_collect_v1());
  CHECK(lang_gc_world_register_v1());
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_can_collect_false_during_other_stop() {
  CHECK(lang_gc_world_register_v1());

  std::atomic<bool> worker_ready{false};
  std::atomic<bool> query_done{false};
  std::atomic<bool> worker_left{false};
  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1());
    worker_ready.store(true, std::memory_order_release);
    while (!lang_gc_world_poll_required_v1())
      std::this_thread::yield();
    // Another collector owns the pending stop. This thread is still ACTIVE
    // and has not parked, but can_collect must be false while that stop owns
    // the world.
    CHECK(!lang_gc_world_can_collect_v1());
    query_done.store(true, std::memory_order_release);
    lang_gc_world_poll_v1(); // now park so the stop can complete
    worker_left.store(true, std::memory_order_release);
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(wait_for([&] { return worker_ready.load(); }, 5000));
  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  CHECK(wait_for([&] { return query_done.load(); }, 2000));
  lang_gc_world_resume_v1();
  CHECK(wait_for([&] { return worker_left.load(); }, 2000));
  worker.join();
  CHECK(lang_gc_world_can_collect_v1());
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_critical_long_region_stop_timeout() {
  CHECK(lang_gc_world_register_v1());

  std::atomic<bool> in_critical{false};
  std::atomic<bool> release_worker{false};
  std::atomic<bool> left{false};
  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_critical_v1();
    in_critical.store(true, std::memory_order_release);
    while (!release_worker.load(std::memory_order_acquire)) {
      // Deferred while the request exists; harmless fast path after retract.
      lang_gc_world_poll_v1();
      sleep_ms(1);
    }
    lang_gc_world_leave_critical_v1();
    left.store(true, std::memory_order_release);
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(wait_for([&] { return in_critical.load(); }, 5000));
  const auto start = Clock::now();
  CHECK(lang_gc_world_stop_v1(50000000LL) == -110); // 50 ms
  const int64_t elapsed = elapsed_ms(start);
  CHECK(elapsed >= 20);
  CHECK(elapsed < 2000);
  CHECK(!lang_gc_world_poll_required_v1());

  // The old request was retracted; leaving the long critical region must not
  // park on a stale epoch or request.
  release_worker.store(true, std::memory_order_release);
  CHECK(wait_for([&] { return left.load(); }, 5000));
  worker.join();
  CHECK(!lang_gc_world_poll_required_v1());
  CHECK(lang_gc_world_can_collect_v1());
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_hundreds_of_stop_resume_rounds() {
  CHECK(lang_gc_world_register_v1());
  MutatorGroup group;
  group.start(2);

  uint64_t last_counter = g_counter.load(std::memory_order_relaxed);
  for (int round = 0; round < 300; ++round) {
    CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
    if ((round % 50) == 0) {
      const uint64_t first = g_counter.load(std::memory_order_relaxed);
      sleep_ms(5);
      CHECK(g_counter.load(std::memory_order_relaxed) == first);
    }
    lang_gc_world_resume_v1();
    if ((round % 50) == 0) {
      CHECK(wait_for([last_counter] {
              return g_counter.load(std::memory_order_relaxed) > last_counter;
            },
            2000));
      last_counter = g_counter.load(std::memory_order_relaxed);
    }
  }

  const uint64_t before_finish = g_counter.load(std::memory_order_relaxed);
  CHECK(wait_for([before_finish] {
          return g_counter.load(std::memory_order_relaxed) > before_finish;
        },
        2000));
  group.finish();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

// The request mirror follows set/retract/resume exactly: a timed-out stop must
// leave it clear so the fast path stops entering the world poll, and a
// successful stop must keep it set until resume.
void test_safepoint_request_mirror_follows_stop() {
  CHECK(lang_gc_world_register_v1());
  CHECK(!safepoint_request());

  std::atomic<bool> in_critical{false};
  std::atomic<bool> release_worker{false};
  std::atomic<bool> left{false};
  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_critical_v1();
    in_critical.store(true, std::memory_order_release);
    while (!release_worker.load(std::memory_order_acquire)) {
      lang_gc_world_poll_v1(); // deferred while the request exists
      sleep_ms(1);
    }
    lang_gc_world_leave_critical_v1();
    left.store(true, std::memory_order_release);
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(wait_for([&] { return in_critical.load(); }, 5000));
  CHECK(lang_gc_world_stop_v1(50000000LL) == -110); // critical keeps it ACTIVE
  CHECK(!lang_gc_world_poll_required_v1());
  CHECK(!safepoint_request());

  release_worker.store(true, std::memory_order_release);
  CHECK(wait_for([&] { return left.load(); }, 5000));
  worker.join();
  CHECK(lang_gc_world_can_collect_v1());

  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  CHECK(lang_gc_world_poll_required_v1());
  CHECK(safepoint_request());
  lang_gc_world_resume_v1();
  CHECK(!lang_gc_world_poll_required_v1());
  CHECK(!safepoint_request());

  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_WORLD_SAFEPOINT_MIRROR_OK\n");
}

// The advisory word alone must be a sufficient mutator fast path: a thread
// that never reads the authoritative request word must still enter the poll and
// park on a stop. This test proves that sufficiency and that a fully registered
// mutator cannot be left waiting on a resume the collector is itself waiting
// for; it cannot prove the store ORDER (two independent reads cannot observe a
// one-store window) — the order is specified in gc_world.hpp and implemented in
// the registry-locked stop/retract/resume/fork-child sites.
void test_safepoint_mirror_alone_parks_on_stop() {
  CHECK(lang_gc_world_register_v1());
  std::atomic<bool> registered{false};
  std::atomic<bool> finish{false};
  std::atomic<bool> entered{false};
  std::atomic<uint64_t> fast_polls{0};
  std::thread mutator([&] {
    CHECK(lang_gc_world_register_v1());
    registered.store(true, std::memory_order_release); // before any stop
    while (!finish.load(std::memory_order_acquire)) {
      if (!safepoint_request())
        continue;
      entered.store(true, std::memory_order_release);
      lang_gc_world_poll_v1(); // advisory-triggered; parks until resume
      fast_polls.fetch_add(1, std::memory_order_relaxed);
    }
    CHECK(lang_gc_world_unregister_v1());
  });
  CHECK(wait_for([&] { return registered.load(); }, 5000));
  sleep_ms(20); // the mutator is inside its advisory loop, not registering
  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  CHECK(wait_for([&] { return entered.load(); }, 5000));
  lang_gc_world_resume_v1();
  finish.store(true, std::memory_order_release);
  mutator.join();
  CHECK(fast_polls.load() >= 1);
  CHECK(!safepoint_request());
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_WORLD_SAFEPOINT_ADVISORY_PARK_OK polls=%llu\n",
              static_cast<unsigned long long>(fast_polls.load()));
}

void test_safe_thread_does_not_block_stop_and_leave_waits() {
  CHECK(lang_gc_world_register_v1());

  std::atomic<bool> safe_ready{false};
  std::atomic<bool> release_leave{false};
  std::atomic<bool> about_to_leave{false};
  std::atomic<bool> left{false};

  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_safe_v1();
    safe_ready.store(true, std::memory_order_release);
    while (!release_leave.load(std::memory_order_acquire))
      sleep_ms(1);
    about_to_leave.store(true, std::memory_order_release);
    lang_gc_world_leave_safe_v1();
    left.store(true, std::memory_order_release);
    // Graph access is legal again only after leave returned.
    g_counter.fetch_add(1, std::memory_order_relaxed);
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(wait_for([&] { return safe_ready.load(std::memory_order_acquire); },
                 5000));
  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  release_leave.store(true, std::memory_order_release);
  CHECK(wait_for([&] { return about_to_leave.load(std::memory_order_acquire); },
                 2000));
  sleep_ms(80);
  CHECK(!left.load(std::memory_order_acquire));

  lang_gc_world_resume_v1();
  CHECK(wait_for([&] { return left.load(std::memory_order_acquire); }, 2000));
  worker.join();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_nested_safe() {
  CHECK(lang_gc_world_register_v1());

  std::atomic<bool> ready{false};
  std::atomic<bool> release_first{false};
  std::atomic<bool> first_left{false};
  std::atomic<bool> release_second{false};
  std::atomic<bool> done{false};

  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_safe_v1();
    lang_gc_world_enter_safe_v1();
    ready.store(true, std::memory_order_release);
    while (!release_first.load(std::memory_order_acquire))
      sleep_ms(1);
    lang_gc_world_leave_safe_v1();
    first_left.store(true, std::memory_order_release);
    while (!release_second.load(std::memory_order_acquire))
      sleep_ms(1);
    lang_gc_world_leave_safe_v1();
    CHECK(lang_gc_world_unregister_v1());
    done.store(true, std::memory_order_release);
  });

  CHECK(wait_for([&] { return ready.load(std::memory_order_acquire); }, 5000));
  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);

  // The first leave only drops the nesting depth, so it must return while the
  // test collector still owns the stop.
  release_first.store(true, std::memory_order_release);
  CHECK(wait_for([&] { return first_left.load(std::memory_order_acquire); },
                 2000));
  CHECK(!done.load(std::memory_order_acquire));

  lang_gc_world_resume_v1();
  release_second.store(true, std::memory_order_release);
  CHECK(wait_for([&] { return done.load(std::memory_order_acquire); }, 2000));
  worker.join();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_registration_during_stop_waits() {
  CHECK(lang_gc_world_register_v1());

  std::atomic<bool> attempting{false};
  std::atomic<bool> returned{false};
  std::atomic<uint64_t> worker_counter{0};

  std::thread late([&] {
    // Reading the exported request word does not require registration.
    while (!lang_gc_world_poll_required_v1())
      std::this_thread::yield();
    attempting.store(true, std::memory_order_release);
    const bool newly_registered = lang_gc_world_register_v1();
    CHECK(newly_registered);
    returned.store(true, std::memory_order_release);
    worker_counter.fetch_add(1, std::memory_order_relaxed);
    lang_gc_world_poll_v1();
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  CHECK(wait_for([&] { return attempting.load(std::memory_order_acquire); },
                 2000));
  sleep_ms(80);
  CHECK(!returned.load(std::memory_order_acquire));
  CHECK(worker_counter.load(std::memory_order_relaxed) == 0);

  lang_gc_world_resume_v1();
  CHECK(wait_for([&] { return returned.load(std::memory_order_acquire); },
                 2000));
  CHECK(worker_counter.load(std::memory_order_relaxed) == 1);
  late.join();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_registration_wait_wakes_on_timeout() {
  CHECK(lang_gc_world_register_v1());

  std::atomic<bool> blocker_ready{false};
  std::atomic<bool> release_blocker{false};
  std::thread blocker([&] {
    CHECK(lang_gc_world_register_v1());
    blocker_ready.store(true, std::memory_order_release);
    while (!release_blocker.load(std::memory_order_acquire))
      sleep_ms(1);
    lang_gc_world_poll_v1();
    CHECK(lang_gc_world_unregister_v1());
  });
  CHECK(wait_for(
      [&] { return blocker_ready.load(std::memory_order_acquire); }, 5000));

  std::atomic<bool> attempting{false};
  std::atomic<bool> returned{false};
  std::atomic<bool> was_new{false};
  std::thread late([&] {
    while (!lang_gc_world_poll_required_v1())
      std::this_thread::yield();
    attempting.store(true, std::memory_order_release);
    // If the request is still owned, register parks until the timeout
    // retraction wakes it. If the retraction already happened, it registers
    // immediately; both paths must return and never start graph work early.
    was_new.store(lang_gc_world_register_v1(), std::memory_order_release);
    returned.store(true, std::memory_order_release);
    lang_gc_world_poll_v1();
    CHECK(lang_gc_world_unregister_v1());
  });

  sleep_ms(20);
  const auto start = Clock::now();
  CHECK(lang_gc_world_stop_v1(300000000LL) == -110); // 300 ms
  CHECK(elapsed_ms(start) >= 100);
  CHECK(wait_for([&] { return attempting.load(std::memory_order_acquire); },
                 2000));
  CHECK(wait_for([&] { return returned.load(std::memory_order_acquire); },
                 2000));
  CHECK(was_new.load(std::memory_order_acquire));
  late.join();

  release_blocker.store(true, std::memory_order_release);
  blocker.join();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_unregister_races_stop() {
  CHECK(lang_gc_world_register_v1());

  std::atomic<bool> racer_done{false};
  std::thread racer([&] {
    for (int iteration = 0; iteration < 150; ++iteration) {
      CHECK(lang_gc_world_register_v1());
      g_counter.fetch_add(1, std::memory_order_relaxed);
      lang_gc_world_poll_v1();
      CHECK(lang_gc_world_unregister_v1());
    }
    racer_done.store(true, std::memory_order_release);
  });

  for (int iteration = 0; iteration < 150; ++iteration) {
    if (racer_done.load(std::memory_order_acquire))
      break;
    const int64_t result = lang_gc_world_stop_v1(500000); // 0.5 ms
    if (result == 0) {
      lang_gc_world_resume_v1();
    } else {
      CHECK(result == -110);
    }
  }

  CHECK(wait_for([&] { return racer_done.load(std::memory_order_acquire); },
                 5000));
  racer.join();

  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  const uint64_t stable = g_counter.load(std::memory_order_relaxed);
  sleep_ms(30);
  CHECK(g_counter.load(std::memory_order_relaxed) == stable);
  lang_gc_world_resume_v1();

  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_unregister_active_lets_stop_proceed() {
  CHECK(lang_gc_world_register_v1());

  std::atomic<bool> registered{false};
  std::atomic<bool> unregistered{false};
  std::thread leaving([&] {
    CHECK(lang_gc_world_register_v1());
    registered.store(true, std::memory_order_release);
    while (!lang_gc_world_poll_required_v1())
      std::this_thread::yield();
    // Deliberately unregister without polling first. Removing the node is the
    // event that lets the collector stop succeed.
    CHECK(lang_gc_world_unregister_v1());
    unregistered.store(true, std::memory_order_release);
  });

  CHECK(wait_for([&] { return registered.load(std::memory_order_acquire); },
                 5000));
  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  CHECK(wait_for([&] { return unregistered.load(std::memory_order_acquire); },
                 2000));
  CHECK(lang_gc_world_stop_v1(0) == -16);
  lang_gc_world_resume_v1();

  leaving.join();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_timeout_retracts_and_wakes_parked_threads() {
  CHECK(lang_gc_world_register_v1());

  std::atomic<bool> blocker_ready{false};
  std::atomic<bool> release_blocker{false};
  std::atomic<bool> stop_workers{false};
  std::atomic<int> ready{0};
  std::vector<std::thread> workers;

  std::thread blocker([&] {
    CHECK(lang_gc_world_register_v1());
    blocker_ready.store(true, std::memory_order_release);
    while (!release_blocker.load(std::memory_order_acquire))
      sleep_ms(1);
    // No request is active now; this poll is only the ordinary fast path.
    lang_gc_world_poll_v1();
    CHECK(lang_gc_world_unregister_v1());
  });

  for (int index = 0; index < 2; ++index) {
    workers.emplace_back([&] {
      CHECK(lang_gc_world_register_v1());
      ready.fetch_add(1, std::memory_order_release);
      while (!stop_workers.load(std::memory_order_acquire)) {
        g_counter.fetch_add(1, std::memory_order_relaxed);
        lang_gc_world_poll_v1();
      }
      CHECK(lang_gc_world_unregister_v1());
    });
  }

  CHECK(wait_for(
      [&] { return ready.load(std::memory_order_acquire) == 2; }, 5000));
  CHECK(wait_for(
      [&] { return blocker_ready.load(std::memory_order_acquire); }, 5000));
  sleep_ms(20);

  // The blocker remains ACTIVE (it is not in SAFE) so the stop cannot succeed.
  CHECK(lang_gc_world_stop_v1(0) == -110);
  uint64_t after_retract = g_counter.load(std::memory_order_relaxed);
  CHECK(wait_for([&] {
          return g_counter.load(std::memory_order_relaxed) > after_retract;
        },
        2000));

  const auto start = Clock::now();
  CHECK(lang_gc_world_stop_v1(80000000) == -110); // 80 ms
  const int64_t elapsed = elapsed_ms(start);
  CHECK(elapsed >= 20);
  CHECK(elapsed < 2000);

  after_retract = g_counter.load(std::memory_order_relaxed);
  CHECK(wait_for([&] {
          return g_counter.load(std::memory_order_relaxed) > after_retract;
        },
        2000));

  stop_workers.store(true, std::memory_order_release);
  release_blocker.store(true, std::memory_order_release);
  for (auto &thread : workers)
    thread.join();
  blocker.join();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

volatile sig_atomic_t g_signal_count = 0;
volatile sig_atomic_t g_in_stop_window = 0;
volatile sig_atomic_t g_signal_during_stop = 0;

extern "C" void signal_counter_handler(int) {
  g_signal_count = g_signal_count + 1;
  if (g_in_stop_window)
    g_signal_during_stop = 1;
}

void test_monotonic_deadline_survives_eintr() {
  CHECK(lang_gc_world_register_v1());

  std::atomic<bool> blocker_ready{false};
  std::atomic<bool> release_blocker{false};
  std::thread blocker([&] {
    CHECK(lang_gc_world_register_v1());
    blocker_ready.store(true, std::memory_order_release);
    while (!release_blocker.load(std::memory_order_acquire))
      sleep_ms(1);
    lang_gc_world_poll_v1();
    CHECK(lang_gc_world_unregister_v1());
  });
  CHECK(wait_for(
      [&] { return blocker_ready.load(std::memory_order_acquire); }, 5000));

  struct sigaction action {};
  action.sa_handler = signal_counter_handler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0; // No SA_RESTART: futex must report EINTR.
  struct sigaction previous {};
  CHECK(sigaction(SIGUSR1, &action, &previous) == 0);

  const pthread_t target = pthread_self();
  std::atomic<bool> sender_stop{false};
  std::thread sender([&] {
    const auto end = Clock::now() + std::chrono::milliseconds(700);
    while (!sender_stop.load(std::memory_order_acquire) &&
           Clock::now() < end) {
      pthread_kill(target, SIGUSR1);
      sleep_ms(2);
    }
  });

  g_signal_count = 0;
  g_signal_during_stop = 0;
  CHECK(wait_for([] { return g_signal_count > 0; }, 2000));
  g_in_stop_window = 1;
  const auto start = Clock::now();
  const int64_t result = lang_gc_world_stop_v1(150000000LL); // 150 ms
  g_in_stop_window = 0;
  const int64_t elapsed = elapsed_ms(start);
  sender_stop.store(true, std::memory_order_release);
  sender.join();
  CHECK(sigaction(SIGUSR1, &previous, nullptr) == 0);

  CHECK(result == -110);
  CHECK(g_signal_count > 0);
  CHECK(g_signal_during_stop == 1);
  CHECK(elapsed >= 100);
  // A relative-timeout retry loop would restart the 150 ms wait on every
  // EINTR and only finish when the 700 ms signal storm ends.
  CHECK(elapsed < 500);

  release_blocker.store(true, std::memory_order_release);
  blocker.join();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_collector_competition_and_self_reentry() {
  CHECK(lang_gc_world_register_v1());

  std::atomic<bool> go{false};
  std::atomic<bool> loser_attempted{false};
  std::atomic<bool> main_resumed{false};
  std::thread winner([&] {
    CHECK(lang_gc_world_register_v1());
    while (!go.load(std::memory_order_acquire))
      std::this_thread::yield();
    CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
    CHECK(lang_gc_world_stop_v1(0) == -16); // Self reentry.
    CHECK(wait_for(
        [&] { return loser_attempted.load(std::memory_order_acquire); }, 2000));
    lang_gc_world_resume_v1();
    CHECK(wait_for(
        [&] { return main_resumed.load(std::memory_order_acquire); }, 2000));
    CHECK(lang_gc_world_unregister_v1());
  });

  go.store(true, std::memory_order_release);
  CHECK(wait_for([] { return lang_gc_world_poll_required_v1(); }, 2000));
  // The winner already owns the stop. This collector must get EBUSY, then
  // park so the winner's stop can complete.
  CHECK(lang_gc_world_stop_v1(2000000000LL) == -16);
  loser_attempted.store(true, std::memory_order_release);
  lang_gc_world_poll_v1();
  main_resumed.store(true, std::memory_order_release);

  winner.join();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_same_tid_register_unregister_cycles() {
  for (int iteration = 0; iteration < 1000; ++iteration) {
    CHECK(lang_gc_world_register_v1());
    CHECK(!lang_gc_world_register_v1());
    CHECK(lang_gc_world_unregister_v1());
    CHECK(!lang_gc_world_unregister_v1());
    expect_live(0);
  }
}

void test_thread_exit_cycles() {
  for (int iteration = 0; iteration < 200; ++iteration) {
    std::atomic<bool> finished{false};
    std::thread worker([&] {
      CHECK(lang_gc_world_register_v1());
      CHECK(!lang_gc_world_register_v1());
      g_counter.fetch_add(1, std::memory_order_relaxed);
      lang_gc_world_poll_v1();
      CHECK(lang_gc_world_unregister_v1());
      CHECK(!lang_gc_world_unregister_v1());
      finished.store(true, std::memory_order_release);
    });
    worker.join();
    CHECK(finished.load(std::memory_order_acquire));
    expect_live(0);
  }
}

struct ChildReport {
  uint64_t magic;
  int64_t stop_result;
  int64_t second_stop_result;
  uint64_t live_after;
  int32_t register_was_new;
  int32_t unregister_removed;
  int32_t poll_pending_after_fork;
  int32_t ok;
};

constexpr uint64_t kReportMagic = 0x4743574f524c4431ULL; // "GCWORLD1"

bool write_all_fd(int fd, const void *buffer, size_t size) {
  const auto *bytes = static_cast<const char *>(buffer);
  while (size) {
    const ssize_t written = ::write(fd, bytes, size);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    bytes += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool read_all_fd(int fd, void *buffer, size_t size) {
  auto *bytes = static_cast<char *>(buffer);
  while (size) {
    const ssize_t received = ::read(fd, bytes, size);
    if (received < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (received == 0)
      return false;
    bytes += received;
    size -= static_cast<size_t>(received);
  }
  return true;
}

void close_fd(int &fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

void test_fork_prunes_inherited_registry() {
  CHECK(lang_gc_world_register_v1());
  MutatorGroup group;
  group.start(1);
  sleep_ms(20);

  int pipe_fds[2] = {-1, -1};
  CHECK(::pipe(pipe_fds) == 0);

  lang_alloc_fork_prepare_v1();
  lang_gc_world_fork_prepare_v1();
  const pid_t pid = ::fork();
  CHECK(pid >= 0);

  if (pid == 0) {
    close_fd(pipe_fds[0]);
    // Allocator lock first: the GC child hook frees inherited registry nodes.
    lang_alloc_fork_child_v1();
    lang_gc_world_fork_child_v1();
    // The child drops the inherited request mirror before running any code.
    CHECK(!safepoint_request());
    CHECK(!lang_gc_world_poll_required_v1());

    ChildReport report{};
    report.magic = kReportMagic;
    report.register_was_new = lang_gc_world_register_v1() ? 1 : 0;
    report.poll_pending_after_fork = lang_gc_world_poll_required_v1() ? 1 : 0;
    report.stop_result = lang_gc_world_stop_v1(0);
    report.second_stop_result = lang_gc_world_stop_v1(0);
    if (report.second_stop_result == 0) {
      // A second stop cannot succeed without a resume.
      report.second_stop_result = -9999;
    } else {
      lang_gc_world_resume_v1();
    }
    report.unregister_removed = lang_gc_world_unregister_v1() ? 1 : 0;
    report.live_after = gc_test_alloc_live();
    report.ok = report.register_was_new == 0 &&
                report.poll_pending_after_fork == 0 &&
                report.stop_result == 0 &&
                report.second_stop_result == -16 &&
                report.unregister_removed == 1 && report.live_after == 0;
    (void)write_all_fd(pipe_fds[1], &report, sizeof(report));
    ::close(pipe_fds[1]);
    ::_exit(report.ok ? 0 : 1);
  }

  lang_gc_world_fork_parent_v1();
  lang_alloc_fork_parent_v1();
  close_fd(pipe_fds[1]);

  ChildReport report{};
  CHECK(read_all_fd(pipe_fds[0], &report, sizeof(report)));
  close_fd(pipe_fds[0]);
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  CHECK(report.magic == kReportMagic);
  CHECK(report.ok == 1);

  // The parent's worker is still registered and running.
  const uint64_t before = g_counter.load(std::memory_order_relaxed);
  CHECK(wait_for([before] {
          return g_counter.load(std::memory_order_relaxed) > before;
        },
        2000));
  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  lang_gc_world_resume_v1();

  group.finish();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_fork_from_safe_preserves_depth() {
  CHECK(lang_gc_world_register_v1());
  lang_gc_world_enter_safe_v1();

  int pipe_fds[2] = {-1, -1};
  CHECK(::pipe(pipe_fds) == 0);
  lang_alloc_fork_prepare_v1();
  lang_gc_world_fork_prepare_v1();
  const pid_t pid = ::fork();
  CHECK(pid >= 0);

  if (pid == 0) {
    close_fd(pipe_fds[0]);
    lang_alloc_fork_child_v1();
    lang_gc_world_fork_child_v1();

    int ok = 1;
    ok = ok && !lang_gc_world_register_v1();
    ok = ok && !lang_gc_world_poll_required_v1();
    // The surviving thread was forked inside a SAFE region. If the depth was
    // lost, this leave or the unregister below would Panic.
    lang_gc_world_leave_safe_v1();
    ok = ok && (lang_gc_world_stop_v1(0) == 0);
    lang_gc_world_resume_v1();
    ok = ok && lang_gc_world_unregister_v1();
    ok = ok && (gc_test_alloc_live() == 0);
    (void)write_all_fd(pipe_fds[1], &ok, sizeof(ok));
    ::close(pipe_fds[1]);
    ::_exit(ok ? 0 : 1);
  }

  lang_gc_world_fork_parent_v1();
  lang_alloc_fork_parent_v1();
  close_fd(pipe_fds[1]);
  lang_gc_world_leave_safe_v1();

  int child_ok = 0;
  CHECK(read_all_fd(pipe_fds[0], &child_ok, sizeof(child_ok)));
  close_fd(pipe_fds[0]);
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  CHECK(child_ok == 1);

  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_fork_from_critical_preserves_nested_depth() {
  CHECK(lang_gc_world_register_v1());
  lang_gc_world_enter_critical_v1();
  lang_gc_world_enter_critical_v1();
  CHECK(!lang_gc_world_can_collect_v1());

  int pipe_fds[2] = {-1, -1};
  CHECK(::pipe(pipe_fds) == 0);
  // The future fork path enters critical before taking the allocator lock.
  lang_alloc_fork_prepare_v1();
  lang_gc_world_fork_prepare_v1();
  const pid_t pid = ::fork();
  CHECK(pid >= 0);

  if (pid == 0) {
    close_fd(pipe_fds[0]);
    lang_alloc_fork_child_v1();
    lang_gc_world_fork_child_v1();

    int ok = 1;
    ok = ok && !lang_gc_world_register_v1();
    ok = ok && !lang_gc_world_can_collect_v1();
    lang_gc_world_leave_critical_v1(); // depth becomes 1, not zero
    ok = ok && !lang_gc_world_poll_required_v1();
    ok = ok && !lang_gc_world_can_collect_v1();
    // If the nested depth was lost, this stop would succeed (or the next leave
    // would Panic). A -16 proves the surviving thread is still critical.
    ok = ok && (lang_gc_world_stop_v1(0) == -16);
    ok = ok && !lang_gc_world_poll_required_v1();
    lang_gc_world_leave_critical_v1();
    ok = ok && lang_gc_world_can_collect_v1();
    ok = ok && (lang_gc_world_stop_v1(0) == 0);
    lang_gc_world_resume_v1();
    ok = ok && lang_gc_world_unregister_v1();
    ok = ok && (gc_test_alloc_live() == 0);
    (void)write_all_fd(pipe_fds[1], &ok, sizeof(ok));
    ::close(pipe_fds[1]);
    ::_exit(ok ? 0 : 1);
  }

  lang_gc_world_fork_parent_v1();
  lang_alloc_fork_parent_v1();
  close_fd(pipe_fds[1]);

  // The parent still owns both critical levels.
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_world_leave_critical_v1();
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_world_leave_critical_v1();
  CHECK(lang_gc_world_can_collect_v1());

  int child_ok = 0;
  CHECK(read_all_fd(pipe_fds[0], &child_ok, sizeof(child_ok)));
  close_fd(pipe_fds[0]);
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  CHECK(child_ok == 1);

  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void test_fork_from_critical_during_pending_stop() {
  std::atomic<bool> collector_ready{false};
  std::atomic<bool> start_stop{false};
  std::atomic<bool> stop_ok{false};
  std::atomic<bool> resume_now{false};
  std::atomic<bool> forker_in_critical{false};
  std::atomic<bool> fork_finished{false};
  std::atomic<bool> child_ok{false};
  std::atomic<bool> forker_left{false};

  std::thread collector([&] {
    CHECK(lang_gc_world_register_v1());
    collector_ready.store(true, std::memory_order_release);
    while (!start_stop.load(std::memory_order_acquire))
      sleep_ms(1);
    CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
    stop_ok.store(true, std::memory_order_release);
    while (!resume_now.load(std::memory_order_acquire))
      sleep_ms(1);
    lang_gc_world_resume_v1();
    CHECK(lang_gc_world_unregister_v1());
  });

  std::thread forker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_critical_v1();
    forker_in_critical.store(true, std::memory_order_release);
    while (!lang_gc_world_poll_required_v1())
      std::this_thread::yield();

    int pipe_fds[2] = {-1, -1};
    CHECK(::pipe(pipe_fds) == 0);
    // Critical is held before the allocator fork lock. The stop owned by the
    // collector above is still pending, which is exactly the allowed case.
    lang_alloc_fork_prepare_v1();
    lang_gc_world_fork_prepare_v1();
    const pid_t pid = ::fork();
    CHECK(pid >= 0);

    if (pid == 0) {
      close_fd(pipe_fds[0]);
      lang_alloc_fork_child_v1();
      lang_gc_world_fork_child_v1();

      int ok = 1;
      ok = ok && !lang_gc_world_register_v1();
      ok = ok && !lang_gc_world_poll_required_v1(); // child cleared the stop
      ok = ok && !lang_gc_world_can_collect_v1();    // still critical
      lang_gc_world_leave_critical_v1();             // preserved depth 1 -> 0
      ok = ok && lang_gc_world_can_collect_v1();
      ok = ok && (lang_gc_world_stop_v1(0) == 0);
      lang_gc_world_resume_v1();
      ok = ok && lang_gc_world_unregister_v1();
      ok = ok && (gc_test_alloc_live() == 0);
      (void)write_all_fd(pipe_fds[1], &ok, sizeof(ok));
      ::close(pipe_fds[1]);
      ::_exit(ok ? 0 : 1);
    }

    lang_gc_world_fork_parent_v1();
    lang_alloc_fork_parent_v1();
    close_fd(pipe_fds[1]);

    int ok = 0;
    CHECK(read_all_fd(pipe_fds[0], &ok, sizeof(ok)));
    close_fd(pipe_fds[0]);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    child_ok.store(ok == 1, std::memory_order_release);
    fork_finished.store(true, std::memory_order_release);

    // This is the pending stop's safepoint after the fork snapshot.
    lang_gc_world_leave_critical_v1();
    forker_left.store(true, std::memory_order_release);
    CHECK(lang_gc_world_unregister_v1());
  });

  CHECK(wait_for([&] { return collector_ready.load(); }, 5000));
  CHECK(wait_for([&] { return forker_in_critical.load(); }, 5000));
  start_stop.store(true, std::memory_order_release);

  CHECK(wait_for([&] { return fork_finished.load(); }, 5000));
  CHECK(child_ok.load());
  CHECK(wait_for([&] { return stop_ok.load(); }, 5000));
  sleep_ms(50);
  CHECK(!forker_left.load()); // forker is parked inside leave_critical

  resume_now.store(true, std::memory_order_release);
  CHECK(wait_for([&] { return forker_left.load(); }, 5000));
  collector.join();
  forker.join();
  expect_live(0);
}

void test_collector_fork_during_stop_clears_child_world() {
  CHECK(lang_gc_world_register_v1());
  MutatorGroup group;
  group.start(1);
  sleep_ms(20);

  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);

  int pipe_fds[2] = {-1, -1};
  CHECK(::pipe(pipe_fds) == 0);
  lang_alloc_fork_prepare_v1();
  lang_gc_world_fork_prepare_v1();
  const pid_t pid = ::fork();
  CHECK(pid >= 0);

  if (pid == 0) {
    close_fd(pipe_fds[0]);
    lang_alloc_fork_child_v1();
    lang_gc_world_fork_child_v1();

    ChildReport report{};
    report.magic = kReportMagic;
    report.register_was_new = lang_gc_world_register_v1() ? 1 : 0;
    report.poll_pending_after_fork = lang_gc_world_poll_required_v1() ? 1 : 0;
    report.stop_result = lang_gc_world_stop_v1(0);
    report.second_stop_result = lang_gc_world_stop_v1(0);
    if (report.second_stop_result == 0)
      report.second_stop_result = -9999;
    else
      lang_gc_world_resume_v1();
    report.unregister_removed = lang_gc_world_unregister_v1() ? 1 : 0;
    report.live_after = gc_test_alloc_live();
    report.ok = report.register_was_new == 0 &&
                report.poll_pending_after_fork == 0 &&
                report.stop_result == 0 &&
                report.second_stop_result == -16 &&
                report.unregister_removed == 1 && report.live_after == 0;
    (void)write_all_fd(pipe_fds[1], &report, sizeof(report));
    ::close(pipe_fds[1]);
    ::_exit(report.ok ? 0 : 1);
  }

  lang_gc_world_fork_parent_v1();
  lang_alloc_fork_parent_v1();
  close_fd(pipe_fds[1]);

  ChildReport report{};
  CHECK(read_all_fd(pipe_fds[0], &report, sizeof(report)));
  close_fd(pipe_fds[0]);
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  CHECK(report.magic == kReportMagic);
  CHECK(report.ok == 1);

  // The parent's stop survived the fork and still owns the world.
  CHECK(lang_gc_world_stop_v1(0) == -16);
  lang_gc_world_resume_v1();

  const uint64_t before = g_counter.load(std::memory_order_relaxed);
  CHECK(wait_for([before] {
          return g_counter.load(std::memory_order_relaxed) > before;
        },
        2000));
  group.finish();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
}

void benchmark_critical_pair() {
  CHECK(lang_gc_world_register_v1());
  for (int iteration = 0; iteration < 1000; ++iteration) {
    lang_gc_world_enter_critical_v1();
    lang_gc_world_leave_critical_v1();
  }
  constexpr int kPairs = 100000;
  const auto start = Clock::now();
  for (int iteration = 0; iteration < kPairs; ++iteration) {
    lang_gc_world_enter_critical_v1();
    lang_gc_world_leave_critical_v1();
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           Clock::now() - start)
                           .count();
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_WORLD_CRITICAL_PAIR_NS=%lld\n",
              static_cast<long long>(elapsed / kPairs));
}

void test_unregistered_idle_poll_is_fast_path() {
  // The no-request path cannot perform a tid lookup by design. An unregistered
  // thread is caught only when a request makes it enter the slow path; that
  // case is exercised by gc_world_misuse_test.
  lang_gc_world_poll_v1();
  CHECK(!lang_gc_world_poll_required_v1());
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  test_unregistered_idle_poll_is_fast_path();
  test_basic_stop_and_resume();
  test_critical_poll_defers_stop_and_nested_leave();
  test_critical_stop_busy_and_can_collect();
  test_can_collect_false_during_other_stop();
  test_critical_long_region_stop_timeout();
  test_safepoint_request_mirror_follows_stop();
  test_safepoint_mirror_alone_parks_on_stop();
  test_safe_thread_does_not_block_stop_and_leave_waits();
  test_nested_safe();
  test_registration_during_stop_waits();
  test_registration_wait_wakes_on_timeout();
  test_unregister_active_lets_stop_proceed();
  test_unregister_races_stop();
  test_timeout_retracts_and_wakes_parked_threads();
  test_monotonic_deadline_survives_eintr();
  test_collector_competition_and_self_reentry();
  test_same_tid_register_unregister_cycles();
  test_thread_exit_cycles();
  test_fork_prunes_inherited_registry();
  test_fork_from_safe_preserves_depth();
  test_fork_from_critical_preserves_nested_depth();
  test_fork_from_critical_during_pending_stop();
  test_collector_fork_during_stop_clears_child_world();
  test_hundreds_of_stop_resume_rounds();
  benchmark_critical_pair();

  expect_live(0);
  std::printf("GC_WORLD_HOST_OK allocations=%llu\n",
              static_cast<unsigned long long>(gc_test_alloc_total()));
  return 0;
}
