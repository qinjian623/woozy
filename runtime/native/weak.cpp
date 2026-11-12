#include "weak.hpp"

#include <cstddef>
#include <cstdint>

#include "alloc.hpp"

// Actual terminal Panic entry from panic.cpp. It is also used by the native
// ARC and synchronization runtime for allocation/reference-count failures.
extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t cause,
                                                  uint64_t snapshot,
                                                  uint64_t begin,
                                                  uint64_t end) noexcept;

namespace {

constexpr uint64_t kMaxU64 = ~uint64_t{0};

// This lock is used only for reading/CASing target and for detach. It is never
// held across lang_alloc_v1, lang_free_v1, a Panic, or any payload destructor.
// Ticket order avoids a single thread repeatedly stealing the lock under heavy
// upgrade contention.
uint64_t g_weak_lock_next;
uint64_t g_weak_lock_serving;

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__)
  __asm__ volatile("yield" ::: "memory");
#endif
}

inline void weak_lock_acquire() noexcept {
  const uint64_t ticket =
      __atomic_fetch_add(&g_weak_lock_next, 1u, __ATOMIC_RELAXED);
  while (__atomic_load_n(&g_weak_lock_serving, __ATOMIC_ACQUIRE) != ticket)
    cpu_relax();
}

inline void weak_lock_release() noexcept {
  __atomic_fetch_add(&g_weak_lock_serving, 1u, __ATOMIC_RELEASE);
}

[[noreturn]] inline void reference_count_panic() noexcept {
  lang_native_panic_v1(7, 0, 0, 0);
}

[[noreturn]] inline void allocation_panic() noexcept {
  lang_native_panic_v1(5, 0, 0, 0);
}

static_assert(sizeof(LangWeakControl) == 16,
              "Weak control block must remain two machine words");
static_assert(offsetof(LangWeakControl, weak_refs) == 0,
              "Weak control count must stay first");
static_assert(offsetof(LangWeakControl, target) == 8,
              "Weak control target offset changed");
static_assert(alignof(LangWeakControl) == 8,
              "Weak control block must be naturally aligned");

// Allocate outside the Weak lock. Only the winner of the owner-slot CAS keeps
// this block; losing candidates are freed without ever becoming visible.
LangWeakControl *new_control(void *target) noexcept {
  void *memory =
      lang_alloc_v1(sizeof(LangWeakControl), alignof(LangWeakControl));
  if (!memory) allocation_panic();
  auto *control = static_cast<LangWeakControl *>(memory);
  control->weak_refs = 2; // one returned Weak + one implicit alive target
  control->target = target;
  return control;
}

} // namespace

extern "C" LangWeakControl *
lang_weak_downgrade_v1(void *target, LangWeakControl **owner_slot) noexcept {
  if (!target || !owner_slot) return nullptr;

  // A live target is protected by the caller's strong reference, so an
  // already-installed control block cannot be detached/freed during retain.
  auto *existing = __atomic_load_n(owner_slot, __ATOMIC_ACQUIRE);
  if (existing) {
    lang_weak_retain_v1(existing);
    return existing;
  }

  LangWeakControl *candidate = new_control(target);
  LangWeakControl *expected = nullptr;
  if (__atomic_compare_exchange_n(owner_slot, &expected, candidate, false,
                                  __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
    return candidate;

  // Another caller won. Never publish the losing allocation.
  lang_free_v1(candidate);
  lang_weak_retain_v1(expected);
  return expected;
}

extern "C" void lang_weak_retain_v1(LangWeakControl *control) noexcept {
  if (!control) return;
  uint64_t weak_refs = __atomic_load_n(&control->weak_refs, __ATOMIC_RELAXED);
  for (;;) {
    if (weak_refs == kMaxU64) reference_count_panic();
    if (__atomic_compare_exchange_n(&control->weak_refs, &weak_refs,
                                    weak_refs + 1, false, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED))
      return;
  }
}

extern "C" void lang_weak_release_v1(LangWeakControl *control) noexcept {
  if (!control) return;
  uint64_t weak_refs = __atomic_load_n(&control->weak_refs, __ATOMIC_RELAXED);
  for (;;) {
    if (!weak_refs) reference_count_panic();
    const uint64_t next = weak_refs - 1;
    if (__atomic_compare_exchange_n(&control->weak_refs, &weak_refs, next,
                                    false, __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED)) {
      if (next) return;
      break;
    }
  }
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  // No Weak owner remains, and detach has already dropped the implicit
  // reference, so the target pointer is no longer needed.
  lang_free_v1(control);
}

extern "C" void *lang_weak_upgrade_v1(LangWeakControl *control) noexcept {
  if (!control) return nullptr;

  weak_lock_acquire();
  void *target = __atomic_load_n(&control->target, __ATOMIC_RELAXED);
  if (!target) {
    weak_lock_release();
    return nullptr;
  }

  auto *strong = static_cast<uint64_t *>(target);
  uint64_t refs = __atomic_load_n(strong, __ATOMIC_RELAXED);
  for (;;) {
    // The strong reference is the target's existing ARC count. A zero count
    // means final release is already in progress; overflow cannot produce a
    // valid new owner.
    if (!refs || refs == kMaxU64) {
      weak_lock_release();
      return nullptr;
    }
    if (__atomic_compare_exchange_n(strong, &refs, refs + 1, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
      break;
  }
  weak_lock_release();
  return target;
}

extern "C" bool lang_weak_alive_v1(LangWeakControl *control) noexcept {
  if (!control) return false;
  // Reading target is only a liveness hint; it must still happen under the lock
  // because detach/free may be concurrent with this call.
  weak_lock_acquire();
  const bool alive =
      __atomic_load_n(&control->target, __ATOMIC_RELAXED) != nullptr;
  weak_lock_release();
  return alive;
}

extern "C" void lang_weak_detach_v1(LangWeakControl *control) noexcept {
  if (!control) return;

  weak_lock_acquire();
  void *target = __atomic_load_n(&control->target, __ATOMIC_RELAXED);
  if (target)
    __atomic_store_n(&control->target, nullptr, __ATOMIC_RELEASE);
  weak_lock_release();

  // Do not hold the Weak lock across the implicit release: the last release
  // calls lang_free_v1. Detach returns only after target has been unpinned,
  // so the caller may free target immediately afterwards.
  if (target) lang_weak_release_v1(control);
}

extern "C" void lang_weak_fork_prepare_v1() noexcept { weak_lock_acquire(); }

extern "C" void lang_weak_fork_parent_v1() noexcept { weak_lock_release(); }

extern "C" void lang_weak_fork_child_v1() noexcept {
  __atomic_store_n(&g_weak_lock_next, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_weak_lock_serving, 0u, __ATOMIC_RELAXED);
}
