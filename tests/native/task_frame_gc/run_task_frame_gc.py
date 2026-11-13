#!/usr/bin/env python3
"""Real-source Native Task/frame GC metadata checks.

This runner reuses the allocation/Vec observer and command harness from
``tests/native/async_memory`` but owns the Task/frame GC fixtures in this
directory. Every case is built at O0 and O2 by the published Native CLI, run,
rebuilt as LLVM through the Native helper, linked with the existing
``tests/native/language_gc_source/observe_gc.cpp`` observer, and required to
finish with zero live heap allocations, zero live Vec payloads and balanced
ALLOC/FREE and VEC:1/VEC:5 events.

The LLVM checks also assert that a non-null generated frame trace activates
``lang-gc-program/1`` and that provable leaf Tasks keep the no-graph startup
path.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE_RUNNER = HERE.parent / "async_memory" / "run_async_memory.py"
OBSERVER = "tests/native/language_gc_source/observe_gc.cpp"

CASES = (
    {
        "id": "task-move-vec-cycle",
        "stdout": "",
        "min_heap_allocations": 128,
        "min_vec_allocations": 64,
        "purpose": "never-started move Vec<Task> frame owner cycle is reclaimed",
        "llvm_require": (
            "lang.async.frame.meta.",
            "@lang.async.frame.trace.",
            "@lang.async.frame.clear.",
            "@lang_async_publish_v2",
            "call i64 @lang_gc_program_init_v1()",
        ),
        "llvm_forbid": ("@lang_async_publish_v1",),
    },
    {
        "id": "task-move-closure-cycle",
        "stdout": "",
        "min_heap_allocations": 128,
        "min_vec_allocations": 64,
        "purpose": "move closure environment and move Vec<Task> frame cycle is reclaimed",
        "llvm_require": (
            "lang.async.frame.meta.",
            "@lang.async.frame.trace.",
            "@lang.async.frame.clear.",
            "@lang_async_publish_v2",
            "call i64 @lang_gc_program_init_v1()",
        ),
        "llvm_forbid": ("@lang_async_publish_v1",),
    },
    {
        "id": "task-result-cycle",
        "stdout": "",
        "min_heap_allocations": 4,
        "min_vec_allocations": 1,
        "purpose": "completed Task promise-result cycle is reclaimed",
        "llvm_require": (
            "@lang.async.frame.trace.",
            "@lang.async.frame.clear.",
            "call i64 @lang_gc_program_init_v1()",
        ),
        "llvm_forbid": ("@lang_async_publish_v1",),
    },
    {
        "id": "task-error-cycle",
        "stdout": "",
        "min_heap_allocations": 4,
        "min_vec_allocations": 1,
        "purpose": "typed-error promise payload cycle is reclaimed",
        "llvm_require": (
            "@lang.async.frame.trace.",
            "@lang.async.frame.clear.",
            "call i64 @lang_gc_program_init_v1()",
        ),
        "llvm_forbid": ("@lang_async_publish_v1",),
    },
    {
        "id": "task-live-move-root",
        "stdout": "42\n",
        "min_heap_allocations": 2,
        "min_vec_allocations": 1,
        "purpose": "executor-rooted live move-parameter frame survives GC pressure",
        "llvm_require": (
            "@lang.async.frame.trace.",
            "@lang.async.frame.clear.",
            "call i64 @lang_gc_program_init_v1()",
        ),
        "llvm_forbid": ("@lang_async_publish_v1",),
    },
    {
        "id": "task-await-chain",
        "stdout": "7\n",
        "min_heap_allocations": 4,
        "min_vec_allocations": 1,
        "purpose": "await link keeps both move-parameter frames and both references",
        "llvm_require": (
            "@lang.async.frame.trace.",
            "@lang.async.frame.clear.",
            "call i64 @lang_gc_program_init_v1()",
        ),
        "llvm_forbid": ("@lang_async_publish_v1",),
    },
    {
        "id": "task-await-scalar",
        "stdout": "",
        "min_heap_allocations": 4,
        "min_vec_allocations": 0,
        "purpose": "Task-owned await edge activates GC even without any Vec",
        "llvm_require": (
            "@lang.async.frame.trace.",
            "@lang.async.frame.clear.",
            "call i64 @lang_gc_program_init_v1()",
        ),
        "llvm_forbid": ("@lang_async_publish_v1",),
    },
    {
        "id": "task-never-started-move-cleanup",
        "stdout": "",
        "min_heap_allocations": 2,
        "min_vec_allocations": 2,
        "purpose": "never-started move owners drop once and never run body/defer",
        "llvm_require": (
            "@lang.async.frame.trace.",
            "@lang.async.frame.clear.",
            "call i64 @lang_gc_program_init_v1()",
        ),
        "llvm_forbid": ("@lang_async_publish_v1",),
    },
    {
        "id": "task-unit-nograph",
        "stdout": "",
        "min_heap_allocations": 2,
        "min_vec_allocations": 0,
        "purpose": "empty Unit Task is a leaf and keeps the no-graph startup path",
        "llvm_require": (
            "@lang.async.frame.clear.",
            "@lang_async_publish_v2",
        ),
        "llvm_forbid": (
            "@lang.async.frame.trace.",
            "call i64 @lang_gc_program_init_v1()",
            "@lang_async_publish_v1",
        ),
    },
    {
        "id": "task-scalar-nograph",
        "stdout": "scalar-ok\n",
        "min_heap_allocations": 2,
        "min_vec_allocations": 0,
        "purpose": "scalar-only Task is a leaf and keeps the no-graph startup path",
        "llvm_require": (
            "@lang.async.frame.clear.",
            "@lang_async_publish_v2",
        ),
        "llvm_forbid": (
            "@lang.async.frame.trace.",
            "call i64 @lang_gc_program_init_v1()",
            "@lang_async_publish_v1",
        ),
    },
)
CASE_BY_ID = {case["id"]: case for case in CASES}

REJECT_CASES = (
    {
        "id": "async-c-resource-reject",
        "code": "E_CAPABILITY_UNAVAILABLE",
        "message": "external C resource",
        "purpose": "direct async C-resource frame owner is rejected before output",
    },
    {
        "id": "async-c-closure-dyn-order",
        "code": "E_CAPABILITY_UNAVAILABLE",
        "message": "external C resource",
        "purpose": "closure -> dyn -> C resource, make_fn first, is rejected",
    },
    {
        "id": "async-c-closure-dyn-reversed",
        "code": "E_CAPABILITY_UNAVAILABLE",
        "message": "external C resource",
        "purpose": "closure -> dyn -> C resource, make_dyn first, is rejected",
    },
    {
        "id": "async-c-dyn-closure-order",
        "code": "E_CAPABILITY_UNAVAILABLE",
        "message": "external C resource",
        "purpose": "dyn -> closure -> C resource, make_dyn first, is rejected",
    },
    {
        "id": "async-c-dyn-closure-reversed",
        "code": "E_CAPABILITY_UNAVAILABLE",
        "message": "external C resource",
        "purpose": "dyn -> closure -> C resource, make_fn first, is rejected",
    },
    {
        "id": "async-c-closure-dyn-cycle",
        "code": "E_CAPABILITY_UNAVAILABLE",
        "message": "external C resource",
        "purpose": "real Task/closure/dyn/C-resource cycle shape is rejected",
    },
)
REJECT_BY_ID = {case["id"]: case for case in REJECT_CASES}


def load_base():
    spec = importlib.util.spec_from_file_location("async_memory_runner",
                                                  BASE_RUNNER)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {BASE_RUNNER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BASE = load_base()


def build_and_check(case, opt, args, env, commands, hashes):
    case_id = case["id"]
    need = BASE.need
    directory = args.out / "native" / case_id / f"O{opt}"
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    manifest = HERE / "fixtures" / case_id / "lang.toml"
    need(manifest.is_file(), f"missing fixture manifest: {manifest}")

    build = BASE.invoke([
        args.cli, "build", "--project", manifest, "--backend", "native",
        "--opt", str(opt), "--out-dir", directory / "cli", "--format", "json",
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
    executed = BASE.invoke([program], args.timeout, env)
    commands.append(executed)
    need(not executed.get("timeout") and executed["exit"] == 0,
         f"{case_id} O{opt} execution failed: {executed}")
    need(executed["stdout"] == case["stdout"],
         f"{case_id} O{opt} stdout mismatch: {executed}")
    need(executed["stderr"] == "",
         f"{case_id} O{opt} unexpected stderr: {executed}")
    elf_check = BASE.check_static_elf(program, args.readelf, args.timeout, env,
                                      commands)

    ir_dir = directory / "ir"
    ir_dir.mkdir()
    emit = BASE.invoke([
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
    need(emitted.get("status") == "passed", f"{case_id} O{opt} helper: {emitted}")
    llvm_text = (ir_dir / f"O{opt}.before.ll").read_text(encoding="utf-8")
    llvm_checks = {}
    for token in case.get("llvm_require", ()):
        llvm_checks["require:" + token] = token in llvm_text
    for token in case.get("llvm_forbid", ()):
        llvm_checks["forbid:" + token] = token not in llvm_text
    missing = [key for key, present in llvm_checks.items() if not present]
    need(not missing,
         f"{case_id} O{opt} LLVM metadata/startup checks failed: {missing}")

    observe_dir = directory / "observe"
    observe_dir.mkdir()
    linked = BASE.invoke([
        args.clang, f"-O{opt}", "-std=c++20",
        ir_dir / f"O{opt}.before.ll", args.observer, args.runtime,
        "-o", observe_dir / "observe", "-nostdlib", "-static",
        "-Wl,--wrap=lang_alloc_v1", "-Wl,--wrap=lang_free_v1",
    ], args.timeout, env)
    commands.append(linked)
    need(not linked.get("timeout") and linked["exit"] == 0,
         f"{case_id} O{opt} observer link failed: {linked}")
    observer_elf = BASE.check_static_elf(observe_dir / "observe", args.readelf,
                                         args.timeout, env, commands)

    observed = BASE.invoke([observe_dir / "observe"], args.timeout, env)
    commands.append(observed)
    need(not observed.get("timeout") and observed["exit"] == 0,
         f"{case_id} O{opt} observer execution failed")
    need(observed["stdout"] == case["stdout"],
         f"{case_id} O{opt} observer stdout mismatch: {observed}")
    need("LANG_NATIVE_PANIC" not in observed["stderr"],
         f"{case_id} O{opt} panicked: {observed['stderr']}")
    metrics = BASE.parse_observer(observed["stderr"])

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
            BASE.record_hash(hashes, args.out, path)

    return {
        "status": "passed",
        "purpose": case["purpose"],
        "metrics": metrics,
        "llvm_checks": llvm_checks,
        "elf": elf_check,
        "observer_elf": observer_elf,
        "program_sha256": BASE.digest(program),
        "observer_sha256": BASE.digest(observe_dir / "observe"),
    }


def check_reject(case, opt, args, env, commands):
    case_id = case["id"]
    need = BASE.need
    directory = args.out / "reject" / case_id / f"O{opt}"
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    manifest = HERE / "fixtures" / case_id / "lang.toml"
    need(manifest.is_file(), f"missing reject fixture manifest: {manifest}")
    build = BASE.invoke([
        args.cli, "build", "--project", manifest, "--backend", "native",
        "--opt", str(opt), "--out-dir", directory / "cli", "--format", "json",
    ], args.timeout, env)
    commands.append(build)
    need(not build.get("timeout"), f"{case_id} O{opt} build timed out")
    need(build["exit"] != 0, f"{case_id} O{opt} was silently accepted")
    payload = json.loads(build["stdout"])
    need(payload.get("status") == "error", f"{case_id} O{opt}: {payload}")
    diagnostics = payload.get("diagnostics", [])
    matching = [d for d in diagnostics if d.get("code") == case["code"]]
    need(matching, f"{case_id} O{opt} missing {case['code']}: {payload}")
    need(any(case["message"] in d.get("message", "") for d in matching),
         f"{case_id} O{opt} diagnostic does not state the limitation: {payload}")
    need(not (directory / "cli" / "program").exists(),
         f"{case_id} O{opt} produced a program despite rejection")
    need(not (directory / "cli" / "program.o").exists(),
         f"{case_id} O{opt} produced an object despite rejection")
    return {"status": "passed", "purpose": case["purpose"],
            "diagnostic": matching[0]}


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
    parser.add_argument("--observer", type=Path, default=None)
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
    args.observer = (args.observer or args.root / OBSERVER).resolve()
    args.out.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    if args.libxml2_lib:
        previous = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = str(args.libxml2_lib) + (
            ":" + previous if previous else "")

    selected = (set(args.only) if args.only
                else set(CASE_BY_ID) | set(REJECT_BY_ID))
    options = args.opt or [0, 2]
    commands = []
    hashes = {}
    summary = {
        "schema": "woozy/native-task-frame-gc/1",
        "status": "failed",
        "scope": "real-source Native Task/frame GC metadata and ownership",
        "cases": {},
        "limits": [
            "Each fixture is limited by the runner timeout.",
            "The observer covers lang_alloc_v1/lang_free_v1 and Vec events; "
            "source output is checked separately.",
            "External C resources in an async frame/promise are rejected "
            "before output because stopped-world clear cannot run a C release; "
            "the runner asserts E_CAPABILITY_UNAVAILABLE and no program.",
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
    for case in REJECT_CASES:
        if case["id"] not in selected:
            continue
        summary["cases"][case["id"]] = {"O0": {}, "O2": {}}
        case_ok = True
        for opt in options:
            try:
                summary["cases"][case["id"]][f"O{opt}"] = check_reject(
                    case, opt, args, env, commands)
            except Exception as error:
                summary["cases"][case["id"]][f"O{opt}"] = {
                    "status": "failed", "error": str(error)}
                failures.append({"id": case["id"], "opt": opt,
                                 "error": str(error)})
                case_ok = False
        summary["cases"][case["id"]]["status"] = (
            "passed" if case_ok else "failed")

    for path in (Path(__file__).resolve(), args.observer,
                 HERE / "fixtures"):
        if path.is_file():
            BASE.record_hash(hashes, args.root, path)
    for fixture in sorted((HERE / "fixtures").iterdir()):
        for path in sorted(fixture.rglob("*")):
            if path.is_file():
                BASE.record_hash(hashes, args.root, path)

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
