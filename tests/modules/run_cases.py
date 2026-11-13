#!/usr/bin/env python3
"""Run real C++ project/name resolution against independently prepared fixtures.

Default (legacy) mode keeps the original positional compiler and
``[compiler, manifest]`` invocation for the C++ ``lang_module_check``
component.  ``--project-cli`` explicitly selects the modern public CLI:

    run_cases.py <compiler> --project-cli

The modern mode uses ``selfhost/corpus.py::_run_process_impl`` so it has a
finite deadline and process-group cleanup.  It does not auto-detect a CLI from
the executable name or ``--help``.
"""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
CODES = {
    "same_scope_duplicate": "E_NAME_DUPLICATE",
    "parameter_body_same_scope": "E_NAME_DUPLICATE",
    "type_position_nearest_binding_kind": "E_NAME_KIND",
    "cross_package_private_hidden": "E_PRIVATE_ACCESS",
    "inconsistent_package_name": "E_PACKAGE_MISMATCH",
    "top_level_let_unsupported": "E_CAPABILITY_UNAVAILABLE",
    "invalid_logical_import_path": "E_IMPORT_PATH",
    "package_dependency_cycle_full_path": "E_IMPORT_CYCLE",
    "const_dependency_cycle": "E_CONSTANT_CYCLE",
}
PROJECT_CLI_TIMEOUT = 120.0


def validate_case(case, returncode, stderr, result):
    accept = case["expected"]["status"] == "accept"
    if returncode != (0 if accept else 1) or stderr:
        raise ValueError(f"exit={returncode}; stderr={stderr}")
    if result["status"] != ("ok" if accept else "error"):
        raise ValueError(f"status={result['status']}")
    errors = [d for d in result["diagnostics"] if d["severity"] == "error"]
    if accept and errors or not accept and not errors:
        raise ValueError(f"unexpected errors: {errors}")
    if not accept:
        expected = {CODES.get(rule, "E_NAME_NOT_FOUND")
                    for rule in case["expected"]["rules"]}
        if not expected.intersection(d["code"] for d in errors):
            raise ValueError(f"expected {expected}; got {errors}")
        for error in errors:
            span = error["primary"]
            path = Path(span["path"])
            if not path.is_relative_to(ROOT / "tests/modules/fixtures"):
                raise ValueError(f"diagnostic outside fixtures: {span}")
            if not 0 <= span["start_byte"] <= span["end_byte"] <= len(path.read_bytes()):
                raise ValueError(f"invalid raw byte range: {span}")


def legacy_case(compiler, case):
    run = subprocess.run([compiler, str(ROOT / case["manifest"])],
                         capture_output=True, text=True, timeout=20)
    result = json.loads(run.stdout)
    validate_case(case, run.returncode, run.stderr, result)


def load_corpus():
    spec = importlib.util.spec_from_file_location(
        "modules_run_cases_corpus", ROOT / "selfhost/corpus.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def project_cli_case(corpus, compiler, case, timeout):
    process = corpus._run_process_impl(
        [compiler, "check", "--project", str(ROOT / case["manifest"]),
         "--format", "json"],
        ROOT, dict(os.environ), timeout)
    if process["timed_out"]:
        raise ValueError(
            f"timed out after {timeout}s; cleanup_complete="
            f"{process['cleanup_complete']}; abandoned_pipes="
            f"{process['abandoned_pipes']}")
    if not process["cleanup_complete"]:
        raise ValueError("process cleanup was incomplete")
    if process["abandoned_pipes"]:
        raise ValueError("process pipes were abandoned")
    result = json.loads(process["stdout"].decode("utf-8", "replace"))
    validate_case(case, process["exit"],
                  process["stderr"].decode("utf-8", "replace"), result)
    return process


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", nargs="?",
                        help="lang_module_check (legacy) or public CLI")
    parser.add_argument("--project-cli", action="store_true",
                        help="use '<compiler> check --project MANIFEST "
                             "--format json' instead of the legacy positional "
                             "module_check invocation")
    parser.add_argument("--timeout", type=float, default=PROJECT_CLI_TIMEOUT,
                        help="per-case deadline for --project-cli")
    args = parser.parse_args()
    if args.compiler is None:
        parser.error("compiler is required")
    if args.project_cli and (not math.isfinite(args.timeout) or args.timeout <= 0):
        parser.error("--timeout must be finite and positive")
    compiler = str(Path(args.compiler).resolve())
    cases = json.loads((ROOT / "tests/modules/cases.json").read_text())["cases"]
    corpus = load_corpus() if args.project_cli else None
    failures = []
    for case in cases:
        try:
            if args.project_cli:
                project_cli_case(corpus, compiler, case, args.timeout)
            else:
                legacy_case(compiler, case)
        except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
            failures.append({"id": case["id"], "error": str(error)})
    report = {"passed": len(cases) - len(failures), "total": len(cases),
              "failures": failures}
    if args.project_cli:
        report["mode"] = "project-cli"
        report["process_control"] = {
            "timeout_seconds": args.timeout,
            "runner": "selfhost/corpus.py::_run_process_impl",
            "process_group_cleanup": True,
        }
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
