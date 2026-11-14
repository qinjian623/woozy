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

### Public-object mode (ten migrated cases)

`--public-object` builds each selected Source only with the tested role's own
public `build`, runs that `program`, and then links the unchanged observer from
that build's published `program.o` plus a role-matching
`liblang_native_runtime.a`. No C++ frontend or old helper compiles the
candidate Source:

```bash
python3 tests/async_parameter_copy/run_parameter_copy.py \
  --public-object --role candidate \
  --cli "$ROLE/woozyc" \
  --runtime "$ROLE/runtime/liblang_native_runtime.a" \
  --toolchain "$LLVM/bin" --root "$REPO" --out "$FRESH_OUT" \
  [--libxml2-lib "$LIBXML2/usr/lib"] \
  [--strace "$STRACE"] [--trace-allow "$REFERENCE/native/lang_native_helper"] \
  [--resource-root "$ROLE"] [--proposal-out "$PROPOSAL"] \
  [--only param-weak-await] [--opt 0]
```

This mode selects the nine non-string positive cases plus
`reject-resource-copy`. It refuses `--helper`, refuses an empty selection, and
refuses `param-string-await` instead of silently dropping its private LLVM
text assertion. `--role reference|candidate` is required. The reference role
may pass `--trace-allow` for its own compiled-in `lang_native_helper`; a
candidate trace allows only this role's CLI and the fixed LLVM tools.

For every positive case and opt the mode runs a public Source build, the
original `program` execution, a link from this build's `program.o`, the
observer execution, and `readelf -l -d` on both static ELFs. The observer
stdout must equal the program stdout, and its stderr must be fully accepted by
the unchanged `parse_observer`, with zero live heap bytes/counts, balanced
ALLOC/FREE and VEC:1/VEC:5, and the original per-case allocation minima. The
reject case must exit non-zero with `E_CAPABILITY_UNAVAILABLE` and a message
containing `retain_symbol` or `external C resource`, and must publish neither
`program` nor `program.o`; a missing-library link error is not accepted.

Commands run through `selfhost/corpus.py::_run_process_impl` with the original
180s per-command limit (`min(timeout, 60)` for readelf). Each command records
argv, cwd, exit, `timed_out`, `cleanup_complete`, `abandoned_pipes`, and raw
stdout/stderr files. Timeouts, signals, incomplete cleanup, and abandoned pipes
fail the case. Evidence directories must be new; the runner refuses to overwrite
a non-empty output directory and keeps failed evidence. A summary and the
command log are written even when a case raises. Input, CLI, runtime, header,
observer, and tool hashes are captured before and after the run. With
`--resource-root`, the same stdlib tree is hashed before the first build and
again when the run finishes, including case failures.
The stdlib before value remains `resources.stdlib_tree_sha256`; the final
comparison records `stdlib_tree_sha256_after` and `stdlib_tree_unchanged`.
A changed or missing tree fails the run, including the summary's failure status.

`--self-test` runs short controls against the same validators and process
runner: missing `program.o`, wrong program/observer stdout, program stderr,
observer panic or junk lines, missing or duplicate FREE, below-minimum
allocation, wrong resource diagnostic, residual published files, empty and
string selections, and a timed-out grandchild that holds the stderr pipe.
The pure Python stdlib control accepts an unchanged tree and rejects one file
change against the saved before value. The old saved self-test JSON contains
17 checks; these two outcomes bring this amendment's total to 19.

This mode covers exactly the ten migrated cases with this role's published
object. It does not complete the parameter-copy runner, does not migrate
`param-string-await`, and the generic corpus schema does not express the
observer or static-ELF assertions recorded by this private runner.

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

The ten migrated proposals remain disabled. The eleventh case,
`param-string-await`, still requires its original `before.ll`
`lang_text_concat_v1` observation. It remains pending; neither this amendment
nor the ten-case subset completes the whole runner.

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

On the ordinary-parameter-copy implementation, all 22 legacy O0/O2 runs pass.
The public-object mode is scoped to the ten migrated cases and records separate
evidence; it is not a claim that the whole runner is migrated, because
`param-string-await` still needs its private before.ll assertion. In the
separate `native.async_memory` suite, 20 of 24 runs pass: `async-param-lifetime`
now passes at O0/O2, while `task-vec-cycle` and `task-closure-cycle` remain
failed at O0/O2 because Native Task/frame nodes are not yet traced by GC. The
closure cycle now really retains the function environment; this suite does not
mask that missing GC integration.
