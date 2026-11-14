#!/usr/bin/env python3
"""Build and run the private sema NodeKey index component test.

The staged sema package uses the real product state/errors/clone/common/run
files plus all A/B algorithm files except the public check facade, and appends
only this directory's same-package test file.  It does not replace product
logic with a Python model.  The only extra Wz implementation is the explicitly
labelled linear test oracle inside node_index_test.wz.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SELFHOST = ROOT / "selfhost" / "src" / "wzcc"
SEMA = SELFHOST / "sema"
STDLIB = ROOT / "stdlib"
TEST_SRC = HERE / "src"

DEFAULT_STAGE0 = Path(
    "/home/qin/Work/language-dev-20261103/checks/"
    "selfhost-stage123-opt2-const-scratch-20261114.gsscllih/"
    "resources/stage0/woozyc")
FIXED_HELPER = Path(
    "/home/qin/.local/state/cli-agent-bridge/worktrees/"
    "0a0ca4cad6525fc6d10ec8f3/.work/gc-registry-index/build-new/native/"
    "lang_native_helper")
FIXED_NATIVE_RUNTIME = Path(
    "/home/qin/.local/state/cli-agent-bridge/worktrees/"
    "0a0ca4cad6525fc6d10ec8f3/.work/gc-registry-index/build-new/native/"
    "liblang_native_runtime.a")
FIXED_HOSTED_RUNTIME = Path(
    "/home/qin/.local/state/cli-agent-bridge/worktrees/"
    "0a0ca4cad6525fc6d10ec8f3/.work/gc-registry-index/build-new/native/"
    "liblang_hosted_runtime.a")
FIXED_EMBEDDED_STDLIB = Path(
    "/home/qin/.local/state/cli-agent-bridge/worktrees/"
    "0a0ca4cad6525fc6d10ec8f3/stdlib")
FIXED_CLANG = Path(
    "/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/clang")
FIXED_LLD = Path(
    "/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/ld.lld")
FIXED_LLC = Path(
    "/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/llc")
FIXED_OPT = Path(
    "/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/opt")

EXPECTED = {
    DEFAULT_STAGE0: "565da3f6aa0b7f342ac5ba9523b5495643c1e44daaa5ecbb072e5906bd1a1fe4",
    FIXED_HELPER: "3b2e489d88997d7c76ff9ae4827aa560c2547fb1832ebdf7f9427b42f376649e",
    FIXED_NATIVE_RUNTIME: "4df15518f676291522878563e86bb71c99e53d3dad8872f620ec5515acc3a702",
    FIXED_HOSTED_RUNTIME: "7f113be3fab32a62c263a1681ca2b3b34b76a63d6f74f679cce2ac7586a1f75b",
}

SEMA_FILES = tuple(sorted(path.name for path in SEMA.glob("*.wz")
                          if path.name != "check.wz"))
BENCH = re.compile(
    r"BENCH scale=(\d+) product_insert_ns=(-?\d+) "
    r"oracle_insert_ns=(-?\d+) product_lookup_ns=(-?\d+) "
    r"oracle_lookup_ns=(-?\d+) insert_calls=(\d+) lookup_calls=(\d+) "
    r"oracle_insert_comparisons=(\d+) oracle_lookup_comparisons=(\d+) "
    r"product_insert_sum=(\d+) oracle_insert_sum=(\d+) "
    r"product_lookup_sum=(\d+) oracle_lookup_sum=(\d+) "
    r"product_missing=(\d+) oracle_missing=(\d+)")
SUMMARY = re.compile(r"NODE_INDEX passed=(\d+) failed=(\d+)\s*")

sys.path.insert(0, str(ROOT / "selfhost"))
from corpus import _run_process_impl  # noqa: E402


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tree_hashes(root: Path, suffixes: tuple[str, ...] | None = None) -> dict[str, str]:
    result: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file() or "__pycache__" in path.parts:
            continue
        if suffixes is not None and path.suffix not in suffixes:
            continue
        result[str(path.relative_to(root))] = sha256(path)
    return result


def changed(before: dict[str, str], after: dict[str, str]) -> list[str]:
    return [name for name in sorted(set(before) | set(after))
            if before.get(name) != after.get(name)]


def fixed_record(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise RuntimeError(f"fixed input missing: {path}")
    return {"path": str(path), "resolved": str(path.resolve()),
            "sha256": sha256(path)}


def verify_fixed(stage0: Path) -> dict[str, dict[str, str]]:
    if sha256(stage0) != EXPECTED[DEFAULT_STAGE0]:
        raise RuntimeError(f"stage0 identity differs: {stage0}")
    record = {"stage0": fixed_record(stage0)}
    for name, path in (("helper", FIXED_HELPER),
                       ("native_runtime", FIXED_NATIVE_RUNTIME),
                       ("hosted_runtime", FIXED_HOSTED_RUNTIME),
                       ("clang", FIXED_CLANG),
                       ("ld_lld", FIXED_LLD),
                       ("llc", FIXED_LLC),
                       ("opt", FIXED_OPT)):
        item = fixed_record(path)
        if path in EXPECTED and item["sha256"] != EXPECTED[path]:
            raise RuntimeError(f"fixed input identity differs: {path}")
        record[name] = item
    embedded = tree_hashes(FIXED_EMBEDDED_STDLIB, (".wz",))
    current = tree_hashes(STDLIB, (".wz",))
    if embedded != current:
        raise RuntimeError("embedded stage0 stdlib differs from worktree stdlib")
    record["embedded_stdlib"] = {
        "path": str(FIXED_EMBEDDED_STDLIB),
        "files": len(embedded),
        "sha256": hashlib.sha256(
            json.dumps(embedded, sort_keys=True,
                       separators=(",", ":")).encode()).hexdigest(),
    }
    record["worktree_stdlib"] = {
        "path": str(STDLIB),
        "files": len(current),
        "sha256": hashlib.sha256(
            json.dumps(current, sort_keys=True,
                       separators=(",", ":")).encode()).hexdigest(),
    }
    return record


def stage_project(work: Path) -> Path:
    project = work / "project"
    source_root = project / "src"
    sema = source_root / "wzcc" / "sema"
    sema.mkdir(parents=True)
    for name in SEMA_FILES:
        shutil.copy2(SEMA / name, sema / name)
    shutil.copy2(HERE / "node_index_test.wz", sema / "node_index_test.wz")
    for module in sorted(SELFHOST.iterdir()):
        if module.is_dir() and module.name != "sema":
            (source_root / "wzcc" / module.name).symlink_to(
                module, target_is_directory=True)
    (source_root / "std").symlink_to(STDLIB, target_is_directory=True)
    shutil.copy2(TEST_SRC / "main.wz", source_root / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def environment(work: Path) -> dict[str, str]:
    env = dict(os.environ)
    llvm = Path("/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8")
    libxml = Path(
        "/home/qin/Work/language-dev-20261103/toolchains/"
        "libxml2-legacy-2.13.9/usr/lib")
    env["LD_LIBRARY_PATH"] = f"{llvm / 'lib'}:{libxml}"
    env["TMPDIR"] = str(work / "tmp")
    env["LC_ALL"] = "C"
    env["LANG"] = "C.UTF-8"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PATH"] = f"{llvm / 'bin'}:/usr/bin:/bin"
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


def command(argv: list[str], label: str, cwd: Path, timeout: float,
            env: dict[str, str]) -> tuple[dict[str, object], bytes, bytes]:
    print("+", " ".join(argv), flush=True)
    started = time.monotonic()
    process = _run_process_impl(argv, cwd, env, timeout,
                                stdin="/dev/null")
    elapsed = time.monotonic() - started
    stdout = process["stdout"]
    stderr = process["stderr"]
    (cwd / f"{label}.stdout").write_bytes(stdout)
    (cwd / f"{label}.stderr").write_bytes(stderr)
    record = {
        "label": label,
        "argv": [str(part) for part in argv],
        "cwd": str(cwd),
        "timeout_seconds": timeout,
        "elapsed_seconds": elapsed,
        "exit": process["exit"],
        "timed_out": process["timed_out"],
        "cleanup_complete": process["cleanup_complete"],
        "abandoned_pipes": process["abandoned_pipes"],
        "stdout_path": f"{label}.stdout",
        "stderr_path": f"{label}.stderr",
        "stdout_sha256": hashlib.sha256(stdout).hexdigest(),
        "stderr_sha256": hashlib.sha256(stderr).hexdigest(),
    }
    (cwd / f"{label}.json").write_text(
        json.dumps(record, indent=2) + "\n", encoding="utf-8")
    if (process["timed_out"] or process["exit"] != 0
            or not process["cleanup_complete"]
            or process["abandoned_pipes"]):
        raise RuntimeError(
            f"{label}: exit {process['exit']} "
            f"timed_out={process['timed_out']} "
            f"cleanup_complete={process['cleanup_complete']} "
            f"abandoned_pipes={process['abandoned_pipes']}\n"
            f"{stdout.decode('utf-8', errors='replace')}"
            f"{stderr.decode('utf-8', errors='replace')}")
    return record, stdout, stderr


def cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0),
                        help="fixed stage-0 compiler")
    parser.add_argument("--work-dir",
                        help="parent directory for retained run artifacts")
    parser.add_argument("--timeout", type=finite_timeout, default=900.0,
                        help="per-command timeout in seconds")
    parser.add_argument("--small-scale", type=int, default=4096)
    parser.add_argument("--large-scale", type=int, default=16384)
    args = parser.parse_args()
    if args.small_scale <= 0 or args.large_scale <= args.small_scale:
        parser.error("scales must be positive and increasing")
    stage0 = Path(args.woozyc).expanduser().resolve()
    if args.work_dir:
        parent = Path(args.work_dir).expanduser().resolve()
    else:
        parent = ROOT / "build" / "sema-node-index-runs"
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="sema-node-index-", dir=parent))
    (work / "tmp").mkdir()

    fixed = verify_fixed(stage0)
    project = stage_project(work)
    env = environment(work)
    manifest = str(project / "lang.toml")
    print(f"work-dir: {work}", flush=True)

    source_before = tree_hashes(ROOT / "selfhost" / "src", (".wz",))
    test_before = tree_hashes(HERE)
    stdlib_before = tree_hashes(STDLIB, (".wz",))
    staged_sema = tree_hashes(project / "src" / "wzcc" / "sema")
    for name in SEMA_FILES:
        if staged_sema[name] != source_before[f"wzcc/sema/{name}"]:
            raise RuntimeError(f"staged product file differs from Source: {name}")
    if staged_sema["node_index_test.wz"] != sha256(HERE / "node_index_test.wz"):
        raise RuntimeError("staged test file differs from test entry")
    for name in ("main.wz",):
        if sha256(TEST_SRC / name) != sha256(project / "src" / name):
            raise RuntimeError(f"staged test entry differs: {name}")

    syntax: list[dict[str, object]] = []
    for label, path in (("syntax-test", project / "src" / "wzcc" / "sema" /
                         "node_index_test.wz"),
                        ("syntax-main", project / "src" / "main.wz")):
        record, _, _ = command(
            [str(stage0), "check", "--syntax-only", str(path)],
            label, work, min(args.timeout, 120.0), env)
        syntax.append(record)

    check_record, check_stdout, _ = command(
        [str(stage0), "check", "--project", manifest, "--backend", "native",
         "--format", "json"],
        "check", work, args.timeout, env)
    try:
        check_report = json.loads(check_stdout.decode("utf-8"))
    except ValueError as error:
        raise RuntimeError(
            f"check returned invalid JSON: {check_stdout!r}") from error
    if check_report.get("status") != "ok":
        raise RuntimeError(f"check did not succeed: {check_stdout!r}")

    out_dir = work / "out-o2"
    build_record, build_stdout, _ = command(
        [str(stage0), "build", "--project", manifest, "--backend", "native",
         "--opt", "2", "--out-dir", str(out_dir), "--format", "json"],
        "build-o2", work, args.timeout, env)
    try:
        build_report = json.loads(build_stdout.decode("utf-8"))
    except ValueError as error:
        raise RuntimeError(f"build returned invalid JSON: {build_stdout!r}") from error
    if build_report.get("status") != "ok":
        raise RuntimeError(f"build did not succeed: {build_stdout!r}")

    program = out_dir / "program"
    run_record, run_stdout, run_stderr = command(
        [str(program), str(args.small_scale), str(args.large_scale)],
        "run", work, args.timeout, env)
    text = run_stdout.decode("utf-8", errors="replace")
    summary_lines = [line for line in text.splitlines()
                     if line.startswith("NODE_INDEX")]
    summary = SUMMARY.fullmatch(summary_lines[0]) if len(summary_lines) == 1 else None
    if (summary is None or tuple(map(int, summary.groups())) != (315, 0)
            or re.search(r"(?m)^\s*FAIL(?:\b|:)", text)):
        raise RuntimeError(f"test summary failed: {text!r}")
    passed, failed = map(int, summary.groups())
    benches: list[dict[str, object]] = []
    for line in text.splitlines():
        match = BENCH.fullmatch(line)
        if match is None:
            continue
        values = list(map(int, match.groups()))
        bench = {
            "scale": values[0],
            "product_insert_ns": values[1],
            "oracle_insert_ns": values[2],
            "product_lookup_ns": values[3],
            "oracle_lookup_ns": values[4],
            "insert_calls": values[5],
            "lookup_calls": values[6],
            "oracle_insert_comparisons": values[7],
            "oracle_lookup_comparisons": values[8],
            "product_insert_sum": values[9],
            "oracle_insert_sum": values[10],
            "product_lookup_sum": values[11],
            "oracle_lookup_sum": values[12],
            "product_missing": values[13],
            "oracle_missing": values[14],
        }
        if bench["product_insert_ns"] > 0:
            bench["insert_oracle_over_product"] = (
                bench["oracle_insert_ns"] / bench["product_insert_ns"])
        if bench["product_lookup_ns"] > 0:
            bench["lookup_oracle_over_product"] = (
                bench["oracle_lookup_ns"] / bench["product_lookup_ns"])
        benches.append(bench)
    expected_scales = [args.small_scale, args.large_scale]
    if [bench["scale"] for bench in benches] != expected_scales:
        raise RuntimeError(f"bench scales differ: {benches!r}")
    for bench in benches:
        if any(bench[name] <= 0 for name in (
                "product_insert_ns", "oracle_insert_ns",
                "product_lookup_ns", "oracle_lookup_ns")):
            raise RuntimeError(f"bench elapsed time must be positive: {bench!r}")
        if bench["product_insert_sum"] != bench["oracle_insert_sum"]:
            raise RuntimeError(f"insert checksum differs: {bench!r}")
        if bench["product_lookup_sum"] != bench["oracle_lookup_sum"]:
            raise RuntimeError(f"lookup checksum differs: {bench!r}")
        if bench["product_missing"] != 0 or bench["oracle_missing"] != 0:
            raise RuntimeError(f"bench lookup missing: {bench!r}")
        if bench["insert_calls"] != bench["scale"]:
            raise RuntimeError(f"insert call count differs: {bench!r}")
        if bench["lookup_calls"] != bench["scale"] * 2:
            raise RuntimeError(f"lookup call count differs: {bench!r}")

    source_after = tree_hashes(ROOT / "selfhost" / "src", (".wz",))
    test_after = tree_hashes(HERE)
    stdlib_after = tree_hashes(STDLIB, (".wz",))
    changed_sources = changed(source_before, source_after)
    changed_tests = changed(test_before, test_after)
    changed_stdlib = changed(stdlib_before, stdlib_after)
    if changed_sources or changed_tests or changed_stdlib:
        raise RuntimeError(
            f"inputs changed: source={changed_sources} "
            f"test={changed_tests} stdlib={changed_stdlib}")

    generated = {}
    for path in (program, out_dir / "program.o"):
        if path.is_file():
            generated[str(path)] = {"sha256": sha256(path),
                                    "size": path.stat().st_size}
    result = {
        "operation_id": "SH15-sema-node-index-lower-bound-20261114",
        "status": "passed",
        "work_dir": str(work),
        "product_source": {
            "files": len(source_before),
            "sha256": hashlib.sha256(json.dumps(
                source_before, sort_keys=True,
                separators=(",", ":")).encode()).hexdigest(),
            "changed_during_run": changed_sources,
            "three_files_before": {
                name: source_before[f"wzcc/sema/{name}"]
                for name in ("common.wz", "run.wz", "b_call.wz")},
        },
        "test_source": {
            "files": len(test_before),
            "sha256": hashlib.sha256(json.dumps(
                test_before, sort_keys=True,
                separators=(",", ":")).encode()).hexdigest(),
            "changed_during_run": changed_tests,
        },
        "stdlib": {
            "files": len(stdlib_before),
            "sha256": hashlib.sha256(json.dumps(
                stdlib_before, sort_keys=True,
                separators=(",", ":")).encode()).hexdigest(),
            "changed_during_run": changed_stdlib,
        },
        "fixed_inputs": fixed,
        "staged_sema": staged_sema,
        "syntax": syntax,
        "check": check_record,
        "build": build_record,
        "run": run_record,
        "correctness": {"passed": passed, "failed": failed},
        "bench": benches,
        "generated": generated,
        "environment": {
            "python": sys.version,
            "python_executable": sys.executable,
            "platform": platform.platform(),
            "machine": platform.machine(),
            "cpu_model": cpu_model(),
            "opt": 2,
            "backend": "native",
            "scales": [args.small_scale, args.large_scale],
        },
        "notes": [
            "bench insertion and oracle insertion use the same ascending "
            "keys and ids; product lookup calls definition and "
            "b_occurrence_raw twice per key, matching two oracle linear gets",
            "oracle comparison counts are measured in the test oracle; "
            "product comparison counts are not instrumented",
            "the benchmark uses ascending insertion only; reverse/interleaved "
            "suffix moves remain and are checked for correctness in the test",
            "this is a sema component, not a full CLI or self-hosting claim",
        ],
    }
    (work / "result.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    (work / "inputs.before.json").write_text(json.dumps({
        "source": source_before, "test": test_before,
        "stdlib": stdlib_before, "fixed": fixed,
        "staged_sema": staged_sema,
    }, indent=2) + "\n", encoding="utf-8")
    (work / "inputs.after.json").write_text(json.dumps({
        "source": source_after, "test": test_after,
        "stdlib": stdlib_after,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: NODE_INDEX {passed}/{passed} at O2", flush=True)
    for bench in benches:
        print(
            f"PASS: scale={bench['scale']} "
            f"product_insert_ns={bench['product_insert_ns']} "
            f"oracle_insert_ns={bench['oracle_insert_ns']} "
            f"product_lookup_ns={bench['product_lookup_ns']} "
            f"oracle_lookup_ns={bench['oracle_lookup_ns']}", flush=True)
    print(f"result: {work / 'result.json'}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
