#!/usr/bin/env python3
"""Build and execute std/channel concurrency programs at O0 and O2.

Every build and program execution has a finite timeout. Build output and
program output are kept under --out; summary.json records the exact CLI,
environment, source hashes, per-command status and every failure. Concurrency
cases are executed repeatedly. The programs themselves use only atomics,
mutex-protected channel state and joins; they contain no sleeps.
"""
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
PROGRAM_TIMEOUT = 60
PANIC_TIMEOUT = 20

# name, expected exit, repeats, panic prefix (None for normal success)
CASES = [
    ("bounded-mpmc", 0, 3, None),
    ("unbounded-mpmc", 0, 3, None),
    ("bounded-blocking", 0, 3, None),
    ("try-ops", 0, 1, None),
    ("close-drain", 0, 3, None),
    ("close-wake", 0, 3, None),
    ("owned-payload", 0, 3, None),
    ("unbounded-owned", 0, 3, None),
    ("capacity-zero", 101, 1, "LANG_NATIVE_PANIC:v1:division-by-zero:"),
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, env, cwd, timeout):
    started = time.monotonic()
    try:
        result = subprocess.run([str(item) for item in argv], capture_output=True,
                                text=True, timeout=timeout, env=env, cwd=cwd)
        return dict(argv=[str(item) for item in argv], status="exited",
                    exit=result.returncode, stdout=result.stdout,
                    stderr=result.stderr,
                    elapsed_seconds=round(time.monotonic() - started, 3))
    except subprocess.TimeoutExpired as error:
        return dict(argv=[str(item) for item in argv], status="timeout",
                    exit=None,
                    stdout=(error.stdout or "") if isinstance(error.stdout, str) else "",
                    stderr=(error.stderr or "") if isinstance(error.stderr, str) else "",
                    timeout_seconds=timeout,
                    elapsed_seconds=round(time.monotonic() - started, 3))
    except OSError as error:
        return dict(argv=[str(item) for item in argv], status="launch_failed",
                    exit=None, stdout="", stderr=str(error),
                    elapsed_seconds=round(time.monotonic() - started, 3))


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def run_case(case, expected, repeats, panic_prefix, cli, root, out_root, env):
    manifest = root / "tests/native/channels" / case / "lang.toml"
    source = root / "tests/native/channels" / case / "src/main.wz"
    row = dict(case=case, expected_exit=expected, repeats=repeats,
               manifest=str(manifest), source=str(source),
               manifest_sha256=digest(manifest),
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
    if not (args.root / "tests/native/channels").is_dir():
        parser.error(f"not a Woozy root: {args.root}")
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
