#!/usr/bin/env python3
"""Reproduce the HIR verifier per-run lookup-index regression.

The driver loads two real Wz projects through project.load -> resolve ->
sema.check_types, then verifies the published HIR repeatedly in one process,
alternating the two modules.  A cloned module with a missing Source node must
still be rejected.  An optional baseline hirverify directory builds the same
driver against the pre-fix verifier so the two runs can be compared.

This is a focused component test.  It does not rebuild the 116-source compiler
CLI, run the complete corpus, or claim a whole-compiler speedup.  Every command
is bounded and uses the shared corpus process helper.
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

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SELFHOST = ROOT / "selfhost" / "src" / "wzcc"
STDLIB = ROOT / "stdlib"
SMALL_FIXTURE = ROOT / "tests" / "iterator" / "fixtures" / "concrete-counter"
DEFAULT_WOOZYC = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
DEFAULT_WORK = ROOT / "build" / "hirverify-cache-runs"
sys.path.insert(0, str(HERE.parents[2]))
from corpus import _run_process_impl  # noqa: E402

PRODUCTION_FILES = (
    "effectsrc.wz", "flow.wz", "run.wz", "state.wz", "verify.wz",
)
NEW_PROBE = """package hirverify;
import "app/wzcc/model" as model;
pub func cache_probe_compare(left model.NodeRef, right model.NodeRef) bool {
    return source_ref_compare(left, right) == 0;
}
"""
BASELINE_PROBE = """package hirverify;
import "app/wzcc/model" as model;
pub func cache_probe_compare(left model.NodeRef, right model.NodeRef) bool {
    return same_ref(left, right);
}
"""


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
    parser.add_argument("--work-dir", default=str(DEFAULT_WORK))
    parser.add_argument("--baseline-dir", default="",
                        help="pre-fix hirverify package directory")
    parser.add_argument("--new-program", default="",
                        help="reuse an already built fixed-driver program")
    parser.add_argument("--baseline-program", default="",
                        help="reuse an already built baseline-driver program")
    parser.add_argument("--iterations", type=int, default=2,
                        help="verify passes per loaded module")
    parser.add_argument("--repeats", type=int, default=1,
                        help="separate driver process runs")
    parser.add_argument("--opt", type=int, choices=(0, 2), default=0,
                        help="driver build optimization level")
    parser.add_argument("--timeout", type=finite_timeout, default=1200.0,
                        help="per driver build timeout")
    parser.add_argument("--run-timeout", type=finite_timeout, default=300.0,
                        help="per driver execution timeout")
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
            env: dict[str, str]) -> subprocess.CompletedProcess[str]:
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
        "timeout_seconds": timeout,
        "elapsed_seconds": elapsed,
        "exit": process["exit"],
        "timed_out": process["timed_out"],
        "cleanup_complete": process["cleanup_complete"],
        "abandoned_pipes": process["abandoned_pipes"],
        "stdout_sha256": hashlib.sha256(stdout_bytes).hexdigest(),
        "stderr_sha256": hashlib.sha256(stderr_bytes).hexdigest(),
    }
    (work / f"{label}.json").write_text(json.dumps(record, indent=2) + "\n")
    stdout = stdout_bytes.decode("utf-8", errors="replace")
    stderr = stderr_bytes.decode("utf-8", errors="replace")
    if (process["timed_out"] or process["exit"] != 0
            or not process["cleanup_complete"] or process["abandoned_pipes"]):
        raise RuntimeError(
            f"{label}: exit {process['exit']} timed_out={process['timed_out']} "
            f"cleanup_complete={process['cleanup_complete']} "
            f"abandoned_pipes={process['abandoned_pipes']}\n{stdout}{stderr}")
    return subprocess.CompletedProcess(argv, process["exit"], stdout, stderr)


def generate_large_fixture(root: Path, functions: int) -> Path:
    if functions <= 0:
        fail("functions must be positive")
    source = root / "src"
    source.mkdir(parents=True)
    lines = [
        "package app;",
        'import "std/iter" as iter;',
        'import "std/option" as option;',
        "",
        "enum Option[T] { None; Some(value T); }",
        "struct Counter { value i32; limit i32; }",
        "",
        "impl iter.Iterator[i32] for Counter {",
        "    func next(cursor inout Self) option.Option[i32] {",
        "        if cursor.value >= cursor.limit { "
        "return option.Option[i32].None; }",
        "        let item = cursor.value;",
        "        cursor.value = cursor.value + 1;",
        "        return option.Option[i32].Some(item);",
        "    }",
        "}",
        "",
    ]
    for index in range(functions):
        lines.extend([
            f"func use{index}() i32 {{",
            "    let source = Counter(value: 0, limit: 4);",
            "    var sum i32 = 0;",
            "    for item in source { sum += item; }",
            "    return sum - 6;",
            "}",
            "",
        ])
    lines.extend(["func main() i32 {", "    return use0();", "}", ""])
    (source / "main.wz").write_text("\n".join(lines), encoding="utf-8")
    (root / "lang.toml").write_text(
        "schema_version=1\n"
        "[package]\n"
        'name="hirverify_cache_large"\n'
        'source_root="src"\n'
        'entry="app"\n'
        "[build]\n"
        'backend="native"\n'
        "word_bits=64\n", encoding="utf-8")
    return root


def stage_project(work: Path, name: str, baseline: Path | None) -> Path:
    project = work / name / "project"
    source = project / "src"
    modules = source / "wzcc"
    modules.mkdir(parents=True)
    for package in sorted(SELFHOST.iterdir()):
        if not package.is_dir():
            continue
        if package.name == "hirverify":
            source_dir = baseline if baseline is not None else package
            shutil.copytree(source_dir, modules / "hirverify")
            probe = BASELINE_PROBE if baseline is not None else NEW_PROBE
            (modules / "hirverify" / "cache_probe.wz").write_text(
                probe, encoding="utf-8")
        else:
            (modules / package.name).symlink_to(package,
                                                target_is_directory=True)
    (source / "std").symlink_to(STDLIB, target_is_directory=True)
    shutil.copy2(HERE / "driver.wz", source / "main.wz")
    (project / "lang.toml").write_text(
        "schema_version=1\n"
        "[package]\n"
        'name="hirverify_cache_driver"\n'
        'source_root="src"\n'
        'entry="app"\n'
        "[build]\n"
        'backend="native"\n'
        "word_bits=64\n", encoding="utf-8")
    return project


def build_driver(args: argparse.Namespace, project: Path, output: Path,
                 work: Path, env: dict[str, str], label: str) -> Path:
    woozyc = Path(args.woozyc).expanduser().resolve()
    if not woozyc.is_file():
        fail(f"stage0 compiler missing: {woozyc}")
    command([str(woozyc), "build", "--project", str(project / "lang.toml"),
             "--backend", "native", "--opt", str(args.opt),
             "--out-dir", str(output), "--format", "json"],
            label, work, args.timeout, env)
    program = output / "program"
    if not program.is_file():
        fail(f"{label}: no program produced")
    return program


def parse_elapsed(stdout: str) -> int:
    lines = [line for line in stdout.splitlines() if "ELAPSED_NS" in line]
    if len(lines) != 1:
        fail(f"expected exactly one ELAPSED_NS line, got {lines!r}")
    prefix = "ELAPSED_NS "
    if not lines[0].startswith(prefix):
        fail(f"malformed ELAPSED_NS line: {lines[0]!r}")
    value_text = lines[0][len(prefix):]
    if not value_text.isascii() or not value_text.isdigit():
        fail(f"ELAPSED_NS is not a positive integer: {value_text!r}")
    value = int(value_text)
    if value <= 0:
        fail(f"ELAPSED_NS must be positive: {value}")
    return value


def normalize(stdout: str) -> list[str]:
    return [line for line in stdout.splitlines()
            if not line.startswith("ELAPSED_NS ")]


def check_driver_report(stdout: str, small: Path, large: Path,
                        iterations: int) -> None:
    if iterations <= 0:
        fail(f"expected iterations must be positive: {iterations}")
    lines = normalize(stdout)
    text = "\n".join(lines)
    small_line = (f"MODULE {small / 'lang.toml'} "
                  "nodes=105 functions=3 templates=5")
    large_line = (f"MODULE {large / 'lang.toml'} "
                  "nodes=830 functions=42 templates=5")
    required = (
        small_line,
        large_line,
        "BAD_SOURCE rejected=1 failed=1 code=E_IR_INVALID",
        "KEY module-file-node distinct",
    )
    for line in required:
        if line not in lines:
            fail(f"driver report missing {line!r}: {lines!r}")
    module_lines = [line for line in lines if line.startswith("MODULE ")]
    if len(module_lines) != 2:
        fail(f"expected exactly two module lines: {module_lines!r}")
    if lines.count("BAD_SOURCE rejected=1 failed=1 code=E_IR_INVALID") != 2:
        fail(f"missing source was not rejected for both modules: {lines!r}")
    if lines.count("KEY module-file-node distinct") != 1:
        fail(f"expected exactly one key/identity line: {lines!r}")
    summaries = [line for line in lines if line.startswith("ITERATIONS ")]
    if len(summaries) != 1:
        fail(f"expected exactly one ITERATIONS summary: {summaries!r}")
    expected = (f"ITERATIONS {iterations} MODULES 2 "
                f"VERIFIES {iterations * 2} BAD 0")
    if summaries[0] != expected:
        fail(f"unexpected iteration/module/verify summary: {summaries[0]!r}")
    if "REJECTED " in text:
        fail(f"valid module verification failed: {lines!r}")


def snapshot_sources() -> dict[str, str]:
    result = {}
    paths = [HERE / "driver.wz", HERE / "run.py", HERE.parent / "README.md"]
    paths.extend(SELFHOST / "hirverify" / name for name in PRODUCTION_FILES)
    for path in paths:
        result[str(path.relative_to(ROOT))] = sha256(path)
    result[str((SMALL_FIXTURE / "src" / "main.wz").relative_to(ROOT))] = \
        sha256(SMALL_FIXTURE / "src" / "main.wz")
    return result


def main() -> int:
    args = parse_args()
    if args.iterations <= 0 or args.repeats <= 0:
        fail("--iterations and --repeats must be positive")
    woozyc = Path(args.woozyc).expanduser().resolve()
    toolchain_root = resolve_toolchain(args, woozyc)
    parent = Path(args.work_dir).expanduser().resolve()
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="hirverify-cache-", dir=parent)).resolve()
    env = environment(toolchain_root, work)
    print(f"work-dir: {work}", flush=True)
    baseline = Path(args.baseline_dir).expanduser().resolve() \
        if args.baseline_dir else None
    if baseline is not None and not (baseline / "effectsrc.wz").is_file():
        fail(f"baseline hirverify directory missing: {baseline}")
    large = generate_large_fixture(work / "fixtures" / "large", 40)
    small_manifest = (SMALL_FIXTURE / "lang.toml").resolve()
    large_manifest = (large / "lang.toml").resolve()
    state: dict[str, object] = {
        "status": "running",
        "work_dir": str(work),
        "woozyc": file_record(woozyc),
        "toolchain_root": str(toolchain_root),
        "iterations": args.iterations,
        "large_functions": 40,
        "opt": args.opt,
        "timeout_seconds": args.timeout,
        "run_timeout_seconds": args.run_timeout,
        "sources_before": snapshot_sources(),
        "small_fixture": {
            "source": file_record(SMALL_FIXTURE / "src" / "main.wz"),
            "manifest": file_record(SMALL_FIXTURE / "lang.toml"),
        },
        "large_fixture": {
            "source": file_record(large / "src" / "main.wz"),
            "manifest": file_record(large / "lang.toml"),
        },
        "staged_probe": {
            "new": hashlib.sha256(NEW_PROBE.encode()).hexdigest(),
            "baseline": hashlib.sha256(BASELINE_PROBE.encode()).hexdigest(),
        },
    }
    if baseline is not None:
        state["baseline_sources"] = {
            str(path): file_record(path)
            for path in sorted(baseline.glob("*.wz"))}
    try:
        new_program = Path(args.new_program).expanduser().resolve() \
            if args.new_program else None
        baseline_program = Path(args.baseline_program).expanduser().resolve() \
            if args.baseline_program else None
        if new_program is None:
            new_project = stage_project(work, "new", None)
            new_program = build_driver(args, new_project,
                                       work / "new" / f"out-o{args.opt}",
                                       work, env, "new-build")
        if not new_program.is_file():
            fail(f"new driver program missing: {new_program}")
        if baseline is not None and baseline_program is None:
            old_project = stage_project(work, "baseline", baseline)
            baseline_program = build_driver(
                args, old_project, work / "baseline" / f"out-o{args.opt}",
                work, env, "baseline-build")
        if baseline_program is not None and not baseline_program.is_file():
            fail(f"baseline driver program missing: {baseline_program}")
        state["new_program"] = file_record(new_program)
        state["baseline_program"] = (file_record(baseline_program)
                                     if baseline_program is not None else None)
        argv_tail = [str(args.iterations), str(small_manifest), str(STDLIB),
                     str(large_manifest), str(STDLIB)]
        runs = {"new": [], "baseline": []}
        rounds = []
        for repeat in range(args.repeats):
            new_result = command([str(new_program), *argv_tail],
                                 f"new-run-{repeat}", work,
                                 args.run_timeout, env)
            check_driver_report(new_result.stdout, SMALL_FIXTURE, large,
                                args.iterations)
            new_elapsed = parse_elapsed(new_result.stdout)
            runs["new"].append(new_elapsed)
            round_record = {"repeat": repeat, "new_elapsed_ns": new_elapsed}
            if baseline_program is not None:
                old_result = command([str(baseline_program), *argv_tail],
                                     f"baseline-run-{repeat}", work,
                                     args.run_timeout, env)
                check_driver_report(old_result.stdout, SMALL_FIXTURE, large,
                                    args.iterations)
                old_elapsed = parse_elapsed(old_result.stdout)
                runs["baseline"].append(old_elapsed)
                round_record["baseline_elapsed_ns"] = old_elapsed
                equal = normalize(new_result.stdout) ==                     normalize(old_result.stdout)
                round_record["non_timing_output_equal"] = equal
                if not equal:
                    fail(f"repeat {repeat}: fixed and baseline driver reports "
                         "differ beyond ELAPSED_NS")
            rounds.append(round_record)
        state["rounds"] = rounds
        state["elapsed_ns"] = runs
        if runs["new"] and runs["baseline"]:
            new_median = sorted(runs["new"])[len(runs["new"]) // 2]
            old_median = sorted(runs["baseline"])[len(runs["baseline"]) // 2]
            if new_median <= 0 or old_median <= 0:
                fail("elapsed median must be positive")
            state["median_new_ns"] = new_median
            state["median_baseline_ns"] = old_median
            state["observed_speedup"] = old_median / new_median
        state["sources_after"] = snapshot_sources()
        if state["sources_before"] != state["sources_after"]:
            fail("repository sources changed during the run")
        commands = [json.loads(path.read_text())
                    for path in sorted(work.glob("*.json")) if path.is_file()]
        state["commands"] = commands
        state["status"] = "passed"
        (work / "results.json").write_text(json.dumps(state, indent=2) + "\n")
        print(json.dumps({key: state[key] for key in (
            "status", "work_dir", "elapsed_ns", "median_baseline_ns",
            "median_new_ns", "observed_speedup") if key in state}, indent=2))
        return 0
    except Exception as error:  # noqa: BLE001 - retain exact failure evidence
        state["status"] = "failed"
        state["error"] = repr(error)
        try:
            state["commands"] = [json.loads(path.read_text())
                                 for path in sorted(work.glob("*.json"))
                                 if path.is_file()]
        except (OSError, ValueError):
            state["commands"] = []
        (work / "results.json").write_text(json.dumps(state, indent=2) + "\n")
        print(json.dumps({"status": "failed", "work_dir": str(work),
                          "error": repr(error)}, indent=2))
        return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
