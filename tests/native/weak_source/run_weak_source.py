#!/usr/bin/env python3
"""Run the accepted Source Weak subset on the Native backend.

This runner uses only the published ``woozyc`` CLI and the isolated LLVM helper.
It executes the four planned Root ``tests/reference_values`` Weak positives
(empty, Vec lifecycle, Box lifecycle, immortal non-capturing Fn) and three
Root target-eligibility rejects verbatim, then adds real source cases for the
canonical handle projections, generic/aggregate Weak fields, alias/final-owner
liveness, and additional invalid targets.

Captured-Fn Weak values are recorded as ``unsupported`` because Source capture
is still rejected until LANG01 lands. That case is never counted as a pass.
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

BUILD_TIMEOUT = 300
RUN_TIMEOUT = 120
HELPER_TIMEOUT = 300

ROOT_POSITIVE = (
    "root-weak-empty-upgrade-none",
    "root-weak-vec-lifecycle",
    "root-weak-box-lifecycle",
    "root-weak-fn-immortal",
    "root-weak-fn-captured-dead",
)
ADDITIONAL_POSITIVE = (
    "alias-liveness",
    "canonical-handles",
    "generic-fields",
)
ROOT_REJECT = {
    "root-weak-reject-user-struct": "E_TYPE_MISMATCH",
    "root-weak-reject-string": "E_TYPE_MISMATCH",
    "root-weak-reject-file": "E_TYPE_MISMATCH",
}
ADDITIONAL_REJECT = {
    "weak-reject-slice": "E_TYPE_MISMATCH",
    "weak-reject-ipc-wrapper": "E_TYPE_MISMATCH",
    "weak-reject-fake-box": "E_TYPE_MISMATCH",
    "weak-32bit": "E_CAPABILITY_UNAVAILABLE",
}
NON_EMPTY_POSITIVE = set(ROOT_POSITIVE) - {"root-weak-empty-upgrade-none"}
VEC_SLOT_FIXTURES = {"root-weak-vec-lifecycle", "root-weak-box-lifecycle", "alias-liveness", "generic-fields"}
HANDLE_FIXTURES = {"canonical-handles"}


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


def manifest(root, name):
    return root / "tests/native/weak_source/fixtures" / name / "lang.toml"


def parse_json(result, what):
    try:
        return json.loads(result["stdout"])
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{what} is not JSON: {error}: {result}")


def positive(root, args, out, name, commands, hashes, summary):
    record = summary["native"][name] = {"O0": {}, "O2": {}}
    aggregate_llvm = ""
    aggregate_nir = ""
    for opt in (0, 2):
        directory = out / "native" / name / f"O{opt}"
        fresh(directory)
        built = run([args.cli, "build", "--project", manifest(root, name),
                     "--backend", "native", "--opt", str(opt),
                     "--out-dir", directory / "cli", "--format", "json"],
                    args.timeout, commands, args.env)
        need(built["status"] == "exited" and built["exit"] == 0,
             f"{name} O{opt} build: {built}")
        payload = parse_json(built, f"{name} O{opt} build")
        need(payload.get("status") == "ok", f"{name} O{opt}: {payload}")
        need(not any(d.get("code") == "E_CAPABILITY_UNAVAILABLE"
                     for d in payload.get("diagnostics", [])),
             f"{name} O{opt} silently reported a capability fallback")

        program = directory / "cli" / "program"
        executed = run([program], RUN_TIMEOUT, commands, args.env)
        need(executed["status"] == "exited" and executed["exit"] == 0 and
             not executed["stdout"] and not executed["stderr"],
             f"{name} O{opt} execution: {executed}")

        dynamic = run(["readelf", "-d", program], 30, commands, args.env)
        need(dynamic["status"] == "exited" and dynamic["exit"] == 0,
             f"{name} O{opt} readelf: {dynamic}")
        need("(NEEDED)" not in dynamic["stdout"],
             f"{name} O{opt} is not a static default ELF: {dynamic['stdout']}")

        ir = directory / "ir"
        ir.mkdir(parents=True, exist_ok=True)
        helped = run([args.helper, "--project", manifest(root, name),
                      "--opt", str(opt),
                      "--out-object", ir / f"O{opt}.o",
                      "--out-llvm", ir / f"O{opt}.after.ll",
                      "--out-llvm-before", ir / f"O{opt}.before.ll",
                      "--out-native", ir / f"O{opt}.nir",
                      "--format", "json"],
                     HELPER_TIMEOUT, commands, args.env)
        need(helped["status"] == "exited" and helped["exit"] == 0,
             f"{name} O{opt} helper: {helped}")
        need(parse_json(helped, f"{name} O{opt} helper").get("status") ==
             "passed", f"{name} O{opt} helper rejected verified IR")

        llvm = (ir / f"O{opt}.before.ll").read_text(encoding="utf-8")
        nir = (ir / f"O{opt}.nir").read_text(encoding="utf-8")
        aggregate_llvm += llvm
        aggregate_nir += nir
        need("lang_weak_upgrade_v1" in llvm,
             f"{name} O{opt} has no real Weak upgrade call")
        if name in NON_EMPTY_POSITIVE or name not in ROOT_POSITIVE:
            need("lang_weak_downgrade_v1" in llvm,
                 f"{name} O{opt} has no real Weak downgrade call")
        if name in VEC_SLOT_FIXTURES:
            need("{ i64, i64, i64, i64, i64, ptr, ptr, ptr, i64, ptr }" in llvm
                 and "i32 0, i32 9" in llvm,
                 f"{name} O{opt} does not use the canonical Vec Weak slot")
        if name in HANDLE_FIXTURES:
            need("{ i64, i64, ptr, ptr }" in llvm and "i32 0, i32 3" in llvm,
                 f"{name} O{opt} does not use the canonical handle Weak slot")
        # A Weak operation is the existing system_call; no new Native IR
        # opcode may appear.
        need("system_call" in nir and "system=" in nir,
             f"{name} O{opt} Weak operation is not a system_call")
        need("weak" not in nir.lower(),
             f"{name} O{opt} invented a Weak Native IR opcode")

        for artifact in ("program", "program.o"):
            path = directory / "cli" / artifact
            hashes[str(path.relative_to(out))] = sha256(path)
        for artifact in (f"O{opt}.nir", f"O{opt}.before.ll",
                         f"O{opt}.after.ll", f"O{opt}.o"):
            path = ir / artifact
            hashes[str(path.relative_to(out))] = sha256(path)
        record[f"O{opt}"] = {
            "exit": executed["exit"],
            "program_sha256": sha256(program),
            "nir_sha256": sha256(ir / f"O{opt}.nir"),
            "before_llvm_sha256": sha256(ir / f"O{opt}.before.ll"),
        }
    need("system_call" in aggregate_nir and
         "lang_weak_upgrade_v1" in aggregate_llvm,
         f"{name} did not exercise the Weak system path")


def reject(root, args, out, name, code, commands, summary):
    directory = out / "negative" / name
    fresh(directory)
    result = run([args.cli, "build", "--project", manifest(root, name),
                  "--backend", "native", "--opt", "0",
                  "--out-dir", directory / "cli", "--format", "json"],
                 args.timeout, commands, args.env)
    need(result["status"] == "exited" and result["exit"] != 0,
         f"{name} unexpectedly built: {result}")
    payload = parse_json(result, f"{name} build")
    need(payload.get("status") == "error", f"{name}: {payload}")
    codes = [d.get("code") for d in payload.get("diagnostics", [])]
    need(code in codes, f"{name} expected {code}, got {codes}")
    if code == "E_TYPE_MISMATCH":
        need(any("Weak target" in d.get("message", "")
                 for d in payload.get("diagnostics", [])),
             f"{name} did not report the explicit Weak target rule")
    need(not (directory / "cli" / "program").exists() and
         not (directory / "cli" / "program.o").exists(),
         f"{name} emitted a program despite the source error")
    summary["negative"][name] = {"codes": codes, "backend": "native"}


def unsupported_capture(root, args, out, name, commands, summary):
    directory = out / "unsupported" / name
    fresh(directory)
    result = run([args.cli, "build", "--project", manifest(root, name),
                  "--backend", "native", "--opt", "0",
                  "--out-dir", directory / "cli", "--format", "json"],
                 args.timeout, commands, args.env)
    need(result["status"] == "exited" and result["exit"] != 0,
         f"{name} must stay unsupported, not build: {result}")
    payload = parse_json(result, f"{name} build")
    codes = [d.get("code") for d in payload.get("diagnostics", [])]
    need("E_CAPABILITY_UNAVAILABLE" in codes,
         f"{name} lacks an explicit capability diagnostic: {payload}")
    need(any("capturing a local binding is unavailable" in d.get("message", "")
             for d in payload.get("diagnostics", [])),
         f"{name} did not report the source capture limit: {payload}")
    need(not (directory / "cli" / "program").exists(),
         f"{name} emitted a program")
    summary["unsupported"].append({
        "id": name,
        "status": "unsupported",
        "codes": codes,
        "reason": "Source capture is rejected until LANG01 lands; the "
                  "captured-Fn dead-encoding case is not claimed as passing.",
    })


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--ld-library-path", default="")
    parser.add_argument("--timeout", type=int, default=BUILD_TIMEOUT)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    args.env = os.environ.copy()
    for parent in (args.root, args.root.parent, args.root.parent.parent):
        library = parent / "toolchains/libxml2-legacy-2.13.9/usr/lib"
        if (library / "libxml2.so.2").exists():
            previous = args.env.get("LD_LIBRARY_PATH")
            args.env["LD_LIBRARY_PATH"] = str(library) + (":" + previous if previous else "")
            break
    if args.ld_library_path:
        args.env["LD_LIBRARY_PATH"] = (
            args.ld_library_path + ":" + args.env.get("LD_LIBRARY_PATH", ""))
    if not shutil.which("readelf"):
        print("weak source test: not_run (readelf unavailable)", file=sys.stderr)
        return 2

    commands = []
    hashes = {}
    summary = {
        "status": "failed",
        "scope": "source Weak Native O0/O2 on the published canonical targets",
        "native": {},
        "negative": {},
        "unsupported": [],
        "not_run": [
            "captured-Fn Weak dead encoding: Source capture is still rejected "
            "until LANG01; recorded as unsupported, never as passed",
            "foreign Python/C Weak target: no accepted binding fixture in this "
            "operation (pending binding owner)",
            "Source automatic GC poll/register/trace: explicitly out of this "
            "phase",
            "Python backend Weak emission/execution: separate owner",
        ],
        "hashes": hashes,
        "commands": commands,
    }
    try:
        for name in (*ROOT_POSITIVE, *ADDITIONAL_POSITIVE):
            positive(args.root, args, args.out, name, commands, hashes, summary)
            summary["native"][name]["status"] = "passed"
        for name, code in {**ROOT_REJECT, **ADDITIONAL_REJECT}.items():
            reject(args.root, args, args.out, name, code, commands, summary)
        for path in (
            args.root / "stdlib/weak/weak.wz",
            args.root / "src/backend/native/weak_ops.inc",
            args.root / "src/backend/native/emit.cpp",
        ):
            hashes[str(path.relative_to(args.root))] = sha256(path)
        for fixture in sorted(
                (args.root / "tests/native/weak_source/fixtures").iterdir()):
            for path in sorted(fixture.glob("src/*.wz")):
                hashes[str(path.relative_to(args.root))] = sha256(path)
        summary["status"] = "passed"
        return 0
    except Exception as error:
        summary["error"] = str(error)
        print(f"weak source test failed: {error}", file=sys.stderr)
        return 1
    finally:
        (args.out / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        (args.out / "commands.json").write_text(
            json.dumps(commands, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
