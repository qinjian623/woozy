#!/usr/bin/env python3
"""Build, run and observe real Source Weak programs on Python/64.

Two implementation groups use the same 18 real manifests: 11 accepted
positives and 7 required rejections.

* reference: the C++ ``woozyc build --backend python`` CLI plus the C++
  ``lang_python_emit`` helper.  This legacy ``--cli``/``--helper`` mode remains
  the default when no candidate entry is requested.
* candidate: the public pure-Wz CLI plus the accepted Wz API driver that calls
  ``project.load`` -> Python selection -> ``resolve.resolve_project`` ->
  ``sema.check_project(Backend)`` -> ``pythonemit.emit``.

Inside one group the generated ``program.py``/``langrt.py`` bytes must match.
Every positive program then runs under the unchanged ``observe.py`` with
CPython's automatic collector disabled; the original Weak upgrade,
deactivation and live-GC checks are preserved.  Every rejection must keep the
original diagnostic code/message and must not publish a program.

Every real Source compile runs through the shared bounded process-group helper
in ``selfhost/corpus.py``, is traced with exec-only strace, and is checked by
``corpus.audit_trace`` against the exact executable for that entry.  Observer
runs are traced under a separate, non-compiler policy.  Each command's
argv/cwd/budget/raw bytes/SHAs/process flags/trace is persisted immediately.
``--out`` must be new or empty; failed cases are recorded and the remaining
cases still run.
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
from datetime import datetime, timezone
from pathlib import Path

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
COMPILER_ROOT = HERE.parents[2]
SELFHOST = COMPILER_ROOT / "selfhost"
sys.path.insert(0, str(SELFHOST))
from corpus import (  # noqa: E402
    CorpusError, TraceError, _reject_json_constant, _run_process_impl,
    audit_trace, parse_trace)

# Reference-value cases already in the accepted Root integration snapshot.
ROOT_POSITIVE = (
    ('weak-vec-lifecycle', 'tests/reference_values/fixtures/weak-vec-lifecycle/lang.toml', 0),
    ('weak-box-lifecycle', 'tests/reference_values/fixtures/weak-box-lifecycle/lang.toml', 0),
    ('weak-empty-upgrade-none', 'tests/reference_values/fixtures/weak-empty-upgrade-none/lang.toml', 0),
    ('weak-fn-immortal', 'tests/reference_values/fixtures/weak-fn-immortal/lang.toml', 0),
    ('weak-fn-captured-dead', 'tests/reference_values/fixtures/weak-fn-captured-dead/lang.toml', 0),
)

LOCAL_POSITIVE = (
    ('combo-fields-vec-enum', 0),
    ('held-some-keeps-alive', 0),
    ('generic-target', 0),
)

# Selected old Source projects: function values, a generic function argument,
# and shared ownership through a struct copy.
OLD_SOURCE = (
    ('old-function-values', 'tests/backend_common/fixtures/function-values/lang.toml', 0),
    ('old-generic-function-argument', 'tests/generics/fixtures/function-argument/lang.toml', 5),
    ('old-struct-copy-shares-vec', 'tests/ownership/fixtures/struct-copy-shares-vec/lang.toml', 0),
)

REJECT = {
    'reject-user-struct': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-same-shape-struct': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-string': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-file': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-slice': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-foreign': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-native-handle': ('E_CAPABILITY_UNAVAILABLE',
                             'backend does not support this concrete function signature'),
}

# Only the original 8 Weak descriptor cases require the emitted Weak marker.
WEAK_POSITIVE = frozenset(
    {name for name, _manifest, _expected in ROOT_POSITIVE}
    | {name for name, _expected in LOCAL_POSITIVE})

RUNTIME_VERSION = "compiler-dev/1-python-values/10"
RUNTIME_VERSION_LINE = "RUNTIME_VERSION = 'compiler-dev/1-python-values/10'"
WEAK_DESCRIPTOR = "_rt.ValueType('weak')"
LEGACY_CMAKE_TIMEOUT_SECONDS = 180
PYTHON_VERSION_TIMEOUT_SECONDS = 20
REQUIRED_OBSERVER_KEYS = {
    "status", "exit", "escaped", "program_stdout", "program_stderr",
    "stats", "live_vectors", "stats_after_probe",
    "live_vectors_after_probe",
}
COMPILER_TRACE_SCOPE = "compiler_exec_whitelist"
OBSERVER_TRACE_SCOPE = "observer_execution_not_compiler_whitelist"


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


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def positive_names() -> list[str]:
    return ([name for name, _manifest, _expected in ROOT_POSITIVE]
            + [name for name, _expected in LOCAL_POSITIVE]
            + [name for name, _manifest, _expected in OLD_SOURCE])


def blank_case_states() -> dict[str, dict]:
    return {f"{kind}/{name}": {"status": "not_run"}
            for kind, names in
            (("positive", positive_names()), ("reject", list(REJECT)))
            for name in names}


def manifest_paths(root: Path) -> dict[str, Path]:
    found: dict[str, Path] = {}
    for name, manifest, _expected in ROOT_POSITIVE:
        found[name] = root / manifest
    for name, manifest, _expected in OLD_SOURCE:
        found[name] = root / manifest
    for name, _expected in LOCAL_POSITIVE:
        found[name] = (root / "tests/python/weak_source/fixtures" / name
                       / "lang.toml")
    for name in REJECT:
        found[name] = (root / "tests/python/weak_source/fixtures" / name
                       / "lang.toml")
    return found


def fixture_source(root: Path, name: str) -> Path:
    manifest = manifest_paths(root)[name]
    return manifest.parent / "src" / "main.wz"


def verify_inventory(root: Path) -> None:
    missing = []
    for name, manifest in manifest_paths(root).items():
        if not manifest.is_file():
            missing.append(str(manifest))
            continue
        if not fixture_source(root, name).is_file():
            missing.append(str(fixture_source(root, name)))
    if missing:
        raise RuntimeError("missing fixture(s): " + ", ".join(sorted(missing)))


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
                trace_prefix: str = "",
                trace_scope: str = COMPILER_TRACE_SCOPE) -> dict:
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
        "trace_scope": trace_scope if trace_path is not None else None,
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
        return json.loads(record["stdout"],
                          parse_constant=_reject_json_constant)
    except (json.JSONDecodeError, CorpusError) as error:
        raise RuntimeError(
            f"{record['label']}: expected strict JSON, got "
            f"{record['stdout']!r}: {error}") from error


def audit_candidate_trace(record: dict, allowed: set[str]) -> dict:
    """Audit one real compile trace with corpus's exact whitelist."""
    trace = record.get("trace_path")
    if not trace:
        raise RuntimeError(f"{record['label']}: compile was not traced")
    if record.get("trace_scope") != COMPILER_TRACE_SCOPE:
        raise RuntimeError(
            f"{record['label']}: compile trace scope is not the compiler policy")
    try:
        result = audit_trace(Path(trace), allowed)
    except TraceError as error:
        raise RuntimeError(
            f"{record['label']}: exact exec whitelist rejected trace: {error}") from error
    record["trace_audit"] = result
    write_json(Path(trace + ".audit.json"), {
        "label": record["label"],
        "trace_scope": COMPILER_TRACE_SCOPE,
        **result,
    })
    return result


def check_runtime(directory: Path, label: str) -> None:
    runtime = directory / "langrt.py"
    if not runtime.is_file():
        raise RuntimeError(f"{label}: no generated langrt.py")
    text = runtime.read_text(encoding="utf-8")
    if RUNTIME_VERSION_LINE not in text:
        raise RuntimeError(
            f"{label}: generated runtime is not {RUNTIME_VERSION}")


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
    if strace is not None:
        audit_candidate_trace(record, {str(cli.resolve())})
    require_ok(record)
    document = parse_json(record)
    if document.get("status") != "ok":
        raise RuntimeError(f"{label}: build did not succeed: {document}")
    if document.get("diagnostics"):
        raise RuntimeError(f"{label}: CLI diagnostics: {document}")
    return entry_hashes(directory, "cli", record)


def build_helper(helper: Path, manifest: Path, directory: Path, timeout: float,
                 out: Path, log: list[dict], label: str,
                 env: dict[str, str], strace: Path | None = None,
                 trace_prefix: str = "") -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    record = run_command(
        [helper, "--project", manifest, "--out-dir", directory],
        out, timeout, out, log, label, env, strace, trace_prefix)
    if strace is not None:
        audit_candidate_trace(record, {str(helper.resolve())})
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
    if strace is not None:
        audit_candidate_trace(record, {str(driver.resolve())})
    require_ok(record)
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


def check_observer(name: str, observed: dict, expected_return: int) -> None:
    """Check the unchanged observer document, not only its pass string."""
    missing = REQUIRED_OBSERVER_KEYS - set(observed)
    if missing:
        raise RuntimeError(
            f"{name}: observer document missing {sorted(missing)}: {observed}")
    if observed.get("status") != "passed":
        raise RuntimeError(f"{name}: observer rejected output: {observed}")
    if type(observed.get("exit")) is not int:
        raise RuntimeError(f"{name}: observer exit is not an int")
    if observed["exit"] != expected_return:
        raise RuntimeError(
            f"{name}: observer exit {observed['exit']} != {expected_return}")
    if observed.get("escaped") is not None:
        raise RuntimeError(f"{name}: uncaught Python exception {observed['escaped']}")
    if observed.get("program_stdout") != "":
        raise RuntimeError(f"{name}: program wrote stdout")
    if observed.get("program_stderr") != "":
        raise RuntimeError(f"{name}: program wrote stderr")
    stats = observed["stats"]
    after = observed["stats_after_probe"]
    if stats.get("runtime_version") != RUNTIME_VERSION:
        raise RuntimeError(f"{name}: wrong generated runtime {stats}")
    for label, counters in (("stats", stats), ("stats_after_probe", after)):
        if counters.get("active") is not False:
            raise RuntimeError(f"{name}: collector still active in {label}")
        if type(counters.get("busy")) is not int or counters["busy"] != 0:
            raise RuntimeError(f"{name}: collector busy in {label}")
        if type(counters.get("live")) is not int or counters["live"] != 0:
            raise RuntimeError(f"{name}: live language roots in {label}")
    if observed["live_vectors"] != 0:
        raise RuntimeError(f"{name}: live Vec roots at exit")
    if observed["live_vectors_after_probe"] != 0:
        raise RuntimeError(f"{name}: live Vec roots after the Weak probe")


def run_observer(python: Path, root: Path, cli_directory: Path,
                 expected_return: int, timeout: float, out: Path,
                 log: list[dict], label: str, env: dict[str, str],
                 result_path: Path, observer: Path | None = None,
                 strace: Path | None = None,
                 trace_prefix: str = "") -> dict:
    script = observer or (root / "tests/python/weak_source/observe.py")
    record = run_command(
        [python, "-s", script, cli_directory, str(expected_return)],
        out, timeout, out, log, label, env, strace, trace_prefix,
        trace_scope=OBSERVER_TRACE_SCOPE)
    if strace is not None:
        try:
            observed_execs = parse_trace(Path(record["trace_path"]))
            record["trace_observed"] = observed_execs
            record["trace_audit"] = {
                "status": "ok", "observed": observed_execs,
                "policy": "observer execution; not compiler whitelist",
            }
        except TraceError as error:
            record["trace_audit"] = {
                "status": "unverified", "error": str(error),
                "policy": "observer execution; not compiler whitelist",
            }
        write_json(Path(record["trace_path"] + ".audit.json"), {
            "label": record["label"],
            "trace_scope": OBSERVER_TRACE_SCOPE,
            **record["trace_audit"],
        })
    document = parse_json(record)
    write_json(result_path, document)
    require_ok(record)
    check_observer(label, document, expected_return)
    return document


def check_reject(record: dict, document: dict, code: str, message: str,
                 directory: Path, label: str) -> dict:
    if type(record.get("exit")) is not int or record["exit"] != 1:
        raise RuntimeError(
            f"{label}: negative build did not exit 1 "
            f"(got {record.get('exit')!r})")
    if record.get("timed_out") is not False:
        raise RuntimeError(f"{label}: negative build timed out")
    if record.get("cleanup_complete") is not True:
        raise RuntimeError(f"{label}: negative build did not clean up")
    if record.get("abandoned_pipes") is not False:
        raise RuntimeError(f"{label}: negative build abandoned pipes")
    if document.get("status") != "error":
        raise RuntimeError(f"{label}: negative document is not error: {document}")
    diagnostics = document.get("diagnostics", [])
    codes = [item.get("code") for item in diagnostics]
    messages = " ".join(item.get("message", "") for item in diagnostics)
    if code not in codes:
        raise RuntimeError(f"{label}: expected {code}, got {codes}")
    if message not in messages:
        raise RuntimeError(
            f"{label}: missing original message {message!r}: {messages}")
    if (directory / "program.py").exists():
        raise RuntimeError(f"{label}: generated a program")
    return {"exit": record["exit"], "codes": codes,
            "messages": [item.get("message", "") for item in diagnostics]}


def source_hashes(root: Path, manifest: Path, hashes: dict[str, str]) -> dict[str, str]:
    found = {}
    for path in sorted(manifest.parent.rglob("*")):
        if path.is_file():
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


def run_positive_case(role: str, name: str, manifest: Path, expected_return: int,
                      args, out: Path, first_tool: Path, second_kind: str,
                      second_tool: Path | None, log: list[dict],
                      env: dict[str, str], strace: Path | None,
                      hashes: dict[str, str]) -> dict:
    case_dir = out / role / "positive" / name
    case_dir.mkdir(parents=True, exist_ok=True)
    errors: list[str] = []
    result = {
        "name": name,
        "kind": "positive",
        "role": role,
        "manifest": str(manifest.relative_to(args.root)),
        "expected_return": expected_return,
        "require_weak": name in WEAK_POSITIVE,
        "steps": {},
        "errors": errors,
        "status": "failed",
    }
    first = None
    second = None
    trace_prefix = f"-positive-{name}"
    try:
        first = build_cli(first_tool, manifest,
                          case_dir / "cli", args.timeout, out, log,
                          f"{role}-positive-{name}-cli", env, strace,
                          trace_prefix)
        result["steps"]["cli"] = {"status": "ok", **first}
    except Exception as error:
        result["steps"]["cli"] = {"status": "failed", "error": str(error)}
        errors.append(f"cli: {error}")
    if second_kind == "helper" and second_tool is not None:
        try:
            second = build_helper(second_tool, manifest, case_dir / "helper",
                                  args.timeout, out, log,
                                  f"{role}-positive-{name}-helper", env,
                                  strace, trace_prefix)
            result["steps"]["helper"] = {"status": "ok", **second}
        except Exception as error:
            result["steps"]["helper"] = {"status": "failed", "error": str(error)}
            errors.append(f"helper: {error}")
    elif second_kind == "api" and second_tool is not None:
        try:
            second = build_api(second_tool, manifest, case_dir / "api",
                               args.timeout, out, log,
                               f"{role}-positive-{name}-api", env, strace,
                               trace_prefix)
            result["steps"]["api"] = {"status": "ok", **second}
        except Exception as error:
            result["steps"]["api"] = {"status": "failed", "error": str(error)}
            errors.append(f"api: {error}")
    else:
        errors.append(f"{second_kind} entry is not available")
    if first is not None and second is not None:
        try:
            compare_entries(first, second, f"{name} positive")
            result["steps"]["bytes"] = {
                "status": "ok",
                "program.py": first["program_sha256"],
                "langrt.py": first["runtime_sha256"],
            }
        except Exception as error:
            result["steps"]["bytes"] = {"status": "failed", "error": str(error)}
            errors.append(f"bytes: {error}")
    if first is not None:
        program = (Path(first["path"]) / "program.py").read_text(encoding="utf-8")
        found_weak = WEAK_DESCRIPTOR in program
        result["weak_descriptor"] = found_weak
        if name in WEAK_POSITIVE and not found_weak:
            errors.append("program has no Weak descriptor")
    else:
        result["weak_descriptor"] = None
    try:
        observed = run_observer(
            args.python, args.root, case_dir / "cli", expected_return,
            args.timeout, out, log, f"{role}-positive-{name}-observer", env,
            case_dir / "observer.json", strace=strace,
            trace_prefix=trace_prefix)
        result["observer"] = {
            "status": observed["status"],
            "exit": observed["exit"],
            "stats": observed["stats"],
            "stats_after_probe": observed["stats_after_probe"],
            "live_vectors": observed["live_vectors"],
            "live_vectors_after_probe": observed["live_vectors_after_probe"],
            "path": str(case_dir / "observer.json"),
        }
        result["steps"]["observer"] = {"status": "ok"}
    except Exception as error:
        result["steps"]["observer"] = {"status": "failed", "error": str(error)}
        errors.append(f"observer: {error}")
    result["sources"] = source_hashes(args.root, manifest, hashes)
    entries = {}
    if first is not None:
        entries["cli"] = first
    if second is not None:
        entries[second_kind] = second
    result["generated_hashes"] = {
        kind: record_generated(Path(entry["path"]), out, hashes)
        for kind, entry in entries.items()
    }
    result["status"] = "passed" if not errors else "failed"
    write_json(case_dir / "result.json", result)
    return result


def run_reject_case(role: str, name: str, code: str, message: str, args,
                    out: Path, first_tool: Path, second_kind: str,
                    second_tool: Path | None, log: list[dict],
                    env: dict[str, str], strace: Path | None,
                    hashes: dict[str, str]) -> dict:
    manifest = manifest_paths(args.root)[name]
    case_dir = out / role / "reject" / name
    case_dir.mkdir(parents=True, exist_ok=True)
    errors: list[str] = []
    result = {
        "name": name,
        "kind": "reject",
        "role": role,
        "manifest": str(manifest.relative_to(args.root)),
        "expected_code": code,
        "expected_message": message,
        "steps": {},
        "errors": errors,
        "status": "failed",
    }
    trace_prefix = f"-reject-{name}"
    entries = [("cli", first_tool)]
    if second_tool is not None:
        entries.append((second_kind, second_tool))
    for entry_kind, tool in entries:
        entry_dir = case_dir / entry_kind
        entry_dir.mkdir(parents=True, exist_ok=True)
        label = f"{role}-reject-{name}-{entry_kind}"
        if entry_kind == "cli":
            argv = [tool, "build", "--project", manifest, "--backend",
                    "python", "--out-dir", entry_dir, "--format", "json"]
        elif entry_kind == "helper":
            argv = [tool, "--project", manifest, "--out-dir", entry_dir]
        else:
            argv = [tool, "--project", manifest, "--out-dir", entry_dir,
                    "--format", "json"]
        try:
            record = run_command(argv, out, args.timeout, out, log, label, env,
                                 strace, trace_prefix)
            if strace is not None:
                audit_candidate_trace(record, {str(tool.resolve())})
            document = parse_json(record)
            checked = check_reject(record, document, code, message, entry_dir,
                                   label)
            result["steps"][entry_kind] = {"status": "ok", **checked}
        except Exception as error:
            result["steps"][entry_kind] = {
                "status": "failed", "error": str(error)}
            errors.append(f"{entry_kind}: {error}")
    if second_tool is None:
        errors.append(f"{second_kind} entry is not available")
    result["sources"] = source_hashes(args.root, manifest, hashes)
    result["status"] = "passed" if not errors else "failed"
    write_json(case_dir / "result.json", result)
    return result


def attempted_counts(role: str, log: list[dict]) -> dict[str, int]:
    compiles = 0
    observers = 0
    for record in log:
        label = record.get("label", "")
        if not label.startswith(role + "-"):
            continue
        if label.endswith(("-cli", "-helper", "-api")):
            compiles += 1
        elif label.endswith("-observer"):
            observers += 1
    return {"source_compiles": compiles, "observers": observers}


def run_group(role: str, args, out: Path, first_tool: Path, second_kind: str,
              second_tool: Path | None, log: list[dict], env: dict[str, str],
              case_states: dict[str, dict],
              strace: Path | None) -> dict:
    hashes: dict[str, str] = {}
    result = {
        "name": role,
        "status": "failed",
        "complete": second_tool is not None,
        "error": None,
        "positive": {},
        "rejects": {},
        "hashes": hashes,
        "cases": case_states,
        "attempted": {"source_compiles": 0, "observers": 0},
    }
    manifests = manifest_paths(args.root)
    try:
        for name, _manifest, expected in ROOT_POSITIVE:
            current = f"positive/{name}"
            case_states[current] = {"status": "running"}
            try:
                result["positive"][name] = run_positive_case(
                    role, name, manifests[name], expected, args, out,
                    first_tool, second_kind, second_tool, log, env, strace,
                    hashes)
            except Exception as error:
                result["positive"][name] = {
                    "name": name, "status": "failed", "error": str(error)}
            case_states[current] = {
                "status": result["positive"][name]["status"],
                "error": result["positive"][name].get("error"),
            }
        for name, expected in LOCAL_POSITIVE:
            current = f"positive/{name}"
            case_states[current] = {"status": "running"}
            try:
                result["positive"][name] = run_positive_case(
                    role, name, manifests[name], expected, args, out,
                    first_tool, second_kind, second_tool, log, env, strace,
                    hashes)
            except Exception as error:
                result["positive"][name] = {
                    "name": name, "status": "failed", "error": str(error)}
            case_states[current] = {
                "status": result["positive"][name]["status"],
                "error": result["positive"][name].get("error"),
            }
        for name, _manifest, expected in OLD_SOURCE:
            current = f"positive/{name}"
            case_states[current] = {"status": "running"}
            try:
                result["positive"][name] = run_positive_case(
                    role, name, manifests[name], expected, args, out,
                    first_tool, second_kind, second_tool, log, env, strace,
                    hashes)
            except Exception as error:
                result["positive"][name] = {
                    "name": name, "status": "failed", "error": str(error)}
            case_states[current] = {
                "status": result["positive"][name]["status"],
                "error": result["positive"][name].get("error"),
            }
        for name, (code, message) in REJECT.items():
            current = f"reject/{name}"
            case_states[current] = {"status": "running"}
            try:
                result["rejects"][name] = run_reject_case(
                    role, name, code, message, args, out, first_tool,
                    second_kind, second_tool, log, env, strace, hashes)
            except Exception as error:
                result["rejects"][name] = {
                    "name": name, "status": "failed", "error": str(error)}
            case_states[current] = {
                "status": result["rejects"][name]["status"],
                "error": result["rejects"][name].get("error"),
            }
        result["attempted"] = attempted_counts(role, log)
        failed = [key for key, value in case_states.items()
                  if value.get("status") != "passed"]
        if failed:
            result["error"] = "failed cases: " + ", ".join(sorted(failed))
        elif not result["complete"]:
            result["error"] = f"{second_kind} entry absent"
        else:
            result["status"] = "passed"
    except Exception as error:
        result["status"] = "failed"
        result["error"] = str(error)
    finally:
        result["cases"] = case_states
    return result


def default_stdlib_root(binary: Path, label: str) -> Path:
    """Read the C++ default stdlib path actually embedded in the binary.

    The reference CLI/helper compiles ``LANG_STD_SOURCE_ROOT`` into the
    executable.  This reads that NUL-terminated absolute path instead of
    assuming that an owner-tree copy with the same bytes is the real input.
    """
    data = binary.read_bytes()
    suffix = b"/stdlib\x00"
    found: set[str] = set()
    offset = 0
    while True:
        index = data.find(suffix, offset)
        if index < 0:
            break
        end = index + len(suffix) - 1
        begin = data.rfind(b"\x00", 0, index) + 1
        raw = data[begin:end]
        if raw.startswith(b"/"):
            try:
                found.add(raw.decode("utf-8"))
            except UnicodeDecodeError:
                pass
        offset = index + len(suffix)
    if len(found) != 1:
        raise RuntimeError(
            f"{label}: expected exactly one embedded default stdlib root, "
            f"got {sorted(found)}")
    root = Path(found.pop())
    if not root.is_absolute() or not root.is_dir():
        raise RuntimeError(
            f"{label}: embedded default stdlib root is not an absolute "
            f"directory: {root}")
    if not any(path.is_file() for path in root.rglob("*.wz")):
        raise RuntimeError(
            f"{label}: embedded default stdlib root has no Wz sources: {root}")
    return root


def reference_stdlib_roots(args) -> dict[str, dict]:
    found: dict[str, dict] = {}
    for label, binary in (("reference_cli", args.cli),
                          ("reference_helper", args.helper)):
        if binary is None or not binary.is_file():
            continue
        root = default_stdlib_root(binary, label)
        sources = sorted(path for path in root.rglob("*.wz")
                         if path.is_file())
        found[label] = {
            "root": str(root),
            "wz_files": len(sources),
            "equals_owner_stdlib_tree": (
                root.resolve() == (args.root / "stdlib").resolve()),
        }
    return found


def snapshot_inputs(args, resources: Path | None) -> dict[str, str]:
    paths: set[Path] = set()
    for manifest in manifest_paths(args.root).values():
        for path in manifest.parent.rglob("*"):
            if path.is_file():
                paths.add(path)
    # Frozen compiler package identity (115 wzcc modules + the package main).
    for path in args.root.glob("selfhost/src/**/*.wz"):
        if path.is_file():
            paths.add(path)
    source_manifest = args.root / "selfhost" / "lang.toml"
    if source_manifest.is_file():
        paths.add(source_manifest)
    for pattern in ("stdlib/**/*.wz", "runtime/python/*.py"):
        for path in args.root.glob(pattern):
            if path.is_file():
                paths.add(path)
    paths.add(HERE / "observe.py")
    paths.add(Path(__file__).resolve())
    for tool in (args.cli, args.helper, args.candidate_cli, args.api_driver,
                 args.python, args.strace):
        if tool is not None and tool.is_file():
            paths.add(tool)
    if resources is not None:
        for path in (resources / "stdlib").rglob("*.wz"):
            if path.is_file():
                paths.add(path)
        for path in (resources / "runtime").rglob("*.py"):
            if path.is_file():
                paths.add(path)
    # Actual default stdlib roots embedded in the reference C++ binaries.
    for label, binary in (("reference_cli", args.cli),
                          ("reference_helper", args.helper)):
        if binary is not None and binary.is_file():
            for path in default_stdlib_root(binary, label).rglob("*.wz"):
                if path.is_file():
                    paths.add(path)
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


def input_summary(snapshot: dict[str, str], args, resources: Path | None) -> dict:
    def count(predicate) -> int:
        return sum(1 for key in snapshot if predicate(key))

    fixtures = [str(path) for manifest in manifest_paths(args.root).values()
                for path in sorted(manifest.parent.rglob("*")) if path.is_file()]
    return {
        "total_entries": len(snapshot),
        "fixture_files": len(fixtures),
        "fixture_manifests": 18,
        "compiler_source_wz": count(
            lambda key: "/selfhost/src/" in key and key.endswith(".wz")),
        "wzcc_package_wz": count(
            lambda key: "/selfhost/src/wzcc/" in key and key.endswith(".wz")),
        "stdlib_wz_entries": count(
            lambda key: "/stdlib/" in key and key.endswith(".wz")),
        "stdlib_wz_distinct_hashes": len({
            value for key, value in snapshot.items()
            if "/stdlib/" in key and key.endswith(".wz")}),
        "python_runtime_py_entries": count(
            lambda key: key.endswith(("/runtime/python/langrt.py",
                                      "/runtime/python/unicode18.py"))),
        "python_runtime_py_distinct_hashes": len({
            value for key, value in snapshot.items()
            if key.endswith(("/runtime/python/langrt.py",
                             "/runtime/python/unicode18.py"))}),
        "api_driver": str(args.api_driver) if args.api_driver else None,
        "candidate_cli": str(args.candidate_cli) if args.candidate_cli else None,
        "reference_cli": str(args.cli) if args.cli else None,
        "reference_helper": str(args.helper) if args.helper else None,
        "candidate_resources": str(resources) if resources else None,
        "reference_stdlib": reference_stdlib_roots(args),
        "note": "counts summarize the hash inventory; they are not a tree claim",
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=None,
                        help="reference C++ woozyc CLI (legacy --cli)")
    parser.add_argument("--helper", type=Path, default=None,
                        help="reference C++ lang_python_emit helper")
    parser.add_argument("--candidate-cli", type=Path, default=None,
                        help="candidate pure-Wz CLI for --backend python")
    parser.add_argument("--api-driver", type=Path, default=None,
                        help="accepted prebuilt Wz Python API driver")
    parser.add_argument("--candidate-resources", type=Path, default=None,
                        help="candidate stdlib+runtime directory "
                             "(default: candidate CLI directory)")
    parser.add_argument("--strace", type=Path, default=None,
                        help="strace used to audit every real Source compile")
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--timeout", type=finite_timeout, default=120.0)
    parser.add_argument("--toolchain-root", type=Path, default=None)
    parser.add_argument("--operation-id", default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    for key in ("cli", "helper", "candidate_cli", "api_driver",
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
    reference_requested = args.cli is not None
    candidate_requested = (args.candidate_cli is not None
                           or args.api_driver is not None)
    if not reference_requested and not candidate_requested:
        raise RuntimeError(
            "nothing to run: pass --cli/--helper and/or "
            "--candidate-cli with --api-driver")
    if candidate_requested and args.candidate_cli is None:
        raise RuntimeError("candidate mode requires --candidate-cli")
    if candidate_requested and args.api_driver is None:
        raise RuntimeError("candidate mode requires --api-driver")
    if candidate_requested and args.strace is None:
        raise RuntimeError(
            "candidate mode requires --strace: every Source compile is traced")
    prepare_output(args.out)
    resources = None
    if candidate_requested:
        resources = (args.candidate_resources
                     or args.candidate_cli.parent).resolve()
        for relative in ("stdlib", "runtime/python/unicode18.py",
                         "runtime/python/langrt.py"):
            if not (resources / relative).exists():
                raise RuntimeError(f"candidate: missing resource {relative}")
    env = environment(args.toolchain_root, args.out)
    log: list[dict] = []
    states = {
        "reference": blank_case_states() if reference_requested else {},
        "candidate": blank_case_states() if candidate_requested else {},
    }
    operation_id = args.operation_id or "SH12q-python-weak-source-public-api-20261114"
    summary = {
        "schema": "weak-source-python-api/1",
        "operation_id": operation_id,
        "pid": os.getpid(),
        "started_at": utc_now(),
        "finished_at": None,
        "status": "failed",
        "python": None,
        "legacy_cmake_timeout_seconds": LEGACY_CMAKE_TIMEOUT_SECONDS,
        "elapsed_seconds": 0.0,
        "legacy_cmake_timeout_exceeded": False,
        "groups": {},
        "positive": {},
        "rejects": {},
        "commands": log,
        "hashes": {},
        "inputs_before": {},
        "inputs_after": {},
        "input_summary": {},
        "counts": {
            "expected_source_compiles": 36 * (1 if reference_requested else 0)
            + 36 * (1 if candidate_requested else 0),
            "expected_observers": 11 * (1 if reference_requested else 0)
            + 11 * (1 if candidate_requested else 0),
            "actual_source_compiles": 0,
            "actual_observers": 0,
        },
        "not_run": [],
        "error": None,
    }
    started = time.monotonic()
    write_json(args.out / "run.json", {
        "operation_id": operation_id,
        "pid": os.getpid(),
        "started_at": summary["started_at"],
        "argv": [str(part) for part in sys.argv],
        "out": str(args.out),
    })
    try:
        summary["inputs_before"] = snapshot_inputs(args, resources)
        summary["input_summary"] = input_summary(
            summary["inputs_before"], args, resources)
        version = run_command([args.python, "--version"], args.out,
                              PYTHON_VERSION_TIMEOUT_SECONDS, args.out, log,
                              "python-version", env)
        require_ok(version)
        if version["stdout"].strip() != "Python 3.12.15":
            raise RuntimeError(
                f"Python 3.12.15 required: {version['stdout'].strip()}")
        summary["python"] = version["stdout"].strip()
        verify_inventory(args.root)
        if reference_requested:
            summary["groups"]["reference"] = run_group(
                "reference", args, args.out, args.cli, "helper", args.helper,
                log, env, states["reference"], args.strace)
        if candidate_requested:
            summary["groups"]["candidate"] = run_group(
                "candidate", args, args.out, args.candidate_cli, "api",
                args.api_driver, log, env, states["candidate"], args.strace)
        for group in summary["groups"].values():
            summary["counts"]["actual_source_compiles"] += \
                group["attempted"]["source_compiles"]
            summary["counts"]["actual_observers"] += \
                group["attempted"]["observers"]
        primary = "candidate" if candidate_requested else "reference"
        primary_group = summary["groups"][primary]
        for key in ("positive", "rejects"):
            summary[key] = primary_group[key]
        summary["hashes"] = primary_group["hashes"]
        statuses = [group["status"] for group in summary["groups"].values()]
        if any(status == "failed" for status in statuses):
            summary["status"] = "failed"
        elif (summary["counts"]["actual_source_compiles"]
              != summary["counts"]["expected_source_compiles"]
              or summary["counts"]["actual_observers"]
              != summary["counts"]["expected_observers"]):
            summary["status"] = "failed"
            summary["error"] = (
                "not every required compile/observer actually ran: "
                f"{summary['counts']}")
        else:
            summary["status"] = "passed"
    except Exception as error:
        summary["error"] = f"{type(error).__name__}: {error}"
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
            if summary["error"] is None:
                summary["error"] = "source/resource inputs changed during the run"
        for group, group_states in states.items():
            for key, value in group_states.items():
                if value.get("status") == "not_run":
                    summary["not_run"].append(f"{group}:{key}")
        summary["finished_at"] = utc_now()
        write_json(args.out / "summary.json", summary)
        write_json(args.out / "run.json", {
            "operation_id": operation_id,
            "pid": os.getpid(),
            "started_at": summary["started_at"],
            "finished_at": summary["finished_at"],
            "status": summary["status"],
            "elapsed_seconds": summary["elapsed_seconds"],
            "argv": [str(part) for part in sys.argv],
            "out": str(args.out),
        })
        print(json.dumps({
            "status": summary["status"],
            "python": summary["python"],
            "groups": sorted(summary["groups"]),
            "counts": summary["counts"],
            "not_run": summary["not_run"],
            "error": summary["error"],
        }, sort_keys=True))
    return 0 if summary["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
