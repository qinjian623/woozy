#ifndef LANG_RUNTIME_NATIVE_WEAK_HPP
#define LANG_RUNTIME_NATIVE_WEAK_HPP

#include <cstdint>

// Private runtime ABI for Weak[T] and pure-Woozy strong-cycle reclamation.
//
// The strong reference count stays where it is: offset 0 of an owned object,
// as an atomic u64 (LangVec::refs, LangHandle::refs and generated objects all
// follow this rule). Weak does not extend or move those object headers. It
// owns a small control block instead.
//
// weak_refs counts real Weak owners plus one implicit owner held while the
// target is alive. target is cleared exactly once, by the final strong release,
// before the target storage is destroyed. The control block can therefore
// outlive the target and continue to be copied and dropped; upgrade fails once
// target is null.
//
// Contract for the integrating compiler/runtime:
//   * target points at an object whose first member is an atomic u64 strong
//     count that reaches zero exactly once in the final release.
//   * owner_slot is zero-initialized before the object is published.  It is
//     modified only by lang_weak_downgrade_v1 and
//     lang_weak_invalidate_slot_v1.  Reusing the storage for a new object
//     requires re-initializing the slot to null first.
//   * lang_weak_downgrade_v1 is called while the caller holds a strong
//     reference. The returned pointer owns one real Weak reference.  A slot
//     invalidated by the cycle collector is permanently closed: later calls
//     return null and never install a new control block.
//   * The final strong release calls lang_weak_detach_v1 exactly once before
//     destroying/freeing target.  The slot may already hold the closed tag;
//     detach must accept it as a harmless no-op.  After detach returns, no code
//     may read the target slot (the target may be freed immediately after
//     detach returns).
//   * Every Weak retain has a matching release. Null control pointers are
//     accepted as empty Weak values.
//
// The closed-slot representation is private to weak.cpp: the canonical slot
// holds either null, a naturally aligned LangWeakControl pointer, or the
// impossible pointer value 1.  Value 1 is a permanent tag, never a control
// block, and must not be passed to retain/release/upgrade as a real Weak.
//
// These functions use only atomic builtins, the native allocator and the
// terminal Panic entry. They do not call libc, pthreads or target payload
// destructors while holding the Weak lock.

struct LangWeakControl {
  // Runtime-owned. Real Weak owners plus the implicit alive-target reference.
  uint64_t weak_refs;
  // Runtime-owned. Cleared under the Weak lock by detach before target free.
  void *target;
};

extern "C" {

// Caller holds a strong reference. owner_slot is initially null; concurrent
// callers atomically converge on one control block. Returns one real Weak
// reference per successful call.
LangWeakControl *lang_weak_downgrade_v1(void *target,
                                        LangWeakControl **owner_slot) noexcept;

// Null is an empty Weak and is a no-op. Reference-count overflow and
// underflow are terminal Panics (runtime cause 7).
void lang_weak_retain_v1(LangWeakControl *control) noexcept;
void lang_weak_release_v1(LangWeakControl *control) noexcept;

// Returns target with one new strong reference, or null when dead or when the
// strong count would overflow. Locking orders this CAS before detach, so the
// target cannot be freed after the pointer is read. The caller releases the
// returned pointer according to its concrete ARC type.
void *lang_weak_upgrade_v1(LangWeakControl *control) noexcept;

// Hint only: true does not guarantee that a later upgrade succeeds.
bool lang_weak_alive_v1(LangWeakControl *control) noexcept;

// Called once by the final strong release, before target destruction. Clears
// target under the Weak lock and then drops the implicit alive-target Weak
// reference. The target storage must not be freed before this function
// returns. A second call on the same control block is harmless. Null and the
// private closed-slot tag are harmless no-ops, so final release can call this
// after the cycle collector has invalidated the slot.
void lang_weak_detach_v1(LangWeakControl *control) noexcept;

// Cycle-collector integration.  The caller must hold a strong reference to the
// target (the collector pins every candidate first).  Atomically and
// permanently closes *owner_slot, clears the installed control block's target,
// and releases its implicit alive-target Weak reference.  The slot is tagged
// before any control can be freed, so a concurrent downgrade either observes
// the tag and returns null or already owns a real Weak reference.
//
// This callback must not run user code.  It is idempotent, accepts a null slot
// pointer, and never allocates, frees, or panics while holding the Weak lock.
// Existing real Weak owners remain copyable/droppable; their upgrade/alive
// operations observe the cleared target as dead.
void lang_weak_invalidate_slot_v1(LangWeakControl **owner_slot) noexcept;

// Fork integration. prepare acquires this file's dedicated lock before fork;
// the parent releases it (even when fork fails), while the child resets the
// inherited ticket state before making any Weak call. The caller must not
// already hold the Weak lock (runtime code never forks while holding it).
// Recommended order relative to the allocator hooks:
// allocate lock -> Weak prepare -> fork; parent: Weak parent -> allocate
// parent; child: allocate child -> Weak child. Weak never allocates while its
// lock is held, so the order has no reverse dependency.
void lang_weak_fork_prepare_v1() noexcept;
void lang_weak_fork_parent_v1() noexcept;
void lang_weak_fork_child_v1() noexcept;

} // extern "C"

#endif
