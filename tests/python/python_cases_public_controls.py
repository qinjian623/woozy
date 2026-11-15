#!/usr/bin/env python3
"""Pure controls for the optional public-CLI/API path in run_cases.py.

This file never starts a compiler, API driver, target Python, helper or Source
program.  It replaces only the lowest bounded transport with a scripted fake;
the real PairedRunner/PairedExecutor methods, raw-stream persistence, trace
audit, metadata validation and output-tree checks are exercised.  The original
15 published cases and their legacy helper mode are checked as data/logic only.

Every attempted external exec, spawn, subprocess, target Python invocation or
network socket is recorded and must remain absent.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import os
import shutil
import socket
import stat
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import run_cases  # noqa: E402

ATTEMPTS: list[str] = []
CALLS: list[dict] = []
RUNTIME = "RUNTIME_VERSION = 'compiler-dev/1-python-values/10'\n"
CLI_PROGRAM = "def _f0():\n    return 0\n"
SYMBOLS = [
    {'symbol': 0, 'source_name': 'main', 'generated': '_f0'},
    {'symbol': 1, 'source_name': 'mutate_then_panic', 'generated': '_f1'},
    {'symbol': 2, 'source_name': 'probe32', 'generated': '_f2'},
]
HIR = ' scalar-v1 module=m0\n'
PUBLISHED_IDS = [
    'i8-u8-wrapping', 'signed-division-remainder',
    'division-min-over-minus-one', 'bitwise-integers', 'shift-left-right',
    'dynamic-integer-conversions', 'short-circuit-panic-guard',
    'function-values', 'control-flow-break-continue', 'lexical-shadowing',
    'inout-argument-order', 'panic-division-by-zero', 'panic-invalid-shift',
    'panic-dynamic-conversion', 'panic-negative-shift',
]
EXTRA_IDS = [
    'unused-aggregate-declaration', 'unused-generic-declaration',
    'callee-once-before-arguments', 'lambda-recursion-discard',
    'inout-identity-and-copies', 'short-circuit-effects',
    'nested-loop-step-and-early-return', 'unit-main-and-variable-function',
    'panic-remainder-zero', 'panic-shift-u64-count',
    'panic-unsigned-conversion', 'ordinary-nonzero',
    'ordinary-other-nonzero', 'python-keywords', 'float-arithmetic',
    'float-comparison', 'vec', 'capturing-lambda',
]


def expect_raises(call, needle: str) -> BaseException:
    try:
        call()
    except BaseException as error:
        if needle not in str(error):
            raise RuntimeError(
                f'expected {needle!r} in error, got {error!r}') from error
        return error
    raise RuntimeError(f'expected an error containing {needle!r}')


def sha(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def forbid(name):
    def blocked(*args, **kwargs):
        ATTEMPTS.append(name)
        raise AssertionError(f'external call attempted: {name} {args!r}')
    return blocked


def install_guards() -> None:
    for name in ('system', 'popen', 'execv', 'execve', 'execvp', 'execvpe',
                 'execl', 'execle', 'execlp', 'execlpe', 'spawnv', 'spawnve',
                 'spawnl', 'spawnle', 'spawnvp', 'spawnvpe', 'fork',
                 'forkpty', 'posix_spawn', 'posix_spawnp'):
        if hasattr(os, name):
            setattr(os, name, forbid('os.' + name))
    for name in ('run', 'Popen', 'call', 'check_call', 'check_output'):
        if hasattr(run_cases.subprocess, name):
            setattr(run_cases.subprocess, name,
                    forbid('subprocess.' + name))

    class BlockedSocket:
        def __init__(self, *args, **kwargs):
            ATTEMPTS.append('socket')
            raise AssertionError('network socket attempted')

    socket.socket = BlockedSocket
    socket.create_connection = forbid('socket.create_connection')


def option(args: list[str], name: str) -> str | None:
    if name in args:
        index = args.index(name)
        if index + 1 < len(args):
            return args[index + 1]
    return None


def diagnostic(source: Path, code: str, phase: str = 'sema') -> dict:
    return {'status': 'error', 'diagnostics': [{
        'code': code, 'phase': phase, 'severity': 'error',
        'message': code,
        'primary': {'path': str(source), 'start_byte': 0, 'end_byte': 1},
    }]}


class FakeTransport:
    def __init__(self, scenario: str = 'normal'):
        self.scenario = scenario

    def write_pair(self, directory: Path, program: str = CLI_PROGRAM) -> None:
        directory.mkdir(parents=True, exist_ok=True)
        (directory / 'program.py').write_text(program, encoding='utf-8')
        (directory / 'langrt.py').write_text(RUNTIME, encoding='utf-8')

    def __call__(self, argv, cwd, env, timeout, stdin=None):
        argv = [str(part) for part in argv]
        trace_index = argv.index('-o')
        trace_path = Path(argv[trace_index + 1])
        tool_index = trace_index + 2
        tool = Path(argv[tool_index])
        args = argv[tool_index + 1:]
        trace_path.parent.mkdir(parents=True, exist_ok=True)
        lines = [f'execve({json.dumps(str(tool))}, ["{tool}"], 0x0) = 0\n']
        if self.scenario == 'extra-exec':
            lines.append('execve("/bin/sh", ["/bin/sh", "-c", "true"], 0x0) = 0\n')
        trace_path.write_text(''.join(lines), encoding='utf-8')
        CALLS.append({'tool': str(tool), 'name': tool.name, 'args': args,
                      'cwd': str(cwd), 'timeout': timeout, 'stdin': stdin})
        manifest = option(args, '--project')
        source = Path(manifest).parent / 'src/main.wz' if manifest else None
        directory = option(args, '--out-dir')
        stdout = ''
        stderr = ''
        exit_code = 0
        if tool.name == 'candidate-cli':
            command = args[0] if args else ''
            if self.scenario == 'cli-fail' and command == 'check':
                exit_code = 1
                stdout = json.dumps(diagnostic(source, 'E_TYPE_MISMATCH')) + '\n'
            elif self.scenario == 'reject':
                exit_code = 1
                stdout = json.dumps(
                    diagnostic(source, 'E_CAPABILITY_UNAVAILABLE',
                               'codegen' if 'profile32' in str(directory) else 'sema')) + '\n'
            elif self.scenario == 'empty-stdout' and command == 'build':
                stdout = ''
            elif self.scenario == 'no-artifacts' and command == 'build':
                stdout = json.dumps({'status': 'ok', 'diagnostics': []}) + '\n'
            elif self.scenario == 'reject-publish' and command in (
                    'check', 'build'):
                if command == 'build':
                    self.write_pair(Path(directory))
                exit_code = 1
                stdout = json.dumps(
                    diagnostic(source, 'E_CAPABILITY_UNAVAILABLE')) + '\n'
            else:
                stdout = json.dumps({'status': 'ok', 'diagnostics': []}) + '\n'
                if command == 'build':
                    self.write_pair(Path(directory))
                if command == 'check':
                    cli = Path(tool).parent
                    if self.scenario == 'mutate-cli-stdlib':
                        (cli / 'stdlib/actual.wz').write_text('changed\n')
                    elif self.scenario == 'mutate-cli-runtime':
                        (cli / 'runtime/python/langrt.py').write_text(
                            'changed-runtime\n')
                    elif self.scenario == 'mutate-cli-missing':
                        (cli / 'runtime/python/langrt.py').unlink()
        elif tool.name == 'pythonobs':
            program = CLI_PROGRAM
            if self.scenario == 'api-inconsistent':
                program = "def _f0():\n    return 1\n"
            self.write_pair(Path(directory), program)
            document = {
                'status': 'ok', 'entry': 0, 'symbols': SYMBOLS,
                'verified_hir': HIR,
            }
            if self.scenario == 'api-malformed':
                document['symbols'] = [{'symbol': 0, 'source_name': 'main'}]
            if self.scenario == 'api-no-hir':
                document.pop('verified_hir')
            stdout = json.dumps(document) + '\n'
        elif tool.name == 'helper':
            if self.scenario == 'reject':
                exit_code = 1
                stdout = json.dumps(
                    diagnostic(source, 'E_CAPABILITY_UNAVAILABLE',
                               'codegen' if 'profile32' in str(directory) else 'sema')) + '\n'
            else:
                self.write_pair(Path(directory))
                (Path(directory) / 'metadata.json').write_text(
                    json.dumps({'contract': 'compiler-dev/1', 'backend': 'python',
                                'word_bits': 64, 'entry_symbol': 0,
                                'symbols': SYMBOLS}) + '\n',
                    encoding='utf-8')
                (Path(directory) / 'verified.hir').write_text(
                    ' scalar-v1 module=reference\n', encoding='utf-8')
                stdout = json.dumps({'status': 'ok', 'diagnostics': []}) + '\n'
        elif tool.name.startswith('python'):
            exit_code = 0
            if '--version' in args:
                stdout = 'Python 3.12.15\n'
        else:
            raise AssertionError(f'unexpected fake tool: {tool}')
        return {
            'exit': exit_code,
            'stdout': stdout.encode('utf-8'),
            'stderr': stderr.encode('utf-8'),
            'timed_out': False,
            'cleanup_complete': True,
            'abandoned_pipes': False,
        }


def make_args(root: Path, work: Path) -> argparse.Namespace:
    tools = work / 'tools'
    tools.mkdir(parents=True)
    names = {
        'helper': 'helper',
        'candidate_cli': 'candidate-cli',
        'api_driver': 'pythonobs',
        'python': 'python3.12',
        'strace': 'strace',
    }
    for key, name in names.items():
        path = tools / name
        path.write_text(f'# fake {name}\n', encoding='utf-8')
        path.chmod(0o755)
    resources = work / 'resources'
    (resources / 'stdlib').mkdir(parents=True)
    (resources / 'stdlib/core.wz').write_text('package core;\n', encoding='utf-8')
    (resources / 'stdlib/actual.wz').write_text('actual\n', encoding='utf-8')
    (resources / 'runtime/python').mkdir(parents=True)
    (resources / 'runtime/python/unicode18.py').write_text('# unicode\n')
    (resources / 'runtime/python/langrt.py').write_text(RUNTIME)
    for name in ('stdlib', 'runtime'):
        (tools / name).symlink_to(resources / name, target_is_directory=True)
    source_root = work / 'source'
    (source_root / 'wzcc').mkdir(parents=True)
    (source_root / 'wzcc/main.wz').write_text('package wzcc;\n')
    out = work / 'out'
    return argparse.Namespace(
        helper=tools / names['helper'],
        root=root,
        out=out,
        python=tools / names['python'],
        candidate_cli=tools / names['candidate_cli'],
        api_driver=tools / names['api_driver'],
        source_root=source_root,
        candidate_resources=resources,
        strace=tools / names['strace'],
        toolchain_root=None,
    )


def make_manifest(work: Path) -> Path:
    project = work / 'project'
    (project / 'src').mkdir(parents=True)
    (project / 'src/main.wz').write_text(
        'package app;\nfunc main() i32 {return 0;}\n', encoding='utf-8')
    manifest = project / 'lang.toml'
    manifest.write_text(
        'schema_version = 1\n[package]\nname = "app"\nsource_root = "src"\n'
        'entry = "app"\n[build]\nbackend = "python"\nword_bits = 64\n',
        encoding='utf-8')
    return manifest


def blank_report() -> dict:
    return {'cases': [], 'extra': [], 'negative_controls': [],
            'unsupported': [], 'source_rejections': [], 'failures': []}


def prepare(work: Path, scenario: str) -> tuple:
    transport = FakeTransport(scenario)
    run_cases._run_process_impl = transport
    args = make_args(REPO, work)
    runner = run_cases.PairedRunner(args, args.candidate_resources)
    runner.prepare_driver()
    return args, runner, transport


def run_main(work: Path, scenario: str = 'normal', setup=None) -> tuple:
    args = make_args(REPO, work)
    transport = FakeTransport(scenario)
    run_cases._run_process_impl = transport
    if setup is not None:
        setup(args)
    start = len(CALLS)

    def one_case(executor, published):
        manifest = Path(executor.args.root) / published['cases'][0]['manifest']
        executor.compile_case('main-case', manifest, 'cases')
        executor.verify_trees()

    old_suite = run_cases.run_suite
    old_argv = sys.argv
    sys.argv = [
        'run_cases.py',
        '--helper', str(args.helper),
        '--root', str(args.root),
        '--out', str(args.out),
        '--python', str(args.python),
        '--candidate-cli', str(args.candidate_cli),
        '--api-driver', str(args.api_driver),
        '--source-root', str(args.source_root),
        '--candidate-resources', str(args.candidate_resources),
        '--strace', str(args.strace),
    ]
    stream = io.StringIO()
    try:
        run_cases.run_suite = one_case
        with contextlib.redirect_stdout(stream):
            code = run_cases.main()
    finally:
        run_cases.run_suite = old_suite
        sys.argv = old_argv
    return args, transport, code, CALLS[start:], stream.getvalue()


def test_main_header_and_healthy_resources(work: Path,
                                           controls: list[str]) -> None:
    args, transport, code, calls, _ = run_main(work / 'main-healthy')
    results = json.loads((args.out / 'results.json').read_text())
    if code != 0 or results['status'] != 'passed':
        raise RuntimeError(f'actual main failed: {code} {results}')
    if not str(results['platform']).startswith(sys.platform):
        raise RuntimeError(f'paired platform is not direct/sys: {results["platform"]}')
    if results['inputs_before'] != results['inputs_after']:
        raise RuntimeError('actual main inputs changed')
    if (results['paired']['resources_before']
            != results['paired']['resources_after']):
        raise RuntimeError('actual main resource snapshot changed')
    targets = results['paired']['resource_targets']
    for relative in ('stdlib', 'runtime', 'runtime/python',
                     'runtime/python/unicode18.py',
                     'runtime/python/langrt.py'):
        if relative not in targets:
            raise RuntimeError(f'missing resource target {relative}')
    if len(calls) < 6:
        raise RuntimeError(f'actual main ran too few mock commands: {len(calls)}')
    if ATTEMPTS:
        raise RuntimeError(f'actual main made external attempts: {ATTEMPTS}')
    controls.append('actual main healthy header/resources/symlink')


def test_main_failure_report(work: Path, controls: list[str]) -> None:
    args, transport, code, calls, _ = run_main(work / 'main-fail', 'cli-fail')
    results = json.loads((args.out / 'results.json').read_text())
    if code != 1 or results['status'] != 'failed':
        raise RuntimeError(f'actual main failure was not reported: {code} {results}')
    if 'inputs_after' not in results:
        raise RuntimeError('failure report has no inputs_after')
    if not str(results['platform']).startswith(sys.platform):
        raise RuntimeError(f'failure report platform is not direct/sys: {results["platform"]}')
    if not (args.out / 'commands.jsonl').is_file():
        raise RuntimeError('failure raw commands.jsonl was not retained')
    records = [json.loads(line) for line in
               (args.out / 'commands.jsonl').read_text().splitlines()]
    labels = [item['label'] for item in records]
    if 'main-case-check' not in labels:
        raise RuntimeError(f'failure raw check was not retained: {labels}')
    if ATTEMPTS:
        raise RuntimeError(f'failure report made external attempts: {ATTEMPTS}')
    controls.append('actual main failure report keeps raw/results')


def test_main_resource_preflight(work: Path, controls: list[str]) -> None:
    def mismatch(args):
        link = args.candidate_cli.parent / 'stdlib'
        link.unlink()
        link.mkdir()
        (link / 'different.wz').write_text('different\n')

    args, transport, code, calls, _ = run_main(
        work / 'main-resource-mismatch', setup=mismatch)
    results = json.loads((args.out / 'results.json').read_text())
    if code != 1 or results['status'] != 'failed':
        raise RuntimeError('resource mismatch was not a reported failure')
    if calls:
        raise RuntimeError(f'resource mismatch started commands: {calls}')
    if 'resources resolve differently' not in results['error']:
        raise RuntimeError(f'resource mismatch error is unclear: {results["error"]}')
    controls.append('actual main resource mismatch rejected before command')

    def missing(args):
        (args.candidate_cli.parent / 'runtime').unlink()

    args, transport, code, calls, _ = run_main(
        work / 'main-resource-missing', setup=missing)
    results = json.loads((args.out / 'results.json').read_text())
    if code != 1 or results['status'] != 'failed' or calls:
        raise RuntimeError(f'missing resource did not fail before commands: {results}')
    if 'resources missing' not in results['error']:
        raise RuntimeError(f'missing resource error is unclear: {results["error"]}')
    controls.append('actual main missing required resource rejected before command')


def test_main_resource_changes(work: Path, controls: list[str]) -> None:
    args, transport, code, calls, _ = run_main(
        work / 'main-mutate-stdlib', 'mutate-cli-stdlib')
    results = json.loads((args.out / 'results.json').read_text())
    if code != 1 or results['status'] != 'failed':
        raise RuntimeError('CLI stdlib change was not a reported failure')
    if results['paired']['resources_before'] == results['paired']['resources_after']:
        raise RuntimeError('CLI stdlib change was not detected in resources')
    records = [json.loads(line) for line in
               (args.out / 'commands.jsonl').read_text().splitlines()]
    raw = [item for item in records if item['label'].endswith('-check')]
    if not raw or not Path(raw[0]['stdout_path']).is_file():
        raise RuntimeError('CLI stdlib change raw was not retained')
    controls.append('actual main post-command CLI stdlib change fails')

    args, transport, code, calls, _ = run_main(
        work / 'main-mutate-runtime', 'mutate-cli-runtime')
    results = json.loads((args.out / 'results.json').read_text())
    if code != 1 or results['status'] != 'failed':
        raise RuntimeError('CLI Python runtime change was not a reported failure')
    if results['paired']['resources_before'] == results['paired']['resources_after']:
        raise RuntimeError('CLI Python runtime change was not detected')
    controls.append('actual main post-command CLI Python runtime change fails')

    args, transport, code, calls, _ = run_main(
        work / 'main-mutate-missing', 'mutate-cli-missing')
    results = json.loads((args.out / 'results.json').read_text())
    if code != 1 or results['status'] != 'failed':
        raise RuntimeError('post-command missing required file was not a failure')
    if not any('snapshot' in item or 'resource' in item
               for item in results['failures']):
        raise RuntimeError(f'missing post-command resource failure: {results["failures"]}')
    controls.append('actual main post-command required file loss fails')


def test_normal(work: Path, controls: list[str]) -> None:
    args, runner, transport = prepare(work / 'normal', 'normal')
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'normal')
    directory = executor.compile_case(
        'case-normal', manifest, 'cases', 'return', 0)
    row = report['cases'][0]
    if row['status'] != 'passed':
        raise RuntimeError(f'normal case failed: {row}')
    if row['api']['entry_symbol'] != 0:
        raise RuntimeError(f'wrong API entry: {row["api"]}')
    if [item['source_name'] for item in row['api']['symbols']] != [
            'main', 'mutate_then_panic', 'probe32']:
        raise RuntimeError(f'wrong API symbols: {row["api"]["symbols"]}')
    if sha(directory / 'program.py') != sha(
            args.out / 'api/case-normal/program.py'):
        raise RuntimeError('CLI/API program bytes differ in normal case')
    if set(row['generated_sha256']) != {
            'program.py', 'langrt.py', 'verified.hir', 'metadata.json'}:
        raise RuntimeError(f'missing four-artifact hashes: {row["generated_sha256"]}')
    if not (directory / 'verified.hir').read_text() == HIR:
        raise RuntimeError('API HIR was not published verbatim')
    labels = [command['label'] for command in runner.commands]
    for needle in ('case-normal-check', 'case-normal-build', 'case-normal-api',
                   'case-normal-reference', 'case-normal-program'):
        if needle not in labels:
            raise RuntimeError(f'missing command label {needle}: {labels}')
    for command in runner.commands:
        if command['trace_audit']['observed'] != command['trace_audit']['allowed']:
            raise RuntimeError(f'trace was not exact: {command["label"]}')
    executor.verify_trees()
    controls.append('normal CLI/API/metadata/reference')


def test_cli_failure(work: Path, controls: list[str]) -> None:
    args, runner, transport = prepare(work / 'cli-fail', 'cli-fail')
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'cli-fail')
    expect_raises(
        lambda: executor.compile_case(
            'case-cli-fail', manifest, 'cases', 'return', 0),
        'case-cli-fail')
    row = report['cases'][0]
    if row['status'] != 'failed':
        raise RuntimeError('CLI failure row was not failed')
    labels = [command['label'] for command in runner.commands]
    if labels != ['case-cli-fail-check']:
        raise RuntimeError(f'CLI failure ran later commands: {labels}')
    if (args.out / 'generated/case-cli-fail').exists():
        raise RuntimeError('CLI failure created the candidate output directory')
    if (args.out / 'api/case-cli-fail').exists():
        raise RuntimeError('CLI failure ran the API driver')
    raw = Path(row['candidate_check']['stdout_path'])
    if not raw.is_file() or 'E_TYPE_MISMATCH' not in raw.read_text():
        raise RuntimeError('CLI failure raw stdout was not retained')
    records = [json.loads(line) for line in
               (args.out / 'commands.jsonl').read_text().splitlines()]
    if [item['label'] for item in records] != ['case-cli-fail-check']:
        raise RuntimeError('commands.jsonl lost the failed CLI record')
    controls.append('CLI failure stops before API/helper and keeps raw')

    args, runner, transport = prepare(work / 'cli-old', 'cli-fail')
    old_candidate = args.out / 'generated/case-cli-old'
    old_candidate.mkdir(parents=True)
    sentinel = old_candidate / 'sentinel'
    sentinel.write_text('keep')
    old_mode = stat.S_IMODE(os.stat(sentinel).st_mode)
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'cli-old')
    expect_raises(
        lambda: executor.compile_case(
            'case-cli-old', manifest, 'cases', 'return', 0),
        'case-cli-old')
    if sentinel.read_text() != 'keep':
        raise RuntimeError('CLI failure changed an existing candidate file')
    if stat.S_IMODE(os.stat(sentinel).st_mode) != old_mode:
        raise RuntimeError('CLI failure changed an existing candidate mode')
    controls.append('CLI failure preserves an existing candidate tree')


def test_api_inconsistent(work: Path, controls: list[str]) -> None:
    args, runner, transport = prepare(work / 'api-diff', 'api-inconsistent')
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'api-diff')
    expect_raises(
        lambda: executor.compile_case(
            'case-api-diff', manifest, 'cases', 'return', 0),
        'CLI/API differ')
    directory = args.out / 'generated/case-api-diff'
    if (directory / 'metadata.json').exists() or (directory / 'verified.hir').exists():
        raise RuntimeError('stale metadata/HIR was published before byte comparison')
    row = report['cases'][0]
    if not Path(row['api']['emit']['stdout_path']).is_file():
        raise RuntimeError('API raw stdout was not retained')
    controls.append('CLI/API byte mismatch fails before metadata/HIR')


def test_api_malformed(work: Path, controls: list[str]) -> None:
    args, runner, transport = prepare(work / 'api-bad', 'api-malformed')
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'api-bad')
    expect_raises(
        lambda: executor.compile_case(
            'case-api-bad', manifest, 'cases', 'return', 0),
        'generated name')
    if (args.out / 'generated/case-api-bad/metadata.json').exists():
        raise RuntimeError('malformed API symbols were published')
    bad_row = report['cases'][0]
    if not Path(bad_row['api']['emit']['stdout_path']).is_file():
        raise RuntimeError('malformed API raw stdout was not retained')
    controls.append('malformed API symbol fields fail')

    args, runner, transport = prepare(work / 'api-hir', 'api-no-hir')
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'api-hir')
    expect_raises(
        lambda: executor.compile_case(
            'case-api-hir', manifest, 'cases', 'return', 0),
        'Verified HIR')
    hir_row = report['cases'][0]
    if not Path(hir_row['api']['emit']['stdout_path']).is_file():
        raise RuntimeError('missing-HIR raw stdout was not retained')
    controls.append('missing API verified_hir fails')

    expect_raises(
        lambda: run_cases.api_metadata({
            'status': 'ok', 'entry': 3,
            'symbols': [{'symbol': 0, 'source_name': 'main',
                         'generated': '_f0'}],
            'verified_hir': HIR}, 'api-entry-missing'),
        'absent from symbols')
    expect_raises(
        lambda: run_cases.api_metadata({
            'status': 'ok', 'entry': 0,
            'symbols': [{'symbol': 0, 'source_name': 'not_main',
                         'generated': '_f0'}],
            'verified_hir': HIR}, 'api-entry-name'),
        'source_name')
    expect_raises(
        lambda: run_cases.api_metadata({
            'status': 'ok', 'entry': 0,
            'symbols': [
                {'symbol': 0, 'source_name': 'main', 'generated': '_f0'},
                {'symbol': 0, 'source_name': 'other', 'generated': '_f1'}],
            'verified_hir': HIR}, 'api-duplicate-id'),
        'duplicate symbol id')
    controls.append('malformed entry/duplicate symbol metadata fails')


def test_anonymous_metadata(controls: list[str]) -> None:
    symbols = [
        {'symbol': 0, 'source_name': 'main', 'generated': '_f0'},
        {'symbol': 254, 'source_name': '', 'generated': '_f254'},
        {'symbol': 255, 'source_name': '', 'generated': '_f255'},
    ]
    document = {'entry_symbol': 0, 'symbols': symbols}
    assert run_cases.metadata_entry(document, 'anonymous') == ('_f0', 0)
    assert run_cases.metadata_rows(document, 'anonymous')[0] == symbols
    assert run_cases.named_generated(document, ['main'], 'anonymous') == {
        'main': '_f0'}
    for value in (None, 3, False):
        invalid = {'symbols': [dict(symbols[1], source_name=value)]}
        expect_raises(lambda: run_cases.metadata_rows(invalid, 'invalid'),
                      'source_name is not a string')
    expect_raises(
        lambda: run_cases.metadata_entry(dict(document, entry_symbol=254),
                                        'anonymous-entry'),
        'source_name')
    expect_raises(
        lambda: run_cases.metadata_rows(
            {'symbols': [symbols[0], dict(symbols[1], generated='_f0')]},
            'duplicate-generated'),
        'duplicate generated name')
    controls.append('anonymous function names may be empty; entry and ids remain checked')


def test_unexpected_exec(work: Path, controls: list[str]) -> None:
    args, runner, transport = prepare(work / 'extra-exec', 'extra-exec')
    expect_raises(
        lambda: runner.candidate_check(
            make_manifest(work / 'extra-exec'), 'extra-exec-check'),
        'exec whitelist')
    record = runner.commands[0]
    if not Path(record['stdout_path']).is_file():
        raise RuntimeError('unexpected-exec raw stdout was not retained')
    records = [json.loads(line) for line in
               (args.out / 'commands.jsonl').read_text().splitlines()]
    if not records or records[0]['label'] != 'extra-exec-check':
        raise RuntimeError('unexpected-exec raw record was not persisted')
    controls.append('unexpected exec fails after raw retention')


def test_empty_and_no_artifacts(work: Path, controls: list[str]) -> None:
    args, runner, transport = prepare(work / 'empty', 'empty-stdout')
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'empty')
    expect_raises(
        lambda: executor.compile_case(
            'case-empty', manifest, 'cases', 'return', 0),
        'no stdout')
    controls.append('empty candidate stdout cannot pass')

    args, runner, transport = prepare(work / 'no-artifacts', 'no-artifacts')
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'no-artifacts')
    expect_raises(
        lambda: executor.compile_case(
            'case-no-artifacts', manifest, 'cases', 'return', 0),
        'did not publish')
    controls.append('artifact-free candidate build cannot pass')


def test_rejections(work: Path, controls: list[str]) -> None:
    args, runner, transport = prepare(work / 'reject', 'reject')
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'reject')
    executor.compile_unsupported('unsupported-fake', manifest)
    if report['unsupported'][0]['status'] != 'unsupported':
        raise RuntimeError(f'rejection did not record unsupported: {report}')
    if (args.out / 'generated/unsupported-unsupported-fake').exists():
        raise RuntimeError('rejection created a candidate output directory')
    if report['failures']:
        raise RuntimeError(f'rejection recorded failures: {report["failures"]}')
    controls.append('candidate/reference rejection and no-artifact tree')

    # An existing candidate directory must survive a rejection byte/mode-wise.
    old = work / 'old-output'
    (old / 'generated/unsupported-old').mkdir(parents=True)
    (old / 'generated/unsupported-old/sentinel').write_text('keep')
    mode = stat.S_IMODE(os.stat(old / 'generated/unsupported-old/sentinel').st_mode)
    try:
        run_cases.prepare_output(old, strict=True)
    except RuntimeError as error:
        if 'refusing to overwrite' not in str(error):
            raise
    else:
        raise RuntimeError('non-empty --out was accepted')
    if (old / 'generated/unsupported-old/sentinel').read_text() != 'keep':
        raise RuntimeError('old output sentinel changed')
    if stat.S_IMODE(os.stat(old / 'generated/unsupported-old/sentinel').st_mode) != mode:
        raise RuntimeError('old output sentinel permissions changed')
    controls.append('non-empty old output directory is refused unchanged')

    # The 32-bit profile has no check-stage rejection; only build/codegen is
    # expected, matching the original helper assertion.
    args, runner, transport = prepare(work / 'profile32', 'reject')
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'profile32')
    executor.compile_profile32(manifest)
    if report['unsupported'][0]['status'] != 'unsupported':
        raise RuntimeError(f'32-bit rejection was not recorded: {report}')
    phase = json.loads(report['unsupported'][0]['compile']['stdout'])
    if phase['diagnostics'][0]['phase'] != 'codegen':
        raise RuntimeError('32-bit rejection lost the codegen phase')
    controls.append('32-bit build/codegen rejection')

    # A pre-existing candidate tree is retained byte/mode-wise even when the
    # builder rejects; the before/after output-tree reader is actually used.
    args, runner, transport = prepare(work / 'reject-old', 'reject')
    old_candidate = args.out / 'generated/unsupported-unsupported-old'
    old_candidate.mkdir(parents=True)
    sentinel = old_candidate / 'sentinel'
    sentinel.write_text('keep')
    old_mode = stat.S_IMODE(os.stat(sentinel).st_mode)
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'reject-old')
    executor.compile_unsupported('unsupported-old', manifest)
    if sentinel.read_text() != 'keep':
        raise RuntimeError('rejection changed an existing output file')
    if stat.S_IMODE(os.stat(sentinel).st_mode) != old_mode:
        raise RuntimeError('rejection changed an existing output mode')
    if report['unsupported'][0]['status'] != 'unsupported':
        raise RuntimeError('existing-tree rejection was not recorded')
    controls.append('rejection keeps an existing output tree and permissions')

    args, runner, transport = prepare(work / 'reject-publish', 'reject-publish')
    report = blank_report()
    executor = run_cases.PairedExecutor(args, report, runner)
    manifest = make_manifest(work / 'reject-publish')
    expect_raises(
        lambda: executor.compile_unsupported('unsupported-publish', manifest),
        'rejection published program.py')
    controls.append('rejection that publishes a program fails')


def test_legacy_metadata(work: Path, controls: list[str]) -> None:
    directory = work / 'legacy-meta'
    directory.mkdir(parents=True)
    document = {
        'contract': 'compiler-dev/1', 'backend': 'python', 'word_bits': 64,
        'entry_symbol': 7,
        'symbols': [
            {'symbol': 7, 'source_name': 'main', 'generated': 'renamed_main'},
            {'symbol': 9, 'source_name': 'mutate_then_panic',
             'generated': 'renamed_mutate'},
        ],
    }
    (directory / 'metadata.json').write_text(json.dumps(document))
    loaded = run_cases.read_metadata_file(directory, 'legacy')
    name, entry = run_cases.metadata_entry(loaded, 'legacy')
    if (name, entry) != ('renamed_main', 7):
        raise RuntimeError(f'legacy entry was guessed: {name}, {entry}')
    selected = run_cases.named_generated(
        loaded, ['mutate_then_panic'], 'legacy')
    if selected['mutate_then_panic'] != 'renamed_mutate':
        raise RuntimeError(f'legacy symbol was guessed: {selected}')
    if run_cases.LegacyExecutor.paired is not False:
        raise RuntimeError('legacy executor is marked paired')
    controls.append('entry/symbol selection uses published names, never _f')


def test_case_inventory(controls: list[str]) -> None:
    published = json.loads(
        (REPO / 'tests/backend_common/cases.json').read_text())
    if published['case_count'] != 15 or len(published['cases']) != 15:
        raise RuntimeError('published case count changed')
    if [case['id'] for case in published['cases']] != PUBLISHED_IDS:
        raise RuntimeError('published case order changed')
    if list(run_cases.EXTRA_SOURCES) != EXTRA_IDS:
        raise RuntimeError('EXTRA_SOURCES order/content changed')
    if list(run_cases.UNSUPPORTED_SOURCES) != ['string']:
        raise RuntimeError('UNSUPPORTED_SOURCES changed')
    if list(run_cases.SOURCE_REJECTIONS) != ['ordinary-parameter-rebinding']:
        raise RuntimeError('SOURCE_REJECTIONS changed')
    if list(run_cases.FLOAT_LITERALS) != [
            'zero32', 'one32', 'tie32', 'sub32', 'under32', 'over32',
            'zero64', 'one64', 'tie64', 'sub64', 'under64', 'over64']:
        raise RuntimeError('float literal inventory changed')
    if list(run_cases.CAUSES) != [
            'panic-division-by-zero', 'panic-invalid-shift',
            'panic-negative-shift', 'panic-dynamic-conversion',
            'panic-remainder-zero', 'panic-shift-u64-count',
            'panic-unsigned-conversion']:
        raise RuntimeError('panic inventory changed')
    widths = run_cases.integer_width_project()
    for typ in ('i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64',
                'isize', 'usize'):
        if typ not in widths:
            raise RuntimeError(f'integer width missing: {typ}')
    floats = run_cases.float_project()
    for name in list(run_cases.FLOAT_LITERALS) + ['probe32', 'probe64']:
        if f'func {name}(' not in floats:
            raise RuntimeError(f'float function missing: {name}')
    if 'func mutate_then_panic' not in run_cases.EXTRA_SOURCES[
            'inout-identity-and-copies']:
        raise RuntimeError('inout observation function changed')
    runner_source = (HERE / 'run_cases.py').read_text()
    for marker in ('controlled host bug', 'def broken(',
                   'issubclass(langrt.Panic, BaseException)',
                   'mutate_then_panic', 'struct.pack', '--emit-hir',
                   'compare_entries(directory, api_dir'):
        if marker not in runner_source:
            raise RuntimeError(f'key observation marker missing: {marker}')
    controls.append('15 published cases, EXTRA_SOURCES, widths, floats, '
                    'inout and key observation markers')


def test_input_snapshot(work: Path, controls: list[str]) -> None:
    args = make_args(REPO, work / 'snapshot')
    cases_file = REPO / 'tests/backend_common/cases.json'
    published = json.loads(cases_file.read_text())
    before = run_cases.snapshot_inputs(
        args, args.candidate_resources, published, cases_file)
    after = run_cases.snapshot_inputs(
        args, args.candidate_resources, published, cases_file)
    if not before or before != after:
        raise RuntimeError('input snapshot is not stable')
    if str(REPO / 'tests/python/run_cases.py') not in before:
        raise RuntimeError('runner script missing from the input snapshot')
    if str(REPO / 'tests/backend_common/cases.json') not in before:
        raise RuntimeError('published cases missing from the input snapshot')
    if run_cases.PAIRED_TIMEOUT_SECONDS != 30:
        raise RuntimeError('paired budget changed')
    controls.append('input snapshot before commands and fixed 30s budget')


def test_legacy_dispatch(controls: list[str]) -> None:
    parser = run_cases.build_parser()
    base = ['--helper', '/tmp/helper', '--root', str(REPO),
            '--out', '/tmp/out', '--python', '/tmp/python3.12']
    args = parser.parse_args(base)
    if run_cases.paired_requested(args):
        raise RuntimeError('legacy invocation selected paired mode')
    args = parser.parse_args(base + ['--candidate-cli', '/tmp/cli'])
    if not run_cases.paired_requested(args):
        raise RuntimeError('candidate argument did not select paired mode')
    controls.append('legacy no-new-argument dispatch and partial paired marker')


def main() -> int:
    install_guards()
    controls: list[str] = []
    try:
        with tempfile.TemporaryDirectory(prefix='python-cases-controls-') as raw:
            work = Path(raw)
            test_normal(work, controls)
            test_main_header_and_healthy_resources(work, controls)
            test_main_failure_report(work, controls)
            test_main_resource_preflight(work, controls)
            test_main_resource_changes(work, controls)
            test_cli_failure(work, controls)
            test_api_inconsistent(work, controls)
            test_api_malformed(work, controls)
            test_anonymous_metadata(controls)
            test_unexpected_exec(work, controls)
            test_empty_and_no_artifacts(work, controls)
            test_rejections(work, controls)
            test_legacy_metadata(work, controls)
            test_input_snapshot(work, controls)
            test_case_inventory(controls)
            test_legacy_dispatch(controls)
        if ATTEMPTS:
            raise RuntimeError(f'external calls were attempted: {ATTEMPTS}')
        result = {
            'status': 'passed',
            'scope': ('pure harness controls; actual main with a one-case '
                      'suite stub; not real Source/API conformance'),
            'controls': controls,
            'transport_calls': len(CALLS),
            'external_attempts': ATTEMPTS,
            'calls': [{'name': call['name'], 'timeout': call['timeout'],
                       'stdin': call['stdin']} for call in CALLS],
        }
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except BaseException as error:
        print(json.dumps({
            'status': 'failed',
            'error': str(error),
            'controls': controls,
            'transport_calls': len(CALLS),
            'external_attempts': ATTEMPTS,
        }, ensure_ascii=False, indent=2))
        raise


if __name__ == '__main__':
    raise SystemExit(main())
