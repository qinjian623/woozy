#!/usr/bin/env python3
"""Run json-c-compatible app/wzcc/jsondoc tests at O0 and O2.

Acceptance comes from a test-only C++ program linked against the same json-c
0.19 strict tokenizer used by project.cpp. A separate Python walker, used only
after that oracle accepts the syntax, produces the reference postorder spans
and std/json.Value tree (numbers keep their raw token). Python's strict JSON
parser is never used to decide quirk acceptance.

The earlier 78-case suite was not a valid json-c oracle: it treated a leading
BOM as valid, reported a one-byte trailing span for strict-parser failures, and
rejected json-c number/literal quirks. Those expectations were wrong and are
replaced here rather than used to justify a different grammar.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DRIVER = HERE / "src" / "main.wz"
MANIFEST = HERE / "lang.toml"
ORACLE = HERE / "oracle.cpp"
DEFAULT_STAGE0 = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
TOOLCHAIN = ROOT.parent / "toolchains"
DEFAULT_CLANG = TOOLCHAIN / "llvm-22.1.8" / "bin" / "clang++"
DEFAULT_WORK = Path("/tmp/woozy-jsondoc-tests")
DEFAULT_TIMEOUT = 300.0
TOOLCHAIN_LIBRARY_PATH = ":".join((
    str(TOOLCHAIN / "llvm-22.1.8" / "lib"),
    str(TOOLCHAIN / "libxml2-legacy-2.13.9" / "usr" / "lib"),
))
SNAPSHOT_ID = 7


class RefError(Exception):
    def __init__(self, message: str, begin: int, end: int):
        super().__init__(message)
        self.message = message
        self.begin = begin
        self.end = end


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def environment() -> dict[str, str]:
    env = dict(os.environ)
    previous = env.get("LD_LIBRARY_PATH")
    env["LD_LIBRARY_PATH"] = (
        TOOLCHAIN_LIBRARY_PATH + (":" + previous if previous else "")
    )
    return env


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check, build and run the Wz jsondoc tests.")
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0))
    parser.add_argument("--clang", default=str(DEFAULT_CLANG))
    parser.add_argument("--work-dir", default=str(DEFAULT_WORK))
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    return parser.parse_args(argv)


def command(argv: list[str], label: str, work: Path,
            timeout: float) -> subprocess.CompletedProcess[bytes]:
    print("+", " ".join(argv), flush=True)
    try:
        return subprocess.run(
            argv, cwd=work, env=environment(), capture_output=True,
            check=False, timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"{label}: timed out after {timeout:g}s")
    except OSError as error:
        fail(f"{label}: {error}")
    raise AssertionError("unreachable")


def stage_project(work: Path) -> Path:
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    source = project / "src"
    source.mkdir(parents=True)
    shutil.copy2(DRIVER, source / "main.wz")
    (source / "wzcc").symlink_to(ROOT / "selfhost/src/wzcc")
    shutil.copy2(MANIFEST, project / "lang.toml")
    return project


def snapshot_oracle(raw: bytes) -> tuple[bytes, list[int]]:
    """Reproduce SourceSnapshot BOM removal, CRLF folding and offsets."""
    start = 3 if raw.startswith(b"\xef\xbb\xbf") else 0
    normalized = bytearray()
    offsets = [start]
    index = start
    while index < len(raw):
        byte = raw[index]
        if byte < 0x80:
            width = 1
        elif 0xC2 <= byte <= 0xDF:
            width = 2
        elif 0xE0 <= byte <= 0xEF:
            width = 3
        elif 0xF0 <= byte <= 0xF4:
            width = 4
        else:
            raise AssertionError(f"invalid test UTF-8 lead {byte:#x}")
        if width == 1 and byte == 13:
            if index + 1 >= len(raw) or raw[index + 1] != 10:
                raise AssertionError("standalone CR in test input")
            normalized.append(10)
            offsets.append(index + 2)
            index += 2
            continue
        for part in range(width):
            normalized.append(raw[index + part])
            offsets.append(index + part + 1)
        index += width
    return bytes(normalized), offsets


def repair_surrogates(value: str) -> str:
    out: list[str] = []
    index = 0
    while index < len(value):
        code = ord(value[index])
        if 0xD800 <= code <= 0xDBFF and index + 1 < len(value):
            low = ord(value[index + 1])
            if 0xDC00 <= low <= 0xDFFF:
                out.append(chr(0x10000 + ((code - 0xD800) << 10) +
                               (low - 0xDC00)))
                index += 2
                continue
        if 0xD800 <= code <= 0xDFFF:
            out.append("\ufffd")
        else:
            out.append(value[index])
        index += 1
    return "".join(out)


def decode_string_token(raw: bytes) -> str:
    return repair_surrogates(json.loads(raw.decode("utf-8")))


def pointer_escape(key: str) -> str:
    return "".join(
        "~0" if ch == "~" else "~1" if ch == "/" else ch
        for ch in key
    )


def reference_walk(raw: bytes):
    """Index accepted json-c syntax and build the tagged std/json.Value tree."""
    size = len(raw)
    cursor = 0
    spans: list[tuple[str, int, int]] = []

    def space() -> None:
        nonlocal cursor
        while cursor < size and raw[cursor] in b" \t\r\n":
            cursor += 1

    def string_end() -> None:
        nonlocal cursor
        require(cursor < size and raw[cursor] == 34,
                "reference: expected string")
        cursor += 1
        while raw[cursor] != 34:
            if raw[cursor] == 92:
                cursor += 1
            cursor += 1
        cursor += 1

    def visit(path: str):
        nonlocal cursor
        space()
        begin = cursor
        lead = raw[cursor]
        if lead == 123:
            cursor += 1
            space()
            keys: set[str] = set()
            members = []
            while raw[cursor] != 125:
                key_begin = cursor
                string_end()
                key_end = cursor
                name = decode_string_token(raw[key_begin:key_end])
                if "\x00" in name:
                    raise RefError("JSON field name contains NUL",
                                   key_begin, key_end)
                if name in keys:
                    raise RefError("duplicate JSON field: " + name,
                                   key_begin, key_end)
                keys.add(name)
                space()
                require(raw[cursor] == 58, "reference: expected colon")
                cursor += 1
                child = visit(path + "/" + pointer_escape(name))
                members.append({"name": name, "value": child})
                space()
                if raw[cursor] != 44:
                    break
                cursor += 1
                space()
            cursor += 1
            value = {"k": "object", "v": members}
        elif lead == 91:
            cursor += 1
            space()
            items = []
            index = 0
            while raw[cursor] != 93:
                items.append(visit(path + "/" + str(index)))
                index += 1
                space()
                if raw[cursor] != 44:
                    break
                cursor += 1
                space()
            cursor += 1
            value = {"k": "array", "v": items}
        elif lead == 34:
            string_end()
            value = {"k": "string",
                     "v": decode_string_token(raw[begin:cursor])}
        else:
            while cursor < size and raw[cursor] not in b",]} \t\r\n":
                cursor += 1
            token = raw[begin:cursor]
            if token == b"true":
                value = {"k": "bool", "v": True}
            elif token == b"false":
                value = {"k": "bool", "v": False}
            elif token == b"null":
                value = {"k": "null"}
            else:
                value = {"k": "number", "v": token.decode("ascii")}
        spans.append((path, begin, cursor))
        return value

    root = visit("")
    space()
    require(cursor == size, "reference: accepted input has trailing bytes")
    return root, spans


def oracle_report(binary: Path, raw: bytes) -> dict:
    result = subprocess.run([str(binary)], input=raw, capture_output=True,
                            timeout=60)
    if result.returncode != 0:
        fail(f"json-c oracle exited {result.returncode}: "
             f"{result.stderr.decode('utf-8', 'replace')}")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        fail(f"json-c oracle output is not JSON: {error}\n{result.stdout!r}")


def oracle_accepts(report: dict) -> bool:
    return (report["parse_error"] == "success" and report["nonnull"] and
            report["classification"] == "ok")


def run_case(binary: Path, oracle: Path, raw: bytes, name: str, opt: str) -> None:
    actual = oracle_report(oracle, raw)
    result = subprocess.run([str(binary)], input=raw, capture_output=True,
                            timeout=60)
    if result.returncode not in (0, 2):
        fail(f"{name} O{opt}: exit {result.returncode}\n"
             f"stderr:\n{result.stderr.decode('utf-8', 'replace')}")
    try:
        report = json.loads(result.stdout)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"{name} O{opt}: bad driver report: {error}\n{result.stdout!r}")

    normalized, _ = snapshot_oracle(raw)
    require(report["original"] == raw.hex(),
            f"{name} O{opt}: original bytes changed")
    require(report["normalized"] == normalized.hex(),
            f"{name} O{opt}: normalized bytes changed")

    if not oracle_accepts(actual):
        require(result.returncode == 2 and report["status"] == "error",
                f"{name} O{opt}: json-c rejects but module accepted: {report}")
        require(report["message"] == "invalid JSON binding bundle",
                f"{name} O{opt}: message {report['message']!r}")
        require((report["begin"], report["end"]) == (0, len(raw)),
                f"{name} O{opt}: span [{report['begin']},{report['end']}), "
                f"want [0,{len(raw)})")
        return

    try:
        root, spans = reference_walk(raw)
    except RefError as error:
        require(result.returncode == 2 and report["status"] == "error",
                f"{name} O{opt}: expected index error, got {report}")
        require(report["message"] == error.message,
                f"{name} O{opt}: message {report['message']!r}, "
                f"want {error.message!r}")
        require((report["begin"], report["end"]) == (error.begin, error.end),
                f"{name} O{opt}: span "
                f"[{report['begin']},{report['end']}), "
                f"want [{error.begin},{error.end})")
        return

    require(result.returncode == 0 and report["status"] == "ok",
            f"{name} O{opt}: json-c accepts but module rejected: {report}")
    require(report["root"] == root,
            f"{name} O{opt}: root mismatch\nwant {root!r}\ngot  {report['root']!r}")
    actual_spans = [
        (item["pointer"], item["begin"], item["end"], item["snapshot"])
        for item in report["spans"]
    ]
    require(actual_spans ==
            [(path, begin, end, SNAPSHOT_ID)
             for path, begin, end in spans],
            f"{name} O{opt}: span mismatch\n"
            f"want {spans}\ngot  {actual_spans}")


CASES: list[tuple[str, bytes]] = [
    ("object", b"{}"),
    ("array", b"[]"),
    ("root-string-eof", b'"x"'),
    ("root-string-ws", b'"x" '),
    ("root-array-eof", b"[1,2]"),
    ("root-object-eof", b'{"a":1}'),
    ("crlf-unicode", b'{\r\n"\xe6\xb1\x89\xf0\x9f\x98\x80": [1,2]\r\n}\r\n'),
    ("leading-whitespace", b' \t\r\n{"a": 1}\r\n'),
    ("bom-object", b'\xef\xbb\xbf{}'),
    ("bom-crlf-object", b'\xef\xbb\xbf{\r\n}\r\n'),
    ("bom-duplicate", b'\xef\xbb\xbf{"a":1,"a":2}'),
    ("empty", b""),
    ("whitespace-only", b" \t\r\n"),
    ("ws-space", b" {} "),
    ("ws-tab", b"\t{}\t"),
    ("ws-crlf", b"\r\n{}\r\n"),
    ("ws-lf", b"\n{}\n"),
    ("ws-vtab", b"\v{}"),
    ("ws-formfeed", b"\f{}"),

    ("trailing-object", b"{}x"),
    ("trailing-spaced", b"{} \r\n x"),
    ("trailing-array", b"[]x"),
    ("trailing-string", b'"x"x'),
    ("trailing-number", b"1x"),
    ("trailing-number-space", b"1 x"),
    ("root-number-eof", b"1"),
    ("root-number-ws", b"1\n"),
    ("root-true-eof", b"true"),
    ("root-true-ws", b"true "),
    ("root-false-eof", b"false"),
    ("root-false-ws", b"false\n"),
    ("root-null-eof", b"null"),
    ("root-null-ws", b"null "),
    ("root-null-leading", b" \t null \n"),
    ("root-nan-eof", b"NaN"),
    ("root-nan-ws", b"NaN "),
    ("root-infinity-eof", b"Infinity"),
    ("root-infinity-ws", b"Infinity\n"),
    ("root-neg-infinity-eof", b"-Infinity"),
    ("root-neg-infinity-ws", b"-Infinity "),
    ("root-decimal-eof", b"1."),
    ("root-decimal-ws", b"1. "),
    ("root-exponent-eof", b"1e2"),
    ("root-exponent-ws", b"1e2 "),

    ("object-int", b'{"x":0}'),
    ("integer-i64-min", b'{"x":-9223372036854775808}\n'),
    ("integer-i64-max", b'{"x":9223372036854775807}\n'),
    ("integer-u64-upper-half", b'{"x":9223372036854775808}\n'),
    ("integer-u64-max", b'{"x":18446744073709551615}\n'),
    ("integer-u64-overflow", b'{"x":18446744073709551616}\n'),
    ("integer-negative-overflow", b'{"x":-9223372036854775809}\n'),
    ("integer-unused-field-overflow", b'{"schema_version":1,"unused":1267650600228229401496703205376}\n'),
    ("integer-negative-leading-zeros", b'{"x":-0009223372036854775808}'),
    ("integer-leading-zeros-overflow", b'{"x":-0009223372036854775809}'),
    ("large-float-fraction", b'{"x":18446744073709551616.0}'),
    ("large-float-exponent", b'{"x":18446744073709551616e0}'),
    ("integer-overflow-before-duplicate-check", b'{"x":1,"x":18446744073709551616}'),
    ("integer-overflow-before-nul-key-check", b'{"\\u0000":0,"x":18446744073709551616}'),
    ("root-integer-overflow-terminated", b'18446744073709551616\n'),
    ("object-leading-zero-ok", b'{"x":00}'),
    ("object-leading-zero-bad", b'{"x":01}'),
    ("object-leading-zero-neg", b'{"x":-01}'),
    ("object-leading-zero-more-bad", b'{"x":010}'),
    ("object-fraction-trailing-dot", b'{"x":1.}'),
    ("object-fraction-exp", b'{"x":1.e2}'),
    ("object-fraction-exp-sign", b'{"x":1.e+2}'),
    ("object-neg-trailing-dot", b'{"x":-1.}'),
    ("object-leading-zero-fraction", b'{"x":00.5}'),
    ("object-exponent", b'{"x":1e2}'),
    ("object-upper-exponent", b'{"x":1E+2}'),
    ("object-minus-fraction-no-int", b'{"x":-.5}'),
    ("object-bad-plus", b'{"x":+1}'),
    ("object-bad-hex", b'{"x":0x10}'),
    ("object-bad-dot-start", b'{"x":.5}'),
    ("object-bad-exp-empty", b'{"x":1e}'),
    ("object-bad-exp-sign-empty", b'{"x":1e+}'),
    ("object-bad-neg-hex", b'{"x":-0x1}'),
    ("object-bad-double-dot", b'{"x":1.2.3}'),
    ("object-bad-double-exp", b'{"x":1e2e3}'),

    ("object-nan", b'{"x":NaN}'),
    ("object-infinity", b'{"x":Infinity}'),
    ("object-neg-infinity", b'{"x":-Infinity}'),
    ("object-nan-lower", b'{"x":nan}'),
    ("object-nan-upper", b'{"x":NAN}'),
    ("object-inf-lower", b'{"x":inf}'),
    ("object-plus-inf", b'{"x":+Infinity}'),
    ("object-minus-nan", b'{"x":-NaN}'),
    ("object-plus-nan", b'{"x":+NaN}'),

    ("escaped-nul-value", b'{"a":"\\u0000"}'),
    ("lone-high-value", b'{"x":"\\ud800"}'),
    ("lone-low-value", b'{"x":"\\udc00"}'),
    ("high-normal", b'{"x":"\\ud800a"}'),
    ("two-high", b'{"x":"\\ud800\\ud800"}'),
    ("low-high", b'{"x":"\\udc00\\ud800"}'),
    ("valid-pair", b'{"x":"\\ud83d\\ude00"}'),
    ("pair-and-literal", b'{"x":"\\ud83d\\ude00","y":"\xf0\x9f\x98\x80"}'),
    ("bad-escape", b'{"x":"\\q"}'),
    ("bad-unicode-short", b'{"x":"\\u12"}'),
    ("bad-unicode-hex", b'{"x":"\\uXY00"}'),
    ("raw-control", b'{"x":"\x01"}'),
    ("raw-tab", b'{"x":"\t"}'),
    ("escaped-del", b'{"x":"\\u007f"}'),
    ("escaped-slash", b'{"x":"\\/"}'),

    ("duplicate-decoded", b'{"a":1,"\\u0061":2}'),
    ("duplicate-crlf", b'{\r\n"a":1,\r\n"\\u0061":2}'),
    ("duplicate-surrogate-replacement", b'{"\\ud800":1,"\\ufffd":2}'),
    ("duplicate-emoji",
     b'{"\\ud83d\\ude00":1,"\xf0\x9f\x98\x80":2}'),
    ("nul-key", b'{"a\\u0000":1}'),
    ("nul-key-second", b'{"ok":1,"x\\u0000y":2}'),
    ("duplicate-then-syntax", b'{"a":1,"a":2'),
    ("duplicate-then-bad-value", b'{"a":1,"a":}'),
    ("missing-value", b'{"a":}'),
    ("trailing-comma-object", b'{"a":1,}'),
    ("trailing-comma-array", b'[1,]'),
    ("missing-comma", b'[1 2]'),
    ("missing-colon", b'{"a" 1}'),
    ("bare-key", b'{a:1}'),
]

for _depth in (31, 32, 33, 40, 1000, 10000):
    CASES.append((f"depth-{_depth}-scalar",
                  b"[" * _depth + b"0" + b"]" * _depth))
    CASES.append((f"depth-{_depth}-empty",
                  b"[" * _depth + b"]" * _depth))
for _total in (32, 33):
    CASES.append((f"depth-{_total}-objects-empty",
                  b'{"a":' * (_total - 1) + b"{}" + b"}" * (_total - 1)))
    CASES.append((f"depth-{_total}-arrays-nested-empty",
                  b"[" * (_total - 1) + b"[]" + b"]" * (_total - 1)))

NOT_RUN: list[tuple[str, bytes, str]] = [
    ("raw-nul-source", b"{}\x00", "model.Source rejects raw NUL before jsondoc"),
    ("lone-cr-source", b"{}\r", "model.Source rejects standalone CR before jsondoc"),
    ("invalid-utf8-source", b'{"x":"\xff"}',
     "model.Source rejects invalid UTF-8 before jsondoc"),
]


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = Path(args.woozyc).expanduser().resolve()
    clang = Path(args.clang).expanduser()
    work = Path(args.work_dir).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    if not clang.is_file():
        fail(f"clang++ missing: {clang}")
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")
    work.mkdir(parents=True, exist_ok=True)
    print(f"stage0: {stage0}")
    print(f"clang++: {clang}")
    print(f"work-dir: {work}")

    oracle = work / "jsonc-oracle"
    built_oracle = command(
        [str(clang), "-std=c++20", "-O2", str(ORACLE),
         "-I/usr/include/json-c", "-ljson-c", "-o", str(oracle)],
        "oracle build", work, args.timeout)
    if built_oracle.returncode != 0:
        fail("oracle build failed:\n"
             f"{built_oracle.stdout.decode('utf-8', 'replace')}"
             f"{built_oracle.stderr.decode('utf-8', 'replace')}")
    print("PASS: json-c oracle build")

    project = stage_project(work)
    manifest = str(project / "lang.toml")
    checked = command([str(stage0), "check", "--project", manifest,
                       "--backend", "native"], "check", work, args.timeout)
    if checked.returncode != 0:
        fail("stage0 check failed:\n"
             f"{checked.stdout.decode('utf-8', 'replace')}"
             f"{checked.stderr.decode('utf-8', 'replace')}")
    print("PASS: stage0 check")

    total = 0
    for opt in ("0", "2"):
        out = work / f"opt{opt}"
        if out.exists():
            shutil.rmtree(out)
        built = command(
            [str(stage0), "build", "--project", manifest, "--backend",
             "native", "--out-dir", str(out), "--opt", opt],
            f"build O{opt}", work, args.timeout)
        if built.returncode != 0:
            fail(f"build O{opt} failed:\n"
                 f"{built.stdout.decode('utf-8', 'replace')}"
                 f"{built.stderr.decode('utf-8', 'replace')}")
        binary = out / "program"
        if not binary.is_file():
            fail(f"O{opt}: missing {binary}")
        print(f"PASS: build O{opt}")
        for name, raw in CASES:
            run_case(binary, oracle, raw, name, opt)
            total += 1
            print(f"PASS: {name} O{opt}")
    for name, raw, reason in NOT_RUN:
        print(f"NOT_RUN: {name}: {reason}")
    print(f"PASS: {total}/{total} jsondoc cases at O0 and O2; "
          f"{len(NOT_RUN)} invalid-Source cases not_run")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
