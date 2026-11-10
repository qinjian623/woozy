#!/usr/bin/env python3
"""Prepare and measure one fixed u32 sort comparison.

``--algorithm heap`` (the default) uses the max-heap sift-down fixture;
``--algorithm quick`` uses the three-way quicksort fixture; ``--algorithm
merge`` uses the stable bottom-up merge sort fixture with one full-size work
buffer.  Each selection has
its own templates, task, operation ID, report schemas, and metadata.  One Source
program and one hand-written C++ program are generated from the same templates,
LCG sequence, Python sorted golden array, round count, and per-element check.
``--prepare-only`` builds/runs the primary program at O0/O2, verifies exported
LLVM IR and objects, and rejects a deliberate high-bit-wrong golden.  The
default stage reuses a passed prepare report and records only whole-process
wall-clock samples; it never calls the result a pure sort-function time.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import sys
import traceback
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_native as common  # noqa: E402

CONTRACT = "compiler-dev/1"
PERFORMANCE_DIR = Path(__file__).resolve().parent
ALGORITHMS = {
    "heap": {
        "task": "native-heapsort-benchmark",
        "operation_id": "language-heapsort-benchmark-20261105",
        "prepare_schema": "compiler-dev/native-heapsort-preparation/1",
        "measure_schema": "compiler-dev/native-heapsort-measurement/1",
        "self_test_schema": "compiler-dev/native-heapsort-self-test/1",
        "arrays_schema": "compiler-dev/native-heapsort-arrays/1",
        "directory": PERFORMANCE_DIR / "sort",
        "source_template": "heap.wz.in",
        "reference_template": "reference.cpp.in",
        "manifest_name": "perf_sort_heap",
        "display": "heap-sort",
        "description": "max-heap sift-down heapsort",
        "sort_symbol": "heapsort",
    },
    "quick": {
        "task": "native-quicksort-benchmark",
        "operation_id": "language-quicksort-benchmark-20261105",
        "prepare_schema": "compiler-dev/native-quicksort-preparation/1",
        "measure_schema": "compiler-dev/native-quicksort-measurement/1",
        "self_test_schema": "compiler-dev/native-quicksort-self-test/1",
        "arrays_schema": "compiler-dev/native-quicksort-arrays/1",
        "directory": PERFORMANCE_DIR / "quicksort",
        "source_template": "quick.wz.in",
        "reference_template": "reference.cpp.in",
        "manifest_name": "perf_sort_quick",
        "display": "quicksort",
        "description": "in-place three-way quicksort",
        "sort_symbol": "quicksort",
    },
    "merge": {
        "task": "native-mergesort-benchmark",
        "operation_id": "language-mergesort-benchmark-20261109",
        "prepare_schema": "compiler-dev/native-mergesort-preparation/1",
        "measure_schema": "compiler-dev/native-mergesort-measurement/1",
        "self_test_schema": "compiler-dev/native-mergesort-self-test/1",
        "arrays_schema": "compiler-dev/native-mergesort-arrays/1",
        "directory": PERFORMANCE_DIR / "mergesort",
        "source_template": "merge.wz.in",
        "reference_template": "reference.cpp.in",
        "manifest_name": "perf_sort_merge",
        "display": "merge-sort",
        "description": "stable bottom-up merge sort with one full-size work buffer",
        "sort_symbol": "mergesort",
    },
}
DEFAULT_ALGORITHM = "heap"
DEFAULT_N = 65536
DEFAULT_SEED = 0x6D2B79F5
DEFAULT_ROUNDS = 16
MASK32 = (1 << 32) - 1
TARGET_FLAGS = (
    "--target=x86_64-unknown-linux-gnu", "-march=x86-64", "-mtune=generic",
    "-fno-fast-math", "-fno-lto",
)
PerfError = common.PerfError


def need(condition, message):
    if not condition:
        raise PerfError(message)


def now():
    return datetime.now(timezone.utc).isoformat()


def rel(base, path):
    return str(Path(path).resolve().relative_to(Path(base).resolve()))


def file_rec(path, base=None):
    path = Path(path).resolve()
    record = {"path": str(path), "sha256": common.sha256_file(path),
              "size_bytes": path.stat().st_size}
    if base is not None:
        record["rel"] = rel(base, path)
    return record


def script_rec():
    return file_rec(Path(__file__).resolve())


def common_rec():
    return file_rec(Path(common.__file__).resolve())


def algorithm_config(algorithm):
    need(algorithm in ALGORITHMS, f"unknown sort algorithm: {algorithm}")
    return ALGORITHMS[algorithm]


def template_names(config):
    return (config["source_template"], config["reference_template"])


def template_recs(algorithm):
    config = algorithm_config(algorithm)
    return {name: file_rec(config["directory"] / name)
            for name in template_names(config)}


def templates(algorithm):
    config = algorithm_config(algorithm)
    return {name: (config["directory"] / name).read_text(encoding="utf-8")
            for name in template_names(config)}


def lcg_values(n, seed):
    state, values = seed & MASK32, []
    for _ in range(n):
        state = (state * 1664525 + 1013904223) & MASK32
        values.append(state)
    return values


def seq_sha(values):
    return common.sha256_text("".join(f"{value}\n" for value in values))


def format_values(values):
    lines = []
    for start in range(0, len(values), 16):
        chunk = ", ".join(str(value) for value in values[start:start + 16])
        lines.append("        " + chunk + ("," if start + 16 < len(values) else ""))
    return "\n".join(lines)


def render(template, raw, golden, n, seed, rounds):
    text = template
    for key, value in (("{{ARRAY}}", format_values(raw)),
                       ("{{EXPECTED}}", format_values(golden)),
                       ("{{N}}", str(n)), ("{{SEED}}", hex(seed)),
                       ("{{ROUNDS}}", str(rounds))):
        text = text.replace(key, value)
    need("{{" not in text, "rendered template still has a placeholder")
    return text


def manifest_text(name):
    return ("schema_version = 1\n\n[package]\n"
            f'name = "{name}"\nsource_root = "src"\nentry = "app"\n\n'
            '[build]\nbackend = "native"\nword_bits = 64\n')


def parameters(args):
    return {"algorithm": args.algorithm, "n": args.n, "seed": args.seed,
            "seed_hex": hex(args.seed), "rounds": args.rounds}


def new_out(path):
    path = Path(path).resolve()
    need(not path.exists(), f"--out already exists, refusing to overwrite: {path}")
    path.mkdir(parents=True, exist_ok=False)
    return path


def target_record(args):
    return {"triple": "x86_64-unknown-linux-gnu", "generic_cpu": True,
            "march_native": False, "cpp_flags": list(TARGET_FLAGS),
            "cpp_linker": str(args.llvm_bin / "ld.lld"),
            "fast_math": False, "lto": False}


def base_report(mode, args, env, config):
    report = common.base_report(mode, args, env)
    report.update({
        "schema": config["prepare_schema"] if mode == "prepare" else
                  config["measure_schema"],
        "task": config["task"], "contract": CONTRACT,
        "operation_id": config["operation_id"], "algorithm": args.algorithm,
        "script": script_rec(), "common": common_rec(),
        "templates": template_recs(args.algorithm), "parameters": parameters(args),
        "target": target_record(args)})
    return report


def require_report_algorithm(report, path, algorithm):
    config = algorithm_config(algorithm)
    need(report.get("algorithm") == algorithm,
         f"{path}: prepare algorithm {report.get('algorithm')!r} does not match "
         f"requested {algorithm!r}")
    need(report.get("schema") == config["prepare_schema"],
         f"{path}: unexpected {algorithm} prepare schema")
    need(report.get("task") == config["task"] and report.get("contract") == CONTRACT,
         f"{path}: {algorithm} task/contract mismatch")
    need(report.get("operation_id") == config["operation_id"],
         f"{path}: {algorithm} operation_id mismatch")
    return config


def tool(argv, args, env):
    return common.command(argv, timeout=args.timeout, env=env, cpu=args.cpu)


def run_program(argv, args, env):
    return common.command(argv, timeout=args.run_timeout, env=env, cpu=args.cpu)


def elf(path, executable, label):
    return common.checked_elf(path, executable=executable, label=label)


def verify_ir(path, args, env, report, label):
    result = tool([args.llvm_bin / "opt", "-passes=verify", "-disable-output", path], args, env)
    row = {"path": str(path), "sha256": common.sha256_file(path),
           "command_index": result["command_index"], "success": False}
    report["verifies"].append(row)
    common.require_success(result, label)
    row["success"] = True
    return row


def work_info(path, args, env, report):
    text = Path(path).read_text(encoding="utf-8")
    loops = tool([args.llvm_bin / "opt", "-passes=print<loops>", "-disable-output", path], args, env)
    loop_row = {"path": str(path), "command_index": loops["command_index"], "success": False,
                "loop_count": 0}
    report["loop_analyses"].append(loop_row)
    common.require_success(loops, f"loop analysis {path}")
    loop_row["loop_count"] = loops["stderr"].count("Loop at depth")
    loop_row["loop_present"] = loop_row["loop_count"] > 0
    loop_row["success"] = True
    counts = {name: len(re.findall(pattern, text)) for name, pattern in (
        ("load", r"\bload\b"), ("store", r"\bstore\b"), ("icmp", r"\bicmp\b"),
        ("br", r"\bbr\b"), ("phi", r"\bphi\b"))}
    return {"path": str(path), "sha256": common.sha256_file(path),
            "size_bytes": Path(path).stat().st_size,
            "loop_count": loop_row["loop_count"], "work_counts": counts}


def require_work(info, label):
    counts = info["work_counts"]
    need(info["loop_count"] >= 2 and counts["load"] and counts["store"] and counts["icmp"],
         f"{label}: optimized IR lacks sorting loops/accesses: loops={info['loop_count']} {counts}")


def write_arrays(out, name, raw, golden, params, algorithm):
    config = algorithm_config(algorithm)
    path = out / "arrays" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    data = {"schema": config["arrays_schema"], "algorithm": algorithm,
            "parameters": params,
            "lcg": {"multiplier": 1664525, "increment": 1013904223, "mask": "0xffffffff"},
            "raw": raw, "golden": golden, "raw_sequence_sha256": seq_sha(raw),
            "golden_sequence_sha256": seq_sha(golden)}
    common.write_json(path, data)
    record = file_rec(path, out)
    record.update({"raw_sequence_sha256": data["raw_sequence_sha256"],
                   "golden_sequence_sha256": data["golden_sequence_sha256"],
                   "raw_first": raw[0] if raw else None, "raw_last": raw[-1] if raw else None,
                   "golden_first": golden[0] if golden else None,
                   "golden_last": golden[-1] if golden else None})
    return record


def generate_sources(out, template_map, raw, golden, params, name, algorithm):
    config = algorithm_config(algorithm)
    directory = out / name
    (directory / "src").mkdir(parents=True, exist_ok=False)
    (directory / "src" / "main.wz").write_text(
        render(template_map[config["source_template"]], raw, golden, params["n"],
               params["seed"], params["rounds"]), encoding="utf-8")
    (directory / "reference.cpp").write_text(
        render(template_map[config["reference_template"]], raw, golden, params["n"],
               params["seed"], params["rounds"]), encoding="utf-8")
    (directory / "lang.toml").write_text(manifest_text(config["manifest_name"]),
                                         encoding="utf-8")
    return {"directory": str(directory),
            "manifest": file_rec(directory / "lang.toml", out),
            "source": file_rec(directory / "src" / "main.wz", out),
            "reference": file_rec(directory / "reference.cpp", out)}


def prepare(args):
    config = algorithm_config(args.algorithm)
    env, out, report = common.make_env(args), new_out(args.out), None
    report = {"schema": config["prepare_schema"], "task": config["task"],
              "contract": CONTRACT, "operation_id": config["operation_id"],
              "algorithm": args.algorithm, "mode": "prepare", "status": "running",
              "commands": [], "verifies": [], "loop_analyses": []}
    common._COMMAND_LOG = report["commands"]
    try:
        report.update(base_report("prepare", args, env, config))
        template_map = templates(args.algorithm)
        params = parameters(args)
        raw, golden = lcg_values(args.n, args.seed), None
        golden = sorted(raw)
        report["arrays"] = write_arrays(out, "primary.json", raw, golden, params,
                                        args.algorithm)
        source = generate_sources(out, template_map, raw, golden, params, "source",
                                  args.algorithm)
        manifest = out / source["manifest"]["rel"]
        primary = {"source": source, "builds": [], "executions": [], "helper": [],
                   "work": {}, "cpp_ir_o2": None}
        report["primary"] = primary
        check = tool([args.langc, "check", "--project", manifest, "--backend", "native",
                      "--format", "json"], args, env)
        check_row = {"command_index": check["command_index"], "json": None, "success": False}
        primary["check"] = check_row
        common.require_success(check, "primary langc check")
        check_json = common.parse_json_stdout(check, "primary langc check")
        check_row["json"] = check_json
        need(check_json.get("status") == "ok", f"primary langc check not ok: {check_json}")
        check_row["success"] = True

        for opt in (0, 2):
            langc_dir = out / "build" / "langc" / f"O{opt}"
            langc_dir.mkdir(parents=True, exist_ok=False)
            build = tool([args.langc, "build", "--project", manifest, "--backend", "native",
                          "--opt", str(opt), "--out-dir", langc_dir, "--format", "json"], args, env)
            row = {"implementation": "langc", "opt": opt,
                   "command_index": build["command_index"], "success": False}
            primary["builds"].append(row)
            common.require_success(build, f"primary langc O{opt} build")
            build_json = common.parse_json_stdout(build, f"primary langc O{opt} build")
            row["json"] = build_json
            need(build_json.get("status") == "ok", f"primary langc O{opt} build not ok")
            program, obj = langc_dir / "program", langc_dir / "program.o"
            program_rec, obj_rec = file_rec(program, out), file_rec(obj, out)
            row.update({"program": program_rec, "object": obj_rec,
                        "elf": elf(program, True, f"primary langc O{opt}")})
            run = run_program([program], args, env)
            execution = {"implementation": "langc", "opt": opt,
                         "command_index": run["command_index"], "success": False}
            primary["executions"].append(execution)
            common.require_quiet_success(run, f"primary langc O{opt} program")
            execution.update({"success": True, "exit": run["exit"],
                              "stdout": run["stdout"], "stderr": run["stderr"],
                              "elapsed_ns": run["elapsed_ns"]})
            row["success"] = True

            cpp_dir = out / "build" / "cpp" / f"O{opt}"
            cpp_dir.mkdir(parents=True, exist_ok=False)
            compile_cpp = tool([args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20",
                                f"-O{opt}", f"--ld-path={args.llvm_bin / 'ld.lld'}",
                                out / source["reference"]["rel"], "-o", cpp_dir / "reference"],
                               args, env)
            cpp_row = {"implementation": "cpp", "opt": opt,
                       "command_index": compile_cpp["command_index"], "success": False}
            primary["builds"].append(cpp_row)
            common.require_success(compile_cpp, f"primary cpp O{opt} build")
            cpp_row.update({"program": file_rec(cpp_dir / "reference", out),
                            "elf": elf(cpp_dir / "reference", True, f"primary cpp O{opt}")})
            run = run_program([cpp_dir / "reference"], args, env)
            execution = {"implementation": "cpp", "opt": opt,
                         "command_index": run["command_index"], "success": False}
            primary["executions"].append(execution)
            common.require_quiet_success(run, f"primary cpp O{opt} reference")
            execution.update({"success": True, "exit": run["exit"],
                              "stdout": run["stdout"], "stderr": run["stderr"],
                              "elapsed_ns": run["elapsed_ns"]})
            cpp_row["success"] = True

            llvm_dir = out / "llvm" / "langc" / f"O{opt}"
            llvm_dir.mkdir(parents=True, exist_ok=False)
            helper = tool([args.helper, "--project", manifest, "--opt", str(opt),
                           "--out-object", llvm_dir / "program.o",
                           "--out-llvm", llvm_dir / "after.ll",
                           "--out-llvm-before", llvm_dir / "before.ll",
                           "--out-native", llvm_dir / "program.nir", "--format", "json"], args, env)
            helper_row = {"opt": opt, "command_index": helper["command_index"],
                          "object": file_rec(llvm_dir / "program.o", out),
                          "object_match": False, "success": False}
            primary["helper"].append(helper_row)
            common.require_success(helper, f"primary helper O{opt}")
            helper_json = common.parse_json_stdout(helper, f"primary helper O{opt}")
            helper_row["json"] = helper_json
            need(helper_json.get("status") in ("ok", "passed"),
                 f"primary helper O{opt} status is not ok/passed")
            need(helper_json.get("llvm_verified_before") and helper_json.get("llvm_verified_after"),
                 f"primary helper O{opt} did not verify both IR forms")
            helper_row["object_match"] = obj.read_bytes() == (llvm_dir / "program.o").read_bytes()
            need(helper_row["object_match"], f"primary O{opt}: program.o and helper object differ")
            for kind, filename in (("before", "before.ll"), ("after", "after.ll")):
                ir_path = llvm_dir / filename
                row = verify_ir(ir_path, args, env, report, f"primary O{opt} {kind} IR verify")
                primary.setdefault("llvm", []).append({"opt": opt, "kind": kind, **row})
            work = work_info(llvm_dir / "after.ll", args, env, report)
            primary["work"][f"langc_O{opt}"] = work
            if opt == 2:
                require_work(work, "primary langc O2")
            helper_row["success"] = True

        cpp_ir_dir = out / "llvm" / "cpp" / "O2"
        cpp_ir_dir.mkdir(parents=True, exist_ok=False)
        cpp_ir = cpp_ir_dir / "reference.ll"
        compile_ir = tool([args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20", "-O2",
                           "-S", "-emit-llvm", out / source["reference"]["rel"],
                           "-o", cpp_ir], args, env)
        common.require_success(compile_ir, "primary cpp O2 LLVM IR")
        cpp_verify = verify_ir(cpp_ir, args, env, report, "primary cpp O2 LLVM IR verify")
        cpp_work = work_info(cpp_ir, args, env, report)
        require_work(cpp_work, "primary cpp O2")
        primary["cpp_ir_o2"] = {"compile_command_index": compile_ir["command_index"],
                                "verify": cpp_verify, "work": cpp_work}

        # High-bit-wrong golden control using the same templates.
        n, control_raw = 8, lcg_values(8, args.seed)
        control_golden = sorted(control_raw)
        wrong, flip_index = list(control_golden), 4
        wrong[flip_index] ^= 1 << 31
        need(wrong[flip_index] != control_golden[flip_index], "wrong-golden value did not change")
        need((wrong[flip_index] & 0xFF) == (control_golden[flip_index] & 0xFF),
             "wrong-golden control changed low 8 bits")
        control_params = {"algorithm": args.algorithm, "n": n, "seed": args.seed,
                          "seed_hex": hex(args.seed), "rounds": 1}
        control_arrays = write_arrays(out, "wrong-golden.json", control_raw, wrong,
                                      control_params, args.algorithm)
        control_source = generate_sources(out, template_map, control_raw, wrong,
                                          control_params, "wrong-golden",
                                          args.algorithm)
        control = {"algorithm": args.algorithm, "parameters": control_params,
                   "arrays": control_arrays,
                   "source": control_source, "correct_value": control_golden[flip_index],
                   "wrong_value": wrong[flip_index], "flip_index": flip_index,
                   "high_bit_flipped": 1 << 31, "low8_preserved": True,
                   "builds": [], "executions": [], "summary": {}}
        report["control"] = control
        control_manifest = out / control_source["manifest"]["rel"]
        for opt in (0, 2):
            for implementation in ("langc", "cpp"):
                build_dir = out / "wrong-golden-build" / implementation / f"O{opt}"
                build_dir.mkdir(parents=True, exist_ok=False)
                if implementation == "langc":
                    build = tool([args.langc, "build", "--project", control_manifest,
                                  "--backend", "native", "--opt", str(opt),
                                  "--out-dir", build_dir, "--format", "json"], args, env)
                    program = build_dir / "program"
                else:
                    build = tool([args.llvm_bin / "clang++", *TARGET_FLAGS, "-std=c++20",
                                  f"-O{opt}", f"--ld-path={args.llvm_bin / 'ld.lld'}",
                                  out / control_source["reference"]["rel"],
                                  "-o", build_dir / "reference"], args, env)
                    program = build_dir / "reference"
                build_row = {"implementation": implementation, "opt": opt,
                             "command_index": build["command_index"], "success": False}
                control["builds"].append(build_row)
                common.require_success(build, f"wrong-golden {implementation} O{opt} build")
                build_row["elf"] = elf(program, True, f"wrong-golden {implementation} O{opt}")
                build_row["program"] = file_rec(program, out)
                build_row["success"] = True
                run = run_program([program], args, env)
                execution = {"implementation": implementation, "opt": opt,
                             "command_index": run["command_index"], "expected_exit": 1,
                             "success": False}
                control["executions"].append(execution)
                common.require_mismatch(run, f"wrong-golden {implementation} O{opt} run")
                execution.update({"success": True, "exit": run["exit"],
                                  "stdout": run["stdout"], "stderr": run["stderr"]})
        control["summary"] = {
            "builds": len(control["builds"]),
            "rejections": sum(1 for row in control["executions"] if row["success"]),
            "all_exit_1_quiet": all(row["success"] and row["exit"] == 1 and not row["stdout"]
                                    and not row["stderr"] for row in control["executions"]),
            "failure_commands_preserved": sum(1 for record in report["commands"]
                                              if record["status"] == "exited" and record["exit"] == 1),
        }
        need(control["summary"] == {"builds": 4, "rejections": 4,
                                    "all_exit_1_quiet": True,
                                    "failure_commands_preserved": 4},
             f"wrong-golden control summary is unexpected: {control['summary']}")

        report["summary"] = {
            "parameters": params, "primary_correct_executions": len(primary["executions"]),
            "primary_builds": len(primary["builds"]), "llvm_ir_verifies": len(report["verifies"]),
            "source_ir_verifies": len(primary.get("llvm", [])),
            "object_matches": sum(1 for row in primary["helper"] if row["object_match"]),
            "cpp_o2_ir_verified": bool(primary["cpp_ir_o2"]), "o2_sorting_work_checks": 2,
            "wrong_golden_builds": control["summary"]["builds"],
            "wrong_golden_rejections": control["summary"]["rejections"],
            "wrong_golden_failure_commands": control["summary"]["failure_commands_preserved"],
        }
        report["limitations"] = limitations(args.algorithm)
        report["not_run"] = not_run_items(args.algorithm, formal=False)
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["failure"] = {"type": type(error).__name__, "message": str(error),
                             "traceback": traceback.format_exc()}
        report.setdefault("limitations", limitations(args.algorithm))
        report.setdefault("not_run", not_run_items(args.algorithm, formal=False))
    finally:
        report["finished_at"] = now()
        report["loadavg_after"] = common.host_info()["loadavg"]
        common.write_json(out / "prepare.json", report)
        common._COMMAND_LOG = None
        print(json.dumps({"status": report["status"], "mode": "prepare", "out": str(out),
                          "report": str(out / "prepare.json"),
                          "summary": report.get("summary"),
                          "failure": report.get("failure")},
                         indent=2, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


def load_prepare_report(path, args, current_build_tree, current_host):
    path = Path(path).resolve()
    need(path.is_file(), f"missing prepare report: {path}")
    report = json.loads(path.read_text(encoding="utf-8"))
    require_report_algorithm(report, path, args.algorithm)
    need(report.get("status") == "passed", f"{path}: prepare status is not passed")
    need(report.get("script", {}).get("sha256") == script_rec()["sha256"],
         f"{path}: prepare used a different script version")
    need(report.get("common", {}).get("sha256") == common_rec()["sha256"],
         f"{path}: common run_native.py changed since prepare")
    need(report.get("templates") == template_recs(args.algorithm),
         f"{path}: {args.algorithm} templates changed")
    need(report.get("parameters") == parameters(args),
         f"{path}: algorithm/N/seed/rounds changed")
    need(report.get("target") == target_record(args), f"{path}: target flags changed")
    need(report.get("host", {}).get("cpu_model") == current_host.get("cpu_model"),
         f"{path}: CPU model differs from prepare")
    old_tree = report.get("build_tree", {})
    need(old_tree.get("cmake_build_type") == current_build_tree.get("cmake_build_type"),
         f"{path}: CMAKE_BUILD_TYPE changed")
    need(old_tree.get("defines") == current_build_tree.get("defines"),
         f"{path}: actual LANG_NATIVE_* paths changed")
    for key in ("build_ninja", "cmake_cache"):
        need(old_tree.get(key, {}).get("sha256") == current_build_tree.get(key, {}).get("sha256"),
             f"{path}: {key} changed")
    expected_tools = {"langc": args.langc, "helper": args.helper, "runtime": args.runtime,
                      "clang": args.llvm_bin / "clang", "ld.lld": args.llvm_bin / "ld.lld",
                      "clangxx": args.llvm_bin / "clang++", "opt": args.llvm_bin / "opt"}
    for name, tool_path in expected_tools.items():
        need(report.get("tools", {}).get(name, {}).get("sha256") ==
             common.sha256_file(tool_path), f"{path}: tool {name} SHA-256 changed")
    for name in ("helper", "runtime", "clang", "ld.lld"):
        need(old_tree.get("actual_tools", {}).get(name, {}).get("sha256") ==
             current_build_tree.get("actual_tools", {}).get(name, {}).get("sha256"),
             f"{path}: actual tool {name} SHA-256 changed")

    arrays = report.get("arrays", {})
    arrays_file = path.parent / arrays.get("rel", "")
    need(arrays_file.is_file() and common.sha256_file(arrays_file) == arrays.get("sha256"),
         f"{path}: primary arrays changed")
    array_data = json.loads(arrays_file.read_text(encoding="utf-8"))
    need(array_data.get("parameters") == report.get("parameters") and
         len(array_data.get("raw", [])) == args.n and
         len(array_data.get("golden", [])) == args.n and
         array_data.get("raw_sequence_sha256") == arrays.get("raw_sequence_sha256") and
         array_data.get("golden_sequence_sha256") == arrays.get("golden_sequence_sha256"),
         f"{path}: primary array parameters/sequence changed")
    primary = report.get("primary", {})
    for key, record in primary.get("source", {}).items():
        if isinstance(record, dict):
            file = path.parent / record["rel"]
            need(file.is_file() and common.sha256_file(file) == record["sha256"],
                 f"{path}: generated {key} changed")
    for row in primary.get("builds", []):
        file = path.parent / row["program"]["rel"]
        need(file.is_file() and common.sha256_file(file) == row["program"]["sha256"],
             f"{path}: prepared program changed: {file}")
        if row["implementation"] == "langc":
            obj = path.parent / row["object"]["rel"]
            need(obj.is_file() and common.sha256_file(obj) == row["object"]["sha256"],
                 f"{path}: prepared program.o changed: {obj}")
    for row in primary.get("helper", []):
        obj = path.parent / row["object"]["rel"]
        need(obj.is_file() and common.sha256_file(obj) == row["object"]["sha256"],
             f"{path}: prepared helper object changed: {obj}")
        need(row.get("object_match") is True, f"{path}: prepare did not record object equality")
        langc_row = next(build for build in primary["builds"]
                         if build["implementation"] == "langc" and build["opt"] == row["opt"])
        need((path.parent / langc_row["object"]["rel"]).read_bytes() == obj.read_bytes(),
             f"{path}: program.o and helper object bytes differ for O{row['opt']}")
    control = report.get("control", {})
    control_summary = control.get("summary", {})
    need(control_summary.get("builds") == 4 and control_summary.get("rejections") == 4 and
         control_summary.get("all_exit_1_quiet") is True,
         f"{path}: wrong-golden control did not pass")
    control_arrays = control.get("arrays", {})
    control_file = path.parent / control_arrays.get("rel", "")
    need(control_file.is_file() and common.sha256_file(control_file) == control_arrays.get("sha256"),
         f"{path}: wrong-golden arrays changed")
    for key, record in control.get("source", {}).items():
        if isinstance(record, dict):
            file = path.parent / record["rel"]
            need(file.is_file() and common.sha256_file(file) == record["sha256"],
                 f"{path}: wrong-golden {key} changed")
    need(len(control.get("builds", [])) == 4, f"{path}: missing wrong-golden builds")
    for build in control["builds"]:
        record = build["program"]
        file = path.parent / record["rel"]
        need(file.is_file() and common.sha256_file(file) == record["sha256"],
             f"{path}: wrong-golden program changed: {file}")
    return report, path


def measure(args):
    config = algorithm_config(args.algorithm)
    env, out = common.make_env(args), new_out(args.out)
    report = {"schema": config["measure_schema"], "task": config["task"],
              "contract": CONTRACT, "operation_id": config["operation_id"],
              "algorithm": args.algorithm, "mode": "measure", "status": "running",
              "commands": []}
    common._COMMAND_LOG = report["commands"]
    try:
        report.update(base_report("measure", args, env, config))
        prepared, prepared_path = load_prepare_report(
            args.prepare_report, args, report["build_tree"], report["host"])
        report["prepare"] = {"path": str(prepared_path),
                             "sha256": common.sha256_file(prepared_path),
                             "host": prepared.get("host"),
                             "affinity": prepared.get("affinity")}
        report["runs"] = []
        by_key = {(row["implementation"], row["opt"]): row
                  for row in prepared["primary"]["builds"]}
        for opt in (0, 2):
            implementations = {}
            for name in ("candidate", "reference"):
                implementation = "langc" if name == "candidate" else "cpp"
                prepared_row = by_key[(implementation, opt)]
                binary = prepared_path.parent / prepared_row["program"]["rel"]
                digest = common.sha256_file(binary)
                need(digest == prepared_row["program"]["sha256"],
                     f"O{opt} {name} binary changed before measurement")
                implementations[name] = {"path": str(binary), "sha256": digest,
                                         "warmups": [], "samples": []}
            row = {"opt": opt, "parameters": parameters(args),
                   "order": "alternating candidate/reference by sample index",
                   "implementations": implementations, "warnings": [],
                   "per_round_note": "whole-process wall time includes input generation, sorting, and per-element verification; it is not pure sort-function time"}
            report["runs"].append(row)
            for phase, count in (("warmup", args.warmups), ("measured", args.samples)):
                for index in range(count):
                    order = (("candidate", "reference") if index % 2 == 0
                             else ("reference", "candidate"))
                    for name in order:
                        run = run_program([implementations[name]["path"]], args, env)
                        sample = {"phase": phase, "index": index, "implementation": name,
                                  "command_index": run["command_index"],
                                  "elapsed_ns": run["elapsed_ns"], "status": run["status"],
                                  "exit": run["exit"], "stdout": run["stdout"],
                                  "stderr": run["stderr"], "success": False}
                        implementations[name][phase + "s" if phase == "warmup" else "samples"].append(sample)
                        common.require_quiet_success(
                            run, f"O{opt} {name} {phase} {index}")
                        sample["success"] = True
            for name in ("candidate", "reference"):
                stats = common.measured_stats(implementations[name]["samples"], args.samples)
                implementations[name]["stats"] = stats
                implementations[name]["whole_process_ns_per_round"] = stats["median_ns"] // args.rounds
                if stats["min_ns"] < 50_000_000:
                    row["warnings"].append(
                        f"{name} minimum {stats['min_ns']} ns is below 50 ms; "
                        "process startup/loader overhead may be non-negligible")
        report["summary"] = {
            "warmup_runs": sum(len(row["implementations"][name]["warmups"])
                               for row in report["runs"] for name in ("candidate", "reference")),
            "measured_runs": sum(len(row["implementations"][name]["samples"])
                                 for row in report["runs"] for name in ("candidate", "reference")),
            "warnings": sum(len(row["warnings"]) for row in report["runs"]),
            "threshold_applied": False,
        }
        report["limitations"] = limitations(args.algorithm)
        report["not_run"] = not_run_items(args.algorithm, formal=True)
        report["status"] = "passed"
    except Exception as error:
        report["status"] = "failed"
        report["failure"] = {"type": type(error).__name__, "message": str(error),
                             "traceback": traceback.format_exc()}
        report.setdefault("limitations", limitations(args.algorithm))
        report.setdefault("not_run", not_run_items(args.algorithm, formal=True))
    finally:
        report["finished_at"] = now()
        report["loadavg_after"] = common.host_info()["loadavg"]
        common.write_json(out / "measure.json", report)
        common._COMMAND_LOG = None
        print(json.dumps({"status": report["status"], "mode": "measure", "out": str(out),
                          "report": str(out / "measure.json"),
                          "summary": report.get("summary"),
                          "failure": report.get("failure")},
                         indent=2, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


def limitations(algorithm):
    config = algorithm_config(algorithm)
    return [
        f"Timing is whole-process wall time: process startup, loader, input generation, {config['display']}, and full per-element verification are included; there is no source-level clock or pure sort-function timing.",
        "The Source and C++ programs generate each round's input from the same fixed LCG seed and verify every element against an independent Python sorted golden array; the sorted previous round is overwritten before each sort.",
        "No LTO, fast-math, -march=native, CPU governor change, cache flush, process killing, or performance counter is used.",
        "C++ is compiled with the fixed generic x86_64 target and the same LLVM ld.lld; language build time and C++ compile time are not timing samples.",
        f"Only the fixed x86_64 {config['display']} fixture ({config['description']}) is covered; results do not generalize to other algorithms, targets, compilers, kernels, or input sizes.",
        "Warmups do not control cold file-cache behavior. A minimum below 50 ms is reported as startup/loader-sensitive, not as a pure sorting result.",
    ]


def not_run_items(algorithm, *, formal):
    config = algorithm_config(algorithm)
    items = [
        {"item": "cold_file_cache", "reason": "no controlled cache-drop/flush entry"},
        {"item": "perf_counters", "reason": "no hardware/software performance counters were collected"},
        {"item": "governor_or_cpu_isolation", "reason": "the harness does not change the governor or isolate CPUs"},
        {"item": "other_algorithms_kernels_targets_platforms", "reason": f"only the fixed x86_64 {config['display']} fixture is covered"},
    ]
    if not formal:
        items.append({"item": "formal_timing",
                      "reason": "prepare validates correctness and artifacts only; run the default stage on an idle host"})
    return items


def self_test(algorithm):
    config = algorithm_config(algorithm)
    checks, old_log, log = [], common._COMMAND_LOG, []
    common._COMMAND_LOG = log
    try:
        raw, golden = lcg_values(8, DEFAULT_SEED), sorted(lcg_values(8, DEFAULT_SEED))
        wrong = list(golden)
        wrong[4] ^= 1 << 31
        checks.append({"name": "lcg-nonempty", "status": "passed" if raw else "failed"})
        checks.append({"name": "golden-sorted", "status": "passed" if golden == sorted(raw) else "failed"})
        checks.append({"name": "wrong-golden-differs", "status": "passed" if
                       wrong != golden and (wrong[4] & 0xFF) == (golden[4] & 0xFF) else "failed"})
        template_map = templates(algorithm)
        rendered = render(template_map[config["source_template"]], raw, golden,
                          8, DEFAULT_SEED, 1)
        checks.append({"name": "source-render-complete",
                       "status": "passed" if "{{" not in rendered else "failed"})
        rendered_reference = render(template_map[config["reference_template"]],
                                    raw, golden, 8, DEFAULT_SEED, 1)
        checks.append({"name": "reference-render-complete",
                       "status": "passed" if "{{" not in rendered_reference else "failed"})
        records = template_recs(algorithm)
        checks.append({
            "name": "algorithm-template-identity",
            "status": "passed" if
            set(records) == set(template_names(config)) and
            all(Path(record["path"]).parent == config["directory"].resolve()
                for record in records.values()) and
            f"func {config['sort_symbol']}(" in rendered and
            f"{config['sort_symbol']}(" in rendered_reference and
            config["manifest_name"] in manifest_text(config["manifest_name"])
            else "failed",
        })
        checks.append({"name": "default-algorithm-is-heap",
                       "status": "passed" if parse_args([]).algorithm == "heap" else "failed"})
        checks.append({"name": "explicit-algorithm-selected",
                       "status": "passed" if
                       parse_args(["--self-test", "--algorithm", algorithm]).algorithm == algorithm
                       else "failed"})
        fake = {"algorithm": algorithm, "schema": config["prepare_schema"],
                "task": config["task"], "contract": CONTRACT,
                "operation_id": config["operation_id"]}
        try:
            require_report_algorithm(fake, "self-test", algorithm)
            accepted = True
        except PerfError:
            accepted = False
        checks.append({"name": "matching-algorithm-report-accepted",
                       "status": "passed" if accepted else "failed"})
        other = next(name for name in ALGORITHMS if name != algorithm)
        try:
            require_report_algorithm(fake, "self-test", other)
            rejection = None
        except PerfError as error:
            rejection = str(error)
        checks.append({"name": "cross-algorithm-report-rejected",
                       "status": "passed" if rejection is not None and
                       "algorithm" in rejection and algorithm in rejection and
                       other in rejection else "failed"})
        failed = common.command(
            [sys.executable, "-c", "import sys; print('expected-failure'); sys.exit(7)"],
            timeout=10, env=os.environ.copy())
        checks.append({"name": "failed-command-logged", "status": "passed" if
                       failed["status"] == "exited" and failed["exit"] == 7 and
                       failed["stdout"] == "expected-failure\n" and len(log) == 1 else "failed"})
        timed_out = common.command(
            [sys.executable, "-c", "import time; print('before-timeout', flush=True); time.sleep(5)"],
            timeout=0.2, env=os.environ.copy())
        checks.append({"name": "timeout-command-logged", "status": "passed" if
                       timed_out["status"] == "timeout" and "before-timeout" in timed_out["stdout"]
                       and len(log) == 2 else "failed"})
    finally:
        common._COMMAND_LOG = old_log
    result = {"schema": config["self_test_schema"], "task": config["task"],
              "contract": CONTRACT, "operation_id": config["operation_id"],
              "algorithm": algorithm, "status": "passed" if all(
                  row["status"] == "passed" for row in checks) else "failed",
              "checks": checks, "command_records": len(log)}
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if result["status"] == "passed" else 1


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--algorithm", choices=tuple(ALGORITHMS), default=DEFAULT_ALGORITHM,
                        help="sort fixture to prepare/measure (default: heap)")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--prepare-report", type=Path)
    parser.add_argument("--woozyc", dest="langc", type=Path)
    parser.add_argument("--helper", type=Path)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--compiler-root", type=Path)
    parser.add_argument("--ld-library-path", type=Path)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--n", type=int, default=DEFAULT_N)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=DEFAULT_SEED)
    parser.add_argument("--rounds", type=int, default=DEFAULT_ROUNDS)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--run-timeout", type=float, default=120.0)
    return parser.parse_args(argv)


def validate_args(args):
    if args.self_test:
        return
    need(args.out is not None, "--out is required")
    need(args.n >= 1 and args.rounds >= 1 and args.warmups >= 1 and args.samples >= 1,
         "N, rounds, warmups, and samples must be positive")
    need(0 <= args.seed <= MASK32, "--seed must fit in u32")
    if args.prepare_only:
        need(args.prepare_report is None, "--prepare-report is only used by measurement")
    else:
        need(args.prepare_report is not None, "measurement requires --prepare-report")
    for name in ("langc", "helper", "runtime", "llvm_bin"):
        option = "--woozyc" if name == "langc" else "--" + name.replace("_", "-")
        need(getattr(args, name) is not None, f"{option} is required")
    for label, path in (("langc", args.langc), ("helper", args.helper), ("runtime", args.runtime),
                        ("llvm-bin/clang++", args.llvm_bin / "clang++"),
                        ("llvm-bin/ld.lld", args.llvm_bin / "ld.lld"),
                        ("llvm-bin/opt", args.llvm_bin / "opt")):
        need(path.is_file(), f"{label} is missing: {path}")


def main(argv=None):
    args = parse_args(argv)
    if args.self_test:
        return self_test(args.algorithm)
    try:
        validate_args(args)
        return prepare(args) if args.prepare_only else measure(args)
    except Exception as error:
        print(json.dumps({"status": "failed", "type": type(error).__name__,
                          "message": str(error), "traceback": traceback.format_exc()},
                         indent=2, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    sys.exit(main())
