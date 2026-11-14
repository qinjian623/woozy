#!/usr/bin/env python3
"""Run the focused Wz HIR expression-classification regression.

The driver builds real HirNode values and calls the staged production
hirverify.expression_probe/statement_probe forwarders.  Those forwarders are
package-internal wrappers around the real verifier_expression and
verifier_statement helpers; the test does not copy either predicate.

The runner stages only the Wz compiler package tree, builds one O0 component
driver with the accepted stage0 woozyc, and executes it with the shared
corpus._run_process_impl process-group/deadline helper.  It never rebuilds the
116-source CLI, C++ probes or the complete corpus.

Use --expect-exit 1 with a pre-fix --verifier-dir to retain the observed
failure.  The fixed source must exit 0 and print every CHECK ... ok line.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SELFHOST = ROOT / "selfhost" / "src" / "wzcc"
STDLIB = ROOT / "stdlib"
DEFAULT_WOOZYC = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
DEFAULT_WORK = ROOT / "build" / "hirverify-expression-runs"
sys.path.insert(0, str(ROOT / "selfhost"))
from corpus import _run_process_impl  # noqa: E402

FIXED_CHECKS = (
    "await-42-expression",
    "trait-object-init-43-expression",
    "trait-dispatch-44-expression",
    "block-15-not-expression",
    "block-statement",
    "expression-statement-16-not-expression",
    "expression-statement-statement",
    "noop-19-not-expression",
    "noop-statement",
    "foreign-property-read-0-expression",
    "foreign-index-read-2-expression",
    "foreign-slice-read-4-expression",
    "foreign-no-access-expression",
    "foreign-property-place-1-not-expression",
    "foreign-property-place-statement-rejected",
    "foreign-index-place-3-not-expression",
    "foreign-slice-place-5-not-expression",
)
BUGGY_FAILURES = (
    "await-42-expression",
    "trait-object-init-43-expression",
    "trait-dispatch-44-expression",
)


def fail(message: str) -> None:
    raise RuntimeError(message)


def finite_timeout(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "--timeout must be finite and positive") from error
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("--timeout must be finite and positive")
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", default=str(DEFAULT_WOOZYC))
    parser.add_argument("--toolchain-root", default=os.environ.get(
                            "WOOZY_TOOLCHAIN_ROOT", ""))
    parser.add_argument("--verifier-dir", default="",
                        help="hirverify package directory (default: current)")
    parser.add_argument("--work-dir", default=str(DEFAULT_WORK))
    parser.add_argument("--timeout", type=finite_timeout, default=600.0)
    parser.add_argument("--run-timeout", type=finite_timeout, default=60.0)
    parser.add_argument("--expect-exit", type=int, choices=(0, 1), default=0)
    return parser.parse_args()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def file_record(path: Path) -> dict[str, object]:
    return {"path": str(path), "size": path.stat().st_size,
            "sha256": sha256(path)}


def resolve_toolchain(args: argparse.Namespace, woozyc: Path) -> Path:
    if args.toolchain_root:
        return Path(args.toolchain_root).expanduser().resolve()
    sibling = ROOT.parent / "toolchains"
    if sibling.is_dir():
        return sibling
    if len(woozyc.parents) > 3:
        checkout_sibling = woozyc.parents[3] / "toolchains"
        if checkout_sibling.is_dir():
            return checkout_sibling
    return sibling


def environment(toolchain_root: Path, work: Path) -> dict[str, str]:
    env = dict(os.environ)
    paths = [
        str(toolchain_root / "llvm-22.1.8" / "lib"),
        str(toolchain_root / "libxml2-legacy-2.13.9" / "usr" / "lib"),
    ]
    if env.get("LD_LIBRARY_PATH"):
        paths.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(paths)
    env["LC_ALL"] = "C"
    env["TMPDIR"] = str(work / "tmp")
    (work / "tmp").mkdir(parents=True, exist_ok=True)
    return env


def command(argv: list[str], label: str, work: Path, timeout: float,
            env: dict[str, str], expected_exit: int = 0
            ) -> subprocess.CompletedProcess[str]:
    if not math.isfinite(timeout) or timeout <= 0:
        raise RuntimeError(f"{label}: timeout must be finite and positive")
    print("+", " ".join(str(part) for part in argv), flush=True)
    started = time.monotonic()
    process = _run_process_impl(argv, work, env, timeout)
    elapsed = time.monotonic() - started
    stdout_bytes = process["stdout"]
    stderr_bytes = process["stderr"]
    (work / f"{label}.stdout").write_bytes(stdout_bytes)
    (work / f"{label}.stderr").write_bytes(stderr_bytes)
    record = {
        "label": label,
        "argv": [str(part) for part in argv],
        "cwd": str(work),
        "timeout_seconds": timeout,
        "elapsed_seconds": elapsed,
        "exit": process["exit"],
        "expected_exit": expected_exit,
        "timed_out": process["timed_out"],
        "cleanup_complete": process["cleanup_complete"],
        "abandoned_pipes": process["abandoned_pipes"],
        "stdout_path": f"{label}.stdout",
        "stderr_path": f"{label}.stderr",
        "stdout_sha256": hashlib.sha256(stdout_bytes).hexdigest(),
        "stderr_sha256": hashlib.sha256(stderr_bytes).hexdigest(),
        "stdout_bytes": len(stdout_bytes),
        "stderr_bytes": len(stderr_bytes),
    }
    (work / f"{label}.json").write_text(json.dumps(record, indent=2) + "\n")
    stdout = stdout_bytes.decode("utf-8", errors="replace")
    stderr = stderr_bytes.decode("utf-8", errors="replace")
    if (process["timed_out"] or process["exit"] != expected_exit
            or not process["cleanup_complete"] or process["abandoned_pipes"]):
        raise RuntimeError(
            f"{label}: exit {process['exit']} (expected {expected_exit}) "
            f"timed_out={process['timed_out']} "
            f"cleanup_complete={process['cleanup_complete']} "
            f"abandoned_pipes={process['abandoned_pipes']}\n{stdout}{stderr}")
    return subprocess.CompletedProcess(argv, process["exit"], stdout, stderr)


def source_snapshot(verifier_dir: Path) -> dict[str, str]:
    result = {}
    paths = [HERE / "driver.wz", HERE / "probe.wz", HERE / "run.py"]
    paths.extend(sorted(verifier_dir.glob("*.wz")))
    for path in paths:
        try:
            key = str(path.relative_to(ROOT))
        except ValueError:
            key = str(path)
        result[key] = sha256(path)
    return result


def stage_project(work: Path, verifier_dir: Path) -> Path:
    project = work / "project"
    source = project / "src"
    modules = source / "wzcc"
    modules.mkdir(parents=True)
    for package in sorted(SELFHOST.iterdir()):
        if not package.is_dir():
            continue
        if package.name == "hirverify":
            shutil.copytree(verifier_dir, modules / "hirverify")
            shutil.copy2(HERE / "probe.wz",
                         modules / "hirverify" / "expression_probe.wz")
        else:
            (modules / package.name).symlink_to(package,
                                                target_is_directory=True)
    (source / "std").symlink_to(STDLIB, target_is_directory=True)
    shutil.copy2(HERE / "driver.wz", source / "main.wz")
    (project / "lang.toml").write_text(
        "schema_version=1\n"
        "[package]\n"
        'name="hirverify_expression_kinds"\n'
        'source_root="src"\n'
        'entry="app"\n'
        "[build]\n"
        'backend="native"\n'
        "word_bits=64\n", encoding="utf-8")
    return project


def check_report(stdout: str, expected_exit: int) -> dict[str, object]:
    lines = stdout.splitlines()
    check_lines = [line for line in lines if line.startswith("CHECK ")]
    summaries = [line for line in lines if line.startswith("SUMMARY ")]
    if expected_exit == 0:
        expected = [f"CHECK {label} ok" for label in FIXED_CHECKS]
        if check_lines != expected:
            fail(f"fixed report lines differ:\n{check_lines!r}\n"
                 f"expected:\n{expected!r}")
        if summaries != [f"SUMMARY failures=0"]:
            fail(f"fixed summary mismatch: {summaries!r}")
    else:
        expected = [
            f"CHECK {label} {'FAIL' if label in BUGGY_FAILURES else 'ok'}"
            for label in FIXED_CHECKS
        ]
        if check_lines != expected:
            fail(f"baseline report lines differ: {check_lines!r}")
        if summaries != [f"SUMMARY failures={len(BUGGY_FAILURES)}"]:
            fail(f"baseline summary mismatch: {summaries!r}")
    return {"checks": check_lines, "summary": summaries[0]}


def main() -> int:
    args = parse_args()
    woozyc = Path(args.woozyc).expanduser().resolve()
    if not woozyc.is_file():
        fail(f"stage0 compiler missing: {woozyc}")
    verifier_dir = Path(args.verifier_dir).expanduser().resolve() \
        if args.verifier_dir else SELFHOST / "hirverify"
    if not (verifier_dir / "verify.wz").is_file():
        fail(f"hirverify package missing: {verifier_dir}")
    toolchain_root = resolve_toolchain(args, woozyc)
    parent = Path(args.work_dir).expanduser().resolve()
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="hirverify-expression-",
                                 dir=parent)).resolve()
    env = environment(toolchain_root, work)
    print(f"work-dir: {work}", flush=True)

    sources_before = source_snapshot(verifier_dir)
    driver_before = {
        "driver": file_record(HERE / "driver.wz"),
        "probe": file_record(HERE / "probe.wz"),
        "runner": file_record(HERE / "run.py"),
    }
    project = stage_project(work, verifier_dir)
    staged_before = source_snapshot(verifier_dir)

    out_dir = work / "out"
    command([str(woozyc), "build", "--project",
             str(project / "lang.toml"), "--backend", "native", "--opt", "0",
             "--out-dir", str(out_dir), "--format", "json"],
            "build", work, args.timeout, env)
    program = out_dir / "program"
    if not program.is_file():
        fail("build produced no component program")
    program_record = file_record(program)

    result = command([str(program)], "run", work, args.run_timeout, env,
                     args.expect_exit)
    if result.stderr:
        fail(f"unexpected stderr: {result.stderr!r}")
    report = check_report(result.stdout, args.expect_exit)

    staged_after = source_snapshot(verifier_dir)
    if staged_before != staged_after:
        fail("source files changed during the run")
    sources_after = source_snapshot(verifier_dir)
    if sources_before != sources_after:
        fail("source files changed during the run")

    state = {
        "status": "passed",
        "mode": "fixed" if args.expect_exit == 0 else "pre-fix-failure",
        "work_dir": str(work),
        "woozyc": file_record(woozyc),
        "toolchain_root": str(toolchain_root),
        "verifier_dir": str(verifier_dir),
        "timeout_seconds": args.timeout,
        "run_timeout_seconds": args.run_timeout,
        "sources_before": sources_before,
        "sources_after": sources_after,
        "driver_before": driver_before,
        "driver_after": {
            "driver": file_record(HERE / "driver.wz"),
            "probe": file_record(HERE / "probe.wz"),
            "runner": file_record(HERE / "run.py"),
        },
        "program": program_record,
        "report": report,
        "commands": [
            json.loads((work / name).read_text())
            for name in ("build.json", "run.json")
        ],
    }
    (work / "results.json").write_text(json.dumps(state, indent=2) + "\n")
    print(json.dumps({
        "status": state["status"],
        "mode": state["mode"],
        "summary": report["summary"],
        "program_sha256": program_record["sha256"],
        "results": str(work / "results.json"),
    }, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
