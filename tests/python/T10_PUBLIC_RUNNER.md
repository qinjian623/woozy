# T10 public-CLI candidate runner

`tests/python/run_t10_bindings.py` keeps its original CMake/helper usage and
adds one optional paired mode.  The complete target is still the fixed 20 named
cases in their original registration order; the three-case
`python.foreign_ownership` wrapper and the separate `python.t10_cli` task are
not the complete target and are not claimed as such here.

## Modes

*Legacy (default).*  The existing CMake argv is unchanged: `--helper`,
`--woozyc`, `--root`, `--out`, `--python`, `--binding`, `--example-binding`,
`--reference`, `--observe`, `--original-observer`, `--reference-observer`,
`--real-observer`, `--only`.  Every emitted `program.py`/`langrt.py`/
`verified.hir`/`metadata.json` still comes from the C++ `lang_python_emit`
helper.  `--only` behaves exactly as before.

*Paired candidate.*  Supplying `--candidate-cli` together with `--api-driver`
and `--strace` switches emission to the real public Wz CLI:

```sh
tests/python/run_t10_bindings.py \
  --helper /path/to/lang_python_emit \        # reference emitter only
  --candidate-cli /path/to/woozyc \           # candidate build --backend python
  --api-driver /path/to/pythonobs \           # real PythonEmitter API observer
  --strace /path/to/strace \
  --compile-timeout 180 \
  ... the same root/out/python/binding/reference/observer arguments ...
```

For every positive case the candidate path is:

1. `candidate-cli build --project MANIFEST --backend python --std-binding
   BUNDLE --out-dir OUT --format json` produces the observed
   `program.py`/`langrt.py`.
2. The candidate CLI check runs first: a positive case needs a clean exit 0;
   a source rejection must have exit 1, retained diagnostics and neither
   `program.py` nor `langrt.py` in the candidate output.
3. The C++ helper then runs as the reference emitter through the same bounded,
   traced transport.  Its clean strict fields are required, but its output is
   never used as candidate bytes, metadata or HIR.
4. `pythonobs --project MANIFEST --out-dir API_OUT --std-binding BUNDLE
   --format json --emit-hir` calls the real
   `load`/`resolve`/`check`/`hirverify.snapshot`/`pythonemit.emit` path.
5. The candidate and API generated files must be byte-for-byte identical before
   anything else happens.  Only then are `metadata.json` (projected from the
   real published `entry`/`symbols`) and `verified.hir` (the real
   `hirdump.dump_module` text) written next to the candidate bytes for the
   unmodified observers.

The runner never guesses `_f`/`_s` names: `source_name`, `generated` and
`entry` come from the API JSON, and the published entry name is only validated
against the unchanged observer convention.  Missing/duplicate/ambiguous
symbols or a mismatched entry fail before the observer runs.

## Real HIR requirement

The original runner requires a real `verified.hir` and its SHA.  The product CLI
does not publish that file, so the test-only `pythonobs` gained an additive
`--emit-hir` flag: it returns the exact `hirdump.dump_module(snapshot)` text in
its existing JSON status line, and the runner writes that text as
`verified.hir` with a temporary-then-replace publication.  Without the flag the
driver's stdout and two-file `langrt.py`/`program.py` publication are unchanged;
no product CLI flag was added.  A retained 55d `pythonobs` binary has no such
interface and cannot be used as the real-HIR candidate; that needs a separately
authorized build of the current component.

## Process and rejection rules

* Candidate CLI, API driver, reference helper and observer/exec runs are traced
  with their own exact exec whitelists.  Raw stdout/stderr, exit, timeout/
  cleanup flags and the trace are persisted before any check can fail; a failed
  `execveat` keeps the raw record and fails that case.
* Process success requires a real `int` exit and real `bool`
  `timed_out`/`cleanup_complete`/`abandoned_pipes`; a timeout, incomplete
  cleanup or abandoned pipe fails even when the exit code is 0.
* `--compile-timeout` keeps the original per-command 180 second budget and
  rejects any value above 180; the original CMake 600/300/1800 and original-04
  300 second budgets are unchanged.
* A failed CLI aborts immediately and never falls back to the reference helper.
* A rejected Source keeps the original diagnostic output and exact codes and
  publishes neither `program.py` nor `langrt.py`, in both candidate and
  reference directories.  The missing-host case remains a real runtime
  environment failure.
* The venv `--python` argv path is kept absolute and is not resolved, so the
  NumPy/PyTorch site-packages are not lost.
* No CMake budget (600/300/1800) was increased and no new product CLI option
  was added.

## Optional observer `uname -p` probe

CPython's `platform` module (used by the real NumPy/PyTorch observers for their
environment record) may probe `uname -p` through `PATH`.  On the observed host
that produced one failed lookup of an earlier `PATH` entry with `ENOENT` and
then one real `/usr/bin/uname` run with argv `["uname", "-p"]`, which the old
exact whitelist rejected as an unexpected subprocess.  The observer policy now
accepts exactly that optional child and nothing else:

* The only accepted tool is the fixed Linux `/usr/bin/uname` (its resolved
  canonical alias is accepted because paths are compared after resolution).
  A temporary same-name script, any other existing executable and any other
  command are rejected; existence or `X_OK` alone is never trusted.
* Before the paired `Runner` starts any real command, it reads `/usr/bin/uname`'s
  logical/canonical path and content SHA and freezes them together with the
  observer `PATH`.  Every observer audit re-reads and compares that identity
  and records it as `trace_audit.uname_identity` (`path`, `canonical`,
  `sha256_before`, `sha256_at_audit`, `path_entries`, `unchanged`,
  `probe_count`).  A post-freeze content change, a missing tool or a canonical
  path change fails the audit.  No OS binary SHA is hardcoded in the repo, and
  legacy mode never reads this new path.
* Only the observer/exec whitelist (`observe`, `real-observe`,
  `original-observer`, `reference-observer`) enables the policy; candidate CLI,
  API driver, reference helper, formal CLI and the observed generated program
  keep their own exact whitelist and still reject `uname` and every helper.
* The traced Python tool must have at least one complete successful `execve`
  record with a real pid.  A failed or malformed required record does not count
  as a successful appearance.  The probe itself stays optional, so a
  Python-only observer trace is accepted and no observation must run `uname`.
* Every retained `uname` line must use argv `["uname", "-p"]`, every failed
  lookup must be the next explicit entry of the frozen observer `PATH` with a
  real `ENOENT` errno token (not a text substring), and the same child pid must
  then execute the frozen tool.  Dangling lookups, unknown/out-of-order lookups,
  a reused observer pid, extra or wrong argv, relative paths and a path that
  both failed and succeeded are rejected.
* `execveat` is rejected outright by this observer policy because its argv and
  target cannot be verified here; no general strace framework is added and the
  global `corpus.parse_trace`/`audit_trace` stay unchanged.
* The raw trace bytes are never edited; the accepted probe is retained under
  `trace_audit.optional_uname` with its pid, argv, failed lookups, executable
  and exit result, after the raw stream and process fields were saved.

## Pure controls

`tests/python/t10_runner_controls.py` is the only runnable delivery here.  It
starts no compiler, driver, observer, program, NumPy or PyTorch: module setup
globally blocks `subprocess`, `os` exec/spawn/fork, `socket` and the lowest
bounded transport, and each test only replaces that lowest transport.  The real
`Runner.command`/`traced`/`emit_paired`/`observe`/`real_observe` branches are
therefore exercised, never replaced by a fake command.  The controls check the
20-name plan, the exact three-case subset, the eleven nested rejection projects,
legacy/paired `main` dispatch with every launch blocked, invalid budget/role/
non-empty strict output rejection before launch, strict process flags, dirty
observer flag rejection, the optional observer `uname -p` probe (Python-only
and the real frozen `/usr/bin/uname` with a failed `PATH` lookup accepted;
missing or failed/malformed Python, arbitrary same-name executables, execveat,
errno text spoofing, unknown/out-of-order or dangling lookups, wrong/extra
argv, relative paths, reused observer pid, other tools and a mixed C++ helper
rejected; a changed or missing frozen tool identity rejected; compile/program
whitelists still reject `uname` and the helper), paired reference-helper
bounded/trace records, raw retention on a failed `execveat` trace, separate
compile/observer whitelists, no helper fallback, the CLI/API byte + symbol + HIR metadata gate, both missing
files on candidate rejection (including a langrt-only residue), unchanged old
directory contents/mode, and a compile budget capped at 180. The existing tree
is captured with the corpus output reader before a rejected candidate command
and compared afterwards, including the output directory itself. Six mutation
controls cover changed bytes, permissions, deleted/added files and directories.

No real Source, component build, or T10 execution is performed by this coding
delivery; see `.work/SH16-t10-public-candidate-correction.md`.
