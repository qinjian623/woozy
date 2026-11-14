#!/usr/bin/env python3
"""Compile native scalar projects, link with Clang/LLD, execute ELF directly.

The default mode is the frozen C++ reference helper pipeline:
``lang_native_helper --out-object/--out-llvm/--out-llvm-before/--out-native``.
Pass ``--wz-native-helper`` (alias ``--driver``) to use the pure-Wz
observation driver in ``selfhost/tests/nativeobs`` instead.  In that mode the
driver performs Source -> verified CFG -> pre-optimization LLVM text; this
runner performs the fixed ``opt``/``llc`` steps, independent link, ELF checks,
and execution.  The candidate driver never invokes the C++ frontend/helper.
Python only drives subprocesses and records evidence; it never evaluates the
source language or generated LLVM.  No shared fixture is rewritten.
"""
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
import tempfile
from datetime import datetime, timezone

PANIC = re.compile(r"LANG_NATIVE_PANIC:v1:(division-by-zero|invalid-shift|integer-conversion):[0-9]+:[0-9]+:[0-9]+\n")
SMOKE_CASES = {"unit-main", "nonzero-main"}
STREAM_NAME = re.compile(r"[^A-Za-z0-9_.-]+")


def panic_record(result):
    return (result.get("status") == "exited" and result["exit"] != 0
            and PANIC.fullmatch(result["stderr"]) is not None
            and result["stdout"] == "")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    return sha256_bytes(Path(path).read_bytes())


def json_hash(value):
    return sha256_bytes(json.dumps(value, sort_keys=True,
                                   separators=(",", ":")).encode())


def load_corpus_support(root):
    path = root / "selfhost/corpus.py"
    spec = importlib.util.spec_from_file_location("woozy_corpus_support", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    sys.dont_write_bytecode = True
    p = argparse.ArgumentParser()
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--toolchains", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--report", type=Path, required=True)
    p.add_argument("--wz-native-helper", "--driver", dest="driver", type=Path,
                   help="pure-Wz observation driver; default keeps the C++ reference helper")
    p.add_argument("--frontend", type=Path,
                   help="frontend binary for the no-LLVM dependency check "
                        "(default: driver in Wz mode, BUILD/frontend/woozyc otherwise)")
    p.add_argument("--seed", type=Path, help="C++ seed compiler used to build the driver (recorded)")
    p.add_argument("--reference-helper", type=Path,
                   help="C++ helper kept only as a recorded reference in Wz mode")
    p.add_argument("--smoke", action="store_true",
                   help="run only unit-main/nonzero-main O0/O2 and the two rejections")
    p.add_argument("--strace", type=Path,
                   help="strace 7.2 for the candidate exec whitelist (default: toolchains/strace-7.2)")
    p.add_argument("--build-report", type=Path,
                   help="sealed driver build report; candidate runs validate it")
    p.add_argument("--component-source-map", type=Path,
                   help="sealed 117-file driver component source map")
    p.add_argument("--stdlib-source", type=Path,
                   help="fixed stdlib source root (default: ROOT/stdlib)")
    p.add_argument("--timeout", type=float, default=30.0)
    p.add_argument("--execute-timeout", type=float, default=5.0)
    a = p.parse_args()
    if any(not math.isfinite(value) or value <= 0
           for value in (a.timeout, a.execute_timeout)):
        p.error("timeouts must be positive and finite")
    root = Path(__file__).resolve().parents[2]
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    out_streams = out / "_streams"
    out_streams.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(a.toolchains / "libxml2-legacy-2.13.9/usr/lib") + ":" + env.get("LD_LIBRARY_PATH", "")
    llvm = a.toolchains / "llvm-22.1.8/bin"
    strace = (a.strace.resolve() if a.strace is not None
              else a.toolchains.resolve() / "strace-7.2/usr/bin/strace")
    stdlib_source = (a.stdlib_source.resolve() if a.stdlib_source is not None
                     else root / "stdlib")
    build = a.build.resolve()
    candidate = a.driver is not None
    driver = a.driver.resolve() if candidate else None
    helper = build / "native/lang_native_helper"
    runtime = build / "native/liblang_native_runtime.a"
    hosted_runtime = build / "native/liblang_hosted_runtime.a"
    if a.frontend is not None:
        frontend = a.frontend.resolve()
    elif candidate:
        frontend = driver
    else:
        frontend = build / "frontend/woozyc"
    reference_helper = (a.reference_helper.resolve() if a.reference_helper is not None
                        else (helper if candidate and helper.is_file() else None))
    corpus = load_corpus_support(root)
    failed = []
    report = {"task": "T05", "contract": "compiler-dev/1",
              "checked_at": datetime.now(timezone.utc).isoformat(),
              "scope": "native scalar observation, fixed LLVM opt/llc, Clang/LLD link, and ELF execution",
              "mode": "wz_native_observation_driver" if candidate else "cpp_reference_helper",
              "smoke": bool(a.smoke),
              "root": str(root), "build": str(build), "toolchains": str(a.toolchains.resolve()),
              "driver": str(driver) if candidate else None,
              "frontend": str(frontend),
              "reference_helper": str(reference_helper) if reference_helper else None,
              "strace": str(strace),
              "stdlib_source": str(stdlib_source),
              "build_report": str(a.build_report.resolve()) if a.build_report else None,
              "preflight": [], "runs": [], "rejections": [],
              "not_run": [], "expected_rows": 0, "attempted_rows": 0}
    expected_rows = []
    attempted_rows = set()
    command_index = 0

    def save():
        expected = {(name, opt) for name, opt in expected_rows}
        report["not_run"] = [f"{name}:O{opt}" for name, opt in expected_rows
                             if (name, opt) not in attempted_rows]
        report["attempted_rows"] = len(attempted_rows)
        report["expected_rows"] = len(expected_rows)
        if "summary" in report:
            report["summary"]["not_run"] = len(report["not_run"])
        report["failed"] = failed
        report["status"] = "failed" if failed else "passed"
        a.report.parent.mkdir(parents=True, exist_ok=True)
        a.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")

    def command(argv, timeout=30, label=None, trace=None, save_streams=True):
        nonlocal command_index
        command_index += 1
        args = [str(x) for x in argv]
        if label is None:
            label = Path(args[0]).name
        stem = STREAM_NAME.sub("_", label)[:80]
        prefix = f"{command_index:04d}_{stem}"
        stdout_path = out_streams / (prefix + ".stdout")
        stderr_path = out_streams / (prefix + ".stderr")
        trace_path = None
        traced_args = None
        if trace is not None:
            trace_dir = out / "_traces"
            trace_dir.mkdir(parents=True, exist_ok=True)
            trace_path = trace_dir / (prefix + ".exec.log")
            traced_args = [str(strace), "--seccomp-bpf", "-f", "-qq", "-s", "16384",
                           "-e", "trace=execve,execveat", "-o", str(trace_path), *args]
            run_args = traced_args
        else:
            run_args = args
        try:
            raw = corpus._run_process_impl(run_args, root, env, timeout)
            stdout_bytes = raw["stdout"]
            stderr_bytes = raw["stderr"]
            timed_out = raw["timed_out"]
            cleanup_complete = raw["cleanup_complete"]
            abandoned_pipes = raw["abandoned_pipes"]
            exit_code = raw["exit"]
            status = ("timeout" if timed_out else
                      "signalled" if exit_code < 0 else "exited")
            launch_error = None
        except OSError as error:
            stdout_bytes = b""
            stderr_bytes = b""
            timed_out = False
            cleanup_complete = True
            abandoned_pipes = False
            exit_code = None
            status = "launch_failed"
            launch_error = str(error)
            failed.append(f"{label}: launch failed: {error}")
        stdout = stdout_bytes.decode("utf-8", "replace")
        stderr = stderr_bytes.decode("utf-8", "replace")
        record = {
            "argv": args,
            "status": status,
            "exit": exit_code,
            "timed_out": timed_out,
            "cleanup_complete": cleanup_complete,
            "abandoned_pipes": abandoned_pipes,
            "stdout": stdout,
            "stderr": stderr,
            "stdout_bytes": len(stdout_bytes),
            "stderr_bytes": len(stderr_bytes),
            "stdout_sha256": sha256_bytes(stdout_bytes),
            "stderr_sha256": sha256_bytes(stderr_bytes),
            "cwd": str(root),
            "trace_required": trace is not None,
        }
        if traced_args is not None:
            record["traced_argv"] = traced_args
            record["trace_path"] = str(trace_path.relative_to(out))
            record["trace_expected"] = [str(x) for x in trace]
            try:
                record["trace"] = corpus.audit_trace(
                    trace_path, [str(x) for x in trace])
            except corpus.TraceError as error:
                record["trace"] = {"status": "violation",
                                   "error": str(error),
                                   "allowed": [str(x) for x in trace]}
                failed.append(f"{label}: exec trace violation: {error}")
        else:
            record["trace"] = {"status": "not_required"}
        if launch_error is not None:
            record["launch_error"] = launch_error
        if timed_out:
            failed.append(f"{label}: timed out after {timeout:g}s")
        if not cleanup_complete:
            failed.append(f"{label}: cleanup incomplete")
        if abandoned_pipes:
            failed.append(f"{label}: abandoned pipes")
        if save_streams:
            stdout_path.write_bytes(stdout_bytes)
            stderr_path.write_bytes(stderr_bytes)
            record["stdout_path"] = str(stdout_path.relative_to(out))
            record["stderr_path"] = str(stderr_path.relative_to(out))
            record["stdout_file_sha256"] = sha256_file(stdout_path)
            record["stderr_file_sha256"] = sha256_file(stderr_path)
            if record["stdout_file_sha256"] != record["stdout_sha256"]:
                failed.append(f"{label}: stdout SHA changed on disk")
            if record["stderr_file_sha256"] != record["stderr_sha256"]:
                failed.append(f"{label}: stderr SHA changed on disk")
        return record

    def exec_alias(path):
        value = Path(path)
        return sorted({str(value), str(value.resolve())})

    def artifact_record(path):
        data = Path(path).read_bytes()
        return {"path": str(path), "size": len(data),
                "sha256": sha256_bytes(data),
                "recorded_at_utc": datetime.now(timezone.utc).isoformat()}

    def persist_row(row, directory):
        write_row = directory / "row.json"
        write_row.write_text(json.dumps(row, indent=2, ensure_ascii=False) + "\n")
        progress = out / "progress.json"
        progress.write_text(json.dumps({
            "attempted_rows": len(attempted_rows),
            "expected_rows": len(expected_rows),
            "last": row.get("id", "") + ":O" + str(row.get("optimization")),
            "failed": failed,
            "status": "failed" if failed else "running",
        }, indent=2, ensure_ascii=False) + "\n")

    def ok(r):
        return r["status"] == "exited" and r["exit"] == 0

    def resource_hashes():
        values = {}
        for name, path in [("driver", driver), ("runtime", runtime),
                           ("hosted_runtime", hosted_runtime),
                           ("frontend", frontend), ("seed", a.seed),
                           ("reference_helper", reference_helper)]:
            if path is not None and path.is_file():
                values[name] = sha256_file(path)
        for name in ("clang", "ld.lld", "opt", "llc", "llvm-readelf", "llvm-nm"):
            path = llvm / name
            if path.is_file():
                values["tool_" + name] = sha256_file(path)
        if a.build_report is not None and a.build_report.is_file():
            values["build_report"] = sha256_file(a.build_report)
        if a.component_source_map is not None and a.component_source_map.is_file():
            values["component_source_map"] = sha256_file(a.component_source_map)
        return values

    try:
        report["base_head"] = command(["git", "-C", root, "rev-parse", "HEAD"],
                                      label="git-head")["stdout"].strip()
        selected = ["include/lang/ir/*.hpp", "include/lang/backend/native.hpp",
                    "src/ir/*.cpp", "src/backend/native/*", "runtime/native/*.cpp",
                    "tests/native/*_test.cpp", "tests/native/run.py"]
        report["implementation_sha256"] = {
            str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest()
            for pattern in selected
            for f in sorted(root.glob(pattern)) if f.is_file()}
        report["corpus_support_sha256"] = sha256_file(root / "selfhost/corpus.py")
        report["driver_source_sha256"] = {
            str(f.relative_to(root)): sha256_file(f)
            for f in sorted((root / "selfhost/tests/nativeobs").glob("*"))
            if f.is_file()}
        if candidate:
            if not driver.is_file():
                raise RuntimeError(f"Wz observation driver missing: {driver}")
            report["driver_sha256"] = sha256_file(driver)
            report["helper_sha256"] = report["driver_sha256"]
            if reference_helper is not None and reference_helper.is_file():
                report["reference_helper_sha256"] = sha256_file(reference_helper)
        else:
            report["helper_sha256"] = sha256_file(helper)
        report["runtime_sha256"] = sha256_file(runtime)
        if hosted_runtime.is_file():
            report["hosted_runtime_sha256"] = sha256_file(hosted_runtime)
        if a.seed is not None and a.seed.is_file():
            report["seed_sha256"] = sha256_file(a.seed)
        report["tools"] = {name: command([llvm / name, "--version"], label="version-" + name)
                           for name in ["clang", "ld.lld", "opt", "llc",
                                        "llvm-readelf", "llvm-nm"]}
        for name, r in report["tools"].items():
            if not ok(r) or "22.1.8" not in r["stdout"]:
                raise RuntimeError("wrong/missing tool version: " + name)
        report["tool_sha256"] = {name: sha256_file(llvm / name)
                                 for name in ["clang", "ld.lld", "opt", "llc",
                                              "llvm-readelf", "llvm-nm"]}
        report["python"] = command([sys.executable, "--version"], label="version-python")
        report["python_sha256"] = sha256_file(Path(sys.executable).resolve())
        if a.build_report is not None:
            build_report_path = a.build_report.resolve()
            if not build_report_path.is_file():
                raise RuntimeError("sealed build report missing: " + str(build_report_path))
            build_report_sha = sha256_file(build_report_path)
            sealed = json.loads(build_report_path.read_text())
            if sealed.get("status") != "passed":
                raise RuntimeError("sealed build report is not passed")
            if sealed.get("driver_sha256") != sha256_file(driver):
                raise RuntimeError("driver SHA does not match sealed build report")
            report["build_report_sha256"] = build_report_sha
            report["sealed_build"] = {
                "path": str(build_report_path),
                "sha256": build_report_sha,
                "status": sealed.get("status"),
                "driver_sha256": sealed.get("driver_sha256"),
                "driver_size": sealed.get("driver_size"),
                "object_sha256": sealed.get("object_sha256"),
                "source116_map_sha256": sealed.get("source116_map_sha256"),
                "component_source_map_sha256": sealed.get("component_source_map_sha256"),
                "stdlib_map_sha256": sealed.get("stdlib_map_sha256"),
                "component_files": sealed.get("component_files"),
                "stdlib_files": sealed.get("stdlib_files"),
            }
            if a.component_source_map is not None:
                component_map_path = a.component_source_map.resolve()
                if not component_map_path.is_file():
                    raise RuntimeError("component source map missing: " + str(component_map_path))
                component_map = json.loads(component_map_path.read_text())
                component_map_sha = json_hash(component_map)
                if component_map_sha != sealed.get("component_source_map_sha256"):
                    raise RuntimeError("component source map SHA does not match build report")
                report["component_source_map"] = {
                    "path": str(component_map_path),
                    "file_sha256": sha256_file(component_map_path),
                    "map_sha256": component_map_sha,
                    "files": len(component_map),
                }
        report["resource_sha256_before"] = resource_hashes()
        common = json.loads((root / "tests/backend_common/cases.json").read_text())["cases"]
        cases = [(c["id"], root / c["manifest"], c["expected"]["status"],
                  c["expected"].get("main_return", 0)) for c in common]
        cases += [(x, root / "tests/fixtures" / x / "lang.toml", "return", 0)
                  for x in ["core", "numeric"]]
        cases += [("mutual-recursion",
                   root / "tests/modules/fixtures/same-package-mutual-recursion/lang.toml",
                   "return", 0)]
        cases += [("cross-package-call",
                   root / "tests/modules/fixtures/cross-package-public/lang.toml",
                   "return", 0)]
        cases += [(x.parent.name, x,
                   "panic" if x.parent.name.startswith("panic-") else "return",
                   7 if x.parent.name == "nonzero-main" else 0)
                  for x in sorted((root / "tests/native/fixtures").glob("*/lang.toml"))]
        if a.smoke:
            cases = [c for c in cases if c[0] in SMOKE_CASES]
            if {c[0] for c in cases} != SMOKE_CASES:
                raise RuntimeError("smoke corpus is incomplete")
        supported = [
            ("strict-float", "func f(x f32) f32 {return x+x;} func main() i32{return 0;}"),
            ("string", 'func main() i32 {let text = "hello";return 0;}'),
            ("defer", "func f() {} func main() i32 {defer {f();} return 0;}"),
        ]
        supported_sources = {}
        external_cases = []
        if not a.smoke:
            for name, source in supported:
                d = out / "previously-rejected" / name
                (d / "src").mkdir(parents=True, exist_ok=True)
                manifest = d / "lang.toml"
                manifest.write_text('schema_version=1\n[package]\nname="supported"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
                (d / "src/main.wz").write_text("package app;\n" + source + "\n")
                supported_sources[name] = source
                external_cases.append((name, manifest, "return", 0))
        expected_rows = [(name, optimization)
                         for name, _, _, _ in cases + external_cases
                         for optimization in [0, 2]]
        # All 38 shared prerequisites must pass before emitting any object.
        # Smoke mode checks only its two selected projects and makes no claim
        # about the other 36.
        for name, manifest, _, _ in cases:
            if candidate:
                r = command([driver, "--project", manifest,
                             "--stdlib", stdlib_source, "--check-only",
                             "--format", "json"], label=name + "-preflight",
                            trace=exec_alias(driver))
            else:
                r = command([helper, "--project", manifest, "--check-only",
                             "--format", "json"], label=name + "-preflight")
            try:
                accepted = ok(r) and json.loads(r["stdout"])["word_bits"] == 64
            except (ValueError, KeyError, TypeError):
                accepted = False
            report["preflight"].append({"id": name,
                                        "result": "passed" if accepted else "failed",
                                        "check": r})
            if not accepted:
                failed.append(name + ": static prerequisite")
        report["shared_preflight"] = ([len(cases), len(cases)] if a.smoke
                                      else [len(common), len(common)])
        report["build_execute_cases"] = len(cases) + len(external_cases)
        if failed:
            raise RuntimeError("static prerequisites failed; no object emitted")
        report["previously_rejected"] = []
        for name, manifest, expected, main_return in cases + external_cases:
            external = name in supported_sources
            for optimization in [0, 2]:
                attempted_rows.add((name, optimization))
                directory = (out / "previously-rejected" / name if external else out / name) / ("O" + str(optimization))
                directory.mkdir(parents=True, exist_ok=True)
                obj, executable = directory / "program.o", directory / "program"
                ll, before, cfg = directory / "program.ll", directory / "before.ll", directory / "program.nir"
                row = {"id": name, "optimization": optimization,
                       "manifest": str(manifest),
                       "source_sha256": {(str(s.relative_to(root)) if s.is_relative_to(root) else str(s)):
                                         hashlib.sha256(s.read_bytes()).hexdigest()
                                         for s in sorted((manifest.parent / "src").rglob("*.wz"))},
                       "expected": {"status": expected, "main_return": main_return}}
                if external:
                    row["source"] = supported_sources[name]
                    report["previously_rejected"].append(row)
                else:
                    report["runs"].append(row)
                compile_ok = False
                if candidate:
                    # The four observation files are first written in a private
                    # stage.  Nothing is published unless the driver, both LLVM
                    # verifies, and llc all succeed.
                    for stale in [obj, executable, ll, before, cfg]:
                        if stale.exists():
                            stale.unlink()
                    stage = Path(tempfile.mkdtemp(prefix=".nativeobs-stage-", dir=directory))
                    stage_before = stage / "before.ll"
                    stage_cfg = stage / "program.nir"
                    stage_ll = stage / "program.ll"
                    stage_obj = stage / "program.o"
                    row["stage"] = str(stage)
                    row["compile"] = command(
                        [driver, "--project", manifest, "--stdlib", stdlib_source,
                         "--opt", str(optimization),
                         "--out-llvm-before", stage_before,
                         "--out-native", stage_cfg, "--format", "json"],
                        timeout=a.timeout, label=f"{name}-O{optimization}-driver",
                        trace=exec_alias(driver))
                    if ok(row["compile"]):
                        if (stage_before.is_file() and stage_before.stat().st_size > 0
                                and stage_cfg.is_file() and stage_cfg.stat().st_size > 0):
                            row["artifact_initial"] = {
                                "before.ll": artifact_record(stage_before),
                                "program.nir": artifact_record(stage_cfg),
                            }
                        else:
                            failed.append(f"{name}: driver did not produce observations O{optimization}")
                        try:
                            metadata = json.loads(row["compile"]["stdout"])
                        except (ValueError, TypeError) as error:
                            metadata = None
                            failed.append(f"{name}: invalid driver metadata O{optimization}: {error}")
                        if metadata is not None:
                            if (metadata.get("status") != "passed"
                                    or metadata.get("cfg_verified") is not True
                                    or type(metadata.get("has_async")) is not bool
                                    or metadata.get("word_bits") != 64
                                    or not isinstance(metadata.get("triple"), str)
                                    or "artifact_initial" not in row):
                                failed.append(f"{name}: driver metadata incomplete O{optimization}")
                                metadata = None
                        if metadata is not None:
                            row["metadata"] = metadata
                            row["passes"] = ("verify,default<O2>,verify" if optimization == 2
                                             else ("verify,default<O0>,verify"
                                                   if metadata["has_async"] else "verify"))
                            row["verify_files"] = [command(
                                [llvm / "opt", "-passes=verify", "-disable-output",
                                 stage_before], timeout=a.timeout,
                                label=f"{name}-O{optimization}-verify-before",
                                trace=exec_alias(llvm / "opt"))]
                            row["opt"] = command(
                                [llvm / "opt", "-S", "-passes=" + row["passes"],
                                 "-verify-each", "-mtriple=" + metadata["triple"],
                                 "-mcpu=generic", stage_before, "-o", stage_ll],
                                timeout=a.timeout, label=f"{name}-O{optimization}-opt",
                                trace=exec_alias(llvm / "opt"))
                            if stage_ll.is_file() and stage_ll.stat().st_size > 0:
                                row["artifact_initial"]["program.ll"] = artifact_record(stage_ll)
                            row["verify_files"].append(command(
                                [llvm / "opt", "-passes=verify", "-disable-output",
                                 stage_ll], timeout=a.timeout,
                                label=f"{name}-O{optimization}-verify-after",
                                trace=exec_alias(llvm / "opt")))
                            row["llc"] = command(
                                [llvm / "llc", "-filetype=obj",
                                 "-O=" + str(optimization),
                                 "-mtriple=" + metadata["triple"],
                                 "-mcpu=generic", "-relocation-model=pic",
                                 "-code-model=small", stage_ll, "-o", stage_obj],
                                timeout=a.timeout, label=f"{name}-O{optimization}-llc",
                                trace=exec_alias(llvm / "llc"))
                            if stage_obj.is_file() and stage_obj.stat().st_size > 0:
                                row["artifact_initial"]["program.o"] = artifact_record(stage_obj)
                            if (ok(row["opt"]) and all(ok(x) for x in row["verify_files"])
                                    and ok(row["llc"])
                                    and stage_before.is_file() and stage_before.stat().st_size > 0
                                    and stage_cfg.is_file() and stage_cfg.stat().st_size > 0
                                    and stage_ll.is_file() and stage_ll.stat().st_size > 0
                                    and stage_obj.is_file() and stage_obj.stat().st_size > 0):
                                metadata["llvm_verified_before"] = True
                                metadata["llvm_verified_after"] = True
                                try:
                                    os.replace(stage_before, before)
                                    os.replace(stage_ll, ll)
                                    os.replace(stage_cfg, cfg)
                                    os.replace(stage_obj, obj)
                                except OSError as error:
                                    failed.append(f"{name}: publish failed O{optimization}: {error}")
                                else:
                                    shutil.rmtree(stage, ignore_errors=True)
                                    row["stage"] = None
                                    row["artifact_paths"] = {
                                        "before.ll": str(before),
                                        "program.ll": str(ll),
                                        "program.nir": str(cfg),
                                        "program.o": str(obj),
                                    }
                                    row["published"] = [
                                        str(x.relative_to(root)) if x.is_relative_to(root) else str(x)
                                        for x in [before, ll, cfg, obj]]
                                    row["object_created"] = True
                                    row["before_created"] = True
                                    row["cfg_created"] = True
                                    compile_ok = True
                            else:
                                failed.append(f"{name}: candidate compile O{optimization}")
                        else:
                            failed.append(f"{name}: compile O{optimization}")
                    else:
                        failed.append(f"{name}: driver O{optimization}")
                    if not compile_ok:
                        row["result"] = "failed"
                        row["stage_preserved"] = stage.is_dir()
                        persist_row(row, directory)
                        continue
                else:
                    row["compile"] = command(
                        [helper, "--project", manifest, "--opt", optimization,
                         "--out-object", obj, "--out-llvm", ll,
                         "--out-llvm-before", before, "--out-native", cfg,
                         "--format", "json"], timeout=a.timeout,
                        label=f"{name}-O{optimization}-helper")
                    if not ok(row["compile"]):
                        row["result"] = "failed"
                        failed.append(name + ": compile O" + str(optimization))
                        persist_row(row, directory)
                        continue
                    if external:
                        row["object_created"] = obj.is_file() and obj.stat().st_size > 0
                        if not row["object_created"]:
                            row["result"] = "failed"
                            failed.append(name + ": object missing O" + str(optimization))
                            persist_row(row, directory)
                            continue
                    row["artifact_initial"] = {
                        "before.ll": artifact_record(before),
                        "program.ll": artifact_record(ll),
                        "program.nir": artifact_record(cfg),
                        "program.o": artifact_record(obj),
                    }
                    row["artifact_paths"] = {
                        "before.ll": str(before),
                        "program.ll": str(ll),
                        "program.nir": str(cfg),
                        "program.o": str(obj),
                    }
                    row["metadata"] = json.loads(row["compile"]["stdout"])
                    row["verify_files"] = [
                        command([llvm / "opt", "-passes=verify", "-disable-output", f],
                                timeout=a.timeout, label=f"{name}-O{optimization}-verify")
                        for f in [before, ll]]
                    if not all(ok(x) for x in row["verify_files"]):
                        row["result"] = "failed"
                        failed.append(name + ": LLVM verify")
                        persist_row(row, directory)
                        continue
                    compile_ok = True
                if not compile_ok:
                    row["result"] = "failed"
                    continue
                row["link"] = command(
                    [llvm / "clang", "--target=x86_64-unknown-linux-gnu",
                     "-nostdlib", "-static",
                     "--ld-path=" + str(llvm / "ld.lld"), obj, runtime,
                     "-o", executable], timeout=a.timeout,
                    label=f"{name}-O{optimization}-link",
                    trace=(exec_alias(llvm / "clang") + exec_alias(llvm / "ld.lld") if candidate else None))
                if not ok(row["link"]):
                    row["result"] = "failed"
                    failed.append(name + ": link")
                    persist_row(row, directory)
                    continue
                data = executable.read_bytes()
                row["elf_x86_64"] = data[:4] == b"\x7fELF" and data[4] == 2 and data[18:20] == b"\x3e\x00"
                row["executable_sha256"] = hashlib.sha256(data).hexdigest()
                row["readelf_program_headers"] = command(
                    [llvm / "llvm-readelf", "-lW", executable],
                    timeout=a.timeout, label=f"{name}-O{optimization}-readelf-l",
                    trace=(exec_alias(llvm / "llvm-readelf") if candidate else None))
                row["readelf_dynamic"] = command(
                    [llvm / "llvm-readelf", "-dW", executable],
                    timeout=a.timeout, label=f"{name}-O{optimization}-readelf-d",
                    trace=(exec_alias(llvm / "llvm-readelf") if candidate else None))
                row["undefined_symbols"] = command(
                    [llvm / "llvm-nm", "-u", "--format=posix", executable],
                    timeout=a.timeout, label=f"{name}-O{optimization}-nm-u",
                    trace=(exec_alias(llvm / "llvm-nm") if candidate else None))
                undefined = [line.split() for line in row["undefined_symbols"]["stdout"].splitlines() if line.split()]
                row["strong_undefined_symbols"] = [fields[0] for fields in undefined
                                                   if len(fields) >= 2 and fields[1] == "U"]
                readelf_text = row["readelf_program_headers"]["stdout"].lstrip()
                row["readelf_shape"] = (
                    ok(row["readelf_program_headers"]) and
                    readelf_text.startswith("Elf file type is ") and
                    "Program Headers:" in readelf_text and
                    ok(row["readelf_dynamic"]) and
                    ok(row["undefined_symbols"]) and
                    all(len(fields) >= 2 for fields in undefined))
                row["static_elf"] = (
                    row["readelf_shape"] and
                    "Elf file type is EXEC" in row["readelf_program_headers"]["stdout"] and
                    "INTERP" not in row["readelf_program_headers"]["stdout"] and
                    "NEEDED" not in row["readelf_dynamic"]["stdout"] and
                    not row["strong_undefined_symbols"])
                if not row["static_elf"]:
                    row["result"] = "failed"
                    failed.append(name + ": static ELF O" + str(optimization))
                    persist_row(row, directory)
                    continue
                row["execute"] = command([executable], timeout=a.execute_timeout,
                                         label=f"{name}-O{optimization}-execute",
                                         trace=(exec_alias(executable) if candidate else None))
                row["panic_record"] = panic_record(row["execute"])
                if expected == "panic":
                    passed = row["panic_record"]
                    cause = ("division-by-zero" if "division" in name or "remainder" in name
                             else "invalid-shift" if "shift" in name
                             else "integer-conversion")
                    passed = passed and (":" + cause + ":") in row["execute"]["stderr"]
                else:
                    passed = (row["execute"]["status"] == "exited"
                              and row["execute"]["exit"] == main_return
                              and not row["panic_record"]
                              and row["execute"]["stdout"] == ""
                              and row["execute"]["stderr"] == "")
                passed = passed and row["elf_x86_64"]
                row["result"] = ("supported" if external else "passed") if passed else "failed"
                if not passed:
                    failed.append(name + ": execute O" + str(optimization))
                persist_row(row, directory)
                print(f"{name} O{optimization}: {row['result']} exit={row['execute']['exit']} panic={row['panic_record']}", flush=True)
        # A normal nonzero main has actually run above. Exit status alone never
        # qualifies; neither does a marker from a successful or unlaunched run.
        report["panic_classifier_controls"] = all(not panic_record(x) for x in [
            {"status": "exited", "exit": 7, "stdout": "", "stderr": ""},
            {"status": "exited", "exit": -11, "stdout": "", "stderr": ""},
            {"status": "exited", "exit": 0, "stdout": "", "stderr": "LANG_NATIVE_PANIC:v1:invalid-shift:0:1:2\n"},
            {"status": "launch_failed", "exit": 101, "stdout": "", "stderr": ""},
        ])
        rejects = [
            ("native32", "func main() i32 {return 0;}", 32,
             "E_CAPABILITY_UNAVAILABLE", "unsupported"),
            ("ordinary-parameter-write",
             "func f(x i32) i32 {x=1;return x;} func main() i32{return 0;}", 64,
             "E_TYPE_MISMATCH", "rejected"),
        ]
        for name, source, width, expected_code, category in rejects:
            d = out / "rejections" / name
            (d / "src").mkdir(parents=True, exist_ok=True)
            manifest = d / "lang.toml"
            manifest.write_text('schema_version=1\n[package]\nname="reject"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=' + str(width) + '\n')
            (d / "src/main.wz").write_text("package app;\n" + source + "\n")
            if candidate:
                r = command([driver, "--project", manifest,
                             "--stdlib", stdlib_source, "--opt", "0",
                             "--out-llvm-before", d / "reject-before.ll",
                             "--out-native", d / "reject.nir",
                             "--format", "json"], timeout=a.timeout,
                            label=name + "-reject",
                            trace=(exec_alias(driver) if candidate else None))
            else:
                r = command([helper, "--project", manifest,
                             "--out-object", d / "must-not-exist.o",
                             "--format", "json"], timeout=a.timeout,
                            label=name + "-reject",
                            trace=(exec_alias(driver) if candidate else None))
            try:
                codes = [x["code"] for x in json.loads(r["stderr"])["diagnostics"]]
            except (ValueError, KeyError, TypeError):
                codes = []
            absent = not (d / "must-not-exist.o").exists()
            passed = r["exit"] == 1 and expected_code in codes and absent
            report["rejections"].append({"id": name, "expected_code": expected_code,
                                         "object_absent": absent,
                                         "result": category if passed else "failed",
                                         "command": r})
            if not passed:
                failed.append(name + ": rejection")
        # All matrix rows have finished.  Re-read the four published artifacts
        # once and compare against the SHA/size captured when each was produced.
        for completed in report["runs"] + report["previously_rejected"]:
            initial = completed.get("artifact_initial") or {}
            paths = completed.get("artifact_paths") or {}
            if not initial or not paths:
                continue
            final = {}
            matched = []
            for artifact in ["before.ll", "program.ll", "program.nir", "program.o"]:
                if artifact not in initial:
                    continue
                path = Path(paths[artifact])
                if not path.is_file():
                    failed.append(completed["id"] + ": artifact missing " + artifact)
                    matched.append(False)
                    continue
                record = artifact_record(path)
                final[artifact] = record
                matched.append(record["sha256"] == initial[artifact]["sha256"]
                               and record["size"] == initial[artifact]["size"])
            completed["artifact_final"] = final
            completed["artifact_sha_match"] = (len(final) == len(initial)
                                               and len(matched) == len(initial)
                                               and all(matched))
            if not completed["artifact_sha_match"]:
                failed.append(completed["id"] + ": artifact initial/final SHA mismatch O" + str(completed["optimization"]))
            directory = Path(paths["program.o"]).parent
            (directory / "row-final.json").write_text(
                json.dumps(completed, indent=2, ensure_ascii=False) + "\n")
        invalid = out / "invalid-llvm.ll"
        invalid.write_text("""define i32 @broken(i1 %c) {
entry: br i1 %c, label %yes, label %no
yes: %v = add i32 1, 2
br label %join
no: br label %join
join: ret i32 %v
}
""")
        report["bad_llvm"] = command([llvm / "opt", "-passes=verify", "-disable-output", invalid],
                                     timeout=a.timeout, label="invalid-llvm",
                                     trace=(exec_alias(llvm / "opt") if candidate else None))
        if (ok(report["bad_llvm"]) or "does not dominate" not in report["bad_llvm"]["stderr"]
                or not report["panic_classifier_controls"]):
            failed.append("invalid IR/classifier accepted")
        report["frontend_dynamic_dependencies"] = command(["ldd", frontend],
                                                          label="frontend-ldd")
        report["frontend_llvm_symbols"] = command(["nm", "-C", "--undefined-only", frontend],
                                                  label="frontend-nm")
        if not frontend.is_file():
            failed.append("frontend missing: " + str(frontend))
        if "llvm::" in report["frontend_llvm_symbols"]["stdout"] or "libLLVM" in report["frontend_dynamic_dependencies"]["stdout"]:
            failed.append("frontend depends on LLVM")
        report["summary"] = {"attempts": len(report["runs"]),
            "build_execute_cases": report["build_execute_cases"],
            "build_execute_attempts": len(attempted_rows),
            "not_run": len(expected_rows) - len(attempted_rows),
            "executions": sum("execute" in r for r in report["runs"]),
            "passed": sum(r.get("result") == "passed" for r in report["runs"]),
            "shared_executions": sum("execute" in r and r["id"] in {x["id"] for x in common}
                                     for r in report["runs"]),
            "previously_rejected_attempts": len(report["previously_rejected"]),
            "rejections": len(report["rejections"])}
    except Exception as e:
        failed.append(str(e))
    try:
        report["resource_sha256_after"] = resource_hashes()
        report["resource_sha256_stable"] = (
            report.get("resource_sha256_before") == report["resource_sha256_after"])
        if not report["resource_sha256_stable"]:
            failed.append("resource SHA changed during the run")
    except Exception as e:
        failed.append("resource sha check: " + str(e))
    save()
    print(json.dumps({"status": report["status"], "failed": failed, "report": str(a.report)}))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
