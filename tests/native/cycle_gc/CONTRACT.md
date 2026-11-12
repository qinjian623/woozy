# Native cycle collector C ABI: `lang-gc/1`

This contract fixes the private C ABI in `runtime/native/gc.hpp`.  It is an
implementation contract between the Woozy native runtime and generated code
(or a future closure environment).  It is not a source-language API.

The collector is deliberately **stopped-world only**:

```c
LangGcStatus lang_gc_collect_stopped_v1(LangGcStats *out) noexcept;
```

The name is intentional.  The function does not stop mutators, does not wait
for a safepoint, and does not pretend to be an automatic trigger.  The caller
(Root) must already have stopped every mutator and must not run another
operation that can free a registered object concurrently with this call.

## Object representation

A registered object begins with one `uint64_t` strong reference count at byte
offset 0:

```c
typedef struct LangGcObject {
  _Atomic uint64_t refs; /* offset 0, 8-byte aligned */
  /* implementation-defined owned fields follow */
} LangGcObject;
```

The count is 64-bit (never compressed to 32 bits).  The collector reads and
writes it only with atomic relaxed/acquire-release operations.  The registry
does **not** retain an object: an object with no strong references may be
finalized normally even while the collector's stopped-world snapshot is being
built, provided Root has not violated the stopped-world requirement.

Only objects whose static type can contain a direct or recursive Wz heap-owner
strong reference are registered.  Strings and other leaf objects that cannot
form a strong cycle do not need registration.  A container that owns a
non-cyclic leaf still drops that leaf through its ordinary `clear`/final
release path.

## Callback table

```c
typedef void (*LangGcVisitor)(void *target, void *context) noexcept;

typedef struct LangGcOps {
  void (*trace)(void *object, LangGcVisitor visitor, void *context) noexcept;
  void (*clear)(void *object) noexcept;
  void (*retain)(void *object) noexcept;
  void (*release)(void *object) noexcept;
  void (*invalidate_weak)(void *object) noexcept; /* optional, trailing */
} LangGcOps;
```

The first four callbacks are required and non-null.  `invalidate_weak` is
optional and trailing; types without Weak leave it null.  Existing four-element
aggregate initialization remains valid and value-initializes the trailing
field to null.

* `trace(object, visitor, context)` enumerates **every direct owned Wz
  heap-owner strong edge** of `object` by calling `visitor(target, context)`
  once per edge.  Aggregate fields and recursive inline fields are expanded.
  If the same target is held twice, the visitor is called twice.  A self-edge
  is reported normally.  `trace` must be side-effect free apart from invoking
  the visitor; it must not retain, release, clear or allocate.  A target that
  is not in the collector's registered snapshot is ignored for the registered
  graph (its reference is therefore external to that graph).
* `clear(object)` empties every owned strong field and drops/releases each
  dropped value exactly through its normal ARC path.  When `clear` returns,
  `trace(object, ...)` must enumerate zero edges.  `clear` may allocate,
  register or unregister unrelated objects and may call the collector again;
  such a nested collection is rejected with `LANG_GC_BUSY`.  `clear` runs
  without the registry lock and without the allocator lock held by the
  collector.  Retaining the object itself from this callback (self-resurrection)
  is an internal/unsafe test of post-clear state, not a normal source-level
  destructor: the object may remain alive, but its Weak slot stays permanently
  closed and safe Source code cannot rely on this escape.
* `retain(object)` performs exactly one normal ARC retain (increment by one),
  with the same overflow panic as the object's ordinary retain function.
* `release(object)` performs exactly one normal ARC release.  At zero it must
  finalize normally, including dropping owned fields and invoking
  `lang_gc_unregister_v1` before the object memory is freed.
* `invalidate_weak(object)`, when non-null, is called for every candidate after
  all candidates have been retained and **before any candidate is cleared**.
  It must only close the object's canonical Weak slot with
  `lang_weak_invalidate_slot_v1(&object_weak_slot)`; it must not run user code,
  allocate, or perform source-level destructors.  The collector calls it
  without holding the registry or allocator lock.  Once a white object's Weak
  slot is closed, upgrade/alive are dead and later downgrade returns an empty
  Weak; a clear callback cannot resurrect a white peer through Weak.

No C++ exception may escape any callback or ABI function.  All callbacks are
`noexcept`.

## Registration

```c
LangGcStatus lang_gc_register_v1(void *object, const LangGcOps *ops) noexcept;
LangGcStatus lang_gc_unregister_v1(void *object) noexcept;
uint64_t lang_gc_registry_live_v1(void) noexcept;
```

`register` is called after a normal object has received its initial strong
reference and before it can become part of a cycle.  Registration is
thread-safe and may run concurrently with other register/unregister calls.
Registering an already registered address returns `LANG_GC_DUPLICATE`.
`unregister` is called immediately before normal final free; it returns
`LANG_GC_NOT_REGISTERED` for an already removed address.  Cleanup code may
call `unregister` recursively, including from a `clear` callback.  Removing an
entry never frees the target object.

Status values are fixed 32-bit values:

| Value | Name | Meaning |
|---:|---|---|
| 0 | `LANG_GC_OK` | operation completed |
| 1 | `LANG_GC_INVALID` | null, misaligned object, or missing callback |
| 2 | `LANG_GC_DUPLICATE` | address already registered |
| 3 | `LANG_GC_NOT_REGISTERED` | address was not in the registry |
| 4 | `LANG_GC_BUSY` | a collection is already active |

`lang_gc_registry_live_v1` returns the current number of registry entries; the
registry does not retain any target object.

## Collection

```c
typedef struct LangGcStats {
  uint64_t registered;          /* objects in this snapshot */
  uint64_t traced_edges;        /* edges visited by the incoming-count pass */
  uint64_t reachable;           /* registered objects reachable from roots */
  uint64_t candidates;          /* registered objects not reachable */
  uint64_t pinned;              /* candidates retained before any clear */
  uint64_t cleared;             /* candidates whose clear callback completed */
  uint64_t collected;           /* candidates actually finalized by pin release */
  uint64_t registry_live_after; /* registry entries after callbacks finished */
} LangGcStats;

LangGcStatus lang_gc_collect_stopped_v1(LangGcStats *out) noexcept;
```

The algorithm is:

1. Snapshot registered objects, their ops, and their original 64-bit strong
   counts under the registry lock, then release the lock.
2. Trace every snapshot object and count registered incoming edges per target.
   Duplicate edges and self-edges each count once per visit.
3. `external = original_refs - incoming_refs`.  A negative result, a zero
   registered strong count, or any arithmetic overflow is a panic.
4. Mark every object with `external > 0` and traverse its registered outgoing
   edges.  Unmarked objects are candidates.
5. Retain every candidate once, so clearing cannot free a candidate through
   another candidate's edge.
6. Call `invalidate_weak` for every candidate, before the first clear.  This is
   the white-Weak barrier: no Weak upgrade to a white candidate can succeed
   once the clear phase begins.
7. Clear every candidate.  Afterwards each candidate is traced again; any
   surviving edge is an ops-contract violation and panics.
8. Release every temporary pin.  `collected` counts candidates for which the
   temporary pin was the only remaining strong reference immediately before
   release, i.e. the candidates that finalize exactly under this release.  A
   candidate resurrected by an internal/unsafe clear callback is not counted as
   collected; its Weak slot stays permanently closed.
9. Fill `registry_live_after` from the current registry.

`out` may be null.  On `LANG_GC_BUSY`, `out` (when non-null) is zeroed and the
active outer collection is unaffected.  On success, `registered`, `candidates`,
`pinned`, `cleared`, and `reachable` describe the snapshot; `collected` is not
just an alias for `candidates`.  A repeated collection with an empty registry
returns `LANG_GC_OK` and zero counts (apart from the current live statistic).

Snapshot and temporary storage use `lang_alloc_v1`/`lang_free_v1`; the
implementation uses no libc, `std::vector`, or C++ exceptions.  Snapshot
sorting is `O(V log V)`; each edge lookup is a binary search, so the graph work
is `O((V + E) log V)`.  Registry allocations and frees are performed outside
the registry lock.  No callback is invoked with the registry lock or allocator
lock held by the collector.

## Fork hooks

```c
void lang_gc_fork_prepare_v1(void) noexcept;
void lang_gc_fork_parent_v1(void) noexcept;
void lang_gc_fork_child_v1(void) noexcept;
```

These are independent hooks for a future Root/process integration.  Root must
call prepare before `fork`, parent after `fork`, and child before any registry
or collection operation in the child.  They assume the caller has already made
the registry quiescent (no collection or registration running in another
thread).  The child hook resets inherited lock and collection-state words; it
does not retain, free, register, or unregister any object.  This file does not
modify the existing process implementation.

## Panics and explicit gaps

Allocation failure uses the existing `allocation-failed` panic; allocation-size
overflow uses `allocation-size-overflow`.  Refcount inconsistency, incoming
count overflow, temp-pin overflow, preserve/retain violations, and clear/trace
contract violations use the existing `reference-count-overflow` panic.  Panic
is terminal and does not unwind.

Not included by this contract or implementation:

* automatic safepoints, mutator stopping, or automatic collection triggers;
* compiler-generated `trace`/`clear`/`invalidate_weak` callbacks and
  registration at allocation sites;
* Python/host cycle management;
* process/fork integration (only the standalone hooks above are provided).

The Weak closed-slot representation and invalidation coordination are specified
in `runtime/native/weak.hpp`; `tests/native/cycle_gc/gc_weak_integration_test.cpp`
exercises the real combined ordering.
