#!/usr/bin/env python3
"""Portable negative controls for run_slice_source.py.

These controls call the real runner helpers.  They do not pretend to test Wz
Source programs: they prove that the harness rejects a wrong exit, a finite
timeout, a grandchild that keeps a pipe open, a one-sided generated-file
difference, a fake observer that only says "passed", and a missing fixture.
They also prove the exact exec whitelist rejects a non-forbidden `/bin/sh`
child and an execveat bypass, and that a real API-driver publish failure does
not replace an old program or leave a new one.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_slice_source as runner  # noqa: E402


def expect_raises(call, needle: str) -> None:
    try:
        call()
    except Exception as error:
        if needle not in str(error):
            raise RuntimeError(
                f"expected {needle!r} in error, got {error!r}") from error
        return
    raise RuntimeError(f"expected an error containing {needle!r}")


def control_wrong_exit(work: Path, env: dict[str, str], log: list[dict]) -> None:
    record = runner.run_command(
        ["sh", "-c", "exit 7"], work, 5, work, log, "control-wrong-exit", env)
    if record["exit"] != 7:
        raise RuntimeError(f"wrong-exit control saw exit {record['exit']}")
    expect_raises(lambda: runner.require_ok(record), "exit 7")


def control_timeout(work: Path, env: dict[str, str], log: list[dict]) -> None:
    record = runner.run_command(
        ["sh", "-c", "sleep 2"], work, 0.2, work, log,
        "control-timeout", env)
    if not record["timed_out"]:
        raise RuntimeError("finite timeout control did not set timed_out")
    if not record["cleanup_complete"] or record["abandoned_pipes"]:
        raise RuntimeError(
            "finite timeout control left incomplete cleanup: "
            f"{record['cleanup_complete']} {record['abandoned_pipes']}")
    expect_raises(lambda: runner.require_ok(record), "timed_out=True")


def control_grandchild_pipe(work: Path, env: dict[str, str],
                            log: list[dict]) -> None:
    record = runner.run_command(
        ["sh", "-c", "sleep 1 & exit 0"], work, 2, work, log,
        "control-grandchild-pipe", env)
    if record["exit"] != 0:
        raise RuntimeError(f"grandchild control parent exit {record['exit']}")
    if record["cleanup_complete"] or not record["abandoned_pipes"]:
        raise RuntimeError(
            "grandchild pipe was not reported as abandoned: "
            f"cleanup_complete={record['cleanup_complete']} "
            f"abandoned_pipes={record['abandoned_pipes']}")
    expect_raises(lambda: runner.require_ok(record), "abandoned_pipes=True")
    # Let the short-lived grandchild observe EOF and exit before cleanup.
    time.sleep(1.1)


def control_single_side_diff(work: Path, log: list[dict]) -> None:
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


def control_fake_observer(work: Path, root: Path, python: Path,
                          env: dict[str, str], log: list[dict]) -> None:
    fake = work / "control-fake-observer.py"
    fake.write_text(
        "import json, sys\n"
        "print(json.dumps({'status': 'passed'}))\n"
        "raise SystemExit(0)\n",
        encoding="utf-8")
    cli = work / "control-fake-cli"
    cli.mkdir(parents=True, exist_ok=True)
    expect_raises(
        lambda: runner.run_observer(
            python, root, cli, False, 5, work, log,
            "control-fake-observer", env, work / "observer.json", fake),
        "observer document missing")


def control_missing_case(work: Path, root: Path) -> None:
    fake_root = work / "control-missing-root"
    fixtures = fake_root / "tests/python/slice_source/fixtures"
    fixtures.mkdir(parents=True)
    real = root / "tests/python/slice_source/fixtures"
    missing = "gc-cycle"
    for name in runner.POSITIVE + tuple(runner.PANIC) + tuple(runner.NEGATIVE):
        if name == missing:
            continue
        (fixtures / name).symlink_to(real / name, target_is_directory=True)
    expect_raises(lambda: runner.verify_inventory(fake_root), missing)



def control_trace_whitelist(work: Path, log: list[dict]) -> None:
    allowed_path = work / "control-allowed-entry"
    allowed_path.write_text("", encoding="utf-8")
    allowed = {str(allowed_path.resolve())}

    plain = work / "control-whitelist.trace"
    plain.write_text(
        f'123 execve("{allowed_path}", ["allowed"], 0x0) = 0\n'
        '123 execve("/bin/sh", ["/bin/sh"], 0x0) = 0\n',
        encoding="utf-8")
    expect_raises(
        lambda: runner.audit_candidate_trace(
            {"label": "control-whitelist", "trace_path": str(plain)}, allowed),
        "unexpected compiler subprocesses")

    bypass = work / "control-execveat.trace"
    bypass.write_text(
        f'123 execve("{allowed_path}", ["allowed"], 0x0) = 0\n'
        '123 execveat(AT_FDCWD, "/bin/sh", ["/bin/sh"], 0x0) = 0\n',
        encoding="utf-8")
    expect_raises(
        lambda: runner.audit_candidate_trace(
            {"label": "control-execveat", "trace_path": str(bypass)}, allowed),
        "unexpected compiler subprocesses")

    empty = work / "control-execveat-empty.trace"
    empty.write_text(
        f'123 execve("{allowed_path}", ["allowed"], 0x0) = 0\n'
        '123 execveat(AT_FDCWD, "", ["x"], 0x0) = 0\n',
        encoding="utf-8")
    expect_raises(
        lambda: runner.audit_candidate_trace(
            {"label": "control-execveat-empty", "trace_path": str(empty)},
            allowed),
        "empty pathname")


def control_io_atomic(work: Path, root: Path, driver: Path,
                      env: dict[str, str], log: list[dict],
                      timeout: float) -> None:
    manifest = root / "tests/python/slice_source/fixtures/python-slice/lang.toml"
    base = work / "control-io"
    base.mkdir(parents=True, exist_ok=True)

    sentinel_out = base / "sentinel"
    sentinel_out.mkdir()
    sentinel = sentinel_out / "program.py"
    sentinel.write_text("OLD-PROGRAM\n", encoding="utf-8")
    (sentinel_out / "langrt.py").mkdir()
    record = runner.run_command(
        [driver, "--project", manifest, "--out-dir", sentinel_out,
         "--format", "json"], work, timeout, work, log,
        "control-io-sentinel", env)
    runner.require_ok(record, expected_exit=2)
    document = runner.parse_json(record)
    codes = [item.get("code") for item in document.get("diagnostics", [])]
    if document.get("status") != "error" or "E_INPUT_IO" not in codes:
        raise RuntimeError(f"I/O control wrong diagnostics: {document}")
    if sentinel.read_text(encoding="utf-8") != "OLD-PROGRAM\n":
        raise RuntimeError("I/O control changed the old program.py")
    if not (sentinel_out / "langrt.py").is_dir():
        raise RuntimeError("I/O control replaced the runtime sentinel")
    if any(path.name.startswith(".woozyc-") for path in sentinel_out.iterdir()):
        raise RuntimeError("I/O control left a staging directory")

    fresh_out = base / "fresh"
    fresh_out.mkdir()
    (fresh_out / "langrt.py").mkdir()
    record = runner.run_command(
        [driver, "--project", manifest, "--out-dir", fresh_out,
         "--format", "json"], work, timeout, work, log,
        "control-io-fresh", env)
    runner.require_ok(record, expected_exit=2)
    document = runner.parse_json(record)
    codes = [item.get("code") for item in document.get("diagnostics", [])]
    if document.get("status") != "error" or "E_INPUT_IO" not in codes:
        raise RuntimeError(f"fresh I/O control wrong diagnostics: {document}")
    if (fresh_out / "program.py").exists():
        raise RuntimeError("I/O control published a new program.py")
    if any(path.name.startswith(".woozyc-") for path in fresh_out.iterdir()):
        raise RuntimeError("fresh I/O control left a staging directory")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path,
                        default=str(runner.COMPILER_ROOT))
    parser.add_argument("--python", type=Path,
                        default=Path(sys.executable))
    parser.add_argument("--work-dir", type=Path, default=None)
    parser.add_argument("--api-driver", type=Path, default=None,
                        help="prebuilt Wz API driver for the I/O control")
    parser.add_argument("--timeout", type=runner.finite_timeout, default=180.0)
    args = parser.parse_args()
    root = args.root.resolve()
    python = args.python.resolve()
    parent = args.work_dir.resolve() if args.work_dir else None
    work = Path(tempfile.mkdtemp(prefix="slice-runner-controls-", dir=parent))
    env = runner.environment(root.parent / "toolchains", work)
    log: list[dict] = []
    controls = []
    try:
        controls.append("wrong-exit")
        control_wrong_exit(work, env, log)
        controls.append("finite-timeout")
        control_timeout(work, env, log)
        controls.append("grandchild-pipe")
        control_grandchild_pipe(work, env, log)
        controls.append("single-side-diff")
        control_single_side_diff(work, log)
        controls.append("fake-observer")
        control_fake_observer(work, root, python, env, log)
        controls.append("missing-case")
        control_missing_case(work, root)
        controls.append("trace-whitelist")
        control_trace_whitelist(work, log)
        if args.api_driver is not None:
            controls.append("io-atomic-publish")
            control_io_atomic(work, root, args.api_driver.resolve(), env, log,
                              args.timeout)
        else:
            print("CONTROLS note: io-atomic-publish not_run "
                  "(pass --api-driver)")
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
