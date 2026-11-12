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

#include "linux.hpp"

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

void registry_add(Future *future) noexcept {
  future->next_all = g_all;
  g_all = future;
  if (future->kind == kKindTask)
    ++g_live_tasks;
}

void registry_remove(Future *future) noexcept {
  Future **link = &g_all;
  while (*link && *link != future)
    link = &(*link)->next_all;
  if (*link == future)
    *link = future->next_all;
  future->next_all = nullptr;
  if (future->kind == kKindTask && g_live_tasks)
    --g_live_tasks;
}

void enqueue(Future *future) noexcept {
  if (future->status != kPending)
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

void destroy_future(Future *future) noexcept {
  if (future->flags & kFdRegistered) {
    EpollEvent event{};
    (void)syscall_epoll_ctl(g_epoll, kEpollCtlDel, future->fd, &event);
    future->flags &= ~kFdRegistered;
  }
  if (future->handle) {
    // The promise is valid only after the first resume initialized it.
    if ((future->flags & kStarted) && future->release_promise)
      future->release_promise(future->promise);
    future->destroy(future->handle);
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

void complete(Future *future, uint32_t status) noexcept {
  if (!future || future->status != kPending)
    return;
  future->status = status;
  future->flags &= ~(kQueued | kWaiting | kSuspended);
  if (future->flags & kFdRegistered) {
    EpollEvent event{};
    (void)syscall_epoll_ctl(g_epoll, kEpollCtlDel, future->fd, &event);
    future->flags &= ~kFdRegistered;
  }
  registry_remove(future);
  Future *waiter = future->waiters;
  future->waiters = nullptr;
  while (waiter) {
    Future *next = waiter->next_waiter;
    waiter->next_waiter = nullptr;
    waiter->waiting_on = nullptr;
    enqueue(waiter);
    waiter = next;
  }
  if (future->flags & kActive) {
    future->flags &= ~kActive;
    release(future);
  }
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
    // released without executing the body.
    complete(future, kCancelled);
    return;
  }
  if (future->flags & kSuspended)
    enqueue(future);
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
  future->flags |= kStarted;
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
    int count = static_cast<int>(
        syscall_epoll_wait(epfd, events, 16, next_timeout_ms()));
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
  auto *child = static_cast<Future *>(raw);
  if (!child)
    return kCancelled;
  if (!g_current)
    return kCancelled;
  if (g_current->flags & kCancelledFlag)
    return kCancelled;
  if (child->status != kPending)
    return child->status;
  if (child == g_current)
    return kCancelled;
  // Awaiting a task that was created but never spawned starts it implicitly;
  // this keeps nested `await f()` useful without an explicit spawn.
  if (child->kind == kKindTask && !(child->flags & (kStarted | kQueued))) {
    retain(child);
    child->flags |= kActive;
    enqueue(child);
  }
  if (child->kind == kKindFd)
    register_fd(child);
  if (g_current->waiting_on == child)
    return kPending;
  g_current->next_waiter = child->waiters;
  child->waiters = g_current;
  g_current->waiting_on = child;
  g_current->flags |= kWaiting;
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
  Future *f = g_all;
  while (f) {
    Future *next = f->next_all;
    if (f->status == kPending)
      cancel_future(f);
    f = next;
  }
  run_loop(nullptr);
  if (g_epoll >= 0) {
    (void)lx::syscall6(3 /* close */, g_epoll, 0, 0, 0, 0, 0);
    g_epoll = -1;
  }
  g_ready_head = g_ready_tail = nullptr;
  g_current = nullptr;
}

void *lang_async_frame_alloc_v1(uint64_t size) noexcept {
  return lang_alloc_v1(size, 16);
}

void lang_async_frame_free_v1(void *memory) noexcept { lang_free_v1(memory); }

} // extern "C"

#endif // x86_64 Linux
