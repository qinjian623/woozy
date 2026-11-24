#!/usr/bin/env python3
"""Build and run std/async TaskGroup cases through an isolated compiler stage.

The released compiler is never modified. Its bundle is copied/symlinked into a
fresh temporary prefix, and that prefix's stdlib is replaced with this
checkout's stdlib. Native builds then resolve the staged stdlib because the
compiler derives its prefix from its own executable path.

Usage:
  python3 tests/stdlib_task_group/run_tests.py \
    --compiler /tmp/woozy-install-linux.mGq1t1xr/prefix/bin/woozyc \
    --out /tmp/stdlib_task_group-evidence
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
DEFAULT_COMPILER = os.environ.get("WOOZYC", "")
BUNDLE_LINKS = ("toolchain", "runtime", "tools")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tree_digest(directory: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for path in sorted(directory.rglob("*")):
        if path.is_file():
            result[str(path.relative_to(directory))] = sha256_file(path)
    return result


def stage_compiler(compiler: Path, stdlib: Path, stage: Path,
                   runtime_archive: Path | None = None) -> Path:
    real = compiler.resolve(strict=True)
    bundle = real.parent
    if not (bundle / "toolchain").is_dir():
        raise SystemExit(f"not a Woozy install bundle (no toolchain/): {bundle}")
    if stage.exists() and any(stage.iterdir()):
        raise SystemExit(f"stage directory must be new or empty: {stage}")

    stage_bundle = stage / "opt" / bundle.name
    stage_bin = stage / "bin"
    stage_bundle.mkdir(parents=True, exist_ok=True)
    stage_bin.mkdir(parents=True, exist_ok=True)

    shutil.copy2(real, stage_bundle / "woozyc")
    helper = bundle / "native" / "lang_native_helper"
    if helper.is_file():
        (stage_bundle / "native").mkdir()
        shutil.copy2(helper, stage_bundle / "native" / helper.name)
    for name in BUNDLE_LINKS:
        source = bundle / name
        if source.exists():
            os.symlink(source, stage_bundle / name)
    shutil.copytree(stdlib, stage_bundle / "stdlib")
    if runtime_archive is not None:
        # Replace the staged native archive only; the installed bundle is read-only.
        runtime_link = stage_bundle / "runtime"
        if runtime_link.is_symlink():
            runtime_link.unlink()
        if runtime_link.exists():
            shutil.rmtree(runtime_link)
        runtime_link.mkdir()
        shutil.copy2(runtime_archive, runtime_link / "liblang_native_runtime.a")
        for name in ("liblang_hosted_runtime.a", "liblang_darwin_core.a"):
            source = bundle / "runtime" / name
            if source.exists():
                shutil.copy2(source, runtime_link / name)

    staged = stage_bin / "woozyc"
    os.symlink(stage_bundle / "woozyc", staged)
    return staged


def command_text(command: list[str]) -> str:
    return " ".join(command)


def run_command(
    command: list[str], timeout: float, cwd: Path
) -> tuple[int, bool, str, str]:
    process = subprocess.Popen(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(process.pid, 9)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
    return process.returncode, timed_out, stdout, stderr


def check_stdout(case: dict, stdout: str) -> list[str]:
    problems: list[str] = []
    if "stdout" in case and stdout != case["stdout"]:
        problems.append(
            "stdout mismatch: expected %r, got %r"
            % (case["stdout"], stdout)
        )
    lines = stdout.splitlines()
    for expected in case.get("stdout_contains", []):
        if expected not in lines:
            problems.append(f"stdout missing line {expected!r}")
    for forbidden in case.get("stdout_absent", []):
        if forbidden in lines:
            problems.append(f"stdout contains forbidden line {forbidden!r}")
    return problems


def run_positive(
    cli: Path,
    root: Path,
    fixture: Path,
    case: dict,
    opt: str,
    out_dir: Path,
    build_timeout: float,
    run_timeout: float,
) -> dict:
    name = case["name"]
    build_dir = out_dir / "builds" / f"{name}_O{opt}"
    build_command = [
        str(cli),
        "build",
        "--project",
        str(fixture / "lang.toml"),
        "--backend",
        "native",
        "--opt",
        opt,
        "--out-dir",
        str(build_dir),
    ]
    result = {
        "kind": "positive",
        "case": name,
        "opt": opt,
        "build_command": command_text(build_command),
        "run_command": command_text([str(build_dir / "program")]),
        "status": "failed",
    }
    build_code, build_timeout_hit, build_stdout, build_stderr = run_command(
        build_command, build_timeout, root
    )
    result.update(
        build_exit=build_code,
        build_timed_out=build_timeout_hit,
        build_stdout=build_stdout,
        build_stderr=build_stderr,
    )
    if build_timeout_hit or build_code != 0:
        result["failure"] = "build failed"
        return result

    run_code, run_timeout_hit, stdout, stderr = run_command(
        [str(build_dir / "program")], run_timeout, root
    )
    result.update(
        exit=run_code,
        timed_out=run_timeout_hit,
        stdout=stdout,
        stderr=stderr,
    )
    expected_exit = case.get("exit", 0)
    problems: list[str] = []
    if run_timeout_hit:
        problems.append("program timed out")
    if run_code != expected_exit:
        problems.append(f"exit {run_code}, expected {expected_exit}")
    problems.extend(check_stdout(case, stdout))
    if problems:
        result["failure"] = "; ".join(problems)
        return result
    result["status"] = "passed"
    return result


def run_reject(
    cli: Path,
    root: Path,
    fixture: Path,
    case: dict,
    opt: str,
    out_dir: Path,
    build_timeout: float,
) -> dict:
    name = case["name"]
    build_dir = out_dir / "builds" / f"{name}_O{opt}"
    command = [
        str(cli),
        "build",
        "--project",
        str(fixture / "lang.toml"),
        "--backend",
        "native",
        "--opt",
        opt,
        "--out-dir",
        str(build_dir),
    ]
    result = {
        "kind": "reject",
        "case": name,
        "opt": opt,
        "build_command": command_text(command),
        "status": "failed",
    }
    code, timed_out, stdout, stderr = run_command(command, build_timeout, root)
    combined = stdout + stderr
    result.update(
        build_exit=code,
        build_timed_out=timed_out,
        build_stdout=stdout,
        build_stderr=stderr,
    )
    problems: list[str] = []
    if timed_out:
        problems.append("build timed out")
    if code == 0:
        problems.append("build unexpectedly succeeded")
    if case["code"] not in combined:
        problems.append(f"diagnostic {case['code']} missing")
    for unpublished in ("program", "program.o"):
        if (build_dir / unpublished).exists():
            problems.append(f"reject build published {unpublished}")
    if problems:
        result["failure"] = "; ".join(problems)
        return result
    result["status"] = "passed"
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=DEFAULT_COMPILER)
    parser.add_argument("--stdlib", default=str(REPO_ROOT / "stdlib"))
    parser.add_argument("--libxml", default="")
    parser.add_argument("--runtime", default="",
                        help="optional liblang_native_runtime.a to use in the stage")
    parser.add_argument("--fixtures", default=str(HERE / "fixtures"))
    parser.add_argument("--cases", default=str(HERE / "cases.json"))
    parser.add_argument("--out", default="")
    parser.add_argument("--stage-dir", default="")
    parser.add_argument("--build-timeout", type=float, default=120.0)
    parser.add_argument("--run-timeout", type=float, default=10.0)
    args = parser.parse_args()
    if not args.compiler:
        parser.error("--compiler is required (or set WOOZYC)")

    if args.libxml:
        library_paths = [args.libxml]
        if os.environ.get("LD_LIBRARY_PATH"):
            library_paths.append(os.environ["LD_LIBRARY_PATH"])
        os.environ["LD_LIBRARY_PATH"] = os.pathsep.join(library_paths)

    compiler = Path(args.compiler)
    stdlib = Path(args.stdlib).resolve()
    fixtures = Path(args.fixtures).resolve()
    cases = json.loads(Path(args.cases).read_text())
    out_dir = (
        Path(args.out).resolve()
        if args.out
        else Path(tempfile.mkdtemp(prefix="stdlib_task_group-evidence."))
    )
    stage = (
        Path(args.stage_dir).resolve()
        if args.stage_dir
        else Path(tempfile.mkdtemp(prefix="woozy-stdlib-taskgroup-stage."))
    )
    out_dir.mkdir(parents=True, exist_ok=True)

    installed_bundle = compiler.resolve(strict=True).parent
    compiler_before = sha256_file(compiler.resolve(strict=True))
    installed_stdlib_before = tree_digest(installed_bundle / "stdlib")

    runtime_archive = Path(args.runtime).resolve() if args.runtime else None
    cli = stage_compiler(compiler, stdlib, stage, runtime_archive)
    version_code, version_timeout, version_stdout, version_stderr = run_command(
        [str(cli), "--version"], 30.0, REPO_ROOT
    )
    print(f"stage: {stage}")
    print(f"evidence: {out_dir}")
    print(f"command: {command_text([str(cli), '--version'])}")
    print(version_stdout.strip())
    if version_timeout or version_code != 0:
        print(version_stderr, file=sys.stderr)
        return 1

    results = []
    failures = 0
    for case in cases.get("positive", []):
        for opt in ("0", "2"):
            result = run_positive(
                cli,
                REPO_ROOT,
                fixtures / case["name"],
                case,
                opt,
                out_dir,
                args.build_timeout,
                args.run_timeout,
            )
            results.append(result)
            label = f"{case['name']} O{opt}"
            if result["status"] == "passed":
                print(f"[PASS] {label}")
            else:
                failures += 1
                print(f"[FAIL] {label}: {result['failure']}")
    for case in cases.get("reject", []):
        for opt in ("0", "2"):
            result = run_reject(
                cli,
                REPO_ROOT,
                fixtures / case["name"],
                case,
                opt,
                out_dir,
                args.build_timeout,
            )
            results.append(result)
            label = f"{case['name']} O{opt}"
            if result["status"] == "passed":
                print(f"[PASS] {label}")
            else:
                failures += 1
                print(f"[FAIL] {label}: {result['failure']}")

    compiler_after = sha256_file(compiler.resolve(strict=True))
    installed_stdlib_after = tree_digest(installed_bundle / "stdlib")
    preserved = (
        compiler_before == compiler_after
        and installed_stdlib_before == installed_stdlib_after
    )
    summary = {
        "schema_version": 1,
        "contract": "compiler-dev/1",
        "compiler": str(compiler),
        "compiler_sha256_before": compiler_before,
        "compiler_sha256_after": compiler_after,
        "installed_stdlib_files": len(installed_stdlib_before),
        "installed_package_preserved": preserved,
        "stage": str(stage),
        "out": str(out_dir),
        "results": results,
        "passed": sum(1 for item in results if item["status"] == "passed"),
        "failed": failures,
    }
    (out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(
        f"summary: {summary['passed']} passed, {summary['failed']} failed, "
        f"installed package preserved={preserved}"
    )
    if not preserved:
        print("installed compiler package changed during the run", file=sys.stderr)
        return 1
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
