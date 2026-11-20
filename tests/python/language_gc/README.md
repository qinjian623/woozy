# Python language ownership GC checks

This directory is the MEM02/Python low-level acceptance slice.  It does not
define source closure syntax or Sema/HIR.  The generated program uses the real
helper/API path and the bundled `runtime/python/langrt.py`.

Run the private runtime checks:

```sh
python3.12 tests/python/language_gc/runtime_gc_test.py
```

## Legacy helper mode (default, unchanged CMake invocation)

```sh
python3.12 tests/python/language_gc/run_language_gc.py \
  --helper BUILD/tests/python/lang_python_emit \
  --root . --out /tmp/language-gc-results \
  --python /path/to/cpython-3.12.15/bin/python3.12
```

Each case owns a separate generated project and observer process with a finite
60 s timeout.  The observer imports the generated `program.py`/`langrt.py`,
wraps `Vec.create` to retain Weak handles, calls the generated source
functions, and checks `_gc_stats()` plus real Weak upgrade results.  It never
calls Python `gc.collect()` or treats CPython collection as language ARC
cleanup.

The same `--out` may be reused.  Every invocation writes a new
`out/runs/<timestamp>-<pid>/` directory, so previous helper/observer records
and logs are kept instead of overwritten.

Both modes check the original source and observer hashes before running.
Legacy mode checks the original CMake registration for this test and records
the current CMake file hash. Paired mode requires the complete historical
CMake file to match its frozen hash.

## Paired public-API mode

```sh
python3.12 tests/python/language_gc/run_language_gc.py \
  --cli reference-cpp-woozyc --helper reference-lang_python_emit \
  --candidate-cli candidate-stage1/woozyc \
  --api-driver checks/SH14-.../run/driver/root/pythonobs \
  --root . --out /fresh/evidence \
  --python /path/to/cpython-3.12.15/bin/python3.12 \
  --strace /path/to/strace-7.2 --toolchain-root /path/to/toolchains
```

Paired mode runs the same eight cases in both groups and requires exactly
`8 x 3 x 2 = 48` main commands: one real CLI build, one real API/helper emit,
and one unchanged observer per role/case.  Each role/case publishes two
emit directories (`cli` and `api`) with two files each (`program.py` and
`langrt.py`), so the runner records `8 cases x 2 roles x 2 emit directories x
2 files = 64` expected generated files.  The observer always imports the CLI
output; the API emit is used for metadata only after `program.py` and
`langrt.py` are byte-identical between the CLI and API directories in the same
group.

For the reference group the API metadata is the real C++ helper
`metadata.json`.  For the candidate group the prebuilt Wz `pythonobs` driver
publishes real `symbols` from the verified HIR snapshot and
`pythonemit.Output.symbols`; the runner selects exactly the eight required
`source_name` rows and writes the observer's original
`functions[].source_name/generated` projection.  Missing, duplicate or
ambiguous names are failures.  No `_f`/`_s` name is guessed, and no C++ helper
is called to produce candidate bytes.

The candidate `pythonobs` binary is the historical SH14 driver built from old
Source tree `727b...`, not from the current Source.  Its `driver-inputs.json`,
old Source files, entry hash and build record are checked and labelled
separately.  Passing this runner proves the current CLI executed the exact
bytes and the real symbols mapping; it does not claim that the current
Source's PythonEmitter API was rebuilt or independently proved.

The reference C++ helper's compiled Source history is also unknown, so its
`source_metadata.current_source_same_api_proof` is `false` with status
`unknown`, not `true`.  The candidate status is `false` and names old Source
`727b...`.  In both roles the identical CLI/API `program.py` and `langrt.py`
hashes are retained as byte-equality evidence: they prove the actual alias
selection for this run, not compiler Source or PythonEmitter API identity.

A paired `--out` must be new or empty.  Each command's argv/cwd, actual
`--timeout` (default 60 s), elapsed time, native `int` exit, strict
`timed_out`/`cleanup_complete`/`abandoned_pipes` booleans, raw stdout/stderr,
stream SHAs and exec trace are written immediately to `commands.jsonl` and
`logs/`/`traces/`.  Compiler traces are audited against exactly the CLI, helper
or API driver used by that step; observer/Python traces are audited against
exactly the fixed Python 3.12.15 executable.  If the exec whitelist rejects a
trace, the command that actually ran is still persisted in `commands.jsonl`
with its complete record and a `trace_audit_error` before the case fails.
Record rewrites finish in a same-directory pending file before replacing
`commands.jsonl`; a write failure preserves the previous complete records.  A
real failure or timeout stays `failed`/partial; the runner does not retry with
a longer budget.

`runner_controls.py` is the portable preflight (no compiler):

```sh
python3.12 tests/python/language_gc/runner_controls.py
```

It checks strict process flags and signal exits, finite positive timeouts,
non-finite JSON, missing/duplicate published symbols, no source-name guessing,
CLI/API byte mismatch, fresh-output refusal, fake/weak observer documents, the
64-file paired count, false/unknown reference and candidate API proof flags,
and the actual `run_command` trace-failure path.  The trace control replaces
only the bounded process helper with an in-memory fixture; it verifies that the
complete formal record, native exit/flags and raw stream hashes survive the
audit rejection.  All external launch entry points are monkeypatched to fail
closed and the control reports zero launch attempts.

Cases cover a struct/`Node` Vec self-cycle, two linked cycles, duplicate strong
edges, a live root plus an unreachable cycle pointing to a live child, nested
owned values, automatic loop-header collection (including an always-continue
loop), and typed-error/defer cleanup.  Runtime checks also cover captured
FunctionValue Weak upgrade, stale wrapper retirement, collector allocation
failure state restoration, and foreign-finalizer reentry.  Source capture
syntax, Native, threads, fork and the full MEM02 scope remain out of this
slice.
