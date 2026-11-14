# Native observation driver (test component)

This directory is a **test-only** component for `tests/native/run.py`.  It is
not a product CLI, adds no `--emit` option, and does not change compiler source
or published APIs.

`main.wz` executes the real self-hosted production pipeline for one manifest:

```text
project.load
  -> resolve.resolve_project
  -> sema.check_project(..., Backend)
  -> nativelower.lower
  -> nativeverify.snapshot -> nativeirdump.dump   (Native CFG text)
  -> nativeemit.emit                              (pre-optimization LLVM text)
```

The manifest's `word_bits` is preserved, while the backend is forced to
`native` exactly as the old C++ helper did.  The entry is selected by scanning
the public `resolved.symbols` (function, entry module, name `main`).  The
snapshot is used only for the CFG dump; `nativeemit.emit` receives the
`VerifiedModule` published by `nativelower.lower`, never the snapshot and never
a fabricated verified value.

The driver emits only `--out-llvm-before` (pre-optimization LLVM) and
`--out-native` (Native CFG).  It deliberately has no object output.  The Python
runner performs the fixed `opt`/`llc` steps, independent Clang/LLD link, ELF
checks, and execution assertions.  The C++ helper is never called in candidate
mode.

## Build once

The component is built once at O0 with the frozen C++ seed:

```sh
python3.12 selfhost/tests/nativeobs/run.py \
  --seed /path/to/seed/build/woozyc \
  --toolchain-root /path/to/toolchains \
  --work-dir /path/to/work
```

This stages the current `selfhost/src/wzcc` plus `main.wz`/`lang.toml`,
builds `driver-build/program`, records source/tool/seed/driver SHA-256 values
and each process's actual `exit`, `timed_out`, `cleanup_complete`,
`abandoned_pipes`, stdout/stderr and their SHA values, then runs the smoke
subset through `tests/native/run.py --smoke`:

- `unit-main` O0/O2
- `nonzero-main` O0/O2
- `native32` -> `E_CAPABILITY_UNAVAILABLE`, no object
- `ordinary-parameter-write` -> `E_TYPE_MISMATCH`, no object

Reuse the same binary for the full matrix; do not rebuild it when no source
changed:

```sh
python3.12 selfhost/tests/nativeobs/run.py \
  --seed /path/to/seed/build/woozyc \
  --toolchain-root /path/to/toolchains \
  --work-dir /path/to/work --skip-build --no-smoke

python3.12 tests/native/run.py \
  --build /path/to/seed/build \
  --toolchains /path/to/toolchains \
  --wz-native-helper /path/to/work/driver-build/program \
  --seed /path/to/seed/build/woozyc \
  --reference-helper /path/to/seed/build/native/lang_native_helper \
  --out /path/to/full-out --report /path/to/full-report.json
```

The default invocation of `tests/native/run.py` (without
`--wz-native-helper`) remains the frozen C++ reference-helper behavior.

## Fixed LLVM arguments

For O2 the runner uses `verify,default<O2>,verify`; for O0 it uses
`verify,default<O0>,verify` only when the emitted module reports `has_async`,
otherwise `verify`.  It then runs:

```text
llc -filetype=obj -O=<0|2> -mtriple=<triple> -mcpu=generic \
    -relocation-model=pic -code-model=small after.ll -o program.o
```

Both pre- and post-optimization LLVM are checked with
`opt -passes=verify -disable-output`.

## Scope and non-claims

- Only the original Native scalar runner cases are covered: 38 shared
  prerequisites plus 3 generated supported sources, each at O0/O2, and the two
  rejection controls.  Other disabled Native families are not claimed.
- The Native CFG file is written but not compared byte-for-byte with the C++
  helper; generating a file is not evidence that two CFGs are equivalent.
- No public `compile_object`, `native_entry`, `--emit`, or other product API is
  added.  The component imports existing production functions only.
- The C++ helper and its four-file output remain the default reference mode.
  Candidate mode records the reference helper only as a SHA-256 reference.

Run `python3 -B selfhost/tests/nativeobs/test_controls.py` for pure-Python rejection controls. These check finite positive timeouts before creating output directories and require exit 0 with correctly typed cleanup flags for build, reuse, smoke and full runs. They do not compile or execute Woozy.
