#!/usr/bin/env python3
"""Prepare the fixed N=128/512 matmul rounds fixture without timing.

The existing small-scale templates, prepare script, Source/C++ files, and
oracle bytes are unchanged inputs.  This script loads them through the existing
renderer/helpers, replaces only the exact old ``main`` text when a rounds
wrapper is requested, and keeps the algorithm prefix byte-for-byte.

The frozen independent large oracle is hash-checked and every A/B/expected
element is verified against the fixed formulas and exact integer dot products
/64 before any Source or C++ output is used.  Each run writes real Source/C++
O0/O2 build, run, LLVM IR, object, ELF, observer, and round/kernel control
records to a new output directory.  Source correctness is observed over the
complete C matrix with the existing test-only host observer.  C++ correctness
is also observed over the complete C matrix for every round through a separate
auxiliary copy whose empty visit body calls the small host observer; the
formal C++ source, the formal O2 program, and its in-program comparison remain
unchanged.  No timing, speedup, R calibration, or <=2x claim is made.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import re
import struct
import sys
import traceback
from fractions import Fraction
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_native as common  # noqa: E402

PERFORMANCE_DIR = Path(__file__).resolve().parent


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load shared helpers: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


legacy = _load("_matmul_large_reuse_legacy_prepare",
               PERFORMANCE_DIR / "prepare.py")
fannkuch = _load("_matmul_large_reuse_fannkuch_large_prepare",
                 PERFORMANCE_DIR.parent / "fannkuch" / "large_prepare.py")
mandel = legacy.mandel
sieve = legacy.sieve

need = legacy.need
now = legacy.now
file_rec = legacy.file_rec
manifest_text = legacy.manifest_text
tool = legacy.tool
run_program = legacy.run_program
verify_ir = legacy.verify_ir
build_langc = legacy.build_langc
emit_metadata = legacy.emit_python
helper_export = legacy.helper_export
parse_functions = legacy.parse_functions
elf_checks = sieve.elf_checks
decode_hex = legacy.decode_hex
sha256_bytes = legacy.sha256_bytes
bits_of = legacy.bits_of
bits_sha256 = legacy.bits_sha256
bits_bytes = legacy.bits_bytes
exact_decimal = legacy.exact_decimal
decimal_fraction = legacy.decimal_fraction
render = legacy.render
MARKER_RE = legacy.MARKER_RE
FAST_FLOAT_RE = legacy.FAST_FLOAT_RE
FMA_RE = legacy.FMA_RE

CONTRACT = "compiler-dev/1"
TASK = "native-matmul-large-preparation"
OPERATION_ID = "language-matmul-large-preparation-20261110"
CORRECTION_OPERATION_ID = "language-matmul-large-correction1-20261110"
INHERITED_SCRIPT_SHA256 = (
    "112ec990e1f3ef41ad2fc76d137bbbd80cd90884d9ed6ee13ed6a7ecd43f3933")
PREPARE_SCHEMA = "compiler-dev/native-matmul-large-preparation/1"
DEFAULT_ORACLE_DIR = Path(
    "/home/qin/Work/language-dev-20261103/checks/"
    "eight-algorithm-large-oracles-20261110")
DEFAULT_PYTHON_HELPER = Path(
    "/home/qin/Work/language-dev-20261103/compiler/build/native-cli-release/"
    "tests/python/lang_python_emit")
N_VALUES = (128, 512)
RESULTS_SHA256 = (
    "e5a58627690ce535086cc3477e819e1f2a45dcb0bebd1ffe2a100b7b4e8f366c")
ORACLE_SCRIPT_SHA256 = (
    "1fce6ad25bfd20c0d0c7d73811547d992f7a090bf752eb571ee2939a7a4cf92b")
PINNED_MATRIX = {
    128: {
        "a": ("3b530dd141d54142fd07d1f8f525a053f9dd28bc8dd0c6db5df3df5e50ec1205",
              131072),
        "b": ("147e7f7ead2d141367c4e708adb223fa4af61e1b444ddd4c00fdc164ed98e2f2",
              131072),
        "expected": (
            "d4ea60022864378efdfdd25712bfaf28e92b49c24e4b60f68a29622d1a13d432",
            131072),
    },
    512: {
        "a": ("a350df939d6b8a91c6706eaf3604a5ae9cafed58abd406fb16a493652f7f4d0b",
              2097152),
        "b": ("9f4e351c0cb3af24c86cde90064b41972b86722752154fecb34d6dda1273c28f",
              2097152),
        "expected": (
            "b5e715b5ccfe8fa2d894d5872f087e778dc4376d425bebf4a22a63b1ef9e7a0e",
            2097152),
    },
}
TARGET_FLAGS = mandel.TARGET_FLAGS
TEMPLATE_NAMES = ("matmul.wz.in", "reference.cpp.in")
VARIANT_EXIT = {"correct": 0, "wrong": 1}

SOURCE_MAIN = """func main() i32 {
    // Read the size through a one-element runtime vector so the optimized
    // Source IR keeps the actual scalar work.
    let dims Vec[i32] = [{{N}}];
    let n i32 = dims[0];
    let expected Vec[f64] = [{{EXPECTED}}];
    let c = multiply(n);

    // Full per-element comparison with the embedded expected matrix.
    let total i32 = {{NN}};
    for var index i32 = 0; index < total; index += 1 {
        if c[index] != expected[index] {
            return 1;
        }
    }

    visit(c);
    return 0;
}
"""
CPP_MAIN = """int main() {
    // Mirror the Source one-element runtime size vector.
    const std::vector<int32_t> dims{ {{N}} };
    const int32_t n = dims[0];
    const std::vector<double> expected{ {{EXPECTED}} };
    const std::vector<double> c = multiply(n);

    // Full per-element comparison with the embedded expected matrix.
    const int32_t total = {{NN}};
    for (int32_t index = 0; index < total; ++index) {
        if (c[static_cast<std::size_t>(index)] !=
            expected[static_cast<std::size_t>(index)]) {
            return 1;
        }
    }

    visit(c);
    return 0;
}
"""
CPP_OBSERVER_SOURCE = "cpp_observer.cpp"
CPP_VISIT_EMPTY = """static void visit(const std::vector<double>& values) {
    (void)values;
}"""
CPP_VISIT_AUX = """extern "C" void matmul_observe_cpp(const double*, std::size_t);

static void visit(const std::vector<double>& values) {
    matmul_observe_cpp(values.data(), values.size());
}"""

FUNC_HEAD_RE = re.compile(
    r"^define\b[^\n]*?@([A-Za-z0-9_.$]+)\([^\n]*\{\s*$", re.MULTILINE)
FUNC_END_RE = re.compile(r"^\}\s*$", re.MULTILINE)
BLOCK_LABEL_RE = re.compile(r"^([A-Za-z0-9_.$]+):", re.MULTILINE)
DOUBLE_BINOP_RE = re.compile(
    r"\b(?:fmul|fadd)\s+(?:fast\s+)?(?:double|<[^>]*\bdouble>)")
DOUBLE_FMUL_RE = re.compile(
    r"\bfmul\s+(?:fast\s+)?(?:double|<[^>]*\bdouble>)")
DOUBLE_FADD_RE = re.compile(
    r"\bfadd\s+(?:fast\s+)?(?:double|<[^>]*\bdouble>)")
DOUBLE_FCMP_RE = re.compile(
    r"\bfcmp\s+(?:fast\s+)?(?:[a-z]+\s+)?(?:double|<[^>]*\bdouble>)")
INIT_RE = re.compile(r"\b(?:call|invoke)\b[^\n]*@lang_vec_init_v1\(")
NEW_RE = re.compile(r"\b(?:call|invoke)\b[^\n]*@lang_vec_new_v1\(")
HEAP_RE = re.compile(r"\b(?:call|invoke)\b[^\n]*@_Znwm\(")


def template_records():
    return {name: file_rec(PERFORMANCE_DIR / name) for name in (
        *TEMPLATE_NAMES, "prepare.py", "README.md", "native_observer.cpp",
        CPP_OBSERVER_SOURCE, "visit_trace.py", "large_prepare.py")}


def read_templates():
    return {name: (PERFORMANCE_DIR / name).read_text(encoding="utf-8")
            for name in TEMPLATE_NAMES}


def load_oracle(path, out):
    """Hash-check the frozen large oracle and freeze byte-identical copies."""
    root = Path(path).resolve()
    results_path = root / "results.json"
    script_path = root / "prepare.py"
    need(results_path.is_file(), f"missing frozen oracle: {results_path}")
    need(script_path.is_file(), f"missing frozen oracle script: {script_path}")
    need(common.sha256_file(results_path) == RESULTS_SHA256,
         f"{results_path}: results.json SHA-256 is not pinned")
    document = json.loads(results_path.read_text(encoding="utf-8"))
    need(list(document) == ["status", "formal_timing", "cases",
                            "script_sha256", "input_sources_unchanged"],
         f"{results_path}: unexpected top-level keys {list(document)}")
    need(document.get("status") == "passed"
         and document.get("formal_timing") is False
         and document.get("input_sources_unchanged") is True,
         f"{results_path}: frozen oracle status/claims are not pinned")
    need(document.get("script_sha256") == ORACLE_SCRIPT_SHA256
         and common.sha256_file(script_path) == ORACLE_SCRIPT_SHA256,
         f"{script_path}: oracle script SHA-256 is not pinned")
    cases = document.get("cases")
    need(isinstance(cases, list), f"{results_path}: cases is not a list")
    records = {}
    originals = [results_path.resolve(), script_path.resolve()]
    for record in cases:
        need(isinstance(record, dict), f"{results_path}: case is not an object")
        if record.get("algorithm") != "matmul":
            continue
        n, role = record.get("n"), record.get("role")
        need(n in N_VALUES and role in ("a", "b", "expected"),
             f"{results_path}: unexpected matmul case {record!r}")
        need((n, role) not in records,
             f"{results_path}: duplicate matmul n={n} role={role}")
        matrix = Path(record["path"])
        need(matrix.is_file(), f"{matrix}: frozen matrix file is missing")
        need(matrix.resolve().parent == root,
             f"{matrix}: frozen matrix is outside {root}")
        want_sha, want_bytes = PINNED_MATRIX[n][role]
        need(record.get("sha256") == want_sha
             and common.sha256_file(matrix) == want_sha,
             f"{matrix}: SHA-256 is not pinned")
        need(record.get("bytes") == want_bytes
             and matrix.stat().st_size == want_bytes,
             f"{matrix}: byte count is not pinned")
        need(want_bytes == n * n * 8, f"{matrix}: byte count is not n*n*8")
        method = record.get("method", "")
        need("integer dot products /64" in method
             and "exactly representable f64" in method,
             f"{matrix}: method is not exact integer dot products /64")
        records[f"n{n}-{role}"] = {
            "path": str(matrix.resolve()), "sha256": want_sha,
            "bytes": want_bytes}
        originals.append(matrix.resolve())
    wanted = {(n, role) for n in N_VALUES for role in ("a", "b", "expected")}
    need({(int(key.split("-")[0][1:]), key.split("-")[1])
          for key in records} == wanted,
         f"{results_path}: matmul record set is incomplete")

    frozen_dir = out / "oracle"
    frozen_dir.mkdir(parents=True, exist_ok=False)
    copies = {}
    for source, name in ((results_path, "results.json"),
                         (script_path, "prepare.py")):
        target = frozen_dir / name
        target.write_bytes(source.read_bytes())
        need(common.sha256_file(target) == common.sha256_file(source),
             f"{target}: frozen copy changed")
    for key, record in records.items():
        source = Path(record["path"])
        target = frozen_dir / source.name
        target.write_bytes(source.read_bytes())
        need(common.sha256_file(target) == record["sha256"],
             f"{target}: frozen matrix copy changed")
        copies[key] = file_rec(target, out)
    return {
        "results_path": str(results_path.resolve()),
        "results_sha256": RESULTS_SHA256,
        "script_path": str(script_path.resolve()),
        "script_sha256": ORACLE_SCRIPT_SHA256,
        "status": document["status"],
        "formal_timing": document["formal_timing"],
        "input_sources_unchanged": document["input_sources_unchanged"],
        "records": records, "frozen_copies": copies,
        "files": [str(path) for path in originals],
    }, originals + [Path(record["path"]) for record in copies.values()]


def canonical_tables(n):
    a_num = [[(17 * i + 13 * j) % 19 - 9 for j in range(19)]
             for i in range(19)]
    b_num = [[(11 * i + 7 * j) % 23 - 11 for j in range(23)]
             for i in range(23)]
    a_bits, b_bits, c_bits = [], [], []
    a_float, b_float, c_float = [], [], []
    a_dec, b_dec, c_dec, c_num = [], [], [], []
    for i in range(19):
        a_bits.append([])
        a_float.append([])
        a_dec.append([])
        for j in range(19):
            value = Fraction(a_num[i][j], 8)
            a_bits[-1].append(int(bits_of(value, f"a[{i}][{j}]"), 16))
            a_float[-1].append(float(value))
            a_dec[-1].append(exact_decimal(value, f"a[{i}][{j}]"))
    for i in range(23):
        b_bits.append([])
        b_float.append([])
        b_dec.append([])
        for j in range(23):
            value = Fraction(b_num[i][j], 8)
            b_bits[-1].append(int(bits_of(value, f"b[{i}][{j}]"), 16))
            b_float[-1].append(float(value))
            b_dec[-1].append(exact_decimal(value, f"b[{i}][{j}]"))
    for i in range(19):
        c_bits.append([])
        c_float.append([])
        c_dec.append([])
        c_num.append([])
        for j in range(23):
            total = sum(a_num[i][k % 19] * b_num[k % 23][j]
                        for k in range(n))
            value = Fraction(total, 64)
            c_num[-1].append(total)
            c_bits[-1].append(int(bits_of(value, f"c[{i}][{j}]"), 16))
            c_float[-1].append(float(value))
            c_dec[-1].append(exact_decimal(value, f"c[{i}][{j}]"))
    return {"a_num": a_num, "b_num": b_num, "c_num": c_num,
            "a_bits": a_bits, "b_bits": b_bits, "c_bits": c_bits,
            "a_float": a_float, "b_float": b_float, "c_float": c_float,
            "a_dec": a_dec, "b_dec": b_dec, "c_dec": c_dec}


def verify_oracle_matrices(n, records):
    def read(role):
        data = Path(records[(n, role)]["path"]).read_bytes()
        need(len(data) == n * n * 8, f"n={n} {role}: wrong byte count")
        return (list(struct.unpack(f"<{n * n}Q", data)), data,
                list(struct.unpack(f"<{n * n}d", data)))

    a_file_bits, a_data, a_file_values = read("a")
    b_file_bits, b_data, b_file_values = read("b")
    c_file_bits, c_data, c_file_values = read("expected")
    tables = canonical_tables(n)
    full = {
        "a": ([tables["a_bits"][i % 19][j % 19]
               for i in range(n) for j in range(n)],
              [tables["a_float"][i % 19][j % 19]
               for i in range(n) for j in range(n)]),
        "b": ([tables["b_bits"][i % 23][j % 23]
               for i in range(n) for j in range(n)],
              [tables["b_float"][i % 23][j % 23]
               for i in range(n) for j in range(n)]),
        "expected": ([tables["c_bits"][i % 19][j % 23]
                      for i in range(n) for j in range(n)],
                     [tables["c_float"][i % 19][j % 23]
                      for i in range(n) for j in range(n)]),
    }
    need(a_file_bits == full["a"][0], f"n={n}: complete A bits differ from formula")
    need(b_file_bits == full["b"][0], f"n={n}: complete B bits differ from formula")
    need(c_file_bits == full["expected"][0],
         f"n={n}: complete expected bits differ from exact integer dot products")
    need(a_file_values == full["a"][1], f"n={n}: complete A values differ")
    need(b_file_values == full["b"][1], f"n={n}: complete B values differ")
    need(c_file_values == full["expected"][1],
         f"n={n}: complete expected values differ from exact dot products")

    tokens = 0
    for i in range(19):
        for j in range(19):
            value = Fraction(tables["a_num"][i][j], 8)
            token = tables["a_dec"][i][j]
            need(decimal_fraction(token, "a") == value
                 and int(bits_of(decimal_fraction(token, "a"), "a"), 16)
                 == tables["a_bits"][i][j],
                 f"n={n}: A decimal token does not round trip")
            tokens += 1
    for i in range(23):
        for j in range(23):
            value = Fraction(tables["b_num"][i][j], 8)
            token = tables["b_dec"][i][j]
            need(decimal_fraction(token, "b") == value
                 and int(bits_of(decimal_fraction(token, "b"), "b"), 16)
                 == tables["b_bits"][i][j],
                 f"n={n}: B decimal token does not round trip")
            tokens += 1
    for i in range(19):
        for j in range(23):
            value = Fraction(tables["c_num"][i][j], 64)
            token = tables["c_dec"][i][j]
            need(decimal_fraction(token, "c") == value
                 and int(bits_of(decimal_fraction(token, "c"), "c"), 16)
                 == tables["c_bits"][i][j],
                 f"n={n}: expected decimal token does not round trip")
            tokens += 1
    need(len(a_data) == len(b_data) == len(c_data) == n * n * 8,
         f"n={n}: matrix byte sizes changed")
    return tables, {
        "n": n, "elements": n * n,
        "a_formula": "((17*i + 13*j) % 19 - 9) / 8",
        "b_formula": "((11*i + 7*j) % 23 - 11) / 8",
        "c_formula": "exact integer dot product of A and B numerators / 64",
        "full_a_bits_checked": n * n,
        "full_b_bits_checked": n * n,
        "full_expected_bits_checked": n * n,
        "full_a_values_checked": n * n,
        "full_b_values_checked": n * n,
        "full_expected_values_checked": n * n,
        "distinct_a_values": 19 * 19,
        "distinct_b_values": 23 * 23,
        "distinct_expected_values": 19 * 23,
        "distinct_decimal_tokens_checked": tokens,
        "unique_dot_products_checked": 19 * 23,
        "dot_product_terms_checked": 19 * 23 * n,
        "row_period": 19, "column_period": 23,
        "expected_file_bits_equal_formula": True,
        "expected_file_values_equal_formula": True,
        "all_values_exactly_representable_binary64": True,
        "not_checksum_or_sampling": True,
    }


def build_literals(n, tables):
    def text(table, row_period, column_period):
        return ", ".join(table[i % row_period][j % column_period]
                         for i in range(n) for j in range(n))

    a_text = text(tables["a_dec"], 19, 19)
    b_text = text(tables["b_dec"], 23, 23)
    expected_text = text(tables["c_dec"], 19, 23)
    zeros_text = ", ".join("0.0" for _ in range(n * n))
    expected_bits = [f"{tables['c_bits'][i % 19][j % 23]:016x}"
                     for i in range(n) for j in range(n)]
    expected_bytes = bits_bytes(expected_bits)
    return {
        "a_text": a_text, "b_text": b_text,
        "expected_text": expected_text, "zeros_text": zeros_text,
        "a_literal_sha256": common.sha256_text(a_text),
        "b_literal_sha256": common.sha256_text(b_text),
        "expected_literal_sha256": common.sha256_text(expected_text),
        "zeros_literal_sha256": common.sha256_text(zeros_text),
        "a_literal_bytes": len(a_text.encode()),
        "b_literal_bytes": len(b_text.encode()),
        "expected_literal_bytes": len(expected_text.encode()),
        "zeros_literal_bytes": len(zeros_text.encode()),
        "expected_bits": expected_bits,
        "expected_bits_sha256": bits_sha256(expected_bits),
        "expected_bytes": expected_bytes,
        "full_expected_bits_covered": n * n,
    }


def wrap_main(old_main, language, rounds):
    body = old_main[old_main.index("{") + 1:old_main.rindex("}")]
    need(body.rstrip().endswith("return 0;"),
         f"{language}: old main does not end with return 0")
    body = body[:body.rindex("return 0;")].rstrip()
    need(body.strip(), f"{language}: old main body is empty")
    indented = "\n".join(("    " + line) if line.strip() else ""
                         for line in body.splitlines()) + "\n"
    brace = "func main() i32 {" if language == "source" else "int main() {"
    loop = (f"    for var round i32 = 0; round < {rounds}; round += 1 {{"
            if language == "source"
            else f"    for (int32_t round = 0; round < {rounds}; ++round) {{")
    return f"{brace}\n{loop}\n{indented}    }}\n    return 0;\n}}\n"


def render_program(template, n, rounds, literals, variant, language):
    old_main = SOURCE_MAIN if language == "source" else CPP_MAIN
    need(template.count(old_main) == 1,
         f"{language}: old main is not a unique exact match")
    prefix = template[:template.index(old_main)]
    marker = ("func multiply" if language == "source"
              else "static std::vector<double> multiply")
    need(prefix.count(marker) == 1,
         f"{language}: algorithm function is not in the preserved prefix")
    new_main = old_main if rounds == 1 else wrap_main(old_main, language, rounds)
    nn = n * n
    mutation = ("" if variant == "correct"
                else f"    c[{nn - 1}] = c[{nn - 1}] + 1.0;")
    mapping = {
        "N": n, "NN": nn, "VARIANT": variant,
        "A": literals["a_text"], "B": literals["b_text"],
        "ZEROS": literals["zeros_text"],
        "EXPECTED": literals["expected_text"], "MUTATION": mutation,
        "A_SHA": PINNED_MATRIX[n]["a"][0],
        "B_SHA": PINNED_MATRIX[n]["b"][0],
        "C_SHA": PINNED_MATRIX[n]["expected"][0],
        "EXPECTED_SHA": literals["expected_bits_sha256"],
        "ORACLE_SHA": RESULTS_SHA256,
    }
    text = render(template.replace(old_main, new_main, 1), mapping)
    rendered_prefix = render(prefix, mapping)
    rendered_main = render(new_main, mapping)
    need(text.startswith(rendered_prefix),
         f"{language}: algorithm prefix changed during main replacement")
    need(text[len(rendered_prefix):] == rendered_main,
         f"{language}: rendered main differs from the wrapper")
    forbidden = [token for token in ("asm", "volatile", "noinline",
                                     "optnone", "__attribute__")
                 if token in text]
    need(not forbidden, f"{language}: forbidden source tokens {forbidden}")
    return text, {
        "old_main_occurrences": 1,
        "main_replacement_offset": len(rendered_prefix),
        "algorithm_prefix_sha256": common.sha256_text(rendered_prefix),
        "algorithm_prefix_matches_legacy_render": True,
        "algorithm_prefix_size_bytes": len(rendered_prefix.encode()),
        "old_main_sha256": common.sha256_text(old_main),
        "new_main_sha256": common.sha256_text(rendered_main),
        "wrapper": "none" if rounds == 1 else "round-loop",
        "old_final_return_inside_loop": False,
        "forbidden_tokens": forbidden,
    }


def verify_generated_large(source, reference, n, rounds, variant, literals,
                           marker):
    nn = n * n
    mutation = ("" if variant == "correct"
                else f"    c[{nn - 1}] = c[{nn - 1}] + 1.0;")
    for text, language in ((source, "Source"), (reference, "C++")):
        match = MARKER_RE.search(text)
        need(match is not None, f"{language} marker is missing")
        actual = (int(match.group(1)), match.group(2), match.group(3),
                  match.group(4), match.group(5), match.group(6),
                  match.group(7))
        need(actual == marker, f"{language} marker {actual} != {marker}")
    for snippet in (
        f"let a Vec[f64] = [{literals['a_text']}];",
        f"let b Vec[f64] = [{literals['b_text']}];",
        f"var c Vec[f64] = [{literals['zeros_text']}];",
        f"let expected Vec[f64] = [{literals['expected_text']}];",
        "for var i i32 = 0; i < n; i += 1 {",
        "for var j i32 = 0; j < n; j += 1 {",
        "for var k i32 = 0; k < n; k += 1 {",
        "sum = sum + a[i * n + k] * b[k * n + j];",
        "c[i * n + j] = sum;",
        "if c[index] != expected[index] {",
        "visit(c);",
    ):
        need(snippet in source, f"n={n} {variant}: Source missing {snippet!r}")
    for snippet in (
        f"const std::vector<double> a{{ {literals['a_text']} }};",
        f"const std::vector<double> b{{ {literals['b_text']} }};",
        f"const std::vector<double> expected{{ {literals['expected_text']} }};",
        "std::vector<double> c(",
        "static_cast<std::size_t>(n) * static_cast<std::size_t>(n), 0.0);",
        "for (int32_t i = 0; i < n; ++i) {",
        "for (int32_t j = 0; j < n; ++j) {",
        "for (int32_t k = 0; k < n; ++k) {",
        "a[static_cast<std::size_t>(i * n + k)] *",
        "b[static_cast<std::size_t>(k * n + j)];",
        "c[static_cast<std::size_t>(i * n + j)] = sum;",
        "visit(c);",
    ):
        need(snippet in reference,
             f"n={n} {variant}: C++ missing {snippet!r}")
    if mutation:
        need(source.count(mutation) == 1
             and reference.count(mutation) == 1,
             f"n={n} {variant}: mutation is missing or duplicated")
    else:
        need("c[index] = c[index] + 1.0" not in source
             and "c[index] = c[index] + 1.0" not in reference,
             f"n={n}: correct programs must not mutate C")
    need("Vec[f32]" not in source and "std::vector<float>" not in reference,
         f"n={n} {variant}: unexpected f32 matrix")
    for banned in ("cblas_", "openblas", "#include <thread>", "std::thread",
                   "#pragma omp", "clock_gettime"):
        need(banned not in source.lower() and banned not in reference.lower(),
             f"n={n} {variant}: forbidden API {banned!r} is present")
    if rounds == 1:
        need("for var round" not in source
             and "for (int32_t round" not in reference,
             f"n={n}: R1 must not contain a rounds loop")
    else:
        need("for var round" in source and "for (int32_t round" in reference,
             f"n={n}: rounds wrapper is missing")
    return {"marker": list(marker), "source_snippets_checked": 11,
            "cpp_snippets_checked": 12,
            "mutation_sha256": common.sha256_text(mutation)}


def generate_case(base, templates, n, rounds, literals, variant):
    directory = base / "cases" / f"n{n}" / f"r{rounds}" / variant
    (directory / "src").mkdir(parents=True, exist_ok=False)
    source_text, source_render = render_program(
        templates["matmul.wz.in"], n, rounds, literals, variant, "source")
    reference_text, reference_render = render_program(
        templates["reference.cpp.in"], n, rounds, literals, variant, "cpp")
    marker = (n, variant, PINNED_MATRIX[n]["a"][0],
              PINNED_MATRIX[n]["b"][0], PINNED_MATRIX[n]["expected"][0],
              literals["expected_bits_sha256"], RESULTS_SHA256)
    checks = verify_generated_large(
        source_text, reference_text, n, rounds, variant, literals, marker)
    source = directory / "src" / "main.wz"
    reference = directory / "reference.cpp"
    manifest = directory / "lang.toml"
    source.write_text(source_text, encoding="utf-8")
    reference.write_text(reference_text, encoding="utf-8")
    manifest.write_text(
        manifest_text(f"perf_matmul_large_n{n}_r{rounds}_{variant}"),
        encoding="utf-8")
    expected_bits_path = directory / "expected_bits.txt"
    expected_bits_path.write_text(
        "\n".join(literals["expected_bits"]) + "\n", encoding="ascii")
    return {
        "n": n, "rounds": rounds, "variant": variant,
        "expected_exit": VARIANT_EXIT[variant],
        "mutation_index": None if variant == "correct" else n * n - 1,
        "manifest": file_rec(manifest, base),
        "source": file_rec(source, base),
        "reference": file_rec(reference, base),
        "expected_bits": file_rec(expected_bits_path, base),
        "source_render": source_render,
        "reference_render": reference_render,
        "generated_checks": checks,
        "a_literal_sha256": literals["a_literal_sha256"],
        "b_literal_sha256": literals["b_literal_sha256"],
        "expected_literal_sha256": literals["expected_literal_sha256"],
        "zeros_literal_sha256": literals["zeros_literal_sha256"],
        "a_literal_bytes": literals["a_literal_bytes"],
        "b_literal_bytes": literals["b_literal_bytes"],
        "expected_literal_bytes": literals["expected_literal_bytes"],
        "expected_bits_sha256": literals["expected_bits_sha256"],
        "cpp_full_comparison_elements": n * n,
        "cpp_embedded_expected_matches_frozen": True,
        "builds": [], "executions": [], "helper": [],
        "cpp_ir": {}, "native_observers": [], "cpp_observers": [],
    }


def export_source_ir(args, env, report, out, manifest, out_dir, opt, label):
    out_dir.mkdir(parents=True, exist_ok=False)
    obj, after, before = (out_dir / "program.o", out_dir / "after.ll",
                          out_dir / "before.ll")
    helper = tool(
        [args.helper, "--project", manifest, "--opt", str(opt),
         "--out-object", obj, "--out-llvm", after, "--out-llvm-before", before,
         "--out-native", out_dir / "program.nir", "--format", "json"],
        args, env)
    common.require_success(helper, f"{label} helper")
    helper_json = common.parse_json_stdout(helper, f"{label} helper")
    need(helper_json.get("status") in ("ok", "passed")
         and helper_json.get("phase") == "object"
         and helper_json.get("llvm_verified_before") is True
         and helper_json.get("llvm_verified_after") is True,
         f"{label}: helper did not export and verify both LLVM files")
    common.checked_elf(obj, executable=False, label=f"{label} helper object")
    return {
        "opt": opt, "command_index": helper["command_index"],
        "json": helper_json, "object": file_rec(obj, out),
        "before": file_rec(before, out), "after": file_rec(after, out),
        "verify_before": verify_ir(before, args, env, report,
                                   f"{label} before verify"),
        "verify_after": verify_ir(after, args, env, report,
                                  f"{label} after verify"),
    }


def compile_cpp_ir_large(args, env, report, out, reference, opt, label):
    slug = re.sub(r"[^A-Za-z0-9_.-]+", "-", label)
    output = out / "llvm" / slug / f"O{opt}.ll"
    output.parent.mkdir(parents=True, exist_ok=True)
    command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         "-g", "-S", "-emit-llvm", reference, "-o", output], args, env)
    common.require_success(command, f"{label} IR build")
    return {"command_index": command["command_index"], "opt": opt,
            "ir": file_rec(output, out),
            "verify": verify_ir(output, args, env, report, f"{label} verify"),
            "path": str(output)}


def execute(args, env, case, implementation, opt, program, expected, label):
    run = run_program([program], args, env)
    row = {"implementation": implementation, "opt": opt,
           "expected_exit": expected, "command_index": run["command_index"],
           "success": False}
    case["executions"].append(row)
    need(run["status"] == "exited" and run["exit"] == expected
         and not run["stdout"] and not run["stderr"],
         f"{label}: expected quiet exit {expected}: status={run['status']} "
         f"exit={run['exit']} stdout={run['stdout'][-500:]!r} "
         f"stderr={run['stderr'][-1000:]!r}")
    row.update(success=True, exit=run["exit"])
    return row


def float_flags(path, label):
    text = Path(path).read_text(encoding="utf-8")
    fast = sorted(set(FAST_FLOAT_RE.findall(text)))
    fma = sorted(set(FMA_RE.findall(text)))
    need(not fast, f"{label}: fast/contract float flags found: {fast}")
    need(not fma, f"{label}: FMA intrinsic found: {fma}")
    return {"path": str(path), "label": label, "fast_float_flags": fast,
            "fma_intrinsics": fma, "success": True}


def emit_source_metadata(args, env, out, manifest, out_dir, label):
    row = emit_metadata(args, env, out, manifest, out_dir, label)
    metadata = json.loads(
        Path(row["files"]["metadata.json"]["path"]).read_text(encoding="utf-8"))
    found = [item for item in metadata["symbols"]
             if item["source_name"] == "multiply"
             and item["generated"].startswith("_f")]
    need(len(found) == 1,
         f"{label}: expected one generated multiply symbol, got {len(found)}")
    row["multiply_symbol"] = int(found[0]["symbol"])
    row["multiply_generated"] = found[0]["generated"]
    return row


def function_span(text, name):
    for match in FUNC_HEAD_RE.finditer(text):
        if match.group(1) != name:
            continue
        end = FUNC_END_RE.search(text, match.end())
        need(end is not None, f"unterminated LLVM function {name}")
        return match.start(), match.end(), end.start()
    return None


def block_spans(text, name):
    span = function_span(text, name)
    need(span is not None, f"LLVM function {name} is missing")
    _, start, end = span
    matches = list(BLOCK_LABEL_RE.finditer(text, start, end))
    need(matches, f"LLVM function {name} has no basic blocks")
    blocks = {}
    for index, match in enumerate(matches):
        stop = matches[index + 1].start() if index + 1 < len(matches) else end
        blocks[match.group(1)] = (match.end(), stop)
    return blocks


def region_names(records, index):
    names = list(records[index]["blocks"])
    child = index + 1
    while child < len(records) and records[child]["depth"] > 1:
        names.extend(name for name in records[child]["blocks"]
                     if name not in names)
        child += 1
    return names, child - index - 1


def count_names(names, block_text, patterns):
    counts = {key: 0 for key in patterns}
    for name in names:
        chunk = "\n".join(block_text.get(name, []))
        for key, pattern in patterns.items():
            counts[key] += len(pattern.findall(chunk))
    return counts


def kernel_loops(rows):
    return [row for row in rows if row["fmul"] > 0 and row["fadd"] > 0]


def init_only_loops(rows):
    return [row for row in rows if row["fmul"] == 0 and row["fadd"] == 0
            and (row["init"] > 0 or row["new"] > 0 or row["heap"] > 0)]


def debug_kernel_count(block_text, names, nodes, target, cache):
    count = 0
    for name in names:
        for line in block_text.get(name, []):
            if DOUBLE_BINOP_RE.search(line) is None:
                continue
            for match in re.finditer(r"!dbg !(\d+)", line):
                if fannkuch.debug_scope(
                        nodes, int(match.group(1)), cache) == target:
                    count += 1
                    break
    return count


def two_trip_boolean_control(header_lines, header, blocks):
    for line in header_lines:
        match = re.match(r"\s*(%[A-Za-z0-9_.$]+)\s*=\s*phi\s+i1\s+(.*)", line)
        if match is None:
            continue
        incoming = re.findall(
            r"\[\s*(true|false)\s*,\s*%([A-Za-z0-9_.$]+)\s*\]",
            match.group(2))
        if len(incoming) != 2 or {value for value, _ in incoming} != {"true", "false"}:
            continue
        initial = next(pred for value, pred in incoming if value == "true")
        latch = next(pred for value, pred in incoming if value == "false")
        if initial in blocks or latch not in blocks:
            continue
        for branch in blocks[latch]:
            if re.search(rf"\bbr\s+i1\s+{re.escape(match.group(1))},\s*"
                         rf"label\s+%{re.escape(header)},\s*label\s+%", branch):
                return {"phi": match.group(1), "latch": latch,
                        "branch": branch.strip()}
    return None


def trip_evidence(header_lines, rounds, header, blocks):
    text = "\n".join(header_lines)
    evidence = []
    if rounds == 2:
        if two_trip_boolean_control(header_lines, header, blocks):
            evidence.append("two_trip_boolean_induction_phi")
    for match in re.finditer(
            r"\bicmp\s+(?:slt|ult|sle|ule|eq|ne)\s+i(?:32|64)\s+"
            r"([^,\s]+)\s*,\s*([^,\s\)]+)", text):
        for value in match.groups():
            if re.fullmatch(r"-?\d+", value) and int(value) == rounds:
                evidence.append(f"icmp_bound_{rounds}")
    return sorted(set(evidence))


def analyze_round_kernel(args, env, report, path, function_name, kernel_symbol,
                         rounds, language, label):
    text = Path(path).read_text(encoding="utf-8")
    sections, loop_row = fannkuch.analyze_loops(
        args, env, report, Path(path), f"{label} loop analysis")
    functions = parse_functions(text)
    need(function_name in functions, f"{label}: function {function_name} missing")
    blocks = fannkuch.function_blocks(text, function_name)
    records = sections.get(function_name, [])
    callee_defined = kernel_symbol in functions
    callee_blocks = (fannkuch.function_blocks(text, kernel_symbol)
                     if callee_defined else {})
    callee_records = sections.get(kernel_symbol, [])
    patterns = {
        "fmul": DOUBLE_FMUL_RE, "fadd": DOUBLE_FADD_RE,
        "fcmp": DOUBLE_FCMP_RE, "init": INIT_RE, "new": NEW_RE,
        "heap": HEAP_RE,
        "calls": re.compile(
            rf"\b(?:call|invoke)\b[^\n]*@{re.escape(kernel_symbol)}\("),
    }
    nodes = fannkuch.metadata_nodes(text) if language == "cpp" else {}
    cache = {}
    callee_rows = []
    for record in callee_records:
        counts = count_names(record["blocks"], callee_blocks, patterns)
        callee_rows.append({"depth": record["depth"],
                            "blocks": len(record["blocks"]), **counts})
    callee_kernels = kernel_loops(callee_rows)
    callee_inits = init_only_loops(callee_rows)

    rows, best = [], None
    for index, record in enumerate(records):
        if record["depth"] != 1:
            continue
        names, nested_count = region_names(records, index)
        header = record["blocks"][0]
        counts = count_names(names, blocks, patterns)
        nested_rows = []
        for nested in records[index + 1:index + 1 + nested_count]:
            nested_counts = count_names(nested["blocks"], blocks, patterns)
            nested_rows.append({"depth": nested["depth"],
                                "blocks": len(nested["blocks"]),
                                **nested_counts})
        region_kernels = kernel_loops(nested_rows)
        region_inits = init_only_loops(nested_rows)
        debug = (debug_kernel_count(blocks, names, nodes, "multiply", cache)
                 if language == "cpp" else 0)
        trips = trip_evidence(blocks[header], rounds, header,
                              {name: blocks[name] for name in names})
        call_mode = (counts["calls"] > 0 and callee_defined
                     and bool(callee_kernels))
        inline_mode = (counts["calls"] == 0 and bool(region_kernels)
                       and (language != "cpp" or debug > 0))
        has_kernel = call_mode or inline_mode
        trip_ok = bool(trips) if rounds >= 2 else False
        reasons = []
        if not has_kernel:
            reasons.append("no multiply-add kernel in round region")
        if counts["fcmp"] == 0:
            reasons.append("no full f64 comparison in round region")
        if not trip_ok:
            reasons.append("no repeated-round trip evidence")
        verified = has_kernel and counts["fcmp"] > 0 and trip_ok
        row = {
            "line": record["line"], "header": header, "blocks": names,
            "block_count": len(names), "nested_loops": nested_count,
            "fmul": counts["fmul"], "fadd": counts["fadd"],
            "fcmp": counts["fcmp"], "kernel_calls": counts["calls"],
            "vec_init_calls": counts["init"], "vec_new_calls": counts["new"],
            "heap_allocations": counts["heap"],
            "kernel_loop_count": len(region_kernels),
            "init_only_loop_count": len(region_inits),
            "kernel_loop_depths": [loop["depth"] for loop in region_kernels],
            "init_only_loop_depths": [loop["depth"] for loop in region_inits],
            "cpp_multiply_debug_kernel_ops": debug,
            "trip_evidence": trips,
            "mode": ("kernel_call_in_round_loop" if call_mode else
                     "inlined_kernel_in_round_loop" if inline_mode else
                     "no_kernel"),
            "verified": verified, "reasons": reasons,
        }
        rows.append(row)
        score = (verified, has_kernel, counts["calls"],
                 len(region_kernels), len(trips))
        if best is None or score > best[0]:
            best = (score, row)
    if (rounds >= 2 and language == "cpp"
            and not any(row["verified"] for row in rows)):
        # Clang at -O2 may legally fully unroll the fixed two-trip round loop.
        # In that case there is no depth-1 round loop left, but the actual IR
        # contains one complete duplicated round body per required round:
        # each copy has an inlined f64 multiply-add kernel (debug-scoped to
        # multiply) and its own full f64 comparison.  Accept only exactly one
        # such kernel region and one comparison region per required round.
        kernel_regions = [
            row for row in rows
            if row["kernel_loop_count"] > 0 and row["fmul"] > 0
            and row["fadd"] > 0 and row["cpp_multiply_debug_kernel_ops"] > 0]
        comparison_regions = [row for row in rows if row["fcmp"] > 0]
        disjoint = (len({name for row in kernel_regions
                         for name in row["blocks"]})
                    + len({name for row in comparison_regions
                           for name in row["blocks"]})
                    == sum(row["block_count"] for row in kernel_regions)
                    + sum(row["block_count"] for row in comparison_regions))
        if (len(kernel_regions) == rounds
                and len(comparison_regions) == rounds and disjoint):
            names = []
            for row in kernel_regions + comparison_regions:
                names.extend(name for name in row["blocks"]
                             if name not in names)
            counts = count_names(names, blocks, patterns)
            debug = debug_kernel_count(
                blocks, names, nodes, "multiply", cache)
            trips = [f"unrolled_round_body_count_{rounds}",
                     f"round_kernel_regions_{len(kernel_regions)}",
                     f"round_comparison_regions_{len(comparison_regions)}"]
            row = {
                "line": "unrolled round bodies (no depth-1 round loop)",
                "header": kernel_regions[0]["header"], "blocks": names,
                "block_count": len(names),
                "nested_loops": sum(item["nested_loops"]
                                    for item in kernel_regions
                                    + comparison_regions),
                "fmul": counts["fmul"], "fadd": counts["fadd"],
                "fcmp": counts["fcmp"], "kernel_calls": counts["calls"],
                "vec_init_calls": counts["init"],
                "vec_new_calls": counts["new"],
                "heap_allocations": counts["heap"],
                "kernel_loop_count": sum(item["kernel_loop_count"]
                                         for item in kernel_regions),
                "init_only_loop_count": sum(item["init_only_loop_count"]
                                            for item in kernel_regions
                                            + comparison_regions),
                "kernel_loop_depths": [depth for item in kernel_regions
                                       for depth in item["kernel_loop_depths"]],
                "init_only_loop_depths": [
                    depth for item in kernel_regions + comparison_regions
                    for depth in item["init_only_loop_depths"]],
                "cpp_multiply_debug_kernel_ops": debug,
                "trip_evidence": trips,
                "mode": "unrolled_round_bodies",
                "round_copies": rounds,
                "round_kernel_regions": len(kernel_regions),
                "round_comparison_regions": len(comparison_regions),
                "verified": True, "reasons": [],
            }
            rows.append(row)
            best = ((True, True, 0, row["kernel_loop_count"], len(trips)),
                    row)
    if not rows:
        # A real R1 main may be fully unrolled by O2, leaving no depth-1 loop.
        # Record that actual whole-function shape and reject explicitly: there
        # is no repeated round region in which a kernel could be proven.
        names = list(blocks)
        counts = count_names(names, blocks, patterns)
        debug = (debug_kernel_count(blocks, names, nodes, "multiply", cache)
                 if language == "cpp" else 0)
        row = {
            "line": "whole function (no depth-1 loop)",
            "header": next(iter(blocks)), "blocks": names,
            "block_count": len(names), "nested_loops": 0,
            "fmul": counts["fmul"], "fadd": counts["fadd"],
            "fcmp": counts["fcmp"], "kernel_calls": counts["calls"],
            "vec_init_calls": counts["init"], "vec_new_calls": counts["new"],
            "heap_allocations": counts["heap"],
            "kernel_loop_count": 0, "init_only_loop_count": 0,
            "kernel_loop_depths": [], "init_only_loop_depths": [],
            "cpp_multiply_debug_kernel_ops": debug,
            "trip_evidence": [],
            "mode": "no_depth1_loop",
            "verified": False,
            "reasons": ["no depth-1 loop in function",
                        "no repeated-round trip evidence"],
        }
        rows.append(row)
        best = ((False, False, 0, 0, 0), row)
    selected = best[1] if best else None
    return {
        "label": label, "implementation": language, "rounds": rounds,
        "function": function_name, "kernel_symbol": kernel_symbol,
        "loop_analysis": loop_row, "regions": rows,
        "callee": {
            "defined": callee_defined, "loops": callee_rows,
            "kernel_loop_count": len(callee_kernels),
            "init_only_loop_count": len(callee_inits),
            "kernel_loop_depths": [loop["depth"] for loop in callee_kernels],
            "init_only_loop_depths": [loop["depth"] for loop in callee_inits],
            "has_kernel_loop": bool(callee_kernels),
        },
        "selected": selected,
        "verified": bool(selected and selected["verified"]),
        "status": ("verified" if selected and selected["verified"]
                   else "not_verified"),
        "not_verified_by_init_loops_only": True,
        "after_ir": file_rec(path),
    }


def instrument_visit_stream(text, symbol, label, path):
    visit = mandel.visit_function(text, symbol, label)
    need("matmul_observe" not in text,
         f"{label}: matmul_observe already exists in the IR")
    returns = list(re.finditer(r"^\s*ret\b[^\n]*$", visit["body"],
                               re.MULTILINE))
    need(len(returns) == 1, f"{label}: expected one return in empty visit")
    call = f"  call void @matmul_observe(ptr {visit['argument']})\n"
    new_body = (visit["body"][:returns[0].start()] + call
                + visit["body"][returns[0].start():])
    first_define = text.index("define ")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text[:first_define])
        handle.write("declare void @matmul_observe(ptr)\n\n")
        handle.write(text[first_define:visit["body_start"]])
        handle.write(new_body)
        handle.write(text[visit["body_end"]:])
    return visit


def compile_native_observer_large(args, env, out, observe_ll, binary, opt,
                                  label):
    binary.parent.mkdir(parents=True, exist_ok=True)
    command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         "-I", str(mandel.WORKTREE / "runtime/native"),
         PERFORMANCE_DIR / "native_observer.cpp", observe_ll, args.runtime,
         f"--ld-path={args.llvm_bin / 'ld.lld'}", "-o", binary], args, env)
    common.require_success(command, f"{label} observer build")
    return {"command_index": command["command_index"],
            "binary": file_rec(binary, out),
            "elf": common.checked_elf(binary, executable=True, label=label),
            "formal_cli": False, "host_libc_dependent": True,
            "not_independent_native_performance_program": True}


def parse_native_matrices(run, elements, expected_rounds, label):
    """Decode every complete C observation printed by the test-only observer.

    R2 keeps ``visit(c)`` inside the repeated round body, so the observer is
    called exactly once per round; requiring one complete matrix per round is
    a dynamic companion to the static round-loop IR check.
    """
    text = run["stdout"]
    need("\r" not in text, f"{label}: observer stdout contains a carriage return")
    lines = text.splitlines()
    need(len(lines) == expected_rounds,
         f"{label}: observer printed {len(lines)} matrices, expected "
         f"{expected_rounds} complete round observations")
    matrices = []
    for index, line in enumerate(lines):
        need(re.fullmatch(r"[0-9a-f]+", line) is not None,
             f"{label}: round {index} output is not lowercase hex")
        need(len(line) == elements * 16,
             f"{label}: round {index} printed {len(line)} hex digits, expected "
             f"{elements * 16} for {elements} f64 elements")
        matrices.append(bytes.fromhex(line))
    return matrices


def observe_native(args, env, report, out, case, opt, helper, literals,
                   expected_file, directory, label):
    directory.mkdir(parents=True, exist_ok=False)
    observe_ll = directory / "observe.ll"
    text = Path(helper["before"]["path"]).read_text(encoding="utf-8")
    visit = instrument_visit_stream(text, helper["visit_symbol"], label,
                                    observe_ll)
    del text
    verify = verify_ir(observe_ll, args, env, report,
                       f"{label} instrumented verify")
    binary = directory / "observer"
    compiled = compile_native_observer_large(
        args, env, out, observe_ll, binary, opt, label)
    run = run_program([binary], args, env)
    need(run["status"] == "exited" and run["exit"] == 0 and not run["stderr"],
         f"{label}: observer run failed: status={run['status']} "
         f"exit={run['exit']} stderr={run['stderr'][-1000:]!r}")
    nn = case["n"] * case["n"]
    matrices = parse_native_matrices(run, nn, case["rounds"], label)
    hex_path = directory / "observed.hex"
    hex_path.write_text(run["stdout"], encoding="utf-8")
    captured_sha = [sha256_bytes(matrix) for matrix in matrices]
    run["stdout_sha256"] = common.sha256_text(run["stdout"])
    run["stdout_bytes"] = len(run["stdout"].encode())
    run["stdout"] = (f"<{len(matrices)} complete C matrices, {nn} f64 "
                     f"elements each; sha256 {captured_sha}>")
    expected_le = Path(expected_file).read_bytes()
    captured_le = [b"".join(matrix[index:index + 8][::-1]
                            for index in range(0, len(matrix), 8))
                   for matrix in matrices]
    matches_bits = [matrix == literals["expected_bytes"]
                    for matrix in matrices]
    matches_file = [little == expected_le for little in captured_le]
    need(all(matches_bits) and all(matches_file),
         f"{label}: complete C matrix does not match frozen expected "
         f"(captured {captured_sha}, expected {literals['expected_bits_sha256']})")
    need(len(set(captured_sha)) == 1,
         f"{label}: the repeated round observations differ from each other")
    row = {
        "implementation": "native", "variant": "correct", "opt": opt,
        "visit_symbol": helper["visit_symbol"],
        "parameter_type": visit["parameter_type"], "argument": visit["argument"],
        "command_index": run["command_index"],
        "instrumented": file_rec(observe_ll, out), "verify": verify,
        "compile": compiled, "hex": file_rec(hex_path, out),
        "captured_rounds": len(matrices),
        "captured_rounds_expected": case["rounds"],
        "captured_sha256": captured_sha,
        "captured_bytes": len(matrices[0]),
        "captured_total_bytes": sum(len(matrix) for matrix in matrices),
        "stdout_sha256": run["stdout_sha256"],
        "stdout_bytes": run["stdout_bytes"],
        "expected_file_sha256": PINNED_MATRIX[case["n"]]["expected"][0],
        "expected_bits_sha256": literals["expected_bits_sha256"],
        "captured_matches_frozen_file_bytes": all(matches_file),
        "captured_matches_expected_bits": all(matches_bits),
        "rounds_match": len(set(captured_sha)) == 1,
        "full_matrix_elements": nn, "not_checksum_or_sampling": True,
        "formal_cli": False, "host_libc_dependent": True,
        "not_independent_native_performance_program": True, "success": True,
    }
    case["native_observers"].append(row)
    return row


def observe_cpp_full(args, env, out, case, opt, reference_text, literals,
                     expected_file, label):
    """Observe every round's complete C from an auxiliary C++ visit copy.

    Only the empty formal visit body is replaced by a host observer call in a
    separate file.  The formal reference source and the formal C++ O2 build do
    not change; this auxiliary binary is never treated as performance output.
    """
    directory = (out / "observers" / "cpp" / f"n{case['n']}" /
                 f"r{case['rounds']}" / f"O{opt}")
    directory.mkdir(parents=True, exist_ok=False)
    need(reference_text.count(CPP_VISIT_EMPTY) == 1,
         f"{label}: formal C++ empty visit is not a unique exact match")
    start = reference_text.index(CPP_VISIT_EMPTY)
    prefix = reference_text[:start]
    suffix = reference_text[start + len(CPP_VISIT_EMPTY):]
    auxiliary_text = prefix + CPP_VISIT_AUX + suffix
    need(auxiliary_text.startswith(prefix)
         and auxiliary_text.endswith(suffix),
         f"{label}: auxiliary C++ source changed more than the empty visit")
    aux_source = directory / "aux-reference.cpp"
    aux_source.write_text(auxiliary_text, encoding="utf-8")
    binary = directory / "observer"
    command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20",
         f"-O{opt}", PERFORMANCE_DIR / CPP_OBSERVER_SOURCE, aux_source,
         f"--ld-path={args.llvm_bin / 'ld.lld'}", "-o", binary], args, env)
    common.require_success(command, f"{label} auxiliary observer build")
    compiled = {
        "command_index": command["command_index"],
        "binary": file_rec(binary, out),
        "elf": common.checked_elf(binary, executable=True, label=label),
        "formal_cli": False, "host_libc_dependent": True,
        "not_independent_native_performance_program": True,
    }
    run = run_program([binary], args, env)
    need(run["status"] == "exited" and run["exit"] == 0
         and not run["stderr"],
         f"{label}: auxiliary observer run failed: status={run['status']} "
         f"exit={run['exit']} stderr={run['stderr'][-1000:]!r}")
    nn = case["n"] * case["n"]
    matrices = parse_native_matrices(run, nn, case["rounds"], label)
    hex_path = directory / "observed.hex"
    hex_path.write_text(run["stdout"], encoding="utf-8")
    captured_sha = [sha256_bytes(matrix) for matrix in matrices]
    run["stdout_sha256"] = common.sha256_text(run["stdout"])
    run["stdout_bytes"] = len(run["stdout"].encode())
    run["stdout"] = (f"<{len(matrices)} complete C matrices, {nn} f64 "
                     f"elements each; sha256 {captured_sha}>")
    expected_le = Path(expected_file).read_bytes()
    captured_le = [b"".join(matrix[index:index + 8][::-1]
                            for index in range(0, len(matrix), 8))
                   for matrix in matrices]
    matches_bits = [matrix == literals["expected_bytes"]
                    for matrix in matrices]
    matches_file = [little == expected_le for little in captured_le]
    need(all(matches_bits) and all(matches_file),
         f"{label}: complete auxiliary C++ C matrix does not match frozen "
         f"expected (captured {captured_sha}, expected "
         f"{literals['expected_bits_sha256']})")
    need(len(set(captured_sha)) == 1,
         f"{label}: repeated auxiliary C++ round observations differ")
    row = {
        "implementation": "cpp-auxiliary-observer", "variant": "correct",
        "opt": opt,
        "formal_reference_sha256": case["reference"]["sha256"],
        "auxiliary_source": file_rec(aux_source, out),
        "auxiliary_change": {
            "old_span_sha256": common.sha256_text(CPP_VISIT_EMPTY),
            "new_span_sha256": common.sha256_text(CPP_VISIT_AUX),
            "formal_prefix_unchanged": auxiliary_text.startswith(prefix),
            "formal_suffix_unchanged": auxiliary_text.endswith(suffix),
            "formal_source_unchanged": case["reference"]["sha256"] ==
                common.sha256_file(case["reference"]["path"]),
        },
        "compile": compiled,
        "command_index": run["command_index"],
        "hex": file_rec(hex_path, out),
        "captured_rounds": len(matrices),
        "captured_rounds_expected": case["rounds"],
        "captured_sha256": captured_sha,
        "captured_bytes": len(matrices[0]),
        "captured_total_bytes": sum(len(matrix) for matrix in matrices),
        "stdout_sha256": run["stdout_sha256"],
        "stdout_bytes": run["stdout_bytes"],
        "expected_file_sha256": PINNED_MATRIX[case["n"]]["expected"][0],
        "expected_bits_sha256": literals["expected_bits_sha256"],
        "captured_matches_frozen_file_bytes": all(matches_file),
        "captured_matches_expected_bits": all(matches_bits),
        "rounds_match": len(set(captured_sha)) == 1,
        "full_matrix_elements": nn, "not_checksum_or_sampling": True,
        "formal_cli": False, "host_libc_dependent": True,
        "not_independent_native_performance_program": True,
        "success": True,
    }
    case["cpp_observers"].append(row)
    return row


def mutate_and_reject(args, env, report, source_path, function_name, selected,
                      kernel_symbol, language, out_dir, label):
    text = Path(source_path).read_text(encoding="utf-8")
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "mutated.ll"
    target = (kernel_symbol if selected["mode"] == "kernel_call_in_round_loop"
              else function_name)
    spans = block_spans(text, target)
    names = (list(spans) if target == kernel_symbol else selected["blocks"])
    edits, count = [], 0
    for name in names:
        need(name in spans, f"{label}: selected block {name} is missing")
        start, end = spans[name]
        replaced, hits = re.subn(
            r"\bfmul(\s+(?:fast\s+)?(?:double|<[^>]*\bdouble>))",
            r"fadd\1", text[start:end])
        if hits:
            edits.append((start, end, replaced))
            count += hits
    need(count > 0, f"{label}: mutation removed no fmul")
    pieces, cursor = [], 0
    for start, end, replaced in sorted(edits):
        pieces.append(text[cursor:start])
        pieces.append(replaced)
        cursor = end
    pieces.append(text[cursor:])
    mutated = "".join(pieces)
    mutation = "replace_kernel_fmul_with_fadd"
    path.write_text(mutated, encoding="utf-8")
    verify = verify_ir(path, args, env, report, f"{label} verify")
    result = analyze_round_kernel(
        args, env, report, path, function_name, kernel_symbol, 2, language,
        f"{label} analysis")
    return {"label": label, "mutation": mutation, "replacements": count,
            "mutated": file_rec(path, out_dir), "verify": verify,
            "analysis_status": result["status"],
            "analysis_verified": result["verified"],
            "selected": result["selected"], "expected": "reject",
            "passed": not result["verified"]}


def reject_false_round_backedge(args, env, report, path, function_name,
                               source_round, out):
    text = Path(path).read_text(encoding="utf-8")
    blocks = fannkuch.function_blocks(text, function_name)
    selected = source_round["selected"]
    header = selected["header"]
    control = two_trip_boolean_control(
        blocks[header], header,
        {name: blocks[name] for name in selected["blocks"]})
    need(control is not None, "Source R2 has no supported two-trip backedge")
    start, end = block_spans(text, function_name)[control["latch"]]
    block = text[start:end]
    need(block.count(control["branch"]) == 1, "Source R2 backedge is not unique")
    replacement = control["branch"].replace(
        "br i1 " + control["phi"], "br i1 false", 1)
    mutated = text[:start] + block.replace(control["branch"], replacement, 1) + text[end:]
    directory = out / "controls" / "false-source-backedge"
    directory.mkdir(parents=True, exist_ok=False)
    changed = directory / "one-round.ll"
    changed.write_text(mutated, encoding="utf-8")
    verify = verify_ir(changed, args, env, report, "constant-false Source backedge")
    analysis = analyze_round_kernel(
        args, env, report, changed, function_name, source_round["kernel_symbol"],
        2, "source", "constant-false Source backedge analysis")
    need(not analysis["verified"], "one-round Source backedge was accepted as R2")
    return {"mutation": "make_round_backedge_constant_false", "actual_rounds": 1,
            "expected_rounds": 2, "mutated": file_rec(changed, out),
            "verify": verify, "analysis": compact_analysis(analysis),
            "passed": not analysis["verified"]}


def compact_analysis(row):
    selected = row.get("selected")
    return {
        "status": row["status"], "verified": row["verified"],
        "kernel_symbol": row["kernel_symbol"],
        "selected": ({key: selected.get(key) for key in (
            "mode", "verified", "kernel_calls", "fmul", "fadd", "fcmp",
            "kernel_loop_count", "init_only_loop_count", "trip_evidence",
            "cpp_multiply_debug_kernel_ops", "round_copies",
            "round_kernel_regions", "round_comparison_regions", "reasons")}
            if selected else None),
        "callee": {key: row["callee"][key] for key in (
            "defined", "kernel_loop_count", "init_only_loop_count",
            "kernel_loop_depths", "has_kernel_loop")},
    }


def resolve_source_main_function(after_text, before_text, entry_symbol,
                                 multiply_symbol, label):
    """Resolve the real optimized function that contains Source main work.

    Some helper builds keep ``lang.fn.<entry>`` as a separate internal
    function.  The actual product helper may inline it, and the multiply
    helper too, completely into the ABI ``main`` and delete both internal
    definitions.  In that case analyze ``main`` only after proving from the
    before/after IR that the metadata entry function was present before,
    is absent after, and the surviving ``main`` body contains the inlined
    f64 multiply/add kernel or a kernel call plus the f64 comparison.
    """
    entry_name = f"lang.fn.{entry_symbol}"
    functions = parse_functions(after_text)
    if entry_name in functions:
        return entry_name, {
            "mode": "metadata_entry_function",
            "function": entry_name,
            "entry_present_before": entry_name in parse_functions(before_text),
            "entry_present_after": True,
        }
    need("main" in functions,
         f"{label}: neither {entry_name} nor main exists after optimization")
    need(entry_name in parse_functions(before_text),
         f"{label}: metadata entry function {entry_name} is absent both "
         "before and after optimization")
    body = functions["main"]["body"]
    kernel_call = re.search(
        rf"\b(?:call|invoke)\b[^\n]*@lang\.fn\.{int(multiply_symbol)}\b",
        body)
    inline_kernel = (bool(DOUBLE_FMUL_RE.search(body))
                     and bool(DOUBLE_FADD_RE.search(body)))
    comparison = bool(DOUBLE_FCMP_RE.search(body))
    need((kernel_call is not None or inline_kernel) and comparison,
         f"{label}: {entry_name} is absent after optimization but main does "
         "not contain the inlined f64 kernel/compare work")
    return "main", {
        "mode": "inlined_metadata_entry_into_abi_main",
        "function": "main",
        "entry_function": entry_name,
        "entry_present_before": True,
        "entry_present_after": False,
        "kernel_call_in_main": kernel_call is not None,
        "inlined_fmul_fadd_in_main": inline_kernel,
        "f64_comparison_in_main": comparison,
    }


def round_controls(args, env, report, out, templates, n, literals,
                   source_round, cpp_round, source_path, cpp_path,
                   cpp_linkage, source_main_function):
    r1 = generate_case(out / "controls" / f"n{n}" / "r1", templates, n, 1,
                       literals, "correct")
    r1_meta = emit_source_metadata(
        args, env, out, r1["manifest"]["path"],
        out / "controls" / f"n{n}" / "r1" / "python", "r1 control metadata")
    r1_helper = export_source_ir(
        args, env, report, out, r1["manifest"]["path"],
        out / "controls" / f"n{n}" / "r1" / "source-ir" / "O2", 2,
        "r1 control Source O2")
    r1_cpp = compile_cpp_ir_large(
        args, env, report, out, r1["reference"]["path"], 2,
        "r1 control C++ O2")
    r1_after_text = Path(r1_helper["after"]["path"]).read_text(encoding="utf-8")
    r1_before_text = Path(r1_helper["before"]["path"]).read_text(encoding="utf-8")
    r1_source_function, r1_resolution = resolve_source_main_function(
        r1_after_text, r1_before_text, r1_meta["entry_symbol"],
        r1_meta["multiply_symbol"], "r1 control Source")
    r1_source = analyze_round_kernel(
        args, env, report, r1_helper["after"]["path"], r1_source_function,
        f"lang.fn.{r1_meta['multiply_symbol']}", 2, "source",
        "r1 control Source")
    r1_cpp_analysis = analyze_round_kernel(
        args, env, report, r1_cpp["path"], "main", cpp_linkage, 2, "cpp",
        "r1 control C++")
    r1_control = {
        "source": compact_analysis(r1_source),
        "cpp": compact_analysis(r1_cpp_analysis),
        "expected": "reject",
        "passed": not r1_source["verified"] and not r1_cpp_analysis["verified"],
        "r1_case": {key: r1[key] for key in ("manifest", "source", "reference")},
        "r1_metadata": {key: r1_meta[key] for key in
                        ("visit_symbol", "multiply_symbol", "files")},
        "r1_source_resolution": r1_resolution,
        "r1_source_ir": {"object": r1_helper["object"],
                         "before": r1_helper["before"],
                         "after": r1_helper["after"]},
        "r1_cpp_ir": {"ir": r1_cpp["ir"]},
    }
    source_mut = mutate_and_reject(
        args, env, report, source_path, source_main_function,
        source_round["selected"], source_round["kernel_symbol"], "source",
        out / "controls" / f"n{n}" / "mutated-source", "r2 mutated Source")
    cpp_mut = mutate_and_reject(
        args, env, report, cpp_path, "main", cpp_round["selected"],
        cpp_linkage, "cpp",
        out / "controls" / f"n{n}" / "mutated-cpp", "r2 mutated C++")
    positive = {"source": compact_analysis(source_round),
                "cpp": compact_analysis(cpp_round), "expected": "accept",
                "passed": source_round["verified"] and cpp_round["verified"]}
    need(r1_control["passed"], "R1 real O2 IR was not rejected")
    need(positive["passed"], "R2 real O2 IR was not accepted")
    need(source_mut["passed"], "mutated Source IR was not rejected")
    need(cpp_mut["passed"], "mutated C++ IR was not rejected")
    reduced_source = reject_false_round_backedge(
        args, env, report, source_path, source_main_function, source_round, out)
    return {"round_r1_real_ir": r1_control,
            "round_r2_real_ir": positive,
            "mutated_r2_source_ir": source_mut,
            "mutated_r2_cpp_ir": cpp_mut,
            "reduced_r2_source_round_ir": reduced_source}


def base_report(args, env):
    report = common.base_report("prepare", args, env)
    runner = report["script"]
    report.update({
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "mode": "prepare",
        "formal_timing": False, "performance_claim": False,
        "script": file_rec(Path(__file__).resolve()), "common_runner": runner,
        "legacy_prepare": file_rec(PERFORMANCE_DIR / "prepare.py"),
        "fannkuch_large_prepare": file_rec(
            PERFORMANCE_DIR.parent / "fannkuch" / "large_prepare.py"),
        "reuse_modules": {
            "legacy": "_matmul_large_reuse_legacy_prepare",
            "fannkuch_large": "_matmul_large_reuse_fannkuch_large_prepare"},
        "runtime_vec_header": file_rec(mandel.RUNTIME_VEC_HEADER),
        "templates": template_records(),
        "frozen_oracle": {"results_sha256": RESULTS_SHA256,
                          "script_sha256": ORACLE_SCRIPT_SHA256},
    })
    product_head = report["repositories"]["compiler_root"]["head"]
    report["product"] = {
        "head": product_head,
        "product": product_head,
        "previous_preparation": {"head": "26cd210", "product": "8ef1322"},
        "note": "actual compiler product; the recorded compiler, runtime, and "
                "Python helper binaries are frozen, hash-checked before and "
                "after this run, and are never rebuilt",
    }
    report["agent"] = {
        "requested_model": "deepseek-flash",
        "reported_model": os.environ.get("PI_MODEL", "unknown"),
        "provider": os.environ.get("PI_PROVIDER", "unknown"),
        "reasoning_level": os.environ.get("PI_REASONING_LEVEL", "unknown"),
        "reported_matches_requested": os.environ.get("PI_MODEL") == "deepseek-flash",
    }
    report["correction"] = {
        "operation_id": CORRECTION_OPERATION_ID,
        "parent_operation_id": OPERATION_ID,
        "inherited_script_sha256": INHERITED_SCRIPT_SHA256,
        "not_rewritten_from_scratch": True,
        "changes": [
            "use metadata entry_symbol -> lang.fn.ID as the Source algorithm main",
            "if the product inlines that entry into ABI main, require before/after provenance and real inlined kernel/compare evidence before analyzing main",
            "recognize legal fully-unrolled C++ R2 round bodies",
            "observe every round's complete C matrix from an auxiliary C++ visit copy",
        ],
    }
    report["target"].update({
        "cpp_flags": list(TARGET_FLAGS),
        "cpp_linker": str(args.llvm_bin / "ld.lld"),
        "cpp_vectorization": "compiler default; no vectorization-disable flag",
        "cpp_contract": "off", "lto": False, "fast_math": False,
        "march_native": False,
    })
    report["tools"]["python_helper"] = common.tool_record(
        args.python_helper, env, args.cpu, None)
    return report


def tool_paths(args):
    return sorted({Path(path).resolve() for path in (
        args.langc, args.helper, args.runtime, args.python, args.python_helper,
        args.llvm_bin / "clang", args.llvm_bin / "clang++",
        args.llvm_bin / "ld.lld", args.llvm_bin / "opt",
        args.llvm_bin / "llvm-readelf", args.llvm_bin / "llvm-nm")})


def input_paths(args, oracle_files):
    paths = [PERFORMANCE_DIR / name for name in (
        *TEMPLATE_NAMES, "prepare.py", "README.md", "native_observer.cpp",
        CPP_OBSERVER_SOURCE, "visit_trace.py", "large_prepare.py")]
    paths += [
        PERFORMANCE_DIR.parent / "fannkuch" / "large_prepare.py",
        PERFORMANCE_DIR.parent / "fannkuch" / "prepare.py",
        PERFORMANCE_DIR.parent / "mandelbrot" / "prepare.py",
        PERFORMANCE_DIR.parent / "sieve" / "prepare.py",
        Path(__file__).resolve(), Path(common.__file__).resolve(),
        mandel.RUNTIME_VEC_HEADER,
    ]
    paths += list(oracle_files) + list(tool_paths(args))
    return sorted({Path(path).resolve() for path in paths})


def snapshot(paths):
    return {str(path): common.sha256_file(path) for path in paths}


def prepare(args):
    env = common.make_env(args)
    out = common.prepare_out(args.out)
    report = {
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "mode": "prepare", "status": "running",
        "commands": [], "verifies": [], "loop_analyses": [], "elf_checks": [],
        "float_flag_checks": [], "not_verified": [], "cases": [],
        "controls": {}, "_out": str(out),
    }
    common._COMMAND_LOG = report["commands"]
    try:
        report.update(base_report(args, env))
        need(args.n in N_VALUES, f"--n must be one of {N_VALUES}")
        need(args.rounds > 0, "--rounds must be an integer > 0")
        oracle, oracle_files = load_oracle(args.oracle_dir, out)
        selected = {(n, role): oracle["records"][f"n{n}-{role}"]
                    for role in ("a", "b", "expected") for n in (args.n,)}
        tables, verification = verify_oracle_matrices(args.n, selected)
        literals = build_literals(args.n, tables)
        expected_file = Path(oracle["records"][f"n{args.n}-expected"]["path"])
        before = snapshot(input_paths(args, oracle_files))
        tool_before = snapshot(tool_paths(args))
        report.update({
            "parameters": {
                "n": args.n, "rounds": args.rounds,
                "variants": {"correct": 0, "wrong": 1},
                "cpp_flags": list(TARGET_FLAGS),
                "cpp_opt_levels": [0, 2], "source_opt_levels": [0, 2],
                "wrong_answer_opt_levels": [0, 2],
                "fixed_large_case": True, "performance_measured": False,
            },
            "oracle": {**oracle, "verification": verification},
            "input_hashes_before": before,
            "tool_hashes_before": tool_before,
        })
        templates = read_templates()
        n, rounds = args.n, args.rounds
        case = generate_case(out, templates, n, rounds, literals, "correct")
        wrong = generate_case(out, templates, n, rounds, literals, "wrong")
        report["cases"] = [case, wrong]
        metadata = emit_source_metadata(
            args, env, out, case["manifest"]["path"],
            out / "python" / f"n{n}" / f"r{rounds}", "correct metadata")
        case["metadata"] = metadata
        multiply_symbol = metadata["multiply_symbol"]

        for opt in (0, 2):
            build = build_langc(
                args, env, out, case["manifest"]["path"],
                out / "source" / f"n{n}" / f"r{rounds}" / "correct" / f"O{opt}",
                opt, f"correct Source O{opt}")
            case["builds"].append(build)
            execute(args, env, case, "langc", opt, build["program"]["path"], 0,
                    f"correct Source O{opt} run")
            elf_checks(args, env, report, Path(build["program"]["path"]),
                       f"correct Source O{opt}", "langc")

        for opt in (0, 2):
            formal = next(row for row in case["builds"]
                          if row["implementation"] == "langc"
                          and row["opt"] == opt)
            helper = helper_export(
                args, env, report, out, case["manifest"]["path"],
                out / "source-ir" / f"n{n}" / f"r{rounds}" / f"O{opt}", opt,
                formal["object"]["path"], metadata["visit_symbol"],
                f"correct Source O{opt} helper", False)
            case["helper"].append(helper)

        for opt in (0, 2):
            helper = next(row for row in case["helper"] if row["opt"] == opt)
            observe_native(
                args, env, report, out, case, opt, helper, literals,
                expected_file,
                out / "observers" / "native" / f"n{n}" / f"r{rounds}" / f"O{opt}",
                f"correct Source O{opt} observer")

        for opt in (0, 2):
            build = mandel.build_cpp(
                args, env, out, case["reference"]["path"],
                out / "cpp" / f"n{n}" / f"r{rounds}" / "correct" / f"O{opt}"
                / "reference", opt, f"correct C++ O{opt}")
            case["builds"].append(build)
            execute(args, env, case, "cpp", opt, build["program"]["path"], 0,
                    f"correct C++ O{opt} run")
            elf_checks(args, env, report, Path(build["program"]["path"]),
                       f"correct C++ O{opt}", "cpp")
        for opt in (0, 2):
            ir = compile_cpp_ir_large(
                args, env, report, out, case["reference"]["path"], opt,
                f"correct C++ O{opt}")
            case["cpp_ir"][str(opt)] = ir
            if opt == 2:
                report["float_flag_checks"].append(
                    float_flags(ir["path"], "correct C++ O2"))

        cpp_reference_text = Path(case["reference"]["path"]).read_text(
            encoding="utf-8")
        for opt in (0, 2):
            observe_cpp_full(
                args, env, out, case, opt, cpp_reference_text, literals,
                expected_file,
                f"correct C++ O{opt} auxiliary full-matrix observer")

        source_o2 = next(row for row in case["helper"] if row["opt"] == 2)
        report["float_flag_checks"].append(
            float_flags(source_o2["after"]["path"], "correct Source O2"))
        source_after_text = Path(source_o2["after"]["path"]).read_text(
            encoding="utf-8")
        source_before_text = Path(source_o2["before"]["path"]).read_text(
            encoding="utf-8")
        source_main_function, source_main_resolution = \
            resolve_source_main_function(
                source_after_text, source_before_text,
                metadata["entry_symbol"], multiply_symbol,
                f"Source n{n} r{rounds}")
        case["source_main_function"] = source_main_function
        case["source_main_resolution"] = source_main_resolution
        proof_rounds = 2 if rounds == 1 else rounds
        source_round = analyze_round_kernel(
            args, env, report, source_o2["after"]["path"], source_main_function,
            f"lang.fn.{multiply_symbol}", proof_rounds, "source",
            f"Source n{n} r{rounds}")
        case["source_round"] = source_round
        cpp_o2 = case["cpp_ir"]["2"]
        cpp_linkage = fannkuch.cpp_linkage(
            Path(cpp_o2["path"]).read_text(encoding="utf-8"), "multiply")
        cpp_round = analyze_round_kernel(
            args, env, report, cpp_o2["path"], "main", cpp_linkage, proof_rounds,
            "cpp", f"C++ n{n} r{rounds}")
        case["cpp_round"] = cpp_round

        for opt in (0, 2):
            build = build_langc(
                args, env, out, wrong["manifest"]["path"],
                out / "source" / f"n{n}" / f"r{rounds}" / "wrong" / f"O{opt}",
                opt, f"wrong Source O{opt}")
            wrong["builds"].append(build)
            execute(args, env, wrong, "langc", opt, build["program"]["path"], 1,
                    f"wrong Source O{opt} run")
            elf_checks(args, env, report, Path(build["program"]["path"]),
                       f"wrong Source O{opt}", "langc")
            cpp = mandel.build_cpp(
                args, env, out, wrong["reference"]["path"],
                out / "cpp" / f"n{n}" / f"r{rounds}" / "wrong" / f"O{opt}"
                / "reference", opt, f"wrong C++ O{opt}")
            wrong["builds"].append(cpp)
            execute(args, env, wrong, "cpp", opt, cpp["program"]["path"], 1,
                    f"wrong C++ O{opt} run")
            elf_checks(args, env, report, Path(cpp["program"]["path"]),
                       f"wrong C++ O{opt}", "cpp")
        report["controls"]["wrong_answer"] = {
            "variant": "wrong", "mutation": "last C element += 1.0",
            "expected_exit": 1, "source_opt_levels": [0, 2],
            "cpp_opt_levels": [0, 2], "all_failed_quietly": True,
        }

        if rounds >= 2:
            need(source_round["verified"],
                 f"R2 Source O2 IR did not prove the f64 kernel inside the "
                 f"round loop: {source_round['selected']}")
            need(cpp_round["verified"],
                 f"R2 C++ O2 IR did not prove the f64 kernel inside the "
                 f"round loop: {cpp_round['selected']}")
            report["controls"].update(round_controls(
                args, env, report, out, templates, n, literals, source_round,
                cpp_round, source_o2["after"]["path"], cpp_o2["path"],
                cpp_linkage, source_main_function))
        else:
            report["controls"]["round_r1_real_ir"] = {
                "source": compact_analysis(source_round),
                "cpp": compact_analysis(cpp_round),
                "expected": "reject",
                "passed": not source_round["verified"]
                          and not cpp_round["verified"],
            }
            need(report["controls"]["round_r1_real_ir"]["passed"],
                 "R1 real O2 IR unexpectedly satisfied the multi-round proof")

        after = snapshot(input_paths(args, oracle_files))
        tool_after = snapshot(tool_paths(args))
        report["input_hashes_after"] = after
        report["tool_hashes_after"] = tool_after
        report["inputs_unchanged"] = before == after
        report["tools_unchanged"] = tool_before == tool_after
        need(report["inputs_unchanged"], "input SHA-256 changed during the run")
        need(report["tools_unchanged"], "tool SHA-256 changed during the run")
        report["summary"] = summary(report, case, wrong, literals)
        report["limitations"] = limitations()
        report["not_run"] = not_run(rounds)
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["failure"] = {"type": type(error).__name__,
                             "message": str(error),
                             "traceback": traceback.format_exc()}
        report.setdefault("limitations", limitations())
        report.setdefault("not_run", not_run(getattr(args, "rounds", 0)))
    finally:
        report["finished_at"] = now()
        report["loadavg_after"] = common.host_info()["loadavg"]
        report.pop("_out", None)
        common.write_json(out / "prepare.json", report)
        common._COMMAND_LOG = None
        print(json.dumps({
            "status": report["status"], "mode": "prepare", "out": str(out),
            "report": str(out / "prepare.json"),
            "summary": report.get("summary"), "failure": report.get("failure"),
        }, indent=2, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


def summary(report, case, wrong, literals):
    n = case["n"]
    source_builds = [row for row in case["builds"]
                     if row["implementation"] == "langc"]
    cpp_builds = [row for row in case["builds"]
                  if row["implementation"] == "cpp"]
    return {
        "n": n, "rounds": case["rounds"],
        "variants": ["correct", "wrong"],
        "source_builds": len(source_builds), "cpp_builds": len(cpp_builds),
        "wrong_source_builds": len([row for row in wrong["builds"]
                                    if row["implementation"] == "langc"]),
        "wrong_cpp_builds": len([row for row in wrong["builds"]
                                 if row["implementation"] == "cpp"]),
        "correct_runs": len(case["executions"]),
        "wrong_runs": len(wrong["executions"]),
        "elf_checks": len(report["elf_checks"]),
        "source_llvm_exports": len(case["helper"]),
        "source_object_matches": len([row for row in case["helper"]
                                      if row["object_match"]]),
        "source_ir_verifies": sum(
            int(row["verify_before"]["success"])
            + int(row["verify_after"]["success"]) for row in case["helper"]),
        "cpp_ir_exports": len(case["cpp_ir"]),
        "cpp_ir_verifies": sum(int(row["verify"]["success"])
                               for row in case["cpp_ir"].values()),
        "native_observers": len(case["native_observers"]),
        "observed_rounds": sum(
            row["captured_rounds"] for row in case["native_observers"]),
        "observed_matrix_elements": sum(
            row["full_matrix_elements"] * row["captured_rounds"]
            for row in case["native_observers"]),
        "cpp_observers": len(case["cpp_observers"]),
        "cpp_observed_rounds": sum(
            row["captured_rounds"] for row in case["cpp_observers"]),
        "cpp_observed_matrix_elements": sum(
            row["full_matrix_elements"] * row["captured_rounds"]
            for row in case["cpp_observers"]),
        "source_full_matrix_matches": all(
            row["captured_matches_frozen_file_bytes"]
            and row["captured_matches_expected_bits"]
            for row in case["native_observers"]),
        "cpp_full_matrix_matches": all(
            row["captured_matches_frozen_file_bytes"]
            and row["captured_matches_expected_bits"]
            for row in case["cpp_observers"]),
        "full_expected_elements_checked": literals["full_expected_bits_covered"],
        "source_main_function": case.get("source_main_function"),
        "source_main_resolution": case.get("source_main_resolution"),
        "source_round_mode": (case["source_round"]["selected"] or {}).get("mode"),
        "cpp_round_mode": (case["cpp_round"]["selected"] or {}).get("mode"),
        "a_file_sha256": PINNED_MATRIX[n]["a"][0],
        "b_file_sha256": PINNED_MATRIX[n]["b"][0],
        "expected_file_sha256": PINNED_MATRIX[n]["expected"][0],
        "a_literal_sha256": literals["a_literal_sha256"],
        "b_literal_sha256": literals["b_literal_sha256"],
        "expected_literal_sha256": literals["expected_literal_sha256"],
        "expected_bits_sha256": literals["expected_bits_sha256"],
        "source_round_status": case["source_round"]["status"],
        "cpp_round_status": case["cpp_round"]["status"],
        "r2_round_proof": case["rounds"] >= 2,
        "round_controls": sorted(report["controls"]),
        "formal_timing": False, "performance_claim": False,
        "inputs_unchanged": report.get("inputs_unchanged"),
        "tools_unchanged": report.get("tools_unchanged"),
        "requested_model": report.get("agent", {}).get("requested_model"),
        "reported_model": report.get("agent", {}).get("reported_model"),
    }


def limitations():
    return [
        "No timing, throughput, speedup, R calibration, or <=2x comparison is performed or claimed.",
        "The fixed N=128/512 matrices are the pinned independent large oracle; old small N=1/3/7/16 results are not reused as large-scale results.",
        "Source correctness is externally observed over the complete C matrix with the existing test-only host observer; C++ correctness is observed over the complete C matrix for every round through a separate auxiliary copy whose empty visit body calls the small host observer.",
        "The C++ auxiliary observer changes only the empty visit body in a temporary source copy; the formal C++ source, the formal O2 program, and its complete in-program comparison remain unchanged. The auxiliary binary is never treated as performance output.",
        "Python large-matrix execution is not performed; lang_python_emit is used only for Source symbol metadata.",
        "R2 round/kernel evidence is static analysis of the actual optimized LLVM IR (Source after.ll and C++ -O2 -emit-llvm), not a dynamic trace of every arithmetic instruction.",
        "The observer is a host-libc-dependent test binary and is never treated as an independent Native performance program.",
        "Only the fixed x86_64 generic target, the recorded LLVM toolchain, N=128/512, and rounds 1/2 are covered.",
        "No asm, volatile, noinline, optnone, SIMD-disable flag, threads, LTO, fast-math, or -march=native is added.",
        "The actual product helper may inline the metadata Source entry function (and multiply) into the ABI main and delete the internal definitions; main is analyzed only after proving the metadata entry function was present before optimization, absent after, and the surviving main body contains the inlined f64 kernel/compare work.",
    ]


def not_run(rounds):
    items = [
        {"item": "formal_timing", "reason": "this operation is preparation only"},
        {"item": "large_python_execution", "reason": "not required; only Source symbol metadata is emitted"},
        {"item": "million_element_vec_probe", "reason": "already passed at the fixed product baseline; not repeated"},
        {"item": "old_small_scale_results", "reason": "old prepare/templates are unchanged inputs; their large-scale execution is not reused"},
        {"item": "oracle_verifier_execution", "reason": "prepare.py is hash-checked; this script performs its own complete full-matrix formula verification"},
        {"item": "other_targets_or_compilers", "reason": "fixed x86_64 generic target and recorded LLVM toolchain only"},
    ]
    if rounds < 2:
        items.append({"item": "r2_round_positive_control",
                      "reason": "performed by the required rounds=2 run with the real R1 control IR"})
    return items


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True,
                        help="new output directory; must not exist")
    parser.add_argument("--n", type=int, choices=N_VALUES, default=128)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--oracle-dir", type=Path, default=DEFAULT_ORACLE_DIR)
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
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--run-timeout", type=float, default=120.0)
    return parser.parse_args(argv)


def validate_args(args):
    need(args.n in N_VALUES, f"--n must be one of {N_VALUES}")
    need(args.rounds > 0, "--rounds must be an integer > 0")
    need(args.oracle_dir.is_dir(), f"--oracle-dir is missing: {args.oracle_dir}")
    for label, path in (
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
    need(args.timeout > 0 and args.run_timeout > 0, "timeouts must be positive")


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
