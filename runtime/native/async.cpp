// Cooperative stackless task executor for async functions.
//
// The compiler lowers an `async func` into a resumable coroutine whose local
// slots live in a heap frame. This translation unit owns the runtime side:
// future/task objects, the run queue, cancellation, Linux epoll event waiting
// and the timer deadline used by `await async.sleep(ms)`.
//
// Design constraints:
//   * One process-wide executor. It runs on whichever thread calls
//     `lang_async_run_v1`, `lang_async_run_until_v1` or
//     `lang_async_shutdown_v1`; tasks never migrate between threads.
//   * A suspended task is a frame plus a record in a future object. The frame
//     is released only when the last Task handle is dropped, so an awaiter can
//     still read the promise after the task completed.
//   * Cancellation is cooperative: it is observed at an await or at the first
//     resume of a queued task. The coroutine then runs its cancel exit plan
//     (defers and owning-local drops) before the frame is destroyed.
//   * This file is freestanding: raw Linux x86_64 syscalls, no libc, no
//     exceptions and no allocation other than lang_alloc_v1.
//
// Lifetime model (lang-async/1):
//
//   Every future object owns one reference per owner:
//     * the source Task value produced by the compiler's async start function
//       (created here with refs == 1 and handed to the source),
//     * the executor while a task is runnable or resumable (kActive),
//     * one reference on each side of a pending `await` link.
//
//   An await link between `awaiter` and `target` retains both objects: the
//   target keeps every frame in `target->waiters` alive, and the awaiter keeps
//   the target alive through `awaiter->waiting_on`. Both references are
//   acquired in the same step and dropped in the same detachment step, so a
//   waiter can never walk a freed list and a target can never be freed under a
//   waiter's `waiting_on` pointer. The executor-owned kActive reference keeps
//   a started (but not yet finished) frame alive even after the source drops
//   its Task handle.
//
//   The registry `g_all` holds exactly the pending futures, so shutdown can
//   cancel every outstanding timer, descriptor waiter and task, and the ready
//   queue never contains a completed or released future. A released future is
//   removed from both the ready queue and the registry before its memory goes
//   away; there are no stale registry nodes.
//
// Blocking: the executor is single-threaded and holds no lock while it waits
// in epoll. The blocking syscall is wrapped in the runtime's SAFE guard
// (see runtime/native/gc_program.hpp) exactly like the thread join and sync
// futex waits in runtime/native/thread.cpp and runtime/native/sync.cpp: the
// ready queue, registry and waiter links are settled before entering SAFE and
// are read again only after leaving it. Suspended frames are not GC roots;
// they are protected by their ordinary ARC references to the values they own.

#include "linux.hpp"

#include "gc_program.hpp"

#include <cstddef>
#include <cstdint>

extern "C" void *lang_alloc_v1(uint64_t, uint64_t) noexcept;
extern "C" void lang_free_v1(void *) noexcept;
extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t, uint64_t,
                                                 uint64_t) noexcept;

#if defined(__x86_64__) && defined(__linux__)

namespace {

namespace lx = lang::native::linux_x86_64;

constexpr long kSysEpollWait = 232;
constexpr long kSysEpollCtl = 233;
constexpr long kSysEpollCreate1 = 291;
constexpr long kSysClockGettime = 228;
constexpr long kClockMonotonic = 1;
constexpr int64_t kEintr = -4;
constexpr int64_t kNanoseconds = 1000000000;

constexpr int kEpollCtlAdd = 1;
constexpr int kEpollCtlDel = 2;
constexpr uint32_t kEpollIn = 0x001;
constexpr uint32_t kEpollOut = 0x004;

constexpr uint32_t kPending = 0;
constexpr uint32_t kDone = 1;
constexpr uint32_t kError = 2;
constexpr uint32_t kCancelled = 3;

constexpr uint32_t kKindTask = 0;
constexpr uint32_t kKindTimer = 1;
constexpr uint32_t kKindFd = 2;

constexpr uint32_t kQueued = 1u << 0;
constexpr uint32_t kStarted = 1u << 1;
constexpr uint32_t kActive = 1u << 2;   // executor-owned reference
constexpr uint32_t kWaiting = 1u << 3;
constexpr uint32_t kCancelledFlag = 1u << 4;
constexpr uint32_t kSuspended = 1u << 5;
constexpr uint32_t kFdRegistered = 1u << 6;
constexpr uint32_t kParentRef = 1u << 7; // this future owns a ref on waiting_on

struct Timespec {
  int64_t seconds;
  int64_t nanoseconds;
};

struct __attribute__((packed)) EpollEvent {
  uint32_t events;
  uint64_t data;
};

struct Future {
  uint64_t refs;
  uint32_t status;
  uint32_t flags;
  uint32_t kind;
  int32_t fd;
  uint32_t events;
  uint32_t reserved;
  uint64_t deadline;
  void *handle;
  void (*resume)(void *);
  void (*destroy)(void *);
  void *promise;
  void (*release_promise)(void *);
  Future *next_ready;
  Future *next_waiter;
  Future *waiters;
  Future *next_all;
  Future *waiting_on;
};

// The executor is single-threaded by contract. These globals are therefore
// only touched by the thread currently inside run/await/shutdown. Future
// reference counts still use atomics because Task values may be copied into
// other threads' data structures even though only one executor thread resumes
// frames.
Future *g_ready_head = nullptr;
Future *g_ready_tail = nullptr;
Future *g_all = nullptr;
Future *g_current = nullptr;
int g_epoll = -1;
uint32_t g_live_tasks = 0;

int64_t syscall_epoll_create1(int flags) noexcept {
  return lx::syscall6(kSysEpollCreate1, flags, 0, 0, 0, 0, 0);
}
int64_t syscall_epoll_ctl(int epfd, int op, int fd, EpollEvent *event) noexcept {
  return lx::syscall6(kSysEpollCtl, epfd, op, fd,
                      reinterpret_cast<long>(event), 0, 0);
}
int64_t syscall_epoll_wait(int epfd, EpollEvent *events, int max,
                           int timeout) noexcept {
  return lx::syscall6(kSysEpollWait, epfd,
                      reinterpret_cast<long>(events), max, timeout, 0, 0);
}
int64_t syscall_clock_gettime(int clock, Timespec *ts) noexcept {
  return lx::syscall6(kSysClockGettime, clock, reinterpret_cast<long>(ts), 0, 0,
                      0, 0);
}

void fail(const char *message) noexcept {
  (void)message;
  lang_native_panic_v1(7, 0, 0, 0);
}

uint64_t now_ns() noexcept {
  Timespec ts{};
  if (syscall_clock_gettime(kClockMonotonic, &ts) < 0)
    fail("clock_gettime");
  return static_cast<uint64_t>(ts.seconds) * kNanoseconds +
         static_cast<uint64_t>(ts.nanoseconds);
}

int ensure_epoll() noexcept {
  if (g_epoll >= 0)
    return g_epoll;
  auto fd = syscall_epoll_create1(0);
  if (fd < 0)
    fail("epoll_create1");
  g_epoll = static_cast<int>(fd);
  return g_epoll;
}

// ---------------------------------------------------------------------------
// Registry. Only pending futures are registered. A future leaves the registry
// exactly once: either it completes, or its last reference is released while it
// is still pending (an unused timer or descriptor waiter).
// ---------------------------------------------------------------------------

void registry_add(Future *future) noexcept {
  future->next_all = g_all;
  g_all = future;
  if (future->kind == kKindTask)
    ++g_live_tasks;
}

bool registry_remove(Future *future) noexcept {
  Future **link = &g_all;
  while (*link && *link != future)
    link = &(*link)->next_all;
  if (*link != future) {
    // Already unregistered (completion happens before final release); do not
    // decrement the pending-task counter twice.
    future->next_all = nullptr;
    return false;
  }
  *link = future->next_all;
  future->next_all = nullptr;
  if (future->kind == kKindTask && g_live_tasks)
    --g_live_tasks;
  return true;
}

bool registry_has_pending() noexcept {
  for (Future *f = g_all; f; f = f->next_all)
    if (f->status == kPending)
      return true;
  return false;
}

// ---------------------------------------------------------------------------
// Ready queue. Every future in the queue is pending and holds kActive; a
// future that completes or is destroyed is unlinked first so a queued node is
// always a live object.
// ---------------------------------------------------------------------------

void enqueue(Future *future) noexcept {
  if (!future || future->status != kPending)
    return;
  if (future->flags & kQueued)
    return;
  future->flags |= kQueued;
  future->next_ready = nullptr;
  if (g_ready_tail)
    g_ready_tail->next_ready = future;
  else
    g_ready_head = future;
  g_ready_tail = future;
}

void ready_remove(Future *future) noexcept {
  if (!future || !(future->flags & kQueued))
    return;
  Future *previous = nullptr;
  Future *current = g_ready_head;
  while (current && current != future) {
    previous = current;
    current = current->next_ready;
  }
  if (current != future) {
    // A stale queued flag must not keep a released object linked.
    future->next_ready = nullptr;
    future->flags &= ~kQueued;
    return;
  }
  if (previous)
    previous->next_ready = current->next_ready;
  else
    g_ready_head = current->next_ready;
  if (g_ready_tail == current)
    g_ready_tail = previous;
  current->next_ready = nullptr;
  current->flags &= ~kQueued;
}

Future *dequeue() noexcept {
  Future *future = g_ready_head;
  if (!future)
    return nullptr;
  g_ready_head = future->next_ready;
  if (!g_ready_head)
    g_ready_tail = nullptr;
  future->next_ready = nullptr;
  future->flags &= ~kQueued;
  return future;
}

void clear_ready_queue() noexcept {
  Future *future = g_ready_head;
  g_ready_head = nullptr;
  g_ready_tail = nullptr;
  while (future) {
    Future *next = future->next_ready;
    future->next_ready = nullptr;
    future->flags &= ~kQueued;
    future = next;
  }
}

// ---------------------------------------------------------------------------
// Reference counting.
// ---------------------------------------------------------------------------

void destroy_future(Future *future) noexcept {
  // A released future must not stay reachable from the executor.
  ready_remove(future);
  (void)registry_remove(future);
  if (future->flags & kFdRegistered) {
    if (g_epoll >= 0) {
      EpollEvent event{};
      (void)syscall_epoll_ctl(g_epoll, kEpollCtlDel, future->fd, &event);
    }
    future->flags &= ~kFdRegistered;
  }
  // Invariant: a future with zero references has no waiters (each waiter owns
  // a reference) and no parent (the link owns a reference). Clear the links
  // defensively without touching reference counts so misuse cannot leave a
  // dangling list entry behind.
  future->waiting_on = nullptr;
  future->next_waiter = nullptr;
  future->waiters = nullptr;
  future->flags &= ~(kQueued | kWaiting | kActive | kSuspended | kParentRef);
  if (future->handle) {
    // The promise is valid only after the first resume initialized it.
    if ((future->flags & kStarted) && future->release_promise)
      future->release_promise(future->promise);
    if (future->destroy)
      future->destroy(future->handle);
    future->handle = nullptr;
  }
  lang_free_v1(future);
}

void retain(Future *future) noexcept {
  if (future)
    __atomic_add_fetch(&future->refs, 1, __ATOMIC_RELAXED);
}

void release(Future *future) noexcept {
  if (!future)
    return;
  if (__atomic_sub_fetch(&future->refs, 1, __ATOMIC_ACQ_REL) == 0)
    destroy_future(future);
}

// ---------------------------------------------------------------------------
// Await links.
// ---------------------------------------------------------------------------

// Remove `waiter` from its target's waiter list and drop the two references
// owned by that link. The caller must hold a reference to `waiter`: complete()
// takes one for the whole notification sequence and a re-linking await runs on
// the current frame, which is kept alive by its executor reference.
void detach_from_parent(Future *waiter) noexcept {
  Future *target = waiter->waiting_on;
  if (!target)
    return;
  Future **link = &target->waiters;
  while (*link && *link != waiter)
    link = &(*link)->next_waiter;
  if (*link == waiter)
    *link = waiter->next_waiter;
  waiter->next_waiter = nullptr;
  waiter->waiting_on = nullptr;
  waiter->flags &= ~(kWaiting | kParentRef);
  release(target); // reference this link held on the awaited target
  release(waiter); // reference the target held on this frame
}

// Detach and wake every frame waiting for `target`. The caller must hold a
// reference to `target` while this runs: dropping the per-link references can
// release the last owner of the target or of a detached waiter.
void notify_waiters(Future *target) noexcept {
  Future *waiter = target->waiters;
  target->waiters = nullptr;
  while (waiter) {
    Future *next = waiter->next_waiter;
    waiter->next_waiter = nullptr;
    waiter->waiting_on = nullptr;
    waiter->flags &= ~(kWaiting | kParentRef);
    // A waiting frame is always a started task owned by the executor
    // (kActive), so queuing it before dropping the link reference is safe.
    enqueue(waiter);
    release(target); // reference this link held on the target
    release(waiter); // reference the target held on this frame
    waiter = next;
  }
}

// ---------------------------------------------------------------------------
// Completion and cancellation.
// ---------------------------------------------------------------------------

void complete(Future *future, uint32_t status) noexcept {
  if (!future || future->status != kPending)
    return;
  // Local reference: detaching the await links and dropping the executor
  // reference can release the last owner, but this object must stay readable
  // until the bookkeeping below is finished.
  retain(future);
  // Publish the new status first: waiters observe it when they resume, and
  // enqueue() only accepts pending futures.
  future->status = status;
  future->flags &= ~kSuspended;
  ready_remove(future);
  if (future->flags & kFdRegistered) {
    if (g_epoll >= 0) {
      EpollEvent event{};
      (void)syscall_epoll_ctl(g_epoll, kEpollCtlDel, future->fd, &event);
    }
    future->flags &= ~kFdRegistered;
  }
  (void)registry_remove(future);
  // Detach from the target we were waiting for first so that target never
  // points at a completed frame, then wake our own waiters.
  detach_from_parent(future);
  notify_waiters(future);
  if (future->flags & kActive) {
    future->flags &= ~kActive;
    release(future);
  }
  release(future);
}

void cancel_future(Future *future) noexcept {
  if (!future || future->status != kPending)
    return;
  if (future->kind != kKindTask) {
    complete(future, kCancelled);
    return;
  }
  future->flags |= kCancelledFlag;
  if (!(future->flags & kStarted)) {
    // Never resumed: no source cleanup was registered, so the frame can be
    // released without executing the body. This also removes a queued task
    // from the ready queue.
    complete(future, kCancelled);
    return;
  }
  if (future == g_current) {
    // A running frame observes the flag at its next await; it must not queue
    // itself while it is still on the stack.
    return;
  }
  if (future->flags & kSuspended) {
    // Resume once so the coroutine runs its cancel exit plan.
    enqueue(future);
  }
}

bool has_pending_tasks() noexcept { return g_live_tasks != 0; }

bool has_event_source() noexcept {
  for (Future *f = g_all; f; f = f->next_all)
    if (f->status == kPending &&
        (f->kind == kKindTimer || (f->kind == kKindFd && f->flags & kFdRegistered)))
      return true;
  return false;
}

int next_timeout_ms() noexcept {
  uint64_t now = now_ns();
  bool found = false;
  uint64_t nearest = 0;
  for (Future *f = g_all; f; f = f->next_all) {
    if (f->status != kPending || f->kind != kKindTimer)
      continue;
    if (!found || f->deadline < nearest) {
      nearest = f->deadline;
      found = true;
    }
  }
  if (!found)
    return -1;
  if (nearest <= now)
    return 0;
  uint64_t delta = nearest - now;
  uint64_t ms = (delta + 999999) / 1000000;
  if (ms > 0x7fffffff)
    ms = 0x7fffffff;
  return static_cast<int>(ms);
}

bool expire_timers() noexcept {
  const uint64_t now = now_ns();
  bool expired = false;
  Future *f = g_all;
  while (f) {
    Future *next = f->next_all;
    if (f->status == kPending && f->kind == kKindTimer &&
        f->deadline <= now) {
      complete(f, kDone);
      expired = true;
    }
    f = next;
  }
  return expired;
}

void register_fd(Future *future) noexcept {
  if (future->flags & kFdRegistered)
    return;
  int epfd = ensure_epoll();
  EpollEvent event{};
  event.events = future->events ? future->events : kEpollIn;
  event.data = reinterpret_cast<uint64_t>(future);
  if (syscall_epoll_ctl(epfd, kEpollCtlAdd, future->fd, &event) < 0)
    fail("epoll_ctl");
  future->flags |= kFdRegistered;
}

void run_one(Future *future) noexcept {
  if (!future || future->status != kPending)
    return;
  if (future->kind != kKindTask) {
    if (future->flags & kCancelledFlag)
      complete(future, kCancelled);
    return;
  }
  if (!future->handle || !future->resume)
    fail("resume of unpublished task");
  future->flags |= kStarted;
  future->flags &= ~kSuspended;
  g_current = future;
  future->resume(future->handle);
  g_current = nullptr;
  const uint32_t status = future->promise
                              ? *static_cast<const uint32_t *>(future->promise)
                              : kPending;
  if (status != kPending)
    complete(future, status);
  else
    future->flags |= kSuspended;
}

void run_loop(Future *until) noexcept {
  for (;;) {
    if (until ? until->status != kPending : !has_pending_tasks())
      return;
    if (Future *ready = dequeue()) {
      run_one(ready);
      continue;
    }
    // Re-evaluate the loop condition after timer completion: an awaited
    // deadline may have satisfied `until` even though a descriptor waiter is
    // still registered.
    const bool expired = expire_timers();
    if (expired || g_ready_head)
      continue;
    if (!has_event_source())
      return;
    int epfd = ensure_epoll();
    EpollEvent events[16];
    // Blocking wait. The ready queue, registry and waiter links are settled
    // and the executor holds no runtime lock, so the collector may park this
    // thread (same convention as the thread join and sync futex waits). The
    // timeout is computed before entering SAFE because it reads the registry.
    const int timeout_ms = next_timeout_ms();
    int64_t waited;
    {
      lang::native::gc_program::SafeGuard safe;
      waited = syscall_epoll_wait(epfd, events, 16, timeout_ms);
    }
    const int count = static_cast<int>(waited);
    if (count < 0) {
      if (count == kEintr)
        continue;
      return;
    }
    for (int i = 0; i < count; ++i)
      complete(reinterpret_cast<Future *>(events[i].data), kDone);
    // Expired timers are handled at the top of the next iteration.
  }
}

Future *new_future(uint32_t kind) noexcept {
  auto *memory = static_cast<Future *>(lang_alloc_v1(sizeof(Future), 16));
  if (!memory)
    fail("async future allocation");
  auto *future = static_cast<Future *>(memory);
  future->refs = 1;
  future->status = kPending;
  future->flags = 0;
  future->kind = kind;
  future->fd = -1;
  future->events = 0;
  future->reserved = 0;
  future->deadline = 0;
  future->handle = nullptr;
  future->resume = nullptr;
  future->destroy = nullptr;
  future->promise = nullptr;
  future->release_promise = nullptr;
  future->next_ready = nullptr;
  future->next_waiter = nullptr;
  future->waiters = nullptr;
  future->next_all = nullptr;
  future->waiting_on = nullptr;
  registry_add(future);
  return future;
}

// Cancel every pending registry entry. Returns true when at least one pending
// future was seen. Each visited future and its registry successor are held by
// a local reference so a cancel that releases the last owner cannot free the
// traversal state.
bool cancel_pending_pass() noexcept {
  bool saw_pending = false;
  Future *f = g_all;
  if (f)
    retain(f);
  while (f) {
    Future *next = f->next_all;
    if (next)
      retain(next);
    if (f->status == kPending) {
      saw_pending = true;
      cancel_future(f);
    }
    release(f);
    f = next;
  }
  return saw_pending;
}

// Last-resort teardown: complete anything that stayed pending after repeated
// cancel/drain passes so no registry node survives shutdown. The frame is then
// released by the normal destroy path without running a cancel exit plan.
void force_complete_pending() noexcept {
  Future *f = g_all;
  if (f)
    retain(f);
  while (f) {
    Future *next = f->next_all;
    if (next)
      retain(next);
    if (f->status == kPending)
      complete(f, kCancelled);
    release(f);
    f = next;
  }
}

// Source value operations. A Future is created by the compiler's start
// function; the compiler publishes the coroutine handle and promise pointers
// before the initial suspend.
} // namespace

extern "C" {

void *lang_async_task_new_v1() noexcept { return new_future(kKindTask); }

void lang_async_publish_v1(void *raw, void *handle, void *resume, void *destroy,
                           void *promise, void *release_promise) noexcept {
  auto *future = static_cast<Future *>(raw);
  if (!future || future->kind != kKindTask)
    fail("publish on non-task future");
  future->handle = handle;
  future->resume =
      reinterpret_cast<void (*)(void *)>(resume);
  future->destroy =
      reinterpret_cast<void (*)(void *)>(destroy);
  future->promise = promise;
  future->release_promise =
      reinterpret_cast<void (*)(void *)>(release_promise);
}

void lang_async_task_retain_v1(void *raw) noexcept {
  retain(static_cast<Future *>(raw));
}

void lang_async_task_release_v1(void *raw) noexcept {
  release(static_cast<Future *>(raw));
}

void lang_async_spawn_v1(void *raw) noexcept {
  auto *future = static_cast<Future *>(raw);
  if (!future || future->kind != kKindTask)
    fail("spawn on non-task future");
  if (future->status != kPending)
    return;
  if (future->flags & (kActive | kQueued | kStarted))
    return;
  retain(future);
  future->flags |= kActive;
  enqueue(future);
}

uint32_t lang_async_status_v1(void *raw) noexcept {
  auto *future = static_cast<Future *>(raw);
  return future ? future->status : kCancelled;
}

void *lang_async_promise_v1(void *raw) noexcept {
  auto *future = static_cast<Future *>(raw);
  return future ? future->promise : nullptr;
}

uint32_t lang_async_finished_v1(void *raw) noexcept {
  return lang_async_status_v1(raw) != kPending;
}

void lang_async_cancel_v1(void *raw) noexcept {
  cancel_future(static_cast<Future *>(raw));
}

uint32_t lang_async_current_cancelled_v1() noexcept {
  return g_current && (g_current->flags & kCancelledFlag) ? 1u : 0u;
}

uint32_t lang_async_await_v1(void *raw) noexcept {
  auto *target = static_cast<Future *>(raw);
  Future *awaiter = g_current;
  if (!target)
    return kCancelled;
  if (!awaiter)
    return kCancelled;
  if (awaiter->flags & kCancelledFlag)
    return kCancelled;
  if (target->status != kPending)
    return target->status;
  if (target == awaiter)
    return kCancelled;
  // Awaiting a task that was created but never spawned starts it implicitly;
  // this keeps nested `await f()` useful without an explicit spawn.
  if (target->kind == kKindTask && !(target->flags & (kStarted | kQueued))) {
    retain(target);
    target->flags |= kActive;
    enqueue(target);
  }
  if (target->kind == kKindFd)
    register_fd(target);
  if (awaiter->waiting_on == target)
    return kPending;
  // A running frame awaits one target at a time; drop a stale link first so a
  // re-await cannot orphan the previous list entry or its references.
  detach_from_parent(awaiter);
  retain(target);  // link reference: the awaiter keeps the target alive
  retain(awaiter); // link reference: the target keeps its waiter alive
  awaiter->next_waiter = target->waiters;
  target->waiters = awaiter;
  awaiter->waiting_on = target;
  awaiter->flags |= kWaiting | kParentRef;
  return kPending;
}

void *lang_async_sleep_v1(int64_t milliseconds) noexcept {
  auto *future = new_future(kKindTimer);
  uint64_t delta = milliseconds > 0
                       ? static_cast<uint64_t>(milliseconds) * 1000000ull
                       : 0;
  uint64_t now = now_ns();
  future->deadline =
      delta > ~uint64_t{0} - now ? ~uint64_t{0} : now + delta;
  return future;
}

void *lang_async_wait_fd_v1(int32_t fd, int32_t events) noexcept {
  if (fd < 0)
    fail("wait_fd requires a non-negative descriptor");
  auto *future = new_future(kKindFd);
  future->fd = fd;
  future->events = static_cast<uint32_t>(events);
  return future;
}

void lang_async_run_v1() noexcept { run_loop(nullptr); }

void lang_async_run_until_v1(void *raw) noexcept {
  run_loop(static_cast<Future *>(raw));
}

void lang_async_shutdown_v1() noexcept {
  // Cancelling wakes suspended tasks so their cancel exit plans run while
  // unwinding. Tasks that never started are completed without a frame resume.
  // A cancelled frame can itself release the last owner of another registry
  // entry (a spawned task whose source handle was already dropped), so each
  // pass holds local references and the registry is drained before the next
  // pass.
  constexpr int kMaxShutdownPasses = 8;
  for (int pass = 0; pass < kMaxShutdownPasses; ++pass) {
    if (!cancel_pending_pass())
      break;
    run_loop(nullptr);
    if (!registry_has_pending())
      break;
  }
  if (registry_has_pending())
    force_complete_pending();
  // Nothing may stay queued across shutdown: every queued future was either
  // completed above or is no longer part of the executor.
  clear_ready_queue();
  if (g_epoll >= 0) {
    (void)lx::syscall6(3 /* close */, g_epoll, 0, 0, 0, 0, 0);
    g_epoll = -1;
  }
  // g_current is owned by run_one(); shutdown must not be called from inside a
  // running task and must not clobber the executor's current-frame marker.
}

void *lang_async_frame_alloc_v1(uint64_t size) noexcept {
  return lang_alloc_v1(size, 16);
}

void lang_async_frame_free_v1(void *memory) noexcept { lang_free_v1(memory); }

} // extern "C"

#endif // x86_64 Linux
