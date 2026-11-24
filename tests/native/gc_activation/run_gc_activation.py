#!/usr/bin/env python3
"""Verify graph-sensitive GC activation without running the full suites.

A module with no possible GC graph node must not call
lang_gc_program_init_v1 and must not call a registration or world-register
entry point. A graph-capable module must keep the full lifecycle. Safepoints
and modification windows remain present in both cases.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from gc_ir_contract import (ContractError, check_gc_poll_contract,
                            selftest as contract_selftest)

CASES = {
    "no-graph-string-vec": False,
    "graph-self-cycle": True,
    "graph-thread-cycle": True,
}
NO_GRAPH_TIMEZONE = "tests/native/timezone/project.toml"


def run(argv, timeout=240, env=None):
    argv = list(map(str, argv))
    completed = subprocess.run(argv, capture_output=True, text=True,
                               timeout=timeout, env=env)
    return {"argv": argv, "exit": completed.returncode,
            "stdout": completed.stdout, "stderr": completed.stderr}


def ok(row, what):
    if row["exit"] != 0:
        raise RuntimeError(f"{what}: {row}")


def calls(llvm: str, symbol: str) -> int:
    return len(re.findall(r"call[^\n]*@" + re.escape(symbol) + r"\b", llvm))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--libxml2-lib", type=Path)
    args = parser.parse_args()
    for name in ("cli", "helper", "root", "out"):
        setattr(args, name, getattr(args, name).resolve())
    args.out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if args.libxml2_lib:
        previous = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = str(args.libxml2_lib) + (
            ":" + previous if previous else "")
    report = {"status": "failed", "cases": {}, "timezone_no_graph": {}}
    # The contract's own mutation checks run before any real module is judged.
    report["contract_selftest"] = contract_selftest()

    def inspect(manifest: Path, label: str, graph: bool):
        row = {"graph": graph, "status": "failed", "runs": []}
        report["cases"][label] = row
        for opt in (0, 2):
            out = args.out / label / f"O{opt}"
            shutil.rmtree(out, ignore_errors=True)
            out.mkdir(parents=True)
            build = run([args.cli, "build", "--project", manifest,
                         "--backend", "native", "--opt", opt,
                         "--out-dir", out / "cli", "--format", "json"],
                        env=env)
            row["runs"].append({"kind": "build", "opt": opt, "result": build})
            ok(build, f"{label} O{opt} build")
            assert json.loads(build["stdout"])["status"] == "ok", build
            execute = run([out / "cli" / "program"], env=env)
            row["runs"].append({"kind": "execute", "opt": opt, "result": execute})
            ok(execute, f"{label} O{opt} execute")
            assert not execute["stdout"] and not execute["stderr"], execute
            ir = out / "ir"
            ir.mkdir()
            emit = run([args.helper, "--project", manifest, "--opt", opt,
                        "--out-object", ir / "program.o",
                        "--out-llvm", ir / "after.ll",
                        "--out-llvm-before", ir / "before.ll",
                        "--format", "json"], env=env)
            row["runs"].append({"kind": "emit", "opt": opt, "result": emit})
            ok(emit, f"{label} O{opt} emit")
            assert json.loads(emit["stdout"])["status"] == "passed", emit
            llvm = (ir / "before.ll").read_text(encoding="utf-8")
            try:
                poll = check_gc_poll_contract(llvm)
            except ContractError as error:
                raise RuntimeError(f"{label} O{opt}: {error}") from error
            facts = {
                "init_calls": calls(llvm, "lang_gc_program_init_v1"),
                "register_calls": calls(llvm, "lang_gc_program_register_v1"),
                "world_register_calls": calls(
                    llvm, "lang_gc_world_register_v1"),
                "finish_calls": calls(llvm, "lang_gc_program_finish_v1"),
                "poll_sites": poll["sites"],
                "poll_functions": poll["functions"],
                "poll_positions_exact": poll["positions_exact"],
                "controller_poll_calls": poll["controller_calls"],
                "direct_poll_calls": (poll["direct_world_calls"] +
                                      poll["direct_pressure_calls"] +
                                      poll["inline_word_loads"]),
                "critical_calls": calls(
                    llvm, "lang_gc_program_enter_critical_v1"),
            }
            row.setdefault("llvm", {})[f"O{opt}"] = facts
            if graph:
                assert facts["init_calls"] > 0, (label, opt, facts)
                assert facts["register_calls"] > 0, (label, opt, facts)
            else:
                assert facts["init_calls"] == 0, (label, opt, facts)
                assert facts["register_calls"] == 0, (label, opt, facts)
                assert facts["world_register_calls"] == 0, (label, opt, facts)
            # The optimization must not remove safepoints or graph-stable
            # modification windows; only the program lifecycle is skipped.
            assert facts["finish_calls"] > 0, (label, opt, facts)
            assert facts["poll_sites"] > 0, (label, opt, facts)
            assert facts["poll_positions_exact"], (label, opt, facts)
            assert facts["controller_poll_calls"] == facts["poll_sites"], (
                label, opt, facts)
            assert facts["direct_poll_calls"] == 0, (label, opt, facts)
            assert facts["critical_calls"] > 0, (label, opt, facts)
        row["status"] = "passed"

    fixtures = args.root / "tests/native/gc_activation"
    for label, graph in CASES.items():
        inspect(fixtures / label / "lang.toml", label, graph)

    # The real timezone source is the performance motivation: Vec[u8] and
    # string-only containers cannot create GC graph nodes. Inspect LLVM only;
    # do not rerun the long timezone suite here.
    tz_manifest = args.root / NO_GRAPH_TIMEZONE
    tz_out = args.out / "timezone-no-graph"
    shutil.rmtree(tz_out, ignore_errors=True)
    tz_out.mkdir(parents=True)
    emit = run([args.helper, "--project", tz_manifest, "--opt", 2,
                "--out-object", tz_out / "program.o",
                "--out-llvm", tz_out / "after.ll",
                "--out-llvm-before", tz_out / "before.ll",
                "--format", "json"], env=env)
    report["timezone_no_graph"]["emit"] = emit
    ok(emit, "timezone LLVM emit")
    assert json.loads(emit["stdout"])["status"] == "passed", emit
    llvm = (tz_out / "before.ll").read_text(encoding="utf-8")
    try:
        poll = check_gc_poll_contract(llvm)
    except ContractError as error:
        raise RuntimeError(f"timezone-no-graph O2: {error}") from error
    facts = {
        "init_calls": calls(llvm, "lang_gc_program_init_v1"),
        "register_calls": calls(llvm, "lang_gc_program_register_v1"),
        "world_register_calls": calls(llvm, "lang_gc_world_register_v1"),
        "finish_calls": calls(llvm, "lang_gc_program_finish_v1"),
        "poll_sites": poll["sites"],
        "poll_functions": poll["functions"],
        "poll_positions_exact": poll["positions_exact"],
        "controller_poll_calls": poll["controller_calls"],
        "direct_poll_calls": (poll["direct_world_calls"] +
                              poll["direct_pressure_calls"] +
                              poll["inline_word_loads"]),
        "critical_calls": calls(llvm, "lang_gc_program_enter_critical_v1"),
    }
    report["timezone_no_graph"]["llvm"] = facts
    assert facts["init_calls"] == 0, facts
    assert facts["register_calls"] == 0, facts
    assert facts["world_register_calls"] == 0, facts
    assert facts["finish_calls"] > 0, facts
    assert facts["poll_sites"] > 0, facts
    assert facts["poll_positions_exact"], facts
    assert facts["controller_poll_calls"] == facts["poll_sites"], facts
    assert facts["direct_poll_calls"] == 0, facts
    assert facts["critical_calls"] > 0, facts
    report["timezone_no_graph"]["status"] = "passed"
    report["status"] = "passed"
    (args.out / "summary.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "cases": list(CASES),
                      "timezone_no_graph": report["timezone_no_graph"]["status"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
