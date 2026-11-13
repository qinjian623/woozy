#!/usr/bin/env python3
"""Check, build and run the Native CFG data-record tests.

The Wz project is staged in a temporary directory from this repository's
model, hir, types and nativeir packages; no production module is duplicated
under tests/.  The C++ oracle includes the frozen Root ir/native.hpp only; it
does not link or rebuild the compiler and never constructs ir::Module (whose
hir field is a VerifiedHirModule without a public constructor).

Each project is checked by stage0, built at O0 and O2, and its output compared
byte-for-byte with the header-only oracle.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_STAGE0 = ROOT / "build/woozy-stdlib-native/woozyc"
DEFAULT_ROOT_INCLUDE = ROOT / "include"
DEFAULT_TYPES_SOURCE = ROOT / "selfhost/src/wzcc/types"
DEFAULT_TOOLCHAIN = ROOT.parent / "toolchains"
DEFAULT_TIMEOUT = 240.0
TRACE_LINES = 77


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check, build and execute the Wz Native CFG record tests.")
    parser.add_argument(
        "--woozyc", default=str(DEFAULT_STAGE0),
        help=f"stage0 compiler (default: {DEFAULT_STAGE0})")
    parser.add_argument(
        "--root-include", default=str(DEFAULT_ROOT_INCLUDE),
        help=f"frozen Root include directory (default: {DEFAULT_ROOT_INCLUDE})")
    parser.add_argument(
        "--types-source", default=str(DEFAULT_TYPES_SOURCE),
        help="accepted Root app/wzcc/types directory "
             f"(default: {DEFAULT_TYPES_SOURCE})")
    parser.add_argument(
        "--toolchain-root", default=str(DEFAULT_TOOLCHAIN),
        help=f"LLVM/libxml2 toolchain root (default: {DEFAULT_TOOLCHAIN})")
    parser.add_argument(
        "--work-dir", default="",
        help="staging directory (default: a new temp directory)")
    parser.add_argument(
        "--timeout", type=float, default=DEFAULT_TIMEOUT,
        help=f"timeout in seconds for each command (default: "
             f"{DEFAULT_TIMEOUT:g})")
    parser.add_argument(
        "--opt", action="append", type=int, choices=(0, 2),
        help="optimization level (default: 0 and 2)")
    return parser.parse_args(argv)


def environment(toolchain: pathlib.Path) -> dict[str, str]:
    env = dict(os.environ)
    parts = [
        str(toolchain / "llvm-22.1.8" / "lib"),
        str(toolchain / "libxml2-legacy-2.13.9" / "usr" / "lib"),
    ]
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        parts.append(previous)
    env["LD_LIBRARY_PATH"] = ":".join(parts)
    return env


def run(command: list[str], label: str, cwd: pathlib.Path,
        env: dict[str, str], timeout: float) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    try:
        return subprocess.run(
            command, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False, timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"{label}: timed out after {timeout:g}s")
    except OSError as error:
        fail(f"{label}: {error}")


def expect_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label}: exit {result.returncode}\n"
             f"stdout:\n{result.stdout}stderr:\n{result.stderr}")
    print(f"PASS: {label}")


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stage_project(work: pathlib.Path, types_source: pathlib.Path) -> pathlib.Path:
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    src = project / "src" / "wzcc"
    for package, names in (
            ("model", ("model.wz", "project.wz", "types.wz")),
            ("hir", ("nodes.wz", "ownership.wz", "module.wz")),
            ("nativeir", ("nativeir.wz",))):
        target = src / package
        target.mkdir(parents=True)
        for name in names:
            shutil.copy2(ROOT / "selfhost" / "src" / "wzcc" / package / name,
                         target / name)
    types = src / "types"
    types.mkdir()
    for file in sorted(types_source.iterdir()):
        if file.is_file():
            shutil.copy2(file, types / file.name)
    shutil.copy2(HERE / "src" / "main.wz", project / "src" / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def build_oracle(root_include: pathlib.Path, work: pathlib.Path,
                 env: dict[str, str], timeout: float) -> pathlib.Path:
    oracle = work / "oracle"
    command = [
        "c++", "-std=c++23", "-O2", "-I", str(root_include),
        str(HERE / "oracle.cpp"), "-o", str(oracle),
    ]
    result = run(command, "compile oracle", work, env, timeout)
    expect_ok(result, "compile oracle")
    return oracle


def oracle_trace(oracle: pathlib.Path, work: pathlib.Path,
                 env: dict[str, str], timeout: float) -> str:
    result = run([str(oracle)], "oracle", work, env, timeout)
    expect_ok(result, "oracle")
    lines = result.stdout.splitlines()
    if len(lines) != TRACE_LINES:
        fail(f"oracle: {len(lines)} lines, expected {TRACE_LINES}")
    print(f"PASS: oracle ({len(lines)} lines)")
    return result.stdout


def wz_traces(stage0: pathlib.Path, project: pathlib.Path,
              work: pathlib.Path, env: dict[str, str], levels: list[int],
              timeout: float) -> list[tuple[int, str]]:
    manifest = str(project / "lang.toml")
    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "Wz stage0 check", work, env,
                  timeout)
    expect_ok(checked, "Wz stage0 check")

    traces: list[tuple[int, str]] = []
    for level in levels:
        out_dir = work / f"opt{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", str(level),
                     "--out-dir", str(out_dir)], f"Wz build O{level}", work,
                    env, timeout)
        expect_ok(built, f"Wz build O{level}")
        program = out_dir / "program"
        if not program.is_file():
            fail(f"Wz O{level}: missing {program}")
        values = run([str(program)], f"Wz values O{level}", work, env,
                     timeout)
        expect_ok(values, f"Wz value tests O{level}")
        lines = values.stdout.splitlines()
        if len(lines) != TRACE_LINES:
            fail(f"Wz O{level}: {len(lines)} lines, expected {TRACE_LINES}")
        traces.append((level, values.stdout))
    return traces


def compare_traces(oracle: str, traces: list[tuple[int, str]]) -> None:
    for level, trace in traces:
        if trace != oracle:
            diff = "".join(difflib.unified_diff(
                oracle.splitlines(keepends=True),
                trace.splitlines(keepends=True),
                fromfile="C++ header oracle", tofile=f"Wz O{level}"))
            fail(f"Wz O{level} trace differs from C++ header oracle\n{diff}")
        print(f"PASS: Wz O{level} trace matches C++ header oracle "
              f"({len(trace.splitlines())} lines)")
    if len(traces) > 1:
        first_level, first = traces[0]
        for level, trace in traces[1:]:
            if trace != first:
                fail(f"Wz O{first_level} and O{level} traces differ")
        print(f"PASS: Wz O"
              f"{'/O'.join(str(level) for level, _ in traces)} traces "
              f"are identical")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    root_include = pathlib.Path(args.root_include).expanduser().resolve()
    types_source = pathlib.Path(args.types_source).expanduser().resolve()
    toolchain = pathlib.Path(args.toolchain_root).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    if not (root_include / "lang" / "ir" / "native.hpp").is_file():
        fail(f"frozen Root native.hpp missing under {root_include}")
    if not (types_source / "types.wz").is_file():
        fail(f"accepted Root types package missing under {types_source}")
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")
    levels = args.opt or [0, 2]
    if args.work_dir:
        work = pathlib.Path(args.work_dir).expanduser().resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        work = pathlib.Path(tempfile.mkdtemp(prefix="woozy-nativeir-records-test-"))
    env = environment(toolchain)
    print(f"stage0: {stage0}")
    print(f"root include: {root_include}")
    print(f"types source: {types_source}")
    print(f"work-dir: {work}")
    print(f"types.wz sha256: {digest(types_source / 'types.wz')}")
    for name in ("model.wz", "project.wz", "types.wz"):
        path = ROOT / "selfhost" / "src" / "wzcc" / "model" / name
        print(f"model/{name} sha256: {digest(path)}")
    for name in ("nodes.wz", "ownership.wz", "module.wz"):
        path = ROOT / "selfhost" / "src" / "wzcc" / "hir" / name
        print(f"hir/{name} sha256: {digest(path)}")
    nativeir = ROOT / "selfhost" / "src" / "wzcc" / "nativeir" / "nativeir.wz"
    print(f"nativeir/nativeir.wz sha256: {digest(nativeir)}")

    project = stage_project(work, types_source)
    oracle = build_oracle(root_include, work, env, args.timeout)
    expected = oracle_trace(oracle, work, env, args.timeout)
    traces = wz_traces(stage0, project, work, env, levels, args.timeout)
    compare_traces(expected, traces)
    print(f"PASS: Native CFG record data tests ({TRACE_LINES} lines, check, "
          f"O{'/O'.join(map(str, levels))}, header oracle); work dir {work}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
