#!/usr/bin/env python3
"""Prepare the fixed N=512/2048 Mandelbrot rounds fixture without timing.

The existing Mandelbrot templates and ``prepare.py`` are inputs and are not
modified.  This script replaces only the exact old ``main`` text with a rounds
wrapper, preserving the old render/visit algorithm prefix byte-for-byte.  It
uses the frozen full PBM payloads from the independent large oracle instead of
regenerating the bitmap.  It reuses the old Mandelbrot preparation helpers,
``run_native.py``, the Sieve generic build/IR/ELF helpers, and the Fannkuch
large-preparation loop parsers.  No timing, speedup, or performance conclusion
is produced.

For each N and round count it checks: Source Native and C++ O0/O2 full-byte
programs, wrong-expected and wrong-bitmap quiet exits, Source helper object
identity, LLVM verifier results, Native observers for every round, and for R2
actual O2 IR evidence that the real f64 render work is inside the round loop
(or in equally unrolled kernel regions).  Deliberately broken LLVM controls
that remove the kernel or reduce the round bound must be rejected.
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

PERFORMANCE_DIR = Path(__file__).resolve().parent
WORKTREE = PERFORMANCE_DIR.parents[2]


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load shared helpers: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


legacy = _load("_mandelbrot_large_reuse_prepare",
               PERFORMANCE_DIR / "prepare.py")
fannkuch_large = _load("_mandelbrot_large_reuse_fannkuch_large",
                       PERFORMANCE_DIR.parent / "fannkuch" / "large_prepare.py")

need = legacy.need
now = legacy.now
file_rec = legacy.file_rec
manifest_text = legacy.manifest_text
tool = legacy.tool
run_program = legacy.run_program
require_exit = legacy.require_exit
verify_ir = legacy.verify_ir
build_langc = legacy.build_langc
build_cpp = legacy.build_cpp
helper_export = legacy.helper_export
instrument_visit = legacy.instrument_visit
compile_native_observer = legacy.compile_native_observer
emit_python = legacy.emit_python
cpp_visit_linkage = legacy.cpp_visit_linkage
cpp_call_count = legacy.cpp_call_count
compile_cpp_ir = legacy.compile_cpp_ir
parse_functions = legacy.parse_functions
elf_checks = legacy.elf_checks
TARGET_FLAGS = legacy.TARGET_FLAGS
FAST_FLOAT_RE = legacy.FAST_FLOAT_RE
FMA_RE = legacy.FMA_RE
PerfError = common.PerfError

CONTRACT = "compiler-dev/1"
TASK = "native-mandelbrot-large-preparation"
OPERATION_ID = "language-mandelbrot-large-preparation-20261110"
PREPARE_SCHEMA = "compiler-dev/native-mandelbrot-large-preparation/1"
N_VALUES = (512, 2048)
ITERATIONS = 50
DEFAULT_ORACLE_DIR = (
    WORKTREE.parent / "checks/eight-algorithm-large-oracles-20261110"
)
DEFAULT_PYTHON_HELPER = (
    WORKTREE / "build/native-cli-release/tests/python/lang_python_emit"
)
ORACLE = {
    512: {
        "sha256": "e85b68bbcfad732b65a0724613e996431b69d0b92fef6a5ca629015b4846a803",
        "bytes": 32779, "bitmap_bytes": 32768, "header": b"P4\n512 512\n",
    },
    2048: {
        "sha256": "f88585aa7e19041953729f6e29577f943a8175c3466c395725a7274b59f2b5cf",
        "bytes": 524301, "bitmap_bytes": 524288, "header": b"P4\n2048 2048\n",
    },
}
PINNED_TOOLS = {
    "langc": "a17e1cc3753371a3588db3b44a5fab36e3caff1440f9889997e416008bab73ec",
    "helper": "f24f410da99eb0f827f7031a76d4220164b50cb03070a4c382e137a12acc989e",
    "runtime": "f0c676c60a6982c5d0497e0f685ed58e3d57cc961e62ab9c62bf8579a1376d02",
    "python_helper": "ec507db4297fbf9ca17844649040da888d60ca2468e1db5e1c864b7650b93116",
}
MARKER_RE = re.compile(
    r"^//\s*PERF-MANDELBROT\s+n=(\d+)\s+iterations=(\d+)"
    r"\s+bytes=(\d+)\s+expected_sha256=([0-9a-f]{64})"
    r"\s+flip=(none|data|expected)\s*$",
    re.MULTILINE,
)
BOUND_RE = re.compile(
    r"\bicmp\s+(?:eq|ne|slt|ult|sle|ule|sgt|ugt|sge|uge)"
    r"\s+i(?:32|64)\b[^\n]*"
)

SOURCE_MAIN = """func main() i32 {
    // Read the size through a one-element runtime vector so the optimized
    // Source IR keeps the actual scalar work.
    let dims Vec[i32] = [{{N}}];
    let n i32 = dims[0];
    let expected Vec[u8] = [{{EXPECTED}}];
    let bitmap Vec[u8] = render(n);

    // Full bitmap check, including row padding bytes/bits.
    let total i32 = ((n + 7) / 8) * n;
    for var index i32 = 0; index < total; index += 1 {
        if bitmap[index] != expected[index] {
            return 1;
        }
    }

    visit(bitmap);
    return 0;
}
"""
CPP_MAIN = """int main() {
    // Mirror the Source one-element runtime size vector.
    const std::vector<int32_t> dims{ {{N}} };
    const int32_t n = dims[0];
    const std::vector<uint8_t> expected{ {{EXPECTED}} };
    const std::vector<uint8_t> bitmap = render(n);

    // Full bitmap check, including row padding bytes/bits.
    const int32_t total = ((n + 7) / 8) * n;
    for (int32_t index = 0; index < total; ++index) {
        if (bitmap[static_cast<std::size_t>(index)] !=
            expected[static_cast<std::size_t>(index)]) {
            return 1;
        }
    }

    visit(bitmap);
    return 0;
}
"""
SOURCE_WRAPPER = """func main() i32 {
    for var round i32 = 0; round < {{ROUNDS}}; round += 1 {
        // Every round reconstructs the runtime size vector, the expected
        // bitmap, and the computed bitmap, then fully compares and observes it.
        // Leaving this scope releases all three Vec values before the next round.
        let dims Vec[i32] = [{{N}}];
        let n i32 = dims[0];
        let expected Vec[u8] = [{{EXPECTED}}];
        let bitmap Vec[u8] = render(n);

        // Full bitmap check, including row padding bytes/bits.
        let total i32 = ((n + 7) / 8) * n;
        for var index i32 = 0; index < total; index += 1 {
            if bitmap[index] != expected[index] {
                return 1;
            }
        }

        visit(bitmap);
    }
    return 0;
}
"""
CPP_WRAPPER = """int main() {
    for (int32_t round = 0; round < {{ROUNDS}}; ++round) {
        // Every round reconstructs the runtime size vector, the expected
        // bitmap, and the computed bitmap, then fully compares and observes it.
        // The vectors are destroyed before the next round.
        const std::vector<int32_t> dims{ {{N}} };
        const int32_t n = dims[0];
        const std::vector<uint8_t> expected{ {{EXPECTED}} };
        const std::vector<uint8_t> bitmap = render(n);

        // Full bitmap check, including row padding bytes/bits.
        const int32_t total = ((n + 7) / 8) * n;
        for (int32_t index = 0; index < total; ++index) {
            if (bitmap[static_cast<std::size_t>(index)] !=
                expected[static_cast<std::size_t>(index)]) {
                return 1;
            }
        }

        visit(bitmap);
    }
    return 0;
}
"""


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def fill(text, values):
    for key, value in values.items():
        text = text.replace("{{" + key + "}}", str(value))
    need("{{" not in text, "rendered text still has a placeholder")
    return text


def template_records():
    names = ("mandelbrot.wz.in", "reference.cpp.in", "native_observer.cpp",
             "visit_trace.py")
    return {name: file_rec(PERFORMANCE_DIR / name) for name in names}


def read_templates():
    return {name: (PERFORMANCE_DIR / name).read_text(encoding="utf-8")
            for name in ("mandelbrot.wz.in", "reference.cpp.in")}


def load_oracle(path):
    path = Path(path).resolve()
    results_path = path / "results.json"
    data = json.loads(results_path.read_text(encoding="utf-8"))
    need(data.get("status") == "passed", "large oracle status is not passed")
    need(data.get("formal_timing") is False, "large oracle contains timing")
    need(data.get("input_sources_unchanged") is True,
         "large oracle input sources changed")
    rows = [row for row in data.get("cases", [])
            if row.get("algorithm") == "mandelbrot"]
    by_n = {int(row["n"]): row for row in rows}
    need(tuple(sorted(by_n)) == N_VALUES,
         f"large oracle mandelbrot N values are {sorted(by_n)}")
    payloads, files = {}, []
    cases = {}
    for n in N_VALUES:
        expected = ORACLE[n]
        row = by_n[n]
        pbm = Path(row["path"]).resolve()
        need(pbm == (path / f"mandelbrot-n{n}.pbm").resolve(),
             f"n={n}: oracle PBM path changed: {pbm}")
        data_bytes = pbm.read_bytes()
        need(len(data_bytes) == expected["bytes"],
             f"n={n}: PBM is {len(data_bytes)} bytes, expected {expected['bytes']}")
        need(sha256_bytes(data_bytes) == expected["sha256"],
             f"n={n}: frozen PBM SHA-256 changed")
        need(data_bytes.startswith(expected["header"]),
             f"n={n}: PBM header is not {expected['header']!r}")
        payload = data_bytes[len(expected["header"]):]
        need(len(payload) == expected["bitmap_bytes"],
             f"n={n}: payload is {len(payload)} bytes, "
             f"expected {expected['bitmap_bytes']}")
        need(row.get("sha256") == expected["sha256"]
             and int(row.get("bytes", -1)) == expected["bytes"]
             and int(row.get("bitmap_bytes", -1)) == expected["bitmap_bytes"]
             and int(row.get("n", -1)) == n,
             f"n={n}: results.json entry does not match the frozen PBM")
        need(row.get("method") ==
             "independent Python complex 50-update full bitmap, "
             "same method as official n200 control",
             f"n={n}: oracle method changed")
        payloads[n], cases[n] = payload, {
            "n": n, "pbm": file_rec(pbm), "sha256": expected["sha256"],
            "bytes": expected["bytes"], "header": expected["header"].decode("ascii"),
            "bitmap_bytes": expected["bitmap_bytes"],
            "bitmap_sha256": sha256_bytes(payload),
            "results_row": row,
        }
        files.append(pbm)
    files.append(results_path)
    return payloads, {
        "directory": str(path), "results": file_rec(results_path),
        "cases": {str(n): row for n, row in cases.items()},
        "pbm_hashes_verified": True, "payloads_embedded_not_regenerated": True,
    }, files


def expected_text(payload):
    return ",\n            ".join(str(value) for value in payload)


def zeros_text(payload):
    return ", ".join("0" for _ in payload)


def altered_payload(payload, index, mask):
    changed = bytearray(payload)
    changed[index] ^= mask
    return bytes(changed)


def verify_generated(source, reference, wanted, payload, expected, zeros,
                     rounds, language_label):
    for text, language in ((source, "Source"), (reference, "C++")):
        match = MARKER_RE.search(text)
        need(match is not None, f"{language_label}: {language} marker is missing")
        actual = (int(match.group(1)), int(match.group(2)),
                  int(match.group(3)), match.group(4), match.group(5))
        need(actual == wanted,
             f"{language_label}: {language} marker {actual} != {wanted}")
        need(expected_text(expected) in text,
             f"{language_label}: {language} does not embed the complete expected payload")
        need(f"round < {rounds};" in text,
             f"{language_label}: {language} rounds wrapper is missing")
    need("Vec[u8]" in source and zeros in source and "visit(bitmap);" in source,
         f"{language_label}: Source is missing the u8 bitmap or visit call")
    for snippet in (
        "zi = 2.0 * zr * zi + ci;", "zr = zr2 - zi2 + cr;",
        "if zr * zr + zi * zi > 4.0 {", "bitmap[byte_index] | mask",
        "for var iteration i32 = 0; iteration < 50; iteration += 1",
    ):
        need(snippet in source, f"{language_label}: Source is missing {snippet!r}")
    need("std::vector<uint8_t>" in reference
         and "std::vector<bool>" not in reference,
         f"{language_label}: C++ must use std::vector<uint8_t>, not std::vector<bool>")
    for snippet in (
        "zi = 2.0 * zr * zi + ci;", "zr = zr2 - zi2 + cr;",
        "if (zr * zr + zi * zi > 4.0) {", "| mask",
        "for (int32_t iteration = 0; iteration < 50; ++iteration)",
    ):
        need(snippet in reference, f"{language_label}: C++ is missing {snippet!r}")
    payload_sha = sha256_bytes(payload)
    need(expected_text(expected) in source and expected_text(expected) in reference,
         f"{language_label}: expected payload SHA is not tied to both programs")


def render_program(template, old_main, new_main, values, language):
    need(template.count(old_main) == 1,
         f"{language}: old main is not a unique exact match")
    prefix = template[:template.index(old_main)]
    need(prefix.count("func visit") + prefix.count("static void visit") == 1,
         f"{language}: visit is not in the preserved prefix")
    need(prefix.count("func render") + prefix.count("static std::vector<uint8_t> render") == 1,
         f"{language}: render is not in the preserved prefix")
    rendered_prefix = fill(prefix, values)
    old_rendered = fill(template, values)
    need(old_rendered.startswith(rendered_prefix),
         f"{language}: algorithm prefix differs from the old renderer")
    text = rendered_prefix + fill(new_main, values)
    forbidden = ("asm", "volatile", "noinline", "optnone", "__attribute__")
    found = [token for token in forbidden if token in text]
    need(not found, f"{language}: forbidden source tokens {found}")
    return text, {
        "old_main_occurrences": 1,
        "main_replacement_offset": len(rendered_prefix),
        "algorithm_prefix_sha256": sha256_bytes(rendered_prefix.encode()),
        "algorithm_prefix_matches_legacy_render": True,
        "algorithm_prefix_size_bytes": len(rendered_prefix.encode()),
        "new_main_sha256": sha256_bytes(
            text[len(rendered_prefix):].encode()),
        "rounds": int(values["ROUNDS"]),
    }


def generate_case(out, templates, n, rounds, payload, variant):
    byte_count = len(payload)
    index, mask = byte_count // 2, 0x80
    if variant == "correct":
        expected = payload
        flip_kind, source_flip, cpp_flip = "none", "", ""
    elif variant == "wrong-expected":
        expected = altered_payload(payload, index, mask)
        flip_kind, source_flip, cpp_flip = "expected", "", ""
    elif variant == "wrong-bitmap":
        expected = payload
        flip_kind = "data"
        source_flip = (f"    bitmap[{index}] = bitmap[{index}] ^ u8({mask});")
        cpp_flip = (f"    bitmap[{index}] = static_cast<uint8_t>("
                    f"bitmap[{index}] ^ UINT8_C({mask}));")
    else:
        raise PerfError(f"unknown variant {variant}")
    zeros = zeros_text(payload)
    mapping = {
        "N": str(n), "ROUNDS": str(rounds), "BYTE_COUNT": str(byte_count),
        "EXPECTED_SHA": sha256_bytes(expected), "FLIP_KIND": flip_kind,
        "ZEROS": zeros, "EXPECTED": expected_text(expected),
        "PACK_FLIP": source_flip,
    }
    source_text, source_render = render_program(
        templates["mandelbrot.wz.in"], SOURCE_MAIN, SOURCE_WRAPPER,
        mapping, "source")
    cpp_mapping = dict(mapping)
    cpp_mapping["PACK_FLIP"] = cpp_flip
    reference_text, reference_render = render_program(
        templates["reference.cpp.in"], CPP_MAIN, CPP_WRAPPER,
        cpp_mapping, "cpp")
    wanted = (n, ITERATIONS, byte_count, sha256_bytes(expected), flip_kind)
    verify_generated(source_text, reference_text, wanted, payload, expected,
                     zeros, rounds, f"n={n} r={rounds} {variant}")
    directory = out / "cases" / f"n{n}" / f"r{rounds}" / variant
    (directory / "src").mkdir(parents=True, exist_ok=False)
    source = directory / "src" / "main.wz"
    reference = directory / "reference.cpp"
    manifest = directory / "lang.toml"
    source.write_text(source_text, encoding="utf-8")
    reference.write_text(reference_text, encoding="utf-8")
    manifest.write_text(manifest_text(
        f"perf_mandelbrot_large_n{n}_r{rounds}_{variant.replace('-', '_')}"),
        encoding="utf-8")
    expected_path = directory / "expected.bin"
    expected_path.write_bytes(expected)
    return {
        "variant": variant, "n": n, "rounds": rounds,
        "expected_exit": 0 if variant == "correct" else 1,
        "flip_index": None if flip_kind != "data" else index,
        "flip_mask": None if flip_kind != "data" else mask,
        "expected_sha256": sha256_bytes(expected),
        "expected_file": file_rec(expected_path, out),
        "manifest": file_rec(manifest, out), "source": file_rec(source, out),
        "reference": file_rec(reference, out),
        "source_render": source_render, "reference_render": reference_render,
        "builds": [], "executions": [],
    }


def source_symbols(emitted, helper, label):
    metadata_path = Path(emitted["files"]["metadata.json"]["path"])
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    rows = [row for row in metadata["symbols"]
            if row["generated"].startswith("_f")]
    by_name = {}
    for row in rows:
        by_name.setdefault(row["source_name"], []).append(row)
    result = {}
    for name in ("main", "render", "visit"):
        need(len(by_name.get(name, [])) == 1,
             f"{label}: expected one generated {name} symbol, "
             f"got {len(by_name.get(name, []))}")
        result[name] = int(by_name[name][0]["symbol"])
    entry = int(metadata["entry_symbol"])
    need(result["main"] == entry,
         f"{label}: metadata entry {entry} is not main {result['main']}")
    need(emitted["visit_symbol"] == result["visit"],
         f"{label}: emitted visit symbol changed")
    nir = Path(helper["before"]["path"]).parent / "program.nir"
    nir_text = nir.read_text(encoding="utf-8")
    for name, symbol in result.items():
        need(re.search(rf"^function @{symbol}\b", nir_text, re.MULTILINE)
             is not None,
             f"{label}: metadata {name} symbol {symbol} is not a NativeIR function")
    return result, file_rec(nir)


def kernel_counts(text):
    return {
        "fmul_double": len(re.findall(r"\bfmul\b[^\n]*\bdouble\b", text)),
        "fadd_double": len(re.findall(r"\bfadd\b[^\n]*\bdouble\b", text)),
        "fsub_double": len(re.findall(r"\bfsub\b[^\n]*\bdouble\b", text)),
        "fdiv_double": len(re.findall(r"\bfdiv\b[^\n]*\bdouble\b", text)),
        "fcmp_double": len(re.findall(r"\bfcmp\b[^\n]*\bdouble\b", text)),
        "or_i8": len(re.findall(r"\bor\b[^\n]*\bi8\b", text)),
        "store_i8": len(re.findall(r"\bstore\b[^\n]*\bi8\b", text)),
    }


def counts_are_kernel(counts):
    return (counts["fmul_double"] >= 4 and counts["fadd_double"] >= 2
            and counts["fcmp_double"] >= 1
            and counts["or_i8"] >= 1 and counts["store_i8"] >= 1)


def function_kernel(text, sections, name):
    functions = parse_functions(text)
    if name not in functions:
        return {"function": name, "present": False, "kernel_ok": False}
    body = functions[name]["body"]
    counts = kernel_counts(body)
    loops = sections.get(name, [])
    return {
        "function": name, "present": True, "loop_count": len(loops),
        **counts, "kernel_ok": bool(loops and counts_are_kernel(counts)),
    }


def float_policy(path, label):
    text = Path(path).read_text(encoding="utf-8")
    fast = sorted(set(FAST_FLOAT_RE.findall(text)))
    fma = sorted(set(FMA_RE.findall(text)))
    need(not fast, f"{label}: fast/contract float flags found: {fast}")
    need(not fma, f"{label}: FMA intrinsic found: {fma}")
    return {"path": str(path), "fast_float_flags": fast,
            "fma_intrinsics": fma, "success": True}


def inspect_round_work(args, env, report, path, function_name, render_name,
                       rounds, label):
    text = Path(path).read_text(encoding="utf-8")
    sections, loop_row = fannkuch_large.analyze_loops(
        args, env, report, path, f"{label} loops")
    functions = parse_functions(text)
    if function_name not in functions:
        return {"label": label, "verified": False,
                "reason": f"function {function_name} is missing",
                "path": str(path), "loop_analysis": loop_row}
    blocks = fannkuch_large.function_blocks(text, function_name)
    render_work = function_kernel(text, sections, render_name) \
        if render_name else {"function": render_name, "present": False,
                             "kernel_ok": False}
    rows = []
    for record, block_count, nested in fannkuch_large.loop_regions(
            sections.get(function_name, [])):
        region = "\n".join(
            "\n".join(blocks.get(name, [])) for name in record["blocks"])
        counts = kernel_counts(region)
        calls = set(re.findall(
            r"\b(?:call|invoke)\b[^\n]*@([A-Za-z0-9_.$]+)\(", region))
        render_calls = len(re.findall(
            rf"\b(?:call|invoke)\b[^\n]*@{re.escape(render_name)}\(",
            region)) if render_name else 0
        called_kernels = [
            name for name in calls
            if function_kernel(text, sections, name)["kernel_ok"]]
        bound_hits = [line.strip() for line in region.splitlines()
                      if BOUND_RE.search(line)
                      and re.search(rf"\b{rounds}\b", line)]
        header_name = record["blocks"][0]
        boolean_round_phi = None
        for line in region.splitlines():
            match = re.match(
                r"\s*(%[A-Za-z0-9_.$]+)\s*=\s*phi\s+i1\s*"
                r"\[\s*true\s*,[^\]]*\]\s*,\s*"
                r"\[\s*false\s*,[^\]]*\]",
                line)
            if match and re.search(
                    rf"\bbr\s+i1\s+{re.escape(match.group(1))}\s*,\s*"
                    rf"label\s+%{re.escape(header_name)}\b", region):
                boolean_round_phi = line.strip()
                break
        direct_kernel = counts_are_kernel(counts)
        render_kernel = bool(render_calls and render_work["kernel_ok"])
        kernel = direct_kernel or render_kernel or bool(called_kernels)
        releases = len(re.findall(r"\blang_vec_release_v1\b", region))
        deletes = len(re.findall(r"@_ZdlPvm\(", region))
        row = {
            "line": record["line"], "blocks": block_count,
            "block_names": list(record["blocks"]),
            "nested_loops": nested, **counts,
            "render_calls": render_calls, "called_kernels": called_kernels,
            "direct_kernel": direct_kernel, "render_kernel": render_kernel,
            "kernel_present": kernel, "bound_hits": bound_hits,
            "boolean_round_phi": boolean_round_phi,
            "source_release_calls": releases, "cpp_heap_deletes": deletes,
        }
        row["mode"] = (
            "render_call_in_round_loop" if render_kernel else
            "inlined_kernel_in_round_loop" if direct_kernel else
            "called_kernel_in_round_loop" if called_kernels else
            "no_kernel")
        row["verified"] = bool(
            kernel and (bound_hits or boolean_round_phi))
        rows.append(row)
    selected = max(
        rows,
        key=lambda row: (row["verified"], row["kernel_present"],
                         len(row["bound_hits"]),
                         bool(row["boolean_round_phi"]),
                         row["nested_loops"]),
        default=None)
    kernel_regions = [row for row in rows if row["kernel_present"]]
    unrolled = (len(kernel_regions) >= rounds and rounds >= 2
                and sum(row["or_i8"] for row in kernel_regions) >= rounds)
    verified = bool(selected and selected["verified"]) or unrolled
    result = {
        "label": label, "path": str(path), "function": function_name,
        "render_function": render_name, "rounds": rounds,
        "loop_analysis": loop_row, "render_kernel": render_work,
        "regions": rows, "kernel_regions": len(kernel_regions),
        "unrolled_kernel_regions": unrolled, "selected": selected,
        "verified": verified, "status": "verified" if verified else "not_verified",
    }
    if selected and selected["verified"]:
        result["mode"] = selected["mode"]
    elif unrolled:
        result["mode"] = "unrolled_kernel_regions"
    else:
        result["mode"] = "evidence_insufficient"
    return result


def source_round_function(text, entry_symbol):
    name = f"lang.fn.{entry_symbol}"
    functions = parse_functions(text)
    if name in functions:
        return name, "entry_definition"
    if "main" in functions and re.search(
            rf"%{re.escape(name)}\.exit(?:[.:]|\b)", text):
        return "main", "entry_inlined_into_main_with_entry_label"
    raise PerfError(
        f"metadata entry {name} is neither defined nor visibly inlined into main")


def mutate_main_to_no_kernel(text, function_name, rounds):
    pattern = re.compile(
        rf"^define\b[^\n]*@{re.escape(function_name)}\([^\n]*\{{\s*$",
        re.MULTILINE)
    matches = list(pattern.finditer(text))
    need(len(matches) == 1,
         f"no-kernel control: expected one {function_name} definition")
    match = matches[0]
    end = legacy.FUNC_END_RE.search(text, match.end())
    need(end is not None, "no-kernel control: unterminated function")
    body = f"""entry:
  br label %round.test

round.test:
  %round = phi i32 [ 0, %entry ], [ %round.next, %round.body ]
  %round.next = add i32 %round, 1
  %round.done = icmp uge i32 %round.next, {rounds}
  br i1 %round.done, label %exit, label %round.body

round.body:
  br label %round.test

exit:
  ret i32 0
"""
    return text[:match.end()] + "\n" + body + text[end.start():]


def mutate_round_bound(text, function_name, rounds, out):
    result = inspect_round_work(
        out["args"], out["env"], out["report"], Path(out["path"]),
        function_name, out["render_name"], rounds, out["label"])
    selected = result.get("selected") or {}
    hits = selected.get("bound_hits", [])
    boolean_line = selected.get("boolean_round_phi")
    mutated = text
    changed = 0
    if hits or boolean_line:
        for line in hits:
            replacement = re.sub(rf"\b{rounds}\b", str(rounds - 1), line,
                                 count=1)
            if replacement != line:
                mutated = mutated.replace(line, replacement, 1)
                changed += 1
        if boolean_line:
            replacement = re.sub(r"\[\s*true\s*,", "[ false,", boolean_line,
                                 count=1)
            if replacement != boolean_line:
                mutated = mutated.replace(boolean_line, replacement, 1)
                changed += 1
        kind = "reduce-round-bound"
    else:
        need(False, f"round-bound control: no bound {rounds} in selected loop")
    need(changed > 0, "round-bound control did not change any IR line")
    return mutated, "reduce-round-bound"


def generate_real_r1_cpp_control(args, env, report, out, templates, payload,
                                 n, rounds, label):
    root = out / "controls" / "cpp-real-r1"
    case = generate_case(root, templates, n, 1, payload, "correct")
    reference = Path(case["reference"]["path"])
    program = root / "build" / "O2" / "reference"
    build = build_cpp(
        args, env, out, reference, program, 2,
        f"{label} cpp real R1 O2 build")
    run = run_program([program], args, env)
    execution = {"implementation": "cpp", "opt": 2, "variant": "correct",
                 "expected_exit": 0, "command_index": run["command_index"],
                 "success": False}
    require_exit(run, 0, f"{label} cpp real R1 O2 run")
    execution.update({"success": True, "exit": run["exit"],
                      "stdout": run["stdout"], "stderr": run["stderr"]})
    ir_path = root / "cpp-R1-O2.ll"
    ir = compile_cpp_ir(
        args, env, report, out, reference, ir_path, 2,
        f"{label} cpp real R1 O2 IR", report_check_work=False)
    text = ir.pop("text")
    cpp_render = None
    try:
        cpp_render = fannkuch_large.cpp_linkage(text, "render")
    except PerfError:
        cpp_render = None
    work = inspect_round_work(
        args, env, report, ir_path, "main", cpp_render, rounds,
        f"{label} cpp real R1 asked to prove R{rounds}")
    need(work["kernel_regions"] == 1,
         f"{label}: real R1 control does not have exactly one kernel region: "
         f"{work['kernel_regions']}")
    need(not work["verified"],
         f"{label}: real R1 LLVM was incorrectly accepted as R{rounds}")
    return {
        "implementation": "cpp", "kind": "real-r1-program-asked-as-r2",
        "path": str(ir_path), "manifest": case["manifest"],
        "source": case["source"], "reference": case["reference"],
        "build": build, "execution": execution, "ir": ir["ir"],
        "verify": ir["verify"], "work": work,
        "actual_rounds": 1, "expected_rounds_checked": rounds,
        "kernel_regions": work["kernel_regions"], "rejected": True,
    }


def negative_llvm_controls(args, env, report, out, templates, payload, n,
                           source_ir, source_function, source_render, cpp_ir,
                           cpp_render, rounds, label):
    controls = []
    specs = [
        ("source", Path(source_ir), source_function,
         f"lang.fn.{source_render}" if isinstance(source_render, int)
         else source_render),
        ("cpp", Path(cpp_ir), "main", cpp_render),
    ]
    for implementation, path, function_name, render_name in specs:
        text = path.read_text(encoding="utf-8")
        no_kernel = mutate_main_to_no_kernel(text, function_name, rounds)
        no_kernel_path = out / "negative-ir" / implementation / "no-kernel.ll"
        no_kernel_path.parent.mkdir(parents=True, exist_ok=True)
        no_kernel_path.write_text(no_kernel, encoding="utf-8")
        verify = verify_ir(no_kernel_path, args, env, report,
                           f"{label} {implementation} no-kernel verify")
        work = inspect_round_work(
            args, env, report, no_kernel_path, function_name, render_name,
            rounds, f"{label} {implementation} no-kernel")
        need(not work["verified"],
             f"{label}: no-kernel LLVM control was not rejected")
        controls.append({"implementation": implementation,
                         "kind": "remove-kernel", "path": str(no_kernel_path),
                         "verify": verify, "work": work, "rejected": True})
        if implementation == "cpp":
            controls.append(generate_real_r1_cpp_control(
                args, env, report, out, templates, payload, n, rounds,
                label))
            continue
        no_round, no_round_kind = mutate_round_bound(
            text, function_name, rounds,
            {"args": args, "env": env, "report": report, "path": str(path),
             "render_name": render_name, "label":
             f"{label} {implementation} original"})
        no_round_path = out / "negative-ir" / implementation / "no-round.ll"
        no_round_path.write_text(no_round, encoding="utf-8")
        verify = verify_ir(no_round_path, args, env, report,
                           f"{label} {implementation} no-round verify")
        work = inspect_round_work(
            args, env, report, no_round_path, function_name, render_name,
            rounds, f"{label} {implementation} reduced-round")
        need(not work["verified"],
             f"{label}: reduced-round LLVM control was not rejected")
        controls.append({"implementation": implementation,
                         "kind": no_round_kind,
                         "path": str(no_round_path), "verify": verify,
                         "work": work, "rejected": True})
    return controls


def decode_rounds(record, rounds, byte_count, label):
    text = record["stdout"].strip()
    lines = text.splitlines()
    need(len(lines) == rounds,
         f"{label}: observer printed {len(lines)} bitmaps, expected {rounds}")
    result = []
    for index, line in enumerate(lines):
        need(re.fullmatch(r"[0-9a-f]+", line) is not None,
             f"{label}: bitmap {index} is not lowercase hex")
        need(len(line) == 2 * byte_count,
             f"{label}: bitmap {index} has {len(line) // 2} bytes, "
             f"expected {byte_count}")
        result.append(bytes.fromhex(line))
    return result


def observe_native_rounds(args, env, report, out, case, opt, helper, payload,
                          rounds, label):
    before_path = Path(helper["before"]["path"])
    instrumented, visit = instrument_visit(
        before_path.read_text(encoding="utf-8"), helper["visit_symbol"], label)
    directory = out / "observers" / "native" / f"n{case['n']}" / \
        f"r{rounds}" / f"O{opt}"
    directory.mkdir(parents=True, exist_ok=False)
    observe_ll = directory / "observe.ll"
    observe_ll.write_text(instrumented, encoding="utf-8")
    verify = verify_ir(observe_ll, args, env, report,
                       f"{label} instrumented verify")
    compiled = compile_native_observer(
        args, env, out, observe_ll, directory / "observer", opt, label)
    run = run_program([directory / "observer"], args, env)
    need(run["status"] == "exited" and run["exit"] == 0,
         f"{label}: observer run failed: {run}")
    need(not run["stderr"],
         f"{label}: observer wrote stderr: {run['stderr'][-1000:]!r}")
    captured = decode_rounds(run, rounds, len(payload), label)
    hex_path = directory / "observed.hex"
    hex_path.write_text(run["stdout"], encoding="utf-8")
    matches = [value == payload for value in captured]
    need(all(matches),
         f"{label}: one or more round bitmaps differ from the frozen payload")
    row = {
        "implementation": "native", "opt": opt, "rounds": rounds,
        "visit_symbol": helper["visit_symbol"],
        "parameter_type": visit["parameter_type"],
        "argument": visit["argument"], "command_index": run["command_index"],
        "instrumented": file_rec(observe_ll, out), "verify": verify,
        "compile": compiled, "hex": file_rec(hex_path, out),
        "observed_rounds": rounds,
        "round_sha256": [sha256_bytes(value) for value in captured],
        "payload_sha256": sha256_bytes(payload),
        "all_rounds_match_payload": all(matches), "success": True,
        "formal_cli": False, "host_libc_dependent": True,
        "not_independent_native_performance_program": True,
    }
    case["native_observers"].append(row)
    return row


def base_report(args, env):
    report = common.base_report("prepare", args, env)
    report.update({
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "formal_timing": False,
        "script": file_rec(Path(__file__).resolve()),
        "common_runner": file_rec(Path(common.__file__).resolve()),
        "legacy_prepare": file_rec(PERFORMANCE_DIR / "prepare.py"),
        "fannkuch_large_helpers": file_rec(
            PERFORMANCE_DIR.parent / "fannkuch" / "large_prepare.py"),
        "sieve_helpers": file_rec(
            PERFORMANCE_DIR.parent / "sieve" / "prepare.py"),
        "templates": template_records(),
        "python_metadata_only": True,
        "python_large_bitmap_executed": False,
    })
    report["target"].update({
        "cpp_flags": list(TARGET_FLAGS),
        "cpp_linker": str(args.llvm_bin / "ld.lld"),
    })
    report["tools"]["python_helper"] = common.tool_record(
        args.python_helper, env, args.cpu, None)
    return report


def tool_hashes(args):
    paths = {
        "langc": args.langc, "helper": args.helper, "runtime": args.runtime,
        "clang": args.llvm_bin / "clang",
        "clang++": args.llvm_bin / "clang++",
        "ld.lld": args.llvm_bin / "ld.lld", "opt": args.llvm_bin / "opt",
        "llvm-readelf": args.llvm_bin / "llvm-readelf",
        "llvm-nm": args.llvm_bin / "llvm-nm",
        "python": args.python, "python_helper": args.python_helper,
    }
    return {name: file_rec(path) for name, path in paths.items()}


def input_paths(args, oracle_files):
    paths = [PERFORMANCE_DIR / name for name in (
        "mandelbrot.wz.in", "reference.cpp.in", "native_observer.cpp",
        "visit_trace.py", "prepare.py", "large_prepare.py")]
    paths += [
        PERFORMANCE_DIR.parent / "run_native.py",
        PERFORMANCE_DIR.parent / "sieve" / "prepare.py",
        PERFORMANCE_DIR.parent / "fannkuch" / "prepare.py",
        PERFORMANCE_DIR.parent / "fannkuch" / "large_prepare.py",
        WORKTREE / "runtime/native/vec.hpp",
    ]
    paths += list(oracle_files)
    return sorted({Path(path).resolve() for path in paths if Path(path).is_file()})


def snapshot(paths):
    return {str(path): common.sha256_file(path) for path in paths}


def repo_signature(record):
    return (record["head"], record["dirty"], record["status_sha256"])


def prepare(args):
    env = common.make_env(args)
    out = common.prepare_out(args.out)
    report = {
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "mode": "prepare", "status": "running",
        "commands": [], "verifies": [], "loop_analyses": [], "elf_checks": [],
        "not_verified": [], "cases": [],
    }
    common._COMMAND_LOG = report["commands"]
    try:
        report.update(base_report(args, env))
        need(args.n in N_VALUES, f"--n must be one of {N_VALUES}")
        need(args.rounds > 0, "--rounds must be an integer > 0")
        report["parameters"] = {
            "n": args.n, "rounds": args.rounds,
            "iterations": ITERATIONS,
            "coordinate": "cr=2.0*x/N-1.5, ci=2.0*y/N-1.0, z0=0",
            "escape": "after each z=z*z+c update, norm squared > 4",
            "packing": "row-major, MSB first, row padding bits zero",
            "target_flags": list(TARGET_FLAGS),
            "variant_expected_exits": {
                "correct": 0, "wrong-expected": 1, "wrong-bitmap": 1},
            "round_work_proof": "required only for R2",
            "python_metadata_only": True,
        }
        tools_before = tool_hashes(args)
        for name, expected_sha in PINNED_TOOLS.items():
            need(tools_before[name]["sha256"] == expected_sha,
                 f"frozen tool {name} SHA-256 changed: "
                 f"{tools_before[name]['sha256']}")
        report["tools_before"] = tools_before
        report["frozen_tools_verified"] = True

        payloads, oracle, oracle_files = load_oracle(args.oracle_dir)
        before = snapshot(input_paths(args, oracle_files))
        report["oracle"] = oracle
        report["input_hashes_before"] = before
        repositories_before = {
            "worktree": common.repo_info(WORKTREE, env, args.cpu),
            "compiler_root": common.repo_info(args.compiler_root, env, args.cpu),
        }
        report["repositories_before"] = repositories_before

        templates = read_templates()
        n, rounds = args.n, args.rounds
        payload = payloads[n]
        oracle_dir = out / "oracle"
        oracle_dir.mkdir(parents=True, exist_ok=False)
        payload_path = oracle_dir / f"n{n}-bitmap.bin"
        payload_path.write_bytes(payload)
        report["payload"] = file_rec(payload_path, out)

        case = {
            "n": n, "rounds": rounds, "variants": {}, "check": None,
            "builds": [], "correct_executions": [], "wrong_executions": [],
            "helper": [], "cpp_ir": {}, "python": {}, "native_observers": [],
            "source_work": None, "cpp_work": None,
            "negative_llvm_controls": [], "float_policy": [],
        }
        report["cases"].append(case)
        for variant in ("correct", "wrong-expected", "wrong-bitmap"):
            case["variants"][variant] = generate_case(
                out, templates, n, rounds, payload, variant)

        correct = case["variants"]["correct"]
        correct_manifest = Path(correct["manifest"]["path"])
        check = tool([args.langc, "check", "--project", correct_manifest,
                      "--backend", "native", "--format", "json"], args, env)
        common.require_success(check, f"n={n} r={rounds} langc check")
        check_json = common.parse_json_stdout(
            check, f"n={n} r={rounds} langc check")
        need(check_json.get("status") == "ok", "langc check is not ok")
        case["check"] = {"command_index": check["command_index"],
                         "json": check_json, "success": True}

        python_correct = emit_python(
            args, env, out, correct_manifest,
            out / "python" / f"n{n}" / f"r{rounds}" / "correct",
            f"n={n} r={rounds} correct")
        case["python"]["correct"] = python_correct
        case["python"]["executed"] = False
        case["python"]["purpose"] = "metadata only; generated main not executed"

        for opt in (0, 2):
            langc_row = build_langc(
                args, env, out, correct_manifest,
                out / "build" / f"n{n}" / f"r{rounds}" / "correct" /
                "langc" / f"O{opt}", opt,
                f"n={n} r={rounds} correct langc O{opt}")
            case["builds"].append(langc_row)
            run = run_program([langc_row["program"]["path"]], args, env)
            execution = {"implementation": "langc", "opt": opt,
                         "variant": "correct", "expected_exit": 0,
                         "command_index": run["command_index"], "success": False}
            case["correct_executions"].append(execution)
            require_exit(run, 0, f"n={n} r={rounds} correct langc O{opt}")
            execution.update({"success": True, "exit": run["exit"],
                              "stdout": run["stdout"], "stderr": run["stderr"]})
            elf_checks(args, env, report, langc_row["program"]["path"],
                       f"n={n} r={rounds} correct langc O{opt}", "langc")

            cpp_row = build_cpp(
                args, env, out, Path(correct["reference"]["path"]),
                out / "build" / f"n{n}" / f"r{rounds}" / "correct" / "cpp" /
                f"O{opt}" / "reference", opt,
                f"n={n} r={rounds} correct cpp O{opt}")
            case["builds"].append(cpp_row)
            run = run_program([cpp_row["program"]["path"]], args, env)
            execution = {"implementation": "cpp", "opt": opt,
                         "variant": "correct", "expected_exit": 0,
                         "command_index": run["command_index"], "success": False}
            case["correct_executions"].append(execution)
            require_exit(run, 0, f"n={n} r={rounds} correct cpp O{opt}")
            execution.update({"success": True, "exit": run["exit"],
                              "stdout": run["stdout"], "stderr": run["stderr"]})
            elf_checks(args, env, report, cpp_row["program"]["path"],
                       f"n={n} r={rounds} correct cpp O{opt}", "cpp")

            helper = helper_export(
                args, env, report, out, correct_manifest,
                out / "llvm" / f"n{n}" / f"r{rounds}" / f"O{opt}", opt,
                langc_row["object"]["path"], python_correct["visit_symbol"],
                f"n={n} r={rounds} correct Source O{opt}", check_work=False)
            case["helper"].append(helper)
            case["float_policy"].append(float_policy(
                helper["after"]["path"],
                f"n={n} r={rounds} source O{opt}"))
            if opt == 2:
                symbols, nir = source_symbols(
                    python_correct, helper, f"n={n} r={rounds}")
                case["source_symbols"] = symbols
                case["source_nir"] = nir
            observe_native_rounds(
                args, env, report, out, case, opt, helper, payload, rounds,
                f"n={n} r={rounds} correct O{opt} observer")

            for wrong in ("wrong-expected", "wrong-bitmap"):
                wrong_case = case["variants"][wrong]
                wrong_manifest = Path(wrong_case["manifest"]["path"])
                for implementation in ("langc", "cpp"):
                    if implementation == "langc":
                        row = build_langc(
                            args, env, out, wrong_manifest,
                            out / "build" / f"n{n}" / f"r{rounds}" / wrong /
                            "langc" / f"O{opt}", opt,
                            f"n={n} r={rounds} {wrong} langc O{opt}")
                    else:
                        row = build_cpp(
                            args, env, out, Path(wrong_case["reference"]["path"]),
                            out / "build" / f"n{n}" / f"r{rounds}" / wrong /
                            "cpp" / f"O{opt}" / "reference", opt,
                            f"n={n} r={rounds} {wrong} cpp O{opt}")
                    case["builds"].append(row)
                    run = run_program([row["program"]["path"]], args, env)
                    execution = {"implementation": implementation, "opt": opt,
                                 "variant": wrong, "expected_exit": 1,
                                 "command_index": run["command_index"],
                                 "success": False}
                    case["wrong_executions"].append(execution)
                    require_exit(run, 1,
                                 f"n={n} r={rounds} {wrong} {implementation} O{opt}")
                    execution.update({"success": True, "exit": run["exit"],
                                      "stdout": run["stdout"],
                                      "stderr": run["stderr"]})
                    elf_checks(args, env, report, row["program"]["path"],
                               f"n={n} r={rounds} {wrong} {implementation} O{opt}",
                               implementation)

        for opt in (0, 2):
            ir = compile_cpp_ir(
                args, env, report, out, Path(correct["reference"]["path"]),
                out / "llvm" / f"n{n}" / f"r{rounds}" / f"cpp-O{opt}.ll", opt,
                f"n={n} r={rounds} cpp O{opt} IR",
                report_check_work=False)
            case["cpp_ir"][f"O{opt}"] = ir
            case["float_policy"].append(float_policy(
                ir["ir"]["path"], f"n={n} r={rounds} cpp O{opt}"))
        linkage = cpp_visit_linkage(
            case["cpp_ir"]["O0"]["text"], f"n={n} r={rounds} cpp O0")
        cpp_text = case["cpp_ir"]["O2"]["text"]
        for opt in (0, 2):
            ir = case["cpp_ir"][f"O{opt}"]
            ir["visit_linkage"] = linkage
            ir["visit_calls"] = cpp_call_count(ir["text"], linkage)
            ir["text"] = ""
        need(case["cpp_ir"]["O0"]["visit_calls"] > 0,
             "C++ O0 positive mangled visit-call control failed")
        need(case["cpp_ir"]["O2"]["visit_calls"] == 0,
             "C++ O2 still has actual visit calls")

        source_symbols_map = case["source_symbols"]
        source_text = Path(case["helper"][1]["after"]["path"]).read_text(
            encoding="utf-8")
        source_function, source_selection = source_round_function(
            source_text, source_symbols_map["main"])
        cpp_render = None
        try:
            cpp_render = fannkuch_large.cpp_linkage(cpp_text, "render")
        except PerfError:
            cpp_render = None
        checked_rounds = rounds if rounds >= 2 else 2
        source_work = inspect_round_work(
            args, env, report, Path(case["helper"][1]["after"]["path"]),
            source_function, f"lang.fn.{source_symbols_map['render']}",
            checked_rounds,
            f"n={n} r={rounds} Source O2 asked to prove R{checked_rounds}")
        source_work["function_selection"] = source_selection
        source_work["entry_symbol"] = source_symbols_map["main"]
        source_work["requested_rounds"] = checked_rounds
        cpp_work = inspect_round_work(
            args, env, report, Path(case["cpp_ir"]["O2"]["ir"]["path"]),
            "main", cpp_render, checked_rounds,
            f"n={n} r={rounds} cpp O2 asked to prove R{checked_rounds}")
        cpp_work["requested_rounds"] = checked_rounds
        case["source_work"], case["cpp_work"] = source_work, cpp_work
        if rounds >= 2:
            need(source_work["verified"],
                 f"n={n} r={rounds}: Source O2 round work was not verified: "
                 f"{source_work}")
            need(cpp_work["verified"],
                 f"n={n} r={rounds}: C++ O2 round work was not verified: "
                 f"{cpp_work}")
            case["negative_llvm_controls"] = negative_llvm_controls(
                args, env, report, out, templates, payload, n,
                case["helper"][1]["after"]["path"],
                source_function, source_symbols_map["render"],
                case["cpp_ir"]["O2"]["ir"]["path"], cpp_render, rounds,
                f"n={n} r={rounds}")
        else:
            need(not source_work["verified"],
                 f"n={n} r=1: Source R1 IR was incorrectly accepted as R2")
            need(not cpp_work["verified"],
                 f"n={n} r=1: C++ R1 IR was incorrectly accepted as R2")
            source_work["r1_cannot_prove_r2"] = True
            cpp_work["r1_cannot_prove_r2"] = True

        tools_after = tool_hashes(args)
        report["tools_after"] = tools_after
        report["tools_unchanged"] = tools_before == tools_after
        need(report["tools_unchanged"], "tool SHA-256 values changed")
        after = snapshot(input_paths(args, oracle_files))
        report["input_hashes_after"] = after
        report["inputs_unchanged"] = before == after
        need(report["inputs_unchanged"], "source/tool/oracle inputs changed")
        repositories_after = {
            "worktree": common.repo_info(WORKTREE, env, args.cpu),
            "compiler_root": common.repo_info(args.compiler_root, env, args.cpu),
        }
        report["repositories_after"] = repositories_after
        report["compiler_root_unchanged"] = (
            repo_signature(repositories_before["compiler_root"])
            == repo_signature(repositories_after["compiler_root"]))
        need(report["compiler_root_unchanged"],
             "compiler-root HEAD/status changed during prepare")
        report["worktree_note"] = (
            "The worktree status intentionally changes because this run writes "
            "its new checks output directory; fixed input and tool hashes are "
            "the stability evidence.")

        correct_runs = case["correct_executions"]
        wrong = case["wrong_executions"]
        helper = case["helper"]
        native = case["native_observers"]
        summary = {
            "n": n, "rounds": rounds,
            "correct_executions": len(correct_runs),
            "correct_quiet_exit_0": sum(
                1 for row in correct_runs if row["success"] and row["exit"] == 0
                and not row["stdout"] and not row["stderr"]),
            "wrong_executions": len(wrong),
            "wrong_expected_quiet_exit_1": sum(
                1 for row in wrong if row["variant"] == "wrong-expected"
                and row["success"] and row["exit"] == 1
                and not row["stdout"] and not row["stderr"]),
            "wrong_bitmap_quiet_exit_1": sum(
                1 for row in wrong if row["variant"] == "wrong-bitmap"
                and row["success"] and row["exit"] == 1
                and not row["stdout"] and not row["stderr"]),
            "builds": len(case["builds"]),
            "source_builds": sum(1 for row in case["builds"]
                                 if row["implementation"] == "langc"),
            "cpp_builds": sum(1 for row in case["builds"]
                              if row["implementation"] == "cpp"),
            "source_llvm_exports": len(helper),
            "source_ir_verifies": 2 * len(helper),
            "source_object_matches": sum(
                1 for row in helper if row["object_match"]),
            "cpp_ir_exports": 2, "cpp_ir_verifies": 2,
            "all_ir_verifies": len(report["verifies"]),
            "source_o0_positive_cases": sum(
                1 for row in helper if row["opt"] == 0
                and row["visit_calls_before"] > 0
                and row["visit_calls_after"] > 0),
            "source_o2_zero_cases": sum(
                1 for row in helper if row["opt"] == 2
                and row["visit_calls_before"] > 0
                and row["visit_calls_after"] == 0),
            "source_o2_visit_calls": sum(
                row["visit_calls_after"] for row in helper if row["opt"] == 2),
            "cpp_o0_positive_cases": int(
                case["cpp_ir"]["O0"]["visit_calls"] > 0),
            "cpp_o2_zero_cases": int(
                case["cpp_ir"]["O2"]["visit_calls"] == 0),
            "cpp_o2_visit_calls": case["cpp_ir"]["O2"]["visit_calls"],
            "native_observers": len(native),
            "native_observer_rounds": sum(
                row["observed_rounds"] for row in native),
            "native_all_rounds_match": sum(
                1 for row in native if row["all_rounds_match_payload"]),
            "observers_o0_o2": sorted(row["opt"] for row in native),
            "elf_checks": len(report["elf_checks"]),
            "source_static_elf_cases": sum(
                1 for row in report["elf_checks"]
                if row["implementation"] == "langc"),
            "cpp_dynamic_elf_cases": sum(
                1 for row in report["elf_checks"]
                if row["implementation"] == "cpp"),
            "r2_source_work_verified": bool(
                rounds >= 2 and case["source_work"]["verified"]),
            "r2_cpp_work_verified": bool(
                rounds >= 2 and case["cpp_work"]["verified"]),
            "r1_actual_round2_checks": 2 if rounds == 1 else 0,
            "r1_source_round2_claim_rejected": bool(
                rounds == 1 and not case["source_work"]["verified"]),
            "r1_cpp_round2_claim_rejected": bool(
                rounds == 1 and not case["cpp_work"]["verified"]),
            "negative_llvm_controls": len(case["negative_llvm_controls"]),
            "negative_llvm_controls_rejected": sum(
                1 for row in case["negative_llvm_controls"] if row["rejected"]),
            "python_metadata_emitted": True,
            "python_large_bitmap_executed": False,
            "timing_performed": False,
        }
        needed = sorted({library for row in report["elf_checks"]
                         for library in row["needed"]})
        summary["cpp_needed_libraries"] = needed
        expected_counts = {
            "n": n, "rounds": rounds,
            "correct_executions": 4, "correct_quiet_exit_0": 4,
            "wrong_executions": 8, "wrong_expected_quiet_exit_1": 4,
            "wrong_bitmap_quiet_exit_1": 4,
            "builds": 12, "source_builds": 6, "cpp_builds": 6,
            "source_llvm_exports": 2, "source_ir_verifies": 4,
            "source_object_matches": 2, "cpp_ir_exports": 2,
            "cpp_ir_verifies": 2,
            "all_ir_verifies": 12 if rounds >= 2 else 8,
            "source_o0_positive_cases": 1, "source_o2_zero_cases": 1,
            "source_o2_visit_calls": 0, "cpp_o0_positive_cases": 1,
            "cpp_o2_zero_cases": 1, "cpp_o2_visit_calls": 0,
            "native_observers": 2, "native_observer_rounds": 2 * rounds,
            "native_all_rounds_match": 2, "observers_o0_o2": [0, 2],
            "elf_checks": 12, "source_static_elf_cases": 6,
            "cpp_dynamic_elf_cases": 6,
            "r2_source_work_verified": rounds >= 2,
            "r2_cpp_work_verified": rounds >= 2,
            "r1_actual_round2_checks": 2 if rounds == 1 else 0,
            "r1_source_round2_claim_rejected": rounds == 1,
            "r1_cpp_round2_claim_rejected": rounds == 1,
            "negative_llvm_controls": 4 if rounds >= 2 else 0,
            "negative_llvm_controls_rejected": 4 if rounds >= 2 else 0,
            "python_metadata_emitted": True,
            "python_large_bitmap_executed": False,
            "timing_performed": False,
        }
        need(summary == {**expected_counts,
                         "cpp_needed_libraries": needed},
             f"prepare summary is unexpected: {summary}")
        report["summary"] = summary
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
        "The frozen PBM payloads are read and hash/size/header checked, not regenerated and not sampled.",
        "The programs print nothing; correctness is the complete bitmap byte comparison with the frozen payload.",
        "Source Native observers are test-only instrumented binaries linked with the host libc and project runtime; they are not standalone Native performance programs.",
        "The Python helper is used only to obtain metadata symbols; no generated Python program executes an N=512 or N=2048 bitmap.",
        "N=512 and N=2048 are divisible by 8, so this large fixture has no padding negative control. The old small-N padding checks remain unchanged and are not re-run here.",
        "R1 does not claim repeated rounds; its real O2 Source/C++ IR is actually passed to the R2 analyzer and rejected, not hand-filled. R2 uses static O2 IR analysis on both Source Native and C++; Source observers additionally record every round's complete bitmap.",
        "The Source reduced-round control changes the two-iteration boolean loop control to one actual round. The C++ reduced-round control is a real R1 program generated from the same renderer, oracle, and prefix, built normally at O2, run to a quiet exit 0 over the complete bitmap, and then rejected when the same analyzer is asked for two rounds. The no-kernel controls replace the selected wrapper with a valid integer-only loop.",
        "Vectorized or unrolled legal C++ work is recognized by f64 and i8 operation counts inside the round/kernel regions; optimization is not disabled.",
        "Only N=512/2048, R=1/2, the fixed x86_64 generic target, and the recorded LLVM 22.1.8 toolchain are covered.",
    ]


def not_run():
    return [
        {"item": "formal_timing", "reason": "this operation is preparation only"},
        {"item": "python_large_bitmap_execution", "reason": "metadata-only helper use; Native observations cover Source rounds"},
        {"item": "large_padding_negative", "reason": "N=512 and N=2048 are divisible by 8; old small-N padding cases are unchanged"},
        {"item": "old_small_scale_prepare", "reason": "old prepare/templates remain unchanged inputs and are hash-checked"},
        {"item": "simd_or_multithreaded_variant", "reason": "fixture keeps the same single-thread scalar algorithm"},
        {"item": "other_targets_or_compilers", "reason": "fixed x86_64 generic target and recorded LLVM toolchain only"},
    ]


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, help="new output directory; must not exist")
    parser.add_argument("--n", type=int, choices=N_VALUES, required=True)
    parser.add_argument("--rounds", type=int, required=True)
    parser.add_argument("--oracle-dir", type=Path, default=DEFAULT_ORACLE_DIR)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--python-helper", type=Path, default=DEFAULT_PYTHON_HELPER)
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
    need(args.n in N_VALUES, f"--n must be one of {N_VALUES}")
    need(args.rounds > 0, "--rounds must be an integer > 0")


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
