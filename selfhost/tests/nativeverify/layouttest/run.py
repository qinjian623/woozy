#!/usr/bin/env python3
"""Check, build and run the Native aggregate layout tests.

The Wz project is staged in a temporary directory from this repository's
model, hir, types and nativelayout packages plus layouttest/main.wz.  The C++
oracle includes the frozen src/ir/layout.hpp and the accepted liblang_hir.a
and prints the same trace from lang::ir::detail::AggregateTypes.  Every
project is checked with stage0, built at O0 and O2, executed, and its stdout
compared byte-for-byte with the oracle.

The Wz program also performs isolation assertions that are not part of the
compared trace: a returned declaration cannot rewrite module.nominals, and the
private AggregateTypes context never appends to the public HirModule table.
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
DEFAULT_STAGE0 = ROOT / "build/woozy-stdlib-native/woozyc"
DEFAULT_TOOLCHAIN = ROOT.parent / "toolchains"
DEFAULT_HIR_LIBRARY = ROOT / "build/woozy-stdlib-native/liblang_hir.a"
DEFAULT_TIMEOUT = 240.0


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check, build and execute the Native layout tests.")
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0))
    parser.add_argument("--hir-library", default=str(DEFAULT_HIR_LIBRARY))
    parser.add_argument("--toolchain-root", default=str(DEFAULT_TOOLCHAIN))
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
    llvm_bin = str(toolchain / "llvm-22.1.8" / "bin")
    env["PATH"] = llvm_bin + os.pathsep + env.get("PATH", "")
    return env


def run(command: list[str], label: str, cwd: pathlib.Path,
        env: dict[str, str], timeout: float,
        capture: bool = True) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    try:
        return subprocess.run(
            command, cwd=cwd, env=env, text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE if capture else None,
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


def stage_project(work: pathlib.Path) -> pathlib.Path:
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    source = project / "src"
    wizard = source / "wzcc"
    for package, names in (
            ("model", ("model.wz", "project.wz", "types.wz", "support.wz")),
            ("hir", ("nodes.wz", "ownership.wz", "module.wz")),
            ("nativelayout", ("nativelayout.wz",))):
        target = wizard / package
        target.mkdir(parents=True)
        for name in names:
            shutil.copy2(ROOT / "selfhost/src/wzcc" / package / name,
                         target / name)
    types = wizard / "types"
    types.mkdir()
    for file in sorted((ROOT / "selfhost/src/wzcc/types").iterdir()):
        if file.is_file():
            shutil.copy2(file, types / file.name)
    shutil.copy2(HERE / "main.wz", source / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def build_oracle(work: pathlib.Path, hir_library: pathlib.Path,
                 oracle_libraries: list[pathlib.Path], env: dict[str, str],
                 timeout: float) -> pathlib.Path:
    oracle = work / "oracle"
    command = [
        "c++", "-std=c++23", "-O2",
        "-I", str(ROOT / "include"), "-I", str(ROOT / "src"),
        str(HERE / "oracle.cpp"), str(hir_library), str(oracle_libraries[0]),
        str(hir_library), "-lgmpxx", "-lgmp",
        "-o", str(oracle),
    ]
    result = run(command, "compile oracle", work, env, timeout)
    expect_ok(result, "compile oracle")
    return oracle


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
    if not (ROOT / "src/ir/layout.hpp").is_file():
        fail(f"frozen layout.hpp missing under {ROOT / 'src/ir'}")
    levels = args.opt or [0, 2]
    if args.work_dir:
        work = pathlib.Path(args.work_dir).expanduser().resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        work = pathlib.Path(tempfile.mkdtemp(prefix="woozy-layout-test-"))
    env = environment(toolchain)
    print(f"stage0: {stage0}")
    print(f"hir library: {hir_library}")
    print(f"work-dir: {work}")

    project = stage_project(work)
    oracle_libraries = [hir_library.parent / "liblang_frontend.a"]
    for library in oracle_libraries:
        if not library.is_file():
            fail(f"accepted support library missing: {library}")
    oracle = build_oracle(work, hir_library, oracle_libraries, env,
                          args.timeout)
    expected = run([str(oracle)], "oracle", work, env, args.timeout)
    expect_ok(expected, "oracle")
    manifest = str(project / "lang.toml")
    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "Wz check", work, env, args.timeout)
    expect_ok(checked, "Wz check")
    traces: list[tuple[int, str]] = []
    for level in levels:
        out_dir = work / f"opt{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", str(level),
                     "--out-dir", str(out_dir)], f"Wz build O{level}", work,
                    env, args.timeout)
        expect_ok(built, f"Wz build O{level}")
        program = out_dir / "program"
        if not program.is_file():
            fail(f"Wz O{level}: missing {program}")
        values = run([str(program)], f"Wz layout O{level}", work, env,
                     args.timeout)
        expect_ok(values, f"Wz layout O{level} (including isolation checks)")
        if values.stdout != expected.stdout:
            diff = "".join(difflib.unified_diff(
                expected.stdout.splitlines(keepends=True),
                values.stdout.splitlines(keepends=True),
                fromfile="C++ layout.hpp oracle", tofile=f"Wz O{level}"))
            fail(f"Wz O{level} trace differs from C++ layout oracle\n{diff}")
        print(f"PASS: Wz O{level} trace matches C++ layout oracle "
              f"({len(values.stdout.splitlines())} lines)")
        traces.append((level, values.stdout))
    if len(traces) > 1:
        for level, trace in traces[1:]:
            if trace != traces[0][1]:
                fail(f"O{traces[0][0]} and O{level} traces differ")
        print(f"PASS: O{'/O'.join(str(level) for level, _ in traces)} traces "
              "are identical")
    print(f"PASS: Native layout tests; work dir {work}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
