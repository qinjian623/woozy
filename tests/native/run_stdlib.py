#!/usr/bin/env python3
"""Run source-level libraries, including short-write recovery and real sockets.

The five projects and eleven build/execute modes are the original set.  Each
subprocess now goes through ``selfhost/corpus.py::_run_process_impl`` so that
exit/timeout/cleanup fields are recorded with the raw command and byte
streams.  A failure records itself and continues with the remaining projects.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "selfhost"))
import corpus  # noqa: E402

COMMAND_TIMEOUT = 60
CASES = ('stdlib', 'stdlib-pure', 'stdlib-writer-error', 'stdlib-stdin',
         'json-thread')
STDIN = '中😀\r\n\nlast\0end'.encode()
EXPECTED = {
    'stdlib': b'stdlib: ok\n',
    'stdlib-pure': b'',
    'stdlib-writer-error': b'ABCDEFG',
    'json-thread': b'',
    'stdlib-stdin': (
        '中😀\n\nlast\0end\n-8\n-16\n-32\n-64\n-1\n'
        '8\n16\n32\n64\n1\n1.5\n-0\ntrue\n').encode(),
}
READELF_SHAPE = (
    'Elf file type is ',
    'Program Headers:',
    'Section to Segment mapping:',
)


class RunnerError(RuntimeError):
    """A runner invariant failed."""


def validate_timeout(name, value):
    """Reject nan/inf/zero/negative before any output or tool preflight."""
    if not math.isfinite(value) or value <= 0:
        raise RunnerError(
            f'{name} must be a finite positive number: {value!r}')


def _digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def _digest(path):
    return _digest_bytes(Path(path).read_bytes())


def _public(row):
    return {key: value for key, value in row.items()
            if not key.startswith('_')}


def _write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n',
                    encoding='utf-8', errors='backslashreplace')


def checked_process(process):
    """Require int exit (not bool) and real bools for the process flags."""
    if not isinstance(process, dict):
        raise RunnerError('process record is not an object')
    exit_code = process.get('exit')
    if type(exit_code) is not int:
        raise RunnerError(f'process exit is not an int: {exit_code!r}')
    for name in ('timed_out', 'cleanup_complete', 'abandoned_pipes'):
        if type(process.get(name)) is not bool:
            raise RunnerError(
                f'process {name} is not a bool: {process.get(name)!r}')
    for name in ('stdout', 'stderr'):
        if not isinstance(process.get(name), bytes):
            raise RunnerError(f'process {name} is not bytes')
    return process


def readelf_shape(text):
    """Minimal real GNU ``readelf -l -d`` shape, not llvm-readobj output."""
    missing = [marker for marker in READELF_SHAPE if marker not in text]
    if missing:
        raise RunnerError(
            'not GNU readelf -l -d output; missing ' + repr(missing))
    return text


def _snapshot(paths):
    """SHA-256 every watched file, recursing without following symlinks."""
    rows = {}
    for path in paths:
        path = Path(path)
        if path.is_dir():
            for base, directories, files in os.walk(path, followlinks=False):
                directories.sort()
                for name in sorted(files):
                    item = Path(base) / name
                    key = str(item)
                    if item.is_symlink():
                        rows[key] = {'link': str(item.readlink())}
                    elif item.is_file():
                        rows[key] = {'sha256': _digest(item),
                                     'size': item.stat().st_size}
        elif path.is_symlink():
            rows[str(path)] = {'link': str(path.readlink())}
        elif path.is_file():
            rows[str(path)] = {'sha256': _digest(path),
                               'size': path.stat().st_size}
        else:
            rows[str(path)] = {'missing': True}
    return rows


class Recorder:
    def __init__(self, role, root, out, root_repository, strace, readelf):
        self.role = role
        self.root = root
        self.out = out
        self.root_repository = root_repository
        self.strace = strace
        self.readelf = readelf
        self.index = 0
        self.records = out / 'commands'

    def _append(self, row):
        path = self.out / 'commands.jsonl'
        with path.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(row, ensure_ascii=False) + '\n')
            stream.flush()
            os.fsync(stream.fileno())

    def command(self, step, argv, *, timeout, stdin, trace_allowed=None):
        """Run one command, persist its raw result immediately, never raise."""
        self.index += 1
        directory = self.records / f'{self.index:03d}-{step}'
        directory.mkdir(parents=True, exist_ok=False)
        trace = trace_allowed is not None
        trace_path = directory / 'exec.log'
        shown = [str(part) for part in argv]
        invoked = shown
        if trace:
            invoked = [str(self.strace), '--seccomp-bpf', '-f', '-qq',
                       '-s', '16384', '-e', 'trace=execve,execveat',
                       '-o', str(trace_path), *shown]
        stdin = Path(stdin)
        row = {
            'index': self.index,
            'role': self.role,
            'step': step,
            'argv': shown,
            'invoked': invoked,
            'cwd': str(self.root_repository),
            'timeout': timeout,
            'stdin_path': str(stdin),
            'stdin_size': stdin.stat().st_size,
            'stdin_sha256': _digest(stdin),
            'trace_path': 'exec.log' if trace else None,
            'status': 'failed',
        }
        try:
            process = checked_process(corpus._run_process_impl(
                invoked, self.root_repository, None, timeout, stdin))
        except OSError as error:
            (directory / 'stdout.bin').write_bytes(b'')
            (directory / 'stderr.bin').write_bytes(b'')
            row.update({
                'exit': None, 'timed_out': None, 'cleanup_complete': None,
                'abandoned_pipes': None,
                'stdout_path': 'stdout.bin', 'stderr_path': 'stderr.bin',
                'stdout_sha256': _digest_bytes(b''),
                'stderr_sha256': _digest_bytes(b''),
                'stdout_observed': False,
                'spawn_error': {'type': type(error).__name__,
                                'message': str(error)},
                'trace': {'status': 'not_run'},
            })
            _write_json(directory / 'command.json', row)
            self._append(row)
            row['_stdout'] = b''
            row['_stderr'] = b''
            return row
        stdout, stderr = process['stdout'], process['stderr']
        (directory / 'stdout.bin').write_bytes(stdout)
        (directory / 'stderr.bin').write_bytes(stderr)
        row.update({
            'exit': process['exit'],
            'timed_out': process['timed_out'],
            'cleanup_complete': process['cleanup_complete'],
            'abandoned_pipes': process['abandoned_pipes'],
            'stdout_path': 'stdout.bin',
            'stderr_path': 'stderr.bin',
            'stdout_sha256': _digest_bytes(stdout),
            'stderr_sha256': _digest_bytes(stderr),
            'stdout_observed': True,
            'spawn_error': None,
        })
        if trace:
            allowed = {str(Path(item).resolve()) for item in trace_allowed}
            try:
                audit = corpus.audit_trace(trace_path, allowed)
                row['trace'] = {
                    'status': 'ok',
                    'observed': audit['observed'],
                    'allowed': audit['allowed'],
                    'scope': 'execve/execveat only; not a C++ helper build',
                }
            except corpus.TraceError as error:
                row['trace'] = {
                    'status': 'violation',
                    'error': str(error),
                    'allowed': sorted(allowed),
                    'scope': 'execve/execveat only; not a C++ helper build',
                }
        else:
            row['trace'] = {'status': 'not_run'}
        if row['trace']['status'] != 'violation':
            row['status'] = 'passed'
        _write_json(directory / 'command.json', row)
        self._append(row)
        row['_stdout'] = stdout
        row['_stderr'] = stderr
        return row


def check_command(row, label, failures, *, exit_code=0, stdout=None,
                  empty_stderr=False, require_trace=False):
    """Keep the original exit/stdout/stderr checks, bounded-record aware."""
    if row.get('spawn_error'):
        failures.append(f'{label}: cannot start command: '
                        f"{row['spawn_error']['type']}: "
                        f"{row['spawn_error']['message']}")
        return False
    ok = True
    if row['exit'] != exit_code:
        failures.append(f'{label}: exit {row["exit"]} != {exit_code}')
        ok = False
    if row['timed_out']:
        failures.append(f'{label}: timed out and was terminated')
        ok = False
    if row['cleanup_complete'] is not True:
        failures.append(f'{label}: process cleanup was incomplete')
        ok = False
    if row['abandoned_pipes']:
        failures.append(f'{label}: output pipes were abandoned')
        ok = False
    if require_trace and row['trace']['status'] != 'ok':
        failures.append(f'{label}: trace {row["trace"]["status"]}: '
                        f"{row['trace'].get('error', '')}")
        ok = False
    if stdout is not None and row['_stdout'] != stdout:
        failures.append(f'{label}: stdout {row["_stdout"]!r} != {stdout!r}')
        ok = False
    if empty_stderr and row['_stderr']:
        failures.append(f'{label}: stderr {row["_stderr"]!r} is not empty')
        ok = False
    return ok


def expected_modes():
    return [(case, backend, opt) for case in CASES
            for backend, opt in [('native', 0), ('native', 2)] + (
                [('python', None)] if case == 'stdlib-pure' else [])]


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'root', 'out', 'clang', 'lld', 'runtime', 'python'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--role', choices=('reference', 'candidate'),
                        required=True)
    parser.add_argument('--build-timeout', type=float, default=COMMAND_TIMEOUT)
    parser.add_argument('--timeout', type=float, default=COMMAND_TIMEOUT)
    parser.add_argument('--strace', type=Path)
    parser.add_argument('--readelf', type=Path)
    parser.add_argument('--helper', type=Path)
    parser.add_argument('--expect-cli-sha256')
    parser.add_argument(
        '--watch', type=Path, action='append', default=[],
        help='watch an actual CLI resource/test directory; the default '
             'args.root/stdlib is only the owner worktree copy, not every '
             'CLI resource root')
    args = parser.parse_args()
    try:
        validate_timeout('--timeout', args.timeout)
        validate_timeout('--build-timeout', args.build_timeout)
        if args.out.exists():
            if not args.out.is_dir():
                raise RunnerError(
                    f'output path is not a directory: {args.out}')
            if any(args.out.iterdir()):
                raise RunnerError(
                    f'refusing non-empty output directory: {args.out}')
    except RunnerError as error:
        print(f'run_stdlib.py: {error}', file=sys.stderr)
        return 2
    cli = args.cli.resolve(strict=True)
    if not os.access(cli, os.X_OK):
        raise RunnerError(f'compiler is not executable: {cli}')
    cli_sha = _digest(cli)
    if args.expect_cli_sha256 and cli_sha != args.expect_cli_sha256:
        raise RunnerError(
            f'compiler SHA-256 {cli_sha} != {args.expect_cli_sha256}')
    helper = args.helper.resolve(strict=True) if args.helper else None
    if helper is not None and args.role != 'reference':
        raise RunnerError('candidate role must not receive a stage0 helper')
    clang = args.clang.resolve(strict=True)
    lld = args.lld.resolve(strict=True)
    lld_argv0 = Path(os.path.abspath(args.lld))
    strace = (args.strace if args.strace is not None else
              clang.parents[2] / 'strace-7.2/usr/bin/strace')
    strace = strace.resolve(strict=True)
    tools = {name: (clang.parent / name).resolve(strict=True)
             for name in ('opt', 'llc', 'clang', 'clang++', 'ld.lld')}
    tools['ld.lld'] = lld
    readelf = (args.readelf or Path(shutil.which('readelf') or 'readelf'))
    readelf = readelf.resolve(strict=True)
    readelf_version = subprocess.run(
        [str(readelf), '--version'], capture_output=True, text=True,
        timeout=COMMAND_TIMEOUT)
    if (readelf_version.returncode != 0
            or 'GNU readelf' not in readelf_version.stdout
            or 'readobj' in readelf.name):
        raise RunnerError(
            f'readelf is not GNU readelf: {readelf} '
            f'{readelf_version.stdout!r}')
    args.out.mkdir(parents=True, exist_ok=True)

    empty_stdin = args.out / 'stdin-empty.bin'
    empty_stdin.write_bytes(b'')
    standard_stdin = args.out / 'stdin-stdlib-stdin.bin'
    standard_stdin.write_bytes(STDIN)
    if standard_stdin.read_bytes() != STDIN:
        raise RunnerError('stdin fixture bytes changed')

    allowed_build = {str(cli)} | {str(path) for path in tools.values()}
    if helper is not None:
        allowed_build.add(str(helper))
    recorder = Recorder(args.role, args.root.resolve(), args.out,
                        ROOT, strace, readelf)
    watched = [args.root / 'tests/native' / case / 'lang.toml'
               for case in CASES] + \
              [args.root / 'tests/native' / case / 'src/main.wz'
               for case in CASES] + \
              [args.root / 'tests/native/stdlib-writer-error/mock_write.cpp',
               args.root / 'stdlib', cli, helper, args.runtime, args.python,
               strace, readelf] + \
              list(tools.values()) + list(args.watch)
    watched = [path for path in watched if path is not None]
    before = _snapshot(watched)
    report = {
        'status': 'failed',
        'role': args.role,
        'cli': str(cli),
        'cli_sha256': cli_sha,
        'helper': str(helper) if helper else None,
        'helper_sha256': _digest(helper) if helper else None,
        'allowed_build': sorted(allowed_build),
        'tools': {name: str(path) for name, path in tools.items()},
        'lld_argv0': str(lld_argv0),
        'readelf': str(readelf),
        'readelf_version': readelf_version.stdout.strip(),
        'strace': str(recorder.strace),
        'build_timeout': args.build_timeout,
        'command_timeout': args.timeout,
        'stdin_fixture_path': str(standard_stdin),
        'stdin_fixture_hex': STDIN.hex(),
        'runs': [],
        'watch_paths': [str(path) for path in args.watch],
        'watch_note': (
            'default args.root/stdlib is the owner worktree copy; real CLI '
            'resources must be added with --watch, e.g. candidate '
            'resources/stdlib and the C++565 0a0ca4.../stdlib'),
        'failures': [],
        'input_changes': None,
        'hashes_before': str(args.out / 'hashes-before.json'),
    }
    _write_json(args.out / 'hashes-before.json', before)
    try:
        for case in CASES:
            for backend, opt in [('native', 0), ('native', 2)] + (
                    [('python', None)] if case == 'stdlib-pure' else []):
                directory = args.out / 'cases' / f'{case}-{backend}-{opt}'
                build = args.out / 'builds' / f'{case}-{backend}-{opt}'
                record = {
                    'case': case,
                    'backend': backend,
                    'opt': opt,
                    'expected_stdout_hex': EXPECTED[case].hex(),
                    'build_directory': str(build),
                    'status': 'failed',
                    'failures': [],
                    'steps': [],
                }
                report['runs'].append(record)
                try:
                    argv = [cli, 'build', '--project',
                            args.root / f'tests/native/{case}/lang.toml',
                            '--backend', backend, '--out-dir', build,
                            '--format', 'json']
                    if opt is not None:
                        argv += ['--opt', str(opt)]
                    step = recorder.command(
                        f'{case}-{backend}-{opt}-build', argv,
                        timeout=args.build_timeout, stdin=empty_stdin,
                        trace_allowed=allowed_build)
                    record['steps'].append(dict(_public(step), name='build'))
                    ok = check_command(
                        step, 'build', record['failures'],
                        require_trace=True)
                    if step['trace']['status'] == 'ok':
                        observed = set(step['trace']['observed'])
                        if str(cli) not in observed:
                            record['failures'].append(
                                'build: trace does not contain the role CLI')
                            ok = False
                        if helper is None and any(
                                'lang_native_helper' in item
                                for item in observed):
                            record['failures'].append(
                                'build: candidate trace executed a stage0 '
                                'helper')
                            ok = False
                    program = build / 'program'
                    if ok and case == 'stdlib-writer-error':
                        mock = build / 'mock.o'
                        compile_row = recorder.command(
                            f'{case}-{backend}-{opt}-mock-compile',
                            [clang, '-std=c++20', '-O2', '-ffreestanding',
                             '-fno-exceptions', '-fno-rtti',
                             '-fno-stack-protector', '-c',
                             args.root / 'tests/native/stdlib-writer-error/'
                                          'mock_write.cpp', '-o', mock],
                            timeout=args.timeout, stdin=empty_stdin,
                            trace_allowed={str(clang)})
                        record['steps'].append(
                            dict(_public(compile_row), name='mock_compile'))
                        ok = check_command(
                            compile_row, 'mock_compile', record['failures'],
                            require_trace=True) and ok
                        program = build / 'program-mock'
                        if ok:
                            link_row = recorder.command(
                                f'{case}-{backend}-{opt}-mock-link',
                                [clang, '-nostdlib', '-static',
                                 f'--ld-path={lld_argv0}',
                                 '-Wl,--wrap=lang_os_write_v1',
                                 build / 'program.o', mock, args.runtime,
                                 '-o', program],
                                timeout=args.timeout, stdin=empty_stdin,
                                trace_allowed={str(clang), str(lld)})
                            record['steps'].append(
                                dict(_public(link_row), name='mock_link'))
                            ok = check_command(
                                link_row, 'mock_link', record['failures'],
                                require_trace=True) and ok
                    if ok and backend == 'native':
                        elf = recorder.command(
                            f'{case}-{backend}-{opt}-readelf',
                            [readelf, '-l', '-d', program],
                            timeout=args.timeout, stdin=empty_stdin)
                        record['steps'].append(dict(_public(elf), name='elf'))
                        ok = check_command(
                            elf, 'readelf', record['failures']) and ok
                        text = elf['_stdout'].decode(
                            'utf-8', 'backslashreplace')
                        try:
                            readelf_shape(text)
                        except RunnerError as error:
                            record['failures'].append(
                                f'readelf: {error}')
                            ok = False
                        if 'INTERP' in text or '(NEEDED)' in text:
                            record['failures'].append(
                                'readelf: static ELF has INTERP or NEEDED')
                            ok = False
                        record['elf_gnu_shape'] = 'ok' if ok else 'failed'
                    if ok:
                        stdin_path = (standard_stdin if case == 'stdlib-stdin'
                                      else empty_stdin)
                        if case == 'stdlib-stdin' and \
                                stdin_path.read_bytes() != STDIN:
                            record['failures'].append(
                                'stdin: fixture bytes changed')
                            ok = False
                        executed = recorder.command(
                            f'{case}-{backend}-{opt}-run',
                            [program] if backend == 'native' else
                            [args.python, '-B', '-s', build / 'program.py'],
                            timeout=args.timeout, stdin=stdin_path)
                        record['steps'].append(dict(_public(executed), name='run'))
                        ok = check_command(
                            executed, 'run', record['failures'],
                            stdout=EXPECTED[case],
                            empty_stderr=True) and ok
                        record['stdin_sha256'] = _digest(stdin_path)
                        record['stdin_size'] = stdin_path.stat().st_size
                    if ok:
                        record['status'] = 'passed'
                except Exception as error:  # keep the remaining projects
                    record['failures'].append(
                        f'{type(error).__name__}: {error}')
                    record['status'] = 'failed'
                _write_json(directory / 'case.json', record)
        try:
            after = _snapshot(watched)
            _write_json(args.out / 'hashes-after.json', after)
            report['hashes_after'] = str(args.out / 'hashes-after.json')
            added = sorted(set(after) - set(before))
            removed = sorted(set(before) - set(after))
            changed = sorted(key for key in set(before) & set(after)
                             if before[key] != after[key])
            report['input_changes'] = {
                'added': added, 'removed': removed, 'changed': changed}
            for kind, keys in (('added', added), ('removed', removed),
                               ('changed', changed)):
                if keys:
                    report['failures'].append(
                        f'watched inputs {kind} during the run: '
                        f'{len(keys)} path(s): ' + ', '.join(keys))
        except Exception as error:  # keep the partial summary writable
            report['input_changes'] = {
                'error': f'{type(error).__name__}: {error}'}
            report['failures'].append(
                f'input snapshot failed: {type(error).__name__}: {error}')

        case_failures = []
        for run in report['runs']:
            if run['status'] == 'passed' and not run['failures']:
                continue
            detail = '; '.join(run['failures']) or 'case did not pass'
            case_failures.append(
                f"{run['case']}/{run['backend']}/{run['opt']}: {detail}")
        report['case_failures'] = case_failures
        report['failures'] = case_failures + report['failures']
        expected = expected_modes()
        present = [(run['case'], run['backend'], run['opt'])
                   for run in report['runs']]
        missing = sorted(set(expected) - set(present))
        duplicates = sorted({mode for mode in present
                             if present.count(mode) > 1})
        if missing:
            report['failures'].append(
                'missing required case records: ' + repr(missing))
        if duplicates:
            report['failures'].append(
                'duplicate case records: ' + repr(duplicates))
        if len(report['runs']) != len(expected):
            report['failures'].append(
                f'expected {len(expected)} case records, '
                f'found {len(report["runs"])}')
        report['status'] = (
            'passed' if not report['failures']
            and len(report['runs']) == len(expected)
            and all(run['status'] == 'passed' for run in report['runs'])
            else 'failed')
    finally:
        _write_json(args.out / 'stdlib-results.json', report)
    passed = sum(row['status'] == 'passed' for row in report['runs'])
    print(json.dumps({
        'status': report['status'],
        'role': args.role,
        'runs': len(report['runs']),
        'passed': passed,
        'report': str(args.out / 'stdlib-results.json'),
    }, ensure_ascii=False))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
