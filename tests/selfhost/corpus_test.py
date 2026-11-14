"""Small checks for the selfhost corpus driver's audit and failure paths."""

import argparse
import base64
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--root", type=Path,
                    default=Path(os.environ.get("WOOZY_CORPUS_ROOT", ROOT)))
parser.add_argument("--inventory", type=Path,
                    default=ROOT / "selfhost/corpus.json")
parser.add_argument("--stage0", type=Path)
parser.add_argument("--toolchains", type=Path,
                    default=Path(os.environ.get(
                        "WOOZY_TOOLCHAINS", ROOT.parent / "toolchains")))
options = parser.parse_args()
CORPUS_ROOT = options.root.resolve()
INVENTORY = options.inventory.resolve()
STAGE0 = (options.stage0.resolve() if options.stage0 else
          CORPUS_ROOT / "build/woozy-stdlib-native/woozyc")
spec = importlib.util.spec_from_file_location(
    "corpus", ROOT / "selfhost/corpus.py")
corpus = importlib.util.module_from_spec(spec)
spec.loader.exec_module(corpus)
if options.toolchains:
    corpus.DEFAULT_LLVM = options.toolchains.resolve() / "llvm-22.1.8"
    corpus.DEFAULT_STRACE = (options.toolchains.resolve() /
                             "strace-7.2/usr/bin/strace")
    corpus.DEFAULT_PYTHON = (options.toolchains.resolve() /
                             "t10-python-cpu/venv/bin/python")
    corpus.DEFAULT_LIBXML = (options.toolchains.resolve() /
                             "libxml2-legacy-2.13.9/usr/lib")


def document(path="/tmp/run/input.wz", message="expected identifier",
             status="error"):
    if status == "ok":
        return {
            "schema_version": 1,
            "compiler_version": "0.1.0-bootstrap (compiler-dev/1)",
            "source_revision": "cli-request",
            "status": "ok",
            "diagnostics": [],
        }
    return {
        "schema_version": 1,
        "compiler_version": "0.1.0-bootstrap (compiler-dev/1)",
        "source_revision": "cli-request",
        "status": "error",
        "diagnostics": [{
            "code": "E_PARSE_EXPECTED_TOKEN",
            "phase": "parse",
            "severity": "error",
            "message": message,
            "primary": {"path": path, "start_byte": 27, "end_byte": 28},
            "related": [],
            "fixits": [],
            "notes": [],
        }],
    }


def expect_error(function, *args, **kwargs):
    try:
        function(*args, **kwargs)
    except (corpus.CorpusError, corpus.TraceError):
        return
    raise AssertionError(f"accepted invalid input: {function.__name__}")


def check_diagnostics_and_normalization():
    left = document("/tmp/run/input.wz")
    right = document("/tmp/other/input.wz")
    right["compiler_version"] = "different-version"
    normalized_left = corpus.normalize_json(left, Path("/tmp/run"), [])
    normalized_right = corpus.normalize_json(right, Path("/tmp/other"), [])
    assert normalized_left == normalized_right
    assert "compiler_version" not in normalized_left
    assert normalized_left["diagnostics"][0]["primary"]["start_byte"] == 27
    changed = copy.deepcopy(normalized_right)
    changed["diagnostics"][0]["message"] = "different text"
    assert corpus.json_diff(normalized_left, changed)
    changed = copy.deepcopy(normalized_right)
    changed["diagnostics"][0]["primary"]["end_byte"] = 99
    assert corpus.json_diff(normalized_left, changed)
    corpus.validate_diagnostic_document(left)
    for bad in ("missing-primary", "bad-span", "ok-with-error"):
        broken = copy.deepcopy(left)
        if bad == "missing-primary":
            del broken["diagnostics"][0]["primary"]
        elif bad == "bad-span":
            broken["diagnostics"][0]["primary"]["start_byte"] = 99
        else:
            broken["status"] = "ok"
        expect_error(corpus.validate_diagnostic_document, broken)
    expect_error(corpus.parse_diagnostic_stdout, b"{not json")
    expect_error(corpus.parse_diagnostic_stdout, b'{"status":"ok"}')

    accept = {"status": "ok", "diagnostics": []}
    reject = document()
    assert not corpus.check_expected(
        {"operation": "check", "expected": {"exit": 0, "status": "ok"}},
        accept, 0, b"", {"published": []})
    assert corpus.check_expected(
        {"operation": "check", "expected": {"exit": 0, "status": "ok"}},
        accept, 1, b"", {"published": []})
    assert corpus.check_expected(
        {"operation": "check", "expected": {"status": "error", "exit": 1}},
        reject, 0, b"", {"published": []})
    assert corpus.check_expected(
        {"operation": "build_native", "expected": {"status": "ok"}},
        accept, 0, b"", {"published": [], "required": ["program"]})
    assert corpus.check_expected(
        {"operation": "build_native", "expected": {"status": "error"}},
        reject, 1, b"", {"published": ["program"], "required": []})
    assert not corpus.check_expected(
        {"operation": "check",
         "expected": {"status": "error", "code": "E_PARSE_EXPECTED_TOKEN"}},
        reject, 1, b"", {"published": []})
    assert any("expected diagnostic code" in failure
               for failure in corpus.check_expected(
                   {"operation": "check",
                    "expected": {"status": "error", "code": "E_TYPE_MISMATCH"}},
                   reject, 1, b"", {"published": []}))


def check_strict_json_types():
    integer = document()
    boolean = copy.deepcopy(integer)
    boolean["schema_version"] = True
    expect_error(corpus.validate_diagnostic_document, boolean)
    assert corpus.json_diff(integer, boolean)
    floating = copy.deepcopy(integer)
    floating["schema_version"] = 1.0
    expect_error(corpus.validate_diagnostic_document, floating)
    assert corpus.json_diff(integer, floating)

    def run_record(value):
        return {
            "trace": {"status": "ok"},
            "exit": 0,
            "timed_out": False,
            "_normalized_stderr": b"",
            "_normalized": value,
            "_execution": None,
            "artifacts": {"published": []},
        }

    failures, evidence = corpus.compare_runs(
        None, run_record(integer), run_record(boolean))
    assert any("JSON" in failure for failure in failures), failures
    assert evidence["json_diff"]


def check_trace_audit():
    with tempfile.TemporaryDirectory(prefix="woozy-corpus-trace-") as directory:
        work = Path(directory)
        candidate = work / "stage1/woozyc"
        clang = work / "llvm/clang"
        trace = work / "exec.log"
        allowed = {str(candidate.resolve()), str(clang.resolve())}
        trace.write_text(
            f'1 execve("{candidate}", ["woozyc"], 0x0) = 0\n'
            f'2 execveat(AT_FDCWD, "{clang}", ["clang"], 0x0, 0) = 0\n')
        assert corpus.audit_trace(trace, allowed)["status"] == "ok"
        trace.write_text('1 execveat(3, "", ["helper"], 0x0, AT_EMPTY_PATH) = 0\n')
        expect_error(corpus.parse_trace, trace)
        trace.write_text('1 execve("./clang", ["clang"], 0x0) = 0\n')
        expect_error(corpus.parse_trace, trace)
        trace.write_text(
            '1 execveat(AT_FDCWD, "./clang", ["clang"], 0x0, 0) = 0\n')
        expect_error(corpus.parse_trace, trace)
        trace.write_text('1 execve(0x7fff, ["clang"], 0x0) = 0\n')
        expect_error(corpus.parse_trace, trace)
        trace.write_text('1 execveat(3, 0x7fff, ["clang"], 0x0, 0) = 0\n')
        expect_error(corpus.parse_trace, trace)
        trace.write_text(
            f'1 execve("{candidate}", ["woozyc"], 0x0) = 0\n'
            '2 execve("/old/lang_native_helper", ["helper"], 0x0) = 0\n')
        expect_error(corpus.audit_trace, trace, allowed)
        trace.write_text("+++ exited with 0 +++\n")
        expect_error(corpus.parse_trace, trace)


def check_seccomp_exec_trace():
    """Real exec controls for the fixed strace 7.2 on Linux x86_64."""
    with tempfile.TemporaryDirectory(prefix="woozy-corpus-seccomp-") as directory:
        work = Path(directory)
        script = work / "child.py"
        script.write_text(
            "import ctypes, os, sys\n"
            "pid = os.fork()\n"
            "if pid == 0:\n"
            "    if sys.argv[1] == 'execve':\n"
            "        os.execv('/usr/bin/true', ['/usr/bin/true'])\n"
            "    libc = ctypes.CDLL(None, use_errno=True)\n"
            "    libc.execveat.argtypes = [ctypes.c_int, ctypes.c_char_p,\n"
            "        ctypes.POINTER(ctypes.c_char_p),\n"
            "        ctypes.POINTER(ctypes.c_char_p), ctypes.c_int]\n"
            "    argv = (ctypes.c_char_p * 2)(b'/usr/bin/true', None)\n"
            "    envp = (ctypes.c_char_p * 1)(None)\n"
            "    libc.execveat(-100, b'/usr/bin/true', argv, envp, 0)\n"
            "    raise OSError(ctypes.get_errno(), 'execveat failed')\n"
            "_, status = os.waitpid(pid, 0)\n"
            "raise SystemExit(os.waitstatus_to_exitcode(status))\n")
        python = Path(sys.executable).resolve()
        true = Path("/usr/bin/true").resolve()
        allowed = {str(python), str(true)}
        for call in ("execve", "execveat"):
            trace = work / f"{call}.log"
            result = corpus._run_process_impl([
                str(corpus.DEFAULT_STRACE), "--seccomp-bpf", "-f", "-qq",
                "-s", "16384", "-e", "trace=execve,execveat", "-o", str(trace),
                str(python), "-B", str(script), call,
            ], work, None, 10)
            assert result["exit"] == 0, result
            assert result["timed_out"] is False, result
            assert result["cleanup_complete"] is True, result
            assert result["abandoned_pipes"] is False, result
            assert result["stdout"] == result["stderr"] == b"", result
            assert f"{call}(" in trace.read_text(), call
            assert corpus.audit_trace(trace, allowed)["observed"] == sorted(allowed)
            expect_error(corpus.audit_trace, trace, {str(python)})
    print("PASS: seccomp execve/execveat records and extra-tool rejection")


def check_process_stdin_file():
    """Optional stdin file replays exact bytes; the default keeps fd 0."""
    with tempfile.TemporaryDirectory(prefix="woozy-corpus-stdin-") as directory:
        work = Path(directory)
        payload = "中😀\r\n\nlast\0end".encode()
        source = work / "stdin.bin"
        source.write_bytes(payload)
        python = Path(sys.executable).resolve()
        echo = ("import sys; data=sys.stdin.buffer.read(); "
                "sys.stdout.buffer.write(str(len(data)).encode()+b':'+data)")
        result = corpus._run_process_impl(
            [python, "-c", echo], work, dict(os.environ), 10, source)
        assert result["exit"] == 0, result
        assert result["timed_out"] is False, result
        assert result["cleanup_complete"] is True, result
        assert result["abandoned_pipes"] is False, result
        assert result["stdout"] == (
            str(len(payload)).encode() + b":" + payload), result["stdout"]
        assert result["stderr"] == b"", result
        assert source.read_bytes() == payload

        inherited = work / "inherited.bin"
        inherited.write_bytes(b"inherited\r\n\0tail")
        saved = os.dup(0)
        handle = os.open(inherited, os.O_RDONLY)
        try:
            os.dup2(handle, 0)
        finally:
            os.close(handle)
        try:
            default = corpus._run_process_impl(
                [python, "-c", "import sys; "
                 "sys.stdout.buffer.write(sys.stdin.buffer.read())"],
                work, dict(os.environ), 10)
            isolated = corpus._run_process_impl(
                [python, "-c", "import sys; "
                 "sys.stdout.buffer.write(sys.stdin.buffer.read())"],
                work, dict(os.environ), 10, source)
        finally:
            os.dup2(saved, 0)
            os.close(saved)
        assert default["exit"] == 0, default
        assert default["stdout"] == inherited.read_bytes(), default
        assert isolated["exit"] == 0, isolated
        assert isolated["stdout"] == payload, isolated
    print("PASS: stdin file keeps UTF-8/NUL/EOF bytes; default inherits fd 0")


def check_inventory():
    inventory = corpus.load_inventory(INVENTORY)
    report = corpus.check_inventory(CORPUS_ROOT, inventory)
    assert not report["errors"], report["errors"][:10]
    coverage = report["coverage"]
    source_count = sum(entry["path"].endswith(".wz")
                       for entry in inventory["files"])
    manifest_count = sum(entry["role"] == "manifest"
                         for entry in inventory["files"])
    assert coverage["source_files"] == source_count
    assert coverage["manifests"] == manifest_count
    assert coverage["assigned_sources"] == source_count
    assert coverage["assigned_manifests"] == manifest_count
    assert not coverage["unassigned_sources"]
    assert not coverage["unassigned_manifests"]

    mutated = copy.deepcopy(inventory)
    for entry in mutated["files"]:
        if entry.get("role") == "woozy_source":
            entry["assignment"] = []
            missing = entry["path"]
            break
    else:
        raise AssertionError("inventory has no source file")
    mutated_report = corpus.check_inventory(CORPUS_ROOT, mutated)
    assert any("no case assignment" in error
               for error in mutated_report["errors"]), mutated_report["errors"]
    assert missing in mutated_report["coverage"]["unassigned_sources"]

    removed = copy.deepcopy(inventory)
    removed_path = removed["files"].pop(0)["path"]
    removed_report = corpus.check_inventory(CORPUS_ROOT, removed)
    assert any("not in inventory" in error
               for error in removed_report["errors"]), removed_report["errors"]
    assert removed_path not in {entry["path"] for entry in removed["files"]}

    bad_runner = copy.deepcopy(inventory)
    bad_runner["runners"][0]["status"] = "not-a-status"
    bad_runner_report = corpus.check_inventory(CORPUS_ROOT, bad_runner)
    assert any("runner status" in error
               for error in bad_runner_report["errors"]), bad_runner_report["errors"]
    assert bad_runner_report["coverage"]["complete"] is False


def check_public_cli_expectations():
    inventory = corpus.load_inventory(INVENTORY)
    cases = {case["id"]: case for case in inventory["cases"]}
    evidenced = [case for case in inventory["cases"]
                 if case.get("evidence")]
    assert evidenced
    assert any(case.get("enabled", False) for case in evidenced)
    for case in inventory["cases"]:
        if not case.get("enabled"):
            continue
        if case.get("run_expectation", {}).get("status") == "unrecorded" \
                and case.get("evidence"):
            raise AssertionError(f"placeholder expectation: {case['id']}")
        assert case.get("expected"), case["id"]
        assert not corpus.run_expectation_errors(case), case["id"]
        expected = case["expected"]
        if expected.get("status") == "ok" and case["operation"] in (
                "build_native", "build_python"):
            assert corpus.recorded_run_expectation(case), case["id"]
        if expected.get("status") == "error" and case.get("evidence"):
            assert expected.get("exit") == 1, case["id"]
            assert expected.get("no_stderr") is True, case["id"]
            assert "run_expectation" not in case, case["id"]

    # The six former unrecorded panic cases assert real Panic; the two
    # executable accepts and six check-only accepts retain their runner rules.
    panics = {
        "project:tests/backend_common/fixtures/panic-division-by-zero/lang.toml":
            "LANG_NATIVE_PANIC:v1:division-by-zero:",
        "project:tests/backend_common/fixtures/panic-dynamic-conversion/lang.toml":
            "LANG_NATIVE_PANIC:v1:integer-conversion:",
        "project:tests/backend_common/fixtures/panic-invalid-shift/lang.toml":
            "LANG_NATIVE_PANIC:v1:invalid-shift:",
        "project:tests/backend_common/fixtures/panic-negative-shift/lang.toml":
            "LANG_NATIVE_PANIC:v1:invalid-shift:",
        "project:tests/cleanup/fixtures/case-10/lang.toml":
            "LANG_NATIVE_PANIC:v1:division-by-zero:",
        "project:tests/ownership/fixtures/panic-vec-index/lang.toml":
            "LANG_NATIVE_PANIC:v1:vec-index:",
    }
    for case_id, prefix in panics.items():
        case = cases[case_id]
        assert case["operation"] == "build_native", case
        expectation = case["run_expectation"]
        assert expectation["status"] == "panic", case
        assert expectation["exit"] == 101, case
        assert expectation["panic_stderr_prefix"] == prefix, case
    executed_accepts = (
        "project:tests/modules/fixtures/cross-package-public/lang.toml",
        "project:tests/modules/fixtures/same-package-mutual-recursion/lang.toml",
    )
    for case_id in executed_accepts:
        case = cases[case_id]
        assert case["operation"] == "build_native", case
        assert case["expected"]["status"] == "ok", case
        assert case["run_expectation"]["exit"] == 0, case
        assert case["run_expectation"]["stdout_base64"] == "", case
        assert case["run_expectation"]["stderr_base64"] == "", case
    static_accepts = (
        "project:tests/modules/fixtures/nested-shadow-and-for-init/lang.toml",
        "project:tests/modules/fixtures/self-in-context/lang.toml",
        "project:tests/sema/fixtures/call-labels-and-function-value-accept/lang.toml",
        "project:tests/sema/fixtures/inout-writable-var-accept/lang.toml",
        "project:tests/sema/fixtures/numeric-boundaries-accept/lang.toml",
        "project:tests/sema/fixtures/var-assignment-accept/lang.toml",
    )
    for case_id in static_accepts:
        case = cases[case_id]
        assert case["operation"] == "check", case
        assert case["expected"]["status"] == "ok", case
        assert "run_expectation" not in case, case

    not_ready = {
        "tests/generics/fixtures/cross-package-alias/vendor/dep": "component",
        "tests/generics/nested-patterns/literal-cross-package/vendor/dep": "component",
    }
    for manifest, adaptation in not_ready.items():
        case = cases["project:" + manifest + "/lang.toml"]
        assert case["enabled"] is False, case
        assert case["adaptation"] == adaptation, case
        assert case["expected"] is None, case
        assert case["runner_status"] == (
            "component" if adaptation == "component" else "adaptation_required"), case

    accepted_generics = {
        "tests/generics/inout/constrained/reject": 0,
        "tests/generics/move/constrained-reject": 0,
        "tests/generics/unbound-method-values/reject-capture": 7,
    }
    for manifest, exit_code in accepted_generics.items():
        case = cases["project:" + manifest + "/lang.toml"]
        assert case["enabled"] is True, case
        assert case["adaptation"] == "ready", case
        assert case["support"] == "public_cli", case
        assert case["runner"] == "tests/native/run_generics.py", case
        assert case["runner_status"] == "adaptation_required", case
        assert case["expected"] == {
            "status": "ok", "exit": 0, "build_status": "ok",
            "no_stderr": True}, case
        assert case["run_expectation"] == {
            "exit": exit_code, "stdout_base64": "", "stderr_base64": ""}, case

    # Published nonzero and panic expectations survive the strict checker.
    explicit = cases["project:tests/generics/fixtures/explicit/lang.toml"]
    assert explicit["run_expectation"]["exit"] == 7, explicit
    assert corpus.check_run_expectation(explicit, {
        "exit": 0, "timed_out": False, "cleanup_complete": True,
        "_stdout": b"", "_stderr": b"",
    })
    panic = cases["project:tests/generics/indexed-calls/panic-index/lang.toml"]
    assert corpus.check_run_expectation(panic, {
        "exit": 101, "timed_out": False, "cleanup_complete": True,
        "_stdout": b"", "_stderr": b"LANG_NATIVE_PANIC:v1:vec-index:9:1:2\n",
    }) == []
    assert corpus.check_run_expectation(panic, {
        "exit": 101, "timed_out": False, "cleanup_complete": True,
        "_stdout": b"", "_stderr": b"wrong panic\n",
    })
    positive = cases["project:tests/generics/aggregate-inout/reject-alias/field-place/lang.toml"]
    reject = cases["project:tests/generics/aggregate-inout/reject-alias/duplicate/lang.toml"]
    assert positive["expected"]["status"] == "ok", positive
    assert positive["run_expectation"]["exit"] == 0, positive
    assert reject["expected"]["status"] == "error", reject
    assert reject["operation"] == "build_native", reject
    check_only = cases["project:tests/generics/generic-traits/reject-own-generic/lang.toml"]
    assert check_only["operation"] == "check", check_only
    assert check_only["expected"]["status"] == "ok", check_only


def check_run_expectations():
    assert corpus.run_expectation_errors(
        {"run_expectation": {"todo": "later"}})
    assert corpus.run_expectation_errors({"run_expectation": {}})
    assert corpus.run_expectation_errors(
        {"run_expectation": {"status": "return"}})
    assert corpus.run_expectation_errors(
        {"run_expectation": {"status": "panic", "exit": 0}})
    assert not corpus.recorded_run_expectation(
        {"run_expectation": {"todo": "later"}})
    valid = {"run_expectation": {
        "exit": 99,
        "stdout_base64": base64.b64encode(b"expected").decode("ascii"),
        "stderr_base64": "",
    }}
    assert corpus.recorded_run_expectation(valid)
    execution = {
        "exit": 99, "timed_out": False, "cleanup_complete": True,
        "_stdout": b"expected", "_stderr": b"",
    }
    assert not corpus.check_run_expectation(valid, execution)
    wrong = dict(execution, exit=0, _stdout=b"wrong")
    assert corpus.check_run_expectation(valid, wrong)
    timed_out = {
        "run_expectation": {"status": "panic", "exit": 101},
    }
    assert corpus.check_run_expectation(
        timed_out, {"exit": -9, "timed_out": True,
                    "cleanup_complete": True, "_stdout": b"",
                    "_stderr": b""})
    signalled = dict(execution, exit=-9, timed_out=False)
    assert corpus.check_run_expectation(valid, signalled)


def check_execution_repetition_validation():
    runnable = {
        "id": "repeat-validation",
        "operation": "build_native",
        "expected": {"status": "ok"},
    }
    for value in (1, 2, 3):
        case = dict(runnable, execution_repetitions=value)
        assert not corpus.execution_configuration_errors(case), case
        assert corpus.execution_repetitions(case) == value
    for value in (True, False, 0, -1, 1.0, "3", None):
        case = dict(runnable, execution_repetitions=value)
        assert corpus.execution_configuration_errors(case), (value, case)
        expect_error(corpus.execution_repetitions, case)
    for case in (
            dict(runnable, operation="check", execution_repetitions=2),
            dict(runnable, operation="syntax", execution_repetitions=2),
            dict(runnable, expected={"status": "error"},
                 execution_repetitions=2),
            dict(runnable, expected=None, execution_repetitions=2)):
        assert corpus.execution_configuration_errors(case), case
    assert corpus.execution_repetitions({"id": "default"}) == 1
    classified = {
        "id": "repeat-classification", "enabled": False,
        "support": "public_cli", "adaptation": "ready",
        "operation": "build_native", "expected": {"status": "ok"},
        "execution_repetitions": 0,
    }
    assert any("execution_repetitions" in error
               for error in corpus._case_classification_errors(classified))
    classified["execution_repetitions"] = 2
    classified["execution_timeout_seconds"] = float("inf")
    assert any("execution_timeout_seconds" in error
               for error in corpus._case_classification_errors(classified))

    for value in (60, 60.0):
        case = dict(runnable, execution_timeout_seconds=value)
        assert not corpus.execution_configuration_errors(case), case
        assert corpus.execution_timeout(case, 7) == 60.0
    for value in (True, False, 0, -1, float("nan"), float("inf"),
                  float("-inf"), "60", None, 10**500, -(10**500)):
        case = dict(runnable, execution_timeout_seconds=value)
        assert corpus.execution_configuration_errors(case), (value, case)
        expect_error(corpus.execution_timeout, case, 7)
    assert corpus.execution_timeout({}, 7) == 7.0
    assert corpus.execution_configuration_errors(
        dict(runnable, operation="check", execution_timeout_seconds=2))


def check_large_timeout_configuration():
    decoded = json.loads(
        '{"execution_timeout_seconds": 1' + "0" * 500 + "}")
    huge = decoded["execution_timeout_seconds"]
    assert type(huge) is int and huge == 10**500
    runnable = {
        "id": "huge-timeout",
        "operation": "build_native",
        "expected": {"status": "ok"},
        "execution_timeout_seconds": huge,
    }
    errors = corpus.execution_configuration_errors(runnable)
    assert any("execution_timeout_seconds" in error for error in errors), errors
    try:
        corpus.execution_timeout(runnable, 7)
    except corpus.CorpusError as error:
        assert "execution_timeout_seconds" in str(error)
    except OverflowError as error:
        raise AssertionError(f"raw OverflowError leaked: {error}") from error
    else:
        raise AssertionError("accepted a timeout that cannot be a finite float")

    classified = {
        "id": "huge-timeout-classification",
        "enabled": False,
        "support": "public_cli",
        "adaptation": "ready",
        "operation": "build_native",
        "expected": {"status": "ok"},
        "execution_timeout_seconds": huge,
    }
    classification_errors = corpus._case_classification_errors(classified)
    assert any("execution_timeout_seconds" in error
               for error in classification_errors), classification_errors
    coverage = corpus.coverage_report(
        {"cases": [classified], "files": [], "runners": []})
    assert classified["id"] in coverage["invalid_cases"], coverage["invalid_cases"]

    for value in (20, 60, 20.0, 60.0):
        case = {
            "id": "normal-timeout",
            "operation": "build_native",
            "expected": {"status": "ok"},
            "execution_timeout_seconds": value,
        }
        assert not corpus.execution_configuration_errors(case), case
        assert corpus.execution_timeout(case, 7) == float(value)


def execution_record(index, exit_code=0, stdout=b"", stderr=b"",
                     timed_out=False, cleanup_complete=True):
    return {
        "index": index,
        "exit": exit_code,
        "timed_out": timed_out,
        "cleanup_complete": cleanup_complete,
        "_stdout": stdout,
        "_stderr": stderr,
    }


def check_execution_repetition_expectations():
    case = {
        "id": "repeat-expectation",
        "operation": "build_native",
        "expected": {"status": "ok"},
        "execution_repetitions": 3,
        "run_expectation": {
            "exit": 0, "stdout_base64": "", "stderr_base64": "",
        },
    }
    good = [execution_record(index) for index in range(3)]
    assert not corpus.check_run_expectations(case, good)
    for changed in (
            execution_record(1, exit_code=7),
            execution_record(1, stdout=b"unexpected"),
            execution_record(1, stderr=b"unexpected"),
            execution_record(1, timed_out=True),
            execution_record(1, cleanup_complete=False),
            execution_record(1, exit_code=-9)):
        failures = corpus.check_run_expectations(
            case, [good[0], changed, good[2]])
        assert any("execution[1]" in failure for failure in failures), failures
    failures = corpus.check_run_expectations(case, good[:2])
    assert any("expected 3" in failure and "got 2" in failure
               for failure in failures), failures
    failures = corpus.check_run_expectations(case, good + [execution_record(3)])
    assert any("expected 3" in failure and "got 4" in failure
               for failure in failures), failures

    panic = {
        "id": "repeat-panic",
        "operation": "build_native",
        "expected": {"status": "ok"},
        "execution_repetitions": 2,
        "run_expectation": {
            "status": "panic", "exit": 101, "stdout_base64": "",
            "panic_stderr_prefix": "LANG_NATIVE_PANIC:v1:division-by-zero:",
        },
    }
    valid_panic = execution_record(
        0, exit_code=101, stderr=b"LANG_NATIVE_PANIC:v1:division-by-zero:9:1\n")
    wrong_panic = execution_record(1, exit_code=101, stderr=b"wrong panic\n")
    assert not corpus.check_run_expectations(
        panic, [valid_panic, dict(valid_panic, index=1)])
    failures = corpus.check_run_expectations(
        panic, [valid_panic, wrong_panic])
    assert any("execution[1]" in failure and "start with" in failure
               for failure in failures), failures

    count_only = {
        "id": "repeat-count-only",
        "operation": "build_native",
        "expected": {"status": "ok"},
        "execution_repetitions": 2,
    }
    assert corpus.check_run_expectations(count_only, good[:1])


def check_execution_repetition_comparison():
    def run_record(executions):
        return {
            "trace": {"status": "ok"},
            "exit": 0,
            "timed_out": False,
            "_normalized_stderr": b"",
            "_normalized": document(status="ok"),
            "_executions": executions,
            "artifacts": {"published": ["program"]},
        }

    reference = run_record([execution_record(index) for index in range(3)])
    candidate = run_record([
        execution_record(0),
        execution_record(1, exit_code=7),
        execution_record(2),
    ])
    failures, _ = corpus.compare_runs(None, reference, candidate)
    assert any("execution[1]" in failure and "exit" in failure
               for failure in failures), failures

    candidate = run_record([
        execution_record(0),
        execution_record(1),
        execution_record(2, stdout=b"extra"),
    ])
    failures, evidence = corpus.compare_runs(None, reference, candidate)
    assert any("execution[2]" in failure and "stdout" in failure
               for failure in failures), failures
    assert evidence["program_stdout_execution_2"]["candidate"] == "extra"

    candidate = run_record([
        execution_record(0),
        execution_record(1, timed_out=True),
        execution_record(2),
    ])
    assert any("execution[1]" in failure and "timed_out" in failure
               for failure in corpus.compare_runs(
                   None, reference, candidate)[0])

    candidate = run_record([
        execution_record(0),
        execution_record(1, cleanup_complete=False),
        execution_record(2),
    ])
    assert any("execution[1]" in failure and "cleanup_complete" in failure
               for failure in corpus.compare_runs(
                   None, reference, candidate)[0])

    candidate = run_record([execution_record(0), execution_record(1)])
    failures, _ = corpus.compare_runs(None, reference, candidate)
    assert any("execution count differs" in failure for failure in failures), failures

    # A record from the old single-execution schema still compares as one run.
    old_left = run_record([])
    old_right = run_record([])
    del old_left["_executions"]
    del old_right["_executions"]
    old_left["_execution"] = execution_record(0)
    old_right["_execution"] = execution_record(0)
    assert not corpus.compare_runs(None, old_left, old_right)[0]


def write_fake_compiler(work, name, behaviors):
    """Write a fake compiler that publishes a scripted Native program."""
    directory = work / name
    directory.mkdir(parents=True, exist_ok=True)
    program_source = (
        "#!" + str(Path(sys.executable).resolve()) + "\n"
        "import json, os, pathlib, signal, subprocess, sys, time\n"
        "behaviors = " + repr(behaviors) + "\n"
        "state = pathlib.Path(__file__).with_name('fake-run-count.json')\n"
        "count = json.loads(state.read_text()) if state.exists() else 0\n"
        "count += 1\n"
        "state.write_text(json.dumps(count))\n"
        "behavior = behaviors[min(count - 1, len(behaviors) - 1)] "
        "if behaviors else 'ok'\n"
        "if behavior == 'exit7':\n"
        "    print('second-run-noise', flush=True)\n"
        "    raise SystemExit(7)\n"
        "if behavior == 'stdout':\n"
        "    print('unexpected stdout', flush=True)\n"
        "if behavior == 'stderr':\n"
        "    print('unexpected stderr', file=sys.stderr, flush=True)\n"
        "if behavior == 'signal':\n"
        "    os.kill(os.getpid(), signal.SIGKILL)\n"
        "if behavior == 'timeout':\n"
        "    time.sleep(5)\n"
        "if behavior == 'cleanup':\n"
        "    subprocess.Popen([sys.executable, '-c', "
        "'import time; time.sleep(1.2)'], start_new_session=True)\n"
        "    time.sleep(5)\n"
        "if behavior == 'remove':\n"
        "    pathlib.Path(__file__).unlink()\n"
        "raise SystemExit(0)\n"
    )
    compiler = directory / "woozyc"
    compiler.write_text(
        "#!" + str(Path(sys.executable).resolve()) + "\n"
        "import json, pathlib, sys\n"
        "args = sys.argv[1:]\n"
        "out = pathlib.Path(args[args.index('--out-dir') + 1])\n"
        "out.mkdir(parents=True, exist_ok=True)\n"
        "program_source = " + repr(program_source) + "\n"
        "program = out / 'program'\n"
        "program.write_text(program_source)\n"
        "program.chmod(0o700)\n"
        "print(json.dumps({'schema_version': 1, "
        "'compiler_version': 'fake', 'source_revision': 'fake', "
        "'status': 'ok', 'diagnostics': []}))\n"
    )
    compiler.chmod(0o700)
    return compiler


def fake_repeat_case(case_id, repetitions=None):
    case = {
        "id": case_id,
        "operation": "build_native",
        "manifest": "lang.toml",
        "opt": [0],
        "expected": {"status": "ok", "build_status": "ok",
                     "no_stderr": True},
        "run_expectation": {
            "exit": 0, "stdout_base64": "", "stderr_base64": "",
        },
        "execution_timeout_seconds": 1.0,
    }
    if repetitions is not None:
        case["execution_repetitions"] = repetitions
    return case


def check_execution_fixture_repetitions():
    with tempfile.TemporaryDirectory(prefix="woozy-corpus-repeat-") as directory:
        work = Path(directory)
        context = probe_context(work)
        context["timeout"] = 2.0
        ok = write_fake_compiler(work, "fake-ok", ["ok", "ok", "ok"])
        bad_exit = write_fake_compiler(
            work, "fake-bad-exit", ["ok", "exit7", "ok"])
        bad_stdout = write_fake_compiler(
            work, "fake-bad-stdout", ["ok", "ok", "stdout"])

        case = fake_repeat_case("fake-repeat", 3)
        record = corpus.run_case(
            case, ok, [], work / "out-repeat", context)
        assert record["status"] == "passed", record
        reference = record["variants"][0]["reference"]
        assert reference["execution_repetitions"] == 3
        assert len(reference["executions"]) == 3
        assert reference["execution"] == reference["executions"][0]
        run_dir = work / "out-repeat" / reference["directory"]
        assert (run_dir / "out" / "program").is_file()
        assert "program" in reference["artifacts"]["published"]
        seen_paths = set()
        for index, execution in enumerate(reference["executions"]):
            assert execution["index"] == index, execution
            assert execution["exit"] == 0, execution
            assert execution["timed_out"] is False, execution
            assert execution["cleanup_complete"] is True, execution
            assert execution["abandoned_pipes"] is False, execution
            assert execution["trace"]["status"] == "ok", execution
            for key in ("stdout_path", "stderr_path", "trace_path"):
                path = execution[key]
                assert path not in seen_paths, (path, seen_paths)
                seen_paths.add(path)
                assert (run_dir / path).is_file(), (run_dir, path)
            assert (run_dir / execution["directory"] / "execute.json").is_file()
            stored = json.loads((run_dir / execution["directory"] /
                                 "execute.json").read_text())
            assert stored["index"] == index
            assert stored["argv"] == execution["argv"]
        assert len(seen_paths) == 9, seen_paths

        case = fake_repeat_case("fake-bad", 3)
        record = corpus.run_case(
            case, ok, [bad_exit, bad_stdout], work / "out-bad", context)
        assert record["status"] == "failed", record
        reference = record["variants"][0]["reference"]
        assert reference["status"] == "passed", reference
        first, second = record["variants"][0]["candidates"]
        assert first["status"] == "failed", first
        assert second["status"] == "failed", second
        assert any("execution[1]" in failure and "exit 7" in failure
                   for failure in first["expected_failures"]), first
        assert any("execution[1]" in failure and "exit" in failure
                   for failure in first["comparison_failures"]), first
        assert any("execution[2]" in failure and "stdout" in failure
                   for failure in second["expected_failures"]), second
        assert any("execution[2]" in failure and "stdout" in failure
                   for failure in second["comparison_failures"]), second
        assert "program_stdout_execution_2" in second["evidence"], second
        bad_exit_dir = work / "out-bad" / first["run"]["directory"]
        bad_stdout_dir = work / "out-bad" / second["run"]["directory"]
        assert (bad_exit_dir / first["run"]["executions"][0]["stdout_path"]
                ).read_bytes() == b""
        assert (bad_exit_dir / first["run"]["executions"][1]["stdout_path"]
                ).read_bytes() == b"second-run-noise\n"
        assert (bad_stdout_dir / second["run"]["executions"][2]["stdout_path"]
                ).read_bytes() == b"unexpected stdout\n"

        case = fake_repeat_case("fake-single")
        record = corpus.run_case(
            case, ok, [], work / "out-single", context)
        assert record["status"] == "passed", record
        reference = record["variants"][0]["reference"]
        assert reference["execution_repetitions"] == 1
        assert len(reference["executions"]) == 1
        assert reference["execution"] == reference["executions"][0]
        single_dir = work / "out-single" / reference["directory"]
        assert not (single_dir / "executions" / "1").exists()

        case = fake_repeat_case("fake-missing", 3)
        original = corpus._execute_artifact

        def skip_third(*args, **kwargs):
            index = kwargs.get("index", args[-1] if args else None)
            if index == 2:
                return None
            return original(*args, **kwargs)

        corpus._execute_artifact = skip_third
        try:
            record = corpus.run_case(
                case, ok, [], work / "out-missing", context)
        finally:
            corpus._execute_artifact = original
        assert record["status"] == "failed", record
        reference = record["variants"][0]["reference"]
        assert len(reference["executions"]) == 2, reference
        failures = "\n".join(record["variants"][0]["reference_failures"])
        assert "program execution 2 did not run" in failures, failures
        assert "program executed 2 of 3" in failures, failures
        assert "expected 3 program executions, got 2" in failures, failures
        missing_dir = work / "out-missing" / reference["directory"]
        assert (missing_dir / "executions" / "1").is_dir()
        assert not (missing_dir / "executions" / "2").exists()


def write_fixture(work, name, body):
    path = work / name
    path.write_text("#!" + str(Path(sys.executable).resolve()) + "\n" + body)
    path.chmod(0o700)
    return path


def probe_context(work):
    project = work / "project"
    (project / "src").mkdir(parents=True, exist_ok=True)
    (project / "lang.toml").write_text(
        'schema_version=1\n[package]\nname="probe"\n'
        'source_root="src"\nentry="app"\n'
        '[build]\nbackend="native"\nword_bits=64\n')
    (project / "src/main.wz").write_text("package app; func main() {}\n")
    return {
        "root": project,
        "tools": {},
        "reference_extra": [],
        "strace": corpus.DEFAULT_STRACE.resolve(),
        "python": corpus.DEFAULT_PYTHON.resolve(),
        "env": dict(os.environ, PYTHONDONTWRITEBYTECODE="1"),
        "timeout": 0.2,
        "roots": [project],
    }


def run_probe_case(work, name, compiler, case):
    record = corpus.run_case(
        case, compiler, [compiler], work / name, probe_context(work))
    return record


def check_timeout_signal_artifacts_and_cleanup():
    error_json = json.dumps(document())
    ok_json = json.dumps(document(status="ok"))
    with tempfile.TemporaryDirectory(prefix="woozy-corpus-failures-") as directory:
        work = Path(directory)
        source = base64.b64encode(b"package x; ?").decode("ascii")

        hang = write_fixture(
            work, "reject_then_hang",
            "import time\nprint(" + repr(error_json) + ",flush=True)\n"
            "time.sleep(5)\n")
        case = {"id": "reject-then-hang", "operation": "syntax",
                "source_base64": source, "expected": {"status": "error"}}
        record = run_probe_case(work, "compiler-timeout", hang, case)
        assert record["status"] == "failed", record
        variant = record["variants"][0]
        assert variant["reference"]["timed_out"] is True
        assert variant["candidates"][0]["run"]["timed_out"] is True
        assert variant["reference"]["exit"] == -9

        selfkill = write_fixture(
            work, "reject_then_selfkill",
            "import os,signal\nprint(" + repr(error_json) + ",flush=True)\n"
            "os.kill(os.getpid(),signal.SIGKILL)\n")
        case = {"id": "reject-then-signal", "operation": "syntax",
                "source_base64": source, "expected": {"status": "error"}}
        record = run_probe_case(work, "compiler-signal", selfkill, case)
        assert record["status"] == "failed", record
        variant = record["variants"][0]
        assert variant["reference"]["timed_out"] is False
        assert variant["reference"]["exit"] == -9
        assert "signal" in json.dumps(record["failures"])

        partial = write_fixture(
            work, "reject_leaving_object",
            "import pathlib,sys\n"
            "out=pathlib.Path(sys.argv[sys.argv.index('--out-dir')+1])\n"
            "out.mkdir()\n(out/'program.o').write_bytes(b'partial output')\n"
            "print(" + repr(error_json) + ")\nraise SystemExit(1)\n")
        case = {"id": "rejected-build-left-object",
                "operation": "build_native", "manifest": "lang.toml",
                "opt": [0], "expected": {"status": "error"}}
        record = run_probe_case(work, "partial-object", partial, case)
        candidate = record["variants"][0]["candidates"][0]
        assert record["status"] == "failed", record
        assert candidate["run"]["artifacts"]["published"] == ["program.o"]

        timeout_build = write_fixture(
            work, "build_nonterminating_program",
            "import pathlib,sys\n"
            "out=pathlib.Path(sys.argv[sys.argv.index('--out-dir')+1])\n"
            "out.mkdir()\n"
            "(out/'program.py').write_text('import time\\ntime.sleep(5)\\n')\n"
            "(out/'langrt.py').write_text('')\n"
            "print(" + repr(ok_json) + ")\n")
        case = {"id": "nonterminating-is-panic",
                "operation": "build_python", "manifest": "lang.toml",
                "expected": {"status": "ok"},
                "run_expectation": {"status": "panic"}}
        record = run_probe_case(work, "execution-timeout", timeout_build, case)
        candidate = record["variants"][0]["candidates"][0]
        assert record["status"] == "failed", record
        assert candidate["run"]["execution"]["timed_out"] is True
        assert candidate["run"]["execution"]["exit"] == -9

        keep = base64.b64encode(b"keep").decode("ascii")
        no_op = write_fixture(
            work, "reject_preserving_output",
            "print(" + repr(error_json) + ")\nraise SystemExit(1)\n")
        case = {"id": "preserve-output", "operation": "build_native",
                "manifest": "lang.toml", "opt": [0],
                "expected": {"status": "error"},
                "preexisting_outputs": {"keep.bin": keep}}
        record = run_probe_case(work, "preserve-output", no_op, case)
        assert record["status"] == "passed", record
        artifacts = record["variants"][0]["candidates"][0]["run"]["artifacts"]
        assert artifacts["published"] == []
        assert artifacts["preexisting_changed"] == []

        changes = write_fixture(
            work, "reject_changing_output",
            "import pathlib,sys\n"
            "out=pathlib.Path(sys.argv[sys.argv.index('--out-dir')+1])\n"
            "out.mkdir(exist_ok=True)\n(out/'keep.bin').write_bytes(b'changed')\n"
            "print(" + repr(error_json) + ")\nraise SystemExit(1)\n")
        record = run_probe_case(work, "change-output", changes, case)
        assert record["status"] == "failed", record
        artifacts = record["variants"][0]["candidates"][0]["run"]["artifacts"]
        assert artifacts["preexisting_changed"] == ["keep.bin"]

        detached = write_fixture(
            work, "escape_session_child",
            "import pathlib,subprocess,sys,time\n"
            "child=subprocess.Popen([sys.executable,'-c',"
            "'import time; time.sleep(1.2)'],start_new_session=True)\n"
            "pathlib.Path(sys.argv[1]).write_text(str(child.pid))\n"
            "print('ready',flush=True)\ntime.sleep(5)\n")
        detached_pid = work / "detached.pid"
        started = time.monotonic()
        result = corpus._run_process_impl(
            [sys.executable, detached, detached_pid], work,
            dict(os.environ), 0.1)
        elapsed = time.monotonic() - started
        assert result["timed_out"] is True
        assert result["cleanup_complete"] is False
        assert result["abandoned_pipes"] is True
        assert elapsed < 1.0, elapsed
        assert result["stdout"] == b"ready\n"
        pid = int(detached_pid.read_text())
        try:
            os.kill(pid, 9)
        except ProcessLookupError:
            pass
        for _ in range(40):
            state = Path(f"/proc/{pid}/stat")
            if not state.exists() or state.read_text().split()[2] == "Z":
                break
            time.sleep(0.05)


def check_persisted_process_and_case_records():
    """Real compile/case records must survive later harness failures."""
    with tempfile.TemporaryDirectory(
            prefix="woozy-corpus-persist-") as directory:
        work = Path(directory)
        context = probe_context(work)
        context["timeout"] = 2.0
        ok = write_fake_compiler(work, "fake-persist", ["ok"])
        case = fake_repeat_case("persist-records")
        variant = corpus.case_variants(case)[0]

        baseline = corpus.run_case(
            case, ok, [], work / "out-baseline", context)
        assert baseline["status"] == "passed", baseline
        reference = baseline["variants"][0]["reference"]
        baseline_dir = work / "out-baseline" / reference["directory"]
        stored = json.loads(
            (baseline_dir / "compiler.process.json").read_text())
        argv_record = json.loads((baseline_dir / "argv.json").read_text())
        assert stored["case"] == case["id"], stored
        assert stored["role"] == "reference", stored
        assert stored["operation"] == case["operation"], stored
        assert stored["variant"] == variant, stored
        assert stored["argv"] == argv_record["argv"], (stored, argv_record)
        assert stored["cwd"] == argv_record["cwd"] == str(context["root"])
        assert stored["timeout"] == context["timeout"], stored
        assert stored["trace_path"] == "exec.log", stored
        for key in ("exit", "timed_out", "cleanup_complete",
                    "abandoned_pipes", "stdout_sha256", "stderr_sha256"):
            assert stored[key] == reference[key], (key, stored, reference)
        stdout_bytes = (baseline_dir / stored["stdout_path"]).read_bytes()
        stderr_bytes = (baseline_dir / stored["stderr_path"]).read_bytes()
        assert stored["stdout_sha256"] == corpus.digest_bytes(stdout_bytes)
        assert stored["stderr_sha256"] == corpus.digest_bytes(stderr_bytes)
        assert json.loads(stdout_bytes)["status"] == "ok"

        case_path = (work / "out-baseline" / "cases" /
                     (corpus.safe_name(case["id"]) + ".json"))
        persisted = json.loads(case_path.read_text())
        assert persisted == json.loads(json.dumps(corpus.public_run(baseline)))
        assert persisted["status"] == "passed", persisted
        assert persisted["variants"][0]["variant"] == variant, persisted

        original = corpus._artifact_info
        intercepted = []

        def broken_artifacts(*args, **kwargs):
            intercepted.append(args)
            raise RuntimeError("injected artifact failure")

        corpus._artifact_info = broken_artifacts
        try:
            corpus.run_case(case, ok, [], work / "out-crash", context)
        except RuntimeError as error:
            assert "injected artifact failure" in str(error), error
        else:
            raise AssertionError(
                "injected artifact failure did not stop the run")
        finally:
            corpus._artifact_info = original
        assert intercepted, intercepted
        crash_dir = work / "out-crash" / reference["directory"]
        early = json.loads((crash_dir / "compiler.process.json").read_text())

        def normalized_argv(record, out_root):
            return [part.replace(str(out_root), "<OUT>")
                    for part in record["argv"]]

        assert normalized_argv(early, work / "out-crash") == [
            part.replace(str(work / "out-baseline"), "<OUT>")
            for part in stored["argv"]], (early["argv"], stored["argv"])
        for key in ("case", "role", "operation", "variant", "cwd",
                    "timeout", "exit", "timed_out", "cleanup_complete",
                    "abandoned_pipes", "stdout_path", "stderr_path",
                    "stdout_sha256", "stderr_sha256", "trace_path"):
            assert early[key] == stored[key], (key, early, stored)
        for name in ("stdout.bin", "stderr.bin", "exec.log"):
            assert (crash_dir / name).is_file(), (crash_dir, name)
        assert (crash_dir / "stdout.bin").read_bytes() == stdout_bytes
        assert (crash_dir / "stderr.bin").read_bytes() == stderr_bytes
        assert not (crash_dir / "artifacts.json").exists()
        assert not (work / "out-crash" / "cases").exists()

        # A later case failing must not touch the already completed case file.
        broken_case = fake_repeat_case("persist-broken")
        corpus._artifact_info = broken_artifacts
        try:
            corpus.run_case(
                broken_case, ok, [], work / "out-baseline", context)
        except RuntimeError:
            pass
        finally:
            corpus._artifact_info = original
        assert not (work / "out-baseline" / "cases" /
                    (corpus.safe_name(broken_case["id"]) + ".json")).exists()
        assert json.loads(case_path.read_text()) == persisted

        # Real compiler failure and timeout must stay failures in both records.
        fail_compiler = write_fixture(
            work, "persist_compiler_failure",
            "import sys\n"
            "print('compiler failed', file=sys.stderr, flush=True)\n"
            "raise SystemExit(3)\n")
        failing = corpus.run_case(
            fake_repeat_case("persist-compiler-failure"), fail_compiler, [],
            work / "out-failure", context)
        assert failing["status"] == "failed", failing
        failing_reference = failing["variants"][0]["reference"]
        failing_dir = work / "out-failure" / failing_reference["directory"]
        failing_process = json.loads(
            (failing_dir / "compiler.process.json").read_text())
        assert failing_process["exit"] == 3, failing_process
        assert failing_process["exit"] == failing_reference["exit"]
        assert failing_process["timed_out"] is False, failing_process
        assert (failing_dir / failing_process["stderr_path"]).read_bytes() == (
            b"compiler failed\n")
        failure_path = (work / "out-failure" / "cases" /
                      (corpus.safe_name("persist-compiler-failure") + ".json"))
        failure_record = json.loads(failure_path.read_text())
        assert failure_record["status"] == "failed", failure_record
        assert failure_record == json.loads(
            json.dumps(corpus.public_run(failing)))

        hang = write_fixture(
            work, "persist_hang", "import time\ntime.sleep(5)\n")
        timeout_case = {
            "id": "persist-timeout", "operation": "syntax",
            "source_base64": base64.b64encode(b"package x; ?").decode("ascii"),
            "expected": {"status": "error"},
        }
        timed = corpus.run_case(
            timeout_case, hang, [], work / "out-timeout",
            dict(context, timeout=0.3))
        assert timed["status"] == "failed", timed
        timed_reference = timed["variants"][0]["reference"]
        assert timed_reference["timed_out"] is True, timed_reference
        timed_process = json.loads(
            (work / "out-timeout" / timed_reference["directory"] /
             "compiler.process.json").read_text())
        assert timed_process["timed_out"] is True, timed_process
        assert timed_process["exit"] == -9, timed_process
        assert timed_process["exit"] == timed_reference["exit"]
        timeout_path = (work / "out-timeout" / "cases" /
                        (corpus.safe_name(timeout_case["id"]) + ".json"))
        timeout_record = json.loads(timeout_path.read_text())
        assert timeout_record["status"] == "failed", timeout_record
        assert timeout_record == json.loads(
            json.dumps(corpus.public_run(timed)))

        # The original non-empty run evidence rejection still protects them.
        protected = failure_path.read_bytes()
        try:
            corpus.run_case(
                fake_repeat_case("persist-compiler-failure"), fail_compiler,
                [], work / "out-failure", context)
        except corpus.CorpusError as error:
            assert "not empty" in str(error), error
        else:
            raise AssertionError(
                "reused a non-empty run evidence directory")
        assert failure_path.read_bytes() == protected


def check_generated_project_controls():
    manifest = (
        'schema_version=1\n[package]\nname="app"\n'
        'source_root="src"\nentry="app"\n[build]\n'
        'backend="native"\nword_bits=64\n')

    def encoded(data):
        return base64.b64encode(data).decode("ascii")

    case = {
        "id": "generated-project",
        "operation": "check",
        "manifest": "lang.toml",
        "backend": "native",
        "generated_files": {
            "lang.toml": encoded(manifest.encode("utf-8")),
            "src/main.wz": encoded(b"package app;func main(){}"),
            "src/hidden/lib.wz": encoded(
                b"package hidden;pub struct S{x i32;}"),
        },
    }
    with tempfile.TemporaryDirectory(
            prefix="woozy-corpus-generated-") as directory:
        work = Path(directory)
        root = corpus._materialize_generated(case, work, "reference")
        assert root is not None
        assert (root / "src/main.wz").read_bytes() == (
            b"package app;func main(){}")
        assert (root / "src/hidden/lib.wz").read_bytes() == (
            b"package hidden;pub struct S{x i32;}")
        before = corpus.snapshot_case(
            case, work, work / "run", root)
        assert before[str((root / "lang.toml").resolve())] == (
            corpus.digest_bytes(manifest.encode("utf-8")))

        (root / "src/main.wz").write_bytes(b"package app;changed")
        assert corpus._materialize_generated(
            case, work, "reference") == root
        assert (root / "src/main.wz").read_bytes() == b"package app;changed"
        after = corpus.snapshot_case(case, work, work / "run", root)
        assert before != after

        for ident, files in (
                ("absolute", {"/src/main.wz": encoded(b"x")}),
                ("parent", {"../main.wz": encoded(b"x")}),
                ("dot", {"src/./main.wz": encoded(b"x")}),
                ("duplicate", {"src/main.wz": encoded(b"x"),
                               "src//main.wz": encoded(b"x")}),
                ("empty", {"": encoded(b"x")}),
                ("not-base64", {"src/main.wz": "***"}),
                ("not-string", {"src/main.wz": 1})):
            bad = dict(case, id="generated-" + ident,
                       generated_files=files)
            expect_error(corpus._materialize_generated, bad, work,
                         "reference")

        symlink = dict(case, id="generated-symlink")
        link = (work / "generated" / corpus.safe_name(symlink["id"])
                / corpus.safe_name("reference"))
        link.parent.mkdir(parents=True)
        outside = work / "outside"
        outside.mkdir()
        (outside / "lang.toml").write_text(manifest)
        link.symlink_to(outside, target_is_directory=True)
        expect_error(corpus._materialize_generated, symlink, work,
                     "reference")

        parent_symlink = dict(case, id="generated-parent-symlink")
        parent_root = (work / "generated"
                       / corpus.safe_name(parent_symlink["id"])
                       / corpus.safe_name("reference"))
        parent_root.mkdir(parents=True)
        (parent_root / "src").symlink_to(outside, target_is_directory=True)
        expect_error(corpus._materialize_generated, parent_symlink, work,
                     "reference")

        seeded = {
            "program": encoded(b"preserve-program"),
            "program.o": encoded(b"preserve-program.o"),
            "program.py": encoded(b"preserve-program.py"),
            "langrt.py": encoded(b"preserve-langrt.py"),
            "unrelated": encoded(b"preserve-unrelated"),
        }
        build = {
            "id": "generated-preeserve",
            "operation": "build_native",
            "manifest": "lang.toml",
            "preexisting_outputs": seeded,
        }
        run_dir = work / "seed-run"
        run_dir.mkdir()
        preexisting = corpus._seed_outputs(build, run_dir)
        artifacts = corpus._artifact_info(build, run_dir, preexisting)
        assert artifacts["preexisting_changed"] == []
        assert artifacts["published"] == []
        (run_dir / "out/program").write_bytes(b"changed")
        artifacts = corpus._artifact_info(build, run_dir, preexisting)
        assert artifacts["preexisting_changed"] == ["program"]

        ordinary = {
            "id": "ordinary-case",
            "operation": "check",
            "manifest": "tests/generics/list-inference/lang.toml",
            "backend": "native",
        }
        expected_manifest = (
            CORPUS_ROOT / "tests/generics/list-inference/lang.toml"
        ).resolve()
        assert corpus.build_command(
            ordinary, Path("woozyc"), CORPUS_ROOT, work / "run",
            {"name": "default", "opt": None}) == [
                "woozyc", "check", "--project", str(expected_manifest),
                "--format", "json", "--backend", "native"]
        snapshot = corpus.snapshot_case(
            ordinary, CORPUS_ROOT, work / "run")
        assert snapshot
        assert all(Path(path).is_relative_to(expected_manifest.parent)
                   for path in snapshot)


def check_generated_assertion_corrections():
    manifest = (
        'schema_version=1\n[package]\nname="app"\n'
        'source_root="src"\nentry="app"\n[build]\n'
        'backend="native"\nword_bits=64\n')
    main = b"package app;func main(){}"

    def encoded(data):
        return base64.b64encode(data).decode("ascii")

    nested = {
        "id": "nested-input-snapshot",
        "operation": "check",
        "manifest": "sub/lang.toml",
        "generated_files": {
            "sub/lang.toml": encoded(manifest.encode("utf-8")),
            "sub/src/main.wz": encoded(main),
            "shared/lib.wz": encoded(b"package shared;func f(){}"),
            "sub/data.bin": encoded(b"original"),
        },
    }
    with tempfile.TemporaryDirectory(
            prefix="woozy-corpus-corrections-") as directory:
        work = Path(directory)
        root = corpus._materialize_generated(nested, work, "reference")
        before = corpus.snapshot_case(
            nested, work, work / "run", root)
        assert set(before) == {
            str((root / relative).resolve())
            for relative in nested["generated_files"]
        }
        (root / "shared/lib.wz").write_bytes(b"package shared;changed")
        (root / "sub/data.bin").write_bytes(b"changed")
        after = corpus.snapshot_case(nested, work, work / "run", root)
        assert before != after

        (root / "escape").symlink_to(work / "outside")
        expect_error(corpus.snapshot_case, nested, work, work / "run", root)
        (root / "escape").unlink()
        os.mkfifo(root / "special")
        expect_error(corpus.snapshot_case, nested, work, work / "run", root)

        fresh = {
            "id": "fresh-output",
            "operation": "build_native",
            "manifest": "lang.toml",
            "expected": {"status": "error", "output_exists": False},
        }
        fresh_dir = work / "fresh"
        (fresh_dir / "out").mkdir(parents=True)
        artifacts = corpus._artifact_info(fresh, fresh_dir, {})
        assert artifacts["output_exists"] is True
        assert artifacts["output_kind"] == "directory"
        assert artifacts["published"] == []
        failures = corpus.check_expected(
            fresh, document(), 1, b"", artifacts)
        assert any("output_exists" in failure for failure in failures), failures

        check = dict(fresh, id="check-output", operation="check")
        check_dir = work / "check"
        (check_dir / "out").mkdir(parents=True)
        check_artifacts = corpus._artifact_info(check, check_dir, {})
        failures = corpus.check_expected(
            check, document(), 1, b"", check_artifacts)
        assert any("output_exists" in failure for failure in failures), failures

        absent_dir = work / "absent"
        absent_dir.mkdir()
        absent = corpus._artifact_info(fresh, absent_dir, {})
        assert absent["output_exists"] is False
        assert absent["output_kind"] == "absent"
        assert not corpus.check_expected(
            fresh, document(), 1, b"", absent)

        existing = {
            "id": "existing-output",
            "operation": "build_native",
            "manifest": "lang.toml",
            "expected": {"status": "error", "output_exists": True},
            "preexisting_outputs": {"program": encoded(b"preserve")},
        }
        existing_dir = work / "existing"
        existing_dir.mkdir()
        seeded = corpus._seed_outputs(existing, existing_dir)
        existing_artifacts = corpus._artifact_info(
            existing, existing_dir, seeded)
        assert existing_artifacts["output_exists"] is True
        assert not corpus.check_expected(
            existing, document(), 1, b"", existing_artifacts)

        primary = {
            "id": "primary-input",
            "operation": "check",
            "manifest": "lang.toml",
            "generated_files": {
                "lang.toml": encoded(manifest.encode("utf-8")),
                "src/main.wz": encoded(main),
            },
            "expected": {
                "status": "error", "exit": 1,
                "code": "E_PARSE_EXPECTED_TOKEN",
                "require_error_span": True,
                "primary_input": "src/main.wz",
            },
        }
        good = document()
        good["diagnostics"][0]["primary"] = {
            "path": "<REPO>/src/main.wz",
            "start_byte": 0,
            "end_byte": len(main),
        }
        assert not corpus.check_expected(
            primary, good, 1, b"", {"published": []})
        wrong_path = copy.deepcopy(good)
        wrong_path["diagnostics"][0]["primary"]["path"] = (
            "<REPO>/lang.toml")
        failures = corpus.check_expected(
            primary, wrong_path, 1, b"", {"published": []})
        assert any("primary on" in failure for failure in failures), failures
        bad_range = copy.deepcopy(good)
        bad_range["diagnostics"][0]["primary"]["end_byte"] = len(main) + 1
        failures = corpus.check_expected(
            primary, bad_range, 1, b"", {"published": []})
        assert any("primary on" in failure for failure in failures), failures
        empty_range = copy.deepcopy(good)
        empty_range["diagnostics"][0]["primary"]["start_byte"] = 1
        empty_range["diagnostics"][0]["primary"]["end_byte"] = 1
        failures = corpus.check_expected(
            primary, empty_range, 1, b"", {"published": []})
        assert any("primary on" in failure for failure in failures), failures
        missing_input = copy.deepcopy(primary)
        missing_input["expected"]["primary_input"] = "missing.wz"
        failures = corpus.check_expected(
            missing_input, good, 1, b"", {"published": []})
        assert any("not a generated input" in failure
                   for failure in failures), failures

        legacy = {
            "operation": "check",
            "expected": {
                "status": "error", "exit": 1,
                "code": "E_PARSE_EXPECTED_TOKEN",
                "require_error_span": True,
            },
        }
        assert not corpus.check_expected(
            legacy, bad_range, 1, b"", {"published": []})


def check_runner_and_expectation_inventory_negative():
    with tempfile.TemporaryDirectory(prefix="woozy-corpus-inventory-") as directory:
        repo = Path(directory) / "repo"
        (repo / "tests").mkdir(parents=True)
        (repo / "main.wz").write_text("package app; func main() {}\n")
        (repo / "tests/run.py").write_text("raise NotImplementedError()\n")
        (repo / "lang.toml").write_text(
            'schema_version=1\n[package]\nname="probe"\n'
            'source_root="."\nentry="app"\n'
            '[build]\nbackend="native"\nword_bits=64\n')
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        subprocess.run(["git", "-C", str(repo), "add", "--",
                        "main.wz", "tests/run.py", "lang.toml"], check=True)
        inventory = {
            "schema": corpus.SCHEMA,
            "files": [
                {"path": "main.wz",
                 "sha256": corpus.digest(repo / "main.wz"),
                 "role": "woozy_source", "assignment": ["source"]},
                {"path": "tests/run.py",
                 "sha256": corpus.digest(repo / "tests/run.py"),
                 "role": "runner_source"},
                {"path": "lang.toml",
                 "sha256": corpus.digest(repo / "lang.toml"),
                 "role": "manifest", "assignment": ["source"]},
            ],
            "cases": [{
                "id": "source", "operation": "syntax", "source": "main.wz",
                "enabled": True, "support": "public_cli",
                "adaptation": "ready", "expected": {"status": "ok"},
            }],
            "runners": [{"path": "tests/run.py"}],
        }
        report = corpus.check_inventory(repo, inventory)
        assert report["errors"], report
        assert report["coverage"]["complete"] is False
        assert report["coverage"]["runners"]["unclassified"] == ["tests/run.py"]

        inventory["runners"][0] = {
            "path": "tests/run.py", "status": "component",
            "reason": "probe component",
        }
        inventory["cases"][0] = {
            "id": "source", "operation": "build_native",
            "manifest": "lang.toml", "enabled": True,
            "support": "public_cli", "adaptation": "ready",
            "expected": {"status": "ok"},
            "run_expectation": {"todo": "later"},
        }
        report = corpus.check_inventory(repo, inventory)
        assert any("run expectation" in error
                   for error in report["errors"]), report["errors"]
        assert corpus.recorded_run_expectation(
            inventory["cases"][0]) is False
        failures = corpus.check_run_expectation(
            inventory["cases"][0],
            {"exit": 99, "timed_out": False, "cleanup_complete": True,
             "_stdout": b"wrong", "_stderr": b"wrong"})
        assert failures

        (repo / "accepted-but-untracked.wz").write_text(
            "package accepted; func f() {}\n")
        report = corpus.check_inventory(repo, inventory)
        assert "accepted-but-untracked.wz" in (
            report["coverage"]["untracked_corpus_files"])
        assert report["source_basis"]["formal_freeze_ready"] is False


def run_harness():
    assert STAGE0.is_file(), f"missing stage0 reference {STAGE0}"
    with tempfile.TemporaryDirectory(prefix="woozy-corpus-harness-") as directory:
        out = Path(directory) / "out"
        run = subprocess.run([
            sys.executable, str(ROOT / "selfhost/corpus.py"), "run",
            "--root", str(CORPUS_ROOT),
            "--inventory", str(INVENTORY),
            "--reference", str(STAGE0),
            "--candidate", str(STAGE0),
            "--candidate", str(STAGE0),
            "--llvm", str(corpus.DEFAULT_LLVM),
            "--strace", str(corpus.DEFAULT_STRACE),
            "--python", str(corpus.DEFAULT_PYTHON),
            "--libxml-dir", str(corpus.DEFAULT_LIBXML),
            "--scope", "harness-selfcheck",
            "--out", str(out),
        ], capture_output=True, text=True, timeout=900)
        assert run.returncode == 0, (run.stdout, run.stderr)
        result = json.loads((out / "result.json").read_text())
        assert result["status"] == "passed", result["status"]
        assert result["complete"] is False
        assert result["full_corpus_executed"] is False
        assert len(result["selected_cases"]) == 6
        assert result["not_run"]
        for case in result["cases"]:
            assert case["status"] == "passed", case
            for variant in case["variants"]:
                assert variant["reference"]["status"] == "passed", variant
                assert len(variant["candidates"]) == 2
                for candidate in variant["candidates"]:
                    assert candidate["status"] == "passed", candidate
                    run_dir = out / candidate["run"]["directory"]
                    for name in ("argv.json", "exec.log", "stdout.bin",
                                 "stderr.bin", "input.json"):
                        assert (run_dir / name).is_file(), (run_dir, name)
                    execution = candidate["run"]["execution"]
                    if execution is not None:
                        for key in ("stdout_path", "stderr_path", "trace_path"):
                            assert (run_dir / execution[key]).is_file(), (
                                run_dir, key)
        python_case = next(
            case for case in result["cases"]
            if case["id"] == "harness:build-python-run")
        execution = python_case["variants"][0]["candidates"][0]["run"][
            "execution"]
        assert execution["exit"] == 0
        assert execution["cleanup_complete"] is True
        assert execution["stdout_sha256"] == corpus.digest_bytes(b"")
        assert execution["stderr_sha256"] == corpus.digest_bytes(b"")
        assert execution["trace"]["status"] == "ok"
        execute_dir = out / python_case["variants"][0]["candidates"][0][
            "run"]["directory"]
        execution = python_case["variants"][0]["candidates"][0]["run"][
            "execution"]
        assert (execute_dir / execution["trace_path"]).is_file()


def check_native_helper_guard():
    """Stage0 delegates to the legacy helper; a candidate must be rejected."""
    assert STAGE0.is_file(), f"missing stage0 reference {STAGE0}"
    args = argparse.Namespace(
        root=CORPUS_ROOT, reference=STAGE0, candidate=[STAGE0],
        llvm=corpus.DEFAULT_LLVM, strace=corpus.DEFAULT_STRACE,
        python=corpus.DEFAULT_PYTHON,
        require_python_version="3.12.15", require_llvm_version="22.1.8",
        libxml_dir=corpus.DEFAULT_LIBXML, reference_helper=None,
        repository_root=[CORPUS_ROOT], timeout=300)
    context = corpus.preflight(args)
    case = {
        "id": "guard:native-helper",
        "operation": "build_native",
        "manifest": "examples/hello/lang.toml",
        "opt": [2],
        "expected": {"status": "ok", "no_stderr": True},
        "run_expectation": {
            "exit": 0,
            "stdout_base64": "SGVsbG8sIHdvcmxkIQo=",
            "stderr_base64": "",
        },
    }
    with tempfile.TemporaryDirectory(prefix="woozy-corpus-guard-") as directory:
        record = corpus.run_case(
            case, context["reference"], context["candidates"],
            Path(directory), context)
        assert record["status"] == "failed", record
        variant = record["variants"][0]
        assert variant["reference"]["status"] == "passed", variant["reference"]
        assert variant["reference"]["execution"]["exit"] == 0
        assert variant["reference"]["execution"]["trace"]["status"] == "ok"
        assert "program" in variant["reference"]["artifacts"]["published"]
        assert "program.o" in variant["reference"]["artifacts"]["published"]
        candidate = variant["candidates"][0]
        assert candidate["status"] == "failed", candidate
        errors = "\n".join(candidate["failures"])
        assert "unexpected compiler subprocesses" in errors, errors
        assert "lang_native_helper" in errors, errors
        assert candidate["run"]["trace"]["status"] == "violation"


def check_full_coverage_is_not_a_pass():
    inventory = corpus.load_inventory(INVENTORY)
    inventory["runners"][0].update(status="adaptation_required",
                                   reason="test incomplete runner coverage")
    with tempfile.TemporaryDirectory(prefix="woozy-corpus-gap-") as directory:
        path = Path(directory) / "inventory.json"
        path.write_text(json.dumps(inventory))
        run = subprocess.run([
            sys.executable, str(ROOT / "selfhost/corpus.py"), "coverage",
            "--root", str(CORPUS_ROOT), "--inventory", str(path),
        ], capture_output=True, text=True, timeout=120)
    assert run.returncode == 1, (run.returncode, run.stdout, run.stderr)
    assert "INCOMPLETE" in run.stdout


check_diagnostics_and_normalization()
check_strict_json_types()
check_trace_audit()
check_seccomp_exec_trace()
check_process_stdin_file()
check_inventory()
check_public_cli_expectations()
check_run_expectations()
check_execution_repetition_validation()
check_large_timeout_configuration()
check_execution_repetition_expectations()
check_execution_repetition_comparison()
check_execution_fixture_repetitions()
check_generated_project_controls()
check_generated_assertion_corrections()
check_timeout_signal_artifacts_and_cleanup()
check_persisted_process_and_case_records()
check_runner_and_expectation_inventory_negative()
run_harness()
check_native_helper_guard()
check_full_coverage_is_not_a_pass()
print("PASS: strict JSON, timeout/signal, cleanup, artifacts, expectations, "
      "repetitions, persisted compiler/case records, coverage, public-CLI "
      "harness expectations, stage0 harness, and legacy-helper rejection")
