#include "sync.hpp"

#include <cstddef>
#include <cstdint>

#include "alloc.hpp"

#if defined(__x86_64__) && defined(__linux__)

// Actual runtime Panic entry from panic.cpp. It is terminal and reports the
// cause plus a source span; synchronization misuse has no source span.
extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t cause,
                                                  uint64_t snapshot,
                                                  uint64_t begin,
                                                  uint64_t end) noexcept;

namespace {

// Local Linux x86_64 syscall numbers, keeping kernel/libc headers out of the
// freestanding runtime.
constexpr long kSysFutex = 202;
constexpr long kSysGettid = 186;
constexpr long kFutexWait = 128; // FUTEX_WAIT_PRIVATE
constexpr long kFutexWake = 129; // FUTEX_WAKE_PRIVATE
constexpr long kErrIntr = -4;

// Mutex word values. A waiter always leaves the word at 2 while it sleeps, so
// an unlock that observes anything other than 1 must wake a sleeper.
constexpr uint32_t kMutexFree = 0;
constexpr uint32_t kMutexHeld = 1;
constexpr uint32_t kMutexContended = 2;

[[noreturn]] inline void misuse_panic() noexcept {
  lang_handle_panic_v1();
}

// Allocation failure uses the existing runtime cause (5).
[[noreturn]] inline void allocation_panic() noexcept {
  lang_native_panic_v1(5, 0, 0, 0);
}

inline uint32_t current_tid() noexcept {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(kSysGettid)
                   : "rcx", "r11", "memory", "cc");
  if (result <= 0) misuse_panic();
  return static_cast<uint32_t>(result);
}

// futex(uaddr, op, val, timeout, uaddr2, val3). The timeout, second address
// and third value are always null/zero here, but x86_64 still requires their
// registers.
inline long sys_futex(uint32_t *address, long operation,
                      uint32_t value) noexcept {
  register long r10 __asm__("r10") = 0;
  register long r8 __asm__("r8") = 0;
  register long r9 __asm__("r9") = 0;
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(kSysFutex), "D"(address), "S"(operation),
                     "d"(static_cast<long>(value)), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory", "cc");
  return result;
}

// Restart only on EINTR. EAGAIN means the futex word changed before sleep and
// the caller's predicate loop must re-check it.
inline void futex_wait(uint32_t *address, uint32_t expected) noexcept {
  for (;;) {
    const long result = sys_futex(address, kFutexWait, expected);
    if (result >= 0 || result == -11) return;
    if (result != kErrIntr) misuse_panic();
  }
}

inline void futex_wake(uint32_t *address, uint32_t count) noexcept {
  if (sys_futex(address, kFutexWake, count) < 0) misuse_panic();
}

inline void cpu_relax() noexcept {
  __asm__ volatile("pause" ::: "memory");
}

// A notifier keeps this lock through wake so the waiter's stack stays alive.
struct SpinLock {
  uint32_t value;
};

inline void spin_acquire(SpinLock &lock) noexcept {
  while (__atomic_exchange_n(&lock.value, 1u, __ATOMIC_ACQUIRE) != 0)
    while (__atomic_load_n(&lock.value, __ATOMIC_RELAXED) != 0)
      cpu_relax();
}

inline void spin_release(SpinLock &lock) noexcept {
  __atomic_store_n(&lock.value, 0u, __ATOMIC_RELEASE);
}

struct MutexState {
  // 0 free, 1 held, 2 held with waiters.
  uint32_t lock;
  // Linux thread id of the owner, or 0 when unlocked.
  uint32_t owner;
};

struct Waiter {
  // Futex word: 0 waiting, 1 notified. Notifiers publish with release and
  // wakeers observe with acquire, so no generation counter can wrap.
  uint32_t signalled;
  Waiter *next;
};

struct CondvarState {
  SpinLock queue_lock;
  Waiter *head;
  Waiter *tail;
  // Registered waiters that have not yet returned to their caller. Notifiers
  // remove entries from the list, but the count remains until the waiter
  // finishes, so destroying a condvar with signalled-but-not-returned waiters
  // still Panics.
  uint32_t waiters;
};

struct AtomicI64State {
  int64_t value;
};

struct MutexObject {
  LangHandle handle;
  MutexState state;
};

struct CondvarObject {
  LangHandle handle;
  CondvarState state;
};

struct AtomicI64Object {
  LangHandle handle;
  AtomicI64State state;
};

static_assert(sizeof(LangHandle) == 24, "LangHandle layout must stay 24 bytes");
static_assert(offsetof(MutexObject, handle) == 0,
              "handle must be the first MutexObject member");
static_assert(offsetof(CondvarObject, handle) == 0,
              "handle must be the first CondvarObject member");
static_assert(offsetof(AtomicI64Object, handle) == 0,
              "handle must be the first AtomicI64Object member");
static_assert(alignof(AtomicI64Object) >= alignof(int64_t),
              "AtomicI64 state must be naturally aligned");
static_assert(offsetof(AtomicI64Object, state) % alignof(int64_t) == 0,
              "AtomicI64 state offset must preserve 8-byte alignment");

template <typename Object>
Object *allocate_object(LangHandleKind kind,
                        void (*destroy)(LangHandle *) noexcept) noexcept {
  void *memory = lang_alloc_v1(sizeof(Object), alignof(Object));
  if (!memory)
    allocation_panic();
  auto *object = static_cast<Object *>(memory);
  object->handle.refs = 1;
  object->handle.kind = kind;
  object->handle.destroy = destroy;
  return object;
}

template <typename Object>
Object *require(LangHandle *handle, LangHandleKind kind) noexcept {
  if (!handle || handle->kind != kind)
    misuse_panic();
  return reinterpret_cast<Object *>(handle);
}

void mutex_destroy(LangHandle *handle) noexcept {
  auto *object = reinterpret_cast<MutexObject *>(handle);
  if (__atomic_load_n(&object->state.lock, __ATOMIC_ACQUIRE) != kMutexFree ||
      __atomic_load_n(&object->state.owner, __ATOMIC_RELAXED) != 0)
    misuse_panic();
  lang_free_v1(handle);
}

void condvar_destroy(LangHandle *handle) noexcept {
  auto *object = reinterpret_cast<CondvarObject *>(handle);
  spin_acquire(object->state.queue_lock);
  const bool busy =
      object->state.head != nullptr || object->state.waiters != 0;
  spin_release(object->state.queue_lock);
  if (busy)
    misuse_panic();
  lang_free_v1(handle);
}

void atomic_i64_destroy(LangHandle *handle) noexcept { lang_free_v1(handle); }

} // namespace

extern "C" {

LangHandle *lang_mutex_new_v1() noexcept {
  auto *object =
      allocate_object<MutexObject>(LangHandleKind::mutex, &mutex_destroy);
  object->state.lock = kMutexFree;
  object->state.owner = 0;
  return &object->handle;
}

void lang_mutex_lock_v1(LangHandle *handle) noexcept {
  auto *object = require<MutexObject>(handle, LangHandleKind::mutex);
  MutexState &state = object->state;
  const uint32_t self = current_tid();

  uint32_t expected = kMutexFree;
  if (__atomic_compare_exchange_n(&state.lock, &expected, kMutexHeld, false,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    __atomic_store_n(&state.owner, self, __ATOMIC_RELAXED);
    return;
  }

  // The word is held. If this thread already owns it, this is a recursive
  // lock, which the nonrecursive contract rejects. The owner word is cleared
  // before unlock publishes the free word, so a stale owner can never make a
  // different holder look like self.
  if (__atomic_load_n(&state.owner, __ATOMIC_RELAXED) == self)
    misuse_panic();

  for (;;) {
    // Mark contention and try to take the word. A zero result means the
    // previous owner released and this thread acquired.
    if (__atomic_exchange_n(&state.lock, kMutexContended, __ATOMIC_ACQUIRE) ==
        kMutexFree)
      break;
    futex_wait(&state.lock, kMutexContended);
  }
  __atomic_store_n(&state.owner, self, __ATOMIC_RELAXED);
}

void lang_mutex_unlock_v1(LangHandle *handle) noexcept {
  auto *object = require<MutexObject>(handle, LangHandleKind::mutex);
  MutexState &state = object->state;
  const uint32_t self = current_tid();

  if (__atomic_load_n(&state.owner, __ATOMIC_RELAXED) != self)
    misuse_panic();

  // Clear ownership before publishing the free word; a new contender that sees
  // the held word but the zero owner simply blocks instead of reporting a
  // recursive lock.
  __atomic_store_n(&state.owner, 0u, __ATOMIC_RELAXED);
  if (__atomic_exchange_n(&state.lock, kMutexFree, __ATOMIC_RELEASE) !=
      kMutexHeld)
    futex_wake(&state.lock, 1u);
}

LangHandle *lang_condvar_new_v1() noexcept {
  auto *object =
      allocate_object<CondvarObject>(LangHandleKind::condvar, &condvar_destroy);
  object->state.queue_lock.value = 0;
  object->state.head = nullptr;
  object->state.tail = nullptr;
  object->state.waiters = 0;
  return &object->handle;
}

void lang_condvar_wait_v1(LangHandle *condvar_handle,
                          LangHandle *mutex_handle) noexcept {
  auto *condvar =
      require<CondvarObject>(condvar_handle, LangHandleKind::condvar);
  auto *mutex = require<MutexObject>(mutex_handle, LangHandleKind::mutex);
  const uint32_t self = current_tid();

  if (__atomic_load_n(&mutex->state.owner, __ATOMIC_RELAXED) != self ||
      __atomic_load_n(&mutex->state.lock, __ATOMIC_RELAXED) == kMutexFree)
    misuse_panic();

  Waiter waiter{0u, nullptr};

  // Register before releasing the external mutex. A notifier that observes the
  // predicate change after the unlock must therefore find this waiter.
  spin_acquire(condvar->state.queue_lock);
  waiter.next = nullptr;
  if (condvar->state.tail)
    condvar->state.tail->next = &waiter;
  else
    condvar->state.head = &waiter;
  condvar->state.tail = &waiter;
  ++condvar->state.waiters;
  spin_release(condvar->state.queue_lock);

  lang_mutex_unlock_v1(mutex_handle);

  // The futex word is checked under acquire. A notification stores it with
  // release before waking, so a wakeup cannot be lost between the check and
  // the sleep; EAGAIN/EINTR both return to this predicate.
  while (!__atomic_load_n(&waiter.signalled, __ATOMIC_ACQUIRE))
    futex_wait(&waiter.signalled, 0u);

  // A notifier may still hold the queue lock while it touches this stack
  // waiter. Reacquire it before removing links, decrementing the count or
  // returning, so the notifier never writes through a dead stack pointer.
  spin_acquire(condvar->state.queue_lock);
  Waiter **link = &condvar->state.head;
  Waiter *previous = nullptr;
  while (*link && *link != &waiter) {
    previous = *link;
    link = &(*link)->next;
  }
  if (*link == &waiter) {
    *link = waiter.next;
    if (condvar->state.tail == &waiter)
      condvar->state.tail = previous;
  }
  if (condvar->state.waiters != 0)
    --condvar->state.waiters;
  spin_release(condvar->state.queue_lock);

  // The contract is that wait returns with the external mutex held.
  lang_mutex_lock_v1(mutex_handle);
}

void lang_condvar_notify_one_v1(LangHandle *handle) noexcept {
  auto *condvar = require<CondvarObject>(handle, LangHandleKind::condvar);
  spin_acquire(condvar->state.queue_lock);

  Waiter *previous = nullptr;
  Waiter *waiter = condvar->state.head;
  while (waiter &&
         __atomic_load_n(&waiter->signalled, __ATOMIC_RELAXED) != 0) {
    previous = waiter;
    waiter = waiter->next;
  }

  if (waiter) {
    if (previous)
      previous->next = waiter->next;
    else
      condvar->state.head = waiter->next;
    if (condvar->state.tail == waiter)
      condvar->state.tail = previous;
    waiter->next = nullptr;

    // Release the signalled word while holding the queue lock, then wake. The
    // waiter cannot return before reacquiring this lock, so this stack node is
    // still alive for the wakeup.
    __atomic_store_n(&waiter->signalled, 1u, __ATOMIC_RELEASE);
    futex_wake(&waiter->signalled, 1u);
  }

  spin_release(condvar->state.queue_lock);
}

void lang_condvar_notify_all_v1(LangHandle *handle) noexcept {
  auto *condvar = require<CondvarObject>(handle, LangHandleKind::condvar);
  spin_acquire(condvar->state.queue_lock);

  Waiter *waiter = condvar->state.head;
  condvar->state.head = nullptr;
  condvar->state.tail = nullptr;
  while (waiter) {
    Waiter *next = waiter->next;
    waiter->next = nullptr;
    if (__atomic_load_n(&waiter->signalled, __ATOMIC_RELAXED) == 0) {
      __atomic_store_n(&waiter->signalled, 1u, __ATOMIC_RELEASE);
      futex_wake(&waiter->signalled, 1u);
    }
    waiter = next;
  }

  spin_release(condvar->state.queue_lock);
}

LangHandle *lang_atomic_i64_new_v1(int64_t initial) noexcept {
  auto *object = allocate_object<AtomicI64Object>(LangHandleKind::atomic_i64,
                                                  &atomic_i64_destroy);
  object->state.value = initial;
  return &object->handle;
}

int64_t lang_atomic_i64_load_v1(LangHandle *handle) noexcept {
  auto *object = require<AtomicI64Object>(handle, LangHandleKind::atomic_i64);
  return __atomic_load_n(&object->state.value, __ATOMIC_SEQ_CST);
}

void lang_atomic_i64_store_v1(LangHandle *handle, int64_t value) noexcept {
  auto *object = require<AtomicI64Object>(handle, LangHandleKind::atomic_i64);
  __atomic_store_n(&object->state.value, value, __ATOMIC_SEQ_CST);
}

int64_t lang_atomic_i64_fetch_add_v1(LangHandle *handle,
                                     int64_t delta) noexcept {
  auto *object = require<AtomicI64Object>(handle, LangHandleKind::atomic_i64);
  return __atomic_fetch_add(&object->state.value, delta, __ATOMIC_SEQ_CST);
}

bool lang_atomic_i64_compare_exchange_v1(LangHandle *handle, int64_t expected,
                                         int64_t desired) noexcept {
  auto *object = require<AtomicI64Object>(handle, LangHandleKind::atomic_i64);
  int64_t observed = expected;
  return __atomic_compare_exchange_n(&object->state.value, &observed, desired,
                                     false, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST);
}

} // extern "C"

#endif
