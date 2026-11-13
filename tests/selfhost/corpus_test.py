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
        if case.get("run_expectation", {}).get("status") == "unrecorded" \
                and case.get("evidence"):
            raise AssertionError(f"placeholder expectation: {case['id']}")
        if not case.get("enabled"):
            continue
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
        "tests/generics/inout/constrained/reject": "required",
        "tests/generics/move/constrained-reject": "required",
        "tests/generics/unbound-method-values/reject-capture": "required",
    }
    for manifest, adaptation in not_ready.items():
        case = cases["project:" + manifest + "/lang.toml"]
        assert case["enabled"] is False, case
        assert case["adaptation"] == adaptation, case
        assert case["expected"] is None, case
        assert case["runner_status"] == (
            "component" if adaptation == "component" else "adaptation_required"), case

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
        assert (execute_dir / "execute.exec.log").is_file()


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
check_inventory()
check_public_cli_expectations()
check_run_expectations()
check_timeout_signal_artifacts_and_cleanup()
check_runner_and_expectation_inventory_negative()
run_harness()
check_native_helper_guard()
check_full_coverage_is_not_a_pass()
print("PASS: strict JSON, timeout/signal, cleanup, artifacts, expectations, "
      "coverage, public-CLI harness expectations, stage0 harness, and "
      "legacy-helper rejection")
