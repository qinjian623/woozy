# Matmul preparation fixture

This directory contains a correctness and artifact preparation check for the
same scalar, single-threaded, dense `f64` matrix multiplication written in
Source and hand-written C++20. It does **not** measure time, throughput, or
speedup, and it does not claim a `<=2x` result.

The fixed algorithm is:

```text
row-major A[N*N], B[N*N], zero-initialized C[N*N]
for i in 0..N-1:
  for j in 0..N-1:
    var sum f64 = 0.0
    for k in 0..N-1 in order:
      sum = sum + A[i*N+k] * B[k*N+j]
    C[i*N+j] = sum
```

Both programs use the same oracle literals for A and B, the same zero
initialization, the same i-j-k order, and one full per-element comparison with
the embedded expected matrix. There is no output, FFI, BLAS, threads, SIMD,
timing call, LTO, fast-math, FMA, or `-march=native`.

## Fixed inputs

| N | elements | A formula | B formula |
| ---: | ---: | --- | --- |
| 1 | 1 | `A[i][j] = ((17*i + 13*j) % 19 - 9) / 8` | `B[i][j] = ((11*i + 7*j) % 23 - 11) / 8` |
| 3 | 9 | same | same |
| 7 | 49 | same | same |
| 16 | 256 | same | same |

The A/B inputs, the complete exact C, and every C binary64 bit pattern come
from the already frozen independent oracle:

```text
<dev-work-dir>/checks/algorithm-reference-inputs-20261109/matmul-exact-small-oracle.json
SHA-256 98ced9047358ae30380d584f19e0f22688c46829b95ac20684e38c5022824a18
```

The oracle stores exact rationals (`Fraction`) and the C bits; it was not
derived from Source or C++. `prepare.py` verifies the SHA-256 before the run,
freezes a byte-identical copy in the output directory, verifies the fixed A/B
formulas, recomputes every matrix product and every running `k`-order partial
sum with exact `Fraction` arithmetic, and requires each one to be exactly
representable as binary64. Every expected bit pattern is decoded back to the
same exact rational. The embedded Source/C++ expected values are shortest
exact decimal renders of those rationals, and each render is parsed back and
checked. Expected values are never read from either implementation.

## Correct and error controls

- `correct`: unmodified C, oracle expected matrix, full element comparison,
  quiet exit `0`.
- `wrong`: the last real C element gets `+ 1.0` after the loops, while the
  embedded expected matrix remains the oracle; both languages exit `1`. For
  N=3,7,16 this is a non-first element; N=1 has only one element.
- `negative`: the last real C element gets `+ 1.0` **and** the embedded
  expected matrix is changed to the matching wrong value, so the program exits
  `0`. The Native and generated-Python observers must capture the exact wrong
  bit patterns and reject them against the oracle, so an observer cannot pass
  by reading the embedded expected matrix or by only observing the program
  exit status.

Every N and every variant is built and run for Source Native and C++ at O0 and
O2; all executions must be quiet.

## Real observers

The programs contain an empty `visit(values Vec[f64])`
(`const std::vector<double>&` in C++). Correct builds leave it empty.

### Native

For each prepared Source case, `prepare.py`:

1. reads the real `visit` SymbolId from the generated Python metadata and
   requires the exact `lang.fn.<SymbolId>` definition in the helper's
   NativeIR and pre-optimization LLVM;
2. requires that LLVM definition to have exactly one real `ptr` argument and an
   empty body;
3. injects `call void @matmul_observe(ptr <argument>)` before its return;
4. runs `opt -passes=verify` on the instrumented IR;
5. links `native_observer.cpp` + instrumented IR + the project runtime at O0
   and O2, runs the observer binary, and decodes every element as a
   16-hex-digit unsigned 64-bit binary64 bit pattern.

The observer uses the real runtime C ABI from `runtime/native/vec.hpp`
(`lang_vec_length_v1` / `lang_vec_at_v1`) to read each stored `f64`. It does
not recompute the matmul. The observer binary depends on the host libc and the
runtime; it is test-only and is not a standalone Native performance program.

`negative` variants mutate the actual last C element and embed the matching
wrong expected value, so the formal program exits `0` but the Native observer
must capture the exact wrong bits and reject them against the oracle.

### Python

`lang_python_emit` generates the unmodified `program.py`, `langrt.py`,
`verified.hir`, and `metadata.json`. `visit_trace.py` imports that module,
finds `visit`/`main` by metadata, wraps the original empty `visit` to record
the actual complete `Vec[f64]`, calls the original visit, and fully executes
`main`. It prints each element as the same 64-bit IEEE-754 pattern text used
by the Native observer and the oracle. It does not recompute the matmul.

## Shared preparation helpers

`prepare.py` loads `tests/performance/mandelbrot/prepare.py` through
`importlib` under the unique module name
`_matmul_reuse_mandelbrot_prepare`. It reuses the already-read Mandelbrot/Sieve
generic helpers: `need`, `now`, `file_rec`, `manifest_text`, `tool`,
`run_program`, `require_exit`, `verify_ir`, `build_langc`, `parse_functions`,
`parse_loop_sections`, `elf_checks`, the empty `visit` recognizer
(`visit_function`), `source_call_count`, `decode_hex`, the mangled C++ visit
helpers, and `emit_python`. Mandelbrot's bitmap/oracle/templates/observer code
and Sieve's algorithm, `main()`, `parse_args()`, and fixed-input statistics are
not used.

`build_cpp` and the C++ IR export remain local because the reference needs
`-ffp-contract=off` plus `-fno-vectorize -fno-slp-vectorize` so the scalar
max-N O2 loop and separate multiply/add rounding stay visible. The
matmul-specific oracle, templates, mutation, Native/Python bit-pattern
observers, work checks, and report remain local.

## What prepare checks

For every N:

- public `woozyc check` JSON for the correct case;
- Source/C++ O0/O2 builds, ELF64 x86_64 checks, and quiet executions for the
  correct, wrong, and negative variants;
- Source LLVM `before.ll`/`after.ll`, two explicit verifier runs, helper-object
  SHA-256 and byte equality with the formal `woozyc build` `program.o`;
- exact `visit` SymbolId recognition and actual call counts: O0 positive, O2
  zero; an unknown or missing symbol fails instead of counting zero;
- O2 main/helper IR checks: no fast/contract float flags and no FMA intrinsic;
- C++ O0/O2 IR export and verifier runs, plus debug-metadata-decoded mangled
  `visit` call counts: O0 positive, O2 zero;
- Native and generated-Python full-matrix bit-pattern observation against the
  independent oracle for correct and negative variants;
- negative observer variants must capture and reject the exact wrong element.

At the maximum N=16, the Source optimized IR and the C++ optimized IR must each
still contain an actual loop with scalar `fmul double`, `fadd double`,
`load double`, and `store double`, with a `store double` inside a loop, no
vector `<N x double>` operations, no fast/contract flags, and no FMA. For the
smaller C++ N=1 and N=3, clang -O2 constant-folds the fixed matrix work; those
cases are recorded as correctness-only and are not performance evidence.
Source keeps its runtime `Vec` loops.

Formal Source CLI ELFs must have no `INTERP`, no `NEEDED`, and no strong
undefined symbol. C++ formal ELFs must be dynamic with non-empty `NEEDED`.
Observer ELFs are recorded separately as host-libc-dependent test binaries.

`prepare.json` retains every command's actual argv, status, exit, stdout,
stderr, timeout, and elapsed duration. Elapsed values are diagnostic command
durations only, not performance samples. The run also writes `oracle.json`, a
frozen oracle copy, `changes.patch`, observed hex files, and all generated
cases.

Tool and input SHA-256 values are recorded before and after the run; a change
fails prepare. The shared main compiler checkout HEAD/status is recorded before
and after but is not required to be unchanged: unrelated commits elsewhere do
not change this run's tool/input hashes, while any real tool or input change
does.

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

"$PY" tests/performance/matmul/prepare.py \
  --out /path/outside/the/worktree/matmul-preparation \
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
