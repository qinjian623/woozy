# Native reference-values backend checks

Scope: backend-only `Function` value representation and ABI checks. The shared
HIR capture schema is already published, but the capturing frontend has not been
accepted, so this directory does **not** claim source-level closure or bound
method integration.

Covered by `run_reference_values_backend.py`:

- Real no-capture source (`fixtures/function-abi`) built and executed at O0/O2.
  The case exercises direct calls, indirect calls, direct `try_call`, indirect
  `try_call`, a function returning a function value, a struct field and a
  `Vec[func]` element.
- Emitted LLVM for that program: `LangFn` is exactly `{ptr, ptr}`, every
  noncapturing value uses a cached env-first thunk, direct calls keep the
  original signature, indirect calls split code/env and use the env-first
  signature, and `Function` copy/drop retain/release a non-null environment.
- `tests/native/concurrency/basic` O0/O2: the thread context starts with
  `LangFn` then `A`, the entry calls `code(env, A)`, and copy/drop are generated
  unconditionally for both fields even when `A` is a scalar.

Not run here (and not claimed as passing):

- Capturing source environment construction and lambda entry slots: waiting for
  the accepted frontend snapshot. The backend rejects a capturing function
  whose `Op::function` has no real HIR origin/child inputs.
- Bound method adapters, because the frontend does not create `BoundReceiver`
  source values yet. The emitter-side adapter is implemented, but no source
  case reaches it.
- Source `Weak[T]`: `TypeKind::Weak` lowering is 16-byte `{control, code}` and
  its value ops are prepared, but the reviewed `SystemOperation` enum has not
  been published and HIR still rejects source `Weak`.
- C callback rejection of captures/bound values: no accepted frontend can
  currently produce such a callback argument. Noncapturing callbacks continue
  through the real source symbol and the existing CFFI suite.

Run:

```sh
ctest -R '^native\.reference_values_backend$' --output-on-failure
```
