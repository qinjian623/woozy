#!/usr/bin/env python3
"""Prepare the fixed inclusive-sieve Source/C++ fixture without timing.

Both programs use one ``u8`` byte array of ``n + 1`` zeros, reset it each
round, mark composites from ``p * p``, and compare the prime count and u64 sum
with values from an independent Python trial-division oracle.  Correct and
wrong count/sum variants are built and run at O0/O2 in both languages.  Source
LLVM before/after IR, verifier runs, helper-object identity, function-scoped
optimized-IR checks, a mutated-IR negative control, and Source/C++ ELF checks
are recorded.  No timing, speedup, or ``<=2x`` conclusion is produced.
"""
from __future__ import annotations
import argparse
import json
import re
import sys
import traceback
from datetime import datetime, timezone
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_native as common  # noqa: E402
CONTRACT = "compiler-dev/1"
TASK = "native-sieve-preparation"
OPERATION_ID = "language-sieve-preparation-20261109"
PREPARE_SCHEMA = "compiler-dev/native-sieve-preparation/1"
ORACLE_SCHEMA = "compiler-dev/native-sieve-oracle/1"
PERFORMANCE_DIR = Path(__file__).resolve().parent
N_VALUES = (2, 3, 31, 100, 4095, 65535)
MAX_N = N_VALUES[-1]
ROUNDS = 8
TARGET_FLAGS = (
    "--target=x86_64-unknown-linux-gnu", "-march=x86-64", "-mtune=generic",
    "-fno-fast-math", "-fno-lto",
)
TEMPLATE_NAMES = ("sieve.lang.in", "reference.cpp.in")
PerfError = common.PerfError
MARKER_RE = re.compile(
    r"^//\s*PERF-SIEVE\s+n=(\d+)\s+rounds=(\d+)\s+count=(\d+)\s+sum=(\d+)\s*$",
    re.MULTILINE,
)
ZERO_LIST_RE = re.compile(
    r"var\s+composite\s+Vec\[u8\]\s*=\s*\[(.*?)\]\s*;", re.DOTALL
)
FUNC_HEAD_RE = re.compile(
    r"^define\b[^\n]*?@([A-Za-z0-9_.$]+)\([^\n]*\{\s*$", re.MULTILINE
)
FUNC_END_RE = re.compile(r"^\}\s*$", re.MULTILINE)
LOAD_I8_RE = re.compile(r"\bload\s+i8\b")
STORE_I8_1_RE = re.compile(r"\bstore\s+i8\s+1\b")
STORE_I8_0_RE = re.compile(r"\bstore\s+i8\s+0\b")
LABEL_RE = re.compile(r"^([A-Za-z0-9_.]+):")
LOOP_FUNC_RE = re.compile(r"^Loop info for function '([^']+)':\s*$")


def need(condition, message):
    if not condition:
        raise PerfError(message)


def now():
    return datetime.now(timezone.utc).isoformat()


def file_rec(path, base=None):
    path = Path(path).resolve()
    rec = {"path": str(path), "sha256": common.sha256_file(path),
           "size_bytes": path.stat().st_size}
    if base is not None:
        rec["rel"] = str(path.relative_to(Path(base).resolve()))
    return rec


def tool_paths(args):
    return {
        "langc": args.langc, "helper": args.helper, "runtime": args.runtime,
        "clang": args.llvm_bin / "clang",
        "clang++": args.llvm_bin / "clang++",
        "ld.lld": args.llvm_bin / "ld.lld", "opt": args.llvm_bin / "opt",
        "llvm-readelf": args.llvm_bin / "llvm-readelf",
        "llvm-nm": args.llvm_bin / "llvm-nm", "python": Path(sys.executable),
    }


def tool_snapshot(args):
    return {name: file_rec(path) for name, path in tool_paths(args).items()}


def render_zero_list(n):
    return ", ".join("0" for _ in range(n + 1))


def trial_division_primes(limit):
    """Independent oracle: no Sieve of Eratosthenes and no compiler output."""
    primes = []
    for candidate in range(2, limit + 1):
        prime, divisor = True, 2
        while divisor * divisor <= candidate:
            if candidate % divisor == 0:
                prime = False
                break
            divisor += 1
        if prime:
            primes.append(candidate)
    return primes


def manifest_text(name):
    return (
        "schema_version = 1\n\n[package]\n"
        f'name = "{name}"\nsource_root = "src"\nentry = "app"\n\n'
        '[build]\nbackend = "native"\nword_bits = 64\n'
    )


def render(template, n, expected_count, expected_sum, zeros):
    text = template
    for key, value in (
        ("{{N}}", str(n)), ("{{ZEROS}}", zeros), ("{{ROUNDS}}", str(ROUNDS)),
        ("{{EXPECTED_COUNT}}", str(expected_count)),
        ("{{EXPECTED_SUM}}", str(expected_sum)),
    ):
        text = text.replace(key, value)
    need("{{" not in text, "rendered template still has a placeholder")
    return text


def marker_values(text, label):
    match = MARKER_RE.search(text)
    need(match is not None, f"{label}: missing PERF-SIEVE marker")
    return tuple(int(match.group(index)) for index in range(1, 5))


def verify_generated(source, reference, n, expected_count, expected_sum, label):
    wanted = (n, ROUNDS, expected_count, expected_sum)
    need(marker_values(source, f"{label} Source") == wanted,
         f"{label}: Source PERF-SIEVE marker mismatch")
    need(marker_values(reference, f"{label} C++") == wanted,
         f"{label}: C++ PERF-SIEVE marker mismatch")
    zero_list = ZERO_LIST_RE.search(source)
    need(zero_list is not None, f"{label}: Source has no Vec[u8] zero list")
    items = [item.strip() for item in zero_list.group(1).split(",")]
    need(len(items) == n + 1 and all(item == "0" for item in items),
         f"{label}: Source zero list has {len(items)} entries, expected {n + 1}")
    pattern = r"\bexpected_{}\s*(?:u64\s*)?=\s*(?:UINT64_C\()?(\d+)\)?\s*;"
    for text, language in ((source, "Source"), (reference, "C++")):
        count = re.search(pattern.format("count"), text)
        total = re.search(pattern.format("sum"), text)
        need(count is not None and int(count.group(1)) == expected_count,
             f"{label}: {language} expected_count is missing or wrong")
        need(total is not None and int(total.group(1)) == expected_sum,
             f"{label}: {language} expected_sum is missing or wrong")
    need("Vec[u8]" in source and "bounds[0]" in source,
         f"{label}: Source is missing the u8 array or runtime bound read")
    need("std::vector<uint8_t>" in reference
         and "std::vector<bool>" not in reference,
         f"{label}: C++ must use std::vector<uint8_t>, not std::vector<bool>")
    need(re.search(rf"bounds\{{\s*{n}\s*\}}", reference) is not None,
         f"{label}: C++ runtime bound vector is missing or wrong")


def generate_case(out, templates, n, variant, expected_count, expected_sum):
    directory = out / "cases" / f"n{n}" / variant
    (directory / "src").mkdir(parents=True, exist_ok=False)
    zeros = render_zero_list(n)
    source_text = render(templates["sieve.lang.in"], n, expected_count,
                         expected_sum, zeros)
    reference_text = render(templates["reference.cpp.in"], n, expected_count,
                            expected_sum, zeros)
    label = f"n={n} {variant}"
    verify_generated(source_text, reference_text, n, expected_count,
                     expected_sum, label)
    source, reference = directory / "src" / "main.lang", directory / "reference.cpp"
    manifest = directory / "lang.toml"
    source.write_text(source_text, encoding="utf-8")
    reference.write_text(reference_text, encoding="utf-8")
    manifest.write_text(manifest_text(
        f"perf_sieve_n{n}_{variant.replace('-', '_')}"), encoding="utf-8")
    return {
        "n": n, "variant": variant, "expected_count": expected_count,
        "expected_sum": expected_sum,
        "expected_exit": {"correct": 0, "wrong_count": 1, "wrong_sum": 2}[variant],
        "manifest": file_rec(manifest, out), "source": file_rec(source, out),
        "reference": file_rec(reference, out), "check": None, "builds": [],
        "executions": [], "helper": [], "o2_work": None,
    }


def base_report(args, env):
    report = common.base_report("prepare", args, env)
    report.update({
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "formal_timing": False,
        "script": file_rec(Path(__file__).resolve()),
        "common_runner": file_rec(Path(common.__file__).resolve()),
        "templates": {name: file_rec(PERFORMANCE_DIR / name)
                      for name in TEMPLATE_NAMES},
    })
    return report


def tool(argv, args, env):
    return common.command(argv, timeout=args.timeout, env=env, cpu=args.cpu)


def run_program(argv, args, env):
    return common.command(argv, timeout=args.run_timeout, env=env, cpu=args.cpu)


def require_exit(record, expected, label):
    if (record["status"] != "exited" or record["exit"] != expected
            or record["stdout"] or record["stderr"]):
        detail = record.get("error") or record.get("stderr", "")[-4000:]
        raise PerfError(
            f"{label}: expected quiet exit {expected}, got status="
            f"{record['status']} exit={record['exit']} "
            f"stdout={record['stdout'][-1000:]!r} "
            f"stderr={record['stderr'][-1000:]!r}: {detail}")


def verify_ir(path, args, env, report, label):
    result = tool([args.llvm_bin / "opt", "-passes=verify",
                   "-disable-output", path], args, env)
    row = {"path": str(path), "sha256": common.sha256_file(path),
           "command_index": result["command_index"], "success": False}
    report["verifies"].append(row)
    common.require_success(result, label)
    row["success"] = True
    return row


def build_langc(args, env, out, manifest, out_dir, opt, label):
    out_dir.mkdir(parents=True, exist_ok=False)
    build = tool([args.langc, "build", "--project", manifest, "--backend",
                  "native", "--opt", str(opt), "--out-dir", out_dir,
                  "--format", "json"], args, env)
    common.require_success(build, f"{label} build")
    build_json = common.parse_json_stdout(build, f"{label} build")
    need(build_json.get("status") == "ok", f"{label}: build status is not ok")
    program, obj = out_dir / "program", out_dir / "program.o"
    return {
        "implementation": "langc", "opt": opt,
        "command_index": build["command_index"], "json": build_json,
        "program": file_rec(program, out), "object": file_rec(obj, out),
        "elf": common.checked_elf(program, executable=True, label=label),
        "object_elf": common.checked_elf(
            obj, executable=False, label=f"{label} object"),
    }


def build_cpp(args, env, out, reference, program, opt, label):
    program.parent.mkdir(parents=True, exist_ok=True)
    command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         f"--ld-path={args.llvm_bin / 'ld.lld'}", reference, "-o", program],
        args, env)
    common.require_success(command, f"{label} build")
    return {"implementation": "cpp", "opt": opt,
            "command_index": command["command_index"],
            "program": file_rec(program, out),
            "elf": common.checked_elf(program, executable=True, label=label)}


def helper_export(args, env, report, out, manifest, out_dir, opt,
                  expected_object, label):
    out_dir.mkdir(parents=True, exist_ok=False)
    obj, after, before = out_dir / "program.o", out_dir / "after.ll", out_dir / "before.ll"
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
    object_match = Path(expected_object).read_bytes() == obj.read_bytes()
    need(object_match, f"{label}: formal program.o and helper object differ")
    return {
        "opt": opt, "command_index": helper["command_index"],
        "json": helper_json, "object": file_rec(obj, out),
        "object_match": object_match, "before": file_rec(before, out),
        "after": file_rec(after, out),
        "verify_before": verify_ir(before, args, env, report,
                                   f"{label} before verify"),
        "verify_after": verify_ir(after, args, env, report,
                                  f"{label} after verify"),
    }


def parse_functions(text):
    functions = {}
    for match in FUNC_HEAD_RE.finditer(text):
        end = FUNC_END_RE.search(text, match.end())
        need(end is not None, f"unterminated LLVM function {match.group(1)}")
        functions[match.group(1)] = {
            "body_start": match.end(),
            "body": text[match.end():end.start()],
        }
    need(functions, "no LLVM function definitions found")
    return functions


def marking_function(text):
    """Find the one function with the real `store i8 1` marking write."""
    functions = parse_functions(text)
    names = [name for name, info in functions.items()
             if STORE_I8_1_RE.search(info["body"])]
    need(len(names) == 1,
         f"expected exactly one function with store i8 1, found {names}")
    name, info = names[0], functions[names[0]]
    body, store = info["body"], STORE_I8_1_RE.search(info["body"])
    load_count = len(LOAD_I8_RE.findall(body))
    need(load_count > 0, f"{name}: no load i8 in the marking function")
    label = None
    for line in reversed(body[:store.start()].splitlines()):
        match = LABEL_RE.match(line.strip())
        if match:
            label = match.group(1)
            break
    need(label is not None, f"{name}: cannot identify the marking store block")
    return {
        "target": name, "target_is_main": name == "main",
        "load_i8": load_count,
        "store_i8_1": len(STORE_I8_1_RE.findall(body)),
        "store_i8_0": len(STORE_I8_0_RE.findall(body)),
        "store_block": label,
        "store_span": (info["body_start"] + store.start(),
                       info["body_start"] + store.end()),
    }


def parse_loop_sections(text):
    sections, current = {}, None
    for line in text.splitlines():
        match = LOOP_FUNC_RE.match(line)
        if match:
            current = match.group(1)
            sections.setdefault(current, [])
        elif current is not None and "Loop at depth" in line:
            sections[current].append(line)
    return sections


def o2_work(args, env, report, out, path, label):
    text = Path(path).read_text(encoding="utf-8")
    mark = marking_function(text)
    loops = tool([args.llvm_bin / "opt", "-passes=print<loops>",
                  "-disable-output", path], args, env)
    row = {"path": str(path), "command_index": loops["command_index"],
           "success": False}
    report["loop_analyses"].append(row)
    common.require_success(loops, f"{label} loop analysis")
    target_loops = parse_loop_sections(loops["stderr"]).get(mark["target"], [])
    row.update({
        "target": mark["target"], "target_is_main": mark["target_is_main"],
        "target_loop_count": len(target_loops),
        "target_loop_lines": target_loops,
        "loop_count": loops["stderr"].count("Loop at depth"),
        "loop_output": loops["stderr"], "load_i8": mark["load_i8"],
        "store_i8_1": mark["store_i8_1"], "store_i8_0": mark["store_i8_0"],
        "store_block": mark["store_block"],
        "store_in_loop": any("%" + mark["store_block"] in line
                             for line in target_loops),
        "file": file_rec(path, out),
    })
    need(row["target_loop_count"] >= 4,
         f"{label}: target {mark['target']} has only "
         f"{row['target_loop_count']} loops")
    need(row["store_in_loop"],
         f"{label}: marking store block {mark['store_block']} is not in a loop")
    need(row["load_i8"] > 0 and row["store_i8_1"] > 0,
         f"{label}: missing load i8 or store i8 1 in {mark['target']}")
    row["success"] = True
    return row


def negative_control(args, env, report, out, after_path, label):
    text = Path(after_path).read_text(encoding="utf-8")
    start, end = marking_function(text)["store_span"]
    mutated = text[:start] + "store i8 0" + text[end:]
    path = out / "negative" / "store-i8-1-to-0.ll"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(mutated, encoding="utf-8")
    verify = tool([args.llvm_bin / "opt", "-passes=verify",
                   "-disable-output", path], args, env)
    row = {"path": str(path), "original": file_rec(after_path, out),
           "mutated": file_rec(path, out),
           "command_index": verify["command_index"], "verify_success": False,
           "rejected": False, "program_executed": False}
    report["negative_controls"].append(row)
    common.require_success(verify, f"{label} negative IR verify")
    row["verify_success"] = True
    try:
        marking_function(mutated)
    except PerfError as error:
        row.update(rejected=True, rejection=str(error))
    need(row["rejected"],
         f"{label}: mutated store i8 1 to i8 0 was not rejected")
    return row


def parse_undefined(text):
    strong, weak = [], []
    for line in text.splitlines():
        match = re.match(r"^\s*([A-Za-z])\s+(\S+)\s*$", line)
        if match is None:
            continue
        kind, symbol = match.group(1), match.group(2)
        if kind == "U":
            strong.append(symbol)
        elif kind in ("w", "v"):
            weak.append(symbol)
    return strong, weak


def elf_checks(args, env, report, path, label, implementation):
    read_l = tool([args.llvm_bin / "llvm-readelf", "-l", path], args, env)
    read_d = tool([args.llvm_bin / "llvm-readelf", "-d", path], args, env)
    nm_u = tool([args.llvm_bin / "llvm-nm", "-u", path], args, env)
    row = {
        "path": str(path), "implementation": implementation,
        "commands": {"readelf_l": read_l["command_index"],
                     "readelf_d": read_d["command_index"],
                     "nm_u": nm_u["command_index"]},
        "success": False, "executed": True,
    }
    report["elf_checks"].append(row)
    common.require_success(read_l, f"{label} readelf -l")
    common.require_success(read_d, f"{label} readelf -d")
    common.require_success(nm_u, f"{label} nm -u")
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]",
                        read_d["stdout"])
    strong, weak = parse_undefined(nm_u["stdout"])
    interp = re.search(r"Requesting program interpreter: ([^\]]+)",
                       read_l["stdout"])
    row.update({
        "has_interp": interp is not None,
        "interp": interp.group(1) if interp else None,
        "needed": needed, "strong_undefined": strong, "weak_undefined": weak,
        "no_python": not any("python" in lib.lower() for lib in needed),
    })
    need(row["no_python"], f"{label}: ELF NEEDED contains Python: {needed}")
    if implementation == "langc":
        need(not row["has_interp"],
             f"{label}: Source ELF has INTERP {row['interp']}")
        need(not needed, f"{label}: Source ELF has NEEDED {needed}")
        need(not strong,
             f"{label}: Source ELF has strong undefined symbols {strong}")
        row["source_static_ok"] = True
    else:
        need(row["has_interp"], f"{label}: C++ ELF lacks INTERP")
        need(needed, f"{label}: C++ ELF lacks dynamic NEEDED entries")
        row["cpp_dynamic_ok"] = True
    row["success"] = True
    return row


def execute(args, env, case, implementation, opt, program, label):
    run = run_program([program], args, env)
    row = {"implementation": implementation, "opt": opt,
           "command_index": run["command_index"],
           "expected_exit": case["expected_exit"], "success": False}
    case["executions"].append(row)
    require_exit(run, case["expected_exit"], f"{label} run")
    row.update(success=True, exit=run["exit"], stdout=run["stdout"],
               stderr=run["stderr"])
    return row


def input_snapshot(report, oracle_path):
    records = {rec["path"]: rec["sha256"] for rec in report["templates"].values()}
    records[str(Path(oracle_path).resolve())] = common.sha256_file(oracle_path)
    for case in report["cases"]:
        for key in ("source", "reference", "manifest"):
            records[case[key]["path"]] = case[key]["sha256"]
    for path in (Path(__file__).resolve(), Path(common.__file__).resolve()):
        records[str(path)] = common.sha256_file(path)
    return records


def repo_signature(record):
    return {"head": record["head"], "dirty": record["dirty"],
            "status_sha256": record["status_sha256"]}


def write_patch(out, args, env):
    worktree = PERFORMANCE_DIR.parents[2]
    names = ("sieve.lang.in", "reference.cpp.in", "prepare.py", "README.md")
    parts = []
    for name in names:
        relative = f"tests/performance/sieve/{name}"
        result = common.command(
            ["git", "-C", str(worktree), "diff", "--no-index", "--binary",
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
    # Keep the new run directory visible to `git status` before the snapshot.
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
        worktree = PERFORMANCE_DIR.parents[2]
        root = Path(args.compiler_root).resolve()
        for label, path in (("langc", args.langc), ("helper", args.helper),
                            ("runtime", args.runtime)):
            need(root in Path(path).resolve().parents,
                 f"{label} is not under --compiler-root {root}: {path}")
        report["tools_under_compiler_root"] = True
        report["compiler_root_is_worktree"] = root == worktree
        cache_path = Path(report["build_tree"]["cmake_cache"]["path"])
        build_testing = re.search(r"^BUILD_TESTING:BOOL=(.*)$",
                                  cache_path.read_text(encoding="utf-8"),
                                  re.MULTILINE)
        report["build_tree"]["build_testing"] = (
            build_testing.group(1).strip() if build_testing else None)
        need(report["build_tree"]["build_testing"] == "OFF",
             f"{cache_path}: BUILD_TESTING is not OFF")
        report["parameters"] = {
            "n_values": list(N_VALUES), "rounds": ROUNDS,
            "variant_expected_exits": {"correct": 0, "wrong_count": 1,
                                       "wrong_sum": 2},
            "target_flags": list(TARGET_FLAGS),
        }
        report["tools_before"] = tool_snapshot(args)
        templates = {name: (PERFORMANCE_DIR / name).read_text(encoding="utf-8")
                     for name in TEMPLATE_NAMES}
        primes = trial_division_primes(MAX_N)
        oracle_cases = []
        for n in N_VALUES:
            selected = [prime for prime in primes if prime <= n]
            oracle_cases.append({
                "n": n, "count": len(selected), "sum": sum(selected),
                "first_primes": selected[:10], "last_primes": selected[-5:],
            })
        by_n = {row["n"]: row for row in oracle_cases}
        oracle_path = out / "oracle.json"
        common.write_json(oracle_path, {
            "schema": ORACLE_SCHEMA, "method": "trial_division",
            "max_n": MAX_N, "primes": primes, "cases": oracle_cases,
        })
        report["oracle"] = {
            "method": "trial_division", "file": file_rec(oracle_path, out),
            "cases": {str(row["n"]): row for row in oracle_cases},
        }
        for n in N_VALUES:
            correct = by_n[n]
            for variant in ("correct", "wrong_count", "wrong_sum"):
                count, total = correct["count"], correct["sum"]
                if variant == "wrong_count":
                    count += 1
                elif variant == "wrong_sum":
                    total += 1
                report["cases"].append(generate_case(
                    out, templates, n, variant, count, total))
        report["inputs_before"] = input_snapshot(report, oracle_path)
        for case in report["cases"]:
            n, variant = case["n"], case["variant"]
            manifest = Path(case["manifest"]["path"])
            reference = Path(case["reference"]["path"])
            if variant == "correct":
                check = tool([args.langc, "check", "--project", manifest,
                              "--backend", "native", "--format", "json"],
                             args, env)
                common.require_success(check, f"n={n} correct langc check")
                check_json = common.parse_json_stdout(
                    check, f"n={n} correct langc check")
                need(check_json.get("status") == "ok",
                     f"n={n}: langc check status is not ok")
                case["check"] = {"command_index": check["command_index"],
                                 "json": check_json, "success": True}
            for opt in (0, 2):
                label = f"n={n} {variant} langc O{opt}"
                langc_row = build_langc(
                    args, env, out, manifest,
                    out / "build" / f"n{n}" / variant / "langc" / f"O{opt}",
                    opt, label)
                case["builds"].append(langc_row)
                execute(args, env, case, "langc", opt,
                        langc_row["program"]["path"], label)
                elf_checks(args, env, report, langc_row["program"]["path"],
                           label, "langc")
                label = f"n={n} {variant} cpp O{opt}"
                cpp_row = build_cpp(
                    args, env, out, reference,
                    out / "build" / f"n{n}" / variant / "cpp" / f"O{opt}" /
                    "reference", opt, label)
                case["builds"].append(cpp_row)
                execute(args, env, case, "cpp", opt,
                        cpp_row["program"]["path"], label)
                elf_checks(args, env, report, cpp_row["program"]["path"],
                           label, "cpp")
                if variant == "correct":
                    helper = helper_export(
                        args, env, report, out, manifest,
                        out / "llvm" / f"n{n}" / f"O{opt}", opt,
                        langc_row["object"]["path"],
                        f"n={n} Source O{opt} LLVM")
                    case["helper"].append(helper)
                    if opt == 2:
                        case["o2_work"] = o2_work(
                            args, env, report, out, helper["after"]["path"],
                            f"n={n} O2")
        nc_case = next(case for case in report["cases"]
                       if case["n"] == 31 and case["variant"] == "correct")
        negative_control(args, env, report, out,
                         nc_case["o2_work"]["file"]["path"], "n=31 O2")
        report["tools_after"] = tool_snapshot(args)
        report["inputs_after"] = input_snapshot(report, oracle_path)
        report["tools_unchanged"] = report["tools_before"] == report["tools_after"]
        report["inputs_unchanged"] = (
            report["inputs_before"] == report["inputs_after"])
        need(report["tools_unchanged"],
             "tool SHA-256 values changed during prepare")
        need(report["inputs_unchanged"],
             "input SHA-256 values changed during prepare")
        repositories_after = {
            "worktree": common.repo_info(worktree, env, args.cpu),
            "compiler_root": common.repo_info(args.compiler_root, env, args.cpu),
        }
        report["repositories_after"] = repositories_after
        old = report["repositories"]
        report["repositories_unchanged"] = all(
            repo_signature(old[name]) == repo_signature(repositories_after[name])
            for name in ("worktree", "compiler_root"))
        need(report["repositories_unchanged"],
             "repository HEAD/status changed during prepare")
        cases = report["cases"]
        builds = [row for case in cases for row in case["builds"]]
        executions = [row for case in cases for row in case["executions"]]
        helpers = [row for case in cases for row in case["helper"]]
        works = [case["o2_work"] for case in cases if case["o2_work"]]
        elf_rows = report["elf_checks"]
        quiet = {
            code: sum(1 for row in executions if row["success"]
                      and row["exit"] == code and not row["stdout"]
                      and not row["stderr"])
            for code in (0, 1, 2)
        }
        summary = {
            "cases": len(cases), "builds": len(builds),
            "executions": len(executions),
            "quiet_exit_0": quiet[0], "quiet_exit_1": quiet[1],
            "quiet_exit_2": quiet[2],
            "source_llvm_exports": len(helpers),
            "source_ir_verifies": len(report["verifies"]),
            "source_object_matches": sum(1 for row in helpers if row["object_match"]),
            "o2_target_loop_checks": len(works),
            "o2_target_loop_checks_passed": sum(1 for row in works if row["success"]),
            "source_elf_static_checks": sum(
                1 for row in elf_rows if row["implementation"] == "langc"),
            "source_elf_static_checks_passed": sum(
                1 for row in elf_rows if row["implementation"] == "langc"
                and row.get("source_static_ok")),
            "cpp_dynamic_elf_checks": sum(
                1 for row in elf_rows if row["implementation"] == "cpp"),
            "cpp_dynamic_elf_checks_passed": sum(
                1 for row in elf_rows if row["implementation"] == "cpp"
                and row.get("cpp_dynamic_ok")),
            "negative_control_rejected": (
                report["negative_controls"][0]["rejected"]
                if report["negative_controls"] else False),
            "tools_unchanged": report["tools_unchanged"],
            "inputs_unchanged": report["inputs_unchanged"],
            "repositories_unchanged": report["repositories_unchanged"],
            "timing_performed": False,
        }
        expected = {
            "cases": 18, "builds": 72, "executions": 72,
            "quiet_exit_0": 24, "quiet_exit_1": 24, "quiet_exit_2": 24,
            "source_llvm_exports": 12, "source_ir_verifies": 24,
            "source_object_matches": 12, "o2_target_loop_checks": 6,
            "o2_target_loop_checks_passed": 6, "source_elf_static_checks": 36,
            "source_elf_static_checks_passed": 36,
            "cpp_dynamic_elf_checks": 36, "cpp_dynamic_elf_checks_passed": 36,
            "negative_control_rejected": True, "tools_unchanged": True,
            "inputs_unchanged": True, "repositories_unchanged": True,
            "timing_performed": False,
        }
        for key, value in expected.items():
            need(summary[key] == value,
                 f"summary[{key}]={summary[key]}, expected {value}")
        report["summary"] = summary
        report["deliverable"] = {
            "worktree": str(worktree),
            "worktree_head": report["repositories"]["worktree"]["head"],
            "files": [file_rec(PERFORMANCE_DIR / name)
                      for name in ("sieve.lang.in", "reference.cpp.in",
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
            "summary": report.get("summary"), "failure": report.get("failure"),
        }, indent=2, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


def limitations():
    return [
        "No timing, throughput, speedup, or <=2x claim; command elapsed_ns is "
        "diagnostic only.",
        "The programs print nothing; correctness is the quiet exit code.",
        "The Source fixture uses a rendered n+1 zero list and a one-element "
        "runtime bound Vec; no new stdlib API is introduced.",
        "O2 checks are static Source LLVM IR evidence; the C++ optimizer may "
        "fold the same bound read.",
        "Only fixed N and x86_64 generic target; no SIMD, threads, LTO, "
        "fast-math, or -march=native.",
    ]


def not_run():
    return [
        {"item": "formal_timing", "reason": "preparation only"},
        {"item": "cpp_llvm_ir_export",
         "reason": "C++ is checked by actual O0/O2 builds and quiet runs; "
                   "only Source LLVM before/after is retained"},
        {"item": "other_n_values",
         "reason": "fixed N=2,3,31,100,4095,65535"},
        {"item": "other_targets_or_compilers",
         "reason": "fixed x86_64 generic target and recorded LLVM 22.1.8"},
        {"item": "performance_counters_or_cold_cache",
         "reason": "no perf counters, cache flush, governor change, or "
                   "wall-clock comparison"},
    ]


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, help="new output directory; must not exist")
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
    if args.timeout <= 0 or args.run_timeout <= 0:
        raise PerfError("timeouts must be positive")


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
