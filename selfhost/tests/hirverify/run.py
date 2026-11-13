#!/usr/bin/env python3
"""Reproduce the HIR verifier component and flow exit-identity regressions.

The six original modes are Wz tests that assemble HIR from real parser ASTs.
They are built and executed at O0 and O2, and their reports are checked against
the same C++ verifier and against fixed expected values.  The exit-identity
case is different: its HIR construction is emitted by a C++ oracle from the
real resolve/check result for the checked-in fixture, then the generated Wz
builder is compiled and run against the current verifier.  The old generated
file is deliberately not checked in.

This is a component test.  It does not run Wz Sema on arbitrary source, does
not build Native/Python backends, and does not claim a complete frontend,
corpus or self-hosting result.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SELFHOST = ROOT / "selfhost" / "src" / "wzcc"
STDLIB = ROOT / "stdlib"
DEFAULT_WOOZYC = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
DEFAULT_BUILD_DIR = DEFAULT_WOOZYC.parent
DEFAULT_WORK = ROOT / "build" / "hirverify-runs"
sys.path.insert(0, str(HERE.parents[1]))
from corpus import _run_process_impl  # noqa: E402

# Explicit import closure of the two test drivers; only these packages are
# staged into the private work project.
PACKAGES = (
    "bigint", "constant", "foreign", "hir", "hirclone", "hirverify",
    "lexer", "literal", "model", "number", "ownership", "parser",
    "pattern", "signature", "source", "system", "types", "valueops",
)
STD_PACKAGES = (
    "fs", "io", "option", "os", "path", "process", "result", "strconv",
    "text", "vec",
)
ORACLE_LIBS = (
    "liblang_sema.a", "liblang_hir.a", "liblang_modules.a",
    "liblang_frontend.a",
)
FIXTURES = {
    "unit": ("fixture.wz", "unit"),
    "nonunit": ("nonunit.wz", "nonunit"),
    "template0": ("template.wz", "template0"),
    "template1": ("template.wz", "template1"),
    "trait": ("trait-impl.wz", "trait"),
    "throw": ("throw.wz", "throw"),
}
CHECK_LABELS = {
    "nonunit": ("fixture-build-nonunit", "type-nonunit", "capability-nonunit",
                "verified-nonunit", "snapshot-nonunit", "malformed-nonunit"),
    "template0": ("fixture-build-template0", "type-template0",
                  "capability-template0", "verified-template0",
                  "snapshot-template0", "malformed-template0"),
    "template1": ("fixture-build-template1", "type-template1",
                  "capability-template1", "verified-template1",
                  "snapshot-template1", "malformed-template1"),
    "trait": ("fixture-build-trait", "type-trait", "capability-trait",
              "verified-trait", "snapshot-trait", "trait-overlap-negative"),
    "throw": ("fixture-build-throw", "type-throw", "capability-throw",
              "verified-throw", "snapshot-throw", "malformed-throw"),
}
UNIT_EXPECTED = {
    "valid": "state=complete diags=0 detail=none",
    "backend": "state=complete diags=0 detail=none",
    "verify": "state=complete diags=0 has=1",
    "snapshot_children": "0",
    "snapshot_after_builder": "0:2",
    "snapshot_after_returned": "0:2",
    "bad-width": "state=failed detail=E_IR_INVALID:invalid target word width",
    "bad-type": "detail=E_IR_INVALID:missing or out-of-range TypeId",
    "bad-child": "detail=E_IR_INVALID:out-of-range HirNodeId",
    "bad-source": "detail=E_IR_INVALID:invalid source span",
    "bad-unsupported": "detail=E_IR_INVALID:unsupported HIR node: probe",
    "bad-cycle": "detail=E_IR_INVALID:HIR node cycle or shared executable child",
    "bad-scope": "detail=E_IR_INVALID:function scope ownership",
    "bad-verify": "has=0 detail=E_IR_INVALID:function scope ownership",
}
SNAPSHOT_KEYS = ("snapshot_children", "snapshot_after_builder",
                 "snapshot_after_returned")
EXIT_EXPECTED = (
    "TYPE complete=1 diag=none",
    "CAP complete=1 diag=none",
    "VERIFY complete=1 has=1 diag=none",
    "BAD_RETURN rejected=1 diag=E_IR_INVALID:lowering:error:primary=0:"
    "return target function mismatch",
    "BAD_EXIT rejected=1 diag=E_IR_INVALID:lowering:error:primary=0:"
    "exit target/scopes/cleanup order/value preservation mismatch",
)
ALIASES = {
    (2, 12), (1, 13), (0, 14), (2, 15), (12, 15),
    (1, 16), (13, 16), (0, 17), (14, 17),
}


def fail(message: str) -> None:
    raise RuntimeError(message)


def finite_timeout(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "--timeout must be finite and positive") from error
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("--timeout must be finite and positive")
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", default=str(DEFAULT_WOOZYC),
                        help="stage0 woozyc executable")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "g++"),
                        help="C++ compiler for the test oracle")
    parser.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR),
                        help="real stage0 build directory with liblang_*.a")
    parser.add_argument("--work-dir", default=str(DEFAULT_WORK),
                        help="parent directory for retained run artifacts")
    parser.add_argument("--toolchain-root", default=os.environ.get(
                            "WOOZY_TOOLCHAIN_ROOT", ""),
                        help="directory containing llvm-22.1.8 and "
                             "libxml2-legacy-2.13.9 (default: repository "
                             "sibling or the stage0 checkout sibling)")
    parser.add_argument("--timeout", type=finite_timeout, default=900.0,
                        help="per build/oracle command timeout in seconds")
    parser.add_argument("--program-timeout", type=finite_timeout, default=30.0,
                        help="per generated program/oracle run timeout")
    return parser.parse_args()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def file_record(path: Path) -> dict[str, object]:
    return {"path": str(path), "size": path.stat().st_size,
            "sha256": sha256(path)}


def snapshot_tree(root: Path) -> dict[str, str]:
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_file():
            result[path.relative_to(root).as_posix()] = sha256(path)
    return result


def snapshot_sources() -> dict[str, str]:
    result = {}
    suffixes = {".wz", ".cpp", ".py", ".md", ".toml"}
    for path in sorted(HERE.rglob("*")):
        if path.is_file() and path.suffix in suffixes:
            result[path.relative_to(HERE).as_posix()] = sha256(path)
    return result


def resolve_toolchain(args: argparse.Namespace, woozyc: Path) -> Path:
    if args.toolchain_root:
        return Path(args.toolchain_root).expanduser().resolve()
    sibling = ROOT.parent / "toolchains"
    if sibling.is_dir():
        return sibling
    if len(woozyc.parents) > 3:
        checkout_sibling = woozyc.parents[3] / "toolchains"
        if checkout_sibling.is_dir():
            return checkout_sibling
    return sibling


def environment(toolchain_root: Path, work: Path) -> dict[str, str]:
    env = dict(os.environ)
    paths = [
        str(toolchain_root / "llvm-22.1.8" / "lib"),
        str(toolchain_root / "libxml2-legacy-2.13.9" / "usr" / "lib"),
    ]
    if env.get("LD_LIBRARY_PATH"):
        paths.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(paths)
    env["LC_ALL"] = "C"
    env["TMPDIR"] = str(work / "tmp")
    (work / "tmp").mkdir(parents=True, exist_ok=True)
    return env


def command(argv: list[str], label: str, work: Path, timeout: float,
            env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    if not math.isfinite(timeout) or timeout <= 0:
        raise RuntimeError(f"{label}: timeout must be finite and positive")
    print("+", " ".join(str(part) for part in argv), flush=True)
    started = time.monotonic()
    process = _run_process_impl(argv, work, env, timeout)
    elapsed = time.monotonic() - started
    stdout_bytes = process["stdout"]
    stderr_bytes = process["stderr"]
    (work / f"{label}.stdout").write_bytes(stdout_bytes)
    (work / f"{label}.stderr").write_bytes(stderr_bytes)
    stdout = stdout_bytes.decode("utf-8", errors="replace")
    stderr = stderr_bytes.decode("utf-8", errors="replace")
    record = {
        "label": label,
        "argv": [str(part) for part in argv],
        "cwd": str(work),
        "timeout_seconds": timeout,
        "elapsed_seconds": elapsed,
        "exit": process["exit"],
        "timed_out": process["timed_out"],
        "cleanup_complete": process["cleanup_complete"],
        "abandoned_pipes": process["abandoned_pipes"],
        "stdout_path": f"{label}.stdout",
        "stderr_path": f"{label}.stderr",
        "stdout_sha256": hashlib.sha256(stdout_bytes).hexdigest(),
        "stderr_sha256": hashlib.sha256(stderr_bytes).hexdigest(),
        "stdout_bytes": len(stdout_bytes),
        "stderr_bytes": len(stderr_bytes),
    }
    (work / f"{label}.json").write_text(json.dumps(record, indent=2) + "\n")
    if (process["timed_out"] or process["exit"] != 0
            or not process["cleanup_complete"] or process["abandoned_pipes"]):
        raise RuntimeError(
            f"{label}: exit {process['exit']} timed_out={process['timed_out']} "
            f"cleanup_complete={process['cleanup_complete']} "
            f"abandoned_pipes={process['abandoned_pipes']}\n{stdout}{stderr}")
    return subprocess.CompletedProcess(argv, process["exit"], stdout, stderr)


def require_empty_stderr(result: subprocess.CompletedProcess[str],
                         label: str) -> None:
    if result.stderr:
        fail(f"{label}: unexpected stderr: {result.stderr!r}")


def compiler_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    try:
        report = json.loads(result.stdout)
    except ValueError as error:
        raise RuntimeError(
            f"{label}: invalid compiler JSON: {result.stdout}") from error
    if report.get("status") != "ok":
        raise RuntimeError(f"{label}: compiler did not succeed: {result.stdout}")


def stage_project(project: Path, main: Path, manifest: Path,
                  generated: bool = False) -> None:
    source = project / "src"
    modules = source / "wzcc"
    modules.mkdir(parents=True)
    for name in PACKAGES:
        shutil.copytree(SELFHOST / name, modules / name)
    for name in STD_PACKAGES:
        shutil.copytree(STDLIB / name, source / "std" / name)
    source.mkdir(parents=True, exist_ok=True)
    shutil.copy2(main, source / "main.wz")
    if not generated:
        shutil.copy2(HERE / "src" / "fixes.wz", source / "fixes.wz")
    shutil.copy2(manifest, project / "lang.toml")


def build_oracle(args: argparse.Namespace, source: Path, output: Path,
                 evidence: Path, env: dict[str, str], label: str) -> None:
    build_dir = Path(args.build_dir).expanduser().resolve()
    for name in ORACLE_LIBS:
        if not (build_dir / name).is_file():
            fail(f"oracle library missing: {build_dir / name}")
    argv = [args.cxx, "-std=c++20", "-O0", "-g0", "-I", str(ROOT / "include"),
            str(source), "-Wl,--start-group",
            *[str(build_dir / name) for name in ORACLE_LIBS],
            "-Wl,--end-group", "-ljson-c", "-ltomlplusplus", "-lgmpxx",
            "-lgmp", "-lm", "-o", str(output)]
    command(argv, label, evidence, args.timeout, env)


def probe_map(output: str, prefix: str = "PROBE ") -> dict[str, str]:
    result = {}
    for line in output.splitlines():
        if not line.startswith(prefix):
            continue
        body = line[len(prefix):]
        if " " in body:
            key, value = body.split(" ", 1)
        elif "=" in body:
            key, value = body.split("=", 1)
        else:
            fail(f"malformed probe line: {line!r}")
        if key in result:
            fail(f"duplicate probe line: {line!r}")
        result[key] = value
    return result


def check_unit(output: str, fixture: Path, label: str) -> dict[str, str]:
    lines = output.splitlines()
    probes = probe_map(output)
    if len(probes) != 14:
        fail(f"{label}: {len(probes)} PROBE lines, expected 14 (source "
             f"SUMMARY still says probes=13)")
    expected = {key: UNIT_EXPECTED[key] for key in UNIT_EXPECTED
                if key not in SNAPSHOT_KEYS}
    for key, value in expected.items():
        if probes.get(key) != value:
            fail(f"{label}: {key}: {probes.get(key)!r}, expected {value!r}")
    for key in SNAPSHOT_KEYS:
        if probes.get(key) != UNIT_EXPECTED[key]:
            fail(f"{label}: {key}: {probes.get(key)!r}, expected "
                 f"{UNIT_EXPECTED[key]!r}")
    summaries = [line for line in lines if line.startswith("SUMMARY ")]
    if summaries != [f"SUMMARY probes=13 fixture={fixture}"]:
        fail(f"{label}: unexpected SUMMARY: {summaries!r}")
    return probes


def check_five_mode(output: str, mode: str, fixture: Path) -> None:
    lines = output.splitlines()
    labels = CHECK_LABELS[mode]
    check_lines = [line for line in lines if line.startswith("CHECK ")]
    if len(check_lines) != len(labels):
        fail(f"{mode}: {len(check_lines)} CHECK lines, expected {len(labels)}")
    for label in labels:
        if f"CHECK {label} ok" not in check_lines:
            fail(f"{mode}: missing CHECK {label} ok in {check_lines!r}")
    if any(" FAIL" in line for line in check_lines):
        fail(f"{mode}: failed CHECK line: {check_lines!r}")
    summaries = [line for line in lines
                 if line.startswith("SUMMARY mode=")]
    expected = f"SUMMARY mode={mode} fixture={fixture} failures=0"
    if summaries != [expected]:
        fail(f"{mode}: unexpected SUMMARY: {summaries!r}")


def check_cpp_mode(output: str, mode: str) -> None:
    probes = probe_map(output)
    expected = {
        f"cpp-type-{mode}": "state=complete diags=0 detail=none",
        f"cpp-capability-{mode}": "state=complete diags=0",
        f"cpp-verify-{mode}": "state=complete has=1 diags=0",
    }
    if mode == "trait":
        expected[f"cpp-trait-overlap"] = (
            "state=failed detail=E_IR_INVALID:duplicate or overlapping impl "
            "patterns")
    if probes != expected:
        fail(f"C++ {mode}: unexpected probes {probes!r}, expected {expected!r}")


def check_cpp_unit(output: str, fixture: Path) -> dict[str, str]:
    probes = probe_map(output)
    expected = {key: UNIT_EXPECTED[key] for key in UNIT_EXPECTED
                if key not in SNAPSHOT_KEYS}
    if probes != expected:
        fail(f"C++ unit: unexpected probes {probes!r}, expected {expected!r}")
    summaries = [line for line in output.splitlines()
                 if line.startswith("SUMMARY ")]
    if summaries != [f"SUMMARY probes=13 fixture={fixture}"]:
        fail(f"C++ unit: unexpected SUMMARY: {summaries!r}")
    return probes


def check_exit_oracle(output: str, fixture: Path) -> dict[str, object]:
    lines = output.splitlines()
    if "CPP public verify=1 diagnostics=0" not in lines:
        fail(f"exit oracle did not accept the real source: {lines!r}")
    aliases = set()
    for line in lines:
        if line.startswith("SOURCE_ALIAS "):
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            aliases.add((int(fields["first"]), int(fields["later"])))
    if aliases != ALIASES:
        fail(f"exit oracle source aliases changed: {sorted(aliases)!r}")
    targets = [line for line in lines if line.startswith("TARGET_RETURN ")]
    if targets != ["TARGET_RETURN 16"]:
        fail(f"exit oracle target changed: {targets!r}")
    expected = (
        "CPP bad-return verify=0 diagnostic=E_IR_INVALID:return target "
        "function mismatch",
        "CPP bad-exit verify=0 diagnostic=E_IR_INVALID:exit "
        "target/scopes/cleanup order/value preservation mismatch",
    )
    for line in expected:
        if line not in lines:
            fail(f"exit oracle missing negative result: {line!r}")
    return {"aliases": sorted(aliases), "target": 16}


def build_and_run_six(args: argparse.Namespace, work: Path, evidence: Path,
                      env: dict[str, str]) -> dict[str, object]:
    project = work / "six" / "project"
    stage_project(project, HERE / "src" / "main.wz", HERE / "lang.toml")
    source_before = snapshot_tree(project / "src")
    oracle = work / "six" / "oracle"
    build_oracle(args, HERE / "oracle.cpp", oracle, evidence, env,
                 "six-oracle-build")
    oracle_outputs: dict[str, str] = {}
    for mode, (fixture_name, argument) in FIXTURES.items():
        fixture = (HERE / fixture_name).resolve()
        result = command([str(oracle), str(fixture), argument],
                         f"oracle-{mode}", evidence, args.program_timeout, env)
        require_empty_stderr(result, f"oracle-{mode}")
        oracle_outputs[mode] = result.stdout
        if mode == "unit":
            cpp_unit = check_cpp_unit(result.stdout, fixture)
        else:
            check_cpp_mode(result.stdout, mode)
    manifest = str(project / "lang.toml")
    reports: dict[int, dict[str, str]] = {}
    programs: dict[str, object] = {}
    for opt in (0, 2):
        out_dir = work / "six" / f"out-o{opt}"
        built = command([str(Path(args.woozyc).expanduser().resolve()),
                         "build", "--project", manifest, "--backend", "native",
                         "--opt", str(opt), "--out-dir", str(out_dir),
                         "--format", "json"],
                        f"six-build-o{opt}", evidence, args.timeout, env)
        compiler_ok(built, f"six-build-o{opt}")
        program = out_dir / "program"
        programs[f"O{opt}"] = {
            "path": str(program),
            "sha256": sha256(program),
        }
        reports[opt] = {}
        for mode, (fixture_name, argument) in FIXTURES.items():
            fixture = (HERE / fixture_name).resolve()
            result = command([str(program), str(fixture), argument],
                             f"six-O{opt}-{mode}", evidence,
                             args.program_timeout, env)
            require_empty_stderr(result, f"six O{opt} {mode}")
            reports[opt][mode] = result.stdout
            if mode == "unit":
                checks = check_unit(result.stdout, fixture, f"O{opt} unit")
                for key, value in cpp_unit.items():
                    if checks.get(key) != value:
                        fail(f"O{opt} unit: Wz {key} {checks.get(key)!r} != "
                             f"C++ {value!r}")
            else:
                check_five_mode(result.stdout, mode, fixture)
    for mode in FIXTURES:
        if reports[0][mode] != reports[2][mode]:
            fail(f"{mode}: O0 and O2 reports differ")
    return {"source_before": source_before, "programs": programs,
            "reports": {f"O{opt}": reports[opt] for opt in reports},
            "oracle": {mode: hashlib.sha256(value.encode()).hexdigest()
                       for mode, value in oracle_outputs.items()}}


def build_and_run_exit_identity(args: argparse.Namespace, work: Path,
                                evidence: Path,
                                env: dict[str, str]) -> dict[str, object]:
    fixture = (HERE / "exit_identity" / "fixture.wz").resolve()
    project = work / "exit-identity" / "project"
    stage_project(project, HERE / "exit_identity" / "src" / "main.wz",
                  HERE / "exit_identity" / "lang.toml", generated=True)
    oracle = work / "exit-identity" / "oracle"
    build_oracle(args, HERE / "exit_identity" / "oracle.cpp", oracle,
                 evidence, env, "exit-oracle-build")
    generated = project / "src" / "generated.wz"
    generated_result = command([str(oracle), str(fixture), str(generated)],
                               "exit-oracle", evidence, args.program_timeout,
                               env)
    require_empty_stderr(generated_result, "exit oracle")
    if not generated.is_file() or "func build_generated" not in \
            generated.read_text(encoding="utf-8"):
        fail("exit oracle did not emit a Wz builder")
    source_before = snapshot_tree(project / "src")
    oracle_info = check_exit_oracle(generated_result.stdout, fixture)

    manifest = str(project / "lang.toml")
    reports: dict[int, str] = {}
    programs: dict[str, object] = {}
    for opt in (0, 2):
        out_dir = work / "exit-identity" / f"out-o{opt}"
        built = command([str(Path(args.woozyc).expanduser().resolve()),
                         "build", "--project", manifest, "--backend", "native",
                         "--opt", str(opt), "--out-dir", str(out_dir),
                         "--format", "json"],
                        f"exit-build-o{opt}", evidence, args.timeout, env)
        compiler_ok(built, f"exit-build-o{opt}")
        program = out_dir / "program"
        programs[f"O{opt}"] = {"path": str(program), "sha256": sha256(program)}
        result = command([str(program), str(fixture)],
                         f"exit-run-o{opt}", evidence, args.program_timeout,
                         env)
        require_empty_stderr(result, f"exit O{opt}")
        lines = tuple(result.stdout.splitlines())
        if lines != EXIT_EXPECTED:
            fail(f"exit O{opt}: unexpected report: {lines!r}, expected "
                 f"{EXIT_EXPECTED!r}")
        reports[opt] = result.stdout
    if reports[0] != reports[2]:
        fail("exit identity: O0 and O2 reports differ")
    return {
        "source_before": source_before,
        "fixture": file_record(fixture),
        "generated": file_record(generated),
        "oracle": oracle_info,
        "programs": programs,
        "reports": {f"O{opt}": reports[opt] for opt in reports},
    }


def main() -> int:
    args = parse_args()
    woozyc = Path(args.woozyc).expanduser().resolve()
    if not woozyc.is_file():
        fail(f"stage0 compiler missing: {woozyc}")
    build_dir = Path(args.build_dir).expanduser().resolve()
    if not build_dir.is_dir():
        fail(f"build directory missing: {build_dir}")
    toolchain_root = resolve_toolchain(args, woozyc)
    parent = Path(args.work_dir).expanduser().resolve()
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="hirverify-", dir=parent)).resolve()
    evidence = work / "evidence"
    evidence.mkdir()
    env = environment(toolchain_root, work)
    print(f"work-dir: {work}", flush=True)

    state: dict[str, object] = {
        "status": "running",
        "work_dir": str(work),
        "woozyc": file_record(woozyc),
        "build_dir": str(build_dir),
        "toolchain_root": str(toolchain_root),
        "timeout_seconds": args.timeout,
        "program_timeout_seconds": args.program_timeout,
        "libraries": {name: file_record(build_dir / name)
                      for name in ORACLE_LIBS},
    }
    try:
        helper = build_dir / "native" / "lang_native_helper"
        runtime = build_dir / "native" / "liblang_native_runtime.a"
        state["helper"] = file_record(helper) if helper.is_file() else None
        state["runtime"] = file_record(runtime) if runtime.is_file() else None
        tools = {}
        for name, relative in (
                ("clang", "llvm-22.1.8/bin/clang"),
                ("clang++", "llvm-22.1.8/bin/clang++"),
                ("ld.lld", "llvm-22.1.8/bin/ld.lld"),
                ("libxml2", "libxml2-legacy-2.13.9/usr/lib/libxml2.so.2")):
            path = toolchain_root / relative
            tools[name] = file_record(path) if path.is_file() else None
        state["tools"] = tools
        cxx = Path(args.cxx)
        if not cxx.is_file():
            found = shutil.which(args.cxx)
            if found:
                cxx = Path(found)
        state["cxx"] = file_record(cxx.resolve()) if cxx.is_file() else {
            "path": args.cxx}
        state["package_sources"] = {
            name: snapshot_tree(SELFHOST / name) for name in PACKAGES}
        state["stdlib_sources"] = {
            name: snapshot_tree(STDLIB / name) for name in STD_PACKAGES}
        repo_before = snapshot_sources()
        six = build_and_run_six(args, work, evidence, env)
        identity = build_and_run_exit_identity(args, work, evidence, env)
        staged_before = {"six": six.pop("source_before"),
                         "exit-identity": identity.pop("source_before")}
        staged_after = {
            name: snapshot_tree(work / name / "project" / "src")
            for name in ("six", "exit-identity")}
        repo_after = snapshot_sources()
        if repo_before != repo_after:
            changed = sorted(set(repo_before) | set(repo_after))
            fail(f"repository test sources changed: {changed}")
        state["source_before"] = {"repo": repo_before, "staged": staged_before}
        state["source_after"] = {"repo": repo_after, "staged": staged_after}
        if state["source_before"] != state["source_after"]:
            fail("staged source changed during the run")
        state["six"] = six
        state["exit_identity"] = identity
        records = [json.loads(path.read_text())
                   for path in sorted(evidence.glob("*.json"))]
        state["commands"] = records
        state["status"] = "passed"
        (work / "results.json").write_text(json.dumps(state, indent=2) + "\n")
        print(json.dumps({
            "status": "passed",
            "work_dir": str(work),
            "six_programs": six["programs"],
            "exit_programs": identity["programs"],
            "exit_generated_sha256": identity["generated"]["sha256"],
        }, indent=2))
        return 0
    except Exception as error:  # noqa: BLE001 - retain exact failure evidence
        state["status"] = "failed"
        state["error"] = repr(error)
        try:
            state["commands"] = [json.loads(path.read_text())
                                 for path in sorted(evidence.glob("*.json"))]
        except (OSError, ValueError):
            state["commands"] = []
        (work / "results.json").write_text(json.dumps(state, indent=2) + "\n")
        print(json.dumps({"status": "failed", "work_dir": str(work),
                          "error": repr(error)}, indent=2))
        return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
