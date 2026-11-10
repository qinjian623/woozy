#!/usr/bin/env python3
"""Prepare the n=9/10 Fannkuch-redux rounds fixture without timing.

The existing n=3..7 templates and prepare script are inputs and are not
modified.  This script replaces only the exact old ``main`` text with a
rounds wrapper, preserving the algorithm function text from the old renderer.
It reuses the existing Fannkuch preparation helpers and the generic Sieve
build/IR/ELF helpers.  The frozen large oracle is hash-checked; its full Python
trace is not re-executed.  No timing or performance conclusion is produced.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
import re
import sys
import traceback
from datetime import datetime, timezone
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


legacy = _load("_fannkuch_large_reuse_legacy_prepare",
               PERFORMANCE_DIR / "prepare.py")
sieve = _load("_fannkuch_large_reuse_sieve_prepare",
              PERFORMANCE_DIR.parent / "sieve" / "prepare.py")

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
emit_python = legacy.emit_python
export_source_llvm = legacy.export_source_llvm
compile_cpp_ir = legacy.compile_cpp_ir
cpp_visit_linkage = legacy.cpp_visit_linkage
cpp_call_count = legacy.cpp_call_count
parse_functions = sieve.parse_functions
elf_checks = sieve.elf_checks
PerfError = common.PerfError

CONTRACT = "compiler-dev/1"
TASK = "native-fannkuch-large-preparation"
OPERATION_ID = "language-fannkuch-large-prepare-20261110"
PREPARE_SCHEMA = "compiler-dev/native-fannkuch-large-preparation/1"
DEFAULT_ORACLE_DIR = Path(
    "/home/qin/Work/language-dev-20261103/checks/fannkuch-large-oracle-20261110")
DEFAULT_PYTHON_HELPER = Path(
    "/home/qin/Work/language-dev-20261103/compiler/build/native-cli-release/"
    "tests/python/lang_python_emit")
N_VALUES = (9, 10)
ORACLE_N_VALUES = (7, 8, 9, 10)
EXPECTED = {7: (228, 16), 8: (1616, 22), 9: (8629, 30), 10: (73196, 38)}
TRACE_SHA = {
    7: "42f91ab9d89ba54cbb6451ad60d6ac0f17db8f865c5da0eb9fd6d8845a37087e",
    8: "5acab58c6585c5bedd0dd2a4f590a4141116b8aa1e8c075eccf22f1051c2bd9b",
    9: "ee9a44472e34ef07468793c7ae739d05597e66b5c5ae3cb799260f02c5d5836e",
    10: "72c9705b5a1e98902428cd9de98ddc7fc9ed83ae7b4d3f2bb666cee9f86bbed3",
}
PINNED_SHA = {
    "results.json": "67803f179ff1b50134d88b1def7553fe5ccd5ea2445529d9a46164d709489c5d",
    "inputs.json": "70fd05c73f054919619128a93a4d572f44a287416a4be8353839581e61a60da6",
    "verify.py": "0990beebb7d54d6869f512ef32d5c1c39065ceb1bdf907dadca383b5894a220c",
}
OLD_ORACLE = Path(
    "/home/qin/Work/language-dev-20261103/checks/fannkuch-oracle-20261109")
TARGET_FLAGS = legacy.TARGET_FLAGS
TEMPLATE_NAMES = ("fannkuch.wz.in", "reference.cpp.in")
FORBIDDEN = ("asm", "volatile", "noinline", "optnone", "__attribute__")

SOURCE_MAIN = """func main() i32 {
    let result = fannkuch({{N}});
    let expected_checksum i64 = {{CHECKSUM}};
    let expected_maximum i32 = {{MAXIMUM}};
    if result.checksum != expected_checksum {
        return 1;
    }
    if result.maximum != expected_maximum {
        return 2;
    }
    return 0;
}
"""
CPP_MAIN = """int main() {
    const FannkuchResult result = fannkuch({{N}});
    const int64_t expected_checksum = INT64_C({{CHECKSUM}});
    const int32_t expected_maximum = INT32_C({{MAXIMUM}});
    if (result.checksum != expected_checksum) {
        return 1;
    }
    if (result.maximum != expected_maximum) {
        return 2;
    }
    return 0;
}
"""
SOURCE_WRAPPER = """func main() i32 {
    for var round i32 = 0; round < {{ROUNDS}}; round += 1 {
        let result = fannkuch({{N}});
        if result.checksum != {{CHECKSUM}} {
            return 1;
        }
        if result.maximum != {{MAXIMUM}} {
            return 2;
        }
    }
    return 0;
}
"""
CPP_WRAPPER = """int main() {
    for (int32_t round = 0; round < {{ROUNDS}}; ++round) {
        const FannkuchResult result = fannkuch({{N}});
        if (result.checksum != INT64_C({{CHECKSUM}})) {
            return 1;
        }
        if (result.maximum != INT32_C({{MAXIMUM}})) {
            return 2;
        }
    }
    return 0;
}
"""


def fill(text, values):
    for key, value in values.items():
        text = text.replace(key, value)
    need("{{" not in text, "rendered text still has a placeholder")
    return text


def template_records():
    return {name: file_rec(PERFORMANCE_DIR / name) for name in TEMPLATE_NAMES}


def read_templates():
    return {name: (PERFORMANCE_DIR / name).read_text(encoding="utf-8")
            for name in TEMPLATE_NAMES}


def render_program(template, n, rounds, checksum, maximum, language):
    old_main = SOURCE_MAIN if language == "source" else CPP_MAIN
    new_main = SOURCE_WRAPPER if language == "source" else CPP_WRAPPER
    need(template.count(old_main) == 1,
         f"{language}: old main is not a unique exact match")
    prefix = template[:template.index(old_main)]
    need(prefix.count("func fannkuch") + prefix.count("static FannkuchResult fannkuch") == 1,
         f"{language}: algorithm function is not in the preserved prefix")
    values = {
        "{{N}}": str(n), "{{CHECKSUM}}": str(checksum),
        "{{MAXIMUM}}": str(maximum), "{{ROUNDS}}": str(rounds),
        "{{ZEROS}}": ", ".join("0" for _ in range(n)),
        "{{VISIT_VALUES}}": "perm1", "{{VISIT_FLIPS}}": "flips",
    }
    text = fill(template.replace(old_main, new_main), values)
    rendered_prefix = fill(prefix, values)
    need(text.startswith(rendered_prefix),
         f"{language}: algorithm prefix changed during main replacement")
    old_rendered = legacy.render(template, n, checksum, maximum, "perm1", "flips")
    need(old_rendered.startswith(rendered_prefix),
         f"{language}: algorithm prefix differs from the old renderer")
    forbidden = [token for token in FORBIDDEN if token in text]
    need(not forbidden, f"{language}: forbidden source tokens {forbidden}")
    main_text = text[len(rendered_prefix):]
    return text, {
        "old_main_occurrences": 1,
        "main_replacement_offset": len(rendered_prefix),
        "algorithm_prefix_sha256": common.sha256_text(rendered_prefix),
        "algorithm_prefix_matches_legacy_render": True,
        "algorithm_prefix_size_bytes": len(rendered_prefix.encode()),
        "old_main_sha256": common.sha256_text(old_main),
        "new_main_sha256": common.sha256_text(main_text),
        "forbidden_tokens": forbidden,
    }


def generate_case(out, templates, n, rounds, expected, variant):
    checksum, maximum = expected
    if variant == "wrong-checksum":
        checksum += 1
    elif variant == "wrong-maximum":
        maximum += 1
    directory = out / "cases" / f"n{n}" / f"r{rounds}" / variant
    (directory / "src").mkdir(parents=True, exist_ok=False)
    source_text, source_render = render_program(
        templates["fannkuch.wz.in"], n, rounds, checksum, maximum, "source")
    reference_text, reference_render = render_program(
        templates["reference.cpp.in"], n, rounds, checksum, maximum, "cpp")
    source, reference = directory / "src" / "main.wz", directory / "reference.cpp"
    manifest = directory / "lang.toml"
    source.write_text(source_text, encoding="utf-8")
    reference.write_text(reference_text, encoding="utf-8")
    manifest.write_text(manifest_text(
        f"perf_fannkuch_large_n{n}_r{rounds}_{variant.replace('-', '_')}"),
        encoding="utf-8")
    return {
        "variant": variant, "n": n, "rounds": rounds,
        "expected_checksum": checksum, "expected_maximum": maximum,
        "expected_exit": {"correct": 0, "wrong-checksum": 1,
                          "wrong-maximum": 2}[variant],
        "manifest": file_rec(manifest), "source": file_rec(source),
        "reference": file_rec(reference),
        "source_render": source_render, "reference_render": reference_render,
    }


def load_oracle(path):
    path = Path(path).resolve()
    for name, expected_sha in PINNED_SHA.items():
        target = path / name
        need(target.is_file(), f"missing frozen oracle file: {target}")
        need(common.sha256_file(target) == expected_sha,
             f"{target}: SHA-256 is not the pinned large-oracle hash")
    results_path, inputs_path, verify_path = (
        path / "results.json", path / "inputs.json", path / "verify.py")
    data = json.loads(results_path.read_text(encoding="utf-8"))
    inputs = json.loads(inputs_path.read_text(encoding="utf-8"))
    need(data.get("status") == "passed", "large oracle status is not passed")
    need(data.get("formal_timing") is False, "large oracle contains timing")
    need(data.get("inputs_unchanged") is True, "large oracle inputs changed")
    need(data.get("methods") == [
        "recursive prefix rotations with slice flips",
        "itertools permutations with inverse mixed-radix rank and pair-swap flips",
    ], "large oracle methods changed")
    need(data.get("record_comparison") ==
         "per-index SHA256 of index/permutation/flips; bijection checked by bitset",
         "large oracle record comparison changed")
    need(inputs.get("n_values") == list(ORACLE_N_VALUES),
         "large oracle inputs n_values changed")
    rows = {int(row["n"]): row for row in data.get("cases", [])}
    need(tuple(sorted(rows)) == ORACLE_N_VALUES,
         f"large oracle n values are {sorted(rows)}")
    expected = {}
    for n in ORACLE_N_VALUES:
        row = rows[n]
        need(int(row["permutations"]) == math.factorial(n),
             f"n={n}: oracle permutation count is wrong")
        need((int(row["checksum"]), int(row["max_flips"])) == EXPECTED[n],
             f"n={n}: oracle checksum/maximum differs from the pinned values")
        need(row["all_index_permutation_flip_records_sha256_matched"] is True,
             f"n={n}: oracle record comparison is not true")
        need(row["ordered_trace_sha256"] == TRACE_SHA[n],
             f"n={n}: oracle ordered trace SHA-256 changed")
        expected[n] = EXPECTED[n]
    source_sha = data.get("source_sha256")
    need(isinstance(source_sha, dict), "large oracle source_sha256 is missing")
    wanted_sources = {
        OLD_ORACLE / "oracle_recursive.py", OLD_ORACLE / "oracle_ranked.py",
        OLD_ORACLE / "results.json", inputs_path, verify_path,
    }
    need({str(Path(key).resolve()) for key in source_sha} ==
         {str(item.resolve()) for item in wanted_sources},
         "large oracle source_sha256 keys changed")
    for source, expected_sha in source_sha.items():
        source = Path(source)
        need(source.is_file(), f"missing oracle source file: {source}")
        need(common.sha256_file(source) == expected_sha,
             f"{source}: oracle source SHA-256 changed")
    need(common.sha256_file(OLD_ORACLE / "results.json") ==
         "37ee2401f703a8a12741e57f7515be96506b7c71e92df369d63354905c74f055",
         "previous n=3..7 oracle results changed")
    files = sorted({item.resolve() for item in path.iterdir() if item.is_file()} |
                   {item.resolve() for item in wanted_sources})
    return expected, {
        "directory": str(path), "results": file_rec(results_path),
        "inputs": file_rec(inputs_path), "verifier": file_rec(verify_path),
        "pinned_sha256": dict(PINNED_SHA), "verifier_executed": False,
        "methods": data["methods"], "record_comparison": data["record_comparison"],
        "cases": {str(n): {"checksum": EXPECTED[n][0],
                            "maximum": EXPECTED[n][1],
                            "permutations": math.factorial(n),
                            "ordered_trace_sha256": TRACE_SHA[n]}
                  for n in ORACLE_N_VALUES},
        "source_sha256_verified": True,
    }, files


def source_metadata(args, env, manifest, out_dir, label):
    row = emit_python(args, env, manifest, out_dir, label)
    metadata = json.loads(
        Path(row["files"]["metadata.json"]["path"]).read_text(encoding="utf-8"))
    found = [item for item in metadata["symbols"]
             if item["source_name"] == "fannkuch"
             and item["generated"].startswith("_f")]
    need(len(found) == 1, f"{label}: expected one generated fannkuch symbol")
    row["fannkuch_symbol"] = int(found[0]["symbol"])
    return row


FUNC_RE = re.compile(r"^Loop info for function '([^']+)':\s*$")
LOOP_RE = re.compile(r"Loop at depth (\d+) containing:(.*)$")
LABEL_RE = re.compile(r"^([A-Za-z0-9_.$]+):")


def parse_loop_output(text):
    sections, current = {}, None
    for line in text.splitlines():
        match = FUNC_RE.match(line)
        if match:
            current = match.group(1)
            sections.setdefault(current, [])
            continue
        match = LOOP_RE.search(line)
        if current is not None and match:
            sections[current].append({
                "depth": int(match.group(1)),
                "blocks": re.findall(r"%([A-Za-z0-9_.$]+)", match.group(2)),
                "line": line.strip(),
            })
    return sections


def analyze_loops(args, env, report, path, label):
    result = tool([args.llvm_bin / "opt", "-passes=print<loops>",
                   "-disable-output", path], args, env)
    row = {"path": str(path), "command_index": result["command_index"],
           "success": False}
    report["loop_analyses"].append(row)
    common.require_success(result, label)
    sections = parse_loop_output(result["stderr"])
    row.update({"success": True,
                "functions": {name: len(loops)
                              for name, loops in sections.items()},
                "loop_count": sum(len(loops) for loops in sections.values())})
    return sections, row


def function_blocks(text, name):
    functions = parse_functions(text)
    need(name in functions, f"LLVM function {name} is missing")
    blocks, current = {}, None
    for line in functions[name]["body"].splitlines():
        match = LABEL_RE.match(line)
        if match:
            current = match.group(1)
            blocks[current] = []
        elif current is not None:
            blocks[current].append(line)
    return blocks


def count_blocks(blocks, block_text, pattern):
    return sum(len(re.findall(pattern, "\n".join(block_text.get(name, []))))
               for name in blocks)


def loop_regions(records):
    rows = []
    for index, record in enumerate(records):
        if record["depth"] != 1:
            continue
        child = index + 1
        nested = 0
        while child < len(records) and records[child]["depth"] > 1:
            nested += 1
            child += 1
        rows.append((record, len(record["blocks"]), nested))
    return rows


def metadata_nodes(text):
    return {int(match.group(1)): match.group(2)
            for match in re.finditer(r"^!(\d+) = (.*)$", text, re.MULTILINE)}


def debug_scope(nodes, number, cache, seen=None):
    if number in cache:
        return cache[number]
    seen = set() if seen is None else seen
    if number in seen or number not in nodes:
        return None
    seen.add(number)
    record = nodes[number]
    match = re.search(r'DISubprogram\(name: "([^"]+)"', record)
    value = match.group(1) if match else None
    if value is None:
        match = re.search(r"\bscope: !(\d+)", record)
        if match:
            value = debug_scope(nodes, int(match.group(1)), cache, seen)
    cache[number] = value
    return value


def debug_count(body, nodes, target, cache):
    return sum(1 for line in body.splitlines()
               for match in re.finditer(r"!dbg !(\d+)", line)
               if debug_scope(nodes, int(match.group(1)), cache) == target)


def debug_count_blocks(blocks, block_text, nodes, target, cache):
    return sum(debug_count("\n".join(block_text.get(name, [])),
                           nodes, target, cache) for name in blocks)


def cpp_linkage(text, name):
    found = set(re.findall(
        rf'DISubprogram\(name: "{re.escape(name)}", linkageName: "([^"]+)"',
        text))
    need(len(found) == 1, f"C++ {name} debug symbol is not unique: {sorted(found)}")
    return next(iter(found))


def cpp_algorithm_evidence(text, opt, sections, label):
    nodes, cache = metadata_nodes(text), {}
    functions = parse_functions(text)
    linkage = cpp_linkage(text, "fannkuch")
    if opt == 0:
        need(linkage in functions, f"{label}: fannkuch function is missing")
        body = functions[linkage]["body"]
        loops = sections.get(linkage, [])
        constructors = len(re.findall(
            r"\b(?:call|invoke)\b[^\n]*@_ZNSt6vectorIiSaIiEEC2EmRKiRKS0_\(",
            body))
        evidence = debug_count(body, nodes, "fannkuch", cache)
        need(len(loops) >= 4 and constructors == 3 and evidence > 0,
             f"{label}: fannkuch function/loops/allocations are missing")
        return {"mode": "function", "linkage": linkage,
                "function_loops": len(loops), "vector_constructors": constructors,
                "fannkuch_debug_locations": evidence}
    main = functions["main"]["body"]
    loops = sections.get("main", [])
    evidence = debug_count(main, nodes, "fannkuch", cache)
    allocations = len(re.findall(
        r"\b(?:call|invoke)\b[^\n]*@_Znwm\(", main))
    calls = len(re.findall(
        rf"\b(?:call|invoke)\b[^\n]*@{re.escape(linkage)}\(", main))
    need(len(loops) >= 4 and evidence >= 20 and allocations == 3,
         f"{label}: inlined fannkuch loops/allocations/debug evidence is missing")
    return {"mode": "inlined" if linkage not in functions else "calls",
            "linkage": linkage, "main_loops": len(loops),
            "fannkuch_debug_locations": evidence,
            "heap_allocations": allocations, "fannkuch_calls": calls}


def source_round_work(args, env, report, after_path, fannkuch_symbol, label):
    text = Path(after_path).read_text(encoding="utf-8")
    sections, loop_row = analyze_loops(args, env, report, after_path, label)
    blocks = function_blocks(text, "main")
    defined = re.search(
        rf"^define[^\n]*@lang\.fn\.{int(fannkuch_symbol)}\(",
        text, re.MULTILINE) is not None
    rows, best = [], None
    for record, block_count, nested in loop_regions(sections.get("main", [])):
        allocs = count_blocks(record["blocks"], blocks,
                              r"\b(?:call|invoke)\b[^\n]*@lang_vec_new_v1\(")
        releases = count_blocks(record["blocks"], blocks,
                                r"\b(?:call|invoke)\b[^\n]*@lang_vec_release_v1\(")
        inits = count_blocks(record["blocks"], blocks,
                             r"\b(?:call|invoke)\b[^\n]*@lang_vec_init_v1\(")
        calls = count_blocks(record["blocks"], blocks,
                             rf"\b(?:call|invoke)\b[^\n]*@lang\.fn\.{int(fannkuch_symbol)}\(")
        row = {"line": record["line"], "blocks": block_count,
               "nested_loops": nested, "vec_new_calls": allocs,
               "vec_release_calls": releases, "vec_init_calls": inits,
               "fannkuch_calls": calls}
        rows.append(row)
        if calls > 0:
            row["mode"], row["verified"] = "fannkuch_call_in_round_loop", True
        elif allocs == 3 and releases >= 3 and inits >= 3 and nested >= 4:
            row["mode"] = "inlined_allocations_and_nested_loops"
            row["verified"] = True
        else:
            row["mode"], row["verified"] = "evidence_insufficient", False
        if best is None or (row["verified"], row["vec_new_calls"],
                            row["nested_loops"]) > (
                best["verified"], best["vec_new_calls"],
                best["nested_loops"]):
            best = row
    result = {"label": label, "definition_present": defined,
              "loop_analysis": loop_row, "regions": rows,
              "verified": bool(best and best["verified"]),
              "selected": best, "status": "not_verified"}
    if result["verified"]:
        result["status"] = "verified"
    result["after_ir"] = file_rec(after_path)
    return result


def cpp_round_work(args, env, report, text, sections, linkage, label):
    nodes, cache = metadata_nodes(text), {}
    blocks = function_blocks(text, "main")
    defined = linkage in parse_functions(text)
    rows, best = [], None
    for record, block_count, nested in loop_regions(sections.get("main", [])):
        allocs = count_blocks(record["blocks"], blocks,
                              r"\b(?:call|invoke)\b[^\n]*@_Znwm\(")
        deletes = count_blocks(record["blocks"], blocks,
                               r"\b(?:call|invoke)\b[^\n]*@_ZdlPvm\(")
        calls = count_blocks(record["blocks"], blocks,
                             rf"\b(?:call|invoke)\b[^\n]*@{re.escape(linkage)}\(")
        evidence = debug_count_blocks(record["blocks"], blocks, nodes,
                                      "fannkuch", cache)
        row = {"line": record["line"], "blocks": block_count,
               "nested_loops": nested, "heap_allocations": allocs,
               "heap_deletes": deletes, "fannkuch_calls": calls,
               "fannkuch_debug_locations": evidence}
        rows.append(row)
        if calls > 0:
            row["mode"], row["verified"] = "fannkuch_call_in_round_loop", True
        elif allocs == 3 and deletes >= 3 and nested >= 4 and evidence >= 4:
            row["mode"] = "inlined_debug_and_nested_loops"
            row["verified"] = True
        else:
            row["mode"], row["verified"] = "evidence_insufficient", False
        if best is None or (row["verified"], row["fannkuch_debug_locations"],
                            row["nested_loops"]) > (
                best["verified"], best["fannkuch_debug_locations"],
                best["nested_loops"]):
            best = row
    result = {"label": label, "definition_present": defined, "regions": rows,
              "verified": bool(best and best["verified"]), "selected": best,
              "status": "not_verified"}
    if result["verified"]:
        result["status"] = "verified"
    return result


def base_report(args, env):
    report = common.base_report("prepare", args, env)
    runner = report["script"]
    report.update({
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "formal_timing": False,
        "script": file_rec(Path(__file__).resolve()), "common_runner": runner,
        "legacy_prepare": file_rec(PERFORMANCE_DIR / "prepare.py"),
        "templates": template_records(),
        "product": {
            "baseline": "797c382",
            "note": "baseline 797c382; later unrelated HEAD changes are recorded, "
                    "not used to infer tool changes",
        },
    })
    report["target"].update({
        "cpp_flags": list(TARGET_FLAGS),
        "cpp_linker": str(args.llvm_bin / "ld.lld"),
    })
    report["tools"]["python_helper"] = common.tool_record(
        args.python_helper, env, args.cpu, None)
    return report


def input_paths(args, oracle_files):
    paths = [
        PERFORMANCE_DIR / name for name in TEMPLATE_NAMES
    ] + [
        PERFORMANCE_DIR / "prepare.py", PERFORMANCE_DIR / "visit_trace.py",
        Path(__file__).resolve(), Path(common.__file__).resolve(),
        args.langc, args.helper, args.runtime, args.python, args.python_helper,
        args.llvm_bin / "clang", args.llvm_bin / "clang++",
        args.llvm_bin / "ld.lld", args.llvm_bin / "opt",
        args.llvm_bin / "llvm-readelf", args.llvm_bin / "llvm-nm",
    ] + list(oracle_files)
    return sorted({Path(item).resolve() for item in paths})


def snapshot(paths):
    return {str(path): common.sha256_file(path) for path in paths}


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
            "expected_checksum": EXPECTED[args.n][0],
            "expected_maximum": EXPECTED[args.n][1],
            "target_flags": list(TARGET_FLAGS),
            "variant_expected_exits": {"correct": 0, "wrong-checksum": 1,
                                       "wrong-maximum": 2},
        }
        expected, oracle, oracle_files = load_oracle(args.oracle_dir)
        before = snapshot(input_paths(args, oracle_files))
        report["oracle"] = oracle
        report["oracle"]["files_before"] = {
            str(path): common.sha256_file(path) for path in oracle_files}
        report["input_hashes_before"] = before
        templates = read_templates()
        n, rounds = args.n, args.rounds
        case = {
            "n": n, "rounds": rounds, "expected_checksum": expected[n][0],
            "expected_maximum": expected[n][1], "variants": {},
            "check": None, "builds": [], "correct_executions": [],
            "wrong_executions": [], "helper": [], "cpp_ir": {},
            "python": {}, "cpp_work": None, "source_work": None,
        }
        report["cases"].append(case)
        for variant in ("correct", "wrong-checksum", "wrong-maximum"):
            case["variants"][variant] = generate_case(
                out, templates, n, rounds, expected[n], variant)
        correct = case["variants"]["correct"]
        manifest = Path(correct["manifest"]["path"])
        check = tool([args.langc, "check", "--project", manifest,
                      "--backend", "native", "--format", "json"], args, env)
        common.require_success(check, f"n={n} r={rounds} langc check")
        check_json = common.parse_json_stdout(
            check, f"n={n} r={rounds} langc check")
        need(check_json.get("status") == "ok", "langc check is not ok")
        case["check"] = {"command_index": check["command_index"],
                         "json": check_json, "success": True}
        python_correct = source_metadata(
            args, env, manifest,
            out / "python" / f"n{n}" / f"r{rounds}" / "correct",
            f"n={n} r={rounds} correct")
        case["python"]["correct"] = python_correct
        manifest = Path(correct["manifest"]["path"])

        for opt in (0, 2):
            langc_row = build_langc(
                args, env, manifest,
                out / "build" / f"n{n}" / f"r{rounds}" / "correct" /
                "langc" / f"O{opt}", opt, f"n={n} r={rounds} langc O{opt}")
            case["builds"].append(langc_row)
            run = run_program([langc_row["program"]["path"]], args, env)
            execution = {"implementation": "langc", "opt": opt,
                         "variant": "correct",
                         "command_index": run["command_index"], "success": False}
            case["correct_executions"].append(execution)
            common.require_quiet_success(
                run, f"n={n} r={rounds} correct langc O{opt}")
            execution.update({"success": True, "exit": run["exit"],
                              "stdout": run["stdout"], "stderr": run["stderr"]})
            case["elf_checks"] = case.get("elf_checks", [])
            elf_checks(args, env, report, langc_row["program"]["path"],
                       f"n={n} r={rounds} correct langc O{opt}", "langc")

            cpp_row = build_cpp(
                args, env, Path(correct["reference"]["path"]),
                out / "build" / f"n{n}" / f"r{rounds}" / "correct" / "cpp" /
                f"O{opt}" / "reference", opt,
                f"n={n} r={rounds} correct cpp O{opt}")
            case["builds"].append(cpp_row)
            run = run_program([cpp_row["program"]["path"]], args, env)
            execution = {"implementation": "cpp", "opt": opt,
                         "variant": "correct",
                         "command_index": run["command_index"], "success": False}
            case["correct_executions"].append(execution)
            common.require_quiet_success(
                run, f"n={n} r={rounds} correct cpp O{opt}")
            execution.update({"success": True, "exit": run["exit"],
                              "stdout": run["stdout"], "stderr": run["stderr"]})
            elf_checks(args, env, report, cpp_row["program"]["path"],
                       f"n={n} r={rounds} correct cpp O{opt}", "cpp")

            helper = export_source_llvm(
                args, env, report, manifest,
                out / "llvm" / f"n{n}" / f"r{rounds}" / f"O{opt}", opt,
                langc_row["object"]["path"], python_correct["visit_symbol"],
                f"n={n} r={rounds} Source O{opt} LLVM")
            case["helper"].append(helper)

            for wrong, expected_exit in (("wrong-checksum", 1),
                                         ("wrong-maximum", 2)):
                wrong_case = case["variants"][wrong]
                wrong_manifest = Path(wrong_case["manifest"]["path"])
                for implementation in ("langc", "cpp"):
                    if implementation == "langc":
                        row = build_langc(
                            args, env, wrong_manifest,
                            out / "build" / f"n{n}" / f"r{rounds}" / wrong /
                            "langc" / f"O{opt}", opt,
                            f"n={n} r={rounds} {wrong} langc O{opt}")
                    else:
                        row = build_cpp(
                            args, env, Path(wrong_case["reference"]["path"]),
                            out / "build" / f"n{n}" / f"r{rounds}" / wrong /
                            "cpp" / f"O{opt}" / "reference", opt,
                            f"n={n} r={rounds} {wrong} cpp O{opt}")
                    case["builds"].append(row)
                    run = run_program([row["program"]["path"]], args, env)
                    execution = {"implementation": implementation, "opt": opt,
                                 "variant": wrong,
                                 "expected_exit": expected_exit,
                                 "command_index": run["command_index"],
                                 "success": False}
                    case["wrong_executions"].append(execution)
                    require_exit(run, expected_exit,
                                 f"n={n} r={rounds} {wrong} {implementation} O{opt}")
                    execution.update({"success": True, "exit": run["exit"],
                                      "stdout": run["stdout"],
                                      "stderr": run["stderr"]})

        for opt in (0, 2):
            ir = compile_cpp_ir(
                args, env, report, Path(correct["reference"]["path"]),
                out / "llvm" / f"n{n}" / f"r{rounds}" / f"cpp-O{opt}.ll", opt,
                f"n={n} r={rounds} cpp O{opt} IR")
            case["cpp_ir"][f"O{opt}"] = ir
        linkage = cpp_visit_linkage(case["cpp_ir"]["O0"]["text"],
                                    f"n={n} r={rounds} cpp O0")
        cpp_loops = {}
        for opt in (0, 2):
            ir = case["cpp_ir"][f"O{opt}"]
            ir["visit_linkage"] = linkage
            ir["visit_calls"] = cpp_call_count(ir["text"], linkage)
            cpp_loops[opt] = analyze_loops(
                args, env, report, Path(ir["ir"]["path"]),
                f"n={n} r={rounds} cpp O{opt} loops")[0]
            ir["algorithm"] = cpp_algorithm_evidence(
                ir["text"], opt, cpp_loops[opt],
                f"n={n} r={rounds} cpp O{opt}")
            del ir["text"]
        need(case["cpp_ir"]["O0"]["visit_calls"] > 0,
             "C++ O0 positive mangled visit-call control failed")
        need(case["cpp_ir"]["O2"]["visit_calls"] == 0,
             "C++ O2 still has actual visit calls")

        if rounds >= 2:
            source_work = source_round_work(
                args, env, report, case["helper"][1]["verify_after"]["path"],
                python_correct["fannkuch_symbol"],
                f"n={n} r={rounds} Source O2 round work")
            cpp_work = cpp_round_work(
                args, env, report,
                Path(case["cpp_ir"]["O2"]["ir"]["path"]).read_text(encoding="utf-8"),
                cpp_loops[2], linkage,
                f"n={n} r={rounds} cpp O2 round work")
            case["source_work"], case["cpp_work"] = source_work, cpp_work
            for work in (source_work, cpp_work):
                if not work["verified"]:
                    report["not_verified"].append(work)

        after = snapshot(input_paths(args, oracle_files))
        report["input_hashes_after"] = after
        report["inputs_unchanged"] = before == after
        need(report["inputs_unchanged"], "source/tool/oracle inputs changed")
        report["oracle"]["files_after"] = {
            str(path): common.sha256_file(path) for path in oracle_files}
        report["oracle"]["files_unchanged"] = (
            report["oracle"]["files_before"] == report["oracle"]["files_after"])
        need(report["oracle"]["files_unchanged"], "frozen oracle files changed")

        correct_runs = case["correct_executions"]
        wrong = case["wrong_executions"]
        helper = case["helper"]
        summary = {
            "n": n, "rounds": rounds,
            "correct_executions": len(correct_runs),
            "correct_quiet_exit_0": sum(
                1 for row in correct_runs if row["success"] and row["exit"] == 0
                and not row["stdout"] and not row["stderr"]),
            "wrong_executions": len(wrong),
            "wrong_checksum_exit_1": sum(
                1 for row in wrong if row["variant"] == "wrong-checksum"
                and row["success"] and row["exit"] == 1
                and not row["stdout"] and not row["stderr"]),
            "wrong_maximum_exit_2": sum(
                1 for row in wrong if row["variant"] == "wrong-maximum"
                and row["success"] and row["exit"] == 2
                and not row["stdout"] and not row["stderr"]),
            "source_llvm_exports": len(helper),
            "source_ir_verifies": 2 * len(helper),
            "cpp_ir_verifies": 2, "all_ir_verifies": len(report["verifies"]),
            "source_object_matches": sum(1 for row in helper if row["object_match"]),
            "source_o0_positive_cases": sum(
                1 for row in helper if row["opt"] == 0
                and row["visit_calls_after"] > 0),
            "source_o2_zero_cases": sum(
                1 for row in helper if row["opt"] == 2
                and row["visit_calls_after"] == 0),
            "source_o2_visit_calls": sum(
                row["visit_calls_after"] for row in helper if row["opt"] == 2),
            "cpp_o0_positive_cases": int(
                case["cpp_ir"]["O0"]["visit_calls"] > 0),
            "cpp_o2_zero_cases": int(
                case["cpp_ir"]["O2"]["visit_calls"] == 0),
            "cpp_o2_visit_calls": case["cpp_ir"]["O2"]["visit_calls"],
            "elf_checks": len(report["elf_checks"]),
            "source_static_elf_cases": sum(
                1 for row in report["elf_checks"]
                if row["implementation"] == "langc"),
            "cpp_dynamic_elf_cases": sum(
                1 for row in report["elf_checks"]
                if row["implementation"] == "cpp"),
            "r2_source_work_verified": bool(
                case["source_work"] and case["source_work"]["verified"]),
            "r2_cpp_work_verified": bool(
                case["cpp_work"] and case["cpp_work"]["verified"]),
            "full_permutation_trace_observed": False,
        }
        needed = sorted({library for row in report["elf_checks"]
                         for library in row["needed"]})
        summary["cpp_needed_libraries"] = needed
        expected_counts = {
            "n": n, "rounds": rounds,
            "correct_executions": 4, "correct_quiet_exit_0": 4,
            "wrong_executions": 8, "wrong_checksum_exit_1": 4,
            "wrong_maximum_exit_2": 4,
            "source_llvm_exports": 2, "source_ir_verifies": 4,
            "cpp_ir_verifies": 2, "all_ir_verifies": 6,
            "source_object_matches": 2, "source_o0_positive_cases": 1,
            "source_o2_zero_cases": 1, "source_o2_visit_calls": 0,
            "cpp_o0_positive_cases": 1, "cpp_o2_zero_cases": 1,
            "cpp_o2_visit_calls": 0, "elf_checks": 4,
            "source_static_elf_cases": 2, "cpp_dynamic_elf_cases": 2,
            "r2_source_work_verified": rounds >= 2,
            "r2_cpp_work_verified": rounds >= 2,
            "full_permutation_trace_observed": False,
        }
        need(summary == {**expected_counts,
                        "cpp_needed_libraries": needed},
             f"prepare summary is unexpected: {summary}")
        report["summary"] = summary
        report["limitations"] = limitations()
        report["not_run"] = not_run()
        report["status"] = "not_verified" if report["not_verified"] else "passed"
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
        "No timing, throughput, speedup, or <=2x comparison is performed or claimed.",
        "The full Python large-input trace is not re-executed here; the frozen independent oracle already recorded per-index SHA-256 equality for every n=7..10 record.",
        "This operation did not observe every n=9/10 permutation dynamically; the frozen oracle supplies that evidence.",
        "The programs print nothing; correctness is the complete checksum/maximum comparison with quiet exit 1 for a wrong checksum and quiet exit 2 for a wrong maximum.",
        "Source R2 work evidence is static O2 IR loop/allocation/init analysis; C++ R2 additionally uses debug metadata for the inlined fannkuch body. It is not a dynamic all-permutation observation.",
        "Only n=9 and n=10, rounds 1 or 2 in this delivery, and the fixed x86_64 generic target are covered.",
        "No asm, volatile, noinline, optnone, SIMD, threads, LTO, fast-math, or -march=native is added.",
        "Native Source ELF checks include no INTERP, no NEEDED, and no strong undefined symbols; C++ dynamic standard-library dependencies are listed separately.",
    ]


def not_run():
    return [
        {"item": "formal_timing", "reason": "this operation is preparation only"},
        {"item": "full_large_python_trace", "reason": "frozen oracle already has per-index full-record evidence; not re-executed"},
        {"item": "oracle_verifier_execution", "reason": "verify.py is hash-checked but not run, avoiding another full n=7..10 Python traversal"},
        {"item": "legacy_n3_to_n7_execution", "reason": "old prepare/templates are unchanged inputs; only their hashes are checked"},
        {"item": "simd_or_multithreaded_variant", "reason": "fixtures keep the same single-thread scalar algorithm"},
        {"item": "other_targets_or_compilers", "reason": "fixed x86_64 generic target and recorded LLVM toolchain only"},
    ]


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, help="new output directory; must not exist")
    parser.add_argument("--n", type=int, choices=N_VALUES, default=9)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--oracle-dir", type=Path, default=DEFAULT_ORACLE_DIR)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--python-helper", type=Path, default=DEFAULT_PYTHON_HELPER)
    parser.add_argument("--woozyc", dest="langc", type=Path)
    parser.add_argument("--helper", type=Path)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--compiler-root", type=Path)
    parser.add_argument("--ld-library-path", type=Path)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--run-timeout", type=float, default=120.0)
    return parser.parse_args(argv)


def validate_args(args):
    legacy.validate_args(args)
    need(args.n in N_VALUES, f"--n must be one of {N_VALUES}")
    need(args.rounds > 0, "--rounds must be an integer > 0")
    for name in ("llvm-readelf", "llvm-nm"):
        need((args.llvm_bin / name).is_file(),
             f"llvm-bin/{name} is missing: {args.llvm_bin / name}")


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
