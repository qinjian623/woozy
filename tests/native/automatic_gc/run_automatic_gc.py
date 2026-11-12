#!/usr/bin/env python3
"""Real-source stress tests for MEM02 pure-Woozy cycle collection.

The suite separates three different results that must not be conflated:

* a selected-scope run (for example one control case) passed;
* the full single-mutator cycle suite passed;
* MEM02 as a whole is complete (it is not: Weak, capturing closures,
  concurrent collection, threads and foreign cycles are out of scope here).

Identity is `(allocation address, per-address generation)`, because a legal
FREE may be followed by a new ALLOC at the same address. Collector metadata
allocations are tracked as raw blocks but excluded from the application Vec
payload budget and from "last rooted Vec" ordering.

Default target mode follows the Root pressure policy: sustained garbage is
bounded by live Vec payload bytes, not by a requirement to collect before a
particular source marker or every N objects. `--baseline-only` records the
existing no-collection leak as `expected_failure` / `not_implemented`.

Raw command output larger than `--raw-output-limit` is written under
`OUT/raw/...` so the candidate tree can keep only a compact report. See
CONTRACT.md and BASELINE.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCHEMA = "woozy/native-automatic-gc-report/2"
COMPACT_SCHEMA = "woozy/native-automatic-gc-compact/1"
LIMITATIONS = [
    "Source-level Weak references are not exercised; MEM01 owns them.",
    "Capturing closures are not exercised; LANG01 owns them and is not implemented.",
    "Concurrent collection, collection during blocking runtime calls, threads, fork and channels are not exercised.",
    "Only Vec objects and their allocator blocks are measured. RSS, page-cache state and 'file' output are not collection evidence.",
    "The observation binary is a test-only static build of the same helper-emitted LLVM plus the Native runtime and observe_gc.cpp; it is not the CLI-linked program.",
    "A normal single-thread main-return final collection may reclaim a watched dead cycle; this is an implementation choice, not a source timing guarantee.",
    "This directory contains tests only. A full-suite pass would not by itself mark MEM02 complete.",
]
NOT_COVERED = [
    "Weak references",
    "capturing closures",
    "concurrent collection",
    "runtime blocking-call SAFE integration",
    "threads, fork, channels and foreign cycles",
]


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class CaseError(RuntimeError):
    def __init__(self, kind: str, message: str):
        super().__init__(message)
        self.kind = kind


class ObserverError(RuntimeError):
    pass


def command(argv, timeout, env, cwd=None):
    argv = [str(x) for x in argv]
    started = time.time()
    try:
        run = subprocess.run(
            argv, capture_output=True, text=True, timeout=timeout, env=env, cwd=cwd
        )
        return {
            "argv": argv,
            "timeout_seconds": timeout,
            "elapsed_seconds": round(time.time() - started, 4),
            "exit": run.returncode,
            "stdout": run.stdout,
            "stderr": run.stderr,
        }
    except subprocess.TimeoutExpired as error:
        return {
            "argv": argv,
            "timeout_seconds": timeout,
            "elapsed_seconds": round(time.time() - started, 4),
            "timeout": True,
            "exit": None,
            "stdout": (error.stdout or "") if isinstance(error.stdout, str) else "",
            "stderr": (error.stderr or "") if isinstance(error.stderr, str) else "",
        }


def compact_command(out_dir: Path, name: str, row: dict, limit: int):
    """Keep small output inline; move large streams under OUT/raw/."""
    for stream in ("stdout", "stderr"):
        text = row.get(stream, "")
        encoded = len(text.encode())
        if encoded <= limit:
            continue
        path = out_dir / "raw" / f"{name}.{stream}"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        row[f"{stream}_file"] = str(path)
        row[f"{stream}_bytes"] = encoded
        row[f"{stream}_sha256"] = digest(path)
        row[stream] = f"<stored:{path}>"
    return row


def require_ok(row, what):
    if row.get("timeout"):
        raise CaseError("harness", f"{what}: timed out after {row['timeout_seconds']}s")
    if row["exit"] != 0:
        raise CaseError(
            "harness",
            f"{what}: exit {row['exit']}\nstdout:\n{row['stdout']}\nstderr:\n{row['stderr']}",
        )
    return row


def elf_is_freestanding(readelf: Path, binary: Path, timeout: int, env):
    row = command([readelf, "-l", "-d", binary], timeout, env)
    require_ok(row, f"readelf {binary.name}")
    text = row["stdout"]
    problems = []
    if "INTERP" in text:
        problems.append("program header INTERP present")
    if "NEEDED" in text:
        problems.append("dynamic NEEDED entry present")
    return row, problems


def histogram_add(table, value, delta=1):
    table[value] = table.get(value, 0) + delta
    if table[value] == 0:
        del table[value]


def parse_events(stderr: str):
    """Parse CONFIG/ALLOC/FREE/VEC with (address, generation) identity."""
    header = None
    generation = {}
    current = {}
    records = {}
    alloc_order = []
    vec_alloc_order = []
    vec_free_order = []
    block_free_order = []
    ref_events = []
    pending_free = None
    total_payload = 0
    peak_payload = 0
    total_alloc = 0
    peak_alloc = 0
    alloc_length_hist = {}
    freed_length_hist = {}
    block_allocations = 0
    vec_allocations = 0
    vec_frees = 0

    for index, line in enumerate(stderr.splitlines()):
        if line.startswith("CONFIG:"):
            if header is not None:
                raise ObserverError("duplicate CONFIG line")
            parts = line.split(":")
            if len(parts) != 3 or parts[1] != "vec_header_bytes":
                raise ObserverError(f"malformed CONFIG line: {line!r}")
            header = int(parts[2])
            if header <= 0 or header % 16:
                raise ObserverError(f"invalid Vec header size: {header}")
            continue
        if line.startswith("ALLOC:"):
            if header is None:
                raise ObserverError("ALLOC before CONFIG")
            parts = line.split(":")
            if len(parts) != 3:
                raise ObserverError(f"malformed ALLOC line: {line!r}")
            pointer, size = parts[1], int(parts[2])
            if pending_free is not None:
                raise ObserverError(
                    f"VEC:5 for {pending_free[0]} was not followed by FREE"
                )
            if pointer in current:
                raise ObserverError(f"ALLOC for live address without FREE: {line!r}")
            gen = generation.get(pointer, 0) + 1
            generation[pointer] = gen
            key = (pointer, gen)
            records[key] = {
                "key": key,
                "pointer": pointer,
                "generation": gen,
                "size": size,
                "vec": False,
                "length": None,
                "payload": None,
                "refs": None,
                "alloc_index": index,
                "free_index": None,
                "freed": False,
                "vec_free_pending": False,
            }
            current[pointer] = key
            alloc_order.append(key)
            block_allocations += 1
            total_alloc += size
            peak_alloc = max(peak_alloc, total_alloc)
            continue
        if line.startswith("FREE:"):
            parts = line.split(":")
            if len(parts) != 2:
                raise ObserverError(f"malformed FREE line: {line!r}")
            pointer = parts[1]
            key = current.pop(pointer, None)
            if key is None:
                raise ObserverError(f"unknown or duplicate FREE: {line!r}")
            record = records[key]
            if record["vec"]:
                if pending_free != key:
                    raise ObserverError(
                        f"Vec {pointer} freed without an immediately preceding VEC:5"
                    )
            elif pending_free is not None:
                raise ObserverError(
                    f"VEC:5 for {pending_free[0]} was followed by raw FREE:{pointer}"
                )
            pending_free = None
            record["freed"] = True
            record["free_index"] = index
            block_free_order.append(key)
            total_alloc -= record["size"]
            if total_alloc < 0:
                raise ObserverError("allocator byte accounting went negative")
            if record["vec"]:
                total_payload -= record["payload"]
                if total_payload < 0:
                    raise ObserverError("Vec payload accounting went negative")
                record["vec_free_position"] = len(vec_free_order)
                vec_free_order.append(key)
                vec_frees += 1
                histogram_add(freed_length_hist, record["length"])
            continue
        if line.startswith("VEC:"):
            if pending_free is not None:
                raise ObserverError(
                    f"VEC:5 for {pending_free[0]} was not immediately followed by FREE"
                )
            parts = line.split(":")
            if len(parts) != 4:
                raise ObserverError(f"malformed VEC line: {line!r}")
            event, pointer, detail = int(parts[1]), parts[2], int(parts[3])
            key = current.get(pointer)
            if key is None:
                raise ObserverError(f"VEC event for unknown address: {line!r}")
            record = records[key]
            if event == 1:
                if record["vec"]:
                    raise ObserverError(f"duplicate VEC:1: {line!r}")
                payload = record["size"] - header
                if payload < 0:
                    raise ObserverError(
                        f"Vec allocation smaller than header: {line!r}"
                    )
                if detail == 0:
                    if payload != 0:
                        raise ObserverError(
                            f"empty Vec has {payload} payload bytes: {line!r}"
                        )
                elif payload <= 0 or payload % detail:
                    raise ObserverError(
                        f"Vec payload {payload} is not length*stride for {line!r}"
                    )
                record["vec"] = True
                record["length"] = detail
                record["payload"] = payload
                record["refs"] = 1
                vec_alloc_order.append(key)
                vec_allocations += 1
                histogram_add(alloc_length_hist, detail)
                total_payload += payload
                peak_payload = max(peak_payload, total_payload)
            elif event in (2, 3):
                if not record["vec"]:
                    raise ObserverError(f"reference event before VEC:1: {line!r}")
                record["refs"] = detail
                ref_events.append(
                    {
                        "key": key,
                        "event": event,
                        "refs": detail,
                        "index": index,
                    }
                )
            elif event == 4:
                if not record["vec"] or detail >= record["length"]:
                    raise ObserverError(f"invalid element drop event: {line!r}")
            elif event == 5:
                if not record["vec"] or record["vec_free_pending"]:
                    raise ObserverError(f"duplicate or invalid VEC:5: {line!r}")
                record["vec_free_pending"] = True
                pending_free = key
            elif event in (6, 7):
                if not record["vec"]:
                    raise ObserverError(f"iteration event before VEC:1: {line!r}")
            else:
                raise ObserverError(f"unknown VEC event: {line!r}")
            continue
        raise ObserverError(f"unexpected observation line: {line!r}")

    if header is None and block_allocations != 0:
        raise ObserverError("missing CONFIG:vec_header_bytes line")
    if pending_free is not None:
        raise ObserverError(f"VEC:5 for {pending_free[0]} was not followed by FREE")

    live_keys = list(current.values())
    live_vec_keys = [key for key in live_keys if records[key]["vec"]]
    live_refs = sorted(records[key]["refs"] for key in live_vec_keys)
    live_length_hist = {}
    live_ref_hist = {}
    for key in live_vec_keys:
        histogram_add(live_length_hist, records[key]["length"])
        histogram_add(live_ref_hist, records[key]["refs"])

    def as_hist(table):
        return {str(key): table[key] for key in sorted(table)}

    vec_free_positions = {
        key: position for position, key in enumerate(vec_free_order)
    }
    block_free_positions = {
        key: position for position, key in enumerate(block_free_order)
    }
    return {
        "header_bytes": header,
        "vec_allocations": vec_allocations,
        "vec_frees": vec_frees,
        "live_vecs_at_exit": len(live_vec_keys),
        "refs_at_exit": live_refs if len(live_refs) <= 64 else None,
        "live_ref_histogram": as_hist(live_ref_hist),
        "freed_lengths": (
            [records[key]["length"] for key in vec_free_order]
            if len(vec_free_order) <= 64
            else None
        ),
        "alloc_lengths": (
            [records[key]["length"] for key in vec_alloc_order]
            if len(vec_alloc_order) <= 64
            else None
        ),
        "freed_length_histogram": as_hist(freed_length_hist),
        "alloc_length_histogram": as_hist(alloc_length_hist),
        "live_length_histogram": as_hist(live_length_hist),
        "block_allocations": block_allocations,
        "raw_allocations": block_allocations - vec_allocations,
        "live_blocks_at_exit": len(live_keys),
        "all_blocks_freed": not live_keys,
        "peak_live_payload_bytes": peak_payload,
        "final_live_payload_bytes": total_payload,
        "peak_live_alloc_bytes": peak_alloc,
        "final_live_alloc_bytes": total_alloc,
        "_records": records,
        "_ref_events": ref_events,
        "_vec_free_order": vec_free_order,
        "_vec_free_positions": vec_free_positions,
        "_block_free_positions": block_free_positions,
    }


def public_metrics(metrics):
    return {key: value for key, value in metrics.items() if not key.startswith("_")}


def unique_vec_by_length(records, length):
    matches = [
        key
        for key, record in records.items()
        if record["vec"] and record["length"] == length
    ]
    return matches[0] if len(matches) == 1 else None


def summarize_record(metrics, key):
    record = metrics["_records"][key]
    ref_events = [
        event for event in metrics["_ref_events"] if event["key"] == key
    ]
    refs = [event["refs"] for event in ref_events]
    return {
        "identity": {"pointer": record["pointer"], "generation": record["generation"]},
        "length": record["length"],
        "size": record["size"],
        "payload_bytes": record["payload"],
        "alloc_index": record["alloc_index"],
        "max_refs": max([1] + refs),
        "release_refs": refs,
        "release_to_one_indices": [
            event["index"]
            for event in ref_events
            if event["event"] == 3 and event["refs"] == 1
        ],
        "freed": record["freed"],
        "free_index": record["free_index"],
        "vec_free_position": metrics["_vec_free_positions"].get(key),
        "block_free_position": metrics["_block_free_positions"].get(key),
    }


def attach_timeline(metrics):
    records = metrics["_records"]
    keys = {
        "live": unique_vec_by_length(records, 9),
        "dead_cycle": unique_vec_by_length(records, 7),
        "marker_a": unique_vec_by_length(records, 3),
        "marker_b": unique_vec_by_length(records, 5),
    }
    timeline = {
        name: (summarize_record(metrics, key) if key is not None else None)
        for name, key in keys.items()
    }
    actual = public_metrics(metrics)
    actual["edge_timeline"] = timeline
    return actual


def compare_expected(expected, actual, label, case_kind):
    failures = []
    for key, want in expected.items():
        if key == "checks":
            continue
        got = actual.get(key)
        ok = True
        if isinstance(want, dict) and ("min" in want or "max" in want):
            if not isinstance(got, int):
                ok = False
            else:
                if "min" in want and got < want["min"]:
                    ok = False
                if "max" in want and got > want["max"]:
                    ok = False
        else:
            ok = got == want
        if ok:
            continue
        category = "metric_mismatch"
        if key in ("vec_allocations", "block_allocations", "header_bytes"):
            category = "metric_mismatch"
        elif case_kind == "control":
            category = "control"
        elif key in ("final_live_payload_bytes", "peak_live_payload_bytes"):
            category = "budget"
        elif key in (
            "vec_frees",
            "live_vecs_at_exit",
            "refs_at_exit",
            "live_ref_histogram",
            "freed_lengths",
            "freed_length_histogram",
            "live_length_histogram",
            "all_blocks_freed",
        ):
            category = "reclamation"
        failures.append(
            {
                "check": f"{label}.{key}",
                "expected": want,
                "actual": got,
                "category": category,
            }
        )
    return failures


def check_cycle_reclaimed(actual, metrics):
    alloc_nonzero = sum(
        count
        for length, count in actual["alloc_length_histogram"].items()
        if int(length) > 0
    )
    free_nonzero = sum(
        count
        for length, count in actual["freed_length_histogram"].items()
        if int(length) > 0
    )
    if alloc_nonzero == 0:
        return [
            {
                "check": "cycle_reclaimed",
                "expected": "at least one non-empty Vec allocation",
                "actual": actual["alloc_length_histogram"],
                "category": "metric_mismatch",
            }
        ]
    if free_nonzero != alloc_nonzero:
        return [
            {
                "check": "cycle_reclaimed",
                "expected": f"free every non-empty Vec ({alloc_nonzero})",
                "actual": f"freed {free_nonzero} non-empty Vecs",
                "category": "reclamation",
            }
        ]
    return []


def check_pressure_budget(case, actual):
    budget = case.get("payload_budget_bytes")
    if budget is None:
        return []
    failures = []
    if actual["peak_live_payload_bytes"] > budget:
        failures.append(
            {
                "check": "pressure_budget.peak_live_payload_bytes",
                "expected": f"<={budget}",
                "actual": actual["peak_live_payload_bytes"],
                "category": "budget",
            }
        )
    if actual["final_live_payload_bytes"] != 0:
        failures.append(
            {
                "check": "pressure_budget.final_live_payload_bytes",
                "expected": 0,
                "actual": actual["final_live_payload_bytes"],
                "category": "budget",
            }
        )
    return failures


def check_root_values_preserved(actual, ctx, main_return):
    failures = []
    for name in ("real_execute", "observe_execute"):
        row = ctx.get(name)
        if row is None:
            continue
        if row["exit"] != main_return:
            failures.append(
                {
                    "check": f"root_values_preserved.{name}_exit",
                    "expected": main_return,
                    "actual": row["exit"],
                    "category": "root_content",
                }
            )
    return failures


def check_dead_cycle_live_edge_release(actual):
    timeline = actual["edge_timeline"]
    live, cycle = timeline.get("live"), timeline.get("dead_cycle")
    marker_a, marker_b = timeline.get("marker_a"), timeline.get("marker_b")
    if live is None or cycle is None or marker_a is None or marker_b is None:
        return [
            {
                "check": "dead_cycle_live_edge_release.identities",
                "expected": "unique length-9 live root, length-7 dead cycle, length-3/5 markers",
                "actual": {
                    name: (None if item is None else item["identity"])
                    for name, item in timeline.items()
                },
                "category": "metric_mismatch",
            }
        ]
    failures = []

    def fail(check, expected, got):
        failures.append(
            {
                "check": f"dead_cycle_live_edge_release.{check}",
                "expected": expected,
                "actual": got,
                "category": "reclamation",
            }
        )

    if live["max_refs"] < 2:
        fail("max_refs", ">= 2 (external root plus the dead cycle edge)", live["max_refs"])
    if not live["release_to_one_indices"]:
        fail(
            "release_to_one",
            "a release event that leaves the live Vec at count 1",
            live["release_refs"],
        )
    if not live["freed"]:
        fail("live_freed", True, False)
    if not cycle["freed"]:
        fail("cycle_freed", True, False)
    if (
        live["freed"]
        and cycle["freed"]
        and cycle["vec_free_position"] >= live["vec_free_position"]
    ):
        fail(
            "free_order",
            "dead cycle freed before the live root's final release",
            {
                "cycle_vec_free_position": cycle["vec_free_position"],
                "live_vec_free_position": live["vec_free_position"],
            },
        )
    if live["freed"] and live["vec_free_position"] != actual["vec_frees"] - 1:
        fail(
            "live_vec_free_last",
            "live Vec is the last Vec freed; collector metadata may follow",
            {
                "live_vec_free_position": live["vec_free_position"],
                "vec_frees": actual["vec_frees"],
            },
        )
    if live["release_refs"] and live["release_refs"][-1] != 0:
        fail("final_release", "final release to 0 followed by free", live["release_refs"])
    if live["free_index"] is not None and live["free_index"] <= marker_b["alloc_index"]:
        fail(
            "live_survives_pressure",
            "live root not freed before the post-pressure marker/read",
            {
                "live_free_index": live["free_index"],
                "marker_b_alloc_index": marker_b["alloc_index"],
            },
        )
    return failures


CHECKS = {
    "cycle_reclaimed": lambda case, actual, metrics, ctx: check_cycle_reclaimed(
        actual, metrics
    ),
    "pressure_budget": lambda case, actual, metrics, ctx: check_pressure_budget(
        case, actual
    ),
    "root_values_preserved": lambda case, actual, metrics, ctx: check_root_values_preserved(
        actual, ctx, case["main_return"]
    ),
    "dead_cycle_live_edge_release": lambda case, actual, metrics, ctx: check_dead_cycle_live_edge_release(
        actual
    ),
}


def record_command(out_dir: Path, name: str, row: dict, limit: int):
    row = compact_command(out_dir, name, row, limit)
    row["name"] = name
    return row


def run_case(case, opt, args, env, report):
    identity = f"{case['id']}/O{opt}"
    case_dir = args.out / case["id"] / f"O{opt}"
    case_dir.mkdir(parents=True, exist_ok=True)
    timeout = case.get("timeout_seconds", args.timeout)
    row = {
        "id": case["id"],
        "kind": case["kind"],
        "opt": opt,
        "status": "failed",
        "commands": [],
        "source_hashes": {},
        "baseline": {"ok": False, "failures": []},
        "target": {"ok": False, "failures": []},
    }
    fixture = HERE / Path(case["manifest"]).parent
    commands = row["commands"]

    def run(name, argv, kind, local_timeout=None):
        result = command(argv, local_timeout or timeout, env, cwd=HERE)
        raw_stdout, raw_stderr = result["stdout"], result["stderr"]
        result = record_command(case_dir, name, result, args.raw_output_limit)
        result["_raw_stdout"] = raw_stdout
        result["_raw_stderr"] = raw_stderr
        result["kind"] = kind
        commands.append(result)
        if result.get("timeout"):
            raise CaseError(kind, f"{identity} {name} timed out after {result['timeout_seconds']}s")
        return result

    try:
        row["source_hashes"] = {
            str(path.relative_to(HERE)): digest(path)
            for path in sorted(fixture.rglob("*"))
            if path.is_file()
        }

        # 1. Real Native source build through the public CLI.
        cli_out = case_dir / "cli"
        build = run(
            "cli-build",
            [
                args.cli,
                "build",
                "--project",
                HERE / case["manifest"],
                "--backend",
                "native",
                "--opt",
                str(opt),
                "--out-dir",
                cli_out,
                "--format",
                "json",
            ],
            "build",
        )
        if build["exit"] != 0:
            raise CaseError("build", f"{identity} cli build exit {build['exit']}")
        build_json = json.loads(build["stdout"])
        if build_json.get("status") != "ok":
            raise CaseError("build", f"{identity} cli build status {build_json.get('status')}")
        program = cli_out / "program"
        if not program.is_file():
            raise CaseError("build", f"{identity} cli build produced no program")
        readelf_real, problems = elf_is_freestanding(
            args.readelf, program, timeout, env
        )
        readelf_real = record_command(
            case_dir, "readelf-real-program", readelf_real, args.raw_output_limit
        )
        commands.append(readelf_real)
        row["real_program_elf"] = {"path": str(program), "problems": problems}
        if problems:
            raise CaseError("static_elf", f"{identity} real program is not freestanding: {problems}")

        # 2. Run the real program: source checks prove rooted content and
        #    typed-error/defer behavior.
        real_execute = run("real-execute", [program], "behavior")
        if real_execute["exit"] != case["main_return"]:
            raise CaseError(
                "source_result",
                f"{identity} real program exit {real_execute['exit']} != {case['main_return']}",
            )
        if real_execute["_raw_stdout"] or real_execute["_raw_stderr"]:
            raise CaseError(
                "source_result",
                f"{identity} real program produced output: "
                f"{real_execute['_raw_stdout']!r} {real_execute['_raw_stderr']!r}",
            )

        # 3. Emit the same program as LLVM from the same helper, then link the
        #    test-only freestanding observer statically.
        observe_dir = case_dir / "observe"
        observe_dir.mkdir(parents=True, exist_ok=True)
        emit = run(
            "helper-emit-llvm",
            [
                args.helper,
                "--project",
                HERE / case["manifest"],
                "--out-object",
                observe_dir / "program.o",
                "--out-llvm",
                observe_dir / "program.ll",
                "--opt",
                str(opt),
            ],
            "build",
        )
        if emit["exit"] != 0:
            raise CaseError("build", f"{identity} helper emit exit {emit['exit']}")
        observe_build = run(
            "observer-link",
            [
                args.clang,
                f"-O{opt}",
                "-std=c++20",
                observe_dir / "program.ll",
                HERE / "observe_gc.cpp",
                args.runtime,
                "-o",
                observe_dir / "observe",
                "-nostdlib",
                "-static",
                "-Wl,--wrap=lang_alloc_v1",
                "-Wl,--wrap=lang_free_v1",
            ],
            "build",
        )
        if observe_build["exit"] != 0:
            raise CaseError("build", f"{identity} observer link exit {observe_build['exit']}")
        readelf_obs, problems = elf_is_freestanding(
            args.readelf, observe_dir / "observe", timeout, env
        )
        readelf_obs = record_command(
            case_dir, "readelf-observer", readelf_obs, args.raw_output_limit
        )
        commands.append(readelf_obs)
        row["observer_elf"] = {
            "path": str(observe_dir / "observe"),
            "problems": problems,
        }
        if problems:
            raise CaseError("static_elf", f"{identity} observer is not freestanding: {problems}")

        observe_execute = run("observe-execute", [observe_dir / "observe"], "behavior")
        if observe_execute["exit"] != 0:
            raise CaseError(
                "source_result",
                f"{identity} observer exit {observe_execute['exit']}",
            )
        if observe_execute["_raw_stdout"]:
            raise CaseError("source_result", f"{identity} observer wrote to stdout")

        # 4. Exact (address, generation) accounting.
        try:
            metrics = parse_events(observe_execute["_raw_stderr"])
        except ObserverError as error:
            raise CaseError("observer", f"{identity}: {error}") from error
        actual = attach_timeline(metrics)
        row["metrics"] = actual

        baseline_failures = compare_expected(
            case["baseline"], actual, "baseline", case["kind"]
        )
        target_failures = compare_expected(
            case["target"], actual, "target", case["kind"]
        )
        ctx = {"real_execute": real_execute, "observe_execute": observe_execute}
        for name in case["target"].get("checks", []):
            if name not in CHECKS:
                target_failures.append(
                    {
                        "check": name,
                        "expected": "known check",
                        "actual": "unknown check name",
                        "category": "metric_mismatch",
                    }
                )
                continue
            target_failures.extend(CHECKS[name](case, actual, metrics, ctx))
        row["baseline"] = {
            "ok": not baseline_failures,
            "failures": baseline_failures,
            "expected": case["baseline"],
        }
        row["target"] = {
            "ok": not target_failures,
            "failures": target_failures,
            "expected": case["target"],
        }
        if args.baseline_only:
            row["status"] = (
                "baseline_matched" if not baseline_failures else "baseline_mismatch"
            )
        else:
            row["status"] = "target_passed" if not target_failures else "target_failed"
    except CaseError as error:
        row["status"] = (
            "harness_error"
            if error.kind in ("build", "static_elf", "observer", "harness")
            else "behavior_error"
        )
        row["error_kind"] = error.kind
        row["failure"] = str(error)
    except Exception as error:  # noqa: BLE001 - one bad case must not abort the run
        row["status"] = "harness_error"
        row["error_kind"] = "internal"
        row["failure"] = str(error)
        row["traceback"] = traceback.format_exc()

    for command_row in commands:
        command_row.pop("_raw_stdout", None)
        command_row.pop("_raw_stderr", None)
    report["cases"].append(row)
    return row


def build_scope(catalog, selected, opts):
    all_ids = {case["id"] for case in catalog}
    selected_ids = {case["id"] for case in selected}
    full_case_set = selected_ids == all_ids and {0, 2}.issubset(set(opts))
    return {
        "mode": "full" if full_case_set else "selected",
        "selected_case_ids": [case["id"] for case in selected],
        "selected_opt_levels": sorted(set(opts)),
        "full_case_set": full_case_set,
    }


def build_coverage(selected, scope):
    ids = {case["id"] for case in selected}
    return {
        "controls": any(case["kind"] == "control" for case in selected),
        "cycles": any(case["kind"] == "cycle" for case in selected),
        "pressure": "loop-continue" in ids or "dead-cycle-live" in ids,
        "full_case_set": scope["full_case_set"],
        "opt_levels": scope["selected_opt_levels"],
        "not_covered": NOT_COVERED,
    }


def classify_outcome(report, catalog, selected, opts, baseline_only):
    """Return (status, gc_test_result, not_implemented, feature_claim)."""
    rows = report["cases"]
    controls = [row for row in rows if row["kind"] == "control"]
    cycles = [row for row in rows if row["kind"] == "cycle"]
    harness = [row for row in rows if row["status"] == "harness_error"]
    behavior = [row for row in rows if row["status"] == "behavior_error"]
    control_failures = [row for row in controls if not row["target"]["ok"]]
    cycle_failures = [row for row in cycles if not row["target"]["ok"]]

    if harness:
        kinds = {row.get("error_kind") for row in harness}
        result = (
            "inconclusive_observer_abi"
            if "observer" in kinds
            else "inconclusive_tool_or_build"
        )
        return "inconclusive", result, False, "none"
    if behavior:
        return "failed", "failed", False, "none"
    if control_failures:
        return "failed", "failed", False, "none"
    if baseline_only:
        if not cycles:
            return "passed", "control_only_passed", False, "none"
        if any(not row["baseline"]["ok"] for row in cycles):
            return "baseline_mismatch", "baseline_mismatch", False, "none"
        return "expected_failure", "not_implemented", True, "cycles_not_reclaimed"

    if not cycles:
        return "passed", "control_only_passed", False, "none"

    categories = {
        failure.get("category")
        for row in cycle_failures
        for failure in row["target"]["failures"]
    }
    if "metric_mismatch" in categories or "root_content" in categories:
        return "failed", "failed", False, "none"
    if cycle_failures:
        if categories <= {"reclamation", "budget"}:
            return "failed", "not_implemented", True, "cycles_not_reclaimed"
        return "failed", "failed", False, "none"

    scope = build_scope(catalog, selected, opts)
    if scope["full_case_set"]:
        return "passed", "full_suite_passed", False, "single_mutator_full_suite_cycles_passed"
    return "passed", "selected_scope_passed", False, "single_mutator_selected_scope_cycles_passed"


def compact_report(report, catalog):
    compact_cases = []
    for row in report["cases"]:
        metrics = row.get("metrics", {})
        target = row.get("target", {})
        baseline = row.get("baseline", {})
        compact_cases.append(
            {
                "id": row["id"],
                "kind": row["kind"],
                "opt": row["opt"],
                "status": row["status"],
                "error_kind": row.get("error_kind"),
                "source_hashes": row.get("source_hashes", {}),
                "real_program_elf": row.get("real_program_elf"),
                "observer_elf": row.get("observer_elf"),
                "metrics": {
                    key: metrics.get(key)
                    for key in (
                        "header_bytes",
                        "vec_allocations",
                        "vec_frees",
                        "live_vecs_at_exit",
                        "block_allocations",
                        "raw_allocations",
                        "peak_live_payload_bytes",
                        "final_live_payload_bytes",
                        "all_blocks_freed",
                        "live_ref_histogram",
                        "live_length_histogram",
                    )
                },
                "baseline_ok": baseline.get("ok"),
                "target_ok": target.get("ok"),
                "target_failures": [
                    {
                        "check": failure["check"],
                        "category": failure.get("category"),
                        "expected": failure["expected"],
                        "actual": failure["actual"],
                    }
                    for failure in target.get("failures", [])
                ],
            }
        )
    return {
        "schema": COMPACT_SCHEMA,
        "operation_id": report["operation_id"],
        "mode": report["mode"],
        "status": report["status"],
        "gc_test_result": report["gc_test_result"],
        "expected_failure": report["expected_failure"],
        "not_implemented": report["not_implemented"],
        "mem02_complete": False,
        "feature_claim": report["feature_claim"],
        "suite_scope": report["suite_scope"],
        "coverage": report["coverage"],
        "tool_pin": report["tool_pin"],
        "cases_sha256": report["cases_sha256"],
        "runner_sha256": report["runner_sha256"],
        "selftest_sha256": report["selftest_sha256"],
        "contract_sha256": report["contract_sha256"],
        "observer_source_sha256": report["observer_source_sha256"],
        "fixture_hashes": report["fixture_hashes"],
        "runs": compact_cases,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--readelf", type=Path, required=True)
    parser.add_argument("--libxml2-lib", type=Path)
    parser.add_argument("--root", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--opt", type=int, choices=(0, 2), action="append")
    parser.add_argument("--case", action="append")
    parser.add_argument("--baseline-only", action="store_true")
    parser.add_argument("--raw-output-limit", type=int, default=65536)
    parser.add_argument("--compact-out", type=Path)
    parser.add_argument("--expect-cli-sha256")
    parser.add_argument("--expect-helper-sha256")
    parser.add_argument("--expect-runtime-sha256")
    args = parser.parse_args()
    if not args.opt:
        args.opt = [0, 2]

    args.out.mkdir(parents=True, exist_ok=True)
    catalog_document = json.loads((HERE / "cases.json").read_text())
    catalog = catalog_document["cases"]
    cases = catalog
    if args.case:
        known = {case["id"] for case in cases}
        unknown = sorted(set(args.case) - known)
        if unknown:
            parser.error(f"unknown case(s): {', '.join(unknown)}")
        cases = [case for case in cases if case["id"] in set(args.case)]

    env = os.environ.copy()
    if args.libxml2_lib:
        env["LD_LIBRARY_PATH"] = (
            str(args.libxml2_lib)
            + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
        )

    report = {
        "schema": SCHEMA,
        "operation_id": catalog_document["operation_id"],
        "mode": "baseline-only" if args.baseline_only else "target",
        "status": "failed",
        "gc_test_result": "not_run",
        "expected_failure": False,
        "not_implemented": False,
        "mem02_complete": False,
        "feature_claim": "none",
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "root": str(args.root) if args.root else None,
        "cases_path": str(HERE / "cases.json"),
        "cases_sha256": digest(HERE / "cases.json"),
        "runner_sha256": digest(Path(__file__)),
        "selftest_sha256": digest(HERE / "selftest_checks.py"),
        "contract_sha256": digest(HERE / "CONTRACT.md"),
        "observer_source_sha256": digest(HERE / "observe_gc.cpp"),
        "fixture_hashes": {
            str(path.relative_to(HERE)): digest(path)
            for path in sorted((HERE / "fixtures").rglob("*"))
            if path.is_file()
        },
        "tool_pin": {},
        "environment": {
            "timeout_seconds": args.timeout,
            "opt_levels": sorted(set(args.opt)),
            "libxml2_lib": str(args.libxml2_lib) if args.libxml2_lib else None,
            "raw_output_limit": args.raw_output_limit,
        },
        "limitations": LIMITATIONS,
        "cases": [],
    }
    scope = build_scope(catalog, cases, args.opt)
    report["suite_scope"] = scope
    report["coverage"] = build_coverage(cases, scope)

    # Pin the exact build products. A changed CLI is not a GC result; it makes
    # the run inconclusive unless a stable expected hash was supplied.
    tool_paths = {
        "cli": args.cli,
        "helper": args.helper,
        "runtime": args.runtime,
        "clang": args.clang,
        "readelf": args.readelf,
    }
    for name, path in tool_paths.items():
        if not path.is_file():
            report["status"] = "inconclusive"
            report["gc_test_result"] = "inconclusive_tool_or_build"
            report["failure"] = f"missing tool: {name}={path}"
            report["tool_pin"] = {
                "actual_start": {},
                "expected": {},
                "changed": {},
            }
            (args.out / "summary.json").write_text(json.dumps(report, indent=2))
            print(json.dumps({"status": report["status"], "failure": report["failure"]}))
            return 2
    actual_start = {name: digest(path) for name, path in tool_paths.items()}
    expected = {
        "cli": args.expect_cli_sha256 or actual_start["cli"],
        "helper": args.expect_helper_sha256 or actual_start["helper"],
        "runtime": args.expect_runtime_sha256 or actual_start["runtime"],
        "clang": actual_start["clang"],
        "readelf": actual_start["readelf"],
    }
    mismatched = {
        name: {"expected": expected[name], "actual": actual_start[name]}
        for name in ("cli", "helper", "runtime")
        if expected[name] != actual_start[name]
    }
    helper_embedded = str(args.helper.resolve()).encode() in args.cli.read_bytes()
    report["tool_pin"] = {
        "expected": expected,
        "actual_start": actual_start,
        "changed": mismatched,
        "helper_path_embedded_in_cli": helper_embedded,
        "paths": {name: str(path) for name, path in tool_paths.items()},
    }
    if mismatched or not helper_embedded:
        report["status"] = "inconclusive"
        report["gc_test_result"] = "inconclusive_tool_pin"
        report["failure"] = (
            "pinned CLI/helper/runtime mismatch"
            if mismatched
            else "woozyc does not embed the supplied helper path"
        )
        (args.out / "summary.json").write_text(json.dumps(report, indent=2))
        print(
            json.dumps(
                {
                    "status": report["status"],
                    "gc_test_result": report["gc_test_result"],
                    "failure": report["failure"],
                }
            )
        )
        return 2
    clang_version = command([args.clang, "--version"], args.timeout, env)
    report["tool_pin"]["clang_version"] = clang_version
    if clang_version["exit"] != 0 or "22.1.8" not in clang_version["stdout"]:
        report["status"] = "inconclusive"
        report["gc_test_result"] = "inconclusive_tool_or_build"
        report["failure"] = "clang is not the fixed 22.1.8 toolchain"
        (args.out / "summary.json").write_text(json.dumps(report, indent=2))
        print(json.dumps({"status": report["status"], "failure": report["failure"]}))
        return 2

    for case in cases:
        for opt in args.opt:
            run_case(case, opt, args, env, report)

    actual_end = {
        name: digest(path) for name, path in tool_paths.items() if name in ("cli", "helper", "runtime")
    }
    changed = {
        name: {"start": actual_start[name], "end": actual_end[name]}
        for name in actual_end
        if actual_start[name] != actual_end[name]
    }
    report["tool_pin"]["actual_end"] = actual_end
    report["tool_pin"]["changed"] = changed

    status, gc_result, not_implemented, feature_claim = classify_outcome(
        report, catalog, cases, args.opt, args.baseline_only
    )
    if changed:
        status = "inconclusive"
        gc_result = "inconclusive_tool_pin"
        not_implemented = False
        feature_claim = "none"
    report["status"] = status
    report["gc_test_result"] = gc_result
    report["expected_failure"] = gc_result == "not_implemented" and not changed
    report["not_implemented"] = not_implemented
    report["feature_claim"] = feature_claim
    report["counts"] = {
        "selected_cases": len(cases),
        "runs": len(report["cases"]),
        "harness_errors": sum(r["status"] == "harness_error" for r in report["cases"]),
        "behavior_errors": sum(r["status"] == "behavior_error" for r in report["cases"]),
        "control_failures": sum(
            r["kind"] == "control" and not r["target"]["ok"] for r in report["cases"]
        ),
        "cycle_target_failures": sum(
            r["kind"] == "cycle" and not r["target"]["ok"] for r in report["cases"]
        ),
        "cycle_baseline_failures": sum(
            r["kind"] == "cycle" and not r["baseline"]["ok"] for r in report["cases"]
        ),
    }
    report["finished_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    (args.out / "summary.json").write_text(json.dumps(report, indent=2))
    if args.compact_out:
        args.compact_out.parent.mkdir(parents=True, exist_ok=True)
        args.compact_out.write_text(json.dumps(compact_report(report, catalog), indent=2))

    print(
        json.dumps(
            {
                "mode": report["mode"],
                "status": report["status"],
                "gc_test_result": report["gc_test_result"],
                "expected_failure": report["expected_failure"],
                "not_implemented": report["not_implemented"],
                "mem02_complete": False,
                "feature_claim": report["feature_claim"],
                "suite_scope": report["suite_scope"],
                "coverage": {
                    key: report["coverage"][key]
                    for key in ("controls", "cycles", "pressure", "full_case_set")
                },
                "counts": report["counts"],
                "tool_pin_changed": report["tool_pin"].get("changed", {}),
                "failed_runs": [
                    {
                        "id": row["id"],
                        "opt": row["opt"],
                        "status": row["status"],
                        "error_kind": row.get("error_kind"),
                        "failure": row.get("failure"),
                        "target_failures": row["target"]["failures"],
                    }
                    for row in report["cases"]
                    if row["status"] in (
                        "failed",
                        "target_failed",
                        "baseline_mismatch",
                        "harness_error",
                        "behavior_error",
                    )
                ],
            }
        )
    )
    if status in ("passed", "expected_failure"):
        return 0
    return 2 if status == "inconclusive" else 1


if __name__ == "__main__":
    raise SystemExit(main())
