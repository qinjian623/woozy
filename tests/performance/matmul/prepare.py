#!/usr/bin/env python3
"""Prepare the fixed scalar dense-f64 matmul Source/C++ fixture without timing.

Both programs use row-major A[N*N] and B[N*N] with the same oracle literals,
zero-initialized C, and the same i-j-k loop order:

    sum = 0.0
    for k in order: sum = sum + A[i*N+k] * B[k*N+j]
    C[i*N+j] = sum

N=1,3,7,16 are prepared.  The fixed A[i][j] = ((17*i+13*j)%19-9)/8 and
B[i][j] = ((11*i+7*j)%23-11)/8 inputs, the complete exact C, and the
binary64 C bit patterns come from the already-frozen independent oracle
``matmul-exact-small-oracle.json`` (SHA-256 recorded below).  This script
verifies that oracle, recomputes every product and partial sum with exact
``Fraction`` arithmetic, and requires every value to be exactly representable
as binary64; the expected values are never read from Source or C++.

Correct programs compare every matrix element and exit 0.  ``wrong`` programs
change the last real C element and keep the oracle expected matrix, so they
exit 1.  ``negative`` programs change the last real C element and also embed
the matching wrong expected value, so the program exits 0; the Native and
generated-Python full-matrix observers must capture the exact wrong bit
patterns and reject them against the oracle.

Reuses ``run_native.py`` for commands, hashing, ELF/build-tree/tool validation,
host/repository records, and JSON.  Generic command/build/IR/ELF helpers and
the empty-visit recognizer are loaded from the already-read Sieve and
Mandelbrot preparation modules under unique importlib names; their algorithms,
fixed inputs, observers, main(), and parse_args() are not reused.  The C++
build and IR export stay local because the reference must keep
``-ffp-contract=off`` and explicit no-vectorization flags.

No timing, throughput, speedup, or <=2x conclusion is produced.  This script
does not modify the compiler, runtime, public harness, existing fixtures, or
baselines.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import struct
import sys
import traceback
from fractions import Fraction
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_native as common  # noqa: E402

# Load the already-read Mandelbrot preparation module under a unique name and
# reuse only its generic empty-visit recognizer, mangled-call helpers, and
# Python emitter.  It in turn exposes the generic Sieve helpers.
MANDELBROT_PREPARE = (
    Path(__file__).resolve().parents[1] / "mandelbrot" / "prepare.py")
_spec = importlib.util.spec_from_file_location(
    "_matmul_reuse_mandelbrot_prepare", MANDELBROT_PREPARE)
if _spec is None or _spec.loader is None:
    raise RuntimeError(f"cannot load shared Mandelbrot helpers: {MANDELBROT_PREPARE}")
mandel = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = mandel
_spec.loader.exec_module(mandel)
sieve = mandel.sieve

need = sieve.need
now = sieve.now
file_rec = sieve.file_rec
manifest_text = sieve.manifest_text
tool = sieve.tool
run_program = sieve.run_program
require_exit = sieve.require_exit
verify_ir = sieve.verify_ir
build_langc = sieve.build_langc
parse_functions = sieve.parse_functions
parse_loop_sections = sieve.parse_loop_sections
elf_checks = sieve.elf_checks
LABEL_RE = sieve.LABEL_RE
PerfError = sieve.PerfError

visit_function = mandel.visit_function
source_call_count = mandel.source_call_count
decode_hex = mandel.decode_hex
cpp_visit_linkage = mandel.cpp_visit_linkage
cpp_call_count = mandel.cpp_call_count
emit_python = mandel.emit_python
helper_export = mandel.helper_export

CONTRACT = "compiler-dev/1"
TASK = "native-matmul-preparation"
OPERATION_ID = "language-matmul-preparation-f9b3e5c-20261110"
PREPARE_SCHEMA = "compiler-dev/native-matmul-preparation/1"
ORACLE_SCHEMA = "compiler-dev/native-matmul-exact-oracle/1"
N_VALUES = (1, 3, 7, 16)
MAX_N = N_VALUES[-1]
VARIANTS = ("correct", "wrong", "negative")
MUTATION_DELTA = Fraction(1)
MATMUL_ORACLE_SHA256 = (
    "98ced9047358ae30380d584f19e0f22688c46829b95ac20684e38c5022824a18"
)
PERFORMANCE_DIR = Path(__file__).resolve().parent
WORKTREE = PERFORMANCE_DIR.parents[2]
RUNTIME_VEC_HEADER = WORKTREE / "runtime/native/vec.hpp"
DEFAULT_ORACLE = (
    WORKTREE.parent / "checks/algorithm-reference-inputs-20261109/"
    "matmul-exact-small-oracle.json"
)
DEFAULT_PYTHON_HELPER = (
    WORKTREE / "build/native-cli-release/tests/python/lang_python_emit"
)
# Fixed x86_64 generic C++20 target.  -ffp-contract=off and -fno-fast-math keep
# separate multiply/add rounding and forbid FMA.  -fno-vectorize and
# -fno-slp-vectorize keep the scalar f64 loop visible in the max-N O2 IR; the
# C++ source is still the same scalar i-j-k loop.
CPP_FLAGS = (
    "--target=x86_64-unknown-linux-gnu", "-march=x86-64", "-mtune=generic",
    "-fno-fast-math", "-ffp-contract=off", "-fno-lto",
    "-fno-vectorize", "-fno-slp-vectorize",
)
TEMPLATE_NAMES = ("matmul.wz.in", "reference.cpp.in")
OBSERVER_SOURCES = ("native_observer.cpp", "visit_trace.py")
MARKER_RE = re.compile(
    r"^//\s*PERF-MATMUL\s+n=(\d+)\s+variant=(correct|wrong|negative)"
    r"\s+a_sha256=([0-9a-f]{64})\s+b_sha256=([0-9a-f]{64})"
    r"\s+c_sha256=([0-9a-f]{64})\s+expected_sha256=([0-9a-f]{64})"
    r"\s+oracle_sha256=([0-9a-f]{64})\s*$",
    re.MULTILINE,
)
FAST_FLOAT_RE = re.compile(
    r"\b(?:fadd|fsub|fmul|fdiv|fcmp)\s+"
    r"(?:fast|contract|reassoc|nnan|ninf|nsz|arcp|afn)\b"
)
FMA_RE = re.compile(r"\bllvm\.(?:fmuladd|fma)\b")
VECTOR_DOUBLE_RE = re.compile(r"<\s*\d+\s+x\s+double\s*>")
RATIONAL_RE = re.compile(r"-?\d+(?:/\d+)?")


def require_observer(run, label):
    """Observer programs print the captured bits, so only stderr must be empty."""
    need(run["status"] == "exited" and run["exit"] == 0,
         f"{label}: observer failed: status={run['status']} "
         f"exit={run['exit']} stderr={run['stderr'][-1000:]!r}")
    need(not run["stderr"],
         f"{label}: observer wrote stderr: {run['stderr'][-1000:]!r}")


def check_float_flags(path, report, label):
    """Require no fast/contract float flags and no FMA intrinsic anywhere."""
    text = Path(path).read_text(encoding="utf-8")
    fast = sorted(set(FAST_FLOAT_RE.findall(text)))
    fma = sorted(set(FMA_RE.findall(text)))
    row = {"path": str(path), "label": label,
           "fast_float_flags": fast, "fma_intrinsics": fma, "success": False}
    report["float_flag_checks"].append(row)
    need(not fast, f"{label}: fast/contract float flags found: {fast}")
    need(not fma, f"{label}: FMA intrinsic found: {fma}")
    row["success"] = True
    return row


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha_map(paths):
    return {str(Path(path).resolve()): common.sha256_file(path)
            for path in paths}


def template_records():
    return {name: file_rec(PERFORMANCE_DIR / name)
            for name in (*TEMPLATE_NAMES, *OBSERVER_SOURCES, "prepare.py",
                         "README.md")}


def read_templates():
    return {name: (PERFORMANCE_DIR / name).read_text(encoding="utf-8")
            for name in TEMPLATE_NAMES}


def fraction_of(text, label):
    need(isinstance(text, str), f"{label}: rational is not a string")
    need(RATIONAL_RE.fullmatch(text) is not None,
         f"{label}: rational {text!r} is not p/q or an integer")
    return Fraction(text)


def representable_as_binary64(value):
    """True when the exact rational is a finite binary64 value."""
    value = Fraction(value)
    if value == 0:
        return True
    numerator, denominator = value.numerator, value.denominator
    exponent = denominator.bit_length() - 1
    if denominator != 1 << exponent:
        return False
    magnitude = abs(numerator)
    trailing = (magnitude & -magnitude).bit_length() - 1
    significand = magnitude >> trailing
    scale = trailing - exponent
    if significand.bit_length() > 53:
        return False
    if scale < -1074:
        return False
    return significand.bit_length() + scale <= 1024


def bits_of(value, label):
    value = Fraction(value)
    need(representable_as_binary64(value),
         f"{label}: {value} is not exactly representable as binary64")
    packed = struct.pack(">d", float(value))
    bits = f"{struct.unpack('>Q', packed)[0]:016x}"
    decoded = struct.unpack(">d", packed)[0]
    need(Fraction(decoded) == value,
         f"{label}: binary64 round trip changed {value} to {decoded!r}")
    return bits


def fraction_from_bits(bits, label):
    need(re.fullmatch(r"[0-9a-f]{16}", bits) is not None,
         f"{label}: {bits!r} is not a 16-digit lowercase hex pattern")
    value = struct.unpack(">d", bytes.fromhex(bits))[0]
    need(value == value and value not in (float("inf"), float("-inf")),
         f"{label}: {bits} is not a finite binary64 value")
    return Fraction(value)


def exact_decimal(value, label):
    """Exact shortest decimal text for a rational with a power-of-two divisor."""
    value = Fraction(value)
    sign = "-" if value < 0 else ""
    numerator = abs(value.numerator)
    denominator = value.denominator
    digits = denominator.bit_length() - 1
    need(denominator == 1 << digits,
         f"{label}: denominator {denominator} is not a power of two")
    scaled = numerator * 5 ** digits
    text = str(scaled).rjust(digits + 1, "0")
    if digits == 0:
        integer, fraction = text, ""
    else:
        integer, fraction = text[:-digits], text[-digits:]
        fraction = fraction.rstrip("0")
    if not fraction:
        fraction = "0"
    return f"{sign}{integer}.{fraction}"


def decimal_fraction(text, label):
    sign = -1 if text.startswith("-") else 1
    body = text[1:] if text[:1] in "+-" else text
    integer, dot, fraction = body.partition(".")
    need(integer.isdigit() and (not dot or fraction.isdigit()),
         f"{label}: {text!r} is not a plain decimal literal")
    scaled = int(integer) * (10 ** len(fraction)) + (int(fraction) if fraction else 0)
    return Fraction(sign * scaled, 10 ** len(fraction))


def decimal_list(values, label):
    parts = []
    for value in values:
        text = exact_decimal(value, label)
        need(decimal_fraction(text, label) == Fraction(value),
             f"{label}: rendered decimal {text} does not round trip")
        parts.append(text)
    return ", ".join(parts)


def bits_sha256(bits):
    return sha256_bytes(bytes.fromhex("".join(bits)))


def bits_bytes(bits):
    return bytes.fromhex("".join(bits))


def expected_inputs(n):
    def a_entry(i, j):
        return Fraction((17 * i + 13 * j) % 19 - 9, 8)

    def b_entry(i, j):
        return Fraction((11 * i + 7 * j) % 23 - 11, 8)

    return ([[a_entry(i, j) for j in range(n)] for i in range(n)],
            [[b_entry(i, j) for j in range(n)] for i in range(n)])


def parse_matrix(rows, n, label):
    need(isinstance(rows, list) and len(rows) == n,
         f"{label}: expected {n} rows")
    matrix = []
    for i, row in enumerate(rows):
        need(isinstance(row, list) and len(row) == n,
             f"{label}: row {i} is not {n} entries")
        matrix.append([fraction_of(value, f"{label}[{i}][{j}]")
                       for j, value in enumerate(row)])
    return matrix


def load_oracle(path, out):
    data = Path(path).read_bytes()
    need(sha256_bytes(data) == MATMUL_ORACLE_SHA256,
         f"{path}: oracle SHA-256 is not {MATMUL_ORACLE_SHA256}")
    document = json.loads(data)
    need(list(document) == ["method", "cases"],
         f"{path}: unexpected oracle top-level keys {list(document)}")
    need("rational" in document["method"].lower(),
         f"{path}: oracle method is not exact-rational")
    by_n = {}
    for case in document["cases"]:
        n = case.get("n")
        need(isinstance(n, int) and n in N_VALUES,
             f"{path}: unexpected case n={n!r}")
        need(n not in by_n, f"{path}: duplicate n={n}")
        by_n[n] = case
    need(sorted(by_n) == list(N_VALUES),
         f"{path}: oracle cases are {sorted(by_n)}, expected {list(N_VALUES)}")

    oracle_dir = out / "oracle"
    oracle_dir.mkdir(parents=True, exist_ok=False)
    frozen = oracle_dir / "matmul-exact-small-oracle.json"
    frozen.write_bytes(data)
    need(frozen.read_bytes() == data, f"{frozen}: frozen oracle copy changed")

    cases = {}
    values_by_n = {}
    products_checked = 0
    sums_checked = 0
    values_checked = 0
    for n in N_VALUES:
        case = by_n[n]
        need(list(case) == ["n", "a", "b", "c", "c_f64_bits"],
             f"{path}: n={n} has unexpected keys {list(case)}")
        a = parse_matrix(case["a"], n, f"n={n} oracle a")
        b = parse_matrix(case["b"], n, f"n={n} oracle b")
        c = parse_matrix(case["c"], n, f"n={n} oracle c")
        wanted_a, wanted_b = expected_inputs(n)
        need(a == wanted_a, f"n={n}: oracle A does not match the fixed formula")
        need(b == wanted_b, f"n={n}: oracle B does not match the fixed formula")
        bits = case["c_f64_bits"]
        need(isinstance(bits, list) and len(bits) == n * n,
             f"n={n}: expected {n * n} c_f64_bits, got {len(bits)}")
        flat_a = [value for row in a for value in row]
        flat_b = [value for row in b for value in row]
        flat_c = [value for row in c for value in row]
        for i in range(n):
            for j in range(n):
                total = Fraction(0)
                for k in range(n):
                    product = a[i][k] * b[k][j]
                    need(representable_as_binary64(product),
                         f"n={n} ({i},{j},{k}): product is not binary64")
                    products_checked += 1
                    total += product
                    need(representable_as_binary64(total),
                         f"n={n} ({i},{j},{k}): partial sum is not binary64")
                    sums_checked += 1
                need(total == c[i][j],
                     f"n={n} ({i},{j}): exact dot product {total} != oracle {c[i][j]}")
                expected_bits = bits_of(c[i][j], f"n={n} c[{i}][{j}]")
                need(expected_bits == bits[i * n + j],
                     f"n={n} c[{i}][{j}]: bits {expected_bits} != "
                     f"oracle {bits[i * n + j]}")
                need(fraction_from_bits(bits[i * n + j],
                                       f"n={n} c[{i}][{j}]") == c[i][j],
                     f"n={n} c[{i}][{j}]: oracle bits do not decode exactly")
                if c[i][j] == 0:
                    need(bits[i * n + j] == "0000000000000000",
                         f"n={n} c[{i}][{j}]: expected canonical +0 bits")
                values_checked += 1
        a_text = decimal_list(flat_a, f"n={n} A")
        b_text = decimal_list(flat_b, f"n={n} B")
        c_text = decimal_list(flat_c, f"n={n} C")
        a_bits = [bits_of(value, f"n={n} A") for value in flat_a]
        b_bits = [bits_of(value, f"n={n} B") for value in flat_b]
        record = {
            "n": n,
            "a": a_text, "b": b_text, "c": c_text,
            "a_bits": a_bits, "b_bits": b_bits, "c_bits": bits,
            "a_sha256": bits_sha256(a_bits),
            "b_sha256": bits_sha256(b_bits),
            "c_sha256": bits_sha256(bits),
            "c_f64_bits_sha256": sha256_bytes(
                "\n".join(bits).encode("ascii")),
        }
        case_path = oracle_dir / f"n{n}.json"
        common.write_json(case_path, record)
        record["file"] = file_rec(case_path, out)
        cases[n] = record
        values_by_n[n] = flat_c

    oracle_json = {
        "schema": ORACLE_SCHEMA,
        "method": "exact Fraction dot products from the frozen independent "
                  "oracle; every product and partial sum proven exactly "
                  "representable as binary64",
        "source_oracle": file_rec(path),
        "source_oracle_sha256": MATMUL_ORACLE_SHA256,
        "frozen_copy": file_rec(frozen, out),
        "n_values": list(N_VALUES),
        "matrix_layout": "row-major, A[i*N+k], B[k*N+j], C[i*N+j]",
        "loop_order": "i, j, k with one multiply and one add per k",
        "variants": {
            "correct": "exact oracle expected; unmodified C; exit 0",
            "wrong": f"last C element + {MUTATION_DELTA}; oracle expected; exit 1",
            "negative": f"last C element + {MUTATION_DELTA}; matching wrong "
                        "expected; exit 0, observers must reject",
        },
        "formula": {
            "a": "((17*i + 13*j) % 19 - 9) / 8",
            "b": "((11*i + 7*j) % 23 - 11) / 8",
            "c": "exact mathematical dot product",
        },
        "cases": {str(n): row for n, row in cases.items()},
        "checks": {
            "cases": len(cases),
            "formula_entries": 2 * sum(n * n for n in N_VALUES),
            "products_exact_checked": products_checked,
            "partial_sums_exact_checked": sums_checked,
            "c_values_checked": values_checked,
        },
    }
    common.write_json(out / "oracle.json", oracle_json)
    return (cases, values_by_n, file_rec(out / "oracle.json", out),
            file_rec(frozen, out))


def expected_for_variant(n, oracle_values, oracle_bits, variant):
    """Return embedded expected values/bits and the mutation index."""
    need(n * n == len(oracle_values) == len(oracle_bits),
         "oracle value and bit-pattern lengths differ")
    last = n * n - 1
    mutate = variant in ("wrong", "negative")
    if variant == "correct":
        values, bits = list(oracle_values), list(oracle_bits)
    elif variant == "wrong":
        values, bits = list(oracle_values), list(oracle_bits)
    elif variant == "negative":
        values = list(oracle_values)
        values[last] = values[last] + MUTATION_DELTA
        need(representable_as_binary64(values[last]),
             f"n={n}: negative expected value is not binary64")
        bits = [bits_of(value, f"n={n} negative expected") for value in values]
        changed = [index for index, (actual, oracle) in
                   enumerate(zip(bits, oracle_bits)) if actual != oracle]
        need(changed == [last],
             f"n={n}: negative expected changed indices {changed}, "
             f"expected only [{last}]")
        need(values[last] != oracle_values[last],
             f"n={n}: negative expected value did not change")
    else:
        raise PerfError(f"unknown variant {variant}")
    return {
        "values": values, "bits": bits, "mutate": mutate,
        "mutation_index": last if mutate else None,
    }


def render(template, mapping):
    text = template
    for key, value in mapping.items():
        text = text.replace("{{" + key + "}}", str(value))
    need("{{" not in text, "rendered template still has a placeholder")
    return text


def verify_generated(source, reference, case, variant, expected, wanted):
    for text, language in ((source, "Source"), (reference, "C++")):
        match = MARKER_RE.search(text)
        need(match is not None, f"{language} marker is missing")
        actual = (int(match.group(1)), match.group(2), match.group(3),
                  match.group(4), match.group(5), match.group(6),
                  match.group(7))
        need(actual == wanted, f"{language} marker {actual} != {wanted}")
    n = case["n"]
    nn = n * n
    last = expected["mutation_index"]
    mutation = "" if last is None else f"    c[{last}] = c[{last}] + 1.0;"
    zeros = ", ".join("0.0" for _ in range(nn))
    expected_text = decimal_list(expected["values"], f"n={n} expected")

    for snippet in (
        f"let a Vec[f64] = [{case['a']}];",
        f"let b Vec[f64] = [{case['b']}];",
        f"var c Vec[f64] = [{zeros}];",
        f"let expected Vec[f64] = [{expected_text}];",
        "var sum f64 = 0.0;",
        "for var i i32 = 0; i < n; i += 1 {",
        "for var j i32 = 0; j < n; j += 1 {",
        "for var k i32 = 0; k < n; k += 1 {",
        "sum = sum + a[i * n + k] * b[k * n + j];",
        "c[i * n + j] = sum;",
        "if c[index] != expected[index] {",
        "visit(c);",
    ):
        need(snippet in source, f"n={n} {variant}: Source is missing {snippet!r}")
    for snippet in (
        f"const std::vector<double> a{{ {case['a']} }};",
        f"const std::vector<double> b{{ {case['b']} }};",
        f"const std::vector<double> expected{{ {expected_text} }};",
        "std::vector<double> c(",
        "static_cast<std::size_t>(n) * static_cast<std::size_t>(n), 0.0);",
        "double sum = 0.0;",
        "for (int32_t i = 0; i < n; ++i) {",
        "for (int32_t j = 0; j < n; ++j) {",
        "for (int32_t k = 0; k < n; ++k) {",
        "a[static_cast<std::size_t>(i * n + k)] *",
        "b[static_cast<std::size_t>(k * n + j)];",
        "c[static_cast<std::size_t>(i * n + j)] = sum;",
        "visit(c);",
    ):
        need(snippet in reference,
             f"n={n} {variant}: C++ is missing {snippet!r}")
    need("std::vector<double>" in reference and "std::vector<bool>" not in reference,
         f"n={n} {variant}: C++ must use std::vector<double>")
    if mutation:
        need(source.count(mutation) == 1,
             f"n={n} {variant}: Source mutation is missing or duplicated")
        need(reference.count(mutation) == 1,
             f"n={n} {variant}: C++ mutation is missing or duplicated")
    else:
        need("c[index] = c[index] + 1.0" not in source,
             f"n={n}: correct Source must not mutate C")
        need("c[index] = c[index] + 1.0" not in reference,
             f"n={n}: correct C++ must not mutate C")
    need("Vec[f32]" not in source and "std::vector<float>" not in reference,
         f"n={n} {variant}: unexpected f32 matrix")
    for banned in ("cblas_", "openblas", "#include <thread>", "std::thread",
                   "#pragma omp", "clock_gettime"):
        need(banned not in source.lower() and banned not in reference.lower(),
             f"n={n} {variant}: forbidden API {banned!r} is present")
    return mutation


def generate_case(out, templates, oracle_sha, case, oracle_values, variant):
    expected = expected_for_variant(
        case["n"], oracle_values, case["c_bits"], variant)
    n = case["n"]
    nn = n * n
    expected_bits_sha = bits_sha256(expected["bits"])
    wanted = (n, variant, case["a_sha256"], case["b_sha256"],
              case["c_sha256"], expected_bits_sha, oracle_sha)
    zeros = ", ".join("0.0" for _ in range(nn))
    mutation = "" if expected["mutation_index"] is None else (
        f"    c[{expected['mutation_index']}] = "
        f"c[{expected['mutation_index']}] + 1.0;")
    mapping = {
        "N": n, "NN": nn, "VARIANT": variant,
        "A": case["a"], "B": case["b"], "ZEROS": zeros,
        "EXPECTED": decimal_list(expected["values"], f"n={n} expected"),
        "MUTATION": mutation,
        "A_SHA": case["a_sha256"], "B_SHA": case["b_sha256"],
        "C_SHA": case["c_sha256"], "EXPECTED_SHA": expected_bits_sha,
        "ORACLE_SHA": oracle_sha,
    }
    source_text = render(templates["matmul.wz.in"], mapping)
    reference_text = render(templates["reference.cpp.in"], mapping)
    verify_generated(source_text, reference_text, case, variant, expected, wanted)
    directory = out / "cases" / f"n{n}" / variant
    (directory / "src").mkdir(parents=True, exist_ok=False)
    source = directory / "src" / "main.wz"
    reference = directory / "reference.cpp"
    manifest = directory / "lang.toml"
    source.write_text(source_text, encoding="utf-8")
    reference.write_text(reference_text, encoding="utf-8")
    manifest.write_text(
        manifest_text(f"perf_matmul_n{n}_{variant}"), encoding="utf-8")
    expected_path = directory / "expected_bits.txt"
    expected_path.write_text(
        "\n".join(expected["bits"]) + "\n", encoding="ascii")
    return {
        "n": n, "variant": variant,
        "mutation_index": expected["mutation_index"],
        "mutation_is_first": expected["mutation_index"] == 0,
        "embedded_expected_sha256": expected_bits_sha,
        "embedded_expected_bits": expected["bits"],
        "manifest": file_rec(manifest, out), "source": file_rec(source, out),
        "reference": file_rec(reference, out),
        "expected_bits": file_rec(expected_path, out),
        "builds": [], "executions": [], "helper": [],
        "native_observers": [], "python_observers": [],
    }


def base_report(args, env):
    report = common.base_report("prepare", args, env)
    report.update({
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "formal_timing": False,
        "script": file_rec(Path(__file__).resolve()),
        "common_runner": file_rec(Path(common.__file__).resolve()),
        "mandelbrot_helpers": file_rec(MANDELBROT_PREPARE),
        "sieve_helpers": file_rec(mandel.SIEVE_PREPARE),
        "reuse_module": "_matmul_reuse_mandelbrot_prepare",
        "runtime_vec_header": file_rec(RUNTIME_VEC_HEADER),
        "templates": template_records(),
    })
    report["target"].update({
        "cpp_flags": list(CPP_FLAGS),
        "cpp_linker": str(args.llvm_bin / "ld.lld"),
        "cpp_vectorization": False,
    })
    report["tools"]["python_helper"] = common.tool_record(
        args.python_helper, env, args.cpu, None)
    return report


def build_cpp(args, env, out, reference, program, opt, label):
    program.parent.mkdir(parents=True, exist_ok=True)
    command = tool(
        [args.llvm_bin / "clang++", *CPP_FLAGS, "-std=c++20", f"-O{opt}",
         f"--ld-path={args.llvm_bin / 'ld.lld'}", reference, "-o", program],
        args, env)
    common.require_success(command, f"{label} build")
    return {
        "implementation": "cpp", "opt": opt,
        "command_index": command["command_index"],
        "program": file_rec(program, out),
        "elf": common.checked_elf(program, executable=True, label=label),
    }


def compile_cpp_ir(args, env, report, out, reference, opt, label):
    output = out / "llvm" / label.replace(" ", "-").replace("=", "") / f"O{opt}.ll"
    output.parent.mkdir(parents=True, exist_ok=True)
    command = tool(
        [args.llvm_bin / "clang++", *CPP_FLAGS, "-std=c++20", f"-O{opt}",
         "-g", "-S", "-emit-llvm", reference, "-o", output], args, env)
    common.require_success(command, f"{label} IR build")
    text = output.read_text(encoding="utf-8")
    return {
        "command_index": command["command_index"],
        "ir": file_rec(output, out), "text": text,
        "verify": verify_ir(output, args, env, report, f"{label} verify"),
    }


def execute(args, env, case, implementation, opt, program, expected, label):
    run = run_program([program], args, env)
    row = {"implementation": implementation, "opt": opt,
           "expected_exit": expected, "command_index": run["command_index"],
           "success": False}
    case["executions"].append(row)
    require_exit(run, expected, label)
    row.update(success=True, exit=run["exit"], stdout=run["stdout"],
               stderr=run["stderr"])
    return row


def work_scan(args, env, report, path, label, n, implementation):
    """Record real scalar f64 work; the max N must retain the actual loop."""
    text = Path(path).read_text(encoding="utf-8")
    fast = sorted(set(FAST_FLOAT_RE.findall(text)))
    fma = sorted(set(FMA_RE.findall(text)))
    need(not fast, f"{label}: fast/contract float flags found: {fast}")
    need(not fma, f"{label}: FMA intrinsic found: {fma}")
    vector_double = VECTOR_DOUBLE_RE.findall(text)
    functions = parse_functions(text)
    candidates = []
    for name, info in functions.items():
        body = info["body"]
        score = (body.count("fmul double") + body.count("fadd double")
                 + body.count("load double") + body.count("store double"))
        if "fmul double" in body and "fadd double" in body:
            candidates.append((score, name))
    row = {
        "implementation": implementation, "n": n, "path": str(path),
        "file": file_rec(path), "success": False,
        "fast_float_flags": fast, "fma_intrinsics": fma,
        "vector_double_ops": len(vector_double),
        "constant_folded": False, "real_loop_work": False,
        "target": None, "target_loop_count": 0,
        "target_loop_lines": [], "fmul_double": 0, "fadd_double": 0,
        "load_double": 0, "store_double": 0, "store_in_loop": False,
    }
    report["work_analyses"].append(row)
    if not candidates:
        need(not vector_double,
             f"{label}: vector f64 work without a scalar loop")
        row.update(constant_folded=True, success=True)
        if n == MAX_N:
            raise PerfError(
                f"{label}: max-N O2 IR has no real scalar f64 multiply/add work")
        return row
    target = max(candidates)[1]
    body = functions[target]["body"]
    loops = tool([args.llvm_bin / "opt", "-passes=print<loops>",
                  "-disable-output", path], args, env)
    loop_row = {"path": str(path), "command_index": loops["command_index"],
                "success": False, "label": label}
    report["loop_analyses"].append(loop_row)
    common.require_success(loops, f"{label} loop analysis")
    loop_row["success"] = True
    target_loops = parse_loop_sections(loops["stderr"]).get(target, [])
    stores = []
    for match in re.finditer(r"^\s*store double\b[^\n]*$", body, re.MULTILINE):
        block = None
        for line in reversed(body[:match.start()].splitlines()):
            found = LABEL_RE.match(line.strip())
            if found:
                block = found.group(1)
                break
        stores.append((block, match.group(0).strip()))
    store_in_loop = any(
        block is not None and any("%" + block in line for line in target_loops)
        for block, _ in stores)
    fmul = body.count("fmul double")
    fadd = body.count("fadd double")
    loads = body.count("load double")
    scalar_stores = len(stores)
    row.update({
        "target": target,
        "target_is_main": target == "main",
        "target_loop_count": len(target_loops),
        "target_loop_lines": target_loops,
        "fmul_double": fmul, "fadd_double": fadd,
        "load_double": loads, "store_double": scalar_stores,
        "store_in_loop": store_in_loop,
        "real_loop_work": (bool(target_loops) and store_in_loop
                           and fmul > 0 and fadd > 0 and loads > 0
                           and scalar_stores > 0 and not vector_double),
    })
    if n == MAX_N:
        need(target_loops, f"{label}: max-N target {target} has no real loop")
        need(store_in_loop,
             f"{label}: max-N C store double is not inside a loop")
        need(fmul > 0 and fadd > 0 and loads > 0 and scalar_stores > 0,
             f"{label}: max-N O2 IR lost the scalar f64 multiply/add/load/store")
        need(not vector_double,
             f"{label}: max-N O2 IR uses vector f64 operations: "
             f"{vector_double[:4]}")
    row["constant_folded"] = not row["real_loop_work"]
    row["success"] = True
    return row


def instrument_visit(text, symbol, label):
    visit = visit_function(text, symbol, label)
    need("matmul_observe" not in text,
         f"{label}: matmul_observe already exists in the IR")
    body = visit["body"]
    returns = list(re.finditer(r"^\s*ret\b[^\n]*$", body, re.MULTILINE))
    need(len(returns) == 1, f"{label}: expected one return in empty visit")
    call = f"  call void @matmul_observe(ptr {visit['argument']})\n"
    new_body = body[:returns[0].start()] + call + body[returns[0].start():]
    new_text = text[:visit["body_start"]] + new_body + text[visit["body_end"]:]
    first_define = new_text.index("define ")
    new_text = (new_text[:first_define]
                + "declare void @matmul_observe(ptr)\n\n"
                + new_text[first_define:])
    return new_text, visit


def compile_native_observer(args, env, out, observe_ll, binary, opt, label):
    binary.parent.mkdir(parents=True, exist_ok=True)
    command = tool(
        [args.llvm_bin / "clang++", *CPP_FLAGS, "-std=c++20", f"-O{opt}",
         "-I", str(WORKTREE / "runtime/native"),
         PERFORMANCE_DIR / "native_observer.cpp", observe_ll,
         args.runtime, f"--ld-path={args.llvm_bin / 'ld.lld'}",
         "-o", binary],
        args, env)
    common.require_success(command, f"{label} observer build")
    return {
        "command_index": command["command_index"],
        "binary": file_rec(binary, out),
        "elf": common.checked_elf(binary, executable=True, label=label),
        "formal_cli": False, "host_libc_dependent": True,
        "not_independent_native_performance_program": True,
    }


def observe_native(args, env, report, out, case, variant, opt, helper,
                   expected_bits):
    before_path = Path(helper["before"]["path"])
    instrumented, visit = instrument_visit(
        before_path.read_text(encoding="utf-8"), helper["visit_symbol"],
        f"n={case['n']} {variant} O{opt}")
    directory = out / "observers" / "native" / (
        f"n{case['n']}/{variant}/O{opt}")
    directory.mkdir(parents=True, exist_ok=False)
    observe_ll = directory / "observe.ll"
    observe_ll.write_text(instrumented, encoding="utf-8")
    verify = verify_ir(observe_ll, args, env, report,
                       f"n={case['n']} {variant} O{opt} instrumented verify")
    binary = directory / "observer"
    compiled = compile_native_observer(
        args, env, out, observe_ll, binary, opt,
        f"n={case['n']} {variant} O{opt}")
    run = run_program([binary], args, env)
    require_observer(run, f"n={case['n']} {variant} O{opt} observer run")
    captured = decode_hex(run, f"n={case['n']} {variant} O{opt}")
    want = bits_bytes(expected_bits)
    oracle_bits = case["oracle_bits"]
    oracle = bits_bytes(oracle_bits)
    need(len(captured) == len(want),
         f"n={case['n']} {variant} O{opt}: observer captured "
         f"{len(captured)} bytes, expected {len(want)}")
    hex_path = directory / "observed.hex"
    hex_path.write_text(run["stdout"], encoding="utf-8")
    row = {
        "implementation": "native", "variant": variant, "opt": opt,
        "visit_symbol": helper["visit_symbol"],
        "parameter_type": visit["parameter_type"],
        "argument": visit["argument"],
        "command_index": run["command_index"],
        "instrumented": file_rec(observe_ll, out), "verify": verify,
        "compile": compiled, "hex": file_rec(hex_path, out),
        "captured_sha256": sha256_bytes(captured),
        "captured_bytes": len(captured),
        "expected_sha256": sha256_bytes(want),
        "oracle_sha256": sha256_bytes(oracle),
        "matches_expected": captured == want,
        "rejected_oracle": captured != oracle,
        "success": False,
    }
    if variant == "correct":
        need(captured == oracle,
             f"n={case['n']} correct O{opt}: Native bits differ from oracle")
        row["observer_control"] = "correct"
    else:
        need(captured == want,
             f"n={case['n']} {variant} O{opt}: Native observer did not "
             "capture the exact embedded wrong bits")
        need(captured != oracle,
             f"n={case['n']} {variant} O{opt}: Native observer did not "
             "reject the wrong element")
        row["observer_control"] = "negative-rejected"
    row["success"] = True
    case["native_observers"].append(row)
    return row


def observe_python(args, env, out, case, variant, emitted, expected_bits):
    directory = out / "observers" / "python" / f"n{case['n']}/{variant}"
    directory.mkdir(parents=True, exist_ok=True)
    run = tool(
        [args.python, "-B", "-s", PERFORMANCE_DIR / "visit_trace.py",
         "--generated-dir", emitted["directory"]], args, env)
    require_observer(run, f"n={case['n']} {variant} Python observer")
    captured = decode_hex(run, f"n={case['n']} {variant} Python observer")
    want = bits_bytes(expected_bits)
    oracle = bits_bytes(case["oracle_bits"])
    need(len(captured) == len(want),
         f"n={case['n']} {variant}: Python observer captured "
         f"{len(captured)} bytes, expected {len(want)}")
    hex_path = directory / "observed.hex"
    hex_path.write_text(run["stdout"], encoding="utf-8")
    row = {
        "implementation": "python", "variant": variant,
        "visit_symbol": emitted["visit_symbol"],
        "visit_generated": emitted["visit_generated"],
        "command_index": run["command_index"], "hex": file_rec(hex_path, out),
        "captured_sha256": sha256_bytes(captured),
        "captured_bytes": len(captured),
        "expected_sha256": sha256_bytes(want),
        "oracle_sha256": sha256_bytes(oracle),
        "matches_expected": captured == want,
        "rejected_oracle": captured != oracle,
        "success": False,
    }
    if variant == "correct":
        need(captured == oracle,
             f"n={case['n']} correct: Python bits differ from oracle")
        row["observer_control"] = "correct"
    else:
        need(captured == want,
             f"n={case['n']} {variant}: Python observer did not capture "
             "the exact embedded wrong bits")
        need(captured != oracle,
             f"n={case['n']} {variant}: Python observer did not reject "
             "the wrong element")
        row["observer_control"] = "negative-rejected"
    row["success"] = True
    case["python_observers"].append(row)
    return row


def input_snapshot(report, out):
    paths = [PERFORMANCE_DIR / name
             for name in (*TEMPLATE_NAMES, *OBSERVER_SOURCES, "prepare.py",
                          "README.md")]
    paths.extend([Path(__file__).resolve(), Path(common.__file__).resolve(),
                  MANDELBROT_PREPARE, mandel.SIEVE_PREPARE, RUNTIME_VEC_HEADER])
    paths.extend([Path(report["oracle"]["source_oracle"]["path"]),
                  Path(report["oracle"]["frozen_copy"]["path"]),
                  Path(report["oracle"]["file"]["path"])])
    for case in report["cases"]:
        for variant in case["variants"].values():
            paths.extend([Path(variant["manifest"]["path"]),
                          Path(variant["source"]["path"]),
                          Path(variant["reference"]["path"]),
                          Path(variant["expected_bits"]["path"])])
    for n, row in report["oracle"]["cases"].items():
        paths.append(Path(row["file"]["path"]))
    return sha_map(paths)


def write_patch(out, args, env):
    names = (*TEMPLATE_NAMES, *OBSERVER_SOURCES, "prepare.py", "README.md")
    parts = []
    for name in names:
        relative = f"tests/performance/matmul/{name}"
        result = common.command(
            ["git", "-C", str(WORKTREE), "diff", "--no-index", "--binary",
             "--src-prefix=a/", "--dst-prefix=b/", "--", "/dev/null", relative],
            timeout=30, env=env, cpu=args.cpu)
        need(result["status"] == "exited" and result["exit"] == 1,
             f"git diff --no-index for {relative} failed: {result}")
        parts.append(result["stdout"])
    patch = out / "changes.patch"
    patch.write_text("".join(parts), encoding="utf-8")
    return {"path": str(patch), "sha256": common.sha256_file(patch),
            "size_bytes": patch.stat().st_size, "files": list(names)}


def prepare(args):
    env = common.make_env(args)
    out = common.prepare_out(args.out)
    (out / "started_at.txt").write_text(now() + "\n", encoding="utf-8")
    report = {
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "mode": "prepare", "status": "running",
        "commands": [], "verifies": [], "loop_analyses": [], "elf_checks": [],
        "work_analyses": [], "float_flag_checks": [],
        "observers": {"native": [], "python": []},
        "cases": [],
    }
    common._COMMAND_LOG = report["commands"]
    try:
        report.update(base_report(args, env))
        root = Path(args.compiler_root).resolve()
        for label, path in (("langc", args.langc), ("helper", args.helper),
                            ("runtime", args.runtime)):
            need(root in Path(path).resolve().parents,
                 f"{label} is not under --compiler-root {root}: {path}")
        report["tools_under_compiler_root"] = True
        report["compiler_root_is_worktree"] = root == WORKTREE.resolve()
        cache_path = Path(report["build_tree"]["cmake_cache"]["path"])
        build_testing = re.search(r"^BUILD_TESTING:BOOL=(.*)$",
                                  cache_path.read_text(encoding="utf-8"),
                                  re.MULTILINE)
        report["build_tree"]["build_testing"] = (
            build_testing.group(1).strip() if build_testing else None)
        need(report["build_tree"]["build_testing"] == "OFF",
             f"{cache_path}: BUILD_TESTING is not OFF")
        report["parameters"] = {
            "n_values": list(N_VALUES), "variants": list(VARIANTS),
            "layout": "row-major A[i*N+k], B[k*N+j], C[i*N+j]",
            "loop_order": "i-j-k, one f64 multiply and add per k",
            "mutation": f"last C element = last C element + {MUTATION_DELTA}",
            "oracle_sha256": MATMUL_ORACLE_SHA256,
            "target_flags": list(CPP_FLAGS),
            "cpp_vectorization": False,
            "program_expected_exit": {"correct": 0, "wrong": 1,
                                      "negative": 0},
        }
        report["tools_before"] = {
            name: file_rec(path) for name, path in {
                "langc": args.langc, "helper": args.helper,
                "runtime": args.runtime, "clang": args.llvm_bin / "clang",
                "clang++": args.llvm_bin / "clang++",
                "ld.lld": args.llvm_bin / "ld.lld",
                "opt": args.llvm_bin / "opt",
                "llvm-readelf": args.llvm_bin / "llvm-readelf",
                "llvm-nm": args.llvm_bin / "llvm-nm",
                "python": args.python,
                "python-helper": args.python_helper,
            }.items()
        }
        templates = read_templates()
        oracle_cases, oracle_values, oracle_json, frozen = load_oracle(
            args.oracle, out)
        oracle_sha = MATMUL_ORACLE_SHA256
        report["oracle"] = {
            "method": "frozen independent exact-Fraction oracle",
            "file": oracle_json,
            "source_oracle": file_rec(args.oracle),
            "frozen_copy": frozen,
            "sha256": oracle_sha,
            "cases": {str(n): row for n, row in oracle_cases.items()},
            "fraction_checks": {
                "products": sum(case["n"] ** 3 for case in oracle_cases.values()),
                "partial_sums": sum(case["n"] ** 3
                                    for case in oracle_cases.values()),
                "c_values": sum(case["n"] ** 2 for case in oracle_cases.values()),
            },
        }
        for n in N_VALUES:
            case = {"n": n, "oracle": oracle_cases[n], "variants": {},
                    "check": None, "cpp_ir": {},
                    "native_observers": [], "python_observers": [],
                    "oracle_bits": oracle_cases[n]["c_bits"]}
            report["cases"].append(case)
            for variant in VARIANTS:
                case["variants"][variant] = generate_case(
                    out, templates, oracle_sha, oracle_cases[n],
                    oracle_values[n], variant)
        report["inputs_before"] = input_snapshot(report, out)

        for case in report["cases"]:
            n = case["n"]
            variants = case["variants"]
            correct = variants["correct"]
            negative = variants["negative"]
            manifest = correct["manifest"]["path"]
            reference = correct["reference"]["path"]
            check = tool([args.langc, "check", "--project", manifest,
                          "--backend", "native", "--format", "json"], args, env)
            common.require_success(check, f"n={n} langc check")
            check_json = common.parse_json_stdout(check, f"n={n} langc check")
            need(check_json.get("status") == "ok", f"n={n}: check is not ok")
            case["check"] = {"command_index": check["command_index"],
                             "json": check_json, "success": True}

            emitted_correct = emit_python(
                args, env, out, manifest,
                out / "python" / "emit" / f"n{n}" / "correct", f"n={n} correct")
            emitted_negative = emit_python(
                args, env, out, negative["manifest"]["path"],
                out / "python" / "emit" / f"n{n}" / "negative",
                f"n={n} negative")
            case["python"] = {"correct": emitted_correct,
                              "negative": emitted_negative}
            observe_python(args, env, out, case, "correct", emitted_correct,
                           case["oracle_bits"])
            observe_python(args, env, out, case, "negative", emitted_negative,
                           negative["embedded_expected_bits"])

            for opt in (0, 2):
                langc_row = build_langc(
                    args, env, out, manifest,
                    out / "build" / f"n{n}" / "correct" / "langc" / f"O{opt}",
                    opt, f"n={n} correct langc O{opt}")
                correct["builds"].append(langc_row)
                execute(args, env, correct, "langc", opt,
                        langc_row["program"]["path"], 0,
                        f"n={n} correct langc O{opt}")
                elf_checks(args, env, report, langc_row["program"]["path"],
                           f"n={n} correct langc O{opt}", "langc")
                cpp_row = build_cpp(
                    args, env, out, reference,
                    out / "build" / f"n{n}" / "correct" / "cpp" / f"O{opt}" /
                    "reference", opt, f"n={n} correct cpp O{opt}")
                correct["builds"].append(cpp_row)
                execute(args, env, correct, "cpp", opt,
                        cpp_row["program"]["path"], 0,
                        f"n={n} correct cpp O{opt}")
                elf_checks(args, env, report, cpp_row["program"]["path"],
                           f"n={n} correct cpp O{opt}", "cpp")
                helper = helper_export(
                    args, env, report, out, manifest,
                    out / "llvm" / f"n{n}" / "correct" / f"O{opt}", opt,
                    langc_row["object"]["path"], emitted_correct["visit_symbol"],
                    f"n={n} correct Source O{opt}", check_work=False)
                correct["helper"].append(helper)
                check_float_flags(Path(helper["before"]["path"]), report,
                                  f"n={n} correct Source O{opt} before")
                check_float_flags(Path(helper["after"]["path"]), report,
                                  f"n={n} correct Source O{opt} after")
                if opt == 2:
                    work_scan(args, env, report, Path(helper["after"]["path"]),
                              f"n={n} correct Source O2", n, "langc")
                observe_native(args, env, report, out, case, "correct", opt,
                               helper, case["oracle_bits"])

                wrong = variants["wrong"]
                langc_wrong = build_langc(
                    args, env, out, wrong["manifest"]["path"],
                    out / "build" / f"n{n}" / "wrong" / "langc" / f"O{opt}",
                    opt, f"n={n} wrong langc O{opt}")
                wrong["builds"].append(langc_wrong)
                execute(args, env, wrong, "langc", opt,
                        langc_wrong["program"]["path"], 1,
                        f"n={n} wrong langc O{opt}")
                elf_checks(args, env, report, langc_wrong["program"]["path"],
                           f"n={n} wrong langc O{opt}", "langc")
                cpp_wrong = build_cpp(
                    args, env, out, wrong["reference"]["path"],
                    out / "build" / f"n{n}" / "wrong" / "cpp" / f"O{opt}" /
                    "reference", opt, f"n={n} wrong cpp O{opt}")
                wrong["builds"].append(cpp_wrong)
                execute(args, env, wrong, "cpp", opt,
                        cpp_wrong["program"]["path"], 1,
                        f"n={n} wrong cpp O{opt}")
                elf_checks(args, env, report, cpp_wrong["program"]["path"],
                           f"n={n} wrong cpp O{opt}", "cpp")

                langc_negative = build_langc(
                    args, env, out, negative["manifest"]["path"],
                    out / "build" / f"n{n}" / "negative" / "langc" / f"O{opt}",
                    opt, f"n={n} negative langc O{opt}")
                negative["builds"].append(langc_negative)
                execute(args, env, negative, "langc", opt,
                        langc_negative["program"]["path"], 0,
                        f"n={n} negative langc O{opt}")
                elf_checks(args, env, report,
                           langc_negative["program"]["path"],
                           f"n={n} negative langc O{opt}", "langc")
                cpp_negative = build_cpp(
                    args, env, out, negative["reference"]["path"],
                    out / "build" / f"n{n}" / "negative" / "cpp" / f"O{opt}" /
                    "reference", opt, f"n={n} negative cpp O{opt}")
                negative["builds"].append(cpp_negative)
                execute(args, env, negative, "cpp", opt,
                        cpp_negative["program"]["path"], 0,
                        f"n={n} negative cpp O{opt}")
                elf_checks(args, env, report, cpp_negative["program"]["path"],
                           f"n={n} negative cpp O{opt}", "cpp")
                negative_helper = helper_export(
                    args, env, report, out, negative["manifest"]["path"],
                    out / "llvm" / f"n{n}" / "negative" / f"O{opt}", opt,
                    langc_negative["object"]["path"],
                    emitted_negative["visit_symbol"],
                    f"n={n} negative Source O{opt}", check_work=False)
                negative["helper"].append(negative_helper)
                check_float_flags(Path(negative_helper["before"]["path"]),
                                  report,
                                  f"n={n} negative Source O{opt} before")
                check_float_flags(Path(negative_helper["after"]["path"]),
                                  report,
                                  f"n={n} negative Source O{opt} after")
                observe_native(args, env, report, out, case, "negative", opt,
                               negative_helper,
                               negative["embedded_expected_bits"])

            for opt in (0, 2):
                ir = compile_cpp_ir(
                    args, env, report, out, reference, opt,
                    f"n={n} cpp")
                case["cpp_ir"][f"O{opt}"] = ir
                check_float_flags(Path(ir["ir"]["path"]), report,
                                  f"n={n} cpp O{opt} IR")
                if opt == 2:
                    work_scan(args, env, report, Path(ir["ir"]["path"]),
                              f"n={n} correct C++ O2", n, "cpp")
            linkage = cpp_visit_linkage(
                case["cpp_ir"]["O0"]["text"], f"n={n} cpp O0")
            for opt in (0, 2):
                ir = case["cpp_ir"][f"O{opt}"]
                ir["visit_linkage"] = linkage
                ir["visit_calls"] = cpp_call_count(ir["text"], linkage)
                del ir["text"]
            need(case["cpp_ir"]["O0"]["visit_calls"] > 0,
                 f"n={n}: C++ O0 positive mangled visit-call control failed")
            need(case["cpp_ir"]["O2"]["visit_calls"] == 0,
                 f"n={n}: C++ O2 still has actual visit calls")

        report["tools_after"] = {
            name: file_rec(path) for name, path in {
                "langc": args.langc, "helper": args.helper,
                "runtime": args.runtime, "clang": args.llvm_bin / "clang",
                "clang++": args.llvm_bin / "clang++",
                "ld.lld": args.llvm_bin / "ld.lld",
                "opt": args.llvm_bin / "opt",
                "llvm-readelf": args.llvm_bin / "llvm-readelf",
                "llvm-nm": args.llvm_bin / "llvm-nm",
                "python": args.python,
                "python-helper": args.python_helper,
            }.items()
        }
        report["inputs_after"] = input_snapshot(report, out)
        report["tools_unchanged"] = (
            report["tools_before"] == report["tools_after"])
        report["inputs_unchanged"] = (
            report["inputs_before"] == report["inputs_after"])
        report["oracle_unchanged"] = (
            common.sha256_file(args.oracle) == MATMUL_ORACLE_SHA256
            and Path(report["oracle"]["frozen_copy"]["path"]).read_bytes()
            == Path(args.oracle).read_bytes())
        need(report["tools_unchanged"], "tool SHA-256 values changed")
        need(report["inputs_unchanged"], "input SHA-256 values changed")
        need(report["oracle_unchanged"],
             "the frozen independent matmul oracle changed")
        repositories_after = {
            "worktree": common.repo_info(WORKTREE, env, args.cpu),
            "compiler_root": common.repo_info(args.compiler_root, env,
                                               args.cpu),
        }
        report["repositories_after"] = repositories_after
        report["repositories_unchanged"] = all(
            (report["repositories"][name]["head"],
             report["repositories"][name]["dirty"],
             report["repositories"][name]["status_sha256"])
            == (repositories_after[name]["head"],
                repositories_after[name]["dirty"],
                repositories_after[name]["status_sha256"])
            for name in ("worktree", "compiler_root"))
        report["repository_note"] = (
            "recorded before/after only; unrelated commits in the shared main "
            "compiler checkout do not change this run's recorded tool/input "
            "SHA-256 values, and any real tool/input change fails prepare")

        cases = report["cases"]
        report["observers"] = {
            "native": [row for case in cases
                       for row in case["native_observers"]],
            "python": [row for case in cases
                       for row in case["python_observers"]],
            "separate_from_program_executions": True,
            "host_libc_dependent_native": True,
        }
        executions = [row for case in cases
                      for variant in case["variants"].values()
                      for row in variant["executions"]]
        builds = [row for case in cases
                  for variant in case["variants"].values()
                  for row in variant["builds"]]
        helpers = [row for case in cases
                   for variant in case["variants"].values()
                   for row in variant["helper"]]
        native = [row for case in cases for row in case["native_observers"]]
        python_obs = [row for case in cases
                      for row in case["python_observers"]]
        wrong_exec = [row for row in executions if row["expected_exit"] == 1]
        quiet0 = sum(1 for row in executions if row["success"]
                     and row["exit"] == 0 and not row["stdout"]
                     and not row["stderr"])
        quiet1 = sum(1 for row in executions if row["success"]
                     and row["exit"] == 1 and not row["stdout"]
                     and not row["stderr"])
        works = report["work_analyses"]
        summary = {
            "cases": len(cases), "n_values": list(N_VALUES),
            "variants": sum(len(case["variants"]) for case in cases),
            "program_executions": len(executions),
            "quiet_exit_0": quiet0, "quiet_exit_1": quiet1,
            "wrong_program_executions": len(wrong_exec),
            "wrong_program_quiet_exit_1": quiet1,
            "wrong_non_first_cases": sum(
                1 for case in cases
                if case["variants"]["wrong"]["mutation_index"] not in (None, 0)),
            "wrong_non_first_executions": sum(
                1 for case in cases
                if case["variants"]["wrong"]["mutation_index"] not in (None, 0)
                for row in case["variants"]["wrong"]["executions"]),
            "wrong_non_first_quiet_exit_1": sum(
                1 for case in cases
                if case["variants"]["wrong"]["mutation_index"] not in (None, 0)
                for row in case["variants"]["wrong"]["executions"]
                if row["success"] and row["exit"] == 1
                and not row["stdout"] and not row["stderr"]),
            "negative_last_element_cases": sum(
                1 for case in cases
                if case["variants"]["negative"]["mutation_index"]
                == case["n"] * case["n"] - 1),
            "builds": len(builds),
            "source_builds": sum(1 for row in builds
                                 if row["implementation"] == "langc"),
            "cpp_builds": sum(1 for row in builds
                              if row["implementation"] == "cpp"),
            "source_helper_exports": len(helpers),
            "source_object_matches": sum(1 for row in helpers
                                         if row["object_match"]),
            "source_ir_verifies": 2 * len(helpers),
            "cpp_ir_exports": 2 * len(cases),
            "cpp_ir_verifies": 2 * len(cases),
            "instrumented_observers": len(native),
            "instrumented_ir_verifies": len(native),
            "all_ir_verifies": len(report["verifies"]),
            "native_observers": len(native),
            "native_observer_matches": sum(1 for row in native
                                           if row["success"]),
            "native_negative_rejections": sum(
                1 for row in native
                if row["observer_control"] == "negative-rejected"),
            "python_emits": sum(len(case["python"]) for case in cases),
            "python_observers": len(python_obs),
            "python_observer_matches": sum(1 for row in python_obs
                                           if row["success"]),
            "python_negative_rejections": sum(
                1 for row in python_obs
                if row["observer_control"] == "negative-rejected"),
            "source_o0_positive_visit_cases": sum(
                1 for row in helpers if row["opt"] == 0
                and row["visit_calls_before"] > 0
                and row["visit_calls_after"] > 0),
            "source_o2_zero_visit_cases": sum(
                1 for row in helpers if row["opt"] == 2
                and row["visit_calls_before"] > 0
                and row["visit_calls_after"] == 0),
            "source_o2_visit_calls": sum(
                row["visit_calls_after"] for row in helpers
                if row["opt"] == 2),
            "cpp_o0_positive_visit_cases": sum(
                1 for case in cases
                if case["cpp_ir"]["O0"]["visit_calls"] > 0),
            "cpp_o2_zero_visit_cases": sum(
                1 for case in cases
                if case["cpp_ir"]["O2"]["visit_calls"] == 0),
            "cpp_o2_visit_calls": sum(
                case["cpp_ir"]["O2"]["visit_calls"] for case in cases),
            "work_analyses": len(works),
            "real_loop_work_cases": sum(1 for row in works
                                        if row["real_loop_work"]),
            "max_n_real_loop_work": sum(
                1 for row in works if row["n"] == MAX_N
                and row["real_loop_work"]),
            "small_n_constant_folded": sum(
                1 for row in works if row["n"] != MAX_N
                and row["constant_folded"]),
            "float_flag_checks": len(report["float_flag_checks"]),
            "float_flag_checks_passed": sum(
                1 for row in report["float_flag_checks"] if row["success"]),
            "small_n_constant_folding_correctness_only": True,
            "source_elf_static_checks": sum(
                1 for row in report["elf_checks"]
                if row["implementation"] == "langc"),
            "source_elf_static_checks_passed": sum(
                1 for row in report["elf_checks"]
                if row["implementation"] == "langc"
                and row.get("source_static_ok")),
            "cpp_dynamic_elf_checks": sum(
                1 for row in report["elf_checks"]
                if row["implementation"] == "cpp"),
            "cpp_dynamic_elf_checks_passed": sum(
                1 for row in report["elf_checks"]
                if row["implementation"] == "cpp"
                and row.get("cpp_dynamic_ok")),
            "oracle_cases": len(cases),
            "oracle_products_exact_checked": sum(
                case["n"] ** 3 for case in cases),
            "tools_unchanged": report["tools_unchanged"],
            "inputs_unchanged": report["inputs_unchanged"],
            "oracle_unchanged": report["oracle_unchanged"],
            "repositories_unchanged": report["repositories_unchanged"],
            "timing_performed": False,
        }
        need(summary["cases"] == len(N_VALUES),
             f"summary cases={summary['cases']}")
        need(summary["variants"] == 12,
             f"summary variants={summary['variants']}")
        need(summary["program_executions"] == 48
             and summary["wrong_program_executions"] == 16,
             "program execution count changed")
        need(summary["quiet_exit_0"] == 32 and summary["quiet_exit_1"] == 16
             and summary["wrong_program_quiet_exit_1"] == 16,
             "quiet exit counts changed")
        need(summary["wrong_non_first_cases"] == 3
             and summary["wrong_non_first_executions"] == 12
             and summary["wrong_non_first_quiet_exit_1"] == 12
             and summary["negative_last_element_cases"] == 4,
             "non-first wrong-value / last-element negative counts changed")
        need(summary["builds"] == 48 and summary["source_builds"] == 24
             and summary["cpp_builds"] == 24,
             "build counts changed")
        need(summary["source_helper_exports"] == 16
             and summary["source_object_matches"] == 16
             and summary["source_ir_verifies"] == 32,
             "Source helper/object/IR counts changed")
        need(summary["cpp_ir_exports"] == 8 and summary["cpp_ir_verifies"] == 8
             and summary["all_ir_verifies"] == 56,
             "C++/all IR verify counts changed")
        need(summary["instrumented_observers"] == 16
             and summary["instrumented_ir_verifies"] == 16,
             "instrumented observer counts changed")
        need(summary["native_observers"] == 16
             and summary["native_observer_matches"] == 16
             and summary["native_negative_rejections"] == 8,
             "Native observer counts changed")
        need(summary["python_emits"] == 8
             and summary["python_observers"] == 8
             and summary["python_observer_matches"] == 8
             and summary["python_negative_rejections"] == 4,
             "Python observer counts changed")
        need(summary["source_o0_positive_visit_cases"] == 8
             and summary["source_o2_zero_visit_cases"] == 8
             and summary["source_o2_visit_calls"] == 0,
             "Source visit-call counts changed")
        need(summary["cpp_o0_positive_visit_cases"] == 4
             and summary["cpp_o2_zero_visit_cases"] == 4
             and summary["cpp_o2_visit_calls"] == 0,
             "C++ visit-call counts changed")
        need(summary["work_analyses"] == 8
             and summary["max_n_real_loop_work"] == 2,
             "O2 real-work counts changed")
        need(summary["float_flag_checks"] == 40
             and summary["float_flag_checks_passed"] == 40,
             "fast-math/FMA flag check counts changed")
        need(summary["source_elf_static_checks"] == 24
             and summary["source_elf_static_checks_passed"] == 24
             and summary["cpp_dynamic_elf_checks"] == 24
             and summary["cpp_dynamic_elf_checks_passed"] == 24,
             "ELF check counts changed")
        need(summary["oracle_cases"] == 4
             and summary["oracle_products_exact_checked"] == 4467,
             "oracle case/product counts changed")
        need(summary["tools_unchanged"] is True
             and summary["inputs_unchanged"] is True
             and summary["oracle_unchanged"] is True,
             "tool/input/oracle stability changed")
        need(summary["timing_performed"] is False, "timing must not run")
        report["summary"] = summary
        report["deliverable"] = {
            "worktree": str(WORKTREE),
            "worktree_head": report["repositories"]["worktree"]["head"],
            "files": [file_rec(PERFORMANCE_DIR / name)
                      for name in (*TEMPLATE_NAMES, *OBSERVER_SOURCES,
                                   "prepare.py", "README.md")],
            "patch": write_patch(out, args, env),
        }
        report["limitations"] = limitations()
        report["not_run"] = not_run()
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["failure"] = {"type": type(error).__name__,
                             "message": str(error),
                             "traceback": traceback.format_exc()}
        report.setdefault("limitations", limitations())
        report.setdefault("not_run", not_run())
    finally:
        report["finished_at"] = now()
        report["loadavg_after"] = common.host_info()["loadavg"]
        common.write_json(out / "prepare.json", report)
        common._COMMAND_LOG = None
        print(json.dumps({
            "status": report["status"], "mode": "prepare", "out": str(out),
            "report": str(out / "prepare.json"),
            "summary": report.get("summary"),
            "failure": report.get("failure"),
        }, indent=2, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


def limitations():
    return [
        "No timing, throughput, speedup, or <=2x comparison is performed or claimed; command elapsed_ns is diagnostic only.",
        "The formal programs print nothing; correctness is the complete per-element matrix comparison with the frozen independent oracle, and observers compare every binary64 bit pattern.",
        "Native observation is a test-only instrumented binary linked with the host libc and the project runtime; it is not a standalone Native performance program.",
        "The Python observer imports the unmodified generated module and records the actual Vec[f64] received by the empty visit; it does not recompute the matmul.",
        "The Native observer is injected only into the exact metadata visit SymbolId whose exported pre-optimization LLVM definition has one real ptr argument; an unknown symbol fails the run instead of counting zero.",
        "Only N=1,3,7,16 and the fixed x86_64 generic target are covered. The source programs use no SIMD, threads, BLAS, LTO, fast-math, or FMA.",
        "C++ is built with -ffp-contract=off, -fno-fast-math, -fno-vectorize, and -fno-slp-vectorize so the max-N O2 reference keeps a scalar f64 multiply/add/load/store loop.",
        "For C++ N=1 and N=3, clang -O2 constant-folds the small fixed matrix work; those cases are correctness-only and are not performance evidence. Source keeps its runtime Vec loops. Every exported Source before/after IR and C++ O0/O2 IR is scanned for fast/contract float flags and FMA intrinsics.",
        "The N=1 case has only one element, so its last-element mutation is also the first element; N=3,7,16 have a non-first last element.",
        "Observer binaries depend on the host libc and the project runtime and are recorded separately; they are never used for timing or performance conclusions.",
        "The frozen oracle JSON is read-only outside this fixture; prepare verifies its recorded SHA-256 before and after the run and never derives expected values from Source or C++.",
    ]


def not_run():
    return [
        {"item": "formal_timing", "reason": "preparation only"},
        {"item": "other_n_values", "reason": "fixed N=1,3,7,16"},
        {"item": "simd_or_multithreaded_variant", "reason": "single-thread scalar fixture only"},
        {"item": "formal_native_performance_program_from_observer", "reason": "observer binaries depend on host libc and are test-only"},
        {"item": "other_targets_or_compilers", "reason": "fixed x86_64 generic target and recorded LLVM 22.1.8"},
        {"item": "cpp_negative_observer", "reason": "C++ has O0/O2 IR call recognition and actual quiet runs; bit-pattern observation is Source Native plus generated Python"},
    ]


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path,
                        help="new output directory; must not exist")
    parser.add_argument("--oracle", type=Path, default=DEFAULT_ORACLE)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--python-helper", type=Path,
                        default=DEFAULT_PYTHON_HELPER)
    parser.add_argument("--woozyc", dest="langc", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--compiler-root", type=Path, required=True)
    parser.add_argument("--ld-library-path", type=Path)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--run-timeout", type=float, default=120.0)
    return parser.parse_args(argv)


def validate_args(args):
    need(args.out is not None, "--out is required")
    for label, path in (
        ("oracle", args.oracle),
        ("python", args.python), ("python-helper", args.python_helper),
        ("langc", args.langc), ("helper", args.helper),
        ("runtime", args.runtime),
        ("llvm-bin/clang", args.llvm_bin / "clang"),
        ("llvm-bin/clang++", args.llvm_bin / "clang++"),
        ("llvm-bin/ld.lld", args.llvm_bin / "ld.lld"),
        ("llvm-bin/opt", args.llvm_bin / "opt"),
        ("llvm-bin/llvm-readelf", args.llvm_bin / "llvm-readelf"),
        ("llvm-bin/llvm-nm", args.llvm_bin / "llvm-nm"),
    ):
        need(path.is_file(), f"{label} is missing: {path}")
    need(args.compiler_root.is_dir(),
         f"compiler-root is missing: {args.compiler_root}")
    if args.ld_library_path is not None:
        need(args.ld_library_path.is_dir(),
             f"--ld-library-path is not a directory: {args.ld_library_path}")
    need(args.timeout > 0 and args.run_timeout > 0,
         "timeouts must be positive")


def main(argv=None):
    try:
        args = parse_args(argv)
        validate_args(args)
        return prepare(args)
    except Exception as error:
        print(json.dumps({"status": "failed", "type": type(error).__name__,
                          "message": str(error),
                          "traceback": traceback.format_exc()},
                         indent=2, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    sys.exit(main())
