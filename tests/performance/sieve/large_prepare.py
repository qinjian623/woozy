#!/usr/bin/env python3
"""Prepare the inclusive N=1,000,000/10,000,000 sieve rounds fixture.

The existing Sieve templates and ``prepare.py`` are inputs and are not
modified.  This script only renders the existing template with the new N,
ROUNDS, count, and sum values, preserving the original algorithm and ordinary
C++ O2 optimization.  It reads the frozen full prime-flag files from the
independent large oracle; it does not regenerate the oracle.

It checks Source Native and C++ O0/O2 correctness, wrong count/sum exits at
R1, Source helper object identity and LLVM verification, C++ LLVM verification,
ELF policy, and full composite-byte observations for both implementations.
For R2 it records actual O2 repeated-work evidence.  The real R1 O2 IR is also
passed to the R2 analyzer and must be rejected.  Broken no-kernel controls and a
real one-round control are verified and rejected.  No timing, speedup, or
``<=2x`` result is produced.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import signal
import subprocess
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_native as common  # noqa: E402

PERFORMANCE_DIR = Path(__file__).resolve().parent
WORKTREE = PERFORMANCE_DIR.parents[2]
MAIN = Path("/home/qin/Work/language-dev-20261103")
MAIN_TEMPLATES = MAIN / "compiler/tests/performance/sieve"


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load shared helpers: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


legacy = _load("_sieve_large_reuse_prepare", PERFORMANCE_DIR / "prepare.py")
fannkuch = _load("_sieve_large_reuse_fannkuch_large",
                 PERFORMANCE_DIR.parent / "fannkuch" / "large_prepare.py")

need = legacy.need
now = legacy.now
file_rec = legacy.file_rec
manifest_text = legacy.manifest_text
parse_functions = legacy.parse_functions
TARGET_FLAGS = legacy.TARGET_FLAGS
MARKER_RE = legacy.MARKER_RE
PerfError = common.PerfError

CONTRACT = "compiler-dev/1"
TASK = "native-sieve-large-preparation"
OPERATION_ID = "language-sieve-large-preparation-20261110"
PREPARE_SCHEMA = "compiler-dev/native-sieve-large-preparation/1"
DEFAULT_ORACLE_DIR = (
    MAIN / "checks/eight-algorithm-large-oracles-20261110")
DEFAULT_PYTHON_HELPER = (
    MAIN / "compiler/build/native-cli-release/tests/python/lang_python_emit")
N_VALUES = (1000000, 10000000)
EXISTING_CASES = (
    "n1000000-r1", "n1000000-r2", "n10000000-r1", "n10000000-r2")
RESULTS_SHA256 = (
    "e5a58627690ce535086cc3477e819e1f2a45dcb0bebd1ffe2a100b7b4e8f366c")
PINNED = {
    1000000: {
        "sha256": "1d8537de67d9eab4b2208f2efd3a137552d4c37d68c93a867c992df240189180",
        "bytes": 1000001, "count": 78498, "sum": 37550402023,
    },
    10000000: {
        "sha256": "ab158d028a45c7419316d831fde03f2b164a2e31c28826083c339b83dcef43f6",
        "bytes": 10000001, "count": 664579, "sum": 3203324994356,
    },
}
PINNED_TOOLS = {
    "langc": "a17e1cc3753371a3588db3b44a5fab36e3caff1440f9889997e416008bab73ec",
    "helper": "f24f410da99eb0f827f7031a76d4220164b50cb03070a4c382e137a12acc989e",
    "runtime": "40ea4d083e97f511c776166ede32e294753aeef3c3417742b84a79885f48d4eb",
}
OBSERVER_SOURCE = PERFORMANCE_DIR / "large_observer.cpp"
TEMPLATE_NAMES = ("sieve.lang.in", "reference.cpp.in")
MARK_STORE_RE = re.compile(r"\bstore\s+i8\s+1\b")
ZERO_STORE_RE = re.compile(r"\bstore\s+i8\s+0\b")
LOAD_I8_RE = re.compile(r"\bload\b[^\n]*\bi8\b")
ADD_I64_RE = re.compile(r"\badd\b[^\n]*\bi64\b")
ICMP_I64_RE = re.compile(r"\bicmp\b[^\n]*\bi64\b")
CHECK_I64_RE = re.compile(r"\bicmp\s+(?:ne|eq)\s+i64\b")
NEW_VEC_RE = re.compile(r"@lang_vec_new_v1\(")
RELEASE_VEC_RE = re.compile(r"@lang_vec_release_v1\(")
HEAP_NEW_RE = re.compile(r"@_Znwm\(|@_Znam\(")
HEAP_DELETE_RE = re.compile(r"@_ZdlPvm\(|@_ZdaPvm\(")
BOUND_LINE_RE = re.compile(
    r"\bicmp\s+(?:slt|ult|sle|ule|eq|ne|sgt|ugt|sge|uge)"
    r"\s+i(?:32|64)\b[^\n]*")


def command(argv, *, timeout, env, cpu=None):
    argv = [str(value) for value in argv]
    started = time.perf_counter_ns()
    preexec = None
    if cpu is not None:
        def preexec():
            os.sched_setaffinity(0, {cpu})
    try:
        process = subprocess.Popen(
            argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            env=env, preexec_fn=preexec, start_new_session=True)
    except (OSError, subprocess.SubprocessError) as error:
        result = {"argv": argv, "status": "launch_failed", "exit": None,
                  "stdout": "", "stderr": "",
                  "elapsed_ns": time.perf_counter_ns() - started,
                  "timeout_s": timeout, "cpu": cpu,
                  "error": f"{type(error).__name__}: {error}"}
        return _log(result)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
        status, exit_code, error = "exited", process.returncode, None
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
        process.wait()
        status, exit_code = "timeout", None
        error = f"TimeoutExpired: {timeout}s; process group killed and reaped"
    result = {"argv": argv, "status": status, "exit": exit_code,
              "stdout": stdout or "", "stderr": stderr or "",
              "elapsed_ns": time.perf_counter_ns() - started,
              "timeout_s": timeout, "cpu": cpu}
    if error is not None:
        result["error"] = error
    return _log(result)


def _log(result):
    if common._COMMAND_LOG is not None:
        result["command_index"] = len(common._COMMAND_LOG)
        common._COMMAND_LOG.append(result)
    return result


def tool(argv, args, env):
    return command(argv, timeout=args.timeout, env=env, cpu=args.cpu)


def run_program(argv, args, env, extra_env=None):
    merged = env if extra_env is None else {**env, **extra_env}
    return command(argv, timeout=args.run_timeout, env=merged, cpu=args.cpu)


legacy.tool = tool
legacy.run_program = lambda argv, args, env: run_program(argv, args, env)
fannkuch.tool = tool

build_langc = legacy.build_langc
build_cpp = legacy.build_cpp
helper_export = legacy.helper_export
elf_checks = legacy.elf_checks
verify_ir = legacy.verify_ir
execute = legacy.execute


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def template_records():
    return {name: file_rec(PERFORMANCE_DIR / name) for name in TEMPLATE_NAMES}


def read_templates():
    return {name: (PERFORMANCE_DIR / name).read_text(encoding="utf-8")
            for name in TEMPLATE_NAMES}


def zero_text(n):
    return ("0, " * (n + 1))[:-2]


def render(template, n, rounds, count, total, zeros):
    mapping = {
        "{{N}}": str(n), "{{ROUNDS}}": str(rounds),
        "{{EXPECTED_COUNT}}": str(count), "{{EXPECTED_SUM}}": str(total),
        "{{ZEROS}}": zeros,
    }
    text = template
    for key, value in mapping.items():
        text = text.replace(key, value)
    need("{{" not in text, "rendered template still has a placeholder")
    return text


def load_oracle(path):
    path = Path(path).resolve()
    results_path = path / "results.json"
    need(common.sha256_file(results_path) == RESULTS_SHA256,
         f"{results_path}: results.json SHA-256 changed")
    data = json.loads(results_path.read_text(encoding="utf-8"))
    need(data.get("status") == "passed" and data.get("formal_timing") is False,
         "large oracle status/formal_timing is wrong")
    rows = [row for row in data.get("cases", [])
            if row.get("algorithm") == "sieve"]
    by_n = {int(row["n"]): row for row in rows}
    need(tuple(sorted(by_n)) == N_VALUES,
         f"large oracle sieve N values are {sorted(by_n)}")
    flags, composite, cases, files = {}, {}, {}, []
    for n in N_VALUES:
        pinned = PINNED[n]
        row = by_n[n]
        flag_path = Path(row["path"]).resolve()
        need(flag_path == (path / f"sieve-n{n}-prime-flags.u8").resolve(),
             f"n={n}: frozen flag path changed: {flag_path}")
        raw = flag_path.read_bytes()
        need(len(raw) == pinned["bytes"] == n + 1,
             f"n={n}: flag file has {len(raw)} bytes, expected {n + 1}")
        need(sha256_bytes(raw) == pinned["sha256"],
             f"n={n}: frozen flag SHA-256 changed")
        need(all(value in (0, 1) for value in raw),
             f"n={n}: frozen flags contain a non-0/1 byte")
        need(raw[0] == 0 and raw[1] == 0,
             f"n={n}: frozen flags[0]/flags[1] are not zero")
        count = sum(raw[2:])
        total = sum(index for index in range(2, n + 1) if raw[index])
        need(count == pinned["count"] == int(row["count"]),
             f"n={n}: count does not match the frozen value")
        need(total == pinned["sum"] == int(row["sum"]),
             f"n={n}: sum does not match the frozen value")
        need(int(row["bytes"]) == pinned["bytes"]
             and row["sha256"] == pinned["sha256"],
             f"n={n}: results.json entry does not match the frozen file")
        expected = bytearray(n + 1)
        for index in range(2, n + 1):
            expected[index] = 1 - raw[index]
        flags[n], composite[n] = raw, bytes(expected)
        cases[n] = {
            "n": n, "flags": file_rec(flag_path),
            "flags_sha256": pinned["sha256"], "flags_bytes": len(raw),
            "count": count, "sum": total,
            "composite_sha256": sha256_bytes(composite[n]),
            "composite_bytes": len(expected),
            "flags_0_1_zero": True,
            "non_0_1_bytes": 0,
            "results_row": row,
        }
        files.append(flag_path)
    files.append(results_path)
    return flags, composite, {
        "directory": str(path), "results": file_rec(results_path),
        "results_sha256": RESULTS_SHA256,
        "cases": {str(n): row for n, row in cases.items()},
        "flags_fully_read": True, "flags_observed_not_regenerated": True,
    }, files


def verify_generated(source, reference, n, rounds, count, total, zeros, label):
    wanted = (n, rounds, count, total)
    for text, language in ((source, "Source"), (reference, "C++")):
        match = MARKER_RE.search(text)
        need(match is not None, f"{label}: {language} marker is missing")
        actual = tuple(int(match.group(index)) for index in range(1, 5))
        need(actual == wanted,
             f"{label}: {language} marker {actual} != {wanted}")
        need("Vec<bool>" not in text and "std::vector<bool>" not in text,
             f"{label}: {language} uses bool storage")
    need("Vec[u8]" in source and zeros in source,
         f"{label}: Source is missing the full u8 zero literal")
    need(zeros.count("0") == n + 1,
         f"{label}: Source zero literal does not contain n+1 entries")
    for snippet in (
        "composite[i] = 0;", "if composite[p] == 0 {",
        "var m i32 = p * p;", "composite[m] = 1;",
        "if composite[i] == 0 {", "count += 1;", "sum += u64(i);",
        "if count != expected_count {", "if sum != expected_sum {",
    ):
        need(snippet in source, f"{label}: Source is missing {snippet!r}")
    need("std::vector<uint8_t>" in reference
         and "std::vector<bool>" not in reference,
         f"{label}: C++ must use std::vector<uint8_t>")
    for snippet in (
        "composite[static_cast<std::size_t>(i)] = 0;",
        "if (composite[static_cast<std::size_t>(p)] == 0) {",
        "for (int32_t m = p * p; m <= n; m += p) {",
        "composite[static_cast<std::size_t>(m)] = 1;",
        "if (composite[static_cast<std::size_t>(i)] == 0) {",
        "count += UINT64_C(1);",
        "sum += static_cast<uint64_t>(i);",
        "if (count != expected_count) {", "if (sum != expected_sum) {",
    ):
        need(snippet in reference, f"{label}: C++ is missing {snippet!r}")
    need(re.search(rf"bounds\{{\s*{n}\s*\}}", reference) is not None,
         f"{label}: C++ runtime bound vector is missing or wrong")


def generate_case(out, templates, n, rounds, count, total, variant):
    if variant == "wrong_count":
        count += 1
    elif variant == "wrong_sum":
        total += 1
    elif variant != "correct":
        raise PerfError(f"unknown variant {variant}")
    zeros = zero_text(n)
    source_text = render(templates["sieve.lang.in"], n, rounds, count, total,
                         zeros)
    reference_text = render(templates["reference.cpp.in"], n, rounds, count,
                            total, zeros)
    label = f"n={n} r={rounds} {variant}"
    verify_generated(source_text, reference_text, n, rounds, count, total,
                     zeros, label)
    directory = out / "cases" / f"n{n}" / f"r{rounds}" / variant
    (directory / "src").mkdir(parents=True, exist_ok=False)
    source, reference = directory / "src/main.lang", directory / "reference.cpp"
    manifest = directory / "lang.toml"
    source.write_text(source_text, encoding="utf-8")
    reference.write_text(reference_text, encoding="utf-8")
    manifest.write_text(manifest_text(
        f"perf_sieve_large_n{n}_r{rounds}_{variant}"), encoding="utf-8")
    return {
        "n": n, "rounds": rounds, "variant": variant,
        "expected_count": count, "expected_sum": total,
        "expected_exit": {"correct": 0, "wrong_count": 1,
                          "wrong_sum": 2}[variant],
        "manifest": file_rec(manifest, out), "source": file_rec(source, out),
        "reference": file_rec(reference, out),
        "builds": [], "executions": [], "helper": [],
    }


def source_symbols(helper, label):
    entry = int(helper["json"]["entry_symbol"])
    before = Path(helper["before"]["path"]).read_text(encoding="utf-8")
    functions = parse_functions(before)
    main_name = f"lang.fn.{entry}"
    need(main_name in functions, f"{label}: entry function is missing")
    calls = sorted({int(value) for value in re.findall(
        r"@lang\.fn\.(\d+)\b", functions[main_name]["body"])})
    candidates = [
        value for value in calls
        if f"lang.fn.{value}" in functions
        and MARK_STORE_RE.search(functions[f"lang.fn.{value}"]["body"])]
    need(len(candidates) == 1,
         f"{label}: expected one sieve callee from entry, got {candidates}")
    sieve = candidates[0]
    need(MARK_STORE_RE.search(functions[f"lang.fn.{sieve}"]["body"]) is not None,
         f"{label}: sieve callee has no marking store")
    return {"main": entry, "sieve": sieve}, functions


def source_work_function(text, entry):
    name = f"lang.fn.{entry}"
    functions = parse_functions(text)
    if name in functions:
        return name, "entry_definition"
    if "main" in functions and re.search(
            rf"%{re.escape(name)}\.exit(?:[.:]|\b)", text):
        return "main", "entry_inlined_into_main_with_entry_label"
    raise PerfError(
        f"entry {name} is neither defined nor visibly inlined into main")


def counts(text):
    return {
        "mark_store_i8_1": len(MARK_STORE_RE.findall(text)),
        "zero_store_i8_0": len(ZERO_STORE_RE.findall(text)),
        "load_i8": len(LOAD_I8_RE.findall(text)),
        "add_i64": len(ADD_I64_RE.findall(text)),
        "icmp_i64": len(ICMP_I64_RE.findall(text)),
        "check_i64": len(CHECK_I64_RE.findall(text)),
        "vec_new": len(NEW_VEC_RE.findall(text)),
        "vec_release": len(RELEASE_VEC_RE.findall(text)),
        "heap_new": len(HEAP_NEW_RE.findall(text)),
        "heap_delete": len(HEAP_DELETE_RE.findall(text)),
    }


def kernel_ok(values, loops):
    return (values["mark_store_i8_1"] > 0 and values["load_i8"] > 0
            and values["add_i64"] > 0 and values["check_i64"] > 0
            and loops >= 3)


def function_kernel(text, sections, name):
    functions = parse_functions(text)
    if name not in functions:
        return {"function": name, "present": False, "kernel_ok": False}
    values = counts(functions[name]["body"])
    loops = sections.get(name, [])
    return {"function": name, "present": True, "loop_count": len(loops),
            **values, "kernel_ok": kernel_ok(values, len(loops))}


def region_names(records, index):
    names = list(records[index]["blocks"])
    child = index + 1
    while child < len(records) and records[child]["depth"] > 1:
        names.extend(name for name in records[child]["blocks"]
                     if name not in names)
        child += 1
    return names, child - index - 1


def find_definition(blocks, value):
    for block_name, lines in blocks.items():
        for line in lines:
            if re.match(rf"\s*{re.escape(value)}\s*=", line):
                return block_name, line.strip()
    return None, None


def guard_error_evidence(blocks, latch, guard, expected_sum):
    """Require the non-phi OR operand to be the real sum-mismatch guard."""
    block, line = find_definition(blocks, guard)
    if block != latch or line is None or expected_sum is None:
        return None
    match = re.match(
        r"\s*(%[A-Za-z0-9_.$]+)\s*=\s*icmp\s+ne\s+i64\s+"
        r"([^,\s]+)\s*,\s*([^,\s\)]+)", line)
    if match is None:
        return None
    left, right = match.group(2), match.group(3)
    left_literal = re.fullmatch(r"-?\d+", left) is not None
    right_literal = re.fullmatch(r"-?\d+", right) is not None
    if left_literal == right_literal:
        return None
    constant = int(left if left_literal else right)
    value = right if left_literal else left
    if not value.startswith("%"):
        return None
    if constant != expected_sum:
        return None
    if not sum_recurrence(blocks, value):
        return None
    return {"guard": guard, "line": line, "value": value,
            "expected_sum": constant, "kind": "sum-mismatch"}


def sum_recurrence(blocks, value):
    # shortcut: recognize the saved scalar Source sieve loop; review new IR layouts.
    ssa = r"%[A-Za-z0-9_.$]+"

    def definition(name, pattern):
        block, line = find_definition(blocks, name)
        return block, re.fullmatch(re.escape(name) + r"\s*=\s*" + pattern,
                                  line or "")

    body, add = definition(value, rf"add i64 ({ssa}), ({ssa})")
    if add is None:
        return False
    for accumulator, term in (add.groups(), add.groups()[::-1]):
        header, phi = definition(accumulator, r"phi i64 (.*)")
        if phi is None:
            continue
        incoming = re.findall(r"\[\s*([^,]+),\s*%([^ ]+)\s*\]", phi[1])
        if len(incoming) != 2 or (value, body) not in incoming:
            continue
        initial = [pred for item, pred in incoming if item == "0"]
        if len(initial) != 1:
            continue
        _, choose = definition(term, rf"select i1 ({ssa}), i64 ({ssa}), i64 0")
        if choose is None:
            continue
        condition, index = choose.groups()
        index_header, induction = definition(index, r"phi i64 (.*)")
        if index_header != header or induction is None:
            continue
        indices = re.findall(r"\[\s*([^,]+),\s*%([^ ]+)\s*\]", induction[1])
        back = [item for item, pred in indices if pred == body]
        if len(indices) != 2 or ("2", initial[0]) not in indices or len(back) != 1:
            continue
        next_body, step = definition(back[0], rf"add (?:nuw )?(?:nsw )?i64 {re.escape(index)}, 1")
        if next_body != body or step is None:
            continue
        _, compare = definition(condition, rf"icmp eq i8 ({ssa}), 0")
        if compare is None:
            continue
        _, load = definition(compare[1], rf"load i8, ptr ({ssa}), align 1")
        if load is None:
            continue
        _, address = definition(load[1], rf"getelementptr i8, ptr ({ssa}), i64 {re.escape(index)}")
        if address is None:
            continue
        # The selected term comes from a flag byte at this induction index.
        return True
    return False


def guard_exit_nonzero(blocks, exit_block, guard):
    """Require the guard's true value to select a nonzero return and false 0."""
    for line in blocks.get(exit_block, []):
        match = re.match(
            r"\s*(%[A-Za-z0-9_.$]+)\s*=\s*select\s+i1\s+"
            r"(%[A-Za-z0-9_.$]+)\s*,\s+i(?:32|64)\s+(-?\d+)\s*,\s*"
            r"i(?:32|64)\s+(-?\d+)", line)
        if match is None or match.group(2) != guard:
            continue
        true_value, false_value = int(match.group(3)), int(match.group(4))
        if true_value != 0 and false_value == 0:
            selected = match.group(1)
            terminators = [text.strip() for text in blocks[exit_block]
                           if text.strip() and not text.lstrip().startswith(";")]
            branch = re.fullmatch(r"br label %([A-Za-z0-9_.$]+)",
                                  terminators[-1])
            if branch is None:
                continue
            destination = blocks.get(branch[1], [])
            returns = [re.fullmatch(r"ret i32 (%[A-Za-z0-9_.$]+)", text.strip())
                       for text in destination]
            returns = [item for item in returns if item is not None]
            if len(returns) != 1:
                continue
            return_block, return_line = find_definition(blocks, returns[0][1])
            if return_block != branch[1] or " = phi i32 " not in return_line:
                continue
            incoming = re.findall(r"\[\s*([^,]+),\s*%([^ ]+)\s*\]", return_line)
            if (selected, exit_block) not in incoming:
                continue
            return {"line": line.strip(), "true_value": true_value,
                    "false_value": false_value, "return_phi": return_line}
    return None


def two_trip_boolean_control(header_lines, header, blocks, expected_sum=None,
                             all_blocks=None):
    if all_blocks is None:
        all_blocks = blocks
    for line in header_lines:
        match = re.match(
            r"\s*(%[A-Za-z0-9_.$]+)\s*=\s*phi\s+i1\s+(.*)", line)
        if match is None:
            continue
        incoming = re.findall(
            r"\[\s*(true|false)\s*,\s*%([A-Za-z0-9_.$]+)\s*\]",
            match.group(2))
        if (len(incoming) != 2
                or {value for value, _ in incoming} != {"true", "false"}):
            continue
        initial_value = next(value for value, pred in incoming
                             if pred not in blocks)
        initial = next(pred for value, pred in incoming if value == initial_value)
        latch = next(pred for value, pred in incoming
                     if value != initial_value)
        if initial in blocks or latch not in blocks:
            continue
        latch_lines = blocks[latch]
        or_defs = {}
        for latch_line in latch_lines:
            definition = re.match(
                r"\s*(%[A-Za-z0-9_.$]+)\s*=\s*or\s+i1\s+"
                r"(%[A-Za-z0-9_.$]+)\s*,\s*(%[A-Za-z0-9_.$]+)", latch_line)
            if definition:
                or_defs[definition.group(1)] = {
                    "line": latch_line.strip(),
                    "operands": {definition.group(2), definition.group(3)}}
        for branch in latch_lines:
            branch_match = re.match(
                r"\s*br\s+i1\s+(%[A-Za-z0-9_.$]+)\s*,\s*"
                r"label\s+%([A-Za-z0-9_.$]+)\s*,\s*"
                r"label\s+%([A-Za-z0-9_.$]+)", branch)
            if branch_match is None:
                continue
            condition = branch_match.group(1)
            guard = None
            guard_evidence = None
            exit_evidence = None
            if condition == match.group(1):
                uses_phi = True
            elif (condition in or_defs
                  and match.group(1) in or_defs[condition]["operands"]):
                uses_phi = True
                guard = next(
                    value for value in or_defs[condition]["operands"]
                    if value != match.group(1))
            else:
                uses_phi = False
            if not uses_phi:
                continue
            true_target, false_target = branch_match.group(2), branch_match.group(3)
            if true_target == header:
                loops_on_value = "true"
            elif false_target == header:
                loops_on_value = "false"
            else:
                continue
            if loops_on_value != initial_value:
                continue
            if guard is not None:
                if loops_on_value != "false":
                    continue
                guard_evidence = guard_error_evidence(
                    blocks, latch, guard, expected_sum)
                if guard_evidence is None:
                    continue
                exit_evidence = guard_exit_nonzero(
                    all_blocks, true_target, guard)
                if exit_evidence is None:
                    continue
            return {"phi": match.group(1), "line": line.strip(),
                    "initial": initial_value, "latch": latch,
                    "loops_on_value": loops_on_value,
                    "branch": branch.strip(), "guard": guard,
                    "guard_evidence": guard_evidence,
                    "exit_evidence": exit_evidence}
    return None


def trip_evidence(header_lines, rounds, header, blocks, expected_sum=None,
                  all_blocks=None):
    evidence = []
    boolean = two_trip_boolean_control(
        header_lines, header, blocks, expected_sum, all_blocks=all_blocks)
    if rounds == 2 and boolean:
        evidence.append("two_trip_boolean_induction_phi")
    hit_lines = [line.strip() for line in header_lines
                 if BOUND_LINE_RE.search(line)
                 and re.search(rf"\b{rounds}\b", line)]
    if hit_lines:
        evidence.append(f"icmp_bound_{rounds}")
    return sorted(set(evidence)), boolean, hit_lines


def inspect_round_work(args, env, report, path, function_name, kernel_symbol,
                       rounds, label, expected_sum=None):
    text = Path(path).read_text(encoding="utf-8")
    sections, loop_row = fannkuch.analyze_loops(
        args, env, report, Path(path), f"{label} loops")
    functions = parse_functions(text)
    need(function_name in functions, f"{label}: {function_name} is missing")
    blocks = fannkuch.function_blocks(text, function_name)
    records = sections.get(function_name, [])
    callee = function_kernel(text, sections, kernel_symbol)
    rows, best = [], None
    for index, record in enumerate(records):
        if record["depth"] != 1:
            continue
        names, nested_count = region_names(records, index)
        header = record["blocks"][0]
        region = "\n".join("\n".join(blocks.get(name, [])) for name in names)
        values = counts(region)
        call_count = len(re.findall(
            rf"\b(?:call|invoke)\b[^\n]*@{re.escape(kernel_symbol)}\(",
            region))
        trips, boolean, hit_lines = trip_evidence(
            blocks[header], rounds, header,
            {name: blocks[name] for name in names}, expected_sum,
            all_blocks=blocks)
        inline_kernel = (values["mark_store_i8_1"] > 0
                         and values["load_i8"] > 0
                         and values["add_i64"] > 0
                         and values["check_i64"] > 0
                         and nested_count >= 3)
        call_mode = bool(call_count and callee["kernel_ok"])
        has_kernel = call_mode or (call_count == 0 and inline_kernel)
        verified = bool(has_kernel and trips and rounds >= 2)
        row = {
            "line": record["line"], "header": header, "blocks": names,
            "block_count": len(names), "nested_loops": nested_count,
            **values, "kernel_calls": call_count,
            "inline_kernel": inline_kernel, "call_kernel": call_mode,
            "trip_evidence": trips,
            "boolean_round_control": boolean,
            "guard": boolean.get("guard") if boolean else None,
            "guard_evidence": (boolean.get("guard_evidence")
                               if boolean else None),
            "exit_evidence": (boolean.get("exit_evidence")
                              if boolean else None),
            "bound_hits": hit_lines,
            "mode": ("kernel_call_in_round_loop" if call_mode else
                     "inlined_kernel_in_round_loop" if inline_kernel else
                     "no_kernel"),
            "verified": verified,
        }
        rows.append(row)
        score = (verified, has_kernel, call_count, nested_count, len(trips))
        if best is None or score > best[0]:
            best = (score, row)
    mark_regions = [row for row in rows
                    if row["mark_store_i8_1"] > 0
                    and row["nested_loops"] > 0 and row["load_i8"] > 0]
    stat_regions = [row for row in rows
                    if row["mark_store_i8_1"] == 0
                    and row["load_i8"] > 0 and row["add_i64"] > 0
                    and row["check_i64"] > 0]
    unrolled = (rounds >= 2 and len(mark_regions) == rounds
                and len(stat_regions) >= rounds
                and len(CHECK_I64_RE.findall(text)) >= 2 * rounds)
    selected = best[1] if best else None
    if selected and selected["verified"]:
        mode = selected["mode"]
    elif unrolled:
        mode = "unrolled_mark_and_stat_loops"
    else:
        mode = "evidence_insufficient"
    return {
        "label": label, "path": str(path), "function": function_name,
        "kernel_symbol": kernel_symbol, "rounds": rounds,
        "loop_analysis": loop_row, "callee": callee, "regions": rows,
        "mark_regions": len(mark_regions),
        "stat_regions": len(stat_regions),
        "unrolled_mark_and_stat_loops": unrolled,
        "selected": selected, "verified": bool(
            (selected and selected["verified"]) or unrolled),
        "status": "verified" if ((selected and selected["verified"]) or unrolled)
        else "not_verified",
        "mode": mode,
        "not_verified_by_loops_or_counts_only": True,
        "after_ir": file_rec(path),
    }


def mutate_no_kernel(text, function_name, rounds):
    pattern = re.compile(
        rf"^define\b[^\n]*@{re.escape(function_name)}\([^\n]*\{{\s*$",
        re.MULTILINE)
    matches = list(pattern.finditer(text))
    need(len(matches) == 1,
         f"no-kernel control: expected one {function_name} definition")
    match = matches[0]
    end = re.compile(r"^\}\s*$", re.MULTILINE).search(text, match.end())
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


def mutate_source_round(text, work):
    selected = work.get("selected") or {}
    boolean = selected.get("boolean_round_control")
    hits = selected.get("bound_hits", [])
    mutated, changed = text, 0
    if boolean:
        from_value = boolean["initial"]
        to_value = "false" if from_value == "true" else "true"
        replacement = re.sub(
            rf"\[\s*{from_value}\s*,", f"[ {to_value},", boolean["line"],
            count=1)
        if replacement != boolean["line"]:
            mutated = mutated.replace(boolean["line"], replacement, 1)
            changed += 1
    for line in hits[:1]:
        replacement = re.sub(
            rf"\b{work['rounds']}\b", str(work["rounds"] - 1), line, count=1)
        if replacement != line:
            mutated = mutated.replace(line, replacement, 1)
            changed += 1
    need(changed > 0, "Source round control did not change any IR line")
    return mutated


def compile_cpp_ir(args, env, report, out, reference, output, opt, label):
    output.parent.mkdir(parents=True, exist_ok=True)
    run = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         "-g", "-S", "-emit-llvm", reference, "-o", output], args, env)
    common.require_success(run, f"{label} IR build")
    return {"command_index": run["command_index"], "ir": file_rec(output, out),
            "verify": verify_ir(output, args, env, report, f"{label} verify")}


def real_r1_cpp_control(args, env, report, out, templates, n, rounds, count,
                        total, label):
    root = out / "controls" / "cpp-real-r1"
    case = generate_case(root, templates, n, 1, count, total, "correct")
    reference = Path(case["reference"]["path"])
    program = root / "build" / "O2" / "reference"
    build = build_cpp(args, env, out, reference, program, 2,
                      f"{label} real R1 cpp O2 build")
    run = run_program([program], args, env)
    need(run["status"] == "exited" and run["exit"] == 0
         and not run["stdout"] and not run["stderr"],
         f"{label}: real R1 cpp did not quiet exit 0")
    ir_path = root / "cpp-R1-O2.ll"
    ir = compile_cpp_ir(args, env, report, out, reference, ir_path, 2,
                        f"{label} real R1 cpp O2 IR")
    text = ir_path.read_text(encoding="utf-8")
    kernel = None
    try:
        kernel = fannkuch.cpp_linkage(text, "sieve")
    except PerfError:
        kernel = None
    work = inspect_round_work(
        args, env, report, ir_path, "main", kernel, rounds,
        f"{label} real R1 asked for R{rounds}", expected_sum=total)
    need(not work["verified"],
         f"{label}: real R1 C++ was incorrectly accepted as R{rounds}")
    return {"implementation": "cpp", "kind": "real-r1-program-asked-as-r2",
            "path": str(ir_path), "manifest": case["manifest"],
            "reference": case["reference"], "build": build,
            "execution": {"command_index": run["command_index"], "exit": 0,
                          "stdout": run["stdout"], "stderr": run["stderr"]},
            "ir": ir["ir"], "verify": ir["verify"], "work": work,
            "actual_rounds": 1, "expected_rounds_checked": rounds,
            "rejected": True}


def negative_controls(args, env, report, out, templates, n, rounds, count,
                      total, source_ir, source_function, source_kernel, cpp_ir,
                      cpp_kernel, label):
    controls = []
    for implementation, path, function_name, kernel in (
            ("source", Path(source_ir), source_function, source_kernel),
            ("cpp", Path(cpp_ir), "main", cpp_kernel)):
        text = path.read_text(encoding="utf-8")
        no_kernel = mutate_no_kernel(text, function_name, rounds)
        no_kernel_path = out / "negative-ir" / implementation / "no-kernel.ll"
        no_kernel_path.parent.mkdir(parents=True, exist_ok=True)
        no_kernel_path.write_text(no_kernel, encoding="utf-8")
        verify = verify_ir(no_kernel_path, args, env, report,
                           f"{label} {implementation} no-kernel verify")
        work = inspect_round_work(
            args, env, report, no_kernel_path, function_name, kernel, rounds,
            f"{label} {implementation} no-kernel", expected_sum=total)
        need(not work["verified"],
             f"{label}: {implementation} no-kernel control was not rejected")
        controls.append({"implementation": implementation,
                         "kind": "remove-kernel", "path": str(no_kernel_path),
                         "verify": verify, "work": work, "rejected": True})
        if implementation == "cpp":
            controls.append(real_r1_cpp_control(
                args, env, report, out, templates, n, rounds, count, total,
                label))
            continue
        original = inspect_round_work(
            args, env, report, path, function_name, kernel, rounds,
            f"{label} {implementation} original round control",
            expected_sum=total)
        reduced = mutate_source_round(text, original)
        reduced_path = out / "negative-ir" / implementation / "one-round.ll"
        reduced_path.write_text(reduced, encoding="utf-8")
        verify = verify_ir(reduced_path, args, env, report,
                           f"{label} {implementation} one-round verify")
        work = inspect_round_work(
            args, env, report, reduced_path, function_name, kernel, rounds,
            f"{label} {implementation} one-round", expected_sum=total)
        need(not work["verified"],
             f"{label}: {implementation} one-round control was not rejected")
        controls.append({"implementation": implementation,
                         "kind": "real-one-round-control",
                         "path": str(reduced_path), "verify": verify,
                         "work": work, "rejected": True})
    return controls


def parse_frames(path, expected_rounds, expected_length, label):
    data = Path(path).read_bytes()
    frames, offset = [], 0
    while offset < len(data):
        need(offset + 8 <= len(data), f"{label}: truncated frame header")
        length = int.from_bytes(data[offset:offset + 8], "little")
        offset += 8
        need(offset + length <= len(data), f"{label}: truncated frame body")
        frames.append(data[offset:offset + length])
        offset += length
    need(len(frames) == expected_rounds,
         f"{label}: observed {len(frames)} rounds, expected {expected_rounds}")
    need(all(len(frame) == expected_length for frame in frames),
         f"{label}: a frame length is not {expected_length}")
    return frames


def observe_source(args, env, out, case, opt, langc_row, n, rounds,
                   expected_composite, expected_flags, label):
    directory = out / "observers" / "source" / f"n{n}" / f"r{rounds}" / f"O{opt}"
    directory.mkdir(parents=True, exist_ok=False)
    binary = directory / "observer"
    build = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         "-fno-fast-math", "-fno-lto", "-I", str(WORKTREE / "runtime/native"),
         f"-DSIEVE_OBSERVE_N={n}", OBSERVER_SOURCE,
         langc_row["object"]["path"], args.runtime,
         f"--ld-path={args.llvm_bin / 'ld.lld'}", "-o", binary], args, env)
    common.require_success(build, f"{label} Source observer build")
    observed_path = directory / "observed.bin"
    run = run_program([binary], args, env,
                      {"SIEVE_OBSERVER_OUT": str(observed_path)})
    need(run["status"] == "exited" and run["exit"] == 0
         and not run["stdout"] and not run["stderr"],
         f"{label}: Source observer run failed")
    frames = parse_frames(observed_path, rounds, n + 1, label)
    matches = [frame == expected_composite for frame in frames]
    need(all(matches), f"{label}: Source composite frames do not match flags")
    observed_flags = [
        bytes([0, 0]) + bytes(1 - value for value in frame[2:])
        for frame in frames]
    need(all(value == expected_flags for value in observed_flags),
         f"{label}: inverse composite flags do not match the frozen flags")
    row = {
        "implementation": "source-auxiliary-observer", "opt": opt,
        "formal_object": langc_row["object"],
        "build": {"command_index": build["command_index"],
                  "binary": file_rec(binary, out),
                  "elf": common.checked_elf(binary, executable=True,
                                            label=f"{label} observer")},
        "run": {"command_index": run["command_index"], "exit": run["exit"],
                "stdout": run["stdout"], "stderr": run["stderr"]},
        "observed": file_rec(observed_path, out),
        "captured_rounds": len(frames),
        "captured_rounds_expected": rounds,
        "captured_bytes": len(frames[0]) if frames else 0,
        "captured_total_bytes": sum(len(frame) for frame in frames),
        "captured_sha256": [sha256_bytes(frame) for frame in frames],
        "observed_flags_sha256": [
            sha256_bytes(value) for value in observed_flags],
        "matches_frozen_composite": all(matches),
        "matches_frozen_flags_via_inverse": True,
        "not_checksum_or_sampling": True,
        "formal_cli": False, "host_libc_dependent": True,
    }
    case["observers"].append(row)
    return row


def observe_cpp(args, env, out, case, n, rounds, expected_composite,
                expected_flags, label):
    directory = out / "observers" / "cpp" / f"n{n}" / f"r{rounds}" / "O2"
    directory.mkdir(parents=True, exist_ok=False)
    reference = case["variants"]["correct"]["reference"]
    formal_text = Path(reference["path"]).read_text(encoding="utf-8")
    include = "#include <vector>\n"
    check = "    if (count != expected_count) {\n"
    need(formal_text.count(include) == 1 and formal_text.count(check) == 1,
         f"{label}: C++ observer insertion points are not unique")
    aux_text = formal_text.replace(
        include, include + '\nextern "C" void sieve_observe_cpp('
        'const uint8_t *, std::size_t);\n', 1).replace(
        check, "    sieve_observe_cpp(composite.data(), composite.size());\n"
        + check, 1)
    aux_source = directory / "aux-reference.cpp"
    aux_source.write_text(aux_text, encoding="utf-8")
    binary = directory / "observer"
    build = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", "-O2",
         "-fno-fast-math", "-fno-lto", f"-DSIEVE_OBSERVE_N={n}",
         "-I", str(WORKTREE / "runtime/native"), OBSERVER_SOURCE, aux_source,
         f"--ld-path={args.llvm_bin / 'ld.lld'}", "-o", binary], args, env)
    common.require_success(build, f"{label} C++ observer build")
    observed_path = directory / "observed.bin"
    run = run_program([binary], args, env,
                      {"SIEVE_OBSERVER_OUT": str(observed_path)})
    need(run["status"] == "exited" and run["exit"] == 0
         and not run["stdout"] and not run["stderr"],
         f"{label}: C++ observer run failed")
    frames = parse_frames(observed_path, rounds, n + 1, label)
    matches = [frame == expected_composite for frame in frames]
    need(all(matches), f"{label}: C++ composite frames do not match flags")
    observed_flags = [
        bytes([0, 0]) + bytes(1 - value for value in frame[2:])
        for frame in frames]
    need(all(value == expected_flags for value in observed_flags),
         f"{label}: inverse C++ composite flags do not match the frozen flags")
    row = {
        "implementation": "cpp-auxiliary-observer", "opt": 2,
        "formal_reference": reference,
        "auxiliary_source": file_rec(aux_source, out),
        "formal_source_unchanged": (
            sha256_bytes(formal_text.encode())
            == reference["sha256"]),
        "insertion": "one extern declaration and one call before count check",
        "build": {"command_index": build["command_index"],
                  "binary": file_rec(binary, out),
                  "elf": common.checked_elf(binary, executable=True,
                                            label=f"{label} observer")},
        "run": {"command_index": run["command_index"], "exit": run["exit"],
                "stdout": run["stdout"], "stderr": run["stderr"]},
        "observed": file_rec(observed_path, out),
        "captured_rounds": len(frames),
        "captured_rounds_expected": rounds,
        "captured_bytes": len(frames[0]) if frames else 0,
        "captured_total_bytes": sum(len(frame) for frame in frames),
        "captured_sha256": [sha256_bytes(frame) for frame in frames],
        "observed_flags_sha256": [
            sha256_bytes(value) for value in observed_flags],
        "matches_frozen_composite": all(matches),
        "matches_frozen_flags_via_inverse": True,
        "not_checksum_or_sampling": True,
        "formal_cli": False, "host_libc_dependent": True,
    }
    case["observers"].append(row)
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
        "templates": template_records(),
        "observer_source": file_rec(OBSERVER_SOURCE),
        "python_metadata_used": False,
        "python_metadata_reason": "helper JSON entry_symbol plus pre-opt IR "
                                  "callee identify main/sieve without emitting "
                                  "a huge generated Python program",
    })
    report["target"].update({
        "cpp_flags": list(TARGET_FLAGS),
        "cpp_linker": str(args.llvm_bin / "ld.lld"),
    })
    report["tools"]["python_helper"] = common.tool_record(
        args.python_helper, env, args.cpu, None)
    return report


def tool_paths(args):
    return {
        "langc": args.langc, "helper": args.helper, "runtime": args.runtime,
        "clang": args.llvm_bin / "clang", "clang++": args.llvm_bin / "clang++",
        "ld.lld": args.llvm_bin / "ld.lld", "opt": args.llvm_bin / "opt",
        "llvm-readelf": args.llvm_bin / "llvm-readelf",
        "llvm-nm": args.llvm_bin / "llvm-nm", "python": args.python,
        "python_helper": args.python_helper,
    }


def input_paths(args, oracle_files):
    paths = [PERFORMANCE_DIR / name for name in TEMPLATE_NAMES]
    paths += [PERFORMANCE_DIR / "prepare.py", PERFORMANCE_DIR / "README.md",
              PERFORMANCE_DIR / "large_prepare.py", OBSERVER_SOURCE,
              PERFORMANCE_DIR.parent / "run_native.py",
              PERFORMANCE_DIR.parent / "fannkuch" / "large_prepare.py",
              WORKTREE / "runtime/native/vec.hpp"]
    paths += list(oracle_files)
    return sorted({Path(path).resolve() for path in paths if Path(path).is_file()})


def snapshot(paths):
    return {str(path): common.sha256_file(path) for path in paths}


def prepare(args):
    env = common.make_env(args)
    out = common.prepare_out(args.out)
    (out / "started_at.txt").write_text(now() + "\n", encoding="utf-8")
    report = {
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "mode": "prepare", "status": "running",
        "commands": [], "verifies": [], "loop_analyses": [], "elf_checks": [],
        "negative_controls": [], "cases": [],
    }
    common._COMMAND_LOG = report["commands"]
    try:
        report.update(base_report(args, env))
        need(args.n in N_VALUES, f"--n must be one of {N_VALUES}")
        need(args.rounds in (1, 2), "--rounds must be 1 or 2")
        report["parameters"] = {
            "n": args.n, "rounds": args.rounds,
            "expected_count": PINNED[args.n]["count"],
            "expected_sum": PINNED[args.n]["sum"],
            "variant_expected_exits": {"correct": 0, "wrong_count": 1,
                                       "wrong_sum": 2},
            "target_flags": list(TARGET_FLAGS),
            "observer_opt": 2,
        }
        tools_before = {name: file_rec(path)
                        for name, path in tool_paths(args).items()}
        for name, expected_sha in PINNED_TOOLS.items():
            need(tools_before[name]["sha256"] == expected_sha,
                 f"frozen tool {name} SHA-256 changed: "
                 f"{tools_before[name]['sha256']}")
        report["tools_before"] = tools_before
        report["frozen_tools_verified"] = True
        main_templates = {
            name: file_rec(MAIN_TEMPLATES / name) for name in TEMPLATE_NAMES}
        need(all(main_templates[name]["sha256"] == report["templates"][name]["sha256"]
                 for name in TEMPLATE_NAMES),
             "worktree templates differ from the main-repo templates")
        report["main_templates"] = main_templates

        flags, composites, oracle, oracle_files = load_oracle(args.oracle_dir)
        expected_composite = composites[args.n]
        before = snapshot(input_paths(args, oracle_files))
        report["oracle"] = oracle
        report["input_hashes_before"] = before

        templates = read_templates()
        n, rounds = args.n, args.rounds
        case = {
            "n": n, "rounds": rounds, "variants": {}, "check": None,
            "builds": [], "executions": [], "helper": [], "cpp_ir": {},
            "observers": [], "source_work": None, "cpp_work": None,
            "negative_llvm_controls": [],
        }
        report["cases"].append(case)
        variants = ["correct"]
        if rounds == 1:
            variants += ["wrong_count", "wrong_sum"]
        for variant in variants:
            case["variants"][variant] = generate_case(
                out, templates, n, rounds, PINNED[n]["count"],
                PINNED[n]["sum"], variant)
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

        for variant_name, variant_row in case["variants"].items():
            variant_manifest = Path(variant_row["manifest"]["path"])
            reference = Path(variant_row["reference"]["path"])
            for opt in (0, 2):
                label = f"n={n} r={rounds} {variant_name} langc O{opt}"
                source_build = build_langc(
                    args, env, out, variant_manifest,
                    out / "build" / f"n{n}" / f"r{rounds}" / variant_name /
                    "langc" / f"O{opt}", opt, label)
                variant_row["builds"].append(source_build)
                execute(args, env, variant_row, "langc", opt,
                        source_build["program"]["path"], label)
                elf_checks(args, env, report, source_build["program"]["path"],
                           label, "langc")
                label = f"n={n} r={rounds} {variant_name} cpp O{opt}"
                cpp_build = build_cpp(
                    args, env, out, reference,
                    out / "build" / f"n{n}" / f"r{rounds}" / variant_name /
                    "cpp" / f"O{opt}" / "reference", opt, label)
                variant_row["builds"].append(cpp_build)
                execute(args, env, variant_row, "cpp", opt,
                        cpp_build["program"]["path"], label)
                elf_checks(args, env, report, cpp_build["program"]["path"],
                           label, "cpp")

        correct_builds = {(row["implementation"], row["opt"]): row
                          for row in correct["builds"]}
        for opt in (0, 2):
            helper = helper_export(
                args, env, report, out, manifest,
                out / "llvm" / f"n{n}" / f"r{rounds}" / f"O{opt}", opt,
                correct_builds[("langc", opt)]["object"]["path"],
                f"n={n} r={rounds} Source O{opt} LLVM")
            need(helper["object_match"],
                 f"n={n} r={rounds} O{opt}: helper object differs from CLI object")
            correct["helper"].append(helper)
            ir = compile_cpp_ir(
                args, env, report, out, Path(correct["reference"]["path"]),
                out / "llvm" / f"n{n}" / f"r{rounds}" / f"cpp-O{opt}.ll", opt,
                f"n={n} r={rounds} cpp O{opt} IR")
            case["cpp_ir"][f"O{opt}"] = ir

        helper_o2 = correct["helper"][1]
        symbols, before_functions = source_symbols(
            helper_o2, f"n={n} r={rounds}")
        case["source_symbols"] = symbols
        source_text = Path(helper_o2["after"]["path"]).read_text(encoding="utf-8")
        source_function, source_selection = source_work_function(
            source_text, symbols["main"])
        case["source_function_selection"] = source_selection
        cpp_text = Path(case["cpp_ir"]["O2"]["ir"]["path"]).read_text(
            encoding="utf-8")
        try:
            cpp_kernel = fannkuch.cpp_linkage(cpp_text, "sieve")
        except PerfError:
            cpp_kernel = None
        checked_rounds = rounds if rounds >= 2 else 2
        source_work = inspect_round_work(
            args, env, report, Path(helper_o2["after"]["path"]),
            source_function, f"lang.fn.{symbols['sieve']}", checked_rounds,
            f"n={n} r={rounds} Source O2 asked for R{checked_rounds}",
            expected_sum=PINNED[n]["sum"])
        source_work["requested_rounds"] = checked_rounds
        cpp_work = inspect_round_work(
            args, env, report, Path(case["cpp_ir"]["O2"]["ir"]["path"]),
            "main", cpp_kernel, checked_rounds,
            f"n={n} r={rounds} cpp O2 asked for R{checked_rounds}",
            expected_sum=PINNED[n]["sum"])
        cpp_work["requested_rounds"] = checked_rounds
        case["source_work"], case["cpp_work"] = source_work, cpp_work

        observe_source(args, env, out, case, 2,
                       correct_builds[("langc", 2)],
                       n, rounds, expected_composite, flags[n],
                       f"n={n} r={rounds} Source O2 observer")
        observe_cpp(args, env, out, case, n, rounds, expected_composite,
                    flags[n], f"n={n} r={rounds} C++ O2 observer")

        if rounds >= 2:
            need(source_work["verified"],
                 f"n={n} r=2: Source O2 repeated work was not verified")
            need(cpp_work["verified"],
                 f"n={n} r=2: C++ O2 repeated work was not verified")
            controls = negative_controls(
                args, env, report, out, templates, n, rounds,
                PINNED[n]["count"], PINNED[n]["sum"],
                Path(helper_o2["after"]["path"]), source_function,
                f"lang.fn.{symbols['sieve']}",
                Path(case["cpp_ir"]["O2"]["ir"]["path"]), cpp_kernel,
                f"n={n} r={rounds}")
            case["negative_llvm_controls"] = controls
        else:
            need(not source_work["verified"],
                 f"n={n} r=1: Source R1 IR was incorrectly accepted as R2")
            need(not cpp_work["verified"],
                 f"n={n} r=1: C++ R1 IR was incorrectly accepted as R2")
            source_work["r1_cannot_prove_r2"] = True
            cpp_work["r1_cannot_prove_r2"] = True

        tools_after = {name: file_rec(path)
                       for name, path in tool_paths(args).items()}
        report["tools_after"] = tools_after
        report["tools_unchanged"] = tools_before == tools_after
        need(report["tools_unchanged"], "tool SHA-256 values changed")
        after = snapshot(input_paths(args, oracle_files))
        report["input_hashes_after"] = after
        report["inputs_unchanged"] = before == after
        need(report["inputs_unchanged"], "source/tool/oracle inputs changed")
        report["repositories_after"] = {
            "worktree": common.repo_info(WORKTREE, env, args.cpu),
            "compiler_root": common.repo_info(args.compiler_root, env, args.cpu),
        }
        report["worktree_note"] = (
            "The worktree status intentionally changes because this run writes "
            "its new checks output directory; fixed input/tool hashes are the "
            "stability evidence.")

        builds = [row for variant in case["variants"].values()
                  for row in variant["builds"]]
        executions = [row for variant in case["variants"].values()
                      for row in variant["executions"]]
        helper_rows = correct["helper"]
        quiet0 = sum(1 for row in executions if row["success"]
                     and row["exit"] == 0 and not row["stdout"]
                     and not row["stderr"])
        quiet1 = sum(1 for row in executions if row["success"]
                     and row["exit"] == 1 and not row["stdout"]
                     and not row["stderr"])
        quiet2 = sum(1 for row in executions if row["success"]
                     and row["exit"] == 2 and not row["stdout"]
                     and not row["stderr"])
        observers = case["observers"]
        summary = {
            "n": n, "rounds": rounds,
            "variants": len(case["variants"]),
            "builds": len(builds),
            "source_builds": sum(1 for row in builds
                                 if row["implementation"] == "langc"),
            "cpp_builds": sum(1 for row in builds
                              if row["implementation"] == "cpp"),
            "executions": len(executions),
            "quiet_exit_0": quiet0, "quiet_exit_1": quiet1,
            "quiet_exit_2": quiet2,
            "source_llvm_exports": len(helper_rows),
            "source_ir_verifies": 2 * len(helper_rows),
            "source_object_matches": sum(1 for row in helper_rows
                                         if row["object_match"]),
            "cpp_ir_exports": 2, "cpp_ir_verifies": 2,
            "all_ir_verifies": len(report["verifies"]),
            "elf_checks": len(report["elf_checks"]),
            "source_elf_static_checks": sum(
                1 for row in report["elf_checks"]
                if row["implementation"] == "langc"),
            "cpp_dynamic_elf_checks": sum(
                1 for row in report["elf_checks"]
                if row["implementation"] == "cpp"),
            "observers": len(observers),
            "observer_rounds": sum(row["captured_rounds"] for row in observers),
            "observer_rounds_expected": 2 * rounds,
            "observers_match": sum(
                1 for row in observers if row["matches_frozen_composite"]),
            "flags_full_bytes": PINNED[n]["bytes"],
            "r2_source_work_verified": bool(
                rounds >= 2 and source_work["verified"]),
            "r2_cpp_work_verified": bool(
                rounds >= 2 and cpp_work["verified"]),
            "r1_actual_round2_checks": 2 if rounds == 1 else 0,
            "r1_source_round2_claim_rejected": bool(
                rounds == 1 and not source_work["verified"]),
            "r1_cpp_round2_claim_rejected": bool(
                rounds == 1 and not cpp_work["verified"]),
            "negative_llvm_controls": len(case["negative_llvm_controls"]),
            "negative_llvm_controls_rejected": sum(
                1 for row in case["negative_llvm_controls"] if row["rejected"]),
            "python_large_program_executed": False,
            "timing_performed": False,
        }
        if rounds == 1:
            expected = {
                "variants": 3, "builds": 12, "source_builds": 6,
                "cpp_builds": 6, "executions": 12, "quiet_exit_0": 4,
                "quiet_exit_1": 4, "quiet_exit_2": 4,
                "source_llvm_exports": 2, "source_ir_verifies": 4,
                "source_object_matches": 2, "cpp_ir_exports": 2,
                "cpp_ir_verifies": 2, "all_ir_verifies": 6,
                "elf_checks": 12, "source_elf_static_checks": 6,
                "cpp_dynamic_elf_checks": 6, "observers": 2,
                "observer_rounds": 2, "observer_rounds_expected": 2,
                "observers_match": 2, "r2_source_work_verified": False,
                "r2_cpp_work_verified": False, "r1_actual_round2_checks": 2,
                "r1_source_round2_claim_rejected": True,
                "r1_cpp_round2_claim_rejected": True,
                "negative_llvm_controls": 0,
                "negative_llvm_controls_rejected": 0,
            }
        else:
            expected = {
                "variants": 1, "builds": 4, "source_builds": 2,
                "cpp_builds": 2, "executions": 4, "quiet_exit_0": 4,
                "quiet_exit_1": 0, "quiet_exit_2": 0,
                "source_llvm_exports": 2, "source_ir_verifies": 4,
                "source_object_matches": 2, "cpp_ir_exports": 2,
                "cpp_ir_verifies": 2, "all_ir_verifies": 10,
                "elf_checks": 4, "source_elf_static_checks": 2,
                "cpp_dynamic_elf_checks": 2, "observers": 2,
                "observer_rounds": 4, "observer_rounds_expected": 4,
                "observers_match": 2, "r2_source_work_verified": True,
                "r2_cpp_work_verified": True, "r1_actual_round2_checks": 0,
                "r1_source_round2_claim_rejected": False,
                "r1_cpp_round2_claim_rejected": False,
                "negative_llvm_controls": 4,
                "negative_llvm_controls_rejected": 4,
            }
        expected.update({"n": n, "rounds": rounds,
                         "flags_full_bytes": PINNED[n]["bytes"],
                         "python_large_program_executed": False,
                         "timing_performed": False})
        need(summary == expected, f"prepare summary is unexpected: {summary}")
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


def compact_work(work):
    selected = work.get("selected") or {}
    return {
        "status": work["status"], "mode": work["mode"],
        "verified": work["verified"],
        "requested_rounds": work.get("rounds"),
        "function": work.get("function"),
        "kernel_symbol": work.get("kernel_symbol"),
        "trip_evidence": selected.get("trip_evidence"),
        "guard": selected.get("guard"),
        "guard_evidence": selected.get("guard_evidence"),
        "exit_evidence": selected.get("exit_evidence"),
        "boolean_round_control": selected.get("boolean_round_control"),
        "mark_regions": work.get("mark_regions"),
        "stat_regions": work.get("stat_regions"),
        "unrolled_mark_and_stat_loops": work.get(
            "unrolled_mark_and_stat_loops"),
        "selected_mode": selected.get("mode"),
        "after_ir": work.get("after_ir"),
    }


def correction_input_paths(args, old_root):
    paths = [
        Path(__file__).resolve(), OBSERVER_SOURCE,
        PERFORMANCE_DIR / "sieve.lang.in",
        PERFORMANCE_DIR / "reference.cpp.in",
        PERFORMANCE_DIR / "prepare.py",
        old_root / "summary.json",
        MAIN / "checks/sieve-large-independent-20261110/source-exit-results.json",
        MAIN / "checks/sieve-large-independent-20261110/source-r2-constant-false-backedge.ll",
    ]
    for name in EXISTING_CASES + ("n1000000-r1-dev1", "n1000000-r1-dev2"):
        report_path = old_root / name / "prepare.json"
        if report_path.is_file():
            paths.append(report_path)
            record = json.loads(report_path.read_text(encoding="utf-8"))
            case = record["cases"][0]
            paths.extend(Path(row["after"]["path"])
                         for row in case["variants"]["correct"]["helper"]
                         if row.get("after"))
            cpp_ir = case.get("cpp_ir", {}).get("O2", {}).get("ir")
            if cpp_ir:
                paths.append(Path(cpp_ir["path"]))
    return sorted({Path(path).resolve() for path in paths if Path(path).is_file()})


def probe_root_mutant(args, env, report, out, root_mutant, expected_sum):
    directory = out / "probes"
    directory.mkdir(parents=True, exist_ok=True)
    copied = directory / "root-bad-or.ll"
    copied.write_bytes(Path(root_mutant).read_bytes())
    verify = verify_ir(copied, args, env, report, "root bad OR guard verify")
    work = inspect_round_work(
        args, env, report, copied, "main", "lang.fn.20", 2,
        "root bad OR guard work", expected_sum=expected_sum)
    need(not work["verified"],
         "root bad OR guard was incorrectly accepted as two rounds")
    binary = directory / "root-bad-or"
    build = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-O2",
         copied, args.runtime, f"--ld-path={args.llvm_bin / 'ld.lld'}",
         "-o", binary], args, env)
    common.require_success(build, "root bad OR guard executable build")
    run = run_program([binary], args, env)
    need(run["status"] == "exited" and run["exit"] == 0
         and not run["stdout"] and not run["stderr"],
         "root bad OR guard executable did not quiet exit 0")
    return {"kind": "root-bad-or-guard", "path": str(copied),
            "source_root": file_rec(root_mutant),
            "verify": verify, "work": compact_work(work),
            "build": {"command_index": build["command_index"],
                      "binary": file_rec(binary, out)},
            "execution": {"command_index": run["command_index"],
                          "exit": run["exit"], "stdout": run["stdout"],
                          "stderr": run["stderr"]},
            "rejected": True}


def probe_source_mutations(args, env, report, out, base_text, expected_sum):
    directory = out / "probes"
    directory.mkdir(parents=True, exist_ok=True)
    phi_line = next(line.strip() for line in base_text.splitlines()
                    if "= phi i1 [ false," in line and "true," in line)
    guard_line = next(line.strip() for line in base_text.splitlines()
                      if "= icmp ne i64" in line and "37550402023" in line)
    select_line = next(line.strip() for line in base_text.splitlines()
                       if "= select i1" in line and "i32 2" in line)
    probes = []
    mutations = [
        ("forced-constant-true-backedge",
         phi_line,
         phi_line.replace("[ false,", "[ true,", 1)),
        ("swapped-phi-predecessors",
         phi_line,
         "%.not1 = phi i1 [ true, %checked2.i ], [ false, %lang.fn.20.exit.i ]"),
        ("count-guard-shortcut",
         guard_line,
         guard_line.replace("37550402023", "78498")),
        ("sum-guard-shortcut",
         guard_line,
         guard_line.replace("37550402023", "0")),
        ("nonzero-exit-control-shortcut",
         select_line,
         select_line.replace("i32 2, i32 0", "i32 0, i32 0")),
    ]
    for name, old, new in mutations:
        need(base_text.count(old) == 1,
             f"{name}: base line is not unique")
        need(new != old, f"{name}: mutation did not change the line")
        text = base_text.replace(old, new, 1)
        path = directory / f"{name}.ll"
        path.write_text(text, encoding="utf-8")
        verify = verify_ir(path, args, env, report, f"{name} verify")
        work = inspect_round_work(
            args, env, report, path, "main", "lang.fn.20", 2, name,
            expected_sum=expected_sum)
        need(not work["verified"],
             f"{name}: mutated IR was incorrectly accepted as two rounds")
        probes.append({"kind": name, "path": str(path),
                       "old_line": old, "new_line": new,
                       "verify": verify, "work": compact_work(work),
                       "rejected": True})
    return probes


def recheck_existing(args):
    env = common.make_env(args)
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    old_root = Path(args.existing_root).resolve()
    report = {
        "schema": "compiler-dev/native-sieve-large-correction/1",
        "task": TASK, "contract": CONTRACT, "operation_id": OPERATION_ID,
        "mode": "correction-recheck", "status": "running",
        "commands": [], "verifies": [], "loop_analyses": [],
        "elf_checks": [], "cases": [], "probes": [],
    }
    common._COMMAND_LOG = report["commands"]
    try:
        need(old_root.is_dir(), f"existing root is missing: {old_root}")
        public_out = out / "public-entry-args-unused"
        public_argv = [
            "--out", str(public_out), "--n", "1000000", "--rounds", "2",
            "--oracle-dir", str(args.oracle_dir),
            "--python", str(args.python),
            "--python-helper", str(args.python_helper),
            "--langc", str(args.langc), "--helper", str(args.helper),
            "--runtime", str(args.runtime),
            "--llvm-bin", str(args.llvm_bin),
            "--compiler-root", str(args.compiler_root),
            "--timeout", str(args.timeout), "--run-timeout", str(args.run_timeout),
        ]
        if args.ld_library_path is not None:
            public_argv += ["--ld-library-path", str(args.ld_library_path)]
        if args.cpu is not None:
            public_argv += ["--cpu", str(args.cpu)]
        public_args = parse_args(public_argv)
        validate_args(public_args)
        report["public_entry_probe"] = {
            "mode": "prepare", "argv": public_argv,
            "parse_and_validate": True,
            "same_work_arguments": True,
        }
        # Use the public argument object for all work checks, not a special path.
        work_args = public_args
        work_env = common.make_env(work_args)

        inputs_before = snapshot(correction_input_paths(work_args, old_root))
        tools_before = {name: file_rec(path)
                        for name, path in tool_paths(work_args).items()}
        for name, expected_sha in PINNED_TOOLS.items():
            need(tools_before[name]["sha256"] == expected_sha,
                 f"frozen tool {name} SHA-256 changed")
        report["tools_before"] = tools_before
        report["inputs_before"] = inputs_before

        cases = []
        source_r2_text = None
        for name in EXISTING_CASES:
            old_report_path = old_root / name / "prepare.json"
            old = json.loads(old_report_path.read_text(encoding="utf-8"))
            need(old.get("status") == "passed", f"{name}: old report is not passed")
            need(old.get("script", {}).get("sha256") ==
                 "b3ffadf7176079992d4e7b4e1d1d742aaa49f7ff99472d5cb5e23fe515e765c6",
                 f"{name}: unexpected old script hash")
            case = old["cases"][0]
            n, rounds = case["n"], case["rounds"]
            expected_sum = PINNED[n]["sum"]
            helpers = case["variants"]["correct"]["helper"]
            source_ir = Path(helpers[1]["after"]["path"])
            source_text = source_ir.read_text(encoding="utf-8")
            source_function, source_selection = source_work_function(
                source_text, case["source_symbols"]["main"])
            source_work = inspect_round_work(
                work_args, work_env, report, source_ir, source_function,
                f"lang.fn.{case['source_symbols']['sieve']}", 2,
                f"{name} Source actual O2 checked as R2",
                expected_sum=expected_sum)
            cpp_ir = Path(case["cpp_ir"]["O2"]["ir"]["path"])
            cpp_text = cpp_ir.read_text(encoding="utf-8")
            try:
                cpp_kernel = fannkuch.cpp_linkage(cpp_text, "sieve")
            except PerfError:
                cpp_kernel = None
            cpp_work = inspect_round_work(
                work_args, work_env, report, cpp_ir, "main", cpp_kernel, 2,
                f"{name} C++ actual O2 checked as R2",
                expected_sum=expected_sum)
            expected_verified = rounds == 2
            need(source_work["verified"] == expected_verified,
                 f"{name}: Source status {source_work['status']} != expected")
            need(cpp_work["verified"] == expected_verified,
                 f"{name}: C++ status {cpp_work['status']} != expected")
            if name == "n1000000-r2":
                source_r2_text = source_text
            cases.append({
                "case": name, "n": n, "rounds": rounds,
                "old_report": file_rec(old_report_path),
                "source_ir": file_rec(source_ir),
                "cpp_ir": file_rec(cpp_ir),
                "source_selection": source_selection,
                "expected_verified": expected_verified,
                "source_work": compact_work(source_work),
                "cpp_work": compact_work(cpp_work),
                "passed": True,
            })
        report["cases"] = cases
        need(source_r2_text is not None, "n1000000-r2 Source IR is missing")

        probes = [probe_root_mutant(
            work_args, work_env, report, out,
            MAIN / "checks/sieve-large-independent-20261110/"
                   "source-r2-constant-false-backedge.ll",
            PINNED[1000000]["sum"])]
        probes.extend(probe_source_mutations(
            work_args, work_env, report, out, source_r2_text,
            PINNED[1000000]["sum"]))
        report["probes"] = probes
        need(all(row["rejected"] for row in probes),
             "one or more mandatory negative probes were not rejected")

        inputs_after = snapshot(correction_input_paths(work_args, old_root))
        tools_after = {name: file_rec(path)
                       for name, path in tool_paths(work_args).items()}
        report["tools_after"] = tools_after
        report["inputs_after"] = inputs_after
        report["tools_unchanged"] = tools_before == tools_after
        report["inputs_unchanged"] = inputs_before == inputs_after
        need(report["tools_unchanged"], "tool SHA-256 changed during recheck")
        need(report["inputs_unchanged"], "input/source/IR SHA changed during recheck")
        report["summary"] = {
            "cases_checked": len(cases),
            "r1_rejected_as_r2": sum(
                1 for row in cases if row["rounds"] == 1
                and not row["source_work"]["verified"]
                and not row["cpp_work"]["verified"]),
            "r2_verified": sum(
                1 for row in cases if row["rounds"] == 2
                and row["source_work"]["verified"]
                and row["cpp_work"]["verified"]),
            "source_guard_evidence_cases": sum(
                1 for row in cases
                if row["source_work"].get("guard_evidence") is not None),
            "mandatory_negatives": len(probes),
            "mandatory_negatives_rejected": sum(
                1 for row in probes if row["rejected"]),
            "root_mutant_quiet_exit_0": True,
            "public_entry_parse_validate": True,
            "tools_unchanged": True, "inputs_unchanged": True,
            "formal_timing": False,
        }
        need(report["summary"]["cases_checked"] == 4, "case count changed")
        need(report["summary"]["r1_rejected_as_r2"] == 2,
             "R1 rejection count changed")
        need(report["summary"]["r2_verified"] == 2,
             "R2 verified count changed")
        need(report["summary"]["source_guard_evidence_cases"] == 2,
             "Source guard evidence count changed")
        need(report["summary"]["mandatory_negatives"] >= 6
             and report["summary"]["mandatory_negatives_rejected"] ==
             report["summary"]["mandatory_negatives"],
             "negative probe count/rejection changed")
        report["limitations"] = [
            "This correction rechecks existing IR and saved outputs; it does not rebuild the million/ten-million Source AST or rerun normal programs.",
            "Source R2 automatic proof is limited to the observed sum-mismatch OR guard associated with a nonzero select/return in these exact inputs.",
            "C++ R2 remains based on the same two unrolled mark/stat regions and allocation/deallocation evidence; it is not a general proof for arbitrary IR or R.",
            "No timing, calibration, speedup, or <=2x claim is made.",
        ]
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["failure"] = {"type": type(error).__name__,
                             "message": str(error),
                             "traceback": traceback.format_exc()}
    finally:
        report["finished_at"] = now()
        common.write_json(out / "recheck.json", report)
        common._COMMAND_LOG = None
        print(json.dumps({
            "status": report["status"], "mode": "correction-recheck",
            "out": str(out), "report": str(out / "recheck.json"),
            "summary": report.get("summary"),
            "failure": report.get("failure"),
        }, indent=2, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


def limitations():
    return [
        "No timing, throughput, speedup, calibration, or <=2x claim; elapsed_ns is diagnostic only.",
        "The frozen flags are fully read and checked for 0/1 bytes, count, and sum; they are not regenerated or sampled.",
        "The programs print nothing. Correctness is count/sum exit 0; wrong count exits 1 and wrong sum exits 2 at R1.",
        "Full composite bytes are observed only for the correct O2 case through auxiliary observers; wrong variants are checked by exit code.",
        "Source observation uses a test-only weak Vec release hook linked with the formal program object. C++ observation uses an auxiliary source copy with one inserted observer call; neither is a formal performance program.",
        "R1 actual O2 IR is passed to the R2 analyzer and rejected. R2 uses actual O2 IR evidence and real one-round/no-kernel controls.",
        "No vector<bool>, new Vec factory, file loader, precomputed result, volatile/asm/noinline, fast-math, LTO, or -march=native is added.",
        "Only N=1,000,000/10,000,000, R=1/2, and the fixed x86_64 generic target are covered.",
    ]


def not_run():
    return [
        {"item": "formal_timing", "reason": "preparation only"},
        {"item": "wrong_count_wrong_sum_at_R2",
         "reason": "both wrong-answer controls are covered at R1 for each N"},
        {"item": "python_large_program_execution",
         "reason": "metadata helper not needed; helper JSON entry_symbol and pre-opt IR callee identify symbols"},
        {"item": "other_n_values", "reason": "fixed N=1,000,000/10,000,000"},
        {"item": "simd_or_multithreaded_variant", "reason": "single-thread scalar source algorithm"},
        {"item": "other_targets_or_compilers", "reason": "fixed x86_64 generic target and recorded LLVM toolchain"},
    ]


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, help="new output directory; must not exist")
    parser.add_argument("--n", type=int, choices=N_VALUES)
    parser.add_argument("--rounds", type=int)
    parser.add_argument("--recheck-existing", action="store_true",
                        help="recheck saved IR/results without rebuilding")
    parser.add_argument("--existing-root", type=Path,
                        default=WORKTREE / "checks/sieve-large-preparation-20261110")
    parser.add_argument("--oracle-dir", type=Path, default=DEFAULT_ORACLE_DIR)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--python-helper", type=Path,
                        default=DEFAULT_PYTHON_HELPER)
    parser.add_argument("--langc", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--compiler-root", type=Path, required=True)
    parser.add_argument("--ld-library-path", type=Path)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--run-timeout", type=float, default=180.0)
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
        ("observer source", OBSERVER_SOURCE),
    ):
        need(path.is_file(), f"{label} is missing: {path}")
    need(args.compiler_root.is_dir(),
         f"compiler-root is missing: {args.compiler_root}")
    if args.ld_library_path is not None:
        need(args.ld_library_path.is_dir(),
             f"--ld-library-path is not a directory: {args.ld_library_path}")
    if not args.recheck_existing:
        need(args.n in N_VALUES, f"--n must be one of {N_VALUES}")
        need(args.rounds in (1, 2), "--rounds must be 1 or 2")
    else:
        need(args.existing_root.is_dir(),
             f"--existing-root is missing: {args.existing_root}")
    need(args.timeout > 0 and args.run_timeout > 0,
         "timeouts must be positive")


def main(argv=None):
    try:
        args = parse_args(argv)
        validate_args(args)
        if args.recheck_existing:
            return recheck_existing(args)
        return prepare(args)
    except Exception as error:
        print(json.dumps({"status": "failed", "type": type(error).__name__,
                          "message": str(error),
                          "traceback": traceback.format_exc()},
                         indent=2, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    sys.exit(main())
