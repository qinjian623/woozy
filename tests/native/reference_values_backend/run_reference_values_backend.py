#!/usr/bin/env python3
"""Backend-only ABI checks for Function values.

These are real no-capture source programs plus private checks of emitted LLVM.
Capturing source integration is intentionally not claimed: the shared frontend
has not been accepted, so no case here constructs an environment row.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


def run(argv, timeout=120):
    result = subprocess.run([str(value) for value in argv],
                            capture_output=True, text=True, timeout=timeout)
    return {
        "argv": [str(value) for value in argv],
        "exit": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    }


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def emit(helper, manifest, out, ident, opt):
    directory = out / ident / f"O{opt}"
    directory.mkdir(parents=True, exist_ok=True)
    built = run([helper, "--project", manifest, "--opt", str(opt),
                 "--out-dir", directory / "cli", "--format", "json"], 180)
    need(built["exit"] == 0, f"{ident} O{opt} build: {built}")
    need(json.loads(built["stdout"]).get("status") == "ok",
         f"{ident} O{opt} status: {built}")
    program = directory / "cli/program"
    executed = run([program], 60)
    need(executed["exit"] == 0 and not executed["stdout"] and
         not executed["stderr"], f"{ident} O{opt} execute: {executed}")

    inspected = run([helper, "--project", manifest, "--opt", str(opt),
                     "--out-object", directory / "program.o",
                     "--out-llvm-before", directory / "before.ll",
                     "--format", "json"], 180)
    need(inspected["exit"] == 0, f"{ident} O{opt} object: {inspected}")
    return (directory / "before.ll").read_text(encoding="utf-8")


def function_abi_checks(text, opt):
    need("define internal { ptr, ptr } @lang.fn." in text,
         f"O{opt}: a function value is not the 16-byte LangFn type")
    need(re.search(r"define internal \{ ptr, ptr \} @lang\.thunk\.\d+\(ptr ",
                   text) is not None,
         f"O{opt}: noncapturing thunk is not env-first")
    thunks = re.findall(r"define internal .*? @lang\.thunk\.(\d+)\(", text)
    need(len(thunks) == len(set(thunks)), f"O{opt}: duplicate thunk symbols")

    # The same noncapturing function used as a value more than once must share
    # one cached thunk (the fixture uses inc and fail several times).
    references = [text.count(f"ptr @lang.thunk.{symbol}")
                  for symbol in set(re.findall(r"@lang\.thunk\.(\d+)", text))]
    need(any(count >= 2 for count in references),
         f"O{opt}: no noncapturing thunk was reused")

    # Direct ordinary call.
    need(re.search(r"call i32 @lang\.fn\.\d+\(i32 0\)", text) is not None,
         f"O{opt}: direct call is not using the original signature")
    # Direct throwing call: original signature plus value/error outputs.
    need(re.search(r"call i1 @lang\.fn\.\d+\(i32 3, ptr %[^,]+, ptr %[^)]+\)",
                   text) is not None,
         f"O{opt}: direct try_call is not using the original signature")
    # Indirect ordinary call: env-first callee with code/env split.
    need(re.search(r"extractvalue \{ ptr, ptr \} %\d+, 0", text) is not None
         and re.search(r"extractvalue \{ ptr, ptr \} %\d+, 1", text) is not None,
         f"O{opt}: indirect callee was not split into code and env")
    need(re.search(r"call i32 %\d+\(ptr %\d+, i32 1\)", text) is not None,
         f"O{opt}: indirect call is not env-first")
    # Indirect throwing call: env first, then formals, then outputs.
    need(re.search(r"call i1 %\d+\(ptr %\d+, i32 4, ptr %[^,]+, ptr %[^)]+\)",
                   text) is not None,
         f"O{opt}: indirect try_call is not env-first")
    # Function value copy/drop must retain/release a non-null environment.
    fn_ops = re.findall(
        r"define internal void @lang\.(?:copy|drop)\.\d+\(ptr %0\) \{(.*?)\n\}",
        text, re.S)
    need(any("extractvalue { ptr, ptr }" in body and
             "lang_vec_retain_v1" in body for body in fn_ops),
         f"O{opt}: Function copy does not retain the environment")
    need(any("extractvalue { ptr, ptr }" in body and
             "lang_vec_release_v1" in body for body in fn_ops),
         f"O{opt}: Function drop does not release the environment")


def thread_checks(text, opt):
    need(re.search(r"\{ \{ ptr, ptr \}, i64 \}", text) is not None,
         f"O{opt}: thread context does not start with LangFn then A")
    entry = re.search(
        r"define internal i64 @lang\.thread\.entry\(ptr %0\) \{(.*?)\n\}",
        text, re.S)
    need(entry is not None, f"O{opt}: missing thread entry")
    body = entry.group(1)
    need("extractvalue { ptr, ptr }" in body and
         re.search(r"call i64 %\d+\(ptr %\d+, i64 %\d+\)", body) is not None,
         f"O{opt}: thread entry does not call code(env, A)")
    need("@lang.thread.copy" in text and "@lang.thread.drop" in text,
         f"O{opt}: thread context has no unconditional copy/drop")
    for name in ("copy", "drop"):
        body = re.search(
            rf"define internal void @lang\.thread\.{name}\(ptr %0\) \{{(.*?)\n\}}",
            text, re.S)
        need(body is not None, f"O{opt}: missing lang.thread.{name}")
        need(len(re.findall(r"call void @lang\.(?:copy|drop)\.\d+", body.group(1))) == 2,
             f"O{opt}: lang.thread.{name} does not handle both LangFn and A")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    summary = {"status": "failed", "checks": [], "not_run": []}
    try:
        function_manifest = (args.root / "tests/native/reference_values_backend"
                             "/fixtures/function-abi/lang.toml")
        thread_manifest = args.root / "tests/native/concurrency/basic/lang.toml"
        for opt in (0, 2):
            text = emit(args.helper, function_manifest, args.out,
                        "function-abi", opt)
            function_abi_checks(text, opt)
            summary["checks"].append(
                {"id": "function-abi", "opt": opt, "status": "passed"})
            text = emit(args.helper, thread_manifest, args.out,
                        "thread-basic", opt)
            thread_checks(text, opt)
            summary["checks"].append(
                {"id": "thread-basic", "opt": opt, "status": "passed"})
        summary["not_run"] = [
            "capturing source environment construction: shared frontend not "
            "accepted in this operation",
            "Weak source operations: SystemOperation schema not published",
        ]
        summary["status"] = "passed"
    except Exception as error:
        summary["error"] = str(error)
    (args.out / "report.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if summary["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
