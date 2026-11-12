# Private Python borrowed-Slice runtime checks

This directory tests the private Python value runtime only. It is an
independent LANG03/Python prerequisite based on the accepted low-level
`runtime/native/slice.hpp`/`slice.cpp` contract. It does not add a source
`Slice` type, emitter/HIR/Sema/Core changes, a source iterator framework, or a
source Weak eligibility decision. Source-level Vec slicing remains the existing
copying `Vec.slice` until the Root integration lands.

The private helper is `runtime/python/langrt.py`:

- `Slice.create(owner, start, end)` validates the half-open range
  `0 <= start <= end <= len(owner.items)` before retaining `owner` once and
  taking one active iteration (`begin`) pin. Empty views still retain and pin.
  `None` endpoints mean the start/end of the owner, matching `Vec.slice`.
- `Slice.blank()` is the only owner-less descriptor. It owns no reference and
  holds no pin.
- `view.copy()` (also `ValueType('slice').copy(view)`) creates an independent
  descriptor with its own retain and pin. Python assignment/move-like transfer
  of the same descriptor adds neither.
- `view.drop()` (also `ValueType('slice').drop(view)`) zeroes the descriptor
  first, then ends its pin and releases its owner. Dropping the same descriptor
  again is a no-op.
- `len(view)` / `view.len()` return the view length. `view.get(index)`,
  `view.set(index, value)` and `view.at(index).value` use slice-relative,
  nonnegative indices. `view.at(index)` returns the existing `Index` place.
- `view.sub(start, end)` creates an independently pinned relative subview.
- `view.to_vec()` creates a new `Vec` container and logically copies each
  element with the owner's normal `ValueType.copy`; nested owned containers are
  shared by value-copy, not aliased as the outer container.
- Structural owner changes (growing reserve, push, non-empty pop, changing
  resize, clear, private length replacement) Panic while any Slice pins the
  owner. A no-op reserve or same-length resize is allowed. Element get/set
  remains visible through every alias.
- A normal ARC view keeps its owner alive after the original Vec binding is
  released. Its final drop performs the Vec's normal final cleanup.

`ValueType('slice')` copy/drop delegate to the descriptor. Trace visits the
owner Vec exactly once per descriptor as one immediate strong edge; it never
traces the owner's element contents from the Slice. The cycle collector counts
those edges, releases each once, and retires every Slice descriptor in white
rows (including nested struct/enum fields) before any foreign/error/custom drop
callback. Retirement ends the pin but does not release the already-counted
owner edge again; this also removes pins from live (black) owners. White Vec
cleanup keeps the pin count valid until those end-pin operations run.

Run the standalone CPython 3.12.15 checks (automatic CPython GC is disabled and
`gc.collect` is replaced with an error):

```sh
python3.12 -s tests/python/slice_runtime/runtime_slice_test.py
```

The cases cover positive/empty/sub/copy/move-like/drop idempotence, get/set and
bounds, struct/enum descriptors, `to_vec` with owned nested elements, normal
owner outliving its binding, struct and enum Slice cycles, duplicate Slice
edges to a live owner, stale-Slice rejection plus GC reentry from a foreign
finalizer, and repeated pressure with a stable live root. The suite also checks
that white pins become zero naturally, with no ghost pin, artificial reset, or
double edge release.

Remaining integration steps, owned outside this task, are to accept the
corrected Source Slice Core, emit these descriptors from the Python backend,
and add source-level Slice construction/use diagnostics. Do not claim LANG03
done from this private runtime alone.
