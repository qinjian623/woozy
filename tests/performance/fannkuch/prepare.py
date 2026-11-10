#!/usr/bin/env python3
"""Prepare the fixed Fannkuch-redux Source/C++ fixture without timing.

Reuses ``run_native.py`` for commands, hashing, ELF checks, build-tree/tool
validation, host records, and report writing.  The Source Python observer is the
unmodified generated module: it imports ``program.py``, finds ``visit``/``main``
through ``metadata.json``, records the actual visit arguments, and then executes
the original empty visit and complete main.

For n=3..7 this script checks Source/C++ O0/O2 correctness, wrong-checksum and
wrong-maximum controls, real generated-Source traces, two Source negative
controls, Source LLVM verifier/object identity with symbol-ID-based visit-call
recognition, and C++ mangled-symbol visit-call recognition.  It records no
timing and makes no performance claim.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import traceback
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_native as common  # noqa: E402

CONTRACT = "compiler-dev/1"
TASK = "native-fannkuch-redux-preparation"
OPERATION_ID = "language-classic-fannkuch-prepare-20261109"
PREPARE_SCHEMA = "compiler-dev/native-fannkuch-preparation/1"
PERFORMANCE_DIR = Path(__file__).resolve().parent
DEFAULT_ORACLE_DIR = Path(
    "/home/qin/Work/language-dev-20261103/checks/fannkuch-oracle-20261109"
)
DEFAULT_PYTHON_HELPER = Path(
    "/home/qin/Work/language-dev-20261103/compiler/build/native-cli-release/"
    "tests/python/lang_python_emit"
)
N_VALUES = (3, 4, 5, 6, 7)
TARGET_FLAGS = (
    "--target=x86_64-unknown-linux-gnu",
    "-march=x86-64",
    "-mtune=generic",
    "-fno-fast-math",
    "-fno-lto",
)
PerfError = common.PerfError


def need(condition, message):
    if not condition:
        raise PerfError(message)


def now():
    return datetime.now(timezone.utc).isoformat()


def file_rec(path):
    path = Path(path).resolve()
    return {
        "path": str(path),
        "sha256": common.sha256_file(path),
        "size_bytes": path.stat().st_size,
    }


def sha_map(paths, base):
    base = Path(base).resolve()
    return {
        str(Path(path).resolve().relative_to(base)): common.sha256_file(path)
        for path in paths
    }


def template_records():
    return {
        name: file_rec(PERFORMANCE_DIR / name)
        for name in ("fannkuch.wz.in", "reference.cpp.in", "visit_trace.py")
    }


def read_templates():
    return {
        name: (PERFORMANCE_DIR / name).read_text(encoding="utf-8")
        for name in ("fannkuch.wz.in", "reference.cpp.in")
    }


def render(template, n, checksum, maximum, visit_values, visit_flips):
    zeros = ", ".join("0" for _ in range(n))
    text = template
    for key, value in (
        ("{{N}}", str(n)),
        ("{{CHECKSUM}}", str(checksum)),
        ("{{MAXIMUM}}", str(maximum)),
        ("{{ZEROS}}", zeros),
        ("{{VISIT_VALUES}}", visit_values),
        ("{{VISIT_FLIPS}}", visit_flips),
    ):
        text = text.replace(key, value)
    need("{{" not in text, "rendered template still has a placeholder")
    return text


def manifest_text(name):
    return (
        "schema_version = 1\n\n[package]\n"
        f'name = "{name}"\nsource_root = "src"\nentry = "app"\n\n'
        '[build]\nbackend = "native"\nword_bits = 64\n'
    )


def generate_case(out, templates, n, expected, variant, visit_values, visit_flips):
    checksum, maximum = expected
    if variant == "wrong-checksum":
        checksum += 1
    elif variant == "wrong-maximum":
        maximum += 1
    directory = out / "cases" / f"n{n}" / variant
    (directory / "src").mkdir(parents=True, exist_ok=False)
    source = directory / "src" / "main.wz"
    reference = directory / "reference.cpp"
    manifest = directory / "lang.toml"
    source.write_text(
        render(templates["fannkuch.wz.in"], n, checksum, maximum,
               visit_values, visit_flips), encoding="utf-8"
    )
    reference.write_text(
        render(templates["reference.cpp.in"], n, checksum, maximum,
               visit_values, visit_flips), encoding="utf-8"
    )
    manifest.write_text(manifest_text(f"perf_fannkuch_n{n}_{variant.replace('-', '_')}"), encoding="utf-8")
    return {
        "variant": variant,
        "n": n,
        "expected_checksum": checksum,
        "expected_maximum": maximum,
        "manifest": file_rec(manifest),
        "source": file_rec(source),
        "reference": file_rec(reference),
    }


def load_oracle(path):
    path = Path(path).resolve()
    results_path = path / "results.json"
    inputs_path = path / "inputs.json"
    need(results_path.is_file(), f"missing frozen oracle results: {results_path}")
    need(inputs_path.is_file(), f"missing frozen oracle inputs: {inputs_path}")
    data = json.loads(results_path.read_text(encoding="utf-8"))
    need(data.get("status") == "passed", f"{results_path}: oracle status is not passed")
    need(data.get("timing_performed") is False, f"{results_path}: oracle contains timing")
    inputs = json.loads(inputs_path.read_text(encoding="utf-8"))
    need(inputs.get("n_values") == list(N_VALUES),
         f"{inputs_path}: n values are not {list(N_VALUES)}")
    by_n = {int(row["n"]): row for row in data.get("cases", [])}
    need(tuple(sorted(by_n)) == N_VALUES,
         f"{results_path}: oracle n values are {sorted(by_n)}")
    files = [results_path, inputs_path]
    expected, traces = {}, {}
    for n in N_VALUES:
        row = by_n[n]
        checksum, maximum = int(row["checksum"]), int(row["max_flips"])
        need(int(row["permutation_count"]) == math.factorial(n),
             f"n={n}: oracle permutation count is wrong")
        trace_path = path / "traces" / f"n{n}.txt"
        output_path = path / "expected-output" / f"n{n}.txt"
        for target in (trace_path, output_path):
            need(target.is_file(), f"missing frozen oracle file: {target}")
            files.append(target)
        need(common.sha256_file(trace_path) == row["trace_sha256"],
             f"{trace_path}: SHA-256 does not match results.json")
        need(common.sha256_file(output_path) == row["expected_stdout_sha256"],
             f"{output_path}: SHA-256 does not match results.json")
        need(output_path.read_text(encoding="utf-8") ==
             f"{checksum}\nPfannkuchen({n}) = {maximum}\n",
             f"{output_path}: expected stdout text changed")
        expected[n], traces[n] = (checksum, maximum), trace_path
    return expected, traces, files


def base_report(args, env):
    report = common.base_report("prepare", args, env)
    runner = report["script"]
    report.update({
        "schema": PREPARE_SCHEMA,
        "task": TASK,
        "contract": CONTRACT,
        "operation_id": OPERATION_ID,
        "formal_timing": False,
        "script": file_rec(Path(__file__).resolve()),
        "common_runner": runner,
        "templates": template_records(),
    })
    report["target"].update({
        "cpp_flags": list(TARGET_FLAGS),
        "cpp_linker": str(args.llvm_bin / "ld.lld"),
    })
    report["tools"]["python_helper"] = common.tool_record(
        args.python_helper, env, args.cpu, None
    )
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
            f"{label}: expected quiet exit {expected}, got status={record['status']} "
            f"exit={record['exit']}: {detail}"
        )


def verify_ir(path, args, env, report, label):
    result = tool([args.llvm_bin / "opt", "-passes=verify", "-disable-output", path],
                  args, env)
    row = {"path": str(path), "sha256": common.sha256_file(path),
           "command_index": result["command_index"], "success": False}
    report["verifies"].append(row)
    common.require_success(result, label)
    row["success"] = True
    return row


def build_langc(args, env, manifest, out_dir, opt, label):
    out_dir.mkdir(parents=True, exist_ok=False)
    build = tool(
        [args.langc, "build", "--project", manifest, "--backend", "native",
         "--opt", str(opt), "--out-dir", out_dir, "--format", "json"],
        args, env,
    )
    common.require_success(build, f"{label} build")
    build_json = common.parse_json_stdout(build, f"{label} build")
    need(build_json.get("status") == "ok", f"{label} build status is not ok")
    program, obj = out_dir / "program", out_dir / "program.o"
    return {
        "implementation": "langc", "opt": opt, "command_index": build["command_index"],
        "program": file_rec(program), "object": file_rec(obj),
        "elf": common.checked_elf(program, executable=True, label=label),
        "object_elf": common.checked_elf(obj, executable=False, label=f"{label} object"),
    }


def build_cpp(args, env, reference, program, opt, label):
    program.parent.mkdir(parents=True, exist_ok=True)
    command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         f"--ld-path={args.llvm_bin / 'ld.lld'}", reference, "-o", program],
        args, env,
    )
    common.require_success(command, f"{label} build")
    return {
        "implementation": "cpp", "opt": opt, "command_index": command["command_index"],
        "program": file_rec(program),
        "elf": common.checked_elf(program, executable=True, label=label),
    }


def source_call_count(text, symbol):
    return len(re.findall(
        rf"\b(?:call|invoke)\b[^\n]*@lang\.fn\.{int(symbol)}\b", text
    ))


def export_source_llvm(args, env, report, manifest, out_dir, opt, expected_object, symbol, label):
    out_dir.mkdir(parents=True, exist_ok=False)
    object_path, after_path, before_path = (
        out_dir / "program.o", out_dir / "after.ll", out_dir / "before.ll"
    )
    helper = tool(
        [args.helper, "--project", manifest, "--opt", str(opt),
         "--out-object", object_path, "--out-llvm", after_path,
         "--out-llvm-before", before_path,
         "--out-native", out_dir / "program.nir", "--format", "json"],
        args, env,
    )
    common.require_success(helper, f"{label} helper")
    helper_json = common.parse_json_stdout(helper, f"{label} helper")
    need(helper_json.get("status") in ("ok", "passed") and
         helper_json.get("phase") == "object" and
         helper_json.get("llvm_verified_before") and
         helper_json.get("llvm_verified_after"),
         f"{label} helper JSON is incomplete: {helper_json}")
    common.checked_elf(object_path, executable=False, label=f"{label} object")
    before_text = before_path.read_text(encoding="utf-8")
    after_text = after_path.read_text(encoding="utf-8")
    nir_text = (out_dir / "program.nir").read_text(encoding="utf-8")
    need(re.search(rf"^function @{int(symbol)}\b", nir_text, re.MULTILINE) is not None,
         f"{label}: Source visit symbol {symbol} is not a NativeIR function")
    need(re.search(rf"\bdefine\b[^\n]*@lang\.fn\.{int(symbol)}\(", before_text),
         f"{label}: Source visit symbol is not defined in before.ll")
    before_count = source_call_count(before_text, symbol)
    after_count = source_call_count(after_text, symbol)
    if opt == 0:
        need(before_count > 0 and after_count > 0,
             f"{label}: O0 positive visit-call control failed "
             f"(before={before_count}, after={after_count})")
    else:
        need(before_count > 0, f"{label}: O2 target call was not present before optimization")
        need(after_count == 0,
             f"{label}: O2 still has {after_count} actual visit call(s)")
    row = {
        "opt": opt, "command_index": helper["command_index"], "json": helper_json,
        "visit_symbol": int(symbol), "visit_calls_before": before_count,
        "visit_calls_after": after_count, "object": file_rec(object_path),
        "object_match": Path(expected_object).read_bytes() == object_path.read_bytes(),
        "verify_before": verify_ir(before_path, args, env, report, f"{label} before verify"),
        "verify_after": verify_ir(after_path, args, env, report, f"{label} after verify"),
    }
    need(row["object_match"], f"{label}: formal program.o and helper object differ")
    return row


def emit_python(args, env, manifest, out_dir, label):
    out_dir.mkdir(parents=True, exist_ok=False)
    command = tool(
        [args.python_helper, "--project", manifest, "--out-dir", out_dir],
        args, env,
    )
    common.require_success(command, f"{label} Python emit")
    result = common.parse_json_stdout(command, f"{label} Python emit")
    need(result.get("status") == "ok", f"{label} Python emit status is not ok")
    metadata = json.loads((out_dir / "metadata.json").read_text(encoding="utf-8"))
    visits = [
        row for row in metadata["symbols"]
        if row["source_name"] == "visit" and row["generated"].startswith("_f")
    ]
    need(len(visits) == 1, f"{label}: expected one generated visit symbol")
    entry = int(metadata["entry_symbol"])
    entry_row = next((row for row in metadata["symbols"]
                      if int(row["symbol"]) == entry), None)
    need(entry_row is not None and entry_row["source_name"] == "main",
         f"{label}: metadata entry is not main")
    return {
        "command_index": command["command_index"], "json": result,
        "directory": str(out_dir), "metadata": file_rec(out_dir / "metadata.json"),
        "visit_symbol": int(visits[0]["symbol"]),
        "visit_generated": visits[0]["generated"],
        "entry_symbol": entry, "entry_generated": entry_row["generated"],
        "files": {name: file_rec(out_dir / name)
                  for name in ("program.py", "langrt.py", "verified.hir", "metadata.json")},
    }


def first_mismatch(actual, frozen):
    actual_lines, frozen_lines = actual.splitlines(), frozen.splitlines()
    for number, (left, right) in enumerate(zip(actual_lines, frozen_lines), 1):
        if left == right:
            continue
        left_fields, right_fields = left.split(":"), right.split(":")
        field = "line"
        if len(left_fields) == len(right_fields) == 3:
            for index, name in enumerate(("index", "permutation", "flips")):
                if left_fields[index] != right_fields[index]:
                    field = name
                    break
        return {"line": number, "field": field, "actual": left, "frozen": right}
    if len(actual_lines) != len(frozen_lines):
        return {"line": min(len(actual_lines), len(frozen_lines)) + 1,
                "field": "line_count", "actual_lines": len(actual_lines),
                "frozen_lines": len(frozen_lines)}
    return None


def run_observer(args, env, generated, frozen_path, output, expected_match, label):
    run = tool(
        [args.python, "-B", "-s", PERFORMANCE_DIR / "visit_trace.py",
         "--generated-dir", generated],
        args, env,
    )
    common.require_success(run, f"{label} observer")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(run["stdout"], encoding="utf-8")
    frozen = Path(frozen_path).read_text(encoding="utf-8")
    mismatch = first_mismatch(run["stdout"], frozen)
    row = {
        "command_index": run["command_index"], "actual": file_rec(output),
        "frozen": file_rec(frozen_path), "lines": len(run["stdout"].splitlines()),
        "exact_text_match": run["stdout"] == frozen, "first_mismatch": mismatch,
        "expected_match": expected_match,
    }
    if expected_match:
        need(row["exact_text_match"], f"{label}: observer trace differs: {mismatch}")
    else:
        need(not row["exact_text_match"] and mismatch is not None,
             f"{label}: observer did not reject the negative control")
    return row


def cpp_visit_linkage(ir_text, label):
    matches = set(re.findall(
        r'!DISubprogram\(name: "visit", linkageName: "([^"]+)"', ir_text
    ))
    need(len(matches) == 1,
         f"{label}: expected one debug-identified C++ visit symbol, got {sorted(matches)}")
    return next(iter(matches))


def cpp_call_count(ir_text, linkage):
    return len(re.findall(
        rf"\b(?:call|invoke)\b[^\n]*@{re.escape(linkage)}\b", ir_text
    ))


def compile_cpp_ir(args, env, report, reference, output, opt, label):
    command = tool(
        [args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", f"-O{opt}",
         "-g", "-S", "-emit-llvm", reference, "-o", output],
        args, env,
    )
    common.require_success(command, label)
    return {
        "command_index": command["command_index"], "ir": file_rec(output),
        "text": output.read_text(encoding="utf-8"),
        "verify": verify_ir(output, args, env, report, label),
    }


def prepare(args):
    env = common.make_env(args)
    out = common.prepare_out(args.out)
    report = {
        "schema": PREPARE_SCHEMA, "task": TASK, "contract": CONTRACT,
        "operation_id": OPERATION_ID, "mode": "prepare", "status": "running",
        "commands": [], "verifies": [], "cases": [],
    }
    common._COMMAND_LOG = report["commands"]
    try:
        report.update(base_report(args, env))
        expected, traces, oracle_files = load_oracle(args.oracle_dir)
        before_sha = sha_map(oracle_files, args.oracle_dir)
        templates = read_templates()
        report["oracle"] = {
            "directory": str(Path(args.oracle_dir).resolve()),
            "results": file_rec(Path(args.oracle_dir) / "results.json"),
            "inputs": file_rec(Path(args.oracle_dir) / "inputs.json"),
            "files_before": before_sha,
            "cases": {
                str(n): {"checksum": expected[n][0], "maximum": expected[n][1],
                         "trace": file_rec(traces[n])}
                for n in N_VALUES
            },
        }

        variants = (
            ("correct", "perm1", "flips"),
            ("wrong-checksum", "perm1", "flips"),
            ("wrong-maximum", "perm1", "flips"),
            ("negative-visit-flips", "perm1", "flips + 1"),
            ("negative-visit-values", "perm", "flips"),
        )
        for n in N_VALUES:
            case = {
                "n": n, "expected_checksum": expected[n][0],
                "expected_maximum": expected[n][1], "variants": {},
                "check": None, "builds": [], "correct_executions": [],
                "wrong_executions": [], "negative_source_executions": [],
                "helper": [], "cpp_ir": {}, "python": {},
                "negative_observers": [],
            }
            report["cases"].append(case)
            for variant, values, flips in variants:
                case["variants"][variant] = generate_case(
                    out, templates, n, expected[n], variant, values, flips
                )
            correct = case["variants"]["correct"]
            manifest = out / correct["manifest"]["path"]
            check = tool(
                [args.langc, "check", "--project", manifest, "--backend", "native",
                 "--format", "json"],
                args, env,
            )
            common.require_success(check, f"n={n} langc check")
            check_json = common.parse_json_stdout(check, f"n={n} langc check")
            need(check_json.get("status") == "ok", f"n={n} langc check is not ok")
            case["check"] = {"command_index": check["command_index"],
                             "json": check_json, "success": True}

            python_correct = emit_python(
                args, env, manifest, out / "python" / "emit" / f"n{n}" / "correct",
                f"n={n} correct",
            )
            case["python"]["correct"] = python_correct
            trace = run_observer(
                args, env, python_correct["directory"], traces[n],
                out / "python" / "trace" / f"n{n}" / "correct.txt",
                True, f"n={n} correct",
            )
            checksum = maximum = 0
            seen = set()
            frozen_text = Path(trace["frozen"]["path"]).read_text(encoding="utf-8")
            for number, line in enumerate(frozen_text.splitlines(), 1):
                fields = line.split(":")
                need(len(fields) == 3, f"n={n} trace line {number} is malformed")
                index = int(fields[0])
                need(index not in seen, f"n={n} trace index {index} repeats")
                seen.add(index)
                permutation = tuple(int(value) for value in fields[1].split(","))
                need(sorted(permutation) == list(range(1, n + 1)),
                     f"n={n} trace line {number} is not a permutation")
                flips = int(fields[2])
                checksum += flips if index % 2 == 0 else -flips
                maximum = max(maximum, flips)
            need((checksum, maximum) == expected[n],
                 f"n={n} trace derives {(checksum, maximum)}, expected {expected[n]}")
            trace["derived_checksum"], trace["derived_maximum"] = checksum, maximum
            case["python"]["trace"] = trace

            for opt in (0, 2):
                langc_row = build_langc(
                    args, env, manifest, out / "build" / f"n{n}" / "correct" /
                    "langc" / f"O{opt}", opt, f"n={n} correct langc O{opt}",
                )
                case["builds"].append(langc_row)
                run = run_program([langc_row["program"]["path"]], args, env)
                execution = {"implementation": "langc", "opt": opt,
                             "variant": "correct", "command_index": run["command_index"],
                             "success": False}
                case["correct_executions"].append(execution)
                common.require_quiet_success(run, f"n={n} correct langc O{opt} run")
                execution.update({"success": True, "exit": run["exit"],
                                  "stdout": run["stdout"], "stderr": run["stderr"]})

                cpp_row = build_cpp(
                    args, env, out / correct["reference"]["path"],
                    out / "build" / f"n{n}" / "correct" / "cpp" / f"O{opt}" /
                    "reference", opt, f"n={n} correct cpp O{opt}",
                )
                case["builds"].append(cpp_row)
                run = run_program([cpp_row["program"]["path"]], args, env)
                execution = {"implementation": "cpp", "opt": opt,
                             "variant": "correct", "command_index": run["command_index"],
                             "success": False}
                case["correct_executions"].append(execution)
                common.require_quiet_success(run, f"n={n} correct cpp O{opt} run")
                execution.update({"success": True, "exit": run["exit"],
                                  "stdout": run["stdout"], "stderr": run["stderr"]})

                case["helper"].append(export_source_llvm(
                    args, env, report, manifest, out / "llvm" / f"n{n}" / f"O{opt}",
                    opt, langc_row["object"]["path"], python_correct["visit_symbol"],
                    f"n={n} Source O{opt} LLVM",
                ))

                for wrong, expected_exit in (
                    ("wrong-checksum", 1), ("wrong-maximum", 2)
                ):
                    wrong_case = case["variants"][wrong]
                    wrong_manifest = out / wrong_case["manifest"]["path"]
                    for implementation in ("langc", "cpp"):
                        if implementation == "langc":
                            row = build_langc(
                                args, env, wrong_manifest,
                                out / "build" / f"n{n}" / wrong / "langc" / f"O{opt}",
                                opt, f"n={n} {wrong} langc O{opt}",
                            )
                        else:
                            row = build_cpp(
                                args, env, out / wrong_case["reference"]["path"],
                                out / "build" / f"n{n}" / wrong / "cpp" / f"O{opt}" /
                                "reference", opt, f"n={n} {wrong} cpp O{opt}",
                            )
                        case["builds"].append(row)
                        run = run_program([row["program"]["path"]], args, env)
                        execution = {"implementation": implementation, "opt": opt,
                                     "variant": wrong, "expected_exit": expected_exit,
                                     "command_index": run["command_index"],
                                     "success": False}
                        case["wrong_executions"].append(execution)
                        require_exit(
                            run, expected_exit,
                            f"n={n} {wrong} {implementation} O{opt}",
                        )
                        execution.update({"success": True, "exit": run["exit"],
                                          "stdout": run["stdout"],
                                          "stderr": run["stderr"]})

                for negative in ("negative-visit-flips", "negative-visit-values"):
                    negative_case = case["variants"][negative]
                    negative_manifest = out / negative_case["manifest"]["path"]
                    row = build_langc(
                        args, env, negative_manifest,
                        out / "build" / f"n{n}" / negative / "langc" / f"O{opt}",
                        opt, f"n={n} {negative} langc O{opt}",
                    )
                    case["builds"].append(row)
                    run = run_program([row["program"]["path"]], args, env)
                    execution = {"implementation": "langc", "opt": opt,
                                 "variant": negative, "command_index": run["command_index"],
                                 "success": False}
                    case["negative_source_executions"].append(execution)
                    common.require_quiet_success(
                        run, f"n={n} {negative} langc O{opt} run"
                    )
                    execution.update({"success": True, "exit": run["exit"],
                                      "stdout": run["stdout"], "stderr": run["stderr"]})

            for opt in (0, 2):
                ir = compile_cpp_ir(
                    args, env, report, out / correct["reference"]["path"],
                    out / "llvm" / f"n{n}" / f"cpp-O{opt}.ll", opt,
                    f"n={n} cpp O{opt} IR",
                )
                case["cpp_ir"][f"O{opt}"] = ir
            linkage = cpp_visit_linkage(case["cpp_ir"]["O0"]["text"], f"n={n} cpp O0")
            for opt in (0, 2):
                ir = case["cpp_ir"][f"O{opt}"]
                ir["visit_linkage"] = linkage
                ir["visit_calls"] = cpp_call_count(ir["text"], linkage)
                del ir["text"]
            need(case["cpp_ir"]["O0"]["visit_calls"] > 0,
                 f"n={n}: C++ O0 positive mangled visit-call control failed")
            need(case["cpp_ir"]["O2"]["visit_calls"] == 0,
                 f"n={n}: C++ O2 still has actual visit calls")

            for negative, field in (
                ("negative-visit-flips", "flips"),
                ("negative-visit-values", "permutation"),
            ):
                negative_case = case["variants"][negative]
                emitted = emit_python(
                    args, env, out / negative_case["manifest"]["path"],
                    out / "python" / "emit" / f"n{n}" / negative,
                    f"n={n} {negative}",
                )
                observer = run_observer(
                    args, env, emitted["directory"], traces[n],
                    out / "python" / "trace" / f"n{n}" / f"{negative}.txt",
                    False, f"n={n} {negative}",
                )
                need(observer["first_mismatch"]["field"] == field,
                     f"n={n} {negative}: first mismatch field is "
                     f"{observer['first_mismatch']['field']}, expected {field}")
                case["negative_observers"].append({
                    "variant": negative, "python": emitted, "observer": observer,
                    "expected_field": field, "rejected": True,
                })

        after_sha = sha_map(oracle_files, args.oracle_dir)
        report["oracle"]["files_after"] = after_sha
        report["oracle"]["files_unchanged"] = before_sha == after_sha
        need(report["oracle"]["files_unchanged"], "frozen oracle files changed")

        cases = report["cases"]
        correct = [e for case in cases for e in case["correct_executions"]]
        wrong = [e for case in cases for e in case["wrong_executions"]]
        negative = [e for case in cases for e in case["negative_source_executions"]]
        helper = [row for case in cases for row in case["helper"]]
        traces_rows = [case["python"]["trace"] for case in cases]
        negative_obs = [row for case in cases for row in case["negative_observers"]]
        report["summary"] = {
            "cases": len(cases), "n_values": list(N_VALUES),
            "correct_executions": len(correct),
            "correct_quiet_exit_0": sum(
                1 for e in correct if e["success"] and e["exit"] == 0
                and not e["stdout"] and not e["stderr"]),
            "wrong_executions": len(wrong),
            "wrong_checksum_exit_1": sum(
                1 for e in wrong if e["variant"] == "wrong-checksum" and
                e["success"] and e["exit"] == 1 and not e["stdout"] and not e["stderr"]),
            "wrong_maximum_exit_2": sum(
                1 for e in wrong if e["variant"] == "wrong-maximum" and
                e["success"] and e["exit"] == 2 and not e["stdout"] and not e["stderr"]),
            "negative_source_executions": len(negative),
            "negative_source_quiet_exit_0": sum(
                1 for e in negative if e["success"] and e["exit"] == 0
                and not e["stdout"] and not e["stderr"]),
            "python_visit_trace_lines": sum(row["lines"] for row in traces_rows),
            "python_visit_trace_matches": sum(
                1 for row in traces_rows if row["exact_text_match"]),
            "negative_observer_executions": len(negative_obs),
            "negative_observer_rejections": sum(
                1 for row in negative_obs if row["rejected"]),
            "source_llvm_exports": len(helper),
            "source_ir_verifies": 2 * len(helper),
            "cpp_ir_verifies": 2 * len(cases),
            "all_ir_verifies": len(report["verifies"]),
            "source_object_matches": sum(1 for row in helper if row["object_match"]),
            "source_o0_positive_cases": sum(
                1 for case in cases for row in case["helper"]
                if row["opt"] == 0 and row["visit_calls_after"] > 0),
            "source_o2_zero_cases": sum(
                1 for case in cases for row in case["helper"]
                if row["opt"] == 2 and row["visit_calls_after"] == 0),
            "source_o2_visit_calls": sum(
                row["visit_calls_after"] for row in helper if row["opt"] == 2),
            "cpp_o0_positive_cases": sum(
                1 for case in cases if case["cpp_ir"]["O0"]["visit_calls"] > 0),
            "cpp_o2_zero_cases": sum(
                1 for case in cases if case["cpp_ir"]["O2"]["visit_calls"] == 0),
            "cpp_o2_visit_calls": sum(
                case["cpp_ir"]["O2"]["visit_calls"] for case in cases),
            "oracle_rows_compared": sum(row["lines"] for row in traces_rows),
            "frozen_oracle_files_unchanged": report["oracle"]["files_unchanged"],
        }
        expected_summary = {
            "cases": 5, "n_values": list(N_VALUES),
            "correct_executions": 20, "correct_quiet_exit_0": 20,
            "wrong_executions": 40, "wrong_checksum_exit_1": 20,
            "wrong_maximum_exit_2": 20,
            "negative_source_executions": 20,
            "negative_source_quiet_exit_0": 20,
            "python_visit_trace_lines": 5910,
            "python_visit_trace_matches": 5,
            "negative_observer_executions": 10,
            "negative_observer_rejections": 10,
            "source_llvm_exports": 10, "source_ir_verifies": 20,
            "cpp_ir_verifies": 10, "all_ir_verifies": 30,
            "source_object_matches": 10,
            "source_o0_positive_cases": 5, "source_o2_zero_cases": 5,
            "source_o2_visit_calls": 0,
            "cpp_o0_positive_cases": 5, "cpp_o2_zero_cases": 5,
            "cpp_o2_visit_calls": 0,
            "oracle_rows_compared": 5910,
            "frozen_oracle_files_unchanged": True,
        }
        need(report["summary"] == expected_summary,
             f"prepare summary is unexpected: {report['summary']}")
        report["limitations"] = limitations()
        report["not_run"] = not_run()
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["failure"] = {"type": type(error).__name__, "message": str(error),
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
            "report": str(out / "prepare.json"), "summary": report.get("summary"),
            "failure": report.get("failure"),
        }, indent=2, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


def limitations():
    return [
        "No timing, throughput, speedup, or <=2x comparison is performed or claimed.",
        "The programs print nothing; correctness is the full checksum i64 and maximum i32 comparison, with quiet exit 1 for a wrong checksum and quiet exit 2 for a wrong maximum.",
        "The Source observer imports the unmodified generated Python module and records the actual visit arguments before calling the original empty visit; it does not recompute Fannkuch, permutations, or flips.",
        "The negative Source variants keep the checksum/maximum checks intact and only alter the arguments passed to empty visit; the observer must reject their actual traces.",
        "Source visit calls are identified by the metadata symbol ID and checked against NativeIR/LLVM symbols; C++ visit calls are identified from the O0 debug metadata linkage name, not by a letter-boundary search.",
        "Only n=3..7 and the fixed x86_64 generic target are covered. No n=8+, other targets, SIMD, threads, LTO, fast-math, or -march=native are used.",
        "LLVM verifier/object identity checks compiler artifacts, not mathematical correctness; the frozen oracle traces and full-value program checks provide the independent evidence.",
    ]


def not_run():
    return [
        {"item": "formal_timing", "reason": "this operation is preparation only"},
        {"item": "n_8_or_larger", "reason": "fixed inputs are n=3..7"},
        {"item": "simd_or_multithreaded_variant", "reason": "fixtures use the same single-thread scalar algorithm"},
        {"item": "cpp_observer", "reason": "negative observer controls are Source programs; C++ IR call recognition is checked separately"},
        {"item": "other_targets_or_compilers", "reason": "fixed x86_64 generic target and the recorded LLVM toolchain only"},
    ]


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--out", type=Path, help="new output directory; must not exist")
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
    need(args.out is not None, "--out is required")
    need(args.oracle_dir.is_dir(), f"--oracle-dir is missing: {args.oracle_dir}")
    for name in ("python", "python_helper", "langc", "helper", "runtime", "llvm_bin"):
        option = "--woozyc" if name == "langc" else "--" + name.replace("_", "-")
        need(getattr(args, name) is not None, f"{option} is required")
    for label, path in (
        ("python", args.python), ("python-helper", args.python_helper),
        ("langc", args.langc), ("helper", args.helper), ("runtime", args.runtime),
        ("llvm-bin/clang++", args.llvm_bin / "clang++"),
        ("llvm-bin/clang", args.llvm_bin / "clang"),
        ("llvm-bin/ld.lld", args.llvm_bin / "ld.lld"),
        ("llvm-bin/opt", args.llvm_bin / "opt"),
    ):
        need(path.is_file(), f"{label} is missing: {path}")


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
