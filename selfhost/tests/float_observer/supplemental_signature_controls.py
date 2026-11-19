#!/usr/bin/env python3
"""Small typed/Panic exact-result controls for supplemental.py.

Reads the retained r4 observation/metadata records, checks that the original
positive records pass, then mutates only ``signature.result`` to i32 and to
the opposite f32/f64 width.  Both the observation checker and the metadata
checker must reject.  Missing exact expectations must also reject when probes
exist.  This does not compile or rerun the 14 projects.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
DEFAULT_EVIDENCE = (REPO.parent / "checks" /
                    "selfhost-float-supplemental-20261114-r4/results.json")
CASES = ("f32-typed-defer", "f64-panic-skip")


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, default=DEFAULT_EVIDENCE)
    parser.add_argument("--out", type=Path, default=Path("signature-controls.json"))
    args = parser.parse_args(argv)
    if not args.evidence.is_file():
        parser.error(f"evidence is absent: {args.evidence}")
    supplemental = load_module("supplemental_under_test", HERE / "supplemental.py")
    cases_module = load_module(
        "original_supplemental_cases",
        REPO / "tests/python/floating_cases.py")
    cases = {ident: (expected, probes)
             for ident, body, expected, probes in cases_module.supplemental()}
    evidence = json.loads(args.evidence.read_text())
    runs = {run["id"]: run for run in evidence["runs"]}
    controls = []

    def accepted(name: str, call):
        try:
            call()
            passed = True
            error = None
        except Exception as exc:  # positive must genuinely pass
            passed = False
            error = f"{type(exc).__name__}: {exc}"
        controls.append({"name": name, "kind": "positive",
                         "passed": passed, "error": error})

    def rejected(name: str, call):
        try:
            call()
            passed = False
            error = "checker accepted the bad record"
        except RuntimeError as exc:
            passed = True
            error = None
        except Exception as exc:  # unexpected failure is not a detected control
            passed = False
            error = f"unexpected {type(exc).__name__}: {exc}"
        controls.append({"name": name, "kind": "negative",
                         "passed": passed, "error": error})

    accepted("no-probe-with-no-expected-accepted",
             lambda: supplemental.strict_validate([], [], "no-probe", None))

    for ident in CASES:
        expected, probes = cases[ident]
        if len(probes) != 1:
            raise RuntimeError(f"{ident}: expected one probe")
        probe = probes[0]
        expected_result = supplemental.PROBE_RESULT_TYPES[ident]
        opposite = "f64" if expected_result == "f32" else "f32"
        if expected_result not in ("f32", "f64"):
            raise RuntimeError(f"{ident}: unexpected exact result")
        if "result" in probe:
            raise RuntimeError(f"{ident}: expected typed/Panic probe without result")
        run = runs.get(ident)
        if not run or run.get("status") != "passed" or len(run.get("values", [])) != 1:
            raise RuntimeError(f"{ident}: retained r4 record is not a positive record")
        row = run["values"][0]["actual"]
        if row.get("signature", {}).get("result") != expected_result:
            raise RuntimeError(f"{ident}: retained r4 signature differs")
        work = Path(evidence["work_dir"])
        metadata_path = work / "runs" / ident / "driver-out" / "float-metadata.json"
        cli_program_path = work / "runs" / ident / "cli" / "program.py"
        cli_langrt_path = work / "runs" / ident / "cli" / "langrt.py"
        metadata = json.loads(metadata_path.read_text())
        cli_program = cli_program_path.read_bytes()
        cli_langrt = cli_langrt_path.read_bytes()
        functions = [item for item in metadata["functions"]
                     if item.get("source_name") == probe["function"]]
        if len(functions) != 1:
            raise RuntimeError(f"{ident}: metadata probe is not unique")

        accepted(
            f"{ident}:row-positive",
            lambda: supplemental.strict_validate(
                [probe], [row], f"{ident}:row-positive", expected_result))
        accepted(
            f"{ident}:metadata-positive",
            lambda: supplemental.metadata_check(
                metadata, [probe], cli_program, cli_langrt,
                f"{ident}:metadata-positive", expected_result))
        for bad_result in ("i32", opposite):
            bad_row = copy.deepcopy(row)
            bad_row["signature"]["result"] = bad_result
            rejected(
                f"{ident}:row-result-{bad_result}",
                lambda bad_row=bad_row: supplemental.strict_validate(
                    [probe], [bad_row], f"{ident}:row-result-{bad_result}",
                    expected_result))
            bad_metadata = copy.deepcopy(metadata)
            bad_functions = [item for item in bad_metadata["functions"]
                             if item.get("source_name") == probe["function"]]
            bad_functions[0]["result"] = bad_result
            rejected(
                f"{ident}:metadata-result-{bad_result}",
                lambda bad_metadata=bad_metadata: supplemental.metadata_check(
                    bad_metadata, [probe], cli_program, cli_langrt,
                    f"{ident}:metadata-result-{bad_result}", expected_result))
        rejected(
            f"{ident}:row-missing-expected",
            lambda: supplemental.strict_validate(
                [probe], [row], f"{ident}:row-missing-expected", None))
        rejected(
            f"{ident}:metadata-missing-expected",
            lambda: supplemental.metadata_check(
                metadata, [probe], cli_program, cli_langrt,
                f"{ident}:metadata-missing-expected", None))

    passed = all(control["passed"] for control in controls)
    result = {
        "status": "passed" if passed else "failed",
        "operation_id": "SH10v2-supplemental-return-signature-controls-20261114",
        "evidence": str(args.evidence),
        "evidence_sha256": sha(args.evidence),
        "fixture": str(REPO / "tests/python/floating_cases.py"),
        "fixture_sha256": sha(REPO / "tests/python/floating_cases.py"),
        "supplemental_sha256": sha(HERE / "supplemental.py"),
        "script_sha256": sha(Path(__file__)),
        "cases": list(CASES),
        "controls": controls,
        "failed": [control["name"] for control in controls
                   if not control["passed"]],
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"status": result["status"],
                      "controls": len(controls),
                      "failed": result["failed"],
                      "out": str(args.out)}, indent=2))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
