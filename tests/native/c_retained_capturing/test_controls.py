#!/usr/bin/env python3
"""Portable controls for run_capturing.py.

These controls use only pure data or temporary files.  They never invoke the
compiler, LLVM, the fixture, a lifecycle binary or any Source command.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile

import run_capturing as rc


def block_external_execution():
    """Make every pure-control attempt to leave Python fail loudly."""

    def blocked(*args, **kwargs):
        raise RuntimeError("external execution is blocked in pure controls")

    subprocess.Popen = blocked
    subprocess.run = blocked
    subprocess.check_output = blocked
    subprocess.check_call = blocked
    os.system = blocked
    socket.socket = blocked
    socket.create_connection = blocked


CHECKS = []


def check(name, condition, detail=""):
    CHECKS.append(
        {
            "name": name,
            "status": "passed" if condition else "failed",
            "detail": detail if not condition else "",
        }
    )


def rejects(name, function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except Exception as error:  # noqa: BLE001 - control boundary
        check(name, True, repr(error))
        return
    check(name, False, "control was accepted")


def accepts(name, function, *args, **kwargs):
    try:
        value = function(*args, **kwargs)
    except Exception as error:  # noqa: BLE001
        check(name, False, repr(error))
        return None
    check(name, True)
    return value


def safe_record(**overrides):
    record = {
        "status": "exited",
        "exit": 0,
        "timed_out": False,
        "cleanup_complete": True,
        "abandoned_pipes": False,
        "spawn_error": None,
    }
    record.update(overrides)
    return record


def test_json():
    accepts(
        "json_valid_object",
        rc.parse_json_bytes,
        b'{"status":"ok","diagnostics":[]}',
        "valid",
    )
    rejects("json_nan", rc.parse_json_bytes, b'{"x":NaN}', "nan")
    rejects("json_infinity", rc.parse_json_bytes, b'{"x":Infinity}', "inf")
    rejects("json_neg_infinity", rc.parse_json_bytes, b'{"x":-Infinity}', "ninf")
    rejects("json_overflow_float", rc.parse_json_bytes, b'{"x":1e999}', "overflow")
    rejects("json_array", rc.parse_json_bytes, b"[]", "array")
    rejects("json_empty", rc.parse_json_bytes, b"", "empty")
    rejects("json_bad_utf8", rc.parse_json_bytes, b"\xff", "bad utf8")
    rejects(
        "json_trailing_garbage",
        rc.parse_json_bytes,
        b'{"status":"ok"} trailing',
        "trailing",
    )


def test_process_guard():
    accepts("process_exit0", rc.process_problems, safe_record(), "exit0", 0)
    accepts("process_exit1", rc.process_problems, safe_record(exit=1), "exit1", 1)
    for name, value in (
        ("bool_false", False),
        ("bool_true", True),
        ("float", 0.0),
        ("none", None),
        ("string", "0"),
        ("negative", -1),
    ):
        check(
            "process_reject_exit_" + name,
            bool(rc.process_problems(safe_record(exit=value), "x", 0)),
        )
    for field in ("timed_out", "cleanup_complete", "abandoned_pipes"):
        for value in (0, 1, None, "false"):
            check(
                f"process_reject_{field}_{value!r}",
                bool(rc.process_problems(safe_record(**{field: value}), "x")),
            )
    check(
        "process_reject_timed_out_true",
        bool(rc.process_problems(safe_record(timed_out=True), "x")),
    )
    check(
        "process_reject_cleanup_false",
        bool(rc.process_problems(safe_record(cleanup_complete=False), "x")),
    )
    check(
        "process_reject_pipes_true",
        bool(rc.process_problems(safe_record(abandoned_pipes=True), "x")),
    )
    check(
        "process_reject_spawn",
        bool(rc.process_problems(safe_record(status="spawn_error"), "x")),
    )


def test_documents():
    ok = {"status": "ok", "diagnostics": []}
    error = {
        "status": "error",
        "diagnostics": [{"code": "E_CALL_ARGUMENT", "severity": "error"}],
    }
    problems, codes = rc.document_problems(ok, "ok", "ok")
    check("document_ok", not problems and codes == [], repr(problems))
    problems, codes = rc.document_problems(
        error, "error", "error", "E_CALL_ARGUMENT"
    )
    check(
        "document_error",
        not problems and codes == ["E_CALL_ARGUMENT"],
        repr(problems),
    )
    problems, _ = rc.document_problems(
        {"status": "ok", "diagnostics": [{"code": "X", "severity": "error"}]},
        "ok-with-error",
        "ok",
    )
    check("document_ok_with_error_rejected", bool(problems))
    problems, _ = rc.document_problems(
        {"status": "error", "diagnostics": []}, "error-empty", "error"
    )
    check("document_error_without_diagnostic_rejected", bool(problems))
    problems, _ = rc.document_problems(ok, "wrong-status", "error")
    check("document_wrong_status_rejected", bool(problems))


def test_reference_preflight():
    runner = rc.Runner.__new__(rc.Runner)
    runner.inputs_before = None
    try:
        runner.verify_reference_resources()
    except rc.EvidenceError as error:
        check("reference_preflight_requires_snapshot", "before snapshot" in str(error))
    else:
        check("reference_preflight_requires_snapshot", False)

    with tempfile.TemporaryDirectory() as directory:
        package = Path(directory)
        executable = package / "bin" / "woozyc"
        executable.parent.mkdir()
        helper = package / "native" / "lang_native_helper"
        helper.parent.mkdir()
        helper.touch()
        check("reference_helper_is_relative_to_package",
              rc.Runner.near_executable(executable, "native/lang_native_helper") == helper)


def test_trace():
    allowed = {"/fixed/woozyc", "/fixed/clang-22", "/fixed/opt"}
    rc.audit_trace_paths(
        ["/fixed/woozyc", "/fixed/clang-22", "/fixed/opt"],
        allowed,
        "allowed",
    )
    check("trace_exact_allowed", True)
    rejects(
        "trace_unexpected_rejected",
        rc.audit_trace_paths,
        ["/fixed/woozyc", "/usr/bin/ld"],
        allowed,
        "unexpected",
    )
    rejects(
        "trace_candidate_helper_rejected",
        rc.audit_trace_paths,
        ["/fixed/woozyc", "/fixed/lang_native_helper"],
        {"/fixed/woozyc", "/fixed/lang_native_helper"},
        "candidate",
        True,
    )


def test_rejections():
    good_record = safe_record(exit=1)
    good_doc = {
        "status": "error",
        "diagnostics": [{"code": "E_BINDING_INVALID", "severity": "error"}],
    }
    ok, problems, codes = rc.negative_result_ok(
        good_record, good_doc, "E_BINDING_INVALID", True
    )
    check("negative_good", ok and not problems and codes, repr(problems))
    cases = (
        ("timeout", rc.negative_result_ok(
            safe_record(exit=1, timed_out=True), good_doc,
            "E_BINDING_INVALID", True)),
        ("signal", rc.negative_result_ok(
            safe_record(exit=-11), good_doc, "E_BINDING_INVALID", True)),
        ("exit0", rc.negative_result_ok(
            safe_record(exit=0), good_doc, "E_BINDING_INVALID", True)),
        ("exit2", rc.negative_result_ok(
            safe_record(exit=2), good_doc, "E_BINDING_INVALID", True)),
        ("wrong_code", rc.negative_result_ok(
            good_record,
            {"status": "error", "diagnostics": [
                {"code": "E_OTHER", "severity": "error"}]},
            "E_BINDING_INVALID", True)),
        ("program_present", rc.negative_result_ok(
            good_record, good_doc, "E_BINDING_INVALID", False)),
    )
    for name, (ok, problems, _codes) in cases:
        check("negative_reject_" + name, not ok and bool(problems))
    ok, problems, _ = rc.no_output_result_ok(
        good_record, good_doc, "E_BINDING_INVALID", True
    )
    check("no_output_good", ok and not problems)
    ok, problems, _ = rc.no_output_result_ok(
        safe_record(exit=1, timed_out=True), good_doc,
        "E_BINDING_INVALID", True
    )
    check("no_output_timeout_rejected", not ok and bool(problems))


def test_loader():
    expected = "/run/fixture/libc_capturing.so"
    line = "  123: calling init: /run/fixture/libc_capturing.so"
    matched, detail, loaded = rc.loader_line_matches(line, expected)
    check(
        "loader_exact",
        matched and detail == line.strip() and loaded == expected,
    )
    suffix = "  123: calling init: /run/fixture/libc_capturing.so.wrong"
    matched, _, _ = rc.loader_line_matches(suffix, expected)
    check("loader_suffix_rejected", not matched)
    matched, _, _ = rc.loader_line_matches(line, "/run/other/libc_capturing.so")
    check("loader_other_rejected", not matched)
    matched, _, _ = rc.loader_line_matches("", expected)
    check("loader_missing_rejected", not matched)
    normalized = "  9: calling init: /run/other/../fixture/libc_capturing.so"
    matched, _, loaded = rc.loader_line_matches(normalized, expected)
    check(
        "loader_normalized_exact",
        matched and loaded == expected,
    )


def run_probe_route(source_tests, stdout_bytes, fixture_ok=True, **overrides):
    """Run the real probe/source_route with an in-memory fake record.

    check_project is the only process boundary replaced here.  The route,
    probe classification, failure recording and final status are the
    production methods used by main().
    """
    with tempfile.TemporaryDirectory() as directory:
        stdout_path = Path(directory) / "stdout.bin"
        stdout_path.write_bytes(stdout_bytes)
        record = safe_record(**overrides)
        record.update(
            {
                "index": 0,
                "stdout_path": str(stdout_path),
                "stderr_path": str(stdout_path),
                "trace": overrides.get(
                    "trace", {"status": "not_requested"}
                ),
            }
        )
        runner = object.__new__(rc.Runner)
        runner.source_tests = source_tests
        runner.opts = (0,)
        runner.failures = []
        runner.not_run = []
        runner.probe_record = None
        runner.tests = Path(directory)
        runner.out = Path(directory)
        runner.route_calls = []
        runner.check_project = lambda *args, **kwargs: record
        runner.full_source_route = lambda: runner.route_calls.append("full")
        runner.static_legacy_route = lambda: runner.route_calls.append("static")
        rc.Runner.source_route(runner, fixture_ok)
        return runner


def test_legacy_auto():
    missing = {
        "status": "error",
        "diagnostics": [
            {
                "code": "E_BINDING_INVALID",
                "message": "unknown registration metadata",
                "phase": "binding",
                "severity": "error",
            }
        ],
    }
    missing_bytes = json.dumps(missing).encode()
    runner = run_probe_route("auto", missing_bytes, exit=1)
    check(
        "auto_missing_skips",
        not runner.failures and runner.route_calls == ["static"],
        repr(runner.failures),
    )
    check(
        "auto_missing_probe_status",
        runner.probe_record is not None
        and runner.probe_record.get("status") == "unsupported",
    )
    check(
        "auto_missing_document_retained",
        runner.probe_record.get("document") == missing,
    )
    check(
        "auto_missing_not_run",
        any(
            item["name"] == "source:integration-probe"
            for item in runner.not_run
        )
        and any(item["name"] == "source:probe:O0" for item in runner.not_run),
    )
    check(
        "auto_missing_core_once",
        sum(item["name"] == "core-freestanding" for item in runner.not_run) == 1,
    )
    check(
        "auto_missing_passed_with_not_run",
        runner.final_status() == "passed_with_not_run",
        runner.final_status(),
    )

    runner = run_probe_route("require", missing_bytes, exit=1)
    check(
        "require_missing_fails",
        bool(runner.failures) and runner.final_status() == "failed",
        repr(runner.failures),
    )
    check(
        "require_missing_no_skip",
        runner.route_calls == ["full"] and not runner.not_run,
        repr((runner.route_calls, runner.not_run)),
    )
    check(
        "require_missing_document_retained",
        runner.probe_record.get("document") == missing,
    )

    ok = {"status": "ok", "diagnostics": []}
    ok_bytes = json.dumps(ok).encode()
    runner = run_probe_route("require", ok_bytes)
    check(
        "require_supported_runs_full",
        not runner.failures and runner.route_calls == ["full"],
        repr(runner.failures),
    )
    runner = run_probe_route("auto", ok_bytes)
    check(
        "auto_supported_runs_full",
        not runner.failures and runner.route_calls == ["full"],
        repr(runner.failures),
    )

    unrelated = {
        "status": "error",
        "diagnostics": [
            {
                "code": "E_OTHER",
                "message": "unrelated rejection",
                "severity": "error",
            }
        ],
    }
    runner = run_probe_route("auto", json.dumps(unrelated).encode(), exit=1)
    check(
        "auto_unrelated_fails",
        bool(runner.failures) and runner.final_status() == "failed",
        repr(runner.failures),
    )
    check(
        "auto_unrelated_no_skip",
        runner.route_calls == ["static"] and not runner.not_run,
        repr((runner.route_calls, runner.not_run)),
    )
    check(
        "auto_unrelated_document_retained",
        runner.probe_record.get("document") == unrelated,
    )

    no_diagnostic = {"status": "error", "diagnostics": []}
    runner = run_probe_route(
        "auto", json.dumps(no_diagnostic).encode(), exit=1
    )
    check(
        "auto_error_without_diagnostic_fails",
        bool(runner.failures) and not runner.not_run,
    )
    error_at_zero = {
        "status": "error",
        "diagnostics": [
            {
                "code": "E_BINDING_INVALID",
                "message": "unknown registration metadata",
                "severity": "error",
            }
        ],
    }
    runner = run_probe_route("auto", json.dumps(error_at_zero).encode(), exit=0)
    check(
        "auto_exit0_error_fails",
        bool(runner.failures) and not runner.not_run,
    )
    runner = run_probe_route("auto", b"{not complete json", exit=1)
    check(
        "auto_malformed_json_fails",
        bool(runner.failures) and not runner.not_run,
    )
    nonfinite = (
        b'{"status":"error","diagnostics":[{"code":"E_BINDING_INVALID",'
        b'"message":"unknown registration metadata","severity":"error"}],'
        b'"x":NaN}'
    )
    runner = run_probe_route("auto", nonfinite, exit=1)
    check(
        "auto_nonfinite_json_fails",
        bool(runner.failures) and not runner.not_run,
    )
    runner = run_probe_route(
        "auto", json.dumps(missing).encode(), timed_out=True
    )
    check(
        "auto_timeout_fails",
        bool(runner.failures) and not runner.not_run,
    )
    runner = run_probe_route("auto", json.dumps(missing).encode(), exit=-11)
    check(
        "auto_signal_fails",
        bool(runner.failures) and not runner.not_run,
    )
    runner = run_probe_route(
        "auto",
        json.dumps(missing).encode(),
        status="spawn_error",
        exit=None,
        spawn_error="boom",
    )
    check(
        "auto_spawn_error_fails",
        bool(runner.failures) and not runner.not_run,
    )
    runner = run_probe_route(
        "auto", json.dumps(missing).encode(), cleanup_complete=0
    )
    check(
        "auto_bad_process_flag_fails",
        bool(runner.failures) and not runner.not_run,
    )
    runner = run_probe_route(
        "auto",
        json.dumps(missing).encode(),
        exit=1,
        trace={"status": "violation"},
    )
    check(
        "auto_trace_violation_fails",
        bool(runner.failures) and not runner.not_run,
    )
    runner = run_probe_route("auto", json.dumps(missing).encode(), exit=2)
    check(
        "auto_wrong_exit_fails",
        bool(runner.failures) and not runner.not_run,
    )
    capability = {
        "status": "error",
        "diagnostics": [
            {
                "code": "E_CAPABILITY_UNAVAILABLE",
                "message": "C binding unavailable",
                "severity": "error",
            }
        ],
    }
    runner = run_probe_route(
        "auto", json.dumps(capability).encode(), exit=1
    )
    check(
        "auto_capability_missing_skips",
        not runner.failures and runner.route_calls == ["static"],
        repr(runner.failures),
    )


def fake_corpus():
    cases = []
    for project in rc.SOURCE_PROJECTS:
        cases.append(
            {
                "id": "project:tests/native/c_retained_capturing/projects/"
                + project
                + "/lang.toml",
                "kind": "project",
                "manifest": "tests/native/c_retained_capturing/projects/"
                + project
                + "/lang.toml",
                "backend": "native",
                "operation": "build_native",
                "operations": ["check", "build_native"],
                "support": "existing_runner",
                "adaptation": "required",
                "runner_status": "adaptation_required",
                "expected": None,
                "enabled": False,
            }
        )
    for name in rc.ALL_NEGATIVES:
        cases.append(
            {
                "id": "project:tests/native/c_retained_capturing/negative/"
                + name
                + "/lang.toml",
                "kind": "project",
                "manifest": "tests/native/c_retained_capturing/negative/"
                + name
                + "/lang.toml",
                "backend": "native",
                "operation": "build_native",
                "operations": ["check", "build_native"],
                "support": "existing_runner",
                "adaptation": "required",
                "runner_status": "adaptation_required",
                "expected": None,
                "enabled": False,
            }
        )
    return cases


def test_proposal():
    rows = rc.proposal_rows(fake_corpus())
    check("proposal_20_rows", len(rows) == 20, str(len(rows)))
    check("proposal_all_disabled", all(not row["enabled"] for row in rows))
    by_manifest = {row["manifest"]: row for row in rows}
    python = by_manifest[
        "tests/native/c_retained_capturing/negative/python_unsupported/lang.toml"
    ]
    static_python = by_manifest[
        "tests/native/c_retained_capturing/negative/"
        "python_static_unsupported/lang.toml"
    ]
    for name, row in (("python", python), ("static_python", static_python)):
        check(name + "_backend_python", row["backend"] == "python")
        check(name + "_operation_check", row["operation"] == "check")
        check(name + "_operations_check", row["operations"] == ["check"])
        check(name + "_correction", bool(row.get("correction")))
    native = by_manifest[
        "tests/native/c_retained_capturing/negative/throws/lang.toml"
    ]
    check("proposal_native_unchanged", native["backend"] == "native")
    check(
        "proposal_native_operations_unchanged",
        native["operations"] == ["check", "build_native"],
    )


def test_snapshot():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "a.wz").write_text("a\n")
        (root / "sub").mkdir()
        (root / "sub" / "b.json").write_text("{}\n")
        (root / ".build").mkdir()
        (root / ".build" / "generated.so").write_bytes(b"x")
        snapshot = rc.tree_snapshot(root, (".build",))
        check("snapshot_files", snapshot["files"] == 2, str(snapshot["files"]))
        check(
            "snapshot_excludes_build",
            all(".build" not in name for name in snapshot["map"]["files"]),
        )
        check("snapshot_map_sha", len(snapshot["map_sha256"]) == 64)


def test_counts():
    check("expected_total_70", rc.EXPECTED_COUNTS["total"] == 70)
    check(
        "two_roles_140",
        rc.EXPECTED_COUNTS["total"] * 2 == 140,
    )
    check(
        "public_builds_29",
        rc.EXPECTED_COUNTS["public_builds"] == 29,
    )
    check(
        "public_checks_8",
        rc.EXPECTED_COUNTS["public_checks"] == 8,
    )
    check(
        "woozy_programs_27",
        rc.EXPECTED_COUNTS["woozy_programs"] == 27,
    )
    check(
        "fixed_budgets",
        (rc.BUILD_TIMEOUT, rc.CHECK_TIMEOUT, rc.PROGRAM_TIMEOUT,
         rc.FIXTURE_COMPILE_TIMEOUT, rc.LIFECYCLE_TIMEOUT,
         rc.NM_TIMEOUT, rc.READELF_TIMEOUT, rc.GIT_TIMEOUT)
        == (240, 120, 30, 120, 60, 60, 30, 30),
    )


def test_status_labels():
    check("status_exited", rc.command_status(0, False) == "exited")
    check("status_signal", rc.command_status(-11, False) == "signalled")
    check("status_timeout", rc.command_status(-9, True) == "timed_out")
    check(
        "status_spawn",
        rc.command_status(None, None, "boom") == "spawn_error",
    )


def test_label_pairs():
    pairs = rc.parse_label_pairs(["a=/x", "b=/y=z"], "--extra-input")
    check("label_pairs", pairs == [("a", "/x"), ("b", "/y=z")])
    rejects("label_pairs_missing_equal", rc.parse_label_pairs, ["a"], "--x")
    rejects("label_pairs_empty_label", rc.parse_label_pairs, ["=/x"], "--x")
    rejects("label_pairs_empty_path", rc.parse_label_pairs, ["a="], "--x")


def main():
    block_external_execution()
    try:
        subprocess.Popen(["true"])
    except RuntimeError:
        check("external_execution_blocked", True)
    else:
        check("external_execution_blocked", False, "Popen was not blocked")
    test_json()
    test_process_guard()
    test_documents()
    test_trace()
    test_reference_preflight()
    test_rejections()
    test_loader()
    test_legacy_auto()
    test_proposal()
    test_snapshot()
    test_counts()
    test_status_labels()
    test_label_pairs()
    failed = [entry for entry in CHECKS if entry["status"] == "failed"]
    print(
        json.dumps(
            {
                "checks": len(CHECKS),
                "passed": len(CHECKS) - len(failed),
                "failed": failed,
            },
            indent=2,
            ensure_ascii=False,
        )
    )
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
