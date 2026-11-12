#!/usr/bin/env python3
"""Build, verify, execute and observe the real Wz Slice[T] source interface.

Every positive fixture is compiled by the Native backend at O0 and O2, run, and
lowered through the compiler's own Native IR verifier. An allocation/Vec
observer is linked separately to prove copy/drop and pin balance. Panic and
negative fixtures assert the documented terminal failure or diagnostic.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
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
POSITIVE = ("core", "owned", "cleanup", "throws-return", "after-drop")
PANIC = {
    "structural-push": "active-iteration",
    "structural-pop": "active-iteration",
    "structural-clear": "active-iteration",
    "structural-resize": "active-iteration",
    "structural-reserve": "active-iteration",
    "empty-still-pins": "active-iteration",
    "negative-index": "vec-index",
    "huge-index": "vec-index",
    "bounds-order": "vec-slice",
    "bounds-high": "vec-slice",
    "sub-bounds": "vec-slice",
}
# The former "python-slice" entry is now positive Python coverage in
# tests/python/slice_source/fixtures/python-slice. Its source is retained here
# so this Native fixture set continues to pin the same program text.
NEGATIVE = {
    "type-mismatch": ("native", "E_TYPE_MISMATCH"),
    "wrong-arity": ("native", "E_TYPE_MISMATCH"),
    "wrong-len": ("native", "E_TYPE_MISMATCH"),
}


def child_setup():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(argv, timeout, log, env=None):
    started = time.time()
    process = subprocess.Popen(
        [str(value) for value in argv], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, start_new_session=True,
        preexec_fn=child_setup, env=env)
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


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_observer(stderr):
    heap, vecs = {}, {}
    events = {index: 0 for index in range(1, 8)}
    allocations = frees = 0
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
        elif line:
            raise RuntimeError(f"unexpected observer output: {line}")
    need(not heap, f"observer heap live at end: {heap}")
    need(not vecs, f"observer Vec live at end: {vecs}")
    need(allocations == frees,
         f"ALLOC/FREE mismatch {allocations}/{frees}")
    return {"allocations": allocations, "frees": frees, "events": events}


def parse_negative(result):
    try:
        payload = json.loads(result["stdout"])
    except json.JSONDecodeError as error:
        raise RuntimeError(f"negative result is not JSON: {error}: {result}")
    need(payload.get("status") == "error", f"negative build accepted: {payload}")
    return [d.get("code") for d in payload.get("diagnostics", [])]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--lld", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--ld-library-path", default="")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    fixtures = args.root / "tests/native/slice_source/fixtures"
    clangxx = args.clang.parent / "clang++"
    commands = []
    hashes = {}
    summary = {"status": "failed", "positive": {}, "panics": {},
               "negative": {}, "hashes": hashes, "commands": commands}
    env = os.environ.copy()
    if args.ld_library_path:
        env["LD_LIBRARY_PATH"] = (args.ld_library_path + ":" +
                                  env.get("LD_LIBRARY_PATH", ""))
    try:
        for name in POSITIVE:
            manifest = fixtures / name / "lang.toml"
            summary["positive"][name] = {}
            ir_text_all = ""
            for opt in (0, 2):
                out = args.out / name / f"O{opt}"
                build = run([args.cli, "build", "--project", manifest,
                             "--backend", "native", "--opt", str(opt),
                             "--out-dir", out / "cli", "--format", "json"],
                            args.timeout, commands, env)
                need(build["status"] == "exited" and build["exit"] == 0,
                     f"{name} O{opt} build: {build}")
                payload = json.loads(build["stdout"])
                need(payload.get("status") == "ok", f"{name} O{opt}: {payload}")
                need(not any(d.get("code") == "E_CAPABILITY_UNAVAILABLE"
                             for d in payload.get("diagnostics", [])),
                     f"{name} O{opt} capability fallback")
                program = out / "cli" / "program"
                execute = run([program], 60, commands, env)
                need(execute["status"] == "exited" and execute["exit"] == 0
                     and not execute["stdout"] and not execute["stderr"],
                     f"{name} O{opt} execution: {execute}")
                dyn = run(["readelf", "-d", program], 30, commands, env)
                need(dyn["status"] == "exited" and dyn["exit"] == 0,
                     f"{name} O{opt} readelf: {dyn}")
                need("(NEEDED)" not in dyn["stdout"],
                     f"{name} O{opt} has a dynamic interpreter dependency")

                (out / "ir").mkdir(parents=True, exist_ok=True)
                helper = run([
                    args.helper, "--project", manifest, "--opt", str(opt),
                    "--out-object", out / "ir" / f"O{opt}.o",
                    "--out-llvm", out / "ir" / f"O{opt}.after.ll",
                    "--out-llvm-before", out / "ir" / f"O{opt}.before.ll",
                    "--out-native", out / "ir" / f"O{opt}.nir",
                    "--format", "json"], args.timeout, commands, env)
                need(helper["status"] == "exited" and helper["exit"] == 0,
                     f"{name} O{opt} helper: {helper}")
                need(json.loads(helper["stdout"]).get("status") == "passed",
                     f"{name} O{opt} helper rejected verified IR")
                nir = (out / "ir" / f"O{opt}.nir").read_text(encoding="utf-8")
                llvm = (out / "ir" / f"O{opt}.after.ll").read_text(
                    encoding="utf-8")
                need("interp" not in nir.lower() and
                     "interp" not in llvm.lower(),
                     f"{name} O{opt} contains an interpreter marker")
                ir_text_all += nir + llvm
                for artifact in ("program", "program.o"):
                    path = out / "cli" / artifact
                    hashes[str(path.relative_to(args.out))] = sha256(path)
                for artifact in (f"O{opt}.nir", f"O{opt}.before.ll",
                                 f"O{opt}.after.ll", f"O{opt}.o"):
                    path = out / "ir" / artifact
                    hashes[str(path.relative_to(args.out))] = sha256(path)
                summary["positive"][name][f"O{opt}"] = {
                    "exit": execute["exit"],
                    "program": str(program),
                    "program_sha256": sha256(program),
                    "nir": str(out / "ir" / f"O{opt}.nir"),
                }

            required = ["lang_slice_new_v1", "lang_slice_length_v1",
                        "lang_slice_copy_v1", "lang_slice_drop_v1",
                        "lang_slice_at_v1"]
            if name == "core":
                required += ["lang_slice_sub_v1", "lang_vec_slice_v1"]
            if name == "owned":
                required += ["lang_vec_slice_v1"]
            if name == "owned":
                required += ["lang_vec_set_v1"]
            for symbol in required:
                need(symbol in ir_text_all,
                     f"{name} IR lacks real low-level call {symbol}")

            # Real allocation/Vec observer link. The observer is a separate
            # linked harness, not a source mock used by the compiler.
            observer_records = {}
            for opt in (0, 2):
                out = args.out / name / f"O{opt}"
                observer = out / "observer"
                compile = run([
                    clangxx, *TARGET_FLAGS, "-std=c++20", "-O2", "-I",
                    args.root / "runtime/native", out / "cli" / "program.o",
                    args.root / "tests/native/vec_observer.cpp",
                    args.root / "tests/native/alloc_observer.cpp", args.runtime,
                    "-Wl,--wrap=lang_alloc_v1", "-Wl,--wrap=lang_free_v1",
                    f"--ld-path={args.lld}", "-o", observer],
                    args.timeout, commands, env)
                need(compile["status"] == "exited" and compile["exit"] == 0,
                     f"{name} O{opt} observer build: {compile}")
                observed = run([observer], 60, commands, env)
                need(observed["status"] == "exited" and observed["exit"] == 0,
                     f"{name} O{opt} observer run: {observed}")
                need(not observed["stdout"], f"{name} O{opt} observer stdout")
                record = parse_observer(observed["stderr"])
                need(record["events"][1] > 0 and record["events"][5] > 0,
                     f"{name} O{opt} no Vec create/free")
                need(record["events"][2] > 0 and record["events"][3] > 0,
                     f"{name} O{opt} no Vec retain/release")
                need(record["events"][6] > 0 and record["events"][7] > 0,
                     f"{name} O{opt} no pin begin/end")
                observer_records[f"O{opt}"] = record
            summary["positive"][name]["observer"] = observer_records

        for name, cause in PANIC.items():
            manifest = fixtures / name / "lang.toml"
            summary["panics"][name] = {}
            for opt in (0, 2):
                out = args.out / "panic" / name / f"O{opt}"
                build = run([args.cli, "build", "--project", manifest,
                             "--backend", "native", "--opt", str(opt),
                             "--out-dir", out, "--format", "json"],
                            args.timeout, commands, env)
                need(build["status"] == "exited" and build["exit"] == 0,
                     f"panic {name} O{opt} build: {build}")
                program = out / "program"
                execute = run([program], 60, commands, env)
                need(execute["status"] == "exited" and execute["exit"] == 101,
                     f"panic {name} O{opt} did not terminate: {execute}")
                marker = f"LANG_NATIVE_PANIC:v1:{cause}:"
                need(marker in execute["stderr"],
                     f"panic {name} O{opt} wrong record: {execute['stderr']}")
                need(not execute["stdout"], f"panic {name} O{opt} stdout")
                summary["panics"][name][f"O{opt}"] = {
                    "exit": execute["exit"], "cause": cause,
                    "program": str(program),
                    "program_sha256": sha256(program)}

        for name, (backend, code) in NEGATIVE.items():
            manifest = fixtures / name / "lang.toml"
            out = args.out / "negative" / name
            argv = [args.cli, "build", "--project", manifest,
                    "--backend", backend]
            if backend == "native":
                argv += ["--opt", "0"]
            argv += ["--out-dir", out, "--format", "json"]
            result = run(argv, args.timeout, commands, env)
            need(result["status"] == "exited" and result["exit"] != 0,
                 f"negative {name} unexpectedly succeeded: {result}")
            codes = parse_negative(result)
            need(code in codes, f"negative {name} expected {code}, got {codes}")
            need(not (out / "program").exists(),
                 f"negative {name} produced a program")
            summary["negative"][name] = {"backend": backend, "codes": codes}

        for path in sorted((args.root / "stdlib/slice").glob("*.wz")):
            hashes[str(path.relative_to(args.root))] = sha256(path)
        for path in sorted(fixtures.glob("*/src/*.wz")):
            hashes[str(path.relative_to(args.root))] = sha256(path)
        summary["status"] = "passed"
        return 0
    except Exception as error:
        summary["error"] = str(error)
        print(f"slice source test failed: {error}", file=sys.stderr)
        return 1
    finally:
        (args.out / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        (args.out / "commands.json").write_text(
            json.dumps(commands, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
