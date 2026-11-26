#!/usr/bin/env python3
"""Compare the Wz required exit/call obligations with src/ir/obligations.hpp.

trace.wz is a test scaffold staged into nativeverify/obligations; it
builds a hand-written HIR control tree (call error exit + nested defer, return
with two defers and a nested throwing call, ForIn with continue/break, an
exhaustive switch with return/break arms, try/catch and await error/cancel) and
prints the obligations of the real required_exits helper.  oracle.cpp builds
the same tree and prints lang::ir::detail::RequiredExits.  Stdout must match
byte-for-byte at O0 and O2.

Packages that are accepted in the repository are staged from ROOT; packages
still landing elsewhere are supplied with --extra-source.  No stub package is
created. The trace exercises the obligation algorithm on builder HIR data;
the project check resolves the other verifier files with their real imports.
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
ROOT = HERE.parents[3]
sys.path.insert(0, str(HERE.parents[1]))
from wzcc_closure import complete_import_closure  # noqa: E402

DEFAULT_STAGE0 = ROOT / "build/woozy-stdlib-native/woozyc"
DEFAULT_HIR_LIBRARY = ROOT / "build/woozy-stdlib-native/liblang_hir.a"
DEFAULT_TIMEOUT = 300.0

PACKAGES = ["model", "hir", "types", "nativeir", "foreign", "nativelayout",
            "nativeverify", "pattern", "valueops"]


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compare required HIR exit/call obligations.")
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0))
    parser.add_argument("--hir-library", default=str(DEFAULT_HIR_LIBRARY))
    parser.add_argument("--toolchain-root", default=str(ROOT.parent / "toolchains"))
    parser.add_argument("--extra-source", action="append", default=[])
    parser.add_argument("--work-dir", default="")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2))
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
    env["PATH"] = (str(toolchain / "llvm-22.1.8" / "bin") + os.pathsep +
                   env.get("PATH", ""))
    return env


def run(command: list[str], label: str, cwd: pathlib.Path,
        env: dict[str, str], timeout: float) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    try:
        return subprocess.run(command, cwd=cwd, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              check=False, timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"{label}: timed out after {timeout:g}s")
    except OSError as error:
        fail(f"{label}: {error}")


def expect_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label}: exit {result.returncode}\n"
             f"stdout:\n{result.stdout}stderr:\n{result.stderr}")
    print(f"PASS: {label}")


def find_package(package: str, extras: list[pathlib.Path]) -> pathlib.Path | None:
    local = ROOT / "selfhost/src/wzcc" / package
    if local.is_dir() and any(local.glob("*.wz")):
        return local
    for extra in extras:
        candidate = extra / "wzcc" / package
        if candidate.is_dir() and any(candidate.glob("*.wz")):
            return candidate
    return None


def stage_project(work: pathlib.Path, sources: dict[str, pathlib.Path]) -> pathlib.Path:
    project = work / "project"
    src = project / "src"
    modules = src / "wzcc"
    modules.mkdir(parents=True)
    for package, path in sources.items():
        shutil.copytree(path, modules / package)
    verifier = modules / "nativeverify"
    shutil.copy2(HERE / "trace.wz", verifier / "obligations/trace.wz")
    # This test covers verifier internals without including the public facade.
    (verifier / "nativeverify.wz").unlink()
    shutil.copy2(HERE / "main.wz", src / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    complete_import_closure(src, ROOT / "selfhost/src/wzcc")
    return project


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    hir_library = pathlib.Path(args.hir_library).expanduser().resolve()
    toolchain = pathlib.Path(args.toolchain_root).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    if not hir_library.is_file():
        fail(f"accepted liblang_hir.a missing: {hir_library}")
    if not (toolchain / "llvm-22.1.8/bin/clang").is_file():
        fail(f"toolchain clang missing under {toolchain}")
    extras = [pathlib.Path(item).expanduser().resolve()
              for item in args.extra_source]
    sources = {}
    missing = []
    for package in PACKAGES:
        path = find_package(package, extras)
        if path is None:
            missing.append(package)
        else:
            sources[package] = path
    if missing:
        print("status: not_run")
        print("reason: required packages are absent")
        for name in missing:
            print(f"  missing wzcc/{name}")
        return 0
    levels = args.opt or [0, 2]
    if args.work_dir:
        work = pathlib.Path(args.work_dir).expanduser().resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        work = pathlib.Path(tempfile.mkdtemp(prefix="woozy-obligations-test-"))
    env = environment(toolchain)
    print(f"stage0: {stage0}")
    print(f"work-dir: {work}")
    project = stage_project(work, sources)
    oracle = work / "oracle"
    command = [
        "c++", "-std=c++23", "-O2",
        "-I", str(ROOT / "include"), "-I", str(ROOT / "src"),
        str(HERE / "oracle.cpp"), str(hir_library),
        str(hir_library.parent / "liblang_frontend.a"), str(hir_library),
        "-lgmpxx", "-lgmp", "-o", str(oracle),
    ]
    expect_ok(run(command, "compile oracle", work, env, args.timeout),
              "compile oracle")
    expected = run([str(oracle)], "oracle", work, env, args.timeout)
    expect_ok(expected, "oracle")
    manifest = str(project / "lang.toml")
    expect_ok(run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "Wz check", work, env,
                  args.timeout), "Wz check")
    traces = []
    for level in levels:
        out_dir = work / f"opt{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        expect_ok(run([str(stage0), "build", "--project", manifest,
                       "--backend", "native", "--opt", str(level),
                       "--out-dir", str(out_dir)], f"Wz build O{level}",
                      work, env, args.timeout), f"Wz build O{level}")
        program = out_dir / "program"
        if not program.is_file():
            fail(f"O{level}: missing {program}")
        values = run([str(program)], f"Wz obligations O{level}", work, env,
                     args.timeout)
        expect_ok(values, f"Wz obligations O{level}")
        if values.stdout != expected.stdout:
            diff = "".join(difflib.unified_diff(
                expected.stdout.splitlines(keepends=True),
                values.stdout.splitlines(keepends=True),
                fromfile="C++ obligations.hpp oracle", tofile=f"Wz O{level}"))
            fail(f"Wz O{level} obligations differ from C++ oracle\n{diff}")
        print(f"PASS: Wz O{level} obligations match C++ oracle "
              f"({len(values.stdout.splitlines())} lines)")
        traces.append((level, values.stdout))
    if len(traces) > 1 and any(trace != traces[0][1] for _, trace in traces[1:]):
        fail("O0 and O2 obligation traces differ")
    print(f"PASS: Native obligations tests; work dir {work}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
