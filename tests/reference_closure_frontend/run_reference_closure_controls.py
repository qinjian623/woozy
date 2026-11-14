#!/usr/bin/env python3
"""Small controls for the reference-closure runner.

These are function-level controls, not the compile matrix.  They use the
runner module from this directory; a stub replaces only the "build" step.  All
program executions go through the real ``corpus._run_process_impl`` / Popen
boundary.

Controls:
1. A program without execute permission makes the first case fail with an
   ``os_error`` phase record (kind/argv/error), the second case still runs and
   passes, the final report is printed and the runner exits 1.
2. A real child that exceeds the command timeout raises ``CommandFailed``
   with kind ``timeout`` (never a pass).
3. A timed-out case is recorded failed and later cases still run.
4. The normal success path still passes when the built program is executable.
5. Only the explicit `backends: ["native"]` thread case is skipped on Python
   (from cases.json metadata), the Native path still runs, and a normal case
   with an unexpected E_CAPABILITY_UNAVAILABLE still fails rather than being
   auto-skipped.

Run from the repository root with the pinned Python 3.12; the script derives
the root from its own path and does not use any ``checks/`` path.
"""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import types

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import run_reference_closure_cases as runner  # noqa: E402

CASE_BAD = "closure-copy-at-creation-sibling"
CASE_GOOD = "closure-nested-transitive"
THREAD = "thread-capture-scalar-arg"
RV_PATH = ROOT / "tests/python/reference_values_backend/run_reference_values.py"


def load_module(name, path):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fake_program(path, executable):
    path.write_text(
        f"#!{sys.executable}\nimport sys\nsys.exit(0)\n", encoding="utf-8")
    os.chmod(path, 0o755 if executable else 0o644)


def build_stub(executable_for):
    """Stub build: publish a program and program.py; execute for real."""
    real_command = runner.command

    def stub(context, argv, timeout=runner.COMMAND_TIMEOUT):
        argv = [str(part) for part in argv]
        if "--out-dir" in argv:
            out = Path(argv[argv.index("--out-dir") + 1])
            out.mkdir(parents=True, exist_ok=True)
            fake_program(out / "program", executable_for(argv))
            (out / "program.py").write_text(
                "import sys\nsys.exit(0)\n", encoding="utf-8")
            return runner.Result(argv, 0, '{"status":"ok","diagnostics":[]}', "")
        return real_command(context, argv)

    return stub


def permission_error_continues():
    real_command = runner.command
    old_positive, old_negative = runner.POSITIVE, runner.NEGATIVE
    old_argv = sys.argv
    runner.POSITIVE = [CASE_BAD, CASE_GOOD]
    runner.NEGATIVE = {}
    runner.command = build_stub(
        lambda argv: CASE_BAD not in " ".join(argv))
    report = None
    try:
        sys.argv = ["controls", "--woozyc", sys.executable, "--root",
                    str(ROOT), "--python", sys.executable, "--opt", "0"]
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            exit_code = runner.main()
        report = json.loads(buffer.getvalue())
    finally:
        runner.POSITIVE, runner.NEGATIVE = old_positive, old_negative
        runner.command = real_command
        sys.argv = old_argv
    cases = {row["case"]: row for row in report["results"]["cases"]}
    bad = cases[CASE_BAD]
    good = cases[CASE_GOOD]
    phase = bad["phases"].get("native_execute", {})
    return {
        "passed": (
            exit_code == 1
            and report["status"] == "failed"
            and bad["status"] == "failed"
            and phase.get("kind") == "os_error"
            and phase.get("argv")
            and "Permission" in phase.get("error", "")
            and good["status"] == "passed"
            and good["phases"].get("native_execute", {}).get("exit") == 0
            and good["phases"].get("python_execute", {}).get("exit") == 0
            and len(report["results"]["failed"]) == 1
            and len(report["results"]["cases"]) == 2
        ),
        "exit": exit_code,
        "first_case": bad["status"],
        "first_phase": phase,
        "second_case": good["status"],
        "failed": report["results"]["failed"],
        "report": report,
    }


def real_timeout_not_pass():
    context = runner.Context(ROOT, sys.executable, sys.executable, 0)
    try:
        runner.command(context, [sys.executable, "-c",
                                 "import time; time.sleep(2)"], timeout=0.3)
    except runner.CommandFailed as error:
        return {"passed": error.kind == "timeout"
                and "timed out" in str(error),
                "kind": error.kind, "error": str(error)}
    return {"passed": False, "detail": "command returned without timeout"}


def timeout_keeps_previous_and_continues():
    real_command = runner.command

    def stub(_context, argv, timeout=runner.COMMAND_TIMEOUT):
        argv = [str(part) for part in argv]
        if "--out-dir" in argv:
            if "closure-move-owning-and-scalar" in " ".join(argv):
                raise runner.CommandFailed(
                    "timed out after 1s", argv, -9, kind="timeout")
            out = Path(argv[argv.index("--out-dir") + 1])
            out.mkdir(parents=True, exist_ok=True)
            fake_program(out / "program", True)
        return runner.Result(argv, 0, '{"status":"ok","diagnostics":[]}', "")

    results = {"passed": [], "not_run": [], "failed": [], "cases": []}
    context = types.SimpleNamespace(root=ROOT, cli=sys.executable,
                                    python=sys.executable, opt=0)
    runner.command = stub
    try:
        with tempfile.TemporaryDirectory(prefix="closure-controls-") as temp:
            for name in (CASE_BAD, "closure-move-owning-and-scalar", CASE_GOOD):
                runner.positive_case(name, context, results, Path(temp))
    finally:
        runner.command = real_command
    statuses = {row["case"]: row["status"] for row in results["cases"]}
    timed = results["cases"][1]["phases"].get("native_build", {})
    return {
        "passed": (statuses.get(CASE_BAD) == "passed"
                   and statuses.get("closure-move-owning-and-scalar") == "failed"
                   and statuses.get(CASE_GOOD) == "passed"
                   and len(results["failed"]) == 1
                   and timed.get("kind") == "timeout"),
        "statuses": statuses,
        "failed": results["failed"],
        "timeout_phase": timed,
    }


def normal_success():
    real_command = runner.command
    runner.command = build_stub(lambda _argv: True)
    results = {"passed": [], "not_run": [], "failed": [], "cases": []}
    context = runner.Context(ROOT, sys.executable, sys.executable, 0)
    try:
        with tempfile.TemporaryDirectory(prefix="closure-controls-") as temp:
            runner.positive_case(CASE_GOOD, context, results, Path(temp))
    finally:
        runner.command = real_command
    return {"passed": not results["failed"]
            and results["cases"][0]["status"] == "passed",
            "status": results["cases"][0]["status"],
            "failed": results["failed"]}


def native_only_scope():
    root = ROOT
    cases = {case["id"]: case for case in json.loads(
        (root / "tests/reference_values/cases.json").read_text())["cases"]}
    thread = cases[THREAD]
    rv = load_module("reference_values_runner", RV_PATH)
    scope_kind, scope_reason = rv.case_scope(thread, "python")
    normal_kind, normal_reason = rv.case_scope(cases[CASE_GOOD], "python")
    bad_kind, _ = rv.case_scope({"id": "bad", "backends": "native"}, "python")

    calls = []

    def stub(_context, argv, timeout=runner.COMMAND_TIMEOUT):
        argv = [str(part) for part in argv]
        calls.append(argv)
        if "--out-dir" in argv:
            out = Path(argv[argv.index("--out-dir") + 1])
            out.mkdir(parents=True, exist_ok=True)
            fake_program(out / "program", True)
        return runner.Result(argv, 0, '{"status":"ok","diagnostics":[]}', "")

    context = runner.Context(root, sys.executable, sys.executable, 0)
    real_metadata = (context.case_backends.get(THREAD) == ["native"]
                     and "Native-only" in context.case_notes.get(THREAD, ""))
    results = {"passed": [], "not_run": [], "failed": [], "cases": []}
    real_command = runner.command
    runner.command = stub
    try:
        with tempfile.TemporaryDirectory(prefix="closure-controls-") as temp:
            runner.positive_case(THREAD, context, results, Path(temp))
    finally:
        runner.command = real_command
    row = results["cases"][0]
    thread_calls = list(calls)
    python_thread_build = any(
        "--out-dir" in argv and "--backend" in argv
        and argv[argv.index("--backend") + 1] == "python"
        for argv in thread_calls)

    calls.clear()
    normal = {"passed": [], "not_run": [], "failed": [], "cases": []}
    runner.command = stub
    try:
        with tempfile.TemporaryDirectory(prefix="closure-controls-") as temp:
            runner.positive_case(CASE_GOOD, context, normal, Path(temp))
    finally:
        runner.command = real_command
    normal_row = normal["cases"][0]
    normal_python_build = any(
        "--out-dir" in argv and "--backend" in argv
        and argv[argv.index("--backend") + 1] == "python"
        for argv in calls)

    def cap_stub(_context, argv, timeout=runner.COMMAND_TIMEOUT):
        argv = [str(part) for part in argv]
        if "--out-dir" in argv:
            if "--backend" in argv and argv[argv.index("--backend") + 1] == "python":
                return runner.Result(argv, 1, json.dumps({
                    "status": "error",
                    "diagnostics": [{"code": "E_CAPABILITY_UNAVAILABLE",
                                     "message": "unexpected"}]}), "")
            out = Path(argv[argv.index("--out-dir") + 1])
            out.mkdir(parents=True, exist_ok=True)
            fake_program(out / "program", True)
            return runner.Result(argv, 0, '{"status":"ok","diagnostics":[]}', "")
        return runner.Result(argv, 0, "", "")

    cap_results = {"passed": [], "not_run": [], "failed": [], "cases": []}
    runner.command = cap_stub
    try:
        with tempfile.TemporaryDirectory(prefix="closure-controls-") as temp:
            runner.positive_case(CASE_GOOD, context, cap_results, Path(temp))
    finally:
        runner.command = real_command

    return {
        "passed": (
            scope_kind == "not_run" and "Native-only" in scope_reason
            and normal_kind is None
            and bad_kind == "failed"
            and thread["backends"] == ["native"]
            and real_metadata
            and row["status"] == "not_run"
            and "Native-only" in results["not_run"][0]
            and "python_build" not in row["phases"]
            and row["phases"].get("native_build", {}).get("exit") == 0
            and row["phases"].get("native_execute", {}).get("exit") == 0
            and not python_thread_build
            and not results["failed"]
            and normal_row["status"] == "passed"
            and normal_python_build
            and not normal["failed"]
            and cap_results["cases"][0]["status"] == "failed"
            and not cap_results["not_run"]
            and bool(cap_results["failed"])
        ),
        "thread_scope": [scope_kind, scope_reason],
        "real_context_metadata": real_metadata,
        "thread_row": row,
        "thread_python_build": python_thread_build,
        "normal_python_build": normal_python_build,
        "normal_status": normal_row["status"],
        "cap_status": cap_results["cases"][0]["status"],
        "cap_not_run": cap_results["not_run"],
        "cap_failed": cap_results["failed"],
    }


def main():
    checks = {
        "permission_error_continues": permission_error_continues(),
        "real_timeout_not_pass": real_timeout_not_pass(),
        "timeout_keeps_previous_and_continues":
            timeout_keeps_previous_and_continues(),
        "normal_success": normal_success(),
        "native_only_scope": native_only_scope(),
    }
    status = "ok" if all(item["passed"] for item in checks.values()) else "failed"
    print(json.dumps({"status": status,
                      "runner_sha256": __import__("hashlib").sha256(
                          Path(runner.__file__).read_bytes()).hexdigest(),
                      "checks": checks}, indent=2))
    return 0 if status == "ok" else 1


if __name__ == "__main__":
    sys.exit(main())
