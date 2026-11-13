"""Process-control checks for the staged test runners.

These use tiny Python fake tools and the real ``command`` functions extracted
from sema_a/run.py, sema_b/run.py and hirverify/run.py.  They test only process
waiting, retained output and finite-deadline validation.  They are not a
language, Checker, HIR or backend pass, and deliberately do not compile Woozy.
"""
import argparse
import ast
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from corpus import _run_process_impl  # noqa: E402

RUNNERS = {
    "sema_a": HERE.parent / "sema_a" / "run.py",
    "sema_b": HERE.parent / "sema_b" / "run.py",
    "hirverify": HERE / "run.py",
}
NAMESPACE = {
    "_run_process_impl": _run_process_impl,
    "Path": Path,
    "argparse": argparse,
    "hashlib": hashlib,
    "json": json,
    "math": math,
    "subprocess": subprocess,
    "time": time,
}


def load_runner(path: Path):
    tree = ast.parse(path.read_text(encoding="utf-8"), str(path))
    functions = {node.name: node for node in tree.body
                 if isinstance(node, ast.FunctionDef)}
    namespace = dict(NAMESPACE)
    for name in ("command", "finite_timeout"):
        if name not in functions:
            raise AssertionError(f"{path}: missing {name}")
        module = ast.Module(body=[functions[name]], type_ignores=[])
        exec(compile(module, str(path), "exec"), namespace)
    return namespace["command"], namespace["finite_timeout"]


class ProcessControlTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.work = Path(self.directory.name)

    def record(self, label: str) -> dict:
        return json.loads((self.work / f"{label}.json").read_text())

    def test_normal_outputs_are_retained(self):
        for name, path in RUNNERS.items():
            with self.subTest(runner=name):
                command, _ = load_runner(path)
                label = f"{name}-normal"
                result = command(
                    [sys.executable, "-c",
                     "import sys; print('out'); print('err', file=sys.stderr)"],
                    label, self.work, 5, dict(os.environ))
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "out\n")
                self.assertEqual(result.stderr, "err\n")
                self.assertEqual((self.work / f"{label}.stdout").read_text(),
                                 "out\n")
                self.assertEqual((self.work / f"{label}.stderr").read_text(),
                                 "err\n")
                record = self.record(label)
                self.assertEqual(record["exit"], 0)
                self.assertIs(record["timed_out"], False)
                self.assertIs(record["cleanup_complete"], True)
                self.assertIs(record["abandoned_pipes"], False)

    def test_timeout_retains_output_and_marks_the_record(self):
        for name, path in RUNNERS.items():
            with self.subTest(runner=name):
                command, _ = load_runner(path)
                label = f"{name}-timeout"
                with self.assertRaises(RuntimeError):
                    command([sys.executable, "-c",
                             "import time; print('started', flush=True); "
                             "time.sleep(10)"],
                            label, self.work, 0.5, dict(os.environ))
                self.assertEqual((self.work / f"{label}.stdout").read_text(),
                                 "started\n")
                record = self.record(label)
                self.assertIs(record["timed_out"], True)
                self.assertIs(record["cleanup_complete"], True)

    def test_nonzero_exit_is_retained_and_rejected(self):
        for name, path in RUNNERS.items():
            with self.subTest(runner=name):
                command, _ = load_runner(path)
                label = f"{name}-nonzero"
                with self.assertRaises(RuntimeError):
                    command([sys.executable, "-c",
                             "import sys; print('out'); "
                             "print('err', file=sys.stderr); sys.exit(7)"],
                            label, self.work, 5, dict(os.environ))
                self.assertEqual((self.work / f"{label}.stdout").read_text(),
                                 "out\n")
                self.assertEqual((self.work / f"{label}.stderr").read_text(),
                                 "err\n")
                record = self.record(label)
                self.assertEqual(record["exit"], 7)
                self.assertIs(record["timed_out"], False)
                self.assertIs(record["cleanup_complete"], True)

    def test_parent_exit_zero_with_child_holding_pipe_fails(self):
        for name, path in RUNNERS.items():
            with self.subTest(runner=name):
                command, _ = load_runner(path)
                label = f"{name}-child"
                pidfile = self.work / f"{label}.pid"
                script = (
                    "import pathlib,subprocess,sys; "
                    "p=subprocess.Popen([sys.executable,'-c',"
                    "'import time; time.sleep(10)']); "
                    "pathlib.Path(sys.argv[1]).write_text(str(p.pid))"
                )
                try:
                    with self.assertRaises(RuntimeError):
                        command([sys.executable, "-c", script, str(pidfile)],
                                label, self.work, 5, dict(os.environ))
                    record = self.record(label)
                    self.assertEqual(record["exit"], 0)
                    self.assertIs(record["cleanup_complete"], False)
                    self.assertIs(record["abandoned_pipes"], True)
                finally:
                    if pidfile.exists():
                        try:
                            os.kill(int(pidfile.read_text()), signal.SIGKILL)
                        except ProcessLookupError:
                            pass

    def test_invalid_deadlines_are_rejected_before_the_process(self):
        values = ("nan", "inf", "-inf", "0", "-1")
        for name, path in RUNNERS.items():
            command, finite = load_runner(path)
            for value in values:
                with self.subTest(runner=name, timeout=value):
                    marker = self.work / f"{name}-{value}.marker"
                    label = f"{name}-invalid-{value}"
                    with self.assertRaises(argparse.ArgumentTypeError):
                        finite(value)
                    with self.assertRaises(RuntimeError):
                        command([sys.executable, "-c",
                                 "import pathlib,sys; "
                                 "pathlib.Path(sys.argv[1]).write_text('ran')",
                                 str(marker)],
                                label, self.work, float(value), dict(os.environ))
                    self.assertFalse(marker.exists(), label)
                    self.assertFalse((self.work / f"{label}.json").exists(),
                                     label)

    def test_cli_invalid_deadlines_exit_two(self):
        extras = {"sema_a": [], "sema_b": ["--woozyc", "/nonexistent-woozyc",
                                           "--a-dir", str(HERE)],
                  "hirverify": []}
        for name, path in RUNNERS.items():
            for value in ("nan", "inf", "-inf", "0", "-1"):
                with self.subTest(runner=name, timeout=value):
                    result = _run_process_impl(
                        [sys.executable, str(path), f"--timeout={value}",
                         *extras[name]],
                        path.parent, dict(os.environ), 10)
                    self.assertEqual(result["exit"], 2, result)
                    self.assertIn(b"must be finite and positive",
                                  result["stderr"])
                    self.assertIs(result["timed_out"], False)
                    self.assertIs(result["cleanup_complete"], True)


def load_hirverify():
    spec = importlib.util.spec_from_file_location("hirverify_runner",
                                                  HERE / "run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ReportControlTest(unittest.TestCase):
    """Mutate reports so the actual HIR checks must reject each regression."""

    @classmethod
    def setUpClass(cls):
        cls.hv = load_hirverify()
        cls.fixture = (HERE / "fixture.wz").resolve()

    def unit_report(self):
        lines = []
        for key, value in self.hv.UNIT_EXPECTED.items():
            if key in self.hv.SNAPSHOT_KEYS:
                lines.append(f"PROBE {key}={value}")
            else:
                lines.append(f"PROBE {key} {value}")
        lines.append(f"SUMMARY probes=13 fixture={self.fixture}")
        return "\n".join(lines) + "\n"

    def five_report(self):
        lines = [f"CHECK {label} ok"
                 for label in self.hv.CHECK_LABELS["nonunit"]]
        lines.append(f"SUMMARY mode=nonunit fixture={self.fixture} failures=0")
        return "\n".join(lines) + "\n"

    def exit_report(self):
        lines = ["CPP public verify=1 diagnostics=0"]
        for first, later in self.hv.ALIASES:
            lines.append(f"SOURCE_ALIAS first={first} later={later} "
                         "source=0 kind=0")
        lines.extend([
            "TARGET_RETURN 16",
            "CPP bad-return verify=0 diagnostic=E_IR_INVALID:return target "
            "function mismatch",
            "CPP bad-exit verify=0 diagnostic=E_IR_INVALID:exit "
            "target/scopes/cleanup order/value preservation mismatch",
        ])
        return "\n".join(lines) + "\n"

    def test_valid_samples_pass(self):
        self.hv.check_unit(self.unit_report(), self.fixture, "sample")
        self.hv.check_five_mode(self.five_report(), "nonunit", self.fixture)
        self.hv.check_exit_oracle(self.exit_report(), self.fixture)

    def test_missing_or_duplicate_probe_is_rejected(self):
        report = self.unit_report()
        with self.assertRaises(RuntimeError):
            self.hv.check_unit(report.replace("PROBE valid", "PROBE extra"),
                               self.fixture, "missing-probe")
        with self.assertRaises(RuntimeError):
            self.hv.check_unit(report + "PROBE valid state=complete "
                               "diags=0 detail=none\n",
                               self.fixture, "duplicate-probe")

    def test_wrong_verified_or_snapshot_value_is_rejected(self):
        report = self.unit_report()
        with self.assertRaises(RuntimeError):
            self.hv.check_unit(report.replace(
                "PROBE verify state=complete diags=0 has=1",
                "PROBE verify state=complete diags=0 has=0"),
                self.fixture, "wrong-has")
        with self.assertRaises(RuntimeError):
            self.hv.check_unit(report.replace(
                "PROBE snapshot_after_builder=0:2",
                "PROBE snapshot_after_builder=0:3"),
                self.fixture, "wrong-snapshot")

    def test_missing_or_failed_check_is_rejected(self):
        report = self.five_report()
        with self.assertRaises(RuntimeError):
            self.hv.check_five_mode(report.replace(
                "CHECK verified-nonunit ok\n", ""), "nonunit", self.fixture)
        with self.assertRaises(RuntimeError):
            self.hv.check_five_mode(report.replace(
                "CHECK verified-nonunit ok", "CHECK verified-nonunit FAIL"),
                "nonunit", self.fixture)
        with self.assertRaises(RuntimeError):
            self.hv.check_five_mode(report.replace("failures=0",
                                                   "failures=1"),
                                    "nonunit", self.fixture)

    def test_missing_exit_negative_or_alias_is_rejected(self):
        report = self.exit_report()
        with self.assertRaises(RuntimeError):
            self.hv.check_exit_oracle(report.replace(
                "CPP public verify=1", "CPP public verify=0"), self.fixture)
        with self.assertRaises(RuntimeError):
            self.hv.check_exit_oracle(report.replace(
                "CPP bad-exit verify=0", "CPP bad-exit verify=1"),
                self.fixture)
        with self.assertRaises(RuntimeError):
            self.hv.check_exit_oracle(report.replace(
                "SOURCE_ALIAS first=2 later=12 source=0 kind=0\n", ""),
                self.fixture)

    def test_unexpected_stderr_is_rejected(self):
        result = subprocess.CompletedProcess([], 0, "ok\n", "warning\n")
        with self.assertRaises(RuntimeError):
            self.hv.require_empty_stderr(result, "stderr-control")
        self.hv.require_empty_stderr(
            subprocess.CompletedProcess([], 0, "ok\n", ""), "clean")


if __name__ == "__main__":
    unittest.main()
