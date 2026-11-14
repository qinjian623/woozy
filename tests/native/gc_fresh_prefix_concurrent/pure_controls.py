#!/usr/bin/env python3
"""Pure-Python controls for the fresh-prefix concurrency probe.

The spawn/exec/socket guards are installed before the tested runner is
imported.  These controls use small textual NIR/LLVM fixtures, synthetic
handshake records and an in-memory process seam; they never compile Woozy, run
a Source program, start a collector, link an observer, probe a version or touch
the network.  The handshake controls feed *parsed* records to the real
assertion function, and the IR controls use the real Cpp literal-stride and Wz
pointer-allocation-size printed forms, so no control depends on source-text
scanning or on a fake ``passed`` value.

Covered rejection paths (each one is a distinct observed condition):

* missed handshake / missing or duplicated report records
* wrong target (length, stride, duplicate selection)
* incomplete initialized prefix at registration
* an extra retain or an unexpected initial count at the constructor return
* the request disappearing before the constructor return or at registration
* the collector finishing before the prefix/registration bracket closes
* the mutator already being in SAFE/critical before the request starts
* a poll that did not park (SAFE/critical/fork-stable contamination, or an
  emitter that parked earlier and let the collector complete)
* a collector timeout status and an exact-``False`` process exit
* existing non-empty output directory refusal with no external command
* IR seams inside the fresh prefix: poll, SAFE, callback, critical, abort/exit
* registration earlier than the prefix, of another object, or missing entirely
* a non-pointer element stride, a quoted/foreign stride expression and a
  missing constructor target
* non-finite protocol JSON, duplicate protocol keys, boolean-as-integer and
  integer-as-boolean protocol fields, and float observer metrics
* a real main that completes successfully (including the first observed
  handshake and the real allocation parser), the known after-snapshot failure,
  changed tool inputs and a Wz emit that really delegated to the Cpp helper
"""
from __future__ import annotations

import contextlib
import copy
import importlib.util
import io
import json
import os
import socket
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.dont_write_bytecode = True

ATTEMPTS = []


def blocked(name):
    def reject(*args, **kwargs):
        ATTEMPTS.append(name)
        raise RuntimeError(f"external launch blocked: {name}")
    return reject


for name in ("Popen", "run", "call", "check_call", "check_output",
             "_fork_exec"):
    if hasattr(subprocess, name):
        setattr(subprocess, name, blocked(f"subprocess.{name}"))
for name in dir(os):
    if (name.startswith("exec") or name.startswith("spawn") or
            name in ("fork", "forkpty", "posix_spawn", "posix_spawnp")):
        value = getattr(os, name)
        if callable(value):
            setattr(os, name, blocked(f"os.{name}"))
for name in ("socket", "create_connection", "create_server", "socketpair"):
    if hasattr(socket, name):
        setattr(socket, name, blocked(f"socket.{name}"))

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "gc_fresh_prefix_runner_controls", HERE / "run.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


# ---------------------------------------------------------------------------
# Small textual NIR/LLVM fixtures for the reused structural prefix assertion


def nir_function(symbol, op, inputs, slots=None):
    if slots is None:
        slots = [{"id": 0, "type": 0, "parameter": None,
                  "inout": False, "name": "%slot"}]
    instruction = {"op": op, "type": 0, "inputs": list(inputs),
                   "span": (0, 0, 0), "result": 0, "slot": None}
    return {
        "symbol": symbol,
        "signature": 0,
        "slots": slots,
        "ops": [instruction],
        "llvm_slots": {str(slot["id"]): slot["name"] for slot in slots},
        "llvm_emitter": "cpp",
    }


WZ_POINTER_STRIDE = ("ptrtoint (ptr getelementptr (ptr, ptr null, i32 1) "
                     "to i64)")


def prefix_lines(length, register=True, extra=None, early=False,
                 register_object="%v", stride=8, memcpy_stride=8, symbol=7,
                 stride_text=None):
    """Build one plausible fresh-prefix body for ``length`` elements."""
    stride_value = WZ_POINTER_STRIDE if stride_text is not None else stride
    lines = [
        f"define void @lang.fn.{symbol}() {{",
        "entry:",
    ]
    for index in range(length):
        lines.append(f"  %s{index} = alloca ptr")
    lines.append(
        f"  %v = call ptr @lang_vec_new_v1(i64 {length}, i64 {stride_value}, "
        "ptr @copy, ptr @drop)")
    if early and register:
        lines.append(
            f"  %r = call i1 @lang_gc_program_register_v1(ptr {register_object}, "
            "ptr @ops)")
    lines += [
        "  %h5 = getelementptr %Vec, ptr %v, i32 0, i32 5",
        "  %data = load ptr, ptr %h5",
        "  %h2 = getelementptr %Vec, ptr %v, i32 0, i32 2",
    ]
    for index in range(length):
        lines += [
            f"  %t{index} = getelementptr i8, ptr %data, i64 "
            f"{index * memcpy_stride}",
            f"  store ptr %arg{index}, ptr %s{index}",
            "  call void @llvm.memcpy.p0.p0.i64(ptr align 1 "
            f"%t{index}, ptr align 1 %s{index}, i64 {memcpy_stride}, "
            "i1 false)",
            f"  store i64 {index + 1}, ptr %h2",
        ]
        if extra is not None and extra[0] == index:
            lines.extend(extra[1])
    if not early and register:
        lines.append(
            f"  %r = call i1 @lang_gc_program_register_v1(ptr {register_object}, "
            "ptr @ops)")
    lines += ["  ret void", "}"]
    return lines


def must_reject(label, action, expected=None):
    try:
        action()
    except RuntimeError as error:
        text = str(error)
        if expected is not None:
            assert expected in text, (label, expected, text)
        return {"control": label, "status": "passed", "rejected": True,
                "message": text}
    raise AssertionError(f"{label}: invalid input was accepted")


def must_accept(label, action):
    value = action()
    return {"control": label, "status": "passed", "rejected": False,
            "result": value}


# ---------------------------------------------------------------------------
# Handshake record helpers


def handshake_record(mode="nonempty"):
    spec = runner.MODE_SPEC[mode]
    length = spec["length"]
    live = spec["min_registry_live"]
    return {
        "schema": "woozy/gc-fresh-prefix/handshake/1",
        "mode": mode,
        "target_length": length,
        "target_stride": spec["stride"],
        "init_at_new": 0,
        "init_at_register": length,
        "refs_at_new": 1,
        "refs_at_register": 1,
        "can_collect_at_new": 1,
        "phase_at_new_return": 1,
        "request_at_new_return": 1,
        "collector_done_at_new_return": 0,
        "phase_at_register": 1,
        "collector_busy_at_register": 1,
        "request_at_register": 1,
        "collector_done_at_register": 0,
        "markers_ok_at_register": True,
        "self_cycles_at_register": True,
        "real_register_true": True,
        "request_after_poll": 0,
        "phase_after_poll": 0,
        "collector_status": 0,
        "target_refs_after_collection": 1,
        "alive_after_collection": True,
        "markers_after_collection": True,
        "self_cycles_after_collection": True,
        "collector_joined": True,
        "target_new_hits": 1,
        "target_register_hits": 1,
        "stats": {
            "registered": live + 1,
            "traced_edges": live + 2,
            "reachable": live,
            "candidates": 1,
            "pinned": 1,
            "cleared": 1,
            "collected": 1,
            "registry_live_after": live,
        },
    }


def counts_record(mode="nonempty"):
    return {
        "schema": "woozy/gc-fresh-prefix/counts/1",
        "mode": mode,
        "nonempty_new_hits": 1,
        "nonempty_register_hits": 1,
        "empty_new_hits": 1,
        "empty_register_hits": 1,
        "target_new_hits": 1,
        "target_register_hits": 1,
        "handshake_completed": True,
        "collector_joined": True,
    }


def parsed(record=None, counts=None, failures=None):
    return {
        "handshake": [] if record is None else [record],
        "counts": [] if counts is None else [counts],
        "failures": list(failures or []),
    }


def mutated(field, value, mode="nonempty"):
    record = handshake_record(mode)
    record[field] = value
    return parsed(record, counts_record(mode))


def shim_text(mode="nonempty", record_override=None):
    record = copy.deepcopy(handshake_record(mode))
    if record_override is not None:
        record_override(record)
    return ("FRESH_PREFIX:" + json.dumps(record) + "\n"
            + "FRESH_PREFIX_COUNTS:" + json.dumps(counts_record(mode)))


def observer_stream():
    lines = ["CONFIG:vec_header_bytes:80"]
    for number in range(4):
        pointer = f"0x{number + 1:x}"
        lines += [f"ALLOC:{pointer}:88", f"VEC:1:{pointer}:1",
                  f"VEC:5:{pointer}:1", f"FREE:{pointer}"]
    return lines


# ---------------------------------------------------------------------------
# In-memory process seam and staged Root-style tool files


def _result(exit_code=0, stdout=b"", stderr=b"", timed_out=False,
            cleanup=True, abandoned=False):
    return {"exit": exit_code, "stdout": stdout, "stderr": stderr,
            "timed_out": timed_out, "cleanup_complete": cleanup,
            "abandoned_pipes": abandoned}


def traced_parts(argv):
    argv = [str(part) for part in argv]
    index = argv.index("-o")
    return Path(argv[index + 1]), argv[index + 2:]


def write_exec_log(log: Path, *executables) -> None:
    log.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    for executable in executables:
        target = str(Path(executable).resolve())
        lines.append(f'execve("{target}", ["{target}"], 0x0 /* 0 vars */) = 0')
    log.write_text("\n".join(lines) + "\n")


def stage_tools(work: Path) -> None:
    """Create the Root-style tool files the pure controls only need to exist."""
    llvm = work / "llvm" / "bin"
    llvm.mkdir(parents=True, exist_ok=True)
    for name in ("clang", "clang++", "opt", "llc", "ld.lld", "llvm-readelf"):
        (llvm / name).write_bytes(b"staged tool\n")
    for name in ("strace", "ld", "helper", "nativeobs", "woozyc",
                 "liblang_hosted_runtime.a"):
        (work / name).write_bytes(b"staged input\n")


def _main_args(work: Path, out: Path, opt="0") -> list:
    return [
        "run.py",
        "--helper", str(work / "helper"),
        "--wz-nativeobs", str(work / "nativeobs"),
        "--cli", str(work / "woozyc"),
        "--runtime", str(work / "liblang_hosted_runtime.a"),
        "--clang", str(work / "llvm/bin/clang"),
        "--readelf", str(work / "llvm/bin/llvm-readelf"),
        "--strace", str(work / "strace"),
        "--ld", str(work / "ld"),
        "--root", str(runner.ROOT),
        "--out", str(out),
        "--opt", opt,
    ]


class FakeMatrix:
    """In-memory process seam for the real main; writes real-format files."""

    def __init__(self, work: Path):
        self.work = Path(work)
        self.calls = []
        self.engine_forms = {}

    def write_engine(self, original):
        native = Path(original[original.index("--out-native") + 1])
        llvm_before = Path(original[original.index("--out-llvm-before") + 1])
        native.parent.mkdir(parents=True, exist_ok=True)
        pointer = "--stdlib" in original
        ranges = runner.SUPPORT.scan_source_functions(
            runner.SOURCE.read_bytes())
        rings = ranges["build_rings"]
        empty = ranges["build_empty"]
        native.write_text(
            "native-scalars compiler-dev/1 native64\n"
            "function @1 type=0\n"
            "  %0 = constant type=0 source=0:0-1\n"
            "function @7 type=0\n"
            " slot $0 type=0\n"
            "  %1 = vec_init type=0 %2 %3 %4 "
            f"source=0:{rings[0]}-{rings[1]}\n"
            "function @8 type=0\n"
            " slot $0 type=0\n"
            "  %1 = vec_init type=0 "
            f"source=0:{empty[0]}-{empty[1]}\n")
        stride_text = WZ_POINTER_STRIDE if pointer else None
        body = (prefix_lines(3, symbol=7, stride_text=stride_text)
                + prefix_lines(0, symbol=8, stride_text=stride_text))
        llvm_before.write_text("\n".join(body) + "\n")
        self.engine_forms[Path(original[0]).name] = (
            "pointer-allocation-size" if pointer else "literal")

    def __call__(self, argv, cwd, env, timeout):
        log, original = traced_parts(list(argv))
        exe = Path(original[0])
        self.calls.append(original)
        write_exec_log(log, exe)
        if "--out-native" in original:
            self.write_engine(original)
            return _result(stdout=b'{"status":"passed","entry_symbol":1}')
        if len(original) > 1 and original[1] == "build":
            program = (Path(original[original.index("--out-dir") + 1])
                       / "program")
            program.parent.mkdir(parents=True, exist_ok=True)
            program.write_bytes(b"hosted public program placeholder\n")
            return _result(stdout=b'{"status":"ok","diagnostics":[]}')
        if exe.name.startswith("clang"):
            target = Path(original[original.index("-o") + 1])
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"hosted observer placeholder\n")
            return _result()
        if "readelf" in exe.name:
            observed, program = original[-2], original[-1]
            text = "".join(
                f"File: {path}\n"
                "  Type: DYN (Position-Independent Executable file)\n"
                "  INTERP 0x0000000000000318 /lib64/ld-linux-x86-64.so.2\n"
                "  NEEDED libc.so.6\n"
                for path in (observed, program))
            return _result(stdout=text.encode())
        if exe.name == "observed":
            mode = env["GC_FRESH_PREFIX_MODE"]
            lines = ["FRESH_PREFIX:" + json.dumps(handshake_record(mode)),
                     "FRESH_PREFIX_COUNTS:" + json.dumps(counts_record(mode))]
            lines += observer_stream()
            return _result(stderr=("\n".join(lines) + "\n").encode())
        if exe.name == "program":
            return _result()
        raise AssertionError(f"unexpected fake command {original}")


class WzDelegationMatrix(FakeMatrix):
    """A Wz emit that really executes the Cpp helper as well."""

    def __call__(self, argv, cwd, env, timeout):
        log, original = traced_parts(list(argv))
        if "--out-native" in original and "--stdlib" in original:
            self.calls.append(original)
            self.write_engine(original)
            write_exec_log(log, original[0], self.work / "helper")
            return _result(stdout=b'{"status":"passed","entry_symbol":1}')
        return super().__call__(argv, cwd, env, timeout)


def stub_inputs(*extra: Path):
    """Configure a tiny real input set so record-level controls can run."""
    root = Path(tempfile.mkdtemp(prefix="gc-fresh-prefix-stub-"))
    strace = root / "strace"
    strace.write_bytes(b"stub strace")
    paths = {"strace": strace}
    for number, path in enumerate(extra):
        paths[f"probe{number}"] = Path(path)
    runner.INPUT_PATHS = paths
    runner.STDLIB_ROOT = None
    runner.configure_output(root)
    return root


def fake_executable(root: Path, name="observed") -> Path:
    path = Path(root) / name
    path.write_bytes(b"#!/bin/sh\nexit 0\n")
    path.chmod(0o755)
    return path


def record_row(result, root=None):
    root = Path(root) if root is not None else stub_inputs()
    exe = fake_executable(root)
    runner.INPUT_PATHS["observed"] = exe
    runner.configure_output(root)

    def process(argv, cwd, env, timeout):
        log, _original = traced_parts(list(argv))
        write_exec_log(log, exe)
        return result

    with patch.object(runner.SUPPORT, "_process_impl", process):
        return runner.run([str(exe)], 1, {}, role="observed", observed=exe)


# ---------------------------------------------------------------------------
# Whole-main controls that never reach a real process


def existing_out_control():
    work = Path(tempfile.mkdtemp(prefix="gc-fresh-prefix-existing-"))
    stage_tools(work)
    out = work / "evidence"
    old = out / "O0" / "cpp" / "old-evidence.bin"
    old.parent.mkdir(parents=True)
    old.write_bytes(b"previous evidence must remain")
    old_summary = out / "summary.json"
    old_summary.write_text('{"old":"summary"}\n')
    old_commands = out / "commands.json"
    old_commands.write_text('{"old":"commands"}\n')
    calls = []

    def refused(*args, **kwargs):
        calls.append(args)
        raise AssertionError("process must not start for existing evidence")

    stderr = io.StringIO()
    with patch.object(sys, "argv", _main_args(work, out)), \
            patch.object(runner.SUPPORT, "_process_impl", refused), \
            contextlib.redirect_stderr(stderr):
        rc = runner.main()
    assert rc == 2, (rc, stderr.getvalue())
    assert not calls, calls
    assert old.read_bytes() == b"previous evidence must remain"
    assert old_summary.read_text() == '{"old":"summary"}\n'
    assert old_commands.read_text() == '{"old":"commands"}\n'
    assert "left unchanged" in stderr.getvalue()
    return {"control": "existing-out-refused", "status": "passed",
            "rejected": True,
            "detail": {"rc": rc, "external_launch_attempts": list(ATTEMPTS)}}


def emit_failure_control():
    """A failing emitter must fail the run before any program execution."""
    work = Path(tempfile.mkdtemp(prefix="gc-fresh-prefix-emitfail-"))
    stage_tools(work)
    out = work / "evidence"
    calls = []

    def process(argv, cwd, env, timeout):
        log, original = traced_parts(list(argv))
        calls.append({"argv": original, "cwd": str(cwd), "timeout": timeout})
        write_exec_log(log, original[0])
        return _result(exit_code=1, stderr=b"fake emit failed\n")

    stderr = io.StringIO()
    with patch.object(sys, "argv", _main_args(work, out)), \
            patch.object(runner.SUPPORT, "_process_impl", process), \
            contextlib.redirect_stderr(stderr):
        rc = runner.main()
    commands = json.loads((out / "commands.json").read_text())
    summary = json.loads((out / "summary.json").read_text())
    assert rc == 1, (rc, stderr.getvalue())
    assert summary["status"] == "failed", summary
    assert "GC fresh-prefix test failed" in stderr.getvalue()
    assert [row["trace"] for row in commands] == ["emit-cpp-O0"], commands
    assert commands[0]["exit"] == 1
    assert commands[0]["exec_audit"]["status"] == "ok"
    assert len(calls) == 1, calls
    assert summary["engines"] == {}, summary
    for name in ("argv", "traced_argv", "exec_log", "exec_audit", "cwd",
                 "timeout_seconds", "exit", "timed_out", "cleanup_complete",
                 "abandoned_pipes", "stdout", "stderr",
                 "inputs_before", "inputs_after", "inputs_unchanged"):
        assert name in commands[0], (name, commands[0])
    assert commands[0]["inputs_unchanged"] is True
    assert "sha256" in commands[0]["stdout"]
    assert "hex" in commands[0]["stderr"]
    return {"control": "emit-failure-stops-run", "status": "passed",
            "rejected": True,
            "detail": {"rc": rc, "traces": [row["trace"] for row in commands]}}


def actual_main_success_control():
    """The real main completes every O0 group, including the first observer."""
    work = Path(tempfile.mkdtemp(prefix="gc-fresh-prefix-full-"))
    stage_tools(work)
    out = work / "evidence"
    fake = FakeMatrix(work)
    stderr = io.StringIO()
    with patch.object(sys, "argv", _main_args(work, out)), \
            patch.object(runner.SUPPORT, "_process_impl", fake), \
            contextlib.redirect_stderr(stderr):
        rc = runner.main()
    summary = json.loads((out / "summary.json").read_text())
    commands = json.loads((out / "commands.json").read_text())
    assert rc == 0, (rc, stderr.getvalue(), summary.get("error"))
    assert summary["status"] == "passed", summary
    assert len(commands) == 16, [row["trace"] for row in commands]
    assert summary["matrix"]["expected_commands"] == 16
    assert summary["matrix"]["completed_commands"] == 16
    assert summary["matrix"]["logical_groups"] == 7
    assert all(group["status"] == "passed"
               for group in summary["matrix"]["groups"])
    first = summary["observed"]["cpp-O0-nonempty"]
    assert first["status"] == "passed", first
    assert first["handshake"]["mode"] == "nonempty"
    assert first["allocation_observer"]["heap_allocations"] == 4
    assert first["allocation_observer"]["final_live_heap_count"] == 0
    assert first["hosted_link"] is True
    assert summary["link_class"]["cpp-O0-nonempty"]["public_program"]["hosted"]
    assert summary["inputs"]["unchanged"] is True
    # Both real printed stride forms were exercised by the actual check_engine.
    assert fake.engine_forms["helper"] == "literal", fake.engine_forms
    assert fake.engine_forms["nativeobs"] == "pointer-allocation-size", \
        fake.engine_forms
    # Every command really carried the fixed trace prefix and a real audit.
    strace = str(runner.INPUT_PATHS["strace"])
    for row in commands:
        assert row["traced_argv"][0] == strace, row["traced_argv"][0]
        assert Path(row["exec_log"]).is_file(), row["exec_log"]
        assert row["exec_log_sha256"], row
        assert row["exec_audit"]["status"] == "ok", row["exec_audit"]
    return {"control": "actual-main-success", "status": "passed",
            "rejected": False,
            "detail": {"rc": rc, "commands": len(commands),
                       "logical_groups": 7,
                       "first_observer": "cpp-O0-nonempty",
                       "first_observer_metrics": first["allocation_observer"],
                       "engine_stride_forms": fake.engine_forms}}


def tool_alias_control():
    """The real main preserves LLVM's argv[0] aliases while auditing targets."""
    work = Path(tempfile.mkdtemp(prefix="gc-fresh-prefix-alias-"))
    stage_tools(work)
    tools = work / "llvm/bin"
    for alias, target in (("clang++", "clang-22"),
                          ("llvm-readelf", "llvm-readobj")):
        (tools / target).write_bytes(b"staged alias target\n")
        (tools / alias).unlink()
        (tools / alias).symlink_to(target)
    out = work / "evidence"
    fake = FakeMatrix(work)
    argv = _main_args(work, out)
    argv[argv.index("--clang") + 1] = str(tools / "clang++")
    with patch.object(sys, "argv", argv), \
            patch.object(runner.SUPPORT, "_process_impl", fake), \
            contextlib.redirect_stderr(io.StringIO()):
        rc = runner.main()
    assert rc == 0, (rc, (out / "summary.json").read_text())
    assert len(fake.calls) == 16
    assert all(Path(cmd[0]).name == "clang++" for cmd in fake.calls
               if "-std=c++20" in cmd)
    assert all(Path(cmd[0]).name == "llvm-readelf" for cmd in fake.calls
               if "-l" in cmd and "-d" in cmd)
    return {"control": "actual-main-preserves-LLVM-tool-aliases",
            "status": "passed", "commands": len(fake.calls)}


def after_snapshot_control():
    """The known after-snapshot failure keeps the executed command record."""
    work = Path(tempfile.mkdtemp(prefix="gc-fresh-prefix-aftersnap-"))
    stage_tools(work)
    out = work / "evidence"
    fake = FakeMatrix(work)
    real = runner.tracked_inputs
    calls = []

    def flaky():
        calls.append(1)
        if len(calls) == 2:
            raise OSError("after snapshot unreadable")
        return real()

    stderr = io.StringIO()
    with patch.object(sys, "argv", _main_args(work, out)), \
            patch.object(runner.SUPPORT, "_process_impl", fake), \
            patch.object(runner, "tracked_inputs", flaky), \
            contextlib.redirect_stderr(stderr):
        rc = runner.main()
    summary = json.loads((out / "summary.json").read_text())
    commands = json.loads((out / "commands.json").read_text())
    assert rc == 1, (rc, stderr.getvalue())
    assert len(commands) == 1, commands
    row = commands[0]
    assert row["trace"] == "emit-cpp-O0", row["trace"]
    assert row["exit"] == 0 and type(row["exit"]) is int
    assert row["stdout"]["sha256"] and row["stdout"]["bytes"] > 0
    assert row["inputs_after"] is None
    assert "after snapshot unreadable" in row["inputs_after_error"]
    assert "after snapshot unreadable" in summary["error"]
    assert [item for item in [1, 1]] == calls[:2]
    return {"control": "actual-main-after-snapshot-preserves-command",
            "status": "passed", "rejected": True,
            "detail": {"rc": rc, "saved_commands": len(commands),
                       "raw_stdout_sha256": row["stdout"]["sha256"],
                       "inputs_after_error": row["inputs_after_error"]}}


def tool_change_control():
    """A changed frozen tool input is rejected after its command is saved."""
    work = Path(tempfile.mkdtemp(prefix="gc-fresh-prefix-toolchange-"))
    stage_tools(work)
    out = work / "evidence"
    fake = FakeMatrix(work)
    real = runner.tracked_inputs
    drifted = {"value": False}

    def drifting():
        value = real()
        if drifted["value"]:
            key = sorted(value)[0]
            value = dict(value)
            value[key] = "0" * 64
        return value

    original_call = fake.__call__

    def process(argv, cwd, env, timeout):
        result = original_call(argv, cwd, env, timeout)
        drifted["value"] = True
        return result

    stderr = io.StringIO()
    with patch.object(sys, "argv", _main_args(work, out)), \
            patch.object(runner.SUPPORT, "_process_impl", process), \
            patch.object(runner, "tracked_inputs", drifting), \
            contextlib.redirect_stderr(stderr):
        rc = runner.main()
    summary = json.loads((out / "summary.json").read_text())
    commands = json.loads((out / "commands.json").read_text())
    assert rc == 1, (rc, stderr.getvalue())
    assert len(commands) == 1, commands
    assert commands[0]["inputs_unchanged"] is False
    assert "tracked inputs changed" in summary["error"], summary["error"]
    assert summary["inputs"]["unchanged"] is False
    return {"control": "actual-main-changed-tool-input-rejected",
            "status": "passed", "rejected": True,
            "detail": {"rc": rc, "error": summary["error"]}}


def stdlib_membership_control(change):
    """The real main rejects files added or removed during its first command."""
    work = Path(tempfile.mkdtemp(prefix="gc-fresh-prefix-membership-"))
    stage_tools(work)
    stdlib = work / "stdlib"
    stdlib.mkdir()
    existing = stdlib / "existing.wz"
    existing.write_text("// original input\n")
    out = work / "evidence"
    fake = FakeMatrix(work)

    def process(argv, cwd, env, timeout):
        result = fake(argv, cwd, env, timeout)
        if len(fake.calls) == 1:
            if change == "added":
                (stdlib / "new-input.wz").write_text("// added input\n")
            else:
                existing.unlink()
        return result

    stderr = io.StringIO()
    argv = _main_args(work, out) + ["--stdlib", str(stdlib)]
    with patch.object(sys, "argv", argv), \
            patch.object(runner.SUPPORT, "_process_impl", process), \
            contextlib.redirect_stderr(stderr):
        rc = runner.main()
    summary = json.loads((out / "summary.json").read_text())
    commands = json.loads((out / "commands.json").read_text())
    assert rc == 1, (rc, stderr.getvalue())
    assert len(commands) == len(fake.calls) == 1, commands
    assert commands[0]["inputs_unchanged"] is False
    assert summary["inputs"]["unchanged"] is False
    assert "tracked inputs changed" in summary["error"]
    return {"control": f"actual-main-stdlib-{change}-file-rejected",
            "status": "passed", "rejected": True,
            "detail": {"rc": rc, "saved_commands": len(commands)}}


def wz_delegation_control():
    """A Wz emit that really executes the Cpp helper is rejected by the audit."""
    work = Path(tempfile.mkdtemp(prefix="gc-fresh-prefix-wzdeleg-"))
    stage_tools(work)
    out = work / "evidence"
    fake = WzDelegationMatrix(work)
    stderr = io.StringIO()
    with patch.object(sys, "argv", _main_args(work, out)), \
            patch.object(runner.SUPPORT, "_process_impl", fake), \
            contextlib.redirect_stderr(stderr):
        rc = runner.main()
    summary = json.loads((out / "summary.json").read_text())
    commands = json.loads((out / "commands.json").read_text())
    assert rc == 1, (rc, stderr.getvalue())
    assert [row["trace"] for row in commands] == ["emit-cpp-O0", "emit-wz-O0"]
    rejected = commands[1]
    assert rejected["exec_audit"]["status"] == "failed", rejected["exec_audit"]
    assert "unexpected" in rejected["exec_audit"]["error"]
    assert "helper" in rejected["exec_audit"]["error"]
    assert rejected["exec_log_sha256"], rejected
    assert "OS exec audit rejected" in summary["error"], summary["error"]
    assert summary["engines"].get("cpp-O0", {}).get("status") == "passed"
    assert "wz-O0" not in summary["engines"]
    return {"control": "trace-rejects-cpp-delegation-in-wz-emit",
            "status": "passed", "rejected": True,
            "detail": {"rc": rc, "saved_commands": len(commands),
                       "exec_audit": rejected["exec_audit"]}}


def typed_record_controls():
    controls = []

    row = record_row(_result(exit_code=False))
    assert row["exit"] is False and type(row["exit"]) is bool
    controls.append(must_reject(
        "record-boolean-exit-rejected",
        lambda row=row: runner.assert_execution_completed(
            row, "record-boolean-exit-rejected"),
        "exit is not an exact int"))

    row = record_row(_result(exit_code=7))
    controls.append(must_reject(
        "record-nonzero-exit-rejected",
        lambda row=row: runner.ok(row, "record-nonzero-exit-rejected"),
        "exited 7"))

    row = record_row(_result(stdout=b"\xffpartial\x00", stderr=b"\xff",
                             timed_out=True))
    assert row["timed_out"] is True
    assert row["stdout"]["hex"] == "ff7061727469616c00"
    assert row["stderr"]["hex"] == "ff"
    controls.append(must_reject(
        "record-timeout-partial-bytes-rejected",
        lambda row=row: runner.assert_execution_completed(
            row, "record-timeout-partial-bytes-rejected"),
        "timed out"))
    controls.append({"control": "record-timeout-partial-bytes-kept",
                     "status": "passed", "rejected": True,
                     "detail": {"stdout_hex": row["stdout"]["hex"],
                                "stderr_hex": row["stderr"]["hex"]}})

    root = stub_inputs()
    exe = fake_executable(root)
    saved = []

    def launch_failure(*args, **kwargs):
        raise OSError("missing helper")

    with patch.object(runner.SUPPORT, "_process_impl", launch_failure):
        controls.append(must_reject(
            "record-launch-failure-rejected",
            lambda: runner.run([str(exe)], 1, {}, role="observed",
                               observed=exe, sink=saved.append),
            "OS exec audit rejected"))
    assert saved, "launch failure must be recorded before the rejection"
    assert saved[0]["launch_failed"] is True
    assert saved[0]["exec_audit"]["status"] == "not_traced"
    controls.append({"control": "record-launch-failure-kept",
                     "status": "passed", "rejected": True,
                     "detail": {"launch_error": saved[0]["launch_error"],
                                "exec_audit": saved[0]["exec_audit"]}})

    root = stub_inputs()
    exe = fake_executable(root)

    def process(argv, cwd, env, timeout):
        log, _original = traced_parts(list(argv))
        write_exec_log(log, exe)
        return _result()

    with patch.object(runner.SUPPORT, "_process_impl", process), \
            patch.object(runner, "tracked_inputs",
                         side_effect=[{"a": "1"}, {"a": "2"}]):
        row = runner.run([str(exe)], 1, {}, role="observed", observed=exe)
    controls.append(must_reject(
        "record-input-sha-change-rejected",
        lambda row=row: runner.ok(row, "record-input-sha-change-rejected"),
        "tracked inputs changed"))
    return controls


def main() -> int:
    controls = []

    # --- structural prefix assertion (reused from the sibling runner) -------
    target = nir_function(7, "vec_init", [0, 1, 2])
    controls.append(must_accept(
        "ir-prefix-positive",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-prefix-positive", {"registered": True, "elements": 3},
            target, prefix_lines(3))))

    poll_seam = (1, ["  call void @lang_gc_runtime_poll_v1()"])
    controls.append(must_reject(
        "ir-prefix-poll-inside",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-prefix-poll-inside", {"registered": True, "elements": 3},
            target, prefix_lines(3, extra=poll_seam)),
        "non-memcpy call inside vec_init prefix"))

    safe_seam = (1, ["  %g = call i32 @lang_gc_program_enter_safe_v1()",
                     "  call void @lang_gc_program_leave_safe_v1(i32 %g)"])
    controls.append(must_reject(
        "ir-prefix-safe-inside",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-prefix-safe-inside", {"registered": True, "elements": 3},
            target, prefix_lines(3, extra=safe_seam)),
        "non-memcpy call inside vec_init prefix"))

    callback_seam = (0, [
        "  %tok = alloca i8, i64 32",
        "  %c = call i32 @lang_gc_program_callback_enter_v1(ptr %tok)",
        "  call void @lang_gc_program_callback_leave_v1(i32 %c, ptr %tok)"])
    controls.append(must_reject(
        "ir-prefix-callback-inside",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-prefix-callback-inside", {"registered": True, "elements": 3},
            target, prefix_lines(3, extra=callback_seam)),
        "non-memcpy call inside vec_init prefix"))

    critical_seam = (2, ["  %g = call i32 @lang_gc_program_enter_critical_v1()",
                         "  call void @lang_gc_program_leave_critical_v1(i32 %g)"])
    controls.append(must_reject(
        "ir-prefix-critical-inside",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-prefix-critical-inside", {"registered": True, "elements": 3},
            target, prefix_lines(3, extra=critical_seam)),
        "critical call inside vec_init prefix"))

    exit_seam = (2, ["  call void @lang_os_exit_v1(i32 9)"])
    controls.append(must_reject(
        "ir-prefix-exit-inside",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-prefix-exit-inside", {"registered": True, "elements": 3},
            target, prefix_lines(3, extra=exit_seam)),
        "non-memcpy call inside vec_init prefix"))

    controls.append(must_reject(
        "ir-prefix-register-early",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-prefix-register-early", {"registered": True, "elements": 3},
            target, prefix_lines(3, early=True)),
        "initialized-prefix stores are"))

    controls.append(must_reject(
        "ir-prefix-register-other-object",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-prefix-register-other-object",
            {"registered": True, "elements": 3}, target,
            prefix_lines(3, register_object="%other")),
        "registration of %v is missing"))

    controls.append(must_reject(
        "ir-prefix-register-missing",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-prefix-register-missing", {"registered": True, "elements": 3},
            target, prefix_lines(3, register=False)),
        "registration of %v is missing"))

    controls.append(must_reject(
        "ir-target-absent",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-target-absent", {"registered": True, "elements": 3},
            nir_function(7, "store", [0]), prefix_lines(3)),
        "no vec_init operation found"))

    controls.append(must_reject(
        "ir-target-missing-constructor",
        lambda: runner.assert_target_stride(
            "ir-target-missing-constructor",
            ["define void @lang.fn.7() {", "entry:", "  ret void", "}"], 3),
        "expected one lang_vec_new_v1 call"))

    controls.append(must_accept(
        "ir-target-stride-positive",
        lambda: runner.assert_target_stride(
            "ir-target-stride-positive", prefix_lines(3), 3)))

    # The real Wz text backend prints the canonical pointer allocation-size
    # expression instead of a literal, so both actual printed forms must pass.
    controls.append(must_accept(
        "ir-target-wz-pointer-stride-positive",
        lambda: runner.assert_target_stride(
            "ir-target-wz-pointer-stride-positive",
            prefix_lines(3, stride_text=WZ_POINTER_STRIDE), 3)))

    controls.append(must_reject(
        "ir-target-wrong-stride",
        lambda: runner.assert_target_stride(
            "ir-target-wrong-stride", prefix_lines(3, stride=16), 3),
        "expected one pointer"))

    controls.append(must_reject(
        "ir-target-wrong-length",
        lambda: runner.assert_target_stride(
            "ir-target-wrong-length", prefix_lines(3), 0),
        "length is 3"))

    controls.append(must_accept(
        "ir-empty-prefix-positive",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-empty-prefix-positive", {"registered": True, "elements": 0},
            nir_function(8, "vec_init", []), prefix_lines(0))))

    controls.append(must_reject(
        "ir-empty-prefix-wrote-init",
        lambda: runner.SUPPORT.assert_prefix_case(
            "ir-empty-prefix-wrote-init", {"registered": True, "elements": 0},
            nir_function(8, "vec_init", []), [
                "define void @lang.fn.8() {",
                "entry:",
                "  %v = call ptr @lang_vec_new_v1(i64 0, i64 8, ptr @copy, "
                "ptr @drop)",
                "  %h2 = getelementptr %Vec, ptr %v, i32 0, i32 2",
                "  store i64 1, ptr %h2",
                "  %r = call i1 @lang_gc_program_register_v1(ptr %v, ptr @ops)",
                "  ret void",
                "}",
            ]),
        "empty Vec wrote an initialized-prefix store"))

    # --- explicit forbidden-seam scan -------------------------------------
    controls.append(must_accept(
        "ir-seam-scan-positive",
        lambda: runner.assert_no_forbidden_seam(
            "ir-seam-scan-positive", prefix_lines(3))))
    for symbol, seam in (("poll", (1, ["  call void @lang_gc_runtime_poll_v1()"])),
                         ("safe", safe_seam),
                         ("callback", callback_seam),
                         ("critical", critical_seam)):
        controls.append(must_reject(
            f"ir-seam-{symbol}-rejected",
            lambda seam=seam, symbol=symbol: runner.assert_no_forbidden_seam(
                "ir-seam-" + symbol, prefix_lines(3, extra=seam)),
            "forbidden seam call"))

    # --- full IR pipeline through the actual check_engine caller ----------
    def engine_case(symbol_lines, label="cpp-O0"):
        work = Path(tempfile.mkdtemp(prefix="gc-fresh-prefix-engine-"))
        ranges = runner.SUPPORT.scan_source_functions(
            runner.SOURCE.read_bytes())
        rings = ranges["build_rings"]
        empty = ranges["build_empty"]
        nir = (
            "native-scalars 1\n"
            "function @1 type=0\n"
            "  %0 = const_int type=0 source=0:0-1\n"
            "function @7 type=0\n"
            " slot $0 type=0\n"
            "  %1 = vec_init type=0 %2 %3 %4 "
            f"source=0:{rings[0]}-{rings[1]}\n"
            "function @8 type=0\n"
            " slot $0 type=0\n"
            "  %1 = vec_init type=0 "
            f"source=0:{empty[0]}-{empty[1]}\n"
        )
        (work / "program.nir").write_text(nir)
        (work / "before.ll").write_text(
            "\n".join(symbol_lines) + "\n")
        emitted = {"stdout": {"text": json.dumps(
            {"status": "passed", "entry_symbol": 1})}}
        return runner.check_engine(label, emitted, work, runner.SOURCE)

    good = prefix_lines(3, symbol=7) + prefix_lines(0, symbol=8)
    controls.append(must_accept(
        "ir-check-engine-positive", lambda: engine_case(good)))

    # The same real pipeline must accept the Wz printed pointer-stride form.
    wz_good = (prefix_lines(3, symbol=7, stride_text=WZ_POINTER_STRIDE)
               + prefix_lines(0, symbol=8, stride_text=WZ_POINTER_STRIDE))
    controls.append(must_accept(
        "ir-check-engine-wz-pointer-stride",
        lambda: engine_case(wz_good, label="wz-O0")))

    bad = prefix_lines(3, symbol=7,
                       extra=(1, ["  call void @lang_gc_runtime_poll_v1()"]))
    bad = bad + prefix_lines(0, symbol=8)
    controls.append(must_reject(
        "ir-check-engine-poll-in-prefix",
        lambda: engine_case(bad),
        "non-memcpy call inside vec_init prefix"))

    # A literal stride must not be accepted in a quoted/foreign spelling.
    bad_stride = [line.replace("i64 8,", 'i64 "8",')
                  for line in prefix_lines(3, symbol=7)]
    bad_stride = bad_stride + prefix_lines(0, symbol=8)
    controls.append(must_reject(
        "ir-check-engine-quoted-stride",
        lambda: engine_case(bad_stride),
        "unrecognized lang_vec_new_v1 element stride expression"))

    bad_empty = prefix_lines(3, symbol=7) + [
        "define void @lang.fn.8() {",
        "entry:",
        "  %v = call ptr @lang_vec_new_v1(i64 0, i64 8, ptr @copy, ptr @drop)",
        "  %g = call i32 @lang_gc_program_enter_critical_v1()",
        "  %r = call i1 @lang_gc_program_register_v1(ptr %v, ptr @ops)",
        "  call void @lang_gc_program_leave_critical_v1(i32 %g)",
        "  ret void",
        "}",
    ]
    controls.append(must_reject(
        "ir-check-engine-critical-in-empty-prefix",
        lambda: engine_case(bad_empty),
        "critical call inside vec_init prefix"))

    # --- handshake positives ----------------------------------------------
    for mode in runner.MODES:
        controls.append(must_accept(
            f"handshake-{mode}-positive",
            lambda mode=mode: runner.assert_handshake(
                f"handshake-{mode}-positive", mode,
                parsed(handshake_record(mode), counts_record(mode)))))

    # --- handshake negatives ----------------------------------------------
    nonempty = "nonempty"
    controls.append(must_reject(
        "handshake-missed-report",
        lambda: runner.assert_handshake("handshake-missed-report", nonempty,
                                        parsed()),
        "report-count"))
    controls.append(must_reject(
        "handshake-shim-failure-line",
        lambda: runner.assert_handshake(
            "handshake-shim-failure-line", nonempty,
            parsed(handshake_record(nonempty), counts_record(nonempty),
                   failures=[{"reason": "request_not_observed_at_new_return"}])),
        "shim-failure"))
    controls.append(must_reject(
        "handshake-duplicate-report",
        lambda: runner.assert_handshake(
            "handshake-duplicate-report", nonempty,
            {"handshake": [handshake_record(nonempty),
                           handshake_record(nonempty)],
             "counts": [counts_record(nonempty)], "failures": []}),
        "report-count"))
    controls.append(must_reject(
        "handshake-missing-counts",
        lambda: runner.assert_handshake(
            "handshake-missing-counts", nonempty,
            parsed(handshake_record(nonempty))),
        "report-count"))
    controls.append(must_reject(
        "handshake-wrong-mode",
        lambda: runner.assert_handshake(
            "handshake-wrong-mode", "empty",
            parsed(handshake_record(nonempty), counts_record(nonempty))),
        "mode"))
    controls.append(must_reject(
        "handshake-wrong-target-length",
        lambda: runner.assert_handshake(
            "handshake-wrong-target-length", nonempty,
            mutated("target_length", 2)),
        "target-identity"))
    controls.append(must_reject(
        "handshake-wrong-target-stride",
        lambda: runner.assert_handshake(
            "handshake-wrong-target-stride", nonempty,
            mutated("target_stride", 16)),
        "target-identity"))
    controls.append(must_reject(
        "handshake-duplicate-target-hits",
        lambda: runner.assert_handshake(
            "handshake-duplicate-target-hits", nonempty,
            mutated("target_new_hits", 2)),
        "selection"))
    controls.append(must_reject(
        "handshake-init-not-zero-at-new",
        lambda: runner.assert_handshake(
            "handshake-init-not-zero-at-new", nonempty,
            mutated("init_at_new", 1)),
        "init-prefix"))
    controls.append(must_reject(
        "handshake-init-prefix-incomplete",
        lambda: runner.assert_handshake(
            "handshake-init-prefix-incomplete", nonempty,
            mutated("init_at_register", 2)),
        "init-prefix"))
    controls.append(must_reject(
        "handshake-extra-retain",
        lambda: runner.assert_handshake(
            "handshake-extra-retain", nonempty,
            mutated("refs_at_register", 2)),
        "target-refs"))
    controls.append(must_reject(
        "handshake-safe-before-request",
        lambda: runner.assert_handshake(
            "handshake-safe-before-request", nonempty,
            mutated("can_collect_at_new", 0)),
        "preexisting-request"))
    controls.append(must_reject(
        "handshake-request-missing-at-new",
        lambda: runner.assert_handshake(
            "handshake-request-missing-at-new", nonempty,
            mutated("request_at_new_return", 0)),
        "request-span"))
    controls.append(must_reject(
        "handshake-request-missing-at-register",
        lambda: runner.assert_handshake(
            "handshake-request-missing-at-register", nonempty,
            mutated("request_at_register", 0)),
        "request-span"))
    controls.append(must_reject(
        "handshake-collector-done-before-new",
        lambda: runner.assert_handshake(
            "handshake-collector-done-before-new", nonempty,
            mutated("collector_done_at_new_return", 1)),
        "request-span"))
    controls.append(must_reject(
        "handshake-collector-done-before-register",
        lambda: runner.assert_handshake(
            "handshake-collector-done-before-register", nonempty,
            mutated("collector_done_at_register", 1)),
        "request-span"))
    controls.append(must_reject(
        "handshake-kernel-before-register",
        lambda: runner.assert_handshake(
            "handshake-kernel-before-register", nonempty,
            mutated("phase_at_register", 2)),
        "phase-order"))
    controls.append(must_reject(
        "handshake-kernel-at-new",
        lambda: runner.assert_handshake(
            "handshake-kernel-at-new", nonempty,
            mutated("phase_at_new_return", 2)),
        "phase-order"))
    controls.append(must_reject(
        "handshake-collector-not-busy",
        lambda: runner.assert_handshake(
            "handshake-collector-not-busy", nonempty,
            mutated("collector_busy_at_register", 0)),
        "phase-order"))
    controls.append(must_reject(
        "handshake-real-register-false",
        lambda: runner.assert_handshake(
            "handshake-real-register-false", nonempty,
            mutated("real_register_true", False)),
        "real-register"))
    controls.append(must_reject(
        "handshake-poll-did-not-park",
        lambda: runner.assert_handshake(
            "handshake-poll-did-not-park", nonempty,
            mutated("request_after_poll", 1)),
        "poll-park"))
    controls.append(must_reject(
        "handshake-phase-after-poll",
        lambda: runner.assert_handshake(
            "handshake-phase-after-poll", nonempty,
            mutated("phase_after_poll", 2)),
        "post-phase"))
    controls.append(must_reject(
        "handshake-collector-timeout",
        lambda: runner.assert_handshake(
            "handshake-collector-timeout", nonempty,
            mutated("collector_status", -110)),
        "collector-status"))
    controls.append(must_reject(
        "handshake-no-reclaim-work",
        lambda: runner.assert_handshake(
            "handshake-no-reclaim-work", nonempty,
            mutated("stats", {"registered": 5, "traced_edges": 6,
                              "reachable": 5, "candidates": 0, "pinned": 0,
                              "cleared": 0, "collected": 0,
                              "registry_live_after": 5})),
        "kernel-stats"))
    controls.append(must_reject(
        "handshake-registry-live-too-small",
        lambda: runner.assert_handshake(
            "handshake-registry-live-too-small", nonempty,
            mutated("stats", {"registered": 5, "traced_edges": 6,
                              "reachable": 3, "candidates": 2, "pinned": 2,
                              "cleared": 2, "collected": 2,
                              "registry_live_after": 3})),
        "kernel-stats"))
    controls.append(must_reject(
        "handshake-target-died",
        lambda: runner.assert_handshake(
            "handshake-target-died", nonempty,
            mutated("alive_after_collection", False)),
        "alive-after"))
    controls.append(must_reject(
        "handshake-marker-lost",
        lambda: runner.assert_handshake(
            "handshake-marker-lost", nonempty,
            mutated("markers_after_collection", False)),
        "alive-after"))
    controls.append(must_reject(
        "handshake-refs-after",
        lambda: runner.assert_handshake(
            "handshake-refs-after", nonempty,
            mutated("target_refs_after_collection", 0)),
        "alive-after"))
    controls.append(must_reject(
        "handshake-not-joined",
        lambda: runner.assert_handshake(
            "handshake-not-joined", nonempty,
            mutated("collector_joined", False)),
        "join"))
    controls.append(must_reject(
        "handshake-control-not-seen",
        lambda: runner.assert_handshake(
            "handshake-control-not-seen", nonempty,
            parsed(handshake_record(nonempty),
                   dict(counts_record(nonempty), empty_new_hits=0))),
        "selection"))

    # --- exact protocol types and finite JSON (F7) -------------------------
    controls.append(must_reject(
        "handshake-boolean-integer-rejected",
        lambda: runner.assert_handshake(
            "handshake-boolean-integer-rejected", nonempty,
            mutated("init_at_new", True)),
        "expected an exact integer"))
    controls.append(must_reject(
        "handshake-float-integer-rejected",
        lambda: runner.assert_handshake(
            "handshake-float-integer-rejected", nonempty,
            mutated("target_stride", 8.0)),
        "expected an exact integer"))
    controls.append(must_reject(
        "handshake-integer-flag-rejected",
        lambda: runner.assert_handshake(
            "handshake-integer-flag-rejected", nonempty,
            mutated("real_register_true", 1)),
        "expected an exact boolean"))
    controls.append(must_reject(
        "handshake-counts-boolean-integer-rejected",
        lambda: runner.assert_handshake(
            "handshake-counts-boolean-integer-rejected", nonempty,
            parsed(handshake_record(nonempty),
                   dict(counts_record(nonempty), target_new_hits=True))),
        "expected an exact integer"))
    controls.append(must_reject(
        "handshake-stats-boolean-integer-rejected",
        lambda: runner.assert_handshake(
            "handshake-stats-boolean-integer-rejected", nonempty,
            mutated("stats", dict(handshake_record(nonempty)["stats"],
                                 collected=True))),
        "stats.collected"))
    controls.append(must_reject(
        "handshake-infinity-rejected",
        lambda: runner.parse_shim(shim_text(
            record_override=lambda record: record["stats"].__setitem__(
                "collected", "__INF__")).replace('"__INF__"', "Infinity")),
        "non-finite JSON constant"))
    controls.append(must_reject(
        "handshake-nan-rejected",
        lambda: runner.parse_shim(shim_text(
            record_override=lambda record: record["stats"].__setitem__(
                "collected", "__NAN__")).replace('"__NAN__"', "NaN")),
        "non-finite JSON constant"))
    controls.append(must_reject(
        "handshake-duplicate-key-rejected",
        lambda: runner.parse_shim(
            'FRESH_PREFIX:{"schema":"a","schema":"b"}'),
        "duplicate JSON key"))
    for token in ("1e1000", "-1e1000"):
        controls.append(must_reject(
            f"protocol-exponent-overflow-{token}-rejected",
            lambda token=token: runner.strict_json(
                '{"extra":' + token + '}', "overflow control"),
            "non-finite JSON number"))

    # --- observer balance ------------------------------------------------
    def balance(**overrides):
        base = {"header_bytes": 80,
                "final_live_heap_count": 0, "final_live_heap_bytes": 0,
                "heap_allocations": 12, "heap_frees": 12,
                "vec_allocations": 6, "vec_frees": 6,
                "live_vecs_at_exit": 0, "final_live_payload_bytes": 0,
                "peak_live_payload_bytes": 64}
        base.update(overrides)
        return base

    controls.append(must_accept(
        "observer-balance-positive",
        lambda: runner.assert_observer_balance(
            "observer-balance-positive", balance())))
    controls.append(must_reject(
        "observer-balance-leak",
        lambda: runner.assert_observer_balance(
            "observer-balance-leak",
            balance(final_live_heap_count=1, heap_frees=11)),
        "leaked heap allocations"))
    controls.append(must_reject(
        "observer-balance-double-free",
        lambda: runner.assert_observer_balance(
            "observer-balance-double-free",
            balance(heap_allocations=12, heap_frees=12,
                    final_live_heap_count=0, vec_frees=7)),
        "unbalanced Vec allocation/free"))
    controls.append(must_reject(
        "observer-balance-vacuous",
        lambda: runner.assert_observer_balance(
            "observer-balance-vacuous",
            balance(heap_allocations=0, heap_frees=0, vec_allocations=0,
                    vec_frees=0)),
        "too few observed allocations"))
    controls.append(must_reject(
        "observer-balance-float-metric-rejected",
        lambda: runner.assert_observer_balance(
            "observer-balance-float-metric-rejected",
            balance(heap_allocations=12.0)),
        "expected an exact integer"))

    controls.extend(typed_record_controls())
    controls.append(existing_out_control())
    controls.append(emit_failure_control())
    controls.append(actual_main_success_control())
    controls.append(tool_alias_control())
    controls.append(after_snapshot_control())
    controls.append(tool_change_control())
    controls.append(stdlib_membership_control("added"))
    controls.append(stdlib_membership_control("removed"))
    controls.append(wz_delegation_control())

    report = {
        "schema": "woozy/gc-fresh-prefix-concurrent/pure-controls/1",
        "status": "passed",
        "controls": controls,
        "control_count": len(controls),
        "external_launch_attempts": ATTEMPTS,
        "import_guard": {
            "subprocess": ["Popen", "run", "call", "check_call",
                           "check_output", "_fork_exec"],
            "os": "exec*/spawn*/fork/forkpty/posix_spawn/posix_spawnp",
            "socket": ["socket", "create_connection", "create_server",
                       "socketpair"],
        },
    }
    assert not ATTEMPTS, ATTEMPTS
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
