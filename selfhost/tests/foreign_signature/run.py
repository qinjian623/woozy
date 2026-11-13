#!/usr/bin/env python3
"""Differential tests for app/wzcc/foreign and app/wzcc/signature.

The C++ oracle is built from the stage-0 tree first and run; the real Wz
modules are then checked and built by stage0 at O0 and O2 in a copied project
and their output must be byte-identical to the oracle.  Production modules are
copied from the repository at run time; this directory stores no module copy.

Defaults are relative to the repository root.  A verifier can pass an absolute
--woozyc, --cxx, --lib-dir, --include-dir, --src-dir or --work-dir.  Stage-0
runtime libraries are taken from WOOZY_TOOLCHAIN_ROOT when it is set, or from
--ld-path.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_STAGE0 = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
DEFAULT_WORK = ROOT / "build" / "foreign-signature-tests"
DEFAULT_TIMEOUT = 1800.0
MODULES = ("model", "types", "hir", "system", "foreign", "signature")
ORACLE_LIBS = ("liblang_hir.a", "liblang_modules.a", "liblang_frontend.a")
EXPECTED_OPERATIONS = 213


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0),
                        help=f"stage0 compiler (default: {DEFAULT_STAGE0})")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"),
                        help="C++ compiler for the oracle (default: $CXX or c++)")
    parser.add_argument("--lib-dir", default=None,
                        help="directory with stage-0 static libraries "
                             "(default: the directory containing --woozyc)")
    parser.add_argument("--include-dir", default=str(ROOT / "include"),
                        help=f"C++ include directory (default: {ROOT / 'include'})")
    parser.add_argument("--src-dir", default=str(ROOT / "src"),
                        help=f"C++ source header directory (default: {ROOT / 'src'})")
    parser.add_argument("--work-dir", default=str(DEFAULT_WORK),
                        help=f"output/copy directory (default: {DEFAULT_WORK})")
    parser.add_argument("--toolchain-root", default=os.environ.get(
        "WOOZY_TOOLCHAIN_ROOT", str(ROOT.parent / "toolchains")),
        help="toolchain root with llvm-*/lib and libxml2-*/usr/lib "
             "(default: $WOOZY_TOOLCHAIN_ROOT or ../toolchains)")
    parser.add_argument("--ld-path", default="",
                        help="extra colon-separated stage0 runtime library "
                             "directories")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"timeout per command (default: {DEFAULT_TIMEOUT:g}s)")
    return parser.parse_args(argv)


def environment(args: argparse.Namespace) -> dict[str, str]:
    env = dict(os.environ)
    parts: list[str] = []
    root = args.toolchain_root.strip()
    if root:
        parts.append(f"{root}/llvm-22.1.8/lib")
        parts.append(f"{root}/libxml2-legacy-2.13.9/usr/lib")
    if args.ld_path.strip():
        parts.extend(part for part in args.ld_path.split(":") if part)
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        parts.append(previous)
    if parts:
        env["LD_LIBRARY_PATH"] = ":".join(parts)
    return env


def run(command: list[str], label: str, cwd: pathlib.Path,
        timeout: float, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
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
        fail(f"{label}: exit {result.returncode}\nstdout:\n{result.stdout}"
             f"stderr:\n{result.stderr}")
    print(f"PASS: {label}")


def copy_modules(project: pathlib.Path) -> None:
    for name in MODULES:
        source = ROOT / "selfhost" / "src" / "wzcc" / name
        if not source.is_dir():
            fail(f"missing production module directory: {source}")
        destination = project / "src" / "wzcc" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(source, destination)
    (project / "src").mkdir(parents=True, exist_ok=True)
    shutil.copy2(HERE / "main.wz", project / "src" / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")


def check_pure_sources() -> None:
    """The ported modules may import only app/wzcc and std modules."""
    for name in MODULES:
        directory = ROOT / "selfhost" / "src" / "wzcc" / name
        for source in sorted(directory.glob("*.wz")):
            text_value = source.read_text(encoding="utf-8")
            for line in text_value.splitlines():
                stripped = line.strip()
                if not stripped.startswith("import "):
                    continue
                path = stripped[len("import "):].split(" as ")[0].strip()
                path = path.strip('"')
                if not (path.startswith("app/wzcc/") or path.startswith("std/")):
                    fail(f"{source}: forbidden import {path!r}")
            for marker in ("woozyc", "lang_native_helper", "dlopen", "dlsym"):
                if marker in text_value:
                    fail(f"{source}: forbidden host/frontend marker {marker!r}")


def build_oracle(args: argparse.Namespace, work: pathlib.Path,
                 env: dict[str, str]) -> pathlib.Path:
    lib_dir = pathlib.Path(args.lib_dir).expanduser().resolve() \
        if args.lib_dir else pathlib.Path(args.woozyc).expanduser().resolve().parent
    include_dir = pathlib.Path(args.include_dir).expanduser().resolve()
    src_dir = pathlib.Path(args.src_dir).expanduser().resolve()
    source = HERE / "oracle.cpp"
    for required in (include_dir / "lang" / "hir" / "hir.hpp",
                     src_dir / "hir" / "foreign.hpp",
                     source):
        if not required.is_file():
            fail(f"missing oracle input: {required}")
    missing = [name for name in ORACLE_LIBS if not (lib_dir / name).is_file()]
    if missing:
        fail(f"oracle libraries not found in {lib_dir}: {', '.join(missing)} "
             "(pass --lib-dir)")
    binary = work / "bin" / "oracle"
    binary.parent.mkdir(parents=True, exist_ok=True)
    command = [args.cxx, "-std=c++20", "-O2", "-I", str(include_dir), "-I",
               str(src_dir), str(source)]
    command += [str(lib_dir / name) for name in ORACLE_LIBS]
    command += ["-ljson-c", "-ltomlplusplus", "-lgmpxx", "-lgmp", "-lm",
                "-o", str(binary)]
    result = run(command, "oracle build", work, args.timeout, env)
    (work / "logs").mkdir(parents=True, exist_ok=True)
    (work / "logs" / "oracle-build.stdout").write_text(result.stdout)
    (work / "logs" / "oracle-build.stderr").write_text(result.stderr)
    expect_ok(result, "oracle build")
    return binary


def validate_oracle(output: str) -> None:
    lines = output.splitlines()
    if "operation_id woozy-SH06f-foreign-signature-20261113" not in lines:
        fail("oracle output is missing the operation_id line")
    if "protocol woozy-selfhost-foreign-signature/1" not in lines:
        fail("oracle output is missing the protocol line")
    operation_cases = [line for line in lines
                       if line.startswith("case op.")
                       and (line.split()[1].endswith(".module")
                            or line.split()[1].endswith(".none"))]
    if len(operation_cases) != EXPECTED_OPERATIONS * 2:
        fail(f"oracle: {len(operation_cases)} operation cases, "
             f"want {EXPECTED_OPERATIONS * 2}")
    if f"operations {EXPECTED_OPERATIONS}" not in lines:
        fail(f"oracle: missing 'operations {EXPECTED_OPERATIONS}' line")
    for label in ("python", "c", "private", "system"):
        if not any(line.startswith(f"table {label} ") for line in lines):
            fail(f"oracle: missing table {label}")
    invalid = [line for line in lines if " invalid " in line]
    if len(invalid) < 40:
        fail(f"oracle: only {len(invalid)} invalid cases; the comparison "
             "does not cover rejection paths")
    if not any(" requires its standard type module" in line for line in lines):
        fail("oracle: no missing-module rejection case")
    if not any("foreign declaration index/identity mismatch" in line for line in lines):
        fail("oracle: no foreign identity mismatch case")
    if not any("C registering callback is not named" in line for line in lines):
        fail("oracle: no registering callback boundary case")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")
    stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    work = pathlib.Path(args.work_dir).expanduser().resolve()
    work.mkdir(parents=True, exist_ok=True)
    env = environment(args)
    print(f"stage0: {stage0}")
    print(f"work-dir: {work}")

    check_pure_sources()
    print("PASS: production modules import only app/wzcc and std")
    oracle = build_oracle(args, work, env)
    oracle_run = run([str(oracle)], "oracle run", work, args.timeout, env)
    expect_ok(oracle_run, "oracle run")
    if oracle_run.stderr.strip():
        fail(f"oracle wrote to stderr:\n{oracle_run.stderr}")
    validate_oracle(oracle_run.stdout)
    expected = oracle_run.stdout
    (work / "logs" / "oracle.out").write_text(expected)

    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    copy_modules(project)
    manifest = str(project / "lang.toml")

    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "check", work, args.timeout, env)
    expect_ok(checked, "stage0 check")

    for opt in ("0", "2"):
        out = work / f"opt{opt}"
        if out.exists():
            shutil.rmtree(out)
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", opt, "--out-dir", str(out)],
                    f"build O{opt}", work, args.timeout, env)
        expect_ok(built, f"stage0 build O{opt}")
        program = out / "program"
        if not program.is_file():
            fail(f"O{opt}: missing {program}")
        program_env = dict(env)
        program_env["PATH"] = "/nonexistent-foreign-signature"
        actual_run = run([str(program)], f"run O{opt}", work, args.timeout,
                         program_env)
        expect_ok(actual_run, f"run O{opt}")
        if actual_run.stderr.strip():
            fail(f"O{opt}: program wrote to stderr:\n{actual_run.stderr}")
        actual = actual_run.stdout
        (work / "logs" / f"wz-O{opt}.out").write_text(actual)
        if actual != expected:
            expected_lines = expected.splitlines()
            actual_lines = actual.splitlines()
            for index in range(min(len(expected_lines), len(actual_lines))):
                if expected_lines[index] != actual_lines[index]:
                    fail(f"O{opt}: first difference at line {index + 1}\n"
                         f"oracle: {expected_lines[index]}\n"
                         f"wz:     {actual_lines[index]}")
            fail(f"O{opt}: output length differs: oracle "
                 f"{len(expected_lines)} lines, wz {len(actual_lines)} lines")
        print(f"PASS: O{opt} output matches the C++ oracle byte for byte "
              f"({len(actual.splitlines())} lines, "
              f"{EXPECTED_OPERATIONS} operations x 2 module modes)")

    print("PASS: foreign/signature differential acceptance "
          "(oracle, check, O0, O2)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
