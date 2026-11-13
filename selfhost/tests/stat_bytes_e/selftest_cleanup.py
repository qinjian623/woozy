#!/usr/bin/env python3
"""Bounded process-cleanup self-test for stat_bytes_e/run.py.

The actual ``run`` function is extracted from run.py with ``ast`` and executed
with the same ``selfhost/corpus.py::_run_process_impl`` helper that run.py uses.
No copy or mock of the helper is used.

Cases:

* a normal command keeps stdout/stderr and reports complete cleanup;
* a short timeout with a parent and child in the same process group holding the
  pipes returns non-zero in bounded time, preserves the output, and leaves
  neither pid behind;
* a signal is a non-zero failure with output preserved;
* a same-group child that outlives its parent makes cleanup incomplete; run()
  must fail even though the direct child exited 0 (the self-test then kills
  that child itself);
* an end-to-end run.py failure path with a fake stage0 keeps status=failed,
  failed_record, stdout/stderr files, and bounded cleanup.

Processes that call setsid()/escape the session are the original corpus
limitation and are deliberately not claimed here.
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import traceback
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
RUN_PY = HERE / 'run.py'
SELFHOST = HERE.parents[1]
ROOT = HERE.parents[2]
if str(SELFHOST) not in sys.path:
    sys.path.insert(0, str(SELFHOST))
from corpus import _run_process_impl  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--work-dir', type=Path)
parser.add_argument('--toolchain-root', type=Path)
parser.add_argument('--runtime-archive', type=Path)
args = parser.parse_args()
if args.work_dir:
    args.work_dir.expanduser().resolve().mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(
    prefix='woozy-stat-bytes-e-selftest-',
    dir=args.work_dir.expanduser().resolve() if args.work_dir else None)).resolve()


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_run():
    source = RUN_PY.read_text(encoding='utf-8')
    tree = ast.parse(source)
    functions = [node for node in tree.body
                 if isinstance(node, ast.FunctionDef) and node.name == 'run']
    if len(functions) != 1:
        raise AssertionError(f'expected exactly one run(), got {len(functions)}')
    function = functions[0]
    uses_helper = any(isinstance(node, ast.Name) and node.id == '_run_process_impl'
                      for node in ast.walk(function))
    if not uses_helper:
        raise AssertionError('run() does not call corpus._run_process_impl')
    module = ast.Module(body=[
        ast.ImportFrom(module='__future__',
                       names=[ast.alias(name='annotations')], level=0),
        function], type_ignores=[])
    ast.fix_missing_locations(module)
    namespace = {
        '_run_process_impl': _run_process_impl,
        'args': SimpleNamespace(timeout=10.0, program_timeout=30.0),
        'WORK': WORK,
        'ENV': dict(os.environ),
        'records': [],
        'hashlib': hashlib,
        'subprocess': subprocess,
        'Path': Path,
    }
    exec(compile(module, str(RUN_PY), 'exec'), namespace)
    return namespace


def expect_failure(namespace, label, argv, timeout):
    started = time.monotonic()
    try:
        namespace['run'](label, argv, timeout=timeout)
    except RuntimeError as error:
        return {'argv': [str(item) for item in argv],
                'timeout_seconds': timeout,
                'elapsed_seconds': time.monotonic() - started,
                'error': str(error),
                'record': namespace['records'][-1]}
    raise AssertionError(f'{label}: expected run() to fail')


def process_state(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().split()
    except (FileNotFoundError, ProcessLookupError):
        return None
    except OSError:
        return 'unknown'
    return fields[2] if len(fields) > 2 else 'unknown'


def wait_gone(pid, timeout=5.0):
    deadline = time.monotonic() + timeout
    observed = []
    while time.monotonic() < deadline:
        state = process_state(pid)
        if state is None:
            return True, observed
        observed.append(state)
        time.sleep(0.02)
    return False, observed


def kill_if_alive(pid):
    try:
        os.kill(pid, 9)
    except ProcessLookupError:
        pass
    return wait_gone(pid)


def read_pids(path):
    parent, child, parent_pgrp, child_pgrp = path.read_text().split()
    return [int(parent), int(child), int(parent_pgrp), int(child_pgrp)]


def main() -> int:
    cases = []
    namespace = extract_run()

    normal = namespace['run'](
        'selftest-normal',
        [sys.executable, '-c',
         'import sys\nprint("ok", flush=True)\n'
         'print("warn", file=sys.stderr, flush=True)\n'],
        timeout=5)
    normal_record = namespace['records'][-1]
    assert normal.returncode == 0, normal_record
    assert normal_record['timed_out'] is False, normal_record
    assert normal_record['cleanup_complete'] is True, normal_record
    assert normal_record['abandoned_pipes'] is False, normal_record
    assert (WORK / 'selftest-normal.stdout').read_bytes() == b'ok\n'
    assert (WORK / 'selftest-normal.stderr').read_bytes() == b'warn\n'
    cases.append({'case': 'normal', 'status': 'passed',
                  'record': normal_record})

    timeout_pidfile = WORK / 'timeout.pids'
    timeout_script = (
        'import os, subprocess, sys, time\n'
        'child = subprocess.Popen([sys.executable, "-c", '
        '"import time; time.sleep(10)"])\n'
        'with open(sys.argv[1], "w") as handle:\n'
        '    handle.write(f"{os.getpid()} {child.pid} {os.getpgrp()} '
        '{os.getpgid(child.pid)}\\n")\n'
        'print("ready", flush=True)\n'
        'time.sleep(10)\n')
    timeout = expect_failure(
        namespace, 'selftest-timeout',
        [sys.executable, '-c', timeout_script, str(timeout_pidfile)], 1.0)
    record = timeout['record']
    assert timeout['elapsed_seconds'] < 3.0, timeout
    assert record['timed_out'] is True, record
    assert record['exit'] != 0, record
    assert record['cleanup_complete'] is True, record
    assert record['abandoned_pipes'] is False, record
    assert (WORK / 'selftest-timeout.stdout').read_bytes() == b'ready\n'
    parent, child, parent_pgrp, child_pgrp = read_pids(timeout_pidfile)
    assert parent == parent_pgrp, (parent, parent_pgrp)
    assert child_pgrp == parent_pgrp, (child_pgrp, parent_pgrp)
    parent_gone, parent_states = wait_gone(parent)
    child_gone, child_states = wait_gone(child)
    assert parent_gone, (parent, parent_states)
    assert child_gone, (child, child_states)
    cases.append({'case': 'timeout-parent-child-same-group',
                  'status': 'passed', 'parent_gone': parent_gone,
                  'child_gone': child_gone, **timeout})

    signal = expect_failure(
        namespace, 'selftest-signal',
        [sys.executable, '-c',
         'import os, signal, sys\nprint("before-signal", flush=True)\n'
         'os.kill(os.getpid(), signal.SIGKILL)\n'], 5)
    record = signal['record']
    assert record['timed_out'] is False, record
    assert record['exit'] == -9, record
    assert record['cleanup_complete'] is True, record
    assert record['abandoned_pipes'] is False, record
    assert (WORK / 'selftest-signal.stdout').read_bytes() == b'before-signal\n'
    cases.append({'case': 'signal', 'status': 'passed', **signal})

    leak_pidfile = WORK / 'leak.pids'
    leak_script = (
        'import os, subprocess, sys\n'
        'child = subprocess.Popen([sys.executable, "-c", '
        '"import time; time.sleep(10)"])\n'
        'with open(sys.argv[1], "w") as handle:\n'
        '    handle.write(f"{os.getpid()} {child.pid} {os.getpgrp()} '
        '{os.getpgid(child.pid)}\\n")\n'
        'print("parent-exits", flush=True)\n')
    leak = expect_failure(
        namespace, 'selftest-incomplete-cleanup',
        [sys.executable, '-c', leak_script, str(leak_pidfile)], 5)
    record = leak['record']
    assert record['timed_out'] is False, record
    assert record['exit'] == 0, record
    assert record['cleanup_complete'] is False, record
    assert record['abandoned_pipes'] is True, record
    assert (WORK / 'selftest-incomplete-cleanup.stdout').read_bytes() == b'parent-exits\n'
    leak_parent, leak_child, leak_parent_pgrp, leak_child_pgrp = read_pids(leak_pidfile)
    assert leak_parent == leak_parent_pgrp, (leak_parent, leak_parent_pgrp)
    assert leak_child_pgrp == leak_parent_pgrp, (leak_child_pgrp, leak_parent_pgrp)
    leak_parent_gone, leak_parent_states = wait_gone(leak_parent)
    assert leak_parent_gone, (leak_parent, leak_parent_states)
    child_gone = kill_if_alive(leak_child)
    assert child_gone[0], (leak_child, child_gone[1])
    cases.append({'case': 'incomplete-cleanup-is-failure', 'status': 'passed',
                  'child_killed_by_selftest': True, **leak})

    toolchain = (args.toolchain_root.expanduser().resolve()
                 if args.toolchain_root else
                 Path(os.environ.get('WOOZY_TOOLCHAIN_ROOT',
                                     str(ROOT.parent / 'toolchains'))))
    runtime = (args.runtime_archive.expanduser().resolve()
               if args.runtime_archive else WORK / 'placeholder-runtime.a')
    if not runtime.is_file():
        runtime.write_bytes(b'')
    e2e_root = WORK / 'e2e'
    e2e_root.mkdir()
    fake_pidfile = e2e_root / 'fake.pids'
    fake_stage0 = e2e_root / 'fake-woozyc'
    fake_stage0.write_text(
        '#!/usr/bin/env python3\n'
        'import os, subprocess, sys, time\n'
        'child = subprocess.Popen([sys.executable, "-c", '
        '"import time; time.sleep(10)"])\n'
        f'with open({str(fake_pidfile)!r}, "w") as handle:\n'
        '    handle.write(f"{os.getpid()} {child.pid} {os.getpgrp()} '
        '{os.getpgid(child.pid)}\\n")\n'
        'print("fake stage0 build starting", flush=True)\n'
        'time.sleep(10)\n',
        encoding='utf-8')
    fake_stage0.chmod(0o755)
    command = [sys.executable, str(RUN_PY),
               '--woozyc', str(fake_stage0),
               '--root-joint', str(HERE.parents[2] / 'selfhost/src/wzcc'),
               '--e-source', str(HERE.parents[2] / 'selfhost/src/wzcc/nativeemit'),
               '--toolchain-root', str(toolchain),
               '--runtime-archive', str(runtime),
               '--work-dir', str(e2e_root),
               '--timeout', '1.5',
               '--program-timeout', '5']
    started = time.monotonic()
    completed = _run_process_impl([str(item) for item in command], str(WORK),
                                  dict(os.environ), 120)
    elapsed = time.monotonic() - started
    (WORK / 'e2e-runpy.stdout').write_bytes(completed['stdout'])
    (WORK / 'e2e-runpy.stderr').write_bytes(completed['stderr'])
    assert completed['timed_out'] is False, completed
    assert completed['cleanup_complete'] is True, completed
    assert completed['abandoned_pipes'] is False, completed
    assert completed['exit'] == 1, (completed['exit'],
                                    completed['stdout'][-1000:],
                                    completed['stderr'][-1000:])
    e2e_work = sorted(e2e_root.glob('woozy-stat-bytes-e-*'))[-1]
    results = json.loads((e2e_work / 'results.json').read_text())
    assert results['status'] == 'failed', results
    failed = results['failed_record']
    assert failed['label'] == 'build-o0', failed
    assert failed['timed_out'] is True, failed
    assert failed['cleanup_complete'] is True, failed
    assert failed['abandoned_pipes'] is False, failed
    assert 'timed out' in results['error'], results['error']
    assert (e2e_work / 'build-o0.stdout').read_bytes() == \
        b'fake stage0 build starting\n'
    assert (e2e_work / 'build-o0.stderr').is_file()
    fake_parent, fake_child, fake_parent_pgrp, fake_child_pgrp = read_pids(fake_pidfile)
    assert fake_parent == fake_parent_pgrp, (fake_parent, fake_parent_pgrp)
    assert fake_child_pgrp == fake_parent_pgrp, (fake_child_pgrp, fake_parent_pgrp)
    fake_parent_gone, fake_parent_states = wait_gone(fake_parent)
    fake_child_gone, fake_child_states = wait_gone(fake_child)
    assert fake_parent_gone, (fake_parent, fake_parent_states)
    assert fake_child_gone, (fake_child, fake_child_states)
    cases.append({'case': 'end-to-end-runpy-failure-results', 'status': 'passed',
                  'elapsed_seconds': elapsed,
                  'run_py_exit': completed['exit'],
                  'results_status': results['status'],
                  'failed_record': failed,
                  'parent_gone': fake_parent_gone,
                  'child_gone': fake_child_gone})

    summary = {
        'status': 'passed',
        'work_dir': str(WORK),
        'run_py': {'path': str(RUN_PY), 'sha256': sha(RUN_PY)},
        'corpus_py': {'path': str(SELFHOST / 'corpus.py'),
                      'sha256': sha(SELFHOST / 'corpus.py')},
        'helper': '_run_process_impl from selfhost/corpus.py',
        'escape_session_limitation': 'not claimed; corpus leaves escaped '
                                     'sessions as cleanup_complete=false',
        'cases': cases,
    }
    (WORK / 'results.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - keep exact failure evidence
        summary = {'status': 'failed', 'work_dir': str(WORK),
                   'error': repr(error), 'traceback': traceback.format_exc()}
        (WORK / 'results.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary, indent=2))
        raise SystemExit(1)
