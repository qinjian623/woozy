// Hosted tests for the private foreign-callback enter/leave API. These tests
// use real graph payloads, real world stops and real external pthreads; they
// do not replace the accepted 42-check world suite.

#include "alloc.hpp"
#include "gc_world.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

extern "C" uint64_t gc_test_alloc_live() noexcept;
extern "C" uint64_t gc_test_alloc_total() noexcept;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int64_t kStopUnset = -9223372036854775807LL - 1;

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

struct GraphPayload {
  uint64_t head;
  uint64_t tail;
  uint64_t checksum;
  uint64_t writes;
};

void write_payload(GraphPayload *payload, uint64_t seed) {
  payload->head = seed;
  payload->tail = seed ^ 0x5555555555555555ULL;
  payload->checksum = payload->head + payload->tail;
  payload->writes += 1;
}

void check_payload(const GraphPayload *payload, uint64_t writes = 1) {
  CHECK(payload->writes == writes);
  CHECK(payload->head != 0);
  CHECK(payload->checksum == payload->head + payload->tail);
}

void expect_live(uint64_t expected) {
  CHECK(gc_test_alloc_live() == expected);
}

void test_external_pthread_entry_cleanup() {
  expect_live(0);
  GraphPayload payload{};
  std::atomic<bool> done{false};
  std::thread worker([&] {
    LangGcWorldCallbackToken token{};
    lang_gc_world_callback_enter_v1(&token);
    CHECK(token.created_registration == 1);
    CHECK(token.saved_safe_depth == 0);
    // The callback entry already registered this thread; a nested register
    // must not create a second registration.
    CHECK(!lang_gc_world_register_v1());
    write_payload(&payload, 0x1111);
    CHECK(lang_gc_world_can_collect_v1());
    // A callback is not allowed to become the collector.
    CHECK(lang_gc_world_stop_v1(0) == -16);
    CHECK(!lang_gc_world_poll_required_v1());
    lang_gc_world_callback_leave_v1(&token);
    // This scope created the registration; leave removed it.
    CHECK(!lang_gc_world_can_collect_v1());
    CHECK(lang_gc_world_register_v1());
    CHECK(lang_gc_world_unregister_v1());
    done.store(true, std::memory_order_release);
  });
  worker.join();
  CHECK(done.load(std::memory_order_acquire));
  check_payload(&payload);
  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_EXTERNAL_OK\n");
}

void test_existing_active_callback() {
  CHECK(lang_gc_world_register_v1());
  GraphPayload payload{};
  LangGcWorldCallbackToken token{};
  lang_gc_world_callback_enter_v1(&token);
  CHECK(token.created_registration == 0);
  CHECK(token.saved_safe_depth == 0);
  write_payload(&payload, 0x1a1a);
  CHECK(lang_gc_world_can_collect_v1());
  CHECK(lang_gc_world_stop_v1(0) == -16);
  lang_gc_world_callback_leave_v1(&token);
  CHECK(lang_gc_world_can_collect_v1());
  CHECK(lang_gc_world_unregister_v1());
  check_payload(&payload);
  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_ACTIVE_OK\n");
}

void test_existing_active_and_safe_depth_restore() {
  CHECK(lang_gc_world_register_v1());
  lang_gc_world_enter_safe_v1();
  lang_gc_world_enter_safe_v1();
  lang_gc_world_enter_safe_v1();

  GraphPayload payload{};
  LangGcWorldCallbackToken token{};
  lang_gc_world_callback_enter_v1(&token);
  CHECK(token.created_registration == 0);
  CHECK(token.saved_safe_depth == 3);
  write_payload(&payload, 0x2222);
  CHECK(lang_gc_world_can_collect_v1());
  CHECK(lang_gc_world_stop_v1(0) == -16);
  CHECK(!lang_gc_world_poll_required_v1());
  lang_gc_world_callback_leave_v1(&token);

  // Exact N=3 SAFE depth was restored, not collapsed to 0 or 1.
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_world_leave_safe_v1();
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_world_leave_safe_v1();
  CHECK(!lang_gc_world_can_collect_v1());
  lang_gc_world_leave_safe_v1();
  CHECK(lang_gc_world_can_collect_v1());
  check_payload(&payload);

  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_SAFE3_OK\n");
}

void test_nested_callback_and_c_safe_scopes() {
  CHECK(lang_gc_world_register_v1());
  lang_gc_world_enter_safe_v1();
  lang_gc_world_enter_safe_v1();
  lang_gc_world_enter_safe_v1();

  GraphPayload payload{};
  LangGcWorldCallbackToken outer{};
  lang_gc_world_callback_enter_v1(&outer);
  CHECK(outer.created_registration == 0);
  CHECK(outer.saved_safe_depth == 3);
  write_payload(&payload, 0x3333);

  // Enter C SAFE work inside the outer callback, then re-enter Wz.
  lang_gc_world_enter_safe_v1();
  LangGcWorldCallbackToken inner_safe{};
  lang_gc_world_callback_enter_v1(&inner_safe);
  CHECK(inner_safe.saved_safe_depth == 1);
  payload.tail ^= 0x1234;
  payload.checksum = payload.head + payload.tail;
  payload.writes += 1;
  lang_gc_world_callback_leave_v1(&inner_safe);
  CHECK(!lang_gc_world_can_collect_v1()); // restored SAFE depth 1
  lang_gc_world_leave_safe_v1();
  CHECK(lang_gc_world_can_collect_v1()); // outer callback active again

  // A second callback nested directly under the active outer callback. Its
  // leave must not park or reset the outer callback's active work.
  LangGcWorldCallbackToken inner_active{};
  lang_gc_world_callback_enter_v1(&inner_active);
  CHECK(inner_active.saved_safe_depth == 0);
  payload.head ^= 0x5678;
  payload.checksum = payload.head + payload.tail;
  payload.writes += 1;
  lang_gc_world_callback_leave_v1(&inner_active);
  CHECK(lang_gc_world_can_collect_v1());

  lang_gc_world_callback_leave_v1(&outer);
  CHECK(!lang_gc_world_can_collect_v1()); // exact outer SAFE depth 3 restored
  lang_gc_world_leave_safe_v1();
  lang_gc_world_leave_safe_v1();
  lang_gc_world_leave_safe_v1();
  CHECK(lang_gc_world_can_collect_v1());

  CHECK(payload.writes == 3);
  CHECK(payload.checksum == payload.head + payload.tail);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_NESTED_OK\n");
}

struct Pair {
  uint64_t a;
  uint64_t b;
  uint64_t sum;
};

void test_register_inside_active_callback_does_not_park() {
  CHECK(lang_gc_world_register_v1());
  Pair pair{1, 2, 3};
  std::atomic<bool> half{false};
  std::atomic<bool> done{false};

  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1()); // existing Wz thread, not created by enter
    LangGcWorldCallbackToken token{};
    lang_gc_world_callback_enter_v1(&token);
    pair.a = 10; // half of the two-field update
    half.store(true, std::memory_order_release);
    while (!lang_gc_world_poll_required_v1())
      std::this_thread::yield();
    // Idempotent re-register is not a callback safepoint. With an ACTIVE
    // callback it must return false without parking mid-update.
    CHECK(!lang_gc_world_register_v1());
    pair.b = 20;
    pair.sum = pair.a + pair.b;
    // The final callback leave is the safepoint that lets the collector stop.
    lang_gc_world_callback_leave_v1(&token);
    CHECK(lang_gc_world_unregister_v1());
    done.store(true, std::memory_order_release);
  });

  CHECK(wait_for([&] { return half.load(std::memory_order_acquire); }, 5000));
  const int64_t stop_result = lang_gc_world_stop_v1(2000000000LL);
  if (stop_result == 0) {
    // The worker is parked after completing the pair, so this read is safe.
    CHECK(pair.a == 10);
    CHECK(pair.b == 20);
    CHECK(pair.sum == pair.a + pair.b);
    lang_gc_world_resume_v1();
  } else {
    // Never inspect the plain pair while the worker may still be writing.
    CHECK(stop_result == -110);
  }
  CHECK(wait_for([&] { return done.load(std::memory_order_acquire); }, 5000));
  worker.join();
  // After join the update is published regardless of stop timeout.
  CHECK(pair.a == 10);
  CHECK(pair.b == 20);
  CHECK(pair.sum == pair.a + pair.b);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_REGISTER_ACTIVE_OK stop=%lld\n",
              static_cast<long long>(stop_result));
}

void test_pending_stop_safe_worker_callback_waits() {
  CHECK(lang_gc_world_register_v1());
  GraphPayload payload{};
  std::atomic<bool> safe_ready{false};
  std::atomic<bool> try_callback{false};
  std::atomic<bool> attempting{false};
  std::atomic<bool> returned{false};
  std::atomic<bool> worker_done{false};

  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_safe_v1();
    safe_ready.store(true, std::memory_order_release);
    while (!try_callback.load(std::memory_order_acquire))
      sleep_ms(1);
    LangGcWorldCallbackToken token{};
    attempting.store(true, std::memory_order_release);
    lang_gc_world_callback_enter_v1(&token);
    returned.store(true, std::memory_order_release);
    write_payload(&payload, 0x4444);
    lang_gc_world_callback_leave_v1(&token);
    lang_gc_world_leave_safe_v1();
    CHECK(lang_gc_world_unregister_v1());
    worker_done.store(true, std::memory_order_release);
  });

  CHECK(wait_for([&] { return safe_ready.load(); }, 5000));
  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  try_callback.store(true, std::memory_order_release);
  CHECK(wait_for([&] { return attempting.load(); }, 2000));
  sleep_ms(50);
  CHECK(!returned.load(std::memory_order_acquire));
  CHECK(payload.writes == 0); // no Wz graph access while stop is held
  CHECK(lang_gc_world_poll_required_v1() != 0);

  lang_gc_world_resume_v1();
  CHECK(wait_for([&] { return returned.load() && worker_done.load(); }, 5000));
  worker.join();
  check_payload(&payload);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_PENDING_OK\n");
}

void test_pending_stop_active_worker_callback_parks() {
  CHECK(lang_gc_world_register_v1());
  GraphPayload payload{};
  std::atomic<bool> worker_ready{false};
  std::atomic<bool> go{false};
  std::atomic<bool> attempting{false};
  std::atomic<bool> returned{false};
  std::atomic<bool> done{false};

  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1());
    worker_ready.store(true, std::memory_order_release);
    while (!go.load(std::memory_order_acquire))
      sleep_ms(1);
    while (!lang_gc_world_poll_required_v1())
      std::this_thread::yield();
    LangGcWorldCallbackToken token{};
    attempting.store(true, std::memory_order_release);
    lang_gc_world_callback_enter_v1(&token);
    returned.store(true, std::memory_order_release);
    write_payload(&payload, 0x4a4a);
    lang_gc_world_callback_leave_v1(&token);
    CHECK(lang_gc_world_unregister_v1());
    done.store(true, std::memory_order_release);
  });

  CHECK(wait_for([&] { return worker_ready.load(); }, 5000));
  go.store(true, std::memory_order_release);
  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  CHECK(wait_for([&] { return attempting.load(); }, 2000));
  sleep_ms(50);
  CHECK(!returned.load(std::memory_order_acquire));
  CHECK(payload.writes == 0);
  CHECK(lang_gc_world_poll_required_v1() != 0);
  lang_gc_world_resume_v1();
  CHECK(wait_for([&] { return returned.load() && done.load(); }, 5000));
  worker.join();
  check_payload(&payload);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_ACTIVE_PENDING_OK\n");
}

void test_active_callback_not_hidden_by_safe() {
  CHECK(lang_gc_world_register_v1());
  GraphPayload payload{};
  std::atomic<bool> in_callback{false};
  std::atomic<bool> release_leave{false};
  std::atomic<bool> done{false};

  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_safe_v1();
    LangGcWorldCallbackToken token{};
    lang_gc_world_callback_enter_v1(&token);
    write_payload(&payload, 0x4b4b);
    in_callback.store(true, std::memory_order_release);
    while (!release_leave.load(std::memory_order_acquire))
      sleep_ms(1);
    lang_gc_world_callback_leave_v1(&token);
    lang_gc_world_leave_safe_v1(); // parked while the stop request is active
    CHECK(lang_gc_world_unregister_v1());
    done.store(true, std::memory_order_release);
  });
  std::thread releaser([&] {
    CHECK(wait_for([&] { return in_callback.load(); }, 5000));
    sleep_ms(60);
    release_leave.store(true, std::memory_order_release);
  });

  CHECK(wait_for([&] { return in_callback.load(); }, 5000));
  const auto start = Clock::now();
  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - start)
                           .count();
  CHECK(elapsed >= 40); // callback was active, not hidden by its prior SAFE
  CHECK(lang_gc_world_poll_required_v1() != 0);
  lang_gc_world_resume_v1();
  CHECK(wait_for([&] { return done.load(); }, 5000));
  worker.join();
  releaser.join();
  check_payload(&payload);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_ACTIVE_STOP_OK\n");
}

void test_external_pending_stop_waits() {
  CHECK(lang_gc_world_register_v1());
  GraphPayload payload{};
  std::atomic<bool> blocker_ready{false};
  std::atomic<bool> release_blocker{false};
  std::atomic<bool> try_callback{false};
  std::atomic<bool> attempting{false};
  std::atomic<bool> returned{false};
  std::atomic<bool> done{false};

  std::thread blocker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_safe_v1();
    blocker_ready.store(true, std::memory_order_release);
    while (!release_blocker.load(std::memory_order_acquire))
      sleep_ms(1);
    lang_gc_world_leave_safe_v1();
    CHECK(lang_gc_world_unregister_v1());
  });
  std::thread external([&] {
    while (!try_callback.load(std::memory_order_acquire))
      sleep_ms(1);
    LangGcWorldCallbackToken token{};
    attempting.store(true, std::memory_order_release);
    // Unregistered: callback enter registers this pthread and parks it until
    // the already-successful stop is resumed.
    lang_gc_world_callback_enter_v1(&token);
    CHECK(token.created_registration == 1);
    returned.store(true, std::memory_order_release);
    write_payload(&payload, 0x4c4c);
    lang_gc_world_callback_leave_v1(&token);
    CHECK(!lang_gc_world_can_collect_v1());
    CHECK(lang_gc_world_register_v1());
    CHECK(lang_gc_world_unregister_v1());
    done.store(true, std::memory_order_release);
  });

  CHECK(wait_for([&] { return blocker_ready.load(); }, 5000));
  CHECK(lang_gc_world_stop_v1(2000000000LL) == 0);
  try_callback.store(true, std::memory_order_release);
  CHECK(wait_for([&] { return attempting.load(); }, 2000));
  sleep_ms(50);
  CHECK(!returned.load(std::memory_order_acquire));
  CHECK(payload.writes == 0);
  CHECK(lang_gc_world_poll_required_v1() != 0);
  lang_gc_world_resume_v1();
  CHECK(wait_for([&] { return returned.load() && done.load(); }, 5000));
  external.join();
  release_blocker.store(true, std::memory_order_release);
  blocker.join();
  check_payload(&payload);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_EXTERNAL_PENDING_OK\n");
}

void test_timeout_withdrawal_callback_resumes() {
  GraphPayload payload{};
  std::atomic<bool> collector_ready{false};
  std::atomic<bool> blocker_ready{false};
  std::atomic<bool> safe_ready{false};
  std::atomic<bool> start{false};
  std::atomic<bool> attempting{false};
  std::atomic<bool> returned{false};
  std::atomic<bool> worker_done{false};
  std::atomic<bool> release_blocker{false};
  std::atomic<int64_t> stop_result{kStopUnset};

  std::thread collector([&] {
    CHECK(lang_gc_world_register_v1());
    collector_ready.store(true, std::memory_order_release);
    while (!start.load(std::memory_order_acquire))
      sleep_ms(1);
    stop_result.store(lang_gc_world_stop_v1(500000000LL),
                      std::memory_order_release);
    CHECK(lang_gc_world_unregister_v1());
  });
  std::thread blocker([&] {
    CHECK(lang_gc_world_register_v1());
    blocker_ready.store(true, std::memory_order_release);
    while (!release_blocker.load(std::memory_order_acquire))
      sleep_ms(1);
    CHECK(lang_gc_world_unregister_v1());
  });
  std::thread worker([&] {
    CHECK(lang_gc_world_register_v1());
    lang_gc_world_enter_safe_v1();
    safe_ready.store(true, std::memory_order_release);
    while (!lang_gc_world_poll_required_v1())
      std::this_thread::yield();
    LangGcWorldCallbackToken token{};
    attempting.store(true, std::memory_order_release);
    lang_gc_world_callback_enter_v1(&token);
    returned.store(true, std::memory_order_release);
    write_payload(&payload, 0x5555);
    lang_gc_world_callback_leave_v1(&token);
    lang_gc_world_leave_safe_v1();
    CHECK(lang_gc_world_unregister_v1());
    worker_done.store(true, std::memory_order_release);
  });

  CHECK(wait_for([&] {
          return collector_ready.load() && blocker_ready.load() &&
                 safe_ready.load();
        },
        5000));
  start.store(true, std::memory_order_release);
  CHECK(wait_for([&] { return attempting.load(); }, 2000));
  sleep_ms(30);
  CHECK(!returned.load(std::memory_order_acquire));
  CHECK(payload.writes == 0);
  CHECK(wait_for([&] {
          return stop_result.load(std::memory_order_acquire) != kStopUnset;
        },
        5000));
  CHECK(stop_result.load(std::memory_order_acquire) == -110);
  CHECK(wait_for([&] { return returned.load() && worker_done.load(); }, 5000));
  release_blocker.store(true, std::memory_order_release);
  collector.join();
  blocker.join();
  worker.join();
  check_payload(&payload);
  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_TIMEOUT_OK\n");
}

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

void test_fork_preserves_callback_tokens() {
  CHECK(lang_gc_world_register_v1());
  int pipe_fds[2] = {-1, -1};
  CHECK(::pipe(pipe_fds) == 0);

  GraphPayload payload{};
  LangGcWorldCallbackToken outer{};
  LangGcWorldCallbackToken inner{};
  lang_gc_world_callback_enter_v1(&outer);
  lang_gc_world_callback_enter_v1(&inner);
  write_payload(&payload, 0x6666);

  lang_alloc_fork_prepare_v1();
  lang_gc_world_fork_prepare_v1();
  const pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    ::close(pipe_fds[0]);
    lang_alloc_fork_child_v1();
    lang_gc_world_fork_child_v1();
    int ok = 1;
    lang_gc_world_callback_leave_v1(&inner);
    lang_gc_world_callback_leave_v1(&outer);
    ok = ok && lang_gc_world_can_collect_v1();
    ok = ok && lang_gc_world_unregister_v1();
    ok = ok && (gc_test_alloc_live() == 0);
    (void)write_all_fd(pipe_fds[1], &ok, sizeof(ok));
    ::close(pipe_fds[1]);
    ::_exit(ok ? 0 : 1);
  }

  lang_gc_world_fork_parent_v1();
  lang_alloc_fork_parent_v1();
  ::close(pipe_fds[1]);
  lang_gc_world_callback_leave_v1(&inner);
  lang_gc_world_callback_leave_v1(&outer);
  CHECK(lang_gc_world_can_collect_v1());

  int child_ok = 0;
  CHECK(read_all_fd(pipe_fds[0], &child_ok, sizeof(child_ok)));
  ::close(pipe_fds[0]);
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
  CHECK(child_ok == 1);
  check_payload(&payload);
  CHECK(lang_gc_world_unregister_v1());
  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_FORK_OK\n");
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);

  test_external_pthread_entry_cleanup();
  test_existing_active_callback();
  test_existing_active_and_safe_depth_restore();
  test_nested_callback_and_c_safe_scopes();
  test_register_inside_active_callback_does_not_park();
  test_pending_stop_safe_worker_callback_waits();
  test_pending_stop_active_worker_callback_parks();
  test_active_callback_not_hidden_by_safe();
  test_external_pending_stop_waits();
  test_timeout_withdrawal_callback_resumes();
  test_fork_preserves_callback_tokens();

  expect_live(0);
  std::printf("GC_WORLD_CALLBACK_HOST_OK allocations=%llu\n",
              static_cast<unsigned long long>(gc_test_alloc_total()));
  return 0;
}
