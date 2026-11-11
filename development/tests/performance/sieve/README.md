# Inclusive sieve preparation fixture

This directory contains a minimal preparation check for the same single-threaded
Sieve of Eratosthenes written in Source and hand-written C++20. It does **not**
measure time, throughput, or speedup, and it does not claim a `<=2x` result.

The fixture uses an inclusive bound `N`, a `Vec[u8]` / `std::vector<uint8_t>`
with exactly `N + 1` zero bytes (never `std::vector<bool>`), the same zero
initialization, per-round reset, `p * p` composite marking, and count/u64-sum
loop. There is no output, FFI, threads, SIMD, timing call, LTO, fast-math, or
`-march=native`.

The current Source `Vec` interface has no sized-zero constructor, so
`prepare.py` renders the fixed zero list. The bound is read from a one-element
runtime `Vec[i32]` in both programs; this keeps the optimized Source IR from
constant-folding the small cases away, while the C++ reference performs the
same source-level read. This is a fixture workaround, not a new stdlib API.

## Fixed inputs and exits

| N | rounds | correct count | correct sum |
| --- | ---: | ---: | ---: |
| 2 | 8 | 1 | 2 |
| 3 | 8 | 2 | 5 |
| 31 | 8 | 11 | 160 |
| 100 | 8 | 25 | 1060 |
| 4095 | 8 | 564 | 1070091 |
| 65535 | 8 | 6542 | 202288087 |

The expected values come from an independent Python trial-division oracle in
`prepare.py`; they are not read from either sieve implementation. Every round
compares the count and u64 sum. Correct values exit `0`; count `+ 1` exits `1`;
sum `+ 1` exits `2`. All three variants are generated for every N, including
`N = 65535`, and are built and run at Native O0/O2 and C++ O0/O2. The script
runs the small bounds first and stops on the first failure.

## What prepare checks

For every correct case:

- public `woozyc check` JSON;
- Native O0/O2 and C++ O0/O2 builds, ELF64 x86_64 executable checks, and quiet
  executions;
- Source LLVM `before.ll` / `after.ll` from `lang_native_helper`, two explicit
  `opt -passes=verify` runs, helper-object SHA-256, and byte equality with the
  formal `woozyc build` `program.o`;
- `opt -passes=print<loops>` on the O2 Source IR, limited to the one function
  that contains a real `store i8 1` marking write (that is `main` when the
  sieve was inlined, or the separate sieve function otherwise). The function
  must contain `load i8`, at least four loops, and the marking-store block must
  be inside one of those loops. Zero-fill `store i8 0` counts are recorded
  separately and are not accepted as marking writes;
- a negative control that changes one real `store i8 1` to `store i8 0` in the
  retained O2 IR. The mutated IR must pass `opt -passes=verify`, and the
  marking-write check must reject it. The mutated program is never executed.

ELF checks use real `llvm-readelf -l`, `llvm-readelf -d`, and `llvm-nm -u`
commands recorded with argv/stdout/stderr/exit:

- every Source executable must have no `INTERP`, no `NEEDED` entry, and no
  strong (`U`) undefined symbol; weak undefined symbols are recorded but
  allowed;
- every C++ executable must be the expected dynamic executable with `INTERP`
  and a non-empty `NEEDED` list (currently `libstdc++`, `libm`, `libgcc_s`,
  `libc`), and its strong/weak undefined lists are recorded separately;
- no executable may have a Python `NEEDED` entry.

The wrong count/sum variants are built and executed at both optimization levels
in both languages with the expected quiet exit. Tools, templates, generated
sources/references/manifests, oracle, and repository HEAD/status are hashed
before and after and must remain unchanged.

`prepare.json` retains every command's actual argv, status, exit, stdout,
stderr, timeout, and elapsed duration. The elapsed values are diagnostic command
durations only, not performance samples. The run also writes `oracle.json`,
`changes.patch`, and the retained LLVM IR/object artifacts.

## Running

The production tools used by the recorded preparation run are under
`/home/qin/Work/language-dev-20261103`; `compiler-root` must point at the main
compiler checkout. The script may run from that checkout or from a worker
worktree, but the supplied `woozyc`, helper, and runtime must be under
`compiler-root` (the worker must not point them at a worker build). The CMake
cache must record `BUILD_TESTING=OFF`, and the script does not rebuild or
replace the supplied tools.

```sh
export MAIN=/home/qin/Work/language-dev-20261103
export PY="$MAIN/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12"
export RELEASE="$MAIN/compiler/build/native-perf-release"
export LLVM="$MAIN/toolchains/llvm-22.1.8/bin"
export XML="$MAIN/toolchains/libxml2-legacy-2.13.9/usr/lib"

"$PY" tests/performance/sieve/prepare.py \
  --out checks/sieve-preparation-20261109-correction1 \
  --woozyc "$RELEASE/woozyc" \
  --helper "$RELEASE/native/lang_native_helper" \
  --runtime "$RELEASE/native/liblang_native_runtime.a" \
  --llvm-bin "$LLVM" \
  --compiler-root "$MAIN/compiler" \
  --ld-library-path "$XML" \
  --cpu 4
```

The output directory must not already exist. A passed run is a correctness and
artifact check, not a formal performance measurement; no measurement stage is
provided by this fixture.
