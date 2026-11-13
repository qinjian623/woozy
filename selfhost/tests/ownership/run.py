#!/usr/bin/env python3
"""Check, build and execute the pure-Wz ownership scenarios at O0 and O2.

The test project is staged in a temporary (or requested) directory from the
repository's model/types/hir/valueops/ownership packages.  No production module
copy is left behind.  --woozyc selects the stage-0 compiler; --cxx and the two
library options are accepted so an external verifier can supply the same
toolchain paths used by the stage-0 build.  --oracle compiles the small
test-only oracle.cpp against the existing C++ libraries and compares every
classification, helper, ownership record and exit-plan field/order line at both
optimization levels.  Program runs get a larger stack so the 512-depth
classification budget probe reaches its typed budget failure.
"""

from __future__ import annotations

import argparse
import difflib
import os
import resource
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SOURCE = ROOT / "selfhost" / "src" / "wzcc"
DEFAULT_WOOZYC = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
DEFAULT_TIMEOUT = 600.0

MANIFEST = """schema_version = 1
[package]
name = "ownership_selfhost_test"
source_root = "src"
entry = "app"
[build]
backend = "native"
word_bits = 64
"""

PACKAGES = ("model", "types", "hir", "valueops", "ownership", "source",
            "lexer", "parser")
INDEPENDENT = HERE / "independent"


def link_packages(source: Path) -> None:
    (source / "wzcc").mkdir(parents=True)
    for name in PACKAGES:
        target = SOURCE / name
        link = source / "wzcc" / name
        try:
            link.symlink_to(target, target_is_directory=True)
        except OSError:
            shutil.copytree(target, link)


def stage(work: Path, name: str, main: Path) -> Path:
    project = work / name
    if project.exists():
        shutil.rmtree(project)
    source = project / "src"
    link_packages(source)
    shutil.copy2(main, source / "main.wz")
    (project / "lang.toml").write_text(MANIFEST, encoding="utf-8")
    return project


def build_oracle(source: Path, output: Path, woozyc: Path,
                 args: argparse.Namespace, work: Path,
                 env: dict[str, str]) -> list[str]:
    if not source.is_file():
        print(f"FAIL: oracle source missing: {source}", file=sys.stderr)
        raise SystemExit(2)
    compiler = args.cxx or "c++"
    built = run([
        compiler, "-std=c++20", "-O2",
        "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src"),
        str(source),
        "-L" + str(woozyc.parent), "-llang_hir", "-llang_modules",
        "-llang_sema", "-llang_frontend", "-lgmpxx", "-lgmp",
        "-o", str(output),
    ], f"oracle build {source.name}", work, args.timeout, env)
    if built.returncode != 0:
        print(built.stdout, end="")
        print(built.stderr, end="", file=sys.stderr)
        print(f"FAIL: oracle build {source.name}", file=sys.stderr)
        raise SystemExit(1)
    tested = run([str(output)], f"oracle run {source.name}", work,
                 args.timeout, env)
    if tested.returncode != 0:
        print(tested.stdout, end="")
        print(tested.stderr, end="", file=sys.stderr)
        print(f"FAIL: oracle run {source.name}", file=sys.stderr)
        raise SystemExit(1)
    return tested.stdout.splitlines()


def environment(args: argparse.Namespace) -> dict[str, str]:
    env = dict(os.environ)
    parts = []
    if args.llvm_lib:
        parts.append(str(Path(args.llvm_lib).expanduser().resolve()))
    if args.xml_lib:
        parts.append(str(Path(args.xml_lib).expanduser().resolve()))
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        parts.append(previous)
    if parts:
        env["LD_LIBRARY_PATH"] = ":".join(parts)
    return env


def larger_stack() -> None:
    limit = 64 * 1024 * 1024
    soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
    if hard != resource.RLIM_INFINITY:
        limit = min(limit, hard)
    resource.setrlimit(resource.RLIMIT_STACK, (max(soft, limit), hard))


def run(command: list[str], label: str, cwd: Path, timeout: float,
        env: dict[str, str], stack: bool = False) -> subprocess.CompletedProcess[str]:
    print("+ " + " ".join(command), flush=True)
    try:
        return subprocess.run(command, cwd=cwd, env=env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              check=False, timeout=timeout,
                              preexec_fn=larger_stack if stack else None)
    except subprocess.TimeoutExpired:
        print(f"FAIL: {label}: timeout after {timeout:g}s", file=sys.stderr)
        raise SystemExit(124)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--woozyc", default=str(DEFAULT_WOOZYC),
                        help="stage-0 compiler (default: %(default)s)")
    parser.add_argument("--cxx", default="c++",
                        help="C++ compiler for the test-only oracle")
    parser.add_argument("--oracle", action="store_true",
                        help="compile oracle.cpp and compare every ownership field/order line")
    parser.add_argument("--oracle-source", default=str(HERE / "oracle.cpp"),
                        help="test-only C++ oracle source")
    parser.add_argument("--llvm-lib", default=str(ROOT.parent / "toolchains/llvm-22.1.8/lib"),
                        help="LLVM runtime library directory")
    parser.add_argument("--xml-lib", default=str(ROOT.parent / "toolchains/libxml2-legacy-2.13.9/usr/lib"),
                        help="libxml2 legacy library directory")
    parser.add_argument("--work-dir", default="",
                        help="staging directory (default: new temp dir)")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help="per-command timeout (default: %(default)g)")
    args = parser.parse_args(argv)

    woozyc = Path(args.woozyc).expanduser().resolve()
    if not woozyc.is_file():
        print(f"FAIL: stage-0 compiler missing: {woozyc}", file=sys.stderr)
        return 2
    work = (Path(args.work_dir).expanduser().resolve() if args.work_dir
            else Path(tempfile.mkdtemp(prefix="woozy-ownership-test-")))
    work.mkdir(parents=True, exist_ok=True)
    project = stage(work, "project", HERE / "src" / "main.wz")
    manifest = str(project / "lang.toml")
    env = environment(args)

    checked = run([str(woozyc), "check", "--project", manifest,
                   "--backend", "native"], "check", work, args.timeout, env)
    if checked.returncode != 0:
        print(checked.stdout, end="")
        print(checked.stderr, end="", file=sys.stderr)
        print("FAIL: stage-0 check", file=sys.stderr)
        return 1
    print("PASS: ownership check")

    oracle_lines: list[str] = []
    independent_oracle_lines: list[str] = []
    if args.oracle:
        oracle_lines = [line for line in build_oracle(
            Path(args.oracle_source).expanduser().resolve(), work / "oracle",
            woozyc, args, work, env) if line.startswith("ORACLE ")]
        print(f"PASS: C++ ownership oracle ({len(oracle_lines)} lines)")
        independent_oracle_lines = build_oracle(
            INDEPENDENT / "oracle.cpp", work / "independent-oracle", woozyc,
            args, work, env)
        print(f"PASS: C++ independent ownership oracle "
              f"({len(independent_oracle_lines)} lines)")

    for level in (0, 2):
        out_dir = work / f"out-o{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        built = run([str(woozyc), "build", "--project", manifest,
                     "--backend", "native", "--out-dir", str(out_dir),
                     "--opt", str(level), "--format", "json"],
                    f"build O{level}", work, args.timeout, env)
        if built.returncode != 0:
            print(built.stdout, end="")
            print(built.stderr, end="", file=sys.stderr)
            print(f"FAIL: ownership build O{level}", file=sys.stderr)
            return 1
        program = out_dir / "program"
        if not program.is_file():
            print(f"FAIL: missing program {program}", file=sys.stderr)
            return 1
        tested = run([str(program)], f"run O{level}", project, args.timeout,
                     env, stack=True)
        print(tested.stdout, end="")
        if tested.stderr:
            print(tested.stderr, end="", file=sys.stderr)
        if args.oracle:
            selfhost_lines = [line for line in tested.stdout.splitlines()
                              if line.startswith("ORACLE ")]
            if selfhost_lines != oracle_lines:
                diff = difflib.unified_diff(oracle_lines, selfhost_lines,
                                            fromfile="C++ oracle",
                                            tofile=f"selfhost O{level}",
                                            lineterm="")
                print("\n".join(diff), file=sys.stderr)
                print(f"FAIL: C++/selfhost ownership field diff at O{level}",
                      file=sys.stderr)
                return 1
            print(f"PASS: C++/selfhost ownership field comparison at O{level}")
        if tested.returncode != 0 or "PASS: ownership scenarios" not in tested.stdout:
            print(f"FAIL: ownership scenarios at O{level} "
                  f"(exit {tested.returncode})", file=sys.stderr)
            return 1
        print(f"PASS: ownership scenarios at O{level}")

    independent = stage(work, "independent-project",
                        INDEPENDENT / "main.wz")
    independent_manifest = str(independent / "lang.toml")
    checked_independent = run([str(woozyc), "check", "--project",
                               independent_manifest, "--backend", "native"],
                              "independent check", work, args.timeout, env)
    if checked_independent.returncode != 0:
        print(checked_independent.stdout, end="")
        print(checked_independent.stderr, end="", file=sys.stderr)
        print("FAIL: independent ownership check", file=sys.stderr)
        return 1
    print("PASS: independent ownership check")
    for level in (0, 2):
        out_dir = independent / f"out-o{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        built = run([str(woozyc), "build", "--project",
                     independent_manifest, "--backend", "native",
                     "--out-dir", str(out_dir), "--opt", str(level),
                     "--format", "json"], f"independent build O{level}", work,
                    args.timeout, env)
        if built.returncode != 0:
            print(built.stdout, end="")
            print(built.stderr, end="", file=sys.stderr)
            print(f"FAIL: independent ownership build O{level}",
                  file=sys.stderr)
            return 1
        program = out_dir / "program"
        if not program.is_file():
            print(f"FAIL: missing independent program {program}",
                  file=sys.stderr)
            return 1
        tested = run([str(program)], f"independent run O{level}",
                     independent, args.timeout, env, stack=True)
        if tested.stderr:
            print(tested.stderr, end="", file=sys.stderr)
        if tested.returncode != 0:
            print(tested.stdout, end="")
            print(f"FAIL: independent scenarios at O{level} "
                  f"(exit {tested.returncode})", file=sys.stderr)
            return 1
        lines = tested.stdout.splitlines()
        if args.oracle:
            if lines != independent_oracle_lines:
                diff = difflib.unified_diff(independent_oracle_lines, lines,
                                            fromfile="C++ independent oracle",
                                            tofile=f"selfhost independent O{level}",
                                            lineterm="")
                print("\n".join(diff), file=sys.stderr)
                print(f"FAIL: independent C++/selfhost field diff at O{level}",
                      file=sys.stderr)
                return 1
            print(f"PASS: independent C++/selfhost comparison at O{level} "
                  f"({len(lines)} lines)")
        else:
            print(f"PASS: independent scenarios at O{level} ({len(lines)} lines)")

    print(f"PASS: ownership O0/O2; work dir {work}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
