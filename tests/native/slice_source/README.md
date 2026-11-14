# Native source `Slice[T]` runner

This directory is a test-only component.  It does not add a product compiler
entry point, change `Source`, fixtures, runtime, CMake, the catalog, or the
Slice forged CTest.

## Routes

The default CMake invocation keeps the frozen C++ reference route:

```sh
python3 tests/native/slice_source/run_slice_source.py \
  --cli build/woozyc --helper build/native/lang_native_helper \
  --clang .../bin/clang --lld .../bin/ld.lld \
  --runtime build/native/liblang_native_runtime.a \
  --root . --fresh --out fresh-results
```

Reference mode uses `--cli` for the 35 public builds, `--helper` for the 10
private Source IR emissions, and the original two observers for the 10
observer links/runs.  The original 107 top-level commands are retained.

Candidate mode is selected by `--wz-native-helper` (alias `--driver`) and uses
the real Wz observation entry instead of the C++ helper for the 10 private
Source IR runs:

```sh
python3 tests/native/slice_source/run_slice_source.py \
  --role candidate \
  --cli .../stage1/woozyc \
  --wz-native-helper .../wz-entry/build/program \
  --build-report .../wz-entry/build-report.json \
  --component-source-map .../wz-entry/component-source-map.json \
  --candidate-ready .../stage1-readiness.json \
  --source-root .../owned-compiler/selfhost/src \
  --stdlib-source .../owned-compiler/stdlib \
  --clang .../llvm-22.1.8/bin/clang \
  --lld .../llvm-22.1.8/bin/ld.lld \
  --runtime .../resources/runtime/liblang_native_runtime.a \
  --toolchains .../toolchains \
  --strace .../strace-7.2/usr/bin/strace \
  --seed .../build-new/woozyc \
  --root . --fresh --out fresh-results
```

Candidate mode runs the Wz driver (`load -> resolve -> check -> lower ->
snapshot -> dump -> emit`) and then the fixed LLVM `verify -> opt -> verify ->
llc` steps.  It counts **147** top-level commands: the original 107 logic plus
40 new fixed LLVM commands (10 verify-before, 10 opt, 10 verify-after, 10 llc).
The Wz entry build is a separate one-command build with a 1800s budget.

## Wz entry builder

`build_nativeobs_ed1.py` stages the unchanged `selfhost/tests/nativeobs/main.wz`
and `lang.toml` with the 115 production Wz files from the frozen ed1 Source.
It builds the component once with the frozen C++ seed, records raw process
fields and resource before/after maps, and writes a sealed `build-report.json`.
The default is **O2** (`--opt` selects 0 or 2).  The accepted final driver is
the O2 binary with SHA-256
`ac17da47784efef72370447362acfab5fcef51d68d6804d443d52c60bcb0e807`;
the earlier O0 driver `430ae2f6...` is a historical prefix only, not the final
result.  `--skip-build` validates that report and the sealed component without
rebuilding.  The builder pins the 116-file ed1 Source map and the fixed
37-file stdlib; it does not change the historical nativeobs builder.  After the
build process returns, the known process record is persisted before resource
hashing, artifact reads, or strict build-JSON checks.  A later failure
therefore leaves `process` populated with exact exit, the three cleanup
booleans, raw bytes/SHA, argv, cwd, and actual timeout instead of
`status:running` with `process:null`.

## Assertions retained

- 5 positives (`core`, `owned`, `cleanup`, `throws-return`, `after-drop`) at
  O0/O2: public build exit 0, program exit 0 with empty stdout/stderr,
  readelf no `NEEDED`, all four IR files (`before.ll`, `after.ll`,
  `module.nir`, object) present with SHA-256, no `interp` marker, and the
  original slice call tokens (including core `lang_slice_sub_v1` /
  `lang_vec_slice_v1` and owned `lang_vec_slice_v1` / `lang_vec_set_v1`).
- Allocation/Vec observer: the public `program.o` is linked with the original
  `tests/native/vec_observer.cpp`, `alloc_observer.cpp`, and matching runtime.
  The original seven-event counts, ALLOC==FREE, and zero live heap/Vec are
  asserted.
- 11 Panic fixtures at O0/O2: normal exit 101, original cause
  (`active-iteration`, `vec-index`, `vec-slice`), empty stdout.
- 3 Native type rejections: normal native int exit 1, strict JSON
  `status:error`, original `E_TYPE_MISMATCH`, no program.  Strict parsing
  rejects `NaN`, `Infinity`, `-Infinity`, and exponent overflow such as
  `1e1000` at any nesting depth; normal finite zero/negative/positive
  decimals remain valid.
- `fixtures/python-slice` is the 20th manifest but is not selected by the
  original Native route; it is not run as Native here.
- The 15-mutation Slice forged CTest is a separate test and is not counted or
  claimed as migrated.

## Process records

Every actual process goes through `selfhost/corpus._run_process_impl` with a
finite timeout, raw stdout/stderr bytes and SHA-256, exact integer exit,
exact boolean `timed_out` / `cleanup_complete` / `abandoned_pipes`, elapsed
time, and bounded process-group cleanup.  Future records include the actual
`timeout_s`; historical records that lack the field are left unchanged rather
than backfilled or rerun.  Candidate commands use exact exec allowlists and
strace 7.2; a missing trace, missing IR file, weak observer, timeout, crash, or
OSError/exec failure is not accepted as an expected rejection or pass.  Records
are written to `commands.json` and `summary.json` before the next assertion,
using a same-directory temporary file and `os.replace`; a write failure cannot
truncate the previous list.

`--out` handling has two modes:

- Explicit fresh mode (`--fresh`, `--role reference`, or candidate mode)
  refuses a non-empty directory rather than overwriting it.
- A bare legacy CMake invocation (no `--fresh` and no `--role`, as used by
  `tests/native/CMakeLists.txt`) remains repeatable.  If the fixed `--out`
  directory is non-empty, the old files are kept and one uniquely named
  `run-*` child directory is created for the new run.  Failed or partial
  prefixes remain in their own directories.

The final accepted session contained **605 main commands** (reference97 with
10 observer-link failures, reference107, candidate147 with the O0 component,
final reference107, final candidate147) plus **2 component builds**, for
**607 non-pure commands**.  The 9 pure controls are separate.  The final
107/147 pair, or its 254-command sum, is not the whole session.

## Controls

```sh
python3 -B tests/native/slice_source/test_controls.py
```

The pure controls install spawn/exec/socket guards before importing the
runner and builder, and do not launch any external process.  They cover the
original exact int exit/flags, rejection signal/timeout/launch-failure, strict
finite JSON (including exponent overflow), trace loss, missing four IR files,
weak observer evidence, the explicit fresh-output guard, legacy repeat
subdirectories, invalid timeouts, and actual mocked builder main failures for
malformed/error/non-finite/fake success plus preservation of a known process
after a later resource error.

Root review: new and sealed builds use the same finite JSON success check and require empty diagnostics. Sixteen pure tests cover non-empty error diagnostics and malformed saved stdout with missing or true build_json_ok; historical build reports are not rewritten.
