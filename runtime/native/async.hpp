#ifndef LANG_RUNTIME_NATIVE_ASYNC_HPP
#define LANG_RUNTIME_NATIVE_ASYNC_HPP

#include <cstdint>

#include "gc.hpp"

// Private native async executor ABI: lang-async/1 with coroutine-frame GC
// metadata.
//
// This header is the shared contract between the Native emitter, the runtime
// and runtime tests. It declares only entry points and the frame callback
// record; the `Future` record stays private to async.cpp. Every Future starts
// with its u64 atomic strong reference count, which is the `lang-gc/1`
// convention for a registered object.
//
// ## Task graph participation
//
// A Future is the single GC node for its coroutine frame. A timer or
// descriptor wait future has no frame and never becomes a registered node; it
// is a leaf that an owning frame edge may point at. Only tasks can hold Wz
// graph edges, so only tasks are registered with `lang-gc/1`.
//
// Registration happens inside `lang_async_publish_v2` and only when all of
// these hold:
//
//   * `frame_ops` is non-null,
//   * `frame_ops->trace` is non-null (the frame can own a strong Wz edge),
//   * `frame_ops->clear_owners` is non-null (a registered node must be able to
//     remove every owned edge), and
//   * `lang-gc-program/1` is enabled (`lang_gc_program_init_v1` succeeded).
//
// With `trace == nullptr` the frame provably owns no strong edge and the task
// stays an unregistered leaf. A null `clear_owners` next to a non-null `trace`
// is a compiler contract violation and Panics. When the program is enabled and
// `lang_gc_program_register_v1` fails, the runtime Panics: a graph node is
// never silently dropped from the registry.
//
// `lang_async_publish_v1` (the pre-GC six-argument form) stays available for
// legacy callers. The current emitter uses v2; v1 never registers with the
// collector because it carries no frame metadata.
//
// COMPILER REQUIREMENT (NoGraph activation): the entry wrapper calls
// `lang_gc_program_init_v1` only when the module can register a graph node.
// Registration is performed by this runtime inside `publish_v2`, not by
// emitted `lang_gc_program_register_v1` calls, so the backend cannot rely on
// that call to set the activation flag. Whenever the backend emits a Task
// `LangAsyncFrameGcOps` with a non-null `trace`, it MUST set
// `gc_program_required = true` (see src/backend/native/gc_ops.inc). Otherwise
// the program never enables `lang-gc-program/1`, the task is not registered,
// and strong cycles through Task frames are never collected even though the
// metadata exists.
//
// ## Edges, roots and borrowed references
//
// The collector requires `incoming <= refs` and treats `refs == incoming` as
// reclaimable, so every real strong reference must be reported exactly once by
// its owner:
//
//   * the frame reports its active owner slots and owned promise values
//     through `frame_ops->trace`;
//   * a pending `await` link is held by two real references, one in each
//     direction, and this runtime reports both: the awaiter reports its
//     `waiting_on` target and a target reports every frame in its waiter list.
//     A source-level await slot is a separate reference and must not be
//     double-counted;
//   * the executor's `kActive` reference is an external root and is never
//     traced. A started task therefore always satisfies `refs > incoming` and
//     is never a candidate, whether it is queued, running or suspended;
//   * the executor registry (`g_all`) and run queue are borrowed pointers, not
//     references, and are never traced.
//
// Candidates are consequently only (a) tasks that never started and still hold
// their creation parameters, and (b) tasks that completed normally, with a
// typed error, or by cancellation, whose source cleanup already ran. The
// collector's `clear` callback asserts both:
// `!kActive && (!kStarted || status != Pending)`.
//
// ## clear callback contract
//
// `task_gc_clear` (runtime side) calls `frame_ops->clear_owners` and, for a
// never-started task, moves it to Cancelled through the normal completion path
// so it leaves the registry, run queue, waiter lists and epoll. It never
// resumes the coroutine, never runs a cancel exit plan and never executes a
// source defer. `clear_owners` must be idempotent, must clear each ownership
// flag before dropping that value, and must drop every edge so the collector's
// post-clear trace check sees no edge.
//
// Final release uses a CAS fast path while refs > 1. When refs is still 1 it
// rechecks and unlinks borrowed executor lists under their short lock, releases
// that lock, calls the real GC unregister with refs still 1, and only after the
// unregister completed stores refs == 0 and runs clear_owners/coroutine
// destroy/free. It never waits on the fork binding reservation. A concurrent
// fork may therefore copy a still-registered node with refs > 0 (a legal
// inherited reference from the final releaser's vanished thread), but never a
// registered zero-ref node. The releasing mutator does not enter SAFE, poll or
// call user code during the unregister window; a concurrent stop cannot
// succeed there. epoll/owned-edge removal runs after the executor unlink.
//
// ## Threading
//
// One process-wide executor thread owns `run*`, `spawn` and `cancel`; the first
// call binds the thread and any other thread calling those entry points is a
// terminal Panic. Task handles may be copied and finally released from other
// threads: reference counts are atomic, and the executor's registry/run-queue
// mutations are protected by a short internal spin lock that is never held
// while running a frame, a typed trace/clear callback, `clear_owners`, a
// coroutine destroy or a SAFE region. A thread that performs a GC-visible Wz
// release must already be a registered `lang-gc-world/1` mutator; the executor
// lock protects the executor tables, not the collector's stopped-world
// invariant.
//
// ## Fork
//
// `runtime/native/process.cpp` calls the private fork hooks below through weak
// references, so a program that only uses process does not link this unit. The
// hooks are part of the same process atfork chain as the accepted allocator,
// Weak and GC/world/runtime hooks; they do not create a second Task or GC
// registry.
//
// Lock order (a fork scope and its reservation are entered before any internal
// runtime lock):
//
//   fork reservation gate (and the program fork scope)
//   launch -> environment -> allocator -> Weak -> GC registry -> GC world
//   -> GC runtime -> async executor ticket lock
//   fork
//   parent: async executor ticket lock -> GC runtime -> world -> GC
//           -> Weak -> allocator -> environment -> launch -> reservation
//   child:  allocator -> Weak -> GC -> world -> runtime -> async child
//           -> process launch/environment -> thread tid
//
// The reservation gate is not the executor ticket lock. It serializes the
// executor-TID decision with a concurrent first bind and is held through the
// fork syscall. A first `run/spawn/cancel` on another thread waits on it, so a
// fork can never read an unbound TID and then copy memory after that thread
// binds. A source-visible `std/process.fork` on a thread other than a bound
// executor returns `-EBUSY` before forking; `os.check` turns that into os.Error.
// An unbound executor stays unbound after the fork. A same-thread executor is
// retargeted to the child's raw gettid. `g_current` is preserved so a fork from
// inside a coroutine body continues the same `run_one` frame in the child.
//
// Hosted direct libc fork goes through the same hooks but its atfork prepare is
// `void`, so it cannot return a Source error. A direct libc fork while another
// thread owns the executor therefore records a unique marker and Panics; it is
// not reported as a typed os.Error and no child is created.
//
// Internal `process.spawn` uses an exec-only fork: a cross-thread executor is
// allowed, no tasks are continued or cleaned, and the child only does its
// descriptor/exec handshake. Exec-only children do not rebuild epoll. Ordinary
// fork children discard their inherited event queue (already absent on Darwin)
// and lazily create a private close-on-exec queue that re-adds exactly the
// pending descriptor waits whose `kFdRegistered` intent survived.
// Timer deadlines, ready/all/await links,
// references, frame GC ops and GC registration are retained unchanged.
//
// The final strong release is deliberately independent of the binding gate. It
// unlinks the executor tables while refs is still 1, performs the real GC
// unregister, and only then publishes refs == 0 and runs clear_owners/coroutine
// destroy. A fork therefore never copies a zero-ref node that is still
// registered with the collector. A node that is still registered at the fork
// snapshot has a positive inherited reference and may survive in the child as
// an ordinary residual reference; the runtime does not fabricate ARC drops for
// a thread that vanished. A Source fork holds the binding reservation across
// user prepare handlers, but a final release never waits on that reservation,
// so user prepare/parent callbacks may join a worker that is releasing the last
// Task.
//
// ## Deferred external releases
//
// `lang_gc_runtime_defer_release_v1` accepts only an external C/reference
// payload and only from a clear/release callback. A frame, Future, Task, Vec or
// coroutine frame is never deferred: the runtime destroys the frame
// synchronously after unregistering it, so its release runs before the world is
// resumed and no frame outlives its metadata.

struct LangAsyncFrameGcOps {
  // Reports one direct owned graph edge per visitor call. Null means the frame
  // and promise cannot own a strong Wz edge; such a task is an unregistered
  // leaf. Duplicate targets are separate edges.
  void (*trace)(void *state, LangGcVisitor visitor, void *context) noexcept;
  // Drops every value still owned by the frame and promise. Idempotent, never
  // resumes the coroutine, never runs a source defer.
  void (*clear_owners)(void *state) noexcept;
};

#ifdef __cplusplus
extern "C" {
#define LANG_ASYNC_NOEXCEPT noexcept
#define LANG_ASYNC_NORETURN [[noreturn]]
#else
#define LANG_ASYNC_NOEXCEPT
#define LANG_ASYNC_NORETURN
#endif

void *lang_async_task_new_v1(void) LANG_ASYNC_NOEXCEPT;

// Pre-GC publication for legacy callers. `release_promise` is the old
// promise cleanup callback; it is invoked once after the body ran, and no GC
// registration happens.
void lang_async_publish_v1(void *future, void *handle, void *resume,
                           void *destroy, void *promise,
                           void *release_promise) LANG_ASYNC_NOEXCEPT;

// GC-aware publication. Must be called after the promise, owner slots, active
// flags and metadata are initialized and before the initial suspend. `gc_state`
// is the typed metadata pointer that is later passed back to the frame
// callbacks; it is never interpreted by the runtime.
void lang_async_publish_v2(void *future, void *handle,
                           void (*resume)(void *) LANG_ASYNC_NOEXCEPT,
                           void (*destroy)(void *) LANG_ASYNC_NOEXCEPT,
                           void *promise, void *gc_state,
                           const LangAsyncFrameGcOps *frame_ops)
    LANG_ASYNC_NOEXCEPT;

void lang_async_task_retain_v1(void *future) LANG_ASYNC_NOEXCEPT;
void lang_async_task_release_v1(void *future) LANG_ASYNC_NOEXCEPT;
void lang_async_spawn_v1(void *future) LANG_ASYNC_NOEXCEPT;
uint32_t lang_async_status_v1(void *future) LANG_ASYNC_NOEXCEPT;
void *lang_async_promise_v1(void *future) LANG_ASYNC_NOEXCEPT;
uint32_t lang_async_finished_v1(void *future) LANG_ASYNC_NOEXCEPT;
void lang_async_cancel_v1(void *future) LANG_ASYNC_NOEXCEPT;
uint32_t lang_async_current_cancelled_v1(void) LANG_ASYNC_NOEXCEPT;
uint32_t lang_async_await_v1(void *target) LANG_ASYNC_NOEXCEPT;
void *lang_async_sleep_v1(int64_t milliseconds) LANG_ASYNC_NOEXCEPT;
void *lang_async_wait_fd_v1(int32_t fd, int32_t events) LANG_ASYNC_NOEXCEPT;
void lang_async_run_v1(void) LANG_ASYNC_NOEXCEPT;
void lang_async_run_until_v1(void *future) LANG_ASYNC_NOEXCEPT;
void lang_async_shutdown_v1(void) LANG_ASYNC_NOEXCEPT;
void *lang_async_frame_alloc_v1(uint64_t size) LANG_ASYNC_NOEXCEPT;
void lang_async_frame_free_v1(void *memory) LANG_ASYNC_NOEXCEPT;

// Private fork hooks for runtime/native/process.cpp. Their declarations there
// are weak; see the Fork section above for the exact pairing and limits.
// reserve: 0 plus one owned reservation, or -EBUSY when another thread owns the
//          executor. exec_only reservations are no-ops.
int64_t lang_async_fork_reserve_v1(int32_t exec_only) LANG_ASYNC_NOEXCEPT;
// Release one reservation without forking (error path, or the outer source
// fork wrapper after a successful/failed fork).
void lang_async_fork_release_v1(void) LANG_ASYNC_NOEXCEPT;
// Last prepare step: acquire the executor ticket lock (ordinary fork only).
void lang_async_fork_prepare_v1(int32_t exec_only) LANG_ASYNC_NOEXCEPT;
// Parent hook: release the executor ticket lock. Idempotent.
void lang_async_fork_parent_v1(int32_t exec_only) LANG_ASYNC_NOEXCEPT;
// Child hook: reset both ticket words and the gate, retarget a same-thread
// executor, close only the child's inherited epoll descriptor and defer the
// private rebuild. Never runs a frame or calls a clear/release callback.
void lang_async_fork_child_v1(int32_t exec_only) LANG_ASYNC_NOEXCEPT;
// Direct libc fork refusal (foreign executor): writes a unique marker and
// raises a terminal runtime Panic.
LANG_ASYNC_NORETURN void lang_async_fork_refuse_v1(void) LANG_ASYNC_NOEXCEPT;

#ifdef __cplusplus
} // extern "C"
#endif

#undef LANG_ASYNC_NOEXCEPT
#undef LANG_ASYNC_NORETURN

#endif // LANG_RUNTIME_NATIVE_ASYNC_HPP
