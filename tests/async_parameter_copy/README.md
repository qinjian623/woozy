# async parameter copy Native tests

These are real-source Native checks for ordinary async owning parameters. The
frame takes a private owning copy during Task creation, before the initial
suspension, so the caller's block may end before the Task is started or resumed.

## Cases

- `param-vec-late-start`: `make` owns a `Vec[i32]`, creates the Task and returns
  it. The caller starts the Task only after `make`'s block has ended.
- `param-string-await`: a dynamic `string` built by `format_i64` and `+` survives
  the caller block and the `await` suspension.
- `param-struct-enum-await`: owning `struct` and `enum` parameters copy their
  `Vec` fields recursively.
- `param-fn-capture-await`: a capturing function parameter retains its closure
  environment.
- `param-weak-await`: a `Weak[Vec[i32]]` parameter retains its control block
  while a strong `Vec` parameter is also frame-owned.
- `param-dyn-await`: a `dyn Trait` parameter clones its boxed concrete value.
- `param-throw`: the typed-error exit drops the frame-owned parameter once and
  keeps the promise-owned error payload separate.
- `param-cancel`: after suspension, cancel runs the source defer and then drops
  both ordinary parameters exactly once.
- `param-cancel-queued`: cancel a queued Task before its body runs; the
  first-resume cancel entry drops the copied parameter through the common final.
- `param-unstarted`: destroying a never-started Task releases the private
  parameter copy through the initial-suspend destroy edge.
- `reject-resource-copy`: a C resource with no `retain_symbol` has no ordinary
  copy operation and is rejected with `E_CAPABILITY_UNAVAILABLE` instead of
  borrowing the resource across suspend.

Every case is built by the published Native CLI at O0 and O2, executed, then
emitted as LLVM and linked with `tests/native/language_gc_source/observe_gc.cpp`.
The observer must report zero live heap allocations, balanced ALLOC/FREE and
VEC:1/VEC:5, and no duplicate or unknown free. Missing frees fail the case.

## Modes

### Legacy private mode (unchanged)

Without `--public-object` the runner keeps its original two-compile flow: the
public `build` produces `program`, and the reference `lang_native_helper`
(`--helper`) recompiles the Source to `before.ll`, which is linked with the
observer. This mode still runs all eleven cases, including the private
`param-string-await` before.ll assertion for `lang_text_concat_v1`. That
assertion is still pending; the runner as a whole remains
`adaptation_required`.

### Public-object mode (eleven cases with an explicit IR driver)

`--public-object` builds each selected Source only with the tested role's own
public `build`, runs that `program`, and then links the unchanged observer from
that build's published `program.o` plus a role-matching
`liblang_native_runtime.a`. No C++ frontend or old helper compiles the
candidate Source. The nine non-string positives and the resource rejection run
without extra options. `param-string-await` keeps its original private
`O0/O2.before.ll` `lang_text_concat_v1` assertion and is accepted only when an
explicit test-only Native observation driver is supplied:

```bash
python3 tests/async_parameter_copy/run_parameter_copy.py \
  --public-object --role candidate \
  --cli "$ROLE/woozyc" \
  --runtime "$ROLE/runtime/liblang_native_runtime.a" \
  --toolchain "$LLVM/bin" --root "$REPO" --out "$FRESH_OUT" \
  [--libxml2-lib "$LIBXML2/usr/lib"] \
  [--strace "$STRACE"] [--trace-allow "$REFERENCE/native/lang_native_helper"] \
  [--resource-root "$ROLE"] [--proposal-out "$PROPOSAL"] \
  [--ir-driver "$NATIVEOBS/program" \
   --ir-driver-sha256 "$NATIVEOBS_SHA" --ir-stdlib "$ROLE/stdlib"] \
  [--snapshot-root "source116=$SOURCE116"] \
  [--snapshot-root "component117=$COMPONENT"] \
  [--extra-input "seed=$SEED"] \
  [--only param-weak-await] [--opt 0]
```

The nine non-string positives plus `reject-resource-copy` still run with no IR
driver. Selecting `param-string-await` without `--ir-driver` is rejected; an
`--ir-driver` without that case is also rejected. Supplying `--helper` remains
rejected. `--role reference|candidate` is required. The reference role may
pass `--trace-allow` for its own compiled-in `lang_native_helper`; a candidate
Source trace allows only this role's CLI and the fixed LLVM tools.

For every positive case and opt the mode runs a public Source build, the
original `program` execution, a link from this build's `program.o`, the
observer execution, and `readelf -l -d` on both static ELFs. The observer
stdout must equal the program stdout, and its stderr must be fully accepted by
the unchanged `parse_observer`, with zero live heap bytes/counts, balanced
ALLOC/FREE and VEC:1/VEC:5, and the original per-case allocation minima. The
reject case must exit non-zero with `E_CAPABILITY_UNAVAILABLE` and a message
containing `retain_symbol` or `external C resource`, and must publish neither
`program` nor `program.o`; a missing-library link error is not accepted.

For `param-string-await`, the explicit IR driver writes a real
`O0/O2.before.ll` and Native CFG from the real Source manifest and matching
stdlib. The runner checks the original literal `lang_text_concat_v1` marker in
each before.ll and runs the fixed `opt -passes=verify -disable-output`; it
records the driver argv, process flags, trace, metadata and before/native
hashes. The before.ll is evidence only: `program` and `program.o` still come
from this role's public build, and the observer links that object. The driver
is a test-only Nativeobs entry built from the same 115 `wzcc` production
modules as the candidate CLI plus its own main and manifest; it is not a
product CLI, does not add an `--emit` API, and its ELF is different from the
CLI. The runner never calls a C++ frontend/helper to generate candidate IR.

Commands run through `selfhost/corpus.py::_run_process_impl` with the original
180s per-command limit (`min(timeout, 60)` for readelf). Each command records
argv, cwd, the real integer exit, `timed_out`, `cleanup_complete`,
`abandoned_pipes`, `timeout_s`, and raw stdout/stderr files. Timeouts, signals,
incomplete cleanup, and abandoned pipes fail the case. Every Source build, IR
emit, observer link, program execution and observer execution runs under
exec-only strace (`execve,execveat`) with a separate exact whitelist: this
role's CLI plus fixed LLVM tools for a Source build, only the sealed Nativeobs
driver for IR, only fixed clang/ld.lld for the observer link, and only the
published program or observer ELF for execution. Evidence directories must be
new; the runner refuses to overwrite a non-empty output directory and keeps
failed evidence. A summary, the command log, and an immediately written
`case.json` are saved even when a case raises. Input, CLI, runtime, header,
observer, and tool hashes are captured before and after the run. With
`--resource-root`, the same stdlib tree is hashed before the first build and
again when the run finishes, including case failures.
The stdlib before value remains `resources.stdlib_tree_sha256`; the final
comparison records `stdlib_tree_sha256_after` and `stdlib_tree_unchanged`.
A changed or missing tree fails the run, including the summary's failure status.
`--snapshot-root LABEL=PATH` writes full per-file SHA maps before and after the
run and fails if the tree changes. `--extra-input LABEL=PATH` does the same for
one file, such as the seed, helper, hosted runtime, observation driver, build
report or source maps. The runner does not use a heartbeat as an input.

`--self-test` runs short controls against the same validators and process
runner: missing `program.o`, wrong program/observer stdout, program stderr,
observer panic or junk lines, missing or duplicate FREE, below-minimum
allocation, wrong resource diagnostic, residual published files, empty and
string selections, and a timed-out grandchild that holds the stderr pipe.
The pure Python stdlib control accepts an unchanged tree and rejects one file
change against the saved before value. The old saved self-test JSON contains
17 checks; the SH12g stdlib amendment made 19. SH12o adds controls for a
string selection without an IR driver, an IR driver without the string case,
an exact driver SHA mismatch, a C++ helper named as the IR driver, a missing
or empty before.ll, a before.ll without `lang_text_concat_v1`, failed IR
metadata, and a real before.ll path/flag/hash round trip through the same
`public_ir_emit` validator. It does not delete or weaken the old controls.

This mode covers all eleven cases with this role's published object when the
IR driver is supplied. The generic corpus schema does not express the
observer, static-ELF or before.ll assertions recorded by this private runner;
the proposal records them as runner-only evidence and leaves all rows
disabled.

## SH12g records

The runner now compares the stdlib after value. Original SH12g records remain
unchanged.

The two final SH12g r2 rounds used C++ reference `565` and the old Wz compiler
`2c11`, with 20 public builds per role. These records do not validate the newer
`9d11` compiler. The entire recorded history contains at least 73 Source build
attempts, not just those final 40:

```text
20 reference r1 + 20 reference r2 + 11 candidate partial
+ 20 candidate r2 + 2 manual reproductions = 73
```

The reference r1 failure was the readelf/readobj argv0 mistake. The candidate
partial directory keeps 60 completed command records (five cases at O0/O2)
and the eleventh build's trace; that interrupted build has no process flags.
The owner later killed its known CLI and strace processes explicitly. The two
manual builds (`/tmp/t2`, `/tmp/t3`) also lack this runner's strict process
flags. Those missing fields stay unknown; no failed or partial directory was
deleted, overwritten, or counted as a passing r2 result.

The old runner saved the stdlib before value ahead of its first build, but
saved no stdlib after value. The independent review at
`checks/SH12g-async-parameter-copy-independent-review-20261114.md` separately
read the same trees at 03:56:13/03:56:14 UTC and found both unchanged. That is
a dated supplemental after check, not a reconstructed field in the old runs.
This amendment makes future runs record and compare their own after value.

The old two-compiler records remain disabled and historical. The eleventh
case, `param-string-await`, now has an implementation path through the
explicit test-only Nativeobs IR driver, but a row is only evidence for the
exact CLI, driver, Source, stdlib and fixture hashes recorded in its SH12o
run. A partial or failed run is not a whole-run pass.

## SH12o records

The new evidence directory is
`checks/SH12o-async-parameter-complete-eleven-new9d11-20261114`. The first
bounded probe used the sealed O0 Nativeobs driver `26409123…` on the real
`param-string-await` manifest and matching 37-file stdlib; it exited 0 in
152.86s, produced a real before.ll containing `lang_text_concat_v1`, and its
exec trace contained only the driver. The driver therefore did not need an O2
rebuild.

Both roles then ran all eleven cases at O0/O2 with the same explicit driver:
C++ reference `565da3f6…` and pure Wz candidate `9d11f015…`. Each role had 22
public Source builds, 20 program executions, 20 observer links, 20 observer
executions, 40 readelf checks, 2 real IR emits and 2 LLVM verifies. Both roles
passed 22/22. The candidate Source trace contained only the 9d11 CLI and fixed
LLVM tools; no C++ helper/frontend ran for candidate Source. Nativeobs output
was used only for the string before.ll/Native CFG and LLVM verify; program,
program.o and the observer link still came from each role's public build.

The candidate's slowest public Source build was 157.26s, below the unchanged
180s limit. Source116 (`727b72dd…`), component117 (`f6d6b488…`), the 9d11
stdlib37 (`473aa54b…`), the reference stdlib, all fixtures/manifests/bindings,
seed, helper, hosted runtime, driver, observer and tools were hashed before
and after and were unchanged. The old `2c11` ten-case records remain
historical and were not combined with this run. The two proposals under the
new directory update only the existing eleven rows, stay disabled, and list
the IR/observer/static-ELF evidence the corpus schema cannot express.

## Running (legacy private mode)

```bash
python3 tests/async_parameter_copy/run_parameter_copy.py \
  --cli "$BUILD/woozyc" \
  --helper "$BUILD/native/lang_native_helper" \
  --runtime "$BUILD/native/liblang_native_runtime.a" \
  --toolchain "$LLVM/bin" \
  --root "$REPO" \
  --out "$OUT" \
  [--libxml2-lib "$LIBXML2/usr/lib"] [--only param-weak-await] [--opt 0]
```

The CMake fragment registers `native.async_parameter_copy` without modifying the
root CMake files. It needs only `LANG_TEST_PYTHON` plus the Native targets.

## Current baseline

On the ordinary-parameter-copy implementation, the legacy private mode still
runs all eleven cases at O0/O2. The public-object mode with an explicit
Nativeobs IR driver runs the same eleven cases from each role's published
object and records the original before.ll marker separately. A full run is
only complete when both the public-object result and the IR observation are
recorded; a timeout or missing before.ll stays `pending`/`failed`, not passed.
In the separate `native.async_memory` suite, 20 of 24 runs pass:
`async-param-lifetime` now passes at O0/O2, while `task-vec-cycle` and
`task-closure-cycle` remain failed at O0/O2 because Native Task/frame nodes are
not yet traced by GC. The closure cycle now really retains the function
environment; this suite does not mask that missing GC integration.
