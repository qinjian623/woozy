#ifndef LANG_RUNTIME_NATIVE_GC_WORLD_HPP
#define LANG_RUNTIME_NATIVE_GC_WORLD_HPP

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

// Private native stop-the-world registry ABI: lang-gc-world/1 (Linux x86_64).
//
// This header is the fixed C ABI used by the compiler backend and by runtime
// support code. It is not a source-language module and is not exposed to
// ordinary Woozy programs. The full protocol and the list of integration
// prerequisites are in tests/native/gc_world/CONTRACT.md.
//
// The registry only decides which mutator threads are allowed to touch the Wz
// object graph. It does not collect objects, run ARC drops for vanished
// threads, or make a non-safepointed program correct. The compiler must still
// place polls at function entries, loop backedges, and blocking runtime calls.
//
// The request word is exported so a backend can inline the no-request fast
// path without a call:
//
//   if (lang_gc_world_poll_required_v1()) lang_gc_world_poll_v1();
//
// Access must use an atomic load. Do not read a copied, ordinary bool. The
// helper below uses the GCC/Clang __atomic builtins; on x86_64 the acquire
// load is an ordinary load plus compiler ordering.

#define LANG_GC_WORLD_REQUEST_BIT 1u

#ifdef __cplusplus
#define LANG_GC_WORLD_NOEXCEPT noexcept
extern "C" {
#else
#define LANG_GC_WORLD_NOEXCEPT
#endif

// The low bit is set while a collector owns a stop request. Other bits are
// reserved and must be ignored by readers.
extern uint32_t lang_gc_world_request_word_v1;

// Atomic, no syscall, no registration lookup. Safe to call from any thread.
int lang_gc_world_request_pending_v1(void) LANG_GC_WORLD_NOEXCEPT;

// Same check as an inlinable helper. The backend should use this (or the
// exported word directly) on the no-request path.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
static inline int lang_gc_world_poll_required_v1(void) {
  return (__atomic_load_n(&lang_gc_world_request_word_v1,
                          __ATOMIC_ACQUIRE) &
          LANG_GC_WORLD_REQUEST_BIT) != 0;
}

// Register the current kernel thread id. Registration is idempotent: the
// first call returns true, a repeated call returns false and does not create a
// second registration. Registration during an active stop parks until resume
// before returning true, so a newly registered thread cannot run graph code
// while the world is stopped. Allocation failure or an invalid tid is Panic.
bool lang_gc_world_register_v1(void) LANG_GC_WORLD_NOEXCEPT;

// Remove the calling thread's registration. Returns true only when this call
// removed it, false when the thread was not registered. It is thread-scoped:
// an external callback that did not itself create the registration must not
// call this, or it removes the language thread's only registration. Panic if
// called by the active stop owner or from inside a SAFE region.
bool lang_gc_world_unregister_v1(void) LANG_GC_WORLD_NOEXCEPT;

// Safepoint poll. With no request the fast path performs one atomic load and
// returns without gettid or futex. With a request the slow path looks up the
// registered node, moves ACTIVE to PARKED, and waits for resume. An
// unregistered thread that reaches the slow path is Panic.
void lang_gc_world_poll_v1(void) LANG_GC_WORLD_NOEXCEPT;

// Nestable SAFE regions for blocking/foreign calls that do not touch the Wz
// graph. All graph writes must be complete before enter. enter/leave are not
// a blanket "make every syscall safe" annotation; a caller that holds an
// allocator or runtime lock still has to reason about that lock.
void lang_gc_world_enter_safe_v1(void) LANG_GC_WORLD_NOEXCEPT;
void lang_gc_world_leave_safe_v1(void) LANG_GC_WORLD_NOEXCEPT;

// Stop all other registered mutators. timeout_ns:
//   -1  wait without a deadline
//    0  try once
//   >0  absolute monotonic deadline of now + timeout_ns
//   <-1 return -22 without changing state
// Returns 0 on success, -110 (ETIMEDOUT) after retracting the request, -16
// (EBUSY) if another collector (or this collector) already owns a stop.
// The caller must be registered, outside SAFE, and becomes the recorded
// collector. On success all other registered nodes are SAFE or PARKED, new
// registrations cannot start graph work, and the caller may continue to use
// the registry/allocator for collection.
int64_t lang_gc_world_stop_v1(int64_t timeout_ns) LANG_GC_WORLD_NOEXCEPT;

// Only the thread that obtained a successful stop may resume. This clears the
// request, wakes parked threads and registration waiters, and lets them become
// ACTIVE again. Any other caller is Panic. If the wake syscall fails, resume
// Panics instead of returning a false success.
void lang_gc_world_resume_v1(void) LANG_GC_WORLD_NOEXCEPT;

// Minimal fork hooks. Prepare acquires the registry lock; parent releases it;
// child prunes the inherited registry down to the surviving thread, remaps that
// node to the child's new tid, clears stop/collector state, and resets the
// inherited lock. The caller must also bracket lang_alloc_v1's fork hooks:
//
//   lang_alloc_fork_prepare_v1(); lang_gc_world_fork_prepare_v1();
//   fork();
//   parent: lang_gc_world_fork_parent_v1(); lang_alloc_fork_parent_v1();
//   child:  lang_alloc_fork_child_v1();     lang_gc_world_fork_child_v1();
//
// Prepare Panic if the caller is unregistered, already inside a stop that it
// does not own, or parked. A collector may fork during its own stop; the child
// clears the stop. The child does not simulate ARC drops for vanished threads.
void lang_gc_world_fork_prepare_v1(void) LANG_GC_WORLD_NOEXCEPT;
void lang_gc_world_fork_parent_v1(void) LANG_GC_WORLD_NOEXCEPT;
void lang_gc_world_fork_child_v1(void) LANG_GC_WORLD_NOEXCEPT;

#ifdef __cplusplus
} // extern "C"
#endif

#undef LANG_GC_WORLD_NOEXCEPT

#endif // LANG_RUNTIME_NATIVE_GC_WORLD_HPP
