# Reference closure frontend cases

Task: LANG01 capturing closures, `move func`, bound method values and
function values. Contract: `compiler-dev/1`.

This directory is owned by the shared frontend/Sema/HIR/ownership/NativeIR
writer. It does not redefine AST/HIR/IR schema and does not edit the Root
`CMakeLists.txt`; `CMakeLists.fragment` is the registration patch for the
integrator.

## Coverage

`reference_closure_frontend_test.cpp` consumes the independent source cases in
`tests/reference_values/fixtures` (closure/bound/function/indirect subset; Weak
fixtures are not executed here because source Weak operations belong to the
parallel owners). For every positive case it runs the real lexer, parser,
resolver, Sema, independent HIR verifier, NativeIR lowering and independent
NativeIR verifier.

The test additionally proves by construction:

- capture order, origin/local/type identity and read-only capture lifetimes;
- nested transitive captures (inner source is the outer capture local while the
  original origin is stable);
- generic instances keep one origin, allocate distinct locals and specialize
  capture types;
- `move func` owning captures use one synthetic source `Move` while scalar
  captures are copied;
- generic `move func` conversion: a dependent capture becomes `Read` for a
  concrete scalar instance and `Move` for an owning instance;
- ordinary/move bound methods store exactly one receiver child and drop the
  first real Self parameter from the callable signature;
- `classify_value(Function)` is `ValueKind::function`, `needs_drop=true`,
  `pod_inout=false`.

Forged HIR is rejected independently of the producer: capture order/origin/
type, move_capture, capture children changed from copy to move or back,
synthetic Move source replacement, lambda local swaps, body reads that keep the
lexical origin instead of the lambda local, capture locals forged with source
definitions, and bound Self mode/receiver changes.

Forged NativeIR is rejected independently: `Op::function` origin dropped or
replaced, capture input order/drop, and capture slot identity changed.

Source rejections cover a const function value that captures, inout-Self bound
methods, reading an owning binding after `move func`, and assigning a read-only
capture.

## Actual execution

The independent `tests/reference_values` corpus remains the source of expected
runtime values. `run_reference_closure_cases.py` builds each positive fixture
with the real backend and executes it:

```sh
python3 tests/reference_closure_frontend/run_reference_closure_cases.py \
  --woozyc build/llvm-clang22/woozyc \
  --root . --python /path/to/python3.12
```

Native O0/O2 must return 0 for all thirteen closure/bound/function/indirect/
thread cases. Python 3.12 returns 0 for twelve; `thread-capture-scalar-arg` is
reported `not_run` on Python because the parallel Python backend still rejects
that call signature. Source `Weak` runtime cases are not part of this task.

## Verification performed

- frontend CTest: 47/47 passed after the new schema/classification
  expectations were updated (Function is owning/non-POD; capture source cases
  are supported).
- integrated Native CTest: 136/136 passed (the C5 aggregate-inout and
  bound-method-value expectations were corrected where the old tests assumed a
  Function was POD/unsupported).
- new closure frontend test: passed.
- Native O0 and O2: all thirteen runtime fixtures returned 0.
- Python 3.12.15: twelve runtime fixtures returned 0; thread capture is
  `not_run` with the backend diagnostic recorded.

## Explicit remaining limitations

- Python/64 still rejects `thread.spawn` with a capturing entry
  (`E_CAPABILITY_UNAVAILABLE: backend does not support this call signature`);
  Native O0/O2 execute it. This belongs to the parallel Python backend.
- Source `Weak` operations/runtime remain with the parallel owners; only the
  shared Weak schema/frontend from the snapshot is assumed here.
- Bound method values through a constrained `T` value receiver (trait method
  selection on a generic value) remain outside this frontend change; concrete
  nominal value receivers are implemented and exercised.
- Constant function values with captures or a bound receiver are rejected by
  design.
