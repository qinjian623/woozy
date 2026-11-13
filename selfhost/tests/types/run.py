#!/usr/bin/env python3
"""Run the pure-Wz type-table tests against the frozen C++ oracle.

Two independent traces are staged in temporary projects:

* the full 180-line type-table/algorithms trace (`src/main.wz` + `oracle.cpp`)
* the 25-line bit_width success/error trace
  (`width/src/main.wz` + `width_oracle.cpp`)

Each Wz project is built and run at O0 and O2.  The C++ oracles are compiled
once and linked against the already-built Root liblang_hir.a (and its normal
dependencies).  The C++ compiler is never rebuilt and the Wz programs never
invoke the old frontend.
"""

from __future__ import annotations

import argparse
import difflib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_STAGE0 = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
DEFAULT_ORACLE_BUILD = DEFAULT_STAGE0.parent
DEFAULT_TOOLCHAIN = ROOT.parent / "toolchains"
DEFAULT_TIMEOUT = 180.0
MAIN_TRACE_LINES = 180
WIDTH_TRACE_LINES = 25


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check, build and execute the Wz type-table tests.")
    parser.add_argument(
        "--woozyc", default=str(DEFAULT_STAGE0),
        help=f"stage0 compiler (default: {DEFAULT_STAGE0})")
    parser.add_argument(
        "--oracle-build-dir", default=str(DEFAULT_ORACLE_BUILD),
        help="directory containing liblang_hir.a/liblang_modules.a/"
             f"liblang_frontend.a (default: {DEFAULT_ORACLE_BUILD})")
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


def stage_project(work: pathlib.Path, name: str, main: pathlib.Path,
                  manifest: pathlib.Path) -> pathlib.Path:
    project = work / name
    if project.exists():
        shutil.rmtree(project)
    model = project / "src" / "wzcc" / "model"
    types = project / "src" / "wzcc" / "types"
    model.mkdir(parents=True)
    types.mkdir(parents=True)
    shutil.copy2(ROOT / "selfhost" / "src" / "wzcc" / "model" / "types.wz",
                 model / "types.wz")
    shutil.copy2(ROOT / "selfhost" / "src" / "wzcc" / "types" / "types.wz",
                 types / "types.wz")
    shutil.copy2(main, project / "src" / "main.wz")
    shutil.copy2(manifest, project / "lang.toml")
    return project


def build_oracle(build_dir: pathlib.Path, work: pathlib.Path,
                  source: pathlib.Path, name: str, env: dict[str, str],
                  timeout: float) -> pathlib.Path:
    libraries = [build_dir / library_name for library_name in
                 ("liblang_hir.a", "liblang_modules.a", "liblang_frontend.a")]
    missing = [str(path) for path in libraries if not path.is_file()]
    if missing:
        fail("missing oracle libraries: " + ", ".join(missing) +
             " (pass --oracle-build-dir)")
    oracle = work / name
    command = [
        "c++", "-std=c++23", "-O2",
        "-I", str(ROOT / "include"),
        str(source),
        *map(str, libraries),
        "-ltomlplusplus", "-ljson-c", "-lm", "-lgmpxx", "-lgmp",
        "-o", str(oracle),
    ]
    result = run(command, f"compile {name}", work, env, timeout)
    expect_ok(result, f"compile {name}")
    return oracle


def oracle_trace(oracle: pathlib.Path, name: str, work: pathlib.Path,
                 env: dict[str, str], timeout: float,
                 expected_lines: int) -> str:
    result = run([str(oracle)], name, work, env, timeout)
    expect_ok(result, name)
    lines = result.stdout.splitlines()
    if len(lines) != expected_lines:
        fail(f"{name}: {len(lines)} lines, expected {expected_lines}")
    print(f"PASS: {name} ({len(lines)} lines)")
    return result.stdout


def wz_traces(stage0: pathlib.Path, project: pathlib.Path, suite: str,
              work: pathlib.Path, env: dict[str, str], levels: list[int],
              timeout: float, expected_lines: int) -> list[tuple[int, str]]:
    manifest = str(project / "lang.toml")
    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], f"{suite} stage0 check", work, env,
                  timeout)
    expect_ok(checked, f"{suite} stage0 check")

    traces: list[tuple[int, str]] = []
    for level in levels:
        out_dir = work / f"{suite}-opt{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", str(level),
                     "--out-dir", str(out_dir)], f"{suite} build O{level}",
                    work, env, timeout)
        expect_ok(built, f"{suite} build O{level}")
        program = out_dir / "program"
        if not program.is_file():
            fail(f"{suite} O{level}: missing {program}")
        values = run([str(program)], f"{suite} values O{level}", work, env,
                     timeout)
        expect_ok(values, f"{suite} value tests O{level}")
        lines = values.stdout.splitlines()
        if len(lines) != expected_lines:
            fail(f"{suite} O{level}: {len(lines)} lines, "
                 f"expected {expected_lines}")
        traces.append((level, values.stdout))
    return traces


def compare_traces(suite: str, oracle: str,
                   traces: list[tuple[int, str]]) -> None:
    for level, trace in traces:
        if trace != oracle:
            diff = "".join(difflib.unified_diff(
                oracle.splitlines(keepends=True),
                trace.splitlines(keepends=True),
                fromfile=f"{suite} C++ oracle", tofile=f"{suite} Wz O{level}"))
            fail(f"{suite} O{level} trace differs from C++ oracle\n{diff}")
        print(f"PASS: {suite} O{level} trace matches C++ oracle "
              f"({len(trace.splitlines())} lines)")

    if len(traces) > 1:
        first_level, first = traces[0]
        for level, trace in traces[1:]:
            if trace != first:
                fail(f"{suite} Wz O{first_level} and O{level} traces differ")
        print(f"PASS: {suite} Wz O"
              f"{'/O'.join(str(level) for level, _ in traces)} traces "
              f"are identical")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    build_dir = pathlib.Path(args.oracle_build_dir).expanduser().resolve()
    toolchain = pathlib.Path(args.toolchain_root).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")
    levels = args.opt or [0, 2]
    if args.work_dir:
        work = pathlib.Path(args.work_dir).expanduser().resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        work = pathlib.Path(tempfile.mkdtemp(prefix="woozy-types-test-"))
    env = environment(toolchain)
    print(f"stage0: {stage0}")
    print(f"oracle build: {build_dir}")
    print(f"work-dir: {work}")

    project = stage_project(work, "project", HERE / "src" / "main.wz",
                             HERE / "lang.toml")
    width_project = stage_project(work, "width-project",
                                   HERE / "width" / "src" / "main.wz",
                                   HERE / "width" / "lang.toml")

    main_oracle = build_oracle(build_dir, work, HERE / "oracle.cpp",
                               "oracle", env, args.timeout)
    width_oracle = build_oracle(build_dir, work, HERE / "width_oracle.cpp",
                                "width_oracle", env, args.timeout)
    main_oracle_trace = oracle_trace(main_oracle, "main oracle", work, env,
                                     args.timeout, MAIN_TRACE_LINES)
    width_oracle_trace = oracle_trace(width_oracle, "width oracle", work, env,
                                      args.timeout, WIDTH_TRACE_LINES)

    main_traces = wz_traces(stage0, project, "main", work, env, levels,
                            args.timeout, MAIN_TRACE_LINES)
    width_traces = wz_traces(stage0, width_project, "width", work, env,
                             levels, args.timeout, WIDTH_TRACE_LINES)
    compare_traces("main", main_oracle_trace, main_traces)
    compare_traces("width", width_oracle_trace, width_traces)

    print(f"PASS: type table tests (main {MAIN_TRACE_LINES} lines + width "
          f"{WIDTH_TRACE_LINES} lines, check, O"
          f"{'/O'.join(map(str, levels))}, C++ oracle); work dir {work}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
