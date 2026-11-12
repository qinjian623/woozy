#include "gc_world.hpp"

#include "alloc.hpp"
#include "linux.hpp"

#if !defined(__x86_64__) || !defined(__linux__)
#error "lang-gc-world/1 is implemented only for Linux x86_64"
#endif

// lang-gc-world/1. See tests/native/gc_world/CONTRACT.md for the protocol and
// for the integration work that this translation unit deliberately does not
// perform. Nothing here uses libc, libstdc++, pthreads, TLS, or dynamic
// initialization. Registry nodes come from lang_alloc_v1 so the runtime can be
// linked into the freestanding native program.

namespace {

using lang::native::linux_x86_64::syscall6;

constexpr uint32_t kRequestBit = LANG_GC_WORLD_REQUEST_BIT;
constexpr uint32_t kStateActive = 0;
constexpr uint32_t kStateSafe = 1;
constexpr uint32_t kStateParked = 2;

constexpr long kSysGettid = 186;
constexpr long kSysFutex = 202;
constexpr long kSysClockGettime = 228;

constexpr int kClockMonotonic = 1;
constexpr int kFutexWaitPrivate = 128;
constexpr int kFutexWakePrivate = 129;
constexpr int kFutexWaitBitsetPrivate = 9 | 128;
constexpr uint32_t kFutexBitsetAny = 0xffffffffu;

constexpr int kEintr = -4;
constexpr int kEagain = -11;
constexpr int kEtimedout = -110;
constexpr int kEbusy = -16;
constexpr int kEinval = -22;

struct Node {
  Node *next;
  int32_t tid;
  uint32_t refs;
  uint32_t state;
  uint32_t safe_depth;
};

struct KernelTimespec {
  long tv_sec;
  long tv_nsec;
};

struct Deadline {
  bool infinite;
  KernelTimespec when;
};

// Registry nodes are protected by this ticket lock. The lock never spans an
// allocation, a free, a futex wait, or a user callback. Node fields other than
// refs are accessed only while it is held. refs is atomic so a lookup can keep
// a node alive after the registry reference is dropped.
Node *g_nodes = nullptr;
uint32_t g_lock_next = 0;
uint32_t g_lock_serving = 0;

// Futex word for state changes. Parked threads and the collector wait on this;
// every relevant transition increments it and wakes all waiters.
uint32_t g_epoch = 0;

uint32_t g_stop_active = 0;
int32_t g_collector_tid = 0;

uint32_t g_fork_prepared = 0;
int32_t g_fork_tid = 0;

} // namespace

extern "C" {
uint32_t lang_gc_world_request_word_v1 = 0;
}

namespace {

inline void cpu_pause() noexcept {
  __asm__ volatile("pause" ::: "memory");
}

void registry_lock() noexcept {
  const uint32_t ticket =
      __atomic_fetch_add(&g_lock_next, 1u, __ATOMIC_RELAXED);
  while (__atomic_load_n(&g_lock_serving, __ATOMIC_ACQUIRE) != ticket)
    cpu_pause();
}

void registry_unlock() noexcept {
  __atomic_fetch_add(&g_lock_serving, 1u, __ATOMIC_RELEASE);
}

int64_t raw_current_tid() noexcept {
  return syscall6(kSysGettid, 0, 0, 0, 0, 0, 0);
}

void write_all(int fd, const char *bytes, uint64_t size) noexcept {
  while (size) {
    const int64_t result =
        syscall6(1 /* SYS_write */, fd, reinterpret_cast<long>(bytes),
                 static_cast<long>(size), 0, 0, 0);
    if (result == kEintr)
      continue;
    if (result <= 0)
      return;
    bytes += result;
    size -= static_cast<uint64_t>(result);
  }
}

// Terminal Panic that never depends on gettid succeeding. The tid argument is
// 0 when the caller could not obtain one (for example, a seccomp filter denied
// gettid). Keeping this helper separate avoids recursion through gc_panic.
[[noreturn]] __attribute__((no_stack_protector)) void
gc_panic_with_tid(const char *reason, int64_t tid) noexcept {
  char record[192];
  uint64_t length = 0;
  const auto append = [&](const char *text) {
    while (*text && length < sizeof(record) - 1)
      record[length++] = *text++;
  };
  const auto append_unsigned = [&](uint64_t value) {
    char digits[24];
    uint64_t count = 0;
    if (value == 0) {
      digits[count++] = '0';
    } else {
      while (value) {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
      }
    }
    while (count && length < sizeof(record) - 1)
      record[length++] = digits[--count];
  };

  append("LANG_NATIVE_PANIC:v1:gc-world:");
  append(reason);
  append(":");
  append_unsigned(tid > 0 ? static_cast<uint64_t>(tid) : 0u);
  append(":0:0\n");
  write_all(2, record, length);
  syscall6(231 /* SYS_exit_group */, 101, 0, 0, 0, 0, 0);
  __builtin_unreachable();
}

// General terminal Panic. It may attempt a raw gettid for diagnostic context,
// but a failed gettid is reported as tid 0 and never traps or calls back into
// current_tid.
[[noreturn]] __attribute__((no_stack_protector)) void
gc_panic(const char *reason) noexcept {
  const int64_t tid = raw_current_tid();
  gc_panic_with_tid(reason, tid > 0 ? tid : 0);
}

int32_t current_tid() noexcept {
  const int64_t tid = raw_current_tid();
  if (tid <= 0)
    gc_panic_with_tid("gettid-failed", 0);
  return static_cast<int32_t>(tid);
}

bool request_active_locked() noexcept {
  return (__atomic_load_n(&lang_gc_world_request_word_v1,
                          __ATOMIC_ACQUIRE) &
          kRequestBit) != 0;
}

bool stop_active_locked() noexcept { return g_stop_active != 0; }

bool is_active_collector_locked(int32_t tid) noexcept {
  return g_stop_active != 0 && g_collector_tid == tid;
}

Node *find_locked(int32_t tid) noexcept {
  for (Node *node = g_nodes; node; node = node->next)
    if (node->tid == tid)
      return node;
  return nullptr;
}

Node *find_and_acquire(int32_t tid) noexcept {
  registry_lock();
  Node *node = find_locked(tid);
  if (node)
    __atomic_add_fetch(&node->refs, 1u, __ATOMIC_RELAXED);
  registry_unlock();
  return node;
}

void destroy_node(Node *node) noexcept { lang_free_v1(node); }

void node_release(Node *node) noexcept {
  if (__atomic_sub_fetch(&node->refs, 1u, __ATOMIC_ACQ_REL) == 0)
    destroy_node(node);
}

Node *allocate_node(int32_t tid) noexcept {
  void *storage = lang_alloc_v1(sizeof(Node), alignof(Node));
  if (!storage)
    gc_panic("allocation-failed");
  auto *node = static_cast<Node *>(storage);
  node->next = nullptr;
  node->tid = tid;
  // One reference belongs to the registry, one to the registering call.
  __atomic_store_n(&node->refs, 2u, __ATOMIC_RELAXED);
  node->state = kStateActive;
  node->safe_depth = 0;
  return node;
}

void notify_waiters() noexcept {
  __atomic_add_fetch(&g_epoch, 1u, __ATOMIC_RELEASE);
  for (;;) {
    const int64_t result =
        syscall6(kSysFutex, reinterpret_cast<long>(&g_epoch),
                 kFutexWakePrivate, 0x7fffffff, 0, 0, 0);
    if (result >= 0)
      return; // The value is the number woken, including a valid zero.
    if (result == kEintr)
      continue;
    // A failed wake means PARKED waiters may never observe the epoch change.
    // Returning would let resume/stop claim recovery without waking them.
    gc_panic("futex-wake-failed");
  }
}

void make_deadline(int64_t timeout_ns, Deadline *deadline) noexcept {
  deadline->infinite = false;
  KernelTimespec now{0, 0};
  const int64_t clock_result =
      syscall6(kSysClockGettime, kClockMonotonic,
               reinterpret_cast<long>(&now), 0, 0, 0, 0);
  if (clock_result < 0)
    gc_panic("clock-failed");

  int64_t seconds = now.tv_sec + timeout_ns / 1000000000LL;
  int64_t nanoseconds = now.tv_nsec + timeout_ns % 1000000000LL;
  if (nanoseconds >= 1000000000LL) {
    nanoseconds -= 1000000000LL;
    seconds += 1;
  }
  deadline->when.tv_sec = seconds;
  deadline->when.tv_nsec = nanoseconds;
}

// Returns false only on an expired absolute deadline. EINTR and EAGAIN retry
// without recomputing the deadline; this is why a signal storm cannot extend a
// positive stop timeout.
bool wait_on_epoch(uint32_t expected, const Deadline &deadline) noexcept {
  for (;;) {
    int result;
    if (deadline.infinite) {
      result = static_cast<int>(syscall6(
          kSysFutex, reinterpret_cast<long>(&g_epoch), kFutexWaitPrivate,
          expected, 0, 0, 0));
    } else {
      result = static_cast<int>(syscall6(
          kSysFutex, reinterpret_cast<long>(&g_epoch),
          kFutexWaitBitsetPrivate, expected,
          reinterpret_cast<long>(&deadline.when), 0, kFutexBitsetAny));
    }
    if (result == 0 || result == kEagain)
      return true;
    if (result == kEintr)
      continue;
    if (result == kEtimedout)
      return false;
    gc_panic("futex-failed");
  }
}

// The caller holds a node reference. It must be PARKED or about to become
// PARKED. Becoming ACTIVE again is serialized by the registry lock with the
// collector's scan, so a resume cannot race a new stop into an active window.
void wait_until_resumed(Node *node) noexcept {
  const Deadline infinite{true, {0, 0}};
  for (;;) {
    registry_lock();
    const bool exempt =
        !request_active_locked() || is_active_collector_locked(node->tid);
    if (exempt) {
      if (node->state == kStateParked)
        node->state = kStateActive;
      registry_unlock();
      return;
    }
    const uint32_t epoch =
        __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
    registry_unlock();
    (void)wait_on_epoch(epoch, infinite);
  }
}

bool all_stopped_locked(int32_t collector_tid) noexcept {
  for (Node *node = g_nodes; node; node = node->next) {
    if (node->tid == collector_tid)
      continue;
    if (node->state == kStateActive)
      return false;
    if (node->state == kStateSafe && node->safe_depth == 0)
      gc_panic("state-corruption");
    if (node->state != kStateSafe && node->state != kStateParked)
      gc_panic("state-corruption");
  }
  return true;
}

void retract_stop(int32_t owner_tid) noexcept {
  bool changed = false;
  registry_lock();
  if (g_stop_active && g_collector_tid == owner_tid) {
    g_stop_active = 0;
    g_collector_tid = 0;
    __atomic_store_n(&lang_gc_world_request_word_v1, 0u, __ATOMIC_RELEASE);
    changed = true;
  }
  registry_unlock();
  if (changed)
    notify_waiters();
}

} // namespace

extern "C" int lang_gc_world_request_pending_v1(void) noexcept {
  return lang_gc_world_poll_required_v1();
}

extern "C" bool lang_gc_world_register_v1(void) noexcept {
  const int32_t tid = current_tid();
  for (;;) {
    bool wait = false;
    bool notify = false;

    registry_lock();
    Node *existing = find_locked(tid);
    if (existing) {
      const bool requested = request_active_locked();
      const bool collector = is_active_collector_locked(tid);
      if (requested && !collector && existing->state == kStateActive) {
        existing->state = kStateParked;
        wait = true;
        notify = true;
      } else if (requested && !collector &&
                 existing->state == kStateParked) {
        wait = true;
      } else if (existing->state == kStateParked && !requested) {
        registry_unlock();
        gc_panic("register-parked");
      }
      if (wait)
        __atomic_add_fetch(&existing->refs, 1u, __ATOMIC_RELAXED);
      registry_unlock();
      if (notify)
        notify_waiters();
      if (wait) {
        wait_until_resumed(existing);
        node_release(existing);
      }
      return false;
    }
    registry_unlock();

    Node *fresh = allocate_node(tid);

    registry_lock();
    existing = find_locked(tid);
    if (existing) {
      registry_unlock();
      // Not published, so no reference protocol is needed.
      destroy_node(fresh);
      continue;
    }
    const bool park = request_active_locked();
    fresh->state = park ? kStateParked : kStateActive;
    fresh->next = g_nodes;
    g_nodes = fresh;
    registry_unlock();

    if (park) {
      notify_waiters();
      wait_until_resumed(fresh);
    }
    node_release(fresh); // Drop the registering call's local reference.
    return true;
  }
}

extern "C" bool lang_gc_world_unregister_v1(void) noexcept {
  const int32_t tid = current_tid();
  Node *node = find_and_acquire(tid);
  if (!node)
    return false;

  bool removed = false;
  bool notify = false;
  registry_lock();
  // Only this thread can remove its own node, but a signal handler or a
  // deliberately re-entered call could have raced it. Verify membership.
  Node **link = &g_nodes;
  while (*link && *link != node)
    link = &(*link)->next;
  if (*link == node) {
    if (is_active_collector_locked(tid)) {
      registry_unlock();
      node_release(node);
      gc_panic("unregister-collector");
    }
    if (node->safe_depth != 0) {
      registry_unlock();
      node_release(node);
      gc_panic("unregister-in-safe");
    }
    if (node->state == kStateParked) {
      registry_unlock();
      node_release(node);
      gc_panic("unregister-parked");
    }
    *link = node->next;
    node->next = nullptr;
    removed = true;
    notify = request_active_locked();
  }
  registry_unlock();

  if (notify)
    notify_waiters();
  node_release(node); // Lookup reference.
  if (removed)
    node_release(node); // Registry reference.
  return removed;
}

extern "C" void lang_gc_world_poll_v1(void) noexcept {
  if (!lang_gc_world_poll_required_v1())
    return;

  const int32_t tid = current_tid();
  Node *node = find_and_acquire(tid);
  if (!node)
    gc_panic("poll-unregistered");

  bool park = false;
  bool notify = false;
  registry_lock();
  if (!request_active_locked()) {
    registry_unlock();
    node_release(node);
    return;
  }
  if (is_active_collector_locked(tid) || node->state == kStateSafe) {
    registry_unlock();
    node_release(node);
    return;
  }
  if (node->state == kStateActive) {
    node->state = kStateParked;
    park = true;
    notify = true;
  } else if (node->state == kStateParked) {
    park = true;
  } else {
    registry_unlock();
    node_release(node);
    gc_panic("state-corruption");
  }
  registry_unlock();

  if (notify)
    notify_waiters();
  if (park)
    wait_until_resumed(node);
  node_release(node);
}

extern "C" void lang_gc_world_enter_safe_v1(void) noexcept {
  const int32_t tid = current_tid();
  Node *node = find_and_acquire(tid);
  if (!node)
    gc_panic("safe-unregistered");

  bool notify = false;
  registry_lock();
  if (node->safe_depth == 0xffffffffu) {
    registry_unlock();
    node_release(node);
    gc_panic("safe-depth-overflow");
  }
  ++node->safe_depth;
  if (node->safe_depth == 1) {
    if (node->state == kStateParked) {
      registry_unlock();
      node_release(node);
      gc_panic("safe-while-parked");
    }
    if (node->state != kStateSafe) {
      node->state = kStateSafe;
      notify = request_active_locked();
    }
  }
  registry_unlock();

  if (notify)
    notify_waiters();
  node_release(node);
}

extern "C" void lang_gc_world_leave_safe_v1(void) noexcept {
  const int32_t tid = current_tid();
  Node *node = find_and_acquire(tid);
  if (!node)
    gc_panic("safe-unregistered");

  bool park = false;
  bool notify = false;
  registry_lock();
  if (node->safe_depth == 0) {
    registry_unlock();
    node_release(node);
    gc_panic("leave-without-enter");
  }
  --node->safe_depth;
  if (node->safe_depth == 0) {
    const bool requested = request_active_locked();
    if (requested && !is_active_collector_locked(tid)) {
      if (node->state != kStateSafe) {
        registry_unlock();
        node_release(node);
        gc_panic("state-corruption");
      }
      node->state = kStateParked;
      park = true;
      notify = true;
    } else {
      node->state = kStateActive;
    }
  } else if (node->state != kStateSafe) {
    registry_unlock();
    node_release(node);
    gc_panic("state-corruption");
  }
  registry_unlock();

  if (notify)
    notify_waiters();
  if (park)
    wait_until_resumed(node);
  node_release(node);
}

extern "C" int64_t lang_gc_world_stop_v1(int64_t timeout_ns) noexcept {
  if (timeout_ns < -1)
    return kEinval;

  const int32_t tid = current_tid();
  Deadline deadline{true, {0, 0}};
  if (timeout_ns > 0)
    make_deadline(timeout_ns, &deadline);

  Node *self = find_and_acquire(tid);
  if (!self)
    gc_panic("stop-unregistered");

  bool notify = false;
  registry_lock();
  if (stop_active_locked()) {
    registry_unlock();
    node_release(self);
    return kEbusy;
  }
  if (self->safe_depth != 0) {
    registry_unlock();
    node_release(self);
    gc_panic("stop-in-safe");
  }
  if (self->state == kStateParked) {
    registry_unlock();
    node_release(self);
    gc_panic("stop-parked");
  }
  g_stop_active = 1;
  g_collector_tid = tid;
  __atomic_store_n(&lang_gc_world_request_word_v1, kRequestBit,
                   __ATOMIC_RELEASE);
  notify = true;
  registry_unlock();

  if (notify)
    notify_waiters();

  for (;;) {
    registry_lock();
    // Read the epoch before the scan, while holding the lock. State changes
    // use the same lock and notify after releasing it. If a change happens
    // after this read, its epoch increment makes the futex wait below return
    // EAGAIN instead of sleeping through an already-satisfied condition.
    const uint32_t epoch =
        __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
    const bool stopped = all_stopped_locked(tid);
    registry_unlock();

    if (stopped) {
      node_release(self);
      return 0;
    }
    if (timeout_ns == 0) {
      retract_stop(tid);
      node_release(self);
      return kEtimedout;
    }
    if (!wait_on_epoch(epoch, deadline)) {
      retract_stop(tid);
      node_release(self);
      return kEtimedout;
    }
  }
}

extern "C" void lang_gc_world_resume_v1(void) noexcept {
  const int32_t tid = current_tid();
  Node *node = find_and_acquire(tid);
  if (!node)
    gc_panic("resume-unregistered");

  registry_lock();
  if (!stop_active_locked() || g_collector_tid != tid) {
    registry_unlock();
    node_release(node);
    gc_panic("resume-not-owner");
  }
  g_stop_active = 0;
  g_collector_tid = 0;
  __atomic_store_n(&lang_gc_world_request_word_v1, 0u, __ATOMIC_RELEASE);
  if (node->state == kStateParked)
    node->state = kStateActive;
  registry_unlock();

  notify_waiters();
  node_release(node);
}

extern "C" void lang_gc_world_fork_prepare_v1(void) noexcept {
  const int32_t tid = current_tid();
  registry_lock();
  Node *node = find_locked(tid);
  if (!node) {
    registry_unlock();
    gc_panic("fork-unregistered");
  }
  if (g_fork_prepared) {
    registry_unlock();
    gc_panic("fork-hook-misuse");
  }
  if (stop_active_locked() && g_collector_tid != tid) {
    registry_unlock();
    gc_panic("fork-active-stop");
  }
  if (node->state == kStateParked) {
    registry_unlock();
    gc_panic("fork-parked");
  }
  g_fork_prepared = 1;
  g_fork_tid = tid;
  // The registry lock stays held across the caller's fork().
}

extern "C" void lang_gc_world_fork_parent_v1(void) noexcept {
  const int32_t tid = current_tid();
  // The lock is already held by prepare. On correct usage tid is still the
  // forking thread and no state change is needed.
  if (!g_fork_prepared || g_fork_tid != tid)
    gc_panic("fork-hook-misuse");
  g_fork_prepared = 0;
  g_fork_tid = 0;
  registry_unlock();
}

extern "C" void lang_gc_world_fork_child_v1(void) noexcept {
  const int32_t child_tid = current_tid();
  if (!g_fork_prepared)
    gc_panic("fork-hook-misuse");
  // In the child gettid is the new child tid. If it is still the parent tid,
  // this is not a fork child and the caller should have used the parent hook.
  if (g_fork_tid == child_tid)
    gc_panic("fork-hook-misuse");

  const int32_t parent_tid = g_fork_tid;
  Node *keep = nullptr;
  Node *free_list = nullptr;
  for (Node *node = g_nodes; node;) {
    Node *next = node->next;
    if (!keep && node->tid == parent_tid) {
      keep = node;
      keep->next = nullptr;
    } else {
      node->next = free_list;
      free_list = node;
    }
    node = next;
  }
  if (!keep) {
    // No lock reset because the process is about to terminate.
    gc_panic("fork-no-current");
  }

  // Only the surviving thread is registered. Its old parent tid was valid only
  // in the parent; the child has a new kernel tid.
  keep->tid = child_tid;
  __atomic_store_n(&keep->refs, 1u, __ATOMIC_RELAXED);
  if (keep->safe_depth == 0)
    keep->state = kStateActive;
  else
    keep->state = kStateSafe;
  g_nodes = keep;

  g_stop_active = 0;
  g_collector_tid = 0;
  g_fork_prepared = 0;
  g_fork_tid = 0;
  __atomic_store_n(&lang_gc_world_request_word_v1, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_epoch, 0u, __ATOMIC_RELEASE);

  // The lock was held by prepare and inherited. There are no other threads in
  // this address space, so reset rather than release the inherited ticket.
  __atomic_store_n(&g_lock_next, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_lock_serving, 0u, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_SEQ_CST);

  // Freed only after the registry lock is gone. The allocator fork-child hook
  // must already have reset the allocator's own lock.
  while (free_list) {
    Node *next = free_list->next;
    destroy_node(free_list);
    free_list = next;
  }
  // keep holds the single registry reference and must be unregistered later.
}
