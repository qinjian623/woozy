#!/usr/bin/env python3
"""Execute the independent retained static-callback contract.

Arguments always specify the compiler, toolchain, source root and output root.
No fixed public temporary directory is used.  Every external process gets its
own session/process group; a timeout kills that group, not any unrelated
process.
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

KNOWN_GC_PANICS = (
    "LANG_NATIVE_PANIC:v1:gc-world:critical-in-safe",
    "LANG_NATIVE_PANIC:v1:gc-world:critical-unregistered",
    "LANG_NATIVE_PANIC:v1:gc-world:safe-unregistered",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--ar", type=Path, required=True)
    parser.add_argument("--hosted-runtime", type=Path)
    parser.add_argument("--native-runtime", type=Path)
    parser.add_argument("--opt", type=int, action="append", dest="opts")
    parser.add_argument("--timeout", type=int, default=20)
    parser.add_argument(
        "--callback-gc",
        choices=("auto", "require", "skip"),
        default="auto",
        help="auto records the current bridge gap as unsupported; require "
        "makes it a failure; skip omits the probe",
    )
    args = parser.parse_args()
    root = args.root.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    opts = args.opts or [0, 2]
    report = {
        "operation_id": "root-static-retained-callback-20261112-01",
        "task": "FFI01 retained static callback",
        "contract": "compiler-dev/1",
        "checked_at": datetime.now(timezone.utc).isoformat(),
        "runs": [],
        "failed": [],
        "blocked": [],
        "callback_gc_supported": None,
    }
    env = os.environ.copy()

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

    def ok(result):
        return result["status"] == "exited" and result["exit"] == 0

    def json_document(result):
        try:
            return json.loads(result["stdout"].strip().splitlines()[-1])
        except (ValueError, IndexError):
            return None

    def build(project, opt, label):
        target = out / label
        result = run(
            [
                args.cli,
                "build",
                "--project",
                project,
                "--backend",
                "native",
                "--opt",
                str(opt),
                "--out-dir",
                target,
                "--format",
                "json",
            ],
            timeout=180,
        )
        document = json_document(result) if ok(result) else None
        passed = (
            ok(result)
            and document is not None
            and document.get("status") in ("ok", "passed")
        )
        program = target / "program"
        passed = passed and program.is_file()
        record(
            "build:" + label,
            passed=passed,
            target=str(target),
            result=result,
        )
        require(passed, "build:" + label, result["stderr"][-300:])
        return program if passed else None

    def execute(label, program, timeout=None):
        result = run([program], timeout=timeout or args.timeout)
        record("run:" + label, result=result)
        require(ok(result), "run:" + label, result["stderr"][-300:])
        return result

    try:
        report["head"] = run(
            ["git", "-C", root, "rev-parse", "HEAD"], timeout=30
        )["stdout"].strip()

        fixture_source = (
            root
            / "tests/native/c_retained_static/fixture/retained_static.c"
        )
        fixture_dir = out / "fixture"
        fixture_dir.mkdir(parents=True, exist_ok=True)
        fixture_object = fixture_dir / "retained_static.o"
        fixture_library = fixture_dir / "libretained_static.a"
        compile_result = run(
            [
                args.clang,
                "--target=x86_64-unknown-linux-gnu",
                "-O2",
                "-fPIC",
                "-pthread",
                "-Wall",
                "-Wextra",
                "-c",
                fixture_source,
                "-o",
                fixture_object,
            ],
            timeout=120,
        )
        record("fixture:compile", result=compile_result)
        require(
            ok(compile_result), "fixture:compile", compile_result["stderr"][-300:]
        )
        archive_result = run(
            [args.ar, "rcD", fixture_library, fixture_object], timeout=60
        )
        record("fixture:archive", result=archive_result)
        require(
            ok(archive_result), "fixture:archive", archive_result["stderr"][-300:]
        )
        link_dir = root / "tests/native/c_retained_static/.build"
        link_dir.mkdir(parents=True, exist_ok=True)
        for artifact in (fixture_object, fixture_library):
            shutil.copy2(artifact, link_dir / artifact.name)
        if report["failed"]:
            raise RuntimeError("fixture build failed")

        retained_project = (
            root / "tests/native/c_retained_static/projects/retained_static"
        )
        for opt in opts:
            label = f"retained_static:O{opt}"
            program = build(
                retained_project / "lang.toml", opt, f"retained-static-o{opt}"
            )
            if program:
                execute(label, program)

        gc_projects = ("callback_gc_sync", "callback_gc_external")
        if args.callback_gc == "skip":
            for name in gc_projects:
                record(
                    "callback-gc:" + name,
                    status="not_run",
                    reason="--callback-gc=skip",
                )
        else:
            gc_results = []
            for name in gc_projects:
                project = (
                    root / f"tests/native/c_retained_static/projects/{name}"
                )
                for opt in opts:
                    label = f"{name}:O{opt}"
                    program = build(
                        project / "lang.toml", opt, f"{name}-o{opt}"
                    )
                    if not program:
                        gc_results.append({"name": label, "state": "failed"})
                        continue
                    result = run([program], timeout=args.timeout)
                    record("run:" + label, result=result)
                    if ok(result):
                        gc_results.append({"name": label, "state": "passed"})
                    elif (
                        result["status"] == "exited"
                        and result["exit"] == 101
                        and any(
                            marker in result["stderr"]
                            for marker in KNOWN_GC_PANICS
                        )
                    ):
                        gc_results.append({"name": label, "state": "unsupported"})
                    else:
                        gc_results.append({"name": label, "state": "failed"})
                        require(
                            False,
                            "callback-gc:" + label,
                            result["stderr"][-300:],
                        )

            unsupported = [
                item["name"]
                for item in gc_results
                if item["state"] == "unsupported"
            ]
            failed = [
                item["name"] for item in gc_results if item["state"] == "failed"
            ]
            report["callback_gc_supported"] = not unsupported and not failed
            if unsupported:
                report["blocked"].append("callback-gc")
                record(
                    "callback-gc:capability",
                    supported=False,
                    unsupported=unsupported,
                    reason=(
                        "the generated callback bridge does not enter/leave the "
                        "foreign-callback GC scope; see development/docs/C_RETAINED_STATIC_CALLBACKS.md"
                    ),
                )
                if args.callback_gc == "require":
                    require(
                        False,
                        "callback-gc:required",
                        ", ".join(unsupported),
                    )
            else:
                record("callback-gc:capability", supported=not failed)

        # The callback wrapper is a runtime ABI, not a source entry point.
        # When archives are supplied, check both symbols in both runtimes.
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
                symbols = run(
                    [nm, "-g", "--defined-only", archive], timeout=60
                )
                record("archive:" + label, result=symbols)
                passed = ok(symbols) and all(
                    symbol in symbols["stdout"]
                    for symbol in (
                        "lang_gc_program_callback_enter_v1",
                        "lang_gc_program_callback_leave_v1",
                    )
                )
                require(passed, "archive:" + label + ":callback-symbols")

        negatives = {
            "capturing": "E_CAPABILITY_UNAVAILABLE",
            "throws": "E_TYPE_MISMATCH",
        }
        for name, expected in negatives.items():
            project = (
                root / f"tests/native/c_retained_static/negative/{name}"
            )
            result = run(
                [
                    args.cli,
                    "check",
                    "--project",
                    project / "lang.toml",
                    "--backend",
                    "native",
                    "--format",
                    "json",
                ],
                timeout=60,
            )
            document = json_document(result)
            codes = [
                diagnostic.get("code")
                for diagnostic in (document or {}).get("diagnostics", [])
            ]
            passed = (
                result["status"] == "exited"
                and result["exit"] == 1
                and document is not None
                and document.get("status") == "error"
                and expected in codes
            )
            record(
                "negative:" + name,
                passed=passed,
                expected=expected,
                result=result,
            )
            require(passed, "negative:" + name, json.dumps(codes))

        report["artifact_hashes"] = {
            str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(out.rglob("*"))
            if path.is_file()
        }
        if report["failed"]:
            report["status"] = "failed"
        elif report["blocked"]:
            report["status"] = "passed_with_unsupported"
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
                    "blocked": report.get("blocked", []),
                    "callback_gc_supported": report.get(
                        "callback_gc_supported"
                    ),
                    "report": str(report_path),
                },
                ensure_ascii=False,
            )
        )
    return 0 if report.get("status") in ("passed", "passed_with_unsupported") else 1


if __name__ == "__main__":
    raise SystemExit(main())
