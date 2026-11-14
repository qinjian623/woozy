#!/usr/bin/env python3
"""Private phase-timing evidence runner (test-only, not a product entry).

It seals the current selfhost/src/wzcc modules plus this directory's main.wz,
builds that private Wz driver once at O2 with the fixed C++ seed, lays out the
fixed stdlib/runtime/LLVM resources next to the driver, then invokes the driver
once on the unmodified tests/native/stdlib-io-errors/lang.toml with a fresh
60-second diagnostic budget.  The generated program is hashed, never run.

No compiler source, product CLI, catalog, or historical evidence is modified.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import sys
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_SOURCE_ROOT = ROOT / "selfhost/src"
DEFAULT_MANIFEST = ROOT / "tests/native/stdlib-io-errors/lang.toml"
DEFAULT_SEED = Path("/home/qin/.local/state/cli-agent-bridge/worktrees/"
                    "0a0ca4cad6525fc6d10ec8f3/.work/gc-registry-index/"
                    "build-new/woozyc")
DEFAULT_STDLIB = Path("/home/qin/.local/state/cli-agent-bridge/worktrees/"
                      "0a0ca4cad6525fc6d10ec8f3/stdlib")
DEFAULT_TOOLCHAIN = Path("/home/qin/Work/language-dev-20261103/toolchains")
DEFAULT_STRACE = DEFAULT_TOOLCHAIN / "strace-7.2/usr/bin/strace"
EXPECTED_SOURCE116 = "da69a017dfced63d3b8ef71016fc2188ba3834bc8c4f7236526cf4b780c41388"
EXPECTED_STDLIB = "473aa54b5ce5c1bbae4985d0d187528aa056a72ddc1737c3f05d3abb9623bd47"
EXPECTED_SEED = "565da3f6aa0b7f342ac5ba9523b5495643c1e44daaa5ecbb072e5906bd1a1fe4"
EXPECTED_HELPER = "3b2e489d88997d7c76ff9ae4827aa560c2547fb1832ebdf7f9427b42f376649e"
EXPECTED_NATIVE_RUNTIME = "4df15518f676291522878563e86bb71c99e53d3dad8872f620ec5515acc3a702"
EXPECTED_HOSTED_RUNTIME = "7f113be3fab32a62c263a1681ca2b3b34b76a63d6f74f679cce2ac7586a1f75b"
TOOL_NAMES = ("clang", "ld.lld", "opt", "llc", "llvm-readelf", "llvm-nm")
PHASES = (
    "project.load",
    "resolve.resolve_project",
    "sema.check_types",
    "hirverify.verify",
    "nativelower.lower",
    "nativeemit.emit",
    "nativebuild.build",
)


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


def process_ok(record):
    return (record.get("status") == "exited"
            and type(record.get("exit")) is int and record["exit"] == 0
            and record.get("timed_out") is False
            and record.get("cleanup_complete") is True
            and record.get("abandoned_pipes") is False)


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
    resources = {
        "seed": file_record(seed),
        "helper": file_record(build / "native/lang_native_helper"),
        "native_runtime": file_record(build / "native/liblang_native_runtime.a"),
        "hosted_runtime": file_record(build / "native/liblang_hosted_runtime.a"),
        "strace": file_record(strace),
        "tools": {name: file_record(llvm / name) for name in TOOL_NAMES},
        "stdlib": {"path": str(stdlib),
                   "files": tree_map(stdlib, ".wz"),
                   "map_sha256": json_hash(tree_map(stdlib, ".wz"))},
    }
    expected = {
        "seed": EXPECTED_SEED,
        "helper": EXPECTED_HELPER,
        "native_runtime": EXPECTED_NATIVE_RUNTIME,
        "hosted_runtime": EXPECTED_HOSTED_RUNTIME,
    }
    for name, digest in expected.items():
        if resources[name]["sha256"] != digest:
            raise RuntimeError(f"fixed {name} SHA changed: {resources[name]['sha256']}")
    if resources["stdlib"]["map_sha256"] != EXPECTED_STDLIB:
        raise RuntimeError("fixed stdlib map changed")
    return resources


def strict_json_object(text, label, violations):
    try:
        value = json.loads(text, parse_constant=lambda token: (_ for _ in ()).throw(
            ValueError(f"non-finite JSON constant {token}")))
    except Exception as error:
        violations.append(f"{label}: not strict JSON: {error}")
        return None
    if not isinstance(value, dict):
        violations.append(f"{label}: JSON is not an object")
        return None
    return value


def parse_phase_markers(stderr_text):
    markers = []
    violations = []
    by_index = {}
    last_index = 0
    last_begin = None
    last_stamp = None
    for line in stderr_text.splitlines():
        if not line.startswith("PHASE_TIMING "):
            continue
        payload = line[len("PHASE_TIMING "):]
        try:
            value = json.loads(payload, parse_constant=lambda token: (_ for _ in ()).throw(
                ValueError(f"non-finite JSON constant {token}")))
        except Exception as error:
            violations.append(f"phase marker is not strict JSON: {error}")
            continue
        if not isinstance(value, dict):
            violations.append("phase marker is not a JSON object")
            continue
        if type(value.get("schema")) is not int or value["schema"] != 1:
            violations.append("phase marker schema is not 1")
        phase = value.get("phase")
        index = value.get("index")
        event = value.get("event")
        stamp = value.get("ns")
        status = value.get("status")
        if phase not in PHASES:
            violations.append(f"unknown phase {phase!r}")
        valid_index = type(index) is int and 1 <= index <= len(PHASES)
        if not valid_index:
            violations.append(f"invalid phase index {index!r}")
        elif PHASES[index - 1] != phase:
            violations.append(f"phase/index mismatch: {phase!r} at {index}")
        if event not in ("begin", "end"):
            violations.append(f"invalid phase event {event!r}")
        if type(stamp) is not int or stamp < 0:
            violations.append(f"invalid phase ns {stamp!r}")
        if type(stamp) is int and stamp >= 0:
            if last_stamp is not None and stamp < last_stamp:
                violations.append("phase timestamp moved backwards")
            last_stamp = stamp
        if event == "begin":
            if status != "begin":
                violations.append("begin marker status is not begin")
            if valid_index:
                if last_begin is not None:
                    violations.append("phase began before the previous phase ended")
                if index != last_index + 1:
                    violations.append("phase begin is not the next sequential phase")
                if last_index and by_index[last_index].get("end_status") != "ok":
                    violations.append("phase began after an unsuccessful phase")
                if index in by_index:
                    violations.append(f"duplicate begin for phase index {index}")
                if index < last_index:
                    violations.append(f"phase index moved backwards to {index}")
                by_index[index] = {"phase": phase, "begin_ns": stamp,
                                   "ended": False}
                last_index = index
                last_begin = {"phase": phase, "index": index, "ns": stamp}
        else:
            if status not in ("ok", "failed"):
                violations.append(f"invalid end status {status!r}")
            begin_ns = value.get("begin_ns")
            elapsed_ns = value.get("elapsed_ns")
            if type(begin_ns) is not int or begin_ns < 0:
                violations.append("end marker lacks a valid begin_ns")
            if type(elapsed_ns) is not int or elapsed_ns < 0:
                violations.append("end marker lacks a valid elapsed_ns")
            if (type(begin_ns) is int and type(elapsed_ns) is int
                    and type(stamp) is int and stamp != begin_ns + elapsed_ns):
                violations.append("end ns is not begin_ns + elapsed_ns")
            if valid_index:
                record = by_index.get(index)
                if record is None:
                    violations.append(f"end without begin for phase index {index}")
                elif record["ended"]:
                    violations.append(f"duplicate end for phase index {index}")
                else:
                    if begin_ns != record["begin_ns"]:
                        violations.append("end begin_ns does not match its begin marker")
                    if last_begin is None or last_begin["index"] != index:
                        violations.append("phase end is not for the active phase")
                    record["ended"] = True
                    record["end_status"] = status
                    record["end_ns"] = stamp
                    record["elapsed_ns"] = elapsed_ns
                    if last_begin is not None and last_begin["index"] == index:
                        last_begin = None
        markers.append(value)
    completed = [PHASES[index - 1] for index in sorted(by_index)
                 if by_index[index].get("ended")
                 and by_index[index].get("end_status") == "ok"]
    return {
        "markers": markers,
        "marker_count": len(markers),
        "completed_phases": completed,
        "all_phases_completed": len(completed) == len(PHASES) and not violations,
        "last_open_begin": last_begin,
        "violations": violations,
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
    parser.add_argument("--driver-main", type=Path, default=HERE / "main.wz")
    parser.add_argument("--driver-manifest", type=Path, default=HERE / "lang.toml")
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--build-timeout", type=finite_timeout, default=1800.0)
    parser.add_argument("--measure-timeout", type=finite_timeout, default=60.0)
    args = parser.parse_args(argv)

    seed = args.seed.resolve(strict=True)
    toolchain = args.toolchain_root.resolve(strict=True)
    strace = args.strace.resolve(strict=True)
    stdlib = args.stdlib.resolve(strict=True)
    source_root = args.source_root.resolve(strict=True)
    manifest = args.manifest.resolve(strict=True)
    driver_main = args.driver_main.resolve(strict=True)
    driver_manifest = args.driver_manifest.resolve(strict=True)
    work = args.work_dir.resolve()
    report_path = args.report.resolve()
    if report_path.exists() and report_path.stat().st_size > 0:
        raise SystemExit(f"report already exists; refusing to overwrite: {report_path}")
    work.mkdir(parents=True, exist_ok=False)
    (work / "raw").mkdir()
    corpus = load_corpus_support()
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = ":".join([
        str(toolchain / "llvm-22.1.8/lib"),
        str(toolchain / "libxml2-legacy-2.13.9/usr/lib"),
        env.get("LD_LIBRARY_PATH", ""),
    ]).rstrip(":")
    env["LC_ALL"] = "C"
    env["TMPDIR"] = str(work / "tmp")
    (work / "tmp").mkdir()
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
                      "-e", "trace=execve,execveat", "-o", str(trace_path), *argv]
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
        if not record.get("stdout_sha256") == record.get("stdout_file_sha256"):
            failures.append(f"{label}: stdout SHA changed on disk")
        if not record.get("stderr_sha256") == record.get("stderr_file_sha256"):
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
            "measured_manifest": file_record(manifest),
            "measured_source": {
                "path": str(manifest.parent / "src"),
                "files": tree_map(manifest.parent / "src", ".wz"),
                "map_sha256": json_hash(tree_map(manifest.parent / "src", ".wz")),
            },
        }

    source_map = tree_map(source_root, ".wz")
    source_sha = json_hash(source_map)
    if source_sha != EXPECTED_SOURCE116:
        raise SystemExit(f"source116 map is not da69: {source_sha}")
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
                  "--backend", "native", "--opt", "2", "--out-dir", str(build_dir),
                  "--format", "json"]
    build_allowed = exec_aliases([seed, helper, *tool_paths.values()])
    build_initial = {
        "operation_id": "SH15-private-phase-timing-20261114",
        "started_at_utc": utc(),
        "status": "running",
        "driver_build_argv": build_argv,
        "driver_build_allowlist": build_allowed,
        "source116_map_sha256": source_sha,
        "source116_files": len(source_map),
        "wzcc_modules": wzcc_modules,
        "entry_main": file_record(driver_main),
        "component_files": len(component_map),
        "component_source_map_sha256": json_hash(component_map),
        "stdlib_map_sha256": stdlib_sha,
        "inputs_before": inputs_before,
        "environment": {"LD_LIBRARY_PATH": env["LD_LIBRARY_PATH"],
                        "LC_ALL": env["LC_ALL"], "TMPDIR": env["TMPDIR"]},
        "driver_build_process": None,
    }
    write_json(work / "build-report.json", build_initial)
    build_record = run_process("driver-build", build_argv, build_timeout,
                               build_allowed)
    inputs_after_build = freeze_inputs(component)
    driver = build_dir / "program"
    obj = build_dir / "program.o"
    build_report = dict(build_initial)
    build_report.update({
        "finished_at_utc": utc(),
        "status": "passed" if (process_ok(build_record) and driver.is_file()
                               and obj.is_file()
                               and inputs_before == inputs_after_build
                               and build_record["trace"].get("status") == "ok")
                  else "failed",
        "driver_build_process": build_record,
        "driver": file_record(driver) if driver.is_file() else None,
        "object": file_record(obj) if obj.is_file() else None,
        "inputs_after": inputs_after_build,
        "inputs_stable": inputs_before == inputs_after_build,
    })
    write_json(work / "build-report.json", build_report)
    if build_report["status"] != "passed":
        report = {
            "operation_id": "SH15-private-phase-timing-20261114",
            "generated_at_utc": utc(),
            "status": "driver_build_failed",
            "diagnostic_only": True,
            "source_measure_executed": False,
            "historical_budgets_unchanged": True,
            "performance_fix_claimed": False,
            "complete_cli_build_claimed": False,
            "source116_map_sha256": source_sha,
            "source116_files": len(source_map),
            "wzcc_modules": wzcc_modules,
            "component_files": len(component_map),
            "entry_main": file_record(driver_main),
            "driver_build": build_report,
            "failures": failures + ["driver build failed; no retry was attempted"],
            "work_dir": str(work),
        }
        write_json(work / "runner-report.json", report)
        write_json(report_path, report)
        print(json.dumps({"status": report["status"], "report": str(report_path)}))
        return 1

    # Fixed links select the frozen resources. The shared runtime directory
    # also contains the helper; the measurement exec allowlist rejects it.
    resource_links = {}
    for name, target in (
            ("stdlib", stdlib),
            ("runtime", (seed.parent / "native").resolve(strict=True)),
            ("toolchain", llvm_bin.resolve(strict=True))):
        link = build_dir / name
        if link.exists() or link.is_symlink():
            raise SystemExit(f"driver resource path already exists: {link}")
        link.symlink_to(target, target_is_directory=True)
        resource_links[name] = {
            "link": str(link), "target": str(target),
            "target_resolved": str(target.resolve(strict=True)),
        }
    driver_file = file_record(driver)
    measure_out = work / "measure/native-out"
    check_empty_or_absent(measure_out, "measured Source output")
    measure_argv = [str(driver), "--project", str(manifest),
                    "--stdlib", str(stdlib), "--out-dir", str(measure_out),
                    "--opt", "2", "--format", "json"]
    measure_allowed = exec_aliases(
        [driver] + [build_dir / "toolchain" / name for name in TOOL_NAMES])
    measure_allowed = sorted(set(measure_allowed)
                             | {str(p) for p in tool_paths.values()})
    measure_inputs_before = freeze_inputs(component)
    measure_report_path = work / "measure-report.json"
    measure_initial = {
        "started_at_utc": utc(),
        "status": "running",
        "measure_argv": measure_argv,
        "measure_allowlist": measure_allowed,
        "measure_timeout_seconds": measure_timeout,
        "driver": driver_file,
        "resource_links": resource_links,
        "inputs_before": measure_inputs_before,
    }
    write_json(measure_report_path, measure_initial)
    measure_record = run_process("source-measure", measure_argv,
                                 measure_timeout, measure_allowed)
    measure_inputs_after = freeze_inputs(component)
    stderr_text = (work / "raw/source-measure.stderr").read_text(
        encoding="utf-8", errors="replace")
    marker_report = parse_phase_markers(stderr_text)
    stdout_text = (work / "raw/source-measure.stdout").read_text(
        encoding="utf-8", errors="replace")
    stdout_json = None
    stdout_violations = []
    if stdout_text.strip():
        stdout_json = strict_json_object(stdout_text, "source-measure stdout",
                                         stdout_violations)
    else:
        stdout_violations.append("source-measure stdout is empty")
    marker_report["violations"].extend(stdout_violations)
    program = measure_out / "program"
    program_obj = measure_out / "program.o"
    artifacts = {
        "program": file_record(program) if program.is_file() else None,
        "program_o": file_record(program_obj) if program_obj.is_file() else None,
    }
    write_json(work / "measure/artifacts.json", artifacts)
    helper_observed = str(helper.resolve()) in {
        str(Path(item).resolve()) for item in measure_record["trace"].get("observed", [])
    }
    seed_observed = str(seed.resolve()) in {
        str(Path(item).resolve()) for item in measure_record["trace"].get("observed", [])
    }
    if helper_observed or seed_observed:
        failures.append("C++ seed/helper was executed during the measured Source run")
    if not marker_report["violations"] and not marker_report["all_phases_completed"]:
        failures.append("phase markers are incomplete")
    if process_ok(measure_record) and artifacts["program"] is None:
        failures.append("measured Source run exited 0 without a program artifact")
    measure_ok = (process_ok(measure_record)
                  and measure_inputs_before == measure_inputs_after
                  and measure_record["trace"].get("status") == "ok"
                  and not marker_report["violations"]
                  and marker_report["all_phases_completed"]
                  and artifacts["program"] is not None)
    if measure_record["timed_out"]:
        status = "source_measure_timed_out"
    elif measure_ok:
        status = "source_measure_completed"
    else:
        status = "source_measure_failed"
    measure_report = dict(measure_initial)
    measure_report.update({
        "finished_at_utc": utc(),
        "status": status,
        "measure_process": measure_record,
        "inputs_after": measure_inputs_after,
        "inputs_stable": measure_inputs_before == measure_inputs_after,
        "phase_markers": marker_report,
        "stdout_json": stdout_json,
        "artifacts": artifacts,
        "program_not_executed": True,
        "cpp_helper_or_seed_observed": helper_observed or seed_observed,
    })
    write_json(measure_report_path, measure_report)

    report = {
        "operation_id": "SH15-private-phase-timing-20261114",
        "generated_at_utc": utc(),
        "status": status,
        "diagnostic_only": True,
        "source_measure_executed": True,
        "original_20s_budget_status": "unchanged_failed",
        "historical_budgets": {"original_20s": "failed_record_retained",
                               "original_1200s": "failed_record_retained",
                               "original_21600s": "failed_record_retained",
                               "this_diagnostic_60s": measure_timeout},
        "historical_failures_preserved": True,
        "performance_fix_claimed": False,
        "complete_cli_build_claimed": False,
        "source116_map_sha256": source_sha,
        "source116_files": len(source_map),
        "source116_expected": EXPECTED_SOURCE116,
        "wzcc_modules": wzcc_modules,
        "component_files": len(component_map),
        "component_source_map_sha256": json_hash(component_map),
        "entry_main": file_record(driver_main),
        "entry_manifest": file_record(driver_manifest),
        "measured_manifest": file_record(manifest),
        "measured_source": {
            "files": measure_inputs_before["measured_source"]["files"],
            "map_sha256": measure_inputs_before["measured_source"]["map_sha256"],
        },
        "production_source_unchanged": (
            measure_inputs_after["source116"]["map_sha256"] == EXPECTED_SOURCE116),
        "stdlib_map_sha256": stdlib_sha,
        "engine_resources": measure_inputs_before["engine"],
        "driver_build": build_report,
        "driver": driver_file,
        "resource_links": resource_links,
        "source_measure": measure_report,
        "phase_markers": marker_report["markers"],
        "phase_summary": {
            "completed_phases": marker_report["completed_phases"],
            "all_phases_completed": marker_report["all_phases_completed"],
            "last_open_begin": marker_report["last_open_begin"],
            "violations": marker_report["violations"],
        },
        "stdout_json": stdout_json,
        "artifacts": artifacts,
        "program_not_executed": True,
        "cpp_helper_or_seed_observed_during_measure": (
            helper_observed or seed_observed),
        "commands_separate": {
            "driver_build": {
                "argv": build_argv,
                "allowlist": build_allowed,
                "help_note": "C++ seed build stage may use the matching helper",
            },
            "source_measure": {
                "argv": measure_argv,
                "allowlist": measure_allowed,
                "help_note": "measured Wz Source stage may use only the Wz driver and fixed LLVM",
            },
        },
        "work_dir": str(work),
        "failures": failures,
        "limitations": [
            "This is a one-off diagnostic run, not the original 20-second test.",
            "It does not build the full product CLI and does not claim a complete selfhost pass.",
            "It does not claim any performance fix; all historical budgets and failures are retained.",
            "If the 60-second run timed out, only the last observed begin phase is known; unentered phases are not inferred.",
            "The generated io-errors program is hashed but not executed.",
        ],
    }
    if status != "source_measure_completed":
        report["phase_summary"]["last_open_begin"] = marker_report["last_open_begin"]
    write_json(work / "runner-report.json", report)
    write_json(report_path, report)
    print(json.dumps({"status": status, "report": str(report_path),
                      "work_dir": str(work)}))
    return 0 if status == "source_measure_completed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
