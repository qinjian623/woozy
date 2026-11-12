# `std/box` source contract

Task: source-level `Box[T]` for approved Weak and read-only-capture work
Contract: `compiler-dev/1`
Date: 2026-11-12

## 1 Public source interface

`stdlib/box/box.wz` defines the module. Callers use the public logical path:

```woozy
import "std/box" as box;

pub struct Box[T] { /* private one-element Vec[T] cell */ }

pub func new[T](value T) Box[T]
pub func get[T](value Box[T]) T
pub func set[T](value Box[T], next T)
```

These three functions are the complete public API. The implementation reuses
an ordinary generic struct and a private one-element `Vec[T]`. It adds no
keyword, intrinsic, heap ABI, compiler feature, public option, or utility API.

- The single field is not exported. External field reads, external struct
  construction naming the field, and unknown names such as `box.len` are
  rejected with `E_PRIVATE_ACCESS` or `E_NAME_NOT_FOUND`.
- There is no exported raw address, pointer, buffer, storage handle, length,
  capacity, push/pop/resize/clear, or any operation that can change the cell
  length.
- The canonical declaration is `std/box.Box`. A later Weak eligibility
  intrinsic may recognize that declaration. This module does not implement
  source-level `Weak`, downgrade/upgrade, or any Weak runtime behavior.

## 2 Value and ownership semantics

- `Box[T]` is a value struct. Copying a `Box[T]` copies the private `Vec[T]`
  handle by the ordinary Vec rule, so all copies share one cell.
- `new` creates a one-element `Vec[T]` and stores an ordinary copy of `value`.
- `get` performs an ordinary Vec element read. The result is a normal owned
  copy: scalar/bool/POD values are plain values; `Vec`, `string`, struct, enum,
  and nested `Box` values follow the existing logical-copy rule, not a deep
  copy.
- `set` performs ordinary Vec element assignment. The input is copied into the
  cell and the previous element is dropped exactly once by the existing Vec set
  operation. No new drop, retain, or lifetime mechanism is introduced.
- Assigning a `Box[T]` binding changes only which cell that binding references;
  it does not change another alias's cell. Mutating through any alias is
  observable through all aliases.
- A `Box[T]` value can outlive the lexical scope of the binding that created
  it; the private Vec handle retains its cell under the existing ARC rules.

## 3 Backends and IR

- Native and Python both use the real public CLI. Native builds also use the
  real `lang_native_helper`; Python cycle metadata uses the real
  `lang_python_emit` helper.
- Native lowering uses the existing source Vec operations. Owning elements
  lower to `lang_vec_at_v1`/`lang_vec_set_v1` after checked reads/sets;
  non-owning elements use the existing direct element path after the same
  checked Vec operations. No `lang_box_*` symbol exists.
- Python lowering uses the ordinary struct/tuple field and Vec `Index` paths.
  The Python collector already traces struct fields down to Vec graph edges,
  so a pure-Woozy cycle that passes through a `Box` field is a normal strong
  cycle.

## 4 Acceptance coverage

- Native O0/O2: aliases, owned `get`, overwrite, original-binding scope,
  scalar/struct/enum/string/Vec elements, `defer`, typed `throws`, nested
  `Box`, and Mutex-guarded multi-thread mutation.
- Python: the same shared behavior fixtures through the CLI, plus a real Box
  cycle reclaimed by the language collector with CPython collection disabled.
- Both backends reject private field reads and private construction, and both
  reject an unexported API name. No program is produced for those cases.
- Native observer evidence: every allocated Vec is freed exactly once, the
  live Vec set returns to zero, and repeated overwrite keeps the peak live Vec
  count bounded rather than leaking each old element.

## 5 Explicit limits

- Native cycle collection is not available in the current source tree. A
  Native `Box` cycle may remain allocated; this task does not assert reclaim
  and does not normalize a leak as a passing pipeline result.
- Source-level `Weak[T]`, Weak downgrade/upgrade, GC tracing changes, and the
  MEM01/MEM02 claims are outside this module. The canonical declaration is
  merely stable for later recognition.
- Cross-thread safety is not added by `Box`. Programs must synchronize shared
  mutation themselves; the Native fixture uses `std/sync.Mutex`.
- Python collection remains limited to pure Woozy strong cycles. Foreign
  Python/C objects and their cycles remain host-managed.
- No arbitrary new utility API, extra manifest option, compiler build change,
  or root documentation change is part of this contract.
