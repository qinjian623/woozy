#!/usr/bin/env python3
"""Prepare and measure a minimal native Source-vs-C++ performance comparison.

The script only drives existing compiler/toolchain binaries with subprocesses.
It does not evaluate the source language, generate expected values from the
compiler, or change compiler/language behavior.  The fixed fixtures contain a
full-width self-check, and this script maintains an independent Python oracle
with explicit masking at every arithmetic step.

Two stages:
  --prepare-only   Validate fixtures, build/run both implementations at O0/O2,
                   export and verify LLVM IR before/after optimization, and
                   require a real loop in the optimized IR.
  default          Reuse the prepared binaries and fixed inputs, then record
                   check/build/program wall-clock samples.  It refuses to run
                   without a passed prepare report and matching tool/fixture
                   SHA-256 values.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import traceback
from datetime import datetime, timezone

TASK = "compiler-pipeline-performance-runner"
CONTRACT = "compiler-dev/1"
OPERATION_ID = "language-native-performance-runner-20261105"
PREPARE_SCHEMA = "compiler-dev/native-performance-preparation/1"
MEASURE_SCHEMA = "compiler-dev/native-performance-measurement/1"
FIXTURE_SCHEMA = "compiler-dev/native-performance-fixture/1"
CASE_IDS = ("u64-recurrence", "u32-branch")
MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1
DEFAULT_FIXTURES = Path(__file__).resolve().parents[2] / "tests/performance/fixtures"
MARKER_RE = re.compile(
    r"^//\s*PERF-META\s+width=(\d+)\s+seed=(0x[0-9a-fA-F]+|\d+)"
    r"\s+iterations=(\d+)\s+expected=(\d+)\s*$",
    re.MULTILINE,
)
_BUILD_DEFINE_RE = re.compile(
    r'-D(LANG_NATIVE_(?:HELPER|CLANG|LLD|RUNTIME))=\\?"([^"\\]+)\\?"'
)
_COMMAND_LOG = None


class PerfError(RuntimeError):
    """A controlled failure that must be preserved in the JSON report."""


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_text(text):
    return hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def text_of(value):
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return value


def _log_command(result):
    if _COMMAND_LOG is not None:
        result["command_index"] = len(_COMMAND_LOG)
        _COMMAND_LOG.append(result)
    return result


def command(argv, *, timeout, env, cpu=None):
    """Run one command and log its complete result before any caller verdict."""
    argv = [str(x) for x in argv]
    started = time.perf_counter_ns()
    preexec = None
    if cpu is not None:
        def preexec():
            os.sched_setaffinity(0, {cpu})
    try:
        completed = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
            preexec_fn=preexec,
        )
        return _log_command({
            "argv": argv,
            "status": "exited",
            "exit": completed.returncode,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
            "elapsed_ns": time.perf_counter_ns() - started,
            "timeout_s": timeout,
            "cpu": cpu,
        })
    except subprocess.TimeoutExpired as error:
        return _log_command({
            "argv": argv,
            "status": "timeout",
            "exit": None,
            "stdout": text_of(error.stdout),
            "stderr": text_of(error.stderr),
            "elapsed_ns": time.perf_counter_ns() - started,
            "timeout_s": timeout,
            "cpu": cpu,
            "error": f"{type(error).__name__}: {error}",
        })
    except (OSError, subprocess.SubprocessError) as error:
        return _log_command({
            "argv": argv,
            "status": "launch_failed",
            "exit": None,
            "stdout": "",
            "stderr": "",
            "elapsed_ns": time.perf_counter_ns() - started,
            "timeout_s": timeout,
            "cpu": cpu,
            "error": f"{type(error).__name__}: {error}",
        })


def require_success(record, label):
    if record["status"] != "exited" or record["exit"] != 0:
        detail = record.get("error") or record.get("stderr", "")[-4000:]
        raise PerfError(
            f"{label}: expected exit 0, got status={record['status']} "
            f"exit={record['exit']}: {detail}"
        )


def require_mismatch(record, label):
    if (record["status"] != "exited" or record["exit"] != 1
            or record["stdout"] or record["stderr"]):
        detail = record.get("error") or record.get("stderr", "")[-4000:]
        raise PerfError(
            f"{label}: expected a quiet exit 1, got status={record['status']} "
            f"exit={record['exit']}: {detail}"
        )


def require_quiet_success(record, label):
    require_success(record, label)
    if record["stdout"] or record["stderr"]:
        raise PerfError(
            f"{label}: successful program produced unexpected output: "
            f"stdout={record['stdout'][-2000:]!r} stderr={record['stderr'][-2000:]!r}"
        )


def parse_json_stdout(record, label):
    try:
        value = json.loads(record["stdout"])
    except (TypeError, ValueError) as error:
        raise PerfError(
            f"{label}: stdout is not JSON ({error}); stderr={record['stderr'][-2000:]!r}"
        ) from error
    if not isinstance(value, dict):
        raise PerfError(f"{label}: JSON result is not an object")
    return value


def oracle_u64(seed, iterations):
    mask = MASK64
    multiplier = 2685821657736338717
    increment = 6364136223846793005
    state = seed & mask
    for index in range(iterations):
        value = state
        value ^= value >> 12
        value &= mask
        value ^= (value << 25) & mask
        value &= mask
        value ^= value >> 27
        value &= mask
        value = (value * multiplier) & mask
        state = (value + state * increment + index) & mask
    return state


def oracle_u32(seed, iterations):
    mask = MASK32
    state = seed & mask
    accumulator = 0
    for index in range(iterations):
        value = (state * 1664525 + 1013904223) & mask
        value ^= value >> 13
        value &= mask
        value = (value * 2654435761) & mask
        value ^= (value << 7) & mask
        value &= mask
        state = value
        if (state & 65535) < 32768:
            accumulator = (accumulator + state) & mask
        else:
            accumulator = (accumulator ^ state) & mask
        if (state & 7) == 0:
            accumulator = (accumulator + index) & mask
    return (accumulator ^ state) & mask


def compute_expected(kind, seed, iterations):
    if iterations < 0:
        raise PerfError("iterations must be non-negative")
    if kind == "u64-xorshift-multiply-v1":
        return oracle_u64(seed, iterations)
    if kind == "u32-branch-mix-v1":
        return oracle_u32(seed, iterations)
    raise PerfError(f"unknown oracle: {kind}")


def require_full_width(actual, expected, width):
    if width not in (32, 64):
        raise PerfError(f"unsupported width: {width}")
    limit = 1 << width
    if not 0 <= expected < limit:
        raise PerfError(f"expected value {expected} is outside u{width}")
    if not 0 <= actual < limit:
        raise PerfError(f"actual value {actual} is outside u{width}")
    if actual != expected:
        raise PerfError(
            f"full-width mismatch for u{width}: actual={actual} expected={expected}"
        )


def parse_marker(text, label):
    match = MARKER_RE.search(text)
    if match is None:
        raise PerfError(f"{label}: missing // PERF-META line")
    return {
        "width": int(match.group(1)),
        "seed": int(match.group(2), 0),
        "iterations": int(match.group(3)),
        "expected": int(match.group(4)),
    }


def load_cases(fixtures):
    fixtures = Path(fixtures).resolve()
    cases = []
    for case_id in CASE_IDS:
        directory = fixtures / case_id
        metadata_path = directory / "fixture.json"
        if not metadata_path.is_file():
            raise PerfError(f"missing fixture metadata: {metadata_path}")
        try:
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise PerfError(f"cannot read {metadata_path}: {error}") from error
        if metadata.get("schema") != FIXTURE_SCHEMA:
            raise PerfError(f"{metadata_path}: unexpected schema")
        if metadata.get("id") != case_id:
            raise PerfError(f"{metadata_path}: id does not match directory")

        width = int(metadata["width"])
        seed = int(metadata["seed"], 0)
        iterations = int(metadata["iterations"])
        expected = int(metadata["expected"])
        kind = metadata["oracle"]
        entry = metadata["entry"]
        source = directory / metadata["source"]
        reference = directory / metadata["reference"]
        manifest = directory / metadata["manifest"]
        for path in (source, reference, manifest):
            if not path.is_file():
                raise PerfError(f"missing fixture file: {path}")
        source_text = source.read_text(encoding="utf-8")
        reference_text = reference.read_text(encoding="utf-8")

        source_marker = parse_marker(source_text, str(source))
        reference_marker = parse_marker(reference_text, str(reference))
        expected_marker = {
            "width": width,
            "seed": seed,
            "iterations": iterations,
            "expected": expected,
        }
        if source_marker != expected_marker:
            raise PerfError(f"{source}: PERF-META does not match fixture.json")
        if reference_marker != expected_marker:
            raise PerfError(f"{reference}: PERF-META does not match fixture.json")

        source_expected = re.search(
            r"\blet\s+expected\s+u(32|64)\s*=\s*(\d+)\s*;", source_text
        )
        if source_expected is None:
            raise PerfError(f"{source}: missing full-width expected check")
        if int(source_expected.group(1)) != width or int(source_expected.group(2)) != expected:
            raise PerfError(f"{source}: embedded expected literal does not match fixture.json")

        reference_expected = re.search(
            r"\bconst\s+uint(32|64)_t\s+expected\s*=\s*UINT(32|64)_C\((\d+)\)\s*;",
            reference_text,
        )
        if reference_expected is None:
            raise PerfError(f"{reference}: missing full-width expected check")
        if (
            reference_expected.group(1) != reference_expected.group(2)
            or int(reference_expected.group(1)) != width
            or int(reference_expected.group(3)) != expected
        ):
            raise PerfError(f"{reference}: embedded expected literal does not match fixture.json")

        source_call = re.findall(
            rf"\b{re.escape(entry)}\(\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*(\d+)\s*\)",
            source_text,
        )
        if len(source_call) != 1:
            raise PerfError(f"{source}: expected exactly one {entry}(seed, count) call")
        if int(source_call[0][0], 0) != seed or int(source_call[0][1]) != iterations:
            raise PerfError(f"{source}: {entry} call does not match fixture.json")

        reference_call = re.findall(
            rf"\b{re.escape(entry)}\(\s*UINT(32|64)_C\(\s*"
            rf"(0x[0-9a-fA-F]+|\d+)\s*\)\s*,\s*UINT(32|64)_C\(\s*(\d+)\s*\)\s*\)",
            reference_text,
        )
        if len(reference_call) != 1:
            raise PerfError(f"{reference}: expected exactly one {entry}(seed, count) call")
        call_width_a, call_seed, call_width_b, call_iterations = reference_call[0]
        if (
            int(call_width_a) != width
            or int(call_width_b) != width
            or int(call_seed, 0) != seed
            or int(call_iterations) != iterations
        ):
            raise PerfError(f"{reference}: {entry} call does not match fixture.json")

        oracle_expected = compute_expected(kind, seed, iterations)
        if oracle_expected != expected:
            raise PerfError(
                f"{case_id}: independent Python oracle {oracle_expected} "
                f"does not match fixture expected {expected}"
            )
        require_full_width(expected, oracle_expected, width)

        cases.append({
            "id": case_id,
            "directory": str(directory),
            "manifest": str(manifest),
            "source": str(source),
            "reference": str(reference),
            "metadata_path": str(metadata_path),
            "metadata": metadata,
            "width": width,
            "seed": seed,
            "iterations": iterations,
            "expected": expected,
            "expected_hex": f"0x{expected:0{width // 4}x}",
            "expected_mod_256": expected & 0xFF,
            "entry": entry,
            "oracle": kind,
            "sha256": {
                "fixture_json": sha256_file(metadata_path),
                "manifest": sha256_file(manifest),
                "source": sha256_file(source),
                "reference": sha256_file(reference),
            },
        })
    return cases


def case_metadata(case):
    return {
        "id": case["id"],
        "width": case["width"],
        "seed": case["seed"],
        "iterations": case["iterations"],
        "expected": str(case["expected"]),
        "expected_hex": case["expected_hex"],
        "expected_mod_256": case["expected_mod_256"],
        "entry": case["entry"],
        "oracle": case["oracle"],
        "metadata": case["metadata"],
        "sha256": case["sha256"],
        "manifest": case["manifest"],
        "source": case["source"],
        "reference": case["reference"],
    }


def build_tree_info(args, env, cpu):
    build_dir = args.langc.parent.resolve()
    ninja_path = build_dir / "build.ninja"
    cache_path = build_dir / "CMakeCache.txt"
    if not ninja_path.is_file() or not cache_path.is_file():
        raise PerfError(
            f"{build_dir}: missing build.ninja/CMakeCache.txt; "
            "this script requires a CMake build tree, not an arbitrary installed langc"
        )
    ninja_text = ninja_path.read_text(encoding="utf-8")
    values = {}
    for key, value in _BUILD_DEFINE_RE.findall(ninja_text):
        values.setdefault(key, set()).add(value)
    requested = {
        "LANG_NATIVE_HELPER": args.helper,
        "LANG_NATIVE_RUNTIME": args.runtime,
        "LANG_NATIVE_CLANG": args.llvm_bin / "clang",
        "LANG_NATIVE_LLD": args.llvm_bin / "ld.lld",
    }
    actual = {}
    for key, requested_path in requested.items():
        found = values.get(key, set())
        if len(found) != 1:
            raise PerfError(
                f"{ninja_path}: expected exactly one {key} definition, found {sorted(found)}"
            )
        actual_path = Path(next(iter(found))).absolute()
        requested_path = requested_path.absolute()
        if actual_path != requested_path:
            raise PerfError(
                f"{ninja_path}: {key} is {actual_path}, requested {requested_path}"
            )
        if not actual_path.is_file():
            raise PerfError(f"{ninja_path}: {key} target is missing: {actual_path}")
        actual[key] = actual_path
    cache_text = cache_path.read_text(encoding="utf-8")
    build_type_match = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", cache_text, re.MULTILINE)
    if build_type_match is None or not build_type_match.group(1).strip():
        raise PerfError(f"{cache_path}: CMAKE_BUILD_TYPE is missing or empty")
    build_type = build_type_match.group(1).strip()
    if build_type != "Release":
        raise PerfError(
            f"{cache_path}: CMAKE_BUILD_TYPE is {build_type!r}; this correction requires Release"
        )
    actual_tools = {}
    for name, key, probe in (
        ("helper", "LANG_NATIVE_HELPER", "help"),
        ("runtime", "LANG_NATIVE_RUNTIME", None),
        ("clang", "LANG_NATIVE_CLANG", "version"),
        ("ld.lld", "LANG_NATIVE_LLD", "version"),
    ):
        path = actual[key]
        row = {"path": str(path), "sha256": sha256_file(path)}
        if probe == "version":
            row["version"] = command([path, "--version"], timeout=30, env=env, cpu=cpu)
            require_success(row["version"], f"actual {name} --version")
        elif probe == "help":
            row["help"] = command([path, "--help"], timeout=30, env=env, cpu=cpu)
        actual_tools[name] = row
    return {
        "build_dir": str(build_dir),
        "build_ninja": {"path": str(ninja_path), "sha256": sha256_file(ninja_path)},
        "cmake_cache": {"path": str(cache_path), "sha256": sha256_file(cache_path)},
        "cmake_build_type": build_type,
        "defines": {key: str(actual[key]) for key in requested},
        "actual_tools": actual_tools,
    }


def copy_fixture_sources(case, out):
    destination = out / "sources" / case["id"]
    destination.mkdir(parents=True, exist_ok=False)
    directory = Path(case["directory"])
    copied = []
    for key in ("metadata_path", "manifest", "source", "reference"):
        source = Path(case[key])
        target = destination / source.relative_to(directory)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        copied.append(str(target))
    return {
        "id": case["id"],
        "directory": str(destination),
        "files": copied,
        "sha256": {Path(path).name: sha256_file(path) for path in copied},
    }


def elf_info(path, *, executable):
    data = Path(path).read_bytes()[:64]
    if len(data) < 20 or data[:4] != b"\x7fELF" or data[4] != 2:
        raise PerfError(f"{path}: not an ELF64 file")
    elf_type = int.from_bytes(data[16:18], "little")
    machine = int.from_bytes(data[18:20], "little")
    if machine != 62:
        raise PerfError(f"{path}: ELF machine {machine} is not x86_64 (62)")
    if executable and elf_type not in (2, 3):
        raise PerfError(f"{path}: ELF type {elf_type} is not executable/PIE")
    if not executable and elf_type != 1:
        raise PerfError(f"{path}: ELF type {elf_type} is not relocatable")
    return {
        "sha256": sha256_file(path),
        "class": 64,
        "type": elf_type,
        "machine": machine,
        "path": str(path),
    }


def checked_elf(path, *, executable, label):
    try:
        return elf_info(path, executable=executable)
    except OSError as error:
        raise PerfError(f"{label}: cannot read {path}: {error}") from error


def make_env(args):
    env = os.environ.copy()
    if args.ld_library_path is not None:
        prefix = str(args.ld_library_path)
        existing = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = prefix + (":" + existing if existing else "")
    return env


def host_info():
    cpu_model = "unknown"
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("model name"):
                cpu_model = line.split(":", 1)[1].strip()
                break
    except OSError:
        cpu_model = platform.processor() or "unknown"
    try:
        loadavg = [round(value, 4) for value in os.getloadavg()]
    except (OSError, AttributeError):
        loadavg = None
    allowed = None
    if hasattr(os, "sched_getaffinity"):
        try:
            allowed = sorted(os.sched_getaffinity(0))
        except OSError:
            allowed = None
    return {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "system": platform.system(),
        "release": platform.release(),
        "cpu_model": cpu_model,
        "nproc": os.cpu_count(),
        "allowed_cpus": allowed,
        "loadavg": loadavg,
    }


def repo_info(path, env, cpu):
    path = Path(path).resolve()
    head = command(["git", "-C", str(path), "rev-parse", "HEAD"], timeout=30, env=env, cpu=cpu)
    status = command(["git", "-C", str(path), "status", "--porcelain"], timeout=30, env=env, cpu=cpu)
    lines = status["stdout"].splitlines()
    return {
        "path": str(path),
        "head": head["stdout"].strip() if head["status"] == "exited" and head["exit"] == 0 else None,
        "head_command": head,
        "dirty": bool(lines),
        "status_count": len(lines),
        "status_sha256": sha256_text(status["stdout"]),
        "status_sample": lines[:50],
        "status_command": status,
    }


def affinity_info(cpu, env):
    supported = hasattr(os, "sched_setaffinity")
    allowed = None
    if hasattr(os, "sched_getaffinity"):
        try:
            allowed = sorted(os.sched_getaffinity(0))
        except OSError:
            allowed = None
    info = {
        "requested_cpu": cpu,
        "supported": supported,
        "parent_allowed_cpus": allowed,
        "mode": "not_attempted" if cpu is None else "child-only",
        "applied_to": None if cpu is None else "all subprocesses",
        "probe": None,
    }
    if cpu is None:
        return info
    if not supported:
        raise PerfError("--cpu requested but os.sched_setaffinity is unavailable")
    if allowed is not None and cpu not in allowed:
        raise PerfError(f"--cpu {cpu} is not in the parent affinity mask {allowed}")
    probe = command(
        [sys.executable, "-c", "import os, json; print(json.dumps(sorted(os.sched_getaffinity(0))))"],
        timeout=30,
        env=env,
        cpu=cpu,
    )
    info["probe"] = probe
    require_success(probe, "affinity probe")
    try:
        child_allowed = json.loads(probe["stdout"])
    except ValueError as error:
        raise PerfError(f"affinity probe returned invalid JSON: {probe['stdout']!r}") from error
    if child_allowed != [cpu]:
        raise PerfError(f"affinity probe expected [{cpu}], got {child_allowed}")
    info["mode"] = "passed"
    return info


def tool_record(path, env, cpu, version_args, *, require_version=False):
    path = Path(path).absolute()
    record = {
        "path": str(path),
        "sha256": sha256_file(path),
    }
    if version_args is not None:
        version = command([path, *version_args], timeout=30, env=env, cpu=cpu)
        record["version"] = version
        if require_version:
            require_success(version, f"{path} {' '.join(version_args)}")
    return record


def base_report(mode, args, env):
    worktree = Path(__file__).resolve().parents[2]
    script = Path(__file__).resolve()
    build_tree = build_tree_info(args, env, args.cpu)
    tools = {
        "langc": tool_record(args.langc, env, args.cpu, ["--version"], require_version=True),
        "helper": build_tree["actual_tools"]["helper"],
        "runtime": build_tree["actual_tools"]["runtime"],
        "clang": build_tree["actual_tools"]["clang"],
        "ld.lld": build_tree["actual_tools"]["ld.lld"],
        "clangxx": tool_record(args.llvm_bin / "clang++", env, args.cpu, ["--version"], require_version=True),
        "opt": tool_record(args.llvm_bin / "opt", env, args.cpu, ["--version"], require_version=True),
        "python": {
            "executable": sys.executable,
            "version": platform.python_version(),
            "implementation": platform.python_implementation(),
            "sha256": sha256_file(sys.executable),
        },
    }
    repositories = {"worktree": repo_info(worktree, env, args.cpu)}
    if args.compiler_root is not None:
        repositories["compiler_root"] = repo_info(args.compiler_root, env, args.cpu)
    return {
        "schema": PREPARE_SCHEMA if mode == "prepare" else MEASURE_SCHEMA,
        "task": TASK,
        "contract": CONTRACT,
        "operation_id": OPERATION_ID,
        "mode": mode,
        "status": "running",
        "started_at": utc_now(),
        "script": {"path": str(script), "sha256": sha256_file(script)},
        "arguments": sys.argv[1:],
        "host": host_info(),
        "affinity": affinity_info(args.cpu, env),
        "environment": {"LD_LIBRARY_PATH": env.get("LD_LIBRARY_PATH", "")},
        "repositories": repositories,
        "tools": tools,
        "build_tree": build_tree,
        "target": {
            "triple": "x86_64-unknown-linux-gnu",
            "generic_cpu": True,
            "march_native": False,
            "fast_math": False,
            "lto": False,
        },
    }


def limitations():
    return [
        "Wall-clock timings include process creation, dynamic loading and file I/O; they are not in-process kernel timings.",
        "The language programs have no output or public FFI value; each program compares its full-width result with the embedded expected value and exits 0 only on exact equality. The harness observes only that exit status.",
        "The independent Python oracle masks after every intermediate operation and is recomputed from the fixed seed/width/iteration inputs; it is not derived from compiler output.",
        "langc build work and clang++ build work are not identical: langc includes its own frontend/check and native link, while clang++ compiles only the C++ reference and links it.",
        "No CPU governor change, cache drop/flush, process killing, perf counters, LTO, fast-math or -march=native is used.",
        "Cold file-cache behavior is not controlled; process warmups do not guarantee cold-cache measurements.",
        "The public CLI has no in-process incremental check entry; check timing is one fresh process per sample.",
        "Only the fixed x86_64 generic target and the two scalar fixtures are covered; results do not generalize to other kernels, programs, targets or CPU models.",
    ]


def not_run_items(*, formal):
    items = [
        {"item": "cold_file_cache", "reason": "no controlled cache-drop/flush entry; not measured"},
        {"item": "in_process_incremental_check", "reason": "the public CLI has no in-process incremental check entry; not measured"},
        {"item": "perf_counters", "reason": "no hardware/software performance counters were collected"},
        {"item": "governor_or_cpu_isolation", "reason": "the harness does not change the system governor or isolate CPUs"},
    ]
    if formal:
        items.append({"item": "other_kernels_targets_platforms", "reason": "only the two fixed x86_64 fixtures were measured"})
    else:
        items.append({"item": "formal_timing", "reason": "prepare validates correctness and artifacts only; run the default measurement stage on an idle host"})
    return items


def prepare_out(path):
    path = Path(path).resolve()
    if path.exists():
        raise PerfError(f"--out already exists, refusing to overwrite: {path}")
    path.mkdir(parents=True, exist_ok=False)
    return path


def write_report(out, name, report):
    write_json(out / name, report)


def run_negative_controls(args, env, out, cases, report):
    control_root = out / "negative-control"
    control_root.mkdir(parents=True, exist_ok=False)
    report["negative_controls"] = []
    for case in cases:
        short_expected = compute_expected(case["oracle"], case["seed"], 2)
        flipped = short_expected ^ (1 << (case["width"] - 1))
        if (short_expected & 0xFF) != (flipped & 0xFF):
            raise PerfError(f"{case['id']}: negative control changed the low 8 bits")
        directory = control_root / case["id"]
        (directory / "src").mkdir(parents=True, exist_ok=False)
        source_text = Path(case["source"]).read_text(encoding="utf-8")
        reference_text = Path(case["reference"]).read_text(encoding="utf-8")
        source_text = source_text.replace(str(case["iterations"]), "2").replace(
            str(case["expected"]), str(flipped)
        )
        reference_text = reference_text.replace(str(case["iterations"]), "2").replace(
            str(case["expected"]), str(flipped)
        )
        source_path = directory / "src" / "main.lang"
        reference_path = directory / "reference.cpp"
        source_path.write_text(source_text, encoding="utf-8")
        reference_path.write_text(reference_text, encoding="utf-8")
        shutil.copyfile(case["manifest"], directory / "lang.toml")
        control = {
            "id": case["id"],
            "width": case["width"],
            "seed": case["seed"],
            "iterations": 2,
            "correct_expected": str(short_expected),
            "flipped_expected": str(flipped),
            "correct_low8": short_expected & 0xFF,
            "flipped_low8": flipped & 0xFF,
            "high_bit_flipped": str(1 << (case["width"] - 1)),
            "source_sha256": sha256_file(source_path),
            "reference_sha256": sha256_file(reference_path),
        }
        (directory / "control.json").write_text(
            json.dumps(control, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
        )
        for implementation in ("langc", "cpp"):
            for opt in (0, 2):
                build_dir = directory / implementation / f"O{opt}"
                build_dir.mkdir(parents=True, exist_ok=False)
                if implementation == "langc":
                    program = build_dir / "program"
                    build = command(
                        [
                            args.langc,
                            "build",
                            "--project",
                            str(directory / "lang.toml"),
                            "--backend",
                            "native",
                            "--opt",
                            str(opt),
                            "--out-dir",
                            str(build_dir),
                            "--format",
                            "json",
                        ],
                        timeout=args.timeout,
                        env=env,
                        cpu=args.cpu,
                    )
                else:
                    program = build_dir / "reference"
                    build = command(
                        [
                            args.llvm_bin / "clang++",
                            "--target=x86_64-unknown-linux-gnu",
                            "-std=c++20",
                            f"-O{opt}",
                            "-fno-fast-math",
                            "-fno-lto",
                            str(reference_path),
                            "-o",
                            str(program),
                        ],
                        timeout=args.timeout,
                        env=env,
                        cpu=args.cpu,
                    )
                build_row = {
                    "id": case["id"],
                    "implementation": implementation,
                    "opt": opt,
                    "phase": "build",
                    "command_index": build["command_index"],
                    "success": False,
                }
                report["negative_controls"].append(build_row)
                require_success(build, f"negative control {case['id']} {implementation} O{opt} build")
                if implementation == "langc":
                    build_json = parse_json_stdout(
                        build, f"negative control {case['id']} langc O{opt} build"
                    )
                    build_row["json"] = build_json
                    if build_json.get("status") != "ok":
                        raise PerfError(
                            f"negative control {case['id']} langc O{opt} build status is not ok"
                        )
                info = checked_elf(
                    program, executable=True,
                    label=f"negative control {case['id']} {implementation} O{opt}",
                )
                build_row["success"] = True
                build_row["program_sha256"] = info["sha256"]
                run = command([program], timeout=args.run_timeout, env=env, cpu=args.cpu)
                run_row = {
                    "id": case["id"],
                    "implementation": implementation,
                    "opt": opt,
                    "phase": "execute",
                    "command_index": run["command_index"],
                    "success": False,
                    "correct_expected": str(short_expected),
                    "flipped_expected": str(flipped),
                    "low8_preserved": True,
                }
                report["negative_controls"].append(run_row)
                require_mismatch(run, f"negative control {case['id']} {implementation} O{opt} run")
                run_row["success"] = True
                run_row["exit"] = run["exit"]
    builds = sum(1 for row in report["negative_controls"] if row["phase"] == "build")
    runs = sum(1 for row in report["negative_controls"] if row["phase"] == "execute")
    if builds != 8 or runs != 8:
        raise PerfError(f"negative control expected 8 builds/8 runs, got {builds}/{runs}")
    return {
        "cases": len(cases),
        "builds": builds,
        "executions": runs,
        "all_nonzero": all(
            row.get("success") for row in report["negative_controls"] if row["phase"] == "execute"
        ),
        "low8_preserved": True,
        "note": "short 2-iteration derived programs; not performance samples",
    }


def prepare(args):
    global _COMMAND_LOG
    env = make_env(args)
    out = prepare_out(args.out)
    report = {
        "schema": PREPARE_SCHEMA,
        "task": TASK,
        "contract": CONTRACT,
        "operation_id": OPERATION_ID,
        "mode": "prepare",
        "status": "running",
        "commands": [],
    }
    _COMMAND_LOG = report["commands"]
    try:
        report.update(base_report("prepare", args, env))
        cases = load_cases(args.fixtures)
        report["fixtures"] = [case_metadata(case) for case in cases]
        report["copied_sources"] = [copy_fixture_sources(case, out) for case in cases]
        report["checks"] = []
        report["executions"] = []
        report["llvm"] = []
        report["cases"] = []

        for case in cases:
            case_row = {
                "id": case["id"],
                "expected": str(case["expected"]),
                "width": case["width"],
                "seed": case["seed"],
                "iterations": case["iterations"],
                "sha256": case["sha256"],
            }
            report["cases"].append(case_row)
            check = command(
                [args.langc, "check", "--project", case["manifest"], "--backend", "native", "--format", "json"],
                timeout=args.timeout,
                env=env,
                cpu=args.cpu,
            )
            check_row = {
                "id": case["id"],
                "command_index": check["command_index"],
                "json": None,
                "success": False,
            }
            report["checks"].append(check_row)
            case_row["check_index"] = len(report["checks"]) - 1
            require_success(check, f"{case['id']} langc check")
            check_json = parse_json_stdout(check, f"{case['id']} langc check")
            check_row["json"] = check_json
            if check_json.get("status") != "ok":
                raise PerfError(f"{case['id']} langc check status is not ok: {check_json}")
            check_row["success"] = True

            for opt in (0, 2):
                langc_dir = out / "build" / "langc" / case["id"] / f"O{opt}"
                langc_row = {
                    "program_rel": str((langc_dir / "program").relative_to(out)),
                    "program_object_rel": str((langc_dir / "program.o").relative_to(out)),
                    "build_json": None,
                    "build_command_index": None,
                    "execute_command_index": None,
                    "program_sha256": None,
                    "program_object_sha256": None,
                    "success": False,
                }
                case_row[f"langc_O{opt}"] = langc_row
                langc_dir.mkdir(parents=True, exist_ok=False)
                build = command(
                    [
                        args.langc,
                        "build",
                        "--project",
                        case["manifest"],
                        "--backend",
                        "native",
                        "--opt",
                        str(opt),
                        "--out-dir",
                        str(langc_dir),
                        "--format",
                        "json",
                    ],
                    timeout=args.timeout,
                    env=env,
                    cpu=args.cpu,
                )
                langc_row["build_command_index"] = build["command_index"]
                require_success(build, f"{case['id']} langc O{opt} build")
                build_json = parse_json_stdout(build, f"{case['id']} langc O{opt} build")
                langc_row["build_json"] = build_json
                if build_json.get("status") != "ok":
                    raise PerfError(f"{case['id']} langc O{opt} build status is not ok: {build_json}")
                program = langc_dir / "program"
                program_object = langc_dir / "program.o"
                program_info = checked_elf(program, executable=True, label=f"{case['id']} langc O{opt} program")
                program_object_info = checked_elf(
                    program_object, executable=False, label=f"{case['id']} langc O{opt} program.o"
                )
                langc_row.update({
                    "program": str(program),
                    "program_sha256": program_info["sha256"],
                    "program_object_sha256": program_object_info["sha256"],
                    "elf": program_info,
                    "program_object": program_object_info,
                })
                execute = command([program], timeout=args.run_timeout, env=env, cpu=args.cpu)
                execution_row = {
                    "id": case["id"],
                    "opt": opt,
                    "implementation": "langc",
                    "program": str(program),
                    "program_sha256": program_info["sha256"],
                    "command_index": execute["command_index"],
                    "success": False,
                }
                report["executions"].append(execution_row)
                require_quiet_success(execute, f"{case['id']} langc O{opt} program")
                execution_row["success"] = True
                langc_row["execute_command_index"] = execute["command_index"]
                langc_row["success"] = True

                cpp_dir = out / "build" / "cpp" / case["id"] / f"O{opt}"
                cpp_row = {
                    "program_rel": str((cpp_dir / "reference").relative_to(out)),
                    "build_command_index": None,
                    "execute_command_index": None,
                    "program_sha256": None,
                    "success": False,
                }
                case_row[f"cpp_O{opt}"] = cpp_row
                cpp_dir.mkdir(parents=True, exist_ok=False)
                reference = cpp_dir / "reference"
                compile_cpp = command(
                    [
                        args.llvm_bin / "clang++",
                        "--target=x86_64-unknown-linux-gnu",
                        "-std=c++20",
                        f"-O{opt}",
                        "-fno-fast-math",
                        "-fno-lto",
                        case["reference"],
                        "-o",
                        str(reference),
                    ],
                    timeout=args.timeout,
                    env=env,
                    cpu=args.cpu,
                )
                cpp_row["build_command_index"] = compile_cpp["command_index"]
                require_success(compile_cpp, f"{case['id']} clang++ O{opt} build")
                reference_info = checked_elf(reference, executable=True, label=f"{case['id']} clang++ O{opt} reference")
                cpp_row.update({
                    "program": str(reference),
                    "program_sha256": reference_info["sha256"],
                    "elf": reference_info,
                })
                execute_reference = command([reference], timeout=args.run_timeout, env=env, cpu=args.cpu)
                execution_row = {
                    "id": case["id"],
                    "opt": opt,
                    "implementation": "cpp",
                    "program": str(reference),
                    "program_sha256": reference_info["sha256"],
                    "command_index": execute_reference["command_index"],
                    "success": False,
                }
                report["executions"].append(execution_row)
                require_quiet_success(execute_reference, f"{case['id']} clang++ O{opt} reference")
                execution_row["success"] = True
                cpp_row["execute_command_index"] = execute_reference["command_index"]
                cpp_row["success"] = True

                llvm_dir = out / "llvm" / case["id"] / f"O{opt}"
                helper_row = {
                    "command_index": None,
                    "json": None,
                    "object_rel": str((llvm_dir / "program.o").relative_to(out)),
                    "object_sha256": None,
                    "program_object_sha256": langc_row["program_object_sha256"],
                    "object_match": False,
                    "before": None,
                    "after": None,
                    "loop_info": None,
                    "work_counts": None,
                    "success": False,
                }
                case_row[f"helper_O{opt}"] = helper_row
                llvm_dir.mkdir(parents=True, exist_ok=False)
                object_path = llvm_dir / "program.o"
                after_path = llvm_dir / "after.ll"
                before_path = llvm_dir / "before.ll"
                native_path = llvm_dir / "program.nir"
                helper = command(
                    [
                        args.helper,
                        "--project",
                        case["manifest"],
                        "--opt",
                        str(opt),
                        "--out-object",
                        str(object_path),
                        "--out-llvm",
                        str(after_path),
                        "--out-llvm-before",
                        str(before_path),
                        "--out-native",
                        str(native_path),
                        "--format",
                        "json",
                    ],
                    timeout=args.timeout,
                    env=env,
                    cpu=args.cpu,
                )
                helper_row["command_index"] = helper["command_index"]
                require_success(helper, f"{case['id']} helper O{opt}")
                helper_json = parse_json_stdout(helper, f"{case['id']} helper O{opt}")
                helper_row["json"] = helper_json
                if helper_json.get("status") not in ("ok", "passed"):
                    raise PerfError(f"{case['id']} helper O{opt} status is not ok/passed: {helper_json}")
                if not helper_json.get("llvm_verified_before") or not helper_json.get("llvm_verified_after"):
                    raise PerfError(f"{case['id']} helper O{opt} did not verify both IR forms: {helper_json}")
                object_info = checked_elf(object_path, executable=False, label=f"{case['id']} helper O{opt} object")
                helper_row["object_sha256"] = object_info["sha256"]
                helper_row["object_match"] = (
                    program_object.read_bytes() == object_path.read_bytes()
                )
                verifies = {}
                for name, path in (("before", before_path), ("after", after_path)):
                    verify = command(
                        [args.llvm_bin / "opt", "-passes=verify", "-disable-output", str(path)],
                        timeout=args.timeout,
                        env=env,
                        cpu=args.cpu,
                    )
                    verify_row = {
                        "id": case["id"],
                        "opt": opt,
                        "file": name,
                        "path": str(path),
                        "sha256": sha256_file(path) if path.is_file() else None,
                        "verify_command_index": verify["command_index"],
                        "success": False,
                    }
                    report["llvm"].append(verify_row)
                    require_success(verify, f"{case['id']} O{opt} {name} IR verify")
                    verify_row["success"] = True
                    verifies[name] = {
                        "path": str(path),
                        "sha256": verify_row["sha256"],
                        "verify_command_index": verify["command_index"],
                    }
                loop_info = command(
                    [args.llvm_bin / "opt", "-passes=print<loops>", "-disable-output", str(after_path)],
                    timeout=args.timeout,
                    env=env,
                    cpu=args.cpu,
                )
                loop_row = {
                    "path": str(llvm_dir / "loop-info.txt"),
                    "command_index": loop_info["command_index"],
                    "loop_present": False,
                    "text": "",
                }
                helper_row["loop_info"] = loop_row
                require_success(loop_info, f"{case['id']} O{opt} loop analysis")
                loop_text = loop_info["stderr"]
                loop_file = llvm_dir / "loop-info.txt"
                loop_file.write_text(loop_text, encoding="utf-8")
                loop_present = "Loop at depth" in loop_text
                loop_row["loop_present"] = loop_present
                loop_row["text"] = loop_text
                after_text = after_path.read_text(encoding="utf-8")
                work_counts = {
                    "mul": len(re.findall(r"\bmul\b", after_text)),
                    "xor": len(re.findall(r"\bxor\b", after_text)),
                    "add": len(re.findall(r"\badd\b", after_text)),
                    "shl_lshr": len(re.findall(r"\b(?:shl|lshr)\b", after_text)),
                }
                helper_row["before"] = verifies["before"]
                helper_row["after"] = verifies["after"]
                helper_row["work_counts"] = work_counts
                if opt == 2:
                    if not loop_present:
                        raise PerfError(
                            f"{case['id']} O2: optimized IR has no loop; refusing to call it a runtime benchmark"
                        )
                    if work_counts["mul"] == 0 or (work_counts["xor"] == 0 and work_counts["add"] == 0):
                        raise PerfError(
                            f"{case['id']} O2: optimized IR lacks expected arithmetic work: {work_counts}"
                        )
                if not helper_row["object_match"]:
                    raise PerfError(
                        f"{case['id']} O{opt}: langc program.o and helper object differ; "
                        "refusing to use the verified IR for timing"
                    )
                helper_row["success"] = True

        report["negative_control_summary"] = run_negative_controls(
            args, env, out, cases, report
        )
        report["summary"] = {
            "cases": len(cases),
            "correct_executions": len(report["executions"]),
            "langc_correct_executions": sum(
                1 for execution in report["executions"] if execution["implementation"] == "langc"
            ),
            "cpp_correct_executions": sum(
                1 for execution in report["executions"] if execution["implementation"] == "cpp"
            ),
            "langc_builds": 4,
            "cpp_builds": 4,
            "llvm_ir_verified": len(report["llvm"]),
            "llvm_objects": 4,
            "object_matches": sum(
                1 for case in report["cases"]
                for opt in (0, 2)
                if case[f"helper_O{opt}"]["object_match"]
            ),
            "o2_loops_present": sum(
                1 for case in report["cases"] if case["helper_O2"]["loop_info"]["loop_present"]
            ),
            "negative_control_builds": report["negative_control_summary"]["builds"],
            "negative_control_executions": report["negative_control_summary"]["executions"],
        }
        report["not_run"] = not_run_items(formal=False)
        report["limitations"] = limitations()
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["failure"] = {
            "type": type(error).__name__,
            "message": str(error),
            "traceback": traceback.format_exc(),
        }
        report.setdefault("not_run", not_run_items(formal=False))
        report.setdefault("limitations", limitations())
    finally:
        report["finished_at"] = utc_now()
        report["loadavg_after"] = host_info()["loadavg"]
        write_report(out, "prepare.json", report)
        _COMMAND_LOG = None
        summary = {
            "status": report["status"],
            "mode": "prepare",
            "out": str(out),
            "report": str(out / "prepare.json"),
            "summary": report.get("summary"),
            "failure": report.get("failure"),
        }
        print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


def load_prepare_report(path, cases, args, env, current_build_tree=None):
    path = Path(path).resolve()
    if not path.is_file():
        raise PerfError(f"missing prepare report: {path}")
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise PerfError(f"cannot read prepare report {path}: {error}") from error
    if report.get("schema") != PREPARE_SCHEMA:
        raise PerfError(f"{path}: unexpected prepare schema")
    if report.get("status") != "passed":
        raise PerfError(f"{path}: prepare status is {report.get('status')!r}, not passed")
    if report.get("task") != TASK or report.get("contract") != CONTRACT:
        raise PerfError(f"{path}: task/contract mismatch")
    if report.get("operation_id") != OPERATION_ID:
        raise PerfError(f"{path}: operation_id mismatch")
    if report.get("script", {}).get("sha256") != sha256_file(Path(__file__).resolve()):
        raise PerfError(f"{path}: prepare used a different script version")
    summary = report.get("summary", {})
    if summary.get("correct_executions") != 8:
        raise PerfError(f"{path}: prepare did not record 8 correct executions")
    if summary.get("llvm_ir_verified") != 8:
        raise PerfError(f"{path}: prepare did not record 8 LLVM verifications")
    if summary.get("object_matches") != 4:
        raise PerfError(f"{path}: prepare did not record 4 program.o/helper object matches")
    if summary.get("o2_loops_present") != 2:
        raise PerfError(f"{path}: prepare did not record 2 optimized loops")
    controls = report.get("negative_control_summary", {})
    if (controls.get("builds") != 8 or controls.get("executions") != 8
            or not controls.get("all_nonzero") or not controls.get("low8_preserved")):
        raise PerfError(f"{path}: prepare negative full-width controls did not pass")

    expected_tools = {
        "langc": args.langc,
        "helper": args.helper,
        "runtime": args.runtime,
        "clang": args.llvm_bin / "clang",
        "ld.lld": args.llvm_bin / "ld.lld",
        "clangxx": args.llvm_bin / "clang++",
        "opt": args.llvm_bin / "opt",
    }
    for name, tool_path in expected_tools.items():
        old = report.get("tools", {}).get(name, {}).get("sha256")
        new = sha256_file(tool_path)
        if old != new:
            raise PerfError(f"{path}: tool {name} SHA-256 changed since prepare")

    if current_build_tree is None:
        current_build_tree = build_tree_info(args, env, args.cpu)
    old_tree = report.get("build_tree", {})
    if old_tree.get("cmake_build_type") != current_build_tree.get("cmake_build_type"):
        raise PerfError(f"{path}: CMAKE_BUILD_TYPE changed since prepare")
    if old_tree.get("defines") != current_build_tree.get("defines"):
        raise PerfError(f"{path}: actual LANG_NATIVE_* paths changed since prepare")
    for key in ("build_ninja", "cmake_cache"):
        if old_tree.get(key, {}).get("sha256") != current_build_tree.get(key, {}).get("sha256"):
            raise PerfError(f"{path}: {key} changed since prepare")
    for name in ("helper", "runtime", "clang", "ld.lld"):
        if old_tree.get("actual_tools", {}).get(name, {}).get("sha256") != \
                current_build_tree.get("actual_tools", {}).get(name, {}).get("sha256"):
            raise PerfError(f"{path}: actual tool {name} SHA-256 changed since prepare")

    prepared_cases = {row["id"]: row for row in report.get("cases", [])}
    for case in cases:
        row = prepared_cases.get(case["id"])
        if row is None:
            raise PerfError(f"{path}: missing prepared case {case['id']}")
        for key in ("fixture_json", "manifest", "source", "reference"):
            if row["sha256"].get(key) != case["sha256"][key]:
                raise PerfError(f"{path}: {case['id']} {key} SHA-256 changed since prepare")
        if int(row["expected"]) != case["expected"]:
            raise PerfError(f"{path}: {case['id']} expected value changed since prepare")
        if int(row["width"]) != case["width"] or int(row["seed"]) != case["seed"]:
            raise PerfError(f"{path}: {case['id']} width/seed changed since prepare")
        if int(row["iterations"]) != case["iterations"]:
            raise PerfError(f"{path}: {case['id']} iteration count changed since prepare")
        for opt in (0, 2):
            for tool in ("langc", "cpp"):
                prepared = row.get(f"{tool}_O{opt}")
                if prepared is None:
                    raise PerfError(f"{path}: missing {case['id']} {tool} O{opt}")
                binary = path.parent / prepared["program_rel"]
                if not binary.is_file():
                    raise PerfError(f"{path}: prepared binary is missing: {binary}")
                if sha256_file(binary) != prepared["program_sha256"]:
                    raise PerfError(f"{path}: prepared binary SHA-256 changed: {binary}")
            langc_prepared = row[f"langc_O{opt}"]
            helper_prepared = row[f"helper_O{opt}"]
            program_object = path.parent / langc_prepared["program_object_rel"]
            helper_object = path.parent / helper_prepared["object_rel"]
            if not program_object.is_file() or not helper_object.is_file():
                raise PerfError(f"{path}: prepared object file is missing for {case['id']} O{opt}")
            if sha256_file(program_object) != langc_prepared["program_object_sha256"]:
                raise PerfError(f"{path}: prepared program.o SHA-256 changed: {program_object}")
            if sha256_file(helper_object) != helper_prepared["object_sha256"]:
                raise PerfError(f"{path}: prepared helper object SHA-256 changed: {helper_object}")
            if not helper_prepared.get("object_match"):
                raise PerfError(f"{path}: prepare did not record a program.o/helper object match")
            if langc_prepared["program_object_sha256"] != helper_prepared["object_sha256"]:
                raise PerfError(f"{path}: program.o and helper object SHA-256 differ for {case['id']} O{opt}")
            if program_object.read_bytes() != helper_object.read_bytes():
                raise PerfError(f"{path}: program.o and helper object bytes differ for {case['id']} O{opt}")
    return report, path


def measured_stats(rows, expected_count):
    values = [row["elapsed_ns"] for row in rows]
    if len(values) != expected_count:
        raise PerfError(f"expected {expected_count} measured samples, got {len(values)}")
    return {
        "count": len(values),
        "raw_ns": values,
        "median_ns": int(statistics.median(values)),
        "min_ns": min(values),
        "max_ns": max(values),
    }


def grouped_stats(rows, expected_count, fields, expected_keys):
    groups = {}
    for row in rows:
        key = tuple(row[field] for field in fields)
        groups.setdefault(key, []).append(row)
    result = []
    for key in sorted(expected_keys, key=lambda value: tuple(str(item) for item in value)):
        group = groups.get(key, [])
        successful = [row for row in group if row.get("success") is True]
        if len(successful) != expected_count:
            raise PerfError(
                f"incomplete successful samples for {dict(zip(fields, key))}: "
                f"{len(successful)}/{expected_count}"
            )
        stats = measured_stats(successful, expected_count)
        result.append({**dict(zip(fields, key)), "success_count": len(successful), **stats})
    return result


def measure(args):
    global _COMMAND_LOG
    env = make_env(args)
    out = prepare_out(args.out)
    report = {
        "schema": MEASURE_SCHEMA,
        "task": TASK,
        "contract": CONTRACT,
        "operation_id": OPERATION_ID,
        "mode": "measure",
        "status": "running",
        "commands": [],
    }
    _COMMAND_LOG = report["commands"]
    try:
        report.update(base_report("measure", args, env))
        cases = load_cases(args.fixtures)
        prepared_report, prepared_path = load_prepare_report(
            args.prepare_report, cases, args, env, report.get("build_tree")
        )
        current_host = report["host"]
        prepared_host = prepared_report.get("host", {})
        if prepared_host.get("cpu_model") != current_host.get("cpu_model"):
            raise PerfError(
                "CPU model differs from prepare: "
                f"{prepared_host.get('cpu_model')!r} != {current_host.get('cpu_model')!r}"
            )
        report["prepare"] = {
            "path": str(prepared_path),
            "sha256": sha256_file(prepared_path),
            "host": prepared_host,
            "affinity": prepared_report.get("affinity"),
        }
        report["fixtures"] = [case_metadata(case) for case in cases]
        report["checks"] = []
        report["builds"] = []
        report["runs"] = []

        for case in cases:
            for sample in range(args.build_samples):
                check_row = {
                    "id": case["id"],
                    "sample": sample,
                    "elapsed_ns": None,
                    "command_index": None,
                    "json": None,
                    "success": False,
                }
                report["checks"].append(check_row)
                check = command(
                    [args.langc, "check", "--project", case["manifest"], "--backend", "native", "--format", "json"],
                    timeout=args.timeout,
                    env=env,
                    cpu=args.cpu,
                )
                check_row["elapsed_ns"] = check["elapsed_ns"]
                check_row["command_index"] = check["command_index"]
                require_success(check, f"{case['id']} check sample {sample}")
                check_json = parse_json_stdout(check, f"{case['id']} check sample {sample}")
                check_row["json"] = check_json
                if check_json.get("status") != "ok":
                    raise PerfError(f"{case['id']} check sample {sample}: status is not ok")
                check_row["success"] = True

        for case in cases:
            for opt in (0, 2):
                for sample in range(args.build_samples):
                    order = ("langc", "cpp") if sample % 2 == 0 else ("cpp", "langc")
                    for implementation in order:
                        if implementation == "langc":
                            directory = out / "build" / "langc" / case["id"] / f"O{opt}" / f"sample-{sample}"
                            build_row = {
                                "id": case["id"],
                                "opt": opt,
                                "implementation": "langc",
                                "sample": sample,
                                "elapsed_ns": None,
                                "command_index": None,
                                "json": None,
                                "success": False,
                            }
                            report["builds"].append(build_row)
                            directory.mkdir(parents=True, exist_ok=False)
                            build = command(
                                [
                                    args.langc,
                                    "build",
                                    "--project",
                                    case["manifest"],
                                    "--backend",
                                    "native",
                                    "--opt",
                                    str(opt),
                                    "--out-dir",
                                    str(directory),
                                    "--format",
                                    "json",
                                ],
                                timeout=args.timeout,
                                env=env,
                                cpu=args.cpu,
                            )
                            build_row["elapsed_ns"] = build["elapsed_ns"]
                            build_row["command_index"] = build["command_index"]
                            require_success(build, f"{case['id']} langc O{opt} build sample {sample}")
                            build_json = parse_json_stdout(build, f"{case['id']} langc O{opt} build sample {sample}")
                            build_row["json"] = build_json
                            if build_json.get("status") != "ok":
                                raise PerfError(f"{case['id']} langc O{opt} build sample {sample}: status is not ok")
                            program = directory / "program"
                            info = checked_elf(program, executable=True, label=f"{case['id']} langc O{opt} sample {sample}")
                            build_row["program"] = str(program)
                            build_row["program_sha256"] = info["sha256"]
                            build_row["success"] = True
                        else:
                            directory = out / "build" / "cpp" / case["id"] / f"O{opt}" / f"sample-{sample}"
                            build_row = {
                                "id": case["id"],
                                "opt": opt,
                                "implementation": "cpp",
                                "sample": sample,
                                "elapsed_ns": None,
                                "command_index": None,
                                "success": False,
                            }
                            report["builds"].append(build_row)
                            directory.mkdir(parents=True, exist_ok=False)
                            reference = directory / "reference"
                            build = command(
                                [
                                    args.llvm_bin / "clang++",
                                    "--target=x86_64-unknown-linux-gnu",
                                    "-std=c++20",
                                    f"-O{opt}",
                                    "-fno-fast-math",
                                    "-fno-lto",
                                    case["reference"],
                                    "-o",
                                    str(reference),
                                ],
                                timeout=args.timeout,
                                env=env,
                                cpu=args.cpu,
                            )
                            build_row["elapsed_ns"] = build["elapsed_ns"]
                            build_row["command_index"] = build["command_index"]
                            require_success(build, f"{case['id']} clang++ O{opt} build sample {sample}")
                            info = checked_elf(reference, executable=True, label=f"{case['id']} clang++ O{opt} sample {sample}")
                            build_row["program"] = str(reference)
                            build_row["program_sha256"] = info["sha256"]
                            build_row["success"] = True

        check_keys = [(case["id"],) for case in cases]
        build_keys = [
            (case["id"], opt, implementation)
            for case in cases
            for opt in (0, 2)
            for implementation in ("langc", "cpp")
        ]
        report["check_stats"] = grouped_stats(
            report["checks"], args.build_samples, ("id",), check_keys
        )
        report["build_stats"] = grouped_stats(
            report["builds"], args.build_samples,
            ("id", "opt", "implementation"), build_keys
        )

        prepared_cases = {row["id"]: row for row in prepared_report["cases"]}
        for case in cases:
            prepared_case = prepared_cases[case["id"]]
            for opt in (0, 2):
                implementations = {}
                for name, key in (("candidate", "langc"), ("reference", "cpp")):
                    prepared = prepared_case[f"{key}_O{opt}"]
                    binary = prepared_path.parent / prepared["program_rel"]
                    digest = sha256_file(binary)
                    if digest != prepared["program_sha256"]:
                        raise PerfError(f"{case['id']} O{opt}: prepared {name} binary changed before measurement")
                    implementations[name] = {
                        "path": str(binary),
                        "sha256": digest,
                        "warmups": [],
                        "samples": [],
                    }
                row = {
                    "id": case["id"],
                    "opt": opt,
                    "expected": str(case["expected"]),
                    "width": case["width"],
                    "seed": case["seed"],
                    "iterations": case["iterations"],
                    "order": "alternating candidate/reference by sample index",
                    "implementations": implementations,
                    "warnings": [],
                }
                report["runs"].append(row)
                for warmup in range(args.warmups):
                    order = ("candidate", "reference") if warmup % 2 == 0 else ("reference", "candidate")
                    for name in order:
                        binary = implementations[name]["path"]
                        run = command([binary], timeout=args.run_timeout, env=env, cpu=args.cpu)
                        warmup_row = {
                            "phase": "warmup",
                            "sample": warmup,
                            "implementation": name,
                            "elapsed_ns": run["elapsed_ns"],
                            "command_index": run["command_index"],
                            "success": False,
                        }
                        implementations[name]["warmups"].append(warmup_row)
                        require_quiet_success(run, f"{case['id']} O{opt} {name} warmup {warmup}")
                        warmup_row["success"] = True
                for sample in range(args.samples):
                    order = ("candidate", "reference") if sample % 2 == 0 else ("reference", "candidate")
                    for name in order:
                        binary = implementations[name]["path"]
                        run = command([binary], timeout=args.run_timeout, env=env, cpu=args.cpu)
                        sample_row = {
                            "phase": "measured",
                            "sample": sample,
                            "implementation": name,
                            "elapsed_ns": run["elapsed_ns"],
                            "command_index": run["command_index"],
                            "success": False,
                        }
                        implementations[name]["samples"].append(sample_row)
                        require_quiet_success(run, f"{case['id']} O{opt} {name} sample {sample}")
                        sample_row["success"] = True
                for name in ("candidate", "reference"):
                    implementations[name]["stats"] = measured_stats(
                        implementations[name]["samples"], args.samples
                    )
                    minimum = implementations[name]["stats"]["min_ns"]
                    if minimum < 50_000_000:
                        row["warnings"].append(
                            f"{name} minimum {minimum} ns is below 50 ms; "
                            "process startup/loader overhead may be non-negligible"
                        )

        report["summary"] = {
            "cases": len(cases),
            "check_samples": len(report["checks"]),
            "build_samples": len(report["builds"]),
            "check_groups": len(report["check_stats"]),
            "build_groups": len(report["build_stats"]),
            "warmup_runs": sum(
                len(row["implementations"][name]["warmups"])
                for row in report["runs"]
                for name in ("candidate", "reference")
            ),
            "measured_runs": sum(
                len(row["implementations"][name]["samples"])
                for row in report["runs"]
                for name in ("candidate", "reference")
            ),
            "warnings": sum(len(row["warnings"]) for row in report["runs"]),
            "threshold_applied": False,
        }
        report["not_run"] = not_run_items(formal=True)
        report["limitations"] = limitations()
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["failure"] = {
            "type": type(error).__name__,
            "message": str(error),
            "traceback": traceback.format_exc(),
        }
        report.setdefault("not_run", not_run_items(formal=True))
        report.setdefault("limitations", limitations())
    finally:
        report["finished_at"] = utc_now()
        report["loadavg_after"] = host_info()["loadavg"]
        write_report(out, "measure.json", report)
        _COMMAND_LOG = None
        summary = {
            "status": report["status"],
            "mode": "measure",
            "out": str(out),
            "report": str(out / "measure.json"),
            "summary": report.get("summary"),
            "failure": report.get("failure"),
        }
        print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


def find_command_records(value):
    found = []
    if isinstance(value, dict):
        if all(key in value for key in ("argv", "status", "exit", "stdout", "stderr", "elapsed_ns", "timeout_s")):
            found.append(value)
        for nested in value.values():
            found.extend(find_command_records(nested))
    elif isinstance(value, list):
        for nested in value:
            found.extend(find_command_records(nested))
    return found


def self_test():
    global _COMMAND_LOG
    checks = []

    def add(name, passed, detail=None):
        checks.append({
            "name": name,
            "status": "passed" if passed else "failed",
            "detail": detail,
        })

    report = {"schema": "compiler-dev/native-performance-self-test-report/1", "commands": []}
    _COMMAND_LOG = report["commands"]
    try:
        got = compute_expected("u64-xorshift-multiply-v1", 0x1234, 1)
        add("oracle-u64-one-step", got == 15514682448277301756, {"got": str(got)})
        got = compute_expected("u64-xorshift-multiply-v1", 0xFFFFFFFFFFFFFFFF, 1)
        add("oracle-u64-overflow-mask", got == 11590811579279114451, {"got": str(got)})
        got = compute_expected("u32-branch-mix-v1", 0xFFFFFFFF, 2)
        add("oracle-u32-two-steps", got == 207895806, {"got": str(got)})
        got = compute_expected("u32-branch-mix-v1", 0x100000001, 0)
        add("oracle-u32-seed-mask", got == 1, {"got": str(got)})

        rejected = False
        try:
            require_full_width(207895806 ^ (1 << 31), 207895806, 32)
        except PerfError:
            rejected = True
        add("full-width-high-bit-mismatch-rejected", rejected)

        env = os.environ.copy()
        failed_command = command(
            [sys.executable, "-c", "import sys; print('FAIL_STDOUT'); print('FAIL_STDERR', file=sys.stderr); sys.exit(9)"],
            timeout=10,
            env=env,
        )
        add(
            "nonzero-command-recorded",
            failed_command["status"] == "exited" and failed_command["exit"] == 9,
            {"status": failed_command["status"], "exit": failed_command["exit"]},
        )
        rejected_command = False
        try:
            require_success(failed_command, "self-test command")
        except PerfError:
            rejected_command = True
        add("nonzero-command-rejected", rejected_command)

        for exit_code, stderr in ((9, ""), (-11, ""), (101, "LANG_NATIVE_PANIC:v1:test\n")):
            rejected_mismatch = False
            try:
                require_mismatch({**failed_command, "exit": exit_code, "stdout": "", "stderr": stderr},
                                 "non-comparison failure control")
            except PerfError:
                rejected_mismatch = True
            add(f"comparison-control-rejects-exit-{exit_code}", rejected_mismatch)

        timeout_command = command(
            [sys.executable, "-c",
             "import sys,time; print('TIMEOUT_STDOUT', flush=True); "
             "print('TIMEOUT_STDERR', file=sys.stderr, flush=True); time.sleep(5)"],
            timeout=1.0,
            env=env,
        )
        add(
            "timeout-command-recorded",
            timeout_command["status"] == "timeout" and timeout_command["exit"] is None,
            {"status": timeout_command["status"], "exit": timeout_command["exit"]},
        )
        rejected_timeout = False
        try:
            require_success(timeout_command, "self-test timeout")
        except PerfError:
            rejected_timeout = True
        add("timeout-command-rejected", rejected_timeout)

        with tempfile.TemporaryDirectory() as temporary:
            report_path = Path(temporary) / "self-test-report.json"
            write_json(report_path, report)
            persisted = json.loads(report_path.read_text(encoding="utf-8"))
        records = find_command_records(persisted)
        failure_record = next(
            (row for row in records if row.get("exit") == 9 and "FAIL_STDOUT" in row.get("stdout", "")),
            None,
        )
        failure_persisted = (
            failure_record is not None
            and failure_record.get("status") == "exited"
            and "FAIL_STDERR" in failure_record.get("stderr", "")
            and failure_record.get("argv")
            and failure_record.get("elapsed_ns") is not None
            and failure_record.get("timeout_s") is not None
        )
        add("failure-record-persisted", failure_persisted, {"record": failure_record})
        timeout_record = next(
            (row for row in records if row.get("status") == "timeout"
             and "TIMEOUT_STDOUT" in row.get("stdout", "")),
            None,
        )
        timeout_persisted = (
            timeout_record is not None
            and timeout_record.get("exit") is None
            and "TIMEOUT_STDERR" in timeout_record.get("stderr", "")
            and timeout_record.get("argv")
            and timeout_record.get("elapsed_ns") is not None
            and timeout_record.get("timeout_s") == 1.0
        )
        add("timeout-record-persisted", timeout_persisted, {"record": timeout_record})
    except Exception as error:
        add("self-test-exception", False, {"type": type(error).__name__, "message": str(error)})
    finally:
        _COMMAND_LOG = None

    status = "passed" if all(row["status"] == "passed" for row in checks) else "failed"
    print(json.dumps({
        "schema": "compiler-dev/native-performance-self-test/1",
        "operation_id": OPERATION_ID,
        "status": status,
        "checks": checks,
    }, indent=2, ensure_ascii=False))
    return 0 if status == "passed" else 1


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="Prepare or run the fixed native Source-vs-C++ scalar performance check.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Prepare:\n"
            "  run_native.py --prepare-only --out NEW_DIR --langc PATH --helper PATH \\\n"
            "      --llvm-bin DIR [--runtime PATH] [--compiler-root PATH]\n\n"
            "Measure (requires a passed prepare report):\n"
            "  run_native.py --out NEW_DIR --prepare-report PREPARE_JSON --langc PATH \\\n"
            "      --helper PATH --llvm-bin DIR [--runtime PATH] [--cpu N]\n\n"
            "Self-test:\n"
            "  run_native.py --self-test"
        ),
    )
    parser.add_argument("--self-test", action="store_true", help="run the short oracle/mismatch/command self-check")
    parser.add_argument("--prepare-only", action="store_true", help="prepare and validate; do not run formal timing")
    parser.add_argument("--out", type=Path, help="new output directory; existing paths are refused")
    parser.add_argument("--prepare-report", type=Path, help="passed prepare.json required by the measurement stage")
    parser.add_argument("--langc", type=Path, help="public langc executable")
    parser.add_argument("--helper", type=Path, help="lang_native_helper executable")
    parser.add_argument("--runtime", type=Path, help="liblang_native_runtime.a (default: next to helper)")
    parser.add_argument("--llvm-bin", type=Path, help="directory containing clang++ and opt")
    parser.add_argument("--fixtures", type=Path, default=DEFAULT_FIXTURES, help="fixed performance fixtures directory")
    parser.add_argument("--compiler-root", type=Path, help="optional main compiler checkout to record read-only HEAD/status")
    parser.add_argument("--ld-library-path", type=Path, help="optional directory prepended to LD_LIBRARY_PATH")
    parser.add_argument("--cpu", type=int, help="optional CPU number for child-process affinity")
    parser.add_argument("--samples", type=int, default=7, help="measured program samples per implementation (minimum 7)")
    parser.add_argument("--build-samples", type=int, default=5, help="check/build samples per item (minimum 5)")
    parser.add_argument("--warmups", type=int, default=2, help="program warmups per implementation (minimum 2)")
    parser.add_argument("--timeout", type=int, default=120, help="timeout for check/build/IR/tool commands in seconds")
    parser.add_argument("--run-timeout", type=int, default=60, help="timeout for one program execution in seconds")
    return parser.parse_args(argv)


def validate_args(args):
    if args.samples < 7:
        raise PerfError("--samples must be at least 7")
    if args.build_samples < 5:
        raise PerfError("--build-samples must be at least 5")
    if args.warmups < 2:
        raise PerfError("--warmups must be at least 2")
    if args.timeout <= 0 or args.run_timeout <= 0:
        raise PerfError("timeouts must be positive")
    if args.cpu is not None and args.cpu < 0:
        raise PerfError("--cpu must be non-negative")
    if args.out is None:
        raise PerfError("--out is required")
    if args.langc is None or args.helper is None or args.llvm_bin is None:
        raise PerfError("--langc, --helper and --llvm-bin are required")
    if args.prepare_only and args.prepare_report is not None:
        raise PerfError("--prepare-report is only valid for the measurement stage")
    if not args.prepare_only and args.prepare_report is None:
        raise PerfError("measurement requires --prepare-report from a passed prepare run")

    args.langc = args.langc.resolve()
    args.helper = args.helper.resolve()
    args.llvm_bin = args.llvm_bin.resolve()
    args.fixtures = args.fixtures.resolve()
    if args.runtime is None:
        args.runtime = args.helper.parent / "liblang_native_runtime.a"
    args.runtime = args.runtime.resolve()
    if args.compiler_root is not None:
        args.compiler_root = args.compiler_root.resolve()
    if args.ld_library_path is not None:
        args.ld_library_path = args.ld_library_path.resolve()

    for path, label in (
        (args.langc, "--langc"),
        (args.helper, "--helper"),
        (args.runtime, "--runtime"),
        (args.fixtures, "--fixtures"),
    ):
        if not path.exists():
            raise PerfError(f"{label} does not exist: {path}")
    if not args.langc.is_file() or not os.access(args.langc, os.X_OK):
        raise PerfError(f"--langc is not executable: {args.langc}")
    if not args.helper.is_file() or not os.access(args.helper, os.X_OK):
        raise PerfError(f"--helper is not executable: {args.helper}")
    for name in ("clang", "clang++", "ld.lld", "opt"):
        tool = args.llvm_bin / name
        if not tool.is_file() or not os.access(tool, os.X_OK):
            raise PerfError(f"--llvm-bin does not contain executable {name}: {tool}")
    if args.ld_library_path is not None and not args.ld_library_path.is_dir():
        raise PerfError(f"--ld-library-path is not a directory: {args.ld_library_path}")
    if args.out.exists():
        raise PerfError(f"--out already exists, refusing to overwrite: {args.out}")
    if args.prepare_report is not None:
        args.prepare_report = args.prepare_report.resolve()
        if not args.prepare_report.is_file():
            raise PerfError(f"--prepare-report does not exist: {args.prepare_report}")
    if args.cpu is not None:
        if not hasattr(os, "sched_setaffinity"):
            raise PerfError("--cpu requested but os.sched_setaffinity is unavailable")
        if hasattr(os, "sched_getaffinity"):
            allowed = os.sched_getaffinity(0)
            if args.cpu not in allowed:
                raise PerfError(f"--cpu {args.cpu} is not in the parent affinity mask {sorted(allowed)}")


def main(argv=None):
    args = parse_args(argv)
    if args.self_test:
        return self_test()
    try:
        validate_args(args)
    except PerfError as error:
        print(json.dumps({"status": "failed", "error": str(error)}), file=sys.stderr)
        return 2
    if args.prepare_only:
        return prepare(args)
    return measure(args)


if __name__ == "__main__":
    sys.exit(main())
