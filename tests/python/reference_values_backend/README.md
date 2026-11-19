# Python reference-value backend probes

Task: `python-closure-backend-20261112`  
Contract: `compiler-dev/1`  
Base revision: `9f8c551` (schema/wiring `68b3cde`, planned source cases `9f8c551`)  
Authority: `development/plans/REFERENCE_VALUES.md` (reviewed Root contract),
`docs/SEMANTICS.md` §7.1/§7.2/§8/§9 and “已确定、尚待实现的闭包与 Weak 规则”.

This directory belongs to the Python backend owner and is the only new test
area of the task. It does **not** register anything in the shared CMake, does
not redefine HIR, and does not claim source closure/bound-method/Weak
acceptance.

## 1 What was implemented (src/backend/python/emitter.cpp)

- `emit_type` emits `_rt.ValueType('function')` for every Function type and
  `_rt.ValueType('weak')` for Weak (the latter is unreachable while the shared
  verifier rejects Weak).
- `owned()` also honours the published `ValueKind::function/weak` descriptors.
  The shared classifier still returns its stage-A result for Function
  (`ValueKind::trivial`, `needs_drop=false`), so old source keeps its exact
  previous ownership shape. When the shared ownership worker publishes
  `ValueKind::function`, the Python emitter starts treating Fn values as owning
  without further changes; it does not force that classification itself
  because the shared analysis would not have produced the matching
  temporary/copy records.
- Capturing `FunctionValue` creation: every capture child is evaluated in
  creation order, an owning Read is taken exactly once (the Read already
  produced one copy), a synthetic `Move` child transfers its temporary, and
  the values are installed into exactly one record row inside a `Vec`
  environment (`_rt.Vec.create(_rt.ValueType('struct', fields=...), (row,))`).
  The wrapper is `_rt.FunctionValue(_fN, env)`; noncapturing values keep the
  plain immutable `_fN`.
- Capturing lambda entry: `def _fN(_env, ...)`, then `_row = _env.get(0)` and
  `_s<local> = _row[i]` for each `HirFunction.captures` entry. Loading is a
  pure borrow: the emitter never calls `_env.get(i)` for i>0, never copies the
  row during entry, and never drops a capture field at function exit
  (`read_only_capture`, capture-locals set, `local_drops`, `block()` and
  `owned_bindings` all exclude capture locals).
- Bound method values: the single child is the receiver installed in the
  environment's row field 0. A generated `_boundN(_env, ...)` adapter passes
  the borrowed field for ordinary Self and, for move Self, does
  `ops.copy(_env.get(0)[0])` on **every** invocation before calling the method;
  it never takes or caches a consumable copy, so a bound Fn stays copyable and
  repeatable. `inout` Self binding is rejected with
  `E_CAPABILITY_UNAVAILABLE` defensively (the frontend owns the real rule).
- Indirect calls: when the callee is an owned temporary, `_frame.borrow(...)`
  is emitted immediately after callee evaluation and before any argument
  evaluation. The matching `end_borrow` steps come from the verified
  success/typed-error plans; the callee owner is released only at the end of
  the full expression.
- Constant function values keep the bare `_fN` and reject captures/bound
  defensively. C callbacks and the const-only/no-capture checks stay in the
  shared frontend/HIR/IR owners and were not changed here.

`runtime/python/langrt.py` was **not** changed: the existing `FunctionValue`,
`Weak`, `ValueType('function'/'weak')`, `Frame` borrows and cycle collector
already provide the needed operations. See §4 for the runtime-shape evidence.

## 2 What is verified

All commands were run in this worktree with GNU C++ Release, `LANG_ENABLE_LLVM=OFF`,
and CPython 3.12.15 from
`${WOOZY_TOOLCHAIN_ROOT}/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12`.

Focused build (no shared CMake edit):

```sh
: "${PYTHON312:?set PYTHON312 to the Python 3.12 executable}"
cmake -S tests/python/reference_values_backend -B build/reference-values-focused \
  -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLANG_RV_PYTHON312="$PYTHON312"
cmake --build build/reference-values-focused -j 8
ctest --test-dir build/reference-values-focused --output-on-failure
```

- `python.reference_values_emitter` — real checked old source
  (`tests/backend_common/fixtures/function-values`, `tests/cleanup/fixtures/case-08`,
  `tests/generics/fixtures/function-argument`) emits deterministically with
  `_rt.ValueType('function')`, no `_rt.FunctionValue(` wrapper and no `_bound`
  adapter. Forged `FunctionValue.captures`, `move_capture`, `bound`
  (ordinary and move), `HirFunction.captures` and `TypeKind::Weak` on that
  checked module are all rejected by `hir::verify` with `E_IR_INVALID`, so no
  forged module can reach the backend (no verifier bypass).
- `python.reference_values_runtime` — 13 private runtime-shape probes:
  one-row borrowed environment; entry borrows and does not drop capture
  fields; owned Read copy + child take exactly once; FunctionValue copy shares
  the environment; move-Self adapter copies the receiver on every call (and
  env field stays intact); ordinary-Self adapter borrows; callee borrow
  ordering on success/typed-error with no body entry on the error path; Weak
  empty/immortal/dead encodings and dead code never invoked.
- `python.reference_values_old_source` — the public CLI
  (`woozyc build --project ... --backend python`) plus CPython executes the
  three old-source fixtures and asserts the exact main result (0/0/5, silent
  stdout/stderr). It then checks every one of the 24 planned fixtures with
  `woozyc check --syntax-only` (all `status=ok`) and records them as `blocked`
  with the published current baseline diagnostics; `kind=reject` entries are
  explicitly **not** counted as passing negatives.

Full root suite (same worktree, all shared tests unchanged):

```sh
: "${PYTHON312:?set PYTHON312 to the Python 3.12 executable}"
cmake -S . -B build/reference-values-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLANG_ENABLE_LLVM=OFF -DBUILD_TESTING=ON -DLANG_PYTHON312="$PYTHON312"
cmake --build build/reference-values-release -j 8
ctest --test-dir build/reference-values-release -j 4 --output-on-failure
# 46/46 passed, 0 skipped
```

## 3 Blocked / deferred (no pass claimed)

- Source capturing closures, bound method values and source `Weak` are not
  accepted at this revision. `tests/reference_values` fixtures fail at
  resolve/type with `E_CAPABILITY_UNAVAILABLE` (capture), `E_IMPORT_NOT_FOUND`
  (`std/weak`) and `E_TYPE_MISMATCH` (method values); the HIR verifier rejects
  every nondefault `FunctionValue.captures/move_capture/bound`,
  `HirFunction.captures`, `read_only_capture` and `TypeKind::Weak`.
- Therefore the emitter’s capture/move/bound code paths and the indirect
  owned-callee borrow cannot be exercised from accepted source yet. They are
  implemented from the published contract and covered by the runtime-shape
  probes only. Do **not** describe these as source closure or LANG01 passes.
- Thread capturing entry, C-callback capture rejection, native IR negative
  tests, Native O0/O2 execution and Weak source acceptance belong to the
  shared owners and remain deferred.
- When the accepted frontend/ownership work lands, this directory must be
  updated: `run_reference_values.py` part 2 must emit, execute and compare the
  published expected values instead of recording blocked diagnostics, and
  `emitter_test.cpp` must assert the capture/bound emission shape (it will
  also need the stage-A `Function is trivial` compatibility assertion removed
  once the shared classifier returns `ValueKind::function`).

## 4 Runtime reuse evidence

`runtime/python/langrt.py` already had, before this task:
`FunctionValue` (env transfer on construction, retain on copy, release on
drop, env as first call argument), `Weak` (immortal `{code}` for no env, dead
`{None,None}` after release, upgrade returning `None` without calling code),
`ValueType('function'/'weak')`, `Frame.borrow/end_borrow`, and cycle
collection that traces a function’s environment Vec. The probes in
`runtime_reference_values_test.py` exercise exactly the shapes the emitter now
generates, including the one-row environment descriptor.
