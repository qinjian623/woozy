#!/usr/bin/env python3
"""Build/execute channel timed/cancel cases at O0/O2 with finite timeouts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

BUILD_TIMEOUT = 240
PROGRAM_TIMEOUT = 90
PANIC_TIMEOUT = 30
CASES = [
    ("timeout-basic", 0, 1, None),
    ("timeout-blocking", 0, 2, None),
    ("cancel-basic", 0, 1, None),
    ("cancel-wake", 0, 3, None),
    ("deadline-spurious", 0, 2, None),
    ("races", 0, 1, None),
    ("owned-timeout-cancel", 0, 2, None),
    ("invalid-send-timeout", 101, 1, "LANG_NATIVE_PANIC:v1:runtime-handle-misuse:"),
    ("invalid-receive-timeout", 101, 1, "LANG_NATIVE_PANIC:v1:runtime-handle-misuse:"),
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, env, cwd, timeout):
    import base64
    import importlib.util

    started = time.monotonic()
    argv = [str(item) for item in argv]

    def elapsed():
        return round(time.monotonic() - started, 3)

    def as_text(data):
        return data.decode("utf-8", "backslashreplace")

    def as_base64(data):
        return base64.b64encode(data).decode("ascii")

    runner = getattr(command, "_run_process_impl", None)
    try:
        if runner is None:
            corpus_path = Path(__file__).resolve().parents[3] / "selfhost/corpus.py"
            spec = importlib.util.spec_from_file_location(
                "channel_waits_corpus", corpus_path)
            if spec is None or spec.loader is None:
                raise OSError(f"cannot load process runner: {corpus_path}")
            corpus = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(corpus)
            runner = corpus._run_process_impl
            command._run_process_impl = runner
        result = runner(argv, cwd, env, timeout)
        stdout_bytes = result["stdout"]
        stderr_bytes = result["stderr"]
        exit_code = result["exit"]
        timed_out = result["timed_out"]
        cleanup_complete = result["cleanup_complete"]
        abandoned_pipes = result["abandoned_pipes"]
    except OSError as error:
        # Popen failed before a child existed, so there is nothing to clean up.
        return dict(argv=argv, status="launch_failed", exit=None,
                    stdout="", stderr=str(error),
                    timed_out=False, cleanup_complete=True,
                    abandoned_pipes=False, stdout_base64="", stderr_base64="",
                    elapsed_seconds=elapsed())

    stdout = as_text(stdout_bytes)
    stderr = as_text(stderr_bytes)
    raw = dict(stdout_base64=as_base64(stdout_bytes),
               stderr_base64=as_base64(stderr_bytes))
    invalid_fields = []
    if type(exit_code) is not int or type(exit_code) is bool:
        invalid_fields.append("exit")
    if type(timed_out) is not bool:
        invalid_fields.append("timed_out")
    if type(cleanup_complete) is not bool:
        invalid_fields.append("cleanup_complete")
    if type(abandoned_pipes) is not bool:
        invalid_fields.append("abandoned_pipes")
    if invalid_fields:
        return dict(argv=argv, status="invalid_process_result", exit=None,
                    stdout=stdout, stderr=stderr,
                    timed_out=None, cleanup_complete=None,
                    abandoned_pipes=None,
                    invalid_process_fields=invalid_fields, **raw,
                    elapsed_seconds=elapsed())
    if timed_out:
        status = "timeout"
    elif abandoned_pipes:
        status = "abandoned_pipes"
    elif not cleanup_complete:
        status = "cleanup_incomplete"
    else:
        status = "exited"
    row = dict(argv=argv, status=status, exit=exit_code,
               stdout=stdout, stderr=stderr,
               timed_out=timed_out, cleanup_complete=cleanup_complete,
               abandoned_pipes=abandoned_pipes, **raw,
               elapsed_seconds=elapsed())
    if status == "timeout":
        row["timeout_seconds"] = timeout
    return row


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def static_elf(program, env, cwd):
    import re

    def shape_errors(kind, text):
        errors = []
        if not text:
            return ["readelf produced empty stdout"]
        for marker in ("Format: elf", "Arch: ", "ElfHeader {",
                       "ProgramHeaders [", "DynamicSection ["):
            if marker in text:
                errors.append(f"llvm-readobj marker {marker!r}")
        if kind == "headers":
            for marker in ("Elf file type is ", "Entry point ",
                           "Program Headers:", "Type", "Offset", "VirtAddr"):
                if marker not in text:
                    errors.append(f"missing GNU header marker {marker!r}")
            if re.search(
                    r"There are [0-9]+ program headers, starting at offset "
                    r"[0-9]+", text) is None:
                errors.append("missing GNU program-header count line")
        elif kind == "dynamic":
            no_dynamic = "There is no dynamic section in this file." in text
            dynamic_section = ("Dynamic section at offset" in text
                               and "Tag" in text and "Name" in text)
            if not (no_dynamic or dynamic_section):
                errors.append("missing GNU dynamic-section shape")
        else:
            errors.append(f"unknown readelf output kind {kind!r}")
        return errors

    headers = command(["readelf", "-l", program], env, cwd, 30)
    dynamic = command(["readelf", "-d", program], env, cwd, 30)
    for row in (headers, dynamic):
        assert row["status"] == "exited" and row["exit"] == 0, row
        assert type(row["exit"]) is int and type(row["exit"]) is not bool, row
        assert row["timed_out"] is False, row
        assert row["cleanup_complete"] is True, row
        assert row["abandoned_pipes"] is False, row
    assert "INTERP" not in headers["stdout"], headers
    assert "(NEEDED)" not in dynamic["stdout"], dynamic
    header_errors = shape_errors("headers", headers["stdout"])
    dynamic_errors = shape_errors("dynamic", dynamic["stdout"])
    assert not header_errors, (header_errors, headers)
    assert not dynamic_errors, (dynamic_errors, dynamic)
    return dict(program_headers=headers, dynamic_section=dynamic,
                has_interp=False, has_needed=False)


def run_case(case, expected, repeats, panic_prefix, cli, root, out_root, env):
    manifest = root / "tests/native/channel-waits" / case / "lang.toml"
    source = root / "tests/native/channel-waits" / case / "src/main.wz"
    row = dict(case=case, expected_exit=expected, repeats=repeats,
               source_sha256=digest(source), builds=[])
    for opt in (0, 2):
        build_dir = out_root / case / f"O{opt}"
        if build_dir.exists():
            shutil.rmtree(build_dir)
        build_dir.mkdir(parents=True)
        build = command([
            cli, "build", "--project", manifest, "--backend", "native",
            "--opt", str(opt), "--out-dir", build_dir, "--format", "json"],
            env, root, BUILD_TIMEOUT)
        build["ok"] = False
        if build["status"] == "exited" and build["exit"] == 0 and not build["stderr"]:
            try:
                response = json.loads(build["stdout"])
                build["response"] = response
                build["ok"] = response.get("status") == "ok"
            except json.JSONDecodeError:
                build["response"] = None
        write_text(build_dir / "build.stdout", build["stdout"])
        write_text(build_dir / "build.stderr", build["stderr"])
        build_row = dict(opt=opt, build=build, executions=[])
        row["builds"].append(build_row)
        if not build["ok"]:
            build_row["failure"] = "build did not produce status ok"
            continue
        program = build_dir / "program"
        build_row["program_sha256"] = digest(program)
        build_row["elf"] = static_elf(program, env, build_dir)
        for run_index in range(repeats):
            execution = command([program], env, build_dir,
                                PANIC_TIMEOUT if panic_prefix else PROGRAM_TIMEOUT)
            write_text(build_dir / f"run-{run_index}.stdout", execution["stdout"])
            write_text(build_dir / f"run-{run_index}.stderr", execution["stderr"])
            execution_row = dict(index=run_index, result=execution)
            build_row["executions"].append(execution_row)
            if execution["status"] != "exited" or execution["exit"] != expected:
                execution_row["failure"] = (
                    f"expected exit {expected}, got {execution['status']} "
                    f"{execution['exit']}")
                continue
            if panic_prefix:
                if not execution["stderr"].startswith(panic_prefix):
                    execution_row["failure"] = "panic prefix missing"
                elif execution["stdout"] != "":
                    execution_row["failure"] = "panic program wrote stdout"
            elif execution["stdout"] != "" or execution["stderr"] != "":
                execution_row["failure"] = "successful program produced output"
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--ld-library-path", default=os.environ.get("LD_LIBRARY_PATH", ""))
    args = parser.parse_args()
    args.cli = args.cli.resolve()
    args.root = args.root.resolve()
    args.out = args.out.resolve()
    if not args.cli.is_file():
        parser.error(f"CLI does not exist: {args.cli}")
    if not (args.root / "tests/native/channel-waits").is_dir():
        parser.error(f"not a CON02 worktree: {args.root}")
    args.out.mkdir(parents=True, exist_ok=True)

    env = dict(os.environ)
    if args.ld_library_path:
        env["LD_LIBRARY_PATH"] = args.ld_library_path
    report = dict(schema_version=1, status="failed",
                  cli=str(args.cli), cli_sha256=digest(args.cli),
                  ld_library_path=args.ld_library_path,
                  root=str(args.root),
                  channel_source=str(args.root / "stdlib/channel/channel.wz"),
                  channel_source_sha256=digest(args.root / "stdlib/channel/channel.wz"),
                  cancel_source=str(args.root / "stdlib/cancel/cancel.wz"),
                  cancel_source_sha256=digest(args.root / "stdlib/cancel/cancel.wz"),
                  started_unix=int(time.time()), runs=[], failures=[])
    try:
        for case, expected, repeats, panic_prefix in CASES:
            row = run_case(case, expected, repeats, panic_prefix,
                           args.cli, args.root, args.out, env)
            report["runs"].append(row)
            for build in row["builds"]:
                if "failure" in build:
                    report["failures"].append(dict(
                        case=case, opt=build["opt"], failure=build["failure"]))
                for execution in build["executions"]:
                    if "failure" in execution:
                        report["failures"].append(dict(
                            case=case, opt=build["opt"],
                            run=execution["index"], failure=execution["failure"]))
        if not report["failures"]:
            report["status"] = "passed"
    finally:
        report["finished_unix"] = int(time.time())
        summary = args.out / "summary.json"
        summary.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        builds = sum(len(row["builds"]) for row in report["runs"])
        executions = sum(len(build["executions"])
                         for row in report["runs"] for build in row["builds"])
        print(f"{report['status']}: {builds} builds, {executions} executions, "
              f"{len(report['failures'])} failures")
    if report["failures"]:
        for failure in report["failures"]:
            print(f"{failure['case']} O{failure['opt']}: {failure['failure']}",
                  file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
