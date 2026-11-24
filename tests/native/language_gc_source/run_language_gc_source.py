#!/usr/bin/env python3
"""Real-source Native automatic cycle collection (MEM02) checks.

Each fixture is built by the published ``woozyc`` CLI at O0 and O2 and run as
the real static program. The same verified program is then emitted as LLVM and
linked with the test-only freestanding observer so live Vec payload can be
measured exactly. No fixture calls a source-level collect operation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from gc_ir_contract import ContractError, check_gc_poll_contract

HERE = Path(__file__).resolve().parent
CASES = (
    "self-cycle",
    "mutual-cycle",
    "closure-cycle",
    "live-root-cycle",
    "stress-cycle",
    "scalar-control",
    "weak-cycle",
)
CYCLE_CASES = tuple(c for c in CASES if c != "scalar-control")
PRESSURE_CASES = {"live-root-cycle", "stress-cycle"}
PAYLOAD_BUDGET = 2 * 1024 * 1024


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, timeout, env, cwd=None):
    argv = [str(value) for value in argv]
    started = time.time()
    try:
        result = subprocess.run(argv, capture_output=True, text=True,
                                timeout=timeout, env=env, cwd=cwd)
        return {
            "argv": argv,
            "elapsed_s": round(time.time() - started, 6),
            "exit": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
        }
    except subprocess.TimeoutExpired as error:
        return {
            "argv": argv,
            "elapsed_s": round(time.time() - started, 6),
            "timeout": True,
            "exit": None,
            "stdout": (error.stdout or "") if isinstance(error.stdout, str) else "",
            "stderr": (error.stderr or "") if isinstance(error.stderr, str) else "",
        }


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def parse_events(text: str):
    """Parse CONFIG/ALLOC/FREE/VEC into exact Vec payload metrics."""
    header = None
    generation = {}
    current = {}
    records = {}
    live_payload = 0
    peak_payload = 0
    vec_allocations = 0
    vec_frees = 0
    pending_free = None

    for line in text.splitlines():
        if line.startswith("CONFIG:"):
            parts = line.split(":")
            need(len(parts) == 3 and parts[1] == "vec_header_bytes",
                 f"malformed CONFIG: {line!r}")
            header = int(parts[2])
            need(header > 0 and header % 16 == 0,
                 f"invalid Vec header size: {header}")
            continue
        if line.startswith("ALLOC:"):
            need(header is not None, "ALLOC before CONFIG")
            parts = line.split(":")
            need(len(parts) == 3, f"malformed ALLOC: {line!r}")
            pointer, size = parts[1], int(parts[2])
            need(pointer not in current, f"ALLOC for live address: {line!r}")
            gen = generation.get(pointer, 0) + 1
            generation[pointer] = gen
            key = (pointer, gen)
            records[key] = {
                "size": size,
                "vec": False,
                "payload": 0,
                "freed": False,
            }
            current[pointer] = key
            continue
        if line.startswith("FREE:"):
            parts = line.split(":")
            need(len(parts) == 2, f"malformed FREE: {line!r}")
            key = current.pop(parts[1], None)
            need(key is not None, f"FREE for unknown address: {line!r}")
            record = records[key]
            need(not record["freed"], f"duplicate FREE: {line!r}")
            record["freed"] = True
            if record["vec"]:
                live_payload -= record["payload"]
                vec_frees += 1
            pending_free = None
            continue
        if line.startswith("VEC:"):
            parts = line.split(":")
            need(len(parts) == 4, f"malformed VEC: {line!r}")
            event, pointer, detail = int(parts[1]), parts[2], int(parts[3])
            key = current.get(pointer)
            need(key is not None, f"VEC for unknown address: {line!r}")
            record = records[key]
            if event == 1:
                need(not record["vec"], f"duplicate VEC:1: {line!r}")
                payload = record["size"] - header
                need(payload >= 0, f"Vec smaller than header: {line!r}")
                record["vec"] = True
                record["payload"] = payload
                live_payload += payload
                peak_payload = max(peak_payload, live_payload)
                vec_allocations += 1
            elif event == 5:
                need(record["vec"], f"VEC:5 for non-Vec: {line!r}")
                pending_free = key
            continue
        raise RuntimeError(f"unexpected observation line: {line!r}")

    need(header is not None, "observer emitted no CONFIG")
    need(pending_free is None, "VEC:5 was not followed by FREE")
    live_vecs = sum(1 for record in records.values()
                    if record["vec"] and not record["freed"])
    return {
        "header_bytes": header,
        "vec_allocations": vec_allocations,
        "vec_frees": vec_frees,
        "live_vecs_at_exit": live_vecs,
        "peak_live_payload_bytes": peak_payload,
        "final_live_payload_bytes": live_payload,
    }


def inspect_llvm(llvm: str, case: str):
    try:
        poll = check_gc_poll_contract(llvm)
    except ContractError as error:
        need(False, f"{case}: {error}")
    checks = {
        "program_init": "lang_gc_program_init_v1" in llvm,
        "program_finish": "lang_gc_program_finish_v1" in llvm,
        "safepoint_poll": poll["sites"] > 0,
        "safepoint_positions": poll["positions_exact"],
        "controller_poll": poll["controller_calls"] == poll["sites"],
        "direct_poll_absent": (poll["direct_world_calls"] == 0 and
                               poll["direct_pressure_calls"] == 0 and
                               poll["inline_word_loads"] == 0),
        "safepoint_functions": poll["functions"] > 0,
        "safepoint_sites": poll["sites"],
    }
    if case == "scalar-control":
        checks["no_registration"] = "lang_gc_program_register_v1" not in llvm
        checks["no_trace_callback"] = "lang.gc.trace." not in llvm
    else:
        checks["registration"] = "lang_gc_program_register_v1" in llvm
        checks["trace_callback"] = "lang.gc.trace." in llvm
        checks["vec_clear_ops"] = "lang_vec_clear_v1" in llvm
        # These fixtures modify a whole Vec element. The runtime setter
        # protects publication; generated stack-slot guards are unrelated.
        checks["guarded_vec_set"] = bool(re.search(
            r"\bcall\s+void\s+@lang_vec_set_v1\(", llvm))
    if case == "closure-cycle":
        checks["closure_environment_trace"] = ("lang.gc.trace.e" in llvm and
                                                "lang.gc.ops.e" in llvm)
    if case == "weak-cycle":
        checks["weak_invalidation"] = "lang_weak_invalidate_slot_v1" in llvm
    failed = [name for name, passed in checks.items() if not passed]
    need(not failed, f"{case}: LLVM checks failed: {failed}")
    return checks


def build_and_check(case, opt, args, env, commands, hashes):
    directory = args.out / "native" / case / f"O{opt}"
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    manifest = args.root / "tests/native/language_gc_source/fixtures" / case / "lang.toml"

    build = run([args.cli, "build", "--project", manifest, "--backend",
                 "native", "--opt", str(opt), "--out-dir", directory / "cli",
                 "--format", "json"], args.timeout, env)
    commands.append(build)
    need(not build.get("timeout") and build["exit"] == 0,
         f"{case} O{opt} CLI build failed: {build}")
    payload = json.loads(build["stdout"])
    need(payload.get("status") == "ok", f"{case} O{opt}: {payload}")
    need(not any(d.get("code") == "E_CAPABILITY_UNAVAILABLE"
                 for d in payload.get("diagnostics", [])),
         f"{case} O{opt} silently fell back")

    program = directory / "cli" / "program"
    need(program.is_file(), f"{case} O{opt} emitted no program")
    executed = run([program], args.timeout, env)
    commands.append(executed)
    need(not executed.get("timeout") and executed["exit"] == 0,
         f"{case} O{opt} execution failed: {executed}")
    need(not executed["stdout"] and not executed["stderr"],
         f"{case} O{opt} produced unexpected output: {executed}")

    elf = run([args.readelf, "-l", "-d", program], 60, env)
    commands.append(elf)
    need(not elf.get("timeout") and elf["exit"] == 0,
         f"{case} O{opt} readelf failed: {elf}")
    need("INTERP" not in elf["stdout"] and "NEEDED" not in elf["stdout"],
         f"{case} O{opt} is not a static freestanding ELF")

    directory_ir = directory / "ir"
    directory_ir.mkdir()
    emit = run([args.helper, "--project", manifest, "--opt", str(opt),
                "--out-object", directory_ir / f"O{opt}.o",
                "--out-llvm", directory_ir / f"O{opt}.after.ll",
                "--out-llvm-before", directory_ir / f"O{opt}.before.ll",
                "--out-native", directory_ir / f"O{opt}.nir",
                "--format", "json"], args.timeout, env)
    commands.append(emit)
    need(not emit.get("timeout") and emit["exit"] == 0,
         f"{case} O{opt} helper emit failed: {emit}")
    emitted = json.loads(emit["stdout"])
    need(emitted.get("status") == "passed", f"{case} O{opt} helper: {emitted}")

    llvm = (directory_ir / f"O{opt}.before.ll").read_text(encoding="utf-8")
    checks = inspect_llvm(llvm, case)

    observer_dir = directory / "observe"
    observer_dir.mkdir()
    linked = run([args.clang, f"-O{opt}", "-std=c++20",
                  directory_ir / f"O{opt}.before.ll",
                  HERE / "observe_gc.cpp", args.runtime,
                  "-o", observer_dir / "observe", "-nostdlib", "-static",
                  "-Wl,--wrap=lang_alloc_v1", "-Wl,--wrap=lang_free_v1"],
                 args.timeout, env)
    commands.append(linked)
    need(not linked.get("timeout") and linked["exit"] == 0,
         f"{case} O{opt} observer link failed: {linked}")
    observe_elf = run([args.readelf, "-l", "-d", observer_dir / "observe"],
                      60, env)
    commands.append(observe_elf)
    need(not observe_elf.get("timeout") and observe_elf["exit"] == 0,
         f"{case} O{opt} observer readelf failed: {observe_elf}")
    need("INTERP" not in observe_elf["stdout"] and
         "NEEDED" not in observe_elf["stdout"],
         f"{case} O{opt} observer is not a static freestanding ELF")

    observed = run([observer_dir / "observe"], args.timeout, env)
    commands.append(observed)
    need(not observed.get("timeout") and observed["exit"] == 0,
         f"{case} O{opt} observer execution failed: {observed}")
    need(not observed["stdout"], f"{case} O{opt} observer wrote to stdout")
    metrics = parse_events(observed["stderr"])

    need(metrics["live_vecs_at_exit"] == 0,
         f"{case} O{opt} leaked Vecs: {metrics}")
    need(metrics["final_live_payload_bytes"] == 0,
         f"{case} O{opt} leaked payload: {metrics}")
    need(metrics["vec_allocations"] == metrics["vec_frees"],
         f"{case} O{opt} unbalanced Vec count: {metrics}")
    if case in PRESSURE_CASES:
        need(metrics["vec_allocations"] >= 8192,
             f"{case} O{opt} did not allocate the pressure set: {metrics}")
        need(metrics["peak_live_payload_bytes"] <= PAYLOAD_BUDGET,
             f"{case} O{opt} exceeded 2 MiB: {metrics}")

    for path in (program, directory / "cli" / "program.o",
                 directory_ir / f"O{opt}.nir",
                 directory_ir / f"O{opt}.before.ll",
                 directory_ir / f"O{opt}.after.ll",
                 directory_ir / f"O{opt}.o",
                 observer_dir / "observe"):
        if path.is_file():
            hashes[str(path.relative_to(args.out))] = digest(path)

    return {
        "status": "passed",
        "exit": executed["exit"],
        "metrics": metrics,
        "llvm_checks": checks,
        "program_sha256": digest(program),
        "observer_sha256": digest(observer_dir / "observe"),
        "before_llvm_sha256": digest(directory_ir / f"O{opt}.before.ll"),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--readelf", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--libxml2-lib", type=Path, default=None)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    args.root = args.root.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    if args.libxml2_lib:
        previous = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = str(args.libxml2_lib) + (
            ":" + previous if previous else "")

    commands = []
    hashes = {}
    summary = {
        "schema": "woozy/native-language-gc-source/1",
        "status": "failed",
        "scope": "real Wz Source Native O0/O2 automatic cycle collection",
        "cases": {},
        "limits": [
            "Single-thread pressure only; no concurrent collector contention.",
            "Only Vec descriptors and closure environments are heap graph nodes; "
            "Native structs/enums are inline and traced through their containing Vec.",
            "A loop-exit collection is an implementation policy at a stable "
            "safepoint; source has no collect/GC API.",
        ],
        "hashes": hashes,
    }
    try:
        for case in CASES:
            summary["cases"][case] = {"O0": {}, "O2": {}}
            for opt in (0, 2):
                summary["cases"][case][f"O{opt}"] = build_and_check(
                    case, opt, args, env, commands, hashes)
            summary["cases"][case]["status"] = "passed"
        for path in (
            args.root / "runtime/native/gc_program.hpp",
            args.root / "runtime/native/gc_program.cpp",
            args.root / "runtime/native/gc.cpp",
            args.root / "runtime/native/gc_world.cpp",
            args.root / "runtime/native/gc_runtime.cpp",
            args.root / "src/backend/native/emit.cpp",
            args.root / "src/backend/native/gc_ops.inc",
            HERE / "observe_gc.cpp",
        ):
            hashes[str(path.relative_to(args.root))] = digest(path)
        for fixture in sorted((HERE / "fixtures").iterdir()):
            for path in sorted(fixture.glob("src/*.wz")):
                hashes[str(path.relative_to(args.root))] = digest(path)
        summary["status"] = "passed"
        summary["commands"] = commands
        return 0
    except Exception as error:
        summary["error"] = str(error)
        summary["commands"] = commands
        print(f"language GC source test failed: {error}", file=sys.stderr)
        return 1
    finally:
        (args.out / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        (args.out / "commands.json").write_text(
            json.dumps(commands, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
