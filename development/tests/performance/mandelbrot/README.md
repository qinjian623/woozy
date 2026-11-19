# Mandelbrot preparation fixture

This directory contains a correctness and artifact preparation check for the
same single-threaded scalar Mandelbrot renderer written in Source and
hand-written C++20. It does **not** measure time, throughput, or speedup and it
does not claim a `<=2x` result.

The fixed algorithm is the official scalar one:

```text
cr = 2.0 * x / N - 1.5
ci = 2.0 * y / N - 1.0
z0 = 0
for at most 50 updates:
    zr2 = zr * zr
    zi2 = zi * zi
    zi  = 2.0 * zr * zi + ci
    zr  = zr2 - zi2 + cr
    if zr * zr + zi * zi > 4.0: escaped
row-major bitmap, MSB first in each byte, row padding bits zero
```

Both programs use `f64`/`double`, the same operation order, a zero-initialized
`Vec[u8]` / `std::vector<uint8_t>`, the same bit packing, and a full-byte
comparison with an independent Python complex oracle. There is no output, FFI,
threads, SIMD, timing call, LTO, fast-math, FMA, or `-march=native`.

## Fixed inputs

| N | bitmap bytes | padding bits per row |
| ---: | ---: | ---: |
| 1 | 1 | 7 |
| 7 | 7 | 1 |
| 8 | 8 | 0 |
| 9 | 18 | 7 |
| 16 | 32 | 0 |
| 200 | 5000 | 0 |

The independent Python oracle in `prepare.py` generates every expected byte.
For N=200 the oracle PBM is additionally compared byte-for-byte with the
official 5011-byte input at
`<dev-work-dir>/checks/algorithm-reference-inputs-20261109/mandelbrot-n200.pbm`
(SHA-256 `97610473750700638fc63d13cfa49d339b67c18e7f26b3f9c9acb61e746472d5`).
The expected bitmaps are never read from either implementation.

## Correct and error controls

Correct programs compare the complete bitmap and exit `0` quietly. Error
programs flip one actual bitmap bit after packing and compare it with the
unmodified oracle:

- `wrong-data`: byte 1 is flipped with mask `0x80` (N=1 has only one byte, so
  its first/only byte is used); the difference is not in the first byte for
  N>=7. The program exits `1`.
- `wrong-padding`: N=7 flips byte 1 mask `0x01`; N=9 flips byte 1 mask `0x01`.
  Those are row padding bits. The program exits `1`.

Both Source Native and C++ are built and run at O0/O2 for every correct and
wrong case.

## Real observers

The programs contain an empty `visit(bytes Vec[u8])` (`const std::vector<uint8_t>&`
in C++). Correct builds leave it empty.

### Native

For each prepared Source case, `prepare.py`:

1. reads the real `visit` SymbolId from the generated Python metadata and
   requires the exact `lang.fn.<SymbolId>` definition in the helper's
   NativeIR and pre-optimization LLVM;
2. requires that LLVM definition to have exactly one real `ptr` argument and an
   empty body;
3. injects `call void @mandel_observe(ptr <argument>)` before its return;
4. runs `opt -passes=verify` on the instrumented IR;
5. links `native_observer.cpp` + instrumented IR + the project runtime at O0
   and O2, runs the observer binary, and decodes its complete lowercase-hex
   bitmap.

The observer uses the real runtime C ABI from `runtime/native/vec.hpp`
(`lang_vec_length_v1` / `lang_vec_at_v1`). It does not recompute Mandelbrot.
The observer binary depends on the host libc and runtime; it is test-only and
is not a standalone Native performance program.

`negative-data` and `negative-padding` variants flip the actual byte and also
embed the matching wrong expected byte, so the formal program exits `0` but the
Native observer must capture the exact wrong byte and reject it against the
oracle.

### Python

`lang_python_emit` generates the unmodified `program.py`, `langrt.py`,
`verified.hir`, and `metadata.json`. `visit_trace.py` imports that module, finds
`visit`/`main` by metadata, wraps the original empty `visit` to record the
actual complete `Vec[u8]`, calls the original visit, and fully executes `main`.
It does not recompute Mandelbrot. The same correct/negative byte checks apply.

## Shared preparation helpers

`prepare.py` loads `tests/performance/sieve/prepare.py` through `importlib`
under the unique module name `_mandelbrot_reuse_sieve_prepare`. It reuses only
the already-read generic helpers: `need`, `now`, `file_rec`, `manifest_text`,
`tool`, `run_program`, `require_exit`, `verify_ir`, `build_langc`,
`parse_functions`, `parse_loop_sections`, `parse_undefined`, `elf_checks`, and
the function/label regexes. Sieve's algorithm, `main()`, `parse_args()`, task
constants, fixed-input statistics, and `build_cpp` are not used.

`build_cpp` remains local because `sieve/prepare.py` uses its own
`TARGET_FLAGS`, which does not include `-ffp-contract=off`. Reusing it would
reintroduce `llvm.fmuladd` (FMA) into the C++ O2 IR and change the required
scalar operation order. The Mandelbrot-specific oracle, templates, visit
instrumentation, observer checks, work checks, and report remain local.

## What prepare checks

For every N:

- public `woozyc check` JSON for the correct case;
- Source/C++ O0/O2 builds, ELF64 x86_64 checks, and quiet executions;
- Source LLVM `before.ll`/`after.ll`, two explicit verifier runs, helper-object
  SHA-256 and byte equality with the formal `woozyc build` `program.o`;
- exact `visit` SymbolId recognition and actual call counts: O0 positive, O2
  zero; an unknown symbol fails instead of counting zero;
- O2 real-work checks on Source and C++ IR: a real loop, `fmul double`,
  `fadd double`, `fcmp`, `or i8`, a non-zero `store i8` inside a loop, no
  fast/contract float flags, and no FMA intrinsic. C++ is compiled with
  `-ffp-contract=off`. For N=1 only, clang -O2 constant-folds the single-byte
  C++ pack to a direct status while the float loop remains; Source N=1 still
  retains the pack store, and the C++ exception is recorded explicitly;
- C++ O0/O2 IR export, verifier runs, and debug-metadata-decoded mangled
  `visit` call counts: O0 positive, O2 zero;
- Native and generated-Python full-bitmap observation against the independent
  oracle, including all padding bytes/bits;
- negative observer variants must capture and reject the exact wrong bit.

Formal Source CLI ELFs must have no `INTERP`, no `NEEDED`, and no strong
undefined symbol. C++ formal ELFs must be dynamic with non-empty `NEEDED`.
Observer ELFs are recorded separately as host-libc-dependent test binaries.

`prepare.json` retains every command's actual argv, status, exit, stdout,
stderr, timeout, and elapsed duration. Elapsed values are diagnostic command
durations only, not performance samples. The run also writes `oracle.json`,
`changes.patch`, observed hex files, and all generated cases.

## Running

The production tools used by the recorded preparation run are under the
development work directory (the repository's parent by default; set
`WOOZY_WORK_DIR` to override); `--compiler-root` must point at the main
compiler checkout (`$REPO`). The script may run from that checkout or from a worker
worktree, but the supplied `woozyc`, helper, and runtime must be under
`--compiler-root`. The CMake cache must record `BUILD_TESTING=OFF`.

```sh
REPO="$(git rev-parse --show-toplevel)"
export MAIN="${WOOZY_WORK_DIR:-$(dirname "$REPO")}"
export PY="$MAIN/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12"
export RELEASE="$REPO/build/native-perf-release"
export LLVM="$MAIN/toolchains/llvm-22.1.8/bin"
export XML="$MAIN/toolchains/libxml2-legacy-2.13.9/usr/lib"

"$PY" tests/performance/mandelbrot/prepare.py \
  --out checks/mandelbrot-preparation-20261109 \
  --woozyc "$RELEASE/woozyc" \
  --helper "$RELEASE/native/lang_native_helper" \
  --runtime "$RELEASE/native/liblang_native_runtime.a" \
  --llvm-bin "$LLVM" \
  --compiler-root "$REPO" \
  --ld-library-path "$XML" \
  --python "$PY" \
  --python-helper "$REPO/build/native-cli-release/tests/python/lang_python_emit" \
  --cpu 4
```

The output directory must not already exist. A passed run is a correctness and
artifact check, not a formal performance measurement; no measurement stage is
provided by this fixture.
