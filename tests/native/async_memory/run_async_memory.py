#!/usr/bin/env python3
"""Real-source Native async Task/frame lifetime and GC memory checks.

Every fixture is built by the published Native CLI at O0 and O2, executed, then
rebuilt as LLVM through the Native helper and linked with the existing
``tests/native/language_gc_source/observe_gc.cpp`` allocation observer.
The observer records exact lang_alloc_v1/lang_free_v1 balance and Vec events.
A case passes only when the app output matches, every Future/frame/cycle is
released, and the observer saw no missing or duplicate allocation/free.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
OBSERVER = "tests/native/language_gc_source/observe_gc.cpp"

# Expected source semantics. These are the permanent regression expectations,
# not a snapshot of the currently known-bad implementation.
CASES = (
    {
        "id": "async-param-lifetime",
        "stdout": "42\n",
        "min_heap_allocations": 2,
        "min_vec_allocations": 1,
        "purpose": "normal Vec async parameter survives caller block exit",
    },
    {
        "id": "task-unstarted",
        "stdout": "",
        "min_heap_allocations": 2,
        "min_vec_allocations": 0,
        "purpose": "never-spawned Task releases Future and untouched frame",
    },
    {
        "id": "task-completed",
        "stdout": "42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "completed Task releases Future, frame and promise value",
    },
    {
        "id": "task-typed-error",
        "stdout": "42\n42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "llvm_same_function": (("lang_vec_new_v1", "llvm.coro.begin"),),
        "purpose": "typed-error Task preserves owning payload across repeated await",
    },
    {
        "id": "task-cancel",
        "stdout": "cancel-cleanup\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 0,
        "purpose": "cancelled suspended frame runs defer and is freed",
    },
    {
        "id": "task-shutdown",
        "stdout": "shutdown-cleanup\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 0,
        "purpose": "shutdown cancels suspended frame and frees it",
    },
    {
        "id": "double-await-vec",
        "stdout": "42\n42\n",
        "min_heap_allocations": 2,
        "min_vec_allocations": 1,
        "purpose": "repeated await copies owning Vec result per ordinary rules",
    },
    {
        "id": "double-await-struct",
        "stdout": "42\n99\n",
        "min_heap_allocations": 2,
        "min_vec_allocations": 1,
        "llvm_same_function": (("lang_vec_new_v1", "llvm.coro.begin"),),
        "purpose": "struct copies scalars by field and shares owning Vec field",
    },
    {
        "id": "double-await-string",
        "stdout": "42!\n42!\n",
        "min_heap_allocations": 2,
        "min_vec_allocations": 0,
        "llvm_same_function": (
            ("lang_text_concat_v1", "llvm.coro.begin"),
        ),
        "purpose": "repeated await keeps dynamic formatted+concatenated string alive",
    },
    {
        "id": "task-live-root",
        "stdout": "42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "executor/source-held live Task is a root under GC pressure",
    },
    {
        "id": "task-vec-cycle",
        "stdout": "",
        "min_heap_allocations": 64,
        "min_vec_allocations": 64,
        "purpose": "unstarted pure Vec<Task> cycles are reclaimed",
    },
    {
        "id": "task-closure-cycle",
        "stdout": "",
        "min_heap_allocations": 64,
        "min_vec_allocations": 64,
        "purpose": "unstarted Task/closure/Vec cycles are reclaimed",
    },
)
CASE_BY_ID = {case["id"]: case for case in CASES}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def kill_process_group(process):
    """Kill the whole session started for one runner command."""
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    except OSError:
        try:
            process.kill()
        except OSError:
            pass


def invoke(argv, timeout, env):
    argv = [str(value) for value in argv]
    started = time.time()
    try:
        process = subprocess.Popen(
            argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env=env, start_new_session=True)
    except OSError as error:
        return {
            "argv": argv,
            "elapsed_s": round(time.time() - started, 6),
            "spawn_error": str(error),
            "exit": None,
            "stdout": "",
            "stderr": "",
        }
    try:
        stdout, stderr = process.communicate(timeout=timeout)
        return {
            "argv": argv,
            "elapsed_s": round(time.time() - started, 6),
            "exit": process.returncode,
            "stdout": stdout,
            "stderr": stderr,
        }
    except subprocess.TimeoutExpired:
        # The CLI may have spawned helper/compiler children. A new session per
        # command makes the process-group kill cover all descendants.
        kill_process_group(process)
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                process.kill()
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
        return {
            "argv": argv,
            "elapsed_s": round(time.time() - started, 6),
            "timeout": True,
            "exit": None,
            "stdout": stdout,
            "stderr": stderr,
        }


def parse_observer(text: str):
    """Parse CONFIG/ALLOC/FREE/VEC and reject malformed ownership streams."""
    header = None
    generation = {}
    current = {}
    records = {}
    vec_live = {}
    live_payload = 0
    peak_payload = 0
    vec_allocations = 0
    vec_frees = 0

    for line in text.splitlines():
        if line.startswith("CONFIG:"):
            need(header is None, "duplicate CONFIG")
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
            generation[pointer] = generation.get(pointer, 0) + 1
            key = (pointer, generation[pointer])
            records[key] = {
                "pointer": pointer,
                "size": size,
                "vec": False,
                "vec_freed": False,
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
            need(not record["vec"] or record["vec_freed"],
                 f"Vec heap free before VEC:5: {line!r}")
            record["freed"] = True
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
                vec_live[key] = {"length": detail, "drops": [], "iterations": 0}
                continue
            need(record["vec"], f"VEC event for non-Vec: {line!r}")
            need(key in vec_live, f"VEC event for freed identity: {line!r}")
            state = vec_live[key]
            if event == 4:
                state["drops"].append(detail)
            elif event == 5:
                need(not record["vec_freed"], f"duplicate VEC:5: {line!r}")
                record["vec_freed"] = True
                live_payload -= record["payload"]
                vec_frees += 1
                del vec_live[key]
            elif event in (6, 7):
                state["iterations"] += 1 if event == 6 else -1
                need(state["iterations"] >= 0, f"iteration underflow: {line!r}")
            elif event not in (2, 3):
                need(False, f"unknown VEC event: {line!r}")
            continue
        raise RuntimeError(f"unexpected observer line: {line!r}")

    need(header is not None, "observer emitted no CONFIG")
    live_heap = [record for record in records.values() if not record["freed"]]
    live_vecs = [record for record in records.values()
                 if record["vec"] and not record["vec_freed"]]
    return {
        "header_bytes": header,
        "heap_allocations": len(records),
        "heap_frees": len(records) - len(live_heap),
        "final_live_heap_count": len(live_heap),
        "final_live_heap_bytes": sum(record["size"] for record in live_heap),
        "vec_allocations": vec_allocations,
        "vec_frees": vec_frees,
        "live_vecs_at_exit": len(live_vecs),
        "peak_live_payload_bytes": peak_payload,
        "final_live_payload_bytes": live_payload,
    }


def wait_pid_gone(pid: int, timeout: float = 3.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        except PermissionError:
            pass
        time.sleep(0.05)
    return False


def self_test():
    """Short checks for parser rejection and timeout process-group cleanup."""
    checks = {}
    clean_text = (
        "CONFIG:vec_header_bytes:80\n"
        "ALLOC:0x1:84\n"
        "VEC:1:0x1:1\n"
        "VEC:4:0x1:0\n"
        "VEC:5:0x1:0\n"
        "FREE:0x1\n"
    )
    clean = parse_observer(clean_text)
    checks["parser_clean"] = (
        clean["final_live_heap_count"] == 0 and
        clean["heap_allocations"] == clean["heap_frees"])

    duplicate_rejected = False
    try:
        parse_observer(clean_text + "FREE:0x1\n")
    except RuntimeError:
        duplicate_rejected = True
    checks["duplicate_free_rejected"] = duplicate_rejected

    missing = parse_observer(clean_text.replace("FREE:0x1\n", ""))
    checks["missing_free_rejected"] = (
        missing["final_live_heap_count"] == 1 and
        missing["heap_allocations"] != missing["heap_frees"])

    # The shell starts a grandchild that inherits the captured stderr pipe.
    # Without a process-group kill, communicate() would wait for that child.
    result = invoke(
        ["/bin/sh", "-c", "sleep 30 & echo CHILD:$! >&2; wait $!"],
        0.4, os.environ.copy())
    checks["timeout_reported"] = bool(result.get("timeout"))
    match = re.search(r"CHILD:(\d+)", result.get("stderr", ""))
    checks["grandchild_pid_seen"] = bool(match)
    checks["grandchild_gone"] = (
        wait_pid_gone(int(match.group(1))) if match else False)
    return {
        "status": "passed" if all(checks.values()) else "failed",
        "checks": checks,
        "timeout_result": result,
    }


def llvm_function_bodies(text: str):
    bodies = {}
    current = None
    for line in text.splitlines():
        if line.startswith("define "):
            match = re.search(r"@([A-Za-z0-9_.$-]+)\(", line)
            current = match.group(1) if match else None
            if current is not None:
                bodies[current] = []
            continue
        if line == "}":
            current = None
            continue
        if current is not None:
            bodies[current].append(line)
    return bodies


def record_hash(hashes, root: Path, path: Path):
    try:
        key = str(path.relative_to(root))
    except ValueError:
        key = str(path)
    hashes[key] = digest(path)


def check_static_elf(path: Path, readelf: Path, timeout, env, commands):
    if not readelf.is_file():
        return {"status": "not_run", "reason": f"missing {readelf}"}
    result = invoke([readelf, "-l", "-d", path], min(timeout, 60), env)
    commands.append(result)
    need(not result.get("timeout") and result["exit"] == 0,
         f"readelf failed for {path}: {result}")
    need("INTERP" not in result["stdout"] and "NEEDED" not in result["stdout"],
         f"not a static freestanding ELF: {path}")
    return {"status": "passed"}


def build_and_check(case, opt, args, env, commands, hashes):
    case_id = case["id"]
    directory = args.out / "native" / case_id / f"O{opt}"
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    manifest = args.root / "tests/native/async_memory/fixtures" / case_id / "lang.toml"
    need(manifest.is_file(), f"missing fixture manifest: {manifest}")

    build = invoke([
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
    executed = invoke([program], args.timeout, env)
    commands.append(executed)
    need(not executed.get("timeout") and executed["exit"] == 0,
         f"{case_id} O{opt} execution failed: {executed}")
    need(executed["stdout"] == case["stdout"],
         f"{case_id} O{opt} stdout mismatch: {executed}")
    need(executed["stderr"] == "",
         f"{case_id} O{opt} unexpected stderr: {executed}")
    elf_check = check_static_elf(program, args.readelf, args.timeout, env, commands)

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
    need(emitted.get("status") == "passed", f"{case_id} O{opt} helper: {emitted}")
    before_llvm = ir_dir / f"O{opt}.before.ll"
    llvm_text = before_llvm.read_text(encoding="utf-8")
    bodies = llvm_function_bodies(llvm_text)
    llvm_checks = {}
    llvm_functions = {}
    for group in case.get("llvm_same_function", ()):
        key = "+".join(group)
        matches = [
            name for name, body in bodies.items()
            if all(any(symbol in line for line in body) for symbol in group)
        ]
        llvm_checks[key] = bool(matches)
        llvm_functions[key] = matches
    missing_llvm = [key for key, present in llvm_checks.items()
                    if not present]
    need(not missing_llvm,
         f"{case_id} O{opt} lacks dynamic owning payload calls in one "
         f"function: {missing_llvm}")

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

    # Correct source semantics: every allocation, including Future, frame,
    # promise and unreachable Vec/Task cycles, is released exactly once.
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
    # Avoid a vacuous pass if a compiler stops creating the object under test.
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
        "llvm_checks": llvm_checks,
        "llvm_functions": llvm_functions,
        "elf": elf_check,
        "observer_elf": observer_elf,
        "program_sha256": digest(program),
        "observer_sha256": digest(observe_dir / "observe"),
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
    parser.add_argument("--observer", type=Path, default=None,
                        help="override the existing language_gc_source observer")
    parser.add_argument("--libxml2-lib", type=Path, default=None)
    parser.add_argument("--only", action="append", choices=tuple(CASE_BY_ID))
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        result = self_test()
        print(json.dumps(result))
        return 0 if result["status"] == "passed" else 1

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
    if args.observer:
        args.observer = args.observer.resolve()
    else:
        args.observer = args.root / OBSERVER
    args.out.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    if args.libxml2_lib:
        previous = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = str(args.libxml2_lib) + (
            ":" + previous if previous else "")

    selected = set(args.only) if args.only else set(CASE_BY_ID)
    options = args.opt or [0, 2]
    commands = []
    hashes = {}
    summary = {
        "schema": "woozy/native-async-memory/1",
        "status": "failed",
        "scope": "real-source Native async Task/frame lifetime and GC",
        "cases": {},
        "limits": [
            "Each fixture is limited by the runner timeout.",
            "The observer covers lang_alloc_v1/lang_free_v1 and Vec events; "
            "source output is checked separately.",
            "A live executor/source Task is a root; unreachable unstarted "
            "Vec<Task> and Task/closure/Vec cycles are garbage.",
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

    for path in (
        args.root / "tests/native/async_memory/run_async_memory.py",
        args.observer,
        args.root / "tests/native/async_memory/fixtures",
    ):
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
