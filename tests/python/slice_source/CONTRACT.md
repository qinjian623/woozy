# Python source `Slice[T]` contract

Task: LANG03 Python-only Source Slice step. Contract `compiler-dev/1`; Python
value runtime `compiler-dev/1-python-values/10`.

## Source interface

`std/slice` is unchanged:

```woozy
pub func view[T](values Vec[T], start usize, end usize) Slice[T]
pub func len[T](value Slice[T]) usize
pub func sub[T](value Slice[T], start usize, end usize) Slice[T]
pub func to_vec[T](value Slice[T]) Vec[T]
```

`Slice[T]` is the builtin generic type. `v[start:end]` on a `Slice[T]` is a
relative subview; on a `Vec[T]` it remains the existing copying `Vec` slice.
String slicing still uses the existing code-point rule.

## Python value mapping

- `TypeKind::Slice` is emitted as `_rt.ValueType('slice')`; its element
  descriptor is emitted recursively, including recursive aggregate descriptors.
- `ValueType('slice').copy` uses `Slice.copy`, which retains the owner Vec once
  and takes one independent iteration pin. Moves transfer the descriptor.
- `ValueType('slice').drop` uses `Slice.drop`, which zeroes the descriptor
  before ending its pin and releasing its owner.
- `ValueType('slice').trace` visits only the owner Vec as one immediate strong
  edge. The private collector from `c934dd7` counts those edges, retires all
  white-row Slice descriptors (including nested struct/enum fields) and skips a
  second owner release.
- `std/slice.view`, `sub`, `len`, `to_vec` lower to `Slice.create`, `sub`,
  `len` and `to_vec(T element)` respectively. `to_vec` passes T's emitted
  `ValueType` only as fallback metadata for the owner-less blank descriptor.
  A live view always uses the authoritative `owner.element`; a distinct but
  equivalent or unrelated Python descriptor neither raises nor replaces the
  real element copy/drop/trace operations.
- `IndexRead`/`IndexPlace` use the existing `Slice.get/set/at` relative-index
  helpers. `VecSlice` chooses `Slice.sub` only when the receiver type is
  `Slice`; the Vec and String branches are unchanged.
- `ForIn` on a Slice uses `Slice.len()` for the bound and does not call
  `Vec.begin/end`. The Slice descriptor's own lifetime pin is the only storage
  pin; a loop value copy and its drop use normal logical copy/drop.
- Source function entry and loops keep their existing `_rt._gc_poll()` safe
  points.

No source `Any`, bare `usize` descriptor, fallback Vec copy, or deep copy is
introduced.

## Capability boundary

Python/64 now accepts `Slice[T]` and `slice_system_operations`; the previous
`E_CAPABILITY_UNAVAILABLE` for `std/slice` is gone. Other unsupported
capabilities stay rejected: the `unsupported-c` fixture declares `Ptr[u8]` and
must still produce `E_CAPABILITY_UNAVAILABLE` from both the CLI and helper.

## Tests

`run_slice_source.py` builds every accepted fixture through the real `woozyc`
CLI (`--backend python`) and the real `lang_python_emit` helper, requires
byte-identical `program.py`/`langrt.py`, and runs the program under
`observe.py`.

- Positive: `python-slice`, `core`, `owned`, `cleanup`, `throws-return`,
  `after-drop`, `empty`, `move`, `nested-to-vec`, `for-in`, `compound`,
  `reassign`, `gc-cycle`.
- Panic: structural push/pop/clear/resize/reserve, empty view pin, negative and
  huge index, view/endpoint bounds, bounds order/high, subview bounds, negative
  and huge subview endpoints.
- Negative: `unsupported-c` retains the CLI/helper capability gate.
- `observe.py` disables CPython automatic GC before importing `langrt`, replaces
  `gc.collect` with an error, and checks exact logical Vec
  retain/release/create/drop, Slice pin/end, descriptor create/copy/drop/
  retirement and final `_LIVE_VECS`/`_gc_stats` roots. It also runs direct
  regressions with distinct equivalent descriptors for trivial and Vec-owned
  elements, an unrelated live-view fallback (must keep `owner.element` and the
  inner owned copy), and the typed blank fallback.

## Explicit limitations

This step does not complete LANG03. `inout Slice`, `Ptr`/`PtrMut`,
field/index `inout` overlap rules, deep Slice copies, source Weak eligibility and
Native GC are unchanged and unclaimed. Python source Slice uses the private
descriptor runtime, not Native GC.
