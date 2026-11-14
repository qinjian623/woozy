#!/usr/bin/env python3
"""Build and run the private HIR needs_drop bool-cache component test.

The runner stages the real selfhost/src/wzcc package tree, appends only this
directory's same-package probe to a private hirverify copy, and builds one
native O2 component with the fixed stage-0 compiler.  It does not build the
116-source CLI, run the complete corpus, or replace product logic with Python.
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
ROOT = HERE.parents[4]
SELFHOST = ROOT / "selfhost" / "src" / "wzcc"
STDLIB = ROOT / "stdlib"
CORPUS = ROOT / "selfhost" / "corpus.py"
SMALL_FIXTURE = ROOT / "tests" / "iterator" / "fixtures" / "concrete-counter"
DEFAULT_WORK = Path(
    "/home/qin/Work/language-dev-20261103/checks/"
    "SH15-hir-needs-drop-cache-20261114")

BASE_HEAD = "5bedfabb8a369afdf778180ce02e60d28ee8573a"
BASE_SOURCE116_MAP = (
    "ed1c65fb9889606a713f03cc4e02d7bd6d818c58e4937610554cdd3f3e866a09")
CURRENT_SOURCE116_MAP = (
    "0c46f7ea6ad2487f574f91147bca062708ef2cdefd0b76fdc1d068eb8a5bcd76")

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
FIXED_LLVM = Path(
    "/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8")
FIXED_CLANG = FIXED_LLVM / "bin" / "clang"
FIXED_LLD = FIXED_LLVM / "bin" / "ld.lld"
FIXED_LLC = FIXED_LLVM / "bin" / "llc"
FIXED_OPT = FIXED_LLVM / "bin" / "opt"
FIXED_STRACE = Path(
    "/home/qin/Work/language-dev-20261103/toolchains/strace-7.2/usr/bin/strace")
FIXED_LIBXML = Path(
    "/home/qin/Work/language-dev-20261103/toolchains/"
    "libxml2-legacy-2.13.9/usr/lib/libxml2.so.2.13.9")

EXPECTED = {
    FIXED_HELPER: "3b2e489d88997d7c76ff9ae4827aa560c2547fb1832ebdf7f9427b42f376649e",
    FIXED_NATIVE_RUNTIME: "4df15518f676291522878563e86bb71c99e53d3dad8872f620ec5515acc3a702",
    FIXED_HOSTED_RUNTIME: "7f113be3fab32a62c263a1681ca2b3b34b76a63d6f74f679cce2ac7586a1f75b",
    FIXED_CLANG: "31a40dc31f3c15a47aa119aa148f339bf363d8f61202ddbdacbd3f24b71ba113",
    FIXED_LLD: "a37ea59e863b4c1fd95ead6a905f7ddb461f7d90d4f30d2d76bcbb2ae9588b1d",
    FIXED_LLC: "75312287569eb638c8540a8282b5e611cd438de694ebc9e116921bdc252fdc21",
    FIXED_OPT: "fc19bcd0ab81c35746c93aad355e8bb81f34ab070e98014cf06ce8433e91d4b1",
    FIXED_STRACE: "28ed5493ecf611b83ac0d0398cc1c4f09a228afaa7fce4737d14c8599cc796e2",
    FIXED_LIBXML: "7e05bdfecc5871ebb5628e501e94e7ea51af592dc8f285decb679177c45dda7f",
}
STDLIB_MAP = "473aa54b5ce5c1bbae4985d0d187528aa056a72ddc1737c3f05d3abb9623bd47"
STAGE0_SHA = "565da3f6aa0b7f342ac5ba9523b5495643c1e44daaa5ecbb072e5906bd1a1fe4"

PRODUCTION_BASE = {
    "selfhost/src/wzcc/hirverify/verify.wz":
        "e72b17eff36a93d6e1d001cb8b7c24951e34b92078ee5af3fd461b00b590795d",
    "selfhost/src/wzcc/hirverify/types.wz":
        "19ea6a41b402b186abb5829ba5f6a920bce7eb482242851c515af56a36b01de1",
    "selfhost/src/wzcc/hirverify/flow.wz":
        "cbc320285d90ca740369fefe49ef448eb6d16ac4ee1d68f7a97fe4e3d5b404c1",
    "selfhost/src/wzcc/hirverify/values.wz":
        "872d25a3de829294e747dc48b8de44e76719efb0c97a0e113c61632c4921bb2c",
    "selfhost/src/wzcc/hirverify/instance.wz":
        "2d26326731974aa64f92aabd1602dbe1ecab88e14cc85e33925214b809535a07",
    "selfhost/src/wzcc/hirverify/effectsrc.wz":
        "f7e56b1ed2b3e2d8417b36dbae31f101c64547318d3196f06b36cb4dddf5535c",
}

BENCH = re.compile(
    r"BENCH trial=(\d+) cached_ns=(-?\d+) uncached_ns=(-?\d+) "
    r"queries=(\d+) cached_sum=(\d+) uncached_sum=(\d+) "
    r"mismatches=(\d+)")
CACHE_SUMMARY = re.compile(r"CACHE_PROBE passed=(\d+) failed=(\d+)")
NEEDS_SUMMARY = re.compile(r"NEEDS_DROP passed=(\d+) failed=(\d+)")
ITER_SUMMARY = re.compile(
    r"ITERATIONS (\d+) MODULES (\d+) VERIFIES (\d+) BAD (\d+)")
ELAPSED = re.compile(r"ELAPSED_NS (\d+)")

sys.path.insert(0, str(ROOT / "selfhost"))
from corpus import (CorpusError, _reject_json_constant, _run_process_impl,
                    audit_trace)  # noqa: E402


def fail(message: str) -> None:
    raise RuntimeError(message)


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


def map_sha(values: dict[str, str]) -> str:
    return hashlib.sha256(json.dumps(
        values, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def file_record(path: Path) -> dict[str, object]:
    if not path.is_file():
        fail(f"fixed input missing: {path}")
    return {"path": str(path), "resolved": str(path.resolve()),
            "size": path.stat().st_size, "sha256": sha256(path)}


def checked_file(path: Path, expected: str | None = None) -> dict[str, object]:
    record = file_record(path)
    if expected is not None and record["sha256"] != expected:
        fail(f"fixed input identity differs: {path}")
    return record


def verify_fixed(stage0: Path) -> dict[str, object]:
    record: dict[str, object] = {
        "stage0": checked_file(stage0, STAGE0_SHA),
    }
    for name, path in (("helper", FIXED_HELPER),
                       ("native_runtime", FIXED_NATIVE_RUNTIME),
                       ("hosted_runtime", FIXED_HOSTED_RUNTIME),
                       ("clang", FIXED_CLANG),
                       ("ld_lld", FIXED_LLD),
                       ("llc", FIXED_LLC),
                       ("opt", FIXED_OPT),
                       ("strace", FIXED_STRACE),
                       ("libxml2_legacy", FIXED_LIBXML)):
        record[name] = checked_file(path, EXPECTED.get(path))
    embedded = tree_hashes(FIXED_EMBEDDED_STDLIB, (".wz",))
    current = tree_hashes(STDLIB, (".wz",))
    if map_sha(embedded) != STDLIB_MAP or map_sha(current) != STDLIB_MAP:
        fail("stdlib identity differs from the fixed map")
    record["embedded_stdlib"] = {
        "path": str(FIXED_EMBEDDED_STDLIB),
        "files": len(embedded),
        "sha256": map_sha(embedded),
    }
    record["worktree_stdlib"] = {
        "path": str(STDLIB),
        "files": len(current),
        "sha256": map_sha(current),
    }
    python_path = Path(sys.executable).resolve()
    python_record = checked_file(python_path)
    python_record["version"] = platform.python_version()
    if platform.python_version() != "3.12.15":
        fail(f"unexpected Python version: {platform.python_version()}")
    record["python"] = python_record
    record["corpus"] = checked_file(CORPUS)
    record["runner"] = checked_file(Path(__file__).resolve())
    return record


def source_snapshot() -> dict[str, object]:
    files = tree_hashes(ROOT / "selfhost" / "src", (".wz",))
    if len(files) != 116:
        fail(f"expected 116 product Source files, got {len(files)}")
    return {"files": files, "module_list": sorted(files),
            "map_sha256": map_sha(files)}


def test_snapshot() -> dict[str, object]:
    files = tree_hashes(HERE)
    return {"files": files, "map_sha256": map_sha(files)}


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
        'name="hirverify_needs_drop_cache_large"\n'
        'source_root="src"\n'
        'entry="app"\n'
        "[build]\n"
        'backend="native"\n'
        "word_bits=64\n", encoding="utf-8")
    return root


def stage_project(work: Path) -> Path:
    project = work / "project"
    source = project / "src"
    modules = source / "wzcc"
    modules.mkdir(parents=True)
    shutil.copytree(SELFHOST / "hirverify", modules / "hirverify")
    shutil.copy2(HERE / "needs_drop_probe.wz",
                 modules / "hirverify" / "needs_drop_probe.wz")
    for package in sorted(SELFHOST.iterdir()):
        if package.is_dir() and package.name != "hirverify":
            (modules / package.name).symlink_to(package,
                                                target_is_directory=True)
    (source / "std").symlink_to(STDLIB, target_is_directory=True)
    shutil.copy2(HERE / "driver.wz", source / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def environment(work: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = f"{FIXED_LLVM / 'lib'}:{FIXED_LIBXML.parent}"
    env["TMPDIR"] = str(work / "tmp")
    env["LC_ALL"] = "C"
    env["LANG"] = "C.UTF-8"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PATH"] = f"{FIXED_LLVM / 'bin'}:/usr/bin:/bin"
    (work / "tmp").mkdir(parents=True, exist_ok=True)
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
    if not math.isfinite(timeout) or timeout <= 0:
        fail(f"{label}: timeout must be finite and positive")
    print("+", " ".join(str(part) for part in argv), flush=True)
    started = time.monotonic()
    process = _run_process_impl(argv, cwd, env, timeout, stdin="/dev/null")
    elapsed = time.monotonic() - started
    stdout = process["stdout"]
    stderr = process["stderr"]
    (cwd / f"{label}.stdout").write_bytes(stdout)
    (cwd / f"{label}.stderr").write_bytes(stderr)
    record: dict[str, object] = {
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
    if type(record["exit"]) is not int:
        fail(f"{label}: exit is not a native integer: {record['exit']!r}")
    for field in ("timed_out", "cleanup_complete", "abandoned_pipes"):
        if type(record[field]) is not bool:
            fail(f"{label}: {field} is not a strict bool: {record[field]!r}")
    if not math.isfinite(record["elapsed_seconds"]):
        fail(f"{label}: elapsed is not finite")
    (cwd / f"{label}.json").write_text(
        json.dumps(record, indent=2, allow_nan=False) + "\n",
        encoding="utf-8")
    if (record["timed_out"] or record["exit"] != 0
            or not record["cleanup_complete"]
            or record["abandoned_pipes"]):
        raise RuntimeError(
            f"{label}: exit {record['exit']} "
            f"timed_out={record['timed_out']} "
            f"cleanup_complete={record['cleanup_complete']} "
            f"abandoned_pipes={record['abandoned_pipes']}\n"
            f"{stdout.decode('utf-8', errors='replace')}"
            f"{stderr.decode('utf-8', errors='replace')}")
    return record, stdout, stderr


def require_clean_streams(label: str, stdout: bytes, stderr: bytes,
                          allow_stderr: bool = False) -> None:
    if not stdout:
        fail(f"{label}: stdout is empty")
    if stderr and not allow_stderr:
        fail(f"{label}: stderr is not empty: {stderr!r}")


def parse_json_report(label: str, stdout: bytes) -> dict[str, object]:
    if not stdout:
        fail(f"{label}: stdout is empty")
    try:
        report = json.loads(stdout.decode("utf-8"),
                            parse_constant=_reject_json_constant)
    except (UnicodeDecodeError, ValueError, CorpusError) as error:
        fail(f"{label}: invalid JSON output: {error}")
    if not isinstance(report, dict) or report.get("status") != "ok":
        fail(f"{label}: status is not ok: {stdout!r}")
    if report.get("diagnostics") not in ([], None):
        fail(f"{label}: diagnostics are not empty: {report!r}")
    return report


def parse_output(text: str, iterations: int, modules: int,
                 small_manifest: Path, large_manifest: Path,
                 require_large: bool) -> dict[str, object]:
    lines = text.splitlines()
    failures = [line for line in lines if line.startswith("FAIL")]
    rejected = [line for line in lines if line.startswith("REJECTED")]
    if failures or rejected:
        fail(f"program reported failures: {failures + rejected!r}")

    module_lines = [line for line in lines if line.startswith("MODULE ")]
    if len(module_lines) != modules:
        fail(f"expected {modules} module lines, got {module_lines!r}")
    small_line = (f"MODULE {small_manifest} "
                  "nodes=105 functions=3 templates=5")
    large_line = (f"MODULE {large_manifest} "
                  "nodes=830 functions=42 templates=5")
    if small_line not in module_lines:
        fail(f"small module line missing: {module_lines!r}")
    if require_large and large_line not in module_lines:
        fail(f"large module line missing: {module_lines!r}")

    key_lines = [line for line in lines
                 if line == "KEY module-file-node distinct"]
    if len(key_lines) != 1:
        fail(f"expected one key line, got {key_lines!r}")
    bad_lines = [line for line in lines
                 if line == "BAD_SOURCE rejected=1 failed=1 code=E_IR_INVALID"]
    if len(bad_lines) != modules:
        fail(f"expected {modules} source rejections, got {bad_lines!r}")
    for kind in ("type", "scope"):
        expected = (f"TAMPER kind={kind} rejected=1 failed=1 "
                    "code=E_IR_INVALID")
        found = [line for line in lines if line == expected]
        if len(found) != modules:
            fail(f"expected {modules} {kind} tamper rejections, got {found!r}")
    warm_lines = [line for line in lines if line == "WARM verified=2"]
    if len(warm_lines) != modules:
        fail(f"expected {modules} warm lines, got {warm_lines!r}")

    cache_lines = [line for line in lines if line.startswith("CACHE_PROBE ")]
    if len(cache_lines) != 1:
        fail(f"expected one cache summary, got {cache_lines!r}")
    cache_match = CACHE_SUMMARY.fullmatch(cache_lines[0])
    if cache_match is None:
        fail(f"malformed cache summary: {cache_lines[0]!r}")
    cache_passed, cache_failed = map(int, cache_match.groups())
    if cache_failed != 0 or cache_passed <= 0:
        fail(f"cache probe failed: {cache_lines[0]!r}")

    needs_lines = [line for line in lines if line.startswith("NEEDS_DROP ")]
    if len(needs_lines) != 1:
        fail(f"expected one needs_drop summary, got {needs_lines!r}")
    needs_match = NEEDS_SUMMARY.fullmatch(needs_lines[0])
    if needs_match is None:
        fail(f"malformed needs_drop summary: {needs_lines[0]!r}")
    needs_passed, needs_failed = map(int, needs_match.groups())
    if needs_failed != 0 or needs_passed <= 0:
        fail(f"component failed: {needs_lines[0]!r}")

    iter_lines = [line for line in lines if line.startswith("ITERATIONS ")]
    if len(iter_lines) != 1:
        fail(f"expected one iteration summary, got {iter_lines!r}")
    iter_match = ITER_SUMMARY.fullmatch(iter_lines[0])
    if iter_match is None:
        fail(f"malformed iteration summary: {iter_lines[0]!r}")
    observed = tuple(map(int, iter_match.groups()))
    expected = (iterations, modules, iterations * modules, 0)
    if observed != expected:
        fail(f"iteration summary differs: {observed!r} != {expected!r}")

    elapsed_lines = [line for line in lines if line.startswith("ELAPSED_NS ")]
    if len(elapsed_lines) != 1:
        fail(f"expected one elapsed line, got {elapsed_lines!r}")
    elapsed_match = ELAPSED.fullmatch(elapsed_lines[0])
    if elapsed_match is None or int(elapsed_match.group(1)) <= 0:
        fail(f"elapsed is not positive: {elapsed_lines[0]!r}")

    bench_lines = [line for line in lines if line.startswith("BENCH ")]
    if len(bench_lines) != 3:
        fail(f"expected three benchmark lines, got {bench_lines!r}")
    benches = []
    for expected_trial, line in enumerate(bench_lines):
        match = BENCH.fullmatch(line)
        if match is None:
            fail(f"malformed benchmark line: {line!r}")
        values = list(map(int, match.groups()))
        bench = {
            "trial": values[0],
            "cached_ns": values[1],
            "uncached_ns": values[2],
            "queries": values[3],
            "cached_sum": values[4],
            "uncached_sum": values[5],
            "mismatches": values[6],
        }
        if bench["trial"] != expected_trial:
            fail(f"benchmark trial order differs: {bench!r}")
        if bench["queries"] != 32 * 64:
            fail(f"benchmark query count differs: {bench!r}")
        if bench["cached_ns"] <= 0 or bench["uncached_ns"] <= 0:
            fail(f"benchmark elapsed is not positive: {bench!r}")
        if bench["mismatches"] != 0:
            fail(f"benchmark results differ: {bench!r}")
        if bench["cached_sum"] != bench["uncached_sum"]:
            fail(f"benchmark checksums differ: {bench!r}")
        if bench["cached_sum"] <= 0:
            fail(f"benchmark checksum is not positive: {bench!r}")
        benches.append(bench)

    return {
        "module_lines": module_lines,
        "cache_passed": cache_passed,
        "cache_failed": cache_failed,
        "needs_passed": needs_passed,
        "needs_failed": needs_failed,
        "iterations": observed,
        "elapsed_ns": int(elapsed_match.group(1)),
        "benches": benches,
    }


def parse_exec_log(path: Path, program: str) -> list[str]:
    return audit_trace(path, {str(Path(program).resolve())})["observed"]


def frozen_record(stage0: Path, large: Path, project: Path,
                  source: dict[str, object], tests: dict[str, object],
                  fixed: dict[str, object]) -> dict[str, object]:
    staged_hirverify = tree_hashes(project / "src" / "wzcc" / "hirverify")
    source_files = source["files"]
    for name, digest in staged_hirverify.items():
        if name == "needs_drop_probe.wz":
            if digest != sha256(HERE / "needs_drop_probe.wz"):
                fail("staged probe differs from its test source")
        elif source_files.get(f"wzcc/hirverify/{name}") != digest:
            fail(f"staged product file differs from Source: {name}")
    if sha256(project / "src" / "main.wz") != sha256(HERE / "driver.wz"):
        fail("staged driver differs from its test source")
    if sha256(project / "lang.toml") != sha256(HERE / "lang.toml"):
        fail("staged manifest differs from its test source")
    return {
        "source116": source,
        "test": tests,
        "staged_hirverify": staged_hirverify,
        "staged_main": file_record(project / "src" / "main.wz"),
        "staged_manifest": file_record(project / "lang.toml"),
        "small_fixture": {
            "source": file_record(SMALL_FIXTURE / "src" / "main.wz"),
            "manifest": file_record(SMALL_FIXTURE / "lang.toml"),
        },
        "large_fixture": {
            "source": file_record(large / "src" / "main.wz"),
            "manifest": file_record(large / "lang.toml"),
        },
        "fixed": fixed,
        "base": {
            "head": BASE_HEAD,
            "source116_map_sha256": BASE_SOURCE116_MAP,
            "production_files_before_changes": PRODUCTION_BASE,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0),
                        help="fixed stage-0 compiler")
    parser.add_argument("--work-dir", default=str(DEFAULT_WORK),
                        help="parent directory for retained run artifacts")
    parser.add_argument("--timeout", type=finite_timeout, default=1800.0,
                        help="check/build timeout in seconds")
    parser.add_argument("--run-timeout", type=finite_timeout, default=300.0,
                        help="per program execution timeout in seconds")
    parser.add_argument("--iterations", type=int, default=2,
                        help="alternating real-Source verify passes")
    parser.add_argument("--expected-source-map", default=CURRENT_SOURCE116_MAP,
                        help="expected current product Source116 map SHA")
    args = parser.parse_args()
    if args.iterations <= 0:
        parser.error("--iterations must be positive")
    stage0 = Path(args.woozyc).expanduser().resolve()
    parent = Path(args.work_dir).expanduser().resolve()
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="needs-drop-cache-", dir=parent))
    env = environment(work)
    state: dict[str, object] = {
        "operation_id": "SH15-hir-needs-drop-cache-20261114",
        "status": "running",
        "work_dir": str(work),
        "runner_argv": [sys.executable, *sys.argv],
    }
    try:
        fixed = verify_fixed(stage0)
        large = generate_large_fixture(work / "fixtures" / "large", 40)
        project = stage_project(work)
        source = source_snapshot()
        tests = test_snapshot()
        if source["map_sha256"] != args.expected_source_map:
            fail(f"current Source116 map differs: {source['map_sha256']}")
        before = frozen_record(stage0, large, project, source, tests, fixed)
        (work / "inputs.before.json").write_text(
            json.dumps(before, indent=2, allow_nan=False) + "\n",
            encoding="utf-8")
        state["inputs_before"] = before
        print(f"work-dir: {work}", flush=True)

        manifest = str(project / "lang.toml")
        check_record, check_stdout, check_stderr = command(
            [str(stage0), "check", "--project", manifest,
             "--backend", "native", "--format", "json"],
            "check", work, args.timeout, env)
        require_clean_streams("check", check_stdout, check_stderr)
        check_report = parse_json_report("check", check_stdout)

        out_dir = work / "out-o2"
        build_record, build_stdout, build_stderr = command(
            [str(stage0), "build", "--project", manifest,
             "--backend", "native", "--opt", "2",
             "--out-dir", str(out_dir), "--format", "json"],
            "build-o2", work, args.timeout, env)
        require_clean_streams("build-o2", build_stdout, build_stderr)
        build_report = parse_json_report("build-o2", build_stdout)
        program = out_dir / "program"
        if not program.is_file() or program.stat().st_size == 0:
            fail("build produced no non-empty program")
        generated = {
            "program": file_record(program),
        }
        if (out_dir / "program.o").is_file():
            generated["program_o"] = file_record(out_dir / "program.o")

        small_manifest = (SMALL_FIXTURE / "lang.toml").resolve()
        large_manifest = (large / "lang.toml").resolve()
        run_argv = [str(program), str(args.iterations),
                    str(small_manifest), str(STDLIB),
                    str(large_manifest), str(STDLIB)]
        run_record, run_stdout, run_stderr = command(
            run_argv, "run", work, args.run_timeout, env)
        require_clean_streams("run", run_stdout, run_stderr)
        parsed = parse_output(run_stdout.decode("utf-8", errors="replace"),
                              args.iterations, 2, small_manifest,
                              large_manifest, True)

        trace_dir = work / "trace"
        trace_dir.mkdir()
        exec_log = trace_dir / "exec.log"
        trace_argv = [
            str(FIXED_STRACE), "--seccomp-bpf", "-f", "-qq", "-s", "16384",
            "-e", "trace=execve,execveat", "-o", str(exec_log),
            str(program), "1", str(small_manifest), str(STDLIB),
        ]
        trace_record, trace_stdout, trace_stderr = command(
            trace_argv, "trace/run", work, args.run_timeout, env)
        require_clean_streams("trace/run", trace_stdout, trace_stderr)
        trace_parsed = parse_output(
            trace_stdout.decode("utf-8", errors="replace"), 1, 1,
            small_manifest, large_manifest, False)
        expected_program = str(program.resolve())
        observed = parse_exec_log(exec_log, expected_program)
        if observed != [expected_program]:
            fail(f"exec trace does not contain exactly the program: {observed!r}")
        trace_info = {
            "scope": "trace-only execution of the already-built program; "
                     "timing lines are not used for the benchmark",
            "command": trace_argv,
            "exit": trace_record["exit"],
            "timed_out": trace_record["timed_out"],
            "cleanup_complete": trace_record["cleanup_complete"],
            "abandoned_pipes": trace_record["abandoned_pipes"],
            "stdout_sha256": trace_record["stdout_sha256"],
            "stderr_sha256": trace_record["stderr_sha256"],
            "exec_log_path": str(exec_log),
            "exec_log_sha256": sha256(exec_log),
            "observed_execs": observed,
            "program": expected_program,
            "checks_passed": trace_parsed["needs_passed"],
            "checks_failed": trace_parsed["needs_failed"],
        }
        (trace_dir / "trace.json").write_text(
            json.dumps(trace_info, indent=2, allow_nan=False) + "\n",
            encoding="utf-8")

        source_after = source_snapshot()
        tests_after = test_snapshot()
        fixed_after = verify_fixed(stage0)
        after = frozen_record(stage0, large, project, source_after,
                              tests_after, fixed_after)
        (work / "inputs.after.json").write_text(
            json.dumps(after, indent=2, allow_nan=False) + "\n",
            encoding="utf-8")
        state["inputs_after"] = after
        problems = []
        for name in ("source116", "test", "staged_hirverify", "small_fixture",
                     "large_fixture", "fixed"):
            if before[name] != after[name]:
                problems.append(name)
        if problems:
            fail(f"frozen inputs changed during run: {problems}")
        if source_after["map_sha256"] != args.expected_source_map:
            fail("Source116 map changed during run")

        commands = []
        for path in sorted(work.rglob("*.json")):
            if path.name in ("inputs.before.json", "inputs.after.json",
                             "result.json", "trace.json"):
                continue
            try:
                commands.append(json.loads(path.read_text(encoding="utf-8")))
            except ValueError as error:
                fail(f"command record is not valid JSON: {path}: {error}")
        state.update({
            "status": "passed",
            "check": check_record,
            "check_report": check_report,
            "build": build_record,
            "build_report": build_report,
            "run": run_record,
            "run_report": parsed,
            "trace": trace_info,
            "generated": generated,
            "commands": commands,
            "environment": {
                "python": sys.version,
                "python_executable": sys.executable,
                "platform": platform.platform(),
                "machine": platform.machine(),
                "opt": 2,
                "backend": "native",
                "iterations": args.iterations,
                "benchmark_ids": 32,
                "benchmark_passes": 64,
                "benchmark_trials": 3,
            },
            "notes": [
                "The benchmark warms the per-Verifier cache before the timed "
                "cached pass; the uncached pass calls the real "
                "valueops.classify_value for the same module and ids.",
                "The benchmark is only this component's repeated-query "
                "comparison; it is not a full compiler or before/after claim.",
                "The trace-only run uses the already-built program with one "
                "module and one iteration; its timing lines are not used.",
            ],
        })
        (work / "result.json").write_text(
            json.dumps(state, indent=2, allow_nan=False) + "\n",
            encoding="utf-8")
        print(f"PASS: cache_probe={parsed['cache_passed']} "
              f"needs_drop={parsed['needs_passed']}", flush=True)
        for bench in parsed["benches"]:
            print(f"PASS: trial={bench['trial']} "
                  f"cached_ns={bench['cached_ns']} "
                  f"uncached_ns={bench['uncached_ns']}", flush=True)
        print(f"result: {work / 'result.json'}", flush=True)
        return 0
    except Exception as error:  # noqa: BLE001 - retain exact failure evidence
        state["status"] = "failed"
        state["error"] = repr(error)
        commands = []
        for path in sorted(work.rglob("*.json")):
            if path.name in ("inputs.before.json", "inputs.after.json",
                             "result.json", "trace.json"):
                continue
            try:
                commands.append(json.loads(path.read_text(encoding="utf-8")))
            except (OSError, ValueError):
                pass
        state["commands"] = commands
        try:
            (work / "result.json").write_text(
                json.dumps(state, indent=2, allow_nan=False) + "\n",
                encoding="utf-8")
        except (OSError, ValueError):
            pass
        print(json.dumps({"status": "failed", "work_dir": str(work),
                          "error": repr(error)}, indent=2), flush=True)
        return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
