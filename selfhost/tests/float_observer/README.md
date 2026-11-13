# Pure-Wz floating observer harness

Test-only. No production language, CLI, stdlib, fixture or gold change.

## Files

- `driver/main.wz` — pure Wz driver for
  `project.load -> resolve -> sema.check_project -> HIR verify ->
  nativelower.lower / Native verify -> nativeemit.emit -> nativebuild.build`
  or `pythonemit.emit`. It takes explicit project/stdlib/resources/out/
  backend/opt arguments and never imports the C++ front end or old helper.
- `llvm_observer.cpp` — LLVM-only observer. It reads the driver's pre-opt LLVM,
  metadata and requests, verifies the input hash/module and actual function
  types, replaces only the private entry body and verifies the result. Duplicate
  source names (generic instances) are resolved by actual result/parameter
  signature; ambiguity is rejected.
- `run.py` — the single harness for the six corrected areas and canonical/full
  numeric runs.
- `README.md` — this file.

## Canonical identity

`run.py` never treats a bare flag or the mere existence of a proof file as
canonical. It requires an `accepted` proof, matching candidate absolute path
and SHA, matching Root `verify_ops`, and an existing frozen proof source
snapshot. Otherwise the report is `component`, never `passed` canonical.

The canonical run uses:

- proof: `checks/native-function-type-fix-accepted-20261114.json`
- candidate: `61785485...` (not the old `5ca02c9d...` component binary)
- Root current 115 Wz files + `driver/main.wz` for the new driver
- frozen copy of those inputs for the run root
- accepted `verify_ops.wz` `a9c375...`

## Strict commands and work directories

Every command is run through the Root `selfhost/corpus.py::_run_process_impl`.
A successful command must have exactly the expected exit status (positive 0,
negative controls their exact non-zero/101), no timeout, complete process-group
cleanup and no abandoned pipes. Partial stdout/stderr and argv are written
before the check. Signal/timeout is never accepted as control detection.

A reused driver needs a successful build record matching the actual binary,
driver main, all 115 Wz source hashes, bootstrap compiler, helper and runtime.
A non-empty `--work-dir` is rejected before logs/results/staging. New runs use
new directories; old reports are not overwritten.

All `--root` reads (driver source, fixtures, cases, vectors, gold/helper
modules, stdlib, runtime) come from that root and are hashed as read.

## Modes

```sh
# short six-area controls
python3 selfhost/tests/float_observer/run.py --controls-only \
  --root <frozen-root> --work-dir <new-empty-dir>

# canonical full numeric run
python3 selfhost/tests/float_observer/run.py --profile full \
  --root <frozen-root> --driver-binary <driver> \
  --driver-build-evidence <evidence.json> \
  --candidate-cli <canonical-6178> --work-dir <new-empty-dir>
```

Full profile uses the original 10 floating fixtures (76 probes), the original
native dynamic fixture (`3138`), the original values-cleanup fixture (`6`), and
the Python APFloat table (`3362`). `supplemental.py` separately checks the
original Python supplemental 14 projects and 9 function observations with the
retained CLI and driver. It checks exact result and parameter types, Cell
updates, TypedError and Panic behavior. Driver metadata does not include
parameter modes or throws declarations; those fields are not independently
checked. This is a frozen-source result, not a build of the latest compiler.

## Runner controls

Run `python3 -B selfhost/tests/float_observer/test_controls.py -v` for the
small process, provenance, strict JSON and timeout checks. These tests use
isolated tool fixtures and do not compile a language program. The runner
validates the accepted compiler's complete frozen inputs with the existing
bootstrap collector; a build record can be reused only after its source,
binary, bootstrap, helper, runtime and clean exit checks pass.

Run `python3 -B supplemental_signature_controls.py --evidence <results.json>`
from this directory for the focused typed/Panic result-signature controls.
Use `supplemental.py --help` for the full supplemental run; supply the frozen
source tree and retained driver/CLI evidence associated with that build.
