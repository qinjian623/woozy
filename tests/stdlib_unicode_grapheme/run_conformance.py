#!/usr/bin/env python3
"""Run the official Unicode 18.0.0 GraphemeBreakTest through a built program.

The generated program calls only the public std/unicode.boundaries,
grapheme_count and graphemes functions.  Expected boundaries come only from
the official test file; the package-internal scanner is not named by the
fixture.  graphemes output is checked independently against the official
boundary slices and against joining back to the original input.

Usage:
    python3 tests/stdlib_unicode_grapheme/run_conformance.py \
        --compiler /path/to/staged/woozyc --work-dir /tmp/grapheme-conf
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "tests/native/unicode/data/GraphemeBreakTest.txt"
OUT = Path(__file__).resolve().parent / "out"


def parse_cases(path):
    cases = []
    with path.open(encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            text = []
            boundaries = []
            for token in line.split():
                if token == "\u00f7":
                    boundaries.append(len(text))
                elif token == "\u00d7":
                    pass
                else:
                    value = int(token, 16)
                    if not 0 <= value <= 0x10FFFF or 0xD800 <= value <= 0xDFFF:
                        raise SystemExit("invalid scalar on line %d" % number)
                    text.append(chr(value))
            cases.append(("case_%d" % len(cases), "".join(text), boundaries))
    if len(cases) != 853:
        raise SystemExit("expected 853 cases, got %d" % len(cases))
    return cases


def wp(value):
    out = ['"']
    for ch in value:
        cp = ord(ch)
        if ch == '"':
            out.append("\\u{22}")
        elif ch == "\\":
            out.append("\\u{5c}")
        elif 0x20 <= cp < 0x7F:
            out.append(ch)
        else:
            out.append("\\u{%x}" % cp)
    out.append('"')
    return "".join(out)


def source(cases):
    lines = [
        "// Generated from GraphemeBreakTest-18.0.0.txt by run_conformance.py.",
        "// Public std/unicode.boundaries and grapheme_count only.",
        "package app;",
        "",
        'import "std/text" as text;',
        'import "std/unicode" as unicode;',
        'import "std/vec" as vec;',
        "",
        "struct Case { name string; value string; spec string; }",
        "struct Group { cases Vec[Case]; }",
        "",
        "func expected(spec string) Vec[usize] {",
        "    let out = vec.empty[usize]();",
        "    var value usize = 0;",
        "    for var index usize = 0; index < text.len(spec); index += 1 {",
        "        if spec[index] == 0x2c {",
        "            vec.push(out,value);",
        "            value = 0;",
        "        } else {",
        "            value = value*10 + usize(spec[index] - 0x30);",
        "        }",
        "    }",
        "    vec.push(out,value);",
        "    return out;",
        "}",
        "",
        "func same(left Vec[usize], right Vec[usize]) bool {",
        "    if vec.len(left) != vec.len(right) { return false; }",
        "    for var index usize = 0; index < vec.len(left); index += 1 {",
        "        if left[index] != right[index] { return false; }",
        "    }",
        "    return true;",
        "}",
        "",
        "func check(item Case) i32 {",
        "    let want = expected(item.spec);",
        "    let got = unicode.boundaries(item.value);",
        "    if !same(got,want) { return 10; }",
        "    if unicode.grapheme_count(item.value) != vec.len(want)-1 { return 11; }",
        "    let parts = unicode.graphemes(item.value);",
        "    if vec.len(parts) != vec.len(want)-1 { return 12; }",
        "    var joined = \"\";",
        "    for var index usize = 1; index < vec.len(want); index += 1 {",
        "        if parts[index-1] != item.value[want[index-1]:want[index]] { return 13; }",
        "        joined = joined + parts[index-1];",
        "    }",
        "    if joined != item.value { return 14; }",
        "    return 0;",
        "}",
        "",
        "func run(group Group) i32 {",
        "    for var index usize = 0; index < vec.len(group.cases); index += 1 {",
        "        let code = check(group.cases[index]);",
        "        if code != 0 { return i32(1000) + i32(index); }",
        "    }",
        "    return 0;",
        "}",
        "",
        "func main() i32 {",
        "    let cases = vec.make[Case](%d, Case(name: \"\", value: \"\", spec: \"0\"));" % len(cases),
    ]
    for index, (name, value, boundaries) in enumerate(cases):
        spec = ",".join(str(x) for x in boundaries)
        lines.append("    cases[%d] = Case(name: %s, value: %s, spec: %s);"
                     % (index, wp(name), wp(value), wp(spec)))
    lines += [
        "    let group = Group(cases: cases);",
        "    return run(group);",
        "}",
        "",
    ]
    return "\n".join(lines)


def run(argv, env, timeout=1200):
    result = subprocess.run([str(x) for x in argv], env=env, capture_output=True,
                            text=True, timeout=timeout)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--work-dir", "--out", type=Path, required=True)
    parser.add_argument("--python", default=sys.executable)
    args = parser.parse_args()
    if args.work_dir.exists():
        raise SystemExit("work dir already exists: %s" % args.work_dir)
    args.work_dir.mkdir(parents=True)
    cases = parse_cases(DATA)
    src = args.work_dir / "src"
    src.mkdir()
    (src / "main.wz").write_text(source(cases))
    (args.work_dir / "lang.toml").write_text(
        'schema_version = 1\n[package]\nname = "app"\nsource_root = "src"\n'
        'entry = "app"\n[build]\nbackend = "native"\nword_bits = 64\n')
    env = os.environ.copy()
    legacy = ROOT.parent / "toolchains/libxml2-legacy-2.13.9/usr/lib"
    if legacy.is_dir():
        env["LD_LIBRARY_PATH"] = str(legacy) + ":" + env.get("LD_LIBRARY_PATH", "")
    report = {"status": "failed", "cases": len(cases), "commands": []}
    try:
        for backend, opt in (("native", 0), ("native", 2), ("python", None)):
            out = args.work_dir / (backend + (str(opt) if opt is not None else ""))
            argv = [args.compiler, "build", "--project", args.work_dir / "lang.toml",
                    "--backend", backend, "--out-dir", out, "--format", "json"]
            if opt is not None:
                argv += ["--opt", str(opt)]
            built = run(argv, env)
            report["commands"].append({"argv": [str(x) for x in argv],
                                       "exit": built.returncode,
                                       "stdout": built.stdout, "stderr": built.stderr})
            if built.returncode != 0:
                raise SystemExit("build failed: %s" % built.stderr)
            if backend == "native":
                program = [out / "program"]
            else:
                program = [args.python, "-B", "-s", out / "program.py"]
            executed = run(program, env, timeout=3600)
            report["commands"].append({"argv": [str(x) for x in program],
                                       "exit": executed.returncode,
                                       "stdout": executed.stdout, "stderr": executed.stderr})
            if executed.returncode != 0:
                report["failed_case"] = executed.returncode - 1000
                raise SystemExit("run failed with exit %d (case %s)\n%s\n%s" % (
                    executed.returncode, executed.returncode - 1000,
                    executed.stdout, executed.stderr))
        report["status"] = "passed"
    finally:
        (args.work_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    (args.work_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("passed: %d official cases on Native O0, Native O2 and Python" % len(cases))


if __name__ == "__main__":
    raise SystemExit(main())
