#!/usr/bin/env python3
"""Build and execute the std/hash, std/map, std/set and std/json test programs.

Every program is compiled with the native backend at -O0 and -O2 and (because
the libraries are pure source) with the Python backend under the requested
Python 3.12 interpreter.  The JSON fixture is additionally cross-checked
against Python's json module with number tokens preserved verbatim.

All build products and the generated JSON oracle source live under --out; the
tracked fixture trees are only read and their hashes are verified unchanged.
Every subprocess has a finite timeout.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

PROGRAM_TIMEOUT = 20
BUILD_TIMEOUT = 180
FIXTURE_ROOT = "tests/native/stdlib-collections"
ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TOOLCHAINS = Path(
    os.environ.get("LANG_TOOLCHAINS")
    or os.environ.get("WOOZY_TOOLCHAIN_ROOT")
    or ROOT.parent / "toolchains")


class NumberToken(str):
    """A JSON number kept as its original decimal text."""


def reject_constant(text):
    raise ValueError("non-RFC constant: " + text)


def canonical(value):
    """Serialize parsed JSON exactly like the Woozy writer (numbers verbatim)."""
    if isinstance(value, NumberToken):
        return str(value)
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, list):
        return "[" + ",".join(canonical(item) for item in value) + "]"
    if isinstance(value, dict):
        return "{" + ",".join(
            json.dumps(name, ensure_ascii=False) + ":" + canonical(item)
            for name, item in value.items()) + "}"
    raise TypeError(type(value))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_hash(root):
    return {str(path.relative_to(root)): digest(path)
            for path in sorted(root.rglob("*")) if path.is_file()}


def wyozy_literal(text):
    """Encode text as an interpreted Woozy string literal."""
    parts = ['"']
    for char in text:
        code = ord(char)
        if char == "\\":
            parts.append("\\\\")
        elif char == '"':
            parts.append('\\"')
        elif char == "\n":
            parts.append("\\n")
        elif char == "\r":
            parts.append("\\r")
        elif char == "\t":
            parts.append("\\t")
        elif code == 0:
            parts.append("\\0")
        elif code < 0x20 or code == 0x7f:
            parts.append("\\u{%X}" % code)
        else:
            parts.append(char)
    parts.append('"')
    return "".join(parts)


def command(argv, env, cwd=None, timeout=PROGRAM_TIMEOUT):
    args = [str(item) for item in argv]
    try:
        result = subprocess.run(args, capture_output=True, text=True,
                                timeout=timeout, env=env, cwd=cwd)
        return dict(argv=args, status="exited", exit=result.returncode,
                    stdout=result.stdout, stderr=result.stderr)
    except subprocess.TimeoutExpired:
        return dict(argv=args, status="timeout", exit=None, stdout="", stderr="")
    except OSError as error:
        return dict(argv=args, status="launch_failed", exit=None,
                    stdout="", stderr=str(error))


def build(cli, manifest, backend, opt, out, env):
    argv = [cli, "build", "--project", manifest, "--backend", backend]
    if backend == "native":
        argv += ["--opt", str(opt)]
    argv += ["--out-dir", out, "--format", "json"]
    result = command(argv, env, timeout=BUILD_TIMEOUT)
    result["ok"] = False
    if result["status"] == "exited" and result["exit"] == 0 and not result["stderr"]:
        try:
            response = json.loads(result["stdout"])
            result["ok"] = response.get("status") == "ok"
            result["response"] = response
        except json.JSONDecodeError:
            result["response"] = None
    return result


def execute(backend, out, python, env):
    if backend == "native":
        return command([out / "program"], env)
    return command([python, "program.py"], env, cwd=out)


LEGAL = [
    "null",
    "true",
    "false",
    "0",
    "-0",
    "1",
    "-1",
    "1.5",
    "2.50",
    "1e10",
    "1E+10",
    "1e-10",
    "12345678901234567890123456789012345678901234567890",
    "0.000000000000000000000000000000000001",
    "1e400",
    "-1.7976931348623157e308",
    '""',
    '"abc"',
    '"\\""',
    '"\\\\"',
    '"\\/"',
    '"\\b\\f\\n\\r\\t"',
    '"\\u0000\\u001f"',
    '"hé😀"',
    '"\\uD83D\\uDE00"',
    '"\\u0041\\u00e9\\u20ac\\uD834\\uDD1E"',
    '"\\u007f"',
    '"\\u2028"',
    "[]",
    "[1,2,3]",
    "[[[]]]",
    "{}",
    '{"a":1}',
    '{"b":1,"a":2}',
    '{"a":1,"b":2,"a":3}',
    '["a",null,true,false,{},[]]',
    ' \t\r\n { "a" : [ 1 , 2 , {"x":"y"} ] } \n',
    '"line\\nnext"',
]

ILLEGAL = [
    ("", "syntax"),
    (" ", "syntax"),
    ("+", "syntax"),
    ("+1", "syntax"),
    (".5", "syntax"),
    ("01", "syntax:1"),
    ("1.", "syntax"),
    ("1e", "syntax"),
    ("1e+", "syntax"),
    ("-", "syntax"),
    ("--1", "syntax"),
    ("NaN", "syntax"),
    ("Infinity", "syntax"),
    ("-Infinity", "syntax"),
    ("tru", "syntax"),
    ("nullx", "syntax"),
    ("[]x", "syntax"),
    ("[1,]", "syntax:3"),
    ("[,1]", "syntax"),
    ("[1 2]", "syntax"),
    ('{"a":}', "syntax"),
    ('{"a" 1}', "syntax"),
    ('{"a":1,}', "syntax"),
    ('{"a":1 "b":2}', "syntax"),
    ('"unterminated', "syntax"),
    ('"\\x01"', "syntax"),
    ('"\\uZZZZ"', "syntax"),
    ('"\\uD800"', "syntax:7", True),
    ('"\\uDC00"', "syntax:7", True),
    ('"\\uD800\\u0041"', "syntax:13", True),
    ('"a\nb"', "syntax:2"),
    ('["😀", x]', "syntax:6"),
    ("\ufeff1", "syntax"),
    ("[" * 257 + "]" * 257, "depth", True),
]


def make_oracle(out, python, env, cli):
    source_dir = out / "json-oracle-src"
    build_dir = out / "json-oracle"
    for path in (source_dir, build_dir):
        if path.exists():
            shutil.rmtree(path)
    (source_dir / "src").mkdir(parents=True)
    (source_dir / "lang.toml").write_text(
        'schema_version = 1\n[package]\nname = "app"\nsource_root = "src"\n'
        'entry = "app"\n[build]\nbackend = "native"\nword_bits = 64\n')

    expected = []
    for source in LEGAL:
        parsed = json.loads(source, parse_int=NumberToken,
                            parse_float=NumberToken,
                            parse_constant=reject_constant)
        expected.append("OK|" + canonical(parsed))

    oracle_rejections = []
    for item in ILLEGAL:
        source, want = item[0], item[1]
        python_accepts = len(item) > 2 and item[2]
        if not python_accepts:
            try:
                json.loads(source, parse_int=NumberToken,
                           parse_float=NumberToken,
                           parse_constant=reject_constant)
            except (ValueError, UnicodeError):
                pass
            else:
                oracle_rejections.append(source[:40])
        expected.append("ERR|" + want)

    sources = LEGAL + [item[0] for item in ILLEGAL]
    lines = [
        "package app;",
        'import "std/json" as json;',
        'import "std/strconv" as strconv;',
        'import "std/text" as text;',
        'import "std/vec" as vec;',
        "func error_name(value json.Error) string {",
        "    switch value {",
        '        case json.Error.Syntax(offset): return "syntax:" + strconv.format_u64(u64(offset));',
        '        case json.Error.DepthLimit: return "depth";',
        '        case json.Error.InvalidNumber: return "invalid";',
        '        case json.Error.NumberRange: return "range";',
        "    }",
        "}",
        "func run_case(source string) string {",
        "    try {",
        "        let value = json.parse(source);",
        "        try { return \"OK|\" + json.serialize(value); }",
        '        catch error json.Error { return "ERR|ser:" + error_name(error); }',
        "    } catch error json.Error {",
        '        return "ERR|" + error_name(error);',
        "    }",
        "}",
        "func matches(actual string, expected string) bool {",
        '    if expected == "ERR|syntax" { return text.starts_with(actual, expected); }',
        "    return actual == expected;",
        "}",
        "func main() i32 {",
        "    let sources = [" + ",".join(wyozy_literal(item) for item in sources) + "];",
        "    let expected = [" + ",".join(wyozy_literal(item) for item in expected) + "];",
        "    if vec.len(sources) != vec.len(expected) { return 120; }",
        "    for var index usize = 0; index < vec.len(sources); index += 1 {",
        "        if !matches(run_case(sources[index]), expected[index]) {",
        "            return i32(index) + 1;",
        "        }",
        "    }",
        "    return 0;",
        "}",
    ]
    (source_dir / "src/main.wz").write_text("\n".join(lines) + "\n")

    manifest = source_dir / "lang.toml"
    runs = []
    for backend, opt in (("native", 0), ("native", 2), ("python", None)):
        name = backend if opt is None else f"native-O{opt}"
        target = build_dir / name
        row = dict(backend=backend, opt=opt, status="failed",
                   python_oracle_rejections=oracle_rejections)
        runs.append(row)
        row["build"] = result = build(cli, manifest, backend, opt, target, env)
        if not result["ok"]:
            continue
        row["execute"] = result = execute(backend, target, python, env)
        if result["status"] != "exited" or result["exit"] != 0:
            code = result["exit"]
            if isinstance(code, int) and 0 < code <= len(sources):
                row["first_mismatch"] = dict(
                    index=code - 1, source=sources[code - 1],
                    expected=expected[code - 1])
            continue
        row["status"] = "passed"
    return dict(status="passed" if all(r["status"] == "passed" for r in runs)
                else "failed", legal=len(LEGAL), illegal=len(ILLEGAL),
                oracle_rejections=oracle_rejections, runs=runs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--python", default="python3.12")
    parser.add_argument("--toolchains", type=Path, default=DEFAULT_TOOLCHAINS)
    args = parser.parse_args()
    root = args.root.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    libxml = args.toolchains / "libxml2-legacy-2.13.9/usr/lib"
    if libxml.is_dir():
        env["LD_LIBRARY_PATH"] = str(libxml) + ":" + env.get("LD_LIBRARY_PATH", "")
    python = shutil.which(args.python)
    if python is None:
        raise SystemExit("missing Python interpreter: " + args.python)

    report = dict(operation_id="woozy-stdlib-map-json-20261111-v1",
                  status="failed", python=python, fixtures=[], oracle=None,
                  source_trees_unchanged=False)
    failures = []
    try:
        manifests = sorted((root / FIXTURE_ROOT).glob("*/lang.toml"))
        if not manifests:
            raise AssertionError("no stdlib collection fixtures found")
        for manifest in manifests:
            fixture = manifest.parent
            before = tree_hash(fixture)
            for backend, opt in (("native", 0), ("native", 2), ("python", None)):
                name = backend if opt is None else f"native-O{opt}"
                target = out / fixture.name / name
                if target.exists():
                    shutil.rmtree(target)
                row = dict(id=fixture.name, backend=backend, opt=opt, status="failed")
                report["fixtures"].append(row)
                row["build"] = result = build(args.cli.resolve(), manifest,
                                              backend, opt, target, env)
                if not result["ok"]:
                    failures.append(dict(id=fixture.name, backend=name,
                                         stage="build", result=result))
                    continue
                row["execute"] = result = execute(backend, target, python, env)
                if (result["status"] != "exited" or result["exit"] != 0
                        or result["stdout"] or result["stderr"]):
                    failures.append(dict(id=fixture.name, backend=name,
                                         stage="execute", result=result))
                    continue
                row["status"] = "passed"
            if tree_hash(fixture) != before:
                failures.append(dict(id=fixture.name, stage="source-mutated"))
        report["source_trees_unchanged"] = not any(
            item.get("stage") == "source-mutated" for item in failures)

        report["oracle"] = oracle = make_oracle(out, python, env, args.cli.resolve())
        if oracle["status"] != "passed":
            for run in oracle["runs"]:
                if run["status"] != "passed":
                    failures.append(dict(id="json-oracle",
                                         backend=run["backend"], opt=run["opt"],
                                         stage="oracle", row=run))
        report["status"] = "passed" if not failures else "failed"
    finally:
        report["failures"] = failures
        path = out / "collections-results.json"
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print(json.dumps(dict(status=report["status"],
                              fixtures=len(report["fixtures"]),
                              passed=sum(1 for r in report["fixtures"]
                                         if r["status"] == "passed"),
                              oracle=(report["oracle"] or {}).get("status"),
                              failures=len(failures), report=str(path)),
                         ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
