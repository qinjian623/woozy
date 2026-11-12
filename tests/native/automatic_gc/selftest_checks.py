#!/usr/bin/env python3
"""Self-test for the automatic-GC metric/checker logic (synthetic, not a GC run).

This feeds hand-written observation streams to the same parser and checks used
by run_automatic_gc.py. It verifies the correction-specific rules:

* object identity is (address, generation), so a legal FREE + reuse of the same
  address does not overwrite the earlier generation's timeline;
* collector metadata allocations are excluded from Vec payload bytes and from
  "last rooted Vec" ordering;
* a collected synthetic pressure stream passes the payload budget while a leak
  still fails;
* a control-only selection reports `control_only_passed`, never a GC claim.

It is not evidence that the current Native runtime collects cycles. Real
programs and their event streams come from run_automatic_gc.py.
"""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "automatic_gc_runner", HERE / "run_automatic_gc.py"
)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

HEADER = 80
LIVE = "0x00000000000000aa"
DEAD = "0x00000000000000bb"
MARKER_A = "0x00000000000000cc"
MARKER_B = "0x00000000000000dd"
EMPTY_A = "0x00000000000000c1"
EMPTY_B = "0x00000000000000c2"
METADATA = "0x0000000000000eee"


def line(text, events):
    events.append(text)


def alloc(pointer, size, events):
    line(f"ALLOC:{pointer}:{size}", events)


def vec_alloc(pointer, length, stride, events):
    alloc(pointer, HEADER + length * stride, events)
    line(f"VEC:1:{pointer}:{length}", events)


def retain(pointer, refs, events):
    line(f"VEC:2:{pointer}:{refs}", events)


def release(pointer, refs, events):
    line(f"VEC:3:{pointer}:{refs}", events)


def free(pointer, length, events):
    for index in range(length - 1, -1, -1):
        line(f"VEC:4:{pointer}:{index}", events)
    line(f"VEC:5:{pointer}:0", events)
    line(f"FREE:{pointer}", events)


def raw_free(pointer, events):
    line(f"FREE:{pointer}", events)


def stream(events):
    return "\n".join(["CONFIG:vec_header_bytes:80", *events]) + "\n"


def collected_small(case_id):
    events = []
    if case_id == "control-empty":
        pass
    elif case_id == "control-acyclic":
        for pointer, length, stride in (
            (EMPTY_A, 0, 16),
            (EMPTY_B, 0, 16),
            (LIVE, 1, 16),
            (DEAD, 1, 16),
        ):
            vec_alloc(pointer, length, stride, events)
        for pointer, length in ((EMPTY_A, 0), (EMPTY_B, 0), (LIVE, 1), (DEAD, 1)):
            release(pointer, 0, events)
            free(pointer, length, events)
    elif case_id == "self-cycle":
        vec_alloc(EMPTY_A, 0, 16, events)
        free(EMPTY_A, 0, events)
        vec_alloc(DEAD, 1, 16, events)
        retain(DEAD, 2, events)
        retain(DEAD, 3, events)
        release(DEAD, 2, events)
        release(DEAD, 1, events)
        release(DEAD, 0, events)
        free(DEAD, 1, events)
    elif case_id == "duplicate-edges":
        vec_alloc(EMPTY_A, 0, 16, events)
        vec_alloc(EMPTY_B, 0, 16, events)
        vec_alloc(DEAD, 1, 24, events)
        retain(DEAD, 2, events)
        retain(DEAD, 3, events)
        retain(DEAD, 4, events)
        release(EMPTY_A, 0, events)
        free(EMPTY_A, 0, events)
        release(EMPTY_B, 0, events)
        free(EMPTY_B, 0, events)
        release(DEAD, 3, events)
        release(DEAD, 2, events)
        release(DEAD, 1, events)
        release(DEAD, 0, events)
        free(DEAD, 1, events)
    elif case_id == "two-cycle":
        vec_alloc(EMPTY_A, 0, 16, events)
        vec_alloc(EMPTY_B, 0, 16, events)
        vec_alloc(LIVE, 1, 16, events)
        vec_alloc(DEAD, 1, 16, events)
        retain(LIVE, 2, events)
        retain(DEAD, 2, events)
        release(EMPTY_A, 0, events)
        free(EMPTY_A, 0, events)
        release(EMPTY_B, 0, events)
        free(EMPTY_B, 0, events)
        release(LIVE, 1, events)
        release(DEAD, 1, events)
        release(LIVE, 0, events)
        release(DEAD, 0, events)
        free(LIVE, 1, events)
        free(DEAD, 1, events)
    elif case_id == "enum-nested-cycle":
        vec_alloc(DEAD, 1, 24, events)
        retain(DEAD, 2, events)
        release(DEAD, 1, events)
        release(DEAD, 0, events)
        free(DEAD, 1, events)
    elif case_id == "typed-throw-defer":
        vec_alloc(EMPTY_A, 0, 16, events)
        free(EMPTY_A, 0, events)
        vec_alloc(DEAD, 1, 16, events)
        retain(DEAD, 2, events)
        retain(DEAD, 3, events)
        release(DEAD, 2, events)
        release(DEAD, 1, events)
        release(DEAD, 0, events)
        free(DEAD, 1, events)
    else:
        raise AssertionError(f"missing synthetic stream for {case_id}")
    return stream(events)


def dead_to_live_events(reuse_dead_address=False, metadata_after_live=False):
    """Collected dead-cycle/live-root/markers shape with unique lengths."""
    events = []
    if metadata_after_live:
        alloc(METADATA, 64, events)  # raw collector metadata, no VEC:1
    vec_alloc(LIVE, 9, 8, events)
    vec_alloc(EMPTY_A, 0, 16, events)
    vec_alloc(EMPTY_B, 0, 16, events)
    vec_alloc(DEAD, 7, 16, events)
    retain(DEAD, 2, events)  # self edge
    retain(DEAD, 3, events)  # local root
    retain(LIVE, 2, events)  # dead cycle -> live edge
    release(EMPTY_A, 0, events)
    free(EMPTY_A, 0, events)
    release(EMPTY_B, 0, events)
    free(EMPTY_B, 0, events)
    release(DEAD, 2, events)

    # Pressure stand-in: a few dead 1-element ~1 KiB cycles.
    for i in range(4):
        payload = f"0x0000000000001{i:03x}"
        ring = f"0x0000000000002{i:03x}"
        vec_alloc(payload, 128, 8, events)
        vec_alloc(ring, 1, 16, events)
        retain(ring, 2, events)
        retain(payload, 2, events)
        release(payload, 1, events)
        release(ring, 1, events)
        release(payload, 0, events)
        release(ring, 0, events)
        free(payload, 128, events)
        free(ring, 1, events)

    if reuse_dead_address:
        # The watched dead cycle is reclaimed after the pressure phase; its
        # address is then legally reused by marker_a (generation 2).
        release(LIVE, 1, events)
        release(DEAD, 1, events)
        release(DEAD, 0, events)
        free(DEAD, 7, events)
        marker_a_pointer = DEAD
    else:
        marker_a_pointer = MARKER_A
    vec_alloc(marker_a_pointer, 3, 8, events)
    vec_alloc(MARKER_B, 5, 8, events)
    if not reuse_dead_address:
        # Normal single-thread final collection at main return.
        release(LIVE, 1, events)
        release(DEAD, 1, events)
        release(DEAD, 0, events)
        free(DEAD, 7, events)
    release(marker_a_pointer, 0, events)
    release(MARKER_B, 0, events)
    free(marker_a_pointer, 3, events)
    free(MARKER_B, 5, events)
    # Main returns: the rooted live Vec is released last.
    release(LIVE, 0, events)
    free(LIVE, 9, events)
    if metadata_after_live:
        raw_free(METADATA, events)
    return stream(events)


def run_target(case, events, ctx=None):
    metrics = runner.parse_events(events)
    actual = runner.attach_timeline(metrics)
    ctx = ctx or {
        "real_execute": {"exit": 0},
        "observe_execute": {"exit": 0},
    }
    failures = runner.compare_expected(case["target"], actual, "target", case["kind"])
    for name in case["target"].get("checks", []):
        failures.extend(runner.CHECKS[name](case, actual, metrics, ctx))
    return metrics, actual, failures


def main():
    catalog_document = json.loads((HERE / "cases.json").read_text())
    catalog = catalog_document["cases"]
    ctx = {"real_execute": {"exit": 0}, "observe_execute": {"exit": 0}}
    failures = []

    # Small collected programs must satisfy their target metrics.
    for case in catalog:
        if case["id"] in ("loop-continue", "dead-cycle-live"):
            continue
        events = collected_small(case["id"])
        metrics, actual, target_failures = run_target(case, events, ctx)
        print(
            f"{case['id']:20s} synthetic-collected "
            f"{'passed' if not target_failures else 'FAILED'}"
        )
        if target_failures:
            failures.append({"id": case["id"], "failures": target_failures})

    # A real leak must still be rejected.
    leak_events = [line for line in collected_small("self-cycle").splitlines()]
    leak_text = "\n".join(
        line for line in leak_events
        if not line.startswith("VEC:3:0x00000000000000bb:0")
        and line != "VEC:5:0x00000000000000bb:0"
        and line != "FREE:0x00000000000000bb"
        and line != "VEC:3:0x00000000000000bb:1"
    ) + "\n"
    self_case = next(c for c in catalog if c["id"] == "self-cycle")
    leak_metrics = runner.parse_events(leak_text)
    leak_actual = runner.attach_timeline(leak_metrics)
    leak_failures = runner.compare_expected(
        self_case["target"], leak_actual, "target", self_case["kind"]
    )
    for name in self_case["target"].get("checks", []):
        leak_failures.extend(
            runner.CHECKS[name](self_case, leak_actual, leak_metrics, ctx)
        )
    print(f"{'self-cycle-leak':20s} synthetic-leak "
          f"{'correctly rejected' if leak_failures else 'NOT REJECTED'}")
    if not leak_failures:
        failures.append({"id": "self-cycle-leak", "failures": ["target accepted a leak"]})

    # Address reuse: the dead cycle's address is reused by marker_a. The
    # timeline must keep generation 1 for the dead cycle and generation 2 for
    # the marker.
    reuse = dead_to_live_events(reuse_dead_address=True)
    reuse_metrics = runner.parse_events(reuse)
    reuse_actual = runner.attach_timeline(reuse_metrics)
    dead_case = next(c for c in catalog if c["id"] == "dead-cycle-live")
    dead_check = runner.check_dead_cycle_live_edge_release(reuse_actual)
    timeline = reuse_actual["edge_timeline"]
    generation_ok = (
        timeline["dead_cycle"]["identity"]["pointer"] == DEAD
        and timeline["dead_cycle"]["identity"]["generation"] == 1
        and timeline["marker_a"]["identity"]["pointer"] == DEAD
        and timeline["marker_a"]["identity"]["generation"] == 2
    )
    print(
        f"{'address-reuse':20s} synthetic-collected "
        f"{'passed' if generation_ok and not dead_check else 'FAILED'}"
    )
    if not generation_ok or dead_check:
        failures.append(
            {
                "id": "address-reuse",
                "generation_ok": generation_ok,
                "failures": dead_check,
                "timeline": timeline,
            }
        )

    # Collector metadata may be allocated first and freed after the live Vec.
    # It is a raw block: excluded from payload budget and Vec free order.
    metadata = dead_to_live_events(metadata_after_live=True)
    metadata_metrics = runner.parse_events(metadata)
    metadata_actual = runner.attach_timeline(metadata_metrics)
    metadata_check = runner.check_dead_cycle_live_edge_release(metadata_actual)
    metadata_ok = (
        metadata_actual["raw_allocations"] == 1
        and metadata_actual["final_live_payload_bytes"] == 0
        and metadata_actual["all_blocks_freed"]
        and not metadata_check
    )
    print(
        f"{'metadata-free-order':20s} synthetic-collected "
        f"{'passed' if metadata_ok else 'FAILED'}"
    )
    if not metadata_ok:
        failures.append(
            {
                "id": "metadata-free-order",
                "metrics": metadata_actual,
                "failures": metadata_check,
            }
        )

    # Budget accounting: raw metadata and Vec headers are not application
    # payload; a leaked Vec payload exceeds the budget.
    budget_case = {
        "kind": "cycle",
        "main_return": 0,
        "payload_budget_bytes": 4096,
        "target": {},
    }
    over = {
        "peak_live_payload_bytes": 4097,
        "final_live_payload_bytes": 4097,
    }
    under = {
        "peak_live_payload_bytes": 4096,
        "final_live_payload_bytes": 0,
    }
    over_failures = runner.check_pressure_budget(budget_case, over)
    under_failures = runner.check_pressure_budget(budget_case, under)
    budget_ok = bool(over_failures) and not under_failures
    print(f"{'payload-budget':20s} synthetic-check "
          f"{'passed' if budget_ok else 'FAILED'}")
    if not budget_ok:
        failures.append({"id": "payload-budget", "over": over_failures, "under": under_failures})

    # Scope/coverage must not turn a controls-only run into a GC claim.
    control_row = {
        "id": "control-empty",
        "kind": "control",
        "status": "target_passed",
        "baseline": {"ok": True},
        "target": {"ok": True, "failures": []},
    }
    control_case = next(c for c in catalog if c["id"] == "control-empty")
    status, result, not_implemented, claim = runner.classify_outcome(
        {"cases": [control_row]}, catalog, [control_case], [0], False
    )
    control_only_ok = (
        status == "passed"
        and result == "control_only_passed"
        and not not_implemented
        and claim == "none"
    )
    print(f"{'control-only-scope':20s} synthetic-classify "
          f"{'passed' if control_only_ok else 'FAILED'}")
    if not control_only_ok:
        failures.append(
            {
                "id": "control-only-scope",
                "status": status,
                "gc_test_result": result,
                "not_implemented": not_implemented,
                "feature_claim": claim,
            }
        )

    # A full selected suite that passes is still only the single-mutator suite.
    full_rows = []
    for case in catalog:
        full_rows.append(
            {
                "id": case["id"],
                "kind": case["kind"],
                "status": "target_passed",
                "baseline": {"ok": True},
                "target": {"ok": True, "failures": []},
            }
        )
    status, result, not_implemented, claim = runner.classify_outcome(
        {"cases": full_rows}, catalog, catalog, [0, 2], False
    )
    full_ok = (
        status == "passed"
        and result == "full_suite_passed"
        and not not_implemented
        and claim == "single_mutator_full_suite_cycles_passed"
    )
    print(f"{'full-suite-scope':20s} synthetic-classify "
          f"{'passed' if full_ok else 'FAILED'}")
    if not full_ok:
        failures.append({"id": "full-suite-scope", "status": status, "result": result})

    # A build/tool failure is inconclusive, not evidence of missing GC.
    harness_row = {
        "id": "self-cycle",
        "kind": "cycle",
        "status": "harness_error",
        "error_kind": "build",
        "baseline": {"ok": False},
        "target": {"ok": False, "failures": []},
    }
    status, result, not_implemented, claim = runner.classify_outcome(
        {"cases": [harness_row]},
        catalog,
        [next(c for c in catalog if c["id"] == "self-cycle")],
        [0],
        False,
    )
    harness_ok = (
        status == "inconclusive"
        and result == "inconclusive_tool_or_build"
        and not not_implemented
    )
    print(f"{'harness-failure':20s} synthetic-classify "
          f"{'passed' if harness_ok else 'FAILED'}")
    if not harness_ok:
        failures.append({"id": "harness-failure", "status": status, "result": result})

    output = {
        "schema": "woozy/native-automatic-gc-selftest/2",
        "status": "passed" if not failures else "failed",
        "note": "Synthetic checker self-test only; not a real program or GC evidence.",
        "failures": failures,
    }
    print(json.dumps(output, indent=2))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
