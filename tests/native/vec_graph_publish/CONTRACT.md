# Vec graph-publication runtime stage

Status: **runtime-only; NOT SourceNativeGC complete.**
Operation: `vec-graph-publish-runtime-20261112`.
Base: `9f8c551` plus the accepted `lang-gc-program/1` correction snapshot.

This directory verifies the next independent runtime stage: existing
`LangVec` mutations and ordinary final cleanup publish complete graph state
before arbitrary copy/drop callbacks, and a pending collector stop cannot park
the mutator in the middle of a graph publication window.

It does **not** add emitter registration, source polls, compiler-emitted
critical regions, source tracers, or the generated `main` wrapper. No `.wz`
program is compiled or measured here. The Source automatic-reclamation target
remains **RED**.

## Files

The implementation change is confined to `runtime/native/panic.cpp` (Vec
operations and ordinary final release). `runtime/native/vec.hpp` keeps the
80-byte layout, offsets and ABI; only private contract comments were updated.
No other runtime ABI, global state, or shared emitter/frontend file changed.

`run_all.sh` builds host O0/O2, host ASAN+UBSAN, a default single-unit static
ELF without `gc_program.cpp`, and the existing VecRuntime/VecGrowth/Slice/Weak
regressions.

## Published runtime rules

### init

`lang_vec_init_v1` copies the element bytes first and only then publishes
`initialized = index + 1`. A GC trace can never observe an initialized slot
whose bytes are incomplete. Init remains a transfer of a pre-copied value: it
does not call `copy`, so there is no double copy. The trivial path adds no
allocator call, critical region, or `gettid`.

The probe uses a source pointer that aliases the descriptor's own `initialized`
word. Copy-before-publish yields the old value `0` in the element; the previous
increment-first order yielded `1`.

### set

For owning elements, `lang_vec_set_v1`:

1. moves the old owner bytes into a small stage (inline for normal sizes, one
   allocator block for larger elements);
2. protects an incoming value that aliases Vec storage with an independent byte
   move, including the exact destination slot;
3. publishes the complete new element in one short nested `CriticalGuard`;
4. releases that region before invoking the old owner's drop callback.

Set is always a transfer of a pre-copied value. If the caller passes the
destination slot itself after retaining its owner, the supplied owned copy is
consumed and the old owner is dropped exactly once; there is no silent
self-assignment return. The drop callback may re-enter Woozy, enter
SAFE/critical, read the same Vec, or perform another set. The probes replace
the remaining slot from the first drop callback and check exact copy/drop counts
plus the self-set pre-copied refcount.

### clear and shrink

`lang_vec_clear_v1` and the shrinking branch of `lang_vec_resize_v1` move the
removed owning prefix into a stage before publishing the new
`length`/`initialized`/stable remaining fields. Only after that publication do
they drop the staged values, outside any graph-critical region.

A destructor may read/write the same Vec or structurally push a new element:
the staged old owners remain valid even when the push reuses the removed slot.
Storage and capacity stay published/unchanged for clear; shrink preserves the
remaining prefix. Drop order remains highest removed index first, matching the
previous reverse-prefix order. Trivial `copy == drop == nullptr` Vecs keep the
allocation-free, gettid-free path.

### resize growth and slice

`copy` is arbitrary external retain code. Resize growth and `lang_vec_slice_v1`
copy one complete element into an independent temporary snapshot **before** the
copy callback, call `copy` on that snapshot, and publish exactly the snapshot
bytes. They never re-read the source after the callback, and never publish an
unretained edge before external code. This matters because a legal callback may
set the original source slot to a different ARC object: the result must own the
originally retained snapshot, not the replacement.

When a `copy` callback exists, the source Vec is pinned with the existing
`iterations` counter so a structural re-entry cannot relocate source or
destination storage. Pins do not stop a non-structural `set`; the per-iteration
snapshot handles that case. The pin is released after publication; drop
callbacks run after the pin is gone. Trivial numeric resize/slice does not pin,
does not allocate a snapshot, and does not enter a critical region.

An aliased fill source is byte-staged before growth can free the old separate
buffer. The self-alias probe grows an owning Vec through a self push, then
grows twice through a resize fill that points into the same Vec, checking exact
retain/drop counts.

### reserve/growth

`grow_storage` copies the initialized prefix into the new buffer, publishes
`data` and `capacity`, then frees the old separate buffer. For owning Vecs the
swap/free tail is one short critical region, so a pending stop cannot park
between the old buffer free and the new publication. Trivial numeric Vecs keep
the same order without `gettid`.

### pop and final release

`lang_vec_pop_v1` transfers one owner out and publishes the shorter prefix; it
never calls `drop`.

Ordinary final strong release keeps the existing order:

1. final `refs` decrement and iteration check;
2. registered-object unregistration hook;
3. canonical Weak detach (closed before any element drop);
4. publish an empty descriptor and move the old payload out;
5. reverse element observer/drop callbacks against the moved payload;
6. free the old separate payload, then the descriptor.

A destructor observes `length == 0`, `initialized == 0`, `data == nullptr`,
`capacity == 0`, `refs == 0`, a dead Weak target, and an empty registry. It can
still read the descriptor because it is freed only after the last drop. The
ASAN build checks this directly.

## Probes

| Mode | Evidence |
| --- | --- |
| `init-publish` | aliasing `initialized` source proves copy-before-publish; owning init invokes no copy/drop. |
| `set-reentrant` | set publishes the new slot before old drop; destructor enters SAFE and performs a nested set; exact counts/order. |
| `clear-reentrant` | clear publishes empty first; destructor pushes the same Vec; staged remaining owners still drop correctly. |
| `shrink-reentrant` | shrink publishes the kept prefix first; destructor replaces a kept slot; staged removed owners still drop. |
| `resize-copy-publish` | all retains happen while the grown prefix is still unpublished; exact refcounts and drop counts. |
| `selfalias-growth` | self push and aliased resize fill survive storage growth with exact owners. |
| `final-release` | unregistered/Weak-closed before drops, empty header visible during drops, payload/descriptor freed after drops. |
| `pending-stop` | a real second thread owns a world stop; the setter parks after publishing the new element and before the old drop. |
| `slice-copy-snapshot` | slice copy callback replaces the original source slot with another ARC object; result owns the retained original, source is released before the result is read, exact refs/drops. |
| `resize-fill-alias-snapshot` | resize fill points at another Vec slot; its copy callback replaces that slot; the new element owns the retained original, source is released before reading the result, exact refs/drops. |
| `self-set-pre-copied` | caller pre-retains the owner and passes the original slot pointer to set; exactly one old-owner drop, final refs 1, no copy callback. |
| `disabled-fast` | seccomp denies `gettid`; owning Vec init/set/push/resize/clear/slice/final release still work without init. |
| default static ELF | links panic/alloc/Weak/Vec only, never calls `lang_gc_program_init_v1`, has no interpreter, `NEEDED`, undefined symbols, libc/libstdc++ strings, or extra GC unit. |

## Remaining emitter/frontend work (not done here)

* Emit `lang_gc_program_init_v1` / `finish_v1` in the outermost generated main.
* Register only cycle-capable owning types; never register primitive numeric
  Vecs. Trace only `initialized`.
* Emit function-entry/loop-backedge polls and use the cheap request word.
* Wrap compiler-inlined struct/enum field replacement, moves, memcpy and
  initialized-progress stores in the same short publication windows. The
  runtime C API changes here do not cover generated inline field writes.
* Emit registration/unregistration around the compiler's own final strong
  release paths where they are not routed through `lang_vec_release_v1`.
* Verify real `.wz` return, typed throw, defer, continue, thread exit, callback
  re-entry, fork/exec, Weak-before-clear and automatic pressure collection.

Until that emitter/frontend work is integrated and measured, this directory is
runtime/ABI evidence only.

## Correction 2026-11-12: independent F1/F2

Independent probes at O0/O2/ASAN confirmed two defects in the original runtime
publication patch:

* **F1** — slice/resize copy callbacks could legally `set` the source slot to a
  different ARC object. The old order called `copy` on the source and then
  re-read/copied the source, so the result owned the replacement without a
  retain while the original retain was lost. Fixed with per-iteration raw
  snapshots retained and published exactly as snapshotted. Resize also covers a
  fill pointer into another Vec slot, which `aliases_payload` cannot see.
* **F2** — `set(value == target)` returned early, leaving a caller's pre-copied
  retain and skipping the old-owner drop. Fixed by treating it as a normal
  pre-copied transfer: stage old owner, snapshot/publish the supplied value,
  drop the old owner once.

The independent reproducers now exit 0, including `slice-uaf` and
`resize-copy-source-mutation`, and the permanent probes above run at O0/O2/ASAN.

## Verification

```sh
bash tests/native/vec_graph_publish/run_all.sh
```

The script uses fixed Clang 22.1.8, `-std=c++20`, `-pedantic-errors`, the
legacy libxml2 `LD_LIBRARY_PATH`, per-build `timeout 300s`, and per-process
`timeout` values. It prints `pass`, `fail` and `skip` counts plus SHA-256 hashes.
The ASAN build skips only the seccomp `disabled-fast` probe because that probe
denies `gettid` to the sanitizer runtime itself.
