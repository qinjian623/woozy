#!/usr/bin/env python3
"""SH15 C-FFI forged HIR/NativeIR Wz-component evidence runner.

The runner is test-only.  It stages the pinned production Source116 plus this
directory's private ``main.wz`` entry, builds that private component once at
O2 with the fixed C++ seed, and runs it once on
``tests/native/c_ffi/forged-project``.

The Wz component itself performs all Source analysis and verification through
the real production modules.  The runner never builds, links or executes the
forged C target, never invokes the old C++ frontend for the candidate work, and
never rewrites Source, fixtures, bindings, gold data, the catalog or PLAN.

A pure negative-control mode (``--self-test-controls``) exercises only the
summary parser and strict process-record checks with mock transport records.
It executes no external command.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_SOURCE_ROOT = ROOT / "selfhost/src"
DEFAULT_MANIFEST = ROOT / "tests/native/c_ffi/forged-project/lang.toml"
DEFAULT_BINDING = ROOT / "tests/native/c_ffi/bindings/fixture.json"
DEFAULT_CPP_TEST = ROOT / "tests/native/c_ffi/forged_test.cpp"
DEFAULT_CPP_CONTRACT = ROOT / "tests/native/c_ffi/CONTRACT.md"
DEFAULT_CPP_RUNNER = ROOT / "tests/native/c_ffi/run_c_ffi.py"
DEFAULT_SEED = Path("/home/qin/.local/state/cli-agent-bridge/worktrees/"
                    "0a0ca4cad6525fc6d10ec8f3/.work/gc-registry-index/"
                    "build-new/woozyc")
DEFAULT_STDLIB = Path("/home/qin/.local/state/cli-agent-bridge/worktrees/"
                      "0a0ca4cad6525fc6d10ec8f3/stdlib")
DEFAULT_TOOLCHAIN = Path("/home/qin/Work/language-dev-20261103/toolchains")
DEFAULT_STRACE = DEFAULT_TOOLCHAIN / "strace-7.2/usr/bin/strace"
EXPECTED_SOURCE116 = "ed1c65fb9889606a713f03cc4e02d7bd6d818c58e4937610554cdd3f3e866a09"
EXPECTED_STDLIB = "473aa54b5ce5c1bbae4985d0d187528aa056a72ddc1737c3f05d3abb9623bd47"
EXPECTED_SEED = "565da3f6aa0b7f342ac5ba9523b5495643c1e44daaa5ecbb072e5906bd1a1fe4"
EXPECTED_HELPER = "3b2e489d88997d7c76ff9ae4827aa560c2547fb1832ebdf7f9427b42f376649e"
EXPECTED_NATIVE_RUNTIME = "4df15518f676291522878563e86bb71c99e53d3dad8872f620ec5515acc3a702"
EXPECTED_HOSTED_RUNTIME = "7f113be3fab32a62c263a1681ca2b3b34b76a63d6f74f679cce2ac7586a1f75b"
TOOL_NAMES = ("clang", "ld.lld", "opt", "llc", "llvm-readelf", "llvm-nm")
EXPECTED_TOOLS = {
    "clang": "31a40dc31f3c15a47aa119aa148f339bf363d8f61202ddbdacbd3f24b71ba113",
    "ld.lld": "a37ea59e863b4c1fd95ead6a905f7ddb461f7d90d4f30d2d76bcbb2ae9588b1d",
    "opt": "fc19bcd0ab81c35746c93aad355e8bb81f34ab070e98014cf06ce8433e91d4b1",
    "llc": "75312287569eb638c8540a8282b5e611cd438de694ebc9e116921bdc252fdc21",
    "llvm-readelf": "338c233bc2419b0abdb8f0344912504c26eeab1cd671d6651f4bb8405bb48385",
    "llvm-nm": "3f2d9af2cd8640a62796191afe29fca84530a2ecdd1b1995f34e1ee235d9ec88",
}
EXPECTED_CASES = (
    "hir_c_callable",
    "hir_signature_type_id",
    "hir_arg_indices",
    "hir_c_field",
    "native_c_callable",
    "native_origin_foreign_call",
    "native_c_field",
    "native_origin_c_ptr_write",
)
CASE_RE = re.compile(r"^CASE (\S+) rejected=E_IR_INVALID baseline=ok$")
HIR_COVERAGE_RE = re.compile(
    r"^COVERAGE HIR foreign_calls=(\d+) c_fields=(\d+)$")
NATIVE_COVERAGE_RE = re.compile(
    r"^COVERAGE NativeIR foreign_calls=(\d+) c_fields=(\d+) "
    r"c_ptr_writes=(\d+)$")
TOTAL_RE = re.compile(r"^TOTAL (\d+)passed (\d+)failed$")


def utc():
    return datetime.now(timezone.utc).isoformat()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    return sha256_bytes(Path(path).read_bytes())


def json_hash(value):
    return sha256_bytes(json.dumps(value, sort_keys=True,
                                   separators=(",", ":")).encode())


def tree_map(root, suffix=None):
    root = Path(root)
    result = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        if suffix is not None and path.suffix != suffix:
            continue
        result[str(path.relative_to(root))] = sha256_file(path)
    return result


def file_record(path):
    path = Path(path)
    return {"path": str(path), "size": path.stat().st_size,
            "sha256": sha256_file(path)}


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False,
                               allow_nan=False) + "\n")


def load_corpus_support():
    path = ROOT / "selfhost/corpus.py"
    spec = importlib.util.spec_from_file_location("woozy_corpus_support", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def finite_timeout(text):
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "timeout must be finite and positive") from error
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("timeout must be finite and positive")
    return value


def strict_bool(value):
    return type(value) is bool


def process_ok(record):
    return (record.get("status") == "exited"
            and type(record.get("exit")) is int and record["exit"] == 0
            and strict_bool(record.get("timed_out"))
            and record["timed_out"] is False
            and strict_bool(record.get("cleanup_complete"))
            and record["cleanup_complete"] is True
            and strict_bool(record.get("abandoned_pipes"))
            and record["abandoned_pipes"] is False
            and record.get("trace", {}).get("status") == "ok")


def check_empty_or_absent(path, label):
    path = Path(path)
    if path.exists():
        if not path.is_dir():
            raise RuntimeError(f"{label} exists and is not a directory: {path}")
        if any(path.iterdir()):
            raise RuntimeError(f"{label} is not empty; refusing to overwrite: {path}")
    else:
        path.mkdir(parents=True)


def exec_aliases(paths):
    result = set()
    for path in paths:
        value = Path(path)
        result.add(str(value))
        result.add(str(value.resolve()))
    return sorted(result)


def engine_resources(seed, toolchain, strace, stdlib):
    build = seed.parent
    llvm = toolchain / "llvm-22.1.8/bin"
    tools = {name: file_record(llvm / name) for name in TOOL_NAMES}
    stdlib_files = tree_map(stdlib, ".wz")
    resources = {
        "seed": file_record(seed),
        "helper": file_record(build / "native/lang_native_helper"),
        "native_runtime": file_record(build / "native/liblang_native_runtime.a"),
        "hosted_runtime": file_record(build / "native/liblang_hosted_runtime.a"),
        "strace": file_record(strace),
        "tools": tools,
        "stdlib": {"path": str(stdlib), "files": stdlib_files,
                   "map_sha256": json_hash(stdlib_files)},
    }
    expected = {
        "seed": EXPECTED_SEED,
        "helper": EXPECTED_HELPER,
        "native_runtime": EXPECTED_NATIVE_RUNTIME,
        "hosted_runtime": EXPECTED_HOSTED_RUNTIME,
    }
    for name, digest in expected.items():
        if resources[name]["sha256"] != digest:
            raise RuntimeError(f"fixed {name} SHA changed: "
                               f"{resources[name]['sha256']}")
    for name, digest in EXPECTED_TOOLS.items():
        if tools[name]["sha256"] != digest:
            raise RuntimeError(f"fixed LLVM tool {name} SHA changed: "
                               f"{tools[name]['sha256']}")
    if resources["stdlib"]["map_sha256"] != EXPECTED_STDLIB:
        raise RuntimeError("fixed stdlib map changed")
    if len(stdlib_files) != 37:
        raise RuntimeError(f"fixed stdlib must contain 37 .wz files: "
                           f"{len(stdlib_files)}")
    return resources


def strict_json_object(text, label, violations):
    try:
        value = json.loads(
            text,
            parse_constant=lambda token: (_ for _ in ()).throw(
                ValueError(f"non-finite JSON constant {token}")))
    except Exception as error:
        violations.append(f"{label}: not strict JSON: {error}")
        return None
    if not isinstance(value, dict):
        violations.append(f"{label}: JSON is not an object")
        return None
    return value


class MockTransport:
    """Pure in-memory transport used only by --self-test-controls."""

    def __init__(self, records):
        self.records = list(records)
        self.calls = 0

    def run(self, label, command, timeout, allowed, trace_required=True):
        self.calls += 1
        if not self.records:
            raise RuntimeError("mock transport exhausted")
        return dict(self.records.pop(0))


def parse_component_output(exit_code, stdout_text, stderr_text):
    """Validate the Wz component's real machine-readable case lines.

    The component's own TOTAL line is never trusted by itself: all eight
    unique CASE lines, both coverage lines, both baseline lines and the exact
    total are required, and any FAIL/unknown line is a violation.
    """
    violations = []
    cases = {}
    coverage = {}
    baselines = []
    totals = []
    unknown = []
    for line in stdout_text.splitlines():
        if not line.strip():
            continue
        match = CASE_RE.match(line)
        if match:
            name = match.group(1)
            if name in cases:
                violations.append(f"duplicate CASE line: {name}")
            cases[name] = "rejected"
            continue
        match = HIR_COVERAGE_RE.match(line)
        if match:
            if "hir" in coverage:
                violations.append("duplicate HIR coverage line")
                continue
            coverage["hir"] = {"foreign_calls": int(match.group(1)),
                               "c_fields": int(match.group(2))}
            continue
        match = NATIVE_COVERAGE_RE.match(line)
        if match:
            if "native" in coverage:
                violations.append("duplicate NativeIR coverage line")
                continue
            coverage["native"] = {
                "foreign_calls": int(match.group(1)),
                "c_fields": int(match.group(2)),
                "c_ptr_writes": int(match.group(3)),
            }
            continue
        if line in ("BASELINE HIR legal", "BASELINE NativeIR legal"):
            baselines.append(line)
            continue
        match = TOTAL_RE.match(line)
        if match:
            totals.append((int(match.group(1)), int(match.group(2))))
            continue
        if line.startswith("FAIL ") or line.startswith("FAIL:"):
            violations.append(f"component failure line: {line}")
            continue
        unknown.append(line)
    if unknown:
        violations.append(f"unknown stdout lines: {unknown!r}")
    if stderr_text.strip():
        violations.append(f"unexpected stderr: {stderr_text!r}")
    if type(exit_code) is not int or exit_code != 0:
        violations.append(f"component exit is not exact int 0: {exit_code!r}")
    if set(cases) != set(EXPECTED_CASES):
        missing = sorted(set(EXPECTED_CASES) - set(cases))
        extra = sorted(set(cases) - set(EXPECTED_CASES))
        violations.append(f"CASE set differs; missing={missing} extra={extra}")
    if len(cases) != len(EXPECTED_CASES):
        violations.append(f"CASE count is {len(cases)}, expected "
                          f"{len(EXPECTED_CASES)}")
    if len(baselines) != 2 or set(baselines) != {
            "BASELINE HIR legal", "BASELINE NativeIR legal"}:
        violations.append(f"baseline lines are not exactly the two legal "
                          f"lines: {baselines!r}")
    if len(totals) != 1:
        violations.append(f"TOTAL line count is {len(totals)}, expected 1")
    elif totals[0] != (len(EXPECTED_CASES), 0):
        violations.append(f"TOTAL is {totals[0]}, expected "
                          f"({len(EXPECTED_CASES)}, 0)")
    if "hir" not in coverage:
        violations.append("missing HIR coverage line")
    else:
        if coverage["hir"]["foreign_calls"] <= 0:
            violations.append("HIR foreign-call coverage is zero")
        if coverage["hir"]["c_fields"] <= 0:
            violations.append("HIR C-field coverage is zero")
    if "native" not in coverage:
        violations.append("missing NativeIR coverage line")
    else:
        for key in ("foreign_calls", "c_fields", "c_ptr_writes"):
            if coverage["native"][key] <= 0:
                violations.append(f"NativeIR {key} coverage is zero")
    return {
        "status": "passed" if not violations else "failed",
        "cases": [{"name": name, "status": cases[name]}
                  for name in EXPECTED_CASES if name in cases],
        "case_count": len(cases),
        "coverage": coverage,
        "baseline_lines": baselines,
        "totals": [{"passed": passed, "failed": failed}
                   for passed, failed in totals],
        "violations": violations,
    }


def evaluate_component_record(record, stdout_text, stderr_text):
    parsed = parse_component_output(record.get("exit"), stdout_text,
                                    stderr_text)
    violations = list(parsed["violations"])
    if not process_ok(record):
        violations.append("process record is not exact exited/0/clean")
    parsed["process_ok"] = process_ok(record)
    parsed["violations"] = violations
    parsed["status"] = "passed" if not violations else "failed"
    return parsed


def self_test_controls():
    good_cases = [f"CASE {name} rejected=E_IR_INVALID baseline=ok"
                  for name in EXPECTED_CASES]
    good_stdout = "\n".join([
        "COVERAGE HIR foreign_calls=3 c_fields=1",
        "BASELINE HIR legal",
        *good_cases[:4],
        "COVERAGE NativeIR foreign_calls=3 c_fields=1 c_ptr_writes=1",
        "BASELINE NativeIR legal",
        *good_cases[4:],
        "TOTAL 8passed 0failed",
    ]) + "\n"
    good_record = {"status": "exited", "exit": 0, "timed_out": False,
                   "cleanup_complete": True, "abandoned_pipes": False,
                   "trace": {"status": "ok"}}
    controls = []

    def add(name, expected, stdout_text, stderr_text="", record=None):
        actual = evaluate_component_record(record or dict(good_record),
                                           stdout_text, stderr_text)
        controls.append({"name": name, "expected": expected,
                         "actual": actual["status"],
                         "violations": actual["violations"],
                         "pass": actual["status"] == expected})

    transport = MockTransport([dict(good_record)])
    first = transport.run("good", [], 1.0, [], False)
    controls.append({"name": "mock transport returns isolated record",
                     "expected": "passed",
                     "actual": "passed" if first == good_record else "failed",
                     "violations": [], "pass": first == good_record})
    add("all 8 cases pass", "passed", good_stdout)

    zero = good_stdout.replace("TOTAL 8passed 0failed",
                               "TOTAL 0passed 8failed")
    zero = "\n".join(line for line in zero.splitlines()
                     if not line.startswith("CASE ")) + "\n"
    zero += "\n".join(f"FAIL {name} expected rejection"
                      for name in EXPECTED_CASES) + "\n"
    add("0/8 summary is rejected", "failed", zero)

    one = "\n".join(line for line in good_stdout.splitlines()
                    if not line.startswith("CASE native_c_field"))
    one = one.replace("TOTAL 8passed 0failed", "TOTAL 7passed 1failed")
    add("1/8 summary is rejected", "failed", one)

    add("duplicate TOTAL summary is rejected", "failed",
        good_stdout + "TOTAL 8passed 0failed\n")

    duplicate_case = good_stdout.replace(
        "CASE native_c_field rejected=E_IR_INVALID baseline=ok",
        "CASE native_c_callable rejected=E_IR_INVALID baseline=ok")
    add("duplicate CASE name is rejected", "failed", duplicate_case)

    missing_target = good_stdout.replace(
        "CASE native_c_field rejected=E_IR_INVALID baseline=ok",
        "FAIL native_c_field target_not_found")
    missing_target = missing_target.replace("TOTAL 8passed 0failed",
                                            "TOTAL 7passed 1failed")
    add("missing mutation target is rejected", "failed", missing_target)

    add("timeout record is rejected", "failed", good_stdout,
        record={**good_record, "status": "timeout", "timed_out": True})
    add("signal record is rejected", "failed", good_stdout,
        record={**good_record, "status": "signalled", "exit": -9})
    add("non-bool flag is rejected", "failed", good_stdout,
        record={**good_record, "timed_out": 0})
    add("non-empty stderr is rejected", "failed", good_stdout,
        stderr_text="warning\n")
    add("unknown stdout line is rejected", "failed",
        good_stdout + "extra\n")
    add("missing coverage is rejected", "failed",
        "\n".join(line for line in good_stdout.splitlines()
                  if not line.startswith("COVERAGE HIR")) + "\n")
    add("fake total without cases is rejected", "failed",
        "TOTAL 8passed 0failed\n")
    add("duplicate HIR coverage is rejected", "failed",
        good_stdout + "COVERAGE HIR foreign_calls=3 c_fields=1\n")
    add("duplicate NativeIR coverage is rejected", "failed",
        good_stdout + "COVERAGE NativeIR foreign_calls=3 c_fields=1 c_ptr_writes=1\n")
    add("zero HIR coverage cannot be replaced", "failed",
        "COVERAGE HIR foreign_calls=0 c_fields=0\n" + good_stdout)
    add("zero NativeIR coverage cannot be replaced", "failed",
        "COVERAGE NativeIR foreign_calls=0 c_fields=0 c_ptr_writes=0\n" + good_stdout)

    return {
        "status": "passed" if all(item["pass"] for item in controls)
                  else "failed",
        "controls": controls,
        "external_commands": 0,
        "transport": "MockTransport (in-memory records only)",
    }


def main(argv=None):
    sys.dont_write_bytecode = True
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=Path, default=DEFAULT_SEED)
    parser.add_argument("--toolchain-root", type=Path, default=DEFAULT_TOOLCHAIN)
    parser.add_argument("--strace", type=Path, default=DEFAULT_STRACE)
    parser.add_argument("--stdlib", type=Path, default=DEFAULT_STDLIB)
    parser.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE_ROOT)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--binding", type=Path, default=DEFAULT_BINDING)
    parser.add_argument("--cpp-test", type=Path, default=DEFAULT_CPP_TEST)
    parser.add_argument("--cpp-contract", type=Path, default=DEFAULT_CPP_CONTRACT)
    parser.add_argument("--cpp-runner", type=Path, default=DEFAULT_CPP_RUNNER)
    parser.add_argument("--driver-main", type=Path, default=HERE / "main.wz")
    parser.add_argument("--driver-manifest", type=Path, default=HERE / "lang.toml")
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--build-timeout", type=finite_timeout, default=1800.0)
    parser.add_argument("--measure-timeout", type=finite_timeout, default=300.0)
    parser.add_argument("--self-test-controls", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test_controls:
        result = self_test_controls()
        print(json.dumps(result, ensure_ascii=False))
        return 0 if result["status"] == "passed" else 1
    if args.work_dir is None or args.report is None:
        parser.error("--work-dir and --report are required unless "
                     "--self-test-controls is used")

    seed = args.seed.resolve(strict=True)
    toolchain = args.toolchain_root.resolve(strict=True)
    strace = args.strace.resolve(strict=True)
    stdlib = args.stdlib.resolve(strict=True)
    source_root = args.source_root.resolve(strict=True)
    manifest = args.manifest.resolve(strict=True)
    binding = args.binding.resolve(strict=True)
    cpp_test = args.cpp_test.resolve(strict=True)
    cpp_contract = args.cpp_contract.resolve(strict=True)
    cpp_runner = args.cpp_runner.resolve(strict=True)
    driver_main = args.driver_main.resolve(strict=True)
    driver_manifest = args.driver_manifest.resolve(strict=True)
    work = args.work_dir.resolve()
    report_path = args.report.resolve()
    if report_path.exists() and report_path.stat().st_size > 0:
        raise SystemExit(f"report already exists; refusing to overwrite: "
                         f"{report_path}")
    if work.exists():
        raise SystemExit(f"work directory already exists; refusing to "
                         f"overwrite: {work}")
    work.mkdir(parents=True, exist_ok=False)
    (work / "raw").mkdir()
    (work / "tmp").mkdir()
    corpus = load_corpus_support()
    env = dict(os.environ)
    libxml_dir = toolchain / "libxml2-legacy-2.13.9/usr/lib"
    env["LD_LIBRARY_PATH"] = ":".join([
        str(toolchain / "llvm-22.1.8/lib"),
        str(libxml_dir),
        env.get("LD_LIBRARY_PATH", ""),
    ]).rstrip(":")
    env["LC_ALL"] = "C"
    env["TMPDIR"] = str(work / "tmp")
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    llvm_bin = toolchain / "llvm-22.1.8/bin"
    tool_paths = {name: (llvm_bin / name).resolve(strict=True)
                  for name in TOOL_NAMES}
    helper = (seed.parent / "native/lang_native_helper").resolve(strict=True)
    build_timeout = args.build_timeout
    measure_timeout = args.measure_timeout
    failures = []

    def run_process(label, command, timeout, allowed, trace_required=True):
        stdout_path = work / "raw" / (label + ".stdout")
        stderr_path = work / "raw" / (label + ".stderr")
        trace_path = work / "raw" / (label + ".exec.log")
        argv = [str(x) for x in command]
        traced = argv
        if trace_required:
            traced = [str(strace), "--seccomp-bpf", "-f", "-qq", "-s", "16384",
                      "-e", "trace=execve,execveat", "-o", str(trace_path),
                      *argv]
        try:
            raw = corpus._run_process_impl(traced, ROOT, env, timeout)
            stdout_bytes = raw["stdout"]
            stderr_bytes = raw["stderr"]
            exit_code = raw["exit"]
            timed_out = raw["timed_out"]
            cleanup_complete = raw["cleanup_complete"]
            abandoned_pipes = raw["abandoned_pipes"]
            launch_error = None
        except OSError as error:
            stdout_bytes = b""
            stderr_bytes = b""
            exit_code = None
            timed_out = False
            cleanup_complete = True
            abandoned_pipes = False
            launch_error = str(error)
        stdout_path.write_bytes(stdout_bytes)
        stderr_path.write_bytes(stderr_bytes)
        status = ("launch_failed" if launch_error is not None else
                  "timeout" if timed_out else
                  "signalled" if type(exit_code) is int and exit_code < 0 else
                  "exited")
        record = {
            "argv": argv,
            "traced_argv": traced if trace_required else None,
            "cwd": str(ROOT),
            "timeout_seconds": timeout,
            "status": status,
            "exit": exit_code,
            "timed_out": timed_out,
            "cleanup_complete": cleanup_complete,
            "abandoned_pipes": abandoned_pipes,
            "stdout_path": str(stdout_path.relative_to(work)),
            "stderr_path": str(stderr_path.relative_to(work)),
            "stdout_bytes": len(stdout_bytes),
            "stderr_bytes": len(stderr_bytes),
            "stdout_sha256": sha256_bytes(stdout_bytes),
            "stderr_sha256": sha256_bytes(stderr_bytes),
            "stdout_file_sha256": sha256_file(stdout_path),
            "stderr_file_sha256": sha256_file(stderr_path),
            "trace_required": trace_required,
            "allowlist": allowed,
        }
        if trace_required:
            record["trace_path"] = str(trace_path.relative_to(work))
            try:
                record["trace"] = corpus.audit_trace(trace_path, allowed)
            except corpus.TraceError as error:
                record["trace"] = {"status": "violation", "error": str(error),
                                   "allowed": allowed}
        else:
            record["trace"] = {"status": "not_required"}
        if launch_error is not None:
            record["launch_error"] = launch_error
        if record["stdout_sha256"] != record["stdout_file_sha256"]:
            failures.append(f"{label}: stdout SHA changed on disk")
        if record["stderr_sha256"] != record["stderr_file_sha256"]:
            failures.append(f"{label}: stderr SHA changed on disk")
        if timed_out:
            failures.append(f"{label}: timed out after {timeout:g}s")
        if not cleanup_complete:
            failures.append(f"{label}: cleanup incomplete")
        if abandoned_pipes:
            failures.append(f"{label}: abandoned pipes")
        if record["trace"].get("status") != "ok":
            failures.append(f"{label}: exec trace violation")
        return record

    def freeze_inputs(component):
        return {
            "source116": {"path": str(source_root),
                          "files": tree_map(source_root, ".wz"),
                          "map_sha256": json_hash(tree_map(source_root, ".wz"))},
            "component": {"path": str(component),
                          "files": tree_map(component),
                          "map_sha256": json_hash(tree_map(component))},
            "engine": engine_resources(seed, toolchain, strace, stdlib),
            "forged_project": {
                "manifest": file_record(manifest),
                "source": {
                    "path": str(manifest.parent / "src"),
                    "files": tree_map(manifest.parent / "src", ".wz"),
                    "map_sha256": json_hash(
                        tree_map(manifest.parent / "src", ".wz")),
                },
                "binding": file_record(binding),
                "contract": file_record(cpp_contract),
            },
            "provenance": {
                "cpp_forged_test": file_record(cpp_test),
                "cpp_runner": file_record(cpp_runner),
                "cpp_contract": file_record(cpp_contract),
            },
            "runner": file_record(Path(__file__).resolve()),
            "corpus": file_record(ROOT / "selfhost/corpus.py"),
            "python": {
                "argv0": sys.executable,
                "resolved": str(Path(sys.executable).resolve()),
                "sha256": sha256_file(Path(sys.executable).resolve()),
                "version": sys.version.split()[0],
            },
            "libxml": {"path": str(libxml_dir),
                       "files": tree_map(libxml_dir)},
            "entry_main": file_record(driver_main),
            "entry_manifest": file_record(driver_manifest),
        }

    source_map = tree_map(source_root, ".wz")
    source_sha = json_hash(source_map)
    if source_sha != EXPECTED_SOURCE116:
        raise SystemExit(f"source116 map is not the fixed ed1c: {source_sha}")
    stdlib_map = tree_map(stdlib, ".wz")
    stdlib_sha = json_hash(stdlib_map)
    if stdlib_sha != EXPECTED_STDLIB:
        raise SystemExit(f"stdlib map is not fixed: {stdlib_sha}")
    wzcc_modules = len(list((source_root / "wzcc").rglob("*.wz")))

    component = work / "component"
    (component / "src").mkdir(parents=True)
    shutil.copytree(source_root / "wzcc", component / "src/wzcc")
    shutil.copy2(driver_main, component / "src/main.wz")
    shutil.copy2(driver_manifest, component / "lang.toml")
    component_map = tree_map(component)
    if len(component_map) != wzcc_modules + 2:
        raise SystemExit("component file count does not match staged modules")
    write_json(work / "source116-map.json", source_map)
    write_json(work / "stdlib-source-map.json", stdlib_map)
    write_json(work / "component-source-map.json", component_map)

    inputs_before = freeze_inputs(component)
    if inputs_before["source116"]["map_sha256"] != EXPECTED_SOURCE116:
        raise SystemExit("source116 changed while staging")
    build_dir = work / "driver-build"
    check_empty_or_absent(build_dir, "driver build output")
    build_argv = [str(seed), "build", "--project", str(component / "lang.toml"),
                  "--backend", "native", "--opt", "2", "--out-dir",
                  str(build_dir), "--format", "json"]
    build_allowed = exec_aliases([seed, helper, *tool_paths.values()])
    build_initial = {
        "operation_id": "SH15-cffi-forged-wz-component-20261114",
        "started_at_utc": utc(),
        "status": "running",
        "driver_build_argv": build_argv,
        "driver_build_allowlist": build_allowed,
        "driver_build_timeout_seconds": build_timeout,
        "source116_map_sha256": source_sha,
        "source116_files": len(source_map),
        "wzcc_modules": wzcc_modules,
        "entry_main": file_record(driver_main),
        "entry_manifest": file_record(driver_manifest),
        "component_files": len(component_map),
        "component_source_map_sha256": json_hash(component_map),
        "stdlib_map_sha256": stdlib_sha,
        "inputs_before": inputs_before,
        "environment": {"LD_LIBRARY_PATH": env["LD_LIBRARY_PATH"],
                        "LC_ALL": env["LC_ALL"], "TMPDIR": env["TMPDIR"],
                        "PYTHONDONTWRITEBYTECODE":
                            env["PYTHONDONTWRITEBYTECODE"]},
        "driver_build_process": None,
    }
    write_json(work / "build-report.json", build_initial)
    build_record = run_process("driver-build", build_argv, build_timeout,
                               build_allowed)
    inputs_after_build = freeze_inputs(component)
    driver = build_dir / "program"
    obj = build_dir / "program.o"
    build_violations = []
    build_stdout_text = (work / "raw/driver-build.stdout").read_text(
        encoding="utf-8", errors="replace")
    build_stderr_text = (work / "raw/driver-build.stderr").read_text(
        encoding="utf-8", errors="replace")
    build_json = strict_json_object(build_stdout_text, "driver-build stdout",
                                    build_violations)
    if build_json is None or build_json.get("status") != "ok":
        build_violations.append(
            f"driver build JSON status is not ok: {build_json!r}")
    if build_stderr_text.strip():
        build_violations.append(
            f"driver build stderr is not empty: {build_stderr_text!r}")
    if not process_ok(build_record):
        build_violations.append("driver build process is not exact "
                                "exited/0/clean")
    if driver.is_file() != obj.is_file():
        build_violations.append("driver program/object presence differs")
    build_status = ("passed" if not build_violations and driver.is_file()
                    and obj.is_file() and inputs_before == inputs_after_build
                    else "failed")
    build_report = dict(build_initial)
    build_report.update({
        "finished_at_utc": utc(),
        "status": build_status,
        "driver_build_process": build_record,
        "driver_build_stdout_json": build_json,
        "driver_build_stderr_empty": not build_stderr_text.strip(),
        "driver": file_record(driver) if driver.is_file() else None,
        "object": file_record(obj) if obj.is_file() else None,
        "inputs_after": inputs_after_build,
        "inputs_stable": inputs_before == inputs_after_build,
        "violations": build_violations,
    })
    write_json(work / "build-report.json", build_report)
    if build_status != "passed":
        report = {
            "operation_id": "SH15-cffi-forged-wz-component-20261114",
            "generated_at_utc": utc(),
            "status": "driver_build_failed",
            "source116_map_sha256": source_sha,
            "source116_files": len(source_map),
            "wzcc_modules": wzcc_modules,
            "component_files": len(component_map),
            "component_source_map_sha256": json_hash(component_map),
            "entry_main": file_record(driver_main),
            "entry_manifest": file_record(driver_manifest),
            "forged_project": inputs_before["forged_project"],
            "provenance": inputs_before["provenance"],
            "engine_resources": inputs_before["engine"],
            "driver_build": build_report,
            "failures": failures + build_violations + [
                "driver build failed; no retry was attempted"],
            "work_dir": str(work),
        }
        write_json(work / "runner-report.json", report)
        write_json(report_path, report)
        print(json.dumps({"status": report["status"],
                          "report": str(report_path),
                          "work_dir": str(work)}))
        return 1

    # The component is the only executable allowed during the measured Source
    # analysis.  It analyzes the forged project in-process and never builds,
    # links or runs the C target.
    run_argv = [str(driver), str(manifest), str(stdlib)]
    run_allowed = exec_aliases([driver])
    run_inputs_before = freeze_inputs(component)
    run_report_path = work / "run-report.json"
    run_initial = {
        "started_at_utc": utc(),
        "status": "running",
        "run_argv": run_argv,
        "run_allowlist": run_allowed,
        "run_timeout_seconds": measure_timeout,
        "driver": file_record(driver),
        "inputs_before": run_inputs_before,
    }
    write_json(run_report_path, run_initial)
    run_record = run_process("component-run", run_argv, measure_timeout,
                             run_allowed)
    run_inputs_after = freeze_inputs(component)
    run_stdout_text = (work / "raw/component-run.stdout").read_text(
        encoding="utf-8", errors="replace")
    run_stderr_text = (work / "raw/component-run.stderr").read_text(
        encoding="utf-8", errors="replace")
    parsed = evaluate_component_record(run_record, run_stdout_text,
                                       run_stderr_text)
    if run_inputs_before != run_inputs_after:
        parsed["violations"].append("input resources changed during the "
                                    "component run")
        parsed["status"] = "failed"
    if run_record["trace"].get("status") != "ok":
        parsed["violations"].append("component run exec trace violation")
        parsed["status"] = "failed"
    if run_record["timed_out"]:
        parsed["violations"].append("component run timed out")
        parsed["status"] = "failed"
    run_report = dict(run_initial)
    run_report.update({
        "finished_at_utc": utc(),
        "status": parsed["status"],
        "component_process": run_record,
        "parsed": parsed,
        "inputs_after": run_inputs_after,
        "inputs_stable": run_inputs_before == run_inputs_after,
    })
    write_json(run_report_path, run_report)

    status = "passed" if (build_status == "passed"
                          and parsed["status"] == "passed") else "failed"
    report = {
        "operation_id": "SH15-cffi-forged-wz-component-20261114",
        "generated_at_utc": utc(),
        "status": status,
        "scope": ("eight original C-FFI forged HIR/NativeIR rejection "
                  "assertions in the real Woozy compiler Wz modules"),
        "claims": {
            "wz_module_tests": True,
            "all_15_c_ffi_cases": False,
            "complete_selfhost": False,
            "program_interop": False,
            "forged_target_built_or_linked": False,
            "old_cpp_forged_test_rebuilt": False,
            "full_cli_built": False,
            "production_source_modified": False,
            "fixtures_bindings_gold_modified": False,
            "catalog_plan_state_modified": False,
        },
        "source116_map_sha256": source_sha,
        "source116_expected": EXPECTED_SOURCE116,
        "source116_files": len(source_map),
        "wzcc_modules": wzcc_modules,
        "component_files": len(component_map),
        "component_source_map_sha256": json_hash(component_map),
        "entry_main": file_record(driver_main),
        "entry_manifest": file_record(driver_manifest),
        "forged_project": inputs_before["forged_project"],
        "provenance": inputs_before["provenance"],
        "runner": inputs_before["runner"],
        "corpus": inputs_before["corpus"],
        "python": inputs_before["python"],
        "libxml": inputs_before["libxml"],
        "engine_resources": inputs_before["engine"],
        "inputs_before": inputs_before,
        "inputs_after": run_inputs_after,
        "inputs_stable": inputs_before == run_inputs_after,
        "driver_build": build_report,
        "component_run": run_report,
        "cases": parsed["cases"],
        "coverage": parsed["coverage"],
        "baseline_lines": parsed["baseline_lines"],
        "totals": parsed["totals"],
        "case_assertions_passed": sum(1 for case in parsed["cases"]
                                      if case["status"] == "rejected"),
        "case_assertions_failed": len(EXPECTED_CASES) - len(parsed["cases"]),
        "commands_separate": {
            "driver_build": {
                "argv": build_argv,
                "allowlist": build_allowed,
                "timeout_seconds": build_timeout,
                "note": "fixed C++ seed builds only this private component",
            },
            "component_run": {
                "argv": run_argv,
                "allowlist": run_allowed,
                "timeout_seconds": measure_timeout,
                "note": "exact exec allowlist is the component itself",
            },
        },
        "violations": parsed["violations"] + build_violations,
        "failures": failures,
        "limitations": [
            "Only the eight original C-FFI forged HIR/NativeIR assertions are claimed.",
            "This is not the full 15-case C-FFI runner, not a complete selfhost pass, and not program interoperability.",
            "The forged target program is only analyzed and verified; it is not built, linked or executed.",
            "The existing C++ forged test is provenance only and was not rebuilt or used as this run's result.",
            "The full product CLI is not built; the private component has its own entry and manifest.",
        ],
        "work_dir": str(work),
    }
    write_json(work / "runner-report.json", report)
    write_json(report_path, report)
    print(json.dumps({"status": status, "report": str(report_path),
                      "work_dir": str(work)}))
    return 0 if status == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
