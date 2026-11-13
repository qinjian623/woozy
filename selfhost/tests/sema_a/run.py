#!/usr/bin/env python3
"""Check and run A-side sema declaration/type/generic tests.

The staged sema package contains the production shared records, common helpers,
and A's helpers/types/declarations.  It deliberately excludes a_instantiate:
instantiate/transform call B's annotate/flow, so this runner makes no complete
Checker or self-hosting claim.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SELFHOST = ROOT / "selfhost" / "src" / "wzcc"
sys.path.insert(0, str(HERE.parents[1]))
from corpus import _run_process_impl  # noqa: E402
DEFAULT_STAGE0 = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
SEMA_FILES = ("state.wz", "errors.wz", "common.wz",
              "a_helpers.wz", "a_types.wz", "a_declarations.wz")
EXPECTED_CHECKS = 47
SUMMARY = re.compile(r"SEMA_A passed=(\d+) failed=(\d+)\s*")


def stage_project(work: Path) -> Path:
    project = work / "project"
    modules = project / "src" / "wzcc"
    sema = modules / "sema"
    sema.mkdir(parents=True)
    for name in SEMA_FILES:
        shutil.copy2(SELFHOST / "sema" / name, sema / name)
    shutil.copy2(HERE / "helpers.wz", sema / "helpers.wz")
    for module in sorted(SELFHOST.iterdir()):
        if module.is_dir() and module.name != "sema":
            (modules / module.name).symlink_to(module, target_is_directory=True)
    shutil.copy2(HERE / "src" / "main.wz", project / "src" / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def environment(stage0: Path) -> dict[str, str]:
    env = dict(os.environ)
    # The stage0 tree's parent is the checkout; its parent holds the
    # repository-external toolchains also used by the existing runners.
    toolchain = stage0.parents[3] / "toolchains"
    paths = [str(toolchain / "llvm-22.1.8" / "lib"),
             str(toolchain / "libxml2-legacy-2.13.9" / "usr" / "lib")]
    if env.get("LD_LIBRARY_PATH"):
        paths.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(paths)
    return env


def finite_timeout(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "--timeout must be finite and positive") from error
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("--timeout must be finite and positive")
    return value


def command(argv: list[str], label: str, work: Path, timeout: float,
            env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    if not math.isfinite(timeout) or timeout <= 0:
        raise RuntimeError(f"{label}: timeout must be finite and positive")
    print("+", " ".join(argv), flush=True)
    started = time.monotonic()
    process = _run_process_impl(argv, work, env, timeout)
    elapsed = time.monotonic() - started
    stdout_bytes = process["stdout"]
    stderr_bytes = process["stderr"]
    (work / f"{label}.stdout").write_bytes(stdout_bytes)
    (work / f"{label}.stderr").write_bytes(stderr_bytes)
    stdout = stdout_bytes.decode("utf-8", errors="replace")
    stderr = stderr_bytes.decode("utf-8", errors="replace")
    record = {
        "label": label,
        "argv": [str(part) for part in argv],
        "cwd": str(work),
        "timeout_seconds": timeout,
        "elapsed_seconds": elapsed,
        "exit": process["exit"],
        "timed_out": process["timed_out"],
        "cleanup_complete": process["cleanup_complete"],
        "abandoned_pipes": process["abandoned_pipes"],
        "stdout_path": f"{label}.stdout",
        "stderr_path": f"{label}.stderr",
    }
    (work / f"{label}.json").write_text(json.dumps(record, indent=2) + "\n")
    if (process["timed_out"] or process["exit"] != 0
            or not process["cleanup_complete"] or process["abandoned_pipes"]):
        raise RuntimeError(
            f"{label}: exit {process['exit']} timed_out={process['timed_out']} "
            f"cleanup_complete={process['cleanup_complete']} "
            f"abandoned_pipes={process['abandoned_pipes']}\n{stdout}{stderr}")
    return subprocess.CompletedProcess(argv, process["exit"], stdout, stderr)


def compiler_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    try:
        report = json.loads(result.stdout)
    except ValueError as error:
        raise RuntimeError(f"{label}: invalid compiler JSON: {result.stdout}") from error
    if report.get("status") != "ok":
        raise RuntimeError(f"{label}: compiler did not succeed: {result.stdout}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0))
    parser.add_argument("--work-dir", help="parent directory for retained run artifacts")
    parser.add_argument("--timeout", type=finite_timeout, default=300.0,
                        help="per-command timeout in seconds")
    args = parser.parse_args()
    stage0 = Path(args.woozyc).expanduser().resolve()
    if not stage0.is_file():
        raise RuntimeError(f"stage0 compiler missing: {stage0}")
    if args.work_dir:
        parent = Path(args.work_dir).expanduser().resolve()
    else:
        parent = ROOT / "build" / "sema-a-runs"
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="sema-a-", dir=parent))
    (work / "tmp").mkdir()
    project = stage_project(work)
    manifest = str(project / "lang.toml")
    env = environment(stage0)
    env["TMPDIR"] = str(work / "tmp")
    print(f"work-dir: {work}", flush=True)
    checked = command([str(stage0), "check", "--project", manifest,
                       "--backend", "native", "--format", "json"],
                      "check", work, args.timeout, env)
    compiler_ok(checked, "check")
    print("PASS: A-side declaration/type/generic type check", flush=True)
    for opt in (0, 2):
        output = work / f"out-o{opt}"
        built = command([str(stage0), "build", "--project", manifest,
                         "--backend", "native", "--opt", str(opt),
                         "--out-dir", str(output), "--format", "json"],
                        f"build-o{opt}", work, args.timeout, env)
        compiler_ok(built, f"build O{opt}")
        tested = command([str(output / "program")], f"run-o{opt}",
                         work, args.timeout, env)
        match = SUMMARY.fullmatch(tested.stdout)
        if match is None or tuple(map(int, match.groups())) != (EXPECTED_CHECKS, 0):
            raise RuntimeError(f"O{opt}: expected {EXPECTED_CHECKS}/0, got {tested.stdout!r}")
        print(f"PASS: A-side tests O{opt} {EXPECTED_CHECKS}/{EXPECTED_CHECKS}", flush=True)
    print("PASS: A declarations/types/generics; instantiate+B not exercised")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
