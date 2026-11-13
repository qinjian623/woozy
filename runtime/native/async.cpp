// Cooperative stackless task executor for async functions.
//
// The compiler lowers an `async func` into a resumable coroutine whose local
// slots live in a heap frame. This translation unit owns the runtime side:
// future/task objects, the run queue, cancellation, Linux epoll event waiting,
// the timer deadline used by `await async.sleep(ms)`, and the Future GC node
// that stands for one coroutine frame in the `lang-gc/1` graph.
//
// Design constraints:
//   * One process-wide executor. It runs on whichever thread first calls
//     `lang_async_spawn_v1`, `lang_async_cancel_v1`, `lang_async_run_v1`,
//     `lang_async_run_until_v1` or `lang_async_shutdown_v1`; that thread is
//     bound once and any other thread using those entry points is a terminal
//     Panic. Tasks never migrate between threads.
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
// GC integration (see async.hpp for the published contract):
//
//   A task Future is the single registered node for its coroutine frame and
//   promise. Registration happens at publish_v2 time, only when the frame
//   reports typed `trace` metadata and `lang-gc-program/1` is enabled. The
//   kActive reference is an executor external root and is never traced; the
//   registry and run queue are borrowed pointers and are never traced. The two
//   references owned by a pending await link are traced by their owner, so the
//   collector sees exactly the ARC edges that exist.
//
//   `task_gc_clear` only ever runs on a task the executor cannot continue
//   (`!kActive && (!kStarted || status != Pending)`): a never-started task with
//   its creation parameters, or a finished task with an unclaimed result or
//   typed error. It drops frame-owned values through the compiler-supplied
//   `clear_owners`, moves a never-started task to Cancelled through the normal
//   completion path, and never resumes the coroutine or runs a source defer.
//
// Blocking: the executor holds no lock while it waits in epoll. The blocking
// syscall is wrapped in the runtime's SAFE guard (runtime/native/gc_program.hpp)
// exactly like the thread join and sync futex waits in runtime/native/thread.cpp
// and runtime/native/sync.cpp: the ready queue, registry and waiter links are
// settled before entering SAFE and are read again only after leaving it.
// Suspended frames are not GC roots; they are protected by their ordinary ARC
// references to the values they own plus the executor's kActive reference.
//
// Threading: Task handles may be copied and finally released from any thread.
// Reference counts are atomic, and every mutation of the executor registry,
// run queue or epoll descriptor happens under a short internal spin lock. That
// lock is never held while running a frame, invoking a typed trace/clear
// callback, `clear_owners`, a coroutine destroy, a legacy promise release or a
// SAFE region, so arbitrary destructors never run inside the executor lock.

#include "async.hpp"

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
constexpr long kSysGettid = 186;
constexpr long kSysClose = 3;
constexpr long kClockMonotonic = 1;
constexpr int64_t kEintr = -4;
constexpr int64_t kEbusy = -16;
constexpr int64_t kNanoseconds = 1000000000;
// EPOLL_CLOEXEC. A private child epoll descriptor must never leak into an
// exec, including the internal spawn fast path and an exec failure that exits
// with _exit.
constexpr int kEpollCloexec = 0x80000;

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
constexpr uint32_t kParentRef = 1u << 7;        // owns a ref on waiting_on
constexpr uint32_t kLegacyPromiseRelease = 1u << 8; // gc_state is a release fn

struct Timespec {
  int64_t seconds;
  int64_t nanoseconds;
};

struct __attribute__((packed)) EpollEvent {
  uint32_t events;
  uint64_t data;
};

struct Future {
  uint64_t refs; // first field: lang-gc/1 reads the strong count here
  uint32_t status;
  uint32_t flags;
  uint32_t kind;
  int32_t fd;
  uint32_t events;
  uint32_t gc_registered;
  uint64_t deadline;
  void *handle;
  void (*resume)(void *);
  void (*destroy)(void *);
  void *promise;
  void *gc_state;
  const LangAsyncFrameGcOps *frame_ops;
  Future *next_ready;
  Future *next_waiter;
  Future *waiters;
  Future *next_all;
  Future *waiting_on;
};

// Executor tables. Mutated by the executor thread and by a final release on any
// thread, always under g_lock; read for tracing by the collector while the
// world is stopped (the executor is then parked in SAFE and touches nothing).
Future *g_ready_head = nullptr;
Future *g_ready_tail = nullptr;
Future *g_all = nullptr;
Future *g_current = nullptr;
int g_epoll = -1;
uint32_t g_live_tasks = 0;
uint32_t g_lock_next = 0;
uint32_t g_lock_serving = 0;
int32_t g_executor_tid = 0; // 0 = unbound; bound by the first executor call

// Fork reservation gate. A fork must hold this word from before its final
// executor-TID check until the fork syscall has returned in the parent/child.
// A concurrent first executor bind takes the same word, so the check and the
// copy cannot interleave with a bind. The gate is a single 64-bit word:
//   low 32 bits  owner tid (0 = free)
//   high 32 bits nesting depth for a same-thread nested fork
// It is deliberately not the executor ticket lock: a first bind may wait for
// it without holding an internal runtime lock. A final Task release never uses
// this gate; it keeps refs == 1 through the real GC unregister instead.
uint64_t g_fork_gate = 0;
int32_t g_fork_parent_tid = 0;
// Only one non-exec fork can be in the gate at a time, so one flag records
// whether the prepare hook acquired the executor ticket lock.
uint32_t g_fork_executor_locked = 0;
// The child inherited the parent's epoll instance. It is closed and this word
// makes the next ensure_epoll() create a private EPOLL_CLOEXEC instance and
// re-add exactly the pending descriptor waits that were registered.
uint32_t g_epoll_needs_rebuild = 0;

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
int64_t syscall_gettid() noexcept {
  return lx::syscall6(kSysGettid, 0, 0, 0, 0, 0, 0);
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

// ---------------------------------------------------------------------------
// Executor lock: a ticket spin lock around the executor tables only. Critical
// sections contain pointer updates and atomic counter work; they never run a
// frame, a typed GC callback, `clear_owners`, a coroutine destroy or a SAFE
// region.
// ---------------------------------------------------------------------------

inline void cpu_relax() noexcept {
  __asm__ volatile("pause" ::: "memory");
}

void executor_lock_acquire() noexcept {
  const uint32_t ticket = __atomic_fetch_add(&g_lock_next, 1u, __ATOMIC_RELAXED);
  while (__atomic_load_n(&g_lock_serving, __ATOMIC_ACQUIRE) != ticket)
    cpu_relax();
}

void executor_lock_release() noexcept {
  __atomic_fetch_add(&g_lock_serving, 1u, __ATOMIC_RELEASE);
}

int32_t current_tid() noexcept {
  const int64_t tid = syscall_gettid();
  if (tid <= 0)
    fail("gettid");
  return static_cast<int32_t>(tid);
}

bool fork_gate_owned_by(int32_t tid) noexcept {
  const uint64_t state = __atomic_load_n(&g_fork_gate, __ATOMIC_ACQUIRE);
  return static_cast<uint32_t>(state) == static_cast<uint32_t>(tid);
}

void fork_gate_acquire(int32_t tid) noexcept {
  for (;;) {
    uint64_t state = __atomic_load_n(&g_fork_gate, __ATOMIC_ACQUIRE);
    const uint32_t owner = static_cast<uint32_t>(state);
    const uint32_t depth = static_cast<uint32_t>(state >> 32);
    if (owner == 0) {
      const uint64_t next = static_cast<uint32_t>(tid) | (uint64_t{1} << 32);
      if (__atomic_compare_exchange_n(&g_fork_gate, &state, next, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
      continue;
    }
    if (owner == static_cast<uint32_t>(tid)) {
      if (depth == 0xffffffffu)
        fail("async fork gate depth overflow");
      const uint64_t next = state + (uint64_t{1} << 32);
      if (__atomic_compare_exchange_n(&g_fork_gate, &state, next, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;
      continue;
    }
    cpu_relax();
  }
}

void fork_gate_release(int32_t tid) noexcept {
  for (;;) {
    uint64_t state = __atomic_load_n(&g_fork_gate, __ATOMIC_ACQUIRE);
    const uint32_t owner = static_cast<uint32_t>(state);
    const uint32_t depth = static_cast<uint32_t>(state >> 32);
    if (owner != static_cast<uint32_t>(tid) || depth == 0)
      fail("async fork gate imbalance");
    const uint64_t next =
        depth == 1 ? 0 : (static_cast<uint64_t>(depth - 1) << 32) | owner;
    if (__atomic_compare_exchange_n(&g_fork_gate, &state, next, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return;
  }
}

struct ExecutorLockGuard {
  ExecutorLockGuard() noexcept { executor_lock_acquire(); }
  ~ExecutorLockGuard() noexcept { executor_lock_release(); }
  ExecutorLockGuard(const ExecutorLockGuard &) = delete;
  ExecutorLockGuard &operator=(const ExecutorLockGuard &) = delete;
};

// Bind the process-wide executor to the calling thread. run/spawn/cancel are a
// single-threaded range by contract; a second thread using them is a terminal
// Panic instead of a silent data race. The first bind shares the fork gate
// with a fork reservation, so a fork can never check an unbound TID and then
// copy the address space after this thread binds.
void require_executor_thread(const char *where) noexcept {
  (void)where;
  const int32_t tid = current_tid();
  for (;;) {
    const int32_t bound = __atomic_load_n(&g_executor_tid, __ATOMIC_ACQUIRE);
    if (bound == tid)
      return;
    if (bound != 0)
      fail("async executor used from a second thread");
    fork_gate_acquire(tid);
    int32_t expected = 0;
    if (__atomic_compare_exchange_n(&g_executor_tid, &expected, tid, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      fork_gate_release(tid);
      return;
    }
    fork_gate_release(tid);
    if (__atomic_load_n(&g_executor_tid, __ATOMIC_ACQUIRE) != tid)
      fail("async executor used from a second thread");
    return;
  }
}

uint32_t load_status(const Future *future) noexcept {
  return __atomic_load_n(&future->status, __ATOMIC_RELAXED);
}

void store_status(Future *future, uint32_t status) noexcept {
  __atomic_store_n(&future->status, status, __ATOMIC_RELAXED);
}

void retain(Future *future) noexcept;
void release(Future *future) noexcept;

// ---------------------------------------------------------------------------
// Registry. Only pending futures are registered: a task joins it in publish_v2
// once its frame metadata is complete, a timer/descriptor future joins it after
// its own fields are initialized.
// ---------------------------------------------------------------------------

void registry_add(Future *future) noexcept {
  ExecutorLockGuard guard;
  future->next_all = g_all;
  g_all = future;
  if (future->kind == kKindTask)
    ++g_live_tasks;
}

bool registry_remove(Future *future) noexcept {
  ExecutorLockGuard guard;
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

// Take a reference only when the strong count is already non-zero, decided by a
// single atomic compare-exchange. This is the only safe way to acquire the
// first reference from a borrowed registry or run-queue pointer: a final
// release unlinks the node under the executor lock while the count is still
// one, then performs GC unregister, and only afterwards publishes zero. A
// separate "load non-zero, then increment" pair could still race a real final
// releaser and revive a node that is no longer in any table; the
// compare-exchange form never increments a count it observed as zero. Callers
// hold the executor lock, so the node storage stays valid until it is
// unlinked.
bool try_retain_nonzero(Future *future) noexcept {
  uint64_t refs = __atomic_load_n(&future->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (refs == 0)
      return false; // last owner already dropped it: never revive
    if (refs == ~uint64_t{0})
      fail("async future reference-count-overflow");
    if (__atomic_compare_exchange_n(&future->refs, &refs, refs + 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      return true;
    // The compare-exchange stored the current count in `refs`; re-evaluate it.
  }
}

// Safe traversal helpers for tables another thread may final-release entries
// from. The registry and run queue hold borrowed pointers, not references, so a
// walk must take a reference before it inspects a node. The final release
// unlinks under the same lock before the count can reach zero, so a zero count
// observed here is defensive garbage that must not be revived; nodes are freed
// only after their unlink (see release/destroy_future).
Future *next_live_locked(Future *future) noexcept {
  for (Future *next = future->next_all; next; next = next->next_all)
    if (try_retain_nonzero(next))
      return next;
  return nullptr;
}

Future *registry_first_retained() noexcept {
  ExecutorLockGuard guard;
  for (Future *first = g_all; first; first = first->next_all)
    if (try_retain_nonzero(first))
      return first;
  return nullptr;
}

Future *registry_next_retained(Future *future) noexcept {
  ExecutorLockGuard guard;
  return next_live_locked(future);
}

// Remove every executor-table link. The caller holds the executor lock. A
// Future is freed only after this unlink, so a concurrent walk holding the lock
// can still read a zero-count node's chain but never a freed node.
void unlink_executor_tables_locked(Future *future) noexcept {
  Future **link = &g_all;
  while (*link && *link != future)
    link = &(*link)->next_all;
  if (*link == future) {
    *link = future->next_all;
    if (future->kind == kKindTask && g_live_tasks)
      --g_live_tasks;
  }
  future->next_all = nullptr;
  if (future->flags & kQueued) {
    Future *previous = nullptr;
    Future *current = g_ready_head;
    while (current && current != future) {
      previous = current;
      current = current->next_ready;
    }
    if (current == future) {
      if (previous)
        previous->next_ready = current->next_ready;
      else
        g_ready_head = current->next_ready;
      if (g_ready_tail == current)
        g_ready_tail = previous;
    }
    future->next_ready = nullptr;
    future->flags &= ~kQueued;
  }
}

// ---------------------------------------------------------------------------
// Ready queue. Every future in the queue is pending and holds kActive; a
// future that completes or is destroyed is unlinked first so a queued node is
// always a live object.
// ---------------------------------------------------------------------------

void enqueue(Future *future) noexcept {
  if (!future || load_status(future) != kPending)
    return;
  ExecutorLockGuard guard;
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
  if (!future)
    return;
  ExecutorLockGuard guard;
  if (!(future->flags & kQueued))
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
  ExecutorLockGuard guard;
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

bool ready_empty() noexcept {
  ExecutorLockGuard guard;
  return g_ready_head == nullptr;
}

void clear_ready_queue() noexcept {
  ExecutorLockGuard guard;
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
// epoll descriptor. Only the executor thread waits in epoll, but a final
// release on another thread may deregister its own descriptor, so the
// descriptor word and the ctl call are serialized with shutdown's close.
// ---------------------------------------------------------------------------

int ensure_epoll_locked() noexcept {
  if (g_epoll >= 0) {
    g_epoll_needs_rebuild = 0;
    return g_epoll;
  }
  const auto fd = syscall_epoll_create1(kEpollCloexec);
  if (fd < 0)
    fail("epoll_create1");
  // A fork child only closes its copy of the inherited instance; it never sends
  // EPOLL_CTL_DEL to the parent's instance. Rebuild the private instance here
  // from the pending descriptor futures whose registration intent survived the
  // fork. Do not add futures that were never awaited/registered.
  if (g_epoll_needs_rebuild) {
    for (Future *future = g_all; future; future = future->next_all) {
      if (load_status(future) != kPending || future->kind != kKindFd ||
          !(future->flags & kFdRegistered))
        continue;
      EpollEvent event{};
      event.events = future->events ? future->events : kEpollIn;
      event.data = reinterpret_cast<uint64_t>(future);
      if (syscall_epoll_ctl(static_cast<int>(fd), kEpollCtlAdd, future->fd,
                            &event) < 0) {
        (void)lx::syscall6(kSysClose, fd, 0, 0, 0, 0, 0);
        fail("epoll_ctl rebuild");
      }
    }
    g_epoll_needs_rebuild = 0;
  }
  // Publish the fully populated descriptor only after every ADD succeeded.
  g_epoll = static_cast<int>(fd);
  return g_epoll;
}

int ensure_epoll() noexcept {
  ExecutorLockGuard guard;
  return ensure_epoll_locked();
}

void epoll_deregister(Future *future) noexcept {
  if (!future)
    return;
  ExecutorLockGuard guard;
  if (!(future->flags & kFdRegistered))
    return;
  if (g_epoll >= 0) {
    EpollEvent event{};
    (void)syscall_epoll_ctl(g_epoll, kEpollCtlDel, future->fd, &event);
  }
  // Before a deferred rebuild g_epoll is -1. Clearing the local intent is the
  // only change; the parent's inherited instance is never touched.
  future->flags &= ~kFdRegistered;
}

void register_fd(Future *future) noexcept {
  ExecutorLockGuard guard;
  const int epfd = ensure_epoll_locked();
  if (future->flags & kFdRegistered)
    return;
  EpollEvent event{};
  event.events = future->events ? future->events : kEpollIn;
  event.data = reinterpret_cast<uint64_t>(future);
  if (syscall_epoll_ctl(epfd, kEpollCtlAdd, future->fd, &event) < 0)
    fail("epoll_ctl");
  future->flags |= kFdRegistered;
}

// ---------------------------------------------------------------------------
// Reference counting and final release.
// ---------------------------------------------------------------------------

void destroy_future(Future *future) noexcept {
  // The normal final release unregistered the GC node while refs was still 1,
  // before publishing zero. This branch is a defensive fallback for any future
  // caller that reaches destroy with a registration still marked; it preserves
  // the pairing and keeps the registry consistent before typed callbacks.
  if (future->gc_registered) {
    future->gc_registered = 0;
    lang_gc_program_unregister_v1(future);
  }
  // The executor-table unlink already happened under the lock in release().
  // A future with zero references cannot be listed as a waiter or hold a parent
  // reference: each link owns a reference. Clear stale links defensively
  // without touching reference counts.
  epoll_deregister(future);
  future->waiting_on = nullptr;
  future->next_waiter = nullptr;
  future->waiters = nullptr;
  future->flags &= ~(kQueued | kWaiting | kActive | kSuspended | kParentRef);
  // Frame-owned values: idempotent, never resumes the coroutine and never runs
  // a source defer.
  if (future->frame_ops && future->frame_ops->clear_owners) {
    future->frame_ops->clear_owners(future->gc_state);
  } else if ((future->flags & kLegacyPromiseRelease) &&
             (future->flags & kStarted) && future->gc_state) {
    // publish_v1 compatibility: the old emitter passes the promise cleanup
    // callback here instead of typed frame metadata.
    reinterpret_cast<void (*)(void *)>(future->gc_state)(future->promise);
  }
  // The coroutine frame is destroyed synchronously after unregistering. A
  // frame is never deferred: no frame outlives the metadata that describes it.
  if (future->handle) {
    if (future->destroy)
      future->destroy(future->handle);
    future->handle = nullptr;
  }
  lang_free_v1(future);
}

// Plain retain is only for a caller that already owns a reference: a source
// Task handle, the executor's kActive root, an await link, or the collector
// pinning a registered candidate under a stopped world. The first reference
// taken from a borrowed registry/run-queue pointer must go through
// try_retain_nonzero, otherwise a zero-count node can be revived.
void retain(Future *future) noexcept {
  if (future)
    __atomic_add_fetch(&future->refs, 1, __ATOMIC_RELAXED);
}

void release(Future *future) noexcept {
  if (!future)
    return;
  for (;;) {
    uint64_t refs = __atomic_load_n(&future->refs, __ATOMIC_ACQUIRE);
    if (refs == 0)
      return; // already unreachable; only the final releaser may free
    if (refs > 1) {
      // Ordinary fast path: other owners/await links/active roots remain.
      if (__atomic_compare_exchange_n(&future->refs, &refs, refs - 1, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;
      continue;
    }
    // Possibly the last reference. Recheck and unlink every borrowed executor
    // pointer under the short lock while refs is still 1, so no table walk can
    // acquire a new reference after the unlink. Do not publish zero yet.
    {
      ExecutorLockGuard guard;
      if (__atomic_load_n(&future->refs, __ATOMIC_ACQUIRE) != 1)
        continue; // a table walk took a reference; retry from the fast path
      unlink_executor_tables_locked(future);
    }
    // refs remains 1 through the real GC unregister. A fork between the unlink
    // and the unregister may copy this object as a registered node with a
    // positive reference; that is a legal inherited reference from the final
    // releaser's vanished thread. It is never a registered zero-ref node.
    // Do not hold the executor lock here: fork order is GC -> async, and a GC
    // unregister may need the collector registry lock.
    if (future->gc_registered) {
      lang_gc_program_unregister_v1(future);
      // Publish "unregistered" only after the real unregister completed.
      future->gc_registered = 0;
    }
    // Only now is it safe to make the strong count zero and run frame cleanup.
    __atomic_store_n(&future->refs, 0, __ATOMIC_RELEASE);
    destroy_future(future);
    return;
  }
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
  if (!future || load_status(future) != kPending)
    return;
  // Local reference: detaching the await links and dropping the executor
  // reference can release the last owner, but this object must stay readable
  // until the bookkeeping below is finished. The collector also pins every
  // candidate before clearing, so a clear callback never frees under the
  // collector's snapshot.
  retain(future);
  // Publish the new status first: waiters observe it when they resume, and
  // enqueue() only accepts pending futures.
  store_status(future, status);
  future->flags &= ~kSuspended;
  ready_remove(future);
  epoll_deregister(future);
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
  if (!future || load_status(future) != kPending)
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

bool has_pending_tasks() noexcept {
  ExecutorLockGuard guard;
  return g_live_tasks != 0;
}

bool has_event_source() noexcept {
  bool found = false;
  Future *future = registry_first_retained();
  while (future) {
    Future *next = registry_next_retained(future);
    if (!found && load_status(future) == kPending &&
        (future->kind == kKindTimer ||
         (future->kind == kKindFd && (future->flags & kFdRegistered))))
      found = true;
    release(future);
    future = next;
  }
  return found;
}

int next_timeout_ms() noexcept {
  const uint64_t now = now_ns();
  bool found = false;
  uint64_t nearest = 0;
  Future *future = registry_first_retained();
  while (future) {
    Future *next = registry_next_retained(future);
    if (load_status(future) == kPending && future->kind == kKindTimer) {
      if (!found || future->deadline < nearest) {
        nearest = future->deadline;
        found = true;
      }
    }
    release(future);
    future = next;
  }
  if (!found)
    return -1;
  if (nearest <= now)
    return 0;
  const uint64_t delta = nearest - now;
  uint64_t ms = (delta + 999999) / 1000000;
  if (ms > 0x7fffffff)
    ms = 0x7fffffff;
  return static_cast<int>(ms);
}

bool expire_timers() noexcept {
  const uint64_t now = now_ns();
  bool expired = false;
  Future *future = registry_first_retained();
  while (future) {
    Future *next = registry_next_retained(future);
    if (load_status(future) == kPending && future->kind == kKindTimer &&
        future->deadline <= now) {
      complete(future, kDone);
      expired = true;
    }
    release(future);
    future = next;
  }
  return expired;
}

bool registry_has_pending() noexcept {
  bool found = false;
  Future *future = registry_first_retained();
  while (future) {
    Future *next = registry_next_retained(future);
    if (load_status(future) == kPending)
      found = true;
    release(future);
    future = next;
  }
  return found;
}

void run_one(Future *future) noexcept {
  if (!future || load_status(future) != kPending)
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
    if (until ? load_status(until) != kPending : !has_pending_tasks())
      return;
    if (Future *ready = dequeue()) {
      run_one(ready);
      continue;
    }
    // Re-evaluate the loop condition after timer completion: an awaited
    // deadline may have satisfied `until` even though a descriptor waiter is
    // still registered.
    const bool expired = expire_timers();
    if (expired || !ready_empty())
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
  future->gc_registered = 0;
  future->deadline = 0;
  future->handle = nullptr;
  future->resume = nullptr;
  future->destroy = nullptr;
  future->promise = nullptr;
  future->gc_state = nullptr;
  future->frame_ops = nullptr;
  future->next_ready = nullptr;
  future->next_waiter = nullptr;
  future->waiters = nullptr;
  future->next_all = nullptr;
  future->waiting_on = nullptr;
  // A timer or descriptor future has no frame metadata and no completion
  // handshake, so it is complete when constructed. The caller registers it once
  // its own fields are initialized. A task joins the registry in publish_v2,
  // after its slots, flags and metadata exist.
  return future;
}

// ---------------------------------------------------------------------------
// Future GC node.
// ---------------------------------------------------------------------------

void task_gc_trace(void *object, LangGcVisitor visitor, void *context) noexcept {
  auto *future = static_cast<Future *>(object);
  if (future->frame_ops && future->frame_ops->trace)
    future->frame_ops->trace(future->gc_state, visitor, context);
  // Runtime-owned await references are real ARC references and are reported by
  // the owner that holds them, exactly once.
  if (future->waiting_on)
    visitor(future->waiting_on, context);
  for (Future *waiter = future->waiters; waiter; waiter = waiter->next_waiter)
    visitor(waiter, context);
}

void task_gc_clear(void *object) noexcept {
  auto *future = static_cast<Future *>(object);
  // The collector may only reclaim a frame that the executor will never
  // resume: never-started, or already finished (normal, typed error, cancel)
  // with its source exit plan complete.
  if ((future->flags & kActive) ||
      ((future->flags & kStarted) && load_status(future) == kPending))
    fail("async gc clear on a live task");
  // Drop the values the frame and promise still own. Idempotent; must not
  // resume the coroutine or run a cancel exit plan or source defer.
  if (future->frame_ops && future->frame_ops->clear_owners)
    future->frame_ops->clear_owners(future->gc_state);
  if (load_status(future) == kPending) {
    // Never started: no user cleanup can run, so the common completion path
    // unlinks the registry, run-queue, waiter and epoll state. The collector
    // pinned this object, so it stays addressable for the post-clear trace.
    complete(future, kCancelled);
  }
}

void task_gc_retain(void *object) noexcept {
  retain(static_cast<Future *>(object));
}

void task_gc_release(void *object) noexcept {
  release(static_cast<Future *>(object));
}

const LangGcOps kTaskGcOps = {&task_gc_trace, &task_gc_clear, &task_gc_retain,
                              &task_gc_release, nullptr};

// A task becomes a registered graph node only when the frame reports typed
// strong-edge metadata and the program layer is enabled. The compiler must set
// gc_program_required for such a frame (see async.hpp).
void register_task_gc(Future *future) noexcept {
  if (!future->frame_ops || !future->frame_ops->trace)
    return; // provably leaf: no Wz strong edge can be owned
  if (!future->frame_ops->clear_owners)
    fail("async frame ops report edges without clear_owners");
  if (!lang::native::gc_program::enabled())
    return; // program compiled without a graph: no collector to join
  if (!lang_gc_program_register_v1(future, &kTaskGcOps))
    fail("async task gc registration");
  future->gc_registered = 1;
}

// Cancel every pending registry entry. Returns true when at least one pending
// future was seen. Each visited future and its registry successor are held by
// a local reference so a cancel that releases the last owner cannot free the
// traversal state.
bool cancel_pending_pass() noexcept {
  bool saw_pending = false;
  Future *future = registry_first_retained();
  while (future) {
    Future *next = registry_next_retained(future);
    if (load_status(future) == kPending) {
      saw_pending = true;
      cancel_future(future);
    }
    release(future);
    future = next;
  }
  return saw_pending;
}

// Last-resort teardown: complete anything that stayed pending after repeated
// cancel/drain passes so no registry node survives shutdown. The frame is then
// released by the normal destroy path without running a cancel exit plan.
void force_complete_pending() noexcept {
  Future *future = registry_first_retained();
  while (future) {
    Future *next = registry_next_retained(future);
    if (load_status(future) == kPending)
      complete(future, kCancelled);
    release(future);
    future = next;
  }
}

void publish_common(Future *future, void *handle, void *resume, void *destroy,
                    void *promise) noexcept {
  future->handle = handle;
  future->resume = reinterpret_cast<void (*)(void *)>(resume);
  future->destroy = reinterpret_cast<void (*)(void *)>(destroy);
  future->promise = promise;
}

} // namespace

extern "C" {

// Private fork hooks used by runtime/native/process.cpp. They are declared
// weakly there so a program that only links process does not pull this unit.
//
// reserve returns 0 when the calling thread now owns the fork gate and the
// executor is unbound or bound to this thread; -EBUSY when another thread owns
// the executor. A successful reservation must be paired with exactly one
// release (error path) or with prepare/parent or prepare/child.
int64_t lang_async_fork_reserve_v1(int32_t exec_only) noexcept {
  if (exec_only)
    return 0; // internal spawn: the child immediately execs, no task handoff
  const int32_t tid = current_tid();
  fork_gate_acquire(tid);
  const int32_t bound = __atomic_load_n(&g_executor_tid, __ATOMIC_ACQUIRE);
  if (bound != 0 && bound != tid) {
    fork_gate_release(tid);
    return kEbusy;
  }
  g_fork_parent_tid = tid;
  return 0;
}

void lang_async_fork_release_v1() noexcept {
  const int32_t tid = current_tid();
  if (fork_gate_owned_by(tid))
    fork_gate_release(tid);
}

void lang_async_fork_prepare_v1(int32_t exec_only) noexcept {
  const int32_t tid = current_tid();
  // The parent tid is needed by both branches: an exec-only child may still
  // retarget a same-thread executor without ever taking the internal lock.
  g_fork_parent_tid = tid;
  if (exec_only)
    return;
  if (!fork_gate_owned_by(tid))
    fail("async fork prepare without reservation");
  executor_lock_acquire();
  g_fork_executor_locked = 1;
}

void lang_async_fork_parent_v1(int32_t exec_only) noexcept {
  if (exec_only)
    return;
  if (g_fork_executor_locked) {
    g_fork_executor_locked = 0;
    executor_lock_release();
  }
}

void lang_async_fork_child_v1(int32_t exec_only) noexcept {
  // The child does not own the parent's ticket number or fork reservation.
  // Reset both ticket words and the gate, then retarget the executor only when
  // the fork came from that executor's own thread. An unbound executor stays
  // unbound; g_current is preserved because a fork from inside a coroutine body
  // returns to the same run_one() frame in the child.
  __atomic_store_n(&g_lock_next, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_lock_serving, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_fork_gate, uint64_t{0}, __ATOMIC_RELAXED);
  g_fork_executor_locked = 0;
  const int32_t child_tid = current_tid();
  const int32_t old_tid = __atomic_load_n(&g_executor_tid, __ATOMIC_RELAXED);
  if (old_tid != 0 && old_tid == g_fork_parent_tid)
    __atomic_store_n(&g_executor_tid, child_tid, __ATOMIC_RELEASE);
  g_fork_parent_tid = 0;
  if (!exec_only) {
    // Never send EPOLL_CTL_DEL/ADD to the inherited instance: it is the parent's
    // kernel object. Close only this child's descriptor and defer a private
    // EPOLL_CLOEXEC rebuild to the next ensure_epoll().
    const int inherited = g_epoll;
    if (inherited >= 0) {
      (void)lx::syscall6(kSysClose, inherited, 0, 0, 0, 0, 0);
      g_epoll = -1;
      g_epoll_needs_rebuild = 1;
    }
  }
  // Exec-only: the child runs dup2/exec/error-pipe/_exit. Its epoll copy is
  // harmless (EPOLL_CLOEXEC and _exit close it); do not close an fd number that
  // another thread could have changed during the unlocked fork.
}

// Direct libc fork cannot return a Source os.Error from a void atfork prepare.
// Refuse the unsupported foreign-executor call with a terminal, recorded Panic
// instead of silently copying an executor owned by a thread that will not
// exist in the child.
[[noreturn]] void lang_async_fork_refuse_v1() noexcept {
  static constexpr char kMarker[] =
      "LANG_ASYNC_FORK_REFUSED:executor-on-another-thread\n";
  const auto *bytes = reinterpret_cast<const char *>(kMarker);
  long written = 0;
  while (written < static_cast<long>(sizeof(kMarker) - 1)) {
    const long result = lx::syscall6(
        1, 2, reinterpret_cast<long>(bytes + written),
        static_cast<long>(sizeof(kMarker) - 1) - written, 0, 0, 0);
    if (result == kEintr)
      continue;
    if (result <= 0)
      break;
    written += result;
  }
  lang_native_panic_v1(10, 0, 0, 0);
}

void *lang_async_task_new_v1() noexcept { return new_future(kKindTask); }

void lang_async_publish_v1(void *raw, void *handle, void *resume, void *destroy,
                           void *promise, void *release_promise) noexcept {
  auto *future = static_cast<Future *>(raw);
  if (!future || future->kind != kKindTask)
    fail("publish on non-task future");
  if (future->handle || future->frame_ops ||
      (future->flags & kLegacyPromiseRelease))
    fail("task published twice");
  publish_common(future, handle, resume, destroy, promise);
  // Pre-GC compatibility: no metadata, no registration. `gc_state` holds the
  // legacy promise cleanup callback.
  future->gc_state = release_promise;
  future->frame_ops = nullptr;
  future->flags |= kLegacyPromiseRelease;
  registry_add(future);
}

void lang_async_publish_v2(void *raw, void *handle,
                           void (*resume)(void *) noexcept,
                           void (*destroy)(void *) noexcept, void *promise,
                           void *gc_state,
                           const LangAsyncFrameGcOps *frame_ops) noexcept {
  auto *future = static_cast<Future *>(raw);
  if (!future || future->kind != kKindTask)
    fail("publish on non-task future");
  if (future->handle || future->frame_ops ||
      (future->flags & kLegacyPromiseRelease))
    fail("task published twice");
  publish_common(future, handle, reinterpret_cast<void *>(resume),
                 reinterpret_cast<void *>(destroy), promise);
  future->gc_state = gc_state;
  future->frame_ops = frame_ops;
  // Register before joining the executor registry so shutdown can never
  // observe a published task that the collector does not know about.
  register_task_gc(future);
  registry_add(future);
}

void lang_async_task_retain_v1(void *raw) noexcept {
  retain(static_cast<Future *>(raw));
}

void lang_async_task_release_v1(void *raw) noexcept {
  release(static_cast<Future *>(raw));
}

void lang_async_spawn_v1(void *raw) noexcept {
  require_executor_thread("spawn");
  auto *future = static_cast<Future *>(raw);
  if (!future || future->kind != kKindTask)
    fail("spawn on non-task future");
  if (load_status(future) != kPending)
    return;
  if (future->flags & (kActive | kQueued | kStarted))
    return;
  retain(future);
  future->flags |= kActive;
  enqueue(future);
}

uint32_t lang_async_status_v1(void *raw) noexcept {
  auto *future = static_cast<Future *>(raw);
  return future ? load_status(future) : kCancelled;
}

void *lang_async_promise_v1(void *raw) noexcept {
  auto *future = static_cast<Future *>(raw);
  return future ? future->promise : nullptr;
}

uint32_t lang_async_finished_v1(void *raw) noexcept {
  return lang_async_status_v1(raw) != kPending;
}

void lang_async_cancel_v1(void *raw) noexcept {
  require_executor_thread("cancel");
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
  if (load_status(target) != kPending)
    return load_status(target);
  if (target == awaiter)
    return kCancelled;
  // Awaiting a task that was created but never spawned starts it implicitly;
  // this keeps nested `await f()` useful without an explicit spawn.
  if (target->kind == kKindTask &&
      !(target->flags & (kStarted | kQueued))) {
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
  registry_add(future);
  return future;
}

void *lang_async_wait_fd_v1(int32_t fd, int32_t events) noexcept {
  if (fd < 0)
    fail("wait_fd requires a non-negative descriptor");
  auto *future = new_future(kKindFd);
  future->fd = fd;
  future->events = static_cast<uint32_t>(events);
  registry_add(future);
  return future;
}

void lang_async_run_v1() noexcept {
  require_executor_thread("run");
  run_loop(nullptr);
}

void lang_async_run_until_v1(void *raw) noexcept {
  require_executor_thread("run_until");
  run_loop(static_cast<Future *>(raw));
}

void lang_async_shutdown_v1() noexcept {
  require_executor_thread("shutdown");
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
  {
    ExecutorLockGuard guard;
    if (g_epoll >= 0) {
      (void)lx::syscall6(3 /* close */, g_epoll, 0, 0, 0, 0, 0);
      g_epoll = -1;
    }
    g_epoll_needs_rebuild = 0;
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
