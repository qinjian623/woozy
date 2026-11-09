#!/usr/bin/env python3
"""Prepare the fixed Mandelbrot Source/C++ fixture without timing.

The fixture is the official scalar algorithm: cr = 2*x/N - 1.5,
ci = 2*y/N - 1.0, z0 = 0, at most 50 z = z*z + c updates with the norm
squared checked after each update, row-major MSB-first PBM packing, and zero
padding bits.  Both programs initialize the same u8 bitmap, use the same f64
operation order, pack bits the same way, and compare every bitmap byte with an
independent Python complex oracle.

N=1,7,8,9,16,200 are prepared.  Correct programs, wrong-bit programs, Native
observers injected only into the exact exported visit SymbolId, generated
Python observers, Source/C++ O0/O2 builds, LLVM verification, object identity,
and ELF checks are recorded.  No timing, speedup, or <=2x conclusion is made.

Reuses ``run_native.py`` for commands, hashing, ELF inspection helpers,
build-tree/tool validation, host records, and JSON writing.  Generic
command/build/IR/ELF helpers are loaded from ``sieve/prepare.py`` under a
unique importlib module name; Sieve's algorithm, main(), parse_args(), task
constants, and fixed-input statistics are not used.  C++ build stays local
because it must keep ``-ffp-contract=off``.  It does not modify the compiler,
runtime, public harness, existing tests, standard library, or baselines.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import re
import sys
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_native as common  # noqa: E402

# Load the already-read Sieve preparation module under a unique name and reuse
# only its generic command/build/IR/ELF helpers.  Its algorithm, main(),
# parse_args(), task constants, and fixed-input statistics are not used.
SIEVE_PREPARE = Path(__file__).resolve().parents[1] / "sieve" / "prepare.py"
_sieve_spec = importlib.util.spec_from_file_location(
    "_mandelbrot_reuse_sieve_prepare", SIEVE_PREPARE)
if _sieve_spec is None or _sieve_spec.loader is None:
    raise RuntimeError(f"cannot load shared Sieve helpers: {SIEVE_PREPARE}")
sieve = importlib.util.module_from_spec(_sieve_spec)
sys.modules[_sieve_spec.name] = sieve
_sieve_spec.loader.exec_module(sieve)

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
FUNC_END_RE = sieve.FUNC_END_RE
PerfError = sieve.PerfError

CONTRACT = "compiler-dev/1"
TASK = "native-mandelbrot-preparation"
OPERATION_ID = "language-mandelbrot-preparation-3a9cb82-20261109"
PREPARE_SCHEMA = "compiler-dev/native-mandelbrot-preparation/1"
N_VALUES = (1, 7, 8, 9, 16, 200)
ITERATIONS = 50
OFFICIAL_SHA256 = (
    "97610473750700638fc63d13cfa49d339b67c18e7f26b3f9c9acb61e746472d5"
)
OFFICIAL_HEADER = b"P4\n200 200\n"
OFFICIAL_SIZE = 5011
PERFORMANCE_DIR = Path(__file__).resolve().parent
WORKTREE = PERFORMANCE_DIR.parents[2]
RUNTIME_VEC_HEADER = WORKTREE / "runtime/native/vec.hpp"
DEFAULT_OFFICIAL = Path(
    "/home/qin/Work/language-dev-20261103/checks/"
    "algorithm-reference-inputs-20261109/mandelbrot-n200.pbm"
)
DEFAULT_OFFICIAL_CHECK = DEFAULT_OFFICIAL.with_name("mandelbrot-oracle-check.json")
DEFAULT_PYTHON_HELPER = Path(
    "/home/qin/Work/language-dev-20261103/compiler/build/native-cli-release/"
    "tests/python/lang_python_emit"
)
TARGET_FLAGS = (
    "--target=x86_64-unknown-linux-gnu", "-march=x86-64", "-mtune=generic",
    "-fno-fast-math", "-ffp-contract=off", "-fno-lto",
)
TEMPLATE_NAMES = ("mandelbrot.lang.in", "reference.cpp.in")
OBSERVER_SOURCES = ("native_observer.cpp", "visit_trace.py")
MARKER_RE = re.compile(
    r"^//\s*PERF-MANDELBROT\s+n=(\d+)\s+iterations=(\d+)"
    r"\s+bytes=(\d+)\s+expected_sha256=([0-9a-f]{64})"
    r"\s+flip=(none|data|padding)\s*$",
    re.MULTILINE,
)
FAST_FLOAT_RE = re.compile(
    r"\b(?:fadd|fsub|fmul|fdiv|fcmp)\s+"
    r"(?:fast|contract|reassoc|nnan|ninf|nsz|arcp|afn)\b"
)
FMA_RE = re.compile(r"\bllvm\.(?:fmuladd|fma)\b")


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha_map(paths):
    return {str(Path(path).resolve()): common.sha256_file(path) for path in paths}


def template_records():
    return {name: file_rec(PERFORMANCE_DIR / name)
            for name in (*TEMPLATE_NAMES, *OBSERVER_SOURCES)}


def read_templates():
    return {name: (PERFORMANCE_DIR / name).read_text(encoding="utf-8")
            for name in TEMPLATE_NAMES}


def mandelbrot_bitmap(n):
    """Independent Python complex recurrence; no compiler output is read."""
    width = (n + 7) // 8
    bitmap = bytearray(width * n)
    for y in range(n):
        ci = 2.0 * y / n - 1.0
        for x in range(n):
            cr = 2.0 * x / n - 1.5
            zr = 0.0
            zi = 0.0
            escaped = False
            for _ in range(ITERATIONS):
                zr2 = zr * zr
                zi2 = zi * zi
                zi = 2.0 * zr * zi + ci
                zr = zr2 - zi2 + cr
                if zr * zr + zi * zi > 4.0:
                    escaped = True
                    break
            if not escaped:
                bitmap[y * width + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(bitmap)


def bit_target(n, kind):
    if kind == "data":
        # N=1 has only one byte, so its single data bit is unavoidable there.
        return (0 if n == 1 else 1), 0x80
    if kind == "padding":
        need(n in (7, 9), f"padding control is only defined for N=7,9, not {n}")
        return 1, 0x01
    raise PerfError(f"unknown flip kind {kind}")


def make_expected(oracle, n, flip_kind, mutate):
    expected = bytearray(oracle)
    flip = None if flip_kind is None else bit_target(n, flip_kind)
    if mutate:
        need(flip is not None, "mutated expected bytes require a flip")
        index, mask = flip
        expected[index] ^= mask
    return bytes(expected), flip


def render(template, mapping):
    text = template
    for key, value in mapping.items():
        text = text.replace("{{" + key + "}}", str(value))
    need("{{" not in text, "rendered template still has a placeholder")
    return text


def verify_generated(source, reference, wanted, byte_count, expected_sha, label):
    for text, language in ((source, "Source"), (reference, "C++")):
        match = MARKER_RE.search(text)
        need(match is not None, f"{label}: {language} marker is missing")
        actual = (int(match.group(1)), int(match.group(2)),
                  int(match.group(3)), match.group(4), match.group(5))
        need(actual == wanted,
             f"{label}: {language} marker {actual} != {wanted}")
    need("Vec[u8]" in source and "visit(bitmap);" in source,
         f"{label}: Source is missing the u8 bitmap or visit call")
    for snippet in (
        "zi = 2.0 * zr * zi + ci;", "zr = zr2 - zi2 + cr;",
        "if zr * zr + zi * zi > 4.0 {", "bitmap[byte_index] | mask",
        "for var iteration i32 = 0; iteration < 50; iteration += 1",
    ):
        need(snippet in source, f"{label}: Source is missing {snippet!r}")
    need("std::vector<uint8_t>" in reference
         and "std::vector<bool>" not in reference,
         f"{label}: C++ must use std::vector<uint8_t>, not std::vector<bool>")
    for snippet in (
        "zi = 2.0 * zr * zi + ci;", "zr = zr2 - zi2 + cr;",
        "if (zr * zr + zi * zi > 4.0) {", "| mask",
        "for (int32_t iteration = 0; iteration < 50; ++iteration)",
    ):
        need(snippet in reference, f"{label}: C++ is missing {snippet!r}")
    need(expected_sha in source and expected_sha in reference,
         f"{label}: expected bitmap SHA is not recorded in both programs")


def generate_case(out, templates, n, oracle, variant, flip_kind, mutate):
    expected, flip = make_expected(oracle, n, flip_kind, mutate)
    byte_count = len(oracle)
    expected_sha = sha256_bytes(expected)
    marker_flip = "none" if flip_kind is None else flip_kind
    wanted = (n, ITERATIONS, byte_count, expected_sha, marker_flip)
    if flip is None:
        source_flip = ""
        cpp_flip = ""
    else:
        index, mask = flip
        source_flip = (
            f"    bitmap[{index}] = bitmap[{index}] ^ u8({mask});"
        )
        cpp_flip = (
            f"    bitmap[{index}] = static_cast<uint8_t>("
            f"bitmap[{index}] ^ UINT8_C({mask}));"
        )
    mapping = {
        "N": n, "BYTE_COUNT": byte_count, "EXPECTED_SHA": expected_sha,
        "FLIP_KIND": marker_flip, "ZEROS": ", ".join("0" for _ in range(byte_count)),
        "EXPECTED": ", ".join(str(value) for value in expected),
        "PACK_FLIP": source_flip,
    }
    cpp_mapping = dict(mapping)
    cpp_mapping["PACK_FLIP"] = cpp_flip
    source_text = render(templates["mandelbrot.lang.in"], mapping)
    reference_text = render(templates["reference.cpp.in"], cpp_mapping)
    label = f"n={n} {variant}"
    verify_generated(source_text, reference_text, wanted, byte_count,
                     expected_sha, label)
    directory = out / "cases" / f"n{n}" / variant
    (directory / "src").mkdir(parents=True, exist_ok=False)
    source = directory / "src" / "main.lang"
    reference = directory / "reference.cpp"
    manifest = directory / "lang.toml"
    source.write_text(source_text, encoding="utf-8")
    reference.write_text(reference_text, encoding="utf-8")
    manifest.write_text(
        manifest_text(f"perf_mandelbrot_n{n}_{variant.replace('-', '_')}"),
        encoding="utf-8")
    expected_path = directory / "expected.bin"
    expected_path.write_bytes(expected)
    return {
        "n": n, "variant": variant, "flip_kind": marker_flip,
        "flip": None if flip is None else {"index": flip[0], "mask": flip[1]},
        "expected_sha256": expected_sha, "expected": file_rec(expected_path, out),
        "manifest": file_rec(manifest, out), "source": file_rec(source, out),
        "reference": file_rec(reference, out), "builds": [],
        "executions": [], "helper": [], "native_observers": [],
    }


def parse_official(path, check_path):
    data = Path(path).read_bytes()
    need(len(data) == OFFICIAL_SIZE,
         f"{path}: expected {OFFICIAL_SIZE} bytes, got {len(data)}")
    need(sha256_bytes(data) == OFFICIAL_SHA256,
         f"{path}: SHA-256 is not the recorded official value")
    need(data.startswith(OFFICIAL_HEADER), f"{path}: header is not P4 200x200")
    check = json.loads(Path(check_path).read_text(encoding="utf-8"))
    need(check.get("sha256") == OFFICIAL_SHA256
         and check.get("official_sha256") == OFFICIAL_SHA256,
         f"{check_path}: recorded SHA does not match")
    need(check.get("size") == 200 and check.get("bytes") == OFFICIAL_SIZE
         and check.get("full_byte_match") is True
         and check.get("mismatched_bytes") == 0,
         f"{check_path}: recorded full-byte match is not true")
    return data, data[len(OFFICIAL_HEADER):], check


def build_oracle(out, official_path, check_path):
    official_data, official_bitmap, official_check = parse_official(
        official_path, check_path)
    oracle_dir = out / "oracle"
    oracle_dir.mkdir(parents=True, exist_ok=False)
    cases = {}
    for n in N_VALUES:
        bitmap = mandelbrot_bitmap(n)
        width = (n + 7) // 8
        need(len(bitmap) == width * n, f"n={n}: oracle byte count is wrong")
        padding_mask = (1 << (8 - (n % 8))) - 1 if n % 8 else 0
        if padding_mask:
            for y in range(n):
                need(bitmap[y * width + width - 1] & padding_mask == 0,
                     f"n={n} row {y}: oracle padding bits are not zero")
        path = oracle_dir / f"n{n}.bin"
        path.write_bytes(bitmap)
        cases[n] = {
            "n": n, "width_bytes": width, "bytes": len(bitmap),
            "sha256": sha256_bytes(bitmap), "file": file_rec(path, out),
            "padding_bits_per_row": (8 - n % 8) % 8,
        }
    need(cases[200]["sha256"] == sha256_bytes(official_bitmap),
         "N=200 independent oracle bitmap differs from the official bitmap")
    # Reconstruct from the written oracle file, not from the official bytes.
    pbm = OFFICIAL_HEADER + Path(cases[200]["file"]["path"]).read_bytes()
    pbm_path = oracle_dir / "n200.pbm"
    pbm_path.write_bytes(pbm)
    need(pbm == official_data,
         "N=200 independent oracle PBM is not byte-identical to the official file")
    oracle_json = {
        "schema": "compiler-dev/native-mandelbrot-oracle/1",
        "method": "independent Python complex recurrence, z0=0, 50 updates, "
                  "escape when norm squared > 4 after each update, "
                  "row-major MSB-first packing with zero padding",
        "n_values": list(N_VALUES), "iterations": ITERATIONS,
        "cases": {str(n): row for n, row in cases.items()},
        "official": {
            "pbm": file_rec(official_path),
            "oracle_check": file_rec(check_path),
            "sha256": OFFICIAL_SHA256, "size": OFFICIAL_SIZE,
            "header": OFFICIAL_HEADER.decode("ascii"),
            "bitmap_sha256": sha256_bytes(official_bitmap),
            "independent_n200_pbm": file_rec(pbm_path, out),
            "full_byte_match": pbm == official_data,
            "recorded_full_byte_match": official_check["full_byte_match"],
        },
    }
    oracle_path = out / "oracle.json"
    common.write_json(oracle_path, oracle_json)
    return cases, file_rec(oracle_path, out), official_data, official_bitmap


def base_report(args, env):
    report = common.base_report("prepare", args, env)
    runner = report["script"]
    report.update({
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "formal_timing": False,
        "script": file_rec(Path(__file__).resolve()), "common_runner": runner,
        "sieve_helpers": file_rec(SIEVE_PREPARE),
        "reuse_module": "_mandelbrot_reuse_sieve_prepare",
        "runtime_vec_header": file_rec(RUNTIME_VEC_HEADER),
        "templates": template_records(),
    })
    report["target"].update({
        "cpp_flags": list(TARGET_FLAGS),
        "cpp_linker": str(args.llvm_bin / "ld.lld"),
    })
    report["tools"]["python_helper"] = common.tool_record(
        args.python_helper, env, args.cpu, None)
    return report


def build_cpp(args, env, out, reference, program, opt, label):
    # Kept local: Sieve.build_cpp uses Sieve.TARGET_FLAGS, which does not
    # include -ffp-contract=off.  Reusing it would reintroduce llvm.fmuladd
    # (FMA) into the C++ O2 IR and change the required scalar operation order.
    program.parent.mkdir(parents=True, exist_ok=True)
    command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         f"--ld-path={args.llvm_bin / 'ld.lld'}", reference, "-o", program],
        args, env)
    common.require_success(command, f"{label} build")
    return {
        "implementation": "cpp", "opt": opt,
        "command_index": command["command_index"],
        "program": file_rec(program, out),
        "elf": common.checked_elf(program, executable=True, label=label),
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


def visit_function(text, symbol, label):
    pattern = re.compile(
        rf"^define\b[^\n]*?@lang\.fn\.{int(symbol)}\(([^)]*)\)[^\n]*\{{\s*$",
        re.MULTILINE)
    matches = list(pattern.finditer(text))
    need(len(matches) == 1,
         f"{label}: expected exactly one @lang.fn.{symbol} definition, "
         f"found {len(matches)}")
    match = matches[0]
    end = FUNC_END_RE.search(text, match.end())
    need(end is not None, f"{label}: unterminated visit function")
    arguments = [arg.strip() for arg in match.group(1).split(",") if arg.strip()]
    need(len(arguments) == 1,
         f"{label}: visit must have exactly one real argument, got {arguments}")
    argument = re.fullmatch(r"ptr\s+(%[A-Za-z0-9_.$]+)", arguments[0])
    need(argument is not None,
         f"{label}: visit argument is not a single ptr: {arguments[0]!r}")
    header = text[match.start():match.end()]
    return_type = re.search(r"^define\b[^\n]*?\b([A-Za-z0-9_.]+)\s+@lang",
                            header)
    need(return_type is not None, f"{label}: cannot read visit return type")
    body = text[match.end():end.start()]
    need(not re.search(r"\b(?:call|invoke|br)\b", body),
         f"{label}: visit is not empty (control flow or call found)")
    need(len(re.findall(r"\bret\b", body)) == 1,
         f"{label}: visit must have exactly one return")
    return {
        "symbol": int(symbol), "header": header,
        "return_type": return_type.group(1), "parameter_type": "ptr",
        "argument": argument.group(1), "body_start": match.end(),
        "body_end": end.start(), "body": body,
    }


def source_call_count(text, symbol):
    return len(re.findall(
        rf"\b(?:call|invoke)\b[^\n]*@lang\.fn\.{int(symbol)}\b", text))


def work_check(args, env, report, path, label, require_bit_pack=True):
    text = Path(path).read_text(encoding="utf-8")
    functions = parse_functions(text)
    candidates = []
    for name, info in functions.items():
        body = info["body"]
        has_float = ("fmul double" in body and "fcmp" in body
                     and (not require_bit_pack or "fadd double" in body))
        has_pack = ("or i8" in body and re.search(r"\bstore i8\b", body))
        score = (body.count("fmul double") + body.count("fadd double")
                 + body.count("fcmp") + body.count("or i8")
                 + len(re.findall(r"\bstore i8\b", body)))
        if has_float and (has_pack or not require_bit_pack):
            candidates.append((score, name))
    need(candidates,
         f"{label}: no function contains the required float work"
         + (" and bit packing" if require_bit_pack else ""))
    target = max(candidates)[1]
    body = functions[target]["body"]
    loops = tool([args.llvm_bin / "opt", "-passes=print<loops>",
                  "-disable-output", path], args, env)
    row = {"path": str(path), "command_index": loops["command_index"],
           "success": False}
    report["loop_analyses"].append(row)
    common.require_success(loops, f"{label} loop analysis")
    target_loops = parse_loop_sections(loops["stderr"]).get(target, [])
    need(target_loops, f"{label}: {target} has no real loop")

    stores = []
    for match in re.finditer(r"^\s*store i8\s+(?!0\b)[^\n]*$", body,
                             re.MULTILINE):
        label_name = None
        for line in reversed(body[:match.start()].splitlines()):
            found = LABEL_RE.match(line.strip())
            if found:
                label_name = found.group(1)
                break
        stores.append((label_name, match.group(0).strip()))
    if require_bit_pack:
        need(stores, f"{label}: no non-zero i8 store in {target}")
        store_in_loop = any(
            label_name is not None
            and any("%" + label_name in line for line in target_loops)
            for label_name, _ in stores)
        need(store_in_loop,
             f"{label}: bit-pack store is not inside a loop of {target}")
    else:
        store_in_loop = False
    fast = sorted(set(FAST_FLOAT_RE.findall(text)))
    fma = sorted(set(FMA_RE.findall(text)))
    need(not fast, f"{label}: fast/contract float flags found: {fast}")
    need(not fma, f"{label}: FMA intrinsic found: {fma}")
    row.update({
        "target": target, "target_is_main": target == "main",
        "target_loop_count": len(target_loops),
        "target_loop_lines": target_loops,
        "loop_count": loops["stderr"].count("Loop at depth"),
        "fmul_double": body.count("fmul double"),
        "fadd_double": body.count("fadd double"),
        "fcmp": body.count("fcmp"),
        "or_i8": body.count("or i8"),
        "nonzero_store_i8": len(stores),
        "store_in_loop": store_in_loop,
        "bit_pack_required": require_bit_pack,
        "bit_pack_present": "or i8" in body and bool(stores),
        "fast_float_flags": fast, "fma_intrinsics": fma,
        "file": file_rec(path),
    })
    row["success"] = True
    return row


def helper_export(args, env, report, out, manifest, out_dir, opt,
                  expected_object, visit_symbol, label, check_work):
    out_dir.mkdir(parents=True, exist_ok=False)
    obj = out_dir / "program.o"
    after = out_dir / "after.ll"
    before = out_dir / "before.ll"
    helper = tool(
        [args.helper, "--project", manifest, "--opt", str(opt),
         "--out-object", obj, "--out-llvm", after, "--out-llvm-before", before,
         "--out-native", out_dir / "program.nir", "--format", "json"],
        args, env)
    common.require_success(helper, f"{label} helper")
    helper_json = common.parse_json_stdout(helper, f"{label} helper")
    need(helper_json.get("status") in ("ok", "passed"),
         f"{label}: helper status is not ok/passed")
    need(helper_json.get("phase") == "object", f"{label}: phase is not object")
    need(helper_json.get("llvm_verified_before") is True
         and helper_json.get("llvm_verified_after") is True,
         f"{label}: helper did not verify both LLVM IR files")
    common.checked_elf(obj, executable=False, label=f"{label} helper object")
    before_text = before.read_text(encoding="utf-8")
    after_text = after.read_text(encoding="utf-8")
    nir_text = (out_dir / "program.nir").read_text(encoding="utf-8")
    need(re.search(rf"^function @{int(visit_symbol)}\b", nir_text,
                   re.MULTILINE) is not None,
         f"{label}: metadata visit symbol {visit_symbol} is not a NativeIR "
         "function")
    visit = visit_function(before_text, visit_symbol, f"{label} before")
    before_count = source_call_count(before_text, visit_symbol)
    after_count = source_call_count(after_text, visit_symbol)
    if opt == 0:
        need(before_count > 0 and after_count > 0,
             f"{label}: O0 positive visit-call control failed "
             f"(before={before_count}, after={after_count})")
    else:
        need(before_count > 0,
             f"{label}: O2 visit call is absent before optimization")
        need(after_count == 0,
             f"{label}: O2 still has {after_count} actual visit call(s)")
    object_match = Path(expected_object).read_bytes() == obj.read_bytes()
    need(object_match, f"{label}: formal program.o and helper object differ")
    row = {
        "opt": opt, "command_index": helper["command_index"],
        "json": helper_json, "visit_symbol": int(visit_symbol),
        "visit": {key: visit[key] for key in
                  ("symbol", "return_type", "parameter_type", "argument")},
        "visit_calls_before": before_count, "visit_calls_after": after_count,
        "object": file_rec(obj, out), "object_match": object_match,
        "before": file_rec(before, out), "after": file_rec(after, out),
        "verify_before": verify_ir(before, args, env, report,
                                   f"{label} before verify"),
        "verify_after": verify_ir(after, args, env, report,
                                  f"{label} after verify"),
        "work": None,
    }
    if check_work:
        need(opt == 2, f"{label}: work check is only defined for O2")
        row["work"] = work_check(args, env, report, after, f"{label} O2")
    return row


def instrument_visit(text, symbol, label):
    visit = visit_function(text, symbol, label)
    need("mandel_observe" not in text,
         f"{label}: mandel_observe already exists in the IR")
    body = visit["body"]
    returns = list(re.finditer(r"^\s*ret\b[^\n]*$", body, re.MULTILINE))
    need(len(returns) == 1, f"{label}: expected one return in empty visit")
    call = f"  call void @mandel_observe(ptr {visit['argument']})\n"
    new_body = body[:returns[0].start()] + call + body[returns[0].start():]
    new_text = text[:visit["body_start"]] + new_body + text[visit["body_end"]:]
    first_define = new_text.index("define ")
    new_text = (new_text[:first_define]
                + "declare void @mandel_observe(ptr)\n\n"
                + new_text[first_define:])
    return new_text, visit


def compile_native_observer(args, env, out, observe_ll, binary, opt, label):
    binary.parent.mkdir(parents=True, exist_ok=True)
    command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         "-fno-fast-math", "-fno-lto", "-I", str(WORKTREE / "runtime/native"),
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


def decode_hex(record, label):
    text = record["stdout"].strip()
    need(re.fullmatch(r"[0-9a-f]*", text) is not None,
         f"{label}: observer stdout is not lowercase hex: {text[:200]!r}")
    need(text, f"{label}: observer printed no bytes")
    return bytes.fromhex(text)


def observe_native(args, env, report, out, case, variant, opt, helper,
                   expected, oracle, label):
    before_path = Path(helper["before"]["path"])
    instrumented, visit = instrument_visit(
        before_path.read_text(encoding="utf-8"), helper["visit_symbol"], label)
    directory = out / "observers" / "native" / f"n{case['n']}" / variant / f"O{opt}"
    directory.mkdir(parents=True, exist_ok=False)
    observe_ll = directory / "observe.ll"
    observe_ll.write_text(instrumented, encoding="utf-8")
    verify = verify_ir(observe_ll, args, env, report,
                       f"{label} instrumented verify")
    binary = directory / "observer"
    compiled = compile_native_observer(args, env, out, observe_ll, binary, opt,
                                       label)
    run = run_program([binary], args, env)
    need(run["status"] == "exited" and run["exit"] == 0,
         f"{label}: observer run failed: {run}")
    need(not run["stderr"],
         f"{label}: observer wrote stderr: {run['stderr'][-1000:]!r}")
    captured = decode_hex(run, label)
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
        "oracle_sha256": sha256_bytes(oracle),
        "expected_sha256": sha256_bytes(expected),
        "matches_expected": captured == expected,
        "rejected_oracle": captured != oracle,
        "success": False,
    }
    if variant == "correct":
        need(captured == oracle, f"{label}: Native observer bytes differ from oracle")
        row["observer_control"] = "correct"
    else:
        need(captured == expected,
             f"{label}: Native observer did not capture the exact wrong bytes")
        need(captured != oracle,
             f"{label}: Native observer did not reject the wrong bit")
        row["observer_control"] = "negative-rejected"
    row["success"] = True
    case["native_observers"].append(row)
    return row


def emit_python(args, env, out, manifest, out_dir, label):
    out_dir.mkdir(parents=True, exist_ok=False)
    command = tool([args.python_helper, "--project", manifest,
                    "--out-dir", out_dir], args, env)
    common.require_success(command, f"{label} Python emit")
    result = common.parse_json_stdout(command, f"{label} Python emit")
    need(result.get("status") == "ok", f"{label} Python emit status is not ok")
    metadata = json.loads((out_dir / "metadata.json").read_text(encoding="utf-8"))
    visits = [row for row in metadata["symbols"]
              if row["source_name"] == "visit"
              and row["generated"].startswith("_f")]
    need(len(visits) == 1, f"{label}: expected one generated visit symbol")
    entry = int(metadata["entry_symbol"])
    entry_row = next((row for row in metadata["symbols"]
                      if int(row["symbol"]) == entry), None)
    need(entry_row is not None and entry_row["source_name"] == "main",
         f"{label}: metadata entry is not main")
    return {
        "command_index": command["command_index"], "json": result,
        "directory": str(out_dir), "metadata": file_rec(out_dir / "metadata.json", out),
        "visit_symbol": int(visits[0]["symbol"]),
        "visit_generated": visits[0]["generated"],
        "entry_symbol": entry, "entry_generated": entry_row["generated"],
        "files": {name: file_rec(out_dir / name, out)
                  for name in ("program.py", "langrt.py", "verified.hir",
                               "metadata.json")},
    }


def observe_python(args, env, out, case, variant, emitted, expected, oracle, label):
    directory = out / "observers" / "python" / f"n{case['n']}" / variant
    directory.mkdir(parents=True, exist_ok=True)
    run = tool(
        [args.python, "-B", "-s", PERFORMANCE_DIR / "visit_trace.py",
         "--generated-dir", emitted["directory"]], args, env)
    common.require_success(run, f"{label} Python observer")
    need(not run["stderr"],
         f"{label}: Python observer wrote stderr: {run['stderr'][-1000:]!r}")
    captured = decode_hex(run, label)
    hex_path = directory / "observed.hex"
    hex_path.write_text(run["stdout"], encoding="utf-8")
    row = {
        "implementation": "python", "variant": variant,
        "visit_symbol": emitted["visit_symbol"],
        "visit_generated": emitted["visit_generated"],
        "command_index": run["command_index"], "hex": file_rec(hex_path, out),
        "captured_sha256": sha256_bytes(captured),
        "captured_bytes": len(captured),
        "oracle_sha256": sha256_bytes(oracle),
        "expected_sha256": sha256_bytes(expected),
        "matches_expected": captured == expected,
        "rejected_oracle": captured != oracle,
        "success": False,
    }
    if variant == "correct":
        need(captured == oracle, f"{label}: Python observer bytes differ from oracle")
        row["observer_control"] = "correct"
    else:
        need(captured == expected,
             f"{label}: Python observer did not capture the exact wrong bytes")
        need(captured != oracle,
             f"{label}: Python observer did not reject the wrong bit")
        row["observer_control"] = "negative-rejected"
    row["success"] = True
    case["python_observers"].append(row)
    return row


def cpp_visit_linkage(ir_text, label):
    matches = set(re.findall(
        r'!DISubprogram\(name: "visit", linkageName: "([^"]+)"', ir_text))
    need(len(matches) == 1,
         f"{label}: expected one debug-identified C++ visit symbol, "
         f"got {sorted(matches)}")
    return next(iter(matches))


def cpp_call_count(ir_text, linkage):
    return len(re.findall(
        rf"\b(?:call|invoke)\b[^\n]*@{re.escape(linkage)}\b", ir_text))


def compile_cpp_ir(args, env, report, out, reference, output, opt, label,
                   report_check_work, require_bit_pack=True):
    command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         "-g", "-S", "-emit-llvm", reference, "-o", output], args, env)
    common.require_success(command, f"{label} IR build")
    text = output.read_text(encoding="utf-8")
    row = {
        "command_index": command["command_index"],
        "ir": file_rec(output, out), "text": text,
        "verify": verify_ir(output, args, env, report, f"{label} verify"),
        "work": None,
    }
    if report_check_work:
        row["work"] = work_check(args, env, report, output, f"{label} O2",
                                 require_bit_pack=require_bit_pack)
    return row


def input_snapshot(report, out):
    paths = [PERFORMANCE_DIR / name for name in (*TEMPLATE_NAMES, *OBSERVER_SOURCES)]
    paths.extend([Path(__file__).resolve(), Path(common.__file__).resolve(),
                  SIEVE_PREPARE, RUNTIME_VEC_HEADER])
    paths.extend([Path(report["oracle"]["file"]["path"]),
                  Path(report["official"]["pbm"]["path"]),
                  Path(report["official"]["oracle_check"]["path"])])
    for case in report["cases"]:
        for variant in case["variants"].values():
            paths.extend([Path(variant["manifest"]["path"]),
                          Path(variant["source"]["path"]),
                          Path(variant["reference"]["path"]),
                          Path(variant["expected"]["path"])])
    for n, row in report["oracle"]["cases"].items():
        paths.append(Path(row["file"]["path"]))
    return sha_map(paths)


def write_patch(out, args, env):
    names = (*TEMPLATE_NAMES, *OBSERVER_SOURCES, "prepare.py", "README.md")
    parts = []
    for name in names:
        relative = f"tests/performance/mandelbrot/{name}"
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
        "observers": {"native": [], "python": []}, "cases": [],
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
            "n_values": list(N_VALUES), "iterations": ITERATIONS,
            "coordinate": "cr=2.0*x/N-1.5, ci=2.0*y/N-1.0, z0=0",
            "escape": "after each z=z*z+c update, norm squared > 4",
            "packing": "row-major, MSB first, row padding bits zero",
            "official_n200_sha256": OFFICIAL_SHA256,
            "target_flags": list(TARGET_FLAGS),
            "program_expected_exit": {"correct": 0, "wrong": 1},
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
        oracle_cases, oracle_rec, official_data, official_bitmap = build_oracle(
            out, args.official, args.official_check)
        report["oracle"] = {
            "method": "independent Python complex recurrence",
            "file": oracle_rec,
            "cases": {str(n): row for n, row in oracle_cases.items()},
            "official": {
                "pbm": file_rec(args.official),
                "oracle_check": file_rec(args.official_check),
                "sha256": OFFICIAL_SHA256,
                "full_byte_match": oracle_cases[200]["sha256"]
                == sha256_bytes(official_bitmap),
            },
        }
        report["official"] = report["oracle"]["official"]
        variants_for = {
            n: (["correct", "wrong-data", "negative-data"]
                + (["wrong-padding", "negative-padding"] if n in (7, 9) else []))
            for n in N_VALUES
        }
        for n in N_VALUES:
            case = {"n": n, "oracle": oracle_cases[n], "variants": {},
                    "check": None, "cpp_ir": {}, "python": {},
                    "native_observers": [], "python_observers": []}
            report["cases"].append(case)
            oracle = Path(oracle_cases[n]["file"]["path"]).read_bytes()
            for variant in variants_for[n]:
                if variant == "correct":
                    flip_kind, mutate = None, False
                elif variant == "wrong-data":
                    flip_kind, mutate = "data", False
                elif variant == "wrong-padding":
                    flip_kind, mutate = "padding", False
                elif variant == "negative-data":
                    flip_kind, mutate = "data", True
                elif variant == "negative-padding":
                    flip_kind, mutate = "padding", True
                else:
                    raise PerfError(f"unknown variant {variant}")
                case["variants"][variant] = generate_case(
                    out, templates, n, oracle, variant, flip_kind, mutate)
        report["inputs_before"] = input_snapshot(report, out)

        for case in report["cases"]:
            n = case["n"]
            variants = case["variants"]
            correct = variants["correct"]
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
            case["python"]["correct"] = emitted_correct
            oracle_correct = Path(case["oracle"]["file"]["path"]).read_bytes()
            observe_python(
                args, env, out, case, "correct", emitted_correct,
                oracle_correct, oracle_correct, f"n={n} correct")

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
                    f"n={n} correct Source O{opt}", check_work=(opt == 2))
                correct["helper"].append(helper)
                oracle = Path(case["oracle"]["file"]["path"]).read_bytes()
                expected = oracle
                observe_native(
                    args, env, report, out, case, "correct", opt, helper,
                    expected, oracle, f"n={n} correct O{opt}")

                for variant in ("wrong-data", "wrong-padding"):
                    if variant not in variants:
                        continue
                    row = variants[variant]
                    variant_manifest = row["manifest"]["path"]
                    langc_bad = build_langc(
                        args, env, out, variant_manifest,
                        out / "build" / f"n{n}" / variant / "langc" / f"O{opt}",
                        opt, f"n={n} {variant} langc O{opt}")
                    row["builds"].append(langc_bad)
                    execute(args, env, row, "langc", opt,
                            langc_bad["program"]["path"], 1,
                            f"n={n} {variant} langc O{opt}")
                    elf_checks(args, env, report, langc_bad["program"]["path"],
                               f"n={n} {variant} langc O{opt}", "langc")
                    cpp_bad = build_cpp(
                        args, env, out, row["reference"]["path"],
                        out / "build" / f"n{n}" / variant / "cpp" / f"O{opt}" /
                        "reference", opt, f"n={n} {variant} cpp O{opt}")
                    row["builds"].append(cpp_bad)
                    execute(args, env, row, "cpp", opt,
                            cpp_bad["program"]["path"], 1,
                            f"n={n} {variant} cpp O{opt}")
                    elf_checks(args, env, report, cpp_bad["program"]["path"],
                               f"n={n} {variant} cpp O{opt}", "cpp")

                for variant in ("negative-data", "negative-padding"):
                    if variant not in variants:
                        continue
                    row = variants[variant]
                    variant_manifest = row["manifest"]["path"]
                    langc_neg = build_langc(
                        args, env, out, variant_manifest,
                        out / "build" / f"n{n}" / variant / "langc" / f"O{opt}",
                        opt, f"n={n} {variant} langc O{opt}")
                    row["builds"].append(langc_neg)
                    execute(args, env, row, "langc", opt,
                            langc_neg["program"]["path"], 0,
                            f"n={n} {variant} langc O{opt}")
                    elf_checks(args, env, report, langc_neg["program"]["path"],
                               f"n={n} {variant} langc O{opt}", "langc")
                    neg_helper = helper_export(
                        args, env, report, out, variant_manifest,
                        out / "llvm" / f"n{n}" / variant / f"O{opt}", opt,
                        langc_neg["object"]["path"],
                        emitted_correct["visit_symbol"],
                        f"n={n} {variant} Source O{opt}", check_work=False)
                    row["helper"].append(neg_helper)
                    oracle = Path(case["oracle"]["file"]["path"]).read_bytes()
                    expected = Path(row["expected"]["path"]).read_bytes()
                    observe_native(
                        args, env, report, out, case, variant, opt, neg_helper,
                        expected, oracle, f"n={n} {variant} O{opt}")

                if opt == 0:
                    for variant in ("negative-data", "negative-padding"):
                        if variant not in variants:
                            continue
                        row = variants[variant]
                        emitted = emit_python(
                            args, env, out, row["manifest"]["path"],
                            out / "python" / "emit" / f"n{n}" / variant,
                            f"n={n} {variant}")
                        row["python"] = emitted
                        oracle = Path(case["oracle"]["file"]["path"]).read_bytes()
                        expected = Path(row["expected"]["path"]).read_bytes()
                        observe_python(
                            args, env, out, case, variant, emitted,
                            expected, oracle, f"n={n} {variant}")

            for opt in (0, 2):
                ir = compile_cpp_ir(
                    args, env, report, out, reference,
                    out / "llvm" / f"n{n}" / f"cpp-O{opt}.ll", opt,
                    f"n={n} cpp O{opt} IR", report_check_work=(opt == 2),
                    require_bit_pack=(n != 1))
                case["cpp_ir"][f"O{opt}"] = ir
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
        need(report["tools_unchanged"], "tool SHA-256 values changed")
        need(report["inputs_unchanged"], "input SHA-256 values changed")
        repositories_after = {
            "worktree": common.repo_info(WORKTREE, env, args.cpu),
            "compiler_root": common.repo_info(args.compiler_root, env, args.cpu),
        }
        report["repositories_after"] = repositories_after
        old = report["repositories"]
        report["repositories_unchanged"] = all(
            (old[name]["head"], old[name]["dirty"],
             old[name]["status_sha256"])
            == (repositories_after[name]["head"],
                repositories_after[name]["dirty"],
                repositories_after[name]["status_sha256"])
            for name in ("worktree", "compiler_root"))
        need(report["repositories_unchanged"],
             "repository HEAD/status changed during prepare")

        cases = report["cases"]
        report["observers"] = {
            "native": [row for case in cases
                       for row in case["native_observers"]],
            "python": [row for case in cases
                       for row in case["python_observers"]],
            "separate_from_program_executions": True,
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
        python_obs = [row for case in cases for row in case["python_observers"]]
        negative_exec = [row for row in executions
                         if row["expected_exit"] == 1]
        quiet0 = sum(1 for row in executions if row["success"]
                     and row["exit"] == 0 and not row["stdout"]
                     and not row["stderr"])
        quiet1 = sum(1 for row in executions if row["success"]
                     and row["exit"] == 1 and not row["stdout"]
                     and not row["stderr"])
        works = [row["work"] for row in helpers if row["work"]]
        works += [case["cpp_ir"]["O2"]["work"] for case in cases]
        summary = {
            "cases": len(cases), "n_values": list(N_VALUES),
            "variants": sum(len(case["variants"]) for case in cases),
            "program_executions": len(executions),
            "quiet_exit_0": quiet0, "quiet_exit_1": quiet1,
            "wrong_program_executions": len(negative_exec),
            "wrong_program_quiet_exit_1": quiet1,
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
            "o2_work_checks": len(works),
            "o2_work_checks_passed": sum(1 for row in works if row["success"]),
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
            "official_n200_full_match": report["oracle"]["official"][
                "full_byte_match"],
            "oracle_padding_zero": True,
            "tools_unchanged": report["tools_unchanged"],
            "inputs_unchanged": report["inputs_unchanged"],
            "repositories_unchanged": report["repositories_unchanged"],
            "timing_performed": False,
        }
        need(summary["cases"] == len(N_VALUES),
             f"summary cases={summary['cases']}")
        need(summary["variants"] == 22,
             f"summary variants={summary['variants']}")
        need(summary["program_executions"] == 72
             and summary["wrong_program_executions"] == 32,
             "program execution count changed")
        need(summary["quiet_exit_0"] == 40 and summary["quiet_exit_1"] == 32
             and summary["wrong_program_quiet_exit_1"] == 32,
             "quiet exit counts changed")
        need(summary["builds"] == 72 and summary["source_builds"] == 44
             and summary["cpp_builds"] == 28,
             "build counts changed")
        need(summary["source_helper_exports"] == 28
             and summary["source_object_matches"] == 28
             and summary["source_ir_verifies"] == 56,
             "Source helper/object/IR counts changed")
        need(summary["cpp_ir_exports"] == 12 and summary["cpp_ir_verifies"] == 12
             and summary["all_ir_verifies"] == 96,
             "C++/all IR verify counts changed")
        need(summary["instrumented_observers"] == 28
             and summary["instrumented_ir_verifies"] == 28,
             "instrumented observer counts changed")
        need(summary["native_observers"] == 28
             and summary["native_observer_matches"] == 28
             and summary["native_negative_rejections"] == 16,
             "Native observer counts changed")
        need(summary["python_observers"] == 14
             and summary["python_observer_matches"] == 14
             and summary["python_negative_rejections"] == 8,
             "Python observer counts changed")
        need(summary["source_o0_positive_visit_cases"] == 14
             and summary["source_o2_zero_visit_cases"] == 14
             and summary["source_o2_visit_calls"] == 0,
             "Source visit-call counts changed")
        need(summary["cpp_o0_positive_visit_cases"] == 6
             and summary["cpp_o2_zero_visit_cases"] == 6
             and summary["cpp_o2_visit_calls"] == 0,
             "C++ visit-call counts changed")
        need(summary["o2_work_checks"] == 12
             and summary["o2_work_checks_passed"] == 12,
             "O2 work-check counts changed")
        need(summary["source_elf_static_checks"] == 44
             and summary["source_elf_static_checks_passed"] == 44
             and summary["cpp_dynamic_elf_checks"] == 28
             and summary["cpp_dynamic_elf_checks_passed"] == 28,
             "ELF check counts changed")
        need(summary["official_n200_full_match"] is True
             and summary["oracle_padding_zero"] is True,
             "official/padding oracle match changed")
        need(summary["tools_unchanged"] is True
             and summary["inputs_unchanged"] is True
             and summary["repositories_unchanged"] is True,
             "tool/input/repository stability changed")
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
        "The formal programs print nothing; correctness is the full bitmap byte comparison with the independent oracle, including padding bytes and bits.",
        "Native observation is a test-only instrumented binary linked with the host libc and the project runtime; it is not a standalone Native performance program.",
        "The Python observer imports the unmodified generated module and records the actual Vec[u8] received by the empty visit; it does not recompute Mandelbrot.",
        "The Native observer is injected only into the exact metadata visit SymbolId whose exported pre-optimization LLVM definition has one real ptr argument; an unknown symbol fails the run instead of counting zero.",
        "Only N=1,7,8,9,16,200 and the fixed x86_64 generic target are covered. No SIMD, threads, LTO, fast-math, FMA, other targets, or -march=native are used.",
        "N=1 has only one bitmap byte, so its wrong-data control must flip that first byte; N=7 and N=9 are the designated non-first-byte and padding controls.",
        "For C++ N=1 only, clang -O2 constant-folds the single-byte pack to a direct status; the float loop remains, Source N=1 keeps the pack store, and the exception is recorded in the O2 work row.",
        "C++ is built with -ffp-contract=off so the reference does not introduce FMA or change the Source operation order.",
        "The official N=200 PBM is used only as an external full-byte oracle comparison; all embedded expected bitmaps come from the independent Python complex recurrence.",
    ]


def not_run():
    return [
        {"item": "formal_timing", "reason": "preparation only"},
        {"item": "other_n_values", "reason": "fixed N=1,7,8,9,16,200"},
        {"item": "simd_or_multithreaded_variant", "reason": "single-thread scalar fixture only"},
        {"item": "formal_native_performance_program_from_observer", "reason": "observer binaries depend on host libc and are test-only"},
        {"item": "other_targets_or_compilers", "reason": "fixed x86_64 generic target and recorded LLVM 22.1.8"},
        {"item": "cpp_negative_observer", "reason": "C++ has O0/O2 IR call recognition and actual quiet runs; byte observation is Source Native plus generated Python"},
    ]


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, help="new output directory; must not exist")
    parser.add_argument("--official", type=Path, default=DEFAULT_OFFICIAL)
    parser.add_argument("--official-check", type=Path, default=DEFAULT_OFFICIAL_CHECK)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--python-helper", type=Path, default=DEFAULT_PYTHON_HELPER)
    parser.add_argument("--langc", type=Path, required=True)
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
        ("official", args.official), ("official-check", args.official_check),
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
