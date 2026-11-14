#!/usr/bin/env python3
"""Build the test-only Wz Nativeobs entry from the frozen ed1 Source.

The existing ``selfhost/tests/nativeobs/run.py`` builder pins the historical
116-file 727b Source and must not be treated as an ed1 builder.  This small
orchestrator reuses its staging/process/resource helpers, but pins the current
116-file ed1 map and the fixed 37-file stdlib.  It builds exactly one private
component (115 production Wz files + the unchanged nativeobs ``main.wz`` +
``lang.toml``) with the frozen C++ seed at O2 by default (``--opt 0`` is also
supported), then writes a sealed build
report before the candidate matrix is allowed to use it.

This is a test component builder, not a product compiler entry point.
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
import tempfile
import time
from datetime import datetime, timezone

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
ED1_SOURCE_MAP_SHA = "ed1c65fb9889606a713f03cc4e02d7bd6d818c58e4937610554cdd3f3e866a09"
STDLIB37_MAP_SHA = "473aa54b5ce5c1bbae4985d0d187528aa056a72ddc1737c3f05d3abb9623bd47"
EXPECTED_SOURCE_FILES = 116
EXPECTED_COMPONENT_FILES = 117
DEFAULT_TOOLCHAIN = ROOT.parent / "toolchains"
DEFAULT_STDLIB = ROOT / "stdlib"


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


def finite_json(value, where="$"):
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError(f"non-finite float at {where}")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            finite_json(item, f"{where}.{key}")
    elif isinstance(value, (list, tuple)):
        for index, item in enumerate(value):
            finite_json(item, f"{where}[{index}]")


def atomic_write_json(path, value):
    path = Path(path)
    finite_json(value)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=path.parent,
                                     prefix=path.name + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2, ensure_ascii=False,
                      allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def strict_json_loads(text):
    """Strict finite JSON, including exponent overflow such as ``1e1000``."""
    def reject(value):
        raise ValueError(f"non-finite JSON constant: {value}")

    def finite_float(value):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError(f"non-finite JSON number: {value}")
        return number

    return json.loads(text, parse_constant=reject, parse_float=finite_float)


def successful_build_json(text):
    document = strict_json_loads(text)
    if not isinstance(document, dict) or document.get("status") != "ok":
        raise ValueError("build stdout must be a JSON object with status ok")
    if document.get("diagnostics") != []:
        raise ValueError("successful build diagnostics must be empty")
    return document


def load_nativeobs_support():
    path = ROOT / "selfhost/tests/nativeobs/run.py"
    spec = importlib.util.spec_from_file_location("nativeobs_builder_support",
                                                  path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_corpus_support():
    path = ROOT / "selfhost/corpus.py"
    spec = importlib.util.spec_from_file_location("woozy_corpus_support", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def process_ok(process):
    return (process.get("status") == "exited"
            and type(process.get("exit")) is int and process["exit"] == 0
            and process.get("timed_out") is False
            and process.get("cleanup_complete") is True
            and process.get("abandoned_pipes") is False)


def archive(path, archive_root, label):
    if not Path(path).exists():
        return None
    target = Path(archive_root) / (label + "-" +
                                   utc().replace(":", "").replace("+", "_"))
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(path), str(target))
    return str(target)


def engine_resources(seed, toolchain, strace):
    tools = toolchain / "llvm-22.1.8/bin"
    names = ["clang", "clang++", "ld.lld", "opt", "llc", "llvm-readelf",
             "llvm-nm"]
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


def run_process(label, command, timeout, work, corpus, env):
    raw = work / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    stdout_path = raw / (label + ".stdout")
    stderr_path = raw / (label + ".stderr")
    stdout_path.write_bytes(b"")
    stderr_path.write_bytes(b"")
    started = time.monotonic()
    try:
        result = corpus._run_process_impl([str(x) for x in command], ROOT, env,
                                          timeout)
        exit_code = result["exit"]
        timed_out = result["timed_out"]
        cleanup_complete = result["cleanup_complete"]
        abandoned_pipes = result["abandoned_pipes"]
        stdout = result["stdout"]
        stderr = result["stderr"]
        launch_error = None
    except OSError as error:
        exit_code = None
        timed_out = False
        cleanup_complete = True
        abandoned_pipes = False
        stdout = b""
        stderr = b""
        launch_error = str(error)
    elapsed = round(time.monotonic() - started, 6)
    stdout_path.write_bytes(stdout)
    stderr_path.write_bytes(stderr)
    status = ("launch_failed" if launch_error is not None else
              "timeout" if timed_out else
              "signalled" if (isinstance(exit_code, int) and exit_code < 0) else
              "exited")
    record = {
        "argv": [str(x) for x in command],
        "status": status,
        "exit": exit_code,
        "timed_out": timed_out,
        "cleanup_complete": cleanup_complete,
        "abandoned_pipes": abandoned_pipes,
        "stdout": stdout.decode("utf-8", "replace"),
        "stderr": stderr.decode("utf-8", "replace"),
        "stdout_bytes": len(stdout),
        "stderr_bytes": len(stderr),
        "stdout_sha256": sha256_bytes(stdout),
        "stderr_sha256": sha256_bytes(stderr),
        "stdout_path": str(stdout_path.relative_to(work)),
        "stderr_path": str(stderr_path.relative_to(work)),
        "cwd": str(ROOT),
        "elapsed_s": elapsed,
        "timeout_s": timeout,
    }
    if launch_error is not None:
        record["launch_error"] = launch_error
    return record


def validate_build(report_path, component, stdlib, seed, toolchain, strace,
                   source_root):
    failures = []
    report_sha_before = sha256_file(report_path)
    report = strict_json_loads(report_path.read_text())
    if report.get("status") != "passed":
        failures.append("build report status is not passed")
    if "build_json_ok" in report and report.get("build_json_ok") is not True:
        failures.append("build stdout was not accepted as strict ok JSON")
    try:
        successful_build_json((report.get("process") or {})["stdout"])
    except (ValueError, TypeError, KeyError) as error:
        failures.append("invalid saved build stdout: " + str(error))
    if report.get("optimization") not in (0, 2):
        failures.append("build report optimization is missing/invalid")
    if not process_ok(report.get("process") or {}):
        failures.append("build process did not exit 0 with complete cleanup")
    if report.get("source116_map_sha256") != ED1_SOURCE_MAP_SHA:
        failures.append("source116 map SHA is not the fixed ed1 value")
    actual_source = tree_map(source_root, ".wz")
    if json_hash(actual_source) != ED1_SOURCE_MAP_SHA:
        failures.append("current source116 map is not ed1")
    stored_component = json.loads(
        (report_path.parent / "component-source-map.json").read_text())
    actual_component = tree_map(component)
    if actual_component != stored_component:
        failures.append("sealed component source map changed")
    if json_hash(actual_component) != report.get("component_source_map_sha256"):
        failures.append("component source map SHA changed")
    actual_stdlib = tree_map(stdlib, ".wz")
    if actual_stdlib != json.loads(
            (report_path.parent / "stdlib-source-map.json").read_text()):
        failures.append("stdlib 37 map changed")
    if json_hash(actual_stdlib) != STDLIB37_MAP_SHA:
        failures.append("stdlib map SHA is not the fixed 37-file value")
    if engine_resources(seed, toolchain, strace) != report.get("engine_resources"):
        failures.append("engine resource SHA changed")
    driver = report_path.parent / "build/program"
    obj = report_path.parent / "build/program.o"
    if not driver.is_file() or sha256_file(driver) != report.get("driver_sha256"):
        failures.append("driver ELF SHA/size changed")
    if not obj.is_file() or sha256_file(obj) != report.get("object_sha256"):
        failures.append("driver object SHA/size changed")
    if sha256_file(report_path) != report_sha_before:
        failures.append("build report changed during validation")
    validation = {
        "status": "passed" if not failures else "failed",
        "checked_at_utc": utc(),
        "build_report": str(report_path),
        "build_report_sha256_before": report_sha_before,
        "build_report_sha256_after": sha256_file(report_path),
        "source116_map_sha256": json_hash(actual_source),
        "component_source_map_sha256": json_hash(actual_component),
        "component_files": len(actual_component),
        "stdlib_map_sha256": json_hash(actual_stdlib),
        "stdlib_files": len(actual_stdlib),
        "driver_sha256": sha256_file(driver) if driver.is_file() else None,
        "object_sha256": sha256_file(obj) if obj.is_file() else None,
        "build_report_not_rewritten": sha256_file(report_path) == report_sha_before,
        "failures": failures,
    }
    atomic_write_json(report_path.parent / "reuse-validation.json", validation)
    return validation


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--stdlib", type=Path, default=DEFAULT_STDLIB)
    parser.add_argument("--driver-main", type=Path,
                        default=ROOT / "selfhost/tests/nativeobs/main.wz")
    parser.add_argument("--manifest", type=Path,
                        default=ROOT / "selfhost/tests/nativeobs/lang.toml")
    parser.add_argument("--toolchain-root", type=Path, default=DEFAULT_TOOLCHAIN)
    parser.add_argument("--strace", type=Path,
                        default=DEFAULT_TOOLCHAIN / "strace-7.2/usr/bin/strace")
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=1800.0)
    parser.add_argument("--opt", type=int, choices=(0, 2), default=2,
                        help="private component build optimization (default O2)")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--restage", action="store_true")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    seed = args.seed.resolve()
    source_root = args.source_root.resolve()
    stdlib = args.stdlib.resolve()
    toolchain = args.toolchain_root.resolve()
    strace = args.strace.resolve()
    work = args.work_dir.resolve()
    corpus = load_corpus_support()
    support = load_nativeobs_support()
    env = dict(os.environ)
    compat = str(toolchain / "libxml2-legacy-2.13.9/usr/lib")
    env["LD_LIBRARY_PATH"] = compat + (":" + env["LD_LIBRARY_PATH"]
                                       if env.get("LD_LIBRARY_PATH") else "")
    component = work / "component"
    build_out = work / "build"
    report_path = work / "build-report.json"
    archive_root = work / "archive"
    failures = []
    try:
        if not args.skip_build:
            if work.exists() and any(work.iterdir()):
                if not args.restage:
                    raise RuntimeError(
                        "work directory is not empty; use --skip-build or --restage")
                for name in ("component", "build", "raw", "build-report.json",
                             "reuse-validation.json"):
                    target = work / name
                    if target.exists():
                        archive(target, archive_root, name)
            work.mkdir(parents=True, exist_ok=True)
            source_map = tree_map(source_root, ".wz")
            source_sha = json_hash(source_map)
            if len(source_map) != EXPECTED_SOURCE_FILES or source_sha != ED1_SOURCE_MAP_SHA:
                raise RuntimeError(
                    f"source root is not the frozen ed1 source: {len(source_map)} "
                    f"files, map {source_sha}")
            stdlib_map = tree_map(stdlib, ".wz")
            stdlib_sha = json_hash(stdlib_map)
            if len(stdlib_map) != 37 or stdlib_sha != STDLIB37_MAP_SHA:
                raise RuntimeError(
                    f"stdlib is not the fixed 37-file source: {len(stdlib_map)} "
                    f"files, map {stdlib_sha}")
            if component.exists() or build_out.exists():
                raise RuntimeError("component/build already exists")
            (component / "src").mkdir(parents=True)
            shutil.copytree(source_root / "wzcc", component / "src/wzcc")
            shutil.copy2(args.driver_main.resolve(), component / "src/main.wz")
            shutil.copy2(args.manifest.resolve(), component / "lang.toml")
            component_map = tree_map(component)
            if len(component_map) != EXPECTED_COMPONENT_FILES:
                raise RuntimeError(
                    "component must contain exactly 117 files: "
                    + str(len(component_map)))
            atomic_write_json(work / "source116-map.json", source_map)
            atomic_write_json(work / "stdlib-source-map.json", stdlib_map)
            atomic_write_json(work / "component-source-map.json", component_map)
            build_argv = [seed, "build", "--project", component / "lang.toml",
                          "--backend", "native", "--opt", str(args.opt),
                          "--out-dir", build_out, "--format", "json"]
            resources_before = resources(component, stdlib, seed, toolchain,
                                         strace)
            initial = {
                "status": "running",
                "started_at_utc": utc(),
                "argv": [str(x) for x in build_argv],
                "cwd": str(ROOT),
                "source_root": str(source_root),
                "optimization": args.opt,
                "builder": str(Path(__file__).resolve()),
                "builder_sha256": sha256_file(Path(__file__).resolve()),
                "source116_map_sha256": source_sha,
                "component_source_map_sha256": json_hash(component_map),
                "component_manifest_sha256": sha256_file(component / "lang.toml"),
                "component_files": len(component_map),
                "stdlib_source": str(stdlib),
                "stdlib_map_sha256": stdlib_sha,
                "stdlib_files": len(stdlib_map),
                "driver_main": str(args.driver_main.resolve()),
                "driver_main_sha256": sha256_file(args.driver_main.resolve()),
                "manifest": str(args.manifest.resolve()),
                "manifest_sha256": sha256_file(args.manifest.resolve()),
                "engine_resources": engine_resources(seed, toolchain, strace),
                "resources_before": resources_before,
                "timeout_s": args.timeout,
                "process": None,
                "raw_stdout": "raw/build.stdout",
                "raw_stderr": "raw/build.stderr",
            }
            atomic_write_json(report_path, initial)
            process = run_process("build", build_argv, args.timeout, work,
                                  corpus, env)
            # Persist the known real process before resource hashing, artifact
            # reads, or strict JSON parsing can fail.  The process record keeps
            # raw bytes/SHA, argv, cwd, timeout, exact exit, and the three exact
            # cleanup booleans.
            report = dict(initial)
            report.update({
                "status": "process_completed",
                "process_saved_at_utc": utc(),
                "process": process,
            })
            atomic_write_json(report_path, report)
            resources_after = resources(component, stdlib, seed, toolchain,
                                        strace)
            driver = build_out / "program"
            obj = build_out / "program.o"
            build_json = None
            build_json_error = None
            try:
                build_json = successful_build_json(process["stdout"])
            except (ValueError, TypeError, KeyError) as error:
                build_json_error = str(error)
            resources_stable = resources_before == resources_after
            report.update({
                "status": ("passed" if process_ok(process)
                           and driver.is_file() and obj.is_file()
                           and resources_stable
                           and build_json_error is None else "failed"),
                "finished_at_utc": utc(),
                "process": process,
                "build_json": build_json,
                "build_json_ok": build_json_error is None,
                "resources_after": resources_after,
                "resources_stable": resources_stable,
                "driver": str(driver),
                "driver_sha256": sha256_file(driver) if driver.is_file() else None,
                "driver_size": driver.stat().st_size if driver.is_file() else None,
                "object": str(obj),
                "object_sha256": sha256_file(obj) if obj.is_file() else None,
                "object_size": obj.stat().st_size if obj.is_file() else None,
            })
            if build_json_error is not None:
                report["build_json_error"] = build_json_error
            atomic_write_json(report_path, report)
            if report["status"] != "passed":
                failures.append("driver build failed; see " + str(report_path))
                raise RuntimeError("; ".join(failures))
        else:
            if not report_path.is_file():
                raise RuntimeError("--skip-build requires an existing build report")
            validation = validate_build(report_path, component, stdlib, seed,
                                        toolchain, strace, source_root)
            if validation["status"] != "passed":
                failures.extend(validation["failures"])
                raise RuntimeError("sealed build validation failed")
    except Exception as error:
        failures.append(str(error))
        # Preserve a known process even when a later resource/artifact/JSON step
        # fails.  Do not rewrite an already passed sealed report.
        try:
            if report_path.is_file():
                saved = json.loads(report_path.read_text())
                if saved.get("status") in ("running", "process_completed"):
                    saved["status"] = "failed"
                    saved["finished_at_utc"] = utc()
                    saved["error"] = str(error)
                    atomic_write_json(report_path, saved)
        except Exception as update_error:
            failures.append(f"build-report failure update: {update_error}")
    report_path = work / "build-report.json"
    final = {
        "status": "failed" if failures else "passed",
        "checked_at_utc": utc(),
        "build_report": str(report_path),
        "reuse_validation": str(work / "reuse-validation.json"),
        "driver": str(work / "build/program"),
        "driver_sha256": (sha256_file(work / "build/program")
                          if (work / "build/program").is_file() else None),
        "source116_map_sha256": ED1_SOURCE_MAP_SHA,
        "stdlib_map_sha256": STDLIB37_MAP_SHA,
        "optimization": (json.loads(report_path.read_text()).get("optimization")
                         if report_path.is_file() else None),
        "builder_sha256": sha256_file(Path(__file__).resolve()),
        "failures": failures,
    }
    atomic_write_json(work / "builder-report.json", final)
    print(json.dumps(final))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
