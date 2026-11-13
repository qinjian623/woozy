#!/usr/bin/env python3
"""Byte-exact comparison of the pure-Wz HIR dumps with the C++ oracle.

The repository tree is never modified.  A copied project containing the
production model/types/HIR modules, the Wz hirdump package and this directory's
main.wz is checked and built at O0 and O2.  A small test-only C++ translation
unit links against the existing stage0 libraries, parses the same real fixture
source, builds the same TypeContext/HirModule records and prints the reference
dumps.  Every full dump and the invalid-word_bits error path must compare byte
for byte.

--woozyc, --cxx, --library-dir, --include-dir and --work-dir allow a verifier
to use absolute stage0/toolchain paths without rebuilding the C++ compiler.
"""

from __future__ import annotations

import argparse
import difflib
import os
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_STAGE0 = ROOT / "build/woozy-stdlib-native/woozyc"
DEFAULT_TOOLCHAIN = ROOT.parent / "toolchains"
DEFAULT_WORK = ROOT / "build" / "hirdump-tests"
DEFAULT_TIMEOUT = 300.0

COPY_DIRS = ["model", "hir", "types", "hirdump", "source", "lexer", "parser"]
LIBRARIES = ["liblang_hir.a", "liblang_modules.a", "liblang_frontend.a"]


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


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


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check, build and byte-compare the Wz HIR dumps.")
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0),
                        help=f"stage0 compiler (default: {DEFAULT_STAGE0})")
    parser.add_argument("--cxx", default="clang++",
                        help="C++ compiler for the test oracle (default: clang++)")
    parser.add_argument("--library-dir", default=None,
                        help="directory with liblang_*.a (default: stage0 parent)")
    parser.add_argument("--include-dir", default=str(ROOT / "include"),
                        help=f"C++ include root (default: {ROOT / 'include'})")
    parser.add_argument("--toolchain-root", default=str(DEFAULT_TOOLCHAIN),
                        help=f"LLVM/libxml2 toolchain root (default: {DEFAULT_TOOLCHAIN})")
    parser.add_argument("--fixture", default=str(HERE / "fixture.wz"),
                        help=f"real Source/AST fixture (default: {HERE / 'fixture.wz'})")
    parser.add_argument("--work-dir", default=str(DEFAULT_WORK),
                        help=f"output/copy directory (default: {DEFAULT_WORK})")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"timeout in seconds for each command (default: {DEFAULT_TIMEOUT:g})")
    return parser.parse_args(argv)


def run(command: list[str], label: str, work: pathlib.Path,
        env: dict[str, str], timeout: float) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    try:
        return subprocess.run(
            command, cwd=work, env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"{label}: timed out after {timeout:g}s")
    except OSError as error:
        fail(f"{label}: {error}")


def expect_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label}: exit {result.returncode}\nstdout:\n{result.stdout}"
             f"stderr:\n{result.stderr}")
    print(f"PASS: {label}")


def copy_project(work: pathlib.Path) -> pathlib.Path:
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    (project / "src" / "wzcc").mkdir(parents=True)
    for directory in COPY_DIRS:
        target = project / "src" / "wzcc" / directory
        target.mkdir(parents=True, exist_ok=True)
        for source in sorted((ROOT / "selfhost" / "src" / "wzcc" / directory).glob("*.wz")):
            shutil.copy2(source, target / source.name)
    shutil.copy2(HERE / "main.wz", project / "src" / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def build_oracle(args: argparse.Namespace, work: pathlib.Path,
                 env: dict[str, str]) -> pathlib.Path:
    library_dir = pathlib.Path(args.library_dir).expanduser().resolve() \
        if args.library_dir else pathlib.Path(args.woozyc).expanduser().resolve().parent
    include_dir = pathlib.Path(args.include_dir).expanduser().resolve()
    oracle_source = HERE / "oracle.cpp"
    missing = [name for name in LIBRARIES if not (library_dir / name).is_file()]
    if missing:
        fail(f"missing stage0 libraries in {library_dir}: {', '.join(missing)}")
    if not oracle_source.is_file():
        fail(f"missing oracle source: {oracle_source}")
    if not (include_dir / "lang" / "hir" / "hir.hpp").is_file():
        fail(f"missing C++ headers under {include_dir}")
    output = work / "oracle"
    command = [
        args.cxx, "-std=c++20", "-O2", "-I", str(include_dir),
        "-isystem", "/usr/include/json-c", str(oracle_source), "-o", str(output),
        str(library_dir / "liblang_hir.a"),
        str(library_dir / "liblang_modules.a"),
        str(library_dir / "liblang_frontend.a"),
        "-ltomlplusplus", "-ljson-c", "-lm", "-lgmpxx", "-lgmp",
    ]
    expect_ok(run(command, "oracle build", work, env, args.timeout),
              "C++ oracle build")
    return output


def compare(label: str, expected: bytes, actual: bytes) -> None:
    if expected == actual:
        print(f"PASS: {label}: {len(actual)} bytes identical")
        return
    expected_text = expected.decode("utf-8", "replace").splitlines(keepends=True)
    actual_text = actual.decode("utf-8", "replace").splitlines(keepends=True)
    diff = "".join(difflib.unified_diff(
        expected_text, actual_text, fromfile="cpp", tofile="wz", n=3))
    fail(f"{label}: byte mismatch (cpp={len(expected)}, wz={len(actual)})\n"
         f"{diff[:8000]}")


def coverage(text: str) -> None:
    type_kinds = {int(m.group(1)) for m in
                  re.finditer(r"^type \d+ kind=(\d+) ", text, re.MULTILINE)}
    node_kinds = {int(m.group(1)) for m in
                  re.finditer(r"^node \d+ kind=(\d+) ", text, re.MULTILINE)}
    missing_types = sorted(set(range(31)) - type_kinds)
    missing_nodes = sorted(set(range(45)) - node_kinds)
    if missing_types:
        fail(f"fixture/dump misses TypeKind ids: {missing_types}")
    if missing_nodes:
        fail(f"fixture/dump misses NodeData/NodeKind ids: {missing_nodes}")
    required = [
        "nominal ", "variant ", "trait ", "prototype ", "impl ",
        "template ", "instance ", "function ", "scope ", "foreign ",
        "use ", "temporary ", "lifetime ", "full_expression ", "call_return ",
        " exit=", " action=", " capture=", " const=", " variant=",
        " bound=", " read_only_capture", " iterator_cursor=",
        " moved_binding=", " slice_start=", " foreign_read=",
        " c_record=", " vtable_index=", " exhaustive=",
    ]
    missing = [marker for marker in required if marker not in text]
    if missing:
        fail(f"dump misses required payload markers: {missing}")

    def ids(pattern: str) -> set[int]:
        return {int(m.group(1)) for m in re.finditer(pattern, text, re.MULTILINE)}

    expected = {
        "Unary/Binary/Logical op": (ids(r" op=(\d+)"), set(range(16))),
        "CalleeKind": (ids(r"callee_kind=(\d+)"), set(range(5))),
        "UseKind": (ids(r"^use \d+:\d+:(\d+):"), set(range(4))),
        "ExitKind": (ids(r" exit=(\d+) "), set(range(6))),
        "CleanupKind": (ids(r" action=(\d+):"), set(range(5))),
        "OwnerKind": (ids(r" action=\d+:(\d+):"), set(range(5))),
        "ParamMode": (ids(r" p=(\d+):"), set(range(3))),
        "Backend": (ids(r"backend=(\d+)"), set(range(2))),
        "ConstantData": (ids(r" const=\d+:(\d+)"), set(range(7))),
    }
    for name, (found, wanted) in expected.items():
        if not wanted.issubset(found):
            fail(f"dump misses {name} ordinals: {sorted(wanted - found)}")
    for marker in ("bool:", "int:", "float:", "string:"):
        if marker not in text:
            fail(f"dump misses pattern literal marker {marker}")
    print(f"PASS: coverage: all TypeKind/NodeData ids plus Unary/Binary/"
          f"Logical/Callee/Use/Exit/Cleanup/Owner/ParamMode/Backend/"
          f"ConstantData ordinals and pattern literals present")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    work = pathlib.Path(args.work_dir).expanduser().resolve()
    fixture = pathlib.Path(args.fixture).expanduser().resolve()
    toolchain = pathlib.Path(args.toolchain_root).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    if not fixture.is_file():
        fail(f"fixture source missing: {fixture}")
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")
    work.mkdir(parents=True, exist_ok=True)
    env = environment(toolchain)
    print(f"stage0: {stage0}")
    print(f"fixture: {fixture}")
    print(f"work-dir: {work}")

    oracle = build_oracle(args, work, env)
    project = copy_project(work)
    manifest = str(project / "lang.toml")

    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "check", work, env, args.timeout)
    expect_ok(checked, "stage0 check")

    cpp_full = run([str(oracle), str(fixture)], "oracle full", work, env,
                   args.timeout)
    expect_ok(cpp_full, "C++ oracle full dump")
    cpp_text = cpp_full.stdout
    coverage(cpp_text)
    cpp_full_path = work / "cpp-full.txt"
    cpp_full_path.write_text(cpp_text, encoding="utf-8")

    cpp_error = run([str(oracle), str(fixture), "width-error"],
                    "oracle width-error", work, env, args.timeout)
    expect_ok(cpp_error, "C++ oracle invalid-word_bits exception")
    if cpp_error.stdout != "ERROR: target word_bits\n":
        fail(f"unexpected C++ width-error text: {cpp_error.stdout!r}")

    outputs: dict[str, str] = {}
    for opt in ("0", "2"):
        out = work / f"opt{opt}"
        if out.exists():
            shutil.rmtree(out)
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", opt,
                     "--out-dir", str(out)], f"build O{opt}", work, env,
                    args.timeout)
        expect_ok(built, f"stage0 build O{opt}")
        program = out / "program"
        if not program.is_file():
            fail(f"O{opt}: missing {program}")
        values = run([str(program), str(fixture)], f"full O{opt}", work, env,
                     args.timeout)
        expect_ok(values, f"Wz full dump O{opt}")
        outputs[opt] = values.stdout
        compare(f"O{opt} full dump", cpp_text.encode("utf-8"),
                values.stdout.encode("utf-8"))
        if values.stdout != cpp_text:
            fail(f"O{opt}: stdout differs from oracle")
        error = run([str(program), str(fixture), "width-error"],
                    f"width-error O{opt}", work, env, args.timeout)
        expect_ok(error, f"Wz invalid-word_bits O{opt}")
        compare(f"O{opt} width-error", cpp_error.stdout.encode("utf-8"),
                error.stdout.encode("utf-8"))

    if outputs["0"] != outputs["2"]:
        fail("O0 and O2 full dumps differ")
    print("PASS: HIR dumps byte-identical to C++ at O0 and O2, including "
          "all type/node records, optional branches and the width error")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
