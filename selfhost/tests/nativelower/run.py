#!/usr/bin/env python3
"""Check, build and run the Native CFG lowering/dump tests.

Stages
======
dump    Compiles oracle.cpp against the real linked lang::ir::dump and
        lang::hir::verify from the stage0 build, then checks the Wz dump
        fixture at O0 and O2 byte-for-byte.  The default fixture is the
        frozen 989-byte empty+special case; the rich fixture covers all 53
        Op records and all 9 Exit terminators and is compared with the
        oracle's --rich mode.

helper  Provisional joint check of the four production lower helper files
        plus the real helper assertions and the 989-byte dump at O0/O2.
        nativelayout/valueops must be supplied explicitly; those candidate
        files are unaccepted dependencies, so this stage is reported as
        provisional, never as the accepted public-package result.

controls
        Confirmed minimal reproduction and control for take_function (the
        defective Unit-as-Function form must fail E_TYPE_MISMATCH; the fixed
        state.f form must pass and execute), plus the real C++ identity
        control (a legal module id 0 / identity 0 is accepted by the dump
        oracle, while identity 1 with no module 1 is rejected with
        E_IR_INVALID: module identity index).

lower   Stages the accepted model/hir/types/nativeir/nativeirdump packages
        plus nativelayout/valueops/hirverify/nativeverify and the nativelower
        package.  If any required package is missing, the stage is reported
        as not_run with the exact missing paths; no stub, fake
        VerifiedHirModule or private verified flag is ever created.
        Otherwise the project is checked, built at O0/O2 and executed.  The
        runner requires Complete + Some + zero error diagnostics for both
        verify and lower, functions=0, dump_lines=1 and the fixed complete
        empty-module dump; a rejected/None/thrown result is never a pass.

cfg     Real-program CFG comparison, run only when both a working C++
        lang_native_helper and a Wz-built helper with the same CLI are given.
        The helper compiles fixtures/cfg at O0/O2 to .nir; the Wz helper must
        produce identical text.  A helper that cannot read the .wz fixture is
        reported as not_run instead of being worked around.

Only the frozen C++ header/oracle is used for comparison; the production
compiler paths never start woozyc, lang_native_helper, Python or a C++ helper.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_STAGE0 = ROOT / "build/woozy-stdlib-native/woozyc"
DEFAULT_TOOLCHAIN = ROOT.parent / "toolchains"
DEFAULT_TIMEOUT = 600.0

EMPTY_DUMP = "native-scalars compiler-dev/1 native64\n"
DUMP_BYTES = 989
DUMP_SHA256 = \
    "f627f745581d7a6883078d0a425bc4d514bd4470d97992bbfadeebd1aaea4e18"
RICH_LINES = 84
HELPER_MESSAGE = ("helpers: map snapshots, Option(0), owner kinds, slot "
                  "opcodes, dead blocks/slots/handlers/SSA passed\n")

OPS = [
    "constant", "function", "load", "address", "store", "unary", "binary",
    "convert", "call", "struct_init", "enum_init", "field_read",
    "field_address", "load_indirect", "store_indirect", "enum_test",
    "enum_payload", "string_literal", "exit_begin", "cleanup_begin",
    "cleanup_end", "copy", "move", "take", "destroy", "borrow", "end_borrow",
    "save_output", "vec_init", "vec_read", "vec_set", "vec_slice",
    "iteration_begin", "iteration_end", "vec_length", "iteration_test",
    "iteration_read", "iteration_next", "ownership_begin", "ownership_end",
    "replace_indirect", "begin_borrow", "vec_check", "vec_element_address",
    "system_call", "foreign_call", "c_field_read", "c_ptr_read", "c_ptr_write",
    "c_buffer_read", "c_buffer_write", "trait_object", "trait_call",
]
EXITS = ["missing", "branch", "conditional", "return", "unreachable",
         "try_call", "error_return", "await", "cancel_return"]

DUMP_PACKAGES = ["model", "hir", "types", "nativeir", "nativeirdump"]
LOWER_PACKAGES = ["model", "hir", "types", "nativeir", "nativeirdump", "nativelayout",
                  "valueops", "hirverify", "nativeverify", "nativelower"]
HELPER_PACKAGES = ["model", "types", "hir", "nativeir", "nativeirdump"]
HELPER_LOWER_FILES = ["state.wz", "expr.wz", "stmt.wz", "construct.wz"]


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check, build and execute the Native CFG lowering/dump "
                    "tests.")
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0),
                        help=f"stage0 compiler (default: {DEFAULT_STAGE0})")
    parser.add_argument("--toolchain-root", default=str(DEFAULT_TOOLCHAIN),
                        help=f"LLVM/libxml2 toolchain root "
                             f"(default: {DEFAULT_TOOLCHAIN})")
    parser.add_argument("--work-dir", default="",
                        help="staging directory (default: a new temp dir)")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT,
                        help=f"timeout in seconds for each command "
                             f"(default: {DEFAULT_TIMEOUT:g})")
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2),
                        help="optimization level (default: 0 and 2)")
    parser.add_argument("--skip-dump", action="store_true",
                        help="skip the dump stage")
    parser.add_argument("--skip-helper", action="store_true",
                        help="skip the provisional helper stage")
    parser.add_argument("--skip-controls", action="store_true",
                        help="skip the minimal reproduction/control stage")
    parser.add_argument("--nativelayout", default="",
                        help="provisional nativelayout.wz used only by the "
                             "helper/control stages; unaccepted dependency")
    parser.add_argument("--valueops", default="",
                        help="provisional valueops.wz used only by the "
                             "helper/control stages; unaccepted dependency")
    parser.add_argument("--wz-native-helper", default="",
                        help="Wz-built helper with lang_native_helper's CLI "
                             "for the real-program cfg stage; omitted means "
                             "not_run")
    parser.add_argument("--native-helper", default="",
                        help="C++ lang_native_helper used only as the cfg "
                             "oracle (default: first working build/*/native/"
                             "lang_native_helper)")
    return parser.parse_args(argv)


def environment(toolchain: pathlib.Path,
                work: pathlib.Path) -> dict[str, str]:
    env = dict(os.environ)
    parts = [
        str(toolchain / "llvm-22.1.8" / "lib"),
        str(toolchain / "libxml2-legacy-2.13.9" / "usr" / "lib"),
    ]
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        parts.append(previous)
    env["LD_LIBRARY_PATH"] = ":".join(parts)
    # Disk-backed staging: never leave compiler temporaries on tmpfs.
    tmp = work / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    env["TMPDIR"] = str(tmp)
    env["TMP"] = str(tmp)
    env["TEMP"] = str(tmp)
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


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stage_project(work: pathlib.Path, name: str, packages: list[str],
                  main_source: pathlib.Path,
                  manifest_source: pathlib.Path) -> pathlib.Path:
    project = work / name
    if project.exists():
        shutil.rmtree(project)
    src = project / "src" / "wzcc"
    src.mkdir(parents=True)
    for package in packages:
        target = ROOT / "selfhost" / "src" / "wzcc" / package
        (src / package).symlink_to(target, target_is_directory=True)
    shutil.copy2(main_source, project / "src" / "main.wz")
    shutil.copy2(manifest_source, project / "lang.toml")
    return project


def stage_helper_project(work: pathlib.Path, name: str,
                         nativelayout: pathlib.Path,
                         valueops: pathlib.Path,
                         extra: pathlib.Path | None = None) -> pathlib.Path:
    """Stage the four real helper files plus provisional dependencies.

    nativelower.wz is intentionally absent: it needs the not-yet-available
    hirverify/nativeverify packages.  This stage never claims to check the
    public package.
    """
    project = work / name
    if project.exists():
        shutil.rmtree(project)
    src = project / "src"
    packages = src / "wzcc"
    packages.mkdir(parents=True)
    for package in HELPER_PACKAGES:
        target = ROOT / "selfhost" / "src" / "wzcc" / package
        (packages / package).symlink_to(target, target_is_directory=True)
    lower = packages / "nativelower"
    lower.mkdir()
    for filename in HELPER_LOWER_FILES:
        shutil.copy2(ROOT / "selfhost" / "src" / "wzcc" / "nativelower"
                     / filename, lower / filename)
    shutil.copy2(HERE / "helpers" / "review.wz", lower / "review.wz")
    if extra is not None:
        shutil.copy2(extra, lower / extra.name)
    for package, source in (("nativelayout", nativelayout),
                            ("valueops", valueops)):
        target = packages / package
        target.mkdir()
        shutil.copy2(source, target / source.name)
    shutil.copy2(HERE / "helpers" / "main.wz", src / "main.wz")
    shutil.copy2(HERE / "helpers" / "lang.toml", project / "lang.toml")
    return project


def build_wz(stage0: pathlib.Path, project: pathlib.Path, work: pathlib.Path,
             env: dict[str, str], levels: list[int],
             timeout: float) -> list[tuple[int, str, str]]:
    manifest = str(project / "lang.toml")
    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "Wz stage0 check", work, env,
                  timeout)
    expect_ok(checked, "Wz stage0 check")
    traces: list[tuple[int, str, str]] = []
    for level in levels:
        out_dir = project / f"opt{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", str(level),
                     "--out-dir", str(out_dir)], f"Wz build O{level}", work,
                    env, timeout)
        expect_ok(built, f"Wz build O{level}")
        program = out_dir / "program"
        if not program.is_file():
            fail(f"Wz O{level}: missing {program}")
        executed = run([str(program)], f"Wz run O{level}", work, env, timeout)
        expect_ok(executed, f"Wz run O{level}")
        traces.append((level, executed.stdout, executed.stderr))
    return traces


def real_link_inputs(stage0: pathlib.Path) -> tuple[pathlib.Path, list[pathlib.Path]]:
    """Real C++ header and static libraries next to the stage0 build."""
    build = stage0.parent
    compiler = build.parent.parent
    include = compiler / "include"
    libraries = [
        build / "native" / "liblang_native_ir.a",
        build / "liblang_hir.a",
        build / "liblang_modules.a",
        build / "liblang_frontend.a",
    ]
    if not (include / "lang" / "ir" / "native.hpp").is_file():
        include = ROOT / "include"
    missing = [str(path) for path in libraries if not path.is_file()]
    if missing:
        fail("real C++ oracle libraries missing: " + ", ".join(missing))
    return include, libraries


def compile_real_cpp(source: pathlib.Path, output: pathlib.Path,
                     include: pathlib.Path, libraries: list[pathlib.Path],
                     work: pathlib.Path, env: dict[str, str],
                     timeout: float, label: str) -> None:
    command = ["c++", "-std=c++23", "-O2", "-I", str(include), str(source),
               "-Wl,--start-group", *[str(path) for path in libraries],
               "-Wl,--end-group", "-lgmpxx", "-lgmp", "-ljson-c",
               "-o", str(output)]
    compiled = run(command, f"compile {label}", work, env, timeout)
    expect_ok(compiled, f"compile {label}")


def dump_stage(args: argparse.Namespace, work: pathlib.Path,
               env: dict[str, str], levels: list[int]) -> str:
    include, libraries = real_link_inputs(args.stage0)
    oracle_binary = work / "dump-oracle"
    compile_real_cpp(HERE / "oracle.cpp", oracle_binary, include, libraries,
                     work, env, args.timeout, "dump oracle")
    oracle = run([str(oracle_binary)], "dump oracle", work, env, args.timeout)
    expect_ok(oracle, "dump oracle")
    expected = oracle.stdout
    if len(expected.encode()) != DUMP_BYTES:
        fail(f"oracle: {len(expected.encode())} bytes, expected {DUMP_BYTES}")
    if hashlib.sha256(expected.encode()).hexdigest() != DUMP_SHA256:
        fail("oracle: frozen 989-byte dump hash changed")
    print(f"PASS: real linked C++ dump oracle ({DUMP_BYTES} bytes, "
          f"sha256 {DUMP_SHA256})")

    project = stage_project(work, "dump", DUMP_PACKAGES,
                            HERE / "src" / "main.wz", HERE / "lang.toml")
    traces = build_wz(args.stage0, project, work, env, levels, args.timeout)
    for level, trace, error in traces:
        if error:
            fail(f"Wz O{level} dump wrote stderr:\n{error}")
        if trace != expected:
            diff = "".join(difflib.unified_diff(
                expected.splitlines(keepends=True),
                trace.splitlines(keepends=True),
                fromfile="C++ dump oracle", tofile=f"Wz O{level}"))
            fail(f"Wz O{level} dump differs\n{diff}")
        print(f"PASS: Wz O{level} 989-byte dump matches the real linked C++ "
              f"dump byte-for-byte")
    levels_seen = ",".join(f"O{level}" for level, _, _ in traces)
    if len(traces) > 1:
        first_level, first, _ = traces[0]
        for level, trace, _ in traces[1:]:
            if trace != first:
                fail(f"Wz O{first_level} and O{level} 989-byte dumps differ")
        print(f"PASS: Wz {levels_seen} 989-byte dumps are identical")

    rich_oracle = run([str(oracle_binary), "--rich"], "rich dump oracle", work,
                      env, args.timeout)
    expect_ok(rich_oracle, "rich dump oracle")
    rich_expected = rich_oracle.stdout
    if len(rich_expected.splitlines()) != RICH_LINES:
        fail(f"rich oracle: {len(rich_expected.splitlines())} lines, expected "
             f"{RICH_LINES}")
    for name in OPS:
        if f"{name} type=" not in rich_expected:
            fail(f"rich oracle: missing Op spelling {name}")
    for name in EXITS:
        if f"\n  {name}" not in rich_expected:
            fail(f"rich oracle: missing Exit spelling {name}")
    print(f"PASS: rich real linked C++ dump oracle ({RICH_LINES} lines, "
          f"53 Op / 9 Exit)")

    rich_project = stage_project(work, "rich", DUMP_PACKAGES,
                                 HERE / "rich" / "src" / "main.wz",
                                 HERE / "lang.toml")
    rich_traces = build_wz(args.stage0, rich_project, work, env, levels,
                           args.timeout)
    for level, trace, error in rich_traces:
        if error:
            fail(f"Wz O{level} rich dump wrote stderr:\n{error}")
        if trace != rich_expected:
            diff = "".join(difflib.unified_diff(
                rich_expected.splitlines(keepends=True),
                trace.splitlines(keepends=True),
                fromfile="C++ rich dump oracle", tofile=f"Wz O{level}"))
            fail(f"Wz O{level} rich dump differs\n{diff}")
        print(f"PASS: Wz O{level} rich dump matches the real linked C++ dump "
              f"byte-for-byte")
    if len(rich_traces) > 1:
        first_level, first, _ = rich_traces[0]
        for level, trace, _ in rich_traces[1:]:
            if trace != first:
                fail(f"Wz O{first_level} and O{level} rich dumps differ")
        print(f"PASS: Wz {levels_seen} rich dumps are identical")
    return "passed"


def lower_trace_state(trace: str, prefix: str) -> tuple[int, int, int, int, int]:
    pattern = re.compile(
        rf"^{prefix} state=(\d+) failure=(\d+) value=([01]) "
        rf"diagnostics=(\d+) errors=(\d+)$", re.MULTILINE)
    matches = pattern.findall(trace)
    if len(matches) != 1:
        fail(f"lower trace has {len(matches)} {prefix} state lines, expected 1:"
             f"\n{trace}")
    state, failure, value, diagnostics, errors = matches[0]
    return (int(state), int(failure), int(value), int(diagnostics),
            int(errors))


def lower_stage(args: argparse.Namespace, work: pathlib.Path,
                env: dict[str, str], levels: list[int]) -> str:
    missing = [package for package in LOWER_PACKAGES
               if not (ROOT / "selfhost" / "src" / "wzcc" / package).is_dir()]
    if missing:
        reason = ("missing packages under selfhost/src/wzcc: "
                  + ", ".join(missing))
        print(f"NOT_RUN: lower package check/run: {reason}")
        print("  blocked on: real hirverify/nativeverify/nativelayout/"
              "valueops integration; no stub or fake VerifiedModule is used")
        return "not_run"
    project = stage_project(work, "lowercheck", LOWER_PACKAGES,
                            HERE / "lowercheck" / "src" / "main.wz",
                            HERE / "lowercheck" / "lang.toml")
    traces = build_wz(args.stage0, project, work, env, levels, args.timeout)
    for level, trace, error in traces:
        if error.strip():
            fail(f"Wz O{level} lower wrote stderr:\n{error}")
        if "FAIL:" in trace:
            fail(f"Wz O{level} lower printed a failure:\n{trace}")
        verify = lower_trace_state(trace, "verify")
        lower = lower_trace_state(trace, "lower")
        for name, fields in (("verify", verify), ("lower", lower)):
            state, failure, value, diagnostics, errors = fields
            if state != 0 or value != 1 or errors != 0:
                fail(f"Wz O{level} {name} is not Complete+Some+zero errors: "
                     f"state={state} failure={failure} value={value} "
                     f"diagnostics={diagnostics} errors={errors}")
        if "functions=0" not in trace.splitlines():
            fail(f"Wz O{level} lower trace is missing functions=0:\n{trace}")
        if "dump_lines=1" not in trace.splitlines():
            fail(f"Wz O{level} lower trace is missing dump_lines=1:\n{trace}")
        if not trace.endswith(EMPTY_DUMP):
            fail(f"Wz O{level} lower trace does not end with the fixed "
                 f"complete empty dump:\n{trace}")
        print(f"PASS: Wz O{level} public lower is Complete+Some+zero errors "
              f"with the fixed complete dump:\n{trace.rstrip()}")
    return "passed"


def helper_stage(args: argparse.Namespace, work: pathlib.Path,
                 env: dict[str, str], levels: list[int],
                 layout: pathlib.Path, valueops: pathlib.Path) -> str:
    print("PROVISIONAL: helper stage uses unaccepted candidate dependencies")
    print(f"  nativelayout {layout} sha256={digest(layout)}")
    print(f"  valueops     {valueops} sha256={digest(valueops)}")
    project = stage_helper_project(work, "helper", layout, valueops)
    traces = build_wz(args.stage0, project, work, env, levels, args.timeout)
    for level, trace, error in traces:
        actual = hashlib.sha256(trace.encode()).hexdigest()
        if actual != DUMP_SHA256:
            fail(f"Wz O{level} helper dump sha256 {actual}, expected "
                 f"{DUMP_SHA256}")
        if error != HELPER_MESSAGE:
            fail(f"Wz O{level} helper assertions did not pass:\n{error}")
        print(f"PASS: Wz O{level} helper assertions and 989-byte dump "
              f"(provisional dependencies)")
    return "provisional"


def controls_stage(args: argparse.Namespace, work: pathlib.Path,
                   env: dict[str, str], levels: list[int],
                   layout: pathlib.Path, valueops: pathlib.Path) -> str:
    include, libraries = real_link_inputs(args.stage0)
    identity_binary = work / "identity-reject"
    compile_real_cpp(HERE / "controls" / "identity_reject.cpp",
                     identity_binary, include, libraries, work, env,
                     args.timeout, "identity control")
    identity = run([str(identity_binary)], "identity control", work, env,
                   args.timeout)
    if identity.returncode != 3 or \
       "E_IR_INVALID: module identity index" not in identity.stdout:
        fail("identity control: identity=1 was not rejected with exit 3:\n"
             f"stdout:\n{identity.stdout}stderr:\n{identity.stderr}")
    print("PASS: identity control rejects module identity 1 with "
          "E_IR_INVALID (module id 0 / identity 0 is accepted by the dump "
          "oracle)")

    print("PROVISIONAL: take_function controls use unaccepted candidate "
          "dependencies")
    print(f"  nativelayout {layout} sha256={digest(layout)}")
    print(f"  valueops     {valueops} sha256={digest(valueops)}")
    defect = stage_helper_project(work, "control-defect", layout, valueops,
                                  HERE / "helpers" / "defect_take.wz")
    checked = run([str(args.stage0), "check", "--project",
                   str(defect / "lang.toml"), "--backend", "native",
                   "--format", "json"], "defect take_function check", work,
                  env, args.timeout)
    combined = checked.stdout + checked.stderr
    if checked.returncode == 0:
        fail("defect take_function check unexpectedly succeeded")
    if "E_TYPE_MISMATCH" not in combined or "defect_take.wz" not in combined:
        fail("defect take_function check did not report E_TYPE_MISMATCH at "
             f"defect_take.wz:\n{combined}")
    print("PASS: confirmed minimal reproduction is rejected with "
          "E_TYPE_MISMATCH")

    control = stage_helper_project(work, "control-fixed", layout, valueops,
                                   HERE / "helpers" / "control_take.wz")
    traces = build_wz(args.stage0, control, work, env, levels, args.timeout)
    for level, trace, error in traces:
        actual = hashlib.sha256(trace.encode()).hexdigest()
        if actual != DUMP_SHA256:
            fail(f"Wz O{level} control dump sha256 {actual}, expected "
                 f"{DUMP_SHA256}")
        if error != HELPER_MESSAGE:
            fail(f"Wz O{level} control assertions did not pass:\n{error}")
        print(f"PASS: Wz O{level} fixed take_function control passes and "
              f"executes")
    return "provisional"


def probe_helper(path: pathlib.Path, fixture: pathlib.Path,
                 work: pathlib.Path, env: dict[str, str],
                 timeout: float) -> tuple[bool, str]:
    if not path.is_file():
        return False, f"helper not found: {path}"
    probe = run([str(path), "--project", str(fixture / "lang.toml"),
                 "--check-only", "--format", "json"], "cfg helper probe",
                work, env, timeout)
    if probe.returncode != 0:
        detail = (probe.stdout + probe.stderr).strip().splitlines()
        return False, (detail[-1] if detail else f"exit {probe.returncode}")
    return True, ""


def find_native_helper(args: argparse.Namespace, fixture: pathlib.Path,
                       work: pathlib.Path, env: dict[str, str],
                       timeout: float) -> pathlib.Path | None:
    candidates: list[pathlib.Path] = [args.stage0.parent / "native"
                                      / "lang_native_helper"]
    for base in (args.stage0.parent.parent, ROOT / "build"):
        candidates.extend(sorted(base.glob("*/native/lang_native_helper")))
    seen: set[pathlib.Path] = set()
    for candidate in candidates:
        candidate = candidate.resolve()
        if candidate in seen:
            continue
        seen.add(candidate)
        ok, reason = probe_helper(candidate, fixture, work, env, timeout)
        if ok:
            return candidate
        print(f"  skipping {candidate}: {reason}")
    return None


def cfg_stage(args: argparse.Namespace, work: pathlib.Path,
              env: dict[str, str], levels: list[int]) -> str:
    fixture = HERE / "fixtures" / "cfg"
    helper = (pathlib.Path(args.native_helper).expanduser().resolve()
              if args.native_helper
              else find_native_helper(args, fixture, work, env, args.timeout))
    if helper is None:
        print("NOT_RUN: real-program cfg comparison: no C++ helper")
        return "not_run"
    print(f"cfg C++ oracle helper: {helper}")
    ok, reason = probe_helper(helper, fixture, work, env, args.timeout)
    if not ok:
        print(f"NOT_RUN: real-program cfg comparison: C++ helper cannot read "
              f"the .wz fixture: {reason}")
        return "not_run"
    print("PASS: C++ helper accepts the .wz fixture (no .lang scan assumed)")
    if not args.wz_native_helper:
        print("NOT_RUN: real-program cfg comparison: no --wz-native-helper")
        print("  next step: point --wz-native-helper at a Wz-built helper "
              "that accepts lang_native_helper's CLI after Root integration")
        return "not_run"
    wz_helper = pathlib.Path(args.wz_native_helper).expanduser().resolve()
    ok, reason = probe_helper(wz_helper, fixture, work, env, args.timeout)
    if not ok:
        fail(f"Wz helper rejected the cfg fixture: {reason}")
    for level in levels:
        expected_path = work / f"cfg-oracle-O{level}.nir"
        actual_path = work / f"cfg-wz-O{level}.nir"
        built = run([str(helper), "--project", str(fixture / "lang.toml"),
                     "--opt", str(level), "--out-object",
                     str(work / f"cfg-oracle-O{level}.o"), "--out-native",
                     str(expected_path)], f"cfg oracle O{level}", work, env,
                    args.timeout)
        expect_ok(built, f"cfg oracle O{level}")
        built = run([str(wz_helper), "--project", str(fixture / "lang.toml"),
                     "--opt", str(level), "--out-object",
                     str(work / f"cfg-wz-O{level}.o"), "--out-native",
                     str(actual_path)], f"cfg Wz O{level}", work, env,
                    args.timeout)
        expect_ok(built, f"cfg Wz O{level}")
        expected = expected_path.read_text()
        actual = actual_path.read_text()
        if actual != expected:
            diff = "".join(difflib.unified_diff(
                expected.splitlines(keepends=True),
                actual.splitlines(keepends=True),
                fromfile=f"C++ helper O{level}",
                tofile=f"Wz helper O{level}"))
            fail(f"cfg O{level} differs\n{diff}")
        print(f"PASS: cfg O{level} CFG dump/source/ids match the C++ helper "
              f"({len(expected.splitlines())} lines)")
    if len(levels) > 1:
        texts = [(level, (work / f"cfg-oracle-O{level}.nir").read_text())
                 for level in levels]
        first_level, first = texts[0]
        for level, text in texts[1:]:
            if text != first:
                fail(f"cfg oracle O{first_level} and O{level} dumps differ")
        print(f"PASS: cfg oracle O{'/O'.join(str(level) for level in levels)} "
              f"dumps are identical")
    return "passed"


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    args.stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    toolchain = pathlib.Path(args.toolchain_root).expanduser().resolve()
    if not args.stage0.is_file():
        fail(f"stage0 compiler missing: {args.stage0}")
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")
    levels = args.opt or [0, 2]
    if args.work_dir:
        work = pathlib.Path(args.work_dir).expanduser().resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        # Disk-backed staging inside this worktree; never write to tmpfs or
        # another worker's tree.
        base = HERE / ".work-tmp"
        base.mkdir(parents=True, exist_ok=True)
        work = pathlib.Path(tempfile.mkdtemp(prefix="nativelower-", dir=base))
    env = environment(toolchain, work)
    layout = (pathlib.Path(args.nativelayout).expanduser().resolve()
              if args.nativelayout else None)
    valueops = (pathlib.Path(args.valueops).expanduser().resolve()
                if args.valueops else None)
    provisional = []
    if layout is not None and layout.is_file():
        provisional.append(("nativelayout", layout))
    if valueops is not None and valueops.is_file():
        provisional.append(("valueops", valueops))
    print(f"stage0: {args.stage0}")
    print(f"work-dir: {work}")
    print(f"TMPDIR: {env['TMPDIR']}")
    for package in DUMP_PACKAGES + [p for p in LOWER_PACKAGES
                                    if p not in DUMP_PACKAGES]:
        directory = ROOT / "selfhost" / "src" / "wzcc" / package
        if directory.is_dir():
            for file in sorted(directory.glob("*.wz")):
                print(f"{package}/{file.name} sha256: {digest(file)}")
        else:
            print(f"{package}: absent (not_run)")
    print(f"oracle.cpp sha256: {digest(HERE / 'oracle.cpp')}")
    for name, path in provisional:
        print(f"{name} (provisional): {path} sha256: {digest(path)}")

    statuses: dict[str, str] = {}
    if not args.skip_dump:
        dump_stage(args, work, env, levels)
        statuses["dump"] = "passed"
    else:
        print("NOT_RUN: dump stage (--skip-dump)")
        statuses["dump"] = "not_run"

    if args.skip_helper:
        print("NOT_RUN: helper stage (--skip-helper)")
        statuses["helper"] = "not_run"
    elif layout is not None and layout.is_file() and \
            valueops is not None and valueops.is_file():
        statuses["helper"] = helper_stage(args, work, env, levels, layout,
                                          valueops)
    else:
        print("NOT_RUN: helper stage: pass --nativelayout and --valueops "
              "candidate files")
        statuses["helper"] = "not_run"

    if args.skip_controls:
        print("NOT_RUN: control stage (--skip-controls)")
        statuses["controls"] = "not_run"
    elif layout is not None and layout.is_file() and \
            valueops is not None and valueops.is_file():
        statuses["controls"] = controls_stage(args, work, env, levels, layout,
                                              valueops)
    else:
        print("NOT_RUN: control stage: pass --nativelayout and --valueops "
              "candidate files")
        statuses["controls"] = "not_run"

    statuses["lower"] = lower_stage(args, work, env, levels)
    statuses["cfg"] = cfg_stage(args, work, env, levels)
    print("summary: " + " ".join(f"{name}={status}"
                                 for name, status in statuses.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
