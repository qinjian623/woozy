"""Check the stat runner's real process handling without compiling a project."""
import ast
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from corpus import _run_process_impl  # noqa: E402


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.work = Path(self.directory.name)
        function, = [node for node in ast.parse((HERE / 'run.py').read_text()).body
                     if isinstance(node, ast.FunctionDef) and node.name == 'run']
        namespace = dict(_run_process_impl=_run_process_impl, os=os, Path=Path,
                         ENV=dict(os.environ), WORK=self.work, records=[],
                         args=SimpleNamespace(timeout=5), hashlib=hashlib,
                         subprocess=subprocess)
        exec(compile(ast.Module(body=[function], type_ignores=[]),
                     str(HERE / 'run.py'), 'exec'), namespace)
        self.run_command = namespace['run']
        self.records = namespace['records']

    def test_raw_filename_bytes_survive(self):
        result = self.run_command('bytes', [os.fsencode(sys.executable), b'-c',
            b'import os,sys; sys.stdout.buffer.write(os.fsencode(sys.argv[1]))',
            b'\xff'])
        self.assertEqual(result.stdout, b'\xff')
        self.assertEqual(self.records[-1]['argv'][-1], r'\xff')
        self.assertIs(self.records[-1]['cleanup_complete'], True)

    def test_timeout_preserves_output(self):
        with self.assertRaises(RuntimeError):
            self.run_command('timeout', [sys.executable, '-c',
                'import time; print("started", flush=True); time.sleep(10)'],
                timeout=0.5)
        self.assertIs(self.records[-1]['timed_out'], True)
        self.assertEqual((self.work / 'timeout.stdout').read_bytes(), b'started\n')
        self.assertIs(self.records[-1]['cleanup_complete'], True)

    def test_invalid_deadlines_are_rejected_before_running_commands(self):
        for value in ('nan', 'inf', '-inf', '0', '-1'):
            with self.subTest(timeout=value):
                result = _run_process_impl(
                    [sys.executable, str(HERE / 'run.py'), f'--timeout={value}'],
                    HERE, dict(os.environ), 2)
                self.assertEqual(result['exit'], 2, result)
                self.assertIn(b'must be finite and positive', result['stderr'])
                self.assertIs(result['timed_out'], False)

    def test_zero_exit_with_child_holding_pipe_is_failure(self):
        pidfile = self.work / 'child.pid'
        script = ('import pathlib,subprocess,sys; '
                  'p=subprocess.Popen([sys.executable,"-c",'
                  '"import time; time.sleep(10)"]); '
                  'pathlib.Path(sys.argv[1]).write_text(str(p.pid))')
        try:
            with self.assertRaises(RuntimeError):
                self.run_command('child', [sys.executable, '-c', script,
                                          str(pidfile)])
            self.assertEqual(self.records[-1]['exit'], 0)
            self.assertIs(self.records[-1]['cleanup_complete'], False)
            self.assertIs(self.records[-1]['abandoned_pipes'], True)
        finally:
            if pidfile.exists():
                try:
                    os.kill(int(pidfile.read_text()), signal.SIGKILL)
                except ProcessLookupError:
                    pass


if __name__ == '__main__':
    unittest.main()
