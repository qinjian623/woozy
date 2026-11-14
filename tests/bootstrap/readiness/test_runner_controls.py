#!/usr/bin/env python3
"""Small controls for the readiness runner's process boundary.

These use real subprocesses (and the existing shared helper) for timeout,
spawn failure and stdin.  Unsafe process values are injected only after a real
helper return so the runner's reject-and-persist order is exercised.
"""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
RUNNER_PATH = HERE / "run.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("readiness_runner_controls", RUNNER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {RUNNER_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


RUNNER = load_runner()
CORPUS = RUNNER.CORPUS


class RunnerControlTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="readiness-controls-")
        self.root = Path(self.temp.name)
        self.records = self.root / "records"
        self.records.mkdir()
        self.old_records = RUNNER._RECORDS_DIR
        self.old_seq = RUNNER._RECORDS_SEQ
        RUNNER._RECORDS_DIR = self.records
        RUNNER._RECORDS_SEQ = 0

    def tearDown(self) -> None:
        RUNNER._RECORDS_DIR = self.old_records
        RUNNER._RECORDS_SEQ = self.old_seq
        self.temp.cleanup()

    def run_real(self, command, *, timeout=5):
        return RUNNER.run(
            [str(part) for part in command],
            env=os.environ.copy(),
            timeout=timeout,
            cwd=self.root,
            label="control",
        )

    def records_now(self):
        return sorted(self.records.glob("*/process.json"))

    def one_record(self):
        paths = self.records_now()
        self.assertEqual(len(paths), 1, paths)
        return json.loads(paths[0].read_text(encoding="utf-8"))

    def with_process_override(self, callback):
        old = CORPUS._run_process_impl

        def wrapped(argv, cwd, env, timeout, stdin=None):
            result = old(argv, cwd, env, timeout, stdin=stdin)
            callback(result)
            return result

        CORPUS._run_process_impl = wrapped
        try:
            with self.assertRaises(RUNNER.CheckFailure):
                self.run_real(["/bin/true", "--version"])
        finally:
            CORPUS._run_process_impl = old

    def test_cleanup_false_is_rejected_after_record(self):
        self.with_process_override(lambda result: result.update(cleanup_complete=False))
        self.assertIs(self.one_record()["cleanup_complete"], False)
        self.assertIs(self.one_record()["timed_out"], False)
        self.assertIs(self.one_record()["abandoned_pipes"], False)

    def test_abandoned_pipes_true_is_rejected_after_record(self):
        self.with_process_override(lambda result: result.update(abandoned_pipes=True))
        self.assertIs(self.one_record()["abandoned_pipes"], True)
        self.assertIs(self.one_record()["cleanup_complete"], True)

    def test_bool_exit_is_rejected_after_record(self):
        self.with_process_override(lambda result: result.update(exit=True))
        record = self.one_record()
        self.assertIs(record["exit"], True)
        self.assertTrue(record["record_errors"])

    def test_missing_flag_is_rejected_after_record(self):
        self.with_process_override(lambda result: result.pop("timed_out", None))
        record = self.one_record()
        self.assertIsNone(record["timed_out"])
        self.assertTrue(record["record_errors"])

    def test_real_timeout_is_rejected_and_cleaned(self):
        pidfile = self.root / "child.pid"
        command = ["/bin/sh", "-c", f'echo $$ > "{pidfile}"; exec sleep 5']
        with self.assertRaises(RUNNER.CheckFailure):
            self.run_real(command, timeout=1)
        record = self.one_record()
        self.assertIs(record["timed_out"], True)
        self.assertIs(record["cleanup_complete"], True)
        self.assertIs(record["abandoned_pipes"], False)
        pid = int(pidfile.read_text(encoding="utf-8").strip())
        for _ in range(50):
            if not Path(f"/proc/{pid}").exists():
                break
            time.sleep(0.02)
        self.assertFalse(Path(f"/proc/{pid}").exists(), f"timed-out child {pid} still exists")

    def test_permission_error_then_continue(self):
        noexec = self.root / "noexec"
        noexec.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        noexec.chmod(0o644)
        outcomes = []
        for command in ([str(noexec)], ["/bin/true"]):
            try:
                outcomes.append(self.run_real(command))
            except PermissionError:
                outcomes.append("permission")
        self.assertEqual(outcomes[0], "permission")
        self.assertEqual(outcomes[1]["exit"], 0)
        self.assertIs(outcomes[1]["cleanup_complete"], True)
        self.assertEqual(len(self.records_now()), 1)

    def test_stdin_is_devnull_and_parent_fd0_is_isolated(self):
        before = os.fstat(0)
        saved = os.dup(0)
        read_end, write_end = os.pipe()
        try:
            os.write(write_end, b"marker")
            os.close(write_end)
            os.dup2(read_end, 0)
            os.close(read_end)
            result = self.run_real([
                sys.executable,
                "-c",
                "import sys; sys.stdout.buffer.write(sys.stdin.buffer.read())",
            ])
            self.assertEqual(result["stdout"], b"")
            self.assertEqual(result["stdin"], "os.devnull")
        finally:
            os.dup2(saved, 0)
            os.close(saved)
        after = os.fstat(0)
        self.assertEqual((before.st_dev, before.st_ino), (after.st_dev, after.st_ino))

    def test_nonempty_work_rejects_before_output_and_record_override(self):
        work = self.root / "work"
        work.mkdir()
        marker = work / "partial.marker"
        marker.write_text("keep\n", encoding="utf-8")
        record_dir = self.root / "fresh-records"
        args = [
            "--woozyc", "/bin/true",
            "--clang", "/bin/true",
            "--lld", "/bin/true",
            "--strace", "/bin/true",
            "--work-dir", str(work),
            "--record-dir", str(record_dir),
        ]
        with self.assertRaisesRegex(
                RUNNER.CheckFailure,
                "work directory is not empty; refusing to delete or overwrite partial results"):
            RUNNER.main(args)
        self.assertEqual(marker.read_text(encoding="utf-8"), "keep\n")
        self.assertFalse((work / "probe-o0").exists())
        self.assertFalse(record_dir.exists())

    def test_invalid_timeout_rejects_before_output(self):
        work = self.root / "timeout-work"
        args = [
            "--woozyc", "/bin/true",
            "--clang", "/bin/true",
            "--lld", "/bin/true",
            "--strace", "/bin/true",
            "--work-dir", str(work),
            "--timeout", "0",
        ]
        with self.assertRaises(RUNNER.CheckFailure):
            RUNNER.main(args)
        self.assertFalse(work.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
