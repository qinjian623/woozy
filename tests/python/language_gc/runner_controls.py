#!/usr/bin/env python3
"""Portable controls for tests/python/language_gc/run_language_gc.py.

These checks do not start a compiler, helper, API driver or observer.  They
exercise the runner's strict process/JSON/metadata/byte helpers so a broken
harness cannot turn a missing symbol or a fake observer document into a pass.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import math
import os
import socket
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_language_gc as runner  # noqa: E402

EXTERNAL_LAUNCH_ATTEMPTS: list[str] = []


def forbid_external_launch(*args, **kwargs):
    command = args[0] if args else kwargs.get('args', '?')
    EXTERNAL_LAUNCH_ATTEMPTS.append(str(command))
    raise AssertionError('external launch forbidden in pure controls')


def expect_raises(call, label: str) -> None:
    try:
        call()
    except Exception:
        return
    raise AssertionError(f'{label}: expected a rejection')


def base_record(**overrides) -> dict:
    record = {
        'label': 'control',
        'exit': 0,
        'timed_out': False,
        'cleanup_complete': True,
        'abandoned_pipes': False,
        'stdout': '',
        'stderr': '',
    }
    record.update(overrides)
    return record


def check_strict_flags() -> None:
    runner.require_ok(base_record())
    expect_raises(lambda: runner.require_ok(base_record(exit=True)),
                  'bool exit')
    expect_raises(lambda: runner.require_ok(base_record(exit=0.0)),
                  'float exit')
    expect_raises(lambda: runner.require_ok(base_record(timed_out=0)),
                  'non-bool timed_out')
    expect_raises(lambda: runner.require_ok(base_record(cleanup_complete=1)),
                  'non-bool cleanup_complete')
    expect_raises(lambda: runner.require_ok(base_record(abandoned_pipes='false')),
                  'string abandoned_pipes')
    expect_raises(lambda: runner.require_ok(base_record(exit=-9)),
                  'signal exit')
    print('strict exit/timed-out/cleanup/pipes types and signal exit')


def check_timeout() -> None:
    if runner.finite_timeout('60') != 60.0:
        raise AssertionError('finite timeout 60 changed')
    for text in ('nan', 'inf', '-inf', '0', '-1'):
        expect_raises(lambda text=text: runner.finite_timeout(text),
                      f'timeout {text}')
    print('strict finite positive --timeout')


def check_nonfinite_json() -> None:
    for text in ('{"x":NaN}', '{"x":Infinity}', '{"x":-Infinity}'):
        expect_raises(
            lambda text=text: runner.parse_json_record(
                base_record(stdout=text)),
            f'non-finite JSON {text}')
    document = runner.parse_json_record(
        base_record(stdout='{"status":"ok","x":1}'))
    if document.get('status') != 'ok':
        raise AssertionError('finite JSON did not parse')
    print('strict JSON rejects NaN/Infinity/-Infinity')


def required_rows():
    return [{'source_name': name, 'generated': f'generated_{index}'}
            for index, name in enumerate(runner.REQUIRED_FUNCTIONS)]


def check_metadata() -> None:
    rows = runner.select_required_symbols(required_rows(), 'control')
    if [row['source_name'] for row in rows] != list(
            runner.REQUIRED_FUNCTIONS):
        raise AssertionError('required symbol order changed')
    missing = required_rows()[:-1]
    expect_raises(lambda: runner.select_required_symbols(missing, 'control'),
                  'missing required symbol')
    duplicate = required_rows() + [required_rows()[0]]
    expect_raises(
        lambda: runner.select_required_symbols(duplicate, 'control'),
        'duplicate required symbol')
    # A generated _f-looking name without its real source_name must never be
    # guessed into the required table.
    no_source = [{'generated': f'_f{index}'} for index in range(8)]
    expect_raises(
        lambda: runner.select_required_symbols(no_source, 'control'),
        'generated names without source_name')
    print('published source_name/generated only; missing/duplicate rejected')


def check_bytes() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        left = root / 'left'
        right = root / 'right'
        left.mkdir()
        right.mkdir()
        for directory in (left, right):
            (directory / 'program.py').write_text('print(1)\n')
            (directory / 'langrt.py').write_text("RUNTIME_VERSION = 'v'\n")
        runner.compare_generated_files(left, right, 'control')
        (right / 'langrt.py').write_text("RUNTIME_VERSION = 'different'\n")
        expect_raises(
            lambda: runner.compare_generated_files(left, right, 'control'),
            'CLI/API byte mismatch')
    print('CLI/API program.py and langrt.py byte mismatch rejected')


def check_fresh_output() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary) / 'out'
        out.mkdir()
        (out / 'sentinel').write_text('old evidence\n')
        expect_raises(lambda: runner.prepare_output(out, strict=True),
                      'non-empty paired output')
        runner.prepare_output(out, strict=False)
        if not (out / 'sentinel').is_file():
            raise AssertionError('legacy output refusal removed old evidence')
    print('paired --out must be fresh; legacy --out keeps old evidence')


def valid_stats(last_collected: int = 1) -> dict:
    return {
        'runtime_version': runner.RUNTIME_VERSION,
        'active': False,
        'live': 0,
        'allocations': 1,
        'since_collection': 0,
        'collections': 1,
        'polls': 1,
        'last_collected': last_collected,
        'total_collected': last_collected,
        'busy': 0,
        'threshold': 10,
        'peak_live': 1,
        'white': 0,
        'weak_dead': 1,
    }


def check_observation() -> None:
    good = {'case': 'self-cycle', 'stats': valid_stats(1), 'live': 0}
    runner.check_observation('self-cycle', good)
    expect_raises(
        lambda: runner.check_observation('self-cycle', {'status': 'passed'}),
        'fake observer document')
    expect_raises(
        lambda: runner.check_observation(
            'self-cycle', {'case': 'two-cycles', 'stats': valid_stats(1),
                           'live': 0}),
        'wrong observer case')
    bad = {'case': 'two-cycles', 'stats': valid_stats(1), 'live': 0}
    expect_raises(
        lambda: runner.check_observation('two-cycles', bad),
        'weak last_collected threshold')
    churn = {'case': 'auto-churn', 'stats': valid_stats(1), 'live': 0}
    churn['stats']['total_collected'] = 10
    expect_raises(
        lambda: runner.check_observation('auto-churn', churn),
        'weak auto-churn total_collected')
    print('fake/weak observer documents rejected; case statistics checked')


def check_generated_file_count() -> None:
    if runner.expected_generated_files(len(runner.CASES)) != 64:
        raise AssertionError('paired generated file count is not 64')
    if runner.expected_generated_files(1) != 8:
        raise AssertionError('single-case generated file count is not 8')
    if runner.PAIRED_ROLE_COUNT != 2:
        raise AssertionError('paired role count changed')
    if runner.EMIT_DIRECTORY_COUNT_PER_ROLE != 2:
        raise AssertionError('emit directory count changed')
    if tuple(runner.GENERATED_FILE_NAMES) != ('program.py', 'langrt.py'):
        raise AssertionError('generated file names changed')
    print('paired generated files: 8 cases x 2 roles x 2 emits x 2 files = 64')


def check_source_proof() -> None:
    compared = {'program.py': 'program-sha', 'langrt.py': 'runtime-sha'}
    reference = runner.source_metadata('helper', compared)
    if reference['current_source_same_api_proof'] is not False:
        raise AssertionError('reference C++ helper claimed current Source API proof')
    if reference['current_source_same_api_proof_status'] != 'unknown':
        raise AssertionError('reference helper proof status is not unknown')
    if reference['compiled_source_history'] != 'unknown':
        raise AssertionError('reference helper history is not unknown')
    if reference['same_generated_bytes_sha256'] != compared:
        raise AssertionError('reference byte-equality evidence was lost')
    candidate = runner.source_metadata('api', compared)
    if candidate['current_source_same_api_proof'] is not False:
        raise AssertionError('historical pythonobs claimed current Source API proof')
    if candidate['current_source_same_api_proof_status'] != 'false':
        raise AssertionError('historical pythonobs proof status is not false')
    if '727b' not in candidate['compiled_source_history']:
        raise AssertionError('historical pythonobs old Source is not labelled')
    print('reference helper proof unknown/false; historical API proof false')


def check_trace_failure_record() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary)
        log: list[dict] = []
        original = runner._run_process_impl

        def process_fixture(argv, cwd, env, timeout):
            trace = Path(argv[argv.index('-o') + 1])
            trace.parent.mkdir(parents=True, exist_ok=True)
            trace.write_text(
                '1 execve("/unexpected", ["/unexpected"], 0x0) = 0\n',
                encoding='utf-8')
            return {
                'exit': -9,
                'timed_out': True,
                'cleanup_complete': False,
                'abandoned_pipes': True,
                'stdout': b'partial stdout\n',
                'stderr': b'partial stderr\n',
            }

        runner._run_process_impl = process_fixture
        try:
            try:
                runner.run_command(
                    ['/allowed'], out, 60, out, log, 'failed-trace', {},
                    Path('/not-executed-strace'),
                    audit_allowed={'/allowed'},
                    meta={'category': 'main', 'role': 'candidate',
                          'case': 'self-cycle'})
            except RuntimeError as error:
                failure = str(error)
            else:
                raise AssertionError('unexpected trace was accepted')
        finally:
            runner._run_process_impl = original

        if 'unexpected compiler subprocesses' not in failure:
            raise AssertionError(f'trace failure lost its cause: {failure}')
        if len(log) != 1:
            raise AssertionError(
                f'actual command record not retained: {len(log)}')
        record = log[0]
        commands = out / 'commands.jsonl'
        if not commands.is_file():
            raise AssertionError('commands.jsonl was not written before failure')
        lines = [line for line in commands.read_text(encoding='utf-8').splitlines()
                 if line.strip()]
        if len(lines) != 1 or json.loads(lines[0]) != record:
            raise AssertionError(
                'commands.jsonl lost the complete formal record')
        expected_stdout = b'partial stdout\n'
        expected_stderr = b'partial stderr\n'
        expected_trace = out / 'traces/000-failed-trace.trace'
        if record['argv'][0] != '/not-executed-strace':
            raise AssertionError('strace argv was not recorded')
        if record['tool_argv'] != ['/allowed']:
            raise AssertionError('tool argv was not recorded')
        if record['cwd'] != str(out):
            raise AssertionError('cwd was not recorded')
        if record['timeout_seconds'] != 60:
            raise AssertionError('timeout was not recorded')
        if type(record['exit']) is not int or record['exit'] != -9:
            raise AssertionError('native int exit was not recorded')
        for key, value in (('timed_out', True), ('cleanup_complete', False),
                           ('abandoned_pipes', True)):
            if record.get(key) is not value:
                raise AssertionError(
                    f'{key} was not recorded as a strict bool')
        if record['stdout'] != expected_stdout.decode():
            raise AssertionError('raw stdout text was lost')
        if record['stderr'] != expected_stderr.decode():
            raise AssertionError('raw stderr text was lost')
        if record['stdout_sha256'] != hashlib.sha256(
                expected_stdout).hexdigest():
            raise AssertionError('stdout hash was lost')
        if record['stderr_sha256'] != hashlib.sha256(
                expected_stderr).hexdigest():
            raise AssertionError('stderr hash was lost')
        if Path(record['stdout_path']).read_bytes() != expected_stdout:
            raise AssertionError('raw stdout bytes were lost')
        if Path(record['stderr_path']).read_bytes() != expected_stderr:
            raise AssertionError('raw stderr bytes were lost')
        if record['trace_path'] != str(expected_trace):
            raise AssertionError('trace path was not recorded')
        if record['trace_sha256'] != hashlib.sha256(
                expected_trace.read_bytes()).hexdigest():
            raise AssertionError('trace hash was lost')
        if 'unexpected compiler subprocesses' not in record.get(
                'trace_audit_error', ''):
            raise AssertionError('trace audit error was not recorded')
        if 'trace_audit' in record:
            raise AssertionError('rejected trace was marked audited')
    print('failed trace keeps complete formal command record and still fails')


def check_persist_write_failure() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        out = Path(temporary)
        previous = {'label': 'previous', 'exit': 0}
        log = [previous]
        commands = out / 'commands.jsonl'
        prefix = json.dumps(previous, sort_keys=True) + '\n'
        commands.write_text(prefix, encoding='utf-8')
        original_open = Path.open

        class FailingWriter:
            def __init__(self, stream):
                self.stream = stream
            def __enter__(self):
                return self
            def __exit__(self, *args):
                self.stream.close()
            def write(self, value):
                raise OSError('injected command record write failure')

        def failing_open(path, mode='r', *args, **kwargs):
            stream = original_open(path, mode, *args, **kwargs)
            if mode == 'w' and path.name in (
                    'commands.jsonl', 'commands.jsonl.pending'):
                return FailingWriter(stream)
            return stream

        def process_fixture(*args, **kwargs):
            return {'exit': 0, 'timed_out': False,
                    'cleanup_complete': True, 'abandoned_pipes': False,
                    'stdout': b'new output', 'stderr': b''}

        with patch.object(runner, '_run_process_impl', process_fixture), \
                patch.object(Path, 'open', failing_open):
            try:
                runner.run_command(['/not-executed'], out, 60, out,
                                   log, 'write-failure', {})
            except OSError as error:
                if 'injected' not in str(error):
                    raise
            else:
                raise AssertionError('record write failure was swallowed')
        if commands.read_text(encoding='utf-8') != prefix:
            raise AssertionError('record write failure damaged previous records')
        if (out / 'logs/001-write-failure.stdout').read_bytes() != b'new output':
            raise AssertionError('record write failure lost raw command output')
    print('record write failure preserves previous records and raw output')


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--json-out', type=Path, default=None)
    args = parser.parse_args()
    checks = {
        'strict_flags': check_strict_flags,
        'timeout': check_timeout,
        'nonfinite_json': check_nonfinite_json,
        'metadata': check_metadata,
        'bytes': check_bytes,
        'fresh_output': check_fresh_output,
        'observation': check_observation,
        'generated_file_count': check_generated_file_count,
        'source_proof': check_source_proof,
        'trace_failure_record': check_trace_failure_record,
        'persist_write_failure': check_persist_write_failure,
    }
    results = {}
    launches = EXTERNAL_LAUNCH_ATTEMPTS
    guarded = [
        (subprocess, 'Popen'), (subprocess, 'run'), (subprocess, 'call'),
        (subprocess, 'check_call'), (subprocess, 'check_output'),
        (os, 'system'), (os, 'posix_spawn'), (os, 'posix_spawnp'),
        (os, 'execv'), (os, 'execve'), (os, 'spawnv'), (os, 'spawnve'),
        (socket, 'socket'), (socket, 'create_connection'),
    ]
    with contextlib.ExitStack() as stack:
        for module, name in guarded:
            if hasattr(module, name):
                stack.enter_context(patch.object(
                    module, name, forbid_external_launch))
        try:
            for name, check in checks.items():
                check()
                results[name] = 'passed'
        except Exception as error:
            results['failed'] = f'{type(error).__name__}: {error}'
            results['external_launch_attempts'] = list(launches)
            if args.json_out:
                args.json_out.parent.mkdir(parents=True, exist_ok=True)
                args.json_out.write_text(
                    json.dumps({'status': 'failed', 'checks': results},
                               indent=2) + '\n', encoding='utf-8')
            print('CONTROLS failed')
            raise
        results['external_launch_attempts'] = list(launches)
        results['status'] = 'passed'
        if args.json_out:
            args.json_out.parent.mkdir(parents=True, exist_ok=True)
            args.json_out.write_text(
                json.dumps(results, indent=2, sort_keys=True) + '\n',
                encoding='utf-8')
        print(f'CONTROLS passed (external launches: {len(launches)})')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
