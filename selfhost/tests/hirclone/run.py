#!/usr/bin/env python3
"""Check, build and run the pure-Wz hirclone tests, then compare the report
with the C++ reference oracle.

The test project imports the real app/wzcc/model, types, hir, hirclone, source,
lexer and parser packages.  They are staged from the repository with symlinks
(never a duplicated production copy); only the small test sources are copied.

Default paths are relative to the repository root.  --woozyc, --cxx,
--lib-dir, --include-dir, --work-dir, --ld-library-path and --timeout let a
verifier use the absolute stage0 toolchain and a fresh output directory.

Exit status is 0 only when:
  * stage0 check passes;
  * O0 and O2 builds succeed and both programs exit 0 with an identical report;
  * the report has 119 probes, failures=0, and every CHECK ok=1;
  * deep nesting (300) succeeds and both recursion failure modes are caught as
    model.SupportError.Invalid("HIR clone nesting limit exceeded") with the
    input module unchanged and no Panic;
  * the C++ oracle report matches every comparable probe/check line, with the
    15 Wozy-only fields and the typed error branch reported as not compared
    (unless --skip-oracle).
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
WZCC = ROOT / "selfhost" / "src" / "wzcc"

DEFAULT_STAGE0 = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
DEFAULT_LIB_DIR = ROOT / "build" / "woozy-stdlib-native"
DEFAULT_INCLUDE_DIR = ROOT / "include"
DEFAULT_WORK = ROOT / "build" / "hirclone-tests"
DEFAULT_TIMEOUT = 900.0

PACKAGES = ["model", "types", "hir", "hirclone", "source", "lexer", "parser"]
TEST_SOURCES = ["main.wz", "fixture.wz", "fingerprint.wz", "probes.wz"]
ERROR_MODES = ["deep-limit", "cycle"]
COMMON_CHECKS = [
    "fixture",
    "clone_fields_equal",
    "option_some_zero",
    "context_isolation",
    "empty_module_clone",
]
EXPECTED_PROBES = 123
EXPECTED_WZ_ONLY = 15

MANIFEST = """schema_version = 1
[package]
name = "hirclone_tests"
source_root = "src"
entry = "app"
[build]
backend = "native"
word_bits = 64
"""

ORACLE_LIBS = [
    "-llang_hir",
    "-llang_modules",
    "-llang_frontend",
    "-ltomlplusplus",
    "-ljson-c",
    "-lgmpxx",
    "-lgmp",
]


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def environment(args: argparse.Namespace,
                work: pathlib.Path) -> dict[str, str]:
    env = dict(os.environ)
    # /tmp may be a small tmpfs with a per-user quota; keep compiler
    # temporaries next to the work directory instead.
    tmp = work / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    env["TMPDIR"] = str(tmp)
    parts = []
    if args.ld_library_path:
        parts.append(args.ld_library_path)
    elif env.get("WOOZY_TOOLCHAIN_ROOT"):
        toolchain = env["WOOZY_TOOLCHAIN_ROOT"]
        parts.append(f"{toolchain}/llvm-22.1.8/lib")
        parts.append(f"{toolchain}/libxml2-legacy-2.13.9/usr/lib")
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        parts.append(previous)
    if parts:
        env["LD_LIBRARY_PATH"] = ":".join(parts)
    return env


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check, build and run the pure-Wz hirclone tests.")
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0),
                        help=f"stage0 compiler (default: {DEFAULT_STAGE0})")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"),
                        help="C++ compiler for the oracle (default: $CXX or c++)")
    parser.add_argument("--lib-dir", default=str(DEFAULT_LIB_DIR),
                        help=f"stage0 static library directory "
                             f"(default: {DEFAULT_LIB_DIR})")
    parser.add_argument("--include-dir", default=str(DEFAULT_INCLUDE_DIR),
                        help=f"C++ include directory (default: "
                             f"{DEFAULT_INCLUDE_DIR})")
    parser.add_argument("--work-dir", default=str(DEFAULT_WORK),
                        help=f"staging/output directory (default: {DEFAULT_WORK})")
    parser.add_argument("--ld-library-path", default="",
                        help="LD_LIBRARY_PATH for generated programs")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"timeout per command in seconds "
                             f"(default: {DEFAULT_TIMEOUT:g})")
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2),
                        help="optimization level; default: 0 and 2")
    parser.add_argument("--skip-oracle", action="store_true",
                        help="skip the C++ comparison and report it as not_run")
    return parser.parse_args(argv)


def run(command: list[str], label: str, work: pathlib.Path,
        timeout: float, env: dict[str, str],
        expect: int | None = 0) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    try:
        result = subprocess.run(
            command, cwd=work, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False, timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"{label}: timed out after {timeout:g}s")
    except OSError as error:
        fail(f"{label}: {error}")
    if expect is not None and result.returncode != expect:
        fail(f"{label}: exit {result.returncode}, want {expect}\n"
             f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def link_or_copy(source: pathlib.Path, target: pathlib.Path) -> None:
    try:
        target.symlink_to(source, target_is_directory=source.is_dir())
    except OSError:
        if source.is_dir():
            shutil.copytree(source, target)
        else:
            shutil.copy2(source, target)


def stage_project(work: pathlib.Path) -> pathlib.Path:
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    wzcc = project / "src" / "wzcc"
    wzcc.mkdir(parents=True)
    for name in PACKAGES:
        link_or_copy(WZCC / name, wzcc / name)
    for name in TEST_SOURCES:
        shutil.copy2(HERE / "src" / name, project / "src" / name)
    manifest = project / "lang.toml"
    manifest.write_text(MANIFEST, encoding="utf-8")
    return project


def report_lines(output: str) -> list[str]:
    return [line for line in output.splitlines()
            if line.startswith(("PROBE ", "CHECK ", "SUMMARY ",
                                "hirclone-report "))]


def summarize_probes(lines: list[str]) -> dict[str, str]:
    probes: dict[str, str] = {}
    for line in lines:
        if line.startswith("PROBE "):
            fields = line.split()
            probes[fields[1]] = line
    return probes


def check_report(lines: list[str], label: str) -> None:
    checks = {line.split()[1]: line for line in lines
              if line.startswith("CHECK ")}
    for name, line in checks.items():
        if "ok=1" not in line:
            fail(f"{label}: {line}")
    probes = summarize_probes(lines)
    if len(probes) != EXPECTED_PROBES:
        fail(f"{label}: {len(probes)} probes, want {EXPECTED_PROBES}")
    if "SUMMARY probes=123 failures=0 wz_only=15" not in lines:
        fail(f"{label}: missing success SUMMARY")
    wz_only = [name for name, line in probes.items() if "wz_only=1" in line]
    if len(wz_only) != EXPECTED_WZ_ONLY:
        fail(f"{label}: {len(wz_only)} wz_only probes, want "
             f"{EXPECTED_WZ_ONLY}")
    for line in probes.values():
        if "left_changed=1" not in line or "right_isolated=1" not in line \
                or "reverse_changed=1" not in line \
                or "reverse_isolated=1" not in line:
            fail(f"{label}: unisolated field: {line}")
    print(f"PASS: {label}: {len(probes)} probes, all isolated both ways, "
          f"{len(checks)} checks, {len(wz_only)} wz-only fields")


def compare_reports(wz_lines: list[str],
                    oracle_lines: list[str]) -> None:
    wz_probes = summarize_probes(wz_lines)
    cpp_probes = summarize_probes(oracle_lines)
    if list(wz_probes) != list(cpp_probes):
        fail("probe name order differs between Wz and C++ reports:\n"
             f"Wz: {list(wz_probes)}\nC++: {list(cpp_probes)}")
    not_compared = []
    for name, wz_line in wz_probes.items():
        cpp_line = cpp_probes[name]
        if "wz_only=1" in wz_line:
            if not cpp_line.endswith("wz_only=1 cpp=none"):
                fail(f"probe {name}: expected cpp=none, got {cpp_line}")
            not_compared.append(name)
        elif wz_line != cpp_line:
            fail(f"probe {name} differs:\nWz: {wz_line}\nC++: {cpp_line}")
    wz_checks = {line.split()[1]: line for line in wz_lines
                 if line.startswith("CHECK ")}
    cpp_checks = {line.split()[1]: line for line in oracle_lines
                  if line.startswith("CHECK ")}
    for name in COMMON_CHECKS:
        if name not in cpp_checks:
            fail(f"C++ oracle is missing CHECK {name}")
        if "ok=1" not in cpp_checks[name]:
            fail(f"C++ oracle reported {cpp_checks[name]}")
    wz_only_checks = [name for name, line in wz_checks.items()
                      if "wz_only=1" in line]
    for name, line in cpp_checks.items():
        if name not in COMMON_CHECKS and name != "fixture":
            fail(f"C++ oracle printed an unknown CHECK {name}: {line}")
    for name in wz_only_checks:
        if name not in wz_checks:
            fail(f"missing Wz-only CHECK {name}")
    wz_summary = [line for line in wz_lines if line.startswith("SUMMARY ")]
    cpp_summary = [line for line in oracle_lines if line.startswith("SUMMARY ")]
    if wz_summary != cpp_summary:
        fail(f"SUMMARY differs:\nWz: {wz_summary}\nC++: {cpp_summary}")
    print(f"PASS: C++ oracle matches all "
          f"{len(wz_probes) - len(not_compared)} comparable probes and "
          f"{len(COMMON_CHECKS)} common checks")
    print("NOT_COMPARED (Wz-only, no mutable C++ counterpart): "
          + ", ".join(not_compared))
    print("NOT_COMPARED (Wz-only checks): " + ", ".join(wz_only_checks))
    print("NOT_COMPARED (Wz-only error branch): nesting-limit and shared-Vec "
          "cycle throw model.SupportError.Invalid(\"HIR clone nesting limit "
          "exceeded\"); C++ freeze() has no limit/cycle counterpart")


def build_oracle(args: argparse.Namespace, work: pathlib.Path,
                 env: dict[str, str]) -> pathlib.Path:
    lib_dir = pathlib.Path(args.lib_dir).expanduser().resolve()
    include_dir = pathlib.Path(args.include_dir).expanduser().resolve()
    if not lib_dir.is_dir():
        fail(f"stage0 library directory missing: {lib_dir}")
    if not (lib_dir / "liblang_hir.a").is_file():
        fail(f"liblang_hir.a missing under {lib_dir}")
    executable = work / "oracle"
    command = [args.cxx, "-std=c++20", "-O1", "-I", str(include_dir),
               str(HERE / "oracle.cpp"), "-L", str(lib_dir), *ORACLE_LIBS,
               "-o", str(executable)]
    run(command, "oracle build", work, args.timeout, env)
    return executable


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    work = pathlib.Path(args.work_dir).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")
    levels = args.opt or [0, 2]
    work.mkdir(parents=True, exist_ok=True)
    env = environment(args, work)
    print(f"stage0: {stage0}")
    print(f"work-dir: {work}")
    project = stage_project(work)
    manifest = str(project / "lang.toml")

    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "check", project, args.timeout, env)
    print("PASS: stage0 check")

    reports: dict[int, list[str]] = {}
    for level in levels:
        out_dir = work / f"opt{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        run([str(stage0), "build", "--project", manifest, "--backend",
             "native", "--opt", str(level), "--out-dir", str(out_dir)],
            f"build O{level}", project, args.timeout, env)
        program = out_dir / "program"
        if not program.is_file():
            fail(f"O{level}: missing {program}")
        values = run([str(program)], f"report O{level}", project,
                     args.timeout, env)
        lines = report_lines(values.stdout)
        check_report(lines, f"O{level} report")
        reports[level] = lines
        deep = run([str(program), "deep-ok"], f"deep-ok O{level}", project,
                   args.timeout, env)
        if "DEEP ok depth=300" not in deep.stdout:
            fail(f"O{level} deep-ok: missing success line: {deep.stdout}")
        print(f"PASS: O{level} deep nesting (300) cloned")
        for mode in ERROR_MODES:
            caught = run([str(program), mode], f"{mode} O{level}", project,
                         args.timeout, env, expect=0)
            marker = "LIMIT caught=1 message_ok=1" if mode == "deep-limit" \
                else "CYCLE caught=1 message_ok=1"
            if marker not in caught.stdout:
                fail(f"O{level} {mode}: missing caught marker\n"
                     f"stdout:\n{caught.stdout}")
            if "LANG_NATIVE_PANIC" in caught.stderr or "failed=1" in \
                    caught.stdout:
                fail(f"O{level} {mode}: typed error was not handled\n"
                     f"stdout:\n{caught.stdout}\nstderr:\n{caught.stderr}")
            print(f"PASS: O{level} {mode} caught SupportError.Invalid with "
                  f"the input unchanged")
    if 0 in reports and 2 in reports and reports[0] != reports[2]:
        fail("O0 and O2 reports differ")
    if 0 in reports and 2 in reports:
        print("PASS: O0 and O2 reports are identical")

    if args.skip_oracle:
        print("NOT_RUN: C++ oracle comparison (--skip-oracle)")
    else:
        oracle = build_oracle(args, work, env)
        result = run([str(oracle)], "oracle run", work, args.timeout, env)
        oracle_lines = report_lines(result.stdout)
        if "SUMMARY probes=123 failures=0 wz_only=15" not in oracle_lines:
            fail("C++ oracle did not report success")
        compare_reports(reports[levels[0]], oracle_lines)

    print("PASS: hirclone tests (check, O0, O2, deep nesting, 2 caught "
          "recursion errors per level" + (", C++ oracle"
                                          if not args.skip_oracle else "") +
          ")")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
