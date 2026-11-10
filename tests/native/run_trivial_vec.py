#!/usr/bin/env python3
"""Check null value-ops for trivial Vec elements and owning nested Vecs."""
from __future__ import annotations

import argparse
import json
import os
import re
import resource
import signal
import subprocess
import sys
import time
from pathlib import Path

TARGET_FLAGS = (
    "--target=x86_64-unknown-linux-gnu", "-march=x86-64", "-mtune=generic",
    "-fno-fast-math", "-ffp-contract=off", "-fno-lto",
)


def child_setup():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(argv, timeout, log):
    started = time.time()
    process = subprocess.Popen(
        [str(value) for value in argv], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, start_new_session=True,
        preexec_fn=child_setup)
    try:
        out, err = process.communicate(timeout=timeout)
        status = "exited"
        code = process.returncode
    except subprocess.TimeoutExpired:
        status = "timeout"
        code = None
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        out, err = process.communicate()
    row = {"argv": [str(value) for value in argv], "status": status,
           "exit": code, "stdout": out, "stderr": err,
           "elapsed_s": round(time.time() - started, 6)}
    log.append(row)
    return row


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def parse_ir_calls(text):
    calls = re.findall(r"call ptr @lang_vec_new_v1\(([^)]*)\)", text)
    null_calls = [call for call in calls if "ptr null, ptr null" in call]
    owning = [call for call in calls
              if re.search(r"ptr (?:nonnull )?@lang\.copy\.", call)
              and re.search(r"ptr (?:nonnull )?@lang\.drop\.", call)]
    return calls, null_calls, owning


def parse_observer(stderr):
    heap = {}
    vecs = {}
    events = {index: 0 for index in range(1, 8)}
    allocations = 0
    frees = 0
    for line in stderr.splitlines():
        if line.startswith("ALLOC:"):
            pointer = line.split(":", 1)[1]
            need(pointer not in heap, f"duplicate live ALLOC {pointer}")
            heap[pointer] = True
            allocations += 1
        elif line.startswith("FREE:"):
            pointer = line.split(":", 1)[1]
            need(pointer in heap, f"unknown FREE {pointer}")
            del heap[pointer]
            frees += 1
        elif line.startswith("VEC:"):
            parts = line.split(":")
            need(len(parts) == 4, f"bad VEC line {line}")
            event = int(parts[1])
            identity = parts[2]
            need(1 <= event <= 7, f"bad VEC event {line}")
            events[event] += 1
            if event == 1:
                need(identity not in vecs, f"duplicate live VEC {identity}")
                vecs[identity] = True
            else:
                need(identity in vecs, f"VEC event on dead object {line}")
                if event == 5:
                    del vecs[identity]
        elif line == "":
            continue
        else:
            raise RuntimeError(f"unexpected observer output: {line}")
    need(not heap, f"observer heap live at end: {heap}")
    need(not vecs, f"observer Vec live at end: {vecs}")
    need(allocations == frees, f"ALLOC/FREE mismatch {allocations}/{frees}")
    need(events[1] > 0 and events[5] > 0, "no Vec create/free events")
    need(events[2] >= 2, f"nested retain events missing: {events[2]}")
    need(events[4] > 0, "no observed per-element drop event")
    return {"allocations": allocations, "frees": frees,
            "events": events}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = args.root / "tests/native/fixtures/trivial-vec/lang.toml"
    clangxx = args.clang.parent / "clang++"
    lld = args.clang.parent / "ld.lld"
    commands = []
    summary = {"status": "failed", "programs": {}, "backend": {},
               "observers": {}, "commands": commands}
    env = os.environ.copy()
    try:
        for opt in (0, 2):
            program_dir = args.out / f"O{opt}" / "cli"
            build = run([args.cli, "build", "--project", manifest,
                         "--backend", "native", "--opt", str(opt),
                         "--out-dir", program_dir, "--format", "json"],
                        180, commands)
            need(build["status"] == "exited" and build["exit"] == 0,
                 f"O{opt} build: {build}")
            build_json = json.loads(build["stdout"])
            need(build_json.get("status") == "ok", f"O{opt} status: {build_json}")
            program = program_dir / "program"
            execute = run([program], 60, commands)
            need(execute["status"] == "exited" and execute["exit"] == 0
                 and not execute["stdout"] and not execute["stderr"],
                 f"O{opt} execution: {execute}")
            summary["programs"][f"O{opt}"] = {
                "build": build, "execute": execute,
                "program": str(program),
                "program_sha256": __import__("hashlib").sha256(
                    program.read_bytes()).hexdigest()}

        helper_dir = args.out / "ir"
        helper_dir.mkdir(parents=True, exist_ok=True)
        summary["backend"] = {}
        for opt in (0, 2):
            helper = run([
                args.helper, "--project", manifest, "--opt", str(opt),
                "--out-object", helper_dir / f"O{opt}.o",
                "--out-llvm", helper_dir / f"O{opt}.after.ll",
                "--out-llvm-before", helper_dir / f"O{opt}.before.ll",
                "--out-native", helper_dir / f"O{opt}.nir", "--format", "json"],
                180, commands)
            need(helper["status"] == "exited" and helper["exit"] == 0,
                 f"O{opt} helper: {helper}")
            ir_text = (helper_dir / (f"O{opt}.before.ll" if opt == 0
                                     else f"O{opt}.after.ll")).read_text(
                encoding="utf-8")
            calls, null_calls, owning = parse_ir_calls(ir_text)
            need(len(null_calls) == 8,
                 f"O{opt}: expected 8 trivial null/null calls, got {len(null_calls)}")
            need(len(owning) == 1,
                 f"O{opt}: expected 1 owning nested Vec callback call, got {len(owning)}")
            summary["backend"][f"O{opt}"] = {
                "calls": len(calls), "null_copy_drop": len(null_calls),
                "owning_copy_drop": len(owning), "call_records": calls,
                "ir": str(helper_dir / (f"O{opt}.before.ll" if opt == 0
                                        else f"O{opt}.after.ll")),
            }

        for opt in (0, 2):
            program_object = args.out / f"O{opt}" / "cli" / "program.o"
            observer = args.out / f"O{opt}" / "observer"
            compile = run([
                clangxx, *TARGET_FLAGS, "-std=c++20", "-O2", "-I",
                args.root / "runtime/native", program_object,
                args.root / "tests/native/vec_observer.cpp",
                args.root / "tests/native/alloc_observer.cpp", args.runtime,
                "-Wl,--wrap=lang_alloc_v1", "-Wl,--wrap=lang_free_v1",
                f"--ld-path={lld}", "-o", observer], 180, commands)
            need(compile["status"] == "exited" and compile["exit"] == 0,
                 f"O{opt} observer build: {compile}")
            observed = run([observer], 60, commands)
            need(observed["status"] == "exited" and observed["exit"] == 0,
                 f"O{opt} observer execution: {observed}")
            need(not observed["stdout"], f"O{opt} observer stdout")
            summary["observers"][f"O{opt}"] = parse_observer(observed["stderr"])
        summary["status"] = "passed"
        return 0
    except Exception as error:
        summary["error"] = str(error)
        print(f"trivial vec test failed: {error}", file=sys.stderr)
        return 1
    finally:
        (args.out / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        (args.out / "commands.json").write_text(
            json.dumps(commands, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
