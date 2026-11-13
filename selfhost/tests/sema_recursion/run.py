#!/usr/bin/env python3
"""Prove generic-recursion stack behavior in the real Wz project/resolve/sema.

This component stages the live selfhost frontend and Sema packages, builds a
small driver at O0 and O2, and loads real project manifests through the Wz
project loader.  It does not build the 116-file CLI or claim a full compiler /
corpus result.

Checks:
  * tests/generics/fixtures/expanding-recursion reaches the unchanged 48-level
    instantiation budget and reports the same code/phase/message/span as C++;
  * a finite generic type chain and a same-argument recursive cache hit are
    accepted (the fix does not reject ordinary/finite recursion);
  * O0 and O2 produce identical driver output on all cases;
  * the 4096 instance-count guard remains present and ordered before the depth
    guard (checked against the production source, not reimplemented here).
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
EXPANDING = ROOT / "tests" / "generics" / "fixtures" / "expanding-recursion"
DEFAULT_WOOZYC = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
sys.path.insert(0, str(HERE.parents[1]))
from corpus import _run_process_impl  # noqa: E402

EXPECTED_MAIN = {
    "code": "E_INSTANTIATION_LIMIT",
    "phase": "type",
    "severity": "error",
    "message": "generic instantiation depth budget exceeded (48)",
    "begin": 71,
    "end": 101,
    "related": 0,
    "fixits": 0,
    "notes": 0,
}
CONTROLS = {
    "finite-chain": """package app;
struct Box[T] { value T; }
func base[T](x T) i32 { return 1; }
func outer[T](x T) i32 { return base[Box[T]](Box[T](value: x)); }
func main() i32 { return outer[i32](0); }
""",
    "cache-hit": """package app;
func loop[T](x T) i32 { return loop[T](x); }
func main() i32 { return loop[i32](0); }
""",
}
MANIFEST = """schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "native"
word_bits = 64
"""


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
    parser.add_argument("--work-dir", default=str(ROOT / "build" / "sema-recursion-runs"),
                        help="parent directory for retained run evidence")
    parser.add_argument("--timeout", type=finite_timeout, default=900.0,
                        help="per command timeout in seconds")
    return parser.parse_args()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save_report(report: dict) -> None:
    work = Path(report["work_dir"])
    temporary = work / "report.json.tmp"
    temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    temporary.replace(work / "report.json")


def input_snapshot(work: Path, stage0: Path) -> dict[str, str]:
    files = [stage0, HERE / "run.py", HERE / "src" / "main.wz",
             work / "component" / "lang.toml",
             work / "component" / "src" / "main.wz"]
    for root in (SELFHOST, STDLIB, EXPANDING, work / "fixtures"):
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
        record["inputs_before"] = input_snapshot(cwd, stage0)
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
        record["inputs_after"] = input_snapshot(cwd, stage0)
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


def write_fixtures(work: Path) -> dict[str, Path]:
    result = {}
    for name, source in CONTROLS.items():
        fixture = work / "fixtures" / name
        (fixture / "src").mkdir(parents=True)
        (fixture / "lang.toml").write_text(MANIFEST, encoding="utf-8")
        (fixture / "src" / "main.wz").write_text(source, encoding="utf-8")
        result[name] = fixture / "lang.toml"
    return result


def parse_driver(stdout: bytes) -> tuple[str, list[dict[str, str]]]:
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


def check_main(state: str, diagnostics: list[dict[str, str]]) -> None:
    if state != "STATE Failed has=0 diags=1":
        fail(f"expanding-recursion state mismatch: {state}")
    if len(diagnostics) != 1:
        fail(f"expanding-recursion diagnostic count: {len(diagnostics)}")
    actual = diagnostics[0]
    for key, expected in EXPECTED_MAIN.items():
        if actual[key] != expected:
            fail(f"expanding-recursion {key}: {actual[key]!r} != {expected!r}")
    expected_path = str((EXPANDING / "src" / "main.wz").resolve())
    if actual["path"] != expected_path:
        fail(f"expanding-recursion path: {actual['path']!r}")


def check_control(name: str, state: str, diagnostics: list[dict[str, str]]) -> None:
    if state != "STATE Complete has=1 diags=0" or diagnostics:
        fail(f"{name} was not accepted: {state} {diagnostics}")


def check_source_budgets() -> None:
    text = (SELFHOST / "sema" / "a_instantiate.wz").read_text(encoding="utf-8")
    count = 'if vec.len(state.instance_cache)>=4096 {'
    depth = 'if state.instantiation_depth>=48 {'
    count_message = '"generic instance count budget exceeded (4096)"'
    depth_message = '"generic instantiation depth budget exceeded (48)"'
    for needle, label in ((count, "4096 guard"), (depth, "48 guard"),
                          (count_message, "4096 message"),
                          (depth_message, "48 message")):
        if needle not in text:
            fail(f"production source lost the {label}: {needle}")
    if text.index(count) > text.index(depth):
        fail("instance-count guard moved after the depth guard")


def reference_main(reference: Path, cwd: Path, env: dict[str, str],
                   timeout: float, report: dict) -> dict | None:
    if not str(reference):
        return None
    raw = run([reference, "check", "--project", EXPANDING / "lang.toml",
               "--format", "json", "--backend", "native"],
              "reference-main", cwd, env, timeout, report, ok_exits=(1,))
    document = json.loads(raw)
    if document.get("status") != "error" or len(document.get("diagnostics", [])) != 1:
        fail(f"reference expanding-recursion result: {document}")
    diagnostic = document["diagnostics"][0]
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
    work = Path(tempfile.mkdtemp(prefix="sema-recursion-", dir=parent))
    (work / "tmp").mkdir()
    env = dict(os.environ)
    env["TMPDIR"] = str(work / "tmp")
    toolchain = stage0.parents[3] / "toolchains"
    paths = [str(toolchain / "llvm-22.1.8" / "lib"),
             str(toolchain / "libxml2-legacy-2.13.9" / "usr" / "lib")]
    if env.get("LD_LIBRARY_PATH"):
        paths.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(paths)
    report = {"scope": "sema generic-recursion component; not a full CLI/corpus claim",
              "work_dir": str(work), "commands": [], "cases": {},
              "status": "running", "stage0": str(stage0)}
    save_report(report)
    print(f"work-dir: {work}", flush=True)
    try:
        report["source_sha256"] = sha256(SELFHOST / "sema" / "a_instantiate.wz")
        report["stage0_sha256"] = sha256(stage0)
        project = stage_project(work)
        fixtures = write_fixtures(work)
        report["inputs_before"] = input_snapshot(work, stage0)
        save_report(report)
        check_source_budgets()
        print("PASS: source budgets 4096/48 present and ordered", flush=True)

        outputs = {}
        for opt in (0, 2):
            out = work / f"out-o{opt}"
            run([stage0, "build", "--project", project / "lang.toml",
                 "--backend", "native", "--opt", str(opt), "--out-dir", out,
                 "--format", "json"], f"build-o{opt}", work, env, args.timeout, report)
            program = out / "program"
            if not program.is_file():
                fail(f"O{opt} build produced no program")
            report.setdefault("program_sha256", {})[str(program.resolve())] = sha256(program)
            save_report(report)
            case_outputs = {}
            state, diagnostics = parse_driver(run(
                [program, EXPANDING / "lang.toml", STDLIB],
                f"main-o{opt}", work, env, args.timeout, report))
            check_main(state, diagnostics)
            case_outputs["expanding-recursion"] = {"state": state,
                                                   "diagnostics": diagnostics}
            for name, manifest in fixtures.items():
                state, diagnostics = parse_driver(run(
                    [program, manifest, STDLIB], f"{name}-o{opt}", work, env,
                    args.timeout, report))
                check_control(name, state, diagnostics)
                case_outputs[name] = {"state": state, "diagnostics": diagnostics}
            outputs[opt] = case_outputs
            print(f"PASS: O{opt} expanding-recursion and controls", flush=True)

        if outputs[0] != outputs[2]:
            fail("O0 and O2 component output differs")
        report["cases"] = outputs[0]
        print("PASS: O0/O2 identical component output", flush=True)

        expected = reference_main(reference, work, env, args.timeout, report) \
            if reference is not None else None
        if expected is not None:
            actual = outputs[0]["expanding-recursion"]["diagnostics"][0]
            for key in (*EXPECTED_MAIN, "path"):
                if actual[key] != expected[key]:
                    fail(f"C++ parity {key}: {actual[key]!r} != {expected[key]!r}")
            report["reference_main"] = expected
            print("PASS: C++ diagnostic parity (code/phase/message/span/counts)",
                  flush=True)
        else:
            report["reference_main"] = "not_run: --reference not supplied"
            print("not_run: C++ diagnostic parity (--reference not supplied)", flush=True)
        report["inputs_after"] = input_snapshot(work, stage0)
        if report["inputs_after"] != report["inputs_before"]:
            fail("inputs changed during the component run")
        report["stage0_sha256_after"] = sha256(stage0)
        report["status"] = "passed"
    except BaseException as error:
        report["status"] = "failed"
        report["error"] = repr(error)
        if "inputs_before" in report:
            try:
                report["inputs_after"] = input_snapshot(work, stage0)
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
