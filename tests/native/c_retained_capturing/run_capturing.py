#!/usr/bin/env python3
"""Evidence runner for retained capturing C callbacks.

The original route has two roles and one complete command matrix per role:

* 13 positive projects built and executed at O0 and O2 (26 builds + 26 runs);
* one integration probe check plus seven rejection checks;
* two no-output rejection builds at O0;
* one freestanding core build, readelf inspection and execution;
* one C fixture shared-library compile and one standalone C++ lifecycle run;
* two runtime-archive nm inspections and one git identity command.

That is 70 top-level commands per role (140 for both roles).  The standalone
C++ lifecycle executable is built once for the whole paired run and is counted
separately.  This file keeps the old legacy invocation working, while adding an
explicit role, per-command tracing, persistent command/case records and strict
input/resource snapshots for the two-role evidence run.

The runner never invents product interfaces, never uses a C++ helper to analyze
candidate Source, never relinks the published ``program.o`` and never changes
the compiler, Source, gold, fixtures, headers, lifecycle test, stdlib or CMake.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone

# The evidence runner must not leave bytecode caches in the worktree or in the
# copied Source tree.
sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent

OPERATION_ID = "SH14-c-retained-capturing-public-records-20261114"
EVIDENCE_SCHEMA = "woozy/c-retained-capturing-public-records/1"
PROPOSAL_SCHEMA = "woozy/c-retained-capturing-proposal/1"
FAMILY_PREFIX = "tests/native/c_retained_capturing/"

CANONICAL_LD_LIBRARY_PATH = (
    "/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/lib:"
    "/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9/usr/lib"
)

SOURCE_PROJECTS = (
    "probe",
    "move_capture",
    "captures_escape",
    "userdata_positions",
    "returns_and_units",
    "sync_register",
    "two_resource_borrow",
    "c_thread",
    "failure_cleanup",
    "close_modes",
    "typed_errors",
    "gc_cleanup",
    "cleanup_observation",
)

# The five integration negatives are skipped only by the legacy auto/skip path
# when the public CLI has not integrated the registration surface.  The two
# static-only rows deliberately stay runnable in that legacy path.
INTEGRATION_NEGATIVES = {
    "throws": ("native", "E_CALL_ARGUMENT"),
    "signature": ("native", "E_CALL_ARGUMENT"),
    "mode": ("native", "E_CAPABILITY_UNAVAILABLE"),
    "bad_metadata": ("native", "E_BINDING_INVALID"),
    "python_unsupported": ("python", "E_CAPABILITY_UNAVAILABLE"),
}
STATIC_NEGATIVES = {
    "static_capture": ("native", "E_CAPABILITY_UNAVAILABLE"),
    "python_static_unsupported": ("python", "E_CAPABILITY_UNAVAILABLE"),
}
ALL_NEGATIVES = {**INTEGRATION_NEGATIVES, **STATIC_NEGATIVES}
NO_OUTPUT_BUILDS = {
    "bad_metadata": "E_BINDING_INVALID",
    "static_capture": "E_CAPABILITY_UNAVAILABLE",
}

HOSTED_SYMBOLS = (
    "lang_c_registration_new_v1",
    "lang_c_registration_finish_v1",
    "lang_c_registration_close_v1",
    "lang_c_registration_release_v1",
    "lang_c_registration_enter_v1",
    "lang_c_registration_leave_v1",
    "lang_c_registration_borrow_v1",
    "lang_c_registration_end_borrow_v1",
)

# Fixed original budgets.  --timeout only controls the generated program run.
BUILD_TIMEOUT = 240
CHECK_TIMEOUT = 120
PROGRAM_TIMEOUT = 30
FIXTURE_COMPILE_TIMEOUT = 120
LIFECYCLE_TIMEOUT = 60
NM_TIMEOUT = 60
READELF_TIMEOUT = 30
GIT_TIMEOUT = 30

EXPECTED_COUNTS = {
    "public_builds": 29,
    "public_checks": 8,
    "woozy_programs": 27,
    "fixture_compile": 1,
    "lifecycle": 1,
    "nm": 2,
    "readelf": 1,
    "git": 1,
    "total": 70,
}

LLVM_TOOL_NAMES = ("clang", "clang-22", "clang++", "opt", "llc", "ld.lld", "lld")


class EvidenceError(RuntimeError):
    """A strict evidence or process-record error."""


# ---------------------------------------------------------------------------
# Pure helpers (also exercised by test_controls.py).


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def maybe_digest(path):
    if path is None:
        return None
    path = Path(path)
    if not path.is_file():
        return None
    return digest(path)


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def jsonable(value):
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, dict):
        return {str(key): jsonable(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [jsonable(item) for item in value]
    return value


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(
        jsonable(value), indent=2, ensure_ascii=False, allow_nan=False
    )
    path.write_text(text + "\n", encoding="utf-8")


def reject_json_constant(value):
    raise ValueError(f"non-standard JSON constant {value!r}")


def reject_nonfinite(value, location="$"):
    if isinstance(value, float) and not math.isfinite(value):
        raise ValueError(f"non-finite JSON number at {location}: {value!r}")
    if isinstance(value, dict):
        for key, item in value.items():
            reject_nonfinite(item, f"{location}.{key}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            reject_nonfinite(item, f"{location}[{index}]")
    return value


def parse_json_bytes(data, label):
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise EvidenceError(f"{label} stdout is not UTF-8: {error}") from error
    if not text.strip():
        raise EvidenceError(f"{label} wrote no JSON document")
    try:
        document = json.loads(text, parse_constant=reject_json_constant)
    except (json.JSONDecodeError, ValueError) as error:
        raise EvidenceError(f"{label} wrote malformed JSON: {error}") from error
    reject_nonfinite(document)
    if not isinstance(document, dict):
        raise EvidenceError(f"{label} JSON document is not an object")
    return document


def command_status(exit_code, timed_out, spawn_error=None):
    if spawn_error is not None:
        return "spawn_error"
    if timed_out:
        return "timed_out"
    if type(exit_code) is not int:
        return "unknown_exit"
    if exit_code < 0:
        return "signalled"
    return "exited"


def process_problems(record, label, expected_exit=None):
    """Return strict process-record problems without raising."""
    problems = []
    status = record.get("status")
    if status != "exited":
        problems.append(f"{label} status is {status!r}, not 'exited'")
    exit_code = record.get("exit")
    if type(exit_code) is not int:
        problems.append(
            f"{label} exit is not an int (missing or wrong type): {exit_code!r}"
        )
    else:
        if exit_code < 0:
            problems.append(f"{label} terminated by signal: exit {exit_code}")
        if expected_exit is not None and exit_code != expected_exit:
            problems.append(
                f"{label} exit is {exit_code}, expected {expected_exit}"
            )
    if record.get("timed_out") is not False:
        problems.append(
            f"{label} timed_out is not exactly False: "
            f"{record.get('timed_out')!r}"
        )
    if record.get("cleanup_complete") is not True:
        problems.append(
            f"{label} cleanup_complete is not exactly True: "
            f"{record.get('cleanup_complete')!r}"
        )
    if record.get("abandoned_pipes") is not False:
        problems.append(
            f"{label} abandoned_pipes is not exactly False: "
            f"{record.get('abandoned_pipes')!r}"
        )
    if record.get("spawn_error") is not None:
        problems.append(f"{label} spawn error: {record.get('spawn_error')}")
    return problems


def document_problems(document, label, expected_status, expected_code=None):
    problems = []
    status = document.get("status")
    if not isinstance(status, str):
        problems.append(f"{label} JSON status is not a string: {status!r}")
    elif status != expected_status:
        problems.append(
            f"{label} JSON status is {status!r}, expected {expected_status!r}"
        )
    diagnostics = document.get("diagnostics")
    if not isinstance(diagnostics, list):
        problems.append(
            f"{label} JSON diagnostics is not a list: {diagnostics!r}"
        )
        diagnostics = []
    codes = []
    for entry in diagnostics:
        if not isinstance(entry, dict):
            problems.append(f"{label} diagnostic is not an object: {entry!r}")
            continue
        code = entry.get("code")
        if not isinstance(code, str):
            problems.append(f"{label} diagnostic code is not a string: {code!r}")
            continue
        codes.append(code)
        severity = entry.get("severity")
        if severity is not None and not isinstance(severity, str):
            problems.append(f"{label} diagnostic severity is not a string")
    if expected_status == "ok":
        if any(
            isinstance(entry, dict) and entry.get("severity") == "error"
            for entry in diagnostics
        ):
            problems.append(f"{label} status ok contains an error diagnostic")
    if expected_status == "error":
        if not any(
            isinstance(entry, dict) and entry.get("severity", "error") == "error"
            for entry in diagnostics
        ):
            problems.append(f"{label} status error contains no error diagnostic")
        if expected_code is not None and expected_code not in codes:
            problems.append(
                f"{label} diagnostics {codes!r} lack {expected_code!r}"
            )
    return problems, codes


def audit_trace_paths(observed, allowed, label, forbid_helper=False):
    observed = [str(Path(path)) for path in observed]
    allowed = {str(Path(path)) for path in allowed}
    unexpected = sorted(set(observed) - allowed)
    if unexpected:
        raise EvidenceError(
            f"{label} trace ran outside the exact allowed set: {unexpected}"
        )
    if forbid_helper:
        forbidden = [
            path
            for path in observed
            if "helper" in Path(path).name.lower()
            or "frontend" in Path(path).name.lower()
        ]
        if forbidden:
            raise EvidenceError(
                f"{label} candidate Source used a C++ helper/frontend: "
                f"{forbidden}"
            )
    return {
        "status": "ok",
        "observed": sorted(set(observed)),
        "allowed": sorted(allowed),
        "policy": label,
    }


def negative_result_ok(record, document, expected_code, program_absent):
    """A rejection is real only for exit 1 + error JSON + exact code."""
    problems = process_problems(record, "negative", expected_exit=1)
    document_issues, codes = document_problems(
        document or {}, "negative", "error", expected_code
    )
    problems.extend(document_issues)
    if not program_absent:
        problems.append("rejection published program/program.py")
    return (not problems), problems, codes


def no_output_result_ok(record, document, expected_code, program_absent):
    problems = process_problems(record, "no-output build", expected_exit=1)
    document_issues, codes = document_problems(
        document or {}, "no-output build", "error", expected_code
    )
    problems.extend(document_issues)
    if not program_absent:
        problems.append("no-output build published program/program.py")
    return (not problems), problems, codes


LOADER_INIT_RE = re.compile(
    r"^\s*(?:\d+:\s*)?calling init:\s*(?P<path>.+?)\s*$"
)


def loader_line_matches(stderr_text, expected_so):
    """Return the exact loader path, not a substring hit."""
    expected = Path(os.path.normpath(str(expected_so)))
    for line in stderr_text.splitlines():
        match = LOADER_INIT_RE.match(line)
        if match is None:
            continue
        loaded = Path(os.path.normpath(match.group("path")))
        if loaded.is_absolute() and loaded == expected:
            return True, line.strip(), str(loaded)
    return False, None, None


def proposal_rows(corpus_cases):
    rows = []
    for case in corpus_cases:
        manifest = case.get("manifest", "")
        if not manifest.startswith(FAMILY_PREFIX):
            continue
        row = {
            "id": case.get("id"),
            "kind": case.get("kind"),
            "manifest": manifest,
            "runner": "tests/native/c_retained_capturing/run_capturing.py",
            "support": case.get("support"),
            "adaptation": case.get("adaptation"),
            "runner_status": case.get("runner_status"),
            "enabled": False,
            "backend": case.get("backend"),
            "operation": case.get("operation"),
            "operations": case.get("operations"),
            "expected": case.get("expected"),
            "tags": case.get("tags", []),
        }
        if manifest in (
            FAMILY_PREFIX + "negative/python_unsupported/lang.toml",
            FAMILY_PREFIX + "negative/python_static_unsupported/lang.toml",
        ):
            row["backend"] = "python"
            row["operation"] = "check"
            row["operations"] = ["check"]
            row["correction"] = (
                "The original runner only executes check --backend python; "
                "it never builds or executes a Python program for this row. "
                "The catalog's build_native/native placeholder is replaced."
            )
        rows.append(row)
    rows.sort(key=lambda item: item["manifest"])
    return rows


def tree_snapshot(path, exclude_prefixes=()):
    path = Path(path)
    if not path.is_dir():
        raise EvidenceError(f"snapshot root is missing: {path}")
    files = {}
    links = {}
    for child in sorted(path.rglob("*")):
        relative = child.relative_to(path).as_posix()
        if any(
            relative == prefix
            or relative.startswith(prefix.rstrip("/") + "/")
            for prefix in exclude_prefixes
        ):
            continue
        if child.is_symlink():
            links[relative] = os.readlink(child)
        elif child.is_file():
            files[relative] = digest(child)
    if not files and not links:
        raise EvidenceError(f"snapshot root contains no files: {path}")
    payload = {"files": files, "links": links}
    return {
        "root": str(path),
        "files": len(files),
        "links": len(links),
        "map_sha256": digest_bytes(
            json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
        ),
        "map": payload,
    }


def file_info(path):
    path = Path(path)
    if not path.exists():
        return {
            "path": str(path),
            "resolved": None,
            "exists": False,
            "sha256": None,
            "size": None,
        }
    return {
        "path": str(path),
        "resolved": str(path.resolve()),
        "exists": True,
        "sha256": digest(path),
        "size": path.stat().st_size,
    }


def safe_label(label):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", label).strip("._") or "case"


def text_of(data):
    return data.decode("utf-8", "surrogateescape")


# ---------------------------------------------------------------------------
# Runner state.


class Runner:
    def __init__(self, args):
        self.args = args
        self.root = Path(args.root).resolve()
        self.out = Path(args.out).resolve()
        self.cli = Path(args.cli).resolve()
        self.role = args.role
        self.opts = tuple(args.opts or (0, 2))
        self.source_tests = args.source_tests
        self.failures = []
        self.not_run = []
        self.commands = []
        self.cases = {}
        self.corpus = None
        self.strace = Path(args.strace).resolve() if args.strace else None
        self.clang = Path(args.clang).resolve()
        self.toolchain = (
            Path(args.toolchain).resolve()
            if args.toolchain
            else self.clang.parent
        )
        self.helper = Path(args.helper).resolve() if args.helper else None
        self.stdlib = Path(args.stdlib).resolve() if args.stdlib else None
        self.native_runtime = (
            Path(args.native_runtime).resolve() if args.native_runtime else None
        )
        self.hosted_runtime = (
            Path(args.hosted_runtime).resolve() if args.hosted_runtime else None
        )
        self.fixture_test = (
            Path(args.fixture_test).resolve() if args.fixture_test else None
        )
        self.nm = Path(args.nm).resolve() if args.nm else None
        self.readelf = (
            Path(args.readelf).resolve()
            if args.readelf
            else Path(shutil.which("readelf") or "/usr/bin/readelf")
        )
        self.git = (
            Path(args.git).resolve()
            if args.git
            else Path(shutil.which("git") or "/usr/bin/git")
        )
        self.ld = (
            Path(args.ld).resolve()
            if args.ld
            else Path("/usr/bin/ld")
        )
        self.env = os.environ.copy()
        self.env["PYTHONDONTWRITEBYTECODE"] = "1"
        self.source_allowed = set()
        self.fixture_allowed = set()
        self.commands_path = self.out / "commands.jsonl"
        self.tests = self.out / "source"
        self.fixture_dir = self.out / "fixture"
        self.fixture_so = self.fixture_dir / "libc_capturing.so"
        self.inputs_before = None
        self.snapshots_before = {}
        self.snapshots_after = {}
        self.head = None
        self.lifecycle_record = None
        self.archives_record = None
        self.core_record = None
        self.generated_initial = {}
        self.generated_final = {}
        self.probe_record = None
        self.source_available = False
        self.prepared = False
        self.inputs_unchanged = False

    # -- failure helpers ---------------------------------------------------

    def fail(self, label, detail=""):
        entry = label + (": " + detail if detail else "")
        self.failures.append(entry)
        print("FAIL " + entry, file=sys.stderr, flush=True)
        return False

    def not_run_case(self, label, reason):
        self.not_run.append({"name": label, "reason": reason})

    # -- preparation -------------------------------------------------------

    def prepare(self):
        if self.out.exists():
            if any(self.out.iterdir()):
                print(
                    f"refusing non-empty evidence directory: {self.out}",
                    file=sys.stderr,
                )
                return False
        else:
            self.out.mkdir(parents=True)
        if not self.cli.is_file():
            print(f"missing CLI: {self.cli}", file=sys.stderr)
            return False
        if not os.access(self.cli, os.X_OK):
            print(f"CLI is not executable: {self.cli}", file=sys.stderr)
            return False
        corpus_path = self.root / "selfhost/corpus.py"
        if not corpus_path.is_file():
            print(f"missing shared corpus runner: {corpus_path}", file=sys.stderr)
            return False
        spec = importlib.util.spec_from_file_location(
            "capturing_shared_corpus", corpus_path
        )
        self.corpus = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(self.corpus)

        if self.opts not in ((0,), (2,), (0, 2), (2, 0)):
            self.fail(
                "options",
                f"fixed O0/O2 only, got {self.opts!r}",
            )
            self.opts = tuple(option for option in self.opts if option in (0, 2))
            if not self.opts:
                return False

        if self.source_tests == "require" and self.strace is None:
            if self.args.require_trace:
                self.fail(
                    "trace:required",
                    "--source-tests require with --require-trace needs --strace",
                )
        if self.strace is not None and not self.strace.is_file():
            self.fail("trace:missing", f"--strace does not exist: {self.strace}")
            self.strace = None

        if self.source_tests == "require" and self.fixture_test is None:
            self.fail(
                "fixture:lifecycle",
                "--fixture-test is required; the C lifecycle is not auto-skipped",
            )
        if self.fixture_test is not None:
            if self.fixture_test.is_file() and os.access(
                self.fixture_test, os.X_OK
            ):
                self.generated_initial["lifecycle-binary"] = {
                    "path": str(self.fixture_test),
                    "initial": file_info(self.fixture_test),
                }
            if not self.fixture_test.is_file() or not os.access(
                self.fixture_test, os.X_OK
            ):
                self.fail(
                    "fixture:lifecycle",
                    f"--fixture-test is not an executable file: {self.fixture_test}",
                )
                self.fixture_test = None
            elif not self.fixture_test.read_bytes()[:4] == b"\x7fELF":
                self.fail(
                    "fixture:lifecycle",
                    f"--fixture-test is not an ELF executable: {self.fixture_test}",
                )
                self.fixture_test = None

        if self.role == "candidate":
            if self.helper is not None:
                self.fail(
                    "role:candidate",
                    "candidate Source must not be given a C++ helper",
                )
                self.helper = None
        else:
            if self.helper is None:
                self.helper = self.detect_embedded_path("lang_native_helper")
            if self.helper is None or not self.helper.is_file():
                self.fail(
                    "role:reference",
                    "reference role needs its matching lang_native_helper",
                )
            else:
                self.helper = self.helper.resolve()

        if self.stdlib is None:
            self.stdlib = self.detect_stdlib()
        if self.stdlib is None or not self.stdlib.is_dir():
            self.fail("role:stdlib", "cannot identify the CLI's actual stdlib")
            self.stdlib = None

        if self.native_runtime is None:
            self.native_runtime = self.detect_runtime("liblang_native_runtime.a")
        if self.native_runtime is None or not self.native_runtime.is_file():
            self.fail("role:native-runtime", "missing native runtime archive")
            self.native_runtime = None
        else:
            self.native_runtime = self.native_runtime.resolve()

        if self.hosted_runtime is None:
            self.hosted_runtime = self.detect_runtime("liblang_hosted_runtime.a")
        if self.hosted_runtime is None or not self.hosted_runtime.is_file():
            self.fail("role:hosted-runtime", "missing hosted runtime archive")
            self.hosted_runtime = None
        else:
            self.hosted_runtime = self.hosted_runtime.resolve()

        # Verify that the archives reported by the caller are the ones the
        # role's compiler actually resolves.  This is a read-only check.
        if self.role == "candidate" and self.cli.parent.joinpath(
            "runtime"
        ).is_dir():
            for label, supplied, name in (
                ("native", self.native_runtime, "liblang_native_runtime.a"),
                ("hosted", self.hosted_runtime, "liblang_hosted_runtime.a"),
            ):
                expected = self.cli.parent / "runtime" / name
                if supplied is not None and expected.is_file():
                    if digest(supplied) != digest(expected):
                        self.fail(
                            "role:" + label + "-runtime",
                            "supplied archive differs from the CLI's "
                            f"actual runtime: {expected}",
                        )
        if self.role == "reference" and self.helper is not None:
            helper_bytes = self.helper.read_bytes()
            for label, supplied, name in (
                ("native", self.native_runtime, "liblang_native_runtime.a"),
                ("hosted", self.hosted_runtime, "liblang_hosted_runtime.a"),
            ):
                if supplied is None:
                    continue
                if str(supplied).encode() not in helper_bytes:
                    self.fail(
                        "role:" + label + "-runtime",
                        "supplied archive path is not embedded in the "
                        f"matching helper: {supplied}",
                    )

        for name in ("clang", "opt", "llc", "ld.lld"):
            path = self.toolchain / name
            if not path.is_file():
                self.fail("toolchain:missing", f"missing fixed LLVM tool: {path}")
        if not self.ld.is_file():
            self.fail("toolchain:missing", f"missing system linker: {self.ld}")
        if self.nm is None:
            candidate_nm = self.clang.with_name("llvm-nm")
            self.nm = (
                candidate_nm.resolve()
                if candidate_nm.is_file()
                else Path(shutil.which("nm") or "/usr/bin/nm").resolve()
            )
        if not self.nm.is_file():
            self.fail("toolchain:missing", f"missing nm: {self.nm}")
        if not self.readelf.is_file():
            self.fail("toolchain:missing", f"missing readelf: {self.readelf}")
        if not self.git.is_file():
            self.fail("toolchain:missing", f"missing git: {self.git}")

        tool_paths = {str(self.cli.resolve())}
        for name in LLVM_TOOL_NAMES:
            path = self.toolchain / name
            if path.exists():
                tool_paths.add(str(path.resolve()))
        self.source_allowed = set(tool_paths)
        if self.role == "reference":
            if self.helper is not None:
                self.source_allowed.add(str(self.helper.resolve()))
            self.source_allowed.add(str(self.ld.resolve()))
        self.fixture_allowed = {
            str(self.clang.resolve()),
            str(self.ld.resolve()),
        }

        # The role-private fixture is the first library path.  The fixed
        # LLVM/libxml directory follows it so the compiler's tools resolve.
        fixture_env = str(self.fixture_dir)
        existing = self.env.get("LD_LIBRARY_PATH")
        library_path = [fixture_env, CANONICAL_LD_LIBRARY_PATH]
        if existing:
            library_path.append(existing)
        self.env["LD_LIBRARY_PATH"] = ":".join(library_path)

        self.out.mkdir(parents=True, exist_ok=True)
        if self.tests.exists():
            self.fail("prepare", f"working source copy already exists: {self.tests}")
            return False
        shutil.copytree(
            self.root / "tests/native/c_retained_capturing",
            self.tests,
            ignore=shutil.ignore_patterns(".build", "__pycache__"),
        )
        if self.fixture_dir.exists():
            self.fail("prepare", f"fixture directory already exists: {self.fixture_dir}")
            return False
        self.fixture_dir.mkdir(parents=True)

        self.commands_path.write_text("", encoding="utf-8")
        self.prepared = True
        self.snapshot_inputs("before")
        return True

    def detect_embedded_path(self, basename):
        try:
            data = self.cli.read_bytes()
        except OSError:
            return None
        pattern = re.compile(
            rb"(/home/[^\x00\s\"']+/" + re.escape(basename.encode()) + rb")"
        )
        found = []
        for match in pattern.finditer(data):
            path = Path(match.group(1).decode("utf-8", "surrogateescape"))
            if path.is_file():
                found.append(path)
        if not found:
            return None
        return sorted(set(found), key=lambda item: len(str(item)), reverse=True)[0]

    def detect_stdlib(self):
        if self.role == "candidate":
            candidate = self.cli.parent / "stdlib"
            if candidate.is_dir():
                return candidate.resolve()
        try:
            data = self.cli.read_bytes()
        except OSError:
            return None
        pattern = re.compile(rb"(/home/[^\x00\s\"']+/stdlib)")
        found = []
        for match in pattern.finditer(data):
            path = Path(match.group(1).decode("utf-8", "surrogateescape"))
            if path.is_dir():
                found.append(path)
        if not found:
            return None
        return sorted(set(found), key=lambda item: len(str(item)), reverse=True)[0]

    def detect_runtime(self, basename):
        if self.role == "candidate":
            candidate = self.cli.parent / "runtime" / basename
            if candidate.is_file():
                return candidate.resolve()
        source = self.helper if self.helper is not None else self.cli
        try:
            data = source.read_bytes()
        except OSError:
            return None
        pattern = re.compile(
            rb"(/home/[^\x00\s\"']+/" + re.escape(basename.encode()) + rb")"
        )
        found = []
        for match in pattern.finditer(data):
            path = Path(match.group(1).decode("utf-8", "surrogateescape"))
            if path.is_file():
                found.append(path)
        if not found:
            return None
        return sorted(set(found), key=lambda item: len(str(item)), reverse=True)[0]

    # -- snapshots ---------------------------------------------------------

    def input_files(self):
        entries = {
            "cli": self.cli,
            "clang": self.clang,
            "opt": self.toolchain / "opt",
            "llc": self.toolchain / "llc",
            "ld.lld": self.toolchain / "ld.lld",
            "lld": self.toolchain / "lld",
            "ld": self.ld,
            "nm": self.nm,
            "readelf": self.readelf,
            "git": self.git,
            "python": Path(sys.executable),
            "shared-corpus": self.root / "selfhost/corpus.py",
            "run-isolated": self.root / "tests/run_isolated.py",
            "runner": HERE / "run_capturing.py",
            "runner-readme": HERE / "README.md",
            "runner-controls": HERE / "test_controls.py",
            "lifecycle-cpp": HERE / "fixture_lifecycle_test.cpp",
            "fixture-c": HERE / "fixture/capturing_fixture.c",
            "fixture-header": HERE / "fixture/capturing_fixture.h",
            "cmake": HERE / "CMakeLists.txt",
            "libxml2": Path(
                "/home/qin/Work/language-dev-20261103/toolchains/"
                "libxml2-legacy-2.13.9/usr/lib/libxml2.so.2.13.9"
            ),
        }
        if self.helper is not None:
            entries["reference-helper"] = self.helper
        if self.strace is not None:
            entries["strace"] = self.strace
        if self.native_runtime is not None:
            entries["native-runtime"] = self.native_runtime
        if self.hosted_runtime is not None:
            entries["hosted-runtime"] = self.hosted_runtime
        if self.fixture_test is not None:
            entries["fixture-test"] = self.fixture_test
        for label, path in self.args.extra_input_pairs:
            entries[label] = Path(path)
        return entries

    def snapshot_roots(self):
        roots = {
            "tests-working": (self.tests, (".build",)),
            "tests-source": (
                self.root / "tests/native/c_retained_capturing",
                (),
            ),
        }
        if self.stdlib is not None:
            roots["stdlib"] = (self.stdlib, ())
        if self.role == "candidate":
            resources = self.cli.parent.parent / "resources"
            if resources.is_dir():
                roots["candidate-resources"] = (resources, ())
            source = self.cli.parent.parent / "source"
            if source.is_dir():
                roots["candidate-source"] = (source, ())
        for label, path in self.args.snapshot_pairs:
            roots[label] = (Path(path), ())
        return roots

    def snapshot_inputs(self, phase):
        files = {}
        for label, path in self.input_files().items():
            if Path(path).is_dir():
                if phase == "before":
                    self.fail(
                        "input:" + label,
                        f"use --snapshot-root for directory input: {path}",
                    )
                continue
            info = file_info(path)
            if not info["exists"] and phase == "before":
                self.fail("input:" + label, f"missing before the first command: {path}")
            files[label] = info
        snapshots = {}
        for label, (path, excludes) in self.snapshot_roots().items():
            try:
                snapshot = tree_snapshot(path, excludes)
            except EvidenceError as error:
                if phase == "before":
                    self.fail("snapshot:" + label, str(error))
                snapshots[label] = {
                    "root": str(path),
                    "files": 0,
                    "links": 0,
                    "map_sha256": None,
                    "map": {"files": {}, "links": {}},
                    "error": str(error),
                }
                continue
            snapshot_path = self.out / "snapshots" / f"{safe_label(label)}.{phase}.json"
            write_json(snapshot_path, snapshot)
            snapshots[label] = {
                "root": snapshot["root"],
                "files": snapshot["files"],
                "links": snapshot["links"],
                "map_sha256": snapshot["map_sha256"],
                "path": str(snapshot_path),
                "sha256": digest(snapshot_path),
            }
        record = {
            "captured_at": datetime.now(timezone.utc).isoformat(),
            "role": self.role,
            "files": files,
            "snapshots": snapshots,
            "environment": {
                "LD_LIBRARY_PATH": self.env.get("LD_LIBRARY_PATH"),
                "PATH": self.env.get("PATH"),
                "PYTHONDONTWRITEBYTECODE": self.env.get(
                    "PYTHONDONTWRITEBYTECODE"
                ),
            },
        }
        if phase == "before":
            self.inputs_before = record
            self.snapshots_before = snapshots
        else:
            self.snapshots_after = snapshots
        write_json(self.out / f"inputs.{phase}.json", record)
        return record

    def compare_snapshots(self):
        after = self.snapshot_inputs("after")
        unchanged = True
        for label, before in self.inputs_before["files"].items():
            current = after["files"].get(label)
            if current != before:
                unchanged = False
                self.fail(
                    "input:" + label,
                    f"changed during the run: before={before!r} after={current!r}",
                )
        for label, before in self.snapshots_before.items():
            current = after["snapshots"].get(label)
            if current is None:
                unchanged = False
                self.fail("snapshot:" + label, "missing after snapshot")
                continue
            if current.get("map_sha256") != before.get("map_sha256"):
                unchanged = False
                self.fail(
                    "snapshot:" + label,
                    "tree changed during the run: "
                    f"{before.get('map_sha256')} -> {current.get('map_sha256')}",
                )
        return unchanged

    # -- commands ----------------------------------------------------------

    def run_command(
        self,
        phase,
        label,
        argv,
        timeout,
        allowed,
        policy,
        forbid_helper=False,
        extra_env=None,
    ):
        argv = [str(part) for part in argv]
        index = len(self.commands)
        command_dir = self.out / "commands" / f"{index:03d}-{phase}"
        if command_dir.exists():
            raise EvidenceError(f"command evidence already exists: {command_dir}")
        command_dir.mkdir(parents=True)
        trace_path = command_dir / "exec.log"
        wrapped = list(argv)
        if self.strace is not None:
            wrapped = [
                str(self.strace),
                "--seccomp-bpf",
                "-f",
                "-qq",
                "-s",
                "16384",
                "-e",
                "trace=execve,execveat",
                "-o",
                str(trace_path),
                *wrapped,
            ]
        env = self.env.copy()
        if extra_env:
            env.update(extra_env)
        started = time.monotonic()
        spawn_error = None
        try:
            result = self.corpus._run_process_impl(
                wrapped,
                self.root,
                env,
                timeout,
                stdin=os.devnull,
            )
        except OSError as error:
            result = None
            spawn_error = repr(error)
        except Exception as error:  # noqa: BLE001 - evidence boundary
            result = None
            spawn_error = "unexpected process error: " + repr(error)
        elapsed = round(time.monotonic() - started, 6)
        if result is None:
            exit_code = None
            timed_out = None
            cleanup_complete = None
            abandoned_pipes = None
            stdout = b""
            stderr = b""
        else:
            exit_code = result["exit"]
            timed_out = result["timed_out"]
            cleanup_complete = result["cleanup_complete"]
            abandoned_pipes = result["abandoned_pipes"]
            stdout = result["stdout"]
            stderr = result["stderr"]
        (command_dir / "stdout.bin").write_bytes(stdout)
        (command_dir / "stderr.bin").write_bytes(stderr)
        status = command_status(exit_code, timed_out, spawn_error)
        trace = {"status": "not_requested" if self.strace is None else "unknown"}
        if self.strace is not None:
            try:
                observed = self.corpus.parse_trace(trace_path)
                trace = audit_trace_paths(
                    observed,
                    allowed,
                    policy,
                    forbid_helper=forbid_helper,
                )
                trace["trace_path"] = str(trace_path)
            except Exception as error:  # noqa: BLE001 - trace failure is evidence
                trace = {
                    "status": "violation",
                    "error": str(error),
                    "trace_path": str(trace_path),
                    "allowed": sorted(str(path) for path in allowed),
                }
                self.fail(label, "trace violation: " + str(error))
        record = {
            "index": index,
            "phase": phase,
            "label": label,
            "argv": argv,
            "wrapped_argv": wrapped,
            "cwd": str(self.root),
            "stdin": os.devnull,
            "exit": exit_code,
            "status": status,
            "signal": -exit_code
            if type(exit_code) is int and exit_code < 0
            else None,
            "timed_out": timed_out,
            "cleanup_complete": cleanup_complete,
            "abandoned_pipes": abandoned_pipes,
            "spawn_error": spawn_error,
            "timeout_s": timeout,
            "elapsed_s": elapsed,
            "stdout_path": str(command_dir / "stdout.bin"),
            "stderr_path": str(command_dir / "stderr.bin"),
            "stdout_sha256": digest_bytes(stdout),
            "stderr_sha256": digest_bytes(stderr),
            "stdout_size": len(stdout),
            "stderr_size": len(stderr),
            "trace_path": str(trace_path) if self.strace is not None else None,
            "trace": trace,
        }
        write_json(command_dir / "command.json", record)
        with self.commands_path.open("a", encoding="utf-8") as stream:
            stream.write(
                json.dumps(jsonable(record), ensure_ascii=False, allow_nan=False)
                + "\n"
            )
        self.commands.append(record)
        return record

    def clean(self, record, label, expected_exit=None):
        problems = process_problems(record, label, expected_exit)
        if problems:
            self.fail(label, "; ".join(problems))
            return False
        return True

    def document(self, record, label, expected_status, expected_code=None):
        try:
            document = parse_json_bytes(record_stdout(record), label)
        except EvidenceError as error:
            self.fail(label, str(error))
            return None, []
        problems, codes = document_problems(
            document, label, expected_status, expected_code
        )
        if problems:
            self.fail(label, "; ".join(problems))
        return document, codes

    def generated_map(self, path):
        path = Path(path)
        if not path.is_dir():
            return {"exists": False, "files": {}, "map_sha256": None}
        try:
            snapshot = tree_snapshot(path)
        except EvidenceError:
            return {"exists": True, "files": {}, "map_sha256": None}
        return {
            "exists": True,
            "files": snapshot["map"]["files"],
            "links": snapshot["map"]["links"],
            "map_sha256": snapshot["map_sha256"],
        }

    def remember_generated(self, label, path, initial):
        self.generated_initial[label] = {
            "path": str(path),
            "initial": initial,
        }

    # -- fixed route commands ---------------------------------------------

    def git_identity(self):
        record = self.run_command(
            "git",
            "git HEAD",
            [self.git, "-C", self.root, "rev-parse", "HEAD"],
            GIT_TIMEOUT,
            {str(self.git.resolve())},
            "git identity: fixed git executable only",
        )
        if self.clean(record, "git HEAD", 0):
            self.head = text_of(record_stdout(record)).strip()
        else:
            self.head = None
        write_json(
            self.out / "git.json",
            {"head": self.head, "command": record["index"]},
        )

    def fixture_compile(self):
        source = self.tests / "fixture/capturing_fixture.c"
        argv = [
            self.clang,
            "--target=x86_64-unknown-linux-gnu",
            "-std=c11",
            "-O2",
            "-fPIC",
            "-pthread",
            "-Wall",
            "-Wextra",
            "-shared",
            "-Wl,-soname,libc_capturing.so",
            source,
            "-o",
            self.fixture_so,
        ]
        record = self.run_command(
            "fixture-compile",
            "fixture:compile",
            argv,
            FIXTURE_COMPILE_TIMEOUT,
            self.fixture_allowed,
            "C fixture compile: fixed clang + fixed system linker only",
        )
        passed = self.clean(record, "fixture:compile", 0)
        if passed and not self.fixture_so.is_file():
            passed = self.fail("fixture:compile", "missing libc_capturing.so")
        link_dir = self.tests / ".build"
        link_dir.mkdir(parents=True, exist_ok=True)
        if passed:
            shutil.copy2(self.fixture_so, link_dir / self.fixture_so.name)
        self.remember_generated(
            "fixture-so",
            self.fixture_dir,
            self.generated_map(self.fixture_dir),
        )
        self.generated_initial["fixture-so-file"] = {
            "path": str(self.fixture_so),
            "initial": file_info(self.fixture_so),
        }
        self.generated_initial["fixture-build-copy"] = {
            "path": str(self.tests / ".build/libc_capturing.so"),
            "initial": file_info(self.tests / ".build/libc_capturing.so"),
        }
        write_json(
            self.out / "fixture.json",
            {
                "passed": passed,
                "command": record["index"],
                "source": str(source),
                "source_sha256": digest(source),
                "shared_library": str(self.fixture_so),
                "shared_library_sha256": digest(self.fixture_so)
                if self.fixture_so.is_file()
                else None,
                "link_copy": str(self.tests / ".build/libc_capturing.so"),
                "link_copy_sha256": digest(self.tests / ".build/libc_capturing.so")
                if (self.tests / ".build/libc_capturing.so").is_file()
                else None,
                "build_dir": str(self.out / "builds"),
            },
        )
        return passed

    def lifecycle(self):
        if self.fixture_test is None:
            self.lifecycle_record = {
                "status": "missing",
                "reason": "--fixture-test was not supplied",
            }
            write_json(self.out / "lifecycle.json", self.lifecycle_record)
            return
        if not self.fixture_so.is_file():
            self.lifecycle_record = {
                "status": "failed",
                "reason": "role-private C fixture was not built",
            }
            self.fail("fixture:lifecycle", self.lifecycle_record["reason"])
            write_json(self.out / "lifecycle.json", self.lifecycle_record)
            return
        expected_so = self.fixture_so.resolve()
        record = self.run_command(
            "lifecycle",
            "fixture:lifecycle",
            [self.fixture_test],
            LIFECYCLE_TIMEOUT,
            {str(self.fixture_test.resolve())},
            "standalone C lifecycle: original C++ test ELF only",
            extra_env={"LD_DEBUG": "libs"},
        )
        passed = self.clean(record, "fixture:lifecycle", 0)
        stderr_text = text_of(
            (self.out / "commands" / f"{record['index']:03d}-lifecycle"
             / "stderr.bin").read_bytes()
        )
        loaded, line, loaded_path_text = loader_line_matches(
            stderr_text, expected_so
        )
        if not loaded:
            passed = self.fail(
                "fixture:lifecycle",
                "loader did not report the role-private fixture: "
                f"{expected_so}",
            )
        loaded_path = Path(loaded_path_text) if loaded else None
        self.lifecycle_record = {
            "status": "passed" if passed else "failed",
            "binary": str(self.fixture_test),
            "binary_sha256": digest(self.fixture_test),
            "expected_shared_library": str(expected_so),
            "expected_shared_library_sha256": digest(self.fixture_so),
            "loaded_shared_library": str(loaded_path) if loaded_path else None,
            "loaded_shared_library_sha256": digest(loaded_path)
            if loaded_path is not None
            else None,
            "loader_debug_line": line,
            "command": record["index"],
            "stdout_sha256": record["stdout_sha256"],
            "stderr_sha256": record["stderr_sha256"],
            "stdout": text_of(
                (self.out / "commands" / f"{record['index']:03d}-lifecycle"
                 / "stdout.bin").read_bytes()
            ),
            "assertions": "original fixture_lifecycle_test.cpp exit status",
        }
        write_json(self.out / "lifecycle.json", self.lifecycle_record)

    def check_project(self, phase, label, project, backend):
        return self.run_command(
            phase,
            label,
            [
                self.cli,
                "check",
                "--project",
                str(project / "lang.toml"),
                "--backend",
                backend,
                "--format",
                "json",
            ],
            CHECK_TIMEOUT,
            self.source_allowed,
            (
                "candidate Source check: this role's CLI + fixed LLVM only"
                if self.role == "candidate"
                else "reference Source check: reference CLI + matching helper + fixed LLVM"
            ),
            forbid_helper=self.role == "candidate",
        )

    def probe(self):
        label = "source:integration-probe"
        record = self.check_project(
            "probe",
            label,
            self.tests / "projects/probe",
            "native",
        )
        document = None
        document_error = None
        try:
            document = parse_json_bytes(record_stdout(record), label)
        except EvidenceError as error:
            document_error = error
        trace_status = (record.get("trace") or {}).get(
            "status", "not_requested"
        )
        problems = process_problems(record, label)
        passed = False
        unsupported = False
        if problems:
            self.fail(label, "; ".join(problems))
        elif trace_status not in ("ok", "not_requested"):
            self.fail(
                label,
                f"trace status is {trace_status!r}, not a clean trace",
            )
        elif record.get("exit") == 0:
            if document is None:
                self.fail(
                    label,
                    str(document_error)
                    if document_error is not None
                    else f"{label} wrote no JSON document",
                )
            else:
                problems, _ = document_problems(document, label, "ok")
                if problems:
                    self.fail(label, "; ".join(problems))
                else:
                    passed = True
        elif (
            self.source_tests == "auto"
            and type(record.get("exit")) is int
            and record.get("exit") == 1
            and document is not None
        ):
            problems, _ = document_problems(document, label, "error")
            if problems:
                self.fail(label, "; ".join(problems))
            else:
                missing, _detail = legacy_integration_missing(document)
                if missing:
                    unsupported = True
                else:
                    self.fail(
                        label,
                        f"{label} exit is 1, expected 0",
                    )
        else:
            if type(record.get("exit")) is int:
                self.fail(
                    label,
                    f"{label} exit is {record.get('exit')}, expected 0",
                )
            if document is None:
                self.fail(
                    label,
                    str(document_error)
                    if document_error is not None
                    else f"{label} wrote no JSON document",
                )
        status = (
            "passed"
            if passed
            else ("unsupported" if unsupported else "failed")
        )
        self.probe_record = {
            "status": status,
            "command": record["index"],
            "document": document,
            "exit": record.get("exit"),
            "trace_status": trace_status,
            "auto_unsupported": unsupported,
        }
        write_json(self.out / "probe.json", self.probe_record)
        return passed, document, unsupported

    def mark_source_not_run(self, reason):
        for project in SOURCE_PROJECTS:
            for opt in self.opts:
                self.not_run_case(f"source:{project}:O{opt}", reason)
        for name in INTEGRATION_NEGATIVES:
            self.not_run_case("negative:" + name, reason)
        self.not_run_case("no-output:bad_metadata", reason)
        self.not_run_case("core-freestanding", reason)

    def build(self, project, opt, label, timeout=BUILD_TIMEOUT):
        target = self.out / "builds" / f"{safe_label(label)}"
        if target.exists():
            self.fail("build:" + label, f"build target already exists: {target}")
            return None, None
        record = self.run_command(
            "build",
            "build:" + label,
            [
                self.cli,
                "build",
                "--project",
                str(project / "lang.toml"),
                "--backend",
                "native",
                "--opt",
                str(opt),
                "--out-dir",
                str(target),
                "--format",
                "json",
            ],
            timeout,
            self.source_allowed,
            (
                "candidate Source compile: this role's CLI + fixed LLVM only"
                if self.role == "candidate"
                else "reference Source compile: reference CLI + matching helper + fixed LLVM"
            ),
            forbid_helper=self.role == "candidate",
        )
        clean = self.clean(record, "build:" + label, 0)
        document = None
        passed = False
        if clean:
            document, _ = self.document(record, "build:" + label, "ok")
            passed = (
                document is not None
                and document.get("status") in ("ok", "passed")
                and (target / "program").is_file()
            )
            if document is not None and not (target / "program").is_file():
                self.fail("build:" + label, "build published no program")
        initial = self.generated_map(target)
        self.remember_generated("build:" + label, target, initial)
        result = {
            "status": "passed" if passed else "failed",
            "label": label,
            "opt": opt,
            "target": str(target),
            "command": record["index"],
            "exit": record["exit"],
            "argv": record["argv"],
            "cwd": record["cwd"],
            "timeout_s": record["timeout_s"],
            "timed_out": record["timed_out"],
            "cleanup_complete": record["cleanup_complete"],
            "abandoned_pipes": record["abandoned_pipes"],
            "stdout_sha256": record["stdout_sha256"],
            "stderr_sha256": record["stderr_sha256"],
            "trace": record["trace"],
            "document": document,
            "generated_initial": initial,
        }
        return (target / "program") if passed else None, result

    def run_program(self, label, program):
        record = self.run_command(
            "program",
            "run:" + label,
            [program],
            self.args.timeout,
            {str(Path(program).resolve())},
            "program execution: the published ELF only",
        )
        passed = self.clean(record, "run:" + label, 0)
        return record, passed

    def positive(self, project_name, opt):
        label = f"{project_name}:O{opt}"
        project = self.tests / "projects" / project_name
        program, build_record = self.build(project, opt, label)
        run_record = None
        run_passed = False
        if program is not None:
            run_record, run_passed = self.run_program(label, program)
        passed = program is not None and run_passed
        case = {
            "status": "passed" if passed else "failed",
            "case": project_name,
            "opt": opt,
            "build": build_record,
            "run": {
                "status": "passed" if run_passed else "failed",
                "command": run_record["index"] if run_record else None,
                "exit": run_record["exit"] if run_record else None,
                "argv": run_record["argv"] if run_record else None,
                "cwd": run_record["cwd"] if run_record else None,
                "timeout_s": run_record["timeout_s"] if run_record else None,
                "timed_out": run_record["timed_out"] if run_record else None,
                "cleanup_complete": run_record["cleanup_complete"]
                if run_record
                else None,
                "abandoned_pipes": run_record["abandoned_pipes"]
                if run_record
                else None,
                "stdout_sha256": run_record["stdout_sha256"]
                if run_record
                else None,
                "stderr_sha256": run_record["stderr_sha256"]
                if run_record
                else None,
                "trace": run_record["trace"] if run_record else None,
                "stdout_path": run_record["stdout_path"] if run_record else None,
                "stderr_path": run_record["stderr_path"] if run_record else None,
            },
            "program": str(program) if program else None,
            "program_sha256": digest(program) if program else None,
        }
        self.cases[label] = case
        write_json(self.out / "cases" / f"{safe_label(label)}.json", case)
        return passed

    def negative(self, name, backend, expected_code):
        project = self.tests / "negative" / name
        record = self.check_project(
            "negative", "negative:" + name, project, backend
        )
        clean = self.clean(record, "negative:" + name, 1)
        document = None
        codes = []
        if clean:
            document, codes = self.document(
                record, "negative:" + name, "error", expected_code
            )
        program_absent = not (project / "program").exists() and not (
            project / "program.py"
        ).exists()
        ok = (
            clean
            and document is not None
            and expected_code in codes
            and program_absent
        )
        if clean and not program_absent:
            self.fail(
                "negative:" + name,
                "rejection published program/program.py",
            )
        case = {
            "status": "passed" if ok else "failed",
            "kind": "negative",
            "name": name,
            "backend": backend,
            "expected_code": expected_code,
            "observed_codes": codes,
            "program_absent": program_absent,
            "command": record["index"],
            "exit": record["exit"],
            "argv": record["argv"],
            "cwd": record["cwd"],
            "timeout_s": record["timeout_s"],
            "timed_out": record["timed_out"],
            "cleanup_complete": record["cleanup_complete"],
            "abandoned_pipes": record["abandoned_pipes"],
            "stdout_sha256": record["stdout_sha256"],
            "stderr_sha256": record["stderr_sha256"],
            "trace": record["trace"],
            "document": document,
        }
        self.cases["negative:" + name] = case
        write_json(self.out / "cases" / f"negative_{safe_label(name)}.json", case)
        return ok

    def no_output_build(self, name, expected_code):
        target = self.out / "no-output" / safe_label(name)
        if target.exists():
            self.fail(
                "no-output:" + name,
                f"no-output target already exists: {target}",
            )
            return False
        record = self.run_command(
            "no-output-build",
            "no-output:" + name,
            [
                self.cli,
                "build",
                "--project",
                str(self.tests / "negative" / name / "lang.toml"),
                "--backend",
                "native",
                "--opt",
                "0",
                "--out-dir",
                str(target),
                "--format",
                "json",
            ],
            BUILD_TIMEOUT,
            self.source_allowed,
            (
                "candidate Source rejection: this role's CLI + fixed LLVM only"
                if self.role == "candidate"
                else "reference Source rejection: reference CLI + matching helper + fixed LLVM"
            ),
            forbid_helper=self.role == "candidate",
        )
        clean = self.clean(record, "no-output:" + name, 1)
        document = None
        codes = []
        if clean:
            document, codes = self.document(
                record, "no-output:" + name, "error", expected_code
            )
        program_absent = not (target / "program").exists() and not (
            target / "program.py"
        ).exists()
        ok = (
            clean
            and document is not None
            and expected_code in codes
            and program_absent
        )
        if clean and not program_absent:
            self.fail(
                "no-output:" + name,
                "no-output build published program/program.py",
            )
        result = {
            "status": "passed" if ok else "failed",
            "name": name,
            "expected_code": expected_code,
            "observed_codes": codes,
            "program_absent": program_absent,
            "target": str(target),
            "published_files": self.generated_map(target),
            "command": record["index"],
            "exit": record["exit"],
            "argv": record["argv"],
            "cwd": record["cwd"],
            "timeout_s": record["timeout_s"],
            "timed_out": record["timed_out"],
            "cleanup_complete": record["cleanup_complete"],
            "abandoned_pipes": record["abandoned_pipes"],
            "stdout_sha256": record["stdout_sha256"],
            "stderr_sha256": record["stderr_sha256"],
            "trace": record["trace"],
            "document": document,
        }
        self.cases["no-output:" + name] = result
        write_json(self.out / "cases" / f"no-output_{safe_label(name)}.json", result)
        return ok

    def archives(self):
        results = {}
        all_clean = True
        for label, archive in (
            ("hosted", self.hosted_runtime),
            ("native", self.native_runtime),
        ):
            if archive is None:
                results[label] = {
                    "status": "missing",
                    "archive": None,
                    "symbols_present": [],
                    "all_required": False,
                }
                all_clean = False
                self.fail("archive:" + label, "missing runtime archive")
                continue
            record = self.run_command(
                "nm",
                "archive:" + label,
                [self.nm, "-g", "--defined-only", str(archive)],
                NM_TIMEOUT,
                {str(self.nm.resolve())},
                "runtime archive symbols: fixed nm only",
            )
            clean = self.clean(record, "archive:" + label, 0)
            symbols = []
            if clean:
                symbols = text_of(record_stdout(record)).splitlines()
            present = [
                symbol
                for symbol in HOSTED_SYMBOLS
                if any(symbol in line for line in symbols)
            ]
            required = label == "hosted"
            passed = clean and (not required or len(present) == len(HOSTED_SYMBOLS))
            if required and not passed:
                self.fail(
                    "archive:" + label + ":registration-symbols",
                    f"hosted archive is missing required symbols; present={present}",
                )
            if not clean:
                passed = False
            all_clean = all_clean and passed
            results[label] = {
                "status": "passed" if passed else "failed",
                "archive": str(archive),
                "archive_sha256": digest(archive),
                "required_symbols": required,
                "symbols_present": present,
                "all_required": len(present) == len(HOSTED_SYMBOLS),
                "command": record["index"],
                "exit": record["exit"],
                "argv": record["argv"],
                "cwd": record["cwd"],
                "timeout_s": record["timeout_s"],
                "timed_out": record["timed_out"],
                "cleanup_complete": record["cleanup_complete"],
                "abandoned_pipes": record["abandoned_pipes"],
                "stdout_sha256": record["stdout_sha256"],
                "stderr_sha256": record["stderr_sha256"],
                "trace": record["trace"],
            }
        self.archives_record = {
            "status": "passed" if all_clean else "failed",
            "archives": results,
        }
        write_json(self.out / "archives.json", self.archives_record)
        return all_clean

    def core(self):
        project = self.root / "tests/fixtures/core"
        label = "core-freestanding"
        program, build_record = self.build(project, 0, label)
        if program is None:
            self.core_record = {
                "status": "failed",
                "build": build_record,
                "readelf": None,
                "run": None,
            }
            write_json(self.out / "core.json", self.core_record)
            return False
        record = self.run_command(
            "readelf",
            "core-freestanding:elf",
            [self.readelf, "-l", "-d", program],
            READELF_TIMEOUT,
            {str(self.readelf.resolve())},
            "core ELF inspection: fixed readelf only",
        )
        clean = self.clean(record, "core-freestanding:elf", 0)
        elf_stdout = text_of(record_stdout(record))
        elf_passed = clean and "INTERP" not in elf_stdout and "NEEDED" not in elf_stdout
        if clean and not elf_passed:
            self.fail(
                "core-freestanding:elf",
                "core ELF has INTERP/NEEDED or readelf failed",
            )
        run_record, run_passed = self.run_program(label, program)
        passed = elf_passed and run_passed
        self.core_record = {
            "status": "passed" if passed else "failed",
            "build": build_record,
            "readelf": {
                "status": "passed" if elf_passed else "failed",
                "command": record["index"],
                "exit": record["exit"],
                "argv": record["argv"],
                "cwd": record["cwd"],
                "timeout_s": record["timeout_s"],
                "timed_out": record["timed_out"],
                "cleanup_complete": record["cleanup_complete"],
                "abandoned_pipes": record["abandoned_pipes"],
                "stdout_sha256": record["stdout_sha256"],
                "stderr_sha256": record["stderr_sha256"],
                "trace": record["trace"],
                "no_interp": "INTERP" not in elf_stdout,
                "no_needed": "NEEDED" not in elf_stdout,
            },
            "run": {
                "status": "passed" if run_passed else "failed",
                "command": run_record["index"],
                "exit": run_record["exit"],
                "argv": run_record["argv"],
                "cwd": run_record["cwd"],
                "timeout_s": run_record["timeout_s"],
                "timed_out": run_record["timed_out"],
                "cleanup_complete": run_record["cleanup_complete"],
                "abandoned_pipes": run_record["abandoned_pipes"],
                "stdout_sha256": run_record["stdout_sha256"],
                "stderr_sha256": run_record["stderr_sha256"],
                "trace": run_record["trace"],
            },
        }
        write_json(self.out / "core.json", self.core_record)
        return passed

    # -- source routing ----------------------------------------------------

    def source_route(self, fixture_ok):
        if self.source_tests == "skip":
            self.mark_source_not_run("--source-tests=skip")
            self.static_legacy_route()
            return
        failures_before = len(self.failures)
        probe_ok = False
        probe_document = None
        probe_unsupported = False
        if self.source_tests == "require" or fixture_ok:
            try:
                probe_ok, probe_document, probe_unsupported = self.probe()
            except Exception as error:  # noqa: BLE001
                self.fail("source:integration-probe", repr(error))
        if self.source_tests == "require":
            self.full_source_route()
            return
        if probe_ok:
            self.full_source_route()
            return
        if probe_unsupported and len(self.failures) == failures_before:
            reason = "capturing registration not integrated"
            missing, detail = legacy_integration_missing(probe_document)
            if missing:
                reason = "capturing registration not integrated: " + detail
            self.not_run_case("source:integration-probe", reason)
            self.mark_source_not_run(reason)
            self.static_legacy_route()
            return
        # A failed or unqualified probe is never an auto-mode capability skip.
        self.static_legacy_route()

    # -- full route --------------------------------------------------------

    def full_source_route(self):
        for project in SOURCE_PROJECTS:
            for opt in self.opts:
                try:
                    self.positive(project, opt)
                except Exception as error:  # noqa: BLE001 - keep the matrix
                    self.fail(
                        f"source:{project}:O{opt}",
                        "case exception: " + repr(error),
                    )
        for name, (backend, expected) in ALL_NEGATIVES.items():
            try:
                self.negative(name, backend, expected)
            except Exception as error:  # noqa: BLE001
                self.fail("negative:" + name, "case exception: " + repr(error))
        for name, expected in NO_OUTPUT_BUILDS.items():
            try:
                self.no_output_build(name, expected)
            except Exception as error:  # noqa: BLE001
                self.fail("no-output:" + name, "case exception: " + repr(error))
        try:
            self.archives()
        except Exception as error:  # noqa: BLE001
            self.fail("archive", "case exception: " + repr(error))
        try:
            self.core()
        except Exception as error:  # noqa: BLE001
            self.fail("core-freestanding", "case exception: " + repr(error))

    def static_legacy_route(self):
        for name, (backend, expected) in STATIC_NEGATIVES.items():
            try:
                self.negative(name, backend, expected)
            except Exception as error:  # noqa: BLE001
                self.fail("negative:" + name, "case exception: " + repr(error))
        try:
            self.no_output_build("static_capture", "E_CAPABILITY_UNAVAILABLE")
        except Exception as error:  # noqa: BLE001
            self.fail("no-output:static_capture", "case exception: " + repr(error))
        try:
            self.archives()
        except Exception as error:  # noqa: BLE001
            self.fail("archive", "case exception: " + repr(error))

    # -- generated after ---------------------------------------------------

    def compare_generated(self):
        changed = False
        final = {}
        for label, item in self.generated_initial.items():
            path = Path(item["path"])
            after = (
                self.generated_map(path)
                if path.is_dir()
                else file_info(path)
            )
            final[label] = {"path": str(path), "after": after}
            if item["initial"] != after:
                changed = True
                self.fail(
                    "generated:" + label,
                    "generated artifact changed after its creation",
                )
        self.generated_final = final
        write_json(self.out / "generated.initial.json", self.generated_initial)
        write_json(self.out / "generated.after.json", final)
        return not changed

    # -- proposal ----------------------------------------------------------

    def write_proposal(self):
        if self.args.proposal_out is None:
            return None
        path = Path(self.args.proposal_out)
        if path.exists():
            self.fail("proposal", f"proposal already exists: {path}")
            return None
        corpus = json.loads((self.root / "selfhost/corpus.json").read_text())
        cases = corpus.get("cases", corpus if isinstance(corpus, list) else [])
        rows = proposal_rows(cases)
        if len(rows) != 20:
            raise EvidenceError(
                f"expected the original 20 family rows, found {len(rows)}"
            )
        document = {
            "schema": PROPOSAL_SCHEMA,
            "operation_id": OPERATION_ID,
            "scope": (
                "The original 20 c_retained_capturing rows only; no new row, "
                "no duplicate alias, all rows stay disabled.  This proposal "
                "does not write the Root catalog."
            ),
            "role": self.role,
            "runner": "tests/native/c_retained_capturing/run_capturing.py",
            "run_status": self.final_status(),
            "rows": rows,
            "runner_only_evidence": {
                "positive_matrix": (
                    "13 projects x O0/O2 public build+program; the exact "
                    "stdout/stderr, argv, cwd, process flags and trace are in "
                    f"{self.out}/commands and {self.out}/cases"
                ),
                "rejections": (
                    "seven check rejections require exit 1, JSON status error, "
                    "the original code, and no program/program.py; the two "
                    "no-output builds are separate O0 builds"
                ),
                "lifecycle": (
                    "the original fixture_lifecycle_test.cpp is built once "
                    "(separately accounted) and run once per role with "
                    "LD_LIBRARY_PATH pointing at that role's private "
                    "libc_capturing.so; LD_DEBUG proves the loaded path"
                ),
                "archives": (
                    "hosted archive requires the eight lang_c_registration_*_v1 "
                    "symbols; the native archive is recorded without a new "
                    "symbol requirement"
                ),
                "core": (
                    "the freestanding core O0 build/readelf/run is runner-only "
                    "evidence.  Its catalog mapping is a separate task "
                    "(091c984); this run does not claim that task's result."
                ),
                "python_rows": (
                    "python_unsupported and python_static_unsupported are "
                    "check-only: the original runner never builds a Python "
                    "program for them; their backend/operation placeholder is "
                    "corrected here."
                ),
            },
            "evidence": {
                "report": str(self.out / "report.json"),
                "commands": str(self.out / "commands.jsonl"),
                "cases": str(self.out / "cases"),
                "inputs_before": str(self.out / "inputs.before.json"),
                "inputs_after": str(self.out / "inputs.after.json"),
            },
        }
        write_json(path, document)
        return {
            "path": str(path),
            "sha256": digest(path),
            "rows": len(rows),
            "all_disabled": all(not row["enabled"] for row in rows),
        }

    # -- report ------------------------------------------------------------

    def status(self):
        if self.failures:
            return "failed"
        if self.not_run:
            return "passed_with_not_run"
        return "passed"

    def final_status(self):
        if self.failures:
            return "failed"
        if self.source_tests == "require" and self.not_run:
            return "failed"
        if self.not_run:
            return "passed_with_not_run"
        return "passed"

    def actual_counts(self):
        phases = Counter(record["phase"] for record in self.commands)
        return {
            "public_builds": phases.get("build", 0)
            + phases.get("no-output-build", 0),
            "public_checks": phases.get("probe", 0) + phases.get("negative", 0),
            "woozy_programs": phases.get("program", 0),
            "fixture_compile": phases.get("fixture-compile", 0),
            "lifecycle": phases.get("lifecycle", 0),
            "nm": phases.get("nm", 0),
            "readelf": phases.get("readelf", 0),
            "git": phases.get("git", 0),
            "total": len(self.commands),
        }

    def case_summary(self):
        summary = Counter()
        for case in self.cases.values():
            summary[case.get("status", "unknown")] += 1
            if case.get("kind") == "negative":
                summary["negative_" + case.get("status", "unknown")] += 1
            elif case.get("name") in NO_OUTPUT_BUILDS:
                summary["no_output_" + case.get("status", "unknown")] += 1
            else:
                summary["positive_" + case.get("status", "unknown")] += 1
        return dict(sorted(summary.items()))

    def stdlib_map_sha(self):
        if self.stdlib is None:
            return None
        try:
            return tree_snapshot(self.stdlib)["map_sha256"]
        except EvidenceError:
            return None

    def tool_hashes(self):
        labels = (
            "clang",
            "opt",
            "llc",
            "ld.lld",
            "lld",
            "ld",
            "nm",
            "readelf",
            "git",
            "python",
            "strace",
            "libxml2",
        )
        files = (self.inputs_before or {}).get("files", {})
        result = {}
        for label in labels:
            info = files.get(label)
            if info is not None:
                result[label] = {
                    "path": info.get("path"),
                    "resolved": info.get("resolved"),
                    "sha256": info.get("sha256"),
                    "exists": info.get("exists"),
                }
        return result

    def write_report(self, proposal):
        actual = self.actual_counts()
        status = self.status()
        if self.source_tests == "require" and self.not_run:
            for item in self.not_run:
                self.failures.append(
                    item["name"]
                    + ": required source test not run ("
                    + item["reason"]
                    + ")"
                )
            status = "failed"
        expected = dict(EXPECTED_COUNTS)
        complete = actual["total"] == expected["total"]
        report = {
            "schema": EVIDENCE_SCHEMA,
            "operation_id": OPERATION_ID,
            "role": self.role,
            "contract": "compiler-dev/1",
            "checked_at": datetime.now(timezone.utc).isoformat(),
            "status": status,
            "root": str(self.root),
            "out": str(self.out),
            "cli": str(self.cli),
            "cli_sha256": maybe_digest(self.cli),
            "helper": str(self.helper) if self.helper else None,
            "helper_sha256": maybe_digest(self.helper),
            "stdlib": str(self.stdlib) if self.stdlib else None,
            "stdlib_tree_sha256": self.stdlib_map_sha(),
            "native_runtime": str(self.native_runtime)
            if self.native_runtime
            else None,
            "native_runtime_sha256": maybe_digest(self.native_runtime),
            "hosted_runtime": str(self.hosted_runtime)
            if self.hosted_runtime
            else None,
            "hosted_runtime_sha256": maybe_digest(self.hosted_runtime),
            "tools": self.tool_hashes(),
            "source_tests": self.source_tests,
            "options": list(self.opts),
            "timeout_s": self.args.timeout,
            "require_trace": bool(self.args.require_trace),
            "strace": str(self.strace) if self.strace else None,
            "strace_sha256": digest(self.strace) if self.strace else None,
            "head": self.head,
            "expected_counts": expected,
            "actual_counts": actual,
            "command_count_complete": complete,
            "commands_path": str(self.commands_path),
            "commands": self.commands,
            "cases": self.cases,
            "case_summary": self.case_summary(),
            "failed": self.failures,
            "not_run": self.not_run,
            "probe": self.probe_record,
            "lifecycle": self.lifecycle_record,
            "archives": self.archives_record,
            "core": self.core_record,
            "inputs_before": str(self.out / "inputs.before.json"),
            "inputs_after": str(self.out / "inputs.after.json")
            if (self.out / "inputs.after.json").is_file()
            else None,
            "snapshots_before": self.snapshots_before,
            "snapshots_after": self.snapshots_after,
            "inputs_unchanged": self.inputs_unchanged,
            "generated_initial": str(self.out / "generated.initial.json"),
            "generated_after": str(self.out / "generated.after.json"),
            "proposal": proposal,
            "cleanup": {
                "out_preserved": True,
                "partial_removed": False,
                "process_runner": (
                    "selfhost/corpus.py::_run_process_impl with bounded "
                    "deadlines and process-group cleanup"
                ),
                "timed_out_commands": sum(
                    1 for command in self.commands if command["timed_out"] is True
                ),
                "signalled_commands": sum(
                    1 for command in self.commands if command["status"] == "signalled"
                ),
                "spawn_errors": sum(
                    1 for command in self.commands if command["status"] == "spawn_error"
                ),
            },
        }
        write_json(self.out / "report.json", report)
        return report


# ---------------------------------------------------------------------------
# Small helpers that need the runner or the shared process helper.


def record_stdout(record):
    return Path(record["stdout_path"]).read_bytes()


def parse_label_pairs(values, option):
    pairs = []
    for value in values or ():
        if "=" not in value:
            raise EvidenceError(f"{option} needs LABEL=PATH, got {value!r}")
        label, path = value.split("=", 1)
        if not label or not path:
            raise EvidenceError(f"{option} needs non-empty LABEL=PATH")
        pairs.append((label, path))
    return pairs


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--hosted-runtime", type=Path)
    parser.add_argument("--native-runtime", type=Path)
    parser.add_argument("--fixture-test", type=Path)
    parser.add_argument("--helper", type=Path)
    parser.add_argument("--stdlib", type=Path)
    parser.add_argument("--toolchain", type=Path)
    parser.add_argument("--nm", type=Path)
    parser.add_argument("--readelf", type=Path)
    parser.add_argument("--git", type=Path)
    parser.add_argument("--ld", type=Path)
    parser.add_argument("--strace", type=Path)
    parser.add_argument("--role", choices=("candidate", "reference"), default="reference")
    parser.add_argument("--opt", type=int, action="append", dest="opts")
    parser.add_argument("--timeout", type=int, default=PROGRAM_TIMEOUT)
    parser.add_argument(
        "--source-tests",
        choices=("auto", "require", "skip"),
        default="auto",
    )
    parser.add_argument("--require-trace", action="store_true")
    parser.add_argument("--extra-input", action="append", default=[])
    parser.add_argument("--snapshot-root", action="append", default=[])
    parser.add_argument("--proposal-out", type=Path)
    args = parser.parse_args()
    try:
        args.extra_input_pairs = parse_label_pairs(args.extra_input, "--extra-input")
        args.snapshot_pairs = parse_label_pairs(args.snapshot_root, "--snapshot-root")
    except EvidenceError as error:
        print(str(error), file=sys.stderr)
        return 2

    runner = Runner(args)
    if not runner.prepare():
        return 2
    report = {"status": "failed"}
    try:
        runner.git_identity()
        fixture_ok = runner.fixture_compile()
        if runner.fixture_test is not None:
            runner.lifecycle()
        elif runner.source_tests == "require":
            runner.lifecycle()
        else:
            runner.lifecycle_record = {
                "status": "not_provided",
                "reason": "legacy mode without --fixture-test",
            }
            write_json(runner.out / "lifecycle.json", runner.lifecycle_record)

        runner.source_route(fixture_ok)
    except Exception as error:  # noqa: BLE001 - evidence runner
        runner.fail("runner exception", repr(error))
    finally:
        try:
            runner.inputs_unchanged = runner.compare_snapshots()
        except Exception as error:  # noqa: BLE001
            runner.inputs_unchanged = False
            runner.fail("inputs:after", repr(error))
        try:
            runner.compare_generated()
        except Exception as error:  # noqa: BLE001
            runner.fail("generated", repr(error))
        proposal = None
        try:
            proposal = runner.write_proposal()
        except Exception as error:  # noqa: BLE001
            runner.fail("proposal", repr(error))
        try:
            report = runner.write_report(proposal)
        except Exception as error:  # noqa: BLE001 - keep evidence, report failure
            report = {"status": "failed"}
            print("FAIL report: " + repr(error), file=sys.stderr, flush=True)
        print(
            json.dumps(
                {
                    "status": report["status"],
                    "role": report["role"],
                    "actual_counts": report["actual_counts"],
                    "failed": report["failed"],
                    "not_run": report["not_run"],
                    "report": str(runner.out / "report.json"),
                },
                ensure_ascii=False,
                allow_nan=False,
            )
        )
    return 0 if report.get("status") in ("passed", "passed_with_not_run") else 1


def legacy_integration_missing(document):
    for entry in (document or {}).get("diagnostics", []):
        code = entry.get("code", "")
        text = (entry.get("message", "") + " " + entry.get("phase", "")).lower()
        if code == "E_BINDING_INVALID" and any(
            token in text
            for token in (
                "registration",
                "registers",
                "callback_userdata",
                "unregister_symbol",
                "unknown",
                "unexpected",
                "additional",
            )
        ):
            return True, entry.get("message", "registration metadata")
        if code == "E_CAPABILITY_UNAVAILABLE" and any(
            token in text
            for token in (
                "c binding",
                "c_binding",
                "native",
                "hosted",
                "llvm",
                "callback",
            )
        ):
            return True, entry.get("message", "C/native capability")
    return False, ""


if __name__ == "__main__":
    raise SystemExit(main())
