#!/usr/bin/env python3
"""Check the C++ CLI against existing syntax expectations, not type correctness."""
import json
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def source_for_case(case, root):
    expected = case["source"].encode("utf-8")
    if "path" not in case:
        return expected
    relative = Path(case["path"])
    actual_path = (root / relative).resolve()
    if relative.is_absolute() or not actual_path.is_relative_to(root.resolve()):
        raise ValueError("fixture path leaves the repository")
    actual = actual_path.read_bytes()
    if actual != expected:
        raise ValueError("fixture differs from the checked syntax inventory")
    return actual

def main():
    compiler = Path(sys.argv[1]).resolve()
    inventory = json.loads((ROOT / "tests/syntax/cases.json").read_text())
    schema = json.loads((ROOT / "contracts/diagnostic.schema.json").read_text())
    validate = runpy.run_path(str(ROOT / "tools/validate_package.py"))["schema_check"]
    cases = inventory["in_memory_cases"] + inventory["file_cases"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="lang-syntax-cases-") as temporary:
        source = Path(temporary) / "input.wz"
        for case in cases:
            try:
                payload = source_for_case(case, ROOT)
                if "path" in case:
                    input_path = (ROOT / case["path"]).resolve()
                else:
                    source.write_bytes(payload)
                    input_path = source
                run = subprocess.run(
                    [str(compiler), "check", "--syntax-only", "--format", "json", str(input_path)],
                    capture_output=True, text=True, timeout=10)
                expected = 0 if case["expected_syntax"] == "accept" else 1
                document = json.loads(run.stdout)
                validate(document, schema, schema)
                if run.returncode != expected:
                    raise ValueError(f"expected exit {expected}, got {run.returncode}")
                if document["status"] != ("ok" if expected == 0 else "error"):
                    raise ValueError("inconsistent diagnostic status")
                if run.stderr:
                    raise ValueError("JSON diagnostic command wrote to stderr")
                errors = [d for d in document["diagnostics"] if d["severity"] == "error"]
                if bool(errors) != bool(expected):
                    raise ValueError("missing or unexpected error diagnostics")
                if expected and any(d["primary"] is None for d in errors):
                    raise ValueError("syntax error is missing its source position")
            except (ValueError, KeyError, OSError, subprocess.TimeoutExpired) as error:
                failures.append({"name": case["name"], "error": str(error)})
    print(json.dumps({"scope": "C++ syntax classifications and diagnostic schema",
                      "passed": len(cases) - len(failures), "total": len(cases),
                      "failures": failures}, ensure_ascii=False, indent=2))
    return bool(failures)

if __name__ == "__main__":
    raise SystemExit(main())
