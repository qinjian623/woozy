#!/usr/bin/env python3
"""Reject contradictory reference exit/status pairs without a compiler build."""
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("sema_generic_arity_runner", HERE / "run.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)

DIAGNOSTIC = {
    "code": "E_TYPE_MISMATCH", "phase": "type", "severity": "error",
    "message": "wrong number of generic arguments",
    "primary": {"path": "/fixture/main.wz", "start_byte": 63, "end_byte": 77},
    "related": [], "fixits": [], "notes": [],
}


class ReferenceResultControls(unittest.TestCase):
    def check_document(self, document, exit_code):
        report = {"commands": []}

        def saved_process(*args, **kwargs):
            report["commands"].append({
                "exit": exit_code, "timed_out": False,
                "cleanup_complete": True, "abandoned_pipes": False,
            })
            return json.dumps(document).encode()

        with patch.object(runner, "run", saved_process):
            return runner.reference_result(
                Path("/reference"), Path("/fixture/lang.toml"), HERE, {},
                1, report, "reference-control")

    def test_valid_acceptance(self):
        self.assertIsNone(self.check_document({"status": "ok", "diagnostics": []}, 0))

    def test_valid_rejection(self):
        actual = self.check_document({"status": "error", "diagnostics": [DIAGNOSTIC]}, 1)
        self.assertEqual(actual["code"], "E_TYPE_MISMATCH")
        self.assertEqual((actual["begin"], actual["end"]), (63, 77))

    def test_contradictory_exit_or_status(self):
        for document, exit_code in [
            ({"status": "ok", "diagnostics": []}, 1),
            ({"status": "error", "diagnostics": [DIAGNOSTIC]}, 0),
            ({"status": "unexpected", "diagnostics": [DIAGNOSTIC]}, 1),
            ({"diagnostics": [DIAGNOSTIC]}, 1),
            ({"status": "ok", "diagnostics": []}, False),
        ]:
            with self.subTest(document=document, exit_code=exit_code):
                with self.assertRaises(RuntimeError):
                    self.check_document(document, exit_code)


if __name__ == "__main__":
    unittest.main()
