#!/usr/bin/env python3
"""Executable checks for the pure-Wz arbitrary-precision integer module.

The module under test is selfhost/src/wzcc/bigint/bigint.wz.  A stage0
woozyc builds real native programs for every case at O0 and O2, then the
programs are run.  Expected values come exclusively from Python's int
oracle in this file; the Wz programs never import or execute Python.

Typical invocation (the LLVM runtime path is required by this stage0):

  WOOZYC=/path/to/woozyc \
  LD_LIBRARY_PATH=/path/to/llvm/lib:/path/to/libxml2/lib \
  python3 selfhost/tests/bigint/run.py --report /tmp/bigint-report.json

Scale is deliberately bounded: six static cases plus one generated vector
case, each built and run at O0 and O2.  Use --case NAME to narrow the run.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
MODULE_DIR = REPO / "selfhost" / "src" / "wzcc" / "bigint"
CASES_DIR = HERE / "cases"

MANIFEST = """schema_version = 1
[package]
name = "woozyc_bigint_test"
source_root = "src"
entry = "app"
[build]
backend = "native"
word_bits = 64
"""

ALIAS_A_TEXT = "9" * 300
ALIAS_B_TEXT = "123456789" * 17
CORE_BIG_TEXT = "1" + "0" * 50
TEN40_TEXT = "1" + "0" * 40
TEN50_TEXT = "1" + "0" * 50


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def trunc_divrem(left: int, right: int) -> tuple[int, int]:
    """Quotient truncates toward zero; remainder keeps the dividend sign."""
    magnitude = abs(left) // abs(right)
    quotient = -magnitude if (left < 0) != (right < 0) else magnitude
    return quotient, left - quotient * right


def shift_right_magnitude(value: int, bits: int) -> int:
    magnitude = abs(value) >> bits
    return -magnitude if value < 0 else magnitude


def digit_value(character: str) -> int | None:
    code = ord(character)
    if 48 <= code <= 57:
        return code - 48
    if 65 <= code <= 90:
        return code - 65 + 10
    if 97 <= code <= 122:
        return code - 97 + 10
    return None


def expected_parse(label: str, value: str, base: int) -> list[str]:
    lines = [label]
    if base < 2 or base > 36:
        return lines + ["InvalidBase"]
    cursor = 0
    sign = 1
    if value and value[0] in "+-":
        if value[0] == "-":
            sign = -1
        cursor = 1
    if cursor == len(value):
        return lines + ["InvalidDigit", str(cursor)]
    for index in range(cursor, len(value)):
        digit = digit_value(value[index])
        if digit is None or digit >= base:
            return lines + ["InvalidDigit", str(index)]
    return lines + ["ok", str(sign * int(value[cursor:], base))]


def expected_core() -> list[str]:
    a = -12345678901234567890
    return [
        "zero.sign",
        "0",
        "zero.len",
        "0",
        f"i64min={-(2**63)}",
        f"i64max={2**63 - 1}",
        f"u64max={2**64 - 1}",
        "minus-one=-1",
        "one=1",
        "bl.zero",
        "0",
        "bl.one",
        "1",
        "bl.minus-two",
        "2",
        "bl.u64max",
        "64",
        "bl.2p64",
        "65",
        f"base2={-int('101010', 2)}",
        f"base8={int('777', 8)}",
        f"base16={int('deadBEEF', 16)}",
        f"base36={int('zz', 36)}",
        f"base36neg={-int('ZZ', 36)}",
        "pluszero=0",
        "minuszero=0",
        "cmp.ab",
        "-1",
        "cmp.ba",
        "1",
        "cmp.aa",
        "0",
        "cmp.za",
        "1",
        "cmp.zb",
        "-1",
        f"negate.a={-a}",
        f"absolute.a={-a}",
        "negate.zero=0",
        f"absolute.min={2**63}",
        "to.i64min",
        str(-(2**63)),
        "to.i64max",
        str(2**63 - 1),
        "to.u64max",
        str(2**64 - 1),
        "to.zero",
        "0",
        "fmt.leading=123",
        f"fmt.big={int(CORE_BIG_TEXT)}",
    ]


def expected_errors() -> list[str]:
    out: list[str] = []
    parse_cases = [
        ("p.base0", "1", 0),
        ("p.base1", "1", 1),
        ("p.base37", "1", 37),
        ("p.empty", "", 10),
        ("p.minus", "-", 10),
        ("p.plus", "+", 10),
        ("p.embedded", "12x4", 10),
        ("p.space", "12 4", 10),
        ("p.prefix", "0x10", 10),
        ("p.binary2", "2", 2),
        ("p.base35z", "z", 35),
        ("p.underscore", "1_0", 10),
        ("p.unicode", "12é4", 10),
        ("p.valid36", "10", 36),
        ("p.maxdigit36", "z", 36),
        ("p.leadingplus", "+FF", 16),
    ]
    for label, value, base in parse_cases:
        out += expected_parse(label, value, base)

    for label, left, right in [
        ("d.one.zero", 1, 0),
        ("d.zero.zero", 0, 0),
    ]:
        out.append(label)
        if right == 0:
            out.append("DivisionByZero")
        else:
            q, r = trunc_divrem(left, right)
            out += ["ok", str(q), str(r)]

    conversions = [
        ("i64.2p63", 2**63, True),
        ("i64.below-min", -(2**63) - 1, True),
        ("i64.min-ok", -(2**63), True),
        ("i64.max-ok", 2**63 - 1, True),
        ("u64.negative", -1, False),
        ("u64.2p64", 2**64, False),
        ("u64.max-ok", 2**64 - 1, False),
        ("u64.zero", 0, False),
    ]
    for label, value, signed in conversions:
        out.append(label)
        low, high = (-(2**63), 2**63 - 1) if signed else (0, 2**64 - 1)
        out += ["ok", str(value)] if low <= value <= high else ["Overflow"]
    return out


def expected_division() -> list[str]:
    out: list[str] = []

    def report_div(label: str, left: int, right: int) -> None:
        quotient, remainder = trunc_divrem(left, right)
        out.append(f"{label}.q={quotient}")
        out.append(f"{label}.r={remainder}")

    minimum = -(2**63)
    two_96_minus_1 = (1 << 96) - 1
    two_32_plus_1 = (1 << 32) + 1
    two_64_minus_1 = (1 << 64) - 1
    two_61_minus_1 = (1 << 61) - 1
    two_100_minus_1 = (1 << 100) - 1
    for label, left, right in [
        ("d.7.3", 7, 3),
        ("d.n7.3", -7, 3),
        ("d.7.n3", 7, -3),
        ("d.n7.n3", -7, -3),
        ("d.6.3", 6, 3),
        ("d.n6.3", -6, 3),
        ("d.6.n3", 6, -3),
        ("d.n6.n3", -6, -3),
        ("d.1.2", 1, 2),
        ("d.100.100", 100, 100),
        ("d.100.n100", 100, -100),
        ("d.min.n1", minimum, -1),
        ("d.min.2", minimum, 2),
        ("d.2p96m1.2p32p1", two_96_minus_1, two_32_plus_1),
        ("d.n2p96m1.2p32p1", -two_96_minus_1, two_32_plus_1),
    ]:
        report_div(label, left, right)

    out += ["exact.q.sign", "1", "exact.r.sign", "0", "exact.r.len", "0"]

    def report_gcd(label: str, left: int, right: int) -> None:
        out.append(f"{label}={math.gcd(left, right)}")

    report_gcd("g.0.0", 0, 0)
    report_gcd("g.0.42", 0, 42)
    report_gcd("g.n42.30", -42, 30)
    report_gcd("g.2p64m1.2p32m1", two_64_minus_1, (1 << 32) - 1)
    report_gcd("g.2p96m1.2p64m1", two_96_minus_1, two_64_minus_1)
    report_gcd("g.neg.neg", -two_96_minus_1, -two_64_minus_1)
    report_gcd("g.coprime", two_100_minus_1, two_61_minus_1)
    return out


def expected_shifts() -> list[str]:
    out: list[str] = []

    def report(label: str, value: int) -> None:
        out.append(f"{label}={value}")

    one = 1
    u64max = 2**64 - 1
    for label, value, bits in [
        ("z.shl.100", 0, 100),
        ("z.shr.100", 0, 100),
        ("one.shl.0", one, 0),
        ("one.shl.31", one, 31),
        ("one.shl.32", one, 32),
        ("one.shl.33", one, 33),
        ("one.shl.63", one, 63),
        ("one.shl.64", one, 64),
        ("one.shl.65", one, 65),
        ("one.shl.100", one, 100),
        ("one.shl.1000", one, 1000),
        ("n1.shl.64", -1, 64),
        ("u64max.shl.32", u64max, 32),
    ]:
        report(label, value << bits)
    for label, value, bits in [
        ("u64max.shr.0", u64max, 0),
        ("u64max.shr.31", u64max, 31),
        ("u64max.shr.32", u64max, 32),
        ("u64max.shr.63", u64max, 63),
        ("u64max.shr.64", u64max, 64),
        ("u64max.shr.65", u64max, 65),
        ("u64max.shr.100", u64max, 100),
        ("n1.shr.1", -1, 1),
        ("n1.shr.64", -1, 64),
        ("n3.shr.1", -3, 1),
        ("n4.shr.2", -4, 2),
        ("n5.shr.2", -5, 2),
    ]:
        report(label, shift_right_magnitude(value, bits))
    report("one.shr.u64max", shift_right_magnitude(one, 2**64 - 1))
    report("u64max.shr.u64max", shift_right_magnitude(u64max, 2**64 - 1))
    x = int("1234567890123456789012345678901234567890")
    report("x.shl.77", x << 77)
    report("x.roundtrip", x)
    report("negx.shl.65", -x << 65)
    report("negx.shr.65", shift_right_magnitude(-x, 65))
    return out


def expected_arith() -> list[str]:
    out: list[str] = []

    def report(label: str, value: int) -> None:
        out.append(f"{label}={value}")

    one = 1
    u32max = 2**32 - 1
    u64max = 2**64 - 1
    two_64 = 1 << 64
    two_96 = 1 << 96
    report("u64max.p1", u64max + 1)
    report("u32max.p.u32max", u32max + u32max)
    report("u64max.p.u64max", u64max + u64max)
    report("2p64.m1", two_64 - 1)
    report("2p96.m1", two_96 - 1)
    report("2p96.m.2p64", two_96 - two_64)
    report("2p64.m.2p64", 0)
    report("u64max.m.2p64", -1)
    report("neg2p64.p1", -two_64 + 1)
    report("5.p.n3", 5 + -3)
    report("n5.p.3", -5 + 3)
    report("5.m.3", 5 - 3)
    report("3.m.5", 3 - 5)
    report("n5.m.n3", -5 - -3)
    report("n5.p.5", 0)
    out += ["cancel.sign", "0", "cancel.len", "0"]
    report("m.u64max.u64max", u64max * u64max)
    report("m.n7.6", -7 * 6)
    report("m.7.n6", 7 * -6)
    report("m.n7.n6", -7 * -6)
    report("m.zero.big", 0)
    report("m.10p50.10p50", int(TEN50_TEXT) ** 2)
    report("m.2p64m1.2p64p1", u64max * (two_64 + 1))
    return out


def expected_alias() -> list[str]:
    a = int(ALIAS_A_TEXT)
    b = -int(ALIAS_B_TEXT)
    quotient, remainder = trunc_divrem(a, b)
    out = [
        f"sum={a + b}",
        f"diff={a - b}",
        f"product={a * b}",
        f"q={quotient}",
        f"r={remainder}",
        f"scaled={a << 77}",
        f"shifted={shift_right_magnitude(b, 65)}",
        f"gcd={math.gcd(a, b)}",
        f"negated={-a}",
        f"magnitude={abs(b)}",
        f"ordering={1 if a > b else (-1 if a < b else 0)}",
        f"reparsed={a}",
        "alias.a.same",
        "true",
        "alias.b.same",
        "true",
        "alias.aa.same",
        "true",
        "alias.bb.same",
        "true",
        "alias.a.len.same",
        "true",
        "alias.b.len.same",
        "true",
        "alias.a.sign",
        "1",
        "alias.b.sign",
        "-1",
        "snapshot.a",
        str(a),
        "snapshot.b",
        str(b),
    ]
    return out


def random_digits(rng: random.Random, length: int) -> int:
    first = rng.randint(1, 9)
    return int(str(first) + "".join(str(rng.randint(0, 9)) for _ in range(length - 1)))


def vector_pairs() -> list[tuple[str, int, int, int]]:
    rng = random.Random(20261113)
    return [
        ("zero", 0, 1, 0),
        ("one", 1, 1, 1),
        ("mixed", -7, 3, 31),
        ("i64min", -(2**63), 97, 64),
        ("u64max", 2**64 - 1, 2**32 + 1, 65),
        ("2p128", (1 << 128) - 1, (1 << 64) + 3, 33),
        ("d50", random_digits(rng, 50), -random_digits(rng, 49), 100),
        ("d100", -random_digits(rng, 100), random_digits(rng, 99), 63),
        ("d300", random_digits(rng, 300), random_digits(rng, 150), 31),
        ("d1000", 10**1000 - 1, 10**500 + 12345, 1000),
        ("d1000rand", random_digits(rng, 1000), -random_digits(rng, 999), 32),
        ("negbig", -(10**800 + 123), 10**400 + 7, 65),
    ]


def generated_vectors() -> tuple[str, list[str]]:
    source = [
        "package app;",
        'import "app/wzcc/bigint" as bigint;',
        'import "std/io" as io;',
        'import "std/os" as os;',
        'import "std/strconv" as strconv;',
        "",
        "func emit(label string, value bigint.BigInt) throws os.Error {",
        '    io.println(label + "=" + bigint.format_decimal(value));',
        "}",
        "",
        "func main() i32 throws os.Error {",
        "    try {",
    ]
    expected: list[str] = []
    for index, (_, a, b, shift) in enumerate(vector_pairs()):
        quotient, remainder = trunc_divrem(a, b)
        source += [
            f'        let a{index} = bigint.parse_digits("{a}", 10);',
            f'        let b{index} = bigint.parse_digits("{b}", 10);',
            f'        emit("v{index}.a", a{index});',
            f'        emit("v{index}.b", b{index});',
            f'        emit("v{index}.add", bigint.add(a{index}, b{index}));',
            f'        emit("v{index}.sub", bigint.subtract(a{index}, b{index}));',
            f'        emit("v{index}.mul", bigint.multiply(a{index}, b{index}));',
            f"        let qr{index} = bigint.divrem(a{index}, b{index});",
            f'        emit("v{index}.q", qr{index}.quotient);',
            f'        emit("v{index}.r", qr{index}.remainder);',
            f'        emit("v{index}.shl", bigint.shift_left(a{index}, {shift}));',
            f'        emit("v{index}.shr", bigint.shift_right(a{index}, {shift}));',
            f'        io.println("v{index}.bits=" + strconv.format_u64(bigint.bit_length(a{index})));',
            f'        emit("v{index}.gcd", bigint.gcd(a{index}, b{index}));',
        ]
        expected += [
            f"v{index}.a={a}",
            f"v{index}.b={b}",
            f"v{index}.add={a + b}",
            f"v{index}.sub={a - b}",
            f"v{index}.mul={a * b}",
            f"v{index}.q={quotient}",
            f"v{index}.r={remainder}",
            f"v{index}.shl={a << shift}",
            f"v{index}.shr={shift_right_magnitude(a, shift)}",
            f"v{index}.bits={abs(a).bit_length()}",
            f"v{index}.gcd={math.gcd(a, b)}",
        ]
    source += [
        "    } catch error bigint.Error {",
        '        io.println("unexpected-bigint-error");',
        "    }",
        "    return 0;",
        "}",
        "",
    ]
    return "\n".join(source), expected


STATIC_CASES = [
    ("core", "core.wz", expected_core),
    ("errors", "errors.wz", expected_errors),
    ("division", "division.wz", expected_division),
    ("shifts", "shifts.wz", expected_shifts),
    ("arith", "arith.wz", expected_arith),
    ("alias", "alias.wz", expected_alias),
]


def resolve_woozyc(requested: str) -> str | None:
    if os.path.sep in requested:
        return requested if Path(requested).is_file() else None
    found = shutil.which(requested)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--woozyc",
        default=os.environ.get("WOOZYC", "woozyc"),
        help="stage0 compiler executable (default: $WOOZYC or PATH)",
    )
    parser.add_argument(
        "--opt",
        type=int,
        nargs="+",
        default=[0, 2],
        choices=(0, 2),
        help="native optimization levels (default: 0 2)",
    )
    parser.add_argument(
        "--case",
        action="append",
        default=[],
        help="run only the named case(s); repeatable",
    )
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--keep", action="store_true", help="keep the temp project")
    parser.add_argument("--report", help="write a JSON report to this path")
    args = parser.parse_args()

    woozyc = resolve_woozyc(args.woozyc)
    if woozyc is None:
        message = f"not_run: stage0 woozyc not found: {args.woozyc}"
        print(message, file=sys.stderr)
        if args.report:
            Path(args.report).write_text(
                json.dumps(
                    {
                        "schema": "woozy-bigint-tests/1",
                        "status": "not_run",
                        "reason": message,
                    },
                    indent=2,
                )
                + "\n"
            )
        return 2

    version = subprocess.run(
        [woozyc, "--version"], capture_output=True, text=True, timeout=60
    )
    version_text = (version.stdout or version.stderr).strip()

    module_file = MODULE_DIR / "bigint.wz"
    module_text = module_file.read_text(encoding="utf-8")
    forbidden = [token for token in ("std/process", "lang_native_helper", "python") if token in module_text]
    if forbidden:
        print(f"FAIL: module references forbidden capability: {forbidden}", file=sys.stderr)
        return 1

    temp_root = Path(tempfile.mkdtemp(prefix="woozy-bigint-"))
    project = temp_root / "project"
    (project / "src" / "wzcc").mkdir(parents=True)
    shutil.copytree(MODULE_DIR, project / "src" / "wzcc" / "bigint")
    (project / "lang.toml").write_text(MANIFEST, encoding="utf-8")

    generated_source, generated_expected = generated_vectors()
    case_payloads: list[tuple[str, str, list[str], str]] = []
    for name, filename, expected in STATIC_CASES:
        source = (CASES_DIR / filename).read_text(encoding="utf-8")
        case_payloads.append((name, source, expected(), f"cases/{filename}"))
    case_payloads.append(("vectors", generated_source, generated_expected, "generated/vectors.wz"))

    selected = set(args.case)
    if selected:
        known = {name for name, _, _, _ in case_payloads}
        unknown = sorted(selected - known)
        if unknown:
            print(f"FAIL: unknown case(s): {', '.join(unknown)}", file=sys.stderr)
            return 1
        case_payloads = [case for case in case_payloads if case[0] in selected]

    records: list[dict] = []
    failures = 0
    passed = 0
    started = time.time()
    for name, source, expected_lines, origin in case_payloads:
        expected_text = "\n".join(expected_lines) + "\n"
        expected_sha = sha256_text(expected_text)
        source_sha = sha256_text(source)
        (project / "src" / "main.wz").write_text(source, encoding="utf-8")
        for opt in args.opt:
            out_dir = project / f"out-{name}-{opt}"
            if out_dir.exists():
                shutil.rmtree(out_dir)
            build_command = [
                woozyc,
                "build",
                "--project",
                "lang.toml",
                "--backend",
                "native",
                "--out-dir",
                str(out_dir),
                "--opt",
                str(opt),
            ]
            record = {
                "case": name,
                "origin": origin,
                "opt": opt,
                "source_sha256": source_sha,
                "expected_sha256": expected_sha,
                "build_command": build_command,
            }
            build_started = time.time()
            try:
                build = subprocess.run(
                    build_command,
                    cwd=project,
                    capture_output=True,
                    text=True,
                    timeout=args.timeout,
                )
            except subprocess.TimeoutExpired:
                record.update(status="failed", reason="build timed out")
                records.append(record)
                failures += 1
                print(f"[O{opt}] {name}: FAIL build timeout")
                continue
            record["build_seconds"] = round(time.time() - build_started, 3)
            record["build_exit"] = build.returncode
            record["build_stdout"] = build.stdout[-4000:]
            record["build_stderr"] = build.stderr[-4000:]
            if build.returncode != 0:
                record.update(status="failed", reason="build failed")
                records.append(record)
                failures += 1
                print(f"[O{opt}] {name}: FAIL build exit {build.returncode}")
                continue
            program = out_dir / "program"
            if not program.is_file():
                record.update(status="failed", reason="build produced no program")
                records.append(record)
                failures += 1
                print(f"[O{opt}] {name}: FAIL build produced no program")
                continue
            run_started = time.time()
            try:
                executed = subprocess.run(
                    [str(program)],
                    cwd=project,
                    capture_output=True,
                    text=True,
                    timeout=args.timeout,
                )
            except subprocess.TimeoutExpired:
                record.update(status="failed", reason="program timed out")
                records.append(record)
                failures += 1
                print(f"[O{opt}] {name}: FAIL run timeout")
                continue
            record["run_seconds"] = round(time.time() - run_started, 3)
            record["run_exit"] = executed.returncode
            record["actual_sha256"] = sha256_text(executed.stdout)
            record["stdout_match"] = executed.stdout == expected_text
            if executed.returncode != 0:
                record.update(status="failed", reason="program exit", stderr=executed.stderr[-4000:])
                failures += 1
                print(f"[O{opt}] {name}: FAIL run exit {executed.returncode}")
            elif not record["stdout_match"]:
                diff = list(
                    difflib.unified_diff(
                        expected_text.splitlines(),
                        executed.stdout.splitlines(),
                        fromfile="python-oracle",
                        tofile=f"{name}-O{opt}",
                        lineterm="",
                    )
                )
                record.update(status="failed", reason="stdout mismatch", diff=diff[:80])
                failures += 1
                print(f"[O{opt}] {name}: FAIL stdout mismatch")
                for line in diff[:20]:
                    print("    " + line)
            else:
                record.update(status="passed")
                passed += 1
                print(
                    f"[O{opt}] {name}: passed "
                    f"(build {record['build_seconds']}s, run {record['run_seconds']}s)"
                )
            records.append(record)

    elapsed = round(time.time() - started, 3)
    summary = {
        "passed": passed,
        "failed": failures,
        "not_run": 0,
        "optimization_levels": args.opt,
        "elapsed_seconds": elapsed,
    }
    if failures == 0:
        print(
            f"PASS: {passed} real builds+runs "
            f"(O{'/O'.join(str(opt) for opt in args.opt)}); "
            f"Python int oracle; {elapsed}s"
        )
    else:
        print(f"FAIL: {failures} failed, {passed} passed; {elapsed}s")

    if args.report:
        report = {
            "schema": "woozy-bigint-tests/1",
            "operation_id": "woozy-SH05a-bigint-20261113",
            "status": "passed" if failures == 0 else "failed",
            "woozyc": woozyc,
            "woozyc_version": version_text,
            "module": str(module_file.relative_to(REPO)),
            "module_sha256": sha256_text(module_text),
            "python_version": sys.version.split()[0],
            "summary": summary,
            "records": records,
        }
        Path(args.report).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"report: {args.report}")

    if args.keep:
        print(f"kept project: {project}")
    else:
        shutil.rmtree(temp_root, ignore_errors=True)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
