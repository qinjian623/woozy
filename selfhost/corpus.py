#!/usr/bin/env python3
"""Run the frozen selfhost corpus through public compiler CLIs.

This is intentionally a small driver, not a second test framework.  It owns
four things only:

* verify the frozen corpus inventory (paths and SHA-256 bytes);
* invoke the real ``check`` / ``build`` CLI with full JSON diagnostics;
* compare reference and candidate results, including diagnostics and spans;
* audit every candidate compiler subprocess with strace.

Existing runners that need private helpers or complex harnesses stay outside
this driver.  They are listed in ``corpus.json`` and in the coverage gaps; the
driver never treats a scan, an unadapted runner, or a partial inventory as a
pass.  In particular, stage0 ``build`` delegates to ``lang_native_helper`` and
is therefore deliberately rejected when stage0 is used as a *candidate*.
"""

import argparse
import ast
import base64
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import selectors
import shutil
import stat
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CORPUS_ROOT = ROOT
SCHEMA = "woozy-selfhost-corpus/1"
RESULT_SCHEMA = "woozy-selfhost-corpus-result/1"
COMMANDS = {"syntax", "check", "build_native", "build_python"}
RUNNER_STATUSES = {
    "public_cli", "component", "adaptation_required", "unmapped"
}
CASE_SUPPORT = {"public_cli", "existing_runner", "component", "unmapped"}
CASE_ADAPTATION = {"ready", "required", "component"}
RUN_EXPECTATION_KEYS = {
    "exit", "status", "stdout_base64", "stderr_base64",
    "panic_stderr_prefix",
}
CLEANUP_TIMEOUT = 0.5
PANIC_EXIT = 101
TOOL_NAMES = ("opt", "llc", "clang", "clang++", "ld.lld")
DIAGNOSTIC_PHASES = {
    "lex", "parse", "resolve", "type", "effect", "ownership",
    "lowering", "codegen", "link", "runtime", "toolchain",
}
DIAGNOSTIC_SEVERITIES = {"error", "warning", "note"}
STATUSES = {"ok", "error", "cancelled", "internal_error"}
HARNESS_EXCLUSIONS = {
    "selfhost/corpus.py",
    "selfhost/corpus.json",
    "tests/selfhost/corpus_test.py",
}
TOOLCHAIN_ROOT = Path(os.environ.get("WOOZY_TOOLCHAIN_ROOT")
                      or ROOT.parent / "toolchains")
DEFAULT_LLVM = TOOLCHAIN_ROOT / "llvm-22.1.8"
DEFAULT_STRACE = TOOLCHAIN_ROOT / "strace-7.2/usr/bin/strace"
DEFAULT_PYTHON = TOOLCHAIN_ROOT / "t10-python-cpu/venv/bin/python"
DEFAULT_LIBXML = TOOLCHAIN_ROOT / "libxml2-legacy-2.13.9/usr/lib"


class CorpusError(RuntimeError):
    """A corpus, inventory, or comparison error."""


class TraceError(CorpusError):
    """An executable was not in the exact allowed compiler process set."""


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def load_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CorpusError(f"cannot read JSON {path}: {error}") from error


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8", errors="backslashreplace")


def safe_name(value):
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "_", value).strip("._") or "case"
    return cleaned[:120] + "-" + digest_bytes(value.encode("utf-8"))[:12]


def _relative(path, root):
    try:
        return str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        return str(path.resolve())


def _is_frozen_corpus_path(path):
    if path in HARNESS_EXCLUSIONS:
        return False
    if path.startswith((".work-tmp/", "build/", "reports/")) or "/.build/" in path:
        return False
    if path.endswith(".wz"):
        return True
    if path.endswith("/lang.toml") or path == "lang.toml":
        return True
    if path.startswith("tests/") and (
            path.endswith(".py") or path.endswith(".sh")
            or path.endswith(".toml") or path.endswith(".json")
            or path.endswith("/CMakeLists.txt")):
        return True
    if path.startswith("selfhost/") and (
            path.endswith(".py") or path.endswith(".sh")
            or path.endswith(".toml") or path.endswith(".json")
            or path.endswith("/CMakeLists.txt")):
        return True
    if path.startswith("stdlib/") and path.endswith(".toml"):
        return True
    if path.startswith("examples/") and path.endswith(".toml"):
        return True
    if path.startswith("contracts/") and path.endswith(".json"):
        return True
    if path.startswith("runtime/") and path.endswith(
            (".py", ".cpp", ".h", ".hpp", ".toml")):
        return True
    return path == "CMakeLists.txt"


def _git_paths(root, *args):
    try:
        raw = subprocess.check_output(
            ["git", "-C", str(root), *args], timeout=60,
            stderr=subprocess.DEVNULL)
    except (OSError, subprocess.SubprocessError) as error:
        raise CorpusError(f"cannot list Git paths: {error}") from error
    return [
        item.decode(sys.getfilesystemencoding(), "surrogateescape")
        for item in raw.split(b"\0") if item
    ]


def frozen_candidates(root):
    """Return tracked corpus paths covered by the freeze.

    New tracked ``.wz`` files, manifests, runner scripts, and case inventories
    are therefore visible to ``freeze`` even when the JSON inventory forgot
    them.  The executor and this file's own unit test are harness code, not
    corpus inputs, so they are excluded by exact path.
    """
    return sorted(set(
        path for path in _git_paths(root, "ls-files", "-z")
        if _is_frozen_corpus_path(path)))


def untracked_corpus_candidates(root):
    """Return untracked paths that would become frozen corpus if accepted."""
    return sorted(set(
        path for path in _git_paths(
            root, "ls-files", "-z", "--others", "--exclude-standard")
        if _is_frozen_corpus_path(path)))


def normalize_text(text, run_dir, repository_roots):
    """Normalize only the run's absolute temp path and known repository roots."""
    # The run directory must be replaced first: it can itself live under a
    # repository root, and a root replacement would otherwise consume it.
    run_values = {str(run_dir), str(run_dir.resolve())}
    for value in sorted(run_values, key=len, reverse=True):
        if value:
            text = text.replace(value, "<RUN>")
    root_values = set()
    for root in repository_roots:
        root_values.add(str(root).rstrip("/"))
        root_values.add(str(root.resolve()).rstrip("/"))
    for value in sorted(root_values, key=len, reverse=True):
        if value:
            text = text.replace(value, "<REPO>")
    return text


def normalize_json(value, run_dir, repository_roots, top=True):
    if isinstance(value, dict):
        result = {}
        for key, child in value.items():
            if top and key == "compiler_version":
                continue
            result[key] = normalize_json(child, run_dir, repository_roots, False)
        return result
    if isinstance(value, list):
        return [normalize_json(item, run_dir, repository_roots, False)
                for item in value]
    if isinstance(value, str):
        return normalize_text(value, run_dir, repository_roots)
    return value


def _is_integer(value):
    return type(value) is int


def _check_span(span, where):
    if not isinstance(span, dict):
        raise CorpusError(f"{where}: span is not an object")
    if set(span) != {"path", "start_byte", "end_byte"}:
        raise CorpusError(f"{where}: span has unsupported fields {sorted(span)}")
    if not isinstance(span["path"], str) or not span["path"]:
        raise CorpusError(f"{where}: span path is empty")
    if not _is_integer(span["start_byte"]) or not _is_integer(span["end_byte"]):
        raise CorpusError(f"{where}: span offsets are not integers")
    if span["start_byte"] < 0 or span["end_byte"] < span["start_byte"]:
        raise CorpusError(f"{where}: invalid span {span['start_byte']}..{span['end_byte']}")


def validate_diagnostic_document(document):
    """Reject malformed JSON diagnostics before any parity comparison."""
    if not isinstance(document, dict):
        raise CorpusError("diagnostic document is not an object")
    required = {"schema_version", "compiler_version", "source_revision",
                "status", "diagnostics"}
    if not required <= set(document):
        raise CorpusError(
            f"diagnostic document missing {sorted(required - set(document))}")
    if not _is_integer(document["schema_version"]) or document["schema_version"] != 1:
        raise CorpusError("diagnostic schema_version is not the integer 1")
    for key in ("compiler_version", "source_revision"):
        if not isinstance(document[key], str):
            raise CorpusError(f"diagnostic {key} is not a string")
    if document["status"] not in STATUSES:
        raise CorpusError(f"unknown diagnostic status {document['status']!r}")
    diagnostics = document["diagnostics"]
    if not isinstance(diagnostics, list):
        raise CorpusError("diagnostics is not an array")
    errors = 0
    for index, item in enumerate(diagnostics):
        where = f"diagnostics[{index}]"
        if not isinstance(item, dict):
            raise CorpusError(f"{where}: not an object")
        for key in ("code", "phase", "severity", "message", "primary"):
            if key not in item:
                raise CorpusError(f"{where}: missing {key}")
        if not isinstance(item["code"], str) or not re.fullmatch(
                r"[EW]_[A-Z0-9_]+", item["code"]):
            raise CorpusError(f"{where}: invalid code {item['code']!r}")
        if item["phase"] not in DIAGNOSTIC_PHASES:
            raise CorpusError(f"{where}: invalid phase {item['phase']!r}")
        if item["severity"] not in DIAGNOSTIC_SEVERITIES:
            raise CorpusError(f"{where}: invalid severity {item['severity']!r}")
        if not isinstance(item["message"], str):
            raise CorpusError(f"{where}: message is not a string")
        if item["primary"] is not None:
            _check_span(item["primary"], where + ".primary")
        related = item.get("related", [])
        if not isinstance(related, list):
            raise CorpusError(f"{where}: related is not an array")
        for related_index, related_item in enumerate(related):
            rwhere = f"{where}.related[{related_index}]"
            if not isinstance(related_item, dict):
                raise CorpusError(f"{rwhere}: not an object")
            if set(related_item) != {"span", "message"}:
                raise CorpusError(f"{rwhere}: unsupported fields")
            _check_span(related_item["span"], rwhere + ".span")
            if not isinstance(related_item["message"], str):
                raise CorpusError(f"{rwhere}: message is not a string")
        fixits = item.get("fixits", [])
        if not isinstance(fixits, list):
            raise CorpusError(f"{where}: fixits is not an array")
        for fixit_index, fixit in enumerate(fixits):
            fwhere = f"{where}.fixits[{fixit_index}]"
            if not isinstance(fixit, dict):
                raise CorpusError(f"{fwhere}: not an object")
            if set(fixit) != {"span", "replacement", "source_revision"}:
                raise CorpusError(f"{fwhere}: unsupported fields")
            _check_span(fixit["span"], fwhere + ".span")
            if not isinstance(fixit["replacement"], str):
                raise CorpusError(f"{fwhere}: replacement is not a string")
            if not isinstance(fixit["source_revision"], str):
                raise CorpusError(f"{fwhere}: source_revision is not a string")
        notes = item.get("notes", [])
        if not isinstance(notes, list) or any(
                not isinstance(note, str) for note in notes):
            raise CorpusError(f"{where}: notes is not an array of strings")
        for key in ("expected_type", "actual_type"):
            if key in item and not isinstance(item[key], str):
                raise CorpusError(f"{where}: {key} is not a string")
        if item["severity"] == "error":
            errors += 1
    if document["status"] == "ok" and errors:
        raise CorpusError("status ok contains error diagnostics")
    if document["status"] == "error" and not errors:
        raise CorpusError("status error contains no error diagnostics")
    return document


def _reject_json_constant(value):
    raise CorpusError(f"non-standard JSON constant {value!r}")


def parse_diagnostic_stdout(stdout):
    try:
        document = json.loads(
            stdout.decode("utf-8"), parse_constant=_reject_json_constant)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CorpusError(f"malformed JSON stdout: {error}") from error
    return validate_diagnostic_document(document)


def _absolute_trace_path(executable, path, call):
    if not os.path.isabs(executable):
        raise TraceError(
            f"{call} pathname is relative and cannot be verified in {path}: "
            f"{executable!r}")
    return str(Path(executable).resolve())


def parse_trace(path):
    """Return every execve/execveat pathname in an strace log.

    Relative pathnames depend on the traced process cwd, which strace does not
    record here; they fail closed.  ``execveat`` using an empty pathname plus
    a file descriptor is likewise not resolvable and is rejected.
    """
    observed = set()
    lines = path.read_text(encoding="utf-8", errors="surrogateescape").splitlines()
    for line in lines:
        match = re.search(r'\bexecve\(("(?:[^"\\]|\\.)*")', line)
        if match:
            executable = ast.literal_eval("b" + match[1]).decode(
                sys.getfilesystemencoding(), "surrogateescape")
            if not executable:
                raise TraceError(
                    f"execve with empty pathname cannot be verified in {path}")
            observed.add(_absolute_trace_path(executable, path, "execve"))
            continue
        if re.search(r'\bexecve\(', line):
            raise TraceError(f"unparseable execve record in {path}: {line}")
        match = re.search(r'\bexecveat\(([^,]+),\s*("(?:[^"\\]|\\.)*")', line)
        if match:
            executable = ast.literal_eval("b" + match[2]).decode(
                sys.getfilesystemencoding(), "surrogateescape")
            if not executable:
                raise TraceError(
                    f"execveat with empty pathname cannot be verified in {path}")
            observed.add(_absolute_trace_path(executable, path, "execveat"))
            continue
        if re.search(r'\bexecveat\(', line):
            raise TraceError(f"unparseable execveat record in {path}: {line}")
    if not observed:
        raise TraceError(f"no execve/execveat recorded in {path}")
    return sorted(observed)


def audit_trace(path, allowed):
    observed = set(parse_trace(path))
    unexpected = observed - set(allowed)
    if unexpected:
        raise TraceError(
            "unexpected compiler subprocesses: " + ", ".join(sorted(unexpected)))
    return {"status": "ok", "observed": sorted(observed),
            "allowed": sorted(allowed)}


def json_diff(expected, actual, location="$", changes=None, limit=80):
    changes = [] if changes is None else changes
    if len(changes) >= limit:
        return changes
    if type(expected) is not type(actual):
        changes.append({"path": location, "expected": expected, "actual": actual,
                        "kind": "type"})
        return changes
    if isinstance(expected, dict):
        for key in sorted(set(expected) | set(actual)):
            if key not in expected:
                changes.append({"path": location + "." + key, "actual": actual[key],
                                "kind": "unexpected"})
            elif key not in actual:
                changes.append({"path": location + "." + key,
                                "expected": expected[key], "kind": "missing"})
            else:
                json_diff(expected[key], actual[key], location + "." + key,
                          changes, limit)
            if len(changes) >= limit:
                break
    elif isinstance(expected, list):
        if len(expected) != len(actual):
            changes.append({"path": location + ".length", "expected": len(expected),
                            "actual": len(actual), "kind": "length"})
        for index, (left, right) in enumerate(zip(expected, actual)):
            json_diff(left, right, f"{location}[{index}]", changes, limit)
            if len(changes) >= limit:
                break
    elif expected != actual:
        changes.append({"path": location, "expected": expected, "actual": actual,
                        "kind": "value"})
    return changes


def expected_status(expected):
    value = expected.get("status", expected.get("syntax"))
    if value == "accept":
        return "ok"
    if value == "reject":
        return "error"
    return value


def _case_executes(case):
    expected = case.get("expected")
    if not (case.get("operation") in ("build_native", "build_python")
            and isinstance(expected, dict)):
        return False
    status = expected_status(expected)
    return status == "ok" and expected.get("build_status", status) == "ok"


def execution_repetitions(case):
    value = case.get("execution_repetitions", 1)
    if not _is_integer(value) or value <= 0:
        raise CorpusError(
            f"{case.get('id', '<case>')}: execution_repetitions must be a "
            "positive integer")
    if "execution_repetitions" in case and not _case_executes(case):
        raise CorpusError(
            f"{case.get('id', '<case>')}: execution_repetitions requires a "
            "runnable successful build case")
    return value


def _timeout_number(value):
    """Return a positive finite float budget or None.

    JSON can contain integers too large for a float.  Converting them raises
    OverflowError, which is a configuration error, not a harness crash.
    """
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    try:
        numeric = float(value)
    except (OverflowError, ValueError):
        return None
    if not math.isfinite(numeric) or numeric <= 0:
        return None
    return numeric


def execution_timeout(case, default):
    value = case.get("execution_timeout_seconds", default)
    numeric = _timeout_number(value)
    if numeric is None:
        raise CorpusError(
            f"{case.get('id', '<case>')}: execution_timeout_seconds must be "
            "a positive finite number")
    if "execution_timeout_seconds" in case and not _case_executes(case):
        raise CorpusError(
            f"{case.get('id', '<case>')}: execution_timeout_seconds requires "
            "a runnable successful build case")
    return numeric


def execution_configuration_errors(case):
    errors = []
    executable = _case_executes(case)
    if "execution_repetitions" in case:
        value = case["execution_repetitions"]
        if not _is_integer(value):
            errors.append(
                f"{case.get('id', '<case>')}: execution_repetitions is not an "
                "integer")
        elif value <= 0:
            errors.append(
                f"{case.get('id', '<case>')}: execution_repetitions is not "
                "positive")
        elif not executable:
            errors.append(
                f"{case.get('id', '<case>')}: execution_repetitions requires "
                "a runnable successful build case")
    if "execution_timeout_seconds" in case:
        value = case["execution_timeout_seconds"]
        if _timeout_number(value) is None:
            errors.append(
                f"{case.get('id', '<case>')}: execution_timeout_seconds is "
                "not a positive finite number")
        elif not executable:
            errors.append(
                f"{case.get('id', '<case>')}: execution_timeout_seconds "
                "requires a runnable successful build case")
    return errors


def run_expectation_errors(case):
    if "run_expectation" not in case:
        return []
    expectation = case.get("run_expectation")
    if not isinstance(expectation, dict):
        return ["run_expectation is not an object"]
    if not expectation:
        return ["run_expectation is empty"]
    errors = []
    unknown = set(expectation) - RUN_EXPECTATION_KEYS
    if unknown:
        errors.append(f"unknown run_expectation fields {sorted(unknown)}")
    has_assertion = False
    if "exit" in expectation:
        if not _is_integer(expectation["exit"]):
            errors.append("run_expectation exit is not an integer")
        else:
            has_assertion = True
    if "status" in expectation:
        if expectation["status"] != "panic":
            errors.append(
                f"unsupported run_expectation status {expectation['status']!r}")
        else:
            has_assertion = True
    for key in ("stdout_base64", "stderr_base64"):
        if key not in expectation:
            continue
        value = expectation[key]
        if not isinstance(value, str):
            errors.append(f"run_expectation {key} is not a string")
            continue
        try:
            base64.b64decode(value, validate=True)
        except (ValueError, TypeError):
            errors.append(f"run_expectation {key} is not valid base64")
        else:
            has_assertion = True
    if "panic_stderr_prefix" in expectation:
        value = expectation["panic_stderr_prefix"]
        if not isinstance(value, str) or not value:
            errors.append("panic_stderr_prefix must be a non-empty string")
        else:
            has_assertion = True
        if expectation.get("status") != "panic":
            errors.append("panic_stderr_prefix requires status=panic")
    if expectation.get("status") == "panic":
        expected_exit = expectation.get("exit", PANIC_EXIT)
        if _is_integer(expected_exit) and expected_exit == 0:
            errors.append("panic run_expectation cannot expect exit 0")
    if not has_assertion:
        errors.append("run_expectation has no effective assertion")
    return errors


def recorded_run_expectation(case):
    if "run_expectation" not in case:
        return False
    return not run_expectation_errors(case)


def check_expected(case, document, exit_code, stderr, artifacts):
    expected = case.get("expected") or {}
    failures = []
    if not expected:
        return failures
    if "exit" in expected and exit_code != expected["exit"]:
        failures.append(f"expected exit {expected['exit']}, got {exit_code}")
    wanted = expected_status(expected)
    if wanted and document["status"] != wanted:
        failures.append(
            f"expected status {wanted}, got {document['status']}")
    if "exit" not in expected and wanted == "ok" and exit_code != 0:
        failures.append(f"successful status has exit {exit_code}")
    if "exit" not in expected and wanted == "error" and exit_code == 0:
        failures.append("error status has exit 0")
    if expected.get("no_stderr") and stderr:
        failures.append("expected empty stderr")
    errors = [item for item in document["diagnostics"]
              if item["severity"] == "error"]
    if wanted == "error" and not errors:
        failures.append("rejection has no error diagnostic")
    if "code" in expected:
        wanted_code = expected["code"]
        if not isinstance(wanted_code, str):
            failures.append("expected diagnostic code is not a string")
        elif not any(
                item["code"] == wanted_code
                and (item["primary"] is not None
                     or not expected.get("require_error_span"))
                for item in errors):
            observed = sorted({item["code"] for item in errors})
            failures.append(
                f"expected diagnostic code {wanted_code}, got {observed}")
    if expected.get("require_error_span") and any(
            item["primary"] is None for item in errors):
        failures.append("rejection diagnostic is missing its primary span")
    if "output_exists" in expected:
        wanted_output = expected["output_exists"]
        actual_output = artifacts.get("output_exists")
        if type(wanted_output) is not bool:
            failures.append("expected output_exists is not a boolean")
        elif actual_output != wanted_output:
            failures.append(
                f"expected output_exists {wanted_output}, got {actual_output}")
    if "primary_input" in expected:
        primary_input = expected["primary_input"]
        if "code" not in expected:
            failures.append("primary_input requires expected code")
        elif not isinstance(primary_input, str) or not primary_input:
            failures.append("primary_input is not a non-empty string")
        else:
            try:
                generated = _generated_file_bytes(case)
                relative = str(_generated_relative(case, primary_input))
                data = generated[relative]
            except (KeyError, CorpusError):
                failures.append(
                    f"primary_input is not a generated input: "
                    f"{primary_input!r}")
            else:
                wanted_path = "<REPO>/" + relative
                matching = [
                    item for item in errors
                    if item["code"] == expected["code"]
                    and item["primary"] is not None
                ]
                if not any(
                        item["primary"]["path"] == wanted_path
                        and _is_integer(item["primary"]["start_byte"])
                        and _is_integer(item["primary"]["end_byte"])
                        and 0 <= item["primary"]["start_byte"]
                        < item["primary"]["end_byte"] <= len(data)
                        for item in matching):
                    failures.append(
                        f"expected {expected['code']} primary on "
                        f"{wanted_path} with "
                        f"0<=start<end<={len(data)}")
    if case["operation"].startswith("build_"):
        if artifacts.get("preexisting_changed"):
            failures.append(
                "preexisting outputs changed or disappeared: " + ", ".join(
                    artifacts["preexisting_changed"]))
        wants_build = expected.get("build_status", wanted)
        if wants_build == "ok":
            required = set(artifacts.get("required", []))
            missing = sorted(required - set(artifacts["published"]))
            if missing:
                failures.append(
                    "successful build is missing newly published files: " +
                    ", ".join(missing))
        if wants_build == "error" and artifacts["published"]:
            failures.append(
                "failed build published files: " + ", ".join(
                    artifacts["published"]))
    return failures


def _decode_expected_bytes(expected, key):
    if key not in expected:
        return None
    try:
        return base64.b64decode(expected[key], validate=True)
    except (ValueError, TypeError) as error:
        raise CorpusError(f"invalid {key} in run expectation: {error}") from error


def check_run_expectations(case, executions):
    """Check the exact repetition count and every recorded execution."""
    failures = []
    if "run_expectation" in case or "execution_repetitions" in case:
        try:
            expected = execution_repetitions(case)
        except CorpusError as error:
            return [str(error)]
        if len(executions) != expected:
            failures.append(
                f"expected {expected} program executions, got "
                f"{len(executions)}")
    if "run_expectation" not in case:
        return failures
    for index, execution in enumerate(executions):
        errors = check_run_expectation(case, execution)
        failures.extend(f"execution[{index}]: {error}" for error in errors)
    return failures


def check_run_expectation(case, execution):
    if "run_expectation" not in case:
        return []
    expectation = case.get("run_expectation")
    errors = run_expectation_errors(case)
    if errors:
        return ["invalid run_expectation: " + "; ".join(errors)]
    if execution is None:
        return ["run expectation exists but program did not execute"]
    failures = []
    if execution.get("timed_out"):
        failures.append("program timed out")
    if execution.get("cleanup_complete") is not True:
        failures.append("program execution cleanup was incomplete")
    exit_code = execution.get("exit")
    if not _is_integer(exit_code):
        failures.append("program exit is unavailable")
    elif exit_code < 0:
        failures.append(f"program terminated by signal {-exit_code}")
    if "exit" in expectation and exit_code != expectation["exit"]:
        failures.append(
            f"program exit {exit_code} != {expectation['exit']}")
    for stream in ("stdout", "stderr"):
        wanted = _decode_expected_bytes(expectation, stream + "_base64")
        if wanted is not None and execution["_" + stream] != wanted:
            failures.append(
                f"program {stream} differs: expected "
                f"{wanted!r}, got {execution['_' + stream]!r}")
    if expectation.get("status") == "panic":
        expected_exit = expectation.get("exit", PANIC_EXIT)
        if exit_code != expected_exit:
            failures.append(
                f"panic exit {exit_code} != {expected_exit}")
        prefix = expectation.get("panic_stderr_prefix")
        if prefix is not None and not execution["_stderr"].startswith(
                prefix.encode("utf-8", "surrogateescape")):
            failures.append(
                f"panic stderr does not start with {prefix!r}")
    return failures


def case_variants(case):
    operation = case["operation"]
    if operation == "build_native":
        opt = case.get("opt", [0, 2])
        values = [opt] if isinstance(opt, int) else list(opt)
        if not values:
            raise CorpusError(f"{case['id']}: no native optimization levels")
        return [{"name": f"O{value}", "opt": value} for value in values]
    return [{"name": "default", "opt": None}]


def _case_source(case, root, directory):
    if "source" in case:
        relative = Path(case["source"])
        if relative.is_absolute():
            raise CorpusError(f"{case['id']}: absolute source path")
        path = (root / relative).resolve()
        if not path.is_relative_to(root.resolve()):
            raise CorpusError(f"{case['id']}: source leaves repository")
        return path, path.read_bytes()
    if "source_base64" in case:
        try:
            data = base64.b64decode(case["source_base64"], validate=True)
        except (ValueError, TypeError) as error:
            raise CorpusError(f"{case['id']}: invalid source_base64: {error}") from error
        wanted = case.get("source_sha256")
        if wanted and digest_bytes(data) != wanted:
            raise CorpusError(f"{case['id']}: embedded source hash mismatch")
        path = directory / "input.wz"
        path.write_bytes(data)
        return path, data
    raise CorpusError(f"{case['id']}: syntax case has no source")


def _generated_relative(case, relative):
    case_id = case.get("id", "<case>")
    if not isinstance(relative, str) or not relative:
        raise CorpusError(f"{case_id}: generated path is not a non-empty string")
    if "\x00" in relative:
        raise CorpusError(f"{case_id}: generated path contains NUL")
    path = PurePosixPath(relative)
    if (path.is_absolute() or str(path) != relative
            or any(part in ("", ".", "..") for part in path.parts)):
        raise CorpusError(
            f"{case_id}: unsafe generated path {relative!r}")
    return path


def _generated_file_bytes(case):
    files = case.get("generated_files")
    if files is None:
        return {}
    case_id = case.get("id", "<case>")
    if not isinstance(files, dict) or not files:
        raise CorpusError(
            f"{case_id}: generated_files is not a non-empty object")
    decoded = {}
    canonical = set()
    for relative, encoded in files.items():
        name = str(_generated_relative(case, relative))
        if name in canonical:
            raise CorpusError(
                f"{case_id}: duplicate generated path {name!r}")
        canonical.add(name)
        if not isinstance(encoded, str):
            raise CorpusError(
                f"{case_id}: generated file {relative!r} is not base64")
        try:
            data = base64.b64decode(encoded, validate=True)
        except (ValueError, TypeError) as error:
            raise CorpusError(
                f"{case_id}: invalid generated file {relative!r}: {error}"
            ) from error
        decoded[name] = data
    return decoded


def _generated_target(case, root, relative):
    path = _generated_relative(case, relative)
    root = Path(root)
    resolved_root = root.resolve(strict=True)
    target = root.joinpath(*path.parts)
    current = root
    for part in path.parts:
        current = current / part
        if current.is_symlink():
            raise CorpusError(
                f"{case.get('id', '<case>')}: generated path escapes "
                f"through symlink {relative!r}")
    if not target.resolve(strict=False).is_relative_to(resolved_root):
        raise CorpusError(
            f"{case.get('id', '<case>')}: generated path escapes root "
            f"{relative!r}")
    return target


def _materialize_generated(case, out_root, role):
    files = _generated_file_bytes(case)
    if not files:
        return None
    out_root = Path(out_root)
    root = (out_root / "generated" / safe_name(case["id"])
            / safe_name(role))
    if not root.resolve(strict=False).is_relative_to(
            out_root.resolve(strict=True)):
        raise CorpusError(
            f"{case['id']}: generated directory escapes output root")
    if root.exists() or root.is_symlink():
        if root.is_symlink() or not root.is_dir():
            raise CorpusError(
                f"{case['id']}: generated directory is not a real directory")
        for relative in files:
            _generated_target(case, root, relative)
        return root
    root.mkdir(parents=True)
    try:
        for relative, data in sorted(files.items()):
            target = _generated_target(case, root, relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            _generated_target(case, root, relative)
            if target.exists() or target.is_symlink():
                raise CorpusError(
                    f"{case['id']}: generated file already exists: "
                    f"{relative!r}")
            target.write_bytes(data)
    except BaseException:
        shutil.rmtree(root, ignore_errors=True)
        raise
    return root


def _case_manifest(case, root, generated_root=None):
    if generated_root is None:
        return (root / case["manifest"]).resolve()
    relative = _generated_relative(case, case.get("manifest"))
    base = Path(generated_root)
    manifest = base.joinpath(*relative.parts).resolve()
    if not manifest.is_relative_to(base.resolve()):
        raise CorpusError(
            f"{case['id']}: generated manifest escapes its root")
    return manifest


def _project_source(manifest):
    files = {}
    for path in sorted(manifest.parent.rglob("*")):
        if path.is_file() and (path.suffix == ".wz" or path.name.endswith(".toml")
                               or path.suffix == ".json"):
            files[str(path)] = digest(path)
    return files


def _generated_snapshot(case, root):
    root = Path(root)
    if root.is_symlink():
        raise CorpusError(
            f"{case['id']}: generated root is a symlink")
    resolved_root = root.resolve(strict=True)
    files = {}

    def visit(directory, prefix):
        try:
            names = sorted(os.listdir(directory))
        except OSError as error:
            raise CorpusError(
                f"{case['id']}: cannot read generated input: {error}"
            ) from error
        for name in names:
            relative = f"{prefix}/{name}" if prefix else name
            _generated_relative(case, relative)
            path = directory / name
            try:
                metadata = os.lstat(path)
            except OSError as error:
                raise CorpusError(
                    f"{case['id']}: cannot stat generated input "
                    f"{relative!r}: {error}") from error
            if stat.S_ISLNK(metadata.st_mode):
                raise CorpusError(
                    f"{case['id']}: generated input is a symlink: "
                    f"{relative!r}")
            if stat.S_ISDIR(metadata.st_mode):
                visit(path, relative)
            elif stat.S_ISREG(metadata.st_mode):
                if not path.resolve(strict=True).is_relative_to(
                        resolved_root):
                    raise CorpusError(
                        f"{case['id']}: generated input escapes root: "
                        f"{relative!r}")
                files[str(path)] = digest(path)
            else:
                raise CorpusError(
                    f"{case['id']}: generated input is not a regular file: "
                    f"{relative!r}")

    visit(root, "")
    return files


def snapshot_case(case, root, directory, generated_root=None):
    operation = case["operation"]
    if operation == "syntax":
        path, data = _case_source(case, root, directory)
        return {str(path): digest_bytes(data)}
    if generated_root is not None:
        return _generated_snapshot(case, generated_root)
    manifest = _case_manifest(case, root, generated_root)
    if not manifest.is_file():
        raise CorpusError(f"{case['id']}: missing manifest {manifest}")
    return _project_source(manifest)


def build_command(case, compiler, root, directory, variant,
                  generated_root=None):
    operation = case["operation"]
    if operation == "syntax":
        path, _ = _case_source(case, root, directory)
        return [str(compiler), "check", "--syntax-only", "--format", "json",
                str(path)]
    manifest = _case_manifest(case, root, generated_root)
    if operation == "check":
        command = [str(compiler), "check", "--project", str(manifest),
                   "--format", "json"]
        if case.get("backend"):
            command += ["--backend", case["backend"]]
        return command
    out_dir = directory / "out"
    if operation == "build_native":
        return [str(compiler), "build", "--project", str(manifest),
                "--backend", "native", "--opt", str(variant["opt"]),
                "--out-dir", str(out_dir), "--format", "json"]
    if operation == "build_python":
        return [str(compiler), "build", "--project", str(manifest),
                "--backend", "python", "--out-dir", str(out_dir),
                "--format", "json"]
    raise CorpusError(f"{case['id']}: unsupported operation {operation}")


def required_artifacts(case):
    if case["operation"] == "build_native":
        return ["program"]
    if case["operation"] == "build_python":
        return ["program.py", "langrt.py"]
    return []


def _output_entry(path, relative):
    metadata = path.lstat()
    entry = {"kind": "other", "mode": oct(stat.S_IMODE(metadata.st_mode)),
             "size": metadata.st_size}
    if path.is_symlink():
        entry["kind"] = "symlink"
        entry["target"] = os.readlink(path)
    elif path.is_dir():
        entry["kind"] = "directory"
    elif path.is_file():
        entry["kind"] = "file"
        entry["sha256"] = digest(path)
    return relative, entry


def _scan_output_tree(out):
    entries = {}
    if not (out.exists() or out.is_symlink()):
        return entries

    def visit(path, relative):
        relative, entry = _output_entry(path, relative)
        entries[relative] = entry
        if entry["kind"] == "directory":
            for child in sorted(path.iterdir()):
                visit(child, f"{relative}/{child.name}")

    if out.is_dir() and not out.is_symlink():
        for child in sorted(out.iterdir()):
            visit(child, child.name)
    else:
        visit(out, out.name)
    return entries


def _seed_outputs(case, directory):
    seeds = case.get("preexisting_outputs") or {}
    if not isinstance(seeds, dict):
        raise CorpusError(f"{case['id']}: preexisting_outputs is not an object")
    out = directory / "out"
    for relative, encoded in seeds.items():
        path = Path(relative)
        if path.is_absolute() or ".." in path.parts or not path.parts:
            raise CorpusError(
                f"{case['id']}: invalid preexisting output {relative!r}")
        if not isinstance(encoded, str):
            raise CorpusError(
                f"{case['id']}: preexisting output {relative!r} is not base64")
        try:
            data = base64.b64decode(encoded, validate=True)
        except (ValueError, TypeError) as error:
            raise CorpusError(
                f"{case['id']}: invalid preexisting output {relative!r}: {error}"
            ) from error
        target = out / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return _scan_output_tree(out)


def _output_kind(path):
    if path.is_symlink():
        return "symlink"
    if path.is_dir():
        return "directory"
    if path.is_file():
        return "file"
    if path.exists():
        return "other"
    return "absent"


def _artifact_info(case, directory, preexisting=None):
    preexisting = preexisting or {}
    required = required_artifacts(case)
    out = directory / "out"
    output_kind = _output_kind(out)
    output = {"output_exists": output_kind != "absent",
              "output_kind": output_kind}
    if not case["operation"].startswith("build_"):
        return {"published": [], "files": {}, "entries": {},
                "preexisting": preexisting, "preexisting_changed": [],
                "required": [], **output}
    entries = _scan_output_tree(out)
    published = sorted(
        name for name, entry in entries.items()
        if preexisting.get(name) != entry)
    preexisting_changed = sorted(
        name for name, entry in preexisting.items()
        if entries.get(name) != entry)
    return {
        "published": published,
        "files": entries,
        "entries": entries,
        "preexisting": preexisting,
        "preexisting_changed": preexisting_changed,
        "required": required,
        **output,
    }


def _run_process_impl(argv, cwd, env, timeout, stdin=None):
    """Run one bounded process; ``stdin`` is an optional file path.

    With ``stdin=None`` the child keeps the caller's stdin, exactly as every
    pre-existing call does.  A path is opened read-only and handed to the
    child as fd 0 so callers can replay exact bytes (UTF-8, NUL, EOF).
    """
    input_file = open(stdin, "rb") if stdin is not None else None
    try:
        process = subprocess.Popen(
            [str(part) for part in argv], cwd=str(cwd), env=env,
            stdin=input_file, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            bufsize=0, start_new_session=True)
    finally:
        if input_file is not None:
            input_file.close()
    buffers = {process.stdout: b"", process.stderr: b""}
    selector = selectors.DefaultSelector()
    for stream in (process.stdout, process.stderr):
        os.set_blocking(stream.fileno(), False)
        selector.register(stream, selectors.EVENT_READ)

    def drain(wait):
        if not selector.get_map():
            return
        for key, _ in selector.select(max(0.0, wait)):
            stream = key.fileobj
            try:
                chunk = os.read(stream.fileno(), 65536)
            except (BlockingIOError, OSError):
                chunk = b""
            if chunk:
                buffers[stream] += chunk
                continue
            try:
                selector.unregister(stream)
            except KeyError:
                pass
            try:
                stream.close()
            except OSError:
                pass

    timed_out = False
    deadline = time.monotonic() + timeout
    while process.poll() is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            timed_out = True
            break
        drain(min(0.05, remaining))
    if timed_out:
        try:
            os.killpg(process.pid, 9)
        except ProcessLookupError:
            pass

    cleanup_complete = True
    cleanup_deadline = time.monotonic() + CLEANUP_TIMEOUT
    while time.monotonic() < cleanup_deadline:
        drain(min(0.05, cleanup_deadline - time.monotonic()))
        if process.poll() is not None and not selector.get_map():
            break
    if process.poll() is None:
        try:
            process.kill()
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=CLEANUP_TIMEOUT)
        except subprocess.TimeoutExpired:
            cleanup_complete = False
    abandoned_pipes = bool(selector.get_map())
    if abandoned_pipes:
        cleanup_complete = False
        for key in list(selector.get_map().values()):
            try:
                selector.unregister(key.fileobj)
            except KeyError:
                pass
            try:
                key.fileobj.close()
            except OSError:
                pass
    if process.poll() is None:
        try:
            process.wait(timeout=CLEANUP_TIMEOUT)
        except subprocess.TimeoutExpired:
            cleanup_complete = False
    exit_code = process.returncode if process.returncode is not None else -9
    stdout = buffers[process.stdout]
    stderr = buffers[process.stderr]
    return {
        "exit": exit_code,
        "stdout": stdout,
        "stderr": stderr,
        "timed_out": timed_out,
        "cleanup_complete": cleanup_complete,
        "abandoned_pipes": abandoned_pipes,
    }


def _run_process(argv, cwd, env, timeout):
    result = _run_process_impl(argv, cwd, env, timeout)
    return (result["exit"], result["stdout"], result["stderr"],
            result["timed_out"])


def _tool_allowed(tools):
    return {str(path.resolve()) for path in tools.values()}


def _compiler_allowed(compiler, tools, extra):
    return {str(Path(compiler).resolve())} | _tool_allowed(tools) | {
        str(path.resolve()) for path in extra}


def _execute_artifact(case, directory, python, cwd, env, timeout, strace,
                      index=0):
    if case["operation"] == "build_native":
        artifact = directory / "out" / "program"
        argv = [str(artifact)]
    elif case["operation"] == "build_python":
        artifact = directory / "out" / "program.py"
        argv = [str(python), artifact.name]
    else:
        return None
    if not artifact.is_file():
        return None
    execution_env = dict(env)
    execution_env["PYTHONDONTWRITEBYTECODE"] = "1"
    execution_dir = directory / "executions" / str(index)
    execution_dir.mkdir(parents=True, exist_ok=False)
    trace_path = execution_dir / "execute.exec.log"
    traced = [str(strace), "--seccomp-bpf", "-f", "-qq", "-s", "16384",
              "-e", "trace=execve,execveat", "-o", str(trace_path), *argv]
    process = _run_process_impl(traced, cwd, execution_env, timeout)
    exit_code, stdout, stderr = (
        process["exit"], process["stdout"], process["stderr"])
    timed_out = process["timed_out"]
    stdout_path = execution_dir / "execute.stdout.bin"
    stderr_path = execution_dir / "execute.stderr.bin"
    stdout_path.write_bytes(stdout)
    stderr_path.write_bytes(stderr)
    try:
        observed = parse_trace(trace_path)
        execution_trace = {"status": "ok", "observed": observed,
                           "policy": "program execution; not compiler whitelist"}
    except TraceError as error:
        execution_trace = {"status": "unverified", "error": str(error),
                           "policy": "program execution; not compiler whitelist"}
    relative = execution_dir.relative_to(directory)
    record = {
        "index": index,
        "directory": str(relative),
        "argv": argv,
        "cwd": str(cwd),
        "exit": exit_code,
        "timed_out": timed_out,
        "cleanup_complete": process["cleanup_complete"],
        "abandoned_pipes": process["abandoned_pipes"],
        "stdout_path": str(relative / "execute.stdout.bin"),
        "stderr_path": str(relative / "execute.stderr.bin"),
        "stdout_sha256": digest_bytes(stdout),
        "stderr_sha256": digest_bytes(stderr),
        "trace_path": str(relative / "execute.exec.log"),
        "trace_scope": "program_execution_not_compiler_whitelist",
        "trace": execution_trace,
    }
    write_json(execution_dir / "execute.json", record)
    record["_stdout"] = stdout
    record["_stderr"] = stderr
    return record


def run_one(case, compiler, role, out_root, context, variant, is_reference):
    directory = out_root / "runs" / safe_name(case["id"]) / variant["name"] / role
    if directory.exists() and any(directory.iterdir()):
        raise CorpusError(f"run evidence directory is not empty: {directory}")
    directory.mkdir(parents=True, exist_ok=True)
    generated_root = _materialize_generated(case, out_root, role)
    preexisting = _seed_outputs(case, directory)
    command = build_command(
        case, compiler, context["root"], directory, variant, generated_root)
    before = snapshot_case(
        case, context["root"], directory, generated_root)
    write_json(directory / "input.json", before)
    argv_record = {
        "case": case["id"],
        "role": role,
        "operation": case["operation"],
        "variant": variant,
        "cwd": str(context["root"]),
        "argv": command,
        "trace_policy": (
            "reference: candidate + fixed LLVM tools + explicit stage0 helper"
            if is_reference else
            "candidate: current candidate + fixed LLVM tools only"),
    }
    write_json(directory / "argv.json", argv_record)
    trace_path = directory / "exec.log"
    traced = [str(context["strace"]), "--seccomp-bpf", "-f", "-qq", "-s", "16384",
              "-e", "trace=execve,execveat", "-o", str(trace_path), *command]
    process = _run_process_impl(
        traced, context["root"], context["env"], context["timeout"])
    exit_code, stdout, stderr = (
        process["exit"], process["stdout"], process["stderr"])
    timed_out = process["timed_out"]
    (directory / "stdout.bin").write_bytes(stdout)
    (directory / "stderr.bin").write_bytes(stderr)
    write_json(directory / "compiler.process.json", {
        "case": case["id"],
        "role": role,
        "operation": case["operation"],
        "variant": variant,
        "cwd": str(context["root"]),
        "argv": command,
        "timeout": context["timeout"],
        "exit": exit_code,
        "timed_out": timed_out,
        "cleanup_complete": process["cleanup_complete"],
        "abandoned_pipes": process["abandoned_pipes"],
        "stdout_path": "stdout.bin",
        "stderr_path": "stderr.bin",
        "stdout_sha256": digest_bytes(stdout),
        "stderr_sha256": digest_bytes(stderr),
        "trace_path": "exec.log",
    })
    after = snapshot_case(
        case, context["root"], directory, generated_root)
    failures = []
    if before != after:
        failures.append("input snapshot changed while the compiler ran")
    if timed_out:
        failures.append("compiler timed out and was terminated")
    if _is_integer(exit_code) and exit_code < 0:
        failures.append(
            f"compiler terminated by signal {-exit_code}")
    if not process["cleanup_complete"]:
        failures.append(
            "compiler process cleanup was incomplete; pipes were abandoned")
    allowed = _compiler_allowed(
        compiler, context["tools"],
        context["reference_extra"] if is_reference else [])
    try:
        trace = audit_trace(trace_path, allowed)
    except TraceError as error:
        trace = {"status": "violation", "error": str(error),
                 "allowed": sorted(allowed)}
        failures.append(str(error))
    normalization_roots = list(context["roots"])
    if generated_root is not None:
        normalization_roots.append(generated_root)
    normalized_stderr = normalize_text(
        stderr.decode("utf-8", "surrogateescape"),
        directory, normalization_roots).encode("utf-8", "surrogateescape")
    (directory / "stderr.normalized.bin").write_bytes(normalized_stderr)
    document = None
    json_error = None
    try:
        document = parse_diagnostic_stdout(stdout)
        normalized = normalize_json(
            document, directory, normalization_roots)
        write_json(directory / "diagnostics.normalized.json", normalized)
    except CorpusError as error:
        json_error = str(error)
        normalized = None
        failures.append(json_error)
    artifacts = _artifact_info(case, directory, preexisting)
    write_json(directory / "artifacts.json", artifacts)
    repetitions = execution_repetitions(case)
    execution_timeout_value = execution_timeout(case, context["timeout"])
    executions = []
    if (case["operation"].startswith("build_")
            and exit_code == 0 and not timed_out
            and process["cleanup_complete"] and document is not None
            and document["status"] == "ok"
            and artifacts["published"]
            and trace["status"] == "ok"):
        for index in range(repetitions):
            execution = _execute_artifact(
                case, directory, context["python"], directory / "out",
                context["env"], execution_timeout_value, context["strace"],
                index)
            if execution is None:
                failures.append(
                    f"program execution {index} did not run: missing artifact")
                break
            executions.append(execution)
            if execution["timed_out"]:
                failures.append(
                    f"program execution {index} timed out and was terminated")
            if _is_integer(execution["exit"]) and execution["exit"] < 0:
                failures.append(
                    f"program execution {index} terminated by signal "
                    f"{-execution['exit']}")
            if execution.get("cleanup_complete") is not True:
                failures.append(
                    f"program execution {index} cleanup was incomplete")
        if len(executions) != repetitions:
            failures.append(
                f"program executed {len(executions)} of {repetitions} "
                "required repetitions")
    execution = executions[0] if executions else None
    record = {
        "case": case["id"],
        "role": role,
        "variant": variant["name"],
        "compiler": str(Path(compiler).resolve()),
        "compiler_sha256": digest(Path(compiler)),
        "hidden_input": bool(
            "source_base64" in case or generated_root is not None),
        "directory": _relative(directory, out_root),
        "exit": exit_code,
        "timed_out": timed_out,
        "cleanup_complete": process["cleanup_complete"],
        "abandoned_pipes": process["abandoned_pipes"],
        "stdout_path": "stdout.bin",
        "stderr_path": "stderr.bin",
        "stdout_sha256": digest_bytes(stdout),
        "stderr_sha256": digest_bytes(stderr),
        "normalized_stderr_sha256": digest_bytes(normalized_stderr),
        "diagnostics_path": ("diagnostics.normalized.json" if normalized is not None
                             else None),
        "json_error": json_error,
        "artifacts": artifacts,
        "execution_repetitions": repetitions,
        "executions": [public_execution(item) for item in executions],
        "execution": public_execution(execution),
        "trace": trace,
        "status": "passed" if not failures else "failed",
        "failures": failures,
        "_document": document,
        "_normalized": normalized,
        "_normalized_stderr": normalized_stderr,
        "_executions": executions,
    }
    if generated_root is not None:
        record["generated_files"] = {
            relative: digest_bytes(data)
            for relative, data in sorted(
                _generated_file_bytes(case).items())
        }
        record["generated_directory"] = _relative(generated_root, out_root)
    return record


def public_execution(execution):
    return None if execution is None else {
        key: value for key, value in execution.items()
        if not key.startswith("_")}


def public_run(record):
    return {key: value for key, value in record.items()
            if not key.startswith("_")}


def _execution_sequence(record):
    if "_executions" in record:
        return record["_executions"] or []
    execution = record.get("_execution")
    return [] if execution is None else [execution]


def compare_runs(case, reference, candidate):
    failures = []
    if candidate["trace"]["status"] != "ok":
        failures.append("candidate trace audit failed")
    if reference["exit"] != candidate["exit"]:
        failures.append(
            f"exit differs: reference {reference['exit']}, "
            f"candidate {candidate['exit']}")
    if reference["timed_out"] != candidate["timed_out"]:
        failures.append("timeout status differs")
    if reference["_normalized_stderr"] != candidate["_normalized_stderr"]:
        failures.append("normalized stderr differs")
    reference_document = reference["_normalized"]
    candidate_document = candidate["_normalized"]
    json_changes = []
    if reference_document is None or candidate_document is None:
        failures.append("JSON diagnostics could not be compared")
    else:
        json_changes = json_diff(reference_document, candidate_document)
        if json_changes:
            failures.append("normalized JSON diagnostics differ")
    evidence = {}
    if json_changes:
        evidence["json_diff"] = json_changes
    if reference["_normalized_stderr"] != candidate["_normalized_stderr"]:
        evidence["stderr"] = {
            "reference": reference["_normalized_stderr"].decode(
                "utf-8", "backslashreplace")[-4000:],
            "candidate": candidate["_normalized_stderr"].decode(
                "utf-8", "backslashreplace")[-4000:],
        }
    reference_executions = _execution_sequence(reference)
    candidate_executions = _execution_sequence(candidate)
    if len(reference_executions) != len(candidate_executions):
        failures.append(
            f"program execution count differs: reference "
            f"{len(reference_executions)}, candidate "
            f"{len(candidate_executions)}")
    for index, (reference_execution, candidate_execution) in enumerate(
            zip(reference_executions, candidate_executions)):
        if reference_execution["exit"] != candidate_execution["exit"]:
            failures.append(
                f"execution[{index}] program exit differs: "
                f"{reference_execution['exit']} vs "
                f"{candidate_execution['exit']}")
        for key in ("timed_out", "cleanup_complete"):
            if reference_execution.get(key) != candidate_execution.get(key):
                failures.append(
                    f"execution[{index}] program {key} differs: "
                    f"{reference_execution.get(key)} vs "
                    f"{candidate_execution.get(key)}")
        for stream in ("stdout", "stderr"):
            if reference_execution["_" + stream] != candidate_execution["_" + stream]:
                failures.append(
                    f"execution[{index}] program {stream} differs")
                evidence[f"program_{stream}_execution_{index}"] = {
                    "reference": reference_execution["_" + stream].decode(
                        "utf-8", "backslashreplace")[-4000:],
                    "candidate": candidate_execution["_" + stream].decode(
                        "utf-8", "backslashreplace")[-4000:],
                }
    if reference["artifacts"]["published"] != candidate["artifacts"]["published"]:
        failures.append("published artifact set differs")
    return failures, evidence


def run_case(case, reference, candidates, out_root, context):
    record = {"id": case["id"], "operation": case["operation"],
              "variants": [], "status": "passed", "failures": []}
    for variant in case_variants(case):
        reference_run = run_one(
            case, reference, "reference", out_root, context, variant, True)
        if reference_run["_document"] is None:
            reference_failures = ["reference JSON diagnostics are malformed"]
        else:
            reference_failures = check_expected(
                case, reference_run["_normalized"], reference_run["exit"],
                reference_run["_normalized_stderr"], reference_run["artifacts"])
        reference_failures += reference_run["failures"]
        reference_failures += check_run_expectations(
            case, reference_run["_executions"])
        variant_record = {
            "variant": variant,
            "reference": public_run(reference_run),
            "reference_failures": reference_failures,
            "candidates": [],
        }
        if reference_failures:
            record["failures"].append({
                "variant": variant["name"], "side": "reference",
                "errors": reference_failures})
        for index, candidate in enumerate(candidates, 1):
            label = f"candidate{index}"
            candidate_run = run_one(
                case, candidate, label, out_root, context, variant, False)
            if candidate_run["_document"] is None:
                candidate_failures = ["candidate JSON diagnostics are malformed"]
            else:
                candidate_failures = check_expected(
                    case, candidate_run["_normalized"], candidate_run["exit"],
                    candidate_run["_normalized_stderr"],
                    candidate_run["artifacts"])
            candidate_failures += check_run_expectations(
                case, candidate_run["_executions"])
            differences, evidence = compare_runs(
                case, reference_run, candidate_run)
            all_failures = candidate_failures + differences
            if candidate_run["status"] == "failed":
                all_failures += candidate_run["failures"]
            variant_record["candidates"].append({
                "label": label,
                "path": str(Path(candidate).resolve()),
                "run": public_run(candidate_run),
                "expected_failures": candidate_failures,
                "comparison_failures": differences,
                "evidence": evidence,
                "status": "passed" if not all_failures else "failed",
                "failures": all_failures,
            })
            if all_failures:
                record["failures"].append({
                    "variant": variant["name"], "side": label,
                    "errors": all_failures})
        record["variants"].append(variant_record)
    if record["failures"]:
        record["status"] = "failed"
    write_json(
        out_root / "cases" / (safe_name(case["id"]) + ".json"),
        public_run(record))
    return record


def load_inventory(path):
    inventory = load_json(path)
    if inventory.get("schema") != SCHEMA:
        raise CorpusError(
            f"{path}: expected inventory schema {SCHEMA}, got {inventory.get('schema')}")
    return inventory


def _case_classification_errors(case):
    errors = []
    case_id = case.get("id", "<unknown>")
    if not isinstance(case.get("enabled"), bool):
        errors.append(f"{case_id}: enabled is not a boolean")
    if case.get("support") not in CASE_SUPPORT:
        errors.append(
            f"{case_id}: invalid or missing support {case.get('support')!r}")
    if case.get("adaptation") not in CASE_ADAPTATION:
        errors.append(
            f"{case_id}: invalid or missing adaptation "
            f"{case.get('adaptation')!r}")
    if "runner" in case:
        runner = case.get("runner")
        if runner is not None and (
                not isinstance(runner, str) or not runner):
            errors.append(f"{case_id}: invalid runner path")
        runner_status = case.get("runner_status")
        if runner_status not in RUNNER_STATUSES:
            errors.append(
                f"{case_id}: invalid or missing runner_status "
                f"{runner_status!r}")
        if runner_status == "component" and not (
                isinstance(case.get("reason"), str) and case["reason"].strip()):
            errors.append(f"{case_id}: component classification lacks a reason")
    if case.get("enabled"):
        if case.get("support") != "public_cli":
            errors.append(f"{case_id}: enabled case is not public_cli")
        if case.get("adaptation") != "ready":
            errors.append(f"{case_id}: enabled case is not adaptation=ready")
    errors.extend(execution_configuration_errors(case))
    return errors


def _tracked_worktree_changes(root):
    try:
        paths = _git_paths(root, "diff", "--name-only", "-z", "HEAD")
    except CorpusError:
        return []
    return sorted(path for path in paths if _is_frozen_corpus_path(path))


def _inventory_errors(root, inventory):
    errors = []
    files = inventory.get("files")
    if not isinstance(files, list):
        return ["inventory files is not an array"]
    seen = set()
    roles = {}
    file_hashes = {}
    for entry in files:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            errors.append("inventory file entry without path")
            continue
        relative = entry["path"]
        path = Path(relative)
        if path.is_absolute() or ".." in path.parts:
            errors.append(f"inventory path escapes repository: {relative}")
            continue
        if relative in seen:
            errors.append(f"duplicate inventory path: {relative}")
        seen.add(relative)
        roles.setdefault(entry.get("role"), set()).add(relative)
        file_hashes[relative] = entry.get("sha256")
        absolute = root / relative
        if not absolute.is_file():
            errors.append(f"frozen file missing: {relative}")
            continue
        wanted = entry.get("sha256")
        if not isinstance(wanted, str) or digest(absolute) != wanted:
            errors.append(f"frozen hash mismatch: {relative}")
    try:
        actual = set(frozen_candidates(root))
    except CorpusError as error:
        errors.append(str(error))
        actual = set()
    expected = set(seen)
    for relative in sorted(actual - expected):
        errors.append(f"tracked corpus file is not in inventory: {relative}")
    for relative in sorted(expected - actual):
        errors.append(f"inventory path is no longer tracked corpus: {relative}")

    case_ids = set()
    cases = inventory.get("cases", [])
    if not isinstance(cases, list):
        errors.append("inventory cases is not an array")
        cases = []
    for case in cases:
        if not isinstance(case, dict) or not isinstance(case.get("id"), str):
            errors.append("case without id")
            continue
        case_id = case["id"]
        if case_id in case_ids:
            errors.append(f"duplicate case id: {case_id}")
        case_ids.add(case_id)
        operation = case.get("operation")
        if operation not in COMMANDS and case.get("enabled"):
            errors.append(f"{case_id}: enabled case has unsupported operation")
        if operation in ("check", "build_native", "build_python") and not case.get(
                "manifest"):
            errors.append(f"{case_id}: project case has no manifest")
        if operation == "syntax" and not (
                case.get("source") or case.get("source_base64")):
            errors.append(f"{case_id}: syntax case has no source")
        errors.extend(_case_classification_errors(case))
        try:
            _generated_file_bytes(case)
        except CorpusError as error:
            errors.append(str(error))
        if case.get("enabled") and operation in ("build_native", "build_python"):
            expected = case.get("expected") or {}
            if expected_status(expected) == "ok" and not (
                    recorded_run_expectation(case)):
                errors.append(
                    f"{case_id}: executable positive lacks a valid run expectation")

    for entry in files:
        if not isinstance(entry, dict):
            continue
        role = entry.get("role")
        if role in ("woozy_source", "stdlib_source", "compiler_source",
                    "auxiliary_dsl_input", "manifest"):
            assignments = entry.get("assignment") or []
            if not assignments:
                errors.append(
                    f"corpus file has no case assignment: {entry.get('path')}")
            for assignment in assignments:
                if assignment not in case_ids:
                    errors.append(
                        f"{entry.get('path')}: unknown case {assignment}")

    runners = inventory.get("runners")
    if not isinstance(runners, list):
        errors.append("inventory runners is not an array")
        runners = []
    runner_seen = set()
    for runner in runners:
        if not isinstance(runner, dict):
            errors.append("runner entry is not an object")
            continue
        path = runner.get("path")
        if not isinstance(path, str) or not path:
            errors.append("runner entry without path")
            continue
        if path in runner_seen:
            errors.append(f"duplicate runner entry: {path}")
        runner_seen.add(path)
        if path not in seen:
            errors.append(f"runner source is not frozen: {path}")
        status = runner.get("status")
        if status not in RUNNER_STATUSES:
            errors.append(f"runner status missing or invalid: {path} ({status!r})")
        reason = runner.get("reason")
        if not isinstance(reason, str) or not reason.strip():
            errors.append(f"runner classification has no reason: {path}")
        if status == "component" and not (
                isinstance(reason, str) and reason.strip()):
            errors.append(f"component runner has no reason: {path}")
    runner_sources = roles.get("runner_source", set())
    for path in sorted(runner_sources - runner_seen):
        errors.append(f"runner_source is not classified in runners: {path}")
    for path in sorted(runner_seen - runner_sources):
        errors.append(f"runner entry is not a frozen runner_source: {path}")

    refreeze = inventory.get("refreeze") or {}
    for path in refreeze.get("new_freeze_paths", []) or []:
        if path not in seen:
            errors.append(f"refreeze new path is not in inventory: {path}")
    for record in refreeze.get("modified_freeze_paths", []) or []:
        path = record.get("path") if isinstance(record, dict) else None
        if path not in seen:
            errors.append(f"refreeze modified path is not in inventory: {path}")
            continue
        if record.get("new_sha256") != file_hashes.get(path):
            errors.append(
                f"refreeze new hash does not match inventory: {path}")
    return errors


def coverage_report(inventory, untracked_files=None, dirty_files=None):
    untracked_files = sorted(untracked_files or [])
    dirty_files = sorted(dirty_files or [])
    cases = {case["id"]: case for case in inventory.get("cases", [])}
    sources = [entry for entry in inventory.get("files", [])
               if entry.get("role") in (
                   "woozy_source", "stdlib_source", "compiler_source",
                   "auxiliary_dsl_input")]
    manifests = [entry for entry in inventory.get("files", [])
                 if entry.get("role") == "manifest"]
    runners = [entry for entry in inventory.get("files", [])
               if entry.get("role") == "runner_source"]
    unassigned_sources = []
    unassigned_manifests = []
    no_enabled_case = []
    for entry in sources:
        assignments = entry.get("assignment") or []
        if not assignments:
            unassigned_sources.append(entry["path"])
        elif not any(cases.get(item, {}).get("enabled") for item in assignments):
            no_enabled_case.append(entry["path"])
    for entry in manifests:
        assignments = entry.get("assignment") or []
        if not assignments:
            unassigned_manifests.append(entry["path"])
    missing_run = []
    invalid_cases = []
    for case in cases.values():
        if _case_classification_errors(case):
            invalid_cases.append(case["id"])
        if case.get("operation") not in ("build_native", "build_python"):
            continue
        expected = case.get("expected") or {}
        if expected_status(expected) == "ok" and not (
                recorded_run_expectation(case)):
            missing_run.append(case["id"])
    disabled = [case["id"] for case in cases.values() if not case.get("enabled")]
    not_ready = [case["id"] for case in cases.values()
                 if case.get("adaptation") == "required"]
    runner_status = {}
    unclassified_runners = []
    for runner in inventory.get("runners", []):
        state = runner.get("status")
        path = runner.get("path")
        if state not in RUNNER_STATUSES:
            unclassified_runners.append(path)
            state = "unclassified"
        if state == "component" and not (
                isinstance(runner.get("reason"), str)
                and runner["reason"].strip()):
            unclassified_runners.append(path)
        runner_status.setdefault(state, []).append(path)
    runner_sources = {entry["path"] for entry in runners}
    missing_runner_classification = sorted(
        runner_sources - set(runner_status.get("public_cli", [])
                             + runner_status.get("component", [])
                             + runner_status.get("adaptation_required", [])
                             + runner_status.get("unmapped", [])
                             + runner_status.get("unclassified", [])))
    reasons = []
    if unassigned_sources:
        reasons.append({"code": "unassigned_source_files",
                        "count": len(unassigned_sources)})
    if unassigned_manifests:
        reasons.append({"code": "unassigned_manifests",
                        "count": len(unassigned_manifests)})
    if no_enabled_case:
        reasons.append({"code": "files_without_enabled_case",
                        "count": len(no_enabled_case)})
    if missing_run:
        reasons.append({"code": "missing_run_expectation",
                        "count": len(missing_run)})
    if invalid_cases:
        reasons.append({"code": "invalid_case_classification",
                        "count": len(invalid_cases)})
    if disabled:
        reasons.append({"code": "disabled_cases", "count": len(disabled)})
    if not_ready:
        reasons.append({"code": "adaptation_required", "count": len(not_ready)})
    adaptation_runners = runner_status.get("adaptation_required", [])
    if adaptation_runners:
        reasons.append({"code": "runners_require_adaptation",
                        "count": len(adaptation_runners)})
    unmapped_runners = runner_status.get("unmapped", [])
    if unmapped_runners:
        reasons.append({"code": "unmapped_runners",
                        "count": len(unmapped_runners)})
    if unclassified_runners or missing_runner_classification:
        reasons.append({"code": "unclassified_runners",
                        "count": len(set(unclassified_runners)
                                      | set(missing_runner_classification))})
    if untracked_files:
        reasons.append({"code": "untracked_corpus_files",
                        "count": len(untracked_files)})
    if dirty_files:
        reasons.append({"code": "dirty_tracked_corpus_files",
                        "count": len(dirty_files)})
    complete = not reasons
    return {
        "source_files": len(sources),
        "stdlib_sources": sum(entry["role"] == "stdlib_source" for entry in sources),
        "compiler_sources": sum(entry["role"] == "compiler_source" for entry in sources),
        "auxiliary_dsl_inputs": sum(
            entry["role"] == "auxiliary_dsl_input" for entry in sources),
        "manifests": len(manifests),
        "runner_sources": len(runners),
        "assigned_sources": len(sources) - len(unassigned_sources),
        "assigned_manifests": len(manifests) - len(unassigned_manifests),
        "unassigned_sources": unassigned_sources,
        "unassigned_manifests": unassigned_manifests,
        "files_without_enabled_case": no_enabled_case,
        "cases": {
            "total": len(cases),
            "enabled": len(cases) - len(disabled),
            "disabled": len(disabled),
            "adaptation_required": len(not_ready),
        },
        "missing_run_expectations": missing_run,
        "invalid_cases": invalid_cases,
        "disabled_case_ids": disabled,
        "runners": {
            "total": len(inventory.get("runners", [])),
            "public_cli": runner_status.get("public_cli", []),
            "component": runner_status.get("component", []),
            "adaptation_required": adaptation_runners,
            "unmapped": unmapped_runners,
            "unclassified": sorted(set(unclassified_runners)
                                   | set(missing_runner_classification)),
        },
        "untracked_corpus_files": untracked_files,
        "dirty_tracked_corpus_files": dirty_files,
        "reasons": reasons,
        "complete": complete,
    }


def check_inventory(root, inventory):
    errors = _inventory_errors(root, inventory)
    try:
        untracked = untracked_corpus_candidates(root)
    except CorpusError as error:
        errors.append(str(error))
        untracked = []
    dirty = _tracked_worktree_changes(root)
    coverage = coverage_report(
        inventory, untracked_files=untracked, dirty_files=dirty)
    return {
        "status": "passed" if not errors else "failed",
        "errors": errors,
        "coverage": coverage,
        "source_basis": {
            "git_head": inventory.get("git_head"),
            "untracked_corpus_files": untracked,
            "dirty_tracked_corpus_files": dirty,
            "formal_freeze_ready": not errors and not untracked and not dirty,
        },
    }


def _version(command, label):
    try:
        run = subprocess.run([str(part) for part in command],
                             capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError) as error:
        raise CorpusError(f"cannot run {label}: {error}") from error
    if run.returncode != 0:
        raise CorpusError(f"{label} failed: {run.stderr.strip()}")
    return (run.stdout + run.stderr).strip()


def preflight(args):
    root = args.root.resolve(strict=True)
    reference = args.reference.resolve(strict=True)
    if not os.access(reference, os.X_OK):
        raise CorpusError(f"reference is not executable: {reference}")
    candidates = [path.resolve(strict=True) for path in args.candidate]
    if not candidates:
        raise CorpusError("at least one candidate is required")
    for candidate in candidates:
        if not os.access(candidate, os.X_OK):
            raise CorpusError(f"candidate is not executable: {candidate}")
    toolchain = args.llvm.resolve(strict=True)
    tools = {}
    for name in TOOL_NAMES:
        path = (toolchain / "bin" / name).resolve(strict=True)
        tools[name] = path
    strace = args.strace.resolve(strict=True)
    python = args.python.resolve(strict=True)
    python_version = _version([python, "--version"], "python")
    if args.require_python_version not in python_version:
        raise CorpusError(
            f"python version {python_version!r} lacks "
            f"{args.require_python_version}")
    clang_version = _version([tools["clang"], "--version"], "clang")
    if args.require_llvm_version not in clang_version:
        raise CorpusError(
            f"clang version {clang_version!r} lacks "
            f"{args.require_llvm_version}")
    strace_version = _version([strace, "--version"], "strace")
    reference_extra = []
    helper = args.reference_helper
    if helper is None:
        default = reference.parent / "native" / "lang_native_helper"
        if default.is_file():
            helper = default
    if helper is not None:
        helper = helper.resolve(strict=True)
        reference_extra.append(helper)
    environment = dict(os.environ)
    libraries = []
    if args.libxml_dir is not None:
        libraries.append(str(args.libxml_dir.resolve()))
    libraries.append(str(toolchain / "lib"))
    if environment.get("LD_LIBRARY_PATH"):
        libraries.append(environment["LD_LIBRARY_PATH"])
    environment["LD_LIBRARY_PATH"] = ":".join(libraries)
    roots = [root] + [Path(value) for value in args.repository_root]
    return {
        "root": root,
        "reference": reference,
        "candidates": candidates,
        "tools": tools,
        "strace": strace,
        "python": python,
        "reference_extra": reference_extra,
        "env": environment,
        "timeout": args.timeout,
        "roots": roots,
        "versions": {
            "python": python_version,
            "clang": clang_version,
            "strace": strace_version,
        },
        "hashes": {
            "reference": digest(reference),
            "candidates": [digest(path) for path in candidates],
            "tools": {name: digest(path) for name, path in tools.items()},
        },
        "reference_helper": None if helper is None else str(helper),
    }


def not_run_report(inventory, scope):
    selected = []
    for case in inventory.get("cases", []):
        if scope == "all":
            run = bool(case.get("enabled"))
        else:
            run = bool(case.get("enabled")) and scope in case.get("tags", [])
        if not run:
            selected.append({
                "id": case["id"],
                "reason": ("outside_scope" if case.get("enabled") else
                           case.get("reason", "inventory case is disabled")),
            })
    return selected


def run(args):
    inventory_path = args.inventory.resolve(strict=True)
    inventory = load_inventory(inventory_path)
    output = args.out.resolve()
    if output.exists() and any(output.iterdir()):
        raise CorpusError(f"output directory must be empty: {output}")
    source = args.root.resolve() / "selfhost/src"
    if output.is_relative_to(source):
        raise CorpusError("corpus output cannot be inside compiler source")
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(inventory_path, output / "inventory.used.json")
    report = {
        "schema": RESULT_SCHEMA,
        "operation_id": "SH10-corpus-runner-20261113",
        "scope": args.scope,
        "status": "running",
        "complete": False,
        "full_corpus_executed": False,
        "selfhost_claimed": False,
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "inventory": {
            "path": str(inventory_path),
            "sha256": digest(inventory_path),
        },
        "cases": [],
        "failures": [],
        "not_run": [],
    }
    result_path = output / "result.json"
    try:
        check = check_inventory(args.root.resolve(), inventory)
        report["inventory_check"] = check
        report["coverage"] = check["coverage"]
        report["source_basis"] = check["source_basis"]
        if check["errors"]:
            raise CorpusError(
                "inventory integrity failed: " + "; ".join(check["errors"][:10]))
        context = preflight(args)
        known_roots = {str(path.resolve()) for path in context["roots"]}
        for value in inventory.get("repository_roots", []):
            path = context["root"] / value
            if str(path.resolve()) not in known_roots:
                context["roots"].append(path)
                known_roots.add(str(path.resolve()))
        report["preflight"] = {
            "root": str(context["root"]),
            "reference": str(context["reference"]),
            "candidates": [str(path) for path in context["candidates"]],
            "tools": {name: str(path)
                      for name, path in context["tools"].items()},
            "strace": str(context["strace"]),
            "python": str(context["python"]),
            "versions": context["versions"],
            "hashes": context["hashes"],
            "reference_helper": context["reference_helper"],
        }
        selected = []
        for case in inventory.get("cases", []):
            if not case.get("enabled"):
                continue
            if args.scope != "all" and args.scope not in case.get("tags", []):
                continue
            selected.append(case)
        if not selected:
            raise CorpusError(f"no enabled cases for scope {args.scope}")
        report["selected_cases"] = [case["id"] for case in selected]
        report["not_run"] = not_run_report(inventory, args.scope)
        report["full_corpus_executed"] = (
            args.scope == "all" and not report["not_run"]
            and check["coverage"]["complete"])
        for case in selected:
            try:
                record = run_case(
                    case, context["reference"], context["candidates"],
                    output, context)
            except Exception as error:  # keep evidence for other cases
                record = {
                    "id": case.get("id"),
                    "operation": case.get("operation"),
                    "status": "failed",
                    "variants": [],
                    "failures": [{"side": "harness", "errors": [str(error)]}],
                }
            report["cases"].append(record)
            if record["status"] != "passed":
                report["failures"].append({
                    "case": record["id"], "errors": record["failures"]})
        if report["failures"]:
            report["status"] = "failed"
        elif args.scope == "all" and not check["coverage"]["complete"]:
            report["status"] = "incomplete"
        else:
            report["status"] = "passed"
        report["complete"] = (
            report["status"] == "passed"
            and check["coverage"]["complete"]
            and report["full_corpus_executed"])
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        report["finished_at"] = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        write_json(result_path, report)
        print(f"corpus result: {result_path}", flush=True)
    return 0 if report["status"] == "passed" else (
        2 if report["status"] == "incomplete" else 1)


def freeze(args):
    inventory = load_inventory(args.inventory.resolve(strict=True))
    report = check_inventory(args.root.resolve(), inventory)
    report["inventory"] = str(args.inventory.resolve())
    report["git_head"] = inventory.get("git_head")
    report["formal_freeze_ready"] = report["source_basis"]["formal_freeze_ready"]
    report["status"] = "passed" if not report["errors"] else "failed"
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        label = report["status"].upper()
        if report["status"] == "passed" and not report["formal_freeze_ready"]:
            label = "TRACKED-ONLY PASS"
        print(f"{label}: frozen inventory {report['inventory']}")
        for error in report["errors"]:
            print(f"  error: {error}")
        coverage = report["coverage"]
        print(f"  files: {coverage['source_files']} sources, "
              f"{coverage['manifests']} manifests, "
              f"{coverage['runner_sources']} runner sources")
        print(f"  coverage complete: {coverage['complete']}")
        print(f"  formal freeze ready: {report['formal_freeze_ready']}")
        if coverage["untracked_corpus_files"]:
            print(f"  untracked corpus files excluded from catalog: "
                  f"{len(coverage['untracked_corpus_files'])}")
        if coverage["dirty_tracked_corpus_files"]:
            print(f"  dirty tracked corpus files: "
                  f"{len(coverage['dirty_tracked_corpus_files'])}")
        for reason in coverage["reasons"]:
            print(f"  gap: {reason['code']} = {reason['count']}")
    return 0 if report["status"] == "passed" else 1


def coverage_command(args):
    inventory = load_inventory(args.inventory.resolve(strict=True))
    report = check_inventory(args.root.resolve(), inventory)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        coverage = report["coverage"]
        print(f"COVERAGE: {'complete' if coverage['complete'] else 'INCOMPLETE'}")
        print(f"  sources: {coverage['assigned_sources']}/"
              f"{coverage['source_files']} assigned; "
              f"manifests: {coverage['assigned_manifests']}/"
              f"{coverage['manifests']}")
        print(f"  cases: {coverage['cases']}")
        print(f"  formal freeze ready: "
              f"{report['source_basis']['formal_freeze_ready']}")
        for reason in coverage["reasons"]:
            print(f"  gap: {reason['code']} = {reason['count']}")
        if coverage["unassigned_sources"]:
            print("  unassigned sources: " +
                  ", ".join(coverage["unassigned_sources"][:20]))
        if coverage["missing_run_expectations"]:
            print("  missing run expectations: " +
                  ", ".join(coverage["missing_run_expectations"][:20]))
        for error in report["errors"][:20]:
            print(f"  error: {error}")
    return 0 if not report["errors"] and report["coverage"]["complete"] else 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    freeze_parser = sub.add_parser(
        "freeze", help="verify every frozen path and SHA-256")
    freeze_parser.add_argument("--inventory", type=Path,
                               default=ROOT / "selfhost/corpus.json")
    freeze_parser.add_argument("--root", type=Path, default=DEFAULT_CORPUS_ROOT)
    freeze_parser.add_argument("--json", action="store_true")

    coverage_parser = sub.add_parser(
        "coverage", help="report assigned and unadapted corpus gaps")
    coverage_parser.add_argument("--inventory", type=Path,
                                 default=ROOT / "selfhost/corpus.json")
    coverage_parser.add_argument("--root", type=Path, default=DEFAULT_CORPUS_ROOT)
    coverage_parser.add_argument("--json", action="store_true")

    run_parser = sub.add_parser(
        "run", help="trace and compare reference/candidate public CLI results")
    run_parser.add_argument("--inventory", type=Path,
                            default=ROOT / "selfhost/corpus.json")
    run_parser.add_argument("--root", type=Path, default=DEFAULT_CORPUS_ROOT)
    run_parser.add_argument("--reference", type=Path, required=True)
    run_parser.add_argument("--candidate", type=Path, action="append",
                            required=True)
    run_parser.add_argument("--out", type=Path, required=True)
    run_parser.add_argument("--scope", choices=("all", "harness-selfcheck"),
                            default="all")
    run_parser.add_argument("--python", type=Path, default=DEFAULT_PYTHON)
    run_parser.add_argument("--require-python-version", default="3.12.15")
    run_parser.add_argument("--strace", type=Path, default=DEFAULT_STRACE)
    run_parser.add_argument("--llvm", type=Path, default=DEFAULT_LLVM)
    run_parser.add_argument("--require-llvm-version", default="22.1.8")
    run_parser.add_argument("--libxml-dir", type=Path, default=DEFAULT_LIBXML)
    run_parser.add_argument("--reference-helper", type=Path)
    run_parser.add_argument("--repository-root", type=Path, action="append",
                            default=[DEFAULT_CORPUS_ROOT])
    run_parser.add_argument("--timeout", type=float, default=1200)
    args = parser.parse_args(argv)
    try:
        if args.command == "freeze":
            return freeze(args)
        if args.command == "coverage":
            return coverage_command(args)
        if args.timeout <= 0:
            raise CorpusError("timeout must be positive")
        return run(args)
    except (OSError, ValueError, CorpusError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
