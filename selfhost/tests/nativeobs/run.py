#!/usr/bin/env python3
"""Seal one source snapshot, build the Wz observation driver once, reuse it.

Correction behavior (SH12d single correction):

* stage ``component/src/wzcc`` from the fixed 116-file Root source plus this
  driver's ``main.wz`` and ``lang.toml`` (117 component files) exactly once;
* run the O0 build through ``selfhost/corpus.py:_run_process_impl`` and write
  an initial build report before spawning the process;
* store actual process fields, raw streams, before/after source/resource SHA
  maps, and driver/object SHA in that report;
* ``--skip-build`` only validates the sealed component, build report and
  driver/resources.  It never recopies source or rewrites the build report;
* optionally run the smoke and full matrix with the same sealed driver.

This is a test build orchestrator, not a product compiler entry point.
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
DEFAULT_TOOLCHAIN = ROOT.parent / "toolchains"
DEFAULT_SOURCE_ROOT = Path("/home/qin/Work/language-dev-20261103/checks/generic-arity-source-integration-20261114/clean-repo/selfhost/src")
DEFAULT_STDLIB = Path("/home/qin/.local/state/cli-agent-bridge/worktrees/0a0ca4cad6525fc6d10ec8f3/stdlib")
SOURCE116_MAP_SHA = "727b72dd473a5b2c6b2e9212e7d496131db9141bc4c9371ba60d25ddbf6eefef"
STDLIB_MAP_SHA = "473aa54b5ce5c1bbae4985d0d187528aa056a72ddc1737c3f05d3abb9623bd47"


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
    result = {}
    for path in sorted(Path(root).rglob("*")):
        if not path.is_file():
            continue
        if suffix is not None and path.suffix != suffix:
            continue
        result[str(path.relative_to(root))] = sha256_file(path)
    return result


def write_json(path, value):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=False) + "\n")


def archive(path, archive_root, label):
    if not Path(path).exists():
        return None
    target = Path(archive_root) / (label + "-" + utc().replace(":", "").replace("+", "_"))
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(path), str(target))
    return str(target)


def load_corpus_support(root):
    path = root / "selfhost/corpus.py"
    spec = importlib.util.spec_from_file_location("woozy_corpus_support", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def engine_resources(seed, toolchain, strace):
    tools = toolchain / "llvm-22.1.8/bin"
    names = ["clang", "ld.lld", "opt", "llc", "llvm-readelf", "llvm-nm"]
    return {
        "seed": {"path": str(seed), "sha256": sha256_file(seed),
                 "size": seed.stat().st_size},
        "helper": {"path": str(seed.parent / "native/lang_native_helper"),
                   "sha256": sha256_file(seed.parent / "native/lang_native_helper"),
                   "size": (seed.parent / "native/lang_native_helper").stat().st_size},
        "native_runtime": {"path": str(seed.parent / "native/liblang_native_runtime.a"),
                           "sha256": sha256_file(seed.parent / "native/liblang_native_runtime.a"),
                           "size": (seed.parent / "native/liblang_native_runtime.a").stat().st_size},
        "hosted_runtime": {"path": str(seed.parent / "native/liblang_hosted_runtime.a"),
                           "sha256": sha256_file(seed.parent / "native/liblang_hosted_runtime.a"),
                           "size": (seed.parent / "native/liblang_hosted_runtime.a").stat().st_size},
        "strace": {"path": str(strace), "sha256": sha256_file(strace),
                   "size": strace.stat().st_size},
        "tools": {name: {"path": str(tools / name),
                         "sha256": sha256_file(tools / name),
                         "size": (tools / name).stat().st_size}
                  for name in names},
    }


def resources(component, stdlib, seed, toolchain, strace):
    return {
        "component": tree_map(component),
        "component_source_map_sha256": json_hash(tree_map(component)),
        "stdlib": tree_map(stdlib, ".wz"),
        "stdlib_map_sha256": json_hash(tree_map(stdlib, ".wz")),
        "engine": engine_resources(seed, toolchain, strace),
        "driver_main_sha256": sha256_file(component / "src/main.wz"),
    }


def process_ok(process):
    return (process.get("status") == "exited"
            and type(process.get("exit")) is int and process["exit"] == 0
            and process.get("timed_out") is False
            and process.get("cleanup_complete") is True
            and process.get("abandoned_pipes") is False)


def validate_build(build_report_path, component, stdlib, seed, toolchain, strace,
                   source_root):
    failures = []
    report_sha_before = sha256_file(build_report_path)
    report = json.loads(build_report_path.read_text())
    if report.get("status") != "passed":
        failures.append("build report status is not passed")
    process = report.get("process") or {}
    if not process_ok(process):
        failures.append("build process did not exit 0 with complete cleanup")
    if report.get("source116_map_sha256") != SOURCE116_MAP_SHA:
        failures.append("source116 map SHA is not the fixed value")
    actual_source116 = tree_map(source_root, ".wz")
    if json_hash(actual_source116) != SOURCE116_MAP_SHA:
        failures.append("current source116 map changed")
    stored_component = json.loads((build_report_path.parent / "component-source-map.json").read_text())
    actual_component = tree_map(component)
    if actual_component != stored_component:
        failures.append("sealed component source map changed")
    if json_hash(actual_component) != report.get("component_source_map_sha256"):
        failures.append("component source map SHA changed")
    actual_stdlib = tree_map(stdlib, ".wz")
    if actual_stdlib != json.loads((build_report_path.parent / "stdlib-source-map.json").read_text()):
        failures.append("stdlib 37 map changed")
    if json_hash(actual_stdlib) != report.get("stdlib_map_sha256"):
        failures.append("stdlib map SHA changed")
    if engine_resources(seed, toolchain, strace) != report.get("engine_resources"):
        failures.append("engine resource SHA changed")
    driver = build_report_path.parent / "build/program"
    obj = build_report_path.parent / "build/program.o"
    if not driver.is_file() or sha256_file(driver) != report.get("driver_sha256"):
        failures.append("driver ELF SHA/size changed")
    if not obj.is_file() or sha256_file(obj) != report.get("object_sha256"):
        failures.append("driver object SHA/size changed")
    if sha256_file(build_report_path) != report_sha_before:
        failures.append("build report changed during validation")
    validation = {
        "status": "passed" if not failures else "failed",
        "checked_at_utc": utc(),
        "build_report": str(build_report_path),
        "build_report_sha256_before": report_sha_before,
        "build_report_sha256_after": sha256_file(build_report_path),
        "source116_map_sha256": json_hash(actual_source116),
        "component_source_map_sha256": json_hash(actual_component),
        "component_files": len(actual_component),
        "stdlib_map_sha256": json_hash(actual_stdlib),
        "stdlib_files": len(actual_stdlib),
        "driver_sha256": sha256_file(driver) if driver.is_file() else None,
        "object_sha256": sha256_file(obj) if obj.is_file() else None,
        "build_report_not_rewritten": sha256_file(build_report_path) == report_sha_before,
        "failures": failures,
    }
    write_json(build_report_path.parent / "reuse-validation.json", validation)
    return validation


def main(argv=None):
    sys.dont_write_bytecode = True
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=Path, required=True)
    parser.add_argument("--toolchain-root", type=Path, default=DEFAULT_TOOLCHAIN)
    parser.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE_ROOT)
    parser.add_argument("--driver-main", type=Path, default=HERE / "main.wz")
    parser.add_argument("--manifest", type=Path, default=HERE / "lang.toml")
    parser.add_argument("--stdlib", type=Path, default=DEFAULT_STDLIB)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--strace", type=Path,
                        default=DEFAULT_TOOLCHAIN / "strace-7.2/usr/bin/strace")
    parser.add_argument("--runner", type=Path, default=ROOT / "tests/native/run.py")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--restage", action="store_true",
                        help="archive the previous component/build and stage fresh; only for a necessary correction")
    parser.add_argument("--no-smoke", action="store_true")
    parser.add_argument("--full", action="store_true",
                        help="after smoke, run all 41 cases and 2 rejections with the same ELF")
    parser.add_argument("--build-timeout", type=float, default=1800.0)
    parser.add_argument("--smoke-timeout", type=float, default=1200.0)
    args = parser.parse_args(argv)
    if any(not math.isfinite(value) or value <= 0
           for value in (args.build_timeout, args.smoke_timeout)):
        parser.error("timeouts must be positive and finite")
    seed = args.seed.resolve()
    toolchain = args.toolchain_root.resolve()
    source_root = args.source_root.resolve()
    stdlib = args.stdlib.resolve()
    strace = args.strace.resolve()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    raw = work / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    archive_root = work / "archive"
    component = work / "component"
    build_out = work / "build"
    build_report_path = work / "build-report.json"
    corpus = load_corpus_support(ROOT)
    env = dict(os.environ)
    compat = str(toolchain / "libxml2-legacy-2.13.9/usr/lib")
    env["LD_LIBRARY_PATH"] = compat + (":" + env["LD_LIBRARY_PATH"]
                                       if env.get("LD_LIBRARY_PATH") else "")
    failures = []

    def run_process(label, command, timeout):
        stdout_path = raw / (label + ".stdout")
        stderr_path = raw / (label + ".stderr")
        stdout_path.write_bytes(b"")
        stderr_path.write_bytes(b"")
        try:
            result = corpus._run_process_impl([str(x) for x in command], ROOT,
                                              env, timeout)
        except OSError as error:
            return {"argv": [str(x) for x in command], "status": "launch_failed",
                    "exit": None, "timed_out": False, "cleanup_complete": True,
                    "abandoned_pipes": False, "stdout": "", "stderr": str(error),
                    "stdout_path": str(stdout_path.relative_to(work)),
                    "stderr_path": str(stderr_path.relative_to(work)),
                    "launch_error": str(error)}
        stdout_path.write_bytes(result["stdout"])
        stderr_path.write_bytes(result["stderr"])
        status = ("timeout" if result["timed_out"]
                  else "signalled" if result["exit"] < 0 else "exited")
        return {
            "argv": [str(x) for x in command],
            "status": status,
            "exit": result["exit"],
            "timed_out": result["timed_out"],
            "cleanup_complete": result["cleanup_complete"],
            "abandoned_pipes": result["abandoned_pipes"],
            "stdout": result["stdout"].decode("utf-8", "replace"),
            "stderr": result["stderr"].decode("utf-8", "replace"),
            "stdout_path": str(stdout_path.relative_to(work)),
            "stderr_path": str(stderr_path.relative_to(work)),
            "stdout_sha256": sha256_file(stdout_path),
            "stderr_sha256": sha256_file(stderr_path),
        }

    try:
        if not args.skip_build:
            if component.exists():
                if not args.restage:
                    raise RuntimeError("component already exists; use --skip-build or --restage")
                archive(component, archive_root, "component")
            if build_out.exists():
                if not args.restage:
                    raise RuntimeError("build directory already exists; use --skip-build or --restage")
                archive(build_out, archive_root, "build")
            source116 = tree_map(source_root, ".wz")
            source116_sha = json_hash(source116)
            if source116_sha != SOURCE116_MAP_SHA:
                raise RuntimeError("source116 map is not fixed: " + source116_sha)
            stdlib_map = tree_map(stdlib, ".wz")
            stdlib_sha = json_hash(stdlib_map)
            if stdlib_sha != STDLIB_MAP_SHA:
                raise RuntimeError("stdlib map is not fixed: " + stdlib_sha)
            (component / "src").mkdir(parents=True)
            shutil.copytree(source_root / "wzcc", component / "src/wzcc")
            shutil.copy2(args.driver_main.resolve(), component / "src/main.wz")
            shutil.copy2(args.manifest.resolve(), component / "lang.toml")
            component_map = tree_map(component)
            if len(component_map) != 117:
                raise RuntimeError("component must contain exactly 117 files: "
                                   + str(len(component_map)))
            write_json(work / "source116-map.json", source116)
            write_json(work / "stdlib-source-map.json", stdlib_map)
            write_json(work / "component-source-map.json", component_map)
            build_argv = [seed, "build", "--project", component / "lang.toml",
                          "--backend", "native", "--opt", "0",
                          "--out-dir", build_out, "--format", "json"]
            resources_before = resources(component, stdlib, seed, toolchain, strace)
            initial = {
                "status": "running",
                "started_at_utc": utc(),
                "argv": [str(x) for x in build_argv],
                "cwd": str(ROOT),
                "source116_map_sha256": source116_sha,
                "component_source_map_sha256": json_hash(component_map),
                "component_manifest_sha256": sha256_file(component / "lang.toml"),
                "component_files": len(component_map),
                "stdlib_map_sha256": stdlib_sha,
                "stdlib_files": len(stdlib_map),
                "engine_resources": engine_resources(seed, toolchain, strace),
                "resources_before": resources_before,
                "process": None,
                "raw_stdout": "raw/build.stdout",
                "raw_stderr": "raw/build.stderr",
            }
            write_json(build_report_path, initial)
            process = run_process("build", build_argv, args.build_timeout)
            resources_after = resources(component, stdlib, seed, toolchain, strace)
            driver = build_out / "program"
            obj = build_out / "program.o"
            report = dict(initial)
            report.update({
                "status": ("passed" if process_ok(process)
                           and driver.is_file() and obj.is_file()
                           and resources_before == resources_after else "failed"),
                "finished_at_utc": utc(),
                "process": process,
                "resources_after": resources_after,
                "resources_stable": resources_before == resources_after,
                "driver_sha256": sha256_file(driver) if driver.is_file() else None,
                "driver_size": driver.stat().st_size if driver.is_file() else None,
                "object_sha256": sha256_file(obj) if obj.is_file() else None,
                "object_size": obj.stat().st_size if obj.is_file() else None,
            })
            write_json(build_report_path, report)
            if report["status"] != "passed":
                failures.append("driver build failed; see " + str(build_report_path))
                raise RuntimeError("; ".join(failures))
        else:
            if not build_report_path.is_file():
                raise RuntimeError("--skip-build requires an existing build report")
            validation = validate_build(build_report_path, component, stdlib,
                                        seed, toolchain, strace, source_root)
            if validation["status"] != "passed":
                failures.extend(validation["failures"])
                raise RuntimeError("sealed build validation failed")
        build_report = json.loads(build_report_path.read_text())
        driver = work / "build/program"
        if not args.no_smoke:
            smoke_out = work / "smoke-out"
            smoke_report = work / "smoke-report.json"
            common = [sys.executable, str(args.runner.resolve()),
                      "--build", str(seed.parent),
                      "--toolchains", str(toolchain),
                      "--wz-native-helper", str(driver),
                      "--seed", str(seed),
                      "--reference-helper", str(seed.parent / "native/lang_native_helper"),
                      "--strace", str(strace),
                      "--build-report", str(build_report_path),
                      "--component-source-map", str(work / "component-source-map.json"),
                      "--stdlib-source", str(stdlib),
                      "--timeout", "120"]
            smoke_process = run_process(
                "smoke", common + ["--out", str(smoke_out),
                                   "--report", str(smoke_report), "--smoke"],
                args.smoke_timeout)
            write_json(work / "smoke-process.json", smoke_process)
            if not process_ok(smoke_process):
                failures.append("smoke matrix failed; see " + str(smoke_report))
                raise RuntimeError("; ".join(failures))
            if args.full:
                full_out = work / "full-out"
                full_report = work / "full-report.json"
                full_process = run_process(
                    "full", common + ["--out", str(full_out),
                                      "--report", str(full_report)],
                    args.smoke_timeout)
                write_json(work / "full-process.json", full_process)
                if not process_ok(full_process):
                    failures.append("full matrix failed; see " + str(full_report))
                    raise RuntimeError("; ".join(failures))
    except Exception as error:
        failures.append(str(error))
    orchestrator = {
        "status": "failed" if failures else "passed",
        "checked_at_utc": utc(),
        "build_report": str(build_report_path),
        "reuse_validation": str(work / "reuse-validation.json"),
        "smoke_process": str(work / "smoke-process.json"),
        "full_process": str(work / "full-process.json"),
        "driver": str(work / "build/program"),
        "driver_sha256": sha256_file(work / "build/program")
                          if (work / "build/program").is_file() else None,
        "failures": failures,
    }
    write_json(work / "orchestrator-report.json", orchestrator)
    print(json.dumps(orchestrator))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
