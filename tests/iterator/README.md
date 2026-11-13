# User Iterator for-in fixtures

Scope: `std/iter.Iterator[T]` and the `for item in iterator` form described in
`docs/SEMANTICS.md`. The collection expression is evaluated once, copied into
a private iterator cursor, advanced only through `next(inout Self)`, and ends
on the real `std/option.Option[T].None`. Existing Vec/Slice/string for-in is
unchanged.

Accept fixtures check:

- Native and Python `check`.
- Native O0/O2 build and actual execution with the independent expected return.
- Native allocation-observer link: every observed Vec allocation is freed and
  no live object remains.
- Python execution and import through `tests/python/observe_ownership.py`:
  no live language owners, balanced allocation/free records, and the same main
  return.

Accept fixtures also cover a generic `impl[T] Iterator[T] for Wrapper[T]`,
an exact `T: Iterator[i32]` bound on a receiver that also implements
`Iterator[bool]`, the existing explicit `(inout cursor).next()` entry, and the
three pre-existing Vec, Slice and string for-in forms. The Slice case runs
the normal Python CLI program but skips the shared ownership-observer import
because that older observer cannot JSON-encode a Slice snapshot; its native
observer and both backends' actual returns still run.

Reject fixtures cover: missing implementation, a same-named fake
`Iterator`/`Option`, two `Iterator[A]` applications for one Self, non-Option
`next`, throwing `next`, ordinary `Self` instead of `inout Self`, and writing
the read-only for-in element binding. Each must fail both frontends with
`E_TYPE_MISMATCH` and leave no generated program.

The ownership static test also forges HIR protocol identities: a same-signature
function outside the Iterator impl as `next`, a same-named non-standard
`Iterator` trait, and a same-named non-standard `Option` payload identity. Each
must be rejected by HIR verification while the real concrete, generic and exact
application HIR remains accepted.

Current intentional limits: no associated types, no automatic `.iter()`
protocol, no generic `Iterator.next` of its own, no dyn Iterator lowering for
for-in, no implicit element conversion, and ambiguity is reported rather than
resolved from the loop body or an expected item type.
