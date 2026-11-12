# FFI02 Python interop operations

Files:

- `CONTRACT.md` — declared property/index/slice schema and type rules.
- `binding.json` — additive FFI02 test bundle (old declarations + access arrays).
- `host.py` — ordinary Python `property`/`__getitem__` fixture with call counts.
- `run_interop.py` — real-build/execute runner plus static negatives and T10 control.

## Build (LLVM off, separate from the main build)

```sh
cmake -S . -B build-ffi02 -DLANG_ENABLE_LLVM=OFF -DBUILD_TESTING=ON
cmake --build build-ffi02 -j8
```

The runner needs the generated-program helper and the dedicated CPython 3.12.15
NumPy/PyTorch environment:

```sh
T10=/home/qin/Work/language-dev-20261103/toolchains/t10-python-cpu/venv/bin/python
"$T10" tests/python/interop_ops/run_interop.py \
  --woozyc build-ffi02/woozyc \
  --helper build-ffi02/tests/python/lang_python_emit \
  --root "$PWD" \
  --out /tmp/ffi02-tests \
  --python "$T10" \
  --binding tests/python/interop_ops/binding.json
```

`run_interop.py` builds every positive case with the public `woozyc build`
command, copies `host.py` as `interophost.py` beside the generated files, and
executes them with the supplied interpreter. It also runs the old T10
`order-objects-errors`, `foreign-ownership-move-aggregate` and
`foreign-ownership-cleanup` cases as a compatibility control.

## Continuous tests

The local verification build was configured with the exact 3.12.15 interpreter
and the T10 venv, so `ctest` does not report environment-only failures:

```sh
cmake -S . -B build-ffi02 -DLANG_ENABLE_LLVM=OFF -DBUILD_TESTING=ON \
  -DLANG_PYTHON312=/home/qin/Work/language-dev-20261103/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12 \
  -DLANG_T10_PYTHON=/home/qin/Work/language-dev-20261103/toolchains/t10-python-cpu/venv/bin/python \
  -DLANG_TEST_PYTHON=/home/qin/Work/language-dev-20261103/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12
cmake --build build-ffi02 -j8
cd build-ffi02 && ctest --output-on-failure -j4
```

Last local result: `interop_ops` 27/27 passed; `ctest` 44/44 passed with
LLVM disabled (Native-LLVM runtime tests are not run in that configuration).
The FFI02 change itself does not touch `runtime/python/langrt.py`,
`src/backend/native`, `src/hir/ownership.cpp` or any CMake file.
`src/hir/foreign.hpp` now extends the existing `binding_type` /
`binding_records` helper with property/indexer/slice records, so
`tests/bindings/static_test.cpp` and the compiler use one projection.

## Negative checks

`run_interop.py` checks undeclared, read-only, wrong-type, multi-index,
compound-assignment, Native-rejection and malformed-binding cases
(unknown field, wrong error type, duplicate property/indexer/id, and explicit
`properties:null` / `indexers:null` / `slices:null`). Each command
has a finite timeout and its complete stdout/stderr is stored in
`results.json` under the requested output directory.
