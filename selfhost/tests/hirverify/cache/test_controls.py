#!/usr/bin/env python3
"""Controls for cache/run.py report, elapsed and repeat-loop validation.

These use the real runner functions and bounded process helper with tiny fake
programs.  They do not compile Woozy source, run the retained ELF drivers, or
claim a verifier result.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
RUNNER = HERE / "run.py"
sys.path.insert(0, str(HERE.parents[2]))
from corpus import _run_process_impl  # noqa: E402

spec = importlib.util.spec_from_file_location("cache_runner_controls", RUNNER)
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


def fail(message: str) -> None:
    raise RuntimeError(message)


def report(small: Path, large: Path, summary: str | None = None,
           extra: list[str] | None = None) -> str:
    lines = [
        f"MODULE {small / 'lang.toml'} nodes=105 functions=3 templates=5",
        f"MODULE {large / 'lang.toml'} nodes=830 functions=42 templates=5",
        "KEY module-file-node distinct",
        "BAD_SOURCE rejected=1 failed=1 code=E_IR_INVALID",
        "BAD_SOURCE rejected=1 failed=1 code=E_IR_INVALID",
        "ELAPSED_NS 10",
        summary or "ITERATIONS 2 MODULES 2 VERIFIES 4 BAD 0",
    ]
    if extra:
        lines.extend(extra)
    return "\n".join(lines) + "\n"


def expect_report(label: str, text: str, small: Path, large: Path,
                  accepted: bool, observations: list[dict[str, object]]) -> None:
    try:
        cache.check_driver_report(text, small, large, 2)
        observed = "accepted"
    except RuntimeError as error:
        observed = f"rejected: {error}"
    observations.append({"label": label, "observed": observed,
                         "accepted": accepted})
    if (observed == "accepted") != accepted:
        fail(f"{label}: {observed}")


def expect_elapsed(label: str, text: str, accepted: bool,
                   observations: list[dict[str, object]]) -> None:
    try:
        value = cache.parse_elapsed(text)
        observed = value
        ok = accepted
    except RuntimeError as error:
        observed = f"rejected: {error}"
        ok = not accepted
    observations.append({"label": label, "observed": observed,
                         "accepted": accepted})
    if not ok:
        fail(f"{label}: {observed}")


def fake_program(path: Path, counter: Path, extra_first: bool,
                 summary: str, elapsed: str) -> None:
    extra_check = "old == 0" if extra_first else "False"
    path.write_text(
        "#!/usr/bin/env python3\n"
        "from pathlib import Path\n"
        "import sys\n"
        f"counter = Path({str(counter)!r})\n"
        "old = int(counter.read_text()) if counter.exists() else 0\n"
        "counter.write_text(str(old + 1))\n"
        "print(f'MODULE {sys.argv[2]} nodes=105 functions=3 templates=5')\n"
        "print(f'MODULE {sys.argv[4]} nodes=830 functions=42 templates=5')\n"
        "print('KEY module-file-node distinct')\n"
        "print('BAD_SOURCE rejected=1 failed=1 code=E_IR_INVALID')\n"
        "print('BAD_SOURCE rejected=1 failed=1 code=E_IR_INVALID')\n"
        f"if {extra_check}:\n"
        "    print('CONTROL first-pair-diff')\n"
        f"print('ELAPSED_NS {elapsed}')\n"
        f"print({summary!r})\n",
        encoding="utf-8")
    path.chmod(0o700)


def runner_process(new_program: Path, baseline_program: Path, work: Path,
                   temp: Path, repeats: int = 2) -> dict[str, object]:
    woozyc = temp / "fake-woozyc"
    woozyc.write_text("fake\n", encoding="utf-8")
    argv = [sys.executable, "-B", str(RUNNER), "--woozyc", str(woozyc),
            "--toolchain-root", str(temp / "toolchain"),
            "--work-dir", str(work), "--new-program", str(new_program),
            "--baseline-program", str(baseline_program),
            "--iterations", "2", "--repeats", str(repeats),
            "--run-timeout", "30"]
    process = _run_process_impl(argv, temp,
                                dict(os.environ, PYTHONDONTWRITEBYTECODE="1"),
                                60)
    record = {key: process[key] for key in
              ("exit", "timed_out", "cleanup_complete", "abandoned_pipes")}
    record["argv"] = argv
    if (process["timed_out"] or not process["cleanup_complete"]
            or process["abandoned_pipes"]):
        fail(f"runner process flags not clean: {record}")
    return record


def find_results(work: Path) -> Path:
    results = sorted(work.glob("hirverify-cache-*/results.json"))
    if len(results) != 1:
        fail(f"expected one results.json, got {results!r}")
    return results[0]


def main() -> int:
    observations: dict[str, object] = {
        "runner_sha256": hashlib.sha256(RUNNER.read_bytes()).hexdigest(),
        "control_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "scope": "synthetic Python report/elapsed/repeat controls; no Wz pass",
    }
    report_observations: list[dict[str, object]] = []
    elapsed_observations: list[dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="cache-controls-") as raw:
        temp = Path(raw)
        small = temp / "small fixture"
        large = temp / "large fixture"
        expect_report("valid_paths_with_spaces", report(small, large),
                      small, large, True, report_observations)
        for label, summary in (
            ("zero_all_counts", "ITERATIONS 0 MODULES 0 VERIFIES 0 BAD 0"),
            ("wrong_iterations", "ITERATIONS 1 MODULES 2 VERIFIES 2 BAD 0"),
            ("wrong_modules", "ITERATIONS 2 MODULES 1 VERIFIES 2 BAD 0"),
            ("zero_verifies", "ITERATIONS 2 MODULES 2 VERIFIES 0 BAD 0"),
            ("wrong_verifies", "ITERATIONS 2 MODULES 2 VERIFIES 3 BAD 0"),
            ("nonzero_bad", "ITERATIONS 2 MODULES 2 VERIFIES 4 BAD 1"),
            ("missing_summary", ""),
            ("duplicate_summary",
             "ITERATIONS 2 MODULES 2 VERIFIES 4 BAD 0\n"
             "ITERATIONS 2 MODULES 2 VERIFIES 4 BAD 0"),
        ):
            text = report(small, large, summary=summary)
            if label == "missing_summary":
                text = text.replace(
                    "ITERATIONS 2 MODULES 2 VERIFIES 4 BAD 0\n", "")
            expect_report(label, text, small, large, False, report_observations)
        expect_report("duplicate_key",
                      report(small, large, extra=["KEY module-file-node distinct"]),
                      small, large, False, report_observations)
        expect_report("duplicate_module",
                      report(small, large, extra=[
                          f"MODULE {small / 'lang.toml'} "
                          "nodes=105 functions=3 templates=5"]),
                      small, large, False, report_observations)
        expect_report("missing_bad_source",
                      report(small, large).replace(
                          "BAD_SOURCE rejected=1 failed=1 code=E_IR_INVALID\n",
                          "", 1),
                      small, large, False, report_observations)

        expect_elapsed("positive", "ELAPSED_NS 10\n", True,
                       elapsed_observations)
        for label, text in (
            ("zero", "ELAPSED_NS 0\n"),
            ("negative", "ELAPSED_NS -1\n"),
            ("missing", "ITERATIONS 1\n"),
            ("duplicate", "ELAPSED_NS 1\nELAPSED_NS 2\n"),
            ("not_integer", "ELAPSED_NS abc\n"),
            ("extra_token", "ELAPSED_NS 1 2\n"),
            ("plus_sign", "ELAPSED_NS +1\n"),
        ):
            expect_elapsed(label, text, False, elapsed_observations)

        mismatch = temp / "mismatch"
        mismatch.mkdir()
        new_bad = mismatch / "new"
        old_ok = mismatch / "old"
        fake_program(new_bad, mismatch / "new.count", True,
                     "ITERATIONS 2 MODULES 2 VERIFIES 4 BAD 0", "10")
        fake_program(old_ok, mismatch / "old.count", False,
                     "ITERATIONS 2 MODULES 2 VERIFIES 4 BAD 0", "10")
        mismatch_record = runner_process(new_bad, old_ok, mismatch / "work",
                                         temp, repeats=2)
        mismatch_result = json.loads(find_results(mismatch / "work").read_bytes())
        if mismatch_result.get("status") != "failed":
            fail(f"first-round mismatch was accepted: {mismatch_result}")
        if "repeat 0" not in str(mismatch_result.get("error", "")):
            fail(f"mismatch error does not identify repeat 0: {mismatch_result}")

        valid = temp / "valid"
        valid.mkdir()
        new_ok = valid / "new"
        old_ok = valid / "old"
        fake_program(new_ok, valid / "new.count", False,
                     "ITERATIONS 2 MODULES 2 VERIFIES 4 BAD 0", "10")
        fake_program(old_ok, valid / "old.count", False,
                     "ITERATIONS 2 MODULES 2 VERIFIES 4 BAD 0", "10")
        valid_record = runner_process(new_ok, old_ok, valid / "work", temp,
                                      repeats=2)
        valid_result = json.loads(find_results(valid / "work").read_bytes())
        if valid_result.get("status") != "passed":
            fail(f"valid fake report was rejected: {valid_result}")
        rounds = valid_result.get("rounds", [])
        if len(rounds) != 2 or any(
                not item.get("non_timing_output_equal") for item in rounds):
            fail(f"valid rounds were not compared/stored: {rounds!r}")
        if valid_result.get("elapsed_ns") != {"new": [10, 10],
                                              "baseline": [10, 10]}:
            fail(f"valid elapsed values changed: {valid_result.get('elapsed_ns')}")

        zero = temp / "zero"
        zero.mkdir()
        new_zero = zero / "new"
        old_zero = zero / "old"
        fake_program(new_zero, zero / "new.count", False,
                     "ITERATIONS 2 MODULES 2 VERIFIES 0 BAD 0", "10")
        fake_program(old_zero, zero / "old.count", False,
                     "ITERATIONS 2 MODULES 2 VERIFIES 0 BAD 0", "10")
        zero_record = runner_process(new_zero, old_zero, zero / "work", temp,
                                     repeats=1)
        zero_result = json.loads(find_results(zero / "work").read_bytes())
        if zero_result.get("status") != "failed":
            fail(f"zero-verify summary was accepted: {zero_result}")

    observations["report_controls"] = report_observations
    observations["elapsed_controls"] = elapsed_observations
    observations["integration_controls"] = {
        "first_round_mismatch": mismatch_record,
        "first_round_mismatch_status": mismatch_result.get("status"),
        "first_round_mismatch_error": mismatch_result.get("error"),
        "valid_pair": valid_record,
        "valid_pair_status": valid_result.get("status"),
        "valid_pair_rounds": valid_result.get("rounds"),
        "zero_verifies": zero_record,
        "zero_verifies_status": zero_result.get("status"),
    }
    print(json.dumps(observations, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
