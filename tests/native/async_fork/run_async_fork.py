#!/usr/bin/env python3
"""Real-Source O0/O2 checks for async fork recovery.

Every case is built by the published Native CLI and run as a normal process.
The runner checks the exact parent output, the process exit status, and the
absence of a runtime Panic marker. Child processes created by the fixtures
either exit with 0 or report failures through the parent's exit status.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
OBSERVER = "tests/native/language_gc_source/observe_gc.cpp"
CHILD_LOG = Path("/tmp/woozy-async-fork-observer-child.log")
CASES = {
    "fork-same-thread-scalar":
        "NoGraph scalar Task before/after a same-thread fork",
    "fork-inside-coroutine-defer":
        "fork inside a coroutine body keeps g_current and runs defer once",
    "fork-foreign-executor-throws":
        "Source fork returns os.Error when another thread owns the executor",
    "spawn-foreign-executor":
        "exec-only spawn and exec failure work with a foreign executor",
    "fork-fd-child-cancel":
        "child cancels an inherited FD wait without touching the parent epoll",
    "fork-fd-both-continue":
        "parent and child complete the inherited FD wait on private epolls",
    "fork-observer-balance":
        "observer allocation/Vec balance for parent and child separately",
}
OBSERVED = "fork-observer-balance"


def need(value, message):
    if not value:
        raise RuntimeError(message)


def invoke(argv, timeout, env):
    try:
        result = subprocess.run([str(x) for x in argv], capture_output=True,
                                timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"timeout: {argv}")
    return {
        "argv": [str(x) for x in argv],
        "exit": result.returncode,
        "stdout": result.stdout.decode("utf-8", "backslashreplace"),
        "stderr": result.stderr.decode("utf-8", "backslashreplace"),
    }


def check_observer(case, opt, manifest, directory, args, env, base):
    ir = directory / "observer-ir"
    ir.mkdir(exist_ok=True)
    emit = invoke([
        args.helper, "--project", manifest, "--opt", str(opt),
        "--out-object", ir / "program.o",
        "--out-llvm", ir / "after.ll",
        "--out-llvm-before", ir / "before.ll",
        "--out-native", ir / "program.nir",
        "--format", "json",
    ], args.timeout, env)
    need(emit["exit"] == 0, f"{case} O{opt} observer emit failed: {emit}")
    clang = args.toolchain / "clang"
    observe = directory / "observe"
    link = invoke([
        clang, f"-O{opt}", "-std=c++20", ir / "before.ll",
        args.root / OBSERVER, args.runtime, "-o", observe,
        "-nostdlib", "-static",
        "-Wl,--wrap=lang_alloc_v1", "-Wl,--wrap=lang_free_v1",
    ], args.timeout, env)
    need(link["exit"] == 0, f"{case} O{opt} observer link failed: {link}")
    if CHILD_LOG.exists():
        CHILD_LOG.unlink()
    run = invoke([observe], args.timeout, env)
    need(run["exit"] == 0, f"{case} O{opt} observer run failed: {run}")
    need(run["stdout"] == "parent-ok\n",
         f"{case} O{opt} observer stdout mismatch: {run['stdout']!r}")
    need(CHILD_LOG.is_file(), f"{case} O{opt} child observer log missing")
    parent_text = run["stderr"]
    child_text = CHILD_LOG.read_text()
    config = [line for line in parent_text.splitlines()
              if line.startswith("CONFIG:")]
    need(len(config) == 1, f"{case} O{opt} parent observer CONFIG missing")
    if "CONFIG:" not in child_text:
        # The child inherited the parent observer's config_emitted word, so its
        # first observed allocation has no second CONFIG line. The header ABI is
        # the same verified value; prepend it for the per-process parse.
        child_text = config[0] + "\n" + child_text
    streams = {
        "parent": base.parse_observer(parent_text),
        "child": base.parse_observer(child_text),
    }
    CHILD_LOG.unlink()
    for side, metrics in streams.items():
        need(metrics["final_live_heap_count"] == 0,
             f"{case} O{opt} {side} leaked heap: {metrics}")
        need(metrics["final_live_heap_bytes"] == 0,
             f"{case} O{opt} {side} leaked heap bytes: {metrics}")
        need(metrics["heap_allocations"] == metrics["heap_frees"],
             f"{case} O{opt} {side} unbalanced heap: {metrics}")
        need(metrics["vec_allocations"] == metrics["vec_frees"],
             f"{case} O{opt} {side} unbalanced Vec count: {metrics}")
        need(metrics["live_vecs_at_exit"] == 0,
             f"{case} O{opt} {side} leaked Vecs: {metrics}")
        need(metrics["final_live_payload_bytes"] == 0,
             f"{case} O{opt} {side} leaked Vec payload: {metrics}")
        need(metrics["heap_allocations"] >= 2,
             f"{case} O{opt} {side} did not allocate the Task set: {metrics}")
        need(metrics["vec_allocations"] >= 1,
             f"{case} O{opt} {side} did not allocate the Vec set: {metrics}")
    return streams


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--only", action="append", choices=tuple(CASES))
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2))
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--helper", type=Path)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--toolchain", type=Path)
    args = parser.parse_args()
    observed = None
    if args.helper and args.runtime and args.toolchain:
        spec = importlib.util.spec_from_file_location(
            "async_memory_runner",
            HERE.parent / "async_memory" / "run_async_memory.py")
        observed = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(observed)
    selected = args.only or list(CASES)
    opts = args.opt or [0, 2]
    args.out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    report = {"status": "running", "cases": []}
    try:
        for case in selected:
            manifest = HERE / "fixtures" / case / "lang.toml"
            need(manifest.is_file(), f"missing fixture manifest: {manifest}")
            for opt in opts:
                directory = args.out / case / f"O{opt}"
                shutil.rmtree(directory, ignore_errors=True)
                directory.mkdir(parents=True)
                build = invoke([
                    args.cli, "build", "--project", manifest,
                    "--backend", "native", "--opt", str(opt),
                    "--out-dir", directory, "--format", "json",
                ], args.timeout, env)
                need(build["exit"] == 0, f"{case} O{opt} build failed: {build}")
                payload = json.loads(build["stdout"])
                need(payload.get("status") == "ok",
                     f"{case} O{opt} rejected: {payload}")
                need(not any(d.get("code") == "E_CAPABILITY_UNAVAILABLE"
                             for d in payload.get("diagnostics", [])),
                     f"{case} O{opt} silently fell back")
                program = directory / "program"
                need(program.is_file(), f"{case} O{opt} emitted no program")
                run = invoke([program], args.timeout, env)
                need(run["exit"] == 0,
                     f"{case} O{opt} exit {run['exit']}: {run}")
                need(run["stdout"] == "parent-ok\n",
                     f"{case} O{opt} stdout mismatch: {run['stdout']!r}")
                need("LANG_NATIVE_PANIC" not in run["stderr"] and
                     "LANG_ASYNC_FORK_REFUSED" not in run["stderr"],
                     f"{case} O{opt} panicked: {run['stderr']}")
                observer_metrics = None
                if case == OBSERVED:
                    if CHILD_LOG.exists():
                        CHILD_LOG.unlink()
                if case == OBSERVED and observed is not None:
                    observer_metrics = check_observer(
                        case, opt, manifest, directory, args, env, observed)
                report["cases"].append({
                    "id": case, "opt": opt, "purpose": CASES[case],
                    "exit": run["exit"], "stdout": run["stdout"],
                    "observer": observer_metrics,
                })
        report["status"] = "passed"
    finally:
        (args.out / "async-fork-results.json").write_text(
            json.dumps(report, ensure_ascii=True, indent=2) + "\n")
    print(json.dumps({"status": report["status"],
                      "cases": len(report["cases"])}))


if __name__ == "__main__":
    main()
