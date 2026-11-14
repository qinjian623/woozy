#!/usr/bin/env python3
"""Check runner rejection paths without compiling or executing Woozy."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


HERE = Path(__file__).resolve().parent
ORCHESTRATOR = load(HERE / "run.py", "nativeobs_orchestrator_controls")
MATRIX = load(HERE.parents[2] / "tests/native/run.py", "native_matrix_controls")
CLEAN = dict(exit=0, timed_out=False, cleanup_complete=True,
             abandoned_pipes=False, stdout=b"", stderr=b"")


class Controls(unittest.TestCase):
    def test_process_fields(self):
        clean = dict(CLEAN, status="exited")
        self.assertTrue(ORCHESTRATOR.process_ok(clean))
        for key, values in {
                "exit": [False, True, 0.0, None, 1],
                "timed_out": [0, None, True],
                "cleanup_complete": [1, None, False],
                "abandoned_pipes": [0, None, True]}.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    self.assertFalse(ORCHESTRATOR.process_ok(dict(clean, **{key: value})))
            self.assertFalse(ORCHESTRATOR.process_ok({k: v for k, v in clean.items() if k != key}))

    def test_orchestrator_invalid_timeouts_write_nothing(self):
        for option in ("--build-timeout", "--smoke-timeout"):
            for value in ("nan", "inf", "-inf", "0", "-1"):
                with self.subTest(option=option, value=value), tempfile.TemporaryDirectory() as temp:
                    work = Path(temp) / "out"
                    with patch.object(ORCHESTRATOR, "load_corpus_support") as loader:
                        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                            ORCHESTRATOR.main(["--seed", temp, "--work-dir", str(work), option + "=" + value])
                        self.assertEqual(raised.exception.code, 2)
                        loader.assert_not_called()
                    self.assertFalse(work.exists())

    def test_matrix_invalid_timeouts_write_nothing(self):
        for option in ("--timeout", "--execute-timeout"):
            for value in ("nan", "inf", "-inf", "0", "-1"):
                with self.subTest(option=option, value=value), tempfile.TemporaryDirectory() as temp:
                    work = Path(temp) / "out"
                    argv = ["run.py", "--build", temp, "--toolchains", temp,
                            "--out", str(work), "--report", str(Path(temp) / "report.json"), option + "=" + value]
                    with patch.object(sys, "argv", argv), contextlib.redirect_stderr(io.StringIO()):
                        with self.assertRaises(SystemExit) as raised:
                            MATRIX.main()
                    self.assertEqual(raised.exception.code, 2)
                    self.assertFalse(work.exists())

    def test_smoke_and_full_check_process_flags(self):
        # The subprocess fixture returns records only; it never starts a child.
        for phase in ("smoke", "full"):
            for change in ({}, {"cleanup_complete": False}, {"abandoned_pipes": True},
                           {"timed_out": True}, {"exit": False}, {"exit": 0.0}):
                with self.subTest(phase=phase, change=change), tempfile.TemporaryDirectory() as temp:
                    work = Path(temp)
                    (work / "build").mkdir()
                    (work / "build/program").write_bytes(b"process fixture; not executable")
                    (work / "build-report.json").write_text('{"status":"passed"}')
                    records = ([dict(CLEAN, **change), dict(CLEAN)] if phase == "smoke"
                               else [dict(CLEAN), dict(CLEAN, **change)])
                    corpus = types.SimpleNamespace(_run_process_impl=unittest.mock.Mock(side_effect=records))
                    with patch.object(ORCHESTRATOR, "load_corpus_support", return_value=corpus), \
                            patch.object(ORCHESTRATOR, "validate_build", return_value={"status": "passed"}), \
                            contextlib.redirect_stdout(io.StringIO()):
                        result = ORCHESTRATOR.main(["--seed", temp, "--work-dir", temp, "--skip-build", "--full"])
                    report = json.loads((work / "orchestrator-report.json").read_text())
                    self.assertEqual(result, 1 if change else 0)
                    self.assertEqual(report["status"], "failed" if change else "passed")
                    if change and phase == "smoke":
                        self.assertEqual(corpus._run_process_impl.call_count, 1)


if __name__ == "__main__":
    unittest.main()
