#ifndef LANG_RUNTIME_NATIVE_GC_RUNTIME_HPP
#define LANG_RUNTIME_NATIVE_GC_RUNTIME_HPP

#include <cstdint>

#include "gc.hpp"

// Private native automatic cycle-collection controller: lang-gc-runtime/1.
//
// This layer connects the accepted lang-gc/1 stopped-world collector to the
// lang-gc-world/1 stop-the-world registry. It is registry/runtime support only:
// it does not instrument Wz source, does not emit polls or critical regions,
// does not install main/thread/process hooks, and does not make source-level
// automatic reclamation complete.
//
// The caller must explicitly register the current thread with
// lang_gc_world_register_v1 before using this controller. The register wrapper
// does not auto-register a world thread, and an incompletely constructed object
// is never presented as a complete graph by this layer.
//
// Pressure starts at 128 successful lang_gc_register_v1 calls. A successful
// collection resets it to max(128, 2 * LangGcStats.registry_live_after).
// lang_gc_runtime_poll_v1 handles a pending world request first and only then
// considers pending pressure and lang_gc_world_can_collect_v1; with neither a
// request nor pressure it performs only atomic loads/branches (no gettid, lock,
// allocation, or kernel call).
//
// Deferred external releases are transferred only by the current collector
// while the kernel clear/release phase is open. The controller detaches that
// batch before resuming the world, clears its collecting state, and only then
// invokes external release callbacks. A non-collector call returns false on a
// fast path and must release its own reference. Pure Wz strong edges are never
// deferred through this mechanism; only external C/reference payloads are.
// A managed C registration may transfer its already-retained external env root
// intact: it is not a traced Wz edge, adds no retain during collection, and
// cannot drop that root until unregister/drain runs after resume.
//
// Forward order for Root's future fork integration (controller hooks do not
// call the other layers themselves):
//
//   enter a genuine graph critical only if modifying the graph; a graph-stable
//   fork uses the separate world fork-stable no-park interval
//   lang_alloc_fork_prepare_v1
//   lang_weak_fork_prepare_v1
//   lang_gc_fork_prepare_v1
//   lang_gc_world_fork_prepare_v1
//   lang_gc_runtime_fork_prepare_v1
//   fork
//   parent: runtime parent, world parent, gc parent, weak parent, alloc parent
//   child:  alloc child, weak child, gc child, world child, runtime child
//
// Controller prepare takes only its short queue lock; it never waits for the
// controller busy/collection state. The child resets synchronization/phase,
// abandons inherited queue nodes without fake ARC/release callbacks, and does
// not synthesize drops for vanished threads. A fork from the active collector
// inside a kernel clear/trace callback is rejected with Panic. A normal foreign
// destructor after resume may fork; the child drain aborts after the callback
// rather than executing more inherited external releases.

#ifdef __cplusplus
extern "C" {
#define LANG_GC_RUNTIME_NOEXCEPT noexcept
#else
#define LANG_GC_RUNTIME_NOEXCEPT
#endif

// Phase values reported by LangGcRuntimeStats.phase.
#define LANG_GC_RUNTIME_PHASE_IDLE 0u
#define LANG_GC_RUNTIME_PHASE_STOPPING 1u
#define LANG_GC_RUNTIME_PHASE_KERNEL 2u
#define LANG_GC_RUNTIME_PHASE_DRAINING 3u

typedef struct LangGcRuntimeStats {
  uint64_t pressure_accum;
  uint64_t pressure_limit;
  uint64_t last_live_after;
  uint64_t queue_depth;
  uint64_t queue_enqueued;
  uint64_t queue_drained;
  uint64_t collect_ok;
  uint64_t collect_busy;
  uint64_t collect_timeout;
  uint64_t collect_invalid;
  uint64_t kernel_busy;
  uint64_t total_registered_scanned;
  uint64_t total_traced_edges;
  uint64_t generation;
  uint32_t phase;
  uint32_t busy;
  int32_t collector_tid;
  uint32_t enqueue_open;
} LangGcRuntimeStats;

// Snapshot only. It does not lock, allocate, run callbacks, or change state.
void lang_gc_runtime_stats_v1(LangGcRuntimeStats *out) LANG_GC_RUNTIME_NOEXCEPT;

// Thin wrappers around lang-gc/1. The world registration is intentionally not
// implicit. Pressure counts only LANG_GC_OK registrations; DUPLICATE, INVALID,
// and NOT_REGISTERED results do not count.
LangGcStatus lang_gc_runtime_register_v1(
    void *object, const LangGcOps *ops) LANG_GC_RUNTIME_NOEXCEPT;
LangGcStatus lang_gc_runtime_unregister_v1(
    void *object) LANG_GC_RUNTIME_NOEXCEPT;

// Private compiler/runtime ABI for generated safepoints. The generated poll
// loads the world request first (and services it if set), then loads the limit
// and accum in that order, all with acquire ordering. It calls
// lang_gc_runtime_poll_pressure_v1 only when accum >= limit. The two words are
// written only by this controller; they are exposed so the fast path can avoid
// a function call when neither a stop nor pressure is pending.
__attribute__((visibility("hidden"))) extern uint64_t
    lang_gc_runtime_pressure_limit_v1;
__attribute__((visibility("hidden"))) extern uint64_t
    lang_gc_runtime_pressure_accum_v1;
__attribute__((visibility("hidden"))) void
lang_gc_runtime_poll_pressure_v1(void) LANG_GC_RUNTIME_NOEXCEPT;

// Automatic safepoint/controller poll. See the header comment for the fast path
// and ordering. It handles a pending world request first; only then does it
// consider pressure and lang_gc_world_can_collect_v1. Critical/SAFE threads do
// not start a collection. Auto stop uses a fixed 10 ms world timeout; timeout
// leaves pressure pending for a later retry and never reports success.
// Generated code may inline the same ordering instead of calling this entry
// point; the pressure handler keeps the collectibility check and collection.
void lang_gc_runtime_poll_v1(void) LANG_GC_RUNTIME_NOEXCEPT;

// Explicit collection. The caller must be explicitly world-registered and,
// like world stop, outside SAFE and critical (poll enforces this through
// lang_gc_world_can_collect_v1; direct callers are responsible). timeout_ns
// follows world stop semantics (-1 infinite, 0 try, >0 absolute monotonic
// deadline, <-1 invalid).
//
// Status mapping:
//   world stop 0 + kernel OK    -> 0
//   controller already busy     -> -16 (LANG_GC_RUNTIME busy)
//   world stop -16              -> -16
//   kernel collect LANG_GC_BUSY -> -16, world is resumed first
//   world stop -110             -> -110, world request retracted by world
//   world stop -22              -> -22
//
// On any failure the controller clears its busy state, leaves pending pressure
// and the graph unchanged, and does not claim a successful collection. Stats
// are written only for a successful kernel collection; on failure the caller's
// output is zeroed.
int64_t lang_gc_runtime_collect_v1(
    int64_t timeout_ns, LangGcStats *out) LANG_GC_RUNTIME_NOEXCEPT;

// Transfer one external C/reference payload to a deferred release. Returns true
// only when the current collector has enqueued it during the stopped kernel
// collection window. The controller cannot observe the kernel's individual
// trace/clear/release callbacks, so source integration must call this only from
// clear/release, never from trace. The controller does not retain the payload and will
// call release(payload) exactly once after the world has resumed and the
// controller collecting state is clear. On false, the caller still owns the
// reference and must release it itself. If the eligible collector path cannot
// allocate a queue node, this is a terminal Panic rather than a false result
// that would force an external release while the world is stopped. Pure Wz
// strong edges must not be deferred through this function.
// The managed C registration exception above preserves an existing external
// root; it does not permit deferring ordinary Fn, Vec or coroutine owners.
bool lang_gc_runtime_defer_release_v1(
    void *payload, void (*release)(void *) noexcept) LANG_GC_RUNTIME_NOEXCEPT;

// Minimal controller fork hooks. Root must call the complete runtime fork
// sequence in the documented order; these hooks do not call world, gc, weak or
// allocator hooks themselves.
//
// Prepare takes only the controller queue lock (never waits for busy/collection
// state), and rejects a fork initiated by the active collector from inside a
// kernel clear/trace callback with a terminal Panic. A normal foreign
// destructor after resume may fork and re-enter the controller.
//
// The child resets inherited controller synchronization/phase and abandons any
// inherited queue without running release callbacks and without faking ARC
// drops for vanished threads. Pressure counters are left as inherited state;
// the child does not synthesize drops.
void lang_gc_runtime_fork_prepare_v1(void) LANG_GC_RUNTIME_NOEXCEPT;
void lang_gc_runtime_fork_parent_v1(void) LANG_GC_RUNTIME_NOEXCEPT;
void lang_gc_runtime_fork_child_v1(void) LANG_GC_RUNTIME_NOEXCEPT;

#ifdef __cplusplus
} // extern "C"
#endif

#undef LANG_GC_RUNTIME_NOEXCEPT

#endif // LANG_RUNTIME_NATIVE_GC_RUNTIME_HPP
