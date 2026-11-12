# Python Source Weak acceptance

Task: `python-source-weak-operations-20261112`  
Contract: `compiler-dev/1`  
Runtime profile: Python/64, `compiler-dev/1-python-values/9`  
Authority: `docs/SEMANTICS.md` §7.1/§9 and the canonical target table in
`development/plans/REFERENCE_VALUES.md` §6.

## Scope

This directory is the only new test area for the Python Weak step. It does not
change shared CMake registration, HIR/Sema/IR, stdlib, or accepted baseline
fixtures. The implementation changes are limited to:

- `src/backend/python/emitter.cpp`: lower the published
  `weak_empty`/`weak_downgrade`/`weak_upgrade` value calls, select the target
  from the verified type, project only the exact canonical `std/box.Box` private
  `cell Vec[T]`, and reject native-only handles instead of simulating them.
- `runtime/python/langrt.py`: route the three operations to the existing
  `_rt.Weak` and canonical `std/option.Option` variant tags. No new wrapper
  framework, address registry, or public liveness API is added.

`Weak` itself remains the existing runtime handle: copy/drop are logical value
operations, `upgrade` retains exactly one strong owner, and the weak edge does
not keep the target alive. Box is represented as the canonical one-field tuple
whose field is the private `cell` Vec; upgrade re-wraps that returned Vec.

## Accepted Source cases

The runner builds every positive case through the public CLI
(`woozyc build --backend python`) and the private `lang_python_emit` helper.
The generated `program.py` and `langrt.py` must be byte-identical. Each program
then runs under `observe.py`, which disables CPython automatic GC before
importing `langrt`, forbids `gc.collect`, and requires:

- the expected `main` return and no stdout/stderr;
- `_gc_stats()["live"] == 0` and `len(_LIVE_VECS) == 0`;
- a direct runtime probe proving a dropped last strong Vec owner makes
  `Weak.upgrade()` return `None` without host collection.

Positive cases:

- Root reference values: `weak-vec-lifecycle`, `weak-box-lifecycle`,
  `weak-empty-upgrade-none`, `weak-fn-immortal`.
- `combo-fields-vec-enum`: Weak in a struct field, a Vec element, and an enum
  payload, copied through aliases; the last strong Vec owner leaves its block.
- `held-some-keeps-alive`: `Some` keeps the target alive across the original
  strong binding's scope, then dropping that `Some` makes `upgrade` return
  `None`.
- `generic-target`: generic `empty`/`downgrade` functions instantiated for
  `Vec[i32]` and `std/box.Box[i32]`.
- Old Source regressions: function values, a generic function argument, and a
  struct copy sharing a Vec.

Expected Source/capability failures:

| fixture | required result |
| --- | --- |
| `reject-user-struct` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-same-shape-struct` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-string` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-file` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-slice` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-foreign` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-native-handle` | `E_CAPABILITY_UNAVAILABLE`, concrete Python signature gate |

No rejected case may write a program. The CLI and helper must both retain the
same gate.

## Explicitly unsupported

Source capturing closures are still rejected at resolve time with
`E_CAPABILITY_UNAVAILABLE` (“capturing a local binding is unavailable”). The
Root `weak-fn-captured-dead` fixture is therefore recorded as **unsupported**,
not passed. Non-capturing function values are immortal Weak targets and are
covered by `weak-fn-immortal`. Native shared handles
(`std/thread.Thread`, `std/sync.Mutex/Condvar/AtomicI64`, `std/ipc.Memory`) are
valid frontend targets but remain an explicit Python capability error; they are
not simulated with host handles.

No arbitrary struct with a single Vec field is unwrapped: the Box projection is
selected only for the unique exported canonical `std/box.Box[T]` declaration
whose single private field is `cell Vec[T]` of the declaration's own parameter.

## Run

```sh
PY312=/home/qin/Work/language-dev-20261103/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12
cmake -S . -B build/weak-source-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLANG_ENABLE_LLVM=OFF -DBUILD_TESTING=ON \
  -DLANG_PYTHON312="$PY312"
cmake --build build/weak-source-release --target woozyc lang_python_emit -j 8

"$PY312" -s tests/python/weak_source/run_weak_source.py \
  --cli build/weak-source-release/woozyc \
  --helper build/weak-source-release/tests/python/lang_python_emit \
  --root . --out /tmp/weak-source-results --python "$PY312" --timeout 120
```

The runner writes `/tmp/weak-source-results/report.json` with command log,
source/generated hashes, diagnostics, observations, and the unsupported case.
It is intentionally not registered in shared CMake by this step.
