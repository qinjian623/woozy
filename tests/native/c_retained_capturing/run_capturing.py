#!/usr/bin/env python3
"""Independent evidence runner for retained capturing C callbacks.

The C fixture lifecycle is always executed.  Source projects are built and run
at O0/O2 only when the compiler accepts the frozen registration metadata; until
Root integrates the frontend/backend/runtime they are recorded as not_run with
the exact rejection reason.  The same command is used before and after
integration; pass --source-tests require after integration to turn not_run into
a failure.

Every external process has its own process group and a timeout.  The runner
does not evaluate generated IR, mutate code/HIR, or run the whole CTest suite.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
from datetime import datetime, timezone

CANONICAL_LD_LIBRARY_PATH = (
    "/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/lib:"
    "/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9/usr/lib"
)

SOURCE_PROJECTS = (
    "probe",
    "move_capture",
    "captures_escape",
    "userdata_positions",
    "returns_and_units",
    "sync_register",
    "two_resource_borrow",
    "c_thread",
    "failure_cleanup",
    "close_modes",
    "typed_errors",
    "gc_cleanup",
    "cleanup_observation",
)

INTEGRATION_NEGATIVES = {
    "throws": ("native", "E_CALL_ARGUMENT"),
    "signature": ("native", "E_CALL_ARGUMENT"),
    "mode": ("native", "E_CAPABILITY_UNAVAILABLE"),
    "bad_metadata": ("native", "E_BINDING_INVALID"),
    "python_unsupported": ("python", "E_CAPABILITY_UNAVAILABLE"),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--hosted-runtime", type=Path)
    parser.add_argument("--native-runtime", type=Path)
    parser.add_argument("--fixture-test", type=Path)
    parser.add_argument("--opt", type=int, action="append", dest="opts")
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument(
        "--source-tests",
        choices=("auto", "require", "skip"),
        default="auto",
        help="auto records a missing integration as not_run; require turns "
        "every source not_run into a failure; skip omits source projects",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Each run owns its binding projects and .build library directory.
    tests = out / "source"
    shutil.copytree(root / "tests/native/c_retained_capturing", tests,
                    dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns(".build", "__pycache__"))
    opts = args.opts or [0, 2]
    report = {
        "operation_id": "root-capturing-callback-source-tests-20261113",
        "task": "retained capturing C callbacks",
        "contract": "compiler-dev/1",
        "checked_at": datetime.now(timezone.utc).isoformat(),
        "runs": [],
        "failed": [],
        "not_run": [],
        "source_tests": args.source_tests,
    }
    env = os.environ.copy()
    fixture_dir = out / "fixture"
    fixture_dir.mkdir(parents=True, exist_ok=True)
    library_path = [str(fixture_dir), CANONICAL_LD_LIBRARY_PATH]
    if env.get("LD_LIBRARY_PATH"):
        library_path.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(library_path)

    def run(argv, timeout, cwd=None):
        command = [str(value) for value in argv]
        proc = None
        try:
            proc = subprocess.Popen(
                command,
                cwd=str(cwd) if cwd else None,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=env,
                start_new_session=True,
            )
            stdout, stderr = proc.communicate(timeout=timeout)
            return {
                "argv": command,
                "status": "exited",
                "exit": proc.returncode,
                "stdout": stdout,
                "stderr": stderr,
            }
        except subprocess.TimeoutExpired:
            if proc is not None and proc.poll() is None:
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    pass
            stdout = stderr = ""
            if proc is not None:
                try:
                    stdout, stderr = proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
            return {
                "argv": command,
                "status": "timeout",
                "exit": None,
                "stdout": stdout,
                "stderr": stderr,
            }
        except OSError as error:
            return {
                "argv": command,
                "status": "launch_failed",
                "exit": None,
                "stdout": "",
                "stderr": str(error),
            }
        finally:
            if proc is not None and proc.poll() is None:
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    pass
                try:
                    proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    pass

    def record(name, **fields):
        entry = {"name": name, **fields}
        report["runs"].append(entry)
        return entry

    def require(condition, name, detail=""):
        if not condition:
            report["failed"].append(name + (": " + detail if detail else ""))
        return condition

    def not_run(name, reason):
        report["not_run"].append({"name": name, "reason": reason})

    def mark_source_not_run(reason):
        for project in SOURCE_PROJECTS:
            for opt in opts:
                not_run(f"source:{project}:O{opt}", reason)
        for name in INTEGRATION_NEGATIVES:
            not_run("negative:" + name, reason)
        not_run("no-output:bad_metadata", reason)

    def ok(result):
        return result["status"] == "exited" and result["exit"] == 0

    def json_document(result):
        try:
            return json.loads(result["stdout"].strip().splitlines()[-1])
        except (ValueError, IndexError):
            return None

    def diagnostics(document):
        return (document or {}).get("diagnostics", [])

    def codes(document):
        return [entry.get("code") for entry in diagnostics(document)]

    def integration_missing(document):
        for entry in diagnostics(document):
            code = entry.get("code", "")
            text = (entry.get("message", "") + " " + entry.get("phase", "")).lower()
            if code == "E_BINDING_INVALID" and any(
                token in text
                for token in (
                    "registration",
                    "registers",
                    "callback_userdata",
                    "unregister_symbol",
                    "unknown",
                    "unexpected",
                    "additional",
                )
            ):
                return True, entry.get("message", "registration metadata")
            if code == "E_CAPABILITY_UNAVAILABLE" and any(
                token in text
                for token in (
                    "c binding",
                    "c_binding",
                    "native",
                    "hosted",
                    "llvm",
                    "callback",
                )
            ):
                return True, entry.get("message", "C/native capability")
        return False, ""

    def check_project(project, backend, timeout=120):
        return run(
            [
                args.cli,
                "check",
                "--project",
                project / "lang.toml",
                "--backend",
                backend,
                "--format",
                "json",
            ],
            timeout=timeout,
        )

    def build(project, opt, label):
        target = out / label
        result = run(
            [
                args.cli,
                "build",
                "--project",
                project / "lang.toml",
                "--backend",
                "native",
                "--opt",
                str(opt),
                "--out-dir",
                target,
                "--format",
                "json",
            ],
            timeout=240,
        )
        document = json_document(result)
        passed = (
            ok(result)
            and document is not None
            and document.get("status") in ("ok", "passed")
            and (target / "program").is_file()
        )
        record("build:" + label, passed=passed, target=str(target), result=result)
        return (target / "program") if passed else None, result, document

    def execute(label, program):
        result = run([program], timeout=args.timeout)
        record("run:" + label, result=result)
        require(ok(result), "run:" + label, result["stderr"][-300:])
        return result

    def expect_diagnostic(name, project, backend, expected):
        result = check_project(project, backend)
        document = json_document(result)
        found = codes(document)
        passed = (
            result["status"] == "exited"
            and result["exit"] == 1
            and document is not None
            and document.get("status") == "error"
            and expected in found
        )
        record(
            "negative:" + name,
            passed=passed,
            expected=expected,
            result=result,
        )
        require(passed, "negative:" + name, json.dumps(found))

    try:
        head = run(["git", "-C", root, "rev-parse", "HEAD"], timeout=30)
        report["head"] = head["stdout"].strip()

        fixture_source = tests / "fixture/capturing_fixture.c"
        fixture_so = fixture_dir / "libc_capturing.so"
        compile_result = run(
            [
                args.clang,
                "--target=x86_64-unknown-linux-gnu",
                "-std=c11",
                "-O2",
                "-fPIC",
                "-pthread",
                "-Wall",
                "-Wextra",
                "-shared",
                "-Wl,-soname,libc_capturing.so",
                fixture_source,
                "-o",
                fixture_so,
            ],
            timeout=120,
        )
        record("fixture:compile", result=compile_result)
        require(
            ok(compile_result), "fixture:compile", compile_result["stderr"][-300:]
        )
        if report["failed"]:
            raise RuntimeError("fixture build failed")
        link_dir = tests / ".build"
        link_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(fixture_so, link_dir / fixture_so.name)

        if args.fixture_test:
            fixture_result = run([args.fixture_test], timeout=60)
            record("fixture:lifecycle", result=fixture_result)
            require(
                ok(fixture_result),
                "fixture:lifecycle",
                fixture_result["stderr"][-300:],
            )

        source_available = False
        source_reason = "--source-tests=skip"
        if args.source_tests == "skip":
            mark_source_not_run(source_reason)
        else:
            probe = check_project(tests / "projects/probe", "native")
            probe_document = json_document(probe)
            if (
                ok(probe)
                and probe_document is not None
                and probe_document.get("status") in ("ok", "passed")
            ):
                source_available = True
                record("source:integration-probe", passed=True, result=probe)
            else:
                missing, reason = integration_missing(probe_document)
                if missing:
                    source_reason = "capturing registration not integrated: " + reason
                    record(
                        "source:integration-probe",
                        passed=False,
                        unsupported=True,
                        reason=source_reason,
                        result=probe,
                    )
                    mark_source_not_run(source_reason)
                else:
                    record(
                        "source:integration-probe",
                        passed=False,
                        result=probe,
                    )
                    require(
                        False,
                        "source:integration-probe",
                        json.dumps(codes(probe_document)) + probe["stderr"][-200:],
                    )

        if source_available:
            for project_name in SOURCE_PROJECTS:
                project = tests / "projects" / project_name
                for opt in opts:
                    label = f"{project_name}:O{opt}"
                    program, result, document = build(project, opt, label)
                    if program is None:
                        missing, reason = integration_missing(document)
                        if missing:
                            not_run(
                                "source:" + label,
                                "capturing registration not integrated: " + reason,
                            )
                        else:
                            require(
                                False,
                                "build:" + label,
                                json.dumps(codes(document)) + result["stderr"][-200:],
                            )
                        continue
                    execute(label, program)

            for name, (backend, expected) in INTEGRATION_NEGATIVES.items():
                expect_diagnostic(
                    name, tests / "negative" / name, backend, expected
                )

            missing = out / "no-output-bad-metadata"
            shutil.rmtree(missing, ignore_errors=True)
            build_result = run(
                [
                    args.cli,
                    "build",
                    "--project",
                    tests / "negative/bad_metadata/lang.toml",
                    "--backend",
                    "native",
                    "--opt",
                    "0",
                    "--out-dir",
                    missing,
                    "--format",
                    "json",
                ],
                timeout=240,
            )
            passed = (
                not ok(build_result)
                and not (missing / "program").exists()
                and not (missing / "program.py").exists()
            )
            record(
                "no-output:bad_metadata",
                passed=passed,
                target=str(missing),
                result=build_result,
            )
            require(passed, "no-output:bad_metadata", build_result["stderr"][-300:])

        # These negatives use only the frozen static callback surface and can
        # run before the capturing registration interface is integrated.
        expect_diagnostic(
            "static_capture",
            tests / "negative/static_capture",
            "native",
            "E_CAPABILITY_UNAVAILABLE",
        )
        expect_diagnostic(
            "python_static_unsupported",
            tests / "negative/python_static_unsupported",
            "python",
            "E_CAPABILITY_UNAVAILABLE",
        )
        missing_static = out / "no-output-static-capture"
        shutil.rmtree(missing_static, ignore_errors=True)
        static_build = run(
            [
                args.cli,
                "build",
                "--project",
                tests / "negative/static_capture/lang.toml",
                "--backend",
                "native",
                "--opt",
                "0",
                "--out-dir",
                missing_static,
                "--format",
                "json",
            ],
            timeout=240,
        )
        static_no_output = (
            not ok(static_build)
            and not (missing_static / "program").exists()
            and not (missing_static / "program.py").exists()
        )
        record(
            "no-output:static_capture",
            passed=static_no_output,
            target=str(missing_static),
            result=static_build,
        )
        require(
            static_no_output,
            "no-output:static_capture",
            static_build["stderr"][-300:],
        )

        # The private registration ABI is hosted-only.  The hosted archive is
        # required to define all eight symbols; the freestanding archive is
        # recorded for information only.
        if args.hosted_runtime or args.native_runtime:
            nm = args.clang.with_name("llvm-nm")
            if not nm.is_file():
                nm = Path("/usr/bin/nm")
            for label, archive in (
                ("hosted", args.hosted_runtime),
                ("native", args.native_runtime),
            ):
                if archive is None:
                    continue
                symbols = run([nm, "-g", "--defined-only", archive], timeout=60)
                record("archive:" + label, result=symbols)
                passed = ok(symbols) and all(
                    symbol in symbols["stdout"]
                    for symbol in (
                        "lang_c_registration_new_v1",
                        "lang_c_registration_finish_v1",
                        "lang_c_registration_close_v1",
                        "lang_c_registration_release_v1",
                        "lang_c_registration_enter_v1",
                        "lang_c_registration_leave_v1",
                        "lang_c_registration_borrow_v1",
                        "lang_c_registration_end_borrow_v1",
                    )
                )
                record("archive:" + label + ":registration-symbols", passed=passed)
                if label == "hosted":
                    if source_available:
                        require(passed, "archive:hosted:registration-symbols")
                    elif not passed:
                        not_run(
                            "archive:hosted:registration-symbols",
                            "registration runtime ABI not linked into hosted archive",
                        )

        # The default freestanding core must keep its old no-INTERP/no-NEEDED
        # ELF shape after the hosted callback ABI is added.
        if not source_available:
            not_run("core-freestanding", source_reason)
        else:
            core_program, core_result, core_document = build(
                root / "tests/fixtures/core", 0, "core-freestanding"
            )
            if core_program is None:
                core_missing, core_reason = integration_missing(core_document)
                if core_missing:
                    not_run("core-freestanding", core_reason)
                else:
                    require(
                        False,
                        "core-freestanding:build",
                        json.dumps(codes(core_document))
                        + core_result["stderr"][-200:],
                    )
            else:
                readelf = shutil.which("readelf")
                if not readelf:
                    not_run("core-freestanding", "readelf unavailable")
                else:
                    elf = run([readelf, "-l", "-d", core_program], timeout=30)
                    passed = (
                        ok(elf)
                        and "INTERP" not in elf["stdout"]
                        and "NEEDED" not in elf["stdout"]
                    )
                    record("core-freestanding:elf", passed=passed, result=elf)
                    require(passed, "core-freestanding:elf", elf["stderr"][-300:])
                    if passed:
                        execute("core-freestanding", core_program)

        if args.source_tests == "require" and report["not_run"]:
            for entry in report["not_run"]:
                report["failed"].append(
                    entry["name"] + ": required source test not run ("
                    + entry["reason"] + ")"
                )

        report["artifact_hashes"] = {
            str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(out.rglob("*"))
            if path.is_file()
        }
        if report["failed"]:
            report["status"] = "failed"
        elif report["not_run"]:
            report["status"] = "passed_with_not_run"
        else:
            report["status"] = "passed"
    except Exception as error:  # noqa: BLE001 - evidence runner
        report["failed"].append("runner exception: " + repr(error))
        report["status"] = "failed"
    finally:
        report_path = out / "report.json"
        report_path.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n"
        )
        print(
            json.dumps(
                {
                    "status": report.get("status", "failed"),
                    "failed": report["failed"],
                    "not_run": report["not_run"],
                    "report": str(report_path),
                },
                ensure_ascii=False,
            )
        )
    return 0 if report.get("status") in ("passed", "passed_with_not_run") else 1


if __name__ == "__main__":
    raise SystemExit(main())
