#!/usr/bin/env python3
"""Prepare the fixed binarytrees Source/C++ fixture without timing.

The official algorithm is used with maxDepth = max(N, 6), a stretch tree of
depth maxDepth + 1, a long-lived tree of depth maxDepth, and batched full trees
at depths 4, 6, ..., maxDepth with 2**(maxDepth - depth + 4) trees per depth.
Every tree is completely created, then recursively visited for its node count,
then completely released.

Source uses the existing recursive ``Node { leaf bool; children Vec[Node]; }``
representation.  Each logical node owns a real Vec object; leaves own a real
zero-length Vec.  C++20 uses one separately allocated Node per logical node,
including leaves, with owning unique_ptr left/right children.

The independent Python oracle uses only the perfect-tree integer formula.  Its
formatted N=10 text is compared byte-for-byte with the official 223-byte file
(SHA-256 b7f92c56b5d8aeb0a4d698842d1d87a57b4909865c3c84e5e10313e16663c3cb)
and its N=6/8/10 values are compared with the recorded oracle-check JSON.

For correct cases, Source and C++ O0/O2 formal programs are built and run; four
wrong-expected variants per N are also built and run with quiet nonzero exits.
Source LLVM before/after, verifier runs, helper-object identity with the formal
program.o, C++ O0/O2 IR verifies, and O2 work checks are recorded.

Real lifecycle traces are separate test-only binaries:

* Source is linked from the same helper program.o with --wrap allocator hooks
  and the runtime's existing weak lang_vec_observe_v1 ABI.  The observer reads
  LangVec through runtime/native/vec.hpp and records all real allocations,
  frees, Vec create/free events, lengths, strides, refs and drops.
* C++ is linked with a global operator new/delete observer.  It records every
  Node allocation/free size and the final live/peak count after
  tree_program_main returns; no static-destructor order is assumed.

Both traces are checked for exact run structure, peak live nodes, long-lived
tree survival, complete release, and the expected node/leaf/internal counts.
No timing, throughput, speedup, or <=2x conclusion is produced.  The observer
ELFs are not used as formal performance programs.
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

PERFORMANCE_PARENT = Path(__file__).resolve().parents[1]
SIEVE_PREPARE = PERFORMANCE_PARENT / "sieve" / "prepare.py"
MANDELBROT_PREPARE = PERFORMANCE_PARENT / "mandelbrot" / "prepare.py"


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load shared helper module: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


# Reuse the already-read generic command/build/IR/ELF helpers from Sieve.  The
# Mandelbrot module is loaded only for its generic render/C++ build/C++ IR
# helpers, which include the fixed -ffp-contract=off flag.  Neither module's
# algorithm, main(), parse_args(), or fixed inputs are used.
sieve = load_module(SIEVE_PREPARE, "_binarytrees_reuse_sieve_prepare")
mandelbrot = load_module(
    MANDELBROT_PREPARE, "_binarytrees_reuse_mandelbrot_prepare")

need = sieve.need
now = sieve.now
file_rec = sieve.file_rec
manifest_text = sieve.manifest_text
tool = sieve.tool
run_program = sieve.run_program
require_exit = sieve.require_exit
verify_ir = sieve.verify_ir
build_langc = sieve.build_langc
helper_export = sieve.helper_export
elf_checks = sieve.elf_checks
parse_functions = sieve.parse_functions
tool_snapshot = sieve.tool_snapshot
sha256_bytes = mandelbrot.sha256_bytes
render = mandelbrot.render
build_cpp = mandelbrot.build_cpp
compile_cpp_ir = mandelbrot.compile_cpp_ir
PerfError = common.PerfError

CONTRACT = "compiler-dev/1"
TASK = "native-binarytrees-preparation"
OPERATION_ID = "language-binarytrees-preparation-f9b3e5c-20261110"
PREPARE_SCHEMA = "compiler-dev/native-binarytrees-preparation/1"
ORACLE_SCHEMA = "compiler-dev/native-binarytrees-oracle/1"
N_VALUES = (6, 8, 10)
OFFICIAL_SHA256 = (
    "b7f92c56b5d8aeb0a4d698842d1d87a57b4909865c3c84e5e10313e16663c3cb"
)
OFFICIAL_SIZE = 223
PERFORMANCE_DIR = Path(__file__).resolve().parent
WORKTREE = PERFORMANCE_DIR.parents[2]
RUNTIME_VEC_HEADER = WORKTREE / "runtime/native/vec.hpp"
DEFAULT_OFFICIAL = Path(
    "/home/qin/Work/language-dev-20261103/checks/"
    "algorithm-reference-inputs-20261109/binarytrees-n10.txt"
)
DEFAULT_OFFICIAL_CHECK = Path(
    "/home/qin/Work/language-dev-20261103/checks/"
    "algorithm-reference-inputs-20261109/binarytrees-oracle-check.json"
)
TARGET_FLAGS = (
    "--target=x86_64-unknown-linux-gnu", "-march=x86-64", "-mtune=generic",
    "-fno-fast-math", "-ffp-contract=off", "-fno-lto",
)
TEMPLATE_NAMES = ("binarytrees.wz.in", "reference.cpp.in")
OBSERVER_SOURCES = ("native_observer.cpp", "cpp_observer.cpp")
DELIVERABLE_NAMES = (*TEMPLATE_NAMES, *OBSERVER_SOURCES, "prepare.py", "README.md")
VARIANTS = ("correct", "wrong-stretch", "wrong-tree-count",
            "wrong-bucket", "wrong-long")
VARIANT_EXITS = {
    "correct": 0,
    "wrong-stretch": 1,
    "wrong-bucket": 2,
    "wrong-long": 3,
    "wrong-tree-count": 4,
}
WRONG_TARGET = {
    "wrong-stretch": "STRETCH_CHECK",
    "wrong-tree-count": "TREES_4",
    "wrong-bucket": "CHECK_4",
    "wrong-long": "LONG_CHECK",
}
MARKER_RE = re.compile(
    r"^//\s*PERF-BINARYTREES\s+n=(\d+)\s+max=(\d+)\s+stretch=(\d+)"
    r"\s+expected_sha256=([0-9a-f]{64})\s+variant=([a-z-]+)\s*$",
    re.MULTILINE,
)
FORBIDDEN_SOURCE = (
    "arena", "pool", "free-list", "free_list", "cache", "thread", "atomic",
    "simd", "malloc", "mmap", "std::", "#include", "extern",
)
FORBIDDEN_CPP = (
    "arena", "pool", "free-list", "free_list", "cache", "thread", "atomic",
    "simd", "malloc", "mmap", "std::vector", "std::pmr", "new Node",
)


def perfect_nodes(depth: int) -> int:
    need(depth >= 0, "tree depth must be non-negative")
    return (1 << (depth + 1)) - 1


def oracle_case(n: int) -> dict:
    max_depth = max(n, 6)
    stretch_depth = max_depth + 1
    stretch_check = perfect_nodes(stretch_depth)
    long_check = perfect_nodes(max_depth)
    lines = [{
        "kind": "stretch", "depth": stretch_depth,
        "check": stretch_check,
        "line": f"stretch tree of depth {stretch_depth}\t check: {stretch_check}",
    }]
    depths = []
    for depth in range(4, max_depth + 1, 2):
        trees = 1 << (max_depth - depth + 4)
        nodes_each = perfect_nodes(depth)
        check = trees * nodes_each
        depths.append({
            "depth": depth, "trees": trees, "nodes_each": nodes_each,
            "check": check,
        })
        lines.append({
            "kind": "bucket", "depth": depth, "trees": trees,
            "nodes_each": nodes_each, "check": check,
            "line": f"{trees}\t trees of depth {depth}\t check: {check}",
        })
    lines.append({
        "kind": "long", "depth": max_depth, "check": long_check,
        "line": f"long lived tree of depth {max_depth}\t check: {long_check}",
    })
    text = "\n".join(row["line"] for row in lines) + "\n"
    return {
        "n": n, "max_depth": max_depth, "stretch_depth": stretch_depth,
        "stretch_check": stretch_check, "long_depth": max_depth,
        "long_check": long_check, "depths": depths, "lines": lines,
        "text": text, "text_sha256": sha256_bytes(text.encode("utf-8")),
    }


def oracle_values(case: dict, variant: str) -> dict:
    values = {
        "STRETCH_CHECK": case["stretch_check"],
        "LONG_CHECK": case["long_check"],
    }
    for row in case["depths"]:
        depth = row["depth"]
        values[f"TREES_{depth}"] = row["trees"]
        values[f"CHECK_{depth}"] = row["check"]
    if variant == "correct":
        return values
    target = WRONG_TARGET.get(variant)
    need(target is not None, f"unknown variant {variant}")
    need(target in values, f"{variant}: target {target} is not generated")
    values[target] += 1
    return values


def verify_variant_values(case: dict, variant: str, values: dict, label: str):
    correct = oracle_values(case, "correct")
    differences = [key for key in correct if values[key] != correct[key]]
    if variant == "correct":
        need(not differences, f"{label}: correct variant changes {differences}")
    else:
        need(differences == [WRONG_TARGET[variant]],
             f"{label}: wrong variant differences are {differences}")
        key = differences[0]
        need(values[key] == correct[key] + 1,
             f"{label}: wrong variant did not add exactly one to {key}")


def source_bucket_calls(case: dict, values: dict) -> str:
    lines = []
    for row in case["depths"]:
        depth = row["depth"]
        lines.extend([
            f"    let b{depth} = bucket_phase({case['max_depth']}, {depth}, "
            f"{values[f'TREES_{depth}']}, {values[f'CHECK_{depth}']});",
            f"    if b{depth} != 0 {{",
            f"        return b{depth};",
            "    }",
        ])
    return "\n".join(lines)


def cpp_bucket_calls(case: dict, values: dict) -> str:
    lines = []
    for row in case["depths"]:
        depth = row["depth"]
        lines.extend([
            f"    const int32_t b{depth} = bucket_phase({case['max_depth']}, "
            f"{depth}, {values[f'TREES_{depth}']}, {values[f'CHECK_{depth}']});",
            f"    if (b{depth} != 0) {{",
            f"        return b{depth};",
            "    }",
        ])
    return "\n".join(lines)


def template_records():
    return {name: file_rec(PERFORMANCE_DIR / name)
            for name in (*TEMPLATE_NAMES, *OBSERVER_SOURCES)}


def read_templates():
    return {name: (PERFORMANCE_DIR / name).read_text(encoding="utf-8")
            for name in TEMPLATE_NAMES}


def marker_values(text: str, label: str):
    match = MARKER_RE.search(text)
    need(match is not None, f"{label}: missing PERF-BINARYTREES marker")
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)),
            match.group(4), match.group(5))


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def verify_generated(source: str, reference: str, case: dict, variant: str,
                     values: dict, label: str):
    wanted = (case["n"], case["max_depth"], case["stretch_depth"],
              case["text_sha256"], variant)
    need(marker_values(source, f"{label} Source") == wanted,
         f"{label}: Source marker mismatch")
    need(marker_values(reference, f"{label} C++") == wanted,
         f"{label}: C++ marker mismatch")
    verify_variant_values(case, variant, values, label)
    for key, value in values.items():
        need(str(value) in source,
             f"{label}: Source is missing {key}={value}")
        need(str(value) in reference,
             f"{label}: C++ is missing {key}={value}")
    for snippet in (
        "struct Node", "leaf bool", "children Vec[Node]",
        "let left = make_tree(depth - 1);",
        "let right = make_tree(depth - 1);",
        "check(node.children[0])", "check(node.children[1])",
        "i32(1) << (max_depth - depth + 4)",
        "let long_lived = make_tree(", "total += check(tree);",
    ):
        need(snippet in source, f"{label}: Source is missing {snippet!r}")
    for snippet in (
        "struct Node", "bool leaf", "std::unique_ptr<Node> left",
        "std::unique_ptr<Node> right", "auto left = make_tree(depth - 1);",
        "auto right = make_tree(depth - 1);", "check(*node.left)",
        "check(*node.right)", "int32_t{1} << (max_depth - depth + 4)",
        "auto long_lived = make_tree(", "total += check(*tree);",
        "#include <memory>",
    ):
        need(snippet in reference, f"{label}: C++ is missing {snippet!r}")
    lowered_source = strip_comments(source).lower()
    for word in FORBIDDEN_SOURCE:
        need(word not in lowered_source,
             f"{label}: Source contains forbidden {word!r}")
    lowered_reference = strip_comments(reference).lower()
    for word in FORBIDDEN_CPP:
        need(word not in lowered_reference,
             f"{label}: C++ contains forbidden {word!r}")
    need("{{" not in source and "{{" not in reference,
         f"{label}: rendered text still has a placeholder")


def generate_case(out: Path, templates: dict, case: dict, variant: str) -> dict:
    values = oracle_values(case, variant)
    mapping = {
        "N": case["n"], "MAX_DEPTH": case["max_depth"],
        "STRETCH_DEPTH": case["stretch_depth"],
        "STRETCH_CHECK": values["STRETCH_CHECK"],
        "LONG_CHECK": values["LONG_CHECK"],
        "EXPECTED_SHA": case["text_sha256"], "VARIANT": variant,
        "BUCKET_CALLS": source_bucket_calls(case, values),
    }
    cpp_mapping = dict(mapping)
    cpp_mapping["BUCKET_CALLS"] = cpp_bucket_calls(case, values)
    source_text = render(templates["binarytrees.wz.in"], mapping)
    reference_text = render(templates["reference.cpp.in"], cpp_mapping)
    label = f"n={case['n']} {variant}"
    verify_generated(source_text, reference_text, case, variant, values, label)
    directory = out / "cases" / f"n{case['n']}" / variant
    (directory / "src").mkdir(parents=True, exist_ok=False)
    source = directory / "src" / "main.wz"
    reference = directory / "reference.cpp"
    manifest = directory / "lang.toml"
    expected = directory / "expected.txt"
    source.write_text(source_text, encoding="utf-8")
    reference.write_text(reference_text, encoding="utf-8")
    manifest.write_text(
        manifest_text(f"perf_binarytrees_n{case['n']}_{variant.replace('-', '_')}"),
        encoding="utf-8")
    expected.write_text(case["text"], encoding="utf-8")
    return {
        "n": case["n"], "variant": variant,
        "expected_exit": VARIANT_EXITS[variant],
        "expected_values": values,
        "wrong_key": WRONG_TARGET.get(variant),
        "expected": file_rec(expected, out),
        "manifest": file_rec(manifest, out),
        "source": file_rec(source, out),
        "reference": file_rec(reference, out),
        "builds": [], "executions": [],
    }


def parse_official(path: Path, check_path: Path):
    data = Path(path).read_bytes()
    need(len(data) == OFFICIAL_SIZE,
         f"{path}: expected {OFFICIAL_SIZE} bytes, got {len(data)}")
    need(sha256_bytes(data) == OFFICIAL_SHA256,
         f"{path}: SHA-256 is not the recorded official value")
    check = json.loads(Path(check_path).read_text(encoding="utf-8"))
    need(check.get("official_n10_full_byte_match") is True,
         f"{check_path}: official_n10_full_byte_match is not true")
    need(check.get("official_sha256") == OFFICIAL_SHA256,
         f"{check_path}: recorded official SHA does not match")
    return data, check


def build_oracle(out: Path, official_path: Path, check_path: Path):
    official_data, official_check = parse_official(official_path, check_path)
    oracle_dir = out / "oracle"
    oracle_dir.mkdir(parents=True, exist_ok=False)
    cases = {}
    for n in N_VALUES:
        case = oracle_case(n)
        path = oracle_dir / f"n{n}.txt"
        path.write_text(case["text"], encoding="utf-8")
        need(path.read_bytes() == case["text"].encode("utf-8"),
             f"n={n}: oracle text round-trip failed")
        case["file"] = file_rec(path, out)
        cases[n] = case
    need(cases[10]["text"].encode("utf-8") == official_data,
         "independent N=10 text is not byte-identical to the official file")
    recorded = {int(row["n"]): row for row in official_check["cases"]}
    for n in N_VALUES:
        case = cases[n]
        row = recorded.get(n)
        need(row is not None, f"oracle-check JSON has no n={n} case")
        need(row["stretch_depth"] == case["stretch_depth"],
             f"n={n}: stretch depth differs from oracle-check JSON")
        need(row["stretch_check"] == case["stretch_check"],
             f"n={n}: stretch check differs from oracle-check JSON")
        need(row["long_lived_depth"] == case["long_depth"]
             and row["long_lived_check"] == case["long_check"],
             f"n={n}: long-lived values differ from oracle-check JSON")
        need(len(row["depths"]) == len(case["depths"]),
             f"n={n}: depth count differs from oracle-check JSON")
        for expected, actual in zip(case["depths"], row["depths"]):
            need(expected["depth"] == actual["depth"]
                 and expected["trees"] == actual["trees"]
                 and expected["nodes_each"] == actual["nodes_each"]
                 and expected["check"] == actual["check"],
                 f"n={n}: depth row differs from oracle-check JSON")
    oracle_json = {
        "schema": ORACLE_SCHEMA,
        "method": "integer perfect-tree formula: nodes(d)=2**(d+1)-1; "
                  "stretch depth max(N,6)+1; long-lived depth max(N,6); "
                  "batch trees 2**(max-depth+4) at depth 4..max step 2",
        "n_values": list(N_VALUES),
        "cases": {
            str(n): {
                "n": case["n"], "max_depth": case["max_depth"],
                "stretch_depth": case["stretch_depth"],
                "stretch_check": case["stretch_check"],
                "long_lived_depth": case["long_depth"],
                "long_lived_check": case["long_check"],
                "depths": case["depths"], "lines": case["lines"],
                "text_sha256": case["text_sha256"],
                "file": case["file"],
            }
            for n, case in cases.items()
        },
        "official": {
            "text": file_rec(official_path),
            "oracle_check": file_rec(check_path),
            "sha256": OFFICIAL_SHA256,
            "size": OFFICIAL_SIZE,
            "n10_full_byte_match": cases[10]["text"].encode("utf-8")
            == official_data,
            "oracle_check_full_match": True,
        },
    }
    oracle_path = out / "oracle.json"
    common.write_json(oracle_path, oracle_json)
    return cases, file_rec(oracle_path, out)


def expected_tree_runs(case: dict):
    max_depth = case["max_depth"]
    stretch = perfect_nodes(max_depth + 1)
    long_lived = perfect_nodes(max_depth)
    depths = case["depths"]
    node_counts = [row["nodes_each"] for row in depths]
    tree_counts = [row["trees"] for row in depths]
    new_runs = [stretch, long_lived + node_counts[0]]
    free_runs = [stretch, node_counts[0]]
    new_runs += [node_counts[0]] * (tree_counts[0] - 1)
    free_runs += [node_counts[0]] * (tree_counts[0] - 1)
    for index in range(1, len(depths)):
        new_runs += [node_counts[index]] * tree_counts[index]
        if index == len(depths) - 1:
            free_runs += [node_counts[index]] * (tree_counts[index] - 1)
            free_runs += [node_counts[index] + long_lived]
        else:
            free_runs += [node_counts[index]] * tree_counts[index]
    return new_runs, free_runs, long_lived, stretch


def expected_new_run_shapes(case: dict):
    max_depth = case["max_depth"]
    stretch = perfect_nodes(max_depth + 1)
    long_lived = perfect_nodes(max_depth)
    depths = case["depths"]
    node_counts = [row["nodes_each"] for row in depths]
    combined = long_lived + node_counts[0]
    expanded = [
        (stretch, (stretch + 1) // 2, (stretch - 1) // 2),
        (combined,
         (long_lived + 1) // 2 + (node_counts[0] + 1) // 2,
         (long_lived - 1) // 2 + (node_counts[0] - 1) // 2),
    ]
    # The first depth's first tree is merged into the combined run; all other
    # bucket trees are single-tree new runs.
    for index, row in enumerate(depths):
        repeats = row["trees"] - 1 if index == 0 else row["trees"]
        count = row["nodes_each"]
        expanded += [(count, (count + 1) // 2, (count - 1) // 2)] * repeats
    return expanded


def analyze_runs(events, case: dict, label: str):
    """Group new/free events into tree lifecycle runs and validate them."""
    runs = []
    for event in events:
        kind = event[0]
        if runs and runs[-1]["kind"] == kind:
            runs[-1]["items"].append(event)
        else:
            runs.append({"kind": kind, "items": [event]})
    new_runs = [run for run in runs if run["kind"] == "new"]
    free_runs = [run for run in runs if run["kind"] == "free"]
    expected_new, expected_free, long_lived, stretch = expected_tree_runs(case)
    need([len(run["items"]) for run in new_runs] == expected_new,
         f"{label}: new-tree run counts differ from the formula")
    need([len(run["items"]) for run in free_runs] == expected_free,
         f"{label}: free-tree run counts differ from the formula")
    # Validate every new run's leaf/internal shape where lengths are available.
    shapes = expected_new_run_shapes(case)
    need(len(shapes) == len(new_runs),
         f"{label}: new-run shape count mismatch")
    for index, (run, shape) in enumerate(zip(new_runs, shapes)):
        lengths = [event[2] for event in run["items"]]
        if lengths and lengths[0] is not None:
            need(len(lengths) == shape[0],
                 f"{label}: new run {index} node count mismatch")
            need(lengths.count(0) == shape[1],
                 f"{label}: new run {index} leaf count mismatch")
            need(lengths.count(2) == shape[2],
                 f"{label}: new run {index} internal count mismatch")
            need(all(length in (0, 2) for length in lengths),
                 f"{label}: new run {index} has an unexpected Vec length")
    # Simulate live identities and expected live counts at every run boundary.
    live = set()
    live_counts = [0]
    run_rows = []
    for run in runs:
        if run["kind"] == "new":
            for event in run["items"]:
                need(event[1] not in live,
                     f"{label}: duplicate live identity {event[1]}")
                live.add(event[1])
        else:
            for event in run["items"]:
                need(event[1] in live,
                     f"{label}: free of unknown identity {event[1]}")
                live.remove(event[1])
        live_counts.append(len(live))
        run_rows.append({"kind": run["kind"], "count": len(run["items"])})
    expected_live = [0]
    for run in runs:
        count = len(run["items"])
        if run["kind"] == "new":
            expected_live.append(expected_live[-1] + count)
        else:
            need(expected_live[-1] >= count,
                 f"{label}: free run count {count} exceeds live "
                 f"{expected_live[-1]}")
            expected_live.append(expected_live[-1] - count)
    need(live_counts == expected_live,
         f"{label}: live node counts do not match the expected lifecycle")
    need(not live, f"{label}: identities remain live after the final free run")
    peak = max(live_counts)
    need(peak == stretch,
         f"{label}: peak live nodes {peak} != stretch tree {stretch}")

    # The second new run is long-lived tree followed by the first bucket tree.
    need(len(new_runs) >= 2, f"{label}: no long-lived new run")
    combined_ids = [event[1] for event in new_runs[1]["items"]]
    need(len(combined_ids) >= long_lived,
         f"{label}: combined run is shorter than the long-lived tree")
    long_ids = set(combined_ids[:long_lived])
    first_bucket_ids = set(combined_ids[long_lived:])
    first_bucket_free = {event[1] for event in free_runs[1]["items"]}
    need(first_bucket_ids == first_bucket_free,
         f"{label}: first bucket tree was not fully released as one run")
    combined_run_index = runs.index(new_runs[1])
    for index, run in enumerate(runs):
        if index <= combined_run_index or run["kind"] != "free":
            # Earlier free runs may share allocator addresses with later
            # long-lived allocations; only runs after the combined allocation
            # can prove a premature long-lived release.
            continue
        freed = {event[1] for event in run["items"]}
        if index < len(runs) - 1:
            need(not (freed & long_ids),
                 f"{label}: long-lived tree was released before the end")
        else:
            need(long_ids <= freed,
                 f"{label}: final run did not release the long-lived tree")
    need(long_ids and not (long_ids & first_bucket_ids),
         f"{label}: long-lived and first bucket identities overlap")
    return {
        "new_runs": len(new_runs), "free_runs": len(free_runs),
        "new_run_counts": [len(run["items"]) for run in new_runs],
        "free_run_counts": [len(run["items"]) for run in free_runs],
        "run_events": run_rows,
        "expected_new_run_counts": expected_new,
        "expected_free_run_counts": expected_free,
        "live_counts": live_counts,
        "peak_live_nodes": peak,
        "long_lived_nodes": long_lived,
        "long_lived_survived_to_final_run": True,
    }


def parse_source_trace(trace_path: Path, case: dict, label: str):
    text = Path(trace_path).read_text(encoding="utf-8")
    lines = text.splitlines()
    events = []
    alloc_records = []
    alloc_sizes = []
    free_records = []
    live_heap = set()
    live_vec = set()
    new_ids = []
    new_lengths = []
    new_data_sizes = []
    vec_object_sizes = set()
    drops = {}
    refs = {}
    vec_creates = 0
    vec_frees = 0
    retain = release = 0
    for line in lines:
        if line.startswith("ALLOC:"):
            parts = line.split(":")
            need(len(parts) == 4, f"{label}: bad ALLOC line {line!r}")
            pointer, size = parts[1], int(parts[2])
            need(pointer not in live_heap,
                 f"{label}: duplicate live heap pointer {pointer}")
            live_heap.add(pointer)
            alloc_records.append((pointer, size))
            alloc_sizes.append(size)
            continue
        if line.startswith("FREE:"):
            parts = line.split(":")
            need(len(parts) == 2, f"{label}: bad FREE line {line!r}")
            pointer = parts[1]
            need(pointer in live_heap,
                 f"{label}: free of unknown heap pointer {pointer}")
            live_heap.remove(pointer)
            free_records.append(pointer)
            continue
        if line.startswith("VEC:"):
            parts = line.split(":")
            need(len(parts) == 7, f"{label}: bad VEC line {line!r}")
            event = int(parts[1])
            identity = parts[2]
            detail = int(parts[3])
            length = int(parts[4])
            stride = int(parts[5])
            ref_count = int(parts[6])
            if event == 1:
                need(identity not in live_vec,
                     f"{label}: duplicate live Vec identity {identity}")
                need(length in (0, 2),
                     f"{label}: unexpected Vec length {length}")
                need(stride > 0, f"{label}: non-positive Vec stride")
                need(len(alloc_records) == vec_creates + 1,
                     f"{label}: expected one allocation before each Vec create")
                pointer, size = alloc_records[-1]
                need(pointer == identity,
                     f"{label}: Vec identity differs from its allocation")
                expected_data = length * stride
                need(size == 64 + expected_data,
                     f"{label}: combined Vec allocation {size} != {64 + expected_data}")
                vec_object_sizes.add(size)
                live_vec.add(identity)
                new_ids.append(identity)
                new_lengths.append(length)
                new_data_sizes.append(expected_data)
                drops[identity] = []
                refs[identity] = ref_count
                vec_creates += 1
                events.append(("new", identity, length))
            elif event == 5:
                need(identity in live_vec,
                     f"{label}: free of unknown Vec identity {identity}")
                need(detail == 0 and ref_count == 0,
                     f"{label}: final Vec release has non-zero detail/refs")
                expected_drops = list(range(length - 1, -1, -1))
                need(drops[identity] == expected_drops,
                     f"{label}: element drop order {drops[identity]} != "
                     f"{expected_drops}")
                live_vec.remove(identity)
                events.append(("free", identity, None))
                vec_frees += 1
            elif event == 4:
                need(identity in live_vec,
                     f"{label}: drop on freed Vec identity {identity}")
                drops[identity].append(detail)
            elif event in (2, 3):
                need(identity in live_vec,
                     f"{label}: ref operation on freed Vec identity {identity}")
                refs[identity] = detail
                if event == 2:
                    retain += 1
                else:
                    release += 1
            elif event in (6, 7):
                need(identity in live_vec,
                     f"{label}: iteration event on freed Vec identity {identity}")
            else:
                raise PerfError(f"{label}: unknown VEC event {event}")
            continue
        if line == "":
            continue
        raise PerfError(f"{label}: unexpected observer trace line {line!r}")
    need(not live_heap, f"{label}: heap pointers remain live")
    need(not live_vec, f"{label}: Vec identities remain live")
    need(len(alloc_records) == len(new_ids),
         f"{label}: expected one heap allocation per logical node")
    need(len(free_records) == len(alloc_records),
         f"{label}: heap allocation/free counts differ")
    need(set(new_ids) <= set(free_records),
         f"{label}: a Vec object identity was not physically freed")
    need(len(new_data_sizes) == len(new_lengths),
         f"{label}: missing element storage sizes")
    lifecycle = analyze_runs(events, case, label)
    leaf_count = new_lengths.count(0)
    internal_count = new_lengths.count(2)
    total_trees = 2 + sum(row["trees"] for row in case["depths"])
    need(leaf_count == internal_count + total_trees,
         f"{label}: full-tree leaf/internal counts are inconsistent")
    # The total node count is the sum of all allocated new runs.
    expected_total = sum(lifecycle["expected_new_run_counts"])
    need(len(new_ids) == expected_total,
         f"{label}: total logical nodes {len(new_ids)} != {expected_total}")
    return {
        "trace": file_rec(trace_path),
        "trace_lines": len(lines),
        "vec_creates": vec_creates,
        "vec_frees": vec_frees,
        "logical_nodes": len(new_ids),
        "full_trees": total_trees,
        "leaf_vecs": leaf_count,
        "internal_vecs": internal_count,
        "retain_events": retain,
        "release_events": release,
        "heap_allocations": len(alloc_records),
        "heap_frees": len(free_records),
        "heap_peak_live": lifecycle["peak_live_nodes"],
        "combined_allocation_sizes": sorted(vec_object_sizes),
        "element_storage_sizes": sorted(set(new_data_sizes)),
        "lifecycle": lifecycle,
    }


def parse_cpp_trace(trace_path: Path, case: dict, label: str):
    text = Path(trace_path).read_text(encoding="utf-8")
    lines = text.splitlines()
    events = []
    live = set()
    allocations = []
    allocation_sizes = []
    frees = []
    total_row = None
    for line in lines:
        if line.startswith("ALLOC:"):
            parts = line.split(":")
            need(len(parts) == 3, f"{label}: bad C++ ALLOC line {line!r}")
            pointer, size = parts[1], int(parts[2])
            need(pointer not in live,
                 f"{label}: duplicate live Node pointer {pointer}")
            live.add(pointer)
            allocations.append(pointer)
            allocation_sizes.append(size)
            events.append(("new", pointer, None))
            continue
        if line.startswith("FREE:"):
            parts = line.split(":")
            need(len(parts) == 2, f"{label}: bad C++ FREE line {line!r}")
            pointer = parts[1]
            need(pointer in live,
                 f"{label}: free of unknown Node pointer {pointer}")
            live.remove(pointer)
            frees.append(pointer)
            events.append(("free", pointer, None))
            continue
        if line.startswith("TOTAL:"):
            parts = line.split(":")
            need(len(parts) == 5, f"{label}: bad C++ TOTAL line {line!r}")
            need(total_row is None, f"{label}: duplicate C++ TOTAL line")
            total_row = tuple(int(part) for part in parts[1:])
            continue
        if line == "":
            continue
        raise PerfError(f"{label}: unexpected C++ observer trace line {line!r}")
    need(total_row is not None, f"{label}: missing C++ TOTAL summary")
    need(not live, f"{label}: Node pointers remain live")
    need(len(allocations) == len(frees),
         f"{label}: Node allocation/free counts differ")
    need(len(set(allocation_sizes)) == 1,
         f"{label}: Node allocation size is not stable: {sorted(set(allocation_sizes))}")
    lifecycle = analyze_runs(events, case, label)
    expected_total = sum(lifecycle["expected_new_run_counts"])
    need(len(allocations) == expected_total,
         f"{label}: total Nodes {len(allocations)} != {expected_total}")
    total_alloc, total_free, total_live, total_peak = total_row
    need(total_alloc == len(allocations) and total_free == len(frees)
         and total_live == 0 and total_peak == lifecycle["peak_live_nodes"],
         f"{label}: TOTAL {total_row} does not match the parsed trace")
    return {
        "trace": file_rec(trace_path),
        "trace_lines": len(lines),
        "node_allocations": len(allocations),
        "node_frees": len(frees),
        "node_size": allocation_sizes[0],
        "summary": {
            "allocations": total_alloc, "frees": total_free,
            "live": total_live, "peak": total_peak,
        },
        "lifecycle": lifecycle,
    }


def build_source_observer(args, env, out: Path, program_object: Path,
                          binary: Path, opt: int, label: str):
    command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         "-I", str(WORKTREE / "runtime/native"),
         program_object, PERFORMANCE_DIR / "native_observer.cpp",
         args.runtime, "-Wl,--wrap=lang_alloc_v1",
         "-Wl,--wrap=lang_free_v1",
         f"--ld-path={args.llvm_bin / 'ld.lld'}", "-o", binary],
        args, env)
    common.require_success(command, f"{label} Source observer build")
    return {
        "implementation": "source-observer", "opt": opt,
        "command_index": command["command_index"],
        "binary": file_rec(binary, out),
        "elf": common.checked_elf(binary, executable=True, label=label),
        "formal_cli": False, "host_libc_dependent": True,
        "not_independent_native_performance_program": True,
        "program_object": file_rec(program_object, out),
    }


def run_source_observer(args, env, out: Path, case: dict, opt: int,
                        helper: dict, label: str):
    directory = out / "observers" / "source" / f"n{case['n']}" / f"O{opt}"
    directory.mkdir(parents=True, exist_ok=False)
    binary = directory / "observer"
    build = build_source_observer(
        args, env, out, Path(helper["object"]["path"]), binary, opt, label)
    trace_path = directory / "trace.txt"
    observer_env = dict(env)
    observer_env["BT_TRACE_PATH"] = str(trace_path)
    run = common.command([binary], timeout=args.run_timeout, env=observer_env,
                         cpu=args.cpu)
    need(run["status"] == "exited" and run["exit"] == 0,
         f"{label}: Source observer run failed: {run}")
    need(not run["stdout"],
         f"{label}: Source observer wrote stdout: {run['stdout'][-1000:]!r}")
    need(not run["stderr"],
         f"{label}: Source observer wrote stderr: {run['stderr'][-2000:]!r}")
    analysis = parse_source_trace(trace_path, case, label)
    return {
        "implementation": "source", "opt": opt,
        "command_index": run["command_index"], "build": build,
        "run": {"exit": run["exit"], "stdout": run["stdout"],
                "stderr": run["stderr"]},
        "trace_path": str(trace_path), "analysis": analysis,
        "formal_cli": False, "host_libc_dependent": True,
        "not_independent_native_performance_program": True,
    }


def build_cpp_observer(args, env, out: Path, reference: Path,
                       binary: Path, opt: int, label: str):
    object_path = binary.parent / "reference_observe.o"
    compile_command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         "-Dmain=tree_program_main", "-c", reference, "-o", object_path],
        args, env)
    common.require_success(compile_command,
                           f"{label} C++ observer reference compile")
    link_command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         PERFORMANCE_DIR / "cpp_observer.cpp", object_path,
         f"--ld-path={args.llvm_bin / 'ld.lld'}", "-o", binary],
        args, env)
    common.require_success(link_command, f"{label} C++ observer link")
    return {
        "implementation": "cpp-observer", "opt": opt,
        "compile_command_index": compile_command["command_index"],
        "link_command_index": link_command["command_index"],
        "binary": file_rec(binary, out),
        "elf": common.checked_elf(binary, executable=True, label=label),
        "formal_cli": False, "host_libc_dependent": True,
        "not_independent_native_performance_program": True,
        "reference_object": file_rec(object_path, out),
    }


def run_cpp_observer(args, env, out: Path, case: dict, opt: int,
                     reference: Path, label: str):
    directory = out / "observers" / "cpp" / f"n{case['n']}" / f"O{opt}"
    directory.mkdir(parents=True, exist_ok=False)
    binary = directory / "observer"
    build = build_cpp_observer(args, env, out, reference, binary, opt, label)
    trace_path = directory / "trace.txt"
    observer_env = dict(env)
    observer_env["BT_TRACE_PATH"] = str(trace_path)
    run = common.command([binary], timeout=args.run_timeout, env=observer_env,
                         cpu=args.cpu)
    need(run["status"] == "exited" and run["exit"] == 0,
         f"{label}: C++ observer run failed: {run}")
    need(not run["stdout"],
         f"{label}: C++ observer wrote stdout: {run['stdout'][-1000:]!r}")
    need(not run["stderr"],
         f"{label}: C++ observer wrote stderr: {run['stderr'][-2000:]!r}")
    analysis = parse_cpp_trace(trace_path, case, label)
    return {
        "implementation": "cpp", "opt": opt,
        "command_index": run["command_index"], "build": build,
        "run": {"exit": run["exit"], "stdout": run["stdout"],
                "stderr": run["stderr"]},
        "trace_path": str(trace_path), "analysis": analysis,
        "formal_cli": False, "host_libc_dependent": True,
        "not_independent_native_performance_program": True,
    }


def source_work_check(args, env, report, path: Path, label: str):
    text = Path(path).read_text(encoding="utf-8")
    functions = parse_functions(text)
    pattern = r"\b(?:call|invoke)\b[^\n]*@{}\b"

    def call_count(symbol):
        return len(re.findall(pattern.format(re.escape(symbol)), text))

    new_calls = call_count("lang_vec_new_v1")
    at_calls = call_count("lang_vec_at_v1")
    release_calls = call_count("lang_vec_release_v1")
    retain_calls = call_count("lang_vec_retain_v1")
    need(new_calls >= 2, f"{label}: fewer than two real Vec creations")
    need(at_calls >= 2, f"{label}: fewer than two real Vec element reads")
    need(release_calls >= 1, f"{label}: no real Vec release call")
    need("load ptr" in text, f"{label}: no Vec handle load remains")

    constructor = None
    checker = None
    constructor_self = 0
    checker_self = 0
    for name, info in functions.items():
        body = info["body"]
        self_calls = len(re.findall(pattern.format(re.escape(name)), body))
        if ("@lang_vec_new_v1" in body and self_calls >= 1
                and "i64 0," in body and "i64 2," in body):
            constructor = name
            constructor_self = self_calls
        if ("@lang_vec_at_v1" in body and self_calls >= 1
                and "br i1" in body
                and ("extractvalue" in body or "load i1" in body)
                and ("load %value" in body or "load ptr" in body)):
            checker = name
            checker_self = self_calls
    need(constructor is not None,
         f"{label}: no recursive tree constructor with leaf/internal Vecs")
    need(checker is not None,
         f"{label}: no recursive traversal with real Vec element reads")
    need(call_count(constructor) > constructor_self,
         f"{label}: constructor has no non-recursive caller")
    need(call_count(checker) > checker_self,
         f"{label}: traversal has no non-recursive caller")
    need("add i32" in text or "add i64" in text,
         f"{label}: no node-count addition remains")
    loops = tool([args.llvm_bin / "opt", "-passes=print<loops>",
                  "-disable-output", path], args, env)
    loop_row = {"path": str(path), "command_index": loops.get("command_index"),
                "success": False}
    report["loop_analyses"].append(loop_row)
    common.require_success(loops, f"{label} loop analysis")
    loop_count = loops["stderr"].count("Loop at depth")
    loop_row.update(success=True, loop_count=loop_count)
    row = {
        "path": str(path), "file": file_rec(path),
        "vec_new_calls": new_calls, "vec_at_calls": at_calls,
        "vec_release_calls": release_calls, "vec_retain_calls": retain_calls,
        "constructor": constructor, "traversal": checker,
        "constructor_self_calls": constructor_self,
        "traversal_self_calls": checker_self,
        "constructor_external_calls": call_count(constructor),
        "traversal_external_calls": call_count(checker),
        "loop_count": loop_count, "success": True,
    }
    return row


def cpp_linkage(ir_text: str, source_name: str, label: str):
    matches = set(re.findall(
        r'!DISubprogram\(name: "' + re.escape(source_name)
        + r'", linkageName: "([^"]+)"', ir_text))
    need(len(matches) == 1,
         f"{label}: expected one debug-identified {source_name} symbol, "
         f"got {sorted(matches)}")
    return next(iter(matches))


def cpp_work_check(args, env, report, path: Path, label: str):
    text = Path(path).read_text(encoding="utf-8")
    functions = parse_functions(text)
    make_tree = cpp_linkage(text, "make_tree", label)
    check = cpp_linkage(text, "check", label)
    need(make_tree in functions,
         f"{label}: make_tree definition {make_tree} is missing")
    need(check in functions, f"{label}: check definition {check} is missing")
    make_body = functions[make_tree]["body"]
    check_body = functions[check]["body"]
    new_calls = len(re.findall(
        r"\b(?:call|invoke)\b[^\n]*@_Znwm\b", text))
    delete_calls = len(re.findall(
        r"\b(?:call|invoke)\b[^\n]*@_ZdlPv", text))
    make_self = len(re.findall(
        r"\b(?:call|invoke)\b[^\n]*@" + re.escape(make_tree) + r"\b",
        make_body))
    check_self = len(re.findall(
        r"\b(?:call|invoke)\b[^\n]*@" + re.escape(check) + r"\b",
        check_body))
    need(new_calls >= 2,
         f"{label}: fewer than two real operator new calls remain")
    need(delete_calls >= 1,
         f"{label}: no real operator delete call remains")
    need(make_self >= 1,
         f"{label}: make_tree has no recursive child construction")
    need(check_self >= 1,
         f"{label}: check has no recursive traversal")
    need("load ptr" in check_body,
         f"{label}: check has no child pointer load")
    need("load i1" in check_body or "load i8" in check_body,
         f"{label}: check has no leaf-flag load")
    need("add i32" in check_body or "add nsw i32" in check_body,
         f"{label}: check has no node-count addition")
    make_total = len(re.findall(
        r"\b(?:call|invoke)\b[^\n]*@" + re.escape(make_tree) + r"\b", text))
    check_total = len(re.findall(
        r"\b(?:call|invoke)\b[^\n]*@" + re.escape(check) + r"\b", text))
    need(make_total > make_self,
         f"{label}: make_tree has no non-recursive caller")
    need(check_total > check_self,
         f"{label}: check has no non-recursive caller")
    loops = tool([args.llvm_bin / "opt", "-passes=print<loops>",
                  "-disable-output", path], args, env)
    loop_row = {"path": str(path), "command_index": loops.get("command_index"),
                "success": False}
    report["loop_analyses"].append(loop_row)
    common.require_success(loops, f"{label} loop analysis")
    loop_count = loops["stderr"].count("Loop at depth")
    loop_row.update(success=True, loop_count=loop_count)
    return {
        "path": str(path), "file": file_rec(path),
        "make_tree_linkage": make_tree, "check_linkage": check,
        "operator_new_calls": new_calls,
        "operator_delete_calls": delete_calls,
        "make_tree_self_calls": make_self, "check_self_calls": check_self,
        "make_tree_total_calls": make_total, "check_total_calls": check_total,
        "loop_count": loop_count, "success": True,
    }


def execute(args, env, row: dict, implementation: str, opt: int,
            program: Path, expected: int, label: str):
    run = run_program([program], args, env)
    execution = {
        "implementation": implementation, "opt": opt,
        "expected_exit": expected, "command_index": run["command_index"],
        "success": False,
    }
    row["executions"].append(execution)
    require_exit(run, expected, f"{label} run")
    execution.update(success=True, exit=run["exit"], stdout=run["stdout"],
                     stderr=run["stderr"])
    return execution


def base_report(args, env, out: Path):
    report = common.base_report("prepare", args, env)
    report.update({
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "formal_timing": False,
        "script": file_rec(Path(__file__).resolve()),
        "common_runner": file_rec(Path(common.__file__).resolve()),
        "sieve_helpers": file_rec(SIEVE_PREPARE),
        "mandelbrot_helpers": file_rec(MANDELBROT_PREPARE),
        "reuse_modules": {
            "sieve": "_binarytrees_reuse_sieve_prepare",
            "mandelbrot": "_binarytrees_reuse_mandelbrot_prepare",
        },
        "runtime_vec_header": file_rec(RUNTIME_VEC_HEADER),
        "templates": template_records(),
        "elf_checks": [], "verifies": [], "loop_analyses": [],
        "cases": [], "source_observers": [], "cpp_observers": [],
    })
    report["target"].update({
        "cpp_flags": list(TARGET_FLAGS),
        "cpp_linker": str(args.llvm_bin / "ld.lld"),
    })
    return report


def input_snapshot(report, out: Path):
    paths = [PERFORMANCE_DIR / name for name in DELIVERABLE_NAMES]
    paths.extend([
        Path(__file__).resolve(), Path(common.__file__).resolve(),
        SIEVE_PREPARE, MANDELBROT_PREPARE, RUNTIME_VEC_HEADER,
        Path(report["official"]["text"]["path"]),
        Path(report["official"]["oracle_check"]["path"]),
        Path(report["oracle"]["file"]["path"]),
    ])
    for case in report["cases"]:
        for variant in case["variants"].values():
            paths.extend([
                Path(variant["manifest"]["path"]),
                Path(variant["source"]["path"]),
                Path(variant["reference"]["path"]),
                Path(variant["expected"]["path"]),
            ])
    for row in report["oracle"]["cases"].values():
        paths.append(Path(row["file"]["path"]))
    return {str(Path(path).resolve()): common.sha256_file(path)
            for path in paths}


def write_patch(out: Path, args, env):
    parts = []
    for name in DELIVERABLE_NAMES:
        relative = f"tests/performance/binarytrees/{name}"
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
            "size_bytes": patch.stat().st_size, "files": list(DELIVERABLE_NAMES)}


def prepare(args):
    env = common.make_env(args)
    out = common.prepare_out(args.out)
    (out / "started_at.txt").write_text(now() + "\n", encoding="utf-8")
    report = {
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "mode": "prepare", "status": "running",
        "commands": [], "verifies": [], "loop_analyses": [], "elf_checks": [],
        "cases": [], "source_observers": [], "cpp_observers": [],
    }
    common._COMMAND_LOG = report["commands"]
    try:
        report.update(base_report(args, env, out))
        root = Path(args.compiler_root).resolve()
        for label, path in (("langc", args.langc), ("helper", args.helper),
                            ("runtime", args.runtime)):
            need(root in Path(path).resolve().parents,
                 f"{label} is not under --compiler-root {root}: {path}")
        report["tools_under_compiler_root"] = True
        report["compiler_root_is_worktree"] = root == WORKTREE.resolve()
        cache_path = Path(report["build_tree"]["cmake_cache"]["path"])
        build_testing = re.search(
            r"^BUILD_TESTING:BOOL=(.*)$",
            cache_path.read_text(encoding="utf-8"), re.MULTILINE)
        report["build_tree"]["build_testing"] = (
            build_testing.group(1).strip() if build_testing else None)
        need(report["build_tree"]["build_testing"] == "OFF",
             f"{cache_path}: BUILD_TESTING is not OFF")
        report["parameters"] = {
            "n_values": list(N_VALUES),
            "max_depth": "max(N, 6)",
            "stretch_depth": "max(N, 6) + 1",
            "long_lived_depth": "max(N, 6)",
            "bucket_depths": "4, 6, ..., max(N, 6)",
            "bucket_trees": "2 ** (max_depth - depth + 4)",
            "node_formula": "2 ** (depth + 1) - 1",
            "source_representation": "Node { leaf bool; children Vec[Node]; }",
            "cpp_representation": "Node { bool leaf; unique_ptr<Node> left/right; }",
            "target_flags": list(TARGET_FLAGS),
            "program_expected_exits": VARIANT_EXITS,
        }
        report["tools_before"] = tool_snapshot(args)
        templates = read_templates()
        oracle_cases, oracle_rec = build_oracle(
            out, args.official, args.official_check)
        report["oracle"] = {
            "method": "integer perfect-tree formula",
            "file": oracle_rec,
            "cases": {
                str(n): {
                    "n": case["n"],
                    "max_depth": case["max_depth"],
                    "stretch_depth": case["stretch_depth"],
                    "stretch_check": case["stretch_check"],
                    "long_lived_depth": case["long_depth"],
                    "long_lived_check": case["long_check"],
                    "depths": case["depths"],
                    "text_sha256": case["text_sha256"],
                    "file": case["file"],
                }
                for n, case in oracle_cases.items()
            },
            "official": {
                "text": file_rec(args.official),
                "oracle_check": file_rec(args.official_check),
                "sha256": OFFICIAL_SHA256,
                "full_byte_match": oracle_cases[10]["text"].encode("utf-8")
                == Path(args.official).read_bytes(),
            },
        }
        report["official"] = report["oracle"]["official"]
        for n in N_VALUES:
            case = {
                "n": n, "max_depth": oracle_cases[n]["max_depth"],
                "stretch_depth": oracle_cases[n]["stretch_depth"],
                "depths": oracle_cases[n]["depths"],
                "oracle": report["oracle"]["cases"][str(n)],
                "variants": {}, "check": None, "cpp_ir": {},
                "source_observers": [], "cpp_observers": [],
            }
            report["cases"].append(case)
            for variant in VARIANTS:
                case["variants"][variant] = generate_case(
                    out, templates, oracle_cases[n], variant)
        report["inputs_before"] = input_snapshot(report, out)

        for case in report["cases"]:
            n = case["n"]
            correct = case["variants"]["correct"]
            manifest = Path(correct["manifest"]["path"])
            reference = Path(correct["reference"]["path"])
            check = tool([args.langc, "check", "--project", str(manifest),
                          "--backend", "native", "--format", "json"], args, env)
            common.require_success(check, f"n={n} correct langc check")
            check_json = common.parse_json_stdout(check, f"n={n} correct langc check")
            need(check_json.get("status") == "ok",
                 f"n={n}: langc check status is not ok")
            case["check"] = {"command_index": check["command_index"],
                             "json": check_json, "success": True}

            for opt in (0, 2):
                source_label = f"n={n} correct Source O{opt}"
                source_build = build_langc(
                    args, env, out, manifest,
                    out / "build" / f"n{n}" / "correct" / "langc" / f"O{opt}",
                    opt, source_label)
                correct["builds"].append(source_build)
                execute(args, env, correct, "langc", opt,
                        Path(source_build["program"]["path"]), 0, source_label)
                elf_checks(args, env, report, source_build["program"]["path"],
                           source_label, "langc")
                cpp_label = f"n={n} correct C++ O{opt}"
                cpp_build = build_cpp(
                    args, env, out, reference,
                    out / "build" / f"n{n}" / "correct" / "cpp" / f"O{opt}" /
                    "reference", opt, cpp_label)
                correct["builds"].append(cpp_build)
                execute(args, env, correct, "cpp", opt,
                        Path(cpp_build["program"]["path"]), 0, cpp_label)
                elf_checks(args, env, report, cpp_build["program"]["path"],
                           cpp_label, "cpp")
                helper = helper_export(
                    args, env, report, out, manifest,
                    out / "llvm" / f"n{n}" / "correct" / f"O{opt}", opt,
                    source_build["object"]["path"],
                    f"{source_label} LLVM")
                correct.setdefault("helper", []).append(helper)
                work = None
                if opt == 2:
                    work = source_work_check(
                        args, env, report, Path(helper["after"]["path"]),
                        f"{source_label} work")
                helper["work"] = work
                observer = run_source_observer(
                    args, env, out, case, opt, helper,
                    f"{n} correct Source O{opt} observer")
                case["source_observers"].append(observer)
                cpp_observer = run_cpp_observer(
                    args, env, out, case, opt, reference,
                    f"{n} correct C++ O{opt} observer")
                case["cpp_observers"].append(cpp_observer)

            for opt in (0, 2):
                ir_row = compile_cpp_ir(
                    args, env, report, out, reference,
                    out / "llvm" / f"n{n}" / f"cpp-O{opt}.ll", opt,
                    f"n={n} C++ O{opt} IR", report_check_work=False)
                work = None
                if opt == 2:
                    work = cpp_work_check(
                        args, env, report, Path(ir_row["ir"]["path"]),
                        f"n={n} C++ O{opt} work")
                ir_row["work"] = work
                case["cpp_ir"][f"O{opt}"] = ir_row
                del ir_row["text"]

            for variant in VARIANTS:
                if variant == "correct":
                    continue
                row = case["variants"][variant]
                variant_manifest = Path(row["manifest"]["path"])
                variant_reference = Path(row["reference"]["path"])
                expected_exit = row["expected_exit"]
                for opt in (0, 2):
                    source_label = f"n={n} {variant} Source O{opt}"
                    source_build = build_langc(
                        args, env, out, variant_manifest,
                        out / "build" / f"n{n}" / variant / "langc" / f"O{opt}",
                        opt, source_label)
                    row["builds"].append(source_build)
                    execute(args, env, row, "langc", opt,
                            Path(source_build["program"]["path"]),
                            expected_exit, source_label)
                    elf_checks(args, env, report,
                               source_build["program"]["path"],
                               source_label, "langc")
                    cpp_label = f"n={n} {variant} C++ O{opt}"
                    cpp_build = build_cpp(
                        args, env, out, variant_reference,
                        out / "build" / f"n{n}" / variant / "cpp" / f"O{opt}" /
                        "reference", opt, cpp_label)
                    row["builds"].append(cpp_build)
                    execute(args, env, row, "cpp", opt,
                            Path(cpp_build["program"]["path"]),
                            expected_exit, cpp_label)
                    elf_checks(args, env, report,
                               cpp_build["program"]["path"], cpp_label, "cpp")

        report["tools_after"] = tool_snapshot(args)
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
        report["repository_note"] = (
            "HEAD/status are recorded only; tool/input SHA-256 changes fail prepare")

        cases = report["cases"]
        variants = [row for case in cases for row in case["variants"].values()]
        builds = [row for variant in variants for row in variant["builds"]]
        executions = [row for variant in variants for row in variant["executions"]]
        helpers = [row for variant in variants for row in variant.get("helper", [])]
        source_observers = [row for case in cases for row in case["source_observers"]]
        cpp_observers = [row for case in cases for row in case["cpp_observers"]]
        works = [row["work"] for row in helpers if row["work"]]
        works += [case["cpp_ir"]["O2"]["work"] for case in cases]
        quiet = {
            code: sum(1 for row in executions if row["success"]
                      and row["exit"] == code and not row["stdout"]
                      and not row["stderr"])
            for code in set(VARIANT_EXITS.values())
        }
        summary = {
            "cases": len(cases), "variants": len(variants),
            "program_executions": len(executions),
            "builds": len(builds),
            "source_builds": sum(1 for row in builds
                                 if row["implementation"] == "langc"),
            "cpp_builds": sum(1 for row in builds
                              if row["implementation"] == "cpp"),
            "quiet_exit_0": quiet[0], "quiet_exit_1": quiet[1],
            "quiet_exit_2": quiet[2], "quiet_exit_3": quiet[3],
            "quiet_exit_4": quiet[4],
            "wrong_program_executions": sum(
                1 for row in executions if row["expected_exit"] != 0),
            "wrong_program_quiet_nonzero": sum(
                1 for row in executions if row["expected_exit"] != 0
                and row["success"] and row["exit"] == row["expected_exit"]
                and not row["stdout"] and not row["stderr"]),
            "source_helper_exports": len(helpers),
            "source_object_matches": sum(1 for row in helpers
                                         if row["object_match"]),
            "source_ir_verifies": 2 * len(helpers),
            "cpp_ir_exports": 2 * len(cases),
            "cpp_ir_verifies": 2 * len(cases),
            "all_ir_verifies": len(report["verifies"]),
            "o2_work_checks": len(works),
            "o2_work_checks_passed": sum(1 for row in works if row["success"]),
            "source_observers": len(source_observers),
            "source_observer_matches": sum(1 for row in source_observers
                                           if row["analysis"]["logical_nodes"]),
            "cpp_observers": len(cpp_observers),
            "cpp_observer_matches": sum(1 for row in cpp_observers
                                        if row["analysis"]["node_allocations"]),
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
            "official_n10_full_match": report["oracle"]["official"][
                "full_byte_match"],
            "tools_unchanged": report["tools_unchanged"],
            "inputs_unchanged": report["inputs_unchanged"],
            "repositories_unchanged": report["repositories_unchanged"],
            "timing_performed": False,
        }
        need(summary["cases"] == 3 and summary["variants"] == 15,
             "case/variant count changed")
        need(summary["program_executions"] == 60
             and summary["wrong_program_executions"] == 48,
             "program execution count changed")
        need(summary["builds"] == 60 and summary["source_builds"] == 30
             and summary["cpp_builds"] == 30,
             "build counts changed")
        need(summary["quiet_exit_0"] == 12
             and summary["quiet_exit_1"] == 12
             and summary["quiet_exit_2"] == 12
             and summary["quiet_exit_3"] == 12
             and summary["quiet_exit_4"] == 12
             and summary["wrong_program_quiet_nonzero"] == 48,
             "quiet exit counts changed")
        need(summary["source_helper_exports"] == 6
             and summary["source_object_matches"] == 6
             and summary["source_ir_verifies"] == 12,
             "Source helper/object/IR counts changed")
        need(summary["cpp_ir_exports"] == 6 and summary["cpp_ir_verifies"] == 6
             and summary["all_ir_verifies"] == 18,
             "C++/all IR verify counts changed")
        need(summary["o2_work_checks"] == 6
             and summary["o2_work_checks_passed"] == 6,
             "O2 work-check counts changed")
        need(summary["source_observers"] == 6
             and summary["source_observer_matches"] == 6,
             "Source observer counts changed")
        need(summary["cpp_observers"] == 6
             and summary["cpp_observer_matches"] == 6,
             "C++ observer counts changed")
        need(summary["source_elf_static_checks"] == 30
             and summary["source_elf_static_checks_passed"] == 30
             and summary["cpp_dynamic_elf_checks"] == 30
             and summary["cpp_dynamic_elf_checks_passed"] == 30,
             "ELF check counts changed")
        need(summary["official_n10_full_match"] is True,
             "official N=10 full-text match changed")
        need(summary["tools_unchanged"] is True
             and summary["inputs_unchanged"] is True,
             "tool/input stability changed")
        need(summary["timing_performed"] is False, "timing must not run")
        report["source_observers"] = source_observers
        report["cpp_observers"] = cpp_observers
        report["summary"] = summary
        report["deliverable"] = {
            "worktree": str(WORKTREE),
            "worktree_head": report["repositories"]["worktree"]["head"],
            "files": [file_rec(PERFORMANCE_DIR / name)
                      for name in DELIVERABLE_NAMES],
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
        "The formal programs print nothing; each line is checked inside the program against independent formula constants, and wrong-expected variants must exit quietly with 1/2/3/4.",
        "Source uses the existing recursive Node { leaf bool; children Vec[Node]; } representation. Every logical node owns one combined Vec descriptor/element allocation, including independent zero-length leaf Vecs; no shared leaf or benchmark-specific arena is used.",
        "C++ allocates one Node per logical node, including leaves, and uses owning unique_ptr left/right. Both implementations allocate once per logical node, with different object sizes.",
        "The Source observer is a test-only instrumented binary linked from the same helper program.o with host libc, --wrap allocator hooks, and the runtime's existing weak lang_vec_observe_v1 ABI. It is not a standalone Native performance program and is not used for timing.",
        "The C++ observer is a test-only binary with global operator new/delete counters. It prints its final count after tree_program_main returns and does not rely on host static-destructor order.",
        "The independent oracle is the integer perfect-tree formula; the N=10 formatted text is compared byte-for-byte with the official file and N=6/8/10 values with the recorded oracle-check JSON.",
        "Only N=6,8,10 and the fixed x86_64 generic target are covered. No SIMD, threads, LTO, fast-math, FMA, other targets, or -march=native are used.",
        "The C++ observer source is compiled with -Dmain=tree_program_main only to link the count observer; the tree algorithm source text is unchanged.",
        "No formal performance measurement is implemented in this fixture.",
    ]


def not_run():
    return [
        {"item": "formal_timing", "reason": "preparation only"},
        {"item": "speedup_or_2x_comparison", "reason": "no timing or performance conclusion"},
        {"item": "other_n_values", "reason": "fixed N=6,8,10"},
        {"item": "python_backend", "reason": "Source/C++ preparation fixture only"},
        {"item": "other_targets_or_compilers", "reason": "fixed x86_64 generic target and recorded LLVM 22.1.8"},
        {"item": "observer_elf_as_formal_performance_program", "reason": "observer binaries depend on host libc and are test-only"},
    ]


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path,
                        help="new output directory; must not exist")
    parser.add_argument("--official", type=Path, default=DEFAULT_OFFICIAL)
    parser.add_argument("--official-check", type=Path,
                        default=DEFAULT_OFFICIAL_CHECK)
    parser.add_argument("--woozyc", dest="langc", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--compiler-root", type=Path, required=True)
    parser.add_argument("--ld-library-path", type=Path)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--run-timeout", type=float, default=180.0)
    return parser.parse_args(argv)


def validate_args(args):
    need(args.out is not None, "--out is required")
    for label, path in (
        ("official", args.official),
        ("official-check", args.official_check),
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
        print(json.dumps({
            "status": "failed", "type": type(error).__name__,
            "message": str(error), "traceback": traceback.format_exc(),
        }, indent=2, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    sys.exit(main())
