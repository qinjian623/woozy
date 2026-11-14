#!/usr/bin/env python3
"""Pure controls for tests/native/c_ffi/run_c_ffi.py.

All process-spawn, exec, fork and socket entry points are blocked *before*
the runner under test is imported.  The checks then call the runner's real
strict process/JSON/ELF/forged/Source-identity/output/persistence helpers with
in-memory or temporary files.  No compiler, C fixture, language program,
component, tool version or network operation is started.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

EXTERNAL_LAUNCH_ATTEMPTS: list[str] = []
STRACE_PATH = Path("/not-executed-strace")


def forbid_external_launch(*args, **kwargs):
    command = args[0] if args else kwargs.get("args", "?")
    EXTERNAL_LAUNCH_ATTEMPTS.append(str(command))
    raise AssertionError("external launch forbidden in pure controls")


def expect_raises(call, label: str) -> None:
    try:
        call()
    except Exception:
        return
    raise AssertionError(f"{label}: expected a rejection")


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def base_record(**overrides) -> dict:
    record = {
        "label": "control",
        "exit": 0,
        "timed_out": False,
        "cleanup_complete": True,
        "abandoned_pipes": False,
        "launch_error": None,
        "stdout": "",
        "stderr": "",
        "timeout_seconds": 60.0,
        "trace": {"status": "not_required"},
    }
    record.update(overrides)
    return record


def good_forged_stdout() -> str:
    cases = [
        f"CASE {name} rejected=E_IR_INVALID baseline=ok"
        for name in (
            "hir_c_callable", "hir_signature_type_id", "hir_arg_indices",
            "hir_c_field", "native_c_callable", "native_origin_foreign_call",
            "native_c_field", "native_origin_c_ptr_write")
    ]
    return "\n".join([
        "COVERAGE HIR foreign_calls=3 c_fields=1",
        "BASELINE HIR legal",
        *cases[:4],
        "COVERAGE NativeIR foreign_calls=3 c_fields=1 c_ptr_writes=1",
        "BASELINE NativeIR legal",
        *cases[4:],
        "TOTAL 8passed 0failed",
    ]) + "\n"


def check_strict_process(runner) -> None:
    runner.require_ok(base_record())
    if not runner.bounds_passed(
            base_record(exit=101,
                        stderr="LANG_NATIVE_PANIC:v1:vec-index:2\n")):
        raise AssertionError("valid original Panic marker was rejected")
    if runner.bounds_passed(base_record(exit=0)):
        raise AssertionError("bounds exit 0 was accepted")
    if runner.bounds_passed(base_record(exit=-9)):
        raise AssertionError("bounds signal was accepted as a Panic")
    if runner.bounds_passed(
            base_record(exit=101, stderr="LANG_NATIVE_PANIC:v1:other\n")):
        raise AssertionError("bounds wrong Panic marker was accepted")
    for label, record in (
            ("bool exit", base_record(exit=True)),
            ("float exit", base_record(exit=0.0)),
            ("string exit", base_record(exit="0")),
            ("non-bool timed_out", base_record(timed_out=0)),
            ("non-bool cleanup", base_record(cleanup_complete=1)),
            ("non-bool pipes", base_record(abandoned_pipes="false")),
            ("timeout", base_record(timed_out=True)),
            ("signal", base_record(exit=-9)),
            ("launch OSError", base_record(exit=None,
                                           launch_error="injected OSError"))):
        expect_raises(lambda record=record: runner.require_ok(record), label)
    print("strict native-int exit, bool flags, timeout, signal and OSError")


def check_timeout_and_json(runner) -> None:
    if runner.finite_timeout("60") != 60.0:
        raise AssertionError("finite timeout 60 changed")
    for text in ("nan", "inf", "-inf", "0", "-1", "1e1000"):
        expect_raises(lambda text=text: runner.finite_timeout(text),
                      f"timeout {text}")
    for text in ('{"x":NaN}', '{"x":Infinity}', '{"x":-Infinity}',
                 '{"x":1e1000}', '{"x":'):
        expect_raises(
            lambda text=text: runner.parse_json_record(
                base_record(stdout=text)),
            f"bad JSON {text}")
    with tempfile.TemporaryDirectory() as temporary:
        raw = Path(temporary) / "stdout.bin"
        raw.write_bytes(b'{"status":"ok","x":"\xff"}')
        expect_raises(
            lambda: runner.parse_json_record(
                base_record(stdout='{"status":"ok","x":"?"}',
                            stdout_path=str(raw))),
            "bad UTF-8 JSON bytes")
    document = runner.parse_json_record(
        base_record(stdout='{"status":"ok","x":1}'))
    if document.get("status") != "ok":
        raise AssertionError("finite JSON did not parse")
    # Missing status and missing diagnostic code must never become green.
    if runner.negative_check_passed(
            base_record(), {"diagnostics": [{"code": "E_TYPE_MISMATCH"}]},
            "E_TYPE_MISMATCH"):
        raise AssertionError("missing JSON status became a negative pass")
    if runner.negative_check_passed(
            base_record(), {"status": "error", "diagnostics": []},
            "E_TYPE_MISMATCH"):
        raise AssertionError("missing diagnostic code became a negative pass")
    if runner.negative_check_passed(
            base_record(timed_out=True),
            {"status": "error", "diagnostics": [{"code": "E_TYPE_MISMATCH"}]},
            "E_TYPE_MISMATCH"):
        raise AssertionError("timeout became an expected negative rejection")
    print("strict JSON rejects NaN/Infinity/overflow/bad UTF-8/missing status")


def check_build_and_elf(runner) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        program = Path(temporary) / "program"
        program.write_bytes(b"program")
        record = base_record()
        document = {"status": "ok"}
        runner.require_build_success(record, document, program)
        if not runner.build_success(record, document, program):
            raise AssertionError("valid build result was not accepted")
        missing = Path(temporary) / "missing-program"
        if runner.build_success(record, document, missing):
            raise AssertionError("fake success without program was accepted")
        expect_raises(
            lambda: runner.require_build_success(record, document, missing),
            "missing target artifact")
        expect_raises(
            lambda: runner.require_build_success(
                record, {"status": "error"}, program),
            "build JSON error status")
    good_hosted = "INTERP\nlibcrypto\n"
    runner.validate_elf("libc:O2", good_hosted, "hosted-libc")
    runner.validate_elf("scalars:O2", "INTERP\n", "hosted")
    expect_raises(lambda: runner.validate_elf(
        "scalars:O2", "no interp\n", "hosted"), "missing INTERP")
    expect_raises(lambda: runner.validate_elf(
        "scalars:O2", "INTERP\nlibstdc++\n", "hosted"), "libstdc++")
    expect_raises(lambda: runner.validate_elf(
        "libc:O2", "INTERP\n", "hosted-libc"), "missing libcrypto")
    runner.validate_elf("core", "Type: EXEC\n", "core")
    expect_raises(lambda: runner.validate_elf(
        "core", "INTERP\n", "core"), "core INTERP")
    expect_raises(lambda: runner.validate_elf(
        "core", "NEEDED\n", "core"), "core NEEDED")
    print("missing artifact/fake success/ELF INTERP/NEEDED/libcrypto checked")


def check_link_failure(runner) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        program = Path(temporary) / "program"
        record = base_record(exit=1,
                             stdout='{"status":"error"}')
        document = {"status": "error",
                    "diagnostics": [{"code": "E_LINK_FAILED"}]}
        if not runner.link_failure_passed(record, document, program):
            raise AssertionError("valid link failure was rejected")
        program.write_bytes(b"leftover")
        if runner.link_failure_passed(record, document, program):
            raise AssertionError("link failure leaving program was accepted")
        if runner.link_failure_passed(
                base_record(exit=-9), document, program):
            raise AssertionError("signalled link build became a link failure")
        if runner.link_failure_passed(
                base_record(exit=1), {"status": "ok"}, program):
            raise AssertionError("successful JSON became a link failure")
    print("link failure requires nonzero clean exit, E_LINK_FAILED, no program")


def check_archive_symbols(runner) -> None:
    hosted = "lang_hosted_startup_v1\n"
    native = "lang_thread_spawn_v1\n"
    runner.validate_archive_symbols(hosted, native)
    for label, left, right in (
            ("hosted startup", "nothing\n", native),
            ("native start", " _start\nlang_hosted_startup_v1\n", native),
            ("cxa", "lang_hosted_startup_v1\n__cxa_finalize\n", native),
            ("unwind", "lang_hosted_startup_v1\n_Unwind_Resume\n", native),
            ("terminate", "lang_hosted_startup_v1\n_ZSt9terminatev\n", native),
            ("thread spawn", hosted, "nothing\n")):
        expect_raises(
            lambda left=left, right=right: runner.validate_archive_symbols(
                left, right), label)
    print("hosted/native archive symbol whitelist and forbidden symbols")


def check_trace(runner) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        helper_trace = root / "execveat.log"
        helper_trace.write_text(
            '1 execveat(3, "/usr/bin/cpp-helper", ["cpp-helper"], 0x0) = 0\n',
            encoding="utf-8")
        expect_raises(
            lambda: runner.audit_trace(
                helper_trace, {"/candidate/woozyc"}),
            "execveat helper")
        failed_exec = root / "failed-exec.log"
        failed_exec.write_text(
            '1 execve("/usr/bin/cpp-helper", ["cpp-helper"], 0x0) = -1 ENOENT\n',
            encoding="utf-8")
        expect_raises(
            lambda: runner.audit_trace(
                failed_exec, {"/candidate/woozyc"}),
            "failed exec helper")
        allowed_trace = root / "allowed.log"
        allowed_trace.write_text(
            '1 execve("/candidate/woozyc", ["woozyc"], 0x0) = 0\n',
            encoding="utf-8")
        audit = runner.audit_trace(allowed_trace, {"/candidate/woozyc"})
        if audit["status"] != "ok":
            raise AssertionError("allowed compiler trace was rejected")
        # Using a subset of the fixed tool allowlist is valid; unused tools do
        # not have to appear in the trace.
        subset = runner.audit_trace(
            allowed_trace,
            {"/candidate/woozyc", "/toolchain/bin/clang"})
        if subset["status"] != "ok":
            raise AssertionError("allowed tool subset was rejected")
        empty = root / "empty.log"
        empty.write_text(
            '1 execveat(3, "", ["x"], 0x0) = 0\n', encoding="utf-8")
        expect_raises(lambda: runner.audit_trace(empty, set()),
                      "empty execveat path")

        out = root / "out"
        out.mkdir()
        log: list[dict] = []
        original = runner._run_process_impl

        def failing_trace(argv, cwd, env, timeout):
            trace = Path(argv[argv.index("-o") + 1])
            trace.parent.mkdir(parents=True, exist_ok=True)
            trace.write_text(
                '1 execveat(3, "/usr/bin/cpp-helper", ["cpp-helper"], 0x0) = 0\n',
                encoding="utf-8")
            return {
                "exit": -9,
                "timed_out": True,
                "cleanup_complete": False,
                "abandoned_pipes": True,
                "stdout": b"partial stdout\n",
                "stderr": b"partial stderr\n",
            }

        runner._run_process_impl = failing_trace
        try:
            try:
                runner.run_command(
                    ["/candidate/woozyc", "build"], out, 60.0, out, log,
                    "failed-trace", {}, Path("/not-executed-strace"),
                    audit_allowed={"/candidate/woozyc"},
                    meta={"role": "candidate", "category": "build"})
            except RuntimeError as error:
                message = str(error)
            else:
                raise AssertionError("unexpected helper trace was accepted")
        finally:
            runner._run_process_impl = original
        if "unexpected compiler subprocesses" not in message:
            raise AssertionError(f"trace cause was lost: {message}")
        if len(log) != 1:
            raise AssertionError("failed trace lost the process record")
        record = log[0]
        commands = out / "commands.jsonl"
        if not commands.is_file():
            raise AssertionError("commands.jsonl was not written first")
        if json.loads(commands.read_text(encoding="utf-8").splitlines()[0]) != record:
            raise AssertionError("commands.jsonl lost the complete record")
        if type(record["exit"]) is not int or record["exit"] != -9:
            raise AssertionError("native int exit was not retained")
        for key, value in (("timed_out", True), ("cleanup_complete", False),
                           ("abandoned_pipes", True)):
            if record.get(key) is not value:
                raise AssertionError(f"{key} was not retained as strict bool")
        if record["stdout"] != "partial stdout\n":
            raise AssertionError("raw stdout was lost")
        if record["stderr"] != "partial stderr\n":
            raise AssertionError("raw stderr was lost")
        if record["stdout_sha256"] != sha(b"partial stdout\n"):
            raise AssertionError("stdout SHA was lost")
        if record["stderr_sha256"] != sha(b"partial stderr\n"):
            raise AssertionError("stderr SHA was lost")
        if "trace_audit_error" not in record:
            raise AssertionError("trace failure was not recorded")
        if "trace_audit" in record:
            raise AssertionError("rejected trace was marked audited")

        oserror_log: list[dict] = []
        original = runner._run_process_impl

        def raising_process(*args, **kwargs):
            raise OSError("injected spawn failure")

        runner._run_process_impl = raising_process
        try:
            oserror = runner.run_command(
                ["/not-executed"], out, 60.0, out, oserror_log,
                "oserror", {}, None)
        finally:
            runner._run_process_impl = original
        if oserror.get("launch_error") != "injected spawn failure":
            raise AssertionError("OSError state was not recorded")
        if type(oserror.get("exit")) is not type(None):
            raise AssertionError("OSError invented an exit code")
        expect_raises(lambda: runner.require_ok(oserror), "OSError as pass")
    print("execve/execveat whitelist; failed process record kept before failure")


def check_forged(runner) -> None:
    good = good_forged_stdout()
    parsed = runner.parse_forged_output(0, good, "")
    if parsed["status"] != "passed" or parsed["case_count"] != 8:
        raise AssertionError("valid Wz forged 8/8 output was rejected")
    record = base_record(stdout=good)
    if runner.forged_component_passed(record)["status"] != "passed":
        raise AssertionError("valid Wz forged process record was rejected")

    fake_total = "TOTAL 8passed 0failed\n"
    no_cases = "\n".join(
        line for line in good.splitlines() if not line.startswith("CASE ")
    ) + "\n"
    no_cases = no_cases.replace("TOTAL 8passed 0failed",
                                "TOTAL 0passed 8failed")
    duplicate_case = good.replace(
        "CASE native_c_field rejected=E_IR_INVALID baseline=ok",
        "CASE native_c_callable rejected=E_IR_INVALID baseline=ok")
    duplicate_hir = good + "COVERAGE HIR foreign_calls=3 c_fields=1\n"
    duplicate_native = good + (
        "COVERAGE NativeIR foreign_calls=3 c_fields=1 c_ptr_writes=1\n")
    zero_coverage = good.replace(
        "COVERAGE HIR foreign_calls=3 c_fields=1",
        "COVERAGE HIR foreign_calls=0 c_fields=0")
    missing_baseline = good.replace("BASELINE NativeIR legal\n", "")
    missing_target = good.replace(
        "CASE native_c_field rejected=E_IR_INVALID baseline=ok",
        "FAIL native_c_field target_not_found")
    missing_target = missing_target.replace(
        "TOTAL 8passed 0failed", "TOTAL 7passed 1failed")
    old_cpp = "C FFI forged HIR/IR rejected: 8\n"

    def reject(text: str) -> None:
        parsed_bad = runner.parse_forged_output(0, text, "")
        if parsed_bad["status"] != "passed":
            raise AssertionError("forged output rejected as expected")
        # An accepted fake must flow out of expect_raises as a failure.

    for label, text in (
            ("fake TOTAL without cases", fake_total),
            ("0/8 summary", no_cases),
            ("duplicate CASE", duplicate_case),
            ("duplicate HIR coverage", duplicate_hir),
            ("duplicate NativeIR coverage", duplicate_native),
            ("zero coverage", zero_coverage),
            ("missing baseline", missing_baseline),
            ("missing mutation target", missing_target),
            ("old C++ output", old_cpp),
            ("empty output", "")):
        expect_raises(lambda text=text: reject(text), label)
    expect_raises(
        lambda: runner.forged_component_passed(
            base_record(timed_out=True, stdout=good)),
        "forged timeout")
    expect_raises(
        lambda: runner.forged_component_passed(
            base_record(exit=1, stdout=good)),
        "forged nonzero")
    expect_raises(
        lambda: runner.forged_component_passed(
            base_record(stdout=good, stderr="warning\n")),
        "forged stderr")
    with tempfile.TemporaryDirectory() as temporary:
        report = {"runs": [], "not_run": [], "failed": []}
        runner._run_forged_wz(
            report, [], Path(temporary), "candidate", None,
            Path(temporary) / "lang.toml", None, {}, None, True, None)
        if (len(report["runs"]) != 1
                or report["runs"][0]["status"] != "not_run"
                or len(report["not_run"]) != 1):
            raise AssertionError("pending Wz component did not stay not_run")
    print("Wz forged 8 exact cases/baselines/coverage; fake/duplicate rejected")


def check_source_identity(runner) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        source = root / "source"
        (source / "wzcc").mkdir(parents=True)
        (source / "wzcc" / "a.wz").write_text("module a;\n", encoding="utf-8")
        stdlib = root / "stdlib"
        stdlib.mkdir()
        (stdlib / "io.wz").write_text("module io;\n", encoding="utf-8")
        manifest = root / "lang.toml"
        manifest.write_text("[package]\n", encoding="utf-8")
        cli = root / "woozyc"
        cli.write_bytes(b"candidate-cli")
        component = root / "component"
        component.write_bytes(b"wz-component")
        source_snapshot = runner.snapshot_tree(source, ".wz")
        stdlib_snapshot = runner.snapshot_tree(stdlib, ".wz")
        cli_record = {
            "status": "passed",
            "source_map_sha256": source_snapshot["map_sha256"],
            "cli": {"sha256": sha(cli.read_bytes())},
        }
        identity = runner.validate_cli_identity(
            cli_record, cli, source_snapshot)
        if identity["source_map_sha256"] != source_snapshot["map_sha256"]:
            raise AssertionError("CLI identity source map was lost")
        mismatched = dict(cli_record)
        mismatched["source_map_sha256"] = "0" * 64
        expect_raises(
            lambda: runner.validate_cli_identity(
                mismatched, cli, source_snapshot),
            "CLI Source mismatch")
        bad_sha = dict(cli_record)
        bad_sha["cli"] = {"sha256": "0" * 64}
        expect_raises(
            lambda: runner.validate_cli_identity(
                bad_sha, cli, source_snapshot),
            "CLI binary mismatch")

        component_record = {
            "status": "passed",
            "component": {"sha256": sha(component.read_bytes())},
            "source116_map_sha256": source_snapshot["map_sha256"],
            "stdlib": {"map_sha256": stdlib_snapshot["map_sha256"]},
            "forged_project": {
                "manifest": {"sha256": sha(manifest.read_bytes())}},
        }
        runner.validate_component_identity(
            component_record, component, source_snapshot, stdlib_snapshot,
            manifest)
        historical = dict(component_record, historical=True)
        expect_raises(
            lambda: runner.validate_component_identity(
                historical, component, source_snapshot, stdlib_snapshot,
                manifest),
            "historical component")
        wrong_source = dict(component_record,
                            source116_map_sha256="0" * 64)
        expect_raises(
            lambda: runner.validate_component_identity(
                wrong_source, component, source_snapshot, stdlib_snapshot,
                manifest),
            "component Source mismatch")
        wrong_stdlib = dict(component_record,
                            stdlib={"map_sha256": "0" * 64})
        expect_raises(
            lambda: runner.validate_component_identity(
                wrong_stdlib, component, source_snapshot, stdlib_snapshot,
                manifest),
            "component stdlib mismatch")

        before = runner.snapshot_tree(source, ".wz")
        (source / "wzcc" / "a.wz").unlink()
        after = runner.snapshot_tree(source, ".wz")
        expect_raises(
            lambda: runner.ensure_maps_equal(
                "source", before["files"], after["files"]),
            "Source file disappeared during execution")
    print("CLI/component Source+stdlib+manifest identity; historical rejected")


def check_input_set(runner) -> None:
    before = {"a": "1", "b": "2"}
    if runner.map_changes(before, before) != {
            "added": [], "removed": [], "changed": []}:
        raise AssertionError("unchanged input map was reported as changed")
    runner.ensure_maps_equal("unchanged", before, before)
    for label, after, expected in (
            ("added", {"a": "1", "b": "2", "c": "3"}, "added"),
            ("removed", {"a": "1"}, "removed"),
            ("changed", {"a": "1", "b": "9"}, "changed")):
        changes = runner.map_changes(before, after)
        if not changes[expected]:
            raise AssertionError(f"input {label} was not detected")
        expect_raises(
            lambda after=after: runner.ensure_maps_equal(
                label, before, after), label)
    print("input set addition/removal/change rejected")


def check_fresh_output(runner) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary) / "out"
        out.mkdir()
        (out / "sentinel").write_text("old evidence\n", encoding="utf-8")
        expect_raises(lambda: runner.prepare_output(out, strict=True),
                      "non-empty formal output")
        runner.prepare_output(out, strict=False)
        if not (out / "sentinel").is_file():
            raise AssertionError("legacy output refusal removed old evidence")
        first = runner.unique_run_dir(out)
        second = runner.unique_run_dir(out)
        if first == second:
            raise AssertionError("legacy second run reused the same directory")
        if not (out / "sentinel").is_file():
            raise AssertionError("legacy second run removed old evidence")
        fresh = Path(temporary) / "fresh"
        runner.prepare_output(fresh, strict=True)
        if not fresh.is_dir():
            raise AssertionError("fresh formal output was not created")
    print("legacy same-out gets a new run dir; formal non-empty refused")


def check_persist_write_failure(runner) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary)
        previous = {"label": "previous", "exit": 0}
        log = [previous]
        commands = out / "commands.jsonl"
        prefix = json.dumps(previous, sort_keys=True) + "\n"
        commands.write_text(prefix, encoding="utf-8")
        original_open = Path.open

        class FailingWriter:
            def __init__(self, stream):
                self.stream = stream

            def __enter__(self):
                return self

            def __exit__(self, *args):
                self.stream.close()

            def write(self, value):
                raise OSError("injected command record write failure")

        def failing_open(path, mode="r", *args, **kwargs):
            stream = original_open(path, mode, *args, **kwargs)
            if mode == "w" and path.name in (
                    "commands.jsonl", "commands.jsonl.pending"):
                return FailingWriter(stream)
            return stream

        def process_fixture(*args, **kwargs):
            return {"exit": 0, "timed_out": False,
                    "cleanup_complete": True, "abandoned_pipes": False,
                    "stdout": b"new output", "stderr": b""}

        with patch.object(runner, "_run_process_impl", process_fixture), \
                patch.object(Path, "open", failing_open):
            try:
                runner.run_command(["x", "build"], out, 60.0, out, log,
                                   "write-failure", {}, None)
            except OSError as error:
                if "injected" not in str(error):
                    raise
            else:
                raise AssertionError("record write failure was swallowed")
        if commands.read_text(encoding="utf-8") != prefix:
            raise AssertionError("write failure damaged previous records")
        if (out / "logs/001-write-failure.stdout").read_bytes() != b"new output":
            raise AssertionError("write failure lost raw command output")
    print("record write failure keeps old JSON and raw stdout bytes")


def check_after_resource_failure(runner) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary)
        log: list[dict] = []
        original = runner._run_process_impl

        def process_fixture(*args, **kwargs):
            return {"exit": 0, "timed_out": False,
                    "cleanup_complete": True, "abandoned_pipes": False,
                    "stdout": b"known process\n", "stderr": b""}

        runner._run_process_impl = process_fixture
        try:
            record = runner.run_command(
                ["x"], out, 60.0, out, log, "known-process", {}, None)
        finally:
            runner._run_process_impl = original
        if record["exit"] != 0 or log != [record]:
            raise AssertionError("known process was not retained")
        before = {"a": "1"}
        after = {"a": "2"}
        expect_raises(lambda: runner.ensure_equal("after", before, after),
                      "after resource snapshot change")
        lines = [line for line in (out / "commands.jsonl").read_text(
            encoding="utf-8").splitlines() if line.strip()]
        if len(lines) != 1:
            raise AssertionError("after-resource failure lost the process record")
        if json.loads(lines[0])["stdout"] != "known process\n":
            raise AssertionError("after-resource failure lost raw process data")
    print("post-process resource failure does not lose the known process")


def check_row_structure(runner) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        report = {"runs": [], "not_run": [], "failed": []}
        log: list[dict] = []
        fake = root / "not-executed"
        runner.run_role(
            report, log, root, "candidate", fake, root, "wz", None, None,
            None, {"hosted": fake, "native": fake}, fake, fake, fake, fake,
            {}, None, None, identity_ok=False,
            identity_error="control-only identity rejection")
        if len(report["runs"]) != 41:
            raise AssertionError(
                f"role not-run row count is {len(report['runs'])}, expected 41")
        if len(report["not_run"]) != 41:
            raise AssertionError("role not-run rows were not recorded")
        if log:
            raise AssertionError("identity rejection started an external command")
        if any(row["status"] != "not_run" for row in report["runs"]):
            raise AssertionError("identity rejection produced a real row")
    print("role identity rejection keeps all 41 expected rows without launch")


def check_nm_rows(runner) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary)
        log: list[dict] = []
        report = {"runs": [], "not_run": [], "failed": []}
        hosted = out / "liblang_hosted_runtime.a"
        native = out / "liblang_native_runtime.a"
        original = runner._run_process_impl

        def process_fixture(argv, cwd, env, timeout):
            text = ("lang_hosted_startup_v1\n"
                    if "hosted" in str(argv[-1])
                    else "lang_thread_spawn_v1\n")
            return {"exit": 0, "timed_out": False,
                    "cleanup_complete": True, "abandoned_pipes": False,
                    "stdout": text.encode(), "stderr": b""}

        runner._run_process_impl = process_fixture
        try:
            runner._run_nm(report, log, out, "legacy", "/usr/bin/nm",
                           hosted, native, {}, None)
        finally:
            runner._run_process_impl = original
        rows = [row for row in report["runs"]
                if row["category"] == "archive-symbols"]
        if len(rows) != 2 or not all(row["passed"] for row in rows):
            raise AssertionError(
                f"nm success did not add two passed rows: {rows}")
        if len(log) != 2:
            raise AssertionError("nm success did not record two commands")
    print("successful nm checks add both hosted/native report rows")


def check_full_legacy_orchestration(runner) -> None:
    """Run the real legacy orchestration with every process faked in memory."""
    def fake_process(argv, cwd, env, timeout):
        a = [str(part) for part in argv]
        name = Path(a[0]).name
        base = {"timed_out": False, "cleanup_complete": True,
                "abandoned_pipes": False, "stderr": b""}
        if name == "git":
            return dict(base, exit=0, stdout=b"deadbeef\n")
        if name in ("clang",):
            target = Path(a[a.index("-o") + 1])
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"object")
            return dict(base, exit=0, stdout=b"")
        if name in ("ar", "llvm-ar"):
            library = Path(a[-2])
            library.parent.mkdir(parents=True, exist_ok=True)
            library.write_bytes(b"archive")
            return dict(base, exit=0, stdout=b"")
        if name == "readelf":
            program = a[-1]
            if "core-freestanding" in program:
                text = "Type: EXEC\n"
            elif "libc-o" in program:
                text = "INTERP\nlibcrypto\n"
            else:
                text = "INTERP\n"
            return dict(base, exit=0, stdout=text.encode())
        if name in ("nm", "llvm-nm"):
            text = ("lang_hosted_startup_v1\n"
                    if "hosted" in a[-1] else "lang_thread_spawn_v1\n")
            return dict(base, exit=0, stdout=text.encode())
        if "woozyc" in name:
            if "build" in a:
                manifest = a[a.index("--project") + 1]
                if "link-failure" in manifest:
                    document = {"status": "error", "diagnostics": [
                        {"code": "E_LINK_FAILED"}]}
                    return dict(base, exit=1,
                                stdout=json.dumps(document).encode())
                out_dir = Path(a[a.index("--out-dir") + 1])
                out_dir.mkdir(parents=True, exist_ok=True)
                (out_dir / "program").write_bytes(b"program")
                document = {"status": "ok", "diagnostics": []}
                return dict(base, exit=0,
                            stdout=json.dumps(document).encode())
            if "check" in a:
                manifest = a[a.index("--project") + 1]
                code = "E_CAPABILITY_UNAVAILABLE"
                for case, expected in runner.NEGATIVE.items():
                    if f"/{case}/" in manifest:
                        code = expected
                document = {"status": "error", "diagnostics": [
                    {"code": code}]}
                return dict(base, exit=1,
                            stdout=json.dumps(document).encode())
        program = a[0]
        if "bounds-panic" in program:
            return dict(base, exit=101,
                        stdout=b"",
                        stderr=b"LANG_NATIVE_PANIC:v1:vec-index:0\n")
        return dict(base, exit=0, stdout=b"raw program output\n")

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        out = root / "out"
        cli = root / "woozyc"
        clang = root / "clang"
        ar = root / "llvm-ar"
        hosted = root / "liblang_hosted_runtime.a"
        native = root / "liblang_native_runtime.a"
        forged = root / "c-ffi-forged"
        for path, data in ((cli, b"cli"), (clang, b"clang"), (ar, b"ar"),
                           (hosted, b"hosted"), (native, b"native"),
                           (forged, b"forged")):
            path.write_bytes(data)
        argv = [
            "--cli", str(cli), "--root", str(runner.COMPILER_ROOT),
            "--out", str(out), "--clang", str(clang), "--ar", str(ar),
            "--hosted-runtime", str(hosted),
            "--native-runtime", str(native), "--forged", str(forged),
        ]
        original = runner._run_process_impl
        runner._run_process_impl = fake_process
        try:
            first_rc = runner.main(argv)
            run_dirs = list((out / "runs").iterdir())
            first_report_path = run_dirs[0] / "report.json"
            first_bytes = first_report_path.read_bytes()
            first_commands = (run_dirs[0] / "commands.jsonl").read_bytes()
            second_rc = runner.main(argv)
        finally:
            runner._run_process_impl = original
        if first_rc != 0 or second_rc != 0:
            raise AssertionError(
                f"fake legacy orchestration returned {first_rc}/{second_rc}")
        run_dirs = list((out / "runs").iterdir())
        if len(run_dirs) != 2:
            raise AssertionError(
                "legacy same --out did not keep two independent run dirs")
        if first_report_path.read_bytes() != first_bytes:
            raise AssertionError("legacy second run overwrote the first report")
        if (first_report_path.parent / "commands.jsonl").read_bytes() != first_commands:
            raise AssertionError("legacy second run overwrote first commands")
        reports = []
        for directory in run_dirs:
            reports.append(json.loads((directory / "report.json").read_text(
                encoding="utf-8")))
        for report in reports:
            if report["status"] != "passed":
                raise AssertionError(
                    f"fake legacy orchestration failed: {report['failed']}")
            if len(report["runs"]) != 43:
                raise AssertionError(
                    f"legacy orchestration rows={len(report['runs'])}, "
                    "expected 43")
            if report["counts"]["external_commands"] != 44:
                raise AssertionError(
                    "legacy orchestration did not execute 44 commands")
            if report["not_run"]:
                raise AssertionError("complete fake legacy run has not_run rows")
        if not all((directory / "commands.jsonl").is_file()
                   for directory in run_dirs):
            raise AssertionError("legacy orchestration wrote no command log")
    print("full legacy orchestration keeps 43 rows / 44 fake commands twice")


def _formal_fixture(runner, root):
    """Create fake formal inputs and argv; no external process is launched."""
    out = root / "out"
    reference_source = root / "reference-source"
    candidate_source = root / "candidate-source"
    stdlib = root / "stdlib"
    for directory, name in ((reference_source, "ref.wz"),
                            (candidate_source, "candidate.wz"),
                            (stdlib, "io.wz")):
        directory.mkdir()
        (directory / name).write_text("module m;\n", encoding="utf-8")
    reference_cli = root / "reference" / "woozyc"
    reference_helper = root / "reference" / "native" / "lang_native_helper"
    candidate_cli = root / "candidate" / "woozyc"
    reference_cli.parent.mkdir()
    candidate_cli.parent.mkdir()
    reference_helper.parent.mkdir(parents=True)
    reference_cli.write_bytes(b"reference-cli")
    reference_helper.write_bytes(b"reference-native-helper")
    candidate_cli.write_bytes(b"candidate-cli")
    clang = root / "clang"
    ar = root / "llvm-ar"
    hosted = root / "liblang_hosted_runtime.a"
    native = root / "liblang_native_runtime.a"
    forged = root / "c-ffi-forged"
    component = root / "wz-component"
    readelf = root / "readelf"
    nm = root / "nm"
    for path, data in ((clang, b"clang"), (ar, b"ar"),
                       (hosted, b"hosted"), (native, b"native"),
                       (forged, b"forged"), (component, b"component"),
                       (readelf, b"readelf"), (nm, b"nm")):
        path.write_bytes(data)
    global STRACE_PATH
    STRACE_PATH = root / "strace"
    STRACE_PATH.write_bytes(b"strace")
    toolchain = root / "toolchain"
    llvm_bin = toolchain / "llvm-22.1.8" / "bin"
    llvm_bin.mkdir(parents=True)
    for name in runner.LLVM_TOOL_NAMES:
        (llvm_bin / name).write_bytes(b"llvm-tool")
    reference_map = runner.snapshot_tree(reference_source, ".wz")
    candidate_map = runner.snapshot_tree(candidate_source, ".wz")
    stdlib_map = runner.snapshot_tree(stdlib, ".wz")
    forged_manifest = (runner.COMPILER_ROOT
                       / "tests/native/c_ffi/forged-project/lang.toml")
    reference_record = root / "reference-record.json"
    reference_record.write_text(json.dumps({
        "status": "passed",
        "cli": {"sha256": sha(reference_cli.read_bytes())},
        "source_map_sha256": reference_map["map_sha256"],
    }), encoding="utf-8")
    candidate_record = root / "candidate-record.json"
    candidate_record.write_text(json.dumps({
        "status": "passed",
        "cli": {"sha256": sha(candidate_cli.read_bytes())},
        "source_map_sha256": candidate_map["map_sha256"],
    }), encoding="utf-8")
    component_record = root / "component-record.json"
    component_record.write_text(json.dumps({
        "status": "passed",
        "component": {"sha256": sha(component.read_bytes())},
        "source116_map_sha256": candidate_map["map_sha256"],
        "stdlib": {"map_sha256": stdlib_map["map_sha256"]},
        "forged_project": {
            "manifest": {"sha256": sha(forged_manifest.read_bytes())}},
    }), encoding="utf-8")
    argv = [
        "--mode", "formal", "--root", str(runner.COMPILER_ROOT),
        "--out", str(out), "--clang", str(clang), "--ar", str(ar),
        "--hosted-runtime", str(hosted),
        "--native-runtime", str(native),
        "--reference-cli", str(reference_cli),
        "--reference-cli-record", str(reference_record),
        "--reference-source-root", str(reference_source),
        "--reference-native-helper", str(reference_helper),
        "--reference-forged", str(forged),
        "--candidate-cli", str(candidate_cli),
        "--candidate-cli-record", str(candidate_record),
        "--candidate-source-root", str(candidate_source),
        "--candidate-forged-component", str(component),
        "--candidate-component-record", str(component_record),
        "--candidate-stdlib", str(stdlib),
        "--toolchain-root", str(toolchain),
        "--strace", str(STRACE_PATH),
        "--readelf", str(readelf), "--nm", str(nm),
    ]
    return {
        "root": root, "out": out, "argv": argv,
        "reference_cli": reference_cli,
        "reference_helper": reference_helper,
        "candidate_cli": candidate_cli,
        "strace": STRACE_PATH,
    }


def _formal_fake_process(runner, fixture, *, trace_helper_for_candidate=False,
                         mutate_helper=None):
    forged_text = good_forged_stdout()
    helper = str(fixture["reference_helper"])
    reference_cli = str(fixture["reference_cli"])
    candidate_cli = str(fixture["candidate_cli"])
    state = {"helper_mutated": False}

    def fake_process(argv, cwd, env, timeout):
        a = [str(part) for part in argv]
        base = {"timed_out": False, "cleanup_complete": True,
                "abandoned_pipes": False, "stderr": b""}
        if a and a[0] == str(fixture["strace"]):
            index = a.index("-o")
            trace = Path(a[index + 1])
            tool = a[index + 2:]
            trace.parent.mkdir(parents=True, exist_ok=True)
            lines = [f"1 execve({json.dumps(tool[0])}, [\"x\"], 0x0) = 0\n"]
            is_build = "build" in tool and "--out-dir" in tool
            if is_build and tool[0] == reference_cli:
                lines.append(
                    f"2 execve({json.dumps(helper)}, [\"helper\"], 0x0) = 0\n")
            elif is_build and trace_helper_for_candidate and tool[0] == candidate_cli:
                lines.append(
                    f"2 execve({json.dumps(helper)}, [\"helper\"], 0x0) = 0\n")
            trace.write_text("".join(lines), encoding="utf-8")
            a = tool
        name = Path(a[0]).name
        if name in ("clang",):
            target = Path(a[a.index("-o") + 1])
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"object")
            result = dict(base, exit=0, stdout=b"")
        elif name in ("ar", "llvm-ar"):
            library = Path(a[-2])
            library.parent.mkdir(parents=True, exist_ok=True)
            library.write_bytes(b"archive")
            result = dict(base, exit=0, stdout=b"")
        elif name == "readelf":
            program = a[-1]
            if "core-freestanding" in program:
                text = "Type: EXEC\n"
            elif "libc-o" in program:
                text = "INTERP\nlibcrypto\n"
            else:
                text = "INTERP\n"
            result = dict(base, exit=0, stdout=text.encode())
        elif name in ("nm", "llvm-nm"):
            text = ("lang_hosted_startup_v1\n"
                    if "hosted" in a[-1] else "lang_thread_spawn_v1\n")
            result = dict(base, exit=0, stdout=text.encode())
        elif "woozyc" in name:
            if "build" in a:
                manifest = a[a.index("--project") + 1]
                if "link-failure" in manifest:
                    document = {"status": "error", "diagnostics": [
                        {"code": "E_LINK_FAILED"}]}
                    result = dict(base, exit=1,
                                  stdout=json.dumps(document).encode())
                else:
                    out_dir = Path(a[a.index("--out-dir") + 1])
                    out_dir.mkdir(parents=True, exist_ok=True)
                    (out_dir / "program").write_bytes(b"program")
                    document = {"status": "ok", "diagnostics": []}
                    result = dict(base, exit=0,
                                  stdout=json.dumps(document).encode())
            elif "check" in a:
                manifest = a[a.index("--project") + 1]
                code = "E_CAPABILITY_UNAVAILABLE"
                for case, expected in runner.NEGATIVE.items():
                    if f"/{case}/" in manifest:
                        code = expected
                document = {"status": "error", "diagnostics": [
                    {"code": code}]}
                result = dict(base, exit=1,
                              stdout=json.dumps(document).encode())
            else:
                result = dict(base, exit=0, stdout=b"")
        elif name == "wz-component":
            result = dict(base, exit=0, stdout=forged_text.encode())
        elif name == "c-ffi-forged":
            result = dict(base, exit=0, stdout=b"C++ forged reference\n")
        else:
            program = a[0]
            if "bounds-panic" in program:
                result = dict(base, exit=101, stdout=b"",
                              stderr=b"LANG_NATIVE_PANIC:v1:vec-index:0\n")
            else:
                result = dict(base, exit=0, stdout=b"raw program output\n")
        if (mutate_helper is not None and not state["helper_mutated"] and
                "build" in a and a[0] == reference_cli and
                "--out-dir" in a):
            mutate_helper(fixture)
            state["helper_mutated"] = True
        return result

    return fake_process


def _run_formal_scenario(runner, *, trace_helper_for_candidate=False,
                         mutate_helper=None, mutate_between_roles=None,
                         check_second_run=False):
    """Run the real formal main with in-memory processes and optional hooks."""
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        fixture = _formal_fixture(runner, root)
        fake_process = _formal_fake_process(
            runner, fixture,
            trace_helper_for_candidate=trace_helper_for_candidate,
            mutate_helper=mutate_helper)
        between_state = {"done": False}
        original_run_role = runner.run_role

        def wrapped_run_role(*args, **kwargs):
            result = original_run_role(*args, **kwargs)
            role = args[3] if len(args) > 3 else kwargs.get("role")
            if (role == "reference" and mutate_between_roles is not None and
                    not between_state["done"]):
                between_state["done"] = True
                mutate_between_roles(fixture)
            return result

        original = runner._run_process_impl
        runner._run_process_impl = fake_process
        try:
            with patch.object(runner, "run_role", wrapped_run_role):
                rc = runner.main(fixture["argv"])
                report_path = fixture["out"] / "report.json"
                report_bytes = (report_path.read_bytes()
                                if report_path.is_file() else None)
                second_rc = (runner.main(fixture["argv"])
                             if check_second_run else None)
                report_bytes_after_second = (
                    report_path.read_bytes()
                    if check_second_run and report_path.is_file() else None)
        finally:
            runner._run_process_impl = original
        report = (json.loads(report_path.read_text(encoding="utf-8"))
                  if report_path.is_file() else None)
        return {
            "rc": rc,
            "second_rc": second_rc,
            "report": report,
            "report_bytes": report_bytes,
            "report_bytes_after_second": report_bytes_after_second,
            "fixture": {
                "out": str(fixture["out"]),
                "reference_helper": str(fixture["reference_helper"]),
                "helper_sha256": sha(fixture["reference_helper"].read_bytes()),
            },
        }


def _fixture_artifacts(fixture):
    shared = Path(fixture["out"]) / "out/shared"
    private = (Path(fixture["out"]) /
               "mirror/tests/native/c_ffi/.build")
    return {
        "object_shared": shared / "ffi_fixture.o",
        "object_private": private / "ffi_fixture.o",
        "archive_shared": shared / "libffi_fixture.a",
        "archive_private": private / "libffi_fixture.a",
    }


def check_full_formal_orchestration(runner) -> None:
    """Run the real formal orchestration with every process faked in memory."""
    scenario = _run_formal_scenario(runner, check_second_run=True)
    rc = scenario["rc"]
    second_rc = scenario["second_rc"]
    report = scenario["report"]
    report_bytes = scenario["report_bytes"]
    out = Path(scenario["fixture"]["out"])
    if rc != 0:
        raise AssertionError(f"fake formal orchestration returned {rc}")
    if second_rc != 1:
        raise AssertionError("formal second run into the same --out passed")
    if scenario["report_bytes_after_second"] != report_bytes:
        raise AssertionError("formal refusal changed the first report")
    if report["status"] != "passed":
        raise AssertionError(
            f"fake formal orchestration failed: {report['failed']}")
    if len(report["runs"]) != 84:
        raise AssertionError(
            f"formal orchestration rows={len(report['runs'])}, expected 84")
    if report["counts"]["external_commands"] != 84:
        raise AssertionError(
            "formal orchestration did not execute 84 declared commands")
    if report["not_run"]:
        raise AssertionError("complete fake formal run reports not_run rows")
    if report["identity"]["candidate_component"]["ok"] is not True:
        raise AssertionError("candidate component identity was not checked")
    helper_record = report["identity"].get("reference_native_helper")
    if not helper_record or helper_record.get("sha256") != scenario[
            "fixture"]["helper_sha256"]:
        raise AssertionError("reference native helper SHA was not recorded")
    if report.get("inputs_stable") is not True:
        raise AssertionError("normal formal run did not report inputs_stable")
    print("full formal orchestration keeps 84 rows / 84 fake commands")
    return {
        "rows": len(report["runs"]),
        "external_commands": report["counts"]["external_commands"],
        "helper_sha256": helper_record["sha256"],
        "inputs_stable": report["inputs_stable"],
    }


def check_reference_helper_controls(runner) -> None:
    """Reference CLI->helper trace is accepted; candidate and mutation fail."""
    normal = _run_formal_scenario(runner)
    normal_report = normal["report"]
    helper_sha = normal["fixture"]["helper_sha256"]
    if normal["rc"] != 0 or normal_report["status"] != "passed":
        raise AssertionError(
            f"reference CLI->helper trace was rejected: "
            f"{normal_report and normal_report.get('failed')}")
    recorded = normal_report["identity"]["reference_native_helper"]
    if recorded.get("sha256") != helper_sha:
        raise AssertionError("reference helper SHA was not frozen in report")
    reference_rows = [row for row in normal_report["runs"]
                      if row["role"] == "reference"]
    if len(reference_rows) != 41 or not all(
            row["status"] == "passed" for row in reference_rows):
        raise AssertionError("reference CLI->helper role did not pass")

    candidate = _run_formal_scenario(
        runner, trace_helper_for_candidate=True)
    candidate_report = candidate["report"]
    if candidate["rc"] != 1 or candidate_report["status"] != "failed":
        raise AssertionError("candidate accepted the reference helper trace")
    candidate_errors = " ".join(candidate_report["failed"])
    if ("unexpected compiler subprocesses" not in candidate_errors and
            "exec allowlist" not in candidate_errors):
        raise AssertionError(
            f"candidate helper trace failure was not explicit: "
            f"{candidate_report['failed']}")

    def change_helper(fixture):
        Path(fixture["reference_helper"]).write_bytes(b"changed-helper")

    mutated = _run_formal_scenario(runner, mutate_helper=change_helper)
    mutated_report = mutated["report"]
    if mutated["rc"] != 1 or mutated_report["status"] != "failed":
        raise AssertionError("reference helper change during run passed")
    if mutated_report.get("inputs_stable") is not False:
        raise AssertionError("reference helper change kept inputs_stable true")
    mutated_errors = " ".join(mutated_report["failed"])
    if "reference native helper" not in mutated_errors:
        raise AssertionError(
            f"helper change failure was not explicit: "
            f"{mutated_report['failed']}")
    if len(mutated_report["runs"]) != 84:
        raise AssertionError("helper change lost known process rows")
    if mutated_report["identity"]["reference_native_helper"][
            "sha256"] != helper_sha:
        raise AssertionError("helper change overwrote the frozen initial SHA")
    print("reference CLI->helper accepted; candidate rejected; change failed")
    return {
        "normal": {
            "status": normal_report["status"],
            "reference_rows": len(reference_rows),
            "helper_sha256": recorded["sha256"],
        },
        "candidate_helper_trace": {
            "status": candidate_report["status"],
            "failed": candidate_report["failed"],
        },
        "helper_changed_during_run": {
            "status": mutated_report["status"],
            "inputs_stable": mutated_report["inputs_stable"],
            "failed": mutated_report["failed"],
            "rows": len(mutated_report["runs"]),
        },
    }


def check_fixture_artifact_controls(runner) -> None:
    """Archive/object shared and private rewrites/missing must fail."""
    scenarios = (
        ("archive-shared-rewrite", "archive_shared", "write"),
        ("archive-private-rewrite", "archive_private", "write"),
        ("object-shared-rewrite", "object_shared", "write"),
        ("object-private-rewrite", "object_private", "write"),
        ("archive-shared-missing", "archive_shared", "unlink"),
        ("archive-private-missing", "archive_private", "unlink"),
        ("object-shared-missing", "object_shared", "unlink"),
        ("object-private-missing", "object_private", "unlink"),
    )
    details = {}
    for name, artifact, action in scenarios:
        def mutate(fixture, artifact=artifact, action=action):
            paths = _fixture_artifacts(fixture)
            path = paths[artifact]
            if action == "write":
                path.write_bytes(("changed " + artifact).encode("ascii"))
            else:
                path.unlink()

        scenario = _run_formal_scenario(
            runner, mutate_between_roles=mutate)
        report = scenario["report"]
        if scenario["rc"] != 1 or report["status"] != "failed":
            raise AssertionError(f"{name}: mutated fixture still passed")
        if report.get("inputs_stable") is not False:
            raise AssertionError(f"{name}: inputs_stable was not false")
        if len(report["runs"]) != 84:
            raise AssertionError(f"{name}: known process rows were lost")
        errors = " ".join(report["failed"])
        if "fixture artifacts" not in errors:
            raise AssertionError(
                f"{name}: fixture failure was not explicit: "
                f"{report['failed']}")
        initial = report.get("fixture_artifacts", {}).get("initial", {})
        final = report.get("fixture_artifacts", {}).get("final", {})
        if not initial.get("state_sha256") or final.get("ok") is not False:
            raise AssertionError(
                f"{name}: initial/final fixture evidence is incomplete")
        details[name] = {
            "status": report["status"],
            "inputs_stable": report["inputs_stable"],
            "rows": len(report["runs"]),
            "initial_state_sha256": initial["state_sha256"],
            "final_ok": final["ok"],
            "failed": report["failed"],
        }
    print("archive/object shared/private rewrites and missing files all fail")
    return details


def check_main_refusal(runner) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        out = root / "out"
        missing = root / "missing"
        rc = runner.main([
            "--mode", "formal",
            "--root", str(root),
            "--out", str(out),
            "--clang", str(missing / "clang"),
            "--ar", str(missing / "ar"),
            "--hosted-runtime", str(missing / "hosted.a"),
            "--native-runtime", str(missing / "native.a"),
        ])
        if rc != 1:
            raise AssertionError("main did not refuse incomplete formal argv")
        if out.exists():
            raise AssertionError("main wrote output before refusing argv")
    print("runner main refuses incomplete formal argv without external launch")


def check_counts(runner) -> None:
    if runner.LEGACY_ROWS != 43:
        raise AssertionError("legacy row count changed")
    if runner.FORMAL_ROWS != 84:
        raise AssertionError("formal row count changed")
    if len(runner.PROJECTS) != 4 or len(runner.NEGATIVE) != 8:
        raise AssertionError("original 15-project row sources changed")
    if len(runner.EXPECTED_CASES) != 8:
        raise AssertionError("forged case count changed")
    if runner.LEGACY_COMMANDS != 44 or runner.FORMAL_COMMANDS != 84:
        raise AssertionError("external command count changed")
    print("43 legacy rows / 44 commands / 84 formal rows / 8 forged cases")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--json-out", type=Path, default=None)
    args = parser.parse_args()
    results: dict[str, str] = {}
    details: dict[str, dict] = {}
    launches = EXTERNAL_LAUNCH_ATTEMPTS
    guarded = [
        (subprocess, "Popen"), (subprocess, "run"), (subprocess, "call"),
        (subprocess, "check_call"), (subprocess, "check_output"),
        (os, "system"), (os, "posix_spawn"), (os, "posix_spawnp"),
        (os, "execv"), (os, "execve"), (os, "execvp"), (os, "execvpe"),
        (os, "spawnv"), (os, "spawnve"), (os, "fork"), (os, "forkpty"),
        (socket, "socket"), (socket, "create_connection"),
    ]
    with contextlib.ExitStack() as stack:
        # Guard every launch primitive before importing the runner under test.
        for module, name in guarded:
            if hasattr(module, name):
                stack.enter_context(patch.object(
                    module, name, forbid_external_launch))
        import run_c_ffi as runner  # noqa: PLC0415
        checks = {
            "strict_process": lambda: check_strict_process(runner),
            "timeout_json": lambda: check_timeout_and_json(runner),
            "build_elf": lambda: check_build_and_elf(runner),
            "link_failure": lambda: check_link_failure(runner),
            "archive_symbols": lambda: check_archive_symbols(runner),
            "trace": lambda: check_trace(runner),
            "forged": lambda: check_forged(runner),
            "source_identity": lambda: check_source_identity(runner),
            "input_set": lambda: check_input_set(runner),
            "fresh_output": lambda: check_fresh_output(runner),
            "persist_write_failure": lambda: check_persist_write_failure(runner),
            "after_resource_failure": lambda: check_after_resource_failure(
                runner),
            "row_structure": lambda: check_row_structure(runner),
            "nm_rows": lambda: check_nm_rows(runner),
            "full_legacy_orchestration": lambda: check_full_legacy_orchestration(
                runner),
            "full_formal_orchestration": lambda: check_full_formal_orchestration(
                runner),
            "reference_helper_controls": lambda: check_reference_helper_controls(
                runner),
            "fixture_artifact_controls": lambda: check_fixture_artifact_controls(
                runner),
            "main_refusal": lambda: check_main_refusal(runner),
            "counts": lambda: check_counts(runner),
        }
        try:
            for name, check in checks.items():
                outcome = check()
                results[name] = "passed"
                if isinstance(outcome, dict):
                    details[name] = outcome
        except Exception as error:
            results["failed"] = f"{type(error).__name__}: {error}"
            results["details"] = details
            results["external_launch_attempts"] = list(launches)
            if args.json_out:
                args.json_out.parent.mkdir(parents=True, exist_ok=True)
                args.json_out.write_text(
                    json.dumps({"status": "failed", "checks": results},
                               indent=2) + "\n", encoding="utf-8")
            print("CONTROLS failed")
            raise
        if launches:
            raise AssertionError(
                f"external launch attempted in pure controls: {launches}")
        results["details"] = details
        results["external_launch_attempts"] = list(launches)
        results["status"] = "passed"
        if args.json_out:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(
                json.dumps(results, indent=2, sort_keys=True) + "\n",
                encoding="utf-8")
        print(f"CONTROLS passed (external launches: {len(launches)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
