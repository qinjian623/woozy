#!/usr/bin/env python3
"""A completed hosted Thread in a real closure cycle must be reaped after GC."""
import argparse
import importlib.util
import json
import os
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "async_memory", HERE.parent / "async_memory/run_async_memory.py")
BASE = importlib.util.module_from_spec(spec)
spec.loader.exec_module(BASE)


def main():
    parser = argparse.ArgumentParser()
    for name in ("cli", "helper", "runtime", "toolchain", "root", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    commands, cases = [], []
    manifest = HERE / "fixtures/thread-cycle/lang.toml"
    env = os.environ.copy()
    for opt in (0, 2):
        directory = args.out / f"O{opt}"
        directory.mkdir()
        try:
            def run(argv):
                result = BASE.invoke(argv, 120, env)
                commands.append(result)
                BASE.need(not result.get("timeout") and result["exit"] == 0,
                          f"command failed: {result}")
                return result

            run([args.cli, "build", "--project", manifest, "--backend", "native",
                 "--opt", str(opt), "--out-dir", directory / "cli"])
            result = run([directory / "cli/program"])
            BASE.need(result["stdout"] == result["stderr"] == "",
                      "unexpected unobserved program output")
            run([args.helper, "--project", manifest, "--opt", str(opt),
                 "--out-object", directory / "program.o"])
            wraps = ("lang_alloc_v1", "lang_free_v1", "pthread_create",
                     "lang_thread_result_v1", "pthread_join",
                     "pthread_cond_destroy", "pthread_mutex_destroy")
            run([args.toolchain / "clang++", "-std=c++20", f"-O{opt}",
                 directory / "program.o", HERE / "observe_thread_cleanup.cpp",
                 args.root / "tests/native/language_gc_source/observe_gc.cpp",
                 args.runtime, "-pthread", *["-Wl,--wrap=" + x for x in wraps],
                 "-o", directory / "observed"])
            result = run([directory / "observed"])
            BASE.need(result["stdout"] == "", "unexpected observed stdout")
            lines = result["stderr"].splitlines()
            reports = [line for line in lines if line.startswith("THREAD_CLEANUP:")]
            BASE.need(reports == ["THREAD_CLEANUP:1:1:1"], reports)
            metrics = BASE.parse_observer("\n".join(
                line for line in lines if not line.startswith("THREAD_CLEANUP:")))
            BASE.need(metrics["final_live_heap_count"] == 0 and
                      metrics["final_live_payload_bytes"] == 0 and
                      metrics["heap_allocations"] == metrics["heap_frees"] and
                      metrics["vec_allocations"] == metrics["vec_frees"], metrics)
            cases.append({"opt": opt, "status": "passed", "metrics": metrics})
        except Exception as error:
            cases.append({"opt": opt, "status": "failed", "error": str(error)})
    passed = sum(case["status"] == "passed" for case in cases)
    report = {"passed": passed, "failed": len(cases) - passed,
              "cases": cases, "commands": commands}
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": passed, "failed": len(cases) - passed}))
    return int(passed != len(cases))


if __name__ == "__main__":
    raise SystemExit(main())
