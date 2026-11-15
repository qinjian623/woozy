# `run_cases.py` public-CLI/API paired mode

This note documents the migration in `tests/python/run_cases.py`.  The catalog
(`selfhost/corpus.json`) is intentionally unchanged; the Root owner refreshes it
after this branch is reviewed.

## Modes

### Legacy (default, unchanged)

The existing CMake argv is unchanged: `--helper`, `--root`, `--out`,
`--python`.  Every generated `program.py`/`langrt.py`/`verified.hir`/
`metadata.json` still comes from the C++ `lang_python_emit` helper, and the
same 15 published cases, `EXTRA_SOURCES`, negative controls, diagnostics,
Panic, host-injection, syntax, `inout`, float/integer-width and 32-bit checks
run in the same order.  The helper command remains a plain 30-second
`subprocess.run` path.

### Paired candidate

Supplying any of `--candidate-cli`, `--api-driver`, `--source-root`,
`--candidate-resources` or `--strace` selects paired mode.  Paired mode
requires `--candidate-cli`, `--api-driver` and `--strace`; `--source-root`
defaults to `ROOT/selfhost/src`, and `--candidate-resources` defaults to the
candidate CLI directory.  The candidate CLI actually reads `stdlib` and
`runtime` next to its resolved executable, so `--candidate-resources` must
expose the same required directories/files through the same resolved targets
as the CLI's bin directory.  A symlinked layout is accepted; a different
directory/content set is rejected before the first command.  `--toolchain-root`
only adds the existing LLVM/libxml2 library paths to the paired environment.
`--out` must be empty in paired mode, so old evidence cannot be overwritten.

```sh
tests/python/run_cases.py \
  --helper /path/to/lang_python_emit \
  --candidate-cli /path/to/woozyc \
  --api-driver /path/to/pythonobs \
  --source-root /path/to/source116d602 \
  --candidate-resources /path/to/candidate-root \
  --strace /path/to/strace \
  --root /path/to/repo --out /path/to/fresh-out \
  --python /path/to/python3.12
```

For every accepted case:

1. Inputs are hashed before the first command: published cases/sources,
   `run_cases.py`, `selfhost/corpus.py`, the pythonobs source tree,
   `--source-root` `.wz` files, candidate stdlib/runtime files, tools and
   optional toolchain files.  The same snapshot is recomputed only after the
   suite finishes.
2. `candidate-cli check --project MANIFEST --backend python --format json`
   must be clean.
3. `candidate-cli build --project MANIFEST --backend python --out-dir
   generated/ID --format json` publishes the candidate `program.py` and
   `langrt.py`.  A missing stdout document, a missing file or a non-zero exit
   fails the run immediately.
4. A staged copy of the prebuilt `pythonobs` driver runs with the candidate
   `stdlib`/`runtime` resources and `--format json --emit-hir`.  It is the only
   source of `program.py`/`langrt.py` metadata and Verified HIR assistance.
5. Candidate and API `program.py`/`langrt.py` must be byte-identical.  Only
   then are the API `entry`, published `symbols` (`source_name`, `generated`)
   and real `verified_hir` text accepted.  `metadata.json` and `verified.hir`
   are written into the candidate tree; the four artifacts are hashed.
6. The C++ helper runs again into `reference/ID` as an independent reference
   emitter.  Its files are snapshotted separately and never supply candidate
   bytes, metadata or HIR.
7. The candidate program and existing observations run under only the explicit
   `--python` interpreter.  Candidate and reference output trees (including
   modes) are compared before and after execution.

Rejections (`E_CAPABILITY_UNAVAILABLE`, source `E_TYPE_MISMATCH` and the
Python/32 codegen rejection) require the same diagnostic code and source span
from the candidate `check`/`build` and the helper reference.  No `program.py`
or `langrt.py` may appear; an existing output tree must remain byte- and
mode-identical.  A rejected CLI never falls through to the API or helper.

## Evidence and process rules

Every paired command uses the shared bounded process-group transport with the
existing 30-second budget (unchanged, no new timeout option).  Raw stdout,
stderr, exit, `timed_out`, `cleanup_complete`, `abandoned_pipes` and trace path
are written under `--out/logs`, `--out/traces` and `--out/commands.jsonl`
before any typed assertion.  Paired reports use `sys.platform` plus direct
`os.uname()` fields, not `platform.platform()`; no `uname -p` lookup or uname
whitelist is added.  A preflight or run failure after `--out` is accepted still
writes the failure `results.json` from the already-created report; the failure
path does not call the report header a second time.  The exact exec whitelist
is:

* candidate CLI commands: only the candidate CLI;
* API emission: only the staged API driver;
* helper reference: only the helper;
* generated program and observations: only the explicit Python interpreter.

No `uname` exception is enabled for this group.  The runner never runs `git`
in paired mode, so `source_head` is reported as not-run rather than adding an
untraced external command.

## Metadata identity

`entry` and generated names come only from the real API document.  A host probe
uses the published `generated` name for `main`; `inout` and float probes use
published `source_name` rows.  There are no `startswith('_f')` checks, no
`_f<entry>` construction and no synthetic symbols, HIR or metadata.  Missing,
ambiguous, duplicate or wrongly typed fields fail before the program runs.
Anonymous functions may have an empty `source_name`; their symbol IDs and
generated names remain unique, and the entry must still be named `main`.

## Resource identity

Both the real CLI bin-adjacent resource root and the
`--candidate-resources`/API root are checked before the first command.  The
required `stdlib`, `runtime`, `runtime/python`, `runtime/python/unicode18.py`
and `runtime/python/langrt.py` entries must exist with the expected type, and
their resolved targets must agree.  Both roots are snapshotted (types, modes,
targets and file bytes) before commands and again after the suite; a required
file loss or a stdlib/runtime byte change fails the run.  Missing required
inputs are not skipped by the snapshot `add()` path.

## Limitations

* This delivery is code and pure-control only.  It does not run a real Source
  compiler, API driver, target Python, version probe or network operation.
* The prebuilt `pythonobs` must already implement the current additive
  `--emit-hir` interface.  An older 55d driver cannot be used as the real-HIR
  candidate without a separately authorized build.
* `--source-root` is provenance/snapshot input only here; this runner does not
  build the driver or a candidate compiler.
* There is no `--std-binding` path in this case group, no Native comparison and
  no attempt to complete the full public corpus.  The C++ helper remains
  required as the paired reference.
* `tests/python/python_cases_public_controls.py` is a pure harness control, not
  real conformance.  It replaces only the lowest transport, calls the real
  `main()` (with a one-case suite stub to isolate header/preflight/resource
  handling) and the real runner/executor/metadata methods, forbids external
  exec/subprocess/target-Python/socket calls and asserts the attempt list is
  empty.

Run the pure control once with:

```sh
python3 tests/python/python_cases_public_controls.py
```
