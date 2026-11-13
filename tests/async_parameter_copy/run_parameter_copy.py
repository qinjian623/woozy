#!/usr/bin/env python3
"""Real-source Native checks for ordinary async parameter copies.

Every fixture creates a Task with an ordinary owning parameter in a block that
ends before the Task is started or awaited. The build is checked at O0 and O2,
then emitted as LLVM and linked with the existing allocation observer. A case
passes only when the source output matches and every allocation is released
exactly once.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
OBSERVER = "tests/native/language_gc_source/observe_gc.cpp"


def load_memory_runner():
    path = HERE.parent / "native/async_memory/run_async_memory.py"
    spec = importlib.util.spec_from_file_location("async_memory_runner", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


MEMORY = load_memory_runner()
need = MEMORY.need
invoke = MEMORY.invoke
parse_observer = MEMORY.parse_observer
check_static_elf = MEMORY.check_static_elf
record_hash = MEMORY.record_hash
digest = MEMORY.digest

# Permanent source expectations. The allocation minima avoid a vacuous pass if
# the compiler stops constructing the owning value under test.
CASES = (
    {
        "id": "param-vec-late-start",
        "stdout": "42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "ordinary Vec parameter survives make's block and late start",
    },
    {
        "id": "param-string-await",
        "stdout": "value=42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 0,
        "purpose": "dynamic string parameter survives the caller block and await",
    },
    {
        "id": "param-struct-enum-await",
        "stdout": "7\n9\n",
        "min_heap_allocations": 4,
        "min_vec_allocations": 2,
        "purpose": "owning struct and enum fields copy recursively across await",
    },
    {
        "id": "param-fn-capture-await",
        "stdout": "42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 2,
        "purpose": "capturing function parameter retains its closure environment",
    },
    {
        "id": "param-weak-await",
        "stdout": "42\n42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "Weak parameter retains its control block across suspend",
    },
    {
        "id": "param-dyn-await",
        "stdout": "42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "dyn Trait parameter clones its owned box across suspend",
    },
    {
        "id": "param-throw",
        "stdout": "42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 2,
        "purpose": "typed-error exit drops the frame-owned parameter once",
    },
    {
        "id": "param-cancel",
        "stdout": "cancel-cleanup\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "cancel exit drops both frame-owned parameters once",
    },
    {
        "id": "param-cancel-queued",
        "stdout": "",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "queued cancel drops the parameter before the body ran",
    },
    {
        "id": "param-unstarted",
        "stdout": "",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "never-started Task drops the private parameter copy",
    },
)
CASE_BY_ID = {case["id"]: case for case in CASES}

# Negative cases: an async ordinary owning parameter needs a real copy. A C
# resource that declares no retain_symbol cannot be borrowed across suspend.
REJECTS = (
    {
        "id": "reject-resource-copy",
        "code": "E_CAPABILITY_UNAVAILABLE",
        "message": "retain_symbol",
        "purpose": "non-copyable C resource parameter is rejected",
    },
)
REJECT_BY_ID = {case["id"]: case for case in REJECTS}


def build_and_check(case, opt, args, env, commands, hashes):
    case_id = case["id"]
    directory = args.out / case_id / f"O{opt}"
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    manifest = HERE / "fixtures" / case_id / "lang.toml"
    need(manifest.is_file(), f"missing fixture manifest: {manifest}")

    build = invoke([
        args.cli, "build", "--project", manifest, "--backend", "native",
        "--opt", str(opt), "--out-dir", directory / "cli",
        "--format", "json",
    ], args.timeout, env)
    commands.append(build)
    need(not build.get("timeout") and build["exit"] == 0,
         f"{case_id} O{opt} CLI build failed: {build}")
    payload = json.loads(build["stdout"])
    need(payload.get("status") == "ok", f"{case_id} O{opt}: {payload}")
    need(not any(diagnostic.get("code") == "E_CAPABILITY_UNAVAILABLE"
                 for diagnostic in payload.get("diagnostics", [])),
         f"{case_id} O{opt} silently fell back")

    program = directory / "cli" / "program"
    need(program.is_file(), f"{case_id} O{opt} emitted no program")
    executed = invoke([program], args.timeout, env)
    commands.append(executed)
    need(not executed.get("timeout") and executed["exit"] == 0,
         f"{case_id} O{opt} execution failed: {executed}")
    need(executed["stdout"] == case["stdout"],
         f"{case_id} O{opt} stdout mismatch: {executed}")
    need(executed["stderr"] == "",
         f"{case_id} O{opt} unexpected stderr: {executed}")
    elf_check = check_static_elf(program, args.readelf, args.timeout, env,
                                 commands)

    ir_dir = directory / "ir"
    ir_dir.mkdir()
    emit = invoke([
        args.helper, "--project", manifest, "--opt", str(opt),
        "--out-object", ir_dir / f"O{opt}.o",
        "--out-llvm", ir_dir / f"O{opt}.after.ll",
        "--out-llvm-before", ir_dir / f"O{opt}.before.ll",
        "--out-native", ir_dir / f"O{opt}.nir",
        "--format", "json",
    ], args.timeout, env)
    commands.append(emit)
    need(not emit.get("timeout") and emit["exit"] == 0,
         f"{case_id} O{opt} helper emit failed: {emit}")
    emitted = json.loads(emit["stdout"])
    need(emitted.get("status") == "passed",
         f"{case_id} O{opt} helper: {emitted}")
    llvm_text = (ir_dir / f"O{opt}.before.ll").read_text(encoding="utf-8")
    if case_id == "param-string-await":
        need("lang_text_concat_v1" in llvm_text,
             f"{case_id} O{opt} did not create a dynamic string")

    observe_dir = directory / "observe"
    observe_dir.mkdir()
    linked = invoke([
        args.clang, f"-O{opt}", "-std=c++20",
        ir_dir / f"O{opt}.before.ll", args.observer, args.runtime,
        "-o", observe_dir / "observe", "-nostdlib", "-static",
        "-Wl,--wrap=lang_alloc_v1", "-Wl,--wrap=lang_free_v1",
    ], args.timeout, env)
    commands.append(linked)
    need(not linked.get("timeout") and linked["exit"] == 0,
         f"{case_id} O{opt} observer link failed: {linked}")
    observer_elf = check_static_elf(observe_dir / "observe", args.readelf,
                                    args.timeout, env, commands)

    observed = invoke([observe_dir / "observe"], args.timeout, env)
    commands.append(observed)
    need(not observed.get("timeout") and observed["exit"] == 0,
         f"{case_id} O{opt} observer execution failed: {observed}")
    need(observed["stdout"] == case["stdout"],
         f"{case_id} O{opt} observer stdout mismatch: {observed}")
    need("LANG_NATIVE_PANIC" not in observed["stderr"],
         f"{case_id} O{opt} panicked: {observed['stderr']}")
    metrics = parse_observer(observed["stderr"])

    need(metrics["final_live_heap_count"] == 0,
         f"{case_id} O{opt} leaked heap allocations: {metrics}")
    need(metrics["final_live_heap_bytes"] == 0,
         f"{case_id} O{opt} leaked heap bytes: {metrics}")
    need(metrics["heap_allocations"] == metrics["heap_frees"],
         f"{case_id} O{opt} unbalanced heap allocations: {metrics}")
    need(metrics["vec_allocations"] == metrics["vec_frees"],
         f"{case_id} O{opt} unbalanced Vec count: {metrics}")
    need(metrics["live_vecs_at_exit"] == 0,
         f"{case_id} O{opt} leaked Vecs: {metrics}")
    need(metrics["final_live_payload_bytes"] == 0,
         f"{case_id} O{opt} leaked Vec payload: {metrics}")
    need(metrics["heap_allocations"] >= case["min_heap_allocations"],
         f"{case_id} O{opt} did not allocate the object set: {metrics}")
    need(metrics["vec_allocations"] >= case["min_vec_allocations"],
         f"{case_id} O{opt} did not allocate the Vec set: {metrics}")

    for path in (program, directory / "cli" / "program.o",
                 ir_dir / f"O{opt}.nir",
                 ir_dir / f"O{opt}.before.ll",
                 ir_dir / f"O{opt}.after.ll",
                 ir_dir / f"O{opt}.o",
                 observe_dir / "observe"):
        if path.is_file():
            record_hash(hashes, args.out, path)

    return {
        "status": "passed",
        "purpose": case["purpose"],
        "metrics": metrics,
        "elf": elf_check,
        "observer_elf": observer_elf,
        "program_sha256": digest(program),
        "observer_sha256": digest(observe_dir / "observe"),
    }


def reject_check(case, opt, args, env, commands, hashes):
    case_id = case["id"]
    directory = args.out / case_id / f"O{opt}"
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    manifest = HERE / "fixtures" / case_id / "lang.toml"
    need(manifest.is_file(), f"missing fixture manifest: {manifest}")

    build = invoke([
        args.cli, "build", "--project", manifest, "--backend", "native",
        "--opt", str(opt), "--out-dir", directory / "cli",
        "--format", "json",
    ], args.timeout, env)
    commands.append(build)
    need(not build.get("timeout"), f"{case_id} O{opt} build timed out")
    need(build["exit"] != 0, f"{case_id} O{opt} was not rejected: {build}")
    payload = json.loads(build["stdout"])
    need(payload.get("status") == "error", f"{case_id} O{opt}: {payload}")
    diagnostics = payload.get("diagnostics", [])
    need(any(diagnostic.get("code") == case["code"] and
             case["message"] in diagnostic.get("message", "")
             for diagnostic in diagnostics),
         f"{case_id} O{opt} rejected with the wrong diagnostic: {diagnostics}")
    for path in (manifest, HERE / "fixtures" / case_id / "binding.json"):
        record_hash(hashes, args.root, path)
    return {
        "status": "passed",
        "purpose": case["purpose"],
        "code": case["code"],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--helper", type=Path)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--toolchain", type=Path,
                        help="LLVM tools directory containing clang and llvm-readelf")
    parser.add_argument("--root", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--clang", type=Path, default=None)
    parser.add_argument("--readelf", type=Path, default=None)
    parser.add_argument("--libxml2-lib", type=Path, default=None)
    parser.add_argument("--only", action="append",
                        choices=tuple(CASE_BY_ID) + tuple(REJECT_BY_ID))
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2))
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()

    required = ("cli", "helper", "runtime", "toolchain", "root", "out")
    missing = [name for name in required if getattr(args, name) is None]
    if missing:
        parser.error("missing required arguments: " + ", ".join(missing))

    args.root = args.root.resolve()
    args.out = args.out.resolve()
    args.cli = args.cli.resolve()
    args.helper = args.helper.resolve()
    args.runtime = args.runtime.resolve()
    args.toolchain = args.toolchain.resolve()
    args.clang = (args.clang or args.toolchain / "clang").resolve()
    args.readelf = (args.readelf or args.toolchain / "llvm-readelf").resolve()
    args.observer = (args.root / OBSERVER).resolve()
    args.out.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    if args.libxml2_lib:
        previous = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = str(args.libxml2_lib) + (
            ":" + previous if previous else "")

    selected = set(args.only) if args.only else (
        set(CASE_BY_ID) | set(REJECT_BY_ID))
    options = args.opt or [0, 2]
    commands = []
    hashes = {}
    summary = {
        "schema": "woozy/native-async-parameter-copy/1",
        "status": "failed",
        "scope": "ordinary async owning parameters copied into the Task frame",
        "cases": {},
        "limits": [
            "Each fixture is limited by the runner timeout.",
            "The observer covers lang_alloc_v1/lang_free_v1 and Vec events; "
            "source output is checked separately.",
        ],
        "hashes": hashes,
    }
    failures = []
    for case in CASES:
        if case["id"] not in selected:
            continue
        summary["cases"][case["id"]] = {"O0": {}, "O2": {}}
        case_ok = True
        for opt in options:
            try:
                summary["cases"][case["id"]][f"O{opt}"] = build_and_check(
                    case, opt, args, env, commands, hashes)
            except Exception as error:
                summary["cases"][case["id"]][f"O{opt}"] = {
                    "status": "failed", "error": str(error)}
                failures.append({"id": case["id"], "opt": opt,
                                 "error": str(error)})
                case_ok = False
        summary["cases"][case["id"]]["status"] = (
            "passed" if case_ok else "failed")

    for case in REJECTS:
        if case["id"] not in selected:
            continue
        summary["cases"][case["id"]] = {"O0": {}, "O2": {}}
        case_ok = True
        for opt in options:
            try:
                summary["cases"][case["id"]][f"O{opt}"] = reject_check(
                    case, opt, args, env, commands, hashes)
            except Exception as error:
                summary["cases"][case["id"]][f"O{opt}"] = {
                    "status": "failed", "error": str(error)}
                failures.append({"id": case["id"], "opt": opt,
                                 "error": str(error)})
                case_ok = False
        summary["cases"][case["id"]]["status"] = (
            "passed" if case_ok else "failed")

    for path in (HERE / "run_parameter_copy.py", args.observer):
        if path.is_file():
            record_hash(hashes, args.root, path)
    for fixture in sorted((HERE / "fixtures").iterdir()):
        for path in sorted(fixture.rglob("*")):
            if path.is_file():
                record_hash(hashes, args.root, path)

    summary["status"] = "passed" if not failures else "failed"
    summary["commands"] = commands
    summary["failures"] = failures
    (args.out / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    (args.out / "commands.json").write_text(
        json.dumps(commands, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "status": summary["status"],
        "passed": len(selected) * len(options) - len(failures),
        "failed": len(failures),
        "failures": failures,
    }))
    return 0 if summary["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
