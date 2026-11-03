#!/usr/bin/env python3
"""Check this development package; this is not a language compiler.

Uses the Python standard library. The schema checker supports only the JSON
Schema keywords used by this package; unknown keywords fail explicitly.
It is not a general JSON Schema implementation or a binding conformance test.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def require(ok, message):
    if not ok:
        raise ValueError(message)


def read_json(path):
    return json.loads((ROOT / path).read_text(encoding="utf-8"))


def schema_check(value, schema, document, location="$", depth=0):
    require(depth < 100, f"schema nesting limit at {location}")
    supported = {"$schema", "$id", "$defs", "$ref", "title", "description",
                 "type", "const", "enum", "anyOf", "properties", "required",
                 "additionalProperties", "items", "pattern", "minimum", "minLength"}
    require(not (set(schema) - supported), f"unsupported schema keys: {set(schema)-supported}")
    if "$ref" in schema:
        ref = schema["$ref"]
        require(ref.startswith("#/$defs/"), "only local defs references supported")
        return schema_check(value, document["$defs"][ref.split("/")[-1]], document, location, depth+1)
    if "anyOf" in schema:
        for item in schema["anyOf"]:
            try:
                schema_check(value, item, document, location, depth+1)
                break
            except ValueError:
                continue
        else:
            raise ValueError(f"anyOf mismatch at {location}")
    if "type" in schema:
        matches = {"object": isinstance(value, dict), "array": isinstance(value, list),
                   "string": isinstance(value, str), "integer": type(value) is int,
                   "boolean": type(value) is bool, "null": value is None}
        require(matches.get(schema["type"], False), f"type mismatch at {location}")
    encode = lambda x: json.dumps(x, ensure_ascii=False, sort_keys=True)
    if "const" in schema:
        require(encode(value) == encode(schema["const"]), f"const mismatch at {location}")
    if "enum" in schema:
        require(any(encode(value) == encode(x) for x in schema["enum"]), f"enum mismatch at {location}")
    if isinstance(value, dict):
        require(set(schema.get("required", [])) <= set(value), f"missing keys at {location}")
        props = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            require(set(value) <= set(props), f"unexpected keys at {location}")
        for key, child in value.items():
            if key in props:
                schema_check(child, props[key], document, location+"."+key, depth+1)
    if isinstance(value, list) and "items" in schema:
        for i, child in enumerate(value):
            schema_check(child, schema["items"], document, f"{location}[{i}]", depth+1)
    if isinstance(value, str):
        if "pattern" in schema:
            require(re.search(schema["pattern"], value) is not None, f"pattern mismatch at {location}")
        if "minLength" in schema:
            require(len(value) >= schema["minLength"], f"short string at {location}")
    if "minimum" in schema:
        require(value >= schema["minimum"], f"minimum mismatch at {location}")


def check_markdown():
    count = 0
    for path in sorted(ROOT.rglob("*.md")):
        text = path.read_text(encoding="utf-8")
        fence = None
        ordinary = []
        for line in text.splitlines():
            match = re.match(r"^\s*(`{3,}|~{3,})", line)
            if match:
                marker = match.group(1)
                if fence is None:
                    fence = marker
                elif marker[0] == fence[0] and len(marker) >= len(fence):
                    fence = None
                continue
            if fence is None:
                ordinary.append(line)
        require(fence is None, f"unclosed fence in {path.relative_to(ROOT)}")
        prose = re.sub(r"(`+)(.*?)\1", "", "\n".join(ordinary), flags=re.S)
        for target in re.findall(r"\[[^\]\n]*\]\(([^\s)]+)\)", prose):
            if re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", target) or target.startswith("#"):
                continue
            file_part = target.split("#", 1)[0]
            require((path.parent / file_part).exists(), f"broken link {target} in {path.relative_to(ROOT)}")
        count += 1
    return count


def check_tasks():
    tasks = read_json("contracts/tasks.json")["tasks"]
    by_id = {t["id"]: t for t in tasks}
    require(len(by_id) == len(tasks) == 13, "expected 13 unique tasks")
    require(set(by_id) == {f"T{i:02d}" for i in range(13)}, "task ID range mismatch")
    visited, active = set(), set()
    def visit(tid):
        require(tid in by_id, f"unknown task {tid}")
        require(tid not in active, f"cyclic task dependency at {tid}")
        if tid in visited:
            return
        active.add(tid)
        for dep in by_id[tid]["depends_on"]:
            visit(dep)
        active.remove(tid)
        visited.add(tid)
    for task in tasks:
        visit(task["id"])
        require(task["primary_role"] != task["backup_role"], "primary/backup role collision")
        for path in task["read"]:
            require((ROOT/path).is_file(), f"missing task input {path}")
    return by_id


def check_bindings():
    binding = read_json("contracts/python_bindings.example.json")
    modules = {m["logical_path"] for m in binding["modules"]}
    require(len(modules) == len(binding["modules"]), "duplicate module")
    types = {t["id"] for t in binding["types"]}
    require(len(types) == len(binding["types"]), "duplicate type")
    builtin = {"i64", "f64", "bool", "string", "Unit"}
    known = types | builtin
    ids = set()
    for item in binding["types"] + binding["values"]:
        require(item["module"] in modules, "unknown export module")
    for item in binding["values"]:
        require(item["type"] in known, "unknown exported value type")
    for call in binding["callables"]:
        require(call["id"] not in ids, "duplicate callable")
        ids.add(call["id"])
        require(call["module"] in modules, "unknown callable module")
        require(call["return_type"] in known, "unknown return type")
        require(call["error_type"] is None or call["error_type"] in types, "unknown error type")
        owner = call["owner_type"]
        require(owner is None or owner in types, "unknown receiver type")
        require((call["call_kind"] in {"method", "callable"}) == (owner is not None), "receiver/kind mismatch")
        require((owner is None) == (call["receiver_escapes"] == "not_applicable"), "receiver escape mismatch")
        names = [p["name"] for p in call["parameters"]]
        require(len(names) == len(set(names)), "duplicate parameter")
        for param in call["parameters"]:
            require(param["type"] in known, "unknown parameter type")
        require((call["return_type"] == "Unit") == (call["ownership"]["return"] == "unit"), "Unit ownership mismatch")
    require(len(ids) == 15, "unexpected minimal callable count")
    return {"modules": len(modules), "types": len(types), "callables": len(ids),
            "runtime_conformance": "not_run"}


def check_baseline():
    manifest = read_json("contracts/baseline.manifest.json")
    for file in manifest["files"]:
        path = ROOT/"baseline"/file["path"]
        require(path.is_file(), f"missing baseline {file['path']}")
        require(hashlib.sha256(path.read_bytes()).hexdigest() == file["sha256"], f"baseline modified {file['path']}")
    return len(manifest["files"])


def check_syntax_cases():
    historical = read_json("baseline/validation.json")
    cases = [("baseline/"+x["path"], x["expected_syntax"]) for x in historical["sample_results"]]
    cases += [(x["path"], x["syntax"]) for x in read_json("contracts/test-cases.json")["cases"]]
    checker = ROOT/"baseline/tools/check_syntax.py"
    run = subprocess.run([sys.executable, str(checker), "--json"] + [x[0] for x in cases],
                         cwd=ROOT, text=True, capture_output=True)
    require(run.returncode in (0, 1), f"checker failed: {run.stderr}")
    data = json.loads(run.stdout)
    actual = {f["path"]: "accept" if f["accepted"] else "reject" for f in data["files"]}
    details = []
    for path, expected in cases:
        require(actual.get(path) == expected, f"syntax mismatch for {path}: {actual.get(path)} != {expected}")
        details.append({"path": path, "expected": expected, "actual": actual[path]})
    selftest = subprocess.run([sys.executable, str(checker), "--self-test"], cwd=ROOT,
                              text=True, capture_output=True)
    require(selftest.returncode == 0, f"baseline self-test failed: {selftest.stdout}\n{selftest.stderr}")
    return {"case_count":len(cases),"matched":len(details),"details":details,
            "self_test_output":selftest.stdout.strip(),"self_test_exit_code":selftest.returncode}


def validate():
    parsed = 0
    for path in ROOT.rglob("*.json"):
        if path.name == "package_validation.json":
            continue
        json.loads(path.read_text(encoding="utf-8"))
        parsed += 1
    pairs = [("diagnostic.schema.json", "diagnostic.example.json"),
             ("python-bindings.schema.json", "python_bindings.example.json"),
             ("test-cases.schema.json", "test-cases.json")]
    for schema_name, data_name in pairs:
        schema = read_json("contracts/"+schema_name)
        schema_check(read_json("contracts/"+data_name), schema, schema)
    by_id = check_tasks()
    codes = {c["code"] for c in read_json("contracts/diagnostic_codes.json")["codes"]}
    for case in read_json("contracts/test-cases.json")["cases"]:
        require((ROOT/case["path"]).is_file(), f"missing test {case['path']}")
        require(set(case["required_tasks"]) <= set(by_id), "unknown case task")
        if "expected_diagnostic" in case:
            require(case["expected_diagnostic"] in codes, "unregistered diagnostic")
    for path in ROOT.rglob("*.md"):
        if "baseline" not in path.relative_to(ROOT).parts:
            used = set(re.findall(r"\b[EW]_[A-Z][A-Z0-9_]*\b", path.read_text()))
            require(used <= codes, f"unregistered codes in {path.relative_to(ROOT)}: {used-codes}")
    return {"status":"passed","scope":"development_package_consistency_only",
            "json_files":parsed,"schema_instance_pairs":len(pairs),
            "schema_engine":"standard_library_subset_for_keywords_used_in_this_package",
            "markdown_files":check_markdown(),"tasks":len(by_id),
            "baseline_files_unchanged":check_baseline(),"bindings":check_bindings(),
            "syntax":check_syntax_cases(),
            "not_executed":["C++ compiler build","type/effect/ownership checking",
                            "LLVM code generation","Python transpilation","PyTorch training",
                            "binding runtime conformance","target platform execution"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--write-report", action="store_true")
    args = parser.parse_args()
    try:
        report = validate()
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as exc:
        print(json.dumps({"status":"failed","error":str(exc)},ensure_ascii=False))
        return 1
    if args.write_report:
        (ROOT/"package_validation.json").write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n")
    if args.json:
        print(json.dumps(report,ensure_ascii=False,indent=2))
    else:
        print(f"PASS: {report['tasks']} tasks; {report['schema_instance_pairs']} schema instances; "
              f"{report['markdown_files']} Markdown files; {report['syntax']['matched']} syntax classifications.")
        print(report['syntax']['self_test_output'])
        print("Scope: document/package checks only; compiler/runtime/PyTorch execution NOT performed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
