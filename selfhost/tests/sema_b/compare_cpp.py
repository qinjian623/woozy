#!/usr/bin/env python3
"""Compare the Wz case-driver RESULT lines with a C++ stage0 check.

The Wz RESULT lines are produced by `run.py --slim --run`.  This script
extracts the same source strings from `src/main_slim.wz`, writes one real
project per case, runs the accepted C++ seed's public `check --project`, and
compares the first diagnostic code/phase/primary byte range.  Accept cases
must be status=ok.  The test-only Python property fixture is skipped here
because a plain C++ project has no binding; its real Wz binding/marker is
checked by the driver.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CASE_RE = re.compile(
    r'Case\(label: "([^"]+)",\s*'
    r'source: "((?:[^"\\]|\\.)*)",\s*'
    r'ok: (true|false), code: "([^"]*)", property: (true|false)\)',
    re.S)
RESULT_RE = re.compile(
    r"RESULT (\S+) ok=([01]) code=(\S*) phase=(\S*) begin=(\d+) end=(\d+)")
MANIFEST = """schema_version=1
[package]
name="sema_b_cpp_case"
source_root="src"
entry="app"
[build]
backend="native"
word_bits=64
"""


def decode_wz(value: str) -> str:
    out: list[str] = []
    i = 0
    while i < len(value):
        c = value[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        i += 1
        if i >= len(value):
            raise ValueError("trailing backslash")
        e = value[i]
        i += 1
        if e == "n":
            out.append("\n")
        elif e == "r":
            out.append("\r")
        elif e == "t":
            out.append("\t")
        elif e == "0":
            out.append("\0")
        elif e == "\\":
            out.append("\\")
        elif e == '"':
            out.append('"')
        elif e == "u":
            if i >= len(value) or value[i] != "{":
                raise ValueError("bad unicode escape")
            end = value.index("}", i)
            out.append(chr(int(value[i + 1:end], 16)))
            i = end + 1
        else:
            raise ValueError(f"unknown escape \\{e}")
    return "".join(out)


def cases() -> list[dict[str, object]]:
    text = (HERE / "src" / "main_slim.wz").read_text(encoding="utf-8")
    result = []
    for match in CASE_RE.finditer(text):
        result.append({
            "label": match.group(1),
            "source": decode_wz(match.group(2)),
            "ok": match.group(3) == "true",
            "code": match.group(4),
            "property": match.group(5) == "true",
        })
    return result


def wz_results(path: Path) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = RESULT_RE.fullmatch(line)
        if match:
            result[match.group(1)] = {
                "ok": match.group(2) == "1",
                "code": match.group(3),
                "phase": match.group(4),
                "begin": int(match.group(5)),
                "end": int(match.group(6)),
            }
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", required=True,
                        help="accepted C++ fast seed")
    parser.add_argument("--wz-results", required=True,
                        help="stdout from the Wz O0 or O2 driver")
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()
    seed = Path(args.woozyc).resolve()
    work = Path(args.work_dir).resolve()
    work.mkdir(parents=True, exist_ok=True)
    wz = wz_results(Path(args.wz_results).resolve())
    env = dict(os.environ)
    toolchain = ROOT.parent / "toolchains"
    env["LD_LIBRARY_PATH"] = ":".join([
        str(toolchain / "llvm-22.1.8" / "lib"),
        str(toolchain / "libxml2-legacy-2.13.9" / "usr" / "lib"),
        env.get("LD_LIBRARY_PATH", ""),
    ])
    env["TMPDIR"] = str(work / "tmp")
    (work / "tmp").mkdir(exist_ok=True)
    failures = 0
    compared = 0
    skipped = 0
    for case in cases():
        label = str(case["label"])
        if case["property"]:
            if label not in wz or not wz[label]["ok"]:
                print(f"FAIL {label}: Wz property case did not pass")
                failures += 1
            else:
                print(f"SKIP {label}: CPP needs a binding; Wz binding/marker passed")
                skipped += 1
            continue
        if label not in wz:
            print(f"FAIL {label}: missing Wz RESULT")
            failures += 1
            continue
        project = work / "cases" / label
        (project / "src").mkdir(parents=True, exist_ok=True)
        (project / "lang.toml").write_text(MANIFEST, encoding="utf-8")
        (project / "src" / "main.wz").write_text(str(case["source"]),
                                                 encoding="utf-8")
        run = subprocess.run(
            [str(seed), "check", "--project", str(project / "lang.toml"),
             "--backend", "native", "--format", "json"],
            cwd=work, env=env, text=True, capture_output=True,
            timeout=args.timeout)
        (project / "check.stdout").write_text(run.stdout, encoding="utf-8")
        (project / "check.stderr").write_text(run.stderr, encoding="utf-8")
        try:
            report = json.loads(run.stdout)
        except ValueError:
            print(f"FAIL {label}: invalid C++ JSON: {run.stdout!r}")
            failures += 1
            continue
        expected_ok = bool(case["ok"])
        actual = wz[label]
        if expected_ok:
            ok = report.get("status") == "ok" and actual["ok"]
            if not ok:
                print(f"FAIL {label}: accept mismatch Wz={actual} CPP={report.get('status')}")
                failures += 1
            else:
                compared += 1
            continue
        diags = report.get("diagnostics", [])
        if report.get("status") == "ok" or not diags:
            print(f"FAIL {label}: C++ unexpectedly accepted")
            failures += 1
            continue
        diag = diags[0]
        primary = diag.get("primary") or {}
        cpp = {
            "code": diag.get("code"),
            "phase": diag.get("phase"),
            "begin": primary.get("start_byte", primary.get("begin_byte")),
            "end": primary.get("end_byte"),
        }
        same = (actual["code"] == cpp["code"] and
                actual["phase"] == cpp["phase"] and
                actual["begin"] == cpp["begin"] and
                actual["end"] == cpp["end"])
        if not same:
            print(f"FAIL {label}: Wz={actual} CPP={cpp}")
            failures += 1
        else:
            compared += 1
        if len(diags) != 1:
            print(f"NOTE {label}: CPP returned {len(diags)} diagnostics; compared first")
    print(f"CPP_COMPARE compared={compared} skipped={skipped} failed={failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
