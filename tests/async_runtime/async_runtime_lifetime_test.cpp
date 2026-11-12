// Focused runtime-lifetime tests for runtime/native/async.cpp (lang-async/1).
//
// These tests link the real executor translation unit into the test binary and
// drive it through the same entry points the compiler emits. A tiny fake
// coroutine models the generated code: resume() calls `lang_async_await_v1`
// and returns (suspends) while the target is pending, exactly like the
// emitter's await check/suspend/re-check sequence.
//
// The file covers the two independent ASan counterexamples from the external
// review plus the lifecycle matrix: queued, never spawned, completed,
// cancelled, shutdown, multiple waiters, repeated cancel/release pairs and
// timer/descriptor registration release. After every scenario the executor
// invariants are checked: the registry holds only pending futures, waiter
// links are consistent, the ready queue holds only pending futures, and the
// pending-task counter matches the registry.
//
// LeakSanitizer does not report leaks on this host, so the allocation stub
// counts live blocks itself and every scenario asserts that all futures and
// frames it created were released.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Freestanding runtime stubs: tracked allocator, terminal panic.
// ---------------------------------------------------------------------------

namespace {

constexpr uint64_t kLiveMagic = 0x4c49564546555431ull; // "LIVEFUT1"
constexpr uint64_t kDeadMagic = 0x4445414446555431ull; // "DEADFUT1"

struct BlockHeader {
  uint64_t magic;
  uint64_t bytes;
  uint64_t align;
  void *raw;
};

std::atomic<uint64_t> g_live_blocks{0};
std::atomic<uint64_t> g_live_bytes{0};
std::atomic<uint64_t> g_allocations{0};
std::atomic<uint64_t> g_bad_frees{0};

} // namespace

extern "C" void *lang_alloc_v1(uint64_t size, uint64_t align) noexcept {
  if (align < alignof(BlockHeader))
    align = alignof(BlockHeader);
  if (align > 4096)
    return nullptr;
  const uint64_t total = size + sizeof(BlockHeader) + align;
  void *raw = std::malloc(static_cast<size_t>(total));
  if (!raw)
    return nullptr;
  const uintptr_t base =
      reinterpret_cast<uintptr_t>(raw) + sizeof(BlockHeader);
  const uintptr_t aligned = (base + (align - 1)) & ~(align - 1);
  auto *header = reinterpret_cast<BlockHeader *>(aligned - sizeof(BlockHeader));
  header->magic = kLiveMagic;
  header->bytes = size;
  header->align = align;
  header->raw = raw;
  ++g_live_blocks;
  g_live_bytes += size;
  ++g_allocations;
  return reinterpret_cast<void *>(aligned);
}

extern "C" void lang_free_v1(void *pointer) noexcept {
  if (!pointer)
    return;
  auto *header = reinterpret_cast<BlockHeader *>(
      reinterpret_cast<uintptr_t>(pointer) - sizeof(BlockHeader));
  if (header->magic != kLiveMagic) {
    ++g_bad_frees;
    std::fprintf(stderr, "async_runtime: double free or corrupt block\n");
    std::abort();
  }
  header->magic = kDeadMagic;
  --g_live_blocks;
  g_live_bytes -= header->bytes;
  void *raw = header->raw;
  std::memset(header, 0, sizeof(BlockHeader));
  std::free(raw);
}

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t cause, uint64_t,
                                                  uint64_t,
                                                  uint64_t) noexcept {
  std::fprintf(stderr, "async_runtime: runtime Panic cause=%u\n", cause);
  std::abort();
}

#include "../../runtime/native/async.cpp"

// ---------------------------------------------------------------------------
// Failure reporting and invariant checks.
// ---------------------------------------------------------------------------

namespace {

int g_failures = 0;
const char *g_scenario = "";

void expect(bool condition, const char *what) noexcept {
  if (!condition) {
    std::fprintf(stderr, "  FAIL [%s]: %s\n", g_scenario, what);
    ++g_failures;
  }
}

void expect_equal(uint64_t actual, uint64_t expected, const char *what) noexcept {
  if (actual != expected) {
    std::fprintf(stderr, "  FAIL [%s]: %s (got %llu, want %llu)\n", g_scenario,
                 what, static_cast<unsigned long long>(actual),
                 static_cast<unsigned long long>(expected));
    ++g_failures;
  }
}

// Registry/queue/link invariants of the executor.
void check_executor_invariants(const char *where) noexcept {
  uint32_t pending_tasks = 0;
  for (Future *f = g_all; f; f = f->next_all) {
    if (f->status != kPending) {
      std::fprintf(stderr, "  FAIL [%s]: completed future in registry (%s)\n",
                   g_scenario, where);
      ++g_failures;
    }
    if (f->refs == 0) {
      std::fprintf(stderr, "  FAIL [%s]: registered future with zero refs (%s)\n",
                   g_scenario, where);
      ++g_failures;
    }
    if (f->kind == kKindTask)
      ++pending_tasks;
    for (Future *other = f->next_all; other; other = other->next_all) {
      if (other == f) {
        std::fprintf(stderr, "  FAIL [%s]: duplicate registry node (%s)\n",
                     g_scenario, where);
        ++g_failures;
        break;
      }
    }
    for (Future *w = f->waiters; w; w = w->next_waiter) {
      if (w->waiting_on != f) {
        std::fprintf(stderr,
                     "  FAIL [%s]: waiter link does not point at its target (%s)\n",
                     g_scenario, where);
        ++g_failures;
      }
      if (w->status != kPending) {
        std::fprintf(stderr, "  FAIL [%s]: completed frame in waiters list (%s)\n",
                     g_scenario, where);
        ++g_failures;
      }
      if (w->refs == 0) {
        std::fprintf(stderr, "  FAIL [%s]: waiter with zero refs (%s)\n",
                     g_scenario, where);
        ++g_failures;
      }
    }
    if (f->waiting_on) {
      bool listed = false;
      for (Future *w = f->waiting_on->waiters; w; w = w->next_waiter)
        if (w == f)
          listed = true;
      if (!listed) {
        std::fprintf(stderr,
                     "  FAIL [%s]: waiting_on target does not list the waiter (%s)\n",
                     g_scenario, where);
        ++g_failures;
      }
      if (!(f->flags & kParentRef)) {
        std::fprintf(stderr, "  FAIL [%s]: link without kParentRef (%s)\n",
                     g_scenario, where);
        ++g_failures;
      }
    } else if (f->flags & kParentRef) {
      std::fprintf(stderr, "  FAIL [%s]: kParentRef without waiting_on (%s)\n",
                   g_scenario, where);
      ++g_failures;
    }
  }
  if (pending_tasks != g_live_tasks) {
    std::fprintf(stderr,
                 "  FAIL [%s]: pending-task counter %u != registry %u (%s)\n",
                 g_scenario, pending_tasks, g_live_tasks, where);
    ++g_failures;
  }
  uint64_t queued_nodes = 0;
  Future *last = nullptr;
  for (Future *f = g_ready_head; f; f = f->next_ready) {
    ++queued_nodes;
    last = f;
    if (!(f->flags & kQueued)) {
      std::fprintf(stderr, "  FAIL [%s]: queued node without kQueued (%s)\n",
                   g_scenario, where);
      ++g_failures;
    }
    if (f->status != kPending) {
      std::fprintf(stderr, "  FAIL [%s]: completed future in ready queue (%s)\n",
                   g_scenario, where);
      ++g_failures;
    }
    for (Future *other = f->next_ready; other; other = other->next_ready) {
      if (other == f) {
        std::fprintf(stderr, "  FAIL [%s]: duplicate ready node (%s)\n",
                     g_scenario, where);
        ++g_failures;
        break;
      }
    }
  }
  if (last != g_ready_tail) {
    std::fprintf(stderr, "  FAIL [%s]: ready tail mismatch (%s)\n", g_scenario,
                 where);
    ++g_failures;
  }
  uint64_t flagged = 0;
  for (Future *f = g_all; f; f = f->next_all)
    if (f->flags & kQueued)
      ++flagged;
  if (flagged != queued_nodes) {
    std::fprintf(stderr,
                 "  FAIL [%s]: %llu kQueued nodes but %llu queued (%s)\n",
                 g_scenario, static_cast<unsigned long long>(flagged),
                 static_cast<unsigned long long>(queued_nodes), where);
    ++g_failures;
  }
}

void check_clean(const char *where) noexcept {
  check_executor_invariants(where);
  expect(g_all == nullptr, "registry empty");
  expect(g_live_tasks == 0, "no pending tasks");
  expect(g_ready_head == nullptr, "ready queue empty");
  expect(g_ready_tail == nullptr, "ready tail empty");
  expect(g_current == nullptr, "no current frame");
  expect(g_epoll == -1, "epoll closed");
  expect_equal(g_live_blocks.load(), 0, "no live future allocations");
  expect_equal(g_bad_frees.load(), 0, "no double frees");
}

// ---------------------------------------------------------------------------
// Fake generated coroutine.
// ---------------------------------------------------------------------------

struct FakePromise {
  uint32_t status;
  uint32_t claimed;
  int64_t value;
};

struct FakeFrame {
  FakePromise promise;
  Future *first;
  Future *second;
  int resume_count;
  int cleanup_count;
  int destroy_count;
  int promise_release_count;
  int stage;
};

void fake_resume(void *raw) noexcept {
  auto *frame = static_cast<FakeFrame *>(raw);
  ++frame->resume_count;
  Future *targets[2] = {frame->first, frame->second};
  while (frame->stage < 2) {
    Future *target = targets[frame->stage];
    if (!target) {
      ++frame->stage;
      continue;
    }
    // Mirrors the emitted cancel exit plan: the coroutine observes the
    // cooperative cancellation flag before it suspends or continues.
    if (lang_async_current_cancelled_v1()) {
      ++frame->cleanup_count;
      frame->promise.status = kCancelled;
      return;
    }
    const uint32_t status = lang_async_await_v1(target);
    if (status == kPending)
      return; // suspended at this await
    if (status == kCancelled) {
      ++frame->cleanup_count;
      frame->promise.status = kCancelled;
      return;
    }
    if (status == kError) {
      frame->promise.status = kError;
      return;
    }
    ++frame->stage;
  }
  frame->promise.status = kDone;
}

void fake_destroy(void *raw) noexcept {
  auto *frame = static_cast<FakeFrame *>(raw);
  ++frame->destroy_count;
}

void fake_release_promise(void *raw) noexcept {
  auto *frame = static_cast<FakeFrame *>(raw);
  ++frame->promise_release_count;
}

void init_frame(FakeFrame *frame, Future *first, Future *second) noexcept {
  std::memset(frame, 0, sizeof(*frame));
  frame->promise.status = kPending;
  frame->first = first;
  frame->second = second;
}

Future *make_task(FakeFrame *frame) noexcept {
  auto *future = static_cast<Future *>(lang_async_task_new_v1());
  frame->promise.status = kPending;
  lang_async_publish_v1(
      future, frame, reinterpret_cast<void *>(&fake_resume),
      reinterpret_cast<void *>(&fake_destroy), &frame->promise,
      reinterpret_cast<void *>(&fake_release_promise));
  return future;
}

Future *make_timer(int64_t milliseconds) noexcept {
  return static_cast<Future *>(lang_async_sleep_v1(milliseconds));
}

Future *make_task_frame(FakeFrame *frame, Future *first = nullptr,
                        Future *second = nullptr) noexcept {
  init_frame(frame, first, second);
  return make_task(frame);
}

} // namespace

// ---------------------------------------------------------------------------
// Scenarios.
// ---------------------------------------------------------------------------

namespace {

// Review counterexample 1 (asan-registry.log): a task is created and its only
// handle released before shutdown. destroy_future must remove the registry
// node, otherwise the shutdown scan reads freed storage.
void scenario_unstarted_release_then_shutdown() noexcept {
  g_scenario = "unstarted-release-then-shutdown";
  void *task = lang_async_task_new_v1();
  expect(g_all == static_cast<Future *>(task), "task registered");
  lang_async_task_release_v1(task);
  expect(g_all == nullptr, "release removes the registry node");
  check_executor_invariants("after release");
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// Review counterexample 2 (asan-cancel-waiter.log): a suspended waiter is
// cancelled, completed and released while a child still lists it. Completion
// must detach the frame from the child's waiter list before the frame dies.
void scenario_cancel_waiter_then_child_complete() noexcept {
  g_scenario = "cancel-waiter-then-child-complete";
  auto *task = static_cast<Future *>(lang_async_task_new_v1());
  auto *timer = make_timer(100);
  g_current = task;
  expect_equal(lang_async_await_v1(timer), kPending, "awaiter suspends");
  g_current = nullptr;
  expect(timer->waiters == task, "target lists the waiter");
  expect(task->waiting_on == timer, "waiter points at the target");
  task->flags |= kStarted | kSuspended;
  lang_async_cancel_v1(task);
  complete(task, kCancelled);
  expect(timer->waiters == nullptr, "completion detaches the waiter");
  expect(task->waiting_on == nullptr, "waiter link cleared");
  lang_async_task_release_v1(task);
  complete(timer, kDone);
  lang_async_task_release_v1(timer);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// Real suspension: a spawned task awaits a timer, is cancelled while
// suspended, then runs its cancel exit plan once.
void scenario_cancel_suspended_task() noexcept {
  g_scenario = "cancel-suspended-task";
  Future *stop = make_timer(1);
  Future *sleep = make_timer(60000);
  FakeFrame frame;
  Future *task = make_task_frame(&frame, sleep);
  lang_async_spawn_v1(task);
  lang_async_run_until_v1(stop);
  expect_equal(lang_async_status_v1(task), kPending, "task suspended");
  expect_equal(frame.resume_count, 1, "task resumed once");
  expect(task->waiting_on == sleep, "task waits on the sleep timer");
  lang_async_cancel_v1(task);
  lang_async_run_until_v1(task);
  expect_equal(lang_async_status_v1(task), kCancelled, "task cancelled");
  expect_equal(frame.cleanup_count, 1, "cancel exit plan ran once");
  expect_equal(frame.resume_count, 2, "cancel resumed the frame once");
  expect(sleep->waiters == nullptr, "sleep timer detached");
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(sleep);
  lang_async_task_release_v1(stop);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// A spawned task whose source handle is dropped while it is suspended must
// still be cancelled and released by shutdown (the executor reference keeps
// it alive). This is the "registry successor freed during the shutdown scan"
// case.
void scenario_drop_handle_while_suspended() noexcept {
  g_scenario = "drop-handle-while-suspended";
  Future *stop = make_timer(1);
  Future *sleep = make_timer(60000);
  FakeFrame frame;
  Future *task = make_task_frame(&frame, sleep);
  lang_async_spawn_v1(task);
  lang_async_run_until_v1(stop);
  expect_equal(frame.resume_count, 1, "task suspended");
  lang_async_task_release_v1(task); // source Task value dropped
  expect_equal(frame.destroy_count, 0, "executor reference keeps the frame");
  lang_async_shutdown_v1();
  expect_equal(frame.destroy_count, 1, "shutdown released the frame once");
  expect_equal(frame.cleanup_count, 1, "cancel exit plan ran once");
  lang_async_task_release_v1(sleep);
  lang_async_task_release_v1(stop);
  check_clean("after shutdown");
}

// A queued task that is cancelled before its first resume is completed
// without running the body, and a later shutdown must not see a stale ready
// queue node.
void scenario_cancel_queued_before_first_run() noexcept {
  g_scenario = "cancel-queued-before-first-run";
  FakeFrame frame;
  Future *task = make_task_frame(&frame);
  lang_async_spawn_v1(task);
  expect(task->flags & kQueued, "spawned task is queued");
  lang_async_cancel_v1(task);
  expect_equal(lang_async_status_v1(task), kCancelled, "queued cancel wins");
  expect((task->flags & kQueued) == 0, "cancel removed the queue node");
  lang_async_task_release_v1(task);
  expect_equal(frame.resume_count, 0, "body never ran");
  expect_equal(frame.destroy_count, 1, "frame destroyed once");
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// A spawned, queued task whose source handle is dropped keeps only the
// executor reference. Shutdown cancels and frees it while walking the
// registry; the scan must not use the freed node as traversal state.
void scenario_drop_queued_task_then_shutdown() noexcept {
  g_scenario = "drop-queued-task-then-shutdown";
  FakeFrame frame;
  Future *task = make_task_frame(&frame);
  lang_async_spawn_v1(task);
  lang_async_task_release_v1(task);
  expect(task->flags & kQueued || task->status != kPending,
         "task stays live through the executor reference");
  lang_async_shutdown_v1();
  expect_equal(frame.resume_count, 0, "cancelled before the first resume");
  expect_equal(frame.destroy_count, 1, "frame destroyed once");
  check_clean("after shutdown");
}

// Several frames awaiting one timer: every waiter is woken exactly once and
// the waiter list is fully detached.
void scenario_multiple_waiters() noexcept {
  g_scenario = "multiple-waiters";
  constexpr int kWaiterCount = 4;
  Future *timer = make_timer(1);
  FakeFrame frames[kWaiterCount];
  Future *tasks[kWaiterCount];
  for (int i = 0; i < kWaiterCount; ++i) {
    tasks[i] = make_task_frame(&frames[i], timer);
    lang_async_spawn_v1(tasks[i]);
  }
  // run_until(timer) stops as soon as the timer itself completed; the woken
  // frames stay queued and run on the next executor entry.
  lang_async_run_until_v1(timer);
  expect(timer->waiters == nullptr, "all waiters detached");
  lang_async_run_v1();
  for (int i = 0; i < kWaiterCount; ++i) {
    expect_equal(lang_async_status_v1(tasks[i]), kDone, "waiter completed");
    expect_equal(frames[i].resume_count, 2, "waiter resumed after wake");
    expect_equal(frames[i].cleanup_count, 0, "no cancel cleanup");
    expect_equal(frames[i].stage, 2, "both await stages passed");
  }
  for (int i = 0; i < kWaiterCount; ++i)
    lang_async_task_release_v1(tasks[i]);
  lang_async_task_release_v1(timer);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// Cancel one of several waiters: it leaves the list, the others still wake.
void scenario_multiple_waiters_cancel_one() noexcept {
  g_scenario = "multiple-waiters-cancel-one";
  Future *stop = make_timer(1);
  Future *timer = make_timer(60000);
  FakeFrame frames[3];
  Future *tasks[3];
  for (int i = 0; i < 3; ++i) {
    tasks[i] = make_task_frame(&frames[i], timer);
    lang_async_spawn_v1(tasks[i]);
  }
  lang_async_run_until_v1(stop);
  check_executor_invariants("three waiters registered");
  lang_async_cancel_v1(tasks[1]);
  lang_async_run_until_v1(tasks[1]);
  expect_equal(lang_async_status_v1(tasks[1]), kCancelled, "middle cancelled");
  expect_equal(frames[1].cleanup_count, 1, "middle ran its cleanup");
  expect_equal(frames[1].resume_count, 2, "middle resumed once for cleanup");
  lang_async_cancel_v1(timer);
  lang_async_run_until_v1(tasks[0]);
  expect_equal(lang_async_status_v1(tasks[0]), kCancelled, "first sees cancel");
  expect_equal(lang_async_status_v1(tasks[2]), kCancelled, "third sees cancel");
  for (int i = 0; i < 3; ++i) {
    expect_equal(frames[i].resume_count, 2, "each frame resumed exactly twice");
    lang_async_task_release_v1(tasks[i]);
  }
  lang_async_task_release_v1(timer);
  lang_async_task_release_v1(stop);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// Repeated cancel on the same handle is a no-op after the first completion,
// and the frame is destroyed exactly once.
void scenario_repeated_cancel_and_release() noexcept {
  g_scenario = "repeated-cancel-and-release";
  Future *stop = make_timer(1);
  Future *sleep = make_timer(60000);
  FakeFrame frame;
  Future *task = make_task_frame(&frame, sleep);
  lang_async_spawn_v1(task);
  lang_async_run_until_v1(stop);
  lang_async_cancel_v1(task);
  lang_async_cancel_v1(task);
  lang_async_cancel_v1(task);
  lang_async_run_until_v1(task);
  lang_async_cancel_v1(task); // completed: must be a no-op
  expect_equal(lang_async_status_v1(task), kCancelled, "stays cancelled");
  expect_equal(frame.cleanup_count, 1, "cleanup ran once");
  expect_equal(frame.destroy_count, 0, "handle still owns the frame");
  lang_async_task_release_v1(task);
  expect_equal(frame.destroy_count, 1, "frame destroyed once");
  lang_async_cancel_v1(sleep);
  lang_async_task_release_v1(sleep);
  lang_async_task_release_v1(stop);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// Task copies (retain/release pairs) plus repeated cancel stay balanced.
void scenario_task_copies_and_cancel() noexcept {
  g_scenario = "task-copies-and-cancel";
  Future *stop = make_timer(1);
  Future *sleep = make_timer(60000);
  FakeFrame frame;
  Future *task = make_task_frame(&frame, sleep);
  lang_async_spawn_v1(task);
  lang_async_run_until_v1(stop);
  lang_async_task_retain_v1(task); // second Task value (copy)
  lang_async_task_retain_v1(task); // third Task value (copy)
  lang_async_cancel_v1(task);
  lang_async_cancel_v1(task);
  lang_async_run_until_v1(task);
  lang_async_task_release_v1(task);
  expect_equal(frame.destroy_count, 0, "two copies still own the frame");
  lang_async_task_release_v1(task);
  expect_equal(frame.destroy_count, 0, "one copy still owns the frame");
  lang_async_task_release_v1(task);
  expect_equal(frame.destroy_count, 1, "last release destroys the frame");
  lang_async_task_release_v1(sleep);
  lang_async_task_release_v1(stop);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// A task that completes normally, then loses its last handle: completion
// removed the registry node and the final release must not unregister twice.
void scenario_completed_task_release() noexcept {
  g_scenario = "completed-task-release";
  FakeFrame frame;
  Future *task = make_task_frame(&frame);
  lang_async_spawn_v1(task);
  lang_async_run_v1(); // runs the queued task to completion
  expect_equal(lang_async_status_v1(task), kDone, "task completed");
  expect_equal(frame.resume_count, 1, "task resumed once");
  expect(g_all == nullptr, "registry empty after completion");
  lang_async_task_release_v1(task);
  expect_equal(frame.destroy_count, 1, "frame destroyed once");
  expect_equal(frame.promise_release_count, 1, "promise released once");
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// An unspawned task that is dropped without shutdown: the frame never ran,
// so the promise was never initialized and must not be released.
void scenario_unspawned_drop_without_shutdown() noexcept {
  g_scenario = "unspawned-drop-without-shutdown";
  FakeFrame frame;
  Future *task = make_task_frame(&frame);
  lang_async_task_release_v1(task);
  expect_equal(frame.resume_count, 0, "body never ran");
  expect_equal(frame.destroy_count, 1, "frame destroyed once");
  expect_equal(frame.promise_release_count, 0, "uninitialized promise kept");
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// A task target that is cancelled notifies its waiting parent; the parent then
// observes the cancellation at its await and runs its own cancel exit plan.
void scenario_await_cancelled_task() noexcept {
  g_scenario = "await-cancelled-task";
  Future *stop = make_timer(1);
  Future *long_sleep = make_timer(60000);
  FakeFrame child_frame;
  Future *child = make_task_frame(&child_frame, long_sleep);
  FakeFrame parent_frame;
  Future *parent = make_task_frame(&parent_frame, child);
  lang_async_spawn_v1(parent);
  lang_async_spawn_v1(child);
  lang_async_run_until_v1(stop);
  expect_equal(parent_frame.resume_count, 1, "parent suspended on the child");
  expect_equal(child_frame.resume_count, 1, "child suspended on its timer");
  expect(child->waiters == parent, "child lists the parent as waiter");
  lang_async_cancel_v1(child);
  lang_async_run_until_v1(child);
  expect_equal(lang_async_status_v1(child), kCancelled, "child cancelled");
  expect_equal(child_frame.cleanup_count, 1, "child cleanup ran");
  expect(child->waiters == nullptr, "parent detached from the child");
  lang_async_run_v1();
  expect_equal(lang_async_status_v1(parent), kCancelled,
               "parent observes the cancelled await");
  expect_equal(parent_frame.cleanup_count, 1, "parent cleanup ran");
  expect_equal(parent_frame.resume_count, 2, "parent resumed once more");
  lang_async_task_release_v1(parent);
  lang_async_task_release_v1(child);
  lang_async_task_release_v1(long_sleep);
  lang_async_task_release_v1(stop);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// A frame that awaits two targets in sequence keeps both links balanced.
void scenario_sequential_awaits() noexcept {
  g_scenario = "sequential-awaits";
  Future *first = make_timer(1);
  Future *second = make_timer(50);
  FakeFrame frame;
  Future *task = make_task_frame(&frame, first, second);
  lang_async_spawn_v1(task);
  lang_async_run_until_v1(first);
  expect(first->waiters == nullptr, "first link detached");
  expect(task->waiting_on == nullptr, "frame no longer waits on the first");
  lang_async_run_v1();
  expect_equal(lang_async_status_v1(task), kDone, "task completed");
  expect_equal(frame.resume_count, 3, "one resume per await wake plus start");
  expect_equal(frame.stage, 2, "both awaits completed");
  expect(second->waiters == nullptr, "second link detached");
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(first);
  lang_async_task_release_v1(second);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// A descriptor waiter registers with epoll on first await, and cancellation
// removes the registration before the frame dies.
void scenario_fd_waiter_cancel() noexcept {
  g_scenario = "fd-waiter-cancel";
  int fds[2] = {-1, -1};
  expect(::pipe(fds) == 0, "pipe created");
  Future *stop = make_timer(1);
  Future *wait = static_cast<Future *>(lang_async_wait_fd_v1(fds[0], 0));
  FakeFrame frame;
  Future *task = make_task_frame(&frame, wait);
  lang_async_spawn_v1(task);
  lang_async_run_until_v1(stop);
  expect((wait->flags & kFdRegistered) != 0, "descriptor registered");
  expect(g_epoll >= 0, "epoll instance created");
  check_executor_invariants("descriptor waiter suspended");
  // Cancelling the frame detaches it from the descriptor future, but the
  // descriptor interest belongs to the still-pending wait object.
  lang_async_cancel_v1(task);
  lang_async_run_until_v1(task);
  expect_equal(lang_async_status_v1(task), kCancelled, "waiter cancelled");
  expect((wait->flags & kFdRegistered) != 0,
         "pending descriptor wait keeps its registration");
  // Cancelling the descriptor wait itself drops the epoll registration.
  lang_async_cancel_v1(wait);
  expect_equal(lang_async_status_v1(wait), kCancelled, "descriptor wait cancelled");
  expect((wait->flags & kFdRegistered) == 0, "registration removed");
  expect(!has_event_source(), "no event source left");
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(wait);
  lang_async_task_release_v1(stop);
  ::close(fds[0]);
  ::close(fds[1]);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// A ready descriptor completes the wait normally and unregisters the fd.
void scenario_fd_ready() noexcept {
  g_scenario = "fd-ready";
  int fds[2] = {-1, -1};
  expect(::pipe(fds) == 0, "pipe created");
  const char byte = 'x';
  expect(::write(fds[1], &byte, 1) == 1, "pipe primed");
  Future *wait = static_cast<Future *>(lang_async_wait_fd_v1(fds[0], 0));
  FakeFrame frame;
  Future *task = make_task_frame(&frame, wait);
  lang_async_spawn_v1(task);
  lang_async_run_until_v1(task);
  expect_equal(lang_async_status_v1(task), kDone, "waiter completed");
  expect_equal(frame.resume_count, 2, "waiter resumed after readability");
  expect((wait->flags & kFdRegistered) == 0, "registration removed");
  expect(!has_event_source(), "no event source left");
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(wait);
  ::close(fds[0]);
  ::close(fds[1]);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// An unawaited timer that is dropped before it expires leaves no registry
// node and no epoll interest.
void scenario_timer_dropped_before_expiry() noexcept {
  g_scenario = "timer-dropped-before-expiry";
  Future *timer = make_timer(60000);
  expect(g_all == timer, "timer registered");
  lang_async_task_release_v1(timer);
  expect(g_all == nullptr, "release removes the timer registry node");
  expect(!has_event_source(), "no event source left");
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// An unawaited timer expires through the executor's blocking wait and is then
// released by its handle.
void scenario_timer_expires_then_release() noexcept {
  g_scenario = "timer-expires-then-release";
  Future *timer = make_timer(1);
  // A lone timer has no pending task, so run() does not drive it; run_until
  // waits for the deadline and completes it.
  lang_async_run_until_v1(timer);
  expect_equal(lang_async_status_v1(timer), kDone, "timer completed");
  expect(g_all == nullptr, "timer left the registry");
  lang_async_task_release_v1(timer);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// shutdown twice is idempotent and does not touch freed state.
void scenario_shutdown_twice() noexcept {
  g_scenario = "shutdown-twice";
  FakeFrame frame;
  Future *task = make_task_frame(&frame);
  lang_async_spawn_v1(task);
  lang_async_run_v1();
  lang_async_shutdown_v1();
  lang_async_shutdown_v1();
  expect_equal(lang_async_status_v1(task), kDone, "task completed");
  lang_async_task_release_v1(task);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// A completed task keeps its promise readable until the last handle drops,
// which is what the emitter's await re-check relies on.
void scenario_promise_readable_after_completion() noexcept {
  g_scenario = "promise-readable-after-completion";
  Future *stop = make_timer(1);
  Future *sleep = make_timer(1);
  FakeFrame frame;
  Future *task = make_task_frame(&frame, sleep);
  frame.promise.value = 42;
  lang_async_spawn_v1(task);
  lang_async_run_until_v1(stop);
  lang_async_run_until_v1(task);
  expect_equal(lang_async_status_v1(task), kDone, "task completed");
  auto *promise = static_cast<FakePromise *>(lang_async_promise_v1(task));
  expect(promise == &frame.promise, "promise pointer survives completion");
  expect_equal(static_cast<uint64_t>(promise->value), 42, "promise value");
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(sleep);
  lang_async_task_release_v1(stop);
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

// Ownership of the real tests/async/cancel fixture: two spawned frames with
// defer-style cleanup, a run_until timer, one explicit cancel and shutdown.
// The runtime must leave no registry node behind. The six source-owned futures
// stay allocated because the emitter does not drop ordinary Task values yet;
// that copy/drop integration is a separate source-backend task. Releasing the
// handles here proves the reference counts are exact.
void scenario_source_cancel_fixture_ownership() noexcept {
  g_scenario = "source-cancel-fixture-ownership";
  Future *child_sleep = make_timer(5000);
  FakeFrame child;
  Future *child_task = make_task_frame(&child, child_sleep);
  Future *first_stop = make_timer(5);
  lang_async_spawn_v1(child_task);
  lang_async_run_until_v1(first_stop);
  expect_equal(child.resume_count, 1, "child suspended on its sleep");
  lang_async_cancel_v1(child_task);
  lang_async_run_until_v1(child_task);
  expect_equal(child.cleanup_count, 1, "child defer cleanup ran");
  expect_equal(lang_async_finished_v1(child_task), 1, "child finished");

  Future *worker_sleep = make_timer(5000);
  FakeFrame worker;
  Future *worker_task = make_task_frame(&worker, worker_sleep);
  Future *second_stop = make_timer(5);
  lang_async_spawn_v1(worker_task);
  lang_async_run_until_v1(second_stop);
  expect_equal(worker.resume_count, 1, "worker suspended on its sleep");
  lang_async_shutdown_v1();
  expect_equal(worker.cleanup_count, 1, "worker defer cleanup ran");
  expect_equal(lang_async_finished_v1(worker_task), 1, "worker finished");
  check_executor_invariants("after shutdown");
  expect(g_all == nullptr, "no registry residue after shutdown");
  expect_equal(g_live_blocks.load(), 6,
               "six source-owned futures remain (documented leak)");
  lang_async_task_release_v1(child_task);
  lang_async_task_release_v1(child_sleep);
  lang_async_task_release_v1(first_stop);
  lang_async_task_release_v1(worker_task);
  lang_async_task_release_v1(worker_sleep);
  lang_async_task_release_v1(second_stop);
  check_clean("after releasing source-owned handles");
}

// The blocking epoll_wait must be wrapped in the runtime SAFE guard, entered
// only while the ready queue is empty and no frame is running.
int g_safe_enter_count = 0;
int g_safe_leave_count = 0;
bool g_safe_saw_ready = false;
bool g_safe_saw_current = false;

int32_t safe_enter_hook() noexcept {
  ++g_safe_enter_count;
  if (g_ready_head)
    g_safe_saw_ready = true;
  if (g_current)
    g_safe_saw_current = true;
  return 1;
}

void safe_leave_hook(int32_t) noexcept { ++g_safe_leave_count; }

void scenario_gc_safe_around_epoll_wait() noexcept {
  g_scenario = "gc-safe-around-epoll-wait";
  lang::native::gc_program::hooks_v1.enter_safe = &safe_enter_hook;
  lang::native::gc_program::hooks_v1.leave_safe = &safe_leave_hook;
  __atomic_store_n(&lang::native::gc_program::enabled_word_v1, 1u,
                   __ATOMIC_RELEASE);
  Future *timer = make_timer(2);
  FakeFrame frame;
  Future *task = make_task_frame(&frame, timer);
  lang_async_spawn_v1(task);
  lang_async_run_v1();
  expect(g_safe_enter_count >= 1, "SAFE entered for the blocking wait");
  expect_equal(g_safe_enter_count, g_safe_leave_count, "SAFE enter/leave balanced");
  expect(!g_safe_saw_ready, "SAFE entered with an empty ready queue");
  expect(!g_safe_saw_current, "SAFE entered outside a frame");
  expect_equal(lang_async_status_v1(task), kDone, "task completed");
  lang_async_task_release_v1(task);
  lang_async_task_release_v1(timer);
  __atomic_store_n(&lang::native::gc_program::enabled_word_v1, 0u,
                   __ATOMIC_RELEASE);
  lang::native::gc_program::hooks_v1.enter_safe = nullptr;
  lang::native::gc_program::hooks_v1.leave_safe = nullptr;
  lang_async_shutdown_v1();
  check_clean("after shutdown");
}

struct Scenario {
  const char *name;
  void (*run)();
};

const Scenario kScenarios[] = {
    {"unstarted-release-then-shutdown", scenario_unstarted_release_then_shutdown},
    {"cancel-waiter-then-child-complete", scenario_cancel_waiter_then_child_complete},
    {"cancel-suspended-task", scenario_cancel_suspended_task},
    {"drop-handle-while-suspended", scenario_drop_handle_while_suspended},
    {"cancel-queued-before-first-run", scenario_cancel_queued_before_first_run},
    {"drop-queued-task-then-shutdown", scenario_drop_queued_task_then_shutdown},
    {"multiple-waiters", scenario_multiple_waiters},
    {"multiple-waiters-cancel-one", scenario_multiple_waiters_cancel_one},
    {"repeated-cancel-and-release", scenario_repeated_cancel_and_release},
    {"task-copies-and-cancel", scenario_task_copies_and_cancel},
    {"completed-task-release", scenario_completed_task_release},
    {"unspawned-drop-without-shutdown", scenario_unspawned_drop_without_shutdown},
    {"await-cancelled-task", scenario_await_cancelled_task},
    {"sequential-awaits", scenario_sequential_awaits},
    {"fd-waiter-cancel", scenario_fd_waiter_cancel},
    {"fd-ready", scenario_fd_ready},
    {"timer-dropped-before-expiry", scenario_timer_dropped_before_expiry},
    {"timer-expires-then-release", scenario_timer_expires_then_release},
    {"shutdown-twice", scenario_shutdown_twice},
    {"promise-readable-after-completion", scenario_promise_readable_after_completion},
    {"source-cancel-fixture-ownership", scenario_source_cancel_fixture_ownership},
    {"gc-safe-around-epoll-wait", scenario_gc_safe_around_epoll_wait},
};

} // namespace

int main(int argc, char **argv) {
  int ran = 0;
  int failed = 0;
  if (argc > 1) {
    for (const Scenario &scenario : kScenarios) {
      if (std::strcmp(scenario.name, argv[1]) != 0)
        continue;
      const int before = g_failures;
      scenario.run();
      ++ran;
      const bool ok = g_failures == before;
      std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", scenario.name);
      if (!ok)
        ++failed;
      return failed == 0 ? 0 : 1;
    }
    std::fprintf(stderr, "unknown scenario: %s\n", argv[1]);
    return 2;
  }
  for (const Scenario &scenario : kScenarios) {
    const int before = g_failures;
    scenario.run();
    ++ran;
    const bool ok = g_failures == before;
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", scenario.name);
    if (!ok)
      ++failed;
  }
  std::printf("%d/%d async runtime lifetime scenarios passed\n", ran - failed,
              ran);
  return failed == 0 ? 0 : 1;
}
