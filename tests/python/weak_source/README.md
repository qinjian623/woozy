# Python Source Weak acceptance

Task: `python-source-weak-operations-20261112`  
Contract: `compiler-dev/1`  
Runtime profile: Python/64, `compiler-dev/1-python-values/10`\
Authority: `docs/SEMANTICS.md` §7.1/§9 and the canonical target table in
`development/plans/REFERENCE_VALUES.md` §6.

## Scope

This directory is the test area for the Python Weak step. The runner does not
change shared CMake registration, HIR/Sema/IR, stdlib, fixtures, the observer,
or accepted baselines. It builds the same 18 real manifests through two
implementation groups:

- **reference** (legacy default, `--cli`/`--helper`): the C++ `woozyc build
  --backend python` driver and the C++ `lang_python_emit` helper;
- **candidate** (`--candidate-cli` + `--api-driver`): the public pure-Wz CLI and
  the accepted Wz Python API driver (`project.load` → Python selection →
  `resolve.resolve_project` → `sema.check_project(Backend)` →
  `pythonemit.emit`).

Every group compiles all 18 manifests twice (CLI and API/helper), so a full
two-role run performs **72 real Source compiles** and **22 real observer
runs** (11 positives per role). The generated `program.py` and `langrt.py`
must be byte-identical inside one group; cross-implementation bytes are
recorded but not used as a substitute for the same-group equality rule.

## Accepted Source cases

Positive cases (11):

- Root reference values: `weak-vec-lifecycle`, `weak-box-lifecycle`,
  `weak-empty-upgrade-none`, `weak-fn-immortal`, `weak-fn-captured-dead`.
  The capturing-function case is supported by the current Source/codegen and
  is a required positive here; the older README note that recorded it as
  unsupported was stale.
- `combo-fields-vec-enum`: Weak in a struct field, a Vec element, and an enum
  payload, copied through aliases; the last strong Vec owner leaves its block.
- `held-some-keeps-alive`: `Some` keeps the target alive across the original
  strong binding's scope, then dropping that `Some` makes `upgrade` return
  `None`.
- `generic-target`: generic `empty`/`downgrade` functions instantiated for
  `Vec[i32]` and `std/box.Box[i32]`.
- Old Source regressions: `old-function-values`,
  `old-generic-function-argument` (expected `main` return 5),
  `old-struct-copy-shares-vec`.

Only the original 8 Weak cases (5 Root + 3 local) require the emitted
`_rt.ValueType('weak')` descriptor. The three old Source regressions exercise
ordinary ownership/function-value/generic behaviour and do not require it.

Expected Source/capability failures (7):

| fixture | required result |
| --- | --- |
| `reject-user-struct` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-same-shape-struct` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-string` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-file` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-slice` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-foreign` | `E_TYPE_MISMATCH`, `Weak target must be` |
| `reject-native-handle` | `E_CAPABILITY_UNAVAILABLE`, concrete Python signature gate |

Both entries must report the original code/message, exit exactly `1`, keep
`timed_out=false`/`cleanup_complete=true`/`abandoned_pipes=false`, and neither
may publish a `program.py`. A signal exit (`-9`/`-11`) or any other non-1 exit
is a harness failure, not a valid rejection. Rejections are not converted into
skips. JSON stdout is parsed with `corpus._reject_json_constant`, so `NaN`,
`Infinity` and `-Infinity` are rejected; the existing document shape rules are
unchanged.

## Observation

Each positive case runs the unchanged `observe.py` with CPython's automatic
collector disabled before `langrt` is imported and `gc.collect` forbidden. The
observer still performs the original full checks: expected `main` return, no
program output, balanced `_gc_stats()` (`active=false`, `busy=0`, `live=0`),
zero live Vec roots, the direct Weak upgrade/downgrade/deactivation probe, and
the post-probe zero-live checks. The runner additionally re-reads the observer
document and requires those fields/invariants; it does not accept
`{"status":"passed"}` alone.

The Weak/GC observation is private to this runner. A static catalog entry that
builds the same manifest is **not** evidence that this observer ran or that GC
was tested.

## Process and provenance

Every command uses `selfhost/corpus.py::_run_process_impl` (process group,
deadline, pipe cleanup) and records argv, cwd, timeout, raw stdout/stderr,
stream SHAs, exit, and `timed_out`/`cleanup_complete`/`abandoned_pipes`
immediately in `commands.jsonl`. `require_ok` requires an `int` exit (not a
bool) and exact booleans for those three flags. `--timeout` defaults to 120 s
and must be finite and positive; the Python version probe keeps its 20 s
budget. A real timeout is recorded as a failure and the remaining cases still
run.

Every real Source compile is traced with

```text
strace --seccomp-bpf -f -qq -s 16384 -e trace=execve,execveat -o <trace> <entry> ...
```

and checked with `corpus.audit_trace` against exactly one allowed executable:
the reference CLI, the reference helper, the candidate CLI, or the API driver.
No LLVM tool, C++ helper/frontend, or `/bin/sh` is allowed in a compile trace.
Observer runs are traced under a separate
`observer_execution_not_compiler_whitelist` policy and are never checked
against the compiler whitelist.

`--out` must be new or empty; the runner refuses to overwrite evidence. Each
case writes its own `result.json`; the run also keeps `run.json`,
`commands.jsonl`, per-command raw bytes/logs/traces, and a final
`summary.json`. Input hashes are taken before and after the run: the 18
manifests plus bindings/Source, the observer and runner, the 115 `wzcc`
modules (+ package main), 37 stdlib Wz files, 2 Python runtime files, the
reference CLI/helper, candidate CLI/API driver, Python/strace, and the fixed
toolchain tools. The actual default stdlib roots embedded in the reference CLI
and helper binaries (`LANG_STD_SOURCE_ROOT`) are detected from those binaries
and the same 37-file sets are snapshotted before/after; an owner-tree copy is
not substituted for a different embedded path. Counts summarize that hash
inventory; they are not a separate tree claim.

## Explicitly unsupported

Native shared handles (`std/thread.Thread`,
`std/sync.Mutex/Condvar/AtomicI64`, `std/ipc.Memory`) are valid frontend
targets but remain an explicit Python capability error; they are not simulated
with host handles. No arbitrary struct with a single Vec field is unwrapped:
the Box projection is selected only for the unique exported canonical
`std/box.Box[T]` declaration whose single private field is `cell Vec[T]` of
the declaration's own parameter.

## Controls

`runner_controls.py` exercises the real runner helpers (no compiler and no
Weak simulation):

```text
strict-fields          int-vs-bool exit and the three strict process flags
reject-exit            valid exit 1 passes; -9/-11/other/float/bool fail
nonfinite-json         NaN/Infinity/-Infinity fail; a finite document passes
single-side-diff       same-group program.py/langrt.py byte mismatch
wrong-diagnostic       wrong code is rejected
residual-program       a rejected case with a leftover program.py is rejected
fake-observer          a document with only {"status":"passed"} is rejected
invalid-timeout        nan/inf/0/-1 are rejected before prepare_output
```

`test_runner_controls.py` is the pure-Python subset, including proof that an
invalid `--timeout` exits before any output directory is prepared.

## Run

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
PY312="${WOOZY_TOOLCHAIN_ROOT}/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12"
MAIN="${WOOZY_WORK_DIR:-$(dirname "$(git rev-parse --show-toplevel)")}"
BUILD_WORKTREE="${WOOZY_BUILD_WORKTREE:?set WOOZY_BUILD_WORKTREE to the worktree with .work/gc-registry-index/build-new}"

"$PY312" -s tests/python/weak_source/run_weak_source.py \
  --cli "$BUILD_WORKTREE/.work/gc-registry-index/build-new/woozyc" \
  --helper build/woozy-stdlib-native/tests/python/lang_python_emit \
  --candidate-cli "$MAIN/checks/selfhost-stage123-opt2-arityfixed-20261114.oob1x0_v/stage1/woozyc" \
  --api-driver "$MAIN/checks/SH12f-python-slice-single-correction-20261114/run-candidate-28/driver/root/pythonobs" \
  --root . --out /tmp/weak-source-results --python "$PY312" \
  --strace "${WOOZY_TOOLCHAIN_ROOT}/strace-7.2/usr/bin/strace" \
  --toolchain-root "${WOOZY_TOOLCHAIN_ROOT}" \
  --timeout 120
```

Omitting the candidate flags keeps the legacy reference `--cli`/`--helper`
mode. The runner is intentionally not registered in shared CMake by this step.
