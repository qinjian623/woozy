#!/usr/bin/env python3
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

WRAPS = [
    "lang_async_task_new_v1",
    "lang_async_sleep_v1",
    "lang_async_wait_fd_v1",
    "lang_async_frame_alloc_v1",
    "lang_async_frame_free_v1",
    "lang_free_v1",
    "lang_async_task_retain_v1",
    "lang_async_task_release_v1",
    "lang_gc_program_finish_v1",
]


class CommandFailure(Exception):
    def __init__(self, message, stdout, stderr, log_dir, timed_out=False):
        super().__init__(message)
        self.stdout = stdout
        self.stderr = stderr
        self.log_dir = log_dir
        self.timed_out = timed_out


def run_command(command, env, timeout, log_dir, label):
    log_dir.mkdir(parents=True, exist_ok=True)
    stdout_path = log_dir / f"{label}.stdout"
    stderr_path = log_dir / f"{label}.stderr"
    with open(stdout_path, "w") as stdout, open(stderr_path, "w") as stderr:
        process = subprocess.Popen(
            [str(part) for part in command],
            env=env,
            stdout=stdout,
            stderr=stderr,
            text=True,
            start_new_session=True,
        )
        timed_out = False
        try:
            status = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            status = process.wait()
            (log_dir / f"{label}.timeout").write_text(
                f"timeout after {timeout}s: {' '.join(map(str, command))}\n"
            )
    captured_stdout = stdout_path.read_text(errors="replace")
    captured_stderr = stderr_path.read_text(errors="replace")
    return status, timed_out, captured_stdout, captured_stderr


def report_failure(title, log_dir, stdout, stderr, timed_out=False):
    print(f"FAILED: {title}")
    if timed_out:
        print("reason: command timed out; process group killed")
    print(f"diagnostics: {log_dir}")
    if stdout:
        print("STDOUT:", stdout)
    if stderr:
        print("STDERR:", stderr)


def run_checked(command, env, timeout, log_dir, label, title):
    status, timed_out, stdout, stderr = run_command(
        command, env, timeout, log_dir, label
    )
    if timed_out or status != 0:
        report_failure(title, log_dir, stdout, stderr, timed_out)
        raise SystemExit(1)
    return stdout, stderr


def run_program(program, env, timeout, log_dir, title):
    status, timed_out, stdout, stderr = run_command(
        [program], env, timeout, log_dir, "run"
    )
    if timed_out:
        report_failure(title, log_dir, stdout, stderr, True)
        raise SystemExit(1)
    return status, stdout, stderr


def parse_counts(stderr):
    match = re.search(r"COUNTS ([^\n]+)", stderr)
    if not match:
        return None
    return {
        key: int(value)
        for key, value in re.findall(r"([A-Za-z_]+)=(\d+)", match.group(1))
    }


def check_observed(title, status, stdout, stderr, case, log_dir):
    expected_exit = case.get("exit", 0)
    if status != expected_exit:
        report_failure(
            f"{title}: exit {status}, expected {expected_exit}", log_dir, stdout, stderr
        )
        raise SystemExit(1)
    if "stdout" in case and stdout != case["stdout"]:
        report_failure(
            f"{title}: stdout mismatch\nEXPECTED:\n{case['stdout']}\nGOT:\n{stdout}",
            log_dir,
            stdout,
            stderr,
        )
        raise SystemExit(1)
    counts = parse_counts(stderr)
    if counts is None:
        report_failure(f"{title}: observer COUNTS line missing", log_dir, stdout, stderr)
        raise SystemExit(1)
    expected = case["counts"]
    if any(counts.get(key) != value for key, value in expected.items()):
        report_failure(
            f"{title}: counts mismatch\nEXPECTED: {expected}\nGOT: {counts}",
            log_dir,
            stdout,
            stderr,
        )
        raise SystemExit(1)
    return counts


def build_regular_cases(cli, root, out_base, env, catalog, build_timeout, run_timeout):
    passed = 0
    for opt in ("0", "2"):
        for case in catalog.get("positive", []):
            name = case["name"]
            manifest = root / "tests" / "async" / name / "lang.toml"
            log_dir = out_base / f"{name}_O{opt}"
            output_dir = log_dir / "build"
            command = [
                cli,
                "build",
                "--project",
                manifest,
                "--backend",
                "native",
                "--opt",
                opt,
                "--out-dir",
                output_dir,
            ]
            run_checked(
                command,
                env,
                build_timeout,
                log_dir,
                "build",
                f"positive case '{name}' O{opt} build",
            )
            status, stdout, stderr = run_program(
                output_dir / "program",
                env,
                run_timeout,
                log_dir,
                f"positive case '{name}' O{opt}",
            )
            expected_exit = case.get("exit", 0)
            expected_stdout = case.get("stdout")
            if status != expected_exit:
                report_failure(
                    f"positive case '{name}' O{opt}: exit {status}, expected {expected_exit}",
                    log_dir,
                    stdout,
                    stderr,
                )
                raise SystemExit(1)
            if expected_stdout is not None and stdout != expected_stdout:
                report_failure(
                    f"positive case '{name}' O{opt}: stdout mismatch\n"
                    f"EXPECTED:\n{expected_stdout}\nGOT:\n{stdout}",
                    log_dir,
                    stdout,
                    stderr,
                )
                raise SystemExit(1)
            print(f"  [PASS] positive (O{opt}): {name}")
            passed += 1
    return passed


def build_reject_cases(cli, root, out_base, env, catalog, build_timeout):
    passed = 0
    for case in catalog.get("reject", []):
        name = case["name"]
        code = case["code"]
        manifest = root / "tests" / "async" / "reject" / name / "lang.toml"
        log_dir = out_base / "reject" / name
        command = [
            cli,
            "build",
            "--project",
            manifest,
            "--backend",
            "native",
            "--opt",
            "0",
            "--out-dir",
            log_dir / "build",
        ]
        status, timed_out, stdout, stderr = run_command(
            command, env, build_timeout, log_dir, "build"
        )
        if timed_out:
            report_failure(
                f"reject case '{name}' build timed out", log_dir, stdout, stderr, True
            )
            raise SystemExit(1)
        if status == 0:
            report_failure(
                f"reject case '{name}' built successfully, expected '{code}'",
                log_dir,
                stdout,
                stderr,
            )
            raise SystemExit(1)
        if code not in stdout + stderr:
            report_failure(
                f"reject case '{name}' did not report '{code}'",
                log_dir,
                stdout,
                stderr,
            )
            raise SystemExit(1)
        print(f"  [PASS] reject: {name} (expected {code})")
        passed += 1
    return passed


def cached_cxx(cli):
    cache = cli.parent / "CMakeCache.txt"
    if not cache.exists():
        return None
    match = re.search(
        r"^CMAKE_CXX_COMPILER:[^=]+=(.+)$", cache.read_text(), re.MULTILINE
    )
    return match.group(1).strip() if match else None


def observer_paths(args, cli):
    helper = args.helper or cli.parent / "native" / "lang_native_helper"
    runtime = args.runtime or cli.parent / "native" / "liblang_native_runtime.a"
    clang = args.clang
    if clang is None:
        clang = os.environ.get("LANG_CLANGXX") or cached_cxx(cli)
    if clang is None:
        clang = shutil.which("clang++")
    ld = args.ld
    if ld is None and clang is not None:
        candidate = Path(clang).parent / "ld.lld"
        if candidate.exists():
            ld = candidate
    if ld is None:
        ld = shutil.which("ld.lld")
    return Path(helper), Path(runtime), Path(clang) if clang else None, (
        Path(ld) if ld else None
    )


def build_observed_cases(args, cli, root, out_base, env, catalog, build_timeout,
                         link_timeout, run_timeout):
    cases = catalog.get("observed", [])
    if not cases or args.observe == "off":
        return 0, False

    helper, runtime, clang, ld = observer_paths(args, cli)
    observer = args.observer or root / "tests" / "async" / "async_observer.cpp"
    missing = [
        str(path)
        for path in (helper, runtime, clang, ld, observer)
        if path is None or not path.exists()
    ]
    if missing:
        message = "observer prerequisites missing: " + ", ".join(missing)
        if args.observe == "on":
            print(f"FAILED: {message}")
            raise SystemExit(1)
        print(f"  [SKIP] observed cases: {message}")
        return 0, False

    observer_object = out_base / "observer.o"
    run_checked(
        [
            clang,
            "-std=c++20",
            "-O2",
            "-ffreestanding",
            "-fno-builtin",
            "-fno-exceptions",
            "-fno-rtti",
            "-fno-stack-protector",
            "-c",
            observer,
            "-o",
            observer_object,
        ],
        env,
        build_timeout,
        out_base / "observer",
        "build",
        "observer build",
    )

    passed = 0
    for opt in ("0", "2"):
        for case in cases:
            name = case["name"]
            manifest = root / "tests" / "async" / name / "lang.toml"
            log_dir = out_base / f"{name}_observe_O{opt}"
            object_path = log_dir / "program.o"
            llvm_path = log_dir / "program.ll"
            program_path = log_dir / "program"
            run_checked(
                [
                    helper,
                    "--project",
                    manifest,
                    "--opt",
                    opt,
                    "--out-object",
                    object_path,
                    "--out-llvm",
                    llvm_path,
                ],
                env,
                build_timeout,
                log_dir,
                "build",
                f"observed case '{name}' O{opt} build",
            )
            link_command = [
                clang,
                "--target=x86_64-unknown-linux-gnu",
                "-nostdlib",
                "-static",
                f"--ld-path={ld}",
                object_path,
                observer_object,
                runtime,
            ]
            link_command.extend(f"-Wl,--wrap={symbol}" for symbol in WRAPS)
            link_command.extend(["-o", program_path])
            run_checked(
                link_command,
                env,
                link_timeout,
                log_dir,
                "link",
                f"observed case '{name}' O{opt} link",
            )
            status, stdout, stderr = run_program(
                program_path,
                env,
                run_timeout,
                log_dir,
                f"observed case '{name}' O{opt}",
            )
            counts = check_observed(
                f"observed case '{name}' O{opt}", status, stdout, stderr, case, log_dir
            )
            print(
                f"  [PASS] observed (O{opt}): {name} "
                f"future={counts['future_new']}/{counts['future_free']} "
                f"frame={counts['frame_new']}/{counts['frame_free']}"
            )
            passed += 1
    return passed, True


def main():
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--cli",
        type=Path,
        default=repo_root / "build" / "woozy-stdlib-native" / "woozyc",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="diagnostics/output root; defaults to a fresh independent temp dir",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=repo_root,
        help="repository root containing tests/async",
    )
    parser.add_argument("--helper", type=Path, default=None)
    parser.add_argument("--runtime", type=Path, default=None)
    parser.add_argument("--clang", type=Path, default=None)
    parser.add_argument("--ld", type=Path, default=None)
    parser.add_argument("--observer", type=Path, default=None)
    parser.add_argument(
        "--observe",
        choices=("auto", "on", "off"),
        default="auto",
        help="run allocation-observer cases when prerequisites are available",
    )
    parser.add_argument("--build-timeout", type=float, default=180)
    parser.add_argument("--link-timeout", type=float, default=120)
    parser.add_argument("--run-timeout", type=float, default=15)
    args = parser.parse_args()

    if args.out is None:
        args.out = Path(tempfile.mkdtemp(prefix="woozy-async-cases-"))
    args.out.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    print(f"diagnostics root: {args.out}")

    cases_file = args.root / "tests" / "async" / "cases.json"
    with open(cases_file) as stream:
        catalog = json.load(stream)

    passed = 0
    passed += build_regular_cases(
        args.cli,
        args.root,
        args.out,
        env,
        catalog,
        args.build_timeout,
        args.run_timeout,
    )
    passed += build_reject_cases(
        args.cli, args.root, args.out, env, catalog, args.build_timeout
    )
    observed_passed, observed_ran = build_observed_cases(
        args,
        args.cli,
        args.root,
        args.out,
        env,
        catalog,
        args.build_timeout,
        args.link_timeout,
        args.run_timeout,
    )
    passed += observed_passed

    expected = (
        len(catalog.get("positive", [])) * 2
        + len(catalog.get("reject", []))
        + (len(catalog.get("observed", [])) * 2 if observed_ran else 0)
    )
    print(f"\nALL {passed}/{expected} ASYNC CASES PASSED!")


if __name__ == "__main__":
    main()
