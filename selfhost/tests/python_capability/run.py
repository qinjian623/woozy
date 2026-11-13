#!/usr/bin/env python3
"""Reproduce the Wz backend capability decision for Native-only system calls.

The driver is built with the real stage-0 compiler.  At runtime it parses,
resolves and type-checks accepted fixture sources with the Wz frontend, then
calls the public Wz backend verifier once with Native/64 and once with
Python/64.  The expected result is: Native accepts; Python rejects the
Native-only std/os system operation with the exact typed-HIR call span; a
Python-supported value operation and an ordinary non-system call still pass.

This is a component test.  It does not build the Wz CLI, does not emit or run
Python, and does not claim a full corpus or self-hosting result.
"""
from __future__ import annotations

import argparse
import hashlib
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


def main_checkout() -> Path:
    try:
        result = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute",
             "--git-common-dir"], text=True, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, check=False)
        if result.returncode == 0 and result.stdout.strip():
            return Path(result.stdout.strip()).parent
    except OSError:
        pass
    return ROOT


def default_stage0() -> Path:
    local = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
    if local.is_file():
        return local
    return main_checkout() / "build" / "woozy-stdlib-native" / "woozyc"


def default_toolchain() -> Path:
    local = ROOT.parent / "toolchains"
    if local.is_dir():
        return local
    return main_checkout().parent / "toolchains"


def finite_timeout(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "--timeout must be finite and positive") from error
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("--timeout must be finite and positive")
    return value


def parse_opts(text: str) -> tuple[int, ...]:
    try:
        values = tuple(int(part) for part in text.split(",") if part)
    except ValueError as error:
        raise argparse.ArgumentTypeError("--opts must be comma-separated integers") from error
    if not values or any(value not in (0, 2) for value in values):
        raise argparse.ArgumentTypeError("--opts accepts only 0 and/or 2")
    return values


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", default=str(default_stage0()),
                        help="stage-0 compiler (default: the checkout build)")
    parser.add_argument("--toolchain-root", default=str(default_toolchain()),
                        help="LLVM/libxml2 toolchain root")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"),
                        help="C++ compiler for the same-source reference oracle")
    parser.add_argument("--build-dir", default=None,
                        help="stage-0 build directory with liblang_*.a "
                             "(default: the stage-0 compiler's directory)")
    parser.add_argument("--work-dir", default=None,
                        help="parent for retained run artifacts")
    parser.add_argument("--timeout", type=finite_timeout, default=1200.0,
                        help="per command timeout in seconds")
    parser.add_argument("--opts", type=parse_opts, default=(0, 2),
                        help="Wz driver optimization levels (default: 0,2)")
    return parser.parse_args()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot_inputs() -> dict[str, str]:
    paths = [HERE / "src" / "main.wz", HERE / "lang.toml",
             SELFHOST / "hirverify" / "backend.wz"]
    paths.extend(sorted((HERE / "fixtures").rglob("*.wz")))
    return {path.relative_to(ROOT).as_posix(): sha256(path) for path in paths}


def stage_project(work: Path) -> Path:
    project = work / "project"
    modules = project / "src" / "wzcc"
    modules.mkdir(parents=True)
    for module in sorted(SELFHOST.iterdir()):
        if module.is_dir():
            (modules / module.name).symlink_to(module, target_is_directory=True)
    (project / "src" / "std").symlink_to(ROOT / "stdlib",
                                           target_is_directory=True)
    shutil.copy2(HERE / "src" / "main.wz", project / "src" / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    shutil.copytree(HERE / "fixtures", work / "fixtures")
    return project


def environment(toolchain_root: Path, work: Path) -> dict[str, str]:
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
    (work / "tmp").mkdir(exist_ok=True)
    env["TMPDIR"] = str(work / "tmp")
    return env


def command(argv: list[str], label: str, work: Path, timeout: float,
            env: dict[str, str], allow_failure: bool = False) -> dict:
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
        "stdout_sha256": hashlib.sha256(stdout_bytes).hexdigest(),
        "stderr_sha256": hashlib.sha256(stderr_bytes).hexdigest(),
    }
    (work / f"{label}.json").write_text(
        json.dumps(record, indent=2) + "\n", encoding="utf-8")
    stdout = stdout_bytes.decode("utf-8", errors="replace")
    stderr = stderr_bytes.decode("utf-8", errors="replace")
    if (process["timed_out"] or not process["cleanup_complete"]
            or process["abandoned_pipes"]
            or (process["exit"] != 0 and not allow_failure)):
        raise RuntimeError(
            f"{label}: exit {process['exit']} "
            f"timed_out={process['timed_out']} "
            f"cleanup_complete={process['cleanup_complete']} "
            f"abandoned_pipes={process['abandoned_pipes']}\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}")
    return {"record": record, "stdout": stdout, "stderr": stderr,
            "exit": process["exit"]}


def compiler_ok(result: dict, label: str) -> None:
    try:
        report = json.loads(result["stdout"])
    except ValueError as error:
        raise RuntimeError(
            f"{label}: invalid compiler JSON: {result['stdout']}") from error
    if report.get("status") != "ok":
        raise RuntimeError(f"{label}: compiler did not succeed: {result['stdout']}")


def operation_id(name: str) -> int:
    text = (SELFHOST / "system" / "system.wz").read_text(encoding="utf-8")
    match = re.search(
        rf'OperationSpec\(operation: (\d+), name: "{re.escape(name)}"', text)
    if match is None:
        raise RuntimeError(f"system operation missing from generated table: {name}")
    return int(match.group(1))


def span_of(path: Path, needle: bytes) -> tuple[int, int]:
    data = path.read_bytes()
    begin = data.index(needle)
    return begin, begin + len(needle)


def expected_lines() -> list[str]:
    fixtures = HERE / "fixtures"
    os_operation = operation_id("__woozy_os_text_length")
    text_operation = operation_id("__woozy_text_len")
    os_begin, os_end = span_of(fixtures / "native_only" / "os.wz",
                               b"__woozy_os_text_length(value)")
    text_begin, text_end = span_of(fixtures / "value" / "text.wz",
                                   b"__woozy_text_len(value)")
    return [
        f"CASE native-only-native resolve=1 typed=1 result=accept code=none "
        f"phase=none primary=0 begin=0 end=0 call=1 operation={os_operation} "
        f"call-begin={os_begin} call-end={os_end} message=none",
        f"CASE native-only-python resolve=1 typed=1 result=reject "
        f"code=E_CAPABILITY_UNAVAILABLE phase=type primary=1 begin={os_begin} "
        f"end={os_end} call=1 operation={os_operation} call-begin={os_begin} "
        f"call-end={os_end} "
        f"message=system operations require the Native/64 profile",
        f"CASE value-python resolve=1 typed=1 result=accept code=none "
        f"phase=none primary=0 begin=0 end=0 call=1 "
        f"operation={text_operation} call-begin={text_begin} "
        f"call-end={text_end} message=none",
        "CASE ordinary-python resolve=1 typed=1 result=accept code=none "
        "phase=none primary=0 begin=0 end=0 call=0 operation=0 "
        "call-begin=0 call-end=0 message=none",
    ]


def compare_output(actual: str, expected: list[str], opt: int) -> None:
    lines = actual.splitlines()
    if lines[:4] != expected:
        raise RuntimeError(
            f"O{opt}: driver report differs\n"
            f"expected:\n" + "\n".join(expected) + "\nactual:\n" + actual)
    if len(lines) != 5:
        raise RuntimeError(f"O{opt}: unexpected extra driver output:\n{actual}")
    summary = re.fullmatch(r"SUMMARY checks=(\d+) failures=0", lines[4])
    if summary is None or int(summary.group(1)) == 0:
        raise RuntimeError(f"O{opt}: failed driver summary: {lines[4]!r}")


def write_results(work: Path, status: str, **values: object) -> None:
    document = {"status": status, **values}
    (work / "results.json").write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")


def main() -> int:
    args = parse_args()
    stage0 = Path(args.woozyc).expanduser().resolve()
    if not stage0.is_file():
        raise RuntimeError(f"stage0 compiler missing: {stage0}")
    toolchain = Path(args.toolchain_root).expanduser().resolve()
    build_dir = (Path(args.build_dir).expanduser().resolve()
                 if args.build_dir else stage0.parent)
    parent = (Path(args.work_dir).expanduser().resolve()
              if args.work_dir else ROOT / "build" / "python-capability-runs")
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="python-capability-", dir=parent))
    print(f"work-dir: {work}", flush=True)
    inputs_before = snapshot_inputs()
    project = stage_project(work)
    manifest = str(project / "lang.toml")
    env = environment(toolchain, work)
    records = []
    tested = []
    try:
        checked = command([str(stage0), "check", "--project", manifest,
                           "--backend", "native", "--format", "json"],
                          "check", work, args.timeout, env)
        compiler_ok(checked, "check")
        records.append(checked["record"])
        expected = expected_lines()
        oracle = work / "oracle"
        libs = [build_dir / name for name in (
            "liblang_sema.a", "liblang_hir.a", "liblang_modules.a",
            "liblang_frontend.a")]
        for library in libs:
            if not library.is_file():
                raise RuntimeError(f"oracle library missing: {library}")
        built_oracle = command([
            args.cxx, "-std=c++20", "-O0", "-g0", "-I",
            str(ROOT / "include"), str(HERE / "oracle.cpp"),
            "-Wl,--start-group", *[str(path) for path in libs],
            "-Wl,--end-group", "-ljson-c", "-ltomlplusplus", "-lgmpxx",
            "-lgmp", "-lm", "-o", str(oracle)], "oracle-build", work,
            args.timeout, env)
        records.append(built_oracle["record"])
        oracle_run = command([str(oracle), str(work / "fixtures")],
                             "oracle-run", work, args.timeout, env)
        records.append(oracle_run["record"])
        expected_cpp = [line.replace("CASE ", "CPP ", 1)
                        .replace(" resolve=1 typed=1", "")
                        for line in expected]
        if oracle_run["stdout"].splitlines() != expected_cpp:
            raise RuntimeError(
                "C++ reference report differs\nexpected:\n" +
                "\n".join(expected_cpp) + "\nactual:\n" +
                oracle_run["stdout"])
        print("PASS: C++ reference matches the expected capability report",
              flush=True)
        for opt in args.opts:
            output = work / f"out-o{opt}"
            built = command([str(stage0), "build", "--project", manifest,
                             "--backend", "native", "--opt", str(opt),
                             "--out-dir", str(output), "--format", "json"],
                            f"build-o{opt}", work, args.timeout, env)
            compiler_ok(built, f"build O{opt}")
            records.append(built["record"])
            ran = command([str(output / "program"), str(work / "fixtures")],
                          f"run-o{opt}", work, args.timeout, env,
                          allow_failure=True)
            records.append(ran["record"])
            compare_output(ran["stdout"], expected, opt)
            if ran["exit"] != 0:
                raise RuntimeError(
                    f"O{opt}: driver exited {ran['exit']} after matching report")
            tested.append({"opt": opt, "program": str(output / "program"),
                           "program_sha256": sha256(output / "program"),
                           "report": ran["stdout"].splitlines()})
            print(f"PASS: O{opt} capability report", flush=True)
        inputs_after = snapshot_inputs()
        if inputs_after != inputs_before:
            raise RuntimeError("Source/resource inputs changed during the run")
        write_results(
            work, "passed", stage0=str(stage0), stage0_sha256=sha256(stage0),
            fixture_sha256={
                path.relative_to(HERE).as_posix(): sha256(path)
                for path in sorted((HERE / "fixtures").rglob("*.wz"))},
            oracle_sha256=sha256(oracle),
            inputs_before=inputs_before, inputs_after=inputs_after,
            commands=records, tested=tested)
        print("PASS: Native-only system call rejected for Python; Native/value/"
              "ordinary controls accepted", flush=True)
        return 0
    except Exception:
        inputs_after = snapshot_inputs()
        write_results(
            work, "failed", stage0=str(stage0),
            stage0_sha256=sha256(stage0) if stage0.is_file() else "",
            inputs_before=inputs_before, inputs_after=inputs_after,
            commands=records, tested=tested)
        raise


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
