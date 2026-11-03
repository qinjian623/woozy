#!/usr/bin/env python3
"""Validate module fixture data, citations and C++ syntax; not name semantics."""
import argparse, json, re, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT / "tests/modules/cases.json"
FIXTURES = ROOT / "tests/modules/fixtures"
ID = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]*$")
def expect(condition, message):
    if not condition:
        raise ValueError(message)

def under(base, value, label):
    expect(isinstance(value, str) and value and not Path(value).is_absolute(),
           f"{label}: expected a relative repository path")
    path = (base / value).resolve()
    expect(path.is_relative_to(ROOT), f"{label}: path leaves the repository")
    return path

def section(document, title, label):
    match = re.search(rf"(?ms)^#+\s+{re.escape(title)}\s*$\n(.*?)(?=^#+\s|\Z)",
                      document)
    expect(match, f"{label}: section {title!r} not found")
    return match.group(1)

def validate_data(path=CASES, root=ROOT, fixtures=FIXTURES):
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"cases JSON: {error}") from error
    expect(document.get("schema") == "compiler-dev/module-fixtures/1", "unexpected schema")
    expect(document.get("contract") == "compiler-dev/1", "unexpected contract")
    rules = document.get("rules")
    expect(isinstance(rules, dict) and rules, "rules must be a non-empty object")
    for rule_id, rule in rules.items():
        expect(ID.fullmatch(rule_id), f"invalid rule id {rule_id!r}")
        expect(isinstance(rule, dict) and rule.get("expected") in {"accept", "reject"},
               f"rule {rule_id}: expected must be accept or reject")
        source = rule.get("source")
        expect(isinstance(source, dict), f"rule {rule_id}: missing source")
        expect(all(isinstance(source.get(key), str) and source[key]
                   for key in ("path", "section", "clause")),
               f"rule {rule_id}: source needs path, section and clause")
        cited = under(root, source["path"], f"rule {rule_id} source")
        expect(cited.is_file(), f"rule {rule_id}: source file does not exist")
        body = section(cited.read_text(encoding="utf-8"), source["section"],
                       f"rule {rule_id}")
        expect(source["clause"] in body, f"rule {rule_id}: cited clause is missing")
    cases = document.get("cases")
    expect(isinstance(cases, list), "cases must be a list")
    expect(document.get("case_count") == len(cases), "case_count does not match cases")
    ids, manifests, sources = set(), set(), set()
    for case in cases:
        expect(isinstance(case, dict), "every case must be an object")
        case_id = case.get("id")
        expect(isinstance(case_id, str) and ID.fullmatch(case_id), "invalid case id")
        expect(case_id not in ids, f"duplicate case id {case_id!r}")
        ids.add(case_id)
        expect(isinstance(case.get("reference"), str) and case["reference"].strip(),
               f"case {case_id}: missing reference")
        expected = case.get("expected")
        expect(isinstance(expected, dict), f"case {case_id}: missing expected")
        status = expected.get("status")
        expect(status in {"accept", "reject"}, f"case {case_id}: invalid status")
        selected = expected.get("rules")
        expect(isinstance(selected, list) and selected
               and len(selected) == len(set(selected)),
               f"case {case_id}: expected.rules must be a non-empty unique list")
        for rule_id in selected:
            expect(rule_id in rules, f"case {case_id}: unknown rule {rule_id!r}")
            expect(rules[rule_id]["expected"] == status,
                   f"case {case_id}: rule {rule_id} expectation differs from case")
        manifest = under(root, case.get("manifest"), f"case {case_id} manifest")
        expect(manifest.is_file(), f"case {case_id}: manifest does not exist")
        expect(manifest not in manifests, f"case {case_id}: duplicate manifest")
        manifests.add(manifest)
        case_dir = (fixtures / case_id).resolve()
        expect(case_dir.is_dir(), f"case {case_id}: fixture directory does not exist")
        native = sorted(path for path in case_dir.rglob("*.lang") if path.is_file())
        expect(native, f"case {case_id}: fixture directory has no .lang files")
        sources.update(path.resolve() for path in native)
    return cases, sorted(sources)

def check_syntax(langc, sources):
    if langc is None:
        return {"status": "not_run", "reason": "--langc not provided",
                "passed": 0, "total": len(sources), "failures": []}
    failures = []
    for source in sources:
        try:
            run = subprocess.run(
                [str(langc), "check", "--syntax-only", "--format", "json", str(source)],
                capture_output=True, text=True, timeout=20)
            payload = json.loads(run.stdout)
            errors = [item for item in payload.get("diagnostics", [])
                      if item.get("severity") == "error"]
            if run.returncode != 0 or payload.get("status") != "ok" or errors:
                failures.append({"path": str(source.relative_to(ROOT)),
                                 "exit": run.returncode, "status": payload.get("status")})
        except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
            failures.append({"path": str(source.relative_to(ROOT)), "error": str(error)})
    return {"status": "failed" if failures else "passed",
            "passed": len(sources) - len(failures), "total": len(sources),
            "failures": failures}

def self_test():
    base = json.loads(CASES.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="module-fixtures-") as temporary:
        path = Path(temporary) / "cases.json"
        broken = json.loads(json.dumps(base))
        broken["cases"][0]["manifest"] = (
            "tests/modules/fixtures/same-package-mutual-recursion/missing.toml")
        for label in ("missing manifest", "invalid status"):
            if label == "invalid status":
                broken = json.loads(json.dumps(base))
                broken["cases"][0]["expected"]["status"] = "invalid"
            path.write_text(json.dumps(broken), encoding="utf-8")
            try:
                validate_data(path, ROOT, FIXTURES)
            except ValueError:
                continue
            raise AssertionError(f"self-test did not reject {label}")
    print(json.dumps({"status": "passed",
                      "self_test": ["missing manifest", "invalid status"]}))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--langc", help="optional C++ langc executable")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    try:
        cases, sources = validate_data()
        langc = Path(args.langc).expanduser().resolve() if args.langc else None
        if langc:
            expect(langc.is_file(), f"--langc is not a file: {args.langc}")
        syntax = check_syntax(langc, sources)
        print(json.dumps({"status": "failed" if syntax["status"] == "failed" else "passed",
                          "cases": len(cases), "source_files": len(sources),
                          "syntax": syntax}, ensure_ascii=False, indent=2))
        return 1 if syntax["status"] == "failed" else 0
    except (ValueError, OSError, UnicodeError) as error:
        print(json.dumps({"status": "failed", "errors": [str(error)]},
                         ensure_ascii=False, indent=2))
        return 1

if __name__ == "__main__":
    raise SystemExit(main())
