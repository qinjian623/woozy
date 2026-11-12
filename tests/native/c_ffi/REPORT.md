# FFI01 correction evidence

operation_id: `woozy-cabi-correction-20261112`
corrects_task: `24da0aec-790d-4745-b082-2608bad3f401`
base HEAD: `148db60b4be4f4b599bf1056d89a4d7d360f2dd8`
contract: `compiler-dev/1`
toolchain: LLVM/Clang/LLD 22.1.8, Python 3.12.15 (T10 CPU venv)

This report is evidence for the three reproduced corrections. It does not mark
FFI01 as fully complete.

## Commands

Configure/build:

```sh
cmake -S . -B build/ffi01 -G Ninja -DLANG_ENABLE_LLVM=ON \
  -DLLVM_DIR=/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/lib/cmake/llvm \
  -Dzstd_STATIC_LIBRARY=/home/qin/Work/language-dev-20261103/toolchains/zstd-1.5.7/lib/libzstd.a \
  -DCMAKE_CXX_COMPILER=/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/clang++ \
  -DCMAKE_C_COMPILER=/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/clang \
  -DLANG_TEST_PYTHON=/home/qin/Work/language-dev-20261103/toolchains/t10-python-cpu/venv/bin/python \
  -DLANG_T10_PYTHON=/home/qin/Work/language-dev-20261103/toolchains/t10-python-cpu/venv/bin/python
cmake --build build/ffi01 -j8
```

In-tree runner and regression:

```sh
ctest --test-dir build/ffi01 -R '^native\.c_ffi' --output-on-failure
ctest --test-dir build/ffi01 -R \
  '^(native\.(c_ffi|ir|llvm|cli|cleanup|ownership|generics|strings|floating)|sema\.(t07|t08|t09|t10)|python\.(interop_operations|t10_bindings|foreign_ownership|t10_cli)|modules\.|bindings\.)' \
  --output-on-failure
```

Latest combined run: 21/21 passed. `native.c_ffi` and
`native.c_ffi_forged` passed; T10/interop and the listed native regressions
passed. Runner report:

```text
build/ffi01/tests/native/c-ffi-results/report.json
status=passed, failed=[], runs=43
```

## Correction 1: record callbacks and C ABI

The generated C adapter now owns the real C callback trampoline. It receives
the C-classified arguments, stores them, and calls the LLVM bridge
`lang_c_callback_bridge_v1_<function>_<callback-type>(void *out,
void *const *args)`. The bridge calls the Woozy function. No C record ABI is
invented in LLVM.

Exact independent-review reproductions were rebuilt with the current compiler:

```text
record-callback O0: build exit 0, output 2211, run exit 0
record-callback O2: build exit 0, output 2211, run exit 0
record-return   O0: build exit 0, output 2211, run exit 0
record-return   O2: build exit 0, output 2211, run exit 0
```

In-tree `projects/scalars` additionally checks Point/tiny/large record
parameters and returns, narrow signed/unsigned/bool/f32 callback, f64 callback,
and narrow `i16` callback return at O0/O2 (exit-code oracles 54-62).

## Correction 2: Buffer record-field layout

`Buffer[T]` is a 16-byte `{pointer,u64}` value with align 8. The schema's
natural-layout check now accepts a Buffer record field only at that layout.

```text
edge/record-buffer:   ok
edge/record-buffer-8: error E_BINDING_INVALID (declared size/align mismatch)
```

In-tree `fixture.BufferView` has `tag` at offset 0 and `data Buffer[u8]` at
offset 8, size 24/align 8; it is returned and passed by value through C, its
Buffer field is read and length-checked. The in-tree negative
`negative/buffer-field-size` is rejected with `E_BINDING_INVALID`.

## Correction 3: hosted Unicode

`runtime/hosted/sources.cmake` now appends only:

```text
runtime/native/unicode/unicode.cpp
runtime/native/unicode/utf8proc_adapter.c
```

The hosted archive contains `lang_unicode_normalize_v1`. Exact independent
review hosted project:

```text
hosted-unicode O0: build exit 0, run exit 0
hosted-unicode O2: build exit 0, run exit 0
```

In-tree `projects/unicode` checks NFC/NFD/casefold at O0/O2. Default
freestanding core remains `-nostdlib -static` with no `INTERP`/`NEEDED`; the
runner checks this.

## Hashes at report time

```text
91e993a9564d77db204106e775bf82758ba93e0f89b8a18a8db9ea66f1532c1f  src/backend/native/emit.cpp
84827caabc9d8b73eb780a64823340f8f2a95a565a8fc27027757f1f9494dfae  src/backend/native/c_ffi.cpp
0b3c7bbe38cc49b143616755a2c7ccf8e51f246386eb5a5bc3244ead9b2f9633  src/backend/native/c_ffi.hpp
f8218a900978de4a2fbc0ce34cffce0f1ac97e2e8a0e11a957b372805e2a662e  src/modules/project.cpp
d39f0eb90a6773d53c3ce64b0a03fe196e56f4901f482bea3ec7dfab7c5b37f9  runtime/hosted/sources.cmake
5477378cc9bea1f554e3af3d27f0a83a5bf723f7ce40a224f9fb0048d04a5b68  tests/native/c_ffi/fixture/ffi_fixture.c
7f999219800e9b71d826fdde75a12aa948c669ecd821b79c69a0f25d99ce6f7c  tests/native/c_ffi/bindings/fixture.json
6637d400d83f3f38514c9b7639401c1bee8a10184323456d0285511250c05f84  tests/native/c_ffi/run_c_ffi.py
fffacff04bbd5ce257458ba6ec31013147a52a38bef66a512323cd7e8fa77a61  tests/native/c_ffi/CONTRACT.md
e90c05c61d20177f2b2443fc3131509d968dfb400898c10b7f83718cc1f638bf  stdlib/ffi/ffi.wz
15248ada51e1307fbd0070df5e70c5d80775d2d107201523879117cbf33d8eba  contracts/c-bindings.schema.json
```

## Remaining / not claimed

- Retained callback registry/unregistration/GC and captured closures are still
  unsupported and explicitly diagnosed/limited.
- C varargs, arrays, unions, packed/bitfield records, C record construction and
  record field assignment remain unsupported.
- C++ ABI, non-x86_64 targets, and the complete hosted standard-library matrix
  are outside this correction.
- Full repository CTest was not run; the listed 21-test regression plus the
  43-row C-FFI runner is the evidence set.
- The hosted thread timeout ABI change is owned by Root and is present in the
  current working tree; the threads test depends on it. This correction did not
  modify Root's start/process/thread files.
