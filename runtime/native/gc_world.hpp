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
// called by the active stop owner or from inside a SAFE or critical region.
bool lang_gc_world_unregister_v1(void) LANG_GC_WORLD_NOEXCEPT;

// Safepoint poll. With no request the fast path performs one atomic load and
// returns without gettid or futex. With a request the slow path looks up the
// registered node. It moves ACTIVE to PARKED and waits unless the thread is in
// SAFE or in a critical region. A critical region defers the stop without
// parking; the outermost leave_critical performs the pending safepoint. An
// unregistered thread that reaches the slow path is Panic.
void lang_gc_world_poll_v1(void) LANG_GC_WORLD_NOEXCEPT;

// Nestable SAFE regions for blocking/foreign calls that do not touch the Wz
// graph. All graph writes must be complete before enter. enter/leave are not
// a blanket "make every syscall safe" annotation; a caller that holds an
// allocator or runtime lock still has to reason about that lock. enter_safe is
// Panic while any critical region is active: SAFE asserts graph quiescence,
// while critical asserts a partial graph modification is in progress.
void lang_gc_world_enter_safe_v1(void) LANG_GC_WORLD_NOEXCEPT;
void lang_gc_world_leave_safe_v1(void) LANG_GC_WORLD_NOEXCEPT;

// Fork support for an outer SAFE region. suspend leaves every current SAFE
// level (honoring a pending stop at the outermost level, as ordinary leave
// does), returns the saved depth, and leaves the thread ACTIVE outside SAFE.
// The caller must be registered. Inside an existing critical region there is
// no SAFE depth by construction, so suspend returns 0 and the fork path nests
// another critical region. restore re-enters exactly that many SAFE levels after
// the runtime fork locks have been released. These two functions are the only
// private world extension used by lang-gc-program/1; they do not expose or
// change any node field.
uint32_t lang_gc_world_suspend_safe_v1(void) LANG_GC_WORLD_NOEXCEPT;
void lang_gc_world_restore_safe_v1(uint32_t safe_depth)
    LANG_GC_WORLD_NOEXCEPT;

// Graph-stable fork no-park interval. The caller must be registered, ACTIVE,
// and outside SAFE (the fork path suspends outer SAFE first). Unlike a graph
// critical region, this interval asserts that the graph is stable, so
// synchronous callbacks and SAFE regions remain legal. While it is active the
// world keeps the node ACTIVE for stop/collection even if safe_depth is
// nonzero, and poll/callback-leave defer parking. The outermost leave publishes
// SAFE or PARKED only after all internal fork locks have been released. A
// genuine graph critical region nested inside remains subject to the normal
// callback-in-critical / safe-in-critical rejection.
void lang_gc_world_enter_fork_stable_v1(void) LANG_GC_WORLD_NOEXCEPT;
void lang_gc_world_leave_fork_stable_v1(void) LANG_GC_WORLD_NOEXCEPT;

// Nestable critical (graph-modification) regions. enter_critical is allowed
// only for the current registered thread when it is ACTIVE and outside SAFE;
// it Panics when parked, inside SAFE, or on depth overflow. A stop request
// observed by poll is deferred while critical_depth is nonzero, so a mutator
// can finish a multi-word graph update. The outermost leave_critical is a
// safepoint: with a request still active it becomes PARKED and waits for
// resume before returning. Inner leaves never park.
void lang_gc_world_enter_critical_v1(void) LANG_GC_WORLD_NOEXCEPT;
void lang_gc_world_leave_critical_v1(void) LANG_GC_WORLD_NOEXCEPT;

// Query only, no registration and no state change. Returns true when no stop
// is currently owned, the calling thread is registered, its node is ACTIVE,
// safe_depth and critical_depth are both zero, and the fork-stable no-park
// depth is zero. It is therefore false for the collector holding a successful
// stop, for every thread while another collector owns a stop, and for any
// thread in SAFE, critical, or a fork-stable no-park interval. It never
// resumes, releases, or consumes an already successful stop. Unregistered
// threads return false (they are not auto-registered).
bool lang_gc_world_can_collect_v1(void) LANG_GC_WORLD_NOEXCEPT;

// One explicit stack token per callback enter scope. The caller must provide a
// distinct, zero-initialized token for each enter, keep it on the same thread's
// stack until the matching leave, and not copy or reuse it while active.
// Nested callbacks must leave in LIFO order. The fields are private to the
// world runtime; callers must not inspect or modify them.
//
// Callback enter is for a foreign C callback that is about to touch the Wz
// object graph. If the current thread is unregistered it registers it (and
// remembers that this scope owns the new registration). If it is an existing Wz
// thread, all nested SAFE depth is saved in the token, the node becomes ACTIVE,
// and the function waits out any pending stop before returning. A callback from
// an active collector, from a critical region, or from a nested callback whose
// outer graph work is ACTIVE while a stop is pending is a terminal Panic.
//
// Callback leave must be called after all graph cleanup and after leaving any
// callback-local SAFE/critical regions. It restores the exact saved SAFE depth
// and ACTIVE/SAFE state. If this entry created the registration and it is the
// outermost callback, leave removes that registration; it never removes an
// existing language-thread registration. Outer leaves never park a nested
// callback mid graph work; an outermost saved-ACTIVE leave parks if a stop is
// pending, while a saved-SAFE leave restores SAFE.
//
// The token may survive a fork executed by the same callback thread; the world
// fork-child hook preserves callback depth, token references and the container.
// The child must still execute the same callback leave scope. Copied tokens,
// tokens used after leave, tokens from another thread, non-LIFO leaves, and a
// callback that never returns through its scope are misuse (Panic or leaked
// process-exit state, never a silent removal of another registration). A
// callback should not call unregister (Panic) or stop (BUSY -16).
typedef struct LangGcWorldCallbackToken {
  void *node;
  void *previous;
  uint32_t saved_safe_depth;
  uint32_t created_registration;
  uint32_t magic;
  uint32_t reserved;
} LangGcWorldCallbackToken;

void lang_gc_world_callback_enter_v1(
    LangGcWorldCallbackToken *token) LANG_GC_WORLD_NOEXCEPT;
void lang_gc_world_callback_leave_v1(
    LangGcWorldCallbackToken *token) LANG_GC_WORLD_NOEXCEPT;

// Stop all other registered mutators. timeout_ns:
//   -1  wait without a deadline
//    0  try once
//   >0  absolute monotonic deadline of now + timeout_ns
//   <-1 return -22 without changing state
// Returns 0 on success, -110 (ETIMEDOUT) after retracting the request, -16
// (EBUSY) if another collector (or this collector) already owns a stop, or if
// the caller is inside a critical or fork-stable no-park region. That case is a
// busy result
// and does not set or change any request. The caller must be registered,
// outside SAFE and outside critical, and becomes the recorded collector. On
// success all other registered nodes are SAFE or PARKED, new registrations
// cannot start graph work, and the caller may continue to use the
// registry/allocator for collection.
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
// does not own, or parked. A critical or fork-stable no-park caller is allowed:
// the fork path enters that interval before taking launch/environment/allocator
// locks. A collector
// may fork during its own stop; the child clears the stop and preserves the
// surviving thread's critical_depth. The child does not simulate ARC drops for
// vanished threads.
void lang_gc_world_fork_prepare_v1(void) LANG_GC_WORLD_NOEXCEPT;
void lang_gc_world_fork_parent_v1(void) LANG_GC_WORLD_NOEXCEPT;
void lang_gc_world_fork_child_v1(void) LANG_GC_WORLD_NOEXCEPT;

#ifdef __cplusplus
} // extern "C"
#endif

#undef LANG_GC_WORLD_NOEXCEPT

#endif // LANG_RUNTIME_NATIVE_GC_WORLD_HPP
