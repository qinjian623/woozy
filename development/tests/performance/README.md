# Native performance preparation and measurement

This directory contains a deliberately small, reproducible native check for:

```text
lex/parse -> type check -> native LLVM IR -> ELF -> execution
```

It is a measurement harness, not a compiler optimization and not a claim that a
performance target has been met. A passed prepare run proves that the fixed
fixtures build, execute, and pass their full-width correctness checks. Run the
measurement stage on an otherwise idle host before drawing performance
conclusions.

The harness uses only the Python standard library. It does not modify the
compiler, backend, runtime, CMake files, shared ABI, baselines, or fixtures.

## Fixed inputs

Both fixtures use plain functions, local `var` bindings, integer expressions,
and a `for` loop. Neither program uses the standard library, output, FFI, or
timing functions.

| id | width | seed | iterations | full-width expected |
| --- | --- | --- | --- | --- |
| `u64-recurrence` | u64 | `0x123456789abcdef0` | 30,000,000 | `16012610758795272003` |
| `u32-branch` | u32 | `0x6d2b79f5` | 30,000,000 | `468995906` |

`u64-recurrence` is a nonlinear xorshift/multiply recurrence with wrapping
addition and multiplication. `u32-branch` has a data-dependent `if/else`
inside the loop; the optimizer may lower it to a `select`, but the loop and its
carried work must remain. The C++ reference uses the corresponding
`uint64_t`/`uint32_t` unsigned wraparound, the same seeds, iteration counts, and
shift amounts.

Each Source and C++ program contains the full expected value and exits `0` only
when the complete integer result matches. The harness also maintains an
independent Python oracle that masks after every intermediate operation. The
fixture metadata, `// PERF-META` lines, expected literals, and actual seed/count
calls are checked against that oracle. Because the native program has no output
or public FFI value, the harness observes the exact-equality exit status rather
than reading the final integer directly.

## Build-tree and actual-tool check

This harness is a check for this repository's CMake/Ninja build tree; it does
not promise to work with an arbitrary installed `woozyc`. Before any fixture
command, it reads `woozyc.parent/build.ninja` and `CMakeCache.txt` and requires:

- exactly one `LANG_NATIVE_HELPER`, `LANG_NATIVE_CLANG`, `LANG_NATIVE_LLD`, and
  `LANG_NATIVE_RUNTIME` definition;
- those paths equal the supplied `--helper`, `--llvm-bin/clang`,
  `--llvm-bin/ld.lld`, and `--runtime`;
- `CMAKE_BUILD_TYPE:STRING=Release`;
- all four actual tool paths exist and their SHA-256/version probes are saved.

The public `woozyc` driver uses the helper path embedded at compile time, and
that helper uses the embedded Clang, LLD, and runtime paths. The harness also
saves each formal `program.o` and compares it byte-for-byte with the object
exported by the independently invoked helper whose before/after IR was
verified. A mismatch fails prepare and blocks formal timing.

The old Debug prepare record is retained outside this directory but is not
reused: the Release report and tool hashes are different.

## Prepare

Prepare validates fixtures and artifacts, but does not produce formal timing
samples. Use a new `--out` path each time.

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
REPO="$(git rev-parse --show-toplevel)"
export MAIN="${WOOZY_WORK_DIR:-$(dirname "$REPO")}"
export PY="${WOOZY_TOOLCHAIN_ROOT}/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12"
export RELEASE="$MAIN/builds/t10-python-backend-root-integration/release"
export LLVM="$MAIN/toolchains/llvm-22.1.8/bin"
export XML="$MAIN/toolchains/libxml2-legacy-2.13.9/usr/lib"

"$PY" tests/performance/run_native.py \
  --prepare-only \
  --out build/performance-prepare/correction-1/prepare-release \
  --woozyc "$RELEASE/woozyc" \
  --helper "$RELEASE/native/lang_native_helper" \
  --runtime "$RELEASE/native/liblang_native_runtime.a" \
  --llvm-bin "$LLVM" \
  --compiler-root "$REPO" \
  --ld-library-path "$XML" \
  --cpu 4
```

For each of the two fixtures, prepare:

1. Runs public `woozyc check` and requires a JSON `ok` status.
2. Runs public `woozyc build --opt 0` and `--opt 2`, verifies ELF64 x86_64,
   saves `program.o` and its SHA-256, and executes the program once.
3. Compiles the C++ reference with Clang at `-O0` and `-O2` using the fixed
   `x86_64-unknown-linux-gnu` target, C++20, `-fno-fast-math`, and `-fno-lto`,
   then executes it once. These are eight correct executions total.
4. Uses the explicitly invoked helper to export before/after LLVM IR, native IR,
   and an object. `opt -passes=verify` must pass both IR files (eight files
   total), and the helper object must byte-match the public `woozyc` `program.o`
   for all four fixture/optimization pairs.
5. Runs `opt -passes=print<loops>` on optimized IR. At `-O2`, the IR must still
   contain a loop and expected arithmetic work; a constant-return artifact is
   rejected.
6. Derives short two-iteration Source and C++ programs under the run directory,
   computes the correct full-width result with the Python oracle, flips only the
   highest expected bit (leaving the low eight bits unchanged), and builds/runs
   both languages at O0/O2. All eight negative-control executions must return
   exit 1 with no stdout or stderr. A crash or Panic fails the control. These
   are correctness controls, not performance samples.

Every command is appended to `report["commands"]` with complete
`argv/status/exit/stdout/stderr/elapsed_ns/timeout_s` before success, quiet,
JSON, or ELF verdicts. The current case and each build/IR/run group are added
to the report before a possible failure. A failing check/build/IR/run therefore
leaves the failed command and all earlier records in `prepare.json`; timeout
stdout/stderr is retained too.

Prepare also records tool/source/binary SHA-256 values, the read-only compiler
HEAD/status, host/CPU data, the Release build-tree definitions, and child
affinity. `--cpu N` pins all child subprocesses to one CPU; without it,
affinity is recorded as not attempted.

## Formal measurement

Formal measurement requires the passed Release prepare report and a new output
directory. It reuses the prepared binaries and fixed inputs:

```sh
"$PY" tests/performance/run_native.py \
  --out build/performance-prepare/correction-1/measure-release-idle \
  --prepare-report build/performance-prepare/correction-1/prepare-release/prepare.json \
  --woozyc "$RELEASE/woozyc" \
  --helper "$RELEASE/native/lang_native_helper" \
  --runtime "$RELEASE/native/liblang_native_runtime.a" \
  --llvm-bin "$LLVM" \
  --compiler-root "$REPO" \
  --ld-library-path "$XML" \
  --cpu 4
```

The measurement stage:

- Rechecks the script, tools, build-tree definitions, fixtures, prepared
  binaries, `program.o`, and helper-object SHA-256 values; it also rechecks
  byte equality before timing.
- Records at least 5 fresh `woozyc check` process samples per fixture.
- Records at least 5 full public `woozyc build` samples and at least 5 full
  Clang C++ build samples per fixture and optimization level, alternating the
  tool order between samples.
- Provides grouped `count/raw_ns/median_ns/min_ns/max_ns` statistics for check
  and build samples by fixture, optimization level, and implementation. Only
  successful samples participate; a missing sample makes the report failed and
  is never counted as zero.
- Records at least 2 warmups and at least 7 measured executions per prepared
  binary. Candidate and reference runs alternate order by sample index.
- Reports raw nanoseconds and `median`/`min`/`max` per run item. A successful
  measurement does not apply a speed threshold.
- Warns if a measured minimum is below 50 ms, where process startup and loader
  overhead may be non-negligible.

The two build timings are not the same amount of work: public `woozyc build`
includes its frontend/check and native link, while the C++ command is only a
Clang compile/link of the reference. Compare them as separate observations.

Formal output is `measure.json` under the new `--out` directory. No report is
overwritten.

## Self-test and expected-failure controls

The short self-test exercises the oracle vectors, a high-bit full-width
mismatch, and a controlled nonzero command. It writes a temporary report and
reads it back to prove that the failed command's argv/exit/stdout/stderr and a
short-timeout command's status/output survive serialization:

```sh
"$PY" tests/performance/run_native.py --self-test
```

The correction run also has expected-failure controls under
`build/performance-prepare/correction-1/`:

- `failure-control/prepare.json`: a fake build-tree `woozyc` exits `9` on check;
- `timeout-control/prepare.json`: a fake build-tree `woozyc` prints markers and
  sleeps until the harness's short timeout;
- `control-results.json`: assertions that both saved reports contain the full
  failed/timeout command records;
- `formal-failure-control/measure.json` plus `formal-failure-result.json`: a
  synthetic injected check failure proving the formal branch also saves the
  failed command and current check row before validation.

These are harness self-checks, not compiler or performance failures. The formal
control does not run performance samples.

## Explicitly not run

- Cold file-cache measurement: no controlled cache-drop/flush entry. Process
  warmups do not imply cold-cache data.
- In-process incremental check timing: the public CLI has no such entry. Check
  samples are fresh processes.
- Hardware/software performance counters.
- CPU governor changes, CPU isolation, cache flushing, or killing other
  processes.
- Other kernels, targets, CPU models, or language features.

Every command has a finite timeout. A failed or timed-out command is preserved
in the JSON report, the stage exits nonzero, and missing samples are never
counted as zero. No performance conclusion is drawn from prepare alone.
