#!/usr/bin/env python3
"""Check only sema common helpers using the real current shared records.

The temporary sema/core package contains production state/errors/common plus the
same-package tests. It deliberately excludes clone and unfinished checker
algorithms; this is not a claim that the full Checker builds. Other packages
link to the current repository sources. No algorithm stubs are created.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SELFHOST = ROOT / "selfhost" / "src" / "wzcc"
DEFAULT_STAGE0 = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
SEMA_FILES = ("state.wz", "errors.wz", "common.wz")
EXPECTED_CHECKS = 46
SUMMARY = re.compile(r"COMMON_HELPERS passed=(\d+) failed=(\d+)\s*")


def stage_project(work: Path) -> Path:
    project = work / "project"
    modules = project / "src" / "wzcc"
    sema = modules / "sema"
    core = sema / "core"
    core.mkdir(parents=True)
    sema_source = SELFHOST / "sema"
    for name in SEMA_FILES:
        shutil.copy2(sema_source / "core" / name, core / name)
    shutil.copytree(sema_source / "borrow", sema / "borrow")
    shutil.copy2(HERE / "helpers.wz", core / "helpers.wz")
    for module in sorted(SELFHOST.iterdir()):
        if module.is_dir() and module.name != "sema":
            (modules / module.name).symlink_to(module, target_is_directory=True)
    shutil.copy2(HERE / "src" / "main.wz", project / "src" / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def environment() -> dict[str, str]:
    env = dict(os.environ)
    toolchain = ROOT.parent / "toolchains"
    paths = [str(toolchain / "llvm-22.1.8" / "lib"),
             str(toolchain / "libxml2-legacy-2.13.9" / "usr" / "lib")]
    if env.get("LD_LIBRARY_PATH"):
        paths.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(paths)
    return env


def command(argv: list[str], label: str, work: Path, timeout: float,
            env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(argv), flush=True)
    process = subprocess.Popen(argv, cwd=work, env=env, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        stdout, stderr = process.communicate()
        (work / f"{label}.stdout").write_text(stdout)
        (work / f"{label}.stderr").write_text(stderr)
        raise RuntimeError(f"{label}: timed out after {timeout:g}s") from None
    (work / f"{label}.stdout").write_text(stdout)
    (work / f"{label}.stderr").write_text(stderr)
    if process.returncode != 0:
        raise RuntimeError(f"{label}: exit {process.returncode}\n{stdout}{stderr}")
    return subprocess.CompletedProcess(argv, process.returncode, stdout, stderr)


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
    parser.add_argument("--timeout", type=float, default=300.0,
                        help="per-command timeout in seconds")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    stage0 = Path(args.woozyc).expanduser().resolve()
    if not stage0.is_file():
        raise RuntimeError(f"stage0 compiler missing: {stage0}")
    parent = None
    if args.work_dir:
        parent = Path(args.work_dir).expanduser().resolve()
        parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="sema-common-", dir=parent))
    project = stage_project(work)
    manifest = str(project / "lang.toml")
    env = environment()
    print(f"work-dir: {work}", flush=True)
    checked = command([str(stage0), "check", "--project", manifest,
                       "--backend", "native", "--format", "json"],
                      "check", work, args.timeout, env)
    compiler_ok(checked, "check")
    print("PASS: common helper type check", flush=True)
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
        print(f"PASS: common helpers O{opt} {EXPECTED_CHECKS}/{EXPECTED_CHECKS}", flush=True)
    print("PASS: common only (check, O0, O2); full Checker not exercised")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
