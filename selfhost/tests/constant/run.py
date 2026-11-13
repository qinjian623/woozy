#!/usr/bin/env python3
"""Check/build/run the literal+constant canonical and regression suites.

Both suites are staged in the runner's work directory with symlinks to the
real `selfhost/src/wzcc` packages.  The accepted `hirclone` package is also
linked from this checkout, or from the explicit --hirclone directory.
No production module is copied into the test directory.

For each suite the runner:
1. checks the staged project with stage0,
2. builds/runs it at every requested optimization level,
3. builds/runs a small C++ oracle against the existing stage0 libraries,
4. compares the complete canonical output line by line.

Defaults use this checkout and the neighboring toolchain directory;
paths can be overridden without rebuilding the C++ compiler.
"""

from __future__ import annotations

import argparse
import difflib
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_COMPILER_ROOT = ROOT
DEFAULT_STAGE0 = (DEFAULT_COMPILER_ROOT / "build" / "woozy-stdlib-native"
                  / "woozyc")
DEFAULT_WORK = ROOT / "build" / "constant-tests"
DEFAULT_LD_LIBRARY_PATH = [
    ROOT.parent / "toolchains/llvm-22.1.8/lib",
    ROOT.parent / "toolchains/libxml2-legacy-2.13.9/usr/lib",
]
DEFAULT_HIRCLONE = [
    ROOT / "selfhost" / "src" / "wzcc" / "hirclone",
]
DEFAULT_TIMEOUT = 300.0
PACKAGES = [
    "bigint", "constant", "hir", "hirclone", "lexer", "literal", "model",
    "number", "parser", "source", "types",
]


class Suite:
    def __init__(self, name: str, main: Path, oracle: Path) -> None:
        self.name = name
        self.main = main
        self.oracle = oracle


SUITES = [
    Suite("canonical", HERE / "src" / "main.wz", HERE / "oracle.cpp"),
    Suite("regression", HERE / "regression" / "src" / "main.wz",
          HERE / "regression" / "oracle.cpp"),
]


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def environment(args: argparse.Namespace, work: Path) -> dict[str, str]:
    env = dict(os.environ)
    paths = [str(path) for path in args.ld_library_path]
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        paths.append(previous)
    if paths:
        env["LD_LIBRARY_PATH"] = ":".join(paths)
    # Keep assembler temporaries on the same filesystem as the runner output.
    env["TMPDIR"] = str(work)
    return env


def run(command: list[str], label: str, cwd: Path,
        timeout: float, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    print("+ " + " ".join(command), flush=True)
    try:
        return subprocess.run(
            command, cwd=cwd, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"{label}: timed out after {timeout:g}s")
    except OSError as error:
        fail(f"{label}: {error}")


def expect_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label}: exit {result.returncode}\n"
             f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    if result.stderr:
        fail(f"{label}: unexpected stderr\n{result.stderr}")


def resolve_hirclone(args: argparse.Namespace) -> Path:
    candidates = ([Path(args.hirclone)] if args.hirclone
                  else list(DEFAULT_HIRCLONE))
    for candidate in candidates:
        path = candidate.expanduser().resolve()
        if (path / "clone.wz").is_file():
            return path
    fail("accepted hirclone package not found; pass --hirclone PATH "
         f"(tried: {', '.join(str(path) for path in candidates)})")


def stage_project(project: Path, suite: Suite,
                  hirclone: Path) -> Path:
    if project.exists():
        shutil.rmtree(project)
    source = project / "src"
    source.mkdir(parents=True)
    dependency = project / "dependency"
    wzcc = dependency / "src" / "wzcc"
    wzcc.mkdir(parents=True)
    (dependency / "lang.toml").write_text(
        "schema_version = 1\n"
        "[package]\n"
        'name = "constant_accepted_dependency"\n'
        'source_root = "src"\n'
        'entry = "app"\n'
        "[build]\n"
        'backend = "native"\n'
        'word_bits = 64\n', encoding="utf-8")
    for package in PACKAGES:
        target = (hirclone if package == "hirclone"
                  else ROOT / "selfhost" / "src" / "wzcc" / package)
        if not target.is_dir():
            fail(f"required package missing: {target}")
        destination = wzcc / package
        try:
            destination.symlink_to(target, target_is_directory=True)
        except OSError:
            shutil.copytree(target, destination)
    try:
        (source / "main.wz").symlink_to(suite.main)
    except OSError:
        shutil.copy2(suite.main, source / "main.wz")
    manifest = project / "lang.toml"
    manifest.write_text(
        "schema_version = 1\n"
        "[package]\n"
        f'name = "{suite.name}_test"\n'
        'source_root = "src"\n'
        'entry = "app"\n'
        "[build]\n"
        'backend = "native"\n'
        'word_bits = 64\n'
        "[dependencies.compiler]\n"
        'kind = "source"\n'
        'path = "dependency"\n', encoding="utf-8")
    return manifest


def oracle_command(args: argparse.Namespace, suite: Suite,
                   output: Path) -> list[str]:
    compiler_root = Path(args.compiler_root).expanduser().resolve()
    lib_dir = (Path(args.lib_dir).expanduser().resolve()
               if args.lib_dir
               else compiler_root / "build" / "woozy-stdlib-native")
    command = [
        args.cxx, "-std=c++20", f"-O{args.oracle_opt}",
        f"-I{compiler_root / 'include'}", f"-I{compiler_root / 'src'}",
        str(suite.oracle),
        str(lib_dir / "liblang_hir.a"),
        str(lib_dir / "liblang_modules.a"),
        str(lib_dir / "liblang_frontend.a"),
    ]
    command.extend(args.oracle_link)
    command.extend(["-lgmpxx", "-lgmp", "-ljson-c", "-o", str(output)])
    return command


def run_suite(suite: Suite, args: argparse.Namespace, work: Path,
              env: dict[str, str], hirclone: Path, levels: list[int]) -> int:
    suite_work = work / suite.name
    suite_work.mkdir(parents=True, exist_ok=True)
    project = suite_work / "project"
    manifest = stage_project(project, suite, hirclone)
    print(f"suite: {suite.name}")
    print(f"  project: {project}")
    print(f"  main: {suite.main}")

    oracle_binary = suite_work / "oracle"
    if oracle_binary.exists():
        oracle_binary.unlink()
    built = run(oracle_command(args, suite, oracle_binary),
                f"{suite.name} oracle build", suite_work,
                args.timeout, env)
    expect_ok(built, f"{suite.name} oracle build")
    oracle_run = run([str(oracle_binary)], f"{suite.name} oracle run",
                     suite_work, args.timeout, env)
    expect_ok(oracle_run, f"{suite.name} oracle run")
    oracle_lines = oracle_run.stdout.splitlines()

    checked = run([str(args.stage0), "check", "--project", str(manifest),
                   "--backend", "native"], f"{suite.name} Wz check",
                  suite_work, args.timeout, env)
    expect_ok(checked, f"{suite.name} Wz check")

    for level in levels:
        out_dir = suite_work / f"out-o{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        build = run([
            str(args.stage0), "build", "--project", str(manifest),
            "--backend", "native", "--opt", str(level),
            "--out-dir", str(out_dir), "--format", "json",
        ], f"{suite.name} Wz build O{level}", suite_work,
           args.timeout, env)
        expect_ok(build, f"{suite.name} Wz build O{level}")
        program = out_dir / "program"
        if not program.is_file():
            fail(f"{suite.name} O{level}: missing built program {program}")
        test = run([str(program)], f"{suite.name} Wz run O{level}",
                   suite_work, args.timeout, env)
        expect_ok(test, f"{suite.name} Wz run O{level}")
        actual = test.stdout.splitlines()
        if actual != oracle_lines:
            diff = "\n".join(difflib.unified_diff(
                oracle_lines, actual, fromfile="cpp-oracle",
                tofile=f"wz-o{level}", lineterm=""))
            fail(f"{suite.name} O{level}: {len(oracle_lines)} oracle lines "
                 f"differ from {len(actual)} Wz lines\n{diff}")
        print(f"PASS: {suite.name} O{level}: {len(actual)} canonical lines "
              f"match the C++ oracle exactly")
    return len(oracle_lines)


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check/build/run the canonical and regression literal/"
                    "constant suites and compare them with the C++ algorithms.")
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0),
                        help=f"stage-0 compiler (default: {DEFAULT_STAGE0})")
    parser.add_argument("--cxx", default="g++",
                        help="C++ compiler used only for the test oracle")
    parser.add_argument("--compiler-root", default=str(DEFAULT_COMPILER_ROOT),
                        help="existing C++ tree with include/, src/ and libs")
    parser.add_argument("--lib-dir", default="",
                        help="directory with liblang_{hir,modules,frontend}.a")
    parser.add_argument("--hirclone", default="",
                        help="accepted hirclone package (default: worktree "
                             "then Root accepted path)")
    parser.add_argument("--oracle-link", action="append", default=[],
                        help="extra oracle linker flag (repeatable)")
    parser.add_argument("--oracle-opt", type=int, choices=(0, 2), default=0,
                        help="optimization level for the C++ oracle")
    parser.add_argument("--ld-library-path", action="append", default=[],
                        help="extra LD_LIBRARY_PATH entry (repeatable)")
    parser.add_argument("--work-dir", default=str(DEFAULT_WORK),
                        help=f"output directory (default: {DEFAULT_WORK})")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"per-command timeout (default: {DEFAULT_TIMEOUT:g})")
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2),
                        help="optimization level (default: 0 and 2)")
    args = parser.parse_args(argv)
    args.stage0 = Path(args.woozyc).expanduser().resolve()
    args.ld_library_path = [Path(path) for path in
                            (args.ld_library_path or
                             [str(path) for path in DEFAULT_LD_LIBRARY_PATH])]
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    compiler_root = Path(args.compiler_root).expanduser().resolve()
    work = Path(args.work_dir).expanduser().resolve()
    if not args.stage0.is_file():
        fail(f"stage0 compiler missing: {args.stage0}")
    lib_dir = (Path(args.lib_dir).expanduser().resolve()
               if args.lib_dir
               else compiler_root / "build" / "woozy-stdlib-native")
    for library in ("liblang_hir.a", "liblang_modules.a", "liblang_frontend.a"):
        if not (lib_dir / library).is_file():
            fail(f"oracle library missing: {lib_dir / library}")
    for suite in SUITES:
        if not suite.main.is_file():
            fail(f"{suite.name} main missing: {suite.main}")
        if not suite.oracle.is_file():
            fail(f"{suite.name} oracle source missing: {suite.oracle}")
    hirclone = resolve_hirclone(args)
    levels = args.opt or [0, 2]
    work.mkdir(parents=True, exist_ok=True)
    env = environment(args, work)
    print(f"stage0: {args.stage0}")
    print(f"compiler-root: {compiler_root}")
    print(f"lib-dir: {lib_dir}")
    print(f"hirclone: {hirclone}")
    print(f"work-dir: {work}")

    counts = {}
    for suite in SUITES:
        counts[suite.name] = run_suite(suite, args, work, env, hirclone,
                                       levels)
    print("PASS: literal+constant acceptance ("
          + ", ".join(f"{name} {count} lines" for name, count in counts.items())
          + f"; check, O{'/O'.join(str(level) for level in levels)}; "
          f"work dir {work})")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
