"""Fixed calibration and sampling rules for the sixteen prepared workloads.

Callers build/review the actual R/2R programs and freeze inputs, IR, objects,
tools and host settings. This module measures programs; it does not prove their
work or replace the required core-language completion and idle-host checks.
"""
from pathlib import Path
import statistics

import run_native as common

GRID = {(name, size) for name, sizes in (
    ("heap", (65536, 1048576)), ("quick", (65536, 1048576)),
    ("merge", (65536, 1048576)), ("sieve", (1000000, 10000000)),
    ("fannkuch", (9, 10)), ("mandelbrot", (512, 2048)),
    ("binarytrees", (14, 18)), ("matmul", (128, 512)),
) for size in sizes}
MAX_ROUNDS = 1 << 20


def timed_run(run, program):
    record = dict(run(program))
    common.require_quiet_success(record, str(program))
    if type(record.get("elapsed_ns")) is not int or record["elapsed_ns"] <= 0:
        raise common.PerfError("elapsed_ns must be a positive integer")
    return record


def calibrate(build_cpp, run):
    """Build C++ only, doubling R; preserve all three trials and the 2R control.

    build_cpp(R) returns a distinct immutable executable path. run(path) returns
    the existing command-record shape, including actual whole-process ns.
    Exceptions propagate: a failed build/run is not a short timing sample.
    """
    trials = []
    paths = set()

    def trial(rounds):
        program = Path(build_cpp(rounds)).resolve()
        if program in paths:
            raise common.PerfError("calibration builds must preserve distinct R programs")
        paths.add(program)
        sha = common.sha256_file(program)
        records = [timed_run(run, program) for _ in range(3)]
        values = [record["elapsed_ns"] for record in records]
        if common.sha256_file(program) != sha:
            raise common.PerfError("calibration executable changed")
        row = {"rounds": rounds, "program": str(program), "sha256": sha,
               "runs": records, "median_ns": statistics.median(values),
               "min_ns": min(values)}
        trials.append(row)
        return row

    rounds = 1
    while rounds <= MAX_ROUNDS:
        selected = trial(rounds)
        if selected["median_ns"] >= 500_000_000 and selected["min_ns"] >= 250_000_000:
            doubled = trial(rounds * 2)
            for row in trials:
                if common.sha256_file(row["program"]) != row["sha256"]:
                    raise common.PerfError("calibration executable changed")
            return {"status": "selected", "rounds": rounds, "trials": trials,
                    "double_round_median_ratio": doubled["median_ns"] / selected["median_ns"],
                    "work_review_required": True}
        rounds *= 2
    return {"status": "incomplete", "rounds": None, "trials": trials,
            "work_review_required": True}


def sample_stats(runs):
    expected = [(phase, pair, name) for phase, count in (("warmup", 2), ("measured", 9))
                for pair in range(count) for name in
                (("source", "cpp") if pair % 2 == 0 else ("cpp", "source"))]
    if [(row.get("phase"), row.get("pair"), row.get("implementation")) for row in runs] != expected:
        raise common.PerfError("expected exactly two warmups and nine alternating pairs")
    for row in runs:
        common.require_quiet_success(row, row["implementation"])
        if type(row.get("elapsed_ns")) is not int or row["elapsed_ns"] <= 0:
            raise common.PerfError("elapsed_ns must be a positive integer")
    stats = {}
    for name in ("source", "cpp"):
        values = [row["elapsed_ns"] for row in runs
                  if row["phase"] == "measured" and row["implementation"] == name]
        stats[name] = {"samples_ns": values, "median_ns": statistics.median(values),
                       "min_ns": min(values), "max_ns": max(values)}
    return stats


def calibrated_rounds(calibration, cpp):
    rounds = calibration.get("rounds")
    if (calibration.get("status") != "selected" or type(rounds) is not int
            or rounds < 1 or rounds > MAX_ROUNDS or rounds & (rounds - 1)):
        raise common.PerfError("a completed power-of-two C++ calibration is required")
    expected = []
    value = 1
    while value <= 2 * rounds:
        expected.append(value)
        value *= 2
    trials = calibration["trials"]
    if [row["rounds"] for row in trials] != expected:
        raise common.PerfError("calibration must preserve every doubling and the 2R control")
    selected = None
    seen = set()
    for trial in trials:
        path = Path(trial["program"]).resolve()
        if path in seen or common.sha256_file(path) != trial["sha256"]:
            raise common.PerfError("calibration executable changed or was reused")
        seen.add(path)
        if len(trial["runs"]) != 3:
            raise common.PerfError("each C++ calibration needs exactly three trials")
        values = []
        for run in trial["runs"]:
            common.require_quiet_success(run, "C++ calibration")
            ns = run.get("elapsed_ns")
            if type(ns) is not int or ns <= 0:
                raise common.PerfError("elapsed_ns must be a positive integer")
            values.append(ns)
        sufficient = statistics.median(values) >= 500_000_000 and min(values) >= 250_000_000
        if trial["rounds"] < rounds and sufficient:
            raise common.PerfError("R must be the first sufficient calibration")
        if trial["rounds"] == rounds:
            if not sufficient or trial["sha256"] != common.sha256_file(cpp):
                raise common.PerfError("C++ executable does not match a sufficient calibration")
            selected = trial
    if selected is None:
        raise common.PerfError("selected C++ calibration is missing")
    return rounds


def sample_pair(source, cpp, run, calibration):
    """Two warmups and nine alternating pairs; no discarded or replaced samples.

    The caller must retain this result even when status is failed. A callback
    should log commands before returning, as run_native.command already does.
    """
    programs = {"source": Path(source).resolve(), "cpp": Path(cpp).resolve()}
    report = {"status": "failed", "runs": [], "programs": {},
              "ratio": None, "limit": 2.0,
              "threshold_applied": False, "threshold_passed": False,
              "calibration": calibration}
    try:
        if programs["source"].samefile(programs["cpp"]):
            raise common.PerfError("Source and C++ must be distinct executable files")
        report["rounds"] = calibrated_rounds(calibration, programs["cpp"])
        report["programs"] = {key: {"path": str(path), "sha256": common.sha256_file(path)}
                              for key, path in programs.items()}
        for phase, count in (("warmup", 2), ("measured", 9)):
            for pair in range(count):
                order = ("source", "cpp") if pair % 2 == 0 else ("cpp", "source")
                for name in order:
                    record = dict(run(programs[name]))
                    record.update(phase=phase, pair=pair, implementation=name)
                    report["runs"].append(record)
                    common.require_quiet_success(record, name)
                    if type(record.get("elapsed_ns")) is not int or record["elapsed_ns"] <= 0:
                        raise common.PerfError("elapsed_ns must be a positive integer")
        for name, path in programs.items():
            if common.sha256_file(path) != report["programs"][name]["sha256"]:
                raise common.PerfError(f"{name} executable changed during sampling")
        report["stats"] = sample_stats(report["runs"])
        source_ns = report["stats"]["source"]["median_ns"]
        cpp_ns = report["stats"]["cpp"]["median_ns"]
        if cpp_ns < 500_000_000 or report["stats"]["cpp"]["min_ns"] < 250_000_000:
            raise common.PerfError("measured C++ durations are below the calibration minimum")
        report.update(status="sampled", ratio=source_ns / cpp_ns,
                      threshold_applied=True, threshold_passed=source_ns <= 2 * cpp_ns)
    except Exception as error:
        report["failure"] = {"type": type(error).__name__, "message": str(error)}
    return report


def summarize_grid(cases):
    """Require every fixed cell, without averaging or ignoring duplicate cells."""
    keys = [(row["algorithm"], row["n"]) for row in cases]
    complete = len(keys) == len(set(keys)) and set(keys) == GRID
    verdicts = []
    checked = []
    for row in cases:
        result = row.get("result", {})
        if not isinstance(result, dict):
            result = {}
        try:
            cpp = Path(result["programs"]["cpp"]["path"])
            source = Path(result["programs"]["source"]["path"])
            if source.samefile(cpp):
                raise common.PerfError("Source and C++ executable files are identical")
            if calibrated_rounds(result["calibration"], cpp) != result["rounds"]:
                raise common.PerfError("sampling rounds differ from calibration")
            for program in result["programs"].values():
                if common.sha256_file(program["path"]) != program["sha256"]:
                    raise common.PerfError("sampled executable changed before grid summary")
            stats = sample_stats(result["runs"])
            if result["status"] != "sampled":
                raise common.PerfError("case did not finish sampling")
            source_ns, cpp_ns = stats["source"]["median_ns"], stats["cpp"]["median_ns"]
            if cpp_ns < 500_000_000 or stats["cpp"]["min_ns"] < 250_000_000:
                raise common.PerfError("measured C++ durations are below the calibration minimum")
            verdicts.append(source_ns <= 2 * cpp_ns)
            result = {**result, "stats": stats, "ratio": source_ns / cpp_ns, "limit": 2.0,
                      "threshold_applied": True, "threshold_passed": verdicts[-1]}
        except (KeyError, TypeError, OSError, common.PerfError) as error:
            verdicts.append(None)
            result = {**result, "ratio": None, "limit": 2.0,
                      "threshold_applied": False, "threshold_passed": False,
                      "validation_failure": str(error)}
        checked.append({**row, "result": result})
    measured = complete and all(value is not None for value in verdicts)
    passed = measured and all(verdicts)
    return {"status": "passed" if passed else "failed" if measured else "incomplete",
            "expected_cells": len(GRID), "supplied_cells": len(keys),
            "threshold_applied": measured, "threshold_passed": passed,
            "cases": checked}
