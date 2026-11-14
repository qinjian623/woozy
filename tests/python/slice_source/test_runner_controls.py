#!/usr/bin/env python3
"""Pure Python checks; no Woozy compiler or generated program is started."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_slice_source as runner
import runner_controls as controls


def record(exit_code=0, **changes):
    return dict(dict(label="process fixture", exit=exit_code,
                     timed_out=False, cleanup_complete=True,
                     abandoned_pipes=False, stdout="", stderr=""), **changes)


class Controls(unittest.TestCase):
    def test_normal_and_expected_error_process_fields(self):
        for expected in (0, 2):
            runner.require_ok(record(expected), expected_exit=expected)
            for key, values in {
                    "exit": [False, True, 0.0, None, -9],
                    "timed_out": [0, None, True],
                    "cleanup_complete": [1, None, False],
                    "abandoned_pipes": [0, None, True]}.items():
                for value in values:
                    with self.subTest(expected=expected, key=key, value=value):
                        with self.assertRaises(RuntimeError):
                            runner.require_ok(record(expected, **{key: value}), expected_exit=expected)

    def test_io_controls_check_both_results(self):
        good = record(2, stdout=json.dumps({"status": "error", "diagnostics": [{"code": "E_INPUT_IO"}]}))
        bad_results = [record(2, cleanup_complete=False), record(2, timed_out=True),
                       record(2, abandoned_pipes=True), record(False),
                       record(2, stdout="not JSON"),
                       record(2, stdout='{"status":"error","diagnostics":[]}'),
                       record(2, stdout='{"status":"ok","diagnostics":[{"code":"E_INPUT_IO"}]}')]
        for phase in (0, 1):
            for bad in bad_results:
                with self.subTest(phase=phase, bad=bad), tempfile.TemporaryDirectory() as temp:
                    results = [bad, good] if phase == 0 else [good, bad]
                    with patch.object(runner, "run_command", side_effect=results):
                        with self.assertRaises(RuntimeError):
                            controls.control_io_atomic(Path(temp), Path(temp), Path("unused"), {}, [], 1)
        with tempfile.TemporaryDirectory() as temp:
            with patch.object(runner, "run_command", side_effect=[good, good]):
                controls.control_io_atomic(Path(temp), Path(temp), Path("unused"), {}, [], 1)

    def test_control_timeout_rejected_before_mkdtemp(self):
        for value in ("nan", "inf", "-inf", "0", "-1"):
            with self.subTest(value=value), patch.object(sys, "argv", ["runner_controls.py", "--timeout=" + value]):
                with patch.object(controls.tempfile, "mkdtemp") as mkdir:
                    with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                        controls.main()
                    self.assertEqual(raised.exception.code, 2)
                    mkdir.assert_not_called()


if __name__ == "__main__":
    unittest.main()
