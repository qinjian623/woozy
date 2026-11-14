#!/usr/bin/env python3
"""Build, run and observe real Wz `Slice[T]` programs on the Python backend.

Each accepted fixture is compiled through two real entry points in the same
implementation group:

* reference: the C++ ``woozyc build --backend python`` CLI and the C++
  ``lang_python_emit`` helper (legacy ``--cli``/``--helper`` invocation);
* candidate: the public pure-Wz CLI and a small Wz API driver that calls
  ``project.load`` -> Python selection -> ``resolve.resolve_project`` ->
  ``sema.check_project(Backend)`` -> ``pythonemit.emit``.

The generated ``program.py``/``langrt.py`` hashes must match inside a group.
Cross-implementation text is not forced to be identical.  Programs then run
under the unchanged ``observe.py``, which disables CPython's automatic
collector, forbids ``gc.collect``, and checks logical Vec retain/release,
Slice pin/end, descriptor drop/retirement and final live roots.

Every subprocess goes through the existing bounded process-group helper in
``selfhost/corpus.py``.  The summary records the real argv/cwd/exit, timeout,
cleanup and stream hashes.  A new ``--out`` directory is required so previous
evidence is never overwritten.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import shutil
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
COMPILER_ROOT = HERE.parents[2]
SELFHOST = COMPILER_ROOT / "selfhost"
sys.path.insert(0, str(SELFHOST))
from corpus import TraceError, _run_process_impl, audit_trace  # noqa: E402

POSITIVE = ("python-slice", "core", "owned", "cleanup", "throws-return",
            "after-drop", "empty", "move", "nested-to-vec", "for-in",
            "compound", "reassign", "gc-cycle")
PANIC = {
    "structural-push": "Vec length changes during iteration",
    "structural-pop": "Vec length changes during iteration",
    "structural-clear": "Vec length changes during iteration",
    "structural-resize": "Vec length changes during iteration",
    "structural-reserve": "Vec storage changes during iteration",
    "empty-still-pins": "Vec length changes during iteration",
    "negative-index": "Slice index out of bounds",
    "huge-index": "Slice index out of bounds",
    "bounds-order": "Slice range out of bounds",
    "bounds-high": "Slice range out of bounds",
    "sub-bounds": "Slice range out of bounds",
    "negative-sub": "Slice start out of bounds",
    "huge-sub": "Slice range out of bounds",
    "huge-view": "Slice range out of bounds",
}
NEGATIVE = {"unsupported-c": "E_CAPABILITY_UNAVAILABLE"}

RUNTIME_VERSION = "compiler-dev/1-python-values/10"
RUNTIME_VERSION_LINE = "RUNTIME_VERSION = 'compiler-dev/1-python-values/10'"
LEGACY_CMAKE_TIMEOUT_SECONDS = 180
REQUIRED_OBSERVER_KEYS = {
    "status", "exit", "escaped", "program_stdout", "program_stderr",
    "counts", "stats", "live_vectors", "blank_to_vec_typed",
    "identity_regressions",
}

# Exact logical facts selected per fixture.  The observer already checks the
# global balance invariants for every positive program; these rows add the
# operation counts that distinguish view/copy/to-vec/GC paths.
EXPECTED = {
    "python-slice": {"slice_create": 1},
    "core": {"slice_to_vec": 1, "min_create": 6, "allocations": 4},
    "owned": {"slice_to_vec": 2, "min_create": 4},
    "cleanup": {"min_create": 3},
    "throws-return": {"min_create": 3},
    "after-drop": {"min_create": 2},
    "empty": {"slice_to_vec": 1, "min_create": 3},
    "move": {"min_create": 2},
    "nested-to-vec": {"slice_to_vec": 1, "min_create": 1},
    "for-in": {"min_create": 2},
    "compound": {"min_create": 1},
    "reassign": {"min_create": 2},
    "gc-cycle": {"min_create": 1, "min_retire": 1, "min_collected": 1},
}


def finite_timeout(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "--timeout must be finite and positive") from error
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("--timeout must be finite and positive")
    return value


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def decode_bytes(data: bytes) -> str:
    return data.decode("utf-8", errors="replace")


def label_name(text: str) -> str:
    return "".join(character if character.isalnum() or character in "-_."
                   else "_" for character in text)


def expected_case_keys() -> list[str]:
    keys = [f"positive/{name}" for name in POSITIVE]
    keys.extend(f"panic/{name}" for name in PANIC)
    keys.extend(f"negative/{name}" for name in NEGATIVE)
    return keys


def blank_case_states() -> dict[str, dict]:
    return {key: {"status": "not_run"} for key in expected_case_keys()}


def verify_inventory(root: Path) -> None:
    fixtures = root / "tests/python/slice_source/fixtures"
    missing = []
    for name in POSITIVE + tuple(PANIC) + tuple(NEGATIVE):
        manifest = fixtures / name / "lang.toml"
        sources = sorted((fixtures / name / "src").glob("*.wz"))
        if not manifest.is_file() or not sources:
            missing.append(name)
    if missing:
        raise RuntimeError(f"missing fixture(s): {', '.join(missing)}")


def prepare_output(out: Path) -> None:
    if out.exists():
        if not out.is_dir():
            raise RuntimeError(f"--out is not a directory: {out}")
        if any(out.iterdir()):
            raise RuntimeError(
                f"refusing to overwrite existing evidence in --out: {out}")
    out.mkdir(parents=True, exist_ok=True)


def environment(toolchain_root: Path | None, work: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONNOUSERSITE"] = "1"
    env.pop("PYTHONPATH", None)
    if toolchain_root is not None and toolchain_root.is_dir():
        paths = []
        llvm = toolchain_root / "llvm-22.1.8" / "lib"
        libxml = toolchain_root / "libxml2-legacy-2.13.9" / "usr" / "lib"
        if llvm.is_dir():
            paths.append(str(llvm))
        if libxml.is_dir():
            paths.append(str(libxml))
        if env.get("LD_LIBRARY_PATH"):
            paths.append(env["LD_LIBRARY_PATH"])
        env["LD_LIBRARY_PATH"] = ":".join(paths)
    tmp = work / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    env["TMPDIR"] = str(tmp)
    return env


def run_command(argv, cwd: Path, timeout: float, out: Path,
                log: list[dict], label: str, env: dict[str, str],
                strace: Path | None = None,
                trace_prefix: str = "") -> dict:
    """Run one real command with the shared bounded process-group helper."""
    if not math.isfinite(timeout) or timeout <= 0:
        raise RuntimeError(f"{label}: timeout must be finite and positive")
    logs = out / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    sequence = len(log)
    stem = f"{sequence:03d}-{label_name(label)}"
    stdout_path = logs / f"{stem}.stdout"
    stderr_path = logs / f"{stem}.stderr"
    trace_path = None
    real_argv = [str(part) for part in argv]
    if strace is not None:
        traces = out / "traces"
        traces.mkdir(parents=True, exist_ok=True)
        trace_path = traces / f"{stem}{trace_prefix}.trace"
        real_argv = [str(strace), "--seccomp-bpf", "-f", "-qq", "-s",
                     "16384", "-e", "trace=execve,execveat", "-o",
                     str(trace_path), *real_argv]
    started = time.monotonic()
    process = _run_process_impl(real_argv, cwd, env, timeout)
    elapsed = time.monotonic() - started
    stdout_path.write_bytes(process["stdout"])
    stderr_path.write_bytes(process["stderr"])
    record = {
        "label": label,
        "argv": real_argv,
        "cwd": str(cwd),
        "timeout_seconds": timeout,
        "elapsed_seconds": elapsed,
        "exit": process["exit"],
        "timed_out": process["timed_out"],
        "cleanup_complete": process["cleanup_complete"],
        "abandoned_pipes": process["abandoned_pipes"],
        "stdout_path": str(stdout_path),
        "stderr_path": str(stderr_path),
        "stdout_sha256": hashlib.sha256(process["stdout"]).hexdigest(),
        "stderr_sha256": hashlib.sha256(process["stderr"]).hexdigest(),
        "stdout": decode_bytes(process["stdout"]),
        "stderr": decode_bytes(process["stderr"]),
    }
    if trace_path is not None:
        record["trace_path"] = str(trace_path)
        record["trace_sha256"] = digest(trace_path) if trace_path.is_file() else ""
    log.append(record)
    # Persist every step immediately, not only in the final summary.  A later
    # crash must not lose earlier argv/flags/streams/trace records.
    with (out / "commands.jsonl").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(record, sort_keys=True) + "\n")
    return record


def require_ok(record: dict, expected_exit: int = 0) -> None:
    if (type(record.get("exit")) is not int
            or record["exit"] != expected_exit
            or record.get("timed_out") is not False
            or record.get("cleanup_complete") is not True
            or record.get("abandoned_pipes") is not False):
        raise RuntimeError(
            f"{record['label']}: exit {record['exit']} "
            f"timed_out={record['timed_out']} "
            f"cleanup_complete={record['cleanup_complete']} "
            f"abandoned_pipes={record['abandoned_pipes']}\n"
            f"stdout:\n{record['stdout']}\nstderr:\n{record['stderr']}")


def parse_json(record: dict) -> dict:
    try:
        return json.loads(record["stdout"])
    except json.JSONDecodeError as error:
        raise RuntimeError(
            f"{record['label']}: expected JSON, got {record['stdout']!r}: "
            f"{error}") from error


def audit_candidate_trace(record: dict, allowed: set[str]) -> dict:
    """Audit one candidate compile trace with corpus's exact whitelist."""
    trace = record.get("trace_path")
    if not trace:
        raise RuntimeError(f"{record['label']}: candidate compile was not traced")
    try:
        result = audit_trace(Path(trace), allowed)
    except TraceError as error:
        raise RuntimeError(
            f"{record['label']}: exact exec whitelist rejected trace: {error}") from error
    record["trace_audit"] = result
    return result


def check_runtime(directory: Path, label: str) -> None:
    runtime = directory / "langrt.py"
    if not runtime.is_file():
        raise RuntimeError(f"{label}: no generated langrt.py")
    text = runtime.read_text(encoding="utf-8")
    if RUNTIME_VERSION_LINE not in text:
        raise RuntimeError(
            f"{label}: generated runtime is not the expected version 10")


def entry_hashes(directory: Path, kind: str, record: dict) -> dict:
    program = directory / "program.py"
    runtime = directory / "langrt.py"
    if not program.is_file() or not runtime.is_file():
        raise RuntimeError(f"{kind} build did not publish both files")
    check_runtime(directory, kind)
    return {
        "kind": kind,
        "path": str(directory),
        "program_sha256": digest(program),
        "runtime_sha256": digest(runtime),
        "command": record["label"],
    }


def build_cli(cli: Path, manifest: Path, directory: Path, timeout: float,
              out: Path, log: list[dict], label: str, env: dict[str, str],
              strace: Path | None = None,
              trace_prefix: str = "") -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    record = run_command(
        [cli, "build", "--project", manifest, "--backend", "python",
         "--out-dir", directory, "--format", "json"],
        out, timeout, out, log, label, env, strace, trace_prefix)
    require_ok(record)
    if strace is not None:
        audit_candidate_trace(record, {str(cli.resolve())})
    document = parse_json(record)
    if document.get("status") != "ok":
        raise RuntimeError(f"{label}: build did not succeed: {document}")
    if document.get("diagnostics"):
        raise RuntimeError(f"{label}: CLI diagnostics: {document}")
    return entry_hashes(directory, "cli", record)


def build_helper(helper: Path, manifest: Path, directory: Path, timeout: float,
                 out: Path, log: list[dict], label: str,
                 env: dict[str, str]) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    record = run_command(
        [helper, "--project", manifest, "--out-dir", directory],
        out, timeout, out, log, label, env)
    require_ok(record)
    document = parse_json(record)
    if document.get("status") != "ok":
        raise RuntimeError(f"{label}: helper build failed: {document}")
    if document.get("diagnostics"):
        raise RuntimeError(f"{label}: helper diagnostics: {document}")
    return entry_hashes(directory, "helper", record)


def build_api(driver: Path, manifest: Path, directory: Path, timeout: float,
              out: Path, log: list[dict], label: str, env: dict[str, str],
              strace: Path | None = None,
              trace_prefix: str = "") -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    record = run_command(
        [driver, "--project", manifest, "--out-dir", directory,
         "--format", "json"],
        out, timeout, out, log, label, env, strace, trace_prefix)
    require_ok(record)
    if strace is not None:
        audit_candidate_trace(record, {str(driver.resolve())})
    document = parse_json(record)
    if document.get("status") != "ok":
        raise RuntimeError(f"{label}: API driver failed: {document}")
    return entry_hashes(directory, "api", record)


def compare_entries(first: dict, second: dict, label: str) -> None:
    for name in ("program.py", "langrt.py"):
        left = Path(first["path"]) / name
        right = Path(second["path"]) / name
        if digest(left) != digest(right):
            raise RuntimeError(
                f"{label}: {first['kind']}/{second['kind']} disagree on "
                f"generated {name} "
                f"({digest(left)} != {digest(right)})")


def run_observer(python: Path, root: Path, cli_directory: Path, panic: bool,
                 timeout: float, out: Path, log: list[dict], label: str,
                 env: dict[str, str], result_path: Path,
                 observer: Path | None = None) -> dict:
    script = observer or (root / "tests/python/slice_source/observe.py")
    argv = [python, "-s", script, cli_directory]
    if panic:
        argv.append("panic")
    record = run_command(argv, out, timeout, out, log, label, env)
    require_ok(record)
    document = parse_json(record)
    missing = REQUIRED_OBSERVER_KEYS - set(document)
    if missing:
        raise RuntimeError(
            f"{label}: observer document missing {sorted(missing)}: {document}")
    if document.get("status") != "passed":
        raise RuntimeError(f"{label}: observer rejected output: {document}")
    write_json(result_path, document)
    return document


def check_positive(name, observed):
    expected = EXPECTED[name]
    if observed["exit"] != 0:
        raise RuntimeError(f"{name}: unexpected exit {observed['exit']}")
    if observed["program_stdout"] or observed["program_stderr"]:
        raise RuntimeError(f"{name}: program produced output")
    if observed.get("escaped") is not None:
        raise RuntimeError(f"{name}: uncaught Python exception {observed['escaped']}")
    if not observed.get("identity_regressions"):
        raise RuntimeError(f"{name}: descriptor identity regressions failed")
    if not observed.get("blank_to_vec_typed"):
        raise RuntimeError(f"{name}: blank to_vec typed fallback failed")
    stats, counts = observed["stats"], observed["counts"]
    if stats["active"] is not False or stats["busy"] != 0:
        raise RuntimeError(f"{name}: collector still active {stats}")
    if stats["live"] != 0 or observed["live_vectors"] != 0:
        raise RuntimeError(f"{name}: live language roots at exit")
    if "slice_to_vec" in expected:
        if counts["slice_to_vec"] != expected["slice_to_vec"]:
            raise RuntimeError(f"{name}: to_vec count {counts}")
    if "min_create" in expected and counts["slice_create"] < expected["min_create"]:
        raise RuntimeError(f"{name}: too few Slice creations {counts}")
    if "min_retire" in expected and counts["slice_retire"] < expected["min_retire"]:
        raise RuntimeError(f"{name}: no collected Slice edge {counts}")
    if "min_collected" in expected and stats["total_collected"] < expected["min_collected"]:
        raise RuntimeError(f"{name}: collector freed nothing {stats}")
    if "allocations" in expected and stats["allocations"] != expected["allocations"]:
        raise RuntimeError(f"{name}: Vec allocation count {stats}")
    for key, value in expected.items():
        if key in ("slice_create", "slice_copy", "slice_drop",
                   "slice_begin", "slice_end"):
            if counts[key] != value:
                raise RuntimeError(f"{name}: {key}={counts[key]} != {value}")


def check_panic(name, observed, message):
    if observed["exit"] != 101:
        raise RuntimeError(f"{name}: expected Panic exit 101, got {observed['exit']}")
    if observed["program_stdout"]:
        raise RuntimeError(f"{name}: Panic wrote stdout")
    if f"LANG_PANIC: {message}\n" != observed["program_stderr"]:
        raise RuntimeError(f"{name}: wrong Panic record {observed['program_stderr']!r}")
    if observed.get("escaped") is not None:
        raise RuntimeError(f"{name}: uncaught Python exception {observed['escaped']}")
    counts = observed["counts"]
    if counts["slice_create"] < 1:
        raise RuntimeError(f"{name}: no attempted view creation {counts}")
    if counts["slice_drop"] or counts["slice_retire"]:
        raise RuntimeError(f"{name}: Panic performed cleanup {counts}")


def check_negative(record: dict, document: dict, code: str, directory: Path,
                   label: str) -> dict:
    if record["exit"] == 0:
        raise RuntimeError(f"{label}: negative build unexpectedly succeeded")
    if document.get("status") == "ok":
        raise RuntimeError(f"{label}: negative document says ok: {document}")
    codes = [item.get("code") for item in document.get("diagnostics", [])]
    if code not in codes:
        raise RuntimeError(f"{label}: expected {code}, got {codes}")
    if (directory / "program.py").exists():
        raise RuntimeError(f"{label}: generated a program")
    return {"exit": record["exit"], "codes": codes,
            "diagnostics": document.get("diagnostics", [])}


def source_hashes(root: Path, fixtures: Path, name: str,
                  hashes: dict[str, str]) -> dict[str, str]:
    found = {}
    for path in sorted((fixtures / name / "src").glob("*.wz")):
        key = str(path.relative_to(root))
        value = digest(path)
        hashes[key] = value
        found[key] = value
    return found


def record_generated(directory: Path, out: Path, hashes: dict[str, str]) -> dict:
    found = {}
    for name in ("program.py", "langrt.py"):
        path = directory / name
        key = str(path.relative_to(out))
        value = digest(path)
        hashes[key] = value
        found[name] = value
    return found


def run_group(group: str, args, out: Path, driver: Path | None,
              log: list[dict], env: dict[str, str],
              case_states: dict[str, dict], strace: Path | None) -> dict:
    fixtures = args.root / "tests/python/slice_source/fixtures"
    hashes: dict[str, str] = {}
    result = {
        "name": group,
        "status": "failed",
        "complete": False,
        "error": None,
        "positive": {},
        "panics": {},
        "negative": {},
        "hashes": hashes,
        "cases": case_states,
    }
    if group == "reference":
        first_cli = args.cli
        second = "helper" if args.helper is not None else None
        complete = args.helper is not None
    else:
        first_cli = args.candidate_cli
        second = "api"
        complete = driver is not None
    current = None
    try:
        for name in POSITIVE:
            if args.only and name not in args.only:
                continue
            current = f"positive/{name}"
            case_states[current] = {"status": "running"}
            case_dir = out / group / "positive" / name
            case_dir.mkdir(parents=True, exist_ok=True)
            manifest = fixtures / name / "lang.toml"
            trace = strace if group == "candidate" else None
            first = build_cli(
                first_cli, manifest, case_dir / "cli", args.timeout, out, log,
                f"{group}-positive-{name}-cli", env, trace,
                f"-positive-{name}")
            second_entry = None
            if second == "helper":
                second_entry = build_helper(
                    args.helper, manifest, case_dir / "helper", args.timeout,
                    out, log, f"{group}-positive-{name}-helper", env)
            elif second == "api":
                second_entry = build_api(
                    driver, manifest, case_dir / "api", args.timeout, out, log,
                    f"{group}-positive-{name}-api", env, trace,
                    f"-positive-{name}")
            if second_entry is not None:
                compare_entries(first, second_entry, f"{name} positive")
            observed = run_observer(
                args.python, args.root, case_dir / "cli", False, args.timeout,
                out, log, f"{group}-positive-{name}-observer", env,
                case_dir / "observer.json")
            check_positive(name, observed)
            entries = {"cli": first}
            if second_entry is not None:
                entries[second] = second_entry
            result["positive"][name] = {
                "generated": first,
                "entries": entries,
                "counts": observed["counts"],
                "stats": observed["stats"],
                "identity_regressions": observed["identity_regressions"],
                "blank_to_vec_typed": observed["blank_to_vec_typed"],
                "observer": str(case_dir / "observer.json"),
                "sources": source_hashes(args.root, fixtures, name, hashes),
                "generated_hashes": {
                    kind: record_generated(Path(entry["path"]), out, hashes)
                    for kind, entry in entries.items()
                },
            }
            write_json(case_dir / "result.json", result["positive"][name])
            case_states[current] = {"status": "passed"}
            current = None

        for name, message in PANIC.items():
            if args.only and name not in args.only:
                continue
            current = f"panic/{name}"
            case_states[current] = {"status": "running"}
            case_dir = out / group / "panic" / name
            case_dir.mkdir(parents=True, exist_ok=True)
            manifest = fixtures / name / "lang.toml"
            trace = strace if group == "candidate" else None
            first = build_cli(
                first_cli, manifest, case_dir / "cli", args.timeout, out, log,
                f"{group}-panic-{name}-cli", env, trace,
                f"-panic-{name}")
            second_entry = None
            if second == "helper":
                second_entry = build_helper(
                    args.helper, manifest, case_dir / "helper", args.timeout,
                    out, log, f"{group}-panic-{name}-helper", env)
            elif second == "api":
                second_entry = build_api(
                    driver, manifest, case_dir / "api", args.timeout, out, log,
                    f"{group}-panic-{name}-api", env, trace,
                    f"-panic-{name}")
            if second_entry is not None:
                compare_entries(first, second_entry, f"{name} panic")
            observed = run_observer(
                args.python, args.root, case_dir / "cli", True, args.timeout,
                out, log, f"{group}-panic-{name}-observer", env,
                case_dir / "observer.json")
            check_panic(name, observed, message)
            entries = {"cli": first}
            if second_entry is not None:
                entries[second] = second_entry
            result["panics"][name] = {
                "generated": first,
                "entries": entries,
                "message": message,
                "counts": observed["counts"],
                "observer": str(case_dir / "observer.json"),
                "sources": source_hashes(args.root, fixtures, name, hashes),
                "generated_hashes": {
                    kind: record_generated(Path(entry["path"]), out, hashes)
                    for kind, entry in entries.items()
                },
            }
            write_json(case_dir / "result.json", result["panics"][name])
            case_states[current] = {"status": "passed"}
            current = None

        for name, code in NEGATIVE.items():
            if args.only and name not in args.only:
                continue
            current = f"negative/{name}"
            case_states[current] = {"status": "running"}
            case_dir = out / group / "negative" / name
            manifest = fixtures / name / "lang.toml"
            cli_dir = case_dir / "cli"
            cli_dir.mkdir(parents=True, exist_ok=True)
            trace = strace if group == "candidate" else None
            cli_record = run_command(
                [first_cli, "build", "--project", manifest, "--backend",
                 "python", "--out-dir", cli_dir, "--format", "json"],
                out, args.timeout, out, log,
                f"{group}-negative-{name}-cli", env, trace,
                f"-negative-{name}")
            if cli_record["timed_out"] or not cli_record["cleanup_complete"] \
                    or cli_record["abandoned_pipes"]:
                raise RuntimeError(
                    f"{group}-negative-{name}-cli: incomplete process cleanup")
            if trace is not None:
                audit_candidate_trace(cli_record, {str(first_cli.resolve())})
            cli_document = parse_json(cli_record)
            cli_checked = check_negative(
                cli_record, cli_document, code, cli_dir,
                f"{group}-negative-{name}-cli")

            second_checked = None
            second_entry = None
            if second == "helper":
                helper_dir = case_dir / "helper"
                helper_dir.mkdir(parents=True, exist_ok=True)
                helper_record = run_command(
                    [args.helper, "--project", manifest, "--out-dir",
                     helper_dir], out, args.timeout, out, log,
                    f"{group}-negative-{name}-helper", env)
                if helper_record["timed_out"] or \
                        not helper_record["cleanup_complete"] or \
                        helper_record["abandoned_pipes"]:
                    raise RuntimeError(
                        f"{group}-negative-{name}-helper: incomplete cleanup")
                helper_document = parse_json(helper_record)
                second_checked = check_negative(
                    helper_record, helper_document, code, helper_dir,
                    f"{group}-negative-{name}-helper")
                second_entry = "helper"
            elif second == "api":
                api_dir = case_dir / "api"
                api_dir.mkdir(parents=True, exist_ok=True)
                api_record = run_command(
                    [driver, "--project", manifest, "--out-dir", api_dir,
                     "--format", "json"], out, args.timeout, out, log,
                    f"{group}-negative-{name}-api", env, trace,
                    f"-negative-{name}")
                if api_record["timed_out"] or \
                        not api_record["cleanup_complete"] or \
                        api_record["abandoned_pipes"]:
                    raise RuntimeError(
                        f"{group}-negative-{name}-api: incomplete cleanup")
                if trace is not None:
                    audit_candidate_trace(api_record, {str(driver.resolve())})
                api_document = parse_json(api_record)
                second_checked = check_negative(
                    api_record, api_document, code, api_dir,
                    f"{group}-negative-{name}-api")
                second_entry = "api"
            result["negative"][name] = {
                "code": code,
                "cli": cli_checked,
                "second_kind": second_entry,
                "second": second_checked,
                "sources": source_hashes(args.root, fixtures, name, hashes),
            }
            write_json(case_dir / "result.json", result["negative"][name])
            case_states[current] = {"status": "passed"}
            current = None

        for path in sorted((args.root / "stdlib/slice").glob("*.wz")):
            hashes[str(path.relative_to(args.root))] = digest(path)
        result["status"] = "passed" if complete else "partial"
        result["complete"] = complete
        if not complete:
            result["error"] = (
                "reference helper/API entry absent; only the CLI entry and "
                "observer ran")
    except Exception as error:
        result["status"] = "failed"
        result["error"] = str(error)
        if current is not None:
            case_states[current] = {"status": "failed", "error": str(error)}
    finally:
        result["cases"] = case_states
    return result


def candidate_resources(args) -> Path:
    if args.candidate_resources is not None:
        return args.candidate_resources
    if args.candidate_cli is not None:
        return args.candidate_cli.parent
    raise RuntimeError("candidate resources require --candidate-cli")


def check_resources(resources: Path, label: str) -> None:
    for relative in ("stdlib", "runtime/python/unicode18.py",
                     "runtime/python/langrt.py"):
        path = resources / relative
        if not path.exists():
            raise RuntimeError(f"{label}: missing resource {path}")


def stage_driver_project(root: Path, source_root: Path, driver_source: Path,
                         resources: Path, work: Path) -> Path:
    project = work / "driver" / "project"
    (project / "src").mkdir(parents=True, exist_ok=True)
    wzcc = project / "src" / "wzcc"
    std = project / "src" / "std"
    if wzcc.exists() or wzcc.is_symlink():
        wzcc.unlink()
    if std.exists() or std.is_symlink():
        std.unlink()
    try:
        wzcc.symlink_to(source_root / "wzcc", target_is_directory=True)
        std.symlink_to(resources / "stdlib", target_is_directory=True)
    except OSError:
        shutil.copytree(source_root / "wzcc", wzcc)
        shutil.copytree(resources / "stdlib", std)
    shutil.copy2(driver_source / "src" / "main.wz", project / "src" / "main.wz")
    shutil.copy2(driver_source / "lang.toml", project / "lang.toml")
    return project


def prepare_driver(args, out: Path, resources: Path, log: list[dict],
                   env: dict[str, str]) -> tuple[Path, dict]:
    driver_root = out / "driver" / "root"
    driver_root.mkdir(parents=True, exist_ok=True)
    info = {"resources": str(resources)}
    if args.api_driver is not None:
        source_driver = args.api_driver
        info["built"] = False
    else:
        if args.driver_compiler is None:
            raise RuntimeError(
                "candidate API mode needs --api-driver or --driver-compiler")
        driver_source = args.driver_source or (
            args.root / "selfhost/tests/pythonobs")
        project = stage_driver_project(
            args.root, args.source_root, driver_source, resources, out)
        build_dir = out / "driver" / "build"
        build_dir.mkdir(parents=True, exist_ok=True)
        record = run_command(
            [args.driver_compiler, "build", "--project",
             project / "lang.toml", "--backend", "native", "--opt", "0",
             "--out-dir", build_dir, "--format", "json"],
            project, args.build_timeout, out, log, "driver-build", env)
        require_ok(record)
        document = parse_json(record)
        if document.get("status") != "ok":
            raise RuntimeError(f"driver build failed: {document}")
        source_driver = build_dir / "program"
        info["built"] = True
        info["compiler"] = str(args.driver_compiler)
        info["compiler_sha256"] = digest(args.driver_compiler)
        info["source"] = str(driver_source)
        info["project"] = str(project)
        info["build_command"] = record["label"]
    if not source_driver.is_file():
        raise RuntimeError(f"API driver missing: {source_driver}")
    target = driver_root / "pythonobs"
    shutil.copy2(source_driver, target)
    target.chmod(0o755)
    for name in ("stdlib", "runtime"):
        link = driver_root / name
        if link.exists() or link.is_symlink():
            link.unlink()
        try:
            link.symlink_to(resources / name, target_is_directory=True)
        except OSError:
            shutil.copytree(resources / name, link)
    info["path"] = str(target)
    info["sha256"] = digest(target)
    return target, info


def snapshot_inputs(args, resources: Path | None) -> dict:
    paths = set()
    # Frozen Wz package source actually used to build the test driver.  The
    # package root has 116 files; the driver itself compiles the 115 wzcc
    # packages plus its own driver main, not the package's top-level main.
    for path in args.source_root.glob("**/*.wz"):
        if path.is_file():
            paths.add(path)
    source_manifest = args.source_root.parent / "lang.toml"
    if source_manifest.is_file():
        paths.add(source_manifest)
    for pattern in (
        "stdlib/**/*.wz",
        "runtime/python/*.py",
        "tests/python/slice_source/fixtures/**/*.wz",
        "tests/python/slice_source/fixtures/**/lang.toml",
        "selfhost/tests/pythonobs/**/*.wz",
        "selfhost/tests/pythonobs/lang.toml",
    ):
        for path in args.root.glob(pattern):
            if path.is_file():
                paths.add(path)
    paths.add(HERE / "observe.py")
    paths.add(Path(__file__).resolve())
    for tool in (args.cli, args.helper, args.candidate_cli, args.api_driver,
                 args.driver_compiler, args.python, args.strace):
        if tool is not None and tool.is_file():
            paths.add(tool)
    if resources is not None:
        for path in (resources / "stdlib").rglob("*.wz"):
            if path.is_file():
                paths.add(path)
        for path in (resources / "runtime").rglob("*.py"):
            if path.is_file():
                paths.add(path)
    # The seed build directory owns the native helper/runtime archives and the
    # configure inputs actually used by the one C++ seed build.
    if args.driver_compiler is not None:
        seed = args.driver_compiler.parent
        for relative in (
            "build.ninja",
            "CMakeCache.txt",
            "native/lang_native_helper",
            "native/liblang_native_runtime.a",
            "native/liblang_hosted_runtime.a",
        ):
            path = seed / relative
            if path.is_file():
                paths.add(path)
    # Fixed LLVM/libxml tools named by the build.  These are read-only.
    if args.toolchain_root is not None:
        for relative in (
            "llvm-22.1.8/bin/opt",
            "llvm-22.1.8/bin/llc",
            "llvm-22.1.8/bin/clang",
            "llvm-22.1.8/bin/clang++",
            "llvm-22.1.8/bin/ld.lld",
            "libxml2-legacy-2.13.9/usr/lib/libxml2.so.2.13.9",
        ):
            path = args.toolchain_root / relative
            if path.is_file():
                paths.add(path)
    return {str(path): digest(path) for path in sorted(paths)}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=None,
                        help="reference C++ woozyc CLI (legacy --cli)")
    parser.add_argument("--helper", type=Path, default=None,
                        help="reference C++ lang_python_emit helper")
    parser.add_argument("--candidate-cli", type=Path, default=None,
                        help="candidate pure-Wz CLI for --backend python")
    parser.add_argument("--api-driver", type=Path, default=None,
                        help="prebuilt Wz API driver (skips driver build)")
    parser.add_argument("--driver-compiler", type=Path, default=None,
                        help="C++ seed compiler used once to build the Wz API "
                             "driver at O0")
    parser.add_argument("--driver-source", type=Path, default=None,
                        help="Wz API driver project directory")
    parser.add_argument("--source-root", type=Path, default=None,
                        help="frozen Wz source package root (contains wzcc/)")
    parser.add_argument("--candidate-resources", type=Path, default=None,
                        help="candidate stdlib+runtime directory "
                             "(default: candidate CLI directory)")
    parser.add_argument("--strace", type=Path, default=None,
                        help="strace used to audit candidate CLI/API execs")
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--timeout", type=finite_timeout, default=180.0)
    parser.add_argument("--build-timeout", type=finite_timeout, default=1800.0)
    parser.add_argument("--toolchain-root", type=Path, default=None)
    parser.add_argument("--only", default="",
                        help="comma-separated case names for a partial preflight")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    for key in ("cli", "helper", "candidate_cli", "api_driver",
                "driver_compiler", "driver_source", "source_root",
                "candidate_resources", "strace", "root", "out", "python",
                "toolchain_root"):
        value = getattr(args, key)
        if value is not None:
            if not value.is_absolute():
                found = shutil.which(str(value))
                value = Path(found) if found else value
            setattr(args, key, value.resolve())
    if args.toolchain_root is None:
        args.toolchain_root = args.root.parent / "toolchains"
    if args.source_root is None:
        args.source_root = args.root / "selfhost/src"
    args.only = {item for item in args.only.split(",") if item}
    unknown = args.only - set(POSITIVE) - set(PANIC) - set(NEGATIVE)
    if unknown:
        raise RuntimeError(f"unknown --only cases: {sorted(unknown)}")
    reference_requested = args.cli is not None
    candidate_requested = (args.candidate_cli is not None
                           or args.api_driver is not None)
    if not reference_requested and not candidate_requested:
        raise RuntimeError(
            "nothing to run: pass --cli/--helper and/or "
            "--candidate-cli with --api-driver/--driver-compiler")
    if candidate_requested and args.candidate_cli is None:
        raise RuntimeError("candidate mode requires --candidate-cli")
    if candidate_requested and args.api_driver is None \
            and args.driver_compiler is None:
        raise RuntimeError(
            "candidate API entry requires --api-driver or --driver-compiler")
    if candidate_requested and args.strace is None:
        raise RuntimeError(
            "candidate mode requires --strace: all 56 Source compiles are traced")
    prepare_output(args.out)
    resources = candidate_resources(args) if candidate_requested else None
    if resources is not None:
        check_resources(resources, "candidate")
    env = environment(args.toolchain_root, args.out)
    log: list[dict] = []
    states = {
        "reference": blank_case_states() if reference_requested else {},
        "candidate": blank_case_states() if candidate_requested else {},
    }
    summary = {
        "status": "failed",
        "python": None,
        "legacy_cmake_timeout_seconds": LEGACY_CMAKE_TIMEOUT_SECONDS,
        "elapsed_seconds": 0.0,
        "legacy_cmake_timeout_exceeded": False,
        "primary_group": None,
        "groups": {},
        "positive": {},
        "panics": {},
        "negative": {},
        "commands": log,
        "hashes": {},
        "inputs_before": {},
        "inputs_after": {},
        "not_run": [],
        "error": None,
    }
    started = time.monotonic()
    driver = None
    try:
        summary["inputs_before"] = snapshot_inputs(args, resources)
        version = run_command([args.python, "--version"], args.out, 20,
                              args.out, log, "python-version", env)
        require_ok(version)
        if version["stdout"].strip() != "Python 3.12.15":
            raise RuntimeError(
                f"Python 3.12.15 required: {version['stdout'].strip()}")
        summary["python"] = version["stdout"].strip()
        verify_inventory(args.root)
        if candidate_requested:
            driver, driver_info = prepare_driver(
                args, args.out, resources, log, env)
            summary["driver"] = driver_info
        if reference_requested:
            summary["groups"]["reference"] = run_group(
                "reference", args, args.out, None, log, env,
                states["reference"], args.strace)
        if candidate_requested:
            summary["groups"]["candidate"] = run_group(
                "candidate", args, args.out, driver, log, env,
                states["candidate"], args.strace)
        primary = "candidate" if candidate_requested else "reference"
        summary["primary_group"] = primary
        primary_group = summary["groups"][primary]
        for key in ("positive", "panics", "negative"):
            summary[key] = primary_group[key]
        summary["hashes"] = primary_group["hashes"]
        statuses = [group["status"] for group in summary["groups"].values()]
        completes = [group["complete"] for group in summary["groups"].values()]
        if any(status == "failed" for status in statuses):
            summary["status"] = "failed"
        elif not all(completes):
            summary["status"] = "partial"
        else:
            not_run = [
                f"{group}:{key}" for group, group_states in states.items()
                for key, value in group_states.items()
                if value.get("status") == "not_run"
            ]
            summary["status"] = "partial" if not_run else "passed"
    except Exception as error:
        summary["error"] = str(error)
        summary["status"] = "failed"
    finally:
        summary["elapsed_seconds"] = time.monotonic() - started
        summary["legacy_cmake_timeout_exceeded"] = (
            summary["elapsed_seconds"] > LEGACY_CMAKE_TIMEOUT_SECONDS)
        try:
            summary["inputs_after"] = snapshot_inputs(args, resources)
        except Exception as error:
            summary["inputs_after"] = {"error": str(error)}
            summary["status"] = "failed"
            if summary["error"] is None:
                summary["error"] = f"input snapshot failed: {error}"
        if summary["inputs_after"] != summary["inputs_before"]:
            summary["status"] = "failed"
            message = "source/resource inputs changed during the run"
            if summary["error"] is None:
                summary["error"] = message
        for group, group_states in states.items():
            for key, value in group_states.items():
                if value.get("status") == "not_run":
                    summary["not_run"].append(f"{group}:{key}")
        write_json(args.out / "summary.json", summary)
        print(f"python slice source test {summary['status']}: "
              f"positive={len(summary['positive'])} "
              f"panics={len(summary['panics'])} "
              f"negative={len(summary['negative'])} "
              f"elapsed={summary['elapsed_seconds']:.1f}s")
        if summary["error"]:
            print(f"error: {summary['error']}", file=sys.stderr)
    return 0 if summary["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
