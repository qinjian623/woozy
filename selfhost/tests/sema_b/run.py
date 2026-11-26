#!/usr/bin/env python3
"""Compile and optionally execute the B-side sema cases with stage-0.

The staged package contains the real shared sema files, the real B files from
this worktree and the real A/hirverify sources supplied by the caller.  It does
not create stubs: without both dependency directories the runner refuses to
claim a full Checker check.  `--run` additionally builds and executes the Wz
case driver; it is optional because the complete Checker build is expensive and
was explicitly allowed to remain not_run while A/B/facade integration is in
progress.
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
SUMMARY = re.compile(r"SEMA_B passed=(\d+) failed=(\d+)\s*")
sys.path.insert(0, str(HERE.parents[1]))
from corpus import _run_process_impl  # noqa: E402


def copy_files(source: Path, target: Path, pattern: str) -> None:
    for path in sorted(source.glob(pattern)):
        shutil.copy2(path, target / path.name)


def stage_project(work: Path, a_dir: Path, hirverify_dir: Path,
                  toolchain_root: Path, slim: bool) -> Path:
    project = work / "project"
    modules = project / "src" / "wzcc"
    sema = modules / "sema"
    sema.mkdir(parents=True)
    shared = ["run.wz"]
    if not slim:
        shared.append("check.wz")
    sema_source = SELFHOST / "sema"
    for name in shared:
        shutil.copy2(sema_source / name, sema / name)
    copy_files(sema_source, sema, "b_*.wz")
    if not list(sema.glob("b_*.wz")):
        raise RuntimeError(f"no B-side sema files in {sema_source}")
    for package in sorted(sema_source.iterdir()):
        if package.is_dir():
            shutil.copytree(package, sema / package.name, ignore=shutil.ignore_patterns("a_*.wz"))
    a_files = {source.relative_to(sema_source) for source in sema_source.rglob("a_*.wz")}
    a_files.update(source.relative_to(a_dir) for source in a_dir.rglob("a_*.wz"))
    for name in sorted(a_files):
        source = a_dir / name
        if not source.is_file():
            raise RuntimeError(f"missing staged A-side sema file: {source}")
        target = sema / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    if not slim:
        if not hirverify_dir.is_dir():
            raise RuntimeError(f"hirverify directory missing: {hirverify_dir}")
        shutil.copytree(hirverify_dir, modules / "hirverify")
    if slim:
        shutil.copy2(HERE / "helpers.wz", sema / "helpers.wz")
    for module in sorted(SELFHOST.iterdir()):
        if module.is_dir() and module.name not in ("sema", "hirverify"):
            (modules / module.name).symlink_to(module, target_is_directory=True)
    (project / "src" / "std").symlink_to(ROOT / "stdlib", target_is_directory=True)
    driver = "main_slim.wz" if slim else "main.wz"
    shutil.copy2(HERE / "src" / driver, project / "src" / "main.wz")
    (project / "lang.toml").write_text(
        "schema_version=1\n"
        "[package]\n"
        'name="sema_b_tests"\n'
        'source_root="src"\n'
        'entry="app"\n'
        "[build]\n"
        'backend="native"\n'
        "word_bits=64\n",
        encoding="utf-8")
    return project


def environment(work: Path, toolchain_root: Path) -> dict[str, str]:
    env = dict(os.environ)
    paths = []
    if toolchain_root.is_dir():
        paths.extend([
            str(toolchain_root / "llvm-22.1.8" / "lib"),
            str(toolchain_root / "libxml2-legacy-2.13.9" / "usr" / "lib"),
        ])
    if env.get("LD_LIBRARY_PATH"):
        paths.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(paths)
    env["TMPDIR"] = str(work / "tmp")
    (work / "tmp").mkdir(exist_ok=True)
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
    parser.add_argument("--woozyc", required=True,
                        help="stage-0 woozyc executable")
    parser.add_argument("--a-dir", required=True,
                        help="sema source directory containing the real a_*.wz files in their package directories")
    parser.add_argument("--hirverify-dir", default="",
                        help="directory containing the real hirverify package; "
                             "required unless --slim")
    parser.add_argument("--toolchain-root",
                        default=str(ROOT.parent / "toolchains"),
                        help="directory containing llvm-22.1.8 and libxml2-legacy")
    parser.add_argument("--work-dir",
                        help="parent directory for retained run artifacts")
    parser.add_argument("--timeout", type=finite_timeout, default=600.0,
                        help="per-stage timeout (default 600s)")
    parser.add_argument("--slim", action="store_true",
                        help="omit check.wz/hirverify for the faster body/effect "
                             "case driver (the full facade check uses the default)")
    parser.add_argument("--run", action="store_true",
                        help="also build and execute the Wz case driver")
    args = parser.parse_args()
    woozyc = Path(args.woozyc).expanduser().resolve()
    if not woozyc.is_file():
        raise RuntimeError(f"stage0 compiler missing: {woozyc}")
    a_dir = Path(args.a_dir).expanduser().resolve()
    hirverify_dir = Path(args.hirverify_dir).expanduser().resolve()
    if not args.slim and not args.hirverify_dir:
        parser.error("--hirverify-dir is required unless --slim")
    toolchain_root = Path(args.toolchain_root).expanduser().resolve()
    parent = None
    if args.work_dir:
        parent = Path(args.work_dir).expanduser().resolve()
        parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="sema-b-", dir=parent))
    project = stage_project(work, a_dir, hirverify_dir, toolchain_root,
                            args.slim)
    env = environment(work, toolchain_root)
    print(f"work-dir: {work}", flush=True)

    sema_source = SELFHOST / "sema"
    syntax_sources = sorted(source for source in sema_source.rglob("*.wz")
                            if source.name.startswith("b_") or
                            (source.parent != sema_source and not source.name.startswith("a_")))
    for source in syntax_sources:
        label = "-".join(source.relative_to(sema_source).with_suffix("").parts)
        command([str(woozyc), "check", "--syntax-only", str(source)],
                label + "-syntax", work, args.timeout, env)
    print("PASS: per-file syntax checks", flush=True)

    manifest = str(project / "lang.toml")
    checked = command([str(woozyc), "check", "--project", manifest,
                       "--backend", "native", "--format", "json"],
                      "check", work, args.timeout, env)
    compiler_ok(checked, "check")
    if args.slim:
        print("PASS: staged A/B/shared slim Checker type check "
              "(facade/verifier omitted)", flush=True)
    else:
        print("PASS: staged A/B/shared/hirverify Checker type check", flush=True)

    if not args.run:
        print("not_run: --run not requested (full native build/execution)")
        return 0
    for opt in (0, 2):
        output = work / f"out-o{opt}"
        built = command([str(woozyc), "build", "--project", manifest,
                         "--backend", "native", "--opt", str(opt),
                         "--out-dir", str(output), "--format", "json"],
                        f"build-o{opt}", work, args.timeout, env)
        compiler_ok(built, f"build-o{opt}")
        tested = command([str(output / "program")], f"run-o{opt}", work,
                         args.timeout, env)
        match = SUMMARY.search(tested.stdout)
        if match is None:
            raise RuntimeError(
                f"case driver did not emit the summary: {tested.stdout!r}")
        passed, failed = map(int, match.groups())
        if failed != 0:
            raise RuntimeError(
                f"case driver failed {failed} of {passed + failed} checks at O{opt}")
        print(f"PASS: B-side semantic cases O{opt} {passed}/{passed}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
