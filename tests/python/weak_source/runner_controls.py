#!/usr/bin/env python3
"""Portable negative controls for run_weak_source.py.

These controls call the real runner helpers.  They do not compile a Woozy
program and they do not reimplement Weak semantics: they prove that the
harness rejects bad process fields, a non-1/signal rejection exit, a
one-sided generated-file difference, a wrong diagnostic, a residual program,
NaN/Infinity JSON, a fake observer that only says "passed", and an invalid
timeout before any output directory is created.
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_weak_source as runner  # noqa: E402


def expect_raises(call, needle: str) -> None:
    try:
        call()
    except Exception as error:
        if needle not in str(error):
            raise RuntimeError(
                f"expected {needle!r} in error, got {error!r}") from error
        return
    raise RuntimeError(f"expected an error containing {needle!r}")


def record(exit_code=1, **changes):
    return dict(dict(label="process fixture", exit=exit_code,
                     timed_out=False, cleanup_complete=True,
                     abandoned_pipes=False, stdout="", stderr=""), **changes)


def control_strict_fields() -> None:
    runner.require_ok(record(0), expected_exit=0)
    bad_fields = {
        "exit": [False, True, 0.0, None, -9],
        "timed_out": [0, None, True],
        "cleanup_complete": [1, None, False],
        "abandoned_pipes": [0, None, True],
    }
    for key, values in bad_fields.items():
        for value in values:
            expect_raises(
                lambda key=key, value=value: runner.require_ok(
                    record(0, **{key: value}), expected_exit=0),
                key)


def control_reject_exit(work: Path) -> None:
    """A real Source/capability rejection is exit 1, never a signal/other."""
    directory = work / "control-reject-exit"
    directory.mkdir()
    document = {
        "status": "error",
        "diagnostics": [{"code": "E_TYPE_MISMATCH",
                          "message": "Weak target must be Vec"}],
    }
    runner.check_reject(record(1), document, "E_TYPE_MISMATCH",
                        "Weak target must be", directory,
                        "control-reject-exit")
    for value in (-9, -11, 2, 0, True, False, 1.0, None):
        expect_raises(
            lambda value=value: runner.check_reject(
                record(value), document, "E_TYPE_MISMATCH",
                "Weak target must be", directory, "control-reject-exit"),
            "did not exit 1")
    for key, value, needle in (
            ("timed_out", True, "timed out"),
            ("cleanup_complete", False, "did not clean up"),
            ("abandoned_pipes", True, "abandoned pipes")):
        expect_raises(
            lambda key=key, value=value: runner.check_reject(
                record(1, **{key: value}), document, "E_TYPE_MISMATCH",
                "Weak target must be", directory, "control-reject-exit"),
            needle)


def control_nonfinite_json(work: Path) -> None:
    """NaN/Infinity/-Infinity are not accepted as a valid rejection document."""
    directory = work / "control-nonfinite-json"
    directory.mkdir()
    valid = ('{"status":"error","diagnostics":[{"code":"E_TYPE_MISMATCH",'
             '"message":"Weak target must be Vec"}],"ratio":1.25}')
    valid_record = record(1, stdout=valid)
    valid_document = runner.parse_json(valid_record)
    runner.check_reject(valid_record, valid_document, "E_TYPE_MISMATCH",
                        "Weak target must be", directory,
                        "control-nonfinite-json")
    for token in ("NaN", "Infinity", "-Infinity"):
        text = ('{"status":"error","diagnostics":[{"code":"E_TYPE_MISMATCH",'
                '"message":"Weak target must be Vec"}],"extra":' + token + '}')
        expect_raises(
            lambda text=text: runner.parse_json(record(1, stdout=text)),
            "strict JSON")


def control_single_side_diff(work: Path) -> None:
    left = work / "control-diff" / "left"
    right = work / "control-diff" / "right"
    left.mkdir(parents=True)
    right.mkdir(parents=True)
    for directory in (left, right):
        (directory / "program.py").write_text("print('same')\n")
        (directory / "langrt.py").write_text("RUNTIME = 1\n")
    first = {"path": str(left), "kind": "cli"}
    second = {"path": str(right), "kind": "api"}
    runner.compare_entries(first, second, "control-diff")
    (right / "program.py").write_text("print('different')\n")
    expect_raises(
        lambda: runner.compare_entries(first, second, "control-diff"),
        "disagree on generated program.py")


def control_wrong_diagnostic(work: Path) -> None:
    directory = work / "control-wrong-diagnostic"
    directory.mkdir()
    document = {
        "status": "error",
        "diagnostics": [{"code": "E_OTHER", "message": "other failure"}],
    }
    expect_raises(
        lambda: runner.check_reject(
            record(1), document, "E_TYPE_MISMATCH", "Weak target must be",
            directory, "control-wrong-diagnostic"),
        "expected E_TYPE_MISMATCH")


def control_residual_program(work: Path) -> None:
    directory = work / "control-residual-program"
    directory.mkdir()
    (directory / "program.py").write_text("print('residual')\n")
    document = {
        "status": "error",
        "diagnostics": [{"code": "E_TYPE_MISMATCH",
                         "message": "Weak target must be Vec"}],
    }
    expect_raises(
        lambda: runner.check_reject(
            record(1), document, "E_TYPE_MISMATCH", "Weak target must be",
            directory, "control-residual-program"),
        "generated a program")


def control_fake_observer(work: Path, root: Path, python: Path,
                          env: dict[str, str], log: list[dict]) -> None:
    fake = work / "control-fake-observer.py"
    fake.write_text(
        "import json\n"
        "print(json.dumps({'status': 'passed'}))\n"
        "raise SystemExit(0)\n",
        encoding="utf-8")
    cli = work / "control-fake-cli"
    cli.mkdir(parents=True, exist_ok=True)
    expect_raises(
        lambda: runner.run_observer(
            python, root, cli, 0, 5, work, log,
            "control-fake-observer", env, work / "observer.json", fake),
        "observer document missing")


def control_invalid_timeout() -> None:
    for value in ("nan", "inf", "-inf", "0", "-1"):
        try:
            runner.finite_timeout(value)
        except argparse.ArgumentTypeError:
            continue
        raise RuntimeError(f"invalid timeout {value!r} was accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path,
                        default=str(runner.COMPILER_ROOT))
    parser.add_argument("--python", type=Path,
                        default=Path(sys.executable))
    parser.add_argument("--work-dir", type=Path, default=None)
    args = parser.parse_args()
    root = args.root.resolve()
    python = args.python.resolve()
    parent = args.work_dir.resolve() if args.work_dir else None
    work = Path(tempfile.mkdtemp(prefix="weak-runner-controls-", dir=parent))
    env = runner.environment(root.parent / "toolchains", work)
    log: list[dict] = []
    controls = []
    try:
        controls.append("strict-fields")
        control_strict_fields()
        controls.append("reject-exit")
        control_reject_exit(work)
        controls.append("nonfinite-json")
        control_nonfinite_json(work)
        controls.append("single-side-diff")
        control_single_side_diff(work)
        controls.append("wrong-diagnostic")
        control_wrong_diagnostic(work)
        controls.append("residual-program")
        control_residual_program(work)
        controls.append("fake-observer")
        control_fake_observer(work, root, python, env, log)
        controls.append("invalid-timeout")
        control_invalid_timeout()
        print(f"CONTROLS passed={len(controls)} " + " ".join(controls))
        return 0
    finally:
        if parent is None:
            shutil.rmtree(work, ignore_errors=True)
        else:
            keep = work / "commands.json"
            keep.write_text(json.dumps(log, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"CONTROLS failed: {error}", file=sys.stderr)
        raise SystemExit(1)
