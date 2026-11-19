#!/usr/bin/env python3
"""Differential resolver check: pure-Wz dump versus the C++ oracle.

The Wz program is built from the real selfhost source/lexer/parser and resolver
modules.  The test-only oracle is compiled from the current C++ resolver,
lexer and parser sources; it is not linked into or called by the self-host
compiler.  Every Symbol, Occurrence, deferred member, Self parameter,
diagnostic span/related entry, phase and failure field is compared in order.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CASES = HERE / "cases"
ORACLE = HERE / "oracle.cpp"
# The oracle is linked against the already-built Root libraries; rebuilding a
# hand-picked source subset silently misses new dependencies such as digest and
# parse-cache symbols.  liblang_driver_process.a owns StagingDir/publish, which
# liblang_modules.a now references through ParseCache::store.
ORACLE_LIBS = (
    "liblang_modules.a",
    "liblang_frontend.a",
    "liblang_driver_process.a",
)


def load_run_module():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("resolve_run", HERE / "run.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def command(argv: list[str], timeout: int, cwd: Path,
            env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    print("+ " + " ".join(argv), flush=True)
    return subprocess.run(argv, cwd=cwd, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=timeout)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--woozyc", required=True)
    parser.add_argument("--work-dir", default="")
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2))
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--lib-dir", default="",
                        help="directory with the current C++ static libraries "
                             "(default: the directory containing --woozyc)")
    args = parser.parse_args()

    run = load_run_module()
    woozyc = Path(args.woozyc).resolve()
    if not woozyc.is_file():
        print(f"stage-0 compiler not found: {woozyc}", file=sys.stderr)
        return 2
    lib_dir = (Path(args.lib_dir).expanduser().resolve()
               if args.lib_dir else woozyc.parent)
    missing_libs = [name for name in ORACLE_LIBS
                    if not (lib_dir / name).is_file()]
    if missing_libs:
        print(f"C++ oracle libraries not found in {lib_dir}: "
              f"{', '.join(missing_libs)}", file=sys.stderr)
        return 2
    if args.work_dir:
        work = Path(args.work_dir).resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        work = Path(tempfile.mkdtemp(prefix="woozy-resolve-diff-"))
    project = run.stage_project(work)
    manifest = str(project / "lang.toml")
    env = os.environ.copy()

    oracle = work / "oracle"
    compile_command = [
        args.cxx, "-std=c++20", "-O0", f"-I{ROOT / 'include'}",
        str(ORACLE),
        *[str(lib_dir / name) for name in ORACLE_LIBS],
        "-ltomlplusplus", "-ljson-c", "-lm", "-o", str(oracle),
    ]
    built = command(compile_command, args.timeout, ROOT, env)
    if built.returncode != 0:
        print(built.stdout, end="")
        print(built.stderr, end="", file=sys.stderr)
        print("FAIL: C++ oracle build", file=sys.stderr)
        return 1

    cases = sorted(path for path in CASES.iterdir() if path.is_dir())
    levels = args.opt or [0, 2]
    for level in levels:
        out_dir = project / f"diff-o{level}"
        build = command([
            str(woozyc), "build", "--project", manifest,
            "--backend", "native", "--out-dir", str(out_dir),
            "--opt", str(level), "--format", "json",
        ], args.timeout, project, env)
        report = run.parse_json_output(build.stdout, build.stderr)
        if build.returncode != 0 or report.get("status") != "ok":
            print(build.stdout, end="")
            print(build.stderr, end="", file=sys.stderr)
            print(f"FAIL: Wz differential build at O{level}", file=sys.stderr)
            return 1
        program = out_dir / "program"
        for case in cases:
            wz = command([str(program), "dump", str(case)], args.timeout,
                         project, env)
            cpp = command([str(oracle), str(case)], args.timeout, project, env)
            if wz.returncode != 0 or cpp.returncode != 0:
                print(wz.stderr, end="", file=sys.stderr)
                print(cpp.stderr, end="", file=sys.stderr)
                print(f"FAIL: {case.name} execution at O{level}",
                      file=sys.stderr)
                return 1
            if wz.stdout != cpp.stdout:
                print(f"FAIL: {case.name} differs at O{level}", file=sys.stderr)
                wz_lines = wz.stdout.splitlines()
                cpp_lines = cpp.stdout.splitlines()
                for index in range(max(len(wz_lines), len(cpp_lines))):
                    left = cpp_lines[index] if index < len(cpp_lines) else "<none>"
                    right = wz_lines[index] if index < len(wz_lines) else "<none>"
                    if left != right:
                        print(f"first difference at line {index + 1}",
                              file=sys.stderr)
                        print(f"  C++: {left}", file=sys.stderr)
                        print(f"  Wz : {right}", file=sys.stderr)
                        break
                return 1
            print(f"PASS: {case.name} O{level} exact field/order match")
    print(f"PASS: resolver differential {len(cases)} cases O"
          f"{'/O'.join(str(level) for level in levels)}; work dir {work}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.TimeoutExpired as error:
        print(f"FAIL: timeout: {error}", file=sys.stderr)
        raise SystemExit(124)
