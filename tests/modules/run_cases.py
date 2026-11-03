#!/usr/bin/env python3
"""Run real C++ project/name resolution against independently prepared fixtures."""
import json
from pathlib import Path
import subprocess
import sys

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


def main():
    compiler = str(Path(sys.argv[1]).resolve())
    cases = json.loads((ROOT / "tests/modules/cases.json").read_text())["cases"]
    failures = []
    for case in cases:
        try:
            run = subprocess.run([compiler, str(ROOT / case["manifest"])],
                                 capture_output=True, text=True, timeout=20)
            result = json.loads(run.stdout)
            accept = case["expected"]["status"] == "accept"
            if run.returncode != (0 if accept else 1) or run.stderr:
                raise ValueError(f"exit={run.returncode}; stderr={run.stderr}")
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
        except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
            failures.append({"id": case["id"], "error": str(error)})
    print(json.dumps({"passed": len(cases) - len(failures), "total": len(cases),
                      "failures": failures}, ensure_ascii=False, indent=2))
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
