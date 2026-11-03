#!/usr/bin/env python3
"""Export the T02 syntax-case inventory from existing records; not a compiler."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import sys

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "tests/syntax/cases.json"
PROTOTYPE = ROOT / "baseline/tools/check_syntax.py"

def fail(message: str) -> None:
    raise SystemExit(message)

def read(relative: str) -> dict:
    return json.loads((ROOT / relative).read_text(encoding="utf-8"))

def baseline_summary() -> str:
    files = read("contracts/baseline.manifest.json")["files"]
    if len(files) != 25:
        fail(f"baseline manifest has {len(files)} files, expected 25")
    summary = hashlib.sha256()
    for item in sorted(files, key=lambda entry: entry["path"]):
        data = (ROOT / "baseline" / item["path"]).read_bytes()
        if hashlib.sha256(data).hexdigest() != item["sha256"]:
            fail(f"baseline changed: {item['path']}")
        summary.update(item["path"].encode() + b"\0" + item["sha256"].encode() + b"\n")
    return summary.hexdigest()

def capture_self_test(checker: dict) -> list[tuple[str, bool, str]]:
    captured = {}
    target = checker["self_test"].__code__

    def tracer(frame, event, arg):
        if event == "return" and frame.f_code is target:
            captured["cases"] = list(frame.f_locals["cases"])
            captured["result"] = arg
        return tracer
    previous = sys.gettrace()
    sys.settrace(tracer)
    try:
        result = checker["self_test"]()
    finally:
        sys.settrace(previous)
    if result != captured.get("result") or result.get("passed") != 59 or result.get("total") != 59 or result.get("failures"):
        fail("prototype self-test did not pass 59/59")
    cases = captured.get("cases", [])
    if len(cases) != 59:
        fail(f"captured {len(cases)} prototype cases, expected 59")
    return cases

def export_once() -> bytes:
    checker = runpy.run_path(str(PROTOTYPE), run_name="check_syntax_prototype")
    memory = []
    for name, accepted, source in capture_self_test(checker):
        expected = "accept" if accepted else "reject"
        result = checker["check_source"](source, "<self-test:" + name + ">")
        actual = "accept" if result["accepted"] else "reject"
        if actual != expected:
            fail(f"prototype case mismatch: {name}")
        memory.append({"name": name, "source": source, "expected_syntax": expected})
    notes = read("baseline/examples_notes.json")
    validation = read("baseline/validation.json")
    phases = {item["path"]: item.get("rejection_phase") for item in notes["negative_examples"]}
    note_syntax = {item["path"]: item["expected_syntax"]
                   for item in notes["positive_examples"] + notes["negative_examples"]}
    files = []
    for record in validation["sample_results"]:
        relative, expected = record["path"], record["expected_syntax"]
        if note_syntax.get(relative) != expected:
            fail(f"baseline expectation mismatch: {relative}")
        path = ROOT / "baseline" / relative
        actual = "accept" if checker["check_file"](path)["accepted"] else "reject"
        if actual != expected:
            fail(f"baseline classification mismatch: {relative}")
        stem = relative[:-5] if relative.endswith(".lang") else relative
        if stem.startswith("samples/"):
            stem = stem[8:]
        entry = {"name": "baseline/" + stem, "path": "baseline/" + relative,
                 "source": path.read_bytes().decode("utf-8"), "expected_syntax": expected}
        if relative in phases:
            entry["rejection_phase"] = phases[relative]
        files.append(entry)
    contract = read("contracts/test-cases.json")["cases"]
    for case in contract:
        path = ROOT / case["path"]
        actual = "accept" if checker["check_file"](path)["accepted"] else "reject"
        if actual != case["syntax"]:
            fail(f"contract classification mismatch: {case['id']}")
        if case["semantic"] == "reject" and case["syntax"] != "accept":
            fail(f"semantic negative does not parse: {case['id']}")
        files.append({"name": "contracts/" + case["id"], "path": case["path"],
                      "source": path.read_bytes().decode("utf-8"),
                      "expected_syntax": case["syntax"],
                      "expected_semantic": case["semantic"]})

    if (len(memory), len(validation["sample_results"]), len(contract), len(files)) != (59, 16, 5, 21):
        fail("unexpected case count")
    names = [entry["name"] for entry in memory + files]
    paths = [entry["path"] for entry in files]
    if len(names) != len(set(names)) or len(paths) != len(set(paths)):
        fail("duplicate case name or path")
    document = {
        "schema": "compiler-dev/syntax-cases/1",
        "contract": "compiler-dev/1",
        "source": {
            "prototype": "baseline/tools/check_syntax.py",
            "baseline_records": ["baseline/validation.json", "baseline/examples_notes.json"],
            "contract_records": ["contracts/test-cases.json"],
        },
        "in_memory_cases": memory,
        "file_cases": files,
    }
    return (json.dumps(document, ensure_ascii=False, indent=2) + "\n").encode("utf-8")

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify without writing")
    args = parser.parse_args()
    before = baseline_summary()
    first = export_once()
    second = export_once()
    if first != second:
        fail("two independent exports differ")
    if args.check:
        if not OUTPUT.is_file() or OUTPUT.read_bytes() != first:
            fail("tests/syntax/cases.json is stale")
        action = "checked, unchanged"
    else:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        previous = OUTPUT.read_bytes() if OUTPUT.is_file() else None
        OUTPUT.write_bytes(first)
        action = "unchanged" if previous == first else "written"

    if baseline_summary() != before:
        fail("baseline changed during export")
    digest = hashlib.sha256(first).hexdigest()
    print(f"syntax cases: {action}: {OUTPUT.relative_to(ROOT)}")
    print("  prototype self-test 59/59; 59 in-memory; 21 file cases (16 baseline + 5 contract)")
    print(f"  baseline 25 files unchanged: {before}")
    print(f"  independent exports byte-identical; sha256 {digest}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
