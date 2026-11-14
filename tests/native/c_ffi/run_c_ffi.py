#!/usr/bin/env python3
"""Execute the FFI01 C ABI contract with public Wz CLI evidence.

Two modes are supported.

Legacy default
    The original CMake argv (``--cli --root --out --clang --ar
    --hosted-runtime --native-runtime --forged``) is kept.  Every invocation
    writes a new ``out/runs/<timestamp>-<pid>/`` directory, so reusing the
    same ``--out`` never overwrites an earlier report, command record, raw
    stdout/stderr or generated program.  The original 43 report rows and the
    extra ``git rev-parse`` command are preserved.

Formal public-CLI mode
    ``--mode formal`` requires an explicit reference group and candidate
    group.  The reference group keeps the original C++ forged test and also
    requires ``--reference-native-helper``; that helper SHA is frozen and only
    reference build traces may launch it.  The
    candidate group uses a real Wz CLI plus a Root-prepared, same-Source Wz
    forged analysis component.  The component and its build record are inputs;
    this runner never builds that component.  A formal ``--out`` must be new
    or empty.

Every external command is executed through ``selfhost/corpus.py``'s bounded
process helper.  Its raw stdout/stderr bytes, SHA-256 values, argv, cwd, actual
timeout, native integer exit, strict ``timed_out``/``cleanup_complete``/
``abandoned_pipes`` booleans, launch/OSError state and exec trace are saved to
``commands.jsonl`` before any assertion is evaluated.  Records are replaced via
a same-directory ``.pending`` file, so a failed rewrite cannot destroy earlier
records.

The runner itself never evaluates generated IR or Source.  All Source is read
by the real CLI or by the Root-prepared Wz component.  C fixtures, C ABI
adapters, libc/crypto/pthread and the fixed LLVM/Clang/LLD tools are part of the
original interop test; they are not a substitute for the candidate compiler.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
import shutil
import sys
import time
import traceback
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

TASK = "FFI01"
OPERATION_ID = "SH15-cffi-full-public-runner-20261114"
CONTRACT = "compiler-dev/1"

# Original budgets.  The outer 600 s CMake timeout and the separate 60 s
# forged CMake timeout stay outside this runner and are controlled by Root.
NORMAL_BUILD_SECONDS = 180.0
NEGATIVE_CHECK_SECONDS = 60.0
LINK_FAILURE_SECONDS = 120.0
PROGRAM_SECONDS = 10.0
READELF_SECONDS = 10.0
FORGED_SECONDS = 60.0
FIXTURE_SECONDS = 60.0
GIT_HEAD_SECONDS = 30.0
ARCHIVE_SYMBOL_SECONDS = 60.0

PROJECTS = ("scalars", "libc", "threads", "unicode")
NEGATIVE = {
    "no-hosted": "E_CAPABILITY_UNAVAILABLE",
    "unsafe-required": "E_TYPE_MISMATCH",
    "readonly-write": "E_TYPE_MISMATCH",
    "buffer-from-vec": "E_TYPE_MISMATCH",
    "bad-binding": "E_BINDING_INVALID",
    "unknown-key": "E_BINDING_INVALID",
    "varargs": "E_BINDING_INVALID",
    "buffer-field-size": "E_BINDING_INVALID",
}
EXPECTED_CASES = (
    "hir_c_callable",
    "hir_signature_type_id",
    "hir_arg_indices",
    "hir_c_field",
    "native_c_callable",
    "native_origin_foreign_call",
    "native_c_field",
    "native_origin_c_ptr_write",
)
FORGED_LINE_HIR = "BASELINE HIR legal"
FORGED_LINE_NATIVE = "BASELINE NativeIR legal"

LEGACY_ROWS = 43
LEGACY_COMMANDS = 44  # 43 rows plus one git rev-parse outside the rows
FORMAL_ROWS = 84      # 2 roles x 41 rows + 2 shared fixture rows
FORMAL_COMMANDS = 84

COMPILER_TRACE_SCOPE = "compiler_exec_whitelist"
PROGRAM_TRACE_SCOPE = "program_execution_not_compiler_whitelist"
FIXTURE_TRACE_SCOPE = "c_fixture_build_exec_whitelist"
FORGED_CPP_TRACE_SCOPE = "reference_cpp_forged_exec_whitelist"
FORGED_WZ_TRACE_SCOPE = "candidate_wz_component_exec_whitelist"
GIT_TRACE_SCOPE = "git_head_exec_whitelist"

LLVM_TOOL_NAMES = (
    "clang", "clang++", "ld.lld", "opt", "llc", "llvm-ar", "llvm-nm",
    "llvm-readelf", "llvm-objcopy", "llvm-strip",
)


class RunnerError(RuntimeError):
    """A checked expectation or evidence invariant failed."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def digest(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def digest_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def decode_bytes(data: bytes) -> str:
    return data.decode("utf-8", errors="replace")


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False,
                   allow_nan=False) + "\n",
        encoding="utf-8")


def write_json_atomic(path: Path, value) -> None:
    pending = path.with_name(path.name + ".pending")
    pending.parent.mkdir(parents=True, exist_ok=True)
    pending.write_text(
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False,
                   allow_nan=False) + "\n",
        encoding="utf-8")
    pending.replace(path)


def canonical_sha(value) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":"),
                   ensure_ascii=False).encode("utf-8")).hexdigest()


def label_name(text: str) -> str:
    return "".join(character if character.isalnum() or character in "-_."
                   else "_" for character in text)


def strict_bool(value) -> bool:
    return type(value) is bool


def _finite_float(token: str) -> float:
    value = float(token)
    if not math.isfinite(value):
        raise ValueError(f"non-finite JSON number {token!r}")
    return value


def strict_json_loads(text: str, label: str) -> dict:
    try:
        document = json.loads(
            text, parse_float=_finite_float,
            parse_constant=_reject_json_constant)
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError,
            CorpusError) as error:
        raise RunnerError(f"{label}: malformed strict JSON: {error}") from error
    if not isinstance(document, dict):
        raise RunnerError(f"{label}: JSON is not an object")
    return document


def load_strict_json(path: Path, label: str) -> dict:
    try:
        text = Path(path).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise RunnerError(f"{label}: cannot read {path}: {error}") from error
    return strict_json_loads(text, label)


def parse_json_record(record: dict) -> dict:
    """Parse the CLI's strict JSON stdout, accepting the original last line."""
    label = str(record.get("label", "command"))
    text = record.get("stdout", "")
    stdout_path = record.get("stdout_path")
    if stdout_path:
        try:
            raw = Path(stdout_path).read_bytes()
        except OSError as error:
            raise RunnerError(f"{label}: cannot read raw stdout: {error}") from error
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise RunnerError(f"{label}: stdout is not valid UTF-8: {error}") from error
    if not isinstance(text, str):
        raise RunnerError(f"{label}: stdout is not text")
    try:
        return strict_json_loads(text.strip(), label)
    except RunnerError as first_error:
        lines = [line for line in text.splitlines() if line.strip()]
        if not lines:
            raise first_error
        try:
            return strict_json_loads(lines[-1].strip(), label)
        except RunnerError:
            raise first_error


def finite_timeout(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(
            "--timeout must be finite and positive") from error
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("--timeout must be finite and positive")
    return value


def require_process(record: dict, expected_exit: int | None = None) -> None:
    label = str(record.get("label", "command"))
    if record.get("launch_error"):
        raise RunnerError(f"{label}: launch failed: {record['launch_error']}")
    if type(record.get("exit")) is not int:
        raise RunnerError(f"{label}: exit is not a native int: "
                          f"{record.get('exit')!r}")
    if not strict_bool(record.get("timed_out")):
        raise RunnerError(f"{label}: timed_out is not a strict bool")
    if record["timed_out"] is not False:
        raise RunnerError(f"{label}: timed out after "
                          f"{record.get('timeout_seconds')!r}s")
    if not strict_bool(record.get("cleanup_complete")):
        raise RunnerError(f"{label}: cleanup_complete is not a strict bool")
    if record["cleanup_complete"] is not True:
        raise RunnerError(f"{label}: process cleanup was incomplete")
    if not strict_bool(record.get("abandoned_pipes")):
        raise RunnerError(f"{label}: abandoned_pipes is not a strict bool")
    if record["abandoned_pipes"] is not False:
        raise RunnerError(f"{label}: pipes were abandoned")
    if record["exit"] < 0:
        raise RunnerError(f"{label}: terminated by signal {-record['exit']}")
    if expected_exit is not None and record["exit"] != expected_exit:
        raise RunnerError(f"{label}: exit {record['exit']} != {expected_exit}")


def require_ok(record: dict, expected_exit: int = 0) -> None:
    require_process(record, expected_exit)
    trace = record.get("trace")
    if trace is not None and isinstance(trace, dict):
        if trace.get("status") not in ("ok", "not_required"):
            raise RunnerError(f"{record.get('label')}: exec trace failed: "
                              f"{trace.get('error')}")


def process_status(launch_error, exit_code, timed_out) -> str:
    if launch_error is not None:
        return "launch_failed"
    if timed_out:
        return "timeout"
    if type(exit_code) is int and exit_code < 0:
        return "signalled"
    return "exited"


def exec_aliases(paths) -> set[str]:
    result: set[str] = set()
    for path in paths:
        if path is None:
            continue
        value = Path(path)
        result.add(str(value))
        try:
            result.add(str(value.resolve()))
        except OSError:
            pass
    return result


def run_command(argv, cwd: Path, timeout: float, out: Path,
                log: list[dict], label: str, env: dict[str, str],
                strace: Path | None = None, *,
                audit_allowed: set[str] | None = None,
                trace_scope: str = COMPILER_TRACE_SCOPE,
                meta: dict | None = None,
                in_report_rows: bool = True) -> dict:
    """Run one real command and persist its complete evidence first."""
    if not math.isfinite(timeout) or timeout <= 0:
        raise RunnerError(f"{label}: timeout must be finite and positive")
    logs = out / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    sequence = len(log)
    stem = f"{sequence:03d}-{label_name(label)}"
    stdout_path = logs / f"{stem}.stdout"
    stderr_path = logs / f"{stem}.stderr"
    tool_argv = [str(part) for part in argv]
    real_argv = list(tool_argv)
    trace_path = None
    if strace is not None:
        traces = out / "traces"
        traces.mkdir(parents=True, exist_ok=True)
        trace_path = traces / f"{stem}.trace"
        real_argv = [str(strace), "--seccomp-bpf", "-f", "-qq", "-s",
                     "16384", "-e", "trace=execve,execveat", "-o",
                     str(trace_path), *real_argv]
    started = time.monotonic()
    try:
        process = _run_process_impl(real_argv, cwd, env, timeout)
        launch_error = None
        stdout_bytes = process["stdout"]
        stderr_bytes = process["stderr"]
        exit_code = process["exit"]
        timed_out = process["timed_out"]
        cleanup_complete = process["cleanup_complete"]
        abandoned_pipes = process["abandoned_pipes"]
    except OSError as error:
        # A failed spawn is recorded, never converted into an expected
        # language rejection.  Independent later cases may still run.
        launch_error = str(error)
        stdout_bytes = b""
        stderr_bytes = b""
        exit_code = None
        timed_out = False
        cleanup_complete = True
        abandoned_pipes = False
    elapsed = time.monotonic() - started
    stdout_path.write_bytes(stdout_bytes)
    stderr_path.write_bytes(stderr_bytes)
    record = dict(meta or {})
    record.update({
        "label": label,
        "in_report_rows": in_report_rows,
        "argv": real_argv,
        "tool_argv": tool_argv,
        "cwd": str(cwd),
        "timeout_seconds": timeout,
        "elapsed_seconds": elapsed,
        "status": process_status(launch_error, exit_code, timed_out),
        "exit": exit_code,
        "timed_out": timed_out,
        "cleanup_complete": cleanup_complete,
        "abandoned_pipes": abandoned_pipes,
        "launch_error": launch_error,
        "stdout_path": str(stdout_path),
        "stderr_path": str(stderr_path),
        "stdout_sha256": digest_bytes(stdout_bytes),
        "stderr_sha256": digest_bytes(stderr_bytes),
        "stdout": decode_bytes(stdout_bytes),
        "stderr": decode_bytes(stderr_bytes),
        "trace_path": None,
        "trace_sha256": None,
        "trace_scope": trace_scope if trace_path is not None else None,
        "allowlist": (sorted(audit_allowed)
                      if audit_allowed is not None else None),
    })

    def persist() -> None:
        if not log or log[-1] is not record:
            log.append(record)
        destination = out / "commands.jsonl"
        pending = destination.with_name(destination.name + ".pending")
        with pending.open("w", encoding="utf-8") as stream:
            for item in log:
                stream.write(json.dumps(item, ensure_ascii=False,
                                        sort_keys=True) + "\n")
        pending.replace(destination)

    persist()
    if trace_path is not None:
        record["trace_path"] = str(trace_path)
        record["trace_sha256"] = digest(trace_path) if trace_path.is_file() else ""
    if strace is not None and launch_error is None:
        if not trace_path.is_file():
            record["trace"] = {"status": "violation",
                               "error": f"missing exec trace {trace_path}"}
            persist()
            raise RunnerError(f"{label}: missing exec trace {trace_path}")
        try:
            if audit_allowed is not None:
                # The allowlist is an upper bound.  A compiler may legitimately
                # use only a subset of the fixed LLVM/Clang/LLD tools.
                audit = audit_trace(trace_path, audit_allowed)
                record["trace"] = dict(audit, status="ok")
                record["trace_audit"] = record["trace"]
                audit_path = Path(str(trace_path) + ".audit.json")
                write_json(audit_path, {
                    "label": label,
                    "trace_scope": trace_scope,
                    **record["trace"],
                })
            else:
                observed = parse_trace(trace_path)
                record["trace"] = {
                    "status": "ok",
                    "observed": observed,
                    "policy": trace_scope,
                }
        except TraceError as error:
            record["trace"] = {"status": "violation", "error": str(error)}
            record["trace_audit_error"] = f"{type(error).__name__}: {error}"
            persist()
            raise RunnerError(
                f"{label}: exec allowlist rejected trace: {error}") from error
        persist()
    else:
        record["trace"] = {
            "status": "not_required" if strace is None else "launch_failed",
            "policy": trace_scope,
        }
        persist()
    return record


def tree_map(root: Path, suffix: str | None = None) -> dict[str, str]:
    root = Path(root)
    if not root.is_dir():
        raise RunnerError(f"input directory is missing: {root}")
    result: dict[str, str] = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if any(part in (".build", "__pycache__") for part in relative.parts):
            continue
        if suffix is not None and path.suffix != suffix:
            continue
        result[str(relative)] = digest(path)
    return result


def file_record(path: Path | None) -> dict | None:
    if path is None:
        return None
    path = Path(path)
    try:
        data = path.read_bytes()
    except OSError as error:
        return {"path": str(path), "error": str(error)}
    return {"path": str(path), "size": len(data),
            "sha256": digest_bytes(data)}


def frozen_file_record(label: str, path: Path) -> dict:
    """Freeze a file identity before it is used by any role."""
    record = file_record(Path(path))
    if record is None or "sha256" not in record:
        raise RunnerError(f"{label}: missing or unreadable file {path}: {record}")
    return record


def ensure_frozen_file(label: str, path: Path, frozen: dict) -> None:
    """Reject a disappeared or changed file after its first SHA was saved."""
    current = file_record(Path(path))
    if (current is None or current.get("sha256") != frozen.get("sha256") or
            current.get("size") != frozen.get("size")):
        raise RunnerError(
            f"{label}: file identity changed or disappeared: {path}; "
            f"expected {frozen.get('sha256')}, got {current}")


def fixture_artifact_record(fixture_obj: Path, fixture_lib: Path,
                            fixture_link: Path) -> dict:
    """Record the shared fixture object/archive and their private copies."""
    record: dict = {}
    pairs = {
        "object": (Path(fixture_obj), Path(fixture_link) / fixture_obj.name),
        "archive": (Path(fixture_lib), Path(fixture_link) / fixture_lib.name),
    }
    for name, (shared_path, private_path) in pairs.items():
        shared = file_record(shared_path)
        private = file_record(private_path)
        consistent = bool(
            shared and private and shared.get("sha256") and
            private.get("sha256") and
            shared.get("size") == private.get("size") and
            shared.get("sha256") == private.get("sha256"))
        record[name] = {
            "shared": shared,
            "private": private,
            "copy_consistent": consistent,
        }
    record["ok"] = all(record[name]["copy_consistent"] for name in pairs)
    return record


def ensure_fixture_artifacts(label: str, frozen: dict, fixture_obj: Path,
                             fixture_lib: Path, fixture_link: Path) -> dict:
    """Compare object/archive bytes with the SHA saved right after copying."""
    current = fixture_artifact_record(fixture_obj, fixture_lib, fixture_link)
    if not current["ok"]:
        raise RunnerError(
            f"{label}: shared/private fixture object/archive is missing or "
            f"is not an identical copy: "
            f"{json.dumps(current, sort_keys=True)}")
    if canonical_sha(current) != canonical_sha(frozen):
        raise RunnerError(
            f"{label}: fixture shared/private object or archive bytes "
            f"changed after the initial copy; before={canonical_sha(frozen)} "
            f"after={canonical_sha(current)}")
    return current


def snapshot_tree(root: Path, suffix: str | None = None) -> dict:
    root = Path(root)
    files = tree_map(root, suffix)
    return {"path": str(root), "files": files,
            "map_sha256": canonical_sha(files)}


def map_changes(before: dict, after: dict) -> dict:
    before_keys = set(before)
    after_keys = set(after)
    return {
        "added": sorted(after_keys - before_keys),
        "removed": sorted(before_keys - after_keys),
        "changed": sorted(key for key in before_keys & after_keys
                          if before[key] != after[key]),
    }


def ensure_maps_equal(label: str, before: dict, after: dict) -> None:
    changes = map_changes(before, after)
    if any(changes.values()):
        raise RunnerError(f"{label}: input set changed during execution: "
                          f"{changes}")


def ensure_equal(label: str, before, after) -> None:
    if before != after:
        raise RunnerError(f"{label}: input resources changed during execution")


def prepare_output(out: Path, strict: bool) -> None:
    if out.exists():
        if not out.is_dir():
            raise RunnerError(f"--out is not a directory: {out}")
        if strict and any(out.iterdir()):
            raise RunnerError(
                f"refusing to overwrite existing evidence in --out: {out}")
    out.mkdir(parents=True, exist_ok=True)


def unique_run_dir(out: Path, prefix: str = "run") -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    base = out / "runs" / f"{prefix}-{stamp}-{os.getpid()}"
    candidate = base
    index = 1
    while candidate.exists():
        candidate = Path(str(base) + f"-{index}")
        index += 1
    candidate.mkdir(parents=True, exist_ok=False)
    return candidate


def make_work_dir(work: Path) -> None:
    if work.exists() and (not work.is_dir() or any(work.iterdir())):
        raise RunnerError(f"work directory is not empty: {work}")
    work.mkdir(parents=True, exist_ok=True)
    (work / "tmp").mkdir(parents=True, exist_ok=True)


def copy_input_mirror(root: Path, work: Path) -> dict[str, Path]:
    """Copy the original C-FFI inputs into private work bytes.

    ``../../.build`` inside project manifests therefore resolves to the
    private fixture archive, never to the owner repository or Root tree.
    """
    root = Path(root)
    source_cffi = root / "tests/native/c_ffi"
    source_core = root / "tests/fixtures/core"
    for path in (source_cffi, source_core):
        if not path.is_dir():
            raise RunnerError(f"missing original input directory: {path}")
    mirror = work / "mirror"
    target_cffi = mirror / "tests/native/c_ffi"
    target_core = mirror / "tests/fixtures/core"
    ignore = shutil.ignore_patterns(".build", "__pycache__", "*.pyc")
    shutil.copytree(source_cffi, target_cffi, ignore=ignore)
    shutil.copytree(source_core, target_core, ignore=ignore)
    # The private mirror must contain exactly the original bytes.
    for source, target in ((source_cffi, target_cffi),
                           (source_core, target_core)):
        original = tree_map(source)
        copied = tree_map(target)
        ensure_maps_equal(f"private mirror of {source}", original, copied)
    return {
        "root": mirror,
        "cffi": target_cffi,
        "core": target_core,
        "fixture_link": target_cffi / ".build",
    }


def environment(work: Path, toolchain_root: Path | None) -> dict[str, str]:
    env = dict(os.environ)
    env["WOOZY_FFI_ENV"] = "1"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["TMPDIR"] = str(work / "tmp")
    if toolchain_root is not None:
        llvm_bin = toolchain_root / "llvm-22.1.8/bin"
        if llvm_bin.is_dir():
            env["PATH"] = str(llvm_bin) + ":" + env.get("PATH", "")
        paths = []
        llvm = toolchain_root / "llvm-22.1.8/lib"
        libxml = toolchain_root / "libxml2-legacy-2.13.9/usr/lib"
        if llvm.is_dir():
            paths.append(str(llvm))
        if libxml.is_dir():
            paths.append(str(libxml))
        if env.get("LD_LIBRARY_PATH"):
            paths.append(env["LD_LIBRARY_PATH"])
        if paths:
            env["LD_LIBRARY_PATH"] = ":".join(paths)
    return env


def toolchain_tools(toolchain_root: Path | None) -> dict[str, Path]:
    if toolchain_root is None:
        return {}
    llvm = Path(toolchain_root) / "llvm-22.1.8/bin"
    result = {}
    for name in LLVM_TOOL_NAMES:
        path = llvm / name
        if path.is_file():
            result[name] = path.resolve()
    return result


def source_paths(record: dict) -> list[tuple[str, ...]]:
    return [
        ("source", "map_sha256"),
        ("source_map_sha256",),
        ("source116_map_sha256",),
        ("source116", "map_sha256"),
        ("inputs_before", "source116", "map_sha256"),
    ]


def nested_value(record: dict, paths: list[tuple[str, ...]], label: str):
    for path in paths:
        value = record
        found = True
        for key in path:
            if not isinstance(value, dict) or key not in value:
                found = False
                break
            value = value[key]
        if found:
            return value
    raise RunnerError(f"{label}: build record is missing its source map")


def extract_source_map(record: dict, label: str) -> str:
    value = nested_value(record, source_paths(record), label)
    if not isinstance(value, str) or not value:
        raise RunnerError(f"{label}: source map is not a non-empty string")
    return value


def extract_cli_sha(record: dict, label: str) -> str:
    value = nested_value(record, [
        ("cli", "sha256"), ("binary", "sha256"), ("artifact", "sha256"),
        ("woozyc", "sha256"), ("cli_sha256",),
    ], label)
    if not isinstance(value, str) or not value:
        raise RunnerError(f"{label}: CLI SHA is not a non-empty string")
    return value


def extract_component_sha(record: dict, label: str) -> str:
    value = nested_value(record, [
        ("driver", "sha256"), ("component", "sha256"),
        ("component_sha256",), ("binary", "sha256"),
    ], label)
    if not isinstance(value, str) or not value:
        raise RunnerError(f"{label}: component SHA is not a non-empty string")
    return value


def extract_stdlib_map(record: dict, label: str) -> str:
    value = nested_value(record, [
        ("stdlib", "map_sha256"), ("stdlib_map_sha256",),
        ("engine_resources", "stdlib", "map_sha256"),
        ("inputs_before", "engine", "stdlib", "map_sha256"),
    ], label)
    if not isinstance(value, str) or not value:
        raise RunnerError(f"{label}: stdlib map is not a non-empty string")
    return value


def extract_manifest_sha(record: dict, label: str) -> str:
    value = nested_value(record, [
        ("forged_project", "manifest", "sha256"),
        ("manifest_sha256",),
        ("inputs_before", "forged_project", "manifest", "sha256"),
    ], label)
    if not isinstance(value, str) or not value:
        raise RunnerError(f"{label}: forged manifest SHA is not a string")
    return value


def record_status_passed(record: dict, label: str) -> None:
    status = record.get("status")
    if status not in ("passed", "ok"):
        raise RunnerError(f"{label}: build record status is {status!r}, "
                          f"not passed/ok")


def validate_cli_identity(record: dict, cli: Path,
                          source_snapshot: dict) -> dict:
    label = f"CLI identity {cli}"
    record_status_passed(record, label)
    cli_sha = extract_cli_sha(record, label)
    actual_sha = digest(cli)
    if cli_sha != actual_sha:
        raise RunnerError(f"{label}: record CLI SHA {cli_sha} != "
                          f"actual {actual_sha}")
    source_sha = extract_source_map(record, label)
    if source_sha != source_snapshot["map_sha256"]:
        raise RunnerError(f"{label}: CLI Source {source_sha} != supplied "
                          f"snapshot {source_snapshot['map_sha256']}")
    return {"cli_sha256": cli_sha, "source_map_sha256": source_sha,
            "record_status": record.get("status")}


def validate_component_identity(record: dict, component: Path,
                                source_snapshot: dict, stdlib_snapshot: dict,
                                forged_manifest: Path) -> dict:
    label = f"Wz forged component {component}"
    if record.get("historical") is True:
        raise RunnerError(f"{label}: historical component cannot be the "
                          "candidate current-Source component")
    record_status_passed(record, label)
    component_sha = extract_component_sha(record, label)
    actual_sha = digest(component)
    if component_sha != actual_sha:
        raise RunnerError(f"{label}: record component SHA {component_sha} != "
                          f"actual {actual_sha}")
    source_sha = extract_source_map(record, label)
    if source_sha != source_snapshot["map_sha256"]:
        raise RunnerError(f"{label}: component Source {source_sha} != "
                          f"candidate Source {source_snapshot['map_sha256']}")
    stdlib_sha = extract_stdlib_map(record, label)
    if stdlib_sha != stdlib_snapshot["map_sha256"]:
        raise RunnerError(f"{label}: component stdlib {stdlib_sha} != "
                          f"supplied stdlib {stdlib_snapshot['map_sha256']}")
    manifest_sha = extract_manifest_sha(record, label)
    actual_manifest = digest(forged_manifest)
    if manifest_sha != actual_manifest:
        raise RunnerError(f"{label}: component manifest {manifest_sha} != "
                          f"forged-project manifest {actual_manifest}")
    return {"component_sha256": component_sha,
            "source_map_sha256": source_sha,
            "stdlib_map_sha256": stdlib_sha,
            "manifest_sha256": manifest_sha,
            "record_status": record.get("status")}


def build_success(record: dict, document: dict, program: Path) -> bool:
    try:
        require_ok(record)
    except RunnerError:
        return False
    if not isinstance(document, dict):
        return False
    if document.get("status") not in ("ok", "passed"):
        return False
    return Path(program).is_file()


def require_build_success(record: dict, document: dict, program: Path) -> None:
    require_ok(record)
    if document.get("status") not in ("ok", "passed"):
        raise RunnerError(f"{record.get('label')}: build JSON status "
                          f"{document.get('status')!r} is not ok/passed")
    if not Path(program).is_file():
        raise RunnerError(f"{record.get('label')}: build reported success but "
                          f"target artifact is missing: {program}")


def diagnostics_codes(document: dict, label: str) -> list[str]:
    diagnostics = document.get("diagnostics")
    if not isinstance(diagnostics, list):
        raise RunnerError(f"{label}: diagnostics is not an array")
    codes = []
    for index, item in enumerate(diagnostics):
        if not isinstance(item, dict) or not isinstance(item.get("code"), str):
            raise RunnerError(f"{label}: diagnostics[{index}] has no string code")
        codes.append(item["code"])
    return codes


def negative_check_passed(record: dict, document: dict,
                          expected_code: str) -> bool:
    try:
        require_process(record)
        if document.get("status") != "error":
            return False
        return expected_code in diagnostics_codes(document, "negative")
    except RunnerError:
        return False


def link_failure_passed(record: dict, document: dict,
                        program: Path) -> bool:
    try:
        require_process(record)
    except RunnerError:
        return False
    if record["exit"] == 0:
        return False
    if document.get("status") != "error":
        return False
    try:
        if "E_LINK_FAILED" not in diagnostics_codes(document, "link-failure"):
            return False
    except RunnerError:
        return False
    return not Path(program).exists()


def bounds_passed(record: dict) -> bool:
    try:
        require_process(record)
    except RunnerError:
        return False
    if record["exit"] == 0:
        return False
    return "LANG_NATIVE_PANIC:v1:vec-index:" in record.get("stderr", "")


def validate_elf(label: str, text: str, kind: str) -> None:
    if kind in ("hosted", "hosted-libc"):
        if "INTERP" not in text:
            raise RunnerError(f"{label}: hosted ELF has no INTERP")
        if "libstdc++" in text:
            raise RunnerError(f"{label}: hosted ELF links libstdc++")
        if kind == "hosted-libc" and "libcrypto" not in text:
            raise RunnerError(f"{label}: libc ELF has no libcrypto")
    elif kind == "core":
        if "INTERP" in text:
            raise RunnerError(f"{label}: core ELF unexpectedly has INTERP")
        if "NEEDED" in text:
            raise RunnerError(f"{label}: core ELF unexpectedly has NEEDED")
    else:
        raise RunnerError(f"{label}: unknown ELF kind {kind!r}")


def validate_archive_symbols(hosted_text: str, native_text: str) -> None:
    if "lang_hosted_startup_v1" not in hosted_text:
        raise RunnerError("hosted archive is missing lang_hosted_startup_v1")
    if " _start" in hosted_text:
        raise RunnerError("hosted archive contains a native _start")
    for token in ("__cxa_", "_Unwind", "_ZSt9terminatev"):
        if token in hosted_text:
            raise RunnerError(f"hosted archive contains C++ runtime {token}")
    if "lang_thread_spawn_v1" not in native_text:
        raise RunnerError("native archive is missing lang_thread_spawn_v1")


_COMPONENT_MODULE = None


def load_accepted_forged_module():
    """Load the accepted Wz component's output-parser module read-only."""
    global _COMPONENT_MODULE
    if _COMPONENT_MODULE is not None:
        return _COMPONENT_MODULE
    path = COMPILER_ROOT / "selfhost/tests/cffi_forged/run.py"
    spec = importlib.util.spec_from_file_location(
        "woozy_cffi_forged_component_reference", path)
    if spec is None or spec.loader is None:
        raise RunnerError(f"cannot load accepted forged parser {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _COMPONENT_MODULE = module
    return module


def parse_forged_output(exit_code, stdout_text: str,
                        stderr_text: str) -> dict:
    module = load_accepted_forged_module()
    return module.parse_component_output(exit_code, stdout_text, stderr_text)


def forged_component_passed(record: dict) -> dict:
    require_process(record)
    parsed = parse_forged_output(record["exit"], record.get("stdout", ""),
                                 record.get("stderr", ""))
    if parsed["status"] != "passed":
        raise RunnerError("Wz forged component output is not 8 exact CASE "
                          "rejections: " + json.dumps(parsed["violations"]))
    return parsed


def _record_failure(report: dict, label: str, detail="") -> None:
    message = label + (": " + detail if detail else "")
    report.setdefault("failed", []).append(message)


def _known_record(log: list[dict], record: dict | None) -> dict | None:
    """Keep the persisted current command even when a post-check raised."""
    if record is not None:
        return record
    return log[-1] if log else None


def _row(report: dict, label: str, role: str, category: str,
         case: str | None = None, **fields) -> dict:
    entry = {"name": label, "role": role, "category": category}
    if case is not None:
        entry["case"] = case
    entry.update(fields)
    report["runs"].append(entry)
    return entry


def _not_run(report: dict, label: str, role: str, category: str,
             case: str | None, reason: str, **fields) -> dict:
    report.setdefault("not_run", []).append(
        {"name": label, "reason": reason})
    return _row(report, label, role, category, case, status="not_run",
                passed=False, reason=reason, **fields)


def _row_result(report: dict, label: str, role: str, category: str,
                case: str | None, record: dict | None, passed: bool,
                assertions: list[dict] | None = None,
                error: str | None = None, **extra) -> dict:
    if passed:
        return _row(report, label, role, category, case, status="passed",
                    passed=True, result=record, assertions=assertions or [],
                    **extra)
    detail = error or ""
    _record_failure(report, label, detail)
    return _row(report, label, role, category, case, status="failed",
                passed=False, result=record, assertions=assertions or [],
                error=error, **extra)


def _compiler_allowlist(cli: Path, tools: dict[str, Path],
                        native_helper: Path | None = None) -> set[str]:
    # The C++ native helper is a separate reference-only compiler input.  It
    # must never be mixed into the shared LLVM/Clang/LLD tool set: candidate
    # roles pass native_helper=None and therefore reject it.
    paths = [cli, *tools.values()]
    if native_helper is not None:
        paths.append(native_helper)
    return exec_aliases(paths)


def _run_build(report: dict, log: list[dict], run_dir: Path, role: str,
               cli: Path, manifest: Path, opt: int, label: str,
               out_dir: Path, env: dict[str, str], strace: Path | None,
               allowlist: set[str], case: str) -> tuple[dict | None, Path]:
    program = out_dir / "program"
    out_dir.parent.mkdir(parents=True, exist_ok=True)
    if out_dir.exists():
        shutil.rmtree(out_dir)
    record = None
    document = None
    try:
        record = run_command(
            [cli, "build", "--project", manifest, "--backend", "native",
             "--opt", str(opt), "--out-dir", out_dir, "--format", "json"],
            run_dir, NORMAL_BUILD_SECONDS, run_dir, log, label, env,
            strace, audit_allowed=allowlist,
            trace_scope=COMPILER_TRACE_SCOPE,
            meta={"role": role, "category": "build", "case": case})
        document = parse_json_record(record)
        require_build_success(record, document, program)
    except RunnerError as error:
        record = _known_record(log, record)
        _row_result(report, label, role, "build", case, record, False,
                    error=str(error))
        return None, program
    _row_result(report, label, role, "build", case, record, True,
                assertions=[
                    {"assertion": "exact exited/0/clean process", "passed": True},
                    {"assertion": "strict JSON status ok/passed", "passed": True},
                    {"assertion": "target program exists", "passed": True},
                ])
    return document, program


def _run_check(report: dict, log: list[dict], run_dir: Path, role: str,
               cli: Path, name: str, code: str, manifest: Path,
               env: dict[str, str], strace: Path | None,
               allowlist: set[str]) -> None:
    label = f"{role}:check:{name}"
    record = None
    document = None
    try:
        record = run_command(
            [cli, "check", "--project", manifest, "--backend", "native",
             "--format", "json"],
            run_dir, NEGATIVE_CHECK_SECONDS, run_dir, log, label, env,
            strace, audit_allowed=allowlist,
            trace_scope=COMPILER_TRACE_SCOPE,
            meta={"role": role, "category": "negative-check", "case": name,
                  "expected_code": code})
        document = parse_json_record(record)
        passed = negative_check_passed(record, document, code)
        if not passed:
            raise RunnerError(f"expected strict status error containing {code}")
    except RunnerError as error:
        record = _known_record(log, record)
        _row_result(report, label, role, "negative-check", name, record,
                    False, error=str(error))
        return
    _row_result(report, label, role, "negative-check", name, record, True,
                assertions=[{"assertion": f"diagnostic code {code}",
                             "passed": True}])


def _run_readelf(report: dict, log: list[dict], run_dir: Path, role: str,
                 readelf, program: Path, label: str, case: str,
                 kind: str, env: dict[str, str],
                 strace: Path | None) -> bool:
    record = None
    try:
        record = run_command(
            [readelf, "-l", "-d", program],
            run_dir, READELF_SECONDS, run_dir, log, label, env, strace,
            audit_allowed=None, trace_scope=PROGRAM_TRACE_SCOPE,
            meta={"role": role, "category": "readelf", "case": case})
        require_ok(record)
        text = record["stdout"]
        validate_elf(label.replace(f"{role}:readelf:", f"{case}:"), text, kind)
    except RunnerError as error:
        record = _known_record(log, record)
        _row_result(report, label, role, "readelf", case, record, False,
                    error=str(error))
        return False
    _row_result(report, label, role, "readelf", case, record, True,
                assertions=[{"assertion": f"ELF kind {kind}", "passed": True}])
    return True


def _run_program(report: dict, log: list[dict], run_dir: Path, role: str,
                 program: Path, label: str, case: str, kind: str,
                 env: dict[str, str], strace: Path | None) -> None:
    record = None
    try:
        record = run_command(
            [program], run_dir, PROGRAM_SECONDS, run_dir, log, label, env,
            strace, audit_allowed=None, trace_scope=PROGRAM_TRACE_SCOPE,
            meta={"role": role, "category": "program", "case": case})
        if kind == "normal":
            require_ok(record)
        elif kind == "bounds":
            if not bounds_passed(record):
                raise RunnerError("bounds program did not exit nonzero with "
                                  "LANG_NATIVE_PANIC:v1:vec-index:")
        else:
            raise RunnerError(f"unknown program kind {kind!r}")
    except RunnerError as error:
        record = _known_record(log, record)
        _row_result(report, label, role, "program", case, record, False,
                    error=str(error))
        return
    assertions = [{"assertion": "exact exited/clean process", "passed": True}]
    if kind == "normal":
        assertions.append({"assertion": "exit 0; raw stdout/stderr retained "
                                        "without extra text gold",
                           "passed": True})
    else:
        assertions.append({"assertion": "nonzero exit and original Panic "
                                        "marker", "passed": True})
    _row_result(report, label, role, "program", case, record, True,
                assertions=assertions)


def _run_link_failure(report: dict, log: list[dict], run_dir: Path,
                      role: str, cli: Path, manifest: Path, out_dir: Path,
                      env: dict[str, str], strace: Path | None,
                      allowlist: set[str]) -> None:
    label = f"{role}:build:link-failure"
    program = out_dir / "program"
    out_dir.parent.mkdir(parents=True, exist_ok=True)
    if out_dir.exists():
        shutil.rmtree(out_dir)
    record = None
    document = None
    try:
        record = run_command(
            [cli, "build", "--project", manifest, "--backend", "native",
             "--opt", "0", "--out-dir", out_dir, "--format", "json"],
            run_dir, LINK_FAILURE_SECONDS, run_dir, log, label, env, strace,
            audit_allowed=allowlist, trace_scope=COMPILER_TRACE_SCOPE,
            meta={"role": role, "category": "link-failure",
                  "case": "link-failure"})
        document = parse_json_record(record)
        if not link_failure_passed(record, document, program):
            raise RunnerError("link failure was not an unsuccessful strict "
                              "E_LINK_FAILED build without program")
    except RunnerError as error:
        record = _known_record(log, record)
        _row_result(report, label, role, "link-failure", "link-failure",
                    record, False, error=str(error))
        return
    _row_result(report, label, role, "link-failure", "link-failure",
                record, True, assertions=[
                    {"assertion": "nonzero exit", "passed": True},
                    {"assertion": "strict status error with E_LINK_FAILED",
                     "passed": True},
                    {"assertion": "no out/program artifact", "passed": True},
                ])


def _run_nm(report: dict, log: list[dict], run_dir: Path, role: str,
            nm, hosted: Path, native: Path, env: dict[str, str],
            strace: Path | None) -> None:
    for name, archive in (("hosted", hosted), ("native", native)):
        label = f"{role}:archive:{name}"
        record = None
        try:
            record = run_command(
                [nm, "-g", "--defined-only", archive],
                run_dir, ARCHIVE_SYMBOL_SECONDS, run_dir, log, label, env,
                strace, audit_allowed=None, trace_scope=PROGRAM_TRACE_SCOPE,
                meta={"role": role, "category": "archive-symbols",
                      "case": name})
            require_ok(record)
        except RunnerError as error:
            record = _known_record(log, record)
            _row_result(report, label, role, "archive-symbols", name,
                        record, False, error=str(error))
        else:
            _row_result(report, label, role, "archive-symbols", name,
                        record, True,
                        assertions=[{"assertion": "exact exited/0/clean "
                                                  "symbol dump",
                                     "passed": True}])
    # The two archive rows must be independent but their assertions compare the
    # captured raw stdout from the just-recorded rows.
    rows = [entry for entry in report["runs"]
            if entry["name"] in (f"{role}:archive:hosted",
                                 f"{role}:archive:native")]
    if len(rows) != 2 or any(entry.get("result") is None for entry in rows):
        return
    hosted_record = next(entry["result"] for entry in rows
                         if entry["name"].endswith(":hosted"))
    native_record = next(entry["result"] for entry in rows
                         if entry["name"].endswith(":native"))
    try:
        validate_archive_symbols(hosted_record.get("stdout", ""),
                                 native_record.get("stdout", ""))
    except RunnerError as error:
        for entry in rows:
            entry["status"] = "failed"
            entry["passed"] = False
            entry["error"] = str(error)
        _record_failure(report, f"{role}:archive:symbols", str(error))


def _run_forged_cpp(report: dict, log: list[dict], run_dir: Path,
                    role: str, forged: Path | None, manifest: Path,
                    env: dict[str, str], strace: Path | None) -> None:
    label = f"{role}:forged:HIR+IR"
    if forged is None or not Path(forged).is_file():
        _not_run(report, label, role, "forged", "HIR+IR",
                 "reference C++ forged target not supplied")
        return
    record = None
    try:
        record = run_command(
            [forged, manifest], run_dir, FORGED_SECONDS, run_dir, log,
            label, env, strace,
            audit_allowed=exec_aliases([forged]),
            trace_scope=FORGED_CPP_TRACE_SCOPE,
            meta={"role": role, "category": "forged", "case": "HIR+IR"})
        require_ok(record)
    except RunnerError as error:
        record = _known_record(log, record)
        _row_result(report, label, role, "forged", "HIR+IR", record, False,
                    error=str(error))
        return
    _row_result(report, label, role, "forged", "HIR+IR", record, True,
                assertions=[{"assertion": "original C++ forged exit 0; "
                                          "no new stdout/stderr gold",
                             "passed": True}])


def _run_forged_wz(report: dict, log: list[dict], run_dir: Path,
                   role: str, component: Path | None, manifest: Path,
                   stdlib: Path | None, env: dict[str, str],
                   strace: Path | None, identity_ok: bool,
                   identity_error: str | None = None) -> None:
    label = f"{role}:forged:HIR+IR"
    if not identity_ok:
        _not_run(report, label, role, "forged", "HIR+IR",
                 f"candidate Source/component identity rejected: "
                 f"{identity_error}")
        return
    if component is None or not Path(component).is_file():
        _not_run(report, label, role, "forged", "HIR+IR",
                 "same-Source Wz forged component is pending; the old ed1 "
                 "component is not a current-Source substitute")
        return
    if stdlib is None or not Path(stdlib).is_dir():
        _not_run(report, label, role, "forged", "HIR+IR",
                 "stdlib is required for the Wz component")
        return
    record = None
    parsed = None
    try:
        record = run_command(
            [component, manifest, stdlib], run_dir, FORGED_SECONDS, run_dir,
            log, label, env, strace,
            audit_allowed=exec_aliases([component]),
            trace_scope=FORGED_WZ_TRACE_SCOPE,
            meta={"role": role, "category": "forged-wz", "case": "HIR+IR"})
        parsed = forged_component_passed(record)
    except RunnerError as error:
        record = _known_record(log, record)
        _row_result(report, label, role, "forged-wz", "HIR+IR", record,
                    False, error=str(error))
        return
    _row_result(report, label, role, "forged-wz", "HIR+IR", record, True,
                assertions=[
                    {"assertion": "8 exact CASE lines rejected=E_IR_INVALID "
                                  "baseline=ok", "passed": True},
                    {"assertion": "legal HIR/NativeIR baselines", "passed": True},
                    {"assertion": "positive HIR/NativeIR coverage", "passed": True},
                    {"assertion": "exactly one TOTAL 8passed 0failed",
                     "passed": True},
                ], parsed=parsed)


def run_role(report: dict, log: list[dict], run_dir: Path, role: str,
             cli: Path, role_root: Path, forged_kind: str,
             forged: Path | None, component: Path | None,
             stdlib: Path | None, runtimes: dict[str, Path],
             clang: Path, ar: Path, readelf, nm, env: dict[str, str],
             toolchain_root: Path | None, strace: Path | None,
             identity_ok: bool, identity_error: str | None,
             forged_identity_ok: bool = True,
             forged_identity_error: str | None = None,
             native_helper: Path | None = None) -> None:
    mirror = run_dir / "mirror"
    cffi = mirror / "tests/native/c_ffi"
    core = mirror / "tests/fixtures/core"
    forged_manifest = cffi / "forged-project/lang.toml"
    out_root = run_dir / "out" / role
    out_root.mkdir(parents=True, exist_ok=True)
    tools = toolchain_tools(toolchain_root)
    allowlist = _compiler_allowlist(cli, tools, native_helper)

    if not identity_ok:
        # Keep the complete expected row set, but execute nothing for a role
        # whose CLI/component Source identity was rejected.
        for name in PROJECTS:
            for opt in (0, 2):
                base = f"{role}:"
                _not_run(report, base + f"build:{name}:O{opt}", role,
                         "build", f"{name}:O{opt}", identity_error)
                _not_run(report, base + f"readelf:{name}:O{opt}", role,
                         "readelf", f"{name}:O{opt}", identity_error)
                _not_run(report, base + f"run:{name}:O{opt}", role,
                         "program", f"{name}:O{opt}", identity_error)
        _not_run(report, f"{role}:build:bounds-panic", role, "build",
                 "bounds-panic", identity_error)
        _not_run(report, f"{role}:run:bounds-panic", role, "program",
                 "bounds-panic", identity_error)
        _not_run(report, f"{role}:build:core-freestanding", role, "build",
                 "core-freestanding", identity_error)
        _not_run(report, f"{role}:readelf:core-freestanding", role,
                 "readelf", "core-freestanding", identity_error)
        _not_run(report, f"{role}:run:core-freestanding", role, "program",
                 "core-freestanding", identity_error)
        for name in NEGATIVE:
            _not_run(report, f"{role}:check:{name}", role, "negative-check",
                     name, identity_error)
        _not_run(report, f"{role}:build:link-failure", role, "link-failure",
                 "link-failure", identity_error)
        _not_run(report, f"{role}:forged:HIR+IR", role, "forged", "HIR+IR",
                 identity_error)
        _not_run(report, f"{role}:archive:hosted", role, "archive-symbols",
                 "hosted", identity_error)
        _not_run(report, f"{role}:archive:native", role, "archive-symbols",
                 "native", identity_error)
        return

    for name in PROJECTS:
        for opt in (0, 2):
            case = f"{name}:O{opt}"
            build_label = f"{role}:build:{name}:O{opt}"
            document, program = _run_build(
                report, log, run_dir, role, cli,
                cffi / f"projects/{name}/lang.toml", opt, build_label,
                out_root / f"{name}-o{opt}", env, strace, allowlist, case)
            if document is None or not program.is_file():
                _not_run(report, f"{role}:readelf:{name}:O{opt}", role,
                         "readelf", case, "build failed")
                _not_run(report, f"{role}:run:{name}:O{opt}", role, "program",
                         case, "build failed")
                continue
            elf_ok = _run_readelf(
                report, log, run_dir, role, readelf, program,
                f"{role}:readelf:{name}:O{opt}", case,
                "hosted-libc" if name == "libc" else "hosted", env, strace)
            if not elf_ok:
                _not_run(report, f"{role}:run:{name}:O{opt}", role, "program",
                         case, "ELF assertion failed")
                continue
            _run_program(report, log, run_dir, role, program,
                         f"{role}:run:{name}:O{opt}", case, "normal", env,
                         strace)

    bounds_doc, bounds_program = _run_build(
        report, log, run_dir, role, cli,
        cffi / "projects/bounds/lang.toml", 0,
        f"{role}:build:bounds-panic", out_root / "bounds-panic",
        env, strace, allowlist, "bounds-panic")
    if bounds_doc is None or not bounds_program.is_file():
        _not_run(report, f"{role}:run:bounds-panic", role, "program",
                 "bounds-panic", "build failed")
    else:
        _run_program(report, log, run_dir, role, bounds_program,
                     f"{role}:run:bounds-panic", "bounds-panic", "bounds",
                     env, strace)

    core_doc, core_program = _run_build(
        report, log, run_dir, role, cli, core / "lang.toml", 0,
        f"{role}:build:core-freestanding", out_root / "core-freestanding",
        env, strace, allowlist, "core-freestanding")
    if core_doc is None or not core_program.is_file():
        _not_run(report, f"{role}:readelf:core-freestanding", role,
                 "readelf", "core-freestanding", "build failed")
        _not_run(report, f"{role}:run:core-freestanding", role, "program",
                 "core-freestanding", "build failed")
    else:
        elf_ok = _run_readelf(
            report, log, run_dir, role, readelf, core_program,
            f"{role}:readelf:core-freestanding", "core-freestanding",
            "core", env, strace)
        if elf_ok:
            _run_program(report, log, run_dir, role, core_program,
                         f"{role}:run:core-freestanding", "core-freestanding",
                         "normal", env, strace)
        else:
            _not_run(report, f"{role}:run:core-freestanding", role,
                     "program", "core-freestanding", "ELF assertion failed")

    for name, code in NEGATIVE.items():
        _run_check(report, log, run_dir, role, cli, name, code,
                   cffi / f"negative/{name}/lang.toml", env, strace,
                   allowlist)

    _run_link_failure(
        report, log, run_dir, role, cli,
        cffi / "negative/link-failure/lang.toml",
        out_root / "link-failure", env, strace, allowlist)

    if forged_kind == "cpp":
        _run_forged_cpp(report, log, run_dir, role, forged, forged_manifest,
                        env, strace)
    else:
        _run_forged_wz(report, log, run_dir, role, component, forged_manifest,
                       stdlib, env, strace, forged_identity_ok,
                       forged_identity_error)

    _run_nm(report, log, run_dir, role, nm, runtimes["hosted"],
            runtimes["native"], env, strace)


def artifact_hashes(run_dir: Path) -> dict[str, str]:
    result = {}
    for path in sorted(run_dir.rglob("*")):
        if not path.is_file():
            continue
        if path.name == "report.json":
            continue
        try:
            result[str(path.relative_to(run_dir))] = digest(path)
        except OSError:
            continue
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("legacy", "formal"),
                        default="legacy")
    # Original/CMake arguments.
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--ar", type=Path, required=True)
    parser.add_argument("--hosted-runtime", type=Path, required=True)
    parser.add_argument("--native-runtime", type=Path, required=True)
    parser.add_argument("--forged", type=Path)
    # Evidence/tooling additions; legacy keeps working without them.
    parser.add_argument("--strace", type=Path)
    parser.add_argument("--readelf", type=Path)
    parser.add_argument("--nm", type=Path)
    parser.add_argument("--toolchain-root", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--timeout", type=finite_timeout, default=None,
                        help="outer informational timeout only; per-command "
                             "original budgets are fixed")
    # Explicit formal roles.
    parser.add_argument("--reference-cli", type=Path)
    parser.add_argument("--reference-cli-record", type=Path)
    parser.add_argument("--reference-source-root", type=Path)
    parser.add_argument("--reference-native-helper", type=Path)
    parser.add_argument("--reference-forged", type=Path)
    parser.add_argument("--candidate-cli", type=Path)
    parser.add_argument("--candidate-cli-record", type=Path)
    parser.add_argument("--candidate-source-root", type=Path)
    parser.add_argument("--candidate-forged-component", type=Path)
    parser.add_argument("--candidate-component-record", type=Path)
    parser.add_argument("--candidate-stdlib", type=Path)
    return parser


def _require(args, names: list[str]) -> None:
    missing = []
    for name in names:
        value = getattr(args, name)
        if value is None:
            missing.append("--" + name.replace("_", "-"))
    if missing:
        raise RunnerError("missing required arguments: " + ", ".join(missing))


def _resolve_optional(path: Path | None, strict: bool) -> Path | None:
    if path is None:
        return None
    try:
        return path.resolve(strict=strict)
    except OSError as error:
        raise RunnerError(f"cannot resolve {path}: {error}") from error


def _check_tool(path, label: str, strict: bool) -> None:
    if path is None:
        return
    if isinstance(path, Path):
        if strict and not path.is_file():
            raise RunnerError(f"{label} is missing: {path}")
    elif strict:
        raise RunnerError(f"{label} is not a path: {path!r}")


def _run(args) -> int:
    formal = args.mode == "formal"
    root = args.root.resolve()
    out = args.out.resolve()
    if formal:
        _require(args, [
            "clang", "ar", "hosted_runtime", "native_runtime",
            "reference_cli", "reference_cli_record", "reference_source_root",
            "candidate_cli", "candidate_cli_record", "candidate_source_root",
            "candidate_stdlib", "strace", "toolchain_root",
            "reference_native_helper",
        ])
    else:
        _require(args, [
            "cli", "clang", "ar", "hosted_runtime", "native_runtime",
        ])
    strict_paths = formal
    cli = _resolve_optional(args.cli, strict_paths)
    reference_cli = _resolve_optional(args.reference_cli, strict_paths)
    candidate_cli = _resolve_optional(args.candidate_cli, strict_paths)
    reference_cli_record = _resolve_optional(args.reference_cli_record, True)
    candidate_cli_record = _resolve_optional(args.candidate_cli_record, True)
    reference_source_root = _resolve_optional(args.reference_source_root,
                                               strict_paths)
    candidate_source_root = _resolve_optional(args.candidate_source_root,
                                               strict_paths)
    component = _resolve_optional(args.candidate_forged_component, True)
    component_record = _resolve_optional(args.candidate_component_record, True)
    stdlib = _resolve_optional(args.candidate_stdlib, True)
    clang = args.clang.resolve()
    ar = args.ar.resolve()
    hosted_runtime = args.hosted_runtime.resolve()
    native_runtime = args.native_runtime.resolve()
    reference_forged = _resolve_optional(args.reference_forged, True)
    reference_native_helper = _resolve_optional(
        args.reference_native_helper, True)
    forged = _resolve_optional(args.forged, True)
    strace = _resolve_optional(args.strace, True)
    readelf_arg = _resolve_optional(args.readelf, True)
    nm_arg = _resolve_optional(args.nm, True)
    toolchain_root = _resolve_optional(args.toolchain_root, strict_paths)
    for path, label in (
            (clang, "clang"), (ar, "ar"),
            (hosted_runtime, "hosted runtime"),
            (native_runtime, "native runtime")):
        if strict_paths and not path.is_file():
            raise RunnerError(f"{label} is missing: {path}")
    readelf = readelf_arg or shutil.which("readelf") or "readelf"
    if nm_arg is not None:
        nm = nm_arg
    else:
        candidate_nm = clang.with_name("llvm-nm")
        nm = candidate_nm if candidate_nm.is_file() else Path("/usr/bin/nm")
    if formal:
        if not Path(str(readelf)).is_file():
            raise RunnerError(f"GNU readelf is missing: {readelf}")
        if not Path(nm).is_file():
            raise RunnerError(f"nm is missing: {nm}")

    reference_native_helper_record = None
    if formal:
        reference_native_helper_record = frozen_file_record(
            "reference native helper", reference_native_helper)

    prepare_output(out, strict=formal)
    run_dir = out if formal else unique_run_dir(out, "run")
    work = _resolve_optional(args.work_dir, False)
    if work is None:
        work = run_dir / "work"
    make_work_dir(work)
    # The mirror must live under this invocation, never under Root or the
    # owner repository's tests/native/c_ffi/.build.
    mirror = copy_input_mirror(root, work)
    # run_role expects the mirror directly under run_dir.
    mirror_target = run_dir / "mirror"
    if mirror["root"] != mirror_target:
        if mirror_target.exists():
            raise RunnerError(f"mirror target already exists: {mirror_target}")
        shutil.move(str(mirror["root"]), str(mirror_target))
        mirror = {
            "root": mirror_target,
            "cffi": mirror_target / "tests/native/c_ffi",
            "core": mirror_target / "tests/fixtures/core",
            "fixture_link": mirror_target / "tests/native/c_ffi/.build",
        }

    report = {
        "task": TASK,
        "operation_id": OPERATION_ID,
        "contract": CONTRACT,
        "mode": args.mode,
        "checked_at": utc_now(),
        "root": str(root),
        "out": str(out),
        "run_dir": str(run_dir),
        "work_dir": str(work),
        "runs": [],
        "commands": "commands.jsonl",
        "failed": [],
        "not_run": [],
        "limits": {
            "normal_build_seconds": NORMAL_BUILD_SECONDS,
            "negative_check_seconds": NEGATIVE_CHECK_SECONDS,
            "link_failure_seconds": LINK_FAILURE_SECONDS,
            "program_seconds": PROGRAM_SECONDS,
            "readelf_seconds": READELF_SECONDS,
            "forged_seconds": FORGED_SECONDS,
            "fixture_seconds": FIXTURE_SECONDS,
            "git_head_seconds": GIT_HEAD_SECONDS,
            "archive_symbols_seconds": ARCHIVE_SYMBOL_SECONDS,
            "outer_cmake_full_runner_seconds": 600,
            "outer_cmake_forged_seconds": 60,
            "component_preparation_seconds_separate": 1800,
        },
        "claims": {
            "source_or_compiler_executed_by_this_script_as_test": True,
            "component_built_by_this_runner": False,
            "product_source_modified": False,
            "fixtures_bindings_gold_modified": False,
            "catalog_plan_state_modified": False,
        },
    }
    log: list[dict] = []
    env = environment(work, toolchain_root)
    runtimes = {"hosted": hosted_runtime, "native": native_runtime}
    cffi = mirror["cffi"]
    core = mirror["core"]
    fixture_source = cffi / "fixture/ffi_fixture.c"
    fixture_obj = run_dir / "out/shared/ffi_fixture.o"
    fixture_lib = run_dir / "out/shared/libffi_fixture.a"
    fixture_link = mirror["fixture_link"]
    fixture_obj.parent.mkdir(parents=True, exist_ok=True)
    fixture_initial = None
    fixture_checks: list[dict] = []
    stability_ok = True

    # Freeze the actual arguments and input bytes before any command runs.
    source_snapshots: dict[str, dict] = {}
    if formal:
        source_snapshots["reference"] = snapshot_tree(reference_source_root,
                                                      ".wz")
        source_snapshots["candidate"] = snapshot_tree(candidate_source_root,
                                                      ".wz")
    else:
        default_source = root / "selfhost/src"
        if default_source.is_dir():
            source_snapshots["legacy"] = snapshot_tree(default_source, ".wz")
    stdlib_snapshot = snapshot_tree(stdlib, ".wz") if stdlib else None
    mirror_before = snapshot_tree(mirror["root"])
    resources_before = {
        "clang": file_record(clang),
        "ar": file_record(ar),
        "hosted_runtime": file_record(hosted_runtime),
        "native_runtime": file_record(native_runtime),
        "readelf": file_record(Path(str(readelf))),
        "nm": file_record(Path(nm)),
        "llvm_tools": {
            name: file_record(path)
            for name, path in toolchain_tools(toolchain_root).items()
        },
        "python": file_record(Path(sys.executable).resolve()),
        "corpus": file_record(SELFHOST / "corpus.py"),
        "runner": file_record(Path(__file__).resolve()),
        "fixture_source": file_record(fixture_source),
        "bindings": {
            name: file_record(cffi / f"bindings/{name}.json")
            for name in ("fixture", "libc")
        },
        "stdlib": stdlib_snapshot,
        "cli": {
            "legacy": file_record(cli) if cli else None,
            "reference": file_record(reference_cli) if reference_cli else None,
            "candidate": file_record(candidate_cli) if candidate_cli else None,
        },
        "build_records": {
            "reference_cli": file_record(reference_cli_record),
            "candidate_cli": file_record(candidate_cli_record),
            "candidate_component": file_record(component_record),
        },
        "reference_native_helper": reference_native_helper_record,
        "forged": {
            "reference_cpp": file_record(reference_forged or forged),
            "candidate_wz": file_record(component),
        },
    }
    write_json(run_dir / "inputs-before.json", {
        "sources": source_snapshots,
        "stdlib": stdlib_snapshot,
        "mirror": mirror_before,
        "resources": resources_before,
    })

    # Identity validation happens before candidate Source compilation.
    role_identity = {}
    if formal:
        for role, cli_path, cli_record, source_root in (
                ("reference", reference_cli, reference_cli_record,
                 reference_source_root),
                ("candidate", candidate_cli, candidate_cli_record,
                 candidate_source_root)):
            try:
                record = load_strict_json(cli_record,
                                          f"{role} CLI build record")
                role_identity[role] = validate_cli_identity(
                    record, cli_path, source_snapshots[role])
                role_identity[role]["ok"] = True
            except RunnerError as error:
                role_identity[role] = {"ok": False, "error": str(error)}
                _record_failure(report, f"{role}:CLI identity", str(error))
        # Candidate component identity is checked against the same Source and
        # stdlib snapshots.  A missing component leaves forged pending rather
        # than pretending a historical ed1 component is current Source.
        candidate_component_identity = {"ok": False,
                                        "error": "component not supplied"}
        if component is not None and component_record is not None:
            try:
                record = load_strict_json(component_record,
                                          "candidate component build record")
                candidate_component_identity = validate_component_identity(
                    record, component,
                    source_snapshots["candidate"], stdlib_snapshot,
                    cffi / "forged-project/lang.toml")
                candidate_component_identity["ok"] = True
            except RunnerError as error:
                candidate_component_identity = {"ok": False,
                                                "error": str(error)}
                _record_failure(report, "candidate:component identity",
                                str(error))
        elif component is not None:
            candidate_component_identity = {
                "ok": False,
                "error": "component build record not supplied"}
            _record_failure(report, "candidate:component identity",
                            candidate_component_identity["error"])
        report["identity"] = {
            "reference": role_identity.get("reference", {}),
            "candidate": role_identity.get("candidate", {}),
            "candidate_component": candidate_component_identity,
            "reference_native_helper": reference_native_helper_record,
            "source_snapshots": source_snapshots,
            "stdlib_snapshot": stdlib_snapshot,
        }
    else:
        report["identity"] = {
            "legacy": {"ok": True,
                       "note": "legacy mode does not claim current-Source "
                               "candidate identity"},
            "source_snapshots": source_snapshots,
        }

    # Shared C fixture compile + archive.  This is one C fixture build, shared
    # by both roles if present; it is not a candidate compiler substitution.
    fixture_ok = False
    fixture_tools = exec_aliases([clang])
    compile_record = None
    try:
        compile_record = run_command(
            [clang, "--target=x86_64-unknown-linux-gnu", "-O2", "-fPIC",
             "-c", fixture_source, "-o", fixture_obj],
            run_dir, FIXTURE_SECONDS, run_dir, log, "fixture:compile", env,
            strace, audit_allowed=fixture_tools,
            trace_scope=FIXTURE_TRACE_SCOPE,
            meta={"role": "shared", "category": "fixture",
                  "case": "fixture:compile"})
        require_ok(compile_record)
        fixture_ok = fixture_obj.is_file()
        if not fixture_ok:
            raise RunnerError("fixture compile produced no object")
    except RunnerError as error:
        compile_record = _known_record(log, compile_record)
        _row_result(report, "fixture:compile", "shared", "fixture",
                    "fixture:compile", compile_record, False,
                    error=str(error))
    else:
        _row_result(report, "fixture:compile", "shared", "fixture",
                    "fixture:compile", compile_record, True)

    archive_record = None
    if not fixture_ok:
        _not_run(report, "fixture:archive", "shared", "fixture",
                 "fixture:archive", "fixture compile failed")
    else:
        try:
            archive_record = run_command(
                [ar, "rcD", fixture_lib, fixture_obj],
                run_dir, FIXTURE_SECONDS, run_dir, log, "fixture:archive",
                env, strace, audit_allowed=exec_aliases([ar]),
                trace_scope=FIXTURE_TRACE_SCOPE,
                meta={"role": "shared", "category": "fixture",
                      "case": "fixture:archive"})
            require_ok(archive_record)
            if not fixture_lib.is_file():
                raise RunnerError("fixture archive produced no library")
            mirror["fixture_link"].mkdir(parents=True, exist_ok=True)
            for artifact in (fixture_obj, fixture_lib):
                (mirror["fixture_link"] / artifact.name).write_bytes(
                    artifact.read_bytes())
            # Freeze the shared originals and the actual private copies right
            # after the copy, not from the final artifact snapshot.
            initial_state = fixture_artifact_record(
                fixture_obj, fixture_lib, fixture_link)
            if not initial_state["ok"]:
                raise RunnerError(
                    "fixture shared/private copy is missing or differs: "
                    + json.dumps(initial_state, sort_keys=True))
            fixture_initial = {
                "captured_at": utc_now(),
                "state": initial_state,
                "state_sha256": canonical_sha(initial_state),
            }
            write_json(run_dir / "fixture-artifacts.json", {
                "initial": fixture_initial,
                "checks": [],
            })
            fixture_ok = True
        except (RunnerError, OSError) as error:
            archive_record = _known_record(log, archive_record)
            _row_result(report, "fixture:archive", "shared", "fixture",
                        "fixture:archive", archive_record, False,
                        error=str(error))
            fixture_ok = False
            stability_ok = False
        else:
            _row_result(report, "fixture:archive", "shared", "fixture",
                        "fixture:archive", archive_record, True)

    if formal:
        def check_fixture_artifacts(label: str) -> None:
            nonlocal stability_ok
            if fixture_initial is None:
                return
            try:
                current = ensure_fixture_artifacts(
                    label, fixture_initial["state"], fixture_obj,
                    fixture_lib, fixture_link)
            except RunnerError as error:
                stability_ok = False
                fixture_checks.append({"label": label, "ok": False,
                                       "error": str(error)})
                _record_failure(report, f"{label}: fixture artifacts",
                                str(error))
            else:
                fixture_checks.append({
                    "label": label, "ok": True,
                    "state_sha256": canonical_sha(current),
                })

        def check_reference_helper(label: str) -> None:
            nonlocal stability_ok
            if reference_native_helper_record is None:
                return
            try:
                ensure_frozen_file(label, reference_native_helper,
                                   reference_native_helper_record)
            except RunnerError as error:
                stability_ok = False
                _record_failure(
                    report, f"{label}: reference native helper", str(error))

        for role, cli_path, forged_kind, forged_path, comp, src_root in (
                ("reference", reference_cli, "cpp", reference_forged, None,
                 reference_source_root),
                ("candidate", candidate_cli, "wz", None, component,
                 candidate_source_root)):
            identity = role_identity.get(role, {"ok": False,
                                                "error": "identity missing"})
            identity_ok = bool(identity.get("ok") and fixture_ok)
            identity_error = identity.get("error") or (
                "shared fixture build failed" if not fixture_ok else None)
            if role == "candidate":
                forged_identity_ok = bool(
                    candidate_component_identity.get("ok"))
                forged_identity_error = candidate_component_identity.get(
                    "error")
                if component is None:
                    # No component supplied: keep the forged row pending, but
                    # do not turn this into a green full-run claim.
                    forged_identity_ok = True
                    forged_identity_error = None
            else:
                forged_identity_ok = True
                forged_identity_error = None
            check_fixture_artifacts(f"{role} fixture before use")
            if role == "reference":
                check_reference_helper("reference native helper before use")
            run_role(
                report, log, run_dir, role, cli_path, root, forged_kind,
                forged_path, comp, stdlib, runtimes, clang, ar, readelf, nm,
                env, toolchain_root, strace, identity_ok, identity_error,
                forged_identity_ok, forged_identity_error,
                native_helper=(reference_native_helper
                               if role == "reference" else None))
            check_fixture_artifacts(f"{role} fixture after use")
            if role == "reference":
                check_reference_helper("reference native helper after use")
        expected_rows = FORMAL_ROWS
        expected_commands = FORMAL_COMMANDS
    else:
        def check_legacy_fixture(label: str) -> None:
            nonlocal stability_ok
            if fixture_initial is None:
                return
            try:
                current = ensure_fixture_artifacts(
                    label, fixture_initial["state"], fixture_obj,
                    fixture_lib, fixture_link)
            except RunnerError as error:
                stability_ok = False
                fixture_checks.append({"label": label, "ok": False,
                                       "error": str(error)})
                _record_failure(report, f"{label}: fixture artifacts",
                                str(error))
            else:
                fixture_checks.append({
                    "label": label, "ok": True,
                    "state_sha256": canonical_sha(current),
                })

        check_legacy_fixture("legacy fixture before use")
        run_role(
            report, log, run_dir, "legacy", cli, root, "cpp", forged, None,
            None, runtimes, clang, ar, readelf, nm, env, toolchain_root,
            strace, fixture_ok,
            None if fixture_ok else "shared fixture build failed")
        check_legacy_fixture("legacy fixture after use")
        # Preserve the original extra git command, outside the 43 report rows.
        git = shutil.which("git")
        if git is not None:
            try:
                git_record = run_command(
                    [git, "-C", root, "rev-parse", "HEAD"],
                    run_dir, GIT_HEAD_SECONDS, run_dir, log, "git:HEAD", env,
                    strace, audit_allowed=exec_aliases([git]),
                    trace_scope=GIT_TRACE_SCOPE,
                    meta={"role": "legacy", "category": "git-head",
                          "case": "HEAD"}, in_report_rows=False)
                report["git_head"] = git_record.get("stdout", "").strip()
            except RunnerError as error:
                report["git_head"] = None
                _record_failure(report, "git:HEAD", str(error))
        expected_rows = LEGACY_ROWS
        expected_commands = LEGACY_COMMANDS if git is not None else LEGACY_ROWS

    # Any row that is not_run means this invocation is not a complete claim.
    # It is reported as incomplete rather than as a green pass; concrete row
    # failures remain in report["failed"].
    report["incomplete_reason"] = (
        None if not report["not_run"] else
        f"{len(report['not_run'])} expected commands were not run")

    # Post-run input/resource stability.  Command records were already saved.
    try:
        mirror_after = snapshot_tree(mirror["root"])
        ensure_maps_equal("private mirror", mirror_before["files"],
                          mirror_after["files"])
        resources_after = {
            "clang": file_record(clang),
            "ar": file_record(ar),
            "hosted_runtime": file_record(hosted_runtime),
            "native_runtime": file_record(native_runtime),
            "readelf": file_record(Path(str(readelf))),
            "nm": file_record(Path(nm)),
            "llvm_tools": {
                name: file_record(path)
                for name, path in toolchain_tools(toolchain_root).items()
            },
            "python": file_record(Path(sys.executable).resolve()),
            "corpus": file_record(SELFHOST / "corpus.py"),
            "runner": file_record(Path(__file__).resolve()),
            "fixture_source": file_record(fixture_source),
            "bindings": {
                name: file_record(cffi / f"bindings/{name}.json")
                for name in ("fixture", "libc")
            },
            "stdlib": snapshot_tree(stdlib, ".wz") if stdlib else None,
            "cli": {
                "legacy": file_record(cli) if cli else None,
                "reference": (file_record(reference_cli)
                              if reference_cli else None),
                "candidate": (file_record(candidate_cli)
                              if candidate_cli else None),
            },
            "build_records": {
                "reference_cli": file_record(reference_cli_record),
                "candidate_cli": file_record(candidate_cli_record),
                "candidate_component": file_record(component_record),
            },
            "reference_native_helper": file_record(
                reference_native_helper if formal else None),
            "forged": {
                "reference_cpp": file_record(reference_forged or forged),
                "candidate_wz": file_record(component),
            },
        }
        ensure_equal("resources", resources_before, resources_after)
        for role, snapshot in source_snapshots.items():
            after = snapshot_tree(
                reference_source_root if role == "reference"
                else candidate_source_root if role == "candidate"
                else root / "selfhost/src", ".wz")
            ensure_maps_equal(f"{role} Source", snapshot["files"],
                              after["files"])
        if fixture_initial is not None:
            try:
                current = ensure_fixture_artifacts(
                    "final fixture artifacts", fixture_initial["state"],
                    fixture_obj, fixture_lib, fixture_link)
            except RunnerError as error:
                stability_ok = False
                fixture_checks.append({"label": "final fixture artifacts",
                                       "ok": False,
                                       "error": str(error)})
                _record_failure(report, "final fixture artifacts",
                                str(error))
            else:
                fixture_checks.append({
                    "label": "final fixture artifacts", "ok": True,
                    "state_sha256": canonical_sha(current),
                })
        if formal and reference_native_helper_record is not None:
            try:
                ensure_frozen_file("reference native helper final",
                                   reference_native_helper,
                                   reference_native_helper_record)
            except RunnerError as error:
                stability_ok = False
                _record_failure(report, "reference native helper final",
                                str(error))
        report["inputs_stable"] = stability_ok
    except RunnerError as error:
        report["inputs_stable"] = False
        _record_failure(report, "post-run input stability", str(error))

    if fixture_initial is not None:
        report["fixture_artifacts"] = {
            "initial": fixture_initial,
            "final": fixture_artifact_record(
                fixture_obj, fixture_lib, fixture_link),
            "checks": fixture_checks,
        }
        write_json_atomic(run_dir / "fixture-artifacts.json",
                          report["fixture_artifacts"])

    rows = report["runs"]
    executed = [entry for entry in rows if entry.get("result") is not None]
    report["counts"] = {
        "expected_rows": expected_rows,
        "actual_rows": len(rows),
        "executed_rows": len(executed),
        "external_commands": len(log),
        "expected_external_commands": expected_commands,
        "report_rows_match": len(rows) == expected_rows,
        "external_commands_match": (
            len(log) + len(report["not_run"]) == expected_commands),
        "not_run": len(report["not_run"]),
    }
    if len(rows) != expected_rows:
        _record_failure(report, "row count",
                        f"{len(rows)} != {expected_rows}")
    if len(log) + len(report["not_run"]) != expected_commands:
        _record_failure(
            report, "external command count",
            f"{len(log)} executed + {len(report['not_run'])} not_run "
            f"!= {expected_commands}")
    report["complete"] = not report["not_run"]
    report["status"] = ("failed" if report["failed"] else
                        "passed" if report["complete"] else "incomplete")
    report["artifact_hashes"] = artifact_hashes(run_dir)
    report["commands_sha256"] = (
        digest(run_dir / "commands.jsonl")
        if (run_dir / "commands.jsonl").is_file() else None)
    write_json_atomic(run_dir / "report.json", report)
    if args.report is not None:
        write_json_atomic(args.report, report)
    if not formal:
        write_json_atomic(out / "latest-run.json", {
            "run_dir": str(run_dir),
            "report": str(run_dir / "report.json"),
            "status": report["status"],
            "complete": report["complete"],
        })
    print(json.dumps({"status": report["status"],
                      "complete": report["complete"],
                      "failed": report["failed"],
                      "counts": report["counts"],
                      "report": str(run_dir / "report.json")},
                     ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return _run(args)
    except RunnerError as error:
        print(json.dumps({"status": "failed", "error": str(error)},
                         ensure_ascii=False))
        return 1
    except Exception as error:  # noqa: BLE001 - evidence runner
        print(json.dumps({
            "status": "failed",
            "error": f"{type(error).__name__}: {error}",
            "traceback": traceback.format_exc(),
        }, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
