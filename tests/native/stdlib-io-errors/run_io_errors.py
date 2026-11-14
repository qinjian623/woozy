#!/usr/bin/env python3
"""Real nonblocking pipes and copy failures, Native O0/O2, 20s per process.

The original ``--cli --root --out`` invocation still builds the two O0/O2
programs and applies the original exit/stdout/stderr/destination assertions.
``--role`` plus ``--record`` add the strict process records: the shared
``selfhost/corpus.py::_run_process_impl`` finite process group, exact int/bool
fields, raw bytes and SHA-256, an exec-only strace allowlist, complete JSON
diagnostics, and retained role/opt fixture directories.

No compiler, Source, gold, or budget is changed here.  A failed command keeps
its partial records and is not retried.
"""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import sys
import tempfile
import time

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[3]
CORPUS_PATH = ROOT / 'selfhost/corpus.py'
MANIFEST = 'tests/native/stdlib-io-errors/lang.toml'
SOURCE = 'tests/native/stdlib-io-errors/src/main.wz'
RUNNER = 'tests/native/stdlib-io-errors/run_io_errors.py'
BUILD_TIMEOUT = 20.0
PROGRAM_TIMEOUT = 20.0
DESTINATION = b'copied\x00\xf0\x9f\x98\x80'
DESTINATION_HEX = '636f7069656400f09f9880'
ABSENT = 'absent-destination'
TOOL_NAMES = ('opt', 'llc', 'clang', 'clang++', 'ld.lld')
DEFAULT_TOOLCHAINS = Path(os.environ.get(
    'LANG_TOOLCHAINS', '/home/qin/Work/language-dev-20261103/toolchains'))


class RunnerError(RuntimeError):
    """A runner invariant failed."""


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def digest(path):
    return digest_bytes(Path(path).read_bytes())


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n',
                    encoding='utf-8', errors='backslashreplace')


def public(row):
    return {key: value for key, value in row.items()
            if not key.startswith('_')}


def load_corpus():
    spec = importlib.util.spec_from_file_location(
        'io_errors_corpus', CORPUS_PATH)
    if spec is None or spec.loader is None:
        raise RunnerError(f'cannot load shared process runner: {CORPUS_PATH}')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def strict_process(process):
    """Do not default a missing exit/flag to a successful false value."""
    if not isinstance(process, dict):
        raise RunnerError('process record is not an object')
    if type(process.get('exit')) is not int:
        raise RunnerError(f'process exit is not an int: {process.get("exit")!r}')
    for name in ('timed_out', 'cleanup_complete', 'abandoned_pipes'):
        if type(process.get(name)) is not bool:
            raise RunnerError(
                f'process {name} is not a bool: {process.get(name)!r}')
    for name in ('stdout', 'stderr'):
        if not isinstance(process.get(name), bytes):
            raise RunnerError(f'process {name} is not bytes')
    return process


def validate_command(row, label, *, exit_code=0, empty_stdout=False,
                     empty_stderr=False, require_trace=True):
    """Return failures; absent flags are failures, not implicit false."""
    failures = []
    if row.get('spawn_error'):
        error = row['spawn_error']
        failures.append(f'{label}: cannot start command: '
                        f"{error['type']}: {error['message']}")
        return failures
    if row.get('exit') != exit_code:
        failures.append(f'{label}: exit {row.get("exit")!r} != {exit_code}')
    if row.get('timed_out') is not False:
        failures.append(f'{label}: timed_out is {row.get("timed_out")!r}')
    if row.get('cleanup_complete') is not True:
        failures.append(
            f'{label}: cleanup_complete is {row.get("cleanup_complete")!r}')
    if row.get('abandoned_pipes') is not False:
        failures.append(
            f'{label}: abandoned_pipes is {row.get("abandoned_pipes")!r}')
    trace = row.get('trace') or {}
    if require_trace and trace.get('status') != 'ok':
        failures.append(f'{label}: trace {trace.get("status")!r}: '
                        f'{trace.get("error", "")}')
    if empty_stdout and row.get('_stdout') != b'':
        failures.append(f'{label}: stdout is not empty: {row.get("_stdout")!r}')
    if empty_stderr and row.get('_stderr') != b'':
        failures.append(f'{label}: stderr is not empty: {row.get("_stderr")!r}')
    return failures


def observe_program_files(cwd):
    """Record destination/absent-destination before any directory cleanup."""
    cwd = Path(cwd)
    destination = cwd / 'destination'
    absent = cwd / ABSENT
    exists = destination.is_file()
    data = destination.read_bytes() if exists else b''
    record = {
        'destination_path': str(destination),
        'destination_exists': destination.exists(),
        'destination_is_file': exists,
        'destination_size': len(data) if exists else None,
        'destination_hex': data.hex() if exists else None,
        'destination_sha256': digest_bytes(data) if exists else None,
        'expected_hex': DESTINATION_HEX,
        'expected_size': len(DESTINATION),
        'absent_path': str(absent),
        'absent_exists': absent.exists(),
        'absent_is_file': absent.is_file(),
        'absent_is_dir': absent.is_dir(),
    }
    failures = []
    if not exists:
        failures.append('destination: file is missing')
    elif data != DESTINATION:
        failures.append(
            'destination: bytes differ: '
            f'{data.hex()} != {DESTINATION_HEX}')
    if absent.exists():
        failures.append('absent-destination: path unexpectedly exists')
    return record, failures


def snapshot(paths):
    """Hash explicit watched paths; do not follow symlinks or scan elsewhere."""
    rows = {}
    for item in sorted({str(Path(path)) for path in paths}):
        path = Path(item)
        if path.is_symlink():
            rows[item] = {'link': str(path.readlink())}
        elif path.is_dir():
            for base, directories, files in os.walk(path, followlinks=False):
                directories.sort()
                for name in sorted(files):
                    child = Path(base) / name
                    key = str(child)
                    if child.is_symlink():
                        rows[key] = {'link': str(child.readlink())}
                    elif child.is_file():
                        rows[key] = {'sha256': digest(child),
                                     'size': child.stat().st_size}
            entries = sorted(child for child in Path(path).iterdir())
            rows[item] = {'directory': True,
                          'entries': [str(child) for child in entries]}
        elif path.is_file():
            rows[item] = {'sha256': digest(path),
                          'size': path.stat().st_size}
        else:
            rows[item] = {'missing': True}
    return rows


class Recorder:
    def __init__(self, corpus, role, out, root, strace, record):
        self.corpus = corpus
        self.role = role
        self.out = out
        self.root = root
        self.strace = strace
        self.record = record
        self.index = 0
        self.commands = out / 'commands'
        self.log = out / 'commands.jsonl'

    def command(self, step, argv, *, cwd, env, timeout, trace_allowed=None):
        self.index += 1
        directory = allocate(
            self.commands / f'{self.index:03d}-{step}', self.record)
        shown = [str(part) for part in argv]
        trace = trace_allowed is not None
        invoked = shown
        trace_path = directory / 'exec.log'
        if trace:
            invoked = [str(self.strace), '--seccomp-bpf', '-f', '-qq',
                       '-s', '16384', '-e', 'trace=execve,execveat',
                       '-o', str(trace_path), *shown]
        row = {
            'index': self.index,
            'role': self.role,
            'step': step,
            'argv': shown,
            'invoked': invoked,
            'cwd': str(cwd),
            'timeout': timeout,
            'timeout_seconds': timeout,
            'trace_path': 'exec.log' if trace else None,
            'status': 'recorded',
        }
        started = time.monotonic()
        try:
            process = strict_process(self.corpus._run_process_impl(
                invoked, cwd, env, timeout))
        except OSError as error:
            row.update({
                'elapsed_seconds': round(time.monotonic() - started, 6),
                'exit': None,
                'timed_out': None,
                'cleanup_complete': None,
                'abandoned_pipes': None,
                'stdout_path': 'stdout.bin',
                'stderr_path': 'stderr.bin',
                'stdout_sha256': digest_bytes(b''),
                'stderr_sha256': digest_bytes(b''),
                'stdout_size': 0,
                'stderr_size': 0,
                'spawn_error': {'type': type(error).__name__,
                                'message': str(error)},
                'trace': {'status': 'not_run'},
            })
            (directory / 'stdout.bin').write_bytes(b'')
            (directory / 'stderr.bin').write_bytes(b'')
            write_json(directory / 'command.json', row)
            self.append(row)
            row['_stdout'] = b''
            row['_stderr'] = b''
            return row
        stdout, stderr = process['stdout'], process['stderr']
        (directory / 'stdout.bin').write_bytes(stdout)
        (directory / 'stderr.bin').write_bytes(stderr)
        row.update({
            'elapsed_seconds': round(time.monotonic() - started, 6),
            'exit': process['exit'],
            'timed_out': process['timed_out'],
            'cleanup_complete': process['cleanup_complete'],
            'abandoned_pipes': process['abandoned_pipes'],
            'stdout_path': 'stdout.bin',
            'stderr_path': 'stderr.bin',
            'stdout_sha256': digest_bytes(stdout),
            'stderr_sha256': digest_bytes(stderr),
            'stdout_size': len(stdout),
            'stderr_size': len(stderr),
            'stdout_hex': stdout.hex(),
            'stderr_hex': stderr.hex(),
            'spawn_error': None,
        })
        if trace:
            allowed = {str(Path(item).resolve()) for item in trace_allowed}
            try:
                audit = self.corpus.audit_trace(trace_path, allowed)
                row['trace'] = {
                    'status': 'ok',
                    'observed': audit['observed'],
                    'allowed': audit['allowed'],
                    'scope': 'execve/execveat only',
                }
            except self.corpus.TraceError as error:
                row['trace'] = {
                    'status': 'violation',
                    'error': str(error),
                    'allowed': sorted(allowed),
                    'scope': 'execve/execveat only',
                }
        else:
            row['trace'] = {'status': 'not_run'}
        write_json(directory / 'command.json', row)
        self.append(row)
        row['_stdout'] = stdout
        row['_stderr'] = stderr
        return row

    def append(self, row):
        value = public(row)
        with self.log.open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(value, ensure_ascii=False) + '\n')
            stream.flush()
            os.fsync(stream.fileno())


def parse_build_json(corpus, data):
    """Use the existing strict JSON diagnostics parser (NaN/Inf rejected)."""
    document = corpus.parse_diagnostic_stdout(data)
    if document['status'] != 'ok':
        raise RunnerError(f'build JSON status is {document["status"]!r}')
    return document


def run_controls(corpus, out):
    """Pure controls: no compiler, Source, program, or network execution."""
    directory = allocate(out / 'controls', False)
    checks = []

    def check(name, function):
        try:
            function()
            checks.append({'name': name, 'status': 'passed'})
        except Exception as error:  # every control failure is a real failure
            checks.append({'name': name, 'status': 'failed',
                           'error': f'{type(error).__name__}: {error}'})

    def expect_error(function, label):
        try:
            function()
        except Exception:
            return
        raise RunnerError(f'{label} was accepted')

    valid = {
        'exit': 0, 'timed_out': False, 'cleanup_complete': True,
        'abandoned_pipes': False, 'stdout': b'', 'stderr': b'',
        '_stdout': b'', '_stderr': b'', 'trace': {'status': 'ok'},
    }

    def process_controls():
        strict_process(valid)
        for key, value in (('exit', True), ('exit', None),
                           ('timed_out', None), ('timed_out', 0),
                           ('cleanup_complete', None),
                           ('abandoned_pipes', 1),
                           ('stdout', ''), ('stderr', None)):
            broken = dict(valid)
            if value is None and key in ('exit',):
                broken.pop(key)
            else:
                broken[key] = value
            expect_error(lambda item=broken: strict_process(item),
                         f'process field {key}={value!r}')

    def command_controls():
        if validate_command(valid, 'control', require_trace=True):
            raise RunnerError('valid command record was rejected')
        variants = {
            'timeout': {'timed_out': True},
            'signal': {'exit': -9},
            'missing_exit': {'exit': None},
            'missing_timeout': {'timed_out': None},
            'cleanup': {'cleanup_complete': False},
            'abandoned': {'abandoned_pipes': True},
            'trace_missing': {'trace': {'status': 'not_run'}},
            'trace_violation': {'trace': {'status': 'violation'}},
            'stdout': {'_stdout': b'x'},
            'stderr': {'_stderr': b'x'},
        }
        for name, change in variants.items():
            row = dict(valid)
            row.update(change)
            if name == 'missing_exit':
                row.pop('exit', None)
            if not validate_command(
                    row, name, empty_stdout=True, empty_stderr=True,
                    require_trace=True):
                raise RunnerError(f'{name} control unexpectedly passed')

    def json_controls():
        base = ('{"schema_version":1,"compiler_version":"x",'
                '"source_revision":"x","status":"ok","diagnostics":[]}')
        if parse_build_json(corpus, base.encode())['status'] != 'ok':
            raise RunnerError('valid JSON control was rejected')
        for token in ('NaN', 'Infinity', '-Infinity'):
            broken = base.replace('"schema_version":1',
                                  f'"schema_version":{token}')
            expect_error(lambda data=broken.encode(): parse_build_json(
                corpus, data), f'JSON {token}')
        expect_error(lambda: parse_build_json(
            corpus, base.replace('"status":"ok"', '"status":"error"').encode()),
            'JSON status error')

    def fixture_controls():
        with tempfile.TemporaryDirectory(prefix='io-errors-controls-') as work:
            work = Path(work)
            (work / 'destination').write_bytes(DESTINATION)
            record, failures = observe_program_files(work)
            if failures or record['destination_hex'] != DESTINATION_HEX:
                raise RunnerError(f'valid fixture rejected: {failures}')
            (work / 'destination').write_bytes(b'wrong')
            if not observe_program_files(work)[1]:
                raise RunnerError('wrong destination bytes passed')
            (work / 'destination').unlink()
            if not observe_program_files(work)[1]:
                raise RunnerError('missing destination passed')
            (work / 'destination').write_bytes(DESTINATION + b'x')
            if not observe_program_files(work)[1]:
                raise RunnerError('extra destination byte passed')
            (work / 'destination').write_bytes(DESTINATION)
            (work / ABSENT).write_bytes(b'created')
            if not observe_program_files(work)[1]:
                raise RunnerError('unexpected absent-destination passed')
            (work / ABSENT).unlink()
            (work / ABSENT).mkdir()
            if not observe_program_files(work)[1]:
                raise RunnerError('unexpected absent-destination dir passed')

    def trace_controls():
        with tempfile.TemporaryDirectory(prefix='io-errors-trace-') as work:
            path = Path(work) / 'exec.log'
            path.write_text(
                '1 execve("/allowed", ["/allowed"], 0x0) = 0\n',
                encoding='utf-8')
            corpus.audit_trace(path, {'/allowed'})
            expect_error(lambda: corpus.audit_trace(path, {'/other'}),
                         'unexpected exec')

    check('strict_process_fields', process_controls)
    check('timeout_signal_flags_cannot_pass', command_controls)
    check('json_nan_infinity_cannot_pass', json_controls)
    check('destination_absent_fixture_controls', fixture_controls)
    check('exec_allowlist_controls', trace_controls)
    result = {
        'status': 'passed' if all(
            item['status'] == 'passed' for item in checks) else 'failed',
        'scope': 'pure controls; no compiler/program/network execution',
        'checks': checks,
    }
    write_json(directory / 'controls.json', result)
    return result, directory


def allocate(path, record):
    """Reuse an empty directory, but never overwrite non-empty evidence."""
    base = path
    attempt = 0
    while path.exists():
        if not path.is_dir():
            raise RunnerError(f'evidence path is not a directory: {path}')
        if not any(path.iterdir()):
            return path
        if record:
            raise RunnerError(f'refusing non-empty evidence directory: {path}')
        attempt += 1
        path = base.with_name(
            f'{base.name}-{time.time_ns()}-{attempt}')
    path.mkdir(parents=True, exist_ok=False)
    return path


def resolve_toolchain(args, record):
    if args.toolchain is not None:
        root = args.toolchain.resolve(strict=True)
    elif record:
        root = (DEFAULT_TOOLCHAINS / 'llvm-22.1.8').resolve(strict=True)
    else:
        return None
    tools = {}
    for name in TOOL_NAMES:
        tools[name] = (root / 'bin' / name).resolve(strict=True)
    return root, tools


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path)
    parser.add_argument('--root', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--role', choices=('reference', 'candidate'),
                        default='reference')
    parser.add_argument('--record', action='store_true',
                        help='strict records: trace, JSON, input hashes')
    parser.add_argument('--self-test', action='store_true',
                        help='run only the pure controls')
    parser.add_argument('--helper', type=Path,
                        help='reference-only matching C++ helper')
    parser.add_argument('--toolchain', type=Path,
                        help='fixed LLVM 22.1.8 root')
    parser.add_argument('--strace', type=Path)
    parser.add_argument('--libxml', type=Path)
    parser.add_argument('--watch', type=Path, action='append', default=[],
                        help='actual candidate Source/resources or reference '
                             'stdlib/runtime input tree')
    parser.add_argument('--expect-cli-sha256')
    parser.add_argument('--expect-helper-sha256')
    parser.add_argument('--operation-id',
                        default='SH15-io-errors-public-records-20261114')
    args = parser.parse_args()
    if args.out is None:
        parser.error('--out is required')
    try:
        for name, value in (('build timeout', BUILD_TIMEOUT),
                            ('program timeout', PROGRAM_TIMEOUT)):
            if not math.isfinite(value) or value <= 0:
                raise RunnerError(f'{name} is not finite and positive')
        if args.record and args.out.exists():
            if not args.out.is_dir():
                raise RunnerError(f'output is not a directory: {args.out}')
            if any(args.out.iterdir()):
                raise RunnerError(
                    f'refusing non-empty record directory: {args.out}')
        elif args.out.exists() and not args.out.is_dir():
            raise RunnerError(f'output is not a directory: {args.out}')
        out = args.out.resolve()
        out.mkdir(parents=True, exist_ok=True)
        corpus = load_corpus()
    except (RunnerError, OSError) as error:
        print(f'run_io_errors.py: {error}', file=sys.stderr)
        return 2
    if args.self_test:
        controls, directory = run_controls(corpus, out)
        print(json.dumps({'status': controls['status'],
                          'controls': str(directory / 'controls.json')}))
        return 0 if controls['status'] == 'passed' else 1
    if args.cli is None or args.root is None:
        parser.error('--cli and --root are required outside --self-test')
    try:
        cli = args.cli.resolve(strict=True)
        root = args.root.resolve(strict=True)
        if not os.access(cli, os.X_OK):
            raise RunnerError(f'compiler is not executable: {cli}')
        cli_sha = digest(cli)
        if args.expect_cli_sha256 and cli_sha != args.expect_cli_sha256:
            raise RunnerError(
                f'compiler SHA-256 {cli_sha} != {args.expect_cli_sha256}')
        if args.helper is not None and args.role != 'reference':
            raise RunnerError('candidate role must not receive a C++ helper')
        helper = args.helper.resolve(strict=True) if args.helper else None
        if helper is not None:
            helper_sha = digest(helper)
            if (args.expect_helper_sha256
                    and helper_sha != args.expect_helper_sha256):
                raise RunnerError(
                    f'helper SHA-256 {helper_sha} != '
                    f'{args.expect_helper_sha256}')
        else:
            helper_sha = None
        toolchain = resolve_toolchain(args, args.record)
        tools = toolchain[1] if toolchain is not None else None
        strace = None
        if args.record:
            strace = (args.strace or (DEFAULT_TOOLCHAINS /
                                     'strace-7.2/usr/bin/strace')).resolve(
                strict=True)
            libxml = (args.libxml or (DEFAULT_TOOLCHAINS /
                                     'libxml2-legacy-2.13.9/usr/lib')).resolve(
                strict=True)
        else:
            libxml = args.libxml.resolve(strict=True) if args.libxml else None
        environment = dict(os.environ)
        environment['PYTHONDONTWRITEBYTECODE'] = '1'
        environment['LC_ALL'] = 'C'
        if args.record:
            tmp = allocate(out / 'tmp', True)
            paths = [str((Path(toolchain[0]) / 'lib').resolve()), str(libxml)]
            existing = environment.get('LD_LIBRARY_PATH')
            if existing:
                paths.append(existing)
            environment['LD_LIBRARY_PATH'] = ':'.join(paths)
            environment['TMPDIR'] = str(tmp)
        recorder = Recorder(corpus, args.role, out, root, strace, args.record)
        controls, controls_dir = run_controls(corpus, out)
        report = {
            'schema': 'woozy-stdlib-io-errors-run/1',
            'operation_id': args.operation_id,
            'role': args.role,
            'record': bool(args.record),
            'cli': str(cli),
            'cli_sha256': cli_sha,
            'root': str(root),
            'manifest': str(root / MANIFEST),
            'source': str(root / SOURCE),
            'manifest_sha256': digest(root / MANIFEST),
            'source_sha256': digest(root / SOURCE),
            'runner': str(Path(__file__).resolve()),
            'runner_sha256': digest(Path(__file__).resolve()),
            'shared_corpus': str(CORPUS_PATH),
            'shared_corpus_sha256': digest(CORPUS_PATH),
            'helper': str(helper) if helper else None,
            'helper_sha256': helper_sha,
            'toolchain': str(toolchain[0]) if toolchain else None,
            'tools': {name: str(path) for name, path in tools.items()}
            if tools else None,
            'strace': str(strace) if strace else None,
            'libxml': str(libxml) if libxml else None,
            'environment': {
                'LD_LIBRARY_PATH': environment.get('LD_LIBRARY_PATH'),
                'TMPDIR': environment.get('TMPDIR'),
                'LC_ALL': environment.get('LC_ALL'),
                'PYTHONDONTWRITEBYTECODE':
                    environment.get('PYTHONDONTWRITEBYTECODE'),
            },
            'budgets': {'build_seconds': BUILD_TIMEOUT,
                        'program_seconds': PROGRAM_TIMEOUT},
            'controls': str(controls_dir / 'controls.json'),
            'runs': [], 'failures': [], 'input_changes': None,
            'status': 'failed',
        }
        allowed_build = None
        if args.record:
            allowed_build = {str(cli)} | {
                str(path) for path in tools.values()}
            if helper is not None:
                allowed_build.add(str(helper))
        report['allowed_build'] = (sorted(allowed_build)
                                   if allowed_build else None)
        watch = [root / MANIFEST, root / SOURCE, Path(__file__).resolve(),
                 CORPUS_PATH, cli, *args.watch]
        if helper is not None:
            watch.append(helper)
        if args.record:
            watch.extend(tools.values())
            watch.extend([strace, libxml])
        before = snapshot(watch) if args.record else None
        if before is not None:
            write_json(out / 'hashes-before.json', before)
            report['hashes_before'] = str(out / 'hashes-before.json')
        try:
            if controls['status'] != 'passed':
                report['failures'].append('pure controls failed')
            for opt in (0, 2):
                run = {'opt': opt, 'status': 'failed', 'failures': [],
                       'build': None, 'program': None, 'files': None}
                report['runs'].append(run)
                try:
                    build_dir = allocate(
                        out / 'builds' / f'{args.role}-O{opt}', args.record)
                    program_dir = allocate(
                        out / 'programs' / f'{args.role}-O{opt}',
                        args.record)
                    run['build_directory'] = str(build_dir)
                    run['program_directory'] = str(program_dir)
                    argv = [cli, 'build', '--project', root / MANIFEST,
                            '--backend', 'native', '--opt', str(opt),
                            '--out-dir', build_dir, '--format', 'json']
                    build = recorder.command(
                        f'{args.role}-O{opt}-build', argv, cwd=root,
                        env=environment, timeout=BUILD_TIMEOUT,
                        trace_allowed=allowed_build)
                    run['build'] = public(build)
                    run['failures'].extend(validate_command(
                        build, f'O{opt} build', empty_stderr=True,
                        require_trace=bool(args.record)))
                    document = None
                    try:
                        document = parse_build_json(corpus, build['_stdout'])
                    except Exception as error:
                        run['failures'].append(
                            f'O{opt} build JSON: {type(error).__name__}: '
                            f'{error}')
                    if document is not None:
                        run['build_json_status'] = document['status']
                    program = build_dir / 'program'
                    run['program_path'] = str(program)
                    if not program.is_file():
                        run['failures'].append(
                            f'O{opt} build: program is missing')
                    else:
                        run['program_sha256_after_build'] = digest(program)
                        if not os.access(program, os.X_OK):
                            run['failures'].append(
                                f'O{opt} build: program is not executable')
                    object_file = build_dir / 'program.o'
                    run['object_path'] = str(object_file)
                    if object_file.is_file():
                        run['object_sha256_after_build'] = digest(object_file)
                    run['status'] = (
                        'passed' if not run['failures'] else 'failed')
                    if run['failures']:
                        continue
                    cwd = program_dir / 'cwd'
                    cwd.mkdir()
                    run['program_cwd'] = str(cwd)
                    program_argv = [program]
                    execute = recorder.command(
                        f'{args.role}-O{opt}-program', program_argv, cwd=cwd,
                        env=environment, timeout=PROGRAM_TIMEOUT,
                        trace_allowed={str(program.resolve())}
                        if args.record else None)
                    run['program'] = public(execute)
                    run['failures'].extend(validate_command(
                        execute, f'O{opt} program', empty_stdout=True,
                        empty_stderr=True,
                        require_trace=bool(args.record)))
                    if args.record:
                        observed = set(execute['trace'].get(
                            'observed', []))
                        if str(program.resolve()) not in observed:
                            run['failures'].append(
                                f'O{opt} program trace does not contain the '
                                'published program')
                    files, file_failures = observe_program_files(cwd)
                    run['files'] = files
                    run['failures'].extend(
                        f'O{opt} {item}' for item in file_failures)
                    run['program_sha256_after_run'] = digest(program)
                    if (run.get('program_sha256_after_build')
                            != run['program_sha256_after_run']):
                        run['failures'].append(
                            f'O{opt} program changed while it ran')
                    if object_file.is_file():
                        run['object_sha256_after_run'] = digest(object_file)
                        if (run.get('object_sha256_after_build')
                                != run['object_sha256_after_run']):
                            run['failures'].append(
                                f'O{opt} program.o changed while it ran')
                    run['status'] = (
                        'passed' if not run['failures'] else 'failed')
                except Exception as error:
                    run['failures'].append(
                        f'{type(error).__name__}: {error}')
                    run['status'] = 'failed'
                write_json(out / 'runs' / f'{args.role}-O{opt}.json', run)
        finally:
            if args.record:
                try:
                    after = snapshot(watch)
                    write_json(out / 'hashes-after.json', after)
                    report['hashes_after'] = str(out / 'hashes-after.json')
                    added = sorted(set(after) - set(before))
                    removed = sorted(set(before) - set(after))
                    changed = sorted(key for key in set(before) & set(after)
                                     if before[key] != after[key])
                    report['input_changes'] = {
                        'added': added, 'removed': removed,
                        'changed': changed}
                    for kind, keys in (('added', added), ('removed', removed),
                                       ('changed', changed)):
                        if keys:
                            report['failures'].append(
                                f'watched inputs {kind} during the run: '
                                f'{len(keys)} path(s)')
                except Exception as error:
                    report['input_changes'] = {
                        'error': f'{type(error).__name__}: {error}'}
                    report['failures'].append(
                        f'input snapshot failed: {type(error).__name__}: '
                        f'{error}')
            for run in report['runs']:
                if run['status'] == 'passed' and not run['failures']:
                    continue
                report['failures'].append(
                    f'O{run["opt"]}: ' +
                    ('; '.join(run['failures']) or 'did not pass'))
            present = [run['opt'] for run in report['runs']]
            if sorted(present) != [0, 2]:
                report['failures'].append(
                    f'expected O0/O2 records, found {present!r}')
            report['status'] = (
                'passed' if not report['failures']
                and controls['status'] == 'passed'
                and len(report['runs']) == 2
                and all(run['status'] == 'passed'
                        for run in report['runs']) else 'failed')
            write_json(out / 'io-errors-results.json', report)
        passed = sum(run['status'] == 'passed' for run in report['runs'])
        print(json.dumps({
            'status': report['status'],
            'role': args.role,
            'runs': len(report['runs']),
            'passed': passed,
            'report': str(out / 'io-errors-results.json'),
        }, ensure_ascii=False))
        return 0 if report['status'] == 'passed' else 1
    except (RunnerError, OSError) as error:
        print(f'run_io_errors.py: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
