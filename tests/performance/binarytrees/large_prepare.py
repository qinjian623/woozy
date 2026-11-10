#!/usr/bin/env python3
"""Prepare the fixed D=14/18, R=1/2 binary-trees large fixture.

This is a preparation/correctness/lifecycle runner, not a timing or calibration
script.  It reuses the existing fixture templates and the already-reviewed
oracle/build/IR/ELF helpers from binarytrees/prepare.py.  The only generated
code change is a test-only round wrapper around the unchanged tree algorithm.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import resource
import signal
import subprocess
import sys
import time
import traceback
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
WORKTREE = HERE.parents[2]
CHECK = WORKTREE / "checks/binarytrees-large-preparation-20261110"
PERF = HERE
OBSERVER = PERF / "large_observer.cpp"
ORACLE = Path("/home/qin/Work/language-dev-20261103/checks/"
              "eight-algorithm-large-oracles-20261110/binarytrees.json")
ORACLE_SHA = "608d851358c559bbad5fe0ecf3d9eeafe9ba510e0444cba02780e6eb5e280528"
LLVM_BIN = Path("/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin")
LD_LIBRARY_PATH = ("/home/qin/Work/language-dev-20261103/toolchains/"
                   "libxml2-legacy-2.13.9/usr/lib")
PYTHON = Path("/home/qin/Work/language-dev-20261103/toolchains/python/"
              "cpython-3.12.15-linux-x86_64-gnu/bin/python3.12")
TOOLS = {
    "langc": Path("/home/qin/Work/language-dev-20261103/compiler/build/"
                  "native-perf-release/woozyc"),
    "helper": Path("/home/qin/Work/language-dev-20261103/compiler/build/"
                   "native-perf-release/native/lang_native_helper"),
    "runtime": Path("/home/qin/Work/language-dev-20261103/compiler/build/"
                    "native-perf-release/native/liblang_native_runtime.a"),
}
TOOL_SHA = {
    "langc": "a17e1cc3753371a3588db3b44a5fab36e3caff1440f9889997e416008bab73ec",
    "helper": "f24f410da99eb0f827f7031a76d4220164b50cb03070a4c382e137a12acc989e",
    "runtime": "40ea4d083e97f511c776166ede32e294753aeef3c3417742b84a79885f48d4eb",
}
N_VALUES = (14, 18)
ROUNDS = (1, 2)
VARIANTS = ("wrong-stretch", "wrong-tree-count", "wrong-bucket", "wrong-long")
BUILD_TIMEOUT = 300
PROGRAM_TIMEOUT = 180
MODEL = "deepseek-flash"


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_rec(path, base=None):
    path = Path(path).resolve()
    result = {"path": str(path), "sha256": sha256_file(path),
              "size_bytes": path.stat().st_size}
    if base is not None:
        result["rel"] = str(path.relative_to(Path(base).resolve()))
    return result


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n",
                         encoding="utf-8")
    os.replace(temporary, path)


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class State:
    def __init__(self, out):
        self.out = out
        self.commands = []
        self.results = {}
        (out / "commands.jsonl").write_text("", encoding="utf-8")

    def record(self, command):
        self.commands.append(command)
        with (self.out / "commands.jsonl").open("a", encoding="utf-8") as log:
            log.write(json.dumps(command, ensure_ascii=False) + "\n")

    def save(self):
        write_json(self.out / "commands.json", self.commands)
        write_json(self.out / "results.json", self.results)


def child_setup():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(argv, timeout, label, kind="command", env=None):
    argv = [str(value) for value in argv]
    child_env = env if env is not None else ENV
    command = {"command_index": len(STATE.commands), "kind": kind,
               "label": label, "argv": argv, "timeout_s": timeout,
               "started_at": utc_now()}
    started = time.perf_counter_ns()
    try:
        process = subprocess.Popen(
            argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=child_env, start_new_session=True, preexec_fn=child_setup)
        try:
            out, err = process.communicate(timeout=timeout)
            status = "exited"
            killed = False
        except subprocess.TimeoutExpired:
            status = "timeout"
            killed = True
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            out, err = process.communicate()
        command.update({
            "status": status,
            "exit": process.returncode if status == "exited" else None,
            "returncode": process.returncode,
            "stdout": (out or b"").decode("utf-8", "replace"),
            "stderr": (err or b"").decode("utf-8", "replace"),
            "elapsed_ns": time.perf_counter_ns() - started,
            "process_group_killed": killed, "reaped": True,
            "ended_at": utc_now(),
        })
    except (OSError, subprocess.SubprocessError) as error:
        command.update({"status": "launch_failed", "exit": None,
                        "stdout": "", "stderr": "",
                        "elapsed_ns": time.perf_counter_ns() - started,
                        "error": f"{type(error).__name__}: {error}"})
    STATE.record(command)
    logdir = STATE.out / "logs"
    logdir.mkdir(exist_ok=True)
    stem = re.sub(r"[^A-Za-z0-9_.-]+", "_", label)
    (logdir / f"{stem}.stdout").write_text(command["stdout"], encoding="utf-8")
    (logdir / f"{stem}.stderr").write_text(command["stderr"], encoding="utf-8")
    STATE.save()
    return command


def tool(argv, args, env):
    return run(argv, args.timeout, "tool")


def require_success(row, label):
    if row["status"] != "exited" or row["exit"] != 0:
        raise RuntimeError(
            f"{label}: expected exit 0, got {row['status']}/{row['exit']}: "
            f"{row.get('stderr', '')[-2000:]}")


def attempt(label, function):
    try:
        return function()
    except Exception as error:
        STATE.results["failures"].append({
            "label": label, "error": str(error),
            "traceback": traceback.format_exc()})
        STATE.save()
        return None


def perfect_nodes(depth):
    return (1 << (depth + 1)) - 1


def compare_oracle(n, frozen, case):
    pairs = {
        "max_depth": (frozen["max_depth"], case["max_depth"]),
        "stretch_depth": (frozen["stretch_depth"], case["stretch_depth"]),
        "stretch_check": (frozen["stretch_check"], case["stretch_check"]),
        "long_lived_check": (frozen["long_lived_check"], case["long_check"]),
    }
    fields = {}
    for name, (a, b) in pairs.items():
        if a != b:
            raise RuntimeError(f"n={n}: oracle field {name}: {a} != {b}")
        fields[name] = {"frozen": a, "oracle_case": b, "match": True}
    if len(frozen["rows"]) != len(case["depths"]):
        raise RuntimeError(f"n={n}: oracle row count differs")
    rows = []
    for index, (a, b) in enumerate(zip(frozen["rows"], case["depths"])):
        row = {}
        for key in ("depth", "trees", "nodes_each", "check"):
            if a[key] != b[key]:
                raise RuntimeError(f"n={n}: row {index} {key} differs")
            row[key] = {"frozen": a[key], "oracle_case": b[key], "match": True}
        rows.append(row)
    return {"n": n, "fields": fields, "rows": rows,
            "text_sha256": case["text_sha256"], "all_equal": True}


def wrap_rounds(text, language, rounds, label):
    round_marker = f"// PERF-BINARYTREES-ROUNDS rounds={rounds}\n"
    if language == "source":
        old = "func main() i32 {\n    return run();\n}\n"
        new = (
            "func main() i32 {\n"
            f"    for var round i32 = 0; round < {rounds}; round += 1 {{\n"
            "        let result = run();\n"
            "        if result != 0 {\n"
            "            return result;\n"
            "        }\n"
            "    }\n"
            "    return 0;\n"
            "}\n")
    else:
        old = "int main() {\n    return run();\n}\n"
        new = (
            "int main() {\n"
            f"    for (int32_t round = 0; round < {rounds}; ++round) {{\n"
            "        const int32_t result = run();\n"
            "        if (result != 0) {\n"
            "            return result;\n"
            "        }\n"
            "    }\n"
            "    return 0;\n"
            "}\n")
    if old not in text:
        raise RuntimeError(f"{label}: cannot find original main")
    text = round_marker + text.replace(old, new)
    match = re.search(r"^// PERF-BINARYTREES-ROUNDS rounds=(\d+)$", text,
                      re.MULTILINE)
    if match is None or int(match.group(1)) != rounds:
        raise RuntimeError(f"{label}: round marker missing")
    if f"round < {rounds}" not in text:
        raise RuntimeError(f"{label}: round bound missing")
    return text


def make_case(base, templates, case, variant, rounds):
    row = BT.generate_case(base, templates, case, variant)
    language_label = f"n={case['n']} r={rounds} {variant}"
    source_path = Path(row["source"]["path"])
    reference_path = Path(row["reference"]["path"])
    source_text = wrap_rounds(source_path.read_text(encoding="utf-8"),
                              "source", rounds, language_label + " Source")
    reference_text = wrap_rounds(reference_path.read_text(encoding="utf-8"),
                                 "cpp", rounds, language_label + " C++")
    source_path.write_text(source_text, encoding="utf-8")
    reference_path.write_text(reference_text, encoding="utf-8")
    BT.verify_generated(source_text, reference_text, case, variant,
                        BT.oracle_values(case, variant), language_label)
    row = dict(row)
    row["source"] = file_rec(source_path, base)
    row["reference"] = file_rec(reference_path, base)
    row["n"] = case["n"]
    row["rounds"] = rounds
    row["variant"] = variant
    row["case"] = case
    row["expected_exit"] = BT.VARIANT_EXITS[variant]
    row["case_dir"] = str(source_path.parent.parent)
    return row


def build_source(case, opt):
    directory = Path(case["case_dir"])
    out_dir = STATE.out / "build" / f"n{case['n']}" / f"r{case['rounds']}" / \
        case["variant"] / "source" / f"O{opt}"
    label = f"n={case['n']} r={case['rounds']} Source O{opt}"
    row = BT.build_langc(ARGS, ENV, STATE.out,
                         directory / "lang.toml", out_dir, opt, label)
    row.update({"label": label, "n": case["n"], "rounds": case["rounds"],
                "variant": case["variant"], "implementation": "langc",
                "opt": opt})
    return row


def build_cpp(case, opt):
    directory = Path(case["case_dir"])
    program = STATE.out / "build" / f"n{case['n']}" / f"r{case['rounds']}" / \
        case["variant"] / "cpp" / f"O{opt}" / "reference"
    label = f"n={case['n']} r={case['rounds']} C++ O{opt}"
    row = BT.build_cpp(ARGS, ENV, STATE.out, directory / "reference.cpp",
                       program, opt, label)
    row.update({"label": label, "n": case["n"], "rounds": case["rounds"],
                "variant": case["variant"], "implementation": "cpp",
                "opt": opt})
    return row


def cpp_work_check_no_debug(ir, label):
    text = Path(ir).read_text(encoding="utf-8")
    functions = BT.parse_functions(text)
    make_name = "_ZL9make_treei"
    check_name = "_ZL5checkRK4Node"
    if make_name not in functions or check_name not in functions:
        raise RuntimeError(f"{label}: missing make_tree/check definitions")
    make_body = functions[make_name]["body"]
    check_body = functions[check_name]["body"]
    new_calls = len(re.findall(r"\b(?:call|invoke)\b[^\n]*@_Znwm\b", text))
    delete_calls = len(re.findall(r"\b(?:call|invoke)\b[^\n]*@_ZdlPv", text))
    make_self = len(re.findall(
        r"\b(?:call|invoke)\b[^\n]*@" + re.escape(make_name) + r"\b", make_body))
    check_self = len(re.findall(
        r"\b(?:call|invoke)\b[^\n]*@" + re.escape(check_name) + r"\b", check_body))
    for name, value in (("operator new", new_calls), ("operator delete", delete_calls),
                        ("make_tree self", make_self), ("check self", check_self)):
        if value < 1:
            raise RuntimeError(f"{label}: {name} missing")
    for needle, name in (("load ptr", "child pointer load"),
                         ("add i32", "node-count add")):
        if needle not in check_body and not (needle == "add i32"
                                             and "add nsw i32" in check_body):
            raise RuntimeError(f"{label}: {name} missing")
    if "load i1" not in check_body and "load i8" not in check_body:
        raise RuntimeError(f"{label}: leaf-flag load missing")
    make_total = len(re.findall(
        r"\b(?:call|invoke)\b[^\n]*@" + re.escape(make_name) + r"\b", text))
    check_total = len(re.findall(
        r"\b(?:call|invoke)\b[^\n]*@" + re.escape(check_name) + r"\b", text))
    if make_total <= make_self or check_total <= check_self:
        raise RuntimeError(f"{label}: missing non-recursive caller")
    loops = tool([LLVM_BIN / "opt", "-passes=print<loops>",
                  "-disable-output", ir], ARGS, ENV)
    require_success(loops, f"{label} loop analysis")
    loop_count = loops["stderr"].count("Loop at depth")
    return {"path": str(ir), "file": file_rec(ir),
            "make_tree": make_name, "check": check_name,
            "operator_new_calls": new_calls,
            "operator_delete_calls": delete_calls,
            "make_tree_self_calls": make_self, "check_self_calls": check_self,
            "make_tree_total_calls": make_total, "check_total_calls": check_total,
            "loop_count": loop_count, "success": True}


def build_cpp_ir(case, opt):
    directory = Path(case["case_dir"])
    ir = STATE.out / "ir" / f"n{case['n']}" / f"r{case['rounds']}" / \
        f"cpp-O{opt}.ll"
    ir.parent.mkdir(parents=True, exist_ok=True)
    flags = [*BT.TARGET_FLAGS, "-std=c++20", f"-O{opt}", "-S", "-emit-llvm"]
    command = tool([
        LLVM_BIN / "clang++", *flags, directory / "reference.cpp", "-o", ir],
        ARGS, ENV)
    require_success(command, f"{case['n']} r={case['rounds']} C++ O{opt} IR")
    verify = BT.verify_ir(ir, ARGS, ENV, STATE.results,
                          f"n={case['n']} r={case['rounds']} C++ O{opt} IR")
    work = None
    if opt == 2:
        work = cpp_work_check_no_debug(
            ir, f"n={case['n']} r={case['rounds']} C++ O2")
    return {"ir": file_rec(ir, STATE.out), "verify": verify, "work": work,
            "flags": [str(flag) for flag in flags],
            "normal_flags": [str(flag) for flag in
                             [*BT.TARGET_FLAGS, "-std=c++20", f"-O{opt}"]],
            "command_index": command["command_index"]}


def helper_export(case, build):
    directory = Path(case["case_dir"])
    label = f"n={case['n']} r={case['rounds']} Source O{build['opt']}"
    out_dir = STATE.out / "helper" / f"n{case['n']}" / f"r{case['rounds']}" / \
        f"O{build['opt']}"
    helper = BT.helper_export(
        ARGS, ENV, STATE.results, STATE.out, directory / "lang.toml", out_dir,
        build["opt"], Path(build["object"]["path"]), label)
    work = None
    if build["opt"] == 2:
        work = BT.source_work_check(
            ARGS, ENV, STATE.results, Path(helper["after"]["path"]), label)
    return {"label": label, "opt": build["opt"], "helper": helper,
            "work": work}


def elf_check(path, implementation, label):
    return BT.elf_checks(ARGS, ENV, STATE.results, Path(path), label,
                         implementation)


def execute(program, expected, label):
    row = run([program], PROGRAM_TIMEOUT, label, kind="program")
    quiet = not row["stdout"] and not row["stderr"]
    completed = (row["status"] == "exited" and row["exit"] == expected and quiet)
    result = {"label": label, "command": row, "expected_exit": expected,
              "quiet": quiet, "completed": completed}
    if not completed:
        STATE.results["failures"].append({
            "label": label,
            "error": (f"expected quiet exit {expected}, got {row['status']}/"
                      f"{row['exit']} stdout={row['stdout'][-500:]!r} "
                      f"stderr={row['stderr'][-500:]!r}")})
    return result


def parse_main_blocks(body):
    blocks = {}
    current = None
    for line in body.splitlines():
        match = re.match(r"^([A-Za-z0-9_.]+):", line)
        if match is not None:
            current = match.group(1)
            blocks[current] = []
        elif current is not None:
            blocks[current].append(line)
    return blocks


def loop_shape(ir, label):
    command = tool([
        LLVM_BIN / "opt", "-passes=print<loops>", "-disable-output", ir],
        ARGS, ENV)
    require_success(command, f"{label} loop analysis")
    lines = []
    in_main = False
    for line in command["stderr"].splitlines():
        if line.startswith("Loop info for function 'main':"):
            in_main = True
            continue
        if in_main and line.startswith("Loop info for function "):
            break
        if in_main and line.strip().startswith("Loop at depth"):
            lines.append(line.strip())
    outer_line = next((line for line in lines
                       if line.startswith("Loop at depth 1")), None)
    header = None
    latch = None
    blocks = set()
    if outer_line is not None:
        tokens = re.findall(r"%([A-Za-z0-9_.]+)", outer_line)
        if tokens:
            header = tokens[0]
        blocks = set(tokens)
        match = re.search(r"%([A-Za-z0-9_.]+)<latch>", outer_line)
        if match is not None:
            latch = match.group(1)
    depth1 = sum(line.startswith("Loop at depth 1") for line in lines)
    depth2 = sum(line.startswith("Loop at depth 2") for line in lines)
    return {"command_index": command["command_index"], "depth1": depth1,
            "depth2": depth2, "lines": lines, "outer_line": outer_line,
            "header": header, "latch": latch, "blocks": sorted(blocks)}


def scalar_trip(ir, header, label):
    if header is None:
        return {"command_index": None, "constant_max": None,
                "symbolic_max": None, "text": ""}
    command = tool([
        LLVM_BIN / "opt", "-passes=print<scalar-evolution>",
        "-disable-output", ir], ARGS, ENV)
    require_success(command, f"{label} scalar evolution")
    text = command["stderr"]
    def field(kind):
        match = re.search(
            rf"Loop %{re.escape(header)}: {kind} backedge-taken count is i32 (\d+)",
            text)
        return int(match.group(1)) if match is not None else None
    return {"command_index": command["command_index"],
            "constant_max": field("constant max"),
            "symbolic_max": field("symbolic max"),
            "text": text}


def check_round_ir(ir, implementation, label, case):
    shape = loop_shape(ir, label)
    scalar = scalar_trip(ir, shape["header"], label)
    text = Path(ir).read_text(encoding="utf-8")
    functions = BT.parse_functions(text)
    main_body = functions.get("main", {}).get("body", "")
    blocks = parse_main_blocks(main_body)
    reasons = []
    outer_blocks = set(shape["blocks"])
    region = "\n".join("\n".join(blocks.get(name, []))
                        for name in shape["blocks"])
    check_values = [case["stretch_check"], case["long_check"]]
    check_values += [row["check"] for row in case["depths"]]
    missing_checks = [
        value for value in check_values
        if re.search(rf"(?<![0-9]){value}(?![0-9])", region) is None]

    if shape["depth1"] < 1 or shape["header"] is None:
        reasons.append("no actual outer loop")
    if shape["depth2"] < 1:
        reasons.append("no nested bucket loop in outer region")
    if missing_checks:
        reasons.append(f"checks missing from outer region: {missing_checks}")

    if implementation == "langc":
        work = {
            "release": "@lang_vec_release_v1" in region,
            "tree_calls": len(set(re.findall(r"@lang\.fn\.[0-9]+", region))),
        }
        work_ok = work["release"] and work["tree_calls"] >= 2
    else:
        work = {
            "make_tree": "@_ZL9make_treei" in region,
            "check": "@_ZL5checkRK4Node" in region,
            "free": ("@_ZNSt10unique_ptrI4NodeSt14default_deleteIS0_EED2Ev"
                     in region or "@_ZdlPvm" in region),
        }
        work_ok = work["make_tree"] and work["check"] and work["free"]
    if not work_ok:
        reasons.append("tree create/check/free work missing from outer region")

    phi = None
    phi_matches = []
    header = shape["header"]
    latch = shape["latch"]
    if header in blocks:
        for line in blocks[header]:
            match = re.search(
                r"(%[A-Za-z0-9_.]+) = phi i1 "
                r"\[ (true|false), %([A-Za-z0-9_.]+) \], "
                r"\[ (true|false), %([A-Za-z0-9_.]+) \]", line)
            if match is not None:
                phi_matches.append(match)
    if len(phi_matches) != 1:
        reasons.append("outer header does not have exactly one two-input i1 phi")
    else:
        match = phi_matches[0]
        phi = {"name": match.group(1),
               "values": {match.group(3): match.group(2) == "true",
                          match.group(5): match.group(4) == "true"}}
        if latch not in phi["values"]:
            reasons.append("outer phi has no incoming value from latch")
        else:
            preheaders = [name for name in phi["values"] if name != latch]
            if len(preheaders) != 1 or preheaders[0] in outer_blocks:
                reasons.append("outer phi has no unique preheader incoming")
            else:
                phi["preheader"] = preheaders[0]
                phi["pre_value"] = phi["values"][preheaders[0]]
                phi["latch_value"] = phi["values"][latch]

    branch = None
    branch_matches = []
    if latch in blocks:
        for line in blocks[latch]:
            match = re.search(
                r"\bbr i1 (%[A-Za-z0-9_.]+), label %([A-Za-z0-9_.]+), "
                r"label %([A-Za-z0-9_.]+)", line)
            if match is not None and (match.group(2) == header
                                      or match.group(3) == header):
                branch_matches.append((line, match))
    if len(branch_matches) != 1:
        reasons.append("outer latch does not have one header/exit branch")
    else:
        line, match = branch_matches[0]
        condition = match.group(1)
        true_target = match.group(2)
        false_target = match.group(3)
        other_target = false_target if true_target == header else true_target
        if other_target in outer_blocks:
            reasons.append("outer latch branch has no outside exit target")
        backedge_on_true = true_target == header
        branch = {"line": line, "condition": condition,
                  "true_target": true_target, "false_target": false_target,
                  "backedge_on_true": backedge_on_true,
                  "exit_bool": "false" if backedge_on_true else "true",
                  "backedge_bool": "true" if backedge_on_true else "false"}

    pattern = None
    if phi is not None and branch is not None and not reasons:
        condition = branch["condition"]
        if condition == phi["name"]:
            first_continues = phi["pre_value"] == branch["backedge_on_true"]
            second_exits = phi["latch_value"] != branch["backedge_on_true"]
            if first_continues and second_exits:
                pattern = {"kind": "phi_direct",
                           "exact_two_iterations": True,
                           "requires_successful_long_check": False}
            else:
                reasons.append("phi incoming values do not yield one backedge then exit")
        else:
            definition = None
            for name in (latch, header):
                for candidate in blocks.get(name, []):
                    match = re.search(
                        rf"{re.escape(condition)}\s*=\s*or i1\s+"
                        r"(%[A-Za-z0-9_.]+),\s*(%[A-Za-z0-9_.]+)",
                        candidate)
                    if match is not None:
                        definition = (name, match)
                        break
                if definition is not None:
                    break
            if definition is None:
                reasons.append("latch condition is neither phi nor phi-or-guard")
            else:
                _, match = definition
                operands = [match.group(1), match.group(2)]
                if phi["name"] not in operands:
                    reasons.append("latch or-condition does not include outer phi")
                else:
                    other = operands[1] if operands[0] == phi["name"] else operands[0]
                    other_definition = None
                    for name in (latch, header):
                        for candidate in blocks.get(name, []):
                            check = re.search(
                                rf"{re.escape(other)}\s*=\s*icmp\s+(ne|eq)\s+"
                                rf"i32\s+[^,\n]+,\s*{case['long_check']}\b",
                                candidate)
                            if check is not None:
                                other_definition = check
                                break
                        if other_definition is not None:
                            break
                    first_continues = phi["pre_value"] == branch["backedge_on_true"]
                    second_exits = phi["latch_value"] != branch["backedge_on_true"]
                    if (other_definition is not None
                            and other_definition.group(1) == "ne"
                            and first_continues and second_exits):
                        pattern = {"kind": "phi_or_long_check",
                                   "exact_two_iterations": False,
                                   "requires_successful_long_check": True}
                    else:
                        reasons.append("phi-or guard is not the successful long-lived check")

    trip_ok = (scalar["constant_max"] == 1
               and scalar["symbolic_max"] == 1)
    if not trip_ok:
        reasons.append(
            f"outer backedge max is not one: {scalar['constant_max']}/"
            f"{scalar['symbolic_max']}")
    accepted = not reasons
    return {
        "label": label, "shape": shape, "scalar": scalar,
        "outer_region_checks": {"all_present": not missing_checks,
                                "missing": missing_checks,
                                "values": check_values},
        "work_in_outer_region": work, "work_ok": work_ok,
        "phi": phi, "branch": branch, "pattern": pattern,
        "trip_max_backedges": scalar["constant_max"],
        "accepted_as_two_rounds": accepted,
        "exact_two_iterations": bool(pattern and pattern["exact_two_iterations"]),
        "requires_successful_long_check": bool(
            pattern and pattern["requires_successful_long_check"]),
        "rejection_reasons": reasons,
    }

def parse_summary(path):
    text = Path(path).read_text(encoding="utf-8")
    lines = text.splitlines()
    rounds = []
    total = None
    status = None
    plan = None
    for line in lines:
        if line.startswith("PLAN:"):
            parts = line.split(":")
            plan = {"depth": int(parts[1]), "rounds": int(parts[2]),
                    "phases": int(parts[3])}
        elif line.startswith("ROUND:"):
            parts = line.split(":")
            values = {"ROUND": int(parts[1])}
            for index in range(2, len(parts), 2):
                values[parts[index]] = int(parts[index + 1])
            rounds.append(values)
        elif line.startswith("TOTAL:"):
            parts = line.split(":")
            total = {parts[index]: int(parts[index + 1])
                     for index in range(1, len(parts), 2)}
        elif line.startswith("STATUS:"):
            status = line.split(":", 1)[1]
    return {"path": str(path), "sha256": sha256_file(path),
            "size_bytes": Path(path).stat().st_size, "plan": plan,
            "rounds": rounds, "total": total, "status": status,
            "line_count": len(lines)}


def verify_summary(parsed, case, rounds, implementation):
    if parsed["status"] != "ok":
        raise RuntimeError(f"observer status is {parsed['status']}")
    if parsed["plan"] != {"depth": case["n"], "rounds": rounds,
                          "phases": 2 + len(case["depths"])}:
        raise RuntimeError(f"observer plan mismatch: {parsed['plan']}")
    phases = [{"kind": 0, "depth": case["stretch_depth"], "trees": 1,
               "nodes": case["stretch_check"]}]
    phases.append({"kind": 1, "depth": case["long_depth"], "trees": 1,
                   "nodes": case["long_check"]})
    for row in case["depths"]:
        phases.append({"kind": 2, "depth": row["depth"],
                       "trees": row["trees"], "nodes": row["nodes_each"]})
    expected_lines = rounds * len(phases)
    if len(parsed["rounds"]) != expected_lines:
        raise RuntimeError(f"observer has {len(parsed['rounds'])} phase lines, "
                           f"expected {expected_lines}")
    long_nodes = case["long_check"]
    max_peak = max([case["stretch_check"], case["long_check"]] +
                   [long_nodes + row["nodes_each"] for row in case["depths"]])
    totals = {"CREATED": 0, "LEAVES": 0, "INTERNAL": 0, "RELEASED": 0}
    for round_index in range(rounds):
        for phase_index, phase in enumerate(phases):
            actual = parsed["rounds"][round_index * len(phases) + phase_index]
            trees = phase["trees"]
            nodes = phase["nodes"]
            leaves = (1 << phase["depth"])
            internal = leaves - 1
            expected = {
                "ROUND": round_index, "KIND": phase["kind"],
                "DEPTH": phase["depth"], "TREES": trees, "ACTUAL": trees,
                "CREATED": trees * nodes,
                "LEAVES": trees * leaves,
                "INTERNAL": trees * internal,
                "RELEASED": trees * nodes,
                "PEAK": (long_nodes + nodes if phase["kind"] == 2
                         else nodes),
                "LONG_LIVE": long_nodes if phase["kind"] == 2 else 0,
                "FINAL": 0,
            }
            if actual != expected:
                raise RuntimeError(f"n={case['n']} r={rounds} phase "
                                   f"{phase_index}: {actual} != {expected}")
            for key in totals:
                totals[key] += expected[key]
    total = parsed["total"]
    if total is None:
        raise RuntimeError("observer total line missing")
    for key, value in totals.items():
        if total.get(key) != value:
            raise RuntimeError(f"observer total {key}: {total.get(key)} != {value}")
    if total.get("PHYSICAL_FINAL") != 0:
        raise RuntimeError("observer physical final is not zero")
    expected_physical = max_peak * (2 if implementation == "langc" else 1)
    if total.get("PHYSICAL_PEAK") != expected_physical:
        raise RuntimeError(f"observer physical peak {total.get('PHYSICAL_PEAK')}"
                           f" != {expected_physical}")
    if parsed["size_bytes"] > 100000:
        raise RuntimeError("observer summary is unexpectedly large")
    return {"status": "passed", "phases_checked": expected_lines,
            "max_node_peak": max_peak, "physical_peak": expected_physical,
            "long_lived_nodes": long_nodes, "totals": totals}


def build_observer(case, source_build, reference):
    label = f"n={case['n']} r={case['rounds']}"
    if source_build is not None:
        binary = STATE.out / "observers" / f"n{case['n']}" / f"r{case['rounds']}" / "source"
        binary.parent.mkdir(parents=True, exist_ok=True)
        command = tool([
            LLVM_BIN / "clang++", *BT.TARGET_FLAGS, "-std=c++20", "-O2",
            "-I", WORKTREE / "runtime/native",
            Path(source_build["object"]["path"]), OBSERVER, TOOLS["runtime"],
            "-Wl,--wrap=lang_alloc_v1", "-Wl,--wrap=lang_free_v1",
            f"--ld-path={LLVM_BIN / 'ld.lld'}", "-o", binary], ARGS, ENV)
        require_success(command, f"{label} Source observer build")
        return {"implementation": "langc", "binary": file_rec(binary, STATE.out),
                "build_command_index": command["command_index"],
                "program_object": source_build["object"]}
    binary = STATE.out / "observers" / f"n{case['n']}" / f"r{case['rounds']}" / "cpp"
    binary.parent.mkdir(parents=True, exist_ok=True)
    object_path = binary.parent / "reference_observe.o"
    compile_command = tool([
        LLVM_BIN / "clang++", *BT.TARGET_FLAGS, "-std=c++20", "-O2",
        "-Dmain=tree_program_main", "-c", reference, "-o", object_path],
        ARGS, ENV)
    require_success(compile_command, f"{label} C++ observer reference")
    link_command = tool([
        LLVM_BIN / "clang++", *BT.TARGET_FLAGS, "-std=c++20", "-O2",
        "-DBT_CPP_OBSERVER", OBSERVER, object_path,
        f"--ld-path={LLVM_BIN / 'ld.lld'}", "-o", binary], ARGS, ENV)
    require_success(link_command, f"{label} C++ observer link")
    return {"implementation": "cpp", "binary": file_rec(binary, STATE.out),
            "compile_command_index": compile_command["command_index"],
            "link_command_index": link_command["command_index"],
            "reference_object": file_rec(object_path, STATE.out)}


def run_observer(case, observer):
    summary = STATE.out / "observers" / f"n{case['n']}" / \
        f"r{case['rounds']}" / f"{observer['implementation']}.summary"
    env = dict(ENV)
    env.update({"BT_LARGE_SUMMARY": str(summary),
                "BT_LARGE_DEPTH": str(case["n"]),
                "BT_LARGE_ROUNDS": str(case["rounds"])})
    command = run([observer["binary"]["path"]], PROGRAM_TIMEOUT,
                  f"n={case['n']} r={case['rounds']} "
                  f"{observer['implementation']} lifecycle observer",
                  kind="observer", env=env)
    if (command["status"] != "exited" or command["exit"] != 0
            or command["stdout"] or command["stderr"]):
        raise RuntimeError(f"observer execution failed: {command}")
    parsed = parse_summary(summary)
    verification = verify_summary(parsed, case["case"], case["rounds"],
                                  observer["implementation"])
    return {"command": command, "summary": parsed,
            "verification": verification}


def snapshot():
    paths = list(TOOLS.values()) + [
        LLVM_BIN / "clang++", LLVM_BIN / "opt", LLVM_BIN / "ld.lld",
        ORACLE, OBSERVER, PERF / "large_prepare.py",
        PERF / "binarytrees.wz.in", PERF / "reference.cpp.in",
        PERF / "prepare.py", WORKTREE / "tests/performance/run_native.py",
        WORKTREE / "tests/performance/sieve/prepare.py",
        WORKTREE / "tests/performance/mandelbrot/prepare.py",
    ]
    return {str(path.resolve()): sha256_file(path) for path in paths}


def git_status():
    head = subprocess.run(["git", "-C", str(WORKTREE), "rev-parse", "HEAD"],
                          capture_output=True, text=True, timeout=30)
    status = subprocess.run(
        ["git", "-C", str(WORKTREE), "status", "--porcelain",
         "--untracked-files=no"], capture_output=True, text=True, timeout=30)
    return {"head": head.stdout.strip(), "tracked_status": status.stdout}


def main(argv=None):
    global STATE, ARGS, ENV, BT
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=CHECK / "r1")
    parser.add_argument("--model", default=MODEL)
    parser.add_argument("--reported-model", default=os.environ.get("PI_MODEL", MODEL))
    args = parser.parse_args(argv)
    out = args.out.resolve()
    if out.exists():
        raise SystemExit(f"refusing to overwrite {out}")
    out.mkdir(parents=True)

    ENV = os.environ.copy()
    ENV["LD_LIBRARY_PATH"] = LD_LIBRARY_PATH
    ARGS = SimpleNamespace(langc=TOOLS["langc"], helper=TOOLS["helper"],
                           runtime=TOOLS["runtime"], llvm_bin=LLVM_BIN,
                           timeout=BUILD_TIMEOUT, run_timeout=PROGRAM_TIMEOUT,
                           cpu=None)
    STATE = State(out)
    (out / "large_prepare.py").write_text(Path(__file__).read_text(),
                                           encoding="utf-8")

    BT = load_module(PERF / "prepare.py", "_binarytrees_large_prepare")
    BT.tool = tool
    BT.sieve.tool = tool
    BT.mandelbrot.tool = tool

    before = snapshot()
    oracle_json = json.loads(ORACLE.read_text(encoding="utf-8"))
    if sha256_file(ORACLE) != ORACLE_SHA:
        raise SystemExit("frozen binarytrees oracle SHA differs")
    frozen = {row["max_depth"]: row for row in oracle_json["cases"]}
    results = {
        "schema": "compiler-dev/native-binarytrees-large-preparation/1",
        "task": "native-binarytrees-large-preparation",
        "operation_id": "language-binarytrees-large-preparation-20261110",
        "contract": "compiler-dev/1",
        "status": "running",
        "requested_model": args.model,
        "reported_model": args.reported_model,
        "model": {"requested": args.model, "reported": args.reported_model,
                  "pi_model_env": os.environ.get("PI_MODEL")},
        "formal_timing": False,
        "calibration": False,
        "started_at": utc_now(),
        "out_dir": str(out),
        "worktree": {"path": str(WORKTREE), **git_status()},
        "tools": {name: {**file_rec(path), "expected_sha256": TOOL_SHA[name],
                         "match": sha256_file(path) == TOOL_SHA[name]}
                  for name, path in TOOLS.items()},
        "oracle": {"path": str(ORACLE), "sha256": sha256_file(ORACLE),
                   "expected_sha256": ORACLE_SHA, "cases": []},
        "templates": {name: file_rec(PERF / name)
                      for name in ("binarytrees.wz.in", "reference.cpp.in")},
        "observer_source": file_rec(OBSERVER),
        "input_snapshot_before": before,
        "input_snapshot_after": None,
        "inputs_unchanged": None,
        "groups": [],
        "generated": {},
        "wrong_controls": [],
        "elf_checks": [],
        "verifies": [],
        "loop_analyses": [],
        "failures": [],
        "summary": {},
        "limitations": [
            "No timing, calibration, speedup, or <=2x conclusion is produced.",
            "R1/R2 work evidence covers only these actual D=14/18 instances.",
            "Lifecycle summaries are test-only and are not formal program output.",
            "The frozen oracle and templates are unchanged.",
        ],
    }
    STATE.results = results
    STATE.save()

    try:
        if not all(row["match"] for row in results["tools"].values()):
            raise RuntimeError("frozen tool SHA mismatch")
        for n in N_VALUES:
            case = BT.oracle_case(n)
            comparison = compare_oracle(n, frozen[n], case)
            results["oracle"]["cases"].append(comparison)
        templates = BT.read_templates()

        # Generate all R=1 variants and the R=2 correct case.
        generated = {}
        for n in N_VALUES:
            case = BT.oracle_case(n)
            for rounds in ROUNDS:
                base = out / "generated" / f"n{n}-r{rounds}"
                variants = ("correct",) if rounds == 2 else ("correct", *VARIANTS)
                generated[(n, rounds)] = {}
                for variant in variants:
                    row = make_case(base, templates, case, variant, rounds)
                    generated[(n, rounds)][variant] = row
                    results["generated"].setdefault(f"n{n}-r{rounds}", {})[variant] = {
                        "source": row["source"],
                        "reference": row["reference"],
                        "manifest": row["manifest"],
                        "expected": row["expected"],
                        "expected_exit": row["expected_exit"],
                    }
                    STATE.save()

        for n in N_VALUES:
            for rounds in ROUNDS:
                correct = generated[(n, rounds)]["correct"]
                group = {"n": n, "rounds": rounds, "case": correct,
                         "builds": {}, "helper": {}, "cpp_ir": {},
                         "runs": [], "observer": {}, "round_ir": {},
                         "round_controls": {}}
                results["groups"].append(group)
                for opt in (2, 0):
                    source_build = attempt(
                        f"n={n} r={rounds} Source O{opt} build",
                        lambda c=correct, o=opt: build_source(c, o))
                    cpp_build = attempt(
                        f"n={n} r={rounds} C++ O{opt} build",
                        lambda c=correct, o=opt: build_cpp(c, o))
                    group["builds"][f"langc-O{opt}"] = source_build
                    group["builds"][f"cpp-O{opt}"] = cpp_build
                    if source_build is not None:
                        attempt(f"n={n} r={rounds} Source O{opt} ELF",
                                lambda b=source_build: elf_check(
                                    b["program"]["path"], "langc",
                                    f"n={n} r={rounds} Source O{opt}"))
                        helper = attempt(
                            f"n={n} r={rounds} Source O{opt} helper",
                            lambda c=correct, b=source_build: helper_export(c, b))
                        if helper is not None:
                            group["helper"][f"O{opt}"] = helper
                    if cpp_build is not None:
                        attempt(f"n={n} r={rounds} C++ O{opt} ELF",
                                lambda b=cpp_build: elf_check(
                                    b["program"]["path"], "cpp",
                                    f"n={n} r={rounds} C++ O{opt}"))
                        ir = attempt(
                            f"n={n} r={rounds} C++ O{opt} IR",
                            lambda c=correct, o=opt: build_cpp_ir(c, o))
                        if ir is not None:
                            group["cpp_ir"][f"O{opt}"] = ir
                    STATE.save()

                for opt in (2, 0):
                    for implementation in ("langc", "cpp"):
                        build = group["builds"].get(f"{implementation}-O{opt}")
                        label = f"n={n} r={rounds} {implementation} O{opt}"
                        if build is None:
                            group["runs"].append({
                                "label": label, "status": "not_run",
                                "reason": "build failed"})
                            continue
                        run_row = attempt(
                            f"{label} execute",
                            lambda b=build, l=label: execute(
                                b["program"]["path"], 0, l))
                        if run_row is not None:
                            group["runs"].append(run_row)
                        STATE.save()

                if group["builds"].get("langc-O2") is not None:
                    observer = attempt(
                        f"n={n} r={rounds} Source observer build",
                        lambda c=correct, b=group["builds"]["langc-O2"]:
                        build_observer(c, b, None))
                    if observer is not None:
                        checked = attempt(
                            f"n={n} r={rounds} Source lifecycle",
                            lambda c=correct, o=observer: run_observer(c, o))
                        if checked is not None:
                            group["observer"]["langc"] = checked
                if group["builds"].get("cpp-O2") is not None:
                    observer = attempt(
                        f"n={n} r={rounds} C++ observer build",
                        lambda c=correct: build_observer(
                            c, None, Path(c["reference"]["path"])))
                    if observer is not None:
                        checked = attempt(
                            f"n={n} r={rounds} C++ lifecycle",
                            lambda c=correct, o=observer: run_observer(c, o))
                        if checked is not None:
                            group["observer"]["cpp"] = checked
                STATE.save()

        # Round-work evidence on actual O2 IR.  Actual R=1 IR must be rejected
        # by the R=2 checker after its own external verify has passed.
        for group in results["groups"]:
            n, rounds, case = group["n"], group["rounds"], group["case"]["case"]
            source_ir = None
            helper = group["helper"].get("O2")
            if helper is not None:
                source_ir = helper["helper"]["after"]["path"]
            cpp_ir = group["cpp_ir"].get("O2")
            if source_ir is not None and cpp_ir is not None and rounds == 2:
                group["round_ir"]["langc"] = attempt(
                    f"n={n} R2 Source IR check",
                    lambda p=source_ir: check_round_ir(
                        p, "langc", f"n={n} R2 Source IR", case))
                group["round_ir"]["cpp"] = attempt(
                    f"n={n} R2 C++ IR check",
                    lambda p=cpp_ir["ir"]["path"]: check_round_ir(
                        p, "cpp", f"n={n} R2 C++ IR", case))
                for implementation, row in group["round_ir"].items():
                    if row is not None and not row["accepted_as_two_rounds"]:
                        results["failures"].append({
                            "label": f"n={n} R2 {implementation} IR",
                            "error": "R2 work not established; legal optimization unhandled"})
                    if row is not None and row["accepted_as_two_rounds"]:
                        branch = row["branch"]
                        for kind, literal in (
                                ("constant-one-trip-early-exit",
                                 branch["exit_bool"]),
                                ("forced-backedge", branch["backedge_bool"])):
                            source_path = Path(source_ir if implementation == "langc"
                                               else cpp_ir["ir"]["path"])
                            mutant = (STATE.out / "round-controls" / f"n{n}" /
                                      f"{implementation}-{kind}.ll")
                            mutant.parent.mkdir(parents=True, exist_ok=True)
                            text = source_path.read_text(encoding="utf-8")
                            old_line = branch["line"]
                            if text.count(old_line) != 1:
                                results["failures"].append({
                                    "label": f"n={n} R2 {implementation} {kind}",
                                    "error": "outer branch line is not unique"})
                                continue
                            new_line = old_line.replace(
                                f"br i1 {branch['condition']}",
                                f"br i1 {literal}", 1)
                            if new_line == old_line:
                                results["failures"].append({
                                    "label": f"n={n} R2 {implementation} {kind}",
                                    "error": "could not mutate outer branch"})
                                continue
                            mutant.write_text(text.replace(old_line, new_line, 1),
                                              encoding="utf-8")
                            verify = attempt(
                                f"n={n} R2 {implementation} {kind} verify",
                                lambda p=mutant: tool([
                                    LLVM_BIN / "opt", "-passes=verify",
                                    "-disable-output", p], ARGS, ENV))
                            checked = attempt(
                                f"n={n} R2 {implementation} {kind} check",
                                lambda p=mutant, i=implementation, k=kind:
                                check_round_ir(p, i,
                                               f"n={n} R2 {i} {k}", case))
                            row_control = {
                                "implementation": implementation, "kind": kind,
                                "literal": literal,
                                "mutant": file_rec(mutant, STATE.out),
                                "verify": verify, "checker": checked,
                                "verify_exit": verify["exit"] if verify else None,
                                "rejected_as_two_rounds": bool(
                                    checked is not None
                                    and not checked["accepted_as_two_rounds"]),
                            }
                            group["round_controls"][f"{implementation}-{kind}"] = row_control
                            if (verify is None or verify["exit"] != 0
                                    or not row_control["rejected_as_two_rounds"]):
                                results["failures"].append({
                                    "label": f"n={n} R2 {implementation} {kind}",
                                    "error": "mutant was not verified and rejected"})
            if rounds == 1 and source_ir is not None and cpp_ir is not None:
                group["round_ir"]["langc"] = attempt(
                    f"n={n} R1 Source IR control",
                    lambda p=source_ir: check_round_ir(
                        p, "langc", f"n={n} R1 Source IR control", case))
                group["round_ir"]["cpp"] = attempt(
                    f"n={n} R1 C++ IR control",
                    lambda p=cpp_ir["ir"]["path"]: check_round_ir(
                        p, "cpp", f"n={n} R1 C++ IR control", case))
                for implementation, row in group["round_ir"].items():
                    if row is not None and row["accepted_as_two_rounds"]:
                        results["failures"].append({
                            "label": f"n={n} R1 {implementation} IR control",
                            "error": "R1 IR was accepted as two rounds"})
            STATE.save()

        # Wrong expectations must be rejected by both normal implementations.
        for n in N_VALUES:
            for variant in VARIANTS:
                wrong = generated[(n, 1)][variant]
                for implementation, build_fn in (("langc", build_source),
                                                  ("cpp", build_cpp)):
                    label = f"n={n} R1 {implementation} O2 {variant}"
                    build = attempt(label + " build",
                                    lambda c=wrong, fn=build_fn: fn(c, 2))
                    if build is None:
                        results["wrong_controls"].append({
                            "label": label, "status": "not_run",
                            "reason": "build failed"})
                        continue
                    run_row = attempt(
                        label + " execute",
                        lambda b=build, l=label, e=wrong["expected_exit"]:
                        execute(b["program"]["path"], e, l))
                    if run_row is not None:
                        run_row.update({"variant": variant, "n": n,
                                        "implementation": implementation,
                                        "expected_exit": wrong["expected_exit"]})
                        results["wrong_controls"].append(run_row)
                    STATE.save()

        after = snapshot()
        results["input_snapshot_after"] = after
        results["inputs_unchanged"] = before == after
        if before != after:
            results["failures"].append({
                "label": "input snapshot",
                "error": "tools/templates/oracle/observer/scripts changed"})

        normal_runs = [run for group in results["groups"]
                       for run in group["runs"]]
        normal_ok = (len(normal_runs) == 16
                     and all(row.get("completed", False) for row in normal_runs))
        wrong_ok = (len(results["wrong_controls"]) == 16
                    and all(row.get("completed", False)
                            for row in results["wrong_controls"]))
        helper_ok = all(
            group["helper"].get(f"O{opt}", {}).get("helper", {}).get("object_match")
            for group in results["groups"] for opt in (2, 0))
        round_rows = [row for group in results["groups"]
                      for row in group["round_ir"].values()]
        r2_rows = [row for group in results["groups"] if group["rounds"] == 2
                   for row in group["round_ir"].values()]
        r1_rows = [row for group in results["groups"] if group["rounds"] == 1
                   for row in group["round_ir"].values()]
        round_control_rows = [row for group in results["groups"]
                              for row in group["round_controls"].values()]
        round_ok = (len(r2_rows) == 4 and all(row is not None
                                              and row["accepted_as_two_rounds"]
                                              for row in r2_rows)
                    and len(r1_rows) == 4 and all(row is not None
                                                  and not row["accepted_as_two_rounds"]
                                                  for row in r1_rows)
                    and len(round_control_rows) == 8
                    and all(row["verify_exit"] == 0
                            and row["rejected_as_two_rounds"]
                            for row in round_control_rows))
        lifecycle_rows = [group["observer"][implementation]
                          for group in results["groups"]
                          for implementation in ("langc", "cpp")
                          if implementation in group["observer"]]
        lifecycle_ok = (len(lifecycle_rows) == 8
                        and all(row["verification"]["status"] == "passed"
                                for row in lifecycle_rows))
        elf_ok = (len(results["elf_checks"]) == 16
                  and all(row["success"] for row in results["elf_checks"]))
        prepare_ok = (normal_ok and wrong_ok and helper_ok and elf_ok
                      and not results["failures"])
        results["summary"] = {
            "normal_planned": 16,
            "normal_completed": sum(row.get("completed", False)
                                    for row in normal_runs),
            "normal_timeouts": sum(row["command"]["status"] == "timeout"
                                   for row in normal_runs
                                   if "command" in row),
            "wrong_controls_planned": 16,
            "wrong_controls_completed": sum(
                row.get("completed", False) for row in results["wrong_controls"]),
            "source_helper_object_matches": sum(
                group["helper"].get(f"O{opt}", {}).get("helper", {}).get(
                    "object_match", False)
                for group in results["groups"] for opt in (2, 0)),
            "cpp_ir_verified": sum(
                group["cpp_ir"].get(f"O{opt}", {}).get("verify", {}).get(
                    "success", False)
                for group in results["groups"] for opt in (2, 0)),
            "r2_ir_accepted": sum(row is not None and
                                  row["accepted_as_two_rounds"] for row in r2_rows),
            "r1_ir_rejected_as_r2": sum(row is not None and
                                        not row["accepted_as_two_rounds"]
                                        for row in r1_rows),
            "round_negative_controls": len(round_control_rows),
            "round_negative_controls_verified_and_rejected": sum(
                row["verify_exit"] == 0 and row["rejected_as_two_rounds"]
                for row in round_control_rows),
            "lifecycle_groups_checked": len(lifecycle_rows),
            "elf_checks_passed": sum(row["success"] for row in results["elf_checks"]),
            "failures": len(results["failures"]),
        }
        results["prepare_status"] = "passed" if prepare_ok else "failed"
        results["round_work_status"] = "passed" if round_ok else "incomplete"
        results["lifecycle_status"] = "passed" if lifecycle_ok else "incomplete"
        results["status"] = ("passed" if prepare_ok and round_ok and lifecycle_ok
                            else "incomplete" if not results["failures"]
                            else "failed")
        results["finished_at"] = utc_now()
        STATE.save()
        return 0 if results["status"] == "passed" else 1
    except Exception as error:
        results["status"] = "failed"
        results["finished_at"] = utc_now()
        results["failures"].append({"label": "unhandled error",
                                    "error": str(error),
                                    "traceback": traceback.format_exc()})
        try:
            after = snapshot()
            results["input_snapshot_after"] = after
            results["inputs_unchanged"] = before == after
        except Exception:
            pass
        STATE.save()
        return 1


if __name__ == "__main__":
    sys.exit(main())
