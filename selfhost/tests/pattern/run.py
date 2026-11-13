#!/usr/bin/env python3
"""SH06e acceptance runner: Wz pattern coverage versus the real C++ analysis.

What it does, in order:

1. Static audit of selfhost/src/wzcc/pattern/pattern.wz: only the published
   app/wzcc/{hir,model,types} and std/{option,strconv,text,vec} imports are
   allowed, and no old-frontend/host hook may appear.
2. Build the test-only C++ oracle from the existing translation units
   (src/hir/pattern_analysis.cpp plus the lang_hir/support/syntax units it
   links against).  This is the oracle only; the Wz compiler never calls it.
3. Run the oracle and check that its dump is complete (record counts) and that
   the accepted semantic facts are present, including every error text, the
   +0/-0 overlap, NaN non-exhaustiveness, generically substituted enum fields,
   uninhabited empty enums and the usefulness/budget boundaries.
4. For each optimization level (0 and 2): stage-0 `check`, `build`, run, then
   compare the Wz dump with the oracle dump byte for byte (field, order and
   error parity), and re-run the pinned acceptance facts on the Wz dump too.

The production modules are never copied: the test project's lang.toml declares
the compiler source tree as a source dependency (path = "../..") and the build
reads the real selfhost/src/wzcc/pattern package.  The driver and oracle keep
the same fixture interning order, so substituted field TypeIds are comparable.

Default paths are relative to the repository root (the parent of selfhost).
Override the toolchain with --woozyc/--cxx/--lib-dir/--cxx-flag/--oracle-lib.
A missing stage-0 compiler or C++ compiler is reported as not_run (exit 2);
an actual mismatch or failed command is reported as FAIL (exit 1).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
MANIFEST = HERE / "lang.toml"
DRIVER = HERE / "src" / "main.wz"
ORACLE_SOURCE = HERE / "oracle.cpp"
PRODUCTION = ROOT / "selfhost" / "src" / "wzcc" / "pattern" / "pattern.wz"

DEFAULT_STAGE0 = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
DEFAULT_WORK = ROOT / "build" / "pattern-tests"
DEFAULT_TIMEOUT = 300.0

# Existing C++ units used only by the oracle binary: the lang_hir library set
# (pattern analysis + the HIR support it references), support/source.cpp for
# validate_utf8, and the syntax units providing split_signature.  The C++
# compiler front end, sema, backends and CLI are not part of the oracle.
ORACLE_SOURCES = [
    ROOT / "src" / "hir" / "pattern_analysis.cpp",
    ROOT / "src" / "hir" / "hir.cpp",
    ROOT / "src" / "hir" / "system_operation.cpp",
    ROOT / "src" / "hir" / "ownership.cpp",
    ROOT / "src" / "hir" / "literal_value.cpp",
    ROOT / "src" / "hir" / "constant_value.cpp",
    ROOT / "src" / "support" / "source.cpp",
    ROOT / "src" / "syntax" / "parser.cpp",
    ROOT / "src" / "syntax" / "lexer.cpp",
]

ALLOWED_IMPORTS = {
    "app/wzcc/hir",
    "app/wzcc/model",
    "app/wzcc/types",
    "std/option",
    "std/strconv",
    "std/text",
    "std/vec",
}
FORBIDDEN_PRODUCTION = [
    "unsafe",
    "__woozy_",
    "extern ",
    "lang_native_helper",
    "woozyc",
    'import "std/ffi"',
    'import "std/process"',
    'import "std/os"',
    'import "std/io"',
    'import "std/json"',
    "PatternCoverage",  # the C++ type name must not be smuggled in
]

BUDGET_TEXT = "switch pattern coverage analysis exceeded its resource budget"
DEPTH_TEXT = "switch pattern coverage analysis exceeded its nesting budget"

# Exact record counts of the accepted oracle dump.  A truncated, duplicated or
# accidentally extended fixture set is a test defect, not a passing run.
EXPECTED_RECORDS = {
    "type": 38,
    "domain": 28,
    "constructors": 16,
    "ctor": 33,
    "add": 571,
    "exhaust": 32,
    "error": 31,
}

# Accepted semantic facts, verbatim, present in the oracle and in every Wz
# dump.  These are the SH06e acceptance criteria, not happy-path samples.
EXPECTED_LINES = [
    # Domain classification: six kinds, empty enum uninhabited, integer
    # widths from the type/config, floats from the type width.
    "domain d_bool kind=1 bits=1 n=0",
    "domain d_empty_enum kind=0 bits=0 n=0",
    "domain d_i32 kind=2 bits=32 n=0",
    "domain d_isize kind=2 bits=64 n=0",
    "domain d32_isize kind=2 bits=32 n=0",
    "domain d_f32 kind=3 bits=32 n=0",
    "domain d_string kind=4 bits=0 n=0",
    "domain d_struct kind=5 bits=0 n=0",
    # Real generic enum field substitution, including a nested generic and a
    # compound field type that the substitution re-interns (Vec[u8]).
    'ctor c_option_i32 1 kind=0 variant=204 key="" fields=7 sigs=2:32:0:',
    'ctor c_option_option_i32 1 kind=0 variant=204 key="" fields=28 '
    "sigs=10:0:102:7",
    'ctor c_pair_i32_f64 0 kind=0 variant=205 key="" fields=7,14 '
    "sigs=2:32:0:,4:64:0:",
    'ctor c_wrap_u8 0 kind=0 variant=210 key="" fields=38 sigs=14:0:0:4',
    'ctor c_list_i32 1 kind=0 variant=208 key="" fields=33 sigs=10:0:104:7',
    # Declaration lookup takes the last matching nominal; arity/orphan errors.
    'error c_e1_args Limit "enum pattern constructor '
    'declaration/type arguments mismatch"',
    'error c_orphan Limit "enum pattern constructor '
    'declaration/type arguments mismatch"',
    'error c_missing_type Limit "enum pattern constructor type is missing"',
    'error d_missing_type Limit "pattern coverage type is missing"',
    # types.Error is converted, text preserved, never a panic.
    'error d48_i32 Invalid "target word_bits"',
    # Nested payload coverage and redundancy.
    "add e1_add_A_again 0",
    "exhaust e1_complete 1",
    "exhaust e1p_B_complete 0",
    "add e1_never_added_A 1",
    # Bool: complete two-value domain, binding is total.
    "add bool_add_wild 0",
    "exhaust bool_complete 1",
    "exhaust bool_bind_complete 1",
    "add bool_bind_then_true 0",
    # Complete small integer domain has no Other class.
    "exhaust u8_complete 1",
    "add u8_add_wild 0",
    "exhaust u8_partial 0",
    "add u8p_add_255 1",
    "exhaust u8_partial_complete 1",
    # Float: +0/-0 overlap, NaN/infinity never finish a finite literal set.
    "add f32_add_neg0 0",
    "add f32_add_pos0_again 0",
    "add f32_add_nan 1",
    "exhaust f32_still_partial 0",
    "exhaust f32_complete 1",
    "add f64_add_neg0 0",
    "add f64_add_one 1",
    # String literals: duplicate/byte-key canonicalization and Other class.
    "add str_add_empty_again 0",
    "add str_add_a_again 0",
    "add str_add_cjk 1",
    "exhaust str_partial 0",
    "exhaust str_complete 1",
    # Opaque non-enum domain, wildcard and literal rejection.
    "exhaust opaque_partial 0",
    "exhaust opaque_complete 1",
    "add opaque_add_wild_again 0",
    'error err_opaque_literal Limit "bool literal pattern type mismatch"',
    # Empty (uninhabited) enum: exhaustive with no arms; no constructors.
    "exhaust empty_complete 1",
    "add empty_add_wild 1",
    "exhaust empty_still_complete 1",
    "add empty_add_A 0",
    'error err_empty_variant_absent Limit "pattern constructor is absent '
    'from its declared type"',
    # Recursive enum: finite arm set stays non-exhaustive; wildcard closes it.
    "add list_add_cons_cons_nil 1",
    "exhaust list_after_three 0",
    "add list_add_wild 1",
    "exhaust list_complete 1",
    "add list_add_cons_wild 0",
    # No Cartesian expansion: a total wildcard row finishes under budget 10.
    "add lazy_add_wild 1",
    "exhaust lazy_complete 1",
    # Exact budget accounting boundaries (2 units per useful, 1 per domain
    # cache miss, 2 per bool constructor set).
    'error budget4_add_wild Limit "' + BUDGET_TEXT + '"',
    "add budget5_add_wild 1",
    'error budget7_exhaust Limit "' + BUDGET_TEXT + '"',
    "add budget8_add_wild 1",
    "exhaust budget8_exhaust 1",
    'error budget_u8_2_add Limit "' + BUDGET_TEXT + '"',
    'error lazy9_exhaust Limit "' + BUDGET_TEXT + '"',
    # Verbatim shape errors from the C++ analysis.
    'error err_variant_outside Limit "enum variant pattern outside an enum '
    'domain"',
    'error err_both Limit "pattern has both variant and literal"',
    'error err_variant_binding Limit "variant pattern has invalid fields"',
    'error err_literal_binding Limit "literal pattern has invalid fields"',
    'error err_literal_payload Limit "literal pattern has invalid fields"',
    'error err_payload_count Limit "enum pattern payload count differs from '
    'its constructor"',
    'error err_column Limit "pattern coverage column type mismatch"',
    'error err_bool_on_int Limit "bool literal pattern type mismatch"',
    'error err_int_on_string Limit "integer literal pattern type mismatch"',
    'error err_float_on_string Limit "float literal pattern type mismatch"',
    'error err_string_on_float Limit "string literal pattern type mismatch"',
    # Nesting budget: 200 levels accepted, 260 rejected (C++ threshold 256).
    "add depth_200_add 1",
    "exhaust depth_200_exhaust 0",
    'error err_depth Limit "' + DEPTH_TEXT + '"',
]


class CheckFailure(Exception):
    pass


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise CheckFailure(message)


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the SH06e Wz/C++ pattern coverage differential test.")
    parser.add_argument(
        "--woozyc", default=str(DEFAULT_STAGE0),
        help=f"stage-0 compiler (default: {DEFAULT_STAGE0})")
    parser.add_argument(
        "--cxx", default=os.environ.get("CXX", "g++"),
        help="C++ compiler for the test-only oracle (default: $CXX or g++)")
    parser.add_argument(
        "--cxx-flag", action="append", default=None, metavar="FLAG",
        help="extra flag for the oracle build; repeatable "
             "(default: -std=c++20)")
    parser.add_argument(
        "--oracle-lib", action="append", default=None, metavar="LIB",
        help="extra library for the oracle link; repeatable "
             "(default: -lgmpxx -lgmp)")
    parser.add_argument(
        "--lib-dir", action="append", default=[], metavar="DIR",
        help="prepend DIR to LD_LIBRARY_PATH for every command; repeatable")
    parser.add_argument(
        "--work-dir", default=str(DEFAULT_WORK),
        help=f"output directory (default: {DEFAULT_WORK})")
    parser.add_argument(
        "--timeout", type=float, default=DEFAULT_TIMEOUT,
        help=f"timeout per command in seconds (default: {DEFAULT_TIMEOUT:g})")
    parser.add_argument(
        "--opt", action="append", type=int, choices=(0, 2), default=None,
        help="optimization level; repeatable (default: 0 and 2)")
    parser.add_argument(
        "--skip-oracle", action="store_true",
        help="skip the C++ oracle and the differential comparison "
             "(comparison is then reported not_run)")
    parser.add_argument(
        "--keep-going", action="store_true",
        help="keep running the remaining steps after a failure")
    return parser.parse_args(argv)


def build_env(lib_dirs: list[str]) -> dict[str, str]:
    env = dict(os.environ)
    toolchain = ROOT.parent / "toolchains"
    lib_dirs = [*lib_dirs, str(toolchain / "llvm-22.1.8/lib"),
                str(toolchain / "libxml2-legacy-2.13.9/usr/lib")]
    if lib_dirs:
        parts = [str(Path(item).expanduser().resolve()) for item in lib_dirs]
        previous = env.get("LD_LIBRARY_PATH")
        if previous:
            parts.append(previous)
        env["LD_LIBRARY_PATH"] = ":".join(parts)
    return env


def run_command(command: list[str], *, label: str, cwd: Path,
                env: dict[str, str], timeout: float) -> str:
    print("+", " ".join(command), flush=True)
    try:
        result = subprocess.run(
            command, cwd=str(cwd), env=env, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"{label}: timed out after {timeout:g}s")
    except OSError as error:
        fail(f"{label}: {error}")
    if result.returncode != 0:
        fail(f"{label}: exit {result.returncode}\n"
             f"command: {' '.join(command)}\n"
             f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    print(f"PASS: {label} (exit 0)")
    return result.stdout


def audit_production() -> None:
    if not PRODUCTION.is_file():
        fail(f"production module missing: {PRODUCTION}")
    text_value = PRODUCTION.read_text(encoding="utf-8")
    imports = set(re.findall(r'^import\s+"([^"]+)"', text_value, re.MULTILINE))
    unexpected = sorted(imports - ALLOWED_IMPORTS)
    if unexpected:
        fail(f"production module imports outside the published contract: "
             f"{unexpected}")
    missing = sorted(ALLOWED_IMPORTS - imports)
    for token in FORBIDDEN_PRODUCTION:
        if token in text_value:
            fail(f"production module contains forbidden token {token!r}")
    print(f"PASS: production imports are confined to "
          f"{len(imports)} published modules (unused: {missing or 'none'})")
    print("PASS: production module contains no host/old-frontend hook")


def audit_test_layout() -> None:
    if not MANIFEST.is_file() or not DRIVER.is_file():
        fail("test project manifest or driver is missing")
    manifest_text = MANIFEST.read_text(encoding="utf-8")
    if 'path = "../.."' not in manifest_text:
        fail("test manifest must depend on the real compiler tree "
             "(path = \"../..\"), not a copy")
    if (HERE / "src" / "wzcc").exists():
        fail("test project must not keep a production module copy")
    for name in ("model", "hir", "types", "pattern"):
        if not (ROOT / "selfhost" / "src" / "wzcc" / name).is_dir():
            fail(f"missing production module directory: wzcc/{name}")
    print("PASS: test project uses the real modules as a source dependency "
          "and keeps no copy")


def compare_dump(expected_path: Path, actual_path: Path, label: str) -> None:
    expected = expected_path.read_text(encoding="utf-8").splitlines()
    actual = actual_path.read_text(encoding="utf-8").splitlines()
    if expected == actual:
        print(f"PASS: {label}: {len(actual)} dump lines identical to the "
              f"C++ oracle (fields, order, errors)")
        return
    first = None
    for index, (left, right) in enumerate(zip(expected, actual), start=1):
        if left != right:
            first = index
            break
    if first is None:
        first = min(len(expected), len(actual)) + 1
    context_begin = max(1, first - 3)
    lines = [f"FAIL: {label}: dump differs from the C++ oracle at line {first}",
             f"  oracle lines={len(expected)} wz lines={len(actual)}"]
    for index in range(context_begin, min(first + 3,
                                          max(len(expected), len(actual))) + 1):
        left = expected[index - 1] if index <= len(expected) else "<missing>"
        right = actual[index - 1] if index <= len(actual) else "<missing>"
        marker = "!" if left != right else " "
        lines.append(f"  {marker} {index:4d} oracle: {left}")
        lines.append(f"  {marker} {index:4d} wz    : {right}")
    fail("\n".join(lines))


def check_oracle_facts(text_value: str, label: str) -> None:
    lines = text_value.splitlines()
    if not lines or lines[0] != "woozy-pattern/1":
        fail(f"{label}: missing woozy-pattern/1 header")
    if len(lines) < 2 or not lines[1].startswith("types "):
        fail(f"{label}: missing type table size record")
    counts = {name: 0 for name in EXPECTED_RECORDS}
    for line in lines[2:]:
        for name in counts:
            if line.startswith(name + " "):
                counts[name] += 1
    if counts != EXPECTED_RECORDS:
        fail(f"{label}: record counts {counts} differ from the accepted "
             f"{EXPECTED_RECORDS}")
    present = set(lines)
    missing = [line for line in EXPECTED_LINES if line not in present]
    if missing:
        fail(f"{label}: missing accepted facts:\n  " + "\n  ".join(missing))
    if f'error err_depth Limit "{DEPTH_TEXT}"' not in present:
        fail(f"{label}: missing the verbatim nesting-budget error")
    budget_errors = sum(1 for line in lines
                        if line.startswith("error ") and BUDGET_TEXT in line)
    if budget_errors != 8:
        fail(f"{label}: expected 8 verbatim resource-budget errors, "
             f"found {budget_errors}")
    print(f"PASS: {label}: {len(lines)} lines, record counts {counts}, "
          f"{len(EXPECTED_LINES)} accepted facts present")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = Path(args.woozyc).expanduser().resolve()
    cxx = shutil.which(args.cxx) or (
        str(Path(args.cxx).expanduser().resolve())
        if Path(args.cxx).expanduser().is_file() else None)
    work = Path(args.work_dir).expanduser().resolve()
    opt_levels = args.opt or [0, 2]
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")
    cxx_flags = args.cxx_flag or ["-std=c++20"]
    oracle_libs = args.oracle_lib or ["-lgmpxx", "-lgmp"]

    if not stage0.is_file():
        print(f"not_run: stage-0 compiler missing: {stage0}", file=sys.stderr)
        return 2
    if cxx is None and not args.skip_oracle:
        print(f"not_run: C++ compiler for the oracle missing: {args.cxx}",
              file=sys.stderr)
        return 2
    work.mkdir(parents=True, exist_ok=True)
    # Some container /tmp mounts are small; keep compiler temporaries next to
    # the outputs so the oracle build does not depend on /tmp capacity.
    tmp_dir = work / "tmp"
    tmp_dir.mkdir(exist_ok=True)
    env = build_env(args.lib_dir)
    env["TMPDIR"] = str(tmp_dir)

    print(f"root: {ROOT}")
    print(f"stage-0: {stage0}")
    print(f"work-dir: {work}")
    print(f"opt levels: {opt_levels}")
    if not args.skip_oracle:
        print(f"c++ oracle: {cxx} {' '.join(cxx_flags)} "
              f"{' '.join(oracle_libs)}")
    print(f"LD_LIBRARY_PATH: {env.get('LD_LIBRARY_PATH', '<unchanged>')}")

    audit_production()
    audit_test_layout()

    oracle_text: str | None = None
    if args.skip_oracle:
        print("not_run: C++ oracle differential comparison (--skip-oracle)")
    else:
        oracle_bin = work / "oracle"
        libraries = [stage0.parent / name for name in
                     ("liblang_hir.a", "liblang_frontend.a")]
        inputs = (libraries if all(path.is_file() for path in libraries)
                  else ORACLE_SOURCES)
        command = [cxx, *cxx_flags, "-I", str(ROOT / "include"),
                   "-I", str(ROOT / "src"), str(ORACLE_SOURCE),
                   *[str(path) for path in inputs],
                   *oracle_libs, "-o", str(oracle_bin)]
        run_command(command, label="oracle build", cwd=ROOT, env=env,
                    timeout=args.timeout)
        oracle_text = run_command([str(oracle_bin)], label="oracle run",
                                  cwd=ROOT, env=env, timeout=args.timeout)
        oracle_path = work / "oracle.txt"
        oracle_path.write_text(oracle_text, encoding="utf-8")
        check_oracle_facts(oracle_text, "oracle")

    checked = run_command(
        [str(stage0), "check", "--project", str(MANIFEST),
         "--backend", "native"], label="stage0 check", cwd=ROOT, env=env,
        timeout=args.timeout)
    if "check ok" not in checked:
        fail(f"stage0 check did not report success:\n{checked}")

    any_failure = False
    results: list[str] = []
    for level in opt_levels:
        out_dir = work / f"o{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        run_command([str(stage0), "build", "--project", str(MANIFEST),
                     "--backend", "native", "--opt", str(level),
                     "--out-dir", str(out_dir)], label=f"stage0 build O{level}",
                    cwd=ROOT, env=env, timeout=args.timeout)
        program = out_dir / "program"
        if not program.is_file():
            fail(f"O{level}: missing built program {program}")
        dump = run_command([str(program)], label=f"run O{level}",
                           cwd=ROOT, env=env, timeout=args.timeout)
        dump_path = work / f"wz-o{level}.txt"
        dump_path.write_text(dump, encoding="utf-8")
        try:
            check_oracle_facts(dump, f"Wz O{level} facts")
            if oracle_text is not None:
                compare_dump(work / "oracle.txt", dump_path, f"Wz O{level}")
            results.append(f"O{level}: check+build+run+facts"
                           + ("+oracle-diff" if oracle_text is not None
                              else " (diff not_run)"))
        except CheckFailure:
            any_failure = True
            results.append(f"O{level}: FAILED")
            if not args.keep_going:
                break

    print("\nsummary:")
    print(f"  pattern module: {PRODUCTION.relative_to(ROOT)}")
    print(f"  driver:         {DRIVER.relative_to(ROOT)}")
    print(f"  oracle:         "
          f"{'not_run (--skip-oracle)' if args.skip_oracle else 'built+run'}")
    for item in results:
        print(f"  {item}")
    if any_failure:
        print("FAIL: SH06e pattern acceptance failed", file=sys.stderr)
        return 1
    if args.skip_oracle:
        print("not_run: C++ differential comparison was skipped; the Wz-side "
              "facts alone are not the full SH06e acceptance", file=sys.stderr)
        return 2
    print("PASS: SH06e pattern port matches the C++ oracle at "
          f"O{'/O'.join(str(level) for level in opt_levels)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CheckFailure:
        raise SystemExit(1)
