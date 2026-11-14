#!/usr/bin/env python3
"""Check explicit generic/method type-argument arity diagnostics at O0.

This component stages the live selfhost project/resolve/sema packages, builds a
small driver with the supplied stage-0 compiler, loads real manifests through
the Wz project loader, and compares the live Sema result with C++.

It does not build the 116-file CLI or claim a full corpus/self-hosting result.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SELFHOST = ROOT / "selfhost" / "src" / "wzcc"
STDLIB = ROOT / "stdlib"
RUNTIME = ROOT / "runtime"
ORIGINAL = ROOT / "tests" / "generics" / "indexed-calls" / "reject-generic-arity"
DEFAULT_WOOZYC = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
sys.path.insert(0, str(HERE.parents[1]))
from corpus import _run_process_impl  # noqa: E402

MANIFEST = """schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "native"
word_bits = 64
"""

# Every error case names the exact primary source substring. The original
# fixture is used unchanged; the other fixtures are generated below.
CASES = [
    {
        "name": "original-generic-too-many",
        "manifest": ORIGINAL / "lang.toml",
        "code": "E_TYPE_MISMATCH",
        "message": "wrong number of generic arguments",
        "primary": "id[i32,i64](1)",
    },
    {
        "name": "generic-call-ok",
        "source": "package app;\nfunc id[T](x T)T{return x;}\n"
                  "func main()i32{return id[i32](1);}\n",
    },
    {
        "name": "generic-too-few",
        "source": "package app;\nfunc pair[T,U](x T,y U)T{return x;}\n"
                  "func main()i32{return pair[i32](1,2);}\n",
        "code": "E_TYPE_MISMATCH",
        "message": "wrong number of generic arguments",
        "primary": "pair[i32](1,2)",
    },
    {
        "name": "generic-function-value-too-many",
        "source": "package app;\nfunc id[T](x T)T{return x;}\n"
                  "func main()i32{let f=id[i32,i64];return f(1);}\n",
        "code": "E_TYPE_MISMATCH",
        "message": "wrong number of generic arguments",
        "primary": "id[i32,i64]",
    },
    {
        "name": "method-too-many",
        "source": "package app;\nstruct K{v i32;}\n"
                  "impl K{func own[U](self Self,x i32)i32{return self.v+x;}}\n"
                  "func main()i32{let k=K(v:1);return k.own[i32,i64](2);}\n",
        "code": "E_TYPE_MISMATCH",
        "message": "wrong number of method type arguments",
        "primary": "k.own[i32,i64](2)",
    },
    {
        "name": "method-too-few",
        "source": "package app;\nstruct K{v i32;}\n"
                  "impl K{func own[U,V](self Self,x i32)i32{return self.v+x;}}\n"
                  "func main()i32{let k=K(v:1);return k.own[i32](2);}\n",
        "code": "E_TYPE_MISMATCH",
        "message": "wrong number of method type arguments",
        "primary": "k.own[i32](2)",
    },
    {
        "name": "trait-method-too-many",
        "source": "package app;\ntrait Pick{func pick[U](self Self,x i32)i32;}\n"
                  "struct K{v i32;}\n"
                  "impl Pick for K{func pick[V](self Self,x i32)i32{return self.v+x;}}\n"
                  "func main()i32{let k=K(v:1);return k.pick[i32,i64](2);}\n",
        "code": "E_TYPE_MISMATCH",
        "message": "wrong number of method type arguments",
        "primary": "k.pick[i32,i64](2)",
    },
    {
        "name": "method-call-ok",
        "source": "package app;\nstruct K{v i32;}\n"
                  "impl K{func own[U](self Self,x i32)i32{return self.v+x;}}\n"
                  "func main()i32{let k=K(v:1);return k.own[i32](2);}\n",
    },
    {
        "name": "value-too-many",
        "source": "package app;\nfunc id[T](x T)T{return x;}\n"
                  "func main()i32{return id[i32](1,2);}\n",
        "code": "E_CALL_ARGUMENT",
        "message": "wrong number of arguments",
        "primary": "id[i32](1,2)",
    },
    {
        "name": "method-value-too-many",
        "source": "package app;\nstruct K{v i32;}\n"
                  "impl K{func own[U](self Self,x i32)i32{return self.v+x;}}\n"
                  "func main()i32{let k=K(v:1);return k.own[i32](2,3);}\n",
        "code": "E_CALL_ARGUMENT",
        "message": "wrong number of arguments",
        "primary": "k.own[i32](2,3)",
    },
]


def fail(message: str) -> None:
    raise RuntimeError(message)


def finite_timeout(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("--timeout must be finite and positive") from error
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("--timeout must be finite and positive")
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", default=str(DEFAULT_WOOZYC),
                        help="stage-0 compiler used to build the component")
    parser.add_argument("--reference", default="",
                        help="optional C++ reference compiler for diagnostic parity")
    parser.add_argument("--work-dir", default=str(ROOT / "build" / "sema-generic-arity-runs"),
                        help="parent directory for retained run evidence")
    parser.add_argument("--timeout", type=finite_timeout, default=900.0,
                        help="per command timeout in seconds")
    parser.add_argument("--toolchain-root", default=os.environ.get("LANG_TOOLCHAIN_ROOT", ""),
                        help="directory containing llvm-*/lib and libxml2-*/usr/lib")
    return parser.parse_args()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_report(report: dict) -> None:
    work = Path(report["work_dir"])
    temporary = work / "report.json.tmp"
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(work / "report.json")


def input_snapshot(work: Path, stage0: Path, reference: Path | None) -> dict[str, str]:
    files = [stage0, HERE / "run.py", HERE / "src" / "main.wz",
             work / "component" / "lang.toml",
             work / "component" / "src" / "main.wz"]
    if reference is not None:
        files.append(reference)
    roots = [SELFHOST, STDLIB, RUNTIME, ORIGINAL, work / "fixtures"]
    for root in roots:
        if not root.is_dir():
            fail(f"input directory missing: {root}")
        for directory, directories, names in os.walk(root):
            if any((Path(directory) / name).is_symlink() for name in directories):
                fail(f"nested input directory symlink: {directory}")
            directories.sort()
            files.extend(Path(directory) / name for name in sorted(names))
    result = {str(path): sha256(path) for path in files}
    link = work / "component" / "src" / "wzcc"
    result[str(link) + " (link target)"] = os.readlink(link)
    return result


def run(argv: list[object], label: str, cwd: Path, env: dict[str, str],
        timeout: float, report: dict, ok_exits: tuple[int, ...] = (0,)) -> bytes:
    started = __import__("time").monotonic()
    record = {
        "label": label,
        "argv": [str(part) for part in argv],
        "cwd": str(cwd),
        "timeout_seconds": timeout,
        "status": "running",
    }
    report["commands"].append(record)
    save_report(report)
    try:
        stage0 = Path(report["stage0"])
        reference = Path(report["reference"]) if report.get("reference") else None
        record["inputs_before"] = input_snapshot(cwd, stage0, reference)
        if record["inputs_before"] != report["inputs_before"]:
            fail(f"{label}: inputs changed before command")
        executable = Path(argv[0]).resolve(strict=True)
        record["executable_sha256_before"] = sha256(executable)
        expected = report.get("program_sha256", {}).get(str(executable))
        if expected is not None and record["executable_sha256_before"] != expected:
            fail(f"{label}: program changed before command")
        save_report(report)
        process = _run_process_impl([str(part) for part in argv], cwd, env, timeout)
        stdout, stderr = process["stdout"], process["stderr"]
        (cwd / f"{label}.stdout").write_bytes(stdout)
        (cwd / f"{label}.stderr").write_bytes(stderr)
        record.update({key: process[key] for key in
                       ("exit", "timed_out", "cleanup_complete", "abandoned_pipes")
                       if key in process})
        record["stdout_sha256"] = hashlib.sha256(stdout).hexdigest()
        record["stderr_sha256"] = hashlib.sha256(stderr).hexdigest()
        record["inputs_after"] = input_snapshot(cwd, stage0, reference)
        record["executable_sha256_after"] = sha256(executable)
        if (record["inputs_after"] != record["inputs_before"] or
                record["executable_sha256_after"] != record["executable_sha256_before"]):
            fail(f"{label}: inputs or executable changed during command")
        if (type(process["exit"]) is not int or process["exit"] not in ok_exits
                or process["timed_out"] is not False
                or process["cleanup_complete"] is not True
                or process["abandoned_pipes"] is not False
                or stderr != b""):
            fail(f"{label}: invalid process result "
                 f"exit={process.get('exit')!r} "
                 f"timed_out={process.get('timed_out')!r} "
                 f"cleanup_complete={process.get('cleanup_complete')!r} "
                 f"abandoned_pipes={process.get('abandoned_pipes')!r}\n"
                 f"{stdout.decode('utf-8', 'replace')}"
                 f"{stderr.decode('utf-8', 'replace')}")
        record["status"] = "passed"
        return stdout
    except BaseException as error:
        record["status"] = report["status"] = "failed"
        record["error"] = repr(error)
        raise
    finally:
        record["elapsed_seconds"] = __import__("time").monotonic() - started
        save_report(report)


def stage_project(work: Path) -> Path:
    project = work / "component"
    source = project / "src"
    source.mkdir(parents=True)
    (source / "wzcc").symlink_to(SELFHOST, target_is_directory=True)
    shutil.copy2(HERE / "src" / "main.wz", source / "main.wz")
    (project / "lang.toml").write_text(MANIFEST, encoding="utf-8")
    return project


def write_fixtures(work: Path) -> None:
    for case in CASES:
        source = case.get("source")
        if source is None:
            continue
        fixture = work / "fixtures" / case["name"]
        (fixture / "src").mkdir(parents=True)
        (fixture / "lang.toml").write_text(MANIFEST, encoding="utf-8")
        (fixture / "src" / "main.wz").write_text(source, encoding="utf-8")


def case_manifest(work: Path, case: dict) -> Path:
    return case.get("manifest", work / "fixtures" / case["name"] / "lang.toml")


def case_source(work: Path, case: dict) -> str:
    manifest = case_manifest(work, case)
    return (manifest.parent / "src" / "main.wz").read_text(encoding="utf-8")


def expected_diagnostic(work: Path, case: dict) -> dict | None:
    code = case.get("code")
    if code is None:
        return None
    source = case_source(work, case)
    primary = case["primary"]
    begin = source.find(primary)
    if begin < 0 or source.find(primary, begin + 1) >= 0:
        fail(f"{case['name']}: primary substring is not unique: {primary!r}")
    return {
        "code": code,
        "phase": "type",
        "severity": "error",
        "message": case["message"],
        "path": str((case_manifest(work, case).parent / "src" / "main.wz").resolve()),
        "begin": begin,
        "end": begin + len(primary),
        "related": 0,
        "fixits": 0,
        "notes": 0,
    }


def parse_driver(stdout: bytes) -> tuple[str, list[dict]]:
    lines = stdout.decode("utf-8").splitlines()
    if not lines or not lines[0].startswith("STATE "):
        fail(f"driver has no STATE line: {stdout!r}")
    state = lines[0]
    diagnostics = []
    for line in lines[1:]:
        if not line.startswith("DIAG "):
            fail(f"unexpected driver line: {line!r}")
        fields = line[len("DIAG "):].split("|")
        if len(fields) != 10:
            fail(f"malformed diagnostic line: {line!r}")
        diagnostics.append({
            "code": fields[0], "phase": fields[1], "severity": fields[2],
            "message": fields[3], "path": fields[4],
            "begin": int(fields[5]), "end": int(fields[6]),
            "related": int(fields[7]), "fixits": int(fields[8]),
            "notes": int(fields[9]),
        })
    return state, diagnostics


def reference_result(reference: Path, manifest: Path, cwd: Path,
                     env: dict[str, str], timeout: float, report: dict,
                     label: str) -> dict | None:
    raw = run([reference, "check", "--project", manifest,
               "--format", "json", "--backend", "native"],
              label, cwd, env, timeout, report, ok_exits=(0, 1))
    document = json.loads(raw)
    status = document.get("status")
    if status not in ("ok", "error"):
        fail(f"{label}: invalid reference status: {status!r}")
    exit_code = report["commands"][-1]["exit"]
    expected_exit = 0 if status == "ok" else 1
    if type(exit_code) is not int or exit_code != expected_exit:
        fail(f"{label}: reference exit {exit_code!r} contradicts status {status!r}")
    diagnostics = document.get("diagnostics", [])
    if status == "ok":
        if diagnostics:
            fail(f"{label}: reference accepted input with diagnostics")
        return None
    if len(diagnostics) != 1:
        fail(f"{label}: reference diagnostic count: {len(diagnostics)}")
    diagnostic = diagnostics[0]
    return {
        "code": diagnostic["code"], "phase": diagnostic["phase"],
        "severity": diagnostic["severity"], "message": diagnostic["message"],
        "path": diagnostic["primary"]["path"],
        "begin": diagnostic["primary"]["start_byte"],
        "end": diagnostic["primary"]["end_byte"],
        "related": len(diagnostic["related"]),
        "fixits": len(diagnostic["fixits"]),
        "notes": len(diagnostic["notes"]),
    }


def check_expected(case: dict, expected: dict | None, state: str,
                   diagnostics: list[dict]) -> None:
    if expected is None:
        if state != "STATE Complete has=1 diags=0" or diagnostics:
            fail(f"{case['name']} was not accepted: {state} {diagnostics}")
        return
    if state != "STATE Failed has=0 diags=1":
        fail(f"{case['name']} state mismatch: {state}")
    if len(diagnostics) != 1:
        fail(f"{case['name']} diagnostic count: {len(diagnostics)}")
    actual = diagnostics[0]
    for key, value in expected.items():
        if actual[key] != value:
            fail(f"{case['name']} {key}: {actual[key]!r} != {value!r}")


def main() -> int:
    args = parse_args()
    stage0 = Path(args.woozyc).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage-0 compiler missing: {stage0}")
    reference = Path(args.reference).expanduser().resolve() if args.reference else None
    if reference is not None and not reference.is_file():
        fail(f"reference compiler missing: {reference}")
    parent = Path(args.work_dir).expanduser().resolve()
    parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="sema-generic-arity-", dir=parent))
    (work / "tmp").mkdir()
    env = dict(os.environ)
    env["TMPDIR"] = str(work / "tmp")
    toolchain = (Path(args.toolchain_root).expanduser().resolve()
                 if args.toolchain_root else stage0.parents[3] / "toolchains")
    paths = [str(toolchain / "llvm-22.1.8" / "lib"),
             str(toolchain / "libxml2-legacy-2.13.9" / "usr" / "lib")]
    if env.get("LD_LIBRARY_PATH"):
        paths.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(paths)
    report = {"scope": "sema generic/method type-argument arity component; not a full CLI/corpus claim",
              "work_dir": str(work), "commands": [], "cases": {},
              "status": "running", "stage0": str(stage0),
              "toolchain_root": str(toolchain),
              "reference": str(reference) if reference is not None else None}
    save_report(report)
    print(f"work-dir: {work}", flush=True)
    try:
        report["source_sha256"] = {
            "b_call.wz": sha256(SELFHOST / "sema" / "b_call.wz"),
            "b_call2.wz": sha256(SELFHOST / "sema" / "b_call2.wz"),
            "b_expr.wz": sha256(SELFHOST / "sema" / "b_expr.wz"),
        }
        report["stage0_sha256"] = sha256(stage0)
        report["reference_sha256"] = sha256(reference) if reference is not None else None
        project = stage_project(work)
        write_fixtures(work)
        expected = {case["name"]: expected_diagnostic(work, case) for case in CASES}
        report["inputs_before"] = input_snapshot(work, stage0, reference)
        save_report(report)

        # Compare diagnostic fields, byte spans and related/fixit/note counts.
        if reference is not None:
            reference_cases = {}
            for case in CASES:
                manifest = case_manifest(work, case)
                actual = reference_result(reference, manifest, work, env,
                                          args.timeout, report,
                                          "reference-" + case["name"])
                if actual != expected[case["name"]]:
                    fail(f"{case['name']} reference: {actual!r} != {expected[case['name']]!r}")
                reference_cases[case["name"]] = actual
            report["reference_cases"] = reference_cases
            print("PASS: C++ expected diagnostics for all cases", flush=True)
        else:
            report["reference_cases"] = "not_run: --reference not supplied"
            print("not_run: C++ expected diagnostics (--reference not supplied)", flush=True)

        out = work / "out-o0"
        run([stage0, "build", "--project", project / "lang.toml",
             "--backend", "native", "--opt", "0", "--out-dir", out,
             "--format", "json"], "build-o0", work, env, args.timeout, report)
        program = out / "program"
        if not program.is_file():
            fail("O0 build produced no program")
        report.setdefault("program_sha256", {})[str(program.resolve())] = sha256(program)
        save_report(report)
        print("PASS: built O0 component driver", flush=True)

        for case in CASES:
            manifest = case_manifest(work, case)
            state, diagnostics = parse_driver(run(
                [program, manifest, STDLIB], "driver-" + case["name"],
                work, env, args.timeout, report))
            check_expected(case, expected[case["name"]], state, diagnostics)
            if reference is not None:
                actual = (diagnostics[0] if diagnostics else None)
                if actual != report["reference_cases"][case["name"]]:
                    fail(f"{case['name']} C++ parity: {actual!r} != "
                         f"{report['reference_cases'][case['name']]!r}")
            report["cases"][case["name"]] = {"state": state,
                                             "diagnostics": diagnostics}
            print(f"PASS: {case['name']}", flush=True)

        report["inputs_after"] = input_snapshot(work, stage0, reference)
        if report["inputs_after"] != report["inputs_before"]:
            fail("inputs changed during the component run")
        report["stage0_sha256_after"] = sha256(stage0)
        if reference is not None:
            report["reference_sha256_after"] = sha256(reference)
        report["status"] = "passed"
    except BaseException as error:
        report["status"] = "failed"
        report["error"] = repr(error)
        if "inputs_before" in report:
            try:
                report["inputs_after"] = input_snapshot(work, stage0, reference)
            except Exception as snapshot_error:
                report["snapshot_error"] = repr(snapshot_error)
        raise
    finally:
        save_report(report)
    print(f"report: {work / 'report.json'}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
