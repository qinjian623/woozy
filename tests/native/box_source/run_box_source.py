#!/usr/bin/env python3
"""Verify the real std/box source API through the Native and Python backends.

The runner builds every positive fixture with the real CLI.  Native builds also
run the real helper, inspect the emitted NativeIR/LLVM, execute the produced
program, and link the existing allocation/Vec observer for single-threaded
fixtures.  Python builds execute the generated program.  Negative fixtures must
be rejected before any program is produced.  The Python collector case imports
the generated program with CPython collection disabled and asks only the
language collector to reclaim the Box cycle.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import resource
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

TARGET_FLAGS = (
    "--target=x86_64-unknown-linux-gnu", "-march=x86-64", "-mtune=generic",
    "-fno-fast-math", "-ffp-contract=off", "-fno-lto",
)
SHARED = ("core", "cleanup", "dropcount")
NATIVE_ONLY = ("mutex",)
OBSERVED = ("core", "cleanup", "dropcount")
NEGATIVE = {
    "private-field": "E_PRIVATE_ACCESS",
    "private-constructor": "E_PRIVATE_ACCESS",
    "no-extra-api": "E_NAME_NOT_FOUND",
}
PYTHON_GC = "python-cycle"
REQUIRED_IR = ("lang_vec_new_v1",)
OWNING_IR = ("lang_vec_at_v1", "lang_vec_set_v1")


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


def fresh(path):
    shutil.rmtree(path, ignore_errors=True)


def parse_observer(stderr):
    heap, vecs = {}, {}
    events = {index: 0 for index in range(1, 8)}
    peak_live_vecs = 0
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
                peak_live_vecs = max(peak_live_vecs, len(vecs))
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
    return {"allocations": allocations, "frees": frees, "events": events,
            "peak_live_vecs": peak_live_vecs}


def parse_negative(result):
    try:
        payload = json.loads(result["stdout"])
    except json.JSONDecodeError as error:
        raise RuntimeError(f"negative result is not JSON: {error}: {result}")
    need(payload.get("status") == "error", f"negative build accepted: {payload}")
    return [d.get("code") for d in payload.get("diagnostics", [])]


def fixture(root, name):
    return root / "tests/native/box_source/fixtures" / name


def write_manifest(root, out, name, backend):
    source = fixture(root, name)
    project = out / "projects" / backend / name
    project.mkdir(parents=True, exist_ok=True)
    (project / "src").mkdir(parents=True, exist_ok=True)
    (project / "src/main.wz").write_text(
        (source / "src/main.wz").read_text(encoding="utf-8"),
        encoding="utf-8")
    (project / "lang.toml").write_text(
        'schema_version=1\n[package]\nname="box"\n'
        'source_root="src"\nentry="app"\n[build]\n'
        f'backend="{backend}"\nword_bits=64\n', encoding="utf-8")
    return project / "lang.toml"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--lld", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--python-helper", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--ld-library-path", default="")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    clangxx = args.clang.parent / "clang++"
    commands = []
    hashes = {}
    summary = {"status": "failed", "native": {}, "python": {}, "negative": {},
               "python_gc": {}, "hashes": hashes, "commands": commands}
    env = os.environ.copy()
    if args.ld_library_path:
        env["LD_LIBRARY_PATH"] = (args.ld_library_path + ":" +
                                  env.get("LD_LIBRARY_PATH", ""))
    pyenv = dict(env, PYTHONDONTWRITEBYTECODE="1", PYTHONNOUSERSITE="1")
    pyenv.pop("PYTHONPATH", None)
    try:
        module = args.root / "stdlib/box/box.wz"
        need(module.is_file(), f"missing module: {module}")
        hashes[str(module.relative_to(args.root))] = sha256(module)
        for path in sorted((args.root / "stdlib/box").iterdir()):
            if path.is_file():
                hashes[str(path.relative_to(args.root))] = sha256(path)
        for name in (*SHARED, *NATIVE_ONLY, *NEGATIVE, PYTHON_GC):
            for path in sorted(fixture(args.root, name).glob("src/*.wz")):
                hashes[str(path.relative_to(args.root))] = sha256(path)
        for path in (args.cli, args.helper, args.python_helper, args.runtime):
            hashes[str(path)] = sha256(path)

        # Native positives: real CLI, helper, execution, and static ELF check.
        for name in (*SHARED, *NATIVE_ONLY):
            summary["native"][name] = {}
            ir_all = ""
            for opt in (0, 2):
                out = args.out / "native" / name / f"O{opt}"
                fresh(out)
                manifest = write_manifest(args.root, args.out, name, "native")
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
                need("lang_box_" not in nir and "lang_box_" not in llvm,
                     f"{name} O{opt} invented a Box runtime ABI")
                ir_all += nir + llvm
                for artifact in ("program", "program.o"):
                    path = out / "cli" / artifact
                    hashes[str(path.relative_to(args.out))] = sha256(path)
                for artifact in (f"O{opt}.nir", f"O{opt}.before.ll",
                                 f"O{opt}.after.ll", f"O{opt}.o"):
                    path = out / "ir" / artifact
                    hashes[str(path.relative_to(args.out))] = sha256(path)
                summary["native"][name][f"O{opt}"] = {
                    "exit": execute["exit"],
                    "program": str(program),
                    "program_sha256": sha256(program),
                    "nir": str(out / "ir" / f"O{opt}.nir"),
                }
            for symbol in REQUIRED_IR:
                need(symbol in ir_all,
                     f"{name} IR lacks real Vec operation {symbol}")
            need("vec_read" in ir_all and "vec_set" in ir_all,
                 f"{name} IR lacks the checked Vec read/set operations")
            if name in ("core", "dropcount"):
                for symbol in OWNING_IR:
                    need(symbol in ir_all,
                         f"{name} IR lacks owned-element Vec operation {symbol}")

        # Real allocation/Vec observer for single-threaded Native fixtures.
        for name in OBSERVED:
            summary["native"][name]["observer"] = {}
            for opt in (0, 2):
                out = args.out / "native" / name / f"O{opt}"
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
                if name == "dropcount":
                    need(record["peak_live_vecs"] <= 16 and
                         record["peak_live_vecs"] * 4 <= record["events"][1],
                         f"{name} O{opt} old elements stayed live: {record}")
                summary["native"][name]["observer"][f"O{opt}"] = record

        # Python positives through the real CLI and generated program.
        for name in SHARED:
            out = args.out / "python" / name
            fresh(out)
            manifest = write_manifest(args.root, args.out, name, "python")
            build = run([args.cli, "build", "--project", manifest,
                         "--backend", "python", "--out-dir", out / "cli",
                         "--format", "json"], args.timeout, commands, env)
            need(build["status"] == "exited" and build["exit"] == 0,
                 f"python {name} build: {build}")
            payload = json.loads(build["stdout"])
            need(payload.get("status") == "ok", f"python {name}: {payload}")
            generated = out / "cli"
            for artifact in ("program.py", "langrt.py"):
                need((generated / artifact).is_file(),
                     f"python {name} missing {artifact}")
                hashes[str((generated / artifact).relative_to(args.out))] = \
                    sha256(generated / artifact)
            execute = run([args.python, "-s", generated / "program.py"], 60,
                          commands, pyenv)
            need(execute["status"] == "exited" and execute["exit"] == 0
                 and not execute["stdout"] and not execute["stderr"],
                 f"python {name} execution: {execute}")
            summary["python"][name] = {
                "exit": execute["exit"], "generated": str(generated),
                "program_sha256": sha256(generated / "program.py"),
            }

        # Native and Python both reject private fields and unknown API names.
        for name, code in NEGATIVE.items():
            summary["negative"][name] = {}
            for backend in ("native", "python"):
                out = args.out / "negative" / name / backend
                fresh(out)
                manifest = write_manifest(args.root, args.out, name, backend)
                argv = [args.cli, "build", "--project", manifest,
                        "--backend", backend]
                if backend == "native":
                    argv += ["--opt", "0"]
                argv += ["--out-dir", out, "--format", "json"]
                result = run(argv, args.timeout, commands, env)
                need(result["status"] == "exited" and result["exit"] != 0,
                     f"negative {name}/{backend} unexpectedly succeeded: {result}")
                codes = parse_negative(result)
                need(code in codes,
                     f"negative {name}/{backend} expected {code}, got {codes}")
                need(not (out / "program").exists() and
                     not (out / "program.py").exists(),
                     f"negative {name}/{backend} produced a program")
                summary["negative"][name][backend] = {
                    "codes": codes, "backend": backend}

        # Python pure-Woozy cycle with Box, host collection disabled.  The CLI
        # build proves the public driver path; the Python helper supplies the
        # source-to-generated symbol metadata used by the observer.
        out = args.out / "python" / PYTHON_GC
        fresh(out)
        manifest = write_manifest(args.root, args.out, PYTHON_GC, "python")
        build = run([args.cli, "build", "--project", manifest,
                     "--backend", "python", "--out-dir", out / "cli",
                     "--format", "json"], args.timeout, commands, env)
        need(build["status"] == "exited" and build["exit"] == 0,
             f"python cycle CLI build: {build}")
        need(json.loads(build["stdout"]).get("status") == "ok",
             "python cycle CLI build rejected")
        helper = run([args.python_helper, "--project", manifest,
                      "--out-dir", out / "helper"], args.timeout, commands,
                     env)
        need(helper["status"] == "exited" and helper["exit"] == 0,
             f"python cycle helper build: {helper}")
        need(json.loads(helper["stdout"]).get("status") == "ok",
             "python cycle helper build rejected")
        generated = out / "helper"
        for artifact in ("program.py", "langrt.py", "metadata.json"):
            hashes[str((generated / artifact).relative_to(args.out))] = \
                sha256(generated / artifact)
        observe = run([args.python, "-s",
                       args.root / "tests/native/box_source/observe_box_cycle.py",
                       generated], 60, commands, pyenv)
        need(observe["status"] == "exited" and observe["exit"] == 0,
             f"python cycle observer: {observe}")
        summary["python_gc"] = json.loads(observe["stdout"].splitlines()[-1])
        summary["python_gc"]["cli_build_stdout"] = build["stdout"].strip()
        summary["python_gc"]["helper_build_stdout"] = helper["stdout"].strip()

        summary["status"] = "passed"
        return 0
    except Exception as error:
        summary["error"] = str(error)
        print(f"box source test failed: {error}", file=sys.stderr)
        return 1
    finally:
        (args.out / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        (args.out / "commands.json").write_text(
            json.dumps(commands, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
