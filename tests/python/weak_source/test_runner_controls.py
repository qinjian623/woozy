#!/usr/bin/env python3
"""Pure Python checks; no Woozy compiler or generated program is started."""
import argparse
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_weak_source as runner


def record(exit_code=1, **changes):
    return dict(dict(label="process fixture", exit=exit_code,
                     timed_out=False, cleanup_complete=True,
                     abandoned_pipes=False, stdout="", stderr=""), **changes)


class Controls(unittest.TestCase):
    def test_strict_process_fields(self):
        runner.require_ok(record(0), expected_exit=0)
        for key, values in {
                "exit": [False, True, 0.0, None, -9],
                "timed_out": [0, None, True],
                "cleanup_complete": [1, None, False],
                "abandoned_pipes": [0, None, True]}.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    with self.assertRaises(RuntimeError):
                        runner.require_ok(record(0, **{key: value}),
                                          expected_exit=0)

    def test_reject_checks_diagnostic_message_and_residual_program(self):
        good = record(1)
        document = {
            "status": "error",
            "diagnostics": [{"code": "E_TYPE_MISMATCH",
                             "message": "Weak target must be Vec"}],
        }
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            runner.check_reject(good, document, "E_TYPE_MISMATCH",
                                "Weak target must be", directory, "fixture")
            with self.assertRaises(RuntimeError):
                runner.check_reject(
                    good,
                    {"status": "error", "diagnostics": [
                        {"code": "E_OTHER", "message": "other"}]},
                    "E_TYPE_MISMATCH", "Weak target must be", directory,
                    "fixture")
            with self.assertRaises(RuntimeError):
                runner.check_reject(
                    good,
                    {"status": "ok", "diagnostics": []},
                    "E_TYPE_MISMATCH", "Weak target must be", directory,
                    "fixture")
            (directory / "program.py").write_text("residual\n")
            with self.assertRaises(RuntimeError):
                runner.check_reject(good, document, "E_TYPE_MISMATCH",
                                    "Weak target must be", directory,
                                    "fixture")

    def test_reject_requires_exit_one(self):
        document = {
            "status": "error",
            "diagnostics": [{"code": "E_TYPE_MISMATCH",
                             "message": "Weak target must be Vec"}],
        }
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            runner.check_reject(record(1), document, "E_TYPE_MISMATCH",
                                "Weak target must be", directory, "fixture")
            for value in (-9, -11, 2, 0, True, False, 1.0, None):
                with self.subTest(value=value):
                    with self.assertRaises(RuntimeError):
                        runner.check_reject(
                            record(value), document, "E_TYPE_MISMATCH",
                            "Weak target must be", directory, "fixture")

    def test_parse_json_rejects_nonfinite(self):
        valid = ('{"status":"error","diagnostics":[{"code":'
                 '"E_TYPE_MISMATCH","message":"Weak target must be Vec"}],'
                 '"ratio":1.25}')
        self.assertEqual(runner.parse_json(record(1, stdout=valid))["ratio"],
                         1.25)
        for token in ("NaN", "Infinity", "-Infinity"):
            text = ('{"status":"error","diagnostics":[{"code":'
                    '"E_TYPE_MISMATCH","message":"Weak target must be Vec"}],'
                    '"extra":' + token + '}')
            with self.subTest(token=token):
                with self.assertRaises(RuntimeError):
                    runner.parse_json(record(1, stdout=text))

    def test_default_stdlib_requires_regular_wz_files(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            root = base / "stdlib"
            root.mkdir()
            (root / "directory.wz").mkdir()
            binary = base / "reference-binary"
            binary.write_bytes(b"\x00" + str(root).encode("utf-8") + b"\x00")
            with self.assertRaisesRegex(RuntimeError, "has no Wz sources"):
                runner.default_stdlib_root(binary, "fixture")
            (root / "main.wz").write_text("package example;\n")
            self.assertEqual(runner.default_stdlib_root(binary, "fixture"),
                             root)
            args = argparse.Namespace(cli=binary, helper=None,
                                      root=base / "owner")
            roots = runner.reference_stdlib_roots(args)
            self.assertEqual(roots["reference_cli"]["wz_files"], 1)

    def test_invalid_timeout_rejected_before_output(self):
        for value in ("nan", "inf", "-inf", "0", "-1"):
            with self.subTest(value=value), \
                    patch.object(sys, "argv", [
                        "run_weak_source.py", "--root", ".",
                        "--out", "unused", "--python", "python3",
                        "--timeout=" + value]):
                with patch.object(runner, "prepare_output") as prepare:
                    with contextlib.redirect_stderr(io.StringIO()), \
                            self.assertRaises(SystemExit) as raised:
                        runner.main()
                    self.assertEqual(raised.exception.code, 2)
                    prepare.assert_not_called()


if __name__ == "__main__":
    unittest.main()
