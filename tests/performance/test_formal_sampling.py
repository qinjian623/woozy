"""Protocol self-checks with synthetic times, not benchmark results."""
from pathlib import Path
import tempfile
import unittest
import copy
from unittest.mock import patch

import formal_sampling as formal


def record(ns, **changes):
    return {"status": "exited", "exit": 0, "stdout": "", "stderr": "",
            "elapsed_ns": ns, **changes}


class SamplingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.cpp = self.root / "cpp"
        self.source.write_bytes(b"source bytes")
        self.cpp.write_bytes(b"cpp bytes")
        self.double = self.root / "cpp-double"
        self.double.write_bytes(b"double-round cpp bytes")
        self.calibration = {"status": "selected", "rounds": 1, "trials": [
            {"rounds": r, "program": str(p), "sha256": formal.common.sha256_file(p),
             "runs": [record(500_000_000 * r) for _ in range(3)]}
            for r, p in ((1, self.cpp), (2, self.double))]}

    def sample(self, run):
        return formal.sample_pair(self.source, self.cpp, run, self.calibration)

    def test_calibration_first_sufficient_and_double(self):
        built = []
        counts = {}
        def build(rounds):
            built.append(rounds)
            path = self.root / str(rounds)
            path.write_text(str(rounds))
            return path
        def run(path):
            rounds = int(path.name)
            counts[rounds] = counts.get(rounds, 0) + 1
            # R2 has a sufficient median but an insufficient minimum.
            values = {1: [100_000_000] * 3,
                      2: [249_999_999, 600_000_000, 700_000_000],
                      4: [250_000_000, 500_000_000, 600_000_000],
                      8: [1_000_000_000] * 3}
            return record(values[rounds][counts[rounds] - 1])
        result = formal.calibrate(build, run)
        self.assertEqual(result["rounds"], 4)
        self.assertEqual(built, [1, 2, 4, 8])
        self.assertEqual(counts, {1: 3, 2: 3, 4: 3, 8: 3})
        self.assertTrue(result["work_review_required"])

    def test_calibration_max_does_not_force_a_result(self):
        def build(rounds):
            path = self.root / str(rounds); path.write_text(str(rounds)); return path
        with patch.object(formal, "MAX_ROUNDS", 2):
            result = formal.calibrate(build, lambda path: record(1))
        self.assertEqual(result["status"], "incomplete")
        self.assertIsNone(result["rounds"])

    def test_calibration_failure_is_not_a_short_sample(self):
        with self.assertRaises(formal.common.PerfError):
            formal.calibrate(lambda rounds: self.cpp, lambda path: record(1, exit=9))

    def test_calibration_preserves_distinct_unchanged_programs(self):
        with self.assertRaisesRegex(formal.common.PerfError, "distinct"):
            formal.calibrate(lambda rounds: self.cpp, lambda path: record(100_000_000))
        def mutate(path):
            self.cpp.write_bytes(b"changed")
            return record(100_000_000)
        with self.assertRaisesRegex(formal.common.PerfError, "changed"):
            formal.calibrate(lambda rounds: self.cpp, mutate)

    def test_exact_order_samples_and_two_times_boundary(self):
        order = []
        def run(path):
            order.append(path.name)
            return record(1_000_000_000 if path == self.source else 500_000_000)
        result = self.sample(run)
        self.assertEqual(order, ["source", "cpp", "cpp", "source"] +
                         [name for i in range(9) for name in
                          (("source", "cpp") if i % 2 == 0 else ("cpp", "source"))])
        self.assertEqual(len(result["runs"]), 22)
        self.assertEqual(len(result["stats"]["source"]["samples_ns"]), 9)
        self.assertEqual(result["ratio"], 2)
        self.assertTrue(result["threshold_passed"])
        failed = self.sample(lambda p: record(1_005_000_000 if p == self.source else 500_000_000))
        self.assertEqual(failed["status"], "sampled")
        self.assertFalse(failed["threshold_passed"])

    def test_failure_kept_and_not_replaced(self):
        count = 0
        def run(path):
            nonlocal count
            count += 1
            return record(100, status="timeout", exit=None) if count == 6 else record(100)
        result = self.sample(run)
        self.assertEqual(count, 6)
        self.assertEqual(len(result["runs"]), 6)
        self.assertEqual(result["runs"][-1]["status"], "timeout")
        self.assertFalse(result["threshold_applied"])
        self.assertIsNone(result["ratio"])

    def test_output_and_binary_mutation_rejected(self):
        result = self.sample(lambda p: record(100, stdout="wrong"))
        self.assertEqual(result["status"], "failed")
        def mutate(path):
            self.source.write_bytes(b"changed")
            return record(100)
        result = self.sample(mutate)
        self.assertEqual(result["status"], "failed")
        self.assertIn("changed", result["failure"]["message"])

    def test_same_executable_rejected(self):
        result = formal.sample_pair(self.source, self.source,
                                    lambda p: self.fail("must not execute"), self.calibration)
        self.assertEqual(result["status"], "failed")
        self.assertIn("distinct", result["failure"]["message"])

    def test_missing_calibration_control_rejected(self):
        self.calibration["trials"].pop()
        result = self.sample(lambda p: self.fail("must not execute"))
        self.assertEqual(result["status"], "failed")

    def test_short_or_changed_calibration_rejected(self):
        original = copy.deepcopy(self.calibration)
        self.calibration["trials"][0]["runs"] = [record(1)] * 3
        self.assertEqual(self.sample(lambda p: self.fail("must not execute"))["status"], "failed")
        self.calibration = original
        self.cpp.write_bytes(b"other C++ executable")
        self.assertEqual(self.sample(lambda p: self.fail("must not execute"))["status"], "failed")

    def test_short_measured_reference_not_passed(self):
        result = self.sample(lambda p: record(100))
        self.assertEqual(result["status"], "failed")
        self.assertIsNone(result["ratio"])

    def test_all_cells_required_and_bad_cell_not_averaged(self):
        rows = [{"algorithm": a, "n": n,
                 "result": self.sample(lambda p: record(500_000_000))}
                for a, n in sorted(formal.GRID)]
        self.assertTrue(formal.summarize_grid(rows)["threshold_passed"])
        self.assertEqual(formal.summarize_grid(rows[:-1])["status"], "incomplete")
        self.assertEqual(formal.summarize_grid(rows + [rows[0]])["status"], "incomplete")
        # A stale/forged passed boolean cannot replace the actual sample values.
        for sample in rows[-1]["result"]["runs"]:
            if sample["implementation"] == "source":
                sample["elapsed_ns"] = 1_005_000_000
        self.assertTrue(rows[-1]["result"]["threshold_passed"])
        summary = formal.summarize_grid(rows)
        self.assertEqual(summary["status"], "failed")
        self.assertFalse(summary["cases"][-1]["result"]["threshold_passed"])
        self.assertEqual(summary["cases"][-1]["result"]["ratio"], 2.01)
        rows[-1]["result"]["runs"].pop()
        self.assertEqual(formal.summarize_grid(rows)["status"], "incomplete")

    def test_grid_requires_calibration_and_frozen_executables(self):
        rows = [{"algorithm": a, "n": n,
                 "result": self.sample(lambda p: record(500_000_000))}
                for a, n in sorted(formal.GRID)]
        del rows[0]["result"]["calibration"]
        self.assertEqual(formal.summarize_grid(rows)["status"], "incomplete")
        rows[0]["result"]["calibration"] = self.calibration
        self.source.write_bytes(b"changed after sampling")
        self.assertEqual(formal.summarize_grid(rows)["status"], "incomplete")


if __name__ == "__main__":
    unittest.main()
