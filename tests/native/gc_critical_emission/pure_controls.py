#!/usr/bin/env python3
"""Pure-Python controls for the GC critical emission assertions.

The spawn/exec/socket guards are installed before the tested runner is
imported.  These controls use small textual NIR/LLVM fixtures and the actual
assertion functions; they do not compile Woozy, run a Source program, start a
component, or touch the network.  The actual-main controls replace only the
bounded-process seam with in-memory rows; ``external_launch_attempts`` must
stay empty.
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
    "gc_critical_runner_controls", HERE / "run_gc_critical_emission.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def nir_function(symbol, op, inputs, slots=None):
    if slots is None:
        if op == "store":
            slots = [{"id": 0, "type": 0, "parameter": None,
                      "inout": False, "name": "%slot"}]
        elif op in ("replace_indirect", "store_indirect"):
            slots = [{"id": 0, "type": 0, "parameter": 0,
                      "inout": True, "name": "%slot"}]
        elif op == "vec_init":
            slots = [{"id": 0, "type": 0, "parameter": None,
                      "inout": False, "name": "%slot"}]
        else:
            slots = []
    instruction = {"op": op, "type": 0, "inputs": list(inputs),
                   "span": (0, 0, 0), "result": 0}
    if op in ("store", "replace_indirect", "store_indirect"):
        instruction["slot"] = 0
    return {
        "symbol": symbol,
        "signature": 0,
        "slots": slots,
        "ops": [instruction],
        "llvm_slot_names": {str(slot["id"]): slot["name"]
                            for slot in slots if "name" in slot},
    }


def must_reject(label, action, message=None):
    try:
        action()
    except RuntimeError as error:
        if message is not None:
            assert message in str(error), (label, str(error))
        return {"control": label, "status": "passed", "rejected": True}
    raise AssertionError(f"{label}: invalid input was accepted")


def must_accept(label, action):
    value = action()
    return {"control": label, "status": "passed", "rejected": False,
            "result": value}


def _result(exit_code=0, stdout=b"", stderr=b"", timed_out=False,
            cleanup=True, abandoned=False):
    return {"exit": exit_code, "stdout": stdout, "stderr": stderr,
            "timed_out": timed_out, "cleanup_complete": cleanup,
            "abandoned_pipes": abandoned}


def _fake_check_engine(label, emitted, directory, source, cases):
    return {"payload": {"entry_symbol": 1}, "nir": {}, "facts": {},
            "nir_text": "fake native ir\n"}


def _fake_process(scenario, calls):
    def process(argv, cwd, env, timeout):
        calls.append(list(argv))
        argv = [str(part) for part in argv]
        names = [Path(part).name for part in argv]
        if "--out-llvm-before" in argv:
            before = Path(argv[argv.index("--out-llvm-before") + 1])
            native = Path(argv[argv.index("--out-native") + 1])
            before.parent.mkdir(parents=True, exist_ok=True)
            before.write_text("; fake before llvm\n")
            native.write_text("native-scalars compiler-dev/1 native64\n")
            return _result(stdout=json.dumps(
                {"status": "passed", "entry_symbol": 1}).encode())
        if len(argv) > 1 and argv[1] == "build":
            if scenario == "build_fail":
                return _result(1, stderr=b"fake build failed\n")
            if scenario == "launch_fail":
                raise OSError("fake helper is not executable")
            out_dir = Path(argv[argv.index("--out-dir") + 1])
            program = out_dir / "program"
            program.parent.mkdir(parents=True, exist_ok=True)
            program.write_bytes(b"fake program\n")
            return _result(stdout=b'{"status":"ok"}')
        if "readelf" in names[0]:
            if scenario == "readelf_fail":
                return _result(1, stderr=b"fake readelf failed\n")
            return _result(stdout=b"")
        if "clang" in names[0]:
            if scenario == "link_fail":
                return _result(1, stderr=b"fake link failed\n")
            return _result(stdout=b"")
        # The remaining command is the built program or an observer binary.
        if scenario == "timeout_partial":
            return _result(0, stdout=b"\xffpartial\x00", stderr=b"\xff",
                           timed_out=True)
        if scenario == "bool_false":
            return _result(False)
        return _result(0)
    return process


def _main_control(label, scenario, tool_aliases=False):
    work = Path(tempfile.mkdtemp(prefix="gc-critical-main-"))
    out = work / "evidence"
    calls = []
    args = [
        "run.py", "--helper", str(work / "helper"),
        "--wz-nativeobs", str(work / "nativeobs"),
        "--cli", str(work / "woozyc"), "--runtime", str(work / "runtime.a"),
        "--clang", str(work / "clang"), "--readelf", str(work / "readelf"),
        "--root", str(runner.ROOT), "--out", str(out), "--opt", "0",
    ]
    if tool_aliases:
        for alias, target in (("clang++", "clang-22"),
                              ("llvm-readelf", "llvm-readobj")):
            (work / target).write_bytes(b"staged alias target\n")
            (work / alias).symlink_to(target)
        args[args.index("--clang") + 1] = str(work / "clang++")
        args[args.index("--readelf") + 1] = str(work / "llvm-readelf")
    stderr = io.StringIO()
    with patch.object(sys, "argv", args), \
            patch.object(runner, "check_engine", _fake_check_engine), \
            patch.object(runner, "_process_impl",
                         _fake_process(scenario, calls)), \
            contextlib.redirect_stderr(stderr):
        rc = runner.main()
    commands = json.loads((out / "commands.json").read_text())
    summary = json.loads((out / "summary.json").read_text())
    traces = [row["trace"] for row in commands]
    assert rc == 1, (label, rc, stderr.getvalue())
    assert summary["status"] == "failed", (label, summary)
    assert "GC critical emission test failed" in stderr.getvalue(), label
    detail = {"rc": rc, "commands": traces, "external_launch_attempts": []}
    if scenario == "build_fail":
        assert "build-O0" in traces, traces
    elif scenario == "launch_fail":
        row = next(row for row in commands if row["trace"] == "build-O0")
        assert row["launch_failed"] is True and row["launch_error"], row
    elif scenario == "link_fail":
        assert "cpp-O0-link" in traces, traces
    elif scenario == "readelf_fail":
        assert "readelf-program-O0" in traces, traces
    elif scenario == "bool_false":
        row = next(row for row in commands if row["trace"] == "program-O0")
        assert row["exit"] is False and type(row["exit"]) is bool, row
    elif scenario == "timeout_partial":
        row = next(row for row in commands if row["trace"] == "program-O0")
        assert row["timed_out"] is True, row
        assert row["stdout"]["hex"] == "ff7061727469616c00", row
        assert row["stderr"]["hex"] == "ff", row
    if tool_aliases:
        assert any(Path(cmd[0]).name == "clang++" for cmd in calls)
        assert any(Path(cmd[0]).name == "llvm-readelf" for cmd in calls)
    return {"control": label, "status": "passed", "rejected": True,
            "detail": detail}


def _existing_out_control(label):
    work = Path(tempfile.mkdtemp(prefix="gc-critical-existing-"))
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

    args = [
        "run.py", "--helper", str(work / "helper"),
        "--wz-nativeobs", str(work / "nativeobs"),
        "--cli", str(work / "woozyc"), "--runtime", str(work / "runtime.a"),
        "--clang", str(work / "clang"), "--readelf", str(work / "readelf"),
        "--root", str(runner.ROOT), "--out", str(out), "--opt", "0",
    ]
    stderr = io.StringIO()
    with patch.object(sys, "argv", args), \
            patch.object(runner, "_process_impl", refused), \
            contextlib.redirect_stderr(stderr):
        rc = runner.main()
    assert rc == 2, (rc, stderr.getvalue())
    assert not calls, calls
    assert old.read_bytes() == b"previous evidence must remain"
    assert old_summary.read_text() == '{"old":"summary"}\n'
    assert old_commands.read_text() == '{"old":"commands"}\n'
    assert "left unchanged" in stderr.getvalue()
    return {"control": label, "status": "passed", "rejected": True,
            "detail": {"rc": rc, "external_launch_attempts": []}}


def main() -> int:
    controls = []
    prefix_ok = [
        "  %v = call ptr @lang_vec_new_v1(i64 1, i64 8, ptr null, ptr null)",
        "  %datah = getelementptr inbounds %LangVec, ptr %v, i32 0, i32 5",
        "  %data = load ptr, ptr %datah",
        "  %target = getelementptr i8, ptr %data, i64 0",
        "  call void @llvm.memcpy.p0.p0.i64(ptr %target, ptr %source, i64 8, i1 false)",
        "  %init = getelementptr inbounds %LangVec, ptr %v, i32 0, i32 2",
        "  store i64 1, ptr %init",
        "  %r = call i1 @lang_gc_program_register_v1(ptr %v, ptr @ops)",
    ]
    prefix_spec = {"registered": True, "elements": 1}
    prefix_function = nir_function(7, "vec_init", [0])
    controls.append(must_accept("prefix-positive", lambda: runner.assert_prefix_case(
        "prefix-positive", prefix_spec, prefix_function, prefix_ok)))

    prefix_extra = list(prefix_ok)
    prefix_extra.insert(5, "  %x = call i32 @lang_gc_program_enter_critical_v1()")
    prefix_extra.insert(6, "  call void @lang_gc_program_leave_critical_v1(i32 %x)")
    controls.append(must_reject("prefix-extra-critical", lambda: runner.assert_prefix_case(
        "prefix-extra-critical", prefix_spec, prefix_function, prefix_extra)))

    prefix_early = [
        "  %v = call ptr @lang_vec_new_v1(i64 1, i64 8, ptr null, ptr null)",
        "  %r = call i1 @lang_gc_program_register_v1(ptr %v, ptr @ops)",
        "  %datah = getelementptr inbounds %LangVec, ptr %v, i32 0, i32 5",
        "  %data = load ptr, ptr %datah",
        "  %target = getelementptr i8, ptr %data, i64 0",
        "  call void @llvm.memcpy.p0.p0.i64(ptr %target, ptr %source, i64 8, i1 false)",
        "  %init = getelementptr inbounds %LangVec, ptr %v, i32 0, i32 2",
        "  store i64 1, ptr %init",
    ]
    controls.append(must_reject("prefix-register-early", lambda: runner.assert_prefix_case(
        "prefix-register-early", prefix_spec, prefix_function, prefix_early)))

    prefix_missing = prefix_ok[:-1]
    controls.append(must_reject("prefix-missing-register", lambda: runner.assert_prefix_case(
        "prefix-missing-register", prefix_spec, prefix_function, prefix_missing)))

    prefix_extra_call = list(prefix_ok)
    prefix_extra_call.insert(2, "  call void @lang_vec_init_v1(ptr %v, ptr %source)")
    controls.append(must_reject("prefix-extra-call", lambda: runner.assert_prefix_case(
        "prefix-extra-call", prefix_spec, prefix_function, prefix_extra_call)))

    nonregistered_spec = {"registered": False, "elements": 1}
    nonregistered_ok = [
        "  %v = call ptr @lang_vec_new_v1(i64 1, i64 8, ptr null, ptr null)",
        "  %datah = getelementptr inbounds %LangVec, ptr %v, i32 0, i32 5",
        "  %data = load ptr, ptr %datah",
        "  %target = getelementptr i8, ptr %data, i64 0",
        "  call void @llvm.memcpy.p0.p0.i64(ptr %target, ptr %source, i64 8, i1 false)",
        "  %init = getelementptr inbounds %LangVec, ptr %v, i32 0, i32 2",
        "  store i64 1, ptr %init",
        "  %g = call i32 @lang_gc_program_enter_critical_v1()",
        "  store ptr %v, ptr %slot",
        "  call void @lang_gc_program_leave_critical_v1(i32 %g)",
        "  store i1 true, ptr %flag",
    ]
    controls.append(must_accept("prefix-nonregistered-following-slot-guard",
                                lambda: runner.assert_prefix_case(
                                    "prefix-nonregistered-following-slot-guard",
                                    nonregistered_spec, prefix_function,
                                    nonregistered_ok)))
    empty_nonregistered = [
        "  %v = call ptr @lang_vec_new_v1(i64 0, i64 8, ptr null, ptr null)",
        "  %g = call i32 @lang_gc_program_enter_critical_v1()",
        "  call void @lang_gc_program_leave_critical_v1(i32 %g)",
    ]
    controls.append(must_reject("prefix-empty-nonregistered-extra-critical",
                                lambda: runner.assert_prefix_case(
                                    "prefix-empty-nonregistered-extra-critical",
                                    {"registered": False, "elements": 0},
                                    nir_function(7, "vec_init", []),
                                    empty_nonregistered), "critical call inside"))
    wrong_register = [
        "  %v = call ptr @lang_vec_new_v1(i64 0, i64 8, ptr null, ptr null)",
        "  %r = call i1 @lang_gc_program_register_v1(ptr %other, ptr @ops)",
    ]
    controls.append(must_reject("prefix-register-wrong-object",
                                lambda: runner.assert_prefix_case(
                                    "prefix-register-wrong-object",
                                    {"registered": True, "elements": 0},
                                    nir_function(7, "vec_init", []),
                                    wrong_register), "registration of %v is missing"))

    store_strong = [
        "  %g = call i32 @lang_gc_program_enter_critical_v1()",
        "  store ptr %value, ptr %slot",
        "  call void @lang_gc_program_leave_critical_v1(i32 %g)",
        "  store i1 true, ptr %flag",
        "  call void @lang.drop.7(ptr %slot)",
    ]
    store_function = nir_function(8, "store", [0])
    controls.append(must_accept("store-strong-positive", lambda: runner.assert_store_case(
        "store-strong-positive", {"guards": 1, "min_drops": 1},
        store_function, store_strong)))
    controls.append(must_reject("store-missing-strong-guard", lambda: runner.assert_store_case(
        "store-missing-strong-guard", {"guards": 1, "min_drops": 1},
        store_function, ["  store ptr %value, ptr %slot",
                         "  call void @lang.drop.7(ptr %slot)"])))
    controls.append(must_reject("store-missing-nonedge-drop", lambda: runner.assert_store_case(
        "store-missing-nonedge-drop", {"guards": 0, "min_drops": 1},
        store_function, ["  store ptr %value, ptr %slot"])))
    controls.append(must_reject("store-drop-inside-guard", lambda: runner.assert_store_case(
        "store-drop-inside-guard", {"guards": 1, "min_drops": 1},
        store_function, [
            "  %g = call i32 @lang_gc_program_enter_critical_v1()",
            "  store ptr %value, ptr %slot",
            "  call void @lang.drop.7(ptr %slot)",
            "  call void @lang_gc_program_leave_critical_v1(i32 %g)",
        ])))
    controls.append(must_reject("store-publish-outside-guard",
                                lambda: runner.assert_store_case(
                                    "store-publish-outside-guard",
                                    {"guards": 1, "min_drops": 1},
                                    store_function,
                                    ["  store ptr %value, ptr %slot",
                                     "  %g = call i32 @lang_gc_program_enter_critical_v1()",
                                     "  store i1 true, ptr %flag",
                                     "  call void @lang_gc_program_leave_critical_v1(i32 %g)",
                                     "  call void @lang.drop.7(ptr %slot)"])))

    replace_strong = [
        "  %old = load ptr, ptr %slot",
        "  store ptr %old, ptr %scratch",
        "  %g = call i32 @lang_gc_program_enter_critical_v1()",
        "  store ptr %new, ptr %slot",
        "  call void @lang_gc_program_leave_critical_v1(i32 %g)",
        "  call void @lang.drop.7(ptr %scratch)",
    ]
    replace_function = nir_function(9, "replace_indirect", [0, 1])
    controls.append(must_accept("replace-strong-positive", lambda: runner.assert_replace_case(
        "replace-strong-positive", {"guards": 1, "drops": 1},
        replace_function, replace_strong)))
    controls.append(must_reject("replace-missing-drop", lambda: runner.assert_replace_case(
        "replace-missing-drop", {"guards": 1, "drops": 1},
        replace_function, [
            "  %old = load ptr, ptr %slot",
            "  store ptr %old, ptr %scratch",
            "  %g = call i32 @lang_gc_program_enter_critical_v1()",
            "  store ptr %new, ptr %slot",
            "  call void @lang_gc_program_leave_critical_v1(i32 %g)",
        ])))
    controls.append(must_reject("replace-drop-inside-guard", lambda: runner.assert_replace_case(
        "replace-drop-inside-guard", {"guards": 1, "drops": 1},
        replace_function, [
            "  %old = load ptr, ptr %slot",
            "  store ptr %old, ptr %scratch",
            "  %g = call i32 @lang_gc_program_enter_critical_v1()",
            "  store ptr %new, ptr %slot",
            "  call void @lang.drop.7(ptr %scratch)",
            "  call void @lang_gc_program_leave_critical_v1(i32 %g)",
        ])))
    controls.append(must_accept("replace-nonstrong-positive", lambda: runner.assert_replace_case(
        "replace-nonstrong-positive", {"guards": 0, "drops": 1},
        replace_function, [
            "  %old = load ptr, ptr %slot",
            "  store ptr %old, ptr %scratch",
            "  store ptr %new, ptr %slot",
            "  call void @lang.drop.7(ptr %scratch)",
        ])))
    controls.append(must_reject("replace-drop-new-owner-instead-of-old",
                                lambda: runner.assert_replace_case(
                                    "replace-drop-new-owner-instead-of-old",
                                    {"guards": 1, "drops": 1},
                                    replace_function, [
                                        "  %old = load ptr, ptr %slot",
                                        "  store ptr %old, ptr %scratch",
                                        "  %g = call i32 @lang_gc_program_enter_critical_v1()",
                                        "  store ptr %new, ptr %slot",
                                        "  call void @lang_gc_program_leave_critical_v1(i32 %g)",
                                        "  call void @lang.drop.7(ptr %slot)",
                                    ])))
    controls.append(must_reject("replace-nonstrong-no-old-load-or-save",
                                lambda: runner.assert_replace_case(
                                    "replace-nonstrong-no-old-load-or-save",
                                    {"guards": 0, "drops": 1},
                                    replace_function, [
                                        "  store ptr %new, ptr %slot",
                                        "  call void @lang.drop.7(ptr %scratch)",
                                    ])))

    indirect_function = nir_function(10, "store_indirect", [0, 1])
    controls.append(must_accept("store-indirect-positive",
                                lambda: runner.assert_store_indirect_case(
                                    "store-indirect-positive", {"guards": 0},
                                    indirect_function,
                                    ["  store i32 7, ptr %slot"])))
    controls.append(must_reject("store-indirect-guard", lambda: runner.assert_store_indirect_case(
        "store-indirect-guard", {"guards": 0}, indirect_function,
        ["  %g = call i32 @lang_gc_program_enter_critical_v1()",
         "  store i32 7, ptr %slot",
         "  call void @lang_gc_program_leave_critical_v1(i32 %g)"])))

    controls.append(must_reject("zero-target", lambda: runner.map_source_target(
        {"functions": {1: nir_function(1, "vec_init", [])}}, 1, 0,
        {"missing": (0, 10)}, "missing")))
    controls.append(must_reject("empty-native-ir", lambda: runner.parse_nir("")))
    controls.append(must_reject("empty-llvm-ir", lambda: runner.parse_llvm("")))

    def fixture_source_control():
        source = (HERE / "fixtures/gc_critical/src/main.wz").read_bytes()
        ranges = runner.scan_source_functions(source)
        cases = json.loads((HERE / "fixtures/gc_critical/cases.json").read_text())
        expected = {"main"}
        for entries in cases.values():
            expected.update(entries)
        missing = sorted(expected - set(ranges))
        assert not missing, missing
        return {"source_functions": sorted(ranges),
                "case_functions": sorted(expected)}

    controls.append(must_accept("fixture-source-metadata",
                                fixture_source_control))

    def source_span_mapping_control():
        source = (HERE / "fixtures/gc_critical/src/main.wz").read_bytes()
        ranges = runner.scan_source_functions(source)

        def mapped(symbol, name):
            begin = ranges[name][0]
            return {"symbol": symbol, "signature": 0, "slots": [],
                    "ops": [{"op": "store", "type": 0, "inputs": [0],
                             "span": (0, begin, begin + 1), "slot": 0,
                             "result": None}]}

        nir = {"functions": {1: mapped(1, "main"),
                             2: mapped(2, "store_string")}}
        assert runner.fixture_snapshot(nir, 1) == 0
        target = runner.map_source_target(nir, 1, 0, ranges, "store_string")
        assert target["symbol"] == 2, target
        return {"entry": 1, "target": target["symbol"]}

    controls.append(must_accept("source-span-mapping-positive",
                                source_span_mapping_control))

    completed = {"exit": 0, "timed_out": False,
                 "cleanup_complete": True, "abandoned_pipes": False}
    controls.append(must_accept("execution-completed",
                                lambda: runner.assert_execution_completed(
                                    completed, "execution-completed")))
    for label, row in (("skipped", {"skipped": True}),
                       ("not-run", {"not_run": True}),
                       ("timeout", {"exit": 0, "timed_out": True,
                                    "cleanup_complete": True,
                                    "abandoned_pipes": False}),
                       ("missing-exit", {"timed_out": False,
                                         "cleanup_complete": True,
                                         "abandoned_pipes": False}),
                       ("boolean-exit", {"exit": False, "timed_out": False,
                                         "cleanup_complete": True,
                                         "abandoned_pipes": False}),
                       ("non-zero", {"exit": 1, "timed_out": False,
                                     "cleanup_complete": True,
                                     "abandoned_pipes": False})):
        controls.append(must_reject(
            f"execution-{label}",
            lambda row=row, label=label: runner.assert_execution_completed(
                row, f"execution-{label}")))

    def timeout_partial_control():
        with patch.object(runner, "_process_impl",
                          lambda argv, cwd, env, timeout: _result(
                              0, stdout=b"\xffpartial\x00", stderr=b"\xff",
                              timed_out=True)):
            row = runner.run(["not-executed"], 1, {})
        text = json.dumps(row)
        assert row["timed_out"] is True
        assert row["stdout"]["hex"] == "ff7061727469616c00"
        assert row["stderr"]["hex"] == "ff"
        assert "ff7061727469616c00" in text
        return {"control": "timeout-partial-bytes", "status": "passed",
                "rejected": True, "serialized": True,
                "stdout_hex": row["stdout"]["hex"]}

    controls.append(must_accept("timeout-partial-bytes",
                                timeout_partial_control))

    def launch_failure_control():
        with patch.object(runner, "_process_impl",
                          side_effect=OSError("missing helper")):
            row = runner.run(["missing"], 1, {})
        assert row["launch_failed"] is True and row["launch_error"]
        controls_assert = must_reject(
            "launch-failure", lambda: runner.assert_execution_completed(
                row, "launch-failure"))
        return controls_assert

    controls.append(must_accept("launch-failure-record", launch_failure_control))

    controls.append(_main_control("main-bool-false", "bool_false"))
    controls.append(_main_control("main-timeout-partial", "timeout_partial"))
    controls.append(_main_control("main-build-failure", "build_fail"))
    controls.append(_main_control("main-link-failure", "link_fail"))
    controls.append(_main_control("main-preserves-LLVM-tool-aliases",
                                  "link_fail", tool_aliases=True))
    controls.append(_main_control("main-launch-failure", "launch_fail"))
    controls.append(_main_control("main-readelf-failure-recorded",
                                  "readelf_fail"))
    controls.append(_existing_out_control("main-existing-out-refused"))

    # Exercise the actual printer layouts, without llvm_slot_names shortcuts.
    def printer_control(engine):
        parameter = "%p0" if engine == "wz" else "%0"
        quoted = '"' if engine == "wz" else ''
        header = f"define void @{quoted}lang.fn.9{quoted}(ptr {parameter}) {{"
        body = [header, "entry:", "  %scratch = alloca ptr, align 8",
                f"  %old = load ptr, ptr {parameter}",
                "  store ptr %old, ptr %scratch",
                f"  %g = call i32 @{quoted}lang_gc_program_enter_critical_v1{quoted}()",
                f"  store ptr %new, ptr {parameter}",
                f"  call void @{quoted}lang_gc_program_leave_critical_v1{quoted}(i32 %g)",
                f"  call void @{quoted}lang.drop.7{quoted}(ptr %scratch)",
                "  ret void", "}"]
        lines = runner.parse_llvm("\n".join(body))["lang.fn.9"]
        function = runner.parse_nir(
            "native-scalars 1\nfunction @9 type=0\n"
            " slot $0 type=0 parameter=0 inout=1\n"
            "  %0 = address type=0 $0\n"
            "  replace_indirect type=0 %0 %1\n")["functions"][9]
        function["llvm_emitter"] = engine
        result = runner.assert_replace_case(
            engine, {"guards": 1, "drops": 1}, function, lines)
        assert result["target"] == parameter
        damaged = [line.replace(f"ptr {parameter}", "ptr %wrong")
                   if "store ptr %new" in line else line for line in lines]
        must_reject(engine + "-wrong-address", lambda: runner.assert_replace_case(
            engine, {"guards": 1, "drops": 1}, function, damaged))
        return result

    for engine in ("cpp", "wz"):
        controls.append(must_accept(engine + "-real-parameter-and-global-printer",
                                    lambda engine=engine: printer_control(engine)))

        def slot_layout_control(engine=engine):
            function = runner.parse_nir(
                "native-scalars 1\nfunction @8 type=0\n"
                " slot $0 type=0\n  store type=0 $0 %0\n")["functions"][8]
            function["llvm_emitter"] = engine
            allocations = ["  %slot = alloca ptr", "  %scratch = alloca ptr"]
            if engine == "cpp":
                allocations.reverse()
            lines = ["define void @lang.fn.8() {", "entry:", *allocations,
                     *store_strong, "  ret void"]
            return runner.assert_store_case(
                engine, {"guards": 1, "min_drops": 1}, function, lines)

        controls.append(must_accept(engine + "-real-slot-scratch-order",
                                    slot_layout_control))

        def check_engine_control(engine=engine):
            with tempfile.TemporaryDirectory(prefix="gc-critical-label-") as temp:
                directory = Path(temp)
                source = b"func main() i32 { return 0; }\nfunc target() i32 { let value Vec[i32] = []; return 0; }\n"
                manifest_source = directory / "main.wz"
                manifest_source.write_bytes(source)
                start, end = runner.scan_source_functions(source)["target"]
                binding_start = source.index(b"let value", start)
                binding_end = source.index(b";", binding_start)
                rhs_start = source.index(b"[]", binding_start)
                (directory / "program.nir").write_text(
                    "native-scalars 1\nfunction @1 type=0\n"
                    "  %0 = const_int type=0 source=0:0-1\n"
                    "function @8 type=0\n slot $0 type=0\n"
                    f"  %0 = vec_init type=0 source=0:{rhs_start}-{rhs_start + 2}\n"
                    f"  store type=0 $0 %0 source=0:{binding_start}-{binding_end}\n"
                    f"  ownership_begin type=0 origin=1 source=0:{binding_start}-{binding_end}\n")
                allocations = ["  %slot = alloca ptr", "  %scratch = alloca ptr"]
                if engine == "cpp":
                    allocations.reverse()
                text = "\n".join(["define void @lang.fn.8() {", "entry:",
                                   *allocations, *store_strong, "  ret void", "}"])
                if engine == "wz":
                    import re
                    text = re.sub(r"@([A-Za-z0-9_.$]+)", r'@"\1"', text)
                (directory / "before.ll").write_text(text)
                emitted = {"stdout": {"text": '{"status":"passed","entry_symbol":1}'}}
                result = runner.check_engine(
                    f"{engine} O0", emitted, directory, manifest_source,
                    {"store": {"target": {"guards": 1, "min_drops": 1}}})
                assert result["facts"]["target"]["target"] == "%slot"
                return {"label": f"{engine} O0", "target": "%slot"}

        controls.append(must_accept(engine + "-actual-check-engine-caller",
                                    check_engine_control))

    def aligned_scratch_prefix_control():
        function = runner.parse_nir(
            "native-scalars 1\nfunction @7 type=0\n"
            " slot $0 type=0\n  %0 = vec_init type=0 %1\n"
        )["functions"][7]
        function["llvm_emitter"] = "cpp"
        lines = ["define void @lang.fn.7() {", "entry:",
                 "  %scratch = alloca ptr", "  %slot = alloca ptr",
                 "  %v = call ptr @lang_vec_new_v1(i64 1, i64 8, ptr null, ptr null)",
                 "  %h5 = getelementptr %Vec, ptr %v, i32 0, i32 5",
                 "  %data = load ptr, ptr %h5",
                 "  %target = getelementptr i8, ptr %data, i64 0",
                 "  store ptr %text, ptr %scratch",
                 "  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %target, ptr align 1 %scratch, i64 8, i1 false)",
                 "  %h2 = getelementptr %Vec, ptr %v, i32 0, i32 2",
                 "  store i64 1, ptr %h2",
                 "  %g = call i32 @lang_gc_program_enter_critical_v1()",
                 "  store ptr %v, ptr %slot",
                 "  call void @lang_gc_program_leave_critical_v1(i32 %g)",
                 "  ret void"]
        result = runner.assert_prefix_case(
            "scratch-prefix", {"registered": False, "elements": 1},
            function, lines)
        damaged = [line.replace("%target, ptr align", "%wrong, ptr align")
                   for line in lines]
        must_reject("scratch-prefix-wrong-copy", lambda: runner.assert_prefix_case(
            "scratch-prefix", {"registered": False, "elements": 1},
            function, damaged))
        return result

    controls.append(must_accept("real-aligned-memcpy-and-stack-scratch-prefix",
                                aligned_scratch_prefix_control))

    def actual_cpp_literal_struct_prefix_control():
        header = "{ i64, i64, i64, i64, i64, ptr, ptr, ptr, i64, ptr }"
        lines = [
            "  %16 = call ptr @lang_vec_new_v1(i64 1, i64 24, ptr @lang.copy.13, ptr @lang.drop.13)",
            "  %17 = getelementptr inbounds nuw " + header + ", ptr %16, i32 0, i32 5",
            "  %18 = load ptr, ptr %17, align 8, !tbaa !0",
            "  store %value.13 %14, ptr %2, align 8",
            "  %19 = getelementptr i8, ptr %18, i64 0",
            "  call void @llvm.memcpy.p0.p0.i64(ptr align 1 %19, ptr align 1 %2, i64 24, i1 false), !tbaa !3",
            "  %20 = getelementptr inbounds nuw " + header + ", ptr %16, i32 0, i32 2",
            "  store i64 1, ptr %20, align 8, !tbaa !0",
            "  %21 = call i1 @lang_gc_program_register_v1(ptr %16, ptr @lang.gc.ops.v13)",
        ]
        function = nir_function(7, "vec_init", [0])
        spec = {"registered": True, "elements": 1}
        result = runner.assert_prefix_case("literal-struct", spec, function, lines)
        wrong_object = [s.replace(", ptr %16, i32", ", ptr %other, i32") for s in lines]
        must_reject("literal-struct-wrong-object", lambda: runner.assert_prefix_case(
            "literal-struct-wrong-object", spec, function, wrong_object))
        wrong_field = [s.replace("i32 0, i32 2", "i32 0, i32 3") for s in lines]
        must_reject("literal-struct-wrong-field", lambda: runner.assert_prefix_case(
            "literal-struct-wrong-field", spec, function, wrong_field))
        return {"facts": result, "wrong_object_rejected": True, "wrong_field_rejected": True}

    controls.append(must_accept("actual-cpp-literal-struct-inbounds-nuw-prefix",
                                actual_cpp_literal_struct_prefix_control))

    # Real binding shape: an RHS temporary is published and taken before the
    # declaration's separate store/lifetime begins.  Neither store is selected
    # by ordinal, and each critical pair must belong to a known publication.
    declaration = b"let value Vec[i32] = [1]"
    rhs_begin = declaration.index(b"[1]")
    rhs_span = (2, rhs_begin, len(declaration))
    binding_span = (2, 0, len(declaration))
    binding_function = {
        "symbol": 80, "signature": 0,
        "slots": [{"id": i, "type": 10, "parameter": None, "inout": False}
                  for i in (0, 1)],
        "ops": [
            {"op": "vec_init", "type": 10, "inputs": [1],
             "span": rhs_span, "result": 4},
            {"op": "store", "type": 1, "inputs": [4], "slot": 0,
             "span": rhs_span, "result": None},
            {"op": "take", "type": 1, "inputs": [],
             "span": rhs_span, "result": None},
            {"op": "store", "type": 1, "inputs": [4], "slot": 1,
             "span": binding_span, "result": None},
            {"op": "ownership_begin", "type": 1, "inputs": [],
             "span": binding_span, "result": None},
        ],
        "llvm_slot_names": {0: "%temp", 1: "%slot"},
        "source_bytes": declaration, "source_snapshot": 2,
        "type_guards": {10: 1},
    }
    binding_lines = [
        "  %g0 = call i32 @lang_gc_program_enter_critical_v1()",
        "  store ptr %fresh, ptr %temp",
        "  call void @lang_gc_program_leave_critical_v1(i32 %g0)",
        "  %g1 = call i32 @lang_gc_program_enter_critical_v1()",
        "  store ptr %fresh, ptr %slot",
        "  call void @lang_gc_program_leave_critical_v1(i32 %g1)",
        "  call void @lang.drop.10(ptr %slot)",
    ]
    binding_spec = {"guards": 1, "min_drops": 1}
    controls.append(must_accept("temporary-and-user-binding-positive",
                                lambda: runner.assert_store_case(
                                    "binding", binding_spec,
                                    copy.deepcopy(binding_function), binding_lines)))
    for label, mutate in (
            ("binding-wrong-lifetime-span", lambda f: f["ops"][-1].update(
                span=(2, 1, len(declaration)))),
            ("binding-missing-lifetime", lambda f: f["ops"].pop()),
            ("temporary-missing-take", lambda f: f["ops"][2].update(
                op="ownership_end")),
            ("temporary-wrong-span", lambda f: f["ops"][1].update(
                span=(2, rhs_begin + 1, len(declaration)))),
            ("temporary-wrong-producer-type", lambda f: f["ops"][0].update(
                type=99)),
            ("temporary-unknown-type-expectation", lambda f: f.update(
                type_guards={})),
    ):
        damaged = copy.deepcopy(binding_function)
        mutate(damaged)
        controls.append(must_reject(label, lambda f=damaged: runner.assert_store_case(
            "binding", binding_spec, f, binding_lines)))
    for label, damaged in (
            ("temporary-wrong-LLVM-target",
             [line.replace("ptr %temp", "ptr %other") for line in binding_lines]),
            ("temporary-missing-guard", binding_lines[1:2] + binding_lines[3:]),
            ("temporary-extra-empty-guard", binding_lines + [
                "  %g2 = call i32 @lang_gc_program_enter_critical_v1()",
                "  call void @lang_gc_program_leave_critical_v1(i32 %g2)"]),
            ("temporary-extra-call-in-guard", binding_lines[:2] + [
                "  call void @unexpected()"] + binding_lines[2:]),
            ("temporary-duplicate-publication", binding_lines[:2] + [
                "  store ptr %fresh, ptr %temp"] + binding_lines[2:]),
            ("temporary-consumed-value-dropped", binding_lines + [
                "  call void @lang.drop.10(ptr %temp)"]),
            ("binding-different-LLVM-value", [line.replace(
                "store ptr %fresh, ptr %slot", "store ptr %wrong, ptr %slot")
                for line in binding_lines]),
    ):
        controls.append(must_reject(label, lambda ll=damaged: runner.assert_store_case(
            "binding", binding_spec, copy.deepcopy(binding_function), ll)))

    assignment = b"value = [1]"
    rhs_span = (2, assignment.index(b"[1]"), len(assignment))
    replace_temp = copy.deepcopy(binding_function)
    replace_temp.update(source_bytes=assignment,
                        llvm_slot_names={0: "%slot", 1: "%temp"})
    replace_temp["slots"][0].update(parameter=0, inout=True)
    replace_temp["ops"] = [
        {"op": "address", "type": 10, "inputs": [], "slot": 0,
         "span": (2, 0, 5), "result": 0},
        {"op": "vec_init", "type": 10, "inputs": [1],
         "span": rhs_span, "result": 4},
        {"op": "store", "type": 1, "inputs": [4], "slot": 1,
         "span": rhs_span, "result": None},
        {"op": "take", "type": 1, "inputs": [],
         "span": rhs_span, "result": None},
        {"op": "replace_indirect", "type": 1, "inputs": [0, 4],
         "span": (2, 0, len(assignment)), "result": None},
    ]
    replace_lines = binding_lines[:3] + [
        "  %old = load ptr, ptr %slot",
        "  store ptr %old, ptr %scratch",
    ] + binding_lines[3:6] + ["  call void @lang.drop.10(ptr %scratch)"]
    controls.append(must_accept("replace-with-constructor-temporary-positive",
                                lambda: runner.assert_replace_case(
                                    "replace-temp", {"guards": 1, "drops": 1},
                                    copy.deepcopy(replace_temp), replace_lines)))
    controls.append(must_reject("replace-temporary-cannot-hide-wrong-old-drop",
                                lambda: runner.assert_replace_case(
                                    "replace-temp", {"guards": 1, "drops": 1},
                                    copy.deepcopy(replace_temp),
                                    [line.replace("@lang.drop.10(ptr %scratch)",
                                                  "@lang.drop.10(ptr %slot)")
                                     for line in replace_lines])))

    report = {
        "schema": "woozy/gc-critical-emission/pure-controls/1",
        "status": "passed",
        "controls": controls,
        "external_launch_attempts": ATTEMPTS,
    }
    assert not ATTEMPTS, ATTEMPTS
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
