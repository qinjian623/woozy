#!/usr/bin/env python3
"""Pure controls for the T10 public-CLI candidate runner.

Every control starts no compiler, driver, observer, generated program, NumPy,
PyTorch, version probe or network socket.  Module setup globally blocks
``subprocess``, ``os`` exec/spawn/fork, ``socket`` and the runner's lowest
bounded transport; individual tests only replace that lowest transport
(``_run_process_impl`` / legacy ``subprocess.run``) with a controlled record.
The real ``Runner.command``/``traced``/``emit_paired``/``observe`` branches are
therefore exercised, never replaced.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import socket
import stat
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path
from types import SimpleNamespace

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_t10_bindings as runner  # noqa: E402

REPO_ROOT = Path(runner.__file__).resolve().parents[2]
BLOCK = {'commands': 0, 'launches': 0, 'attempts': []}
_SAVED = {}


class LaunchBlocked(AssertionError):
    """Raised instead of starting a real process or socket."""


def _blocked_exec(*args, **kwargs):
    BLOCK['commands'] += 1
    BLOCK['attempts'].append('exec')
    raise LaunchBlocked('real external launch blocked')


def _blocked_transport(*args, **kwargs):
    BLOCK['commands'] += 1
    BLOCK['attempts'].append('transport')
    raise LaunchBlocked('real bounded transport blocked')


def setUpModule():
    _SAVED['transport'] = runner._run_process_impl
    runner._run_process_impl = _blocked_transport
    for name in ('run', 'Popen', 'call', 'check_call', 'check_output'):
        function = getattr(subprocess, name, None)
        if callable(function):
            _SAVED['subprocess.' + name] = function
            setattr(subprocess, name, _blocked_exec)
    for name in dir(os):
        if name.startswith(('exec', 'spawn')) or name in (
                'system', 'fork', 'forkpty', 'posix_spawn', 'posix_spawnp'):
            function = getattr(os, name, None)
            if callable(function):
                _SAVED['os.' + name] = function
                setattr(os, name, _blocked_exec)
    for name in ('socket', 'create_connection', 'create_server', 'socketpair'):
        function = getattr(socket, name, None)
        if callable(function):
            _SAVED['socket.' + name] = function
            setattr(socket, name, _blocked_exec)


def tearDownModule():
    runner._run_process_impl = _SAVED.pop('transport')
    while _SAVED:
        key, function = _SAVED.popitem()
        module_name, name = key.split('.')
        setattr({'subprocess': subprocess, 'os': os,
                 'socket': socket}[module_name], name, function)


def make_args(out, **changes):
    base = dict(
        helper=Path('/ref/lang_python_emit'),
        langc=None,
        candidate_cli=None,
        api_driver=None,
        strace=None,
        compile_timeout=180.0,
        root=Path('/root'),
        out=Path(out),
        python=Path('/venv/bin/python'),
        binding=Path('/root/tests/python/t10_binding.json'),
        example_binding=Path('/root/contracts/python_bindings.example.json'),
        reference=Path('/root/baseline/samples/numpy_torch_reference.py'),
        observe=Path('/root/tests/python/observe_t10.py'),
        original_observer=Path('/root/tests/python/t10_original04.py'),
        reference_observer=Path('/root/tests/python/t10_reference04.py'),
        real_observer=Path('/root/tests/python/t10_real_objects.py'),
        only=[],
    )
    base.update(changes)
    return SimpleNamespace(**base)


def main_argv(out, **changes):
    args = make_args(out, **changes)
    argv = ['--helper', str(args.helper), '--root', str(args.root),
            '--out', str(args.out), '--python', str(args.python),
            '--binding', str(args.binding),
            '--example-binding', str(args.example_binding),
            '--reference', str(args.reference),
            '--observe', str(args.observe),
            '--original-observer', str(args.original_observer),
            '--reference-observer', str(args.reference_observer)]
    if args.api_driver is not None:
        argv += ['--api-driver', str(args.api_driver)]
    if args.candidate_cli is not None:
        argv += ['--candidate-cli', str(args.candidate_cli)]
    if args.strace is not None:
        argv += ['--strace', str(args.strace)]
    return argv


def process(**changes):
    base = dict(exit=0, timed_out=False, cleanup_complete=True,
                abandoned_pipes=False)
    base.update(changes)
    return base


def symbols(entry=200):
    return [
        {'symbol': 100, 'source_name': 'pass', 'generated': '_f100'},
        {'symbol': entry, 'source_name': 'main',
         'generated': '_f' + str(entry)},
    ]


def api_document(**changes):
    base = {'status': 'ok', 'entry': 200, 'symbols': symbols()}
    base.update(changes)
    return base


def write_trace(argv):
    """Write one real-shaped strace line and return the executed tool path."""
    index = argv.index('-o')
    trace = Path(argv[index + 1])
    tool = argv[index + 2]
    trace.parent.mkdir(parents=True, exist_ok=True)
    trace.write_text('1  execve(%s, ["tool"], 0) = 0\n' % json.dumps(str(tool)))
    return str(Path(tool).resolve())


def transport_record(exit_code=0, stdout=b'', stderr=b'', **changes):
    record = {'exit': exit_code, 'stdout': stdout, 'stderr': stderr,
              'timed_out': False, 'cleanup_complete': True,
              'abandoned_pipes': False}
    record.update(changes)
    return record


def rejection_document(code='E_CAPABILITY_UNAVAILABLE'):
    return json.dumps({
        'status': 'error',
        'diagnostics': [{'code': code, 'message': 'rejected',
                         'primary': {'begin_byte': 0, 'end_byte': 1}}]})


class Controls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.out = self.root / 'evidence'

    def tearDown(self):
        self.temporary.cleanup()

    def block_launches(self):
        BLOCK['commands'] = 0
        BLOCK['launches'] = 0
        BLOCK['attempts'] = []
        return BLOCK

    def fixture_files(self):
        binding = self.root / 'binding.json'
        binding.write_text(json.dumps({
            'types': [], 'callables': [], 'modules': [],
            'package_requirements': []}) + '\n')
        example = self.root / 'example.json'
        example.write_text('{}\n')
        reference = self.root / 'reference.py'
        reference.write_text('VALUE = 0\n')
        return binding, example, reference

    def root_with_host(self):
        root = self.root / 'compiler'
        (root / 'tests/python').mkdir(parents=True, exist_ok=True)
        (root / 'tests/python/t10_host.py').write_text('HOST = 1\n')
        return root

    def project(self, name='project'):
        project = self.root / name
        (project / 'src').mkdir(parents=True)
        (project / 'lang.toml').write_text('manifest\n')
        return project

    def test_20_named_cases_exact_order(self):
        expected = [
            'order-objects-errors',
            'foreign-ownership-move-aggregate',
            'py-error-aggregate-storage',
            'foreign-ownership-cleanup',
            'generic-foreign-error-key',
            'generic-error-key-escaping',
            'foreign-ownership-rejections',
            'internal-cross-module',
            'internal-escape',
            'base-keyboard-interrupt',
            'base-system-exit',
            'base-panic',
            'bad-unit-contract',
            'no-throws-external-callable',
            'foreign-value-error',
            'function-foreign-error',
            'missing-host-environment-error',
            'real-ndarray-tensor-identity',
            'real-host-exception',
            'original-04-cpu',
        ]
        self.assertEqual(runner.NAMED_CASES, expected)
        self.assertEqual(len(expected), 20)
        self.assertEqual(len(set(expected)), 20)

    def test_foreign_ownership_three_case_subset_is_exact(self):
        self.assertEqual(runner.FOREIGN_OWNERSHIP_ONLY, [
            'foreign-ownership-move-aggregate',
            'foreign-ownership-cleanup',
            'foreign-ownership-rejections'])
        self.assertLess(len(runner.FOREIGN_OWNERSHIP_ONLY), 20)
        for name in runner.FOREIGN_OWNERSHIP_ONLY:
            self.assertIn(name, runner.NAMED_CASES)

    def test_nested_rejection_plan_has_eleven_projects(self):
        plan = runner.rejection_plan()
        self.assertEqual(len(plan), 11)
        self.assertEqual(len({name for name, _ in plan}), 11)
        self.assertEqual(len(runner.FOREIGN_REJECTION_CASES), 8)
        self.assertEqual(len(runner.DIRECT_REJECTION_CASES), 2)
        self.assertEqual(runner.WORD_BITS_REJECTION, 'foreign-reject-32bit')
        names = [name for name, _ in plan]
        self.assertEqual(names[-3:], ['direct-throws', 'direct-catch',
                                      'foreign-reject-32bit'])
        self.assertIn('read-after-move', names)
        self.assertEqual(dict(plan)['move-borrowed-parameter'],
                         'E_CALL_ARGUMENT')

    def test_registration_order_guard(self):
        runner.validate_registration(list(runner.NAMED_CASES))
        for broken in (list(reversed(runner.NAMED_CASES)),
                       runner.NAMED_CASES[:-1],
                       runner.NAMED_CASES + ['extra']):
            with self.subTest(length=len(broken)):
                with self.assertRaises(AssertionError):
                    runner.validate_registration(broken)

    def test_main_dispatch_legacy_with_launch_blocked(self):
        binding, example, reference = self.fixture_files()
        state = self.block_launches()
        changes = dict(root=REPO_ROOT, binding=binding,
                       example_binding=example, reference=reference)
        self.assertEqual(runner.dispatch_mode(make_args(self.out, **changes)),
                         'legacy')
        status = runner.main(main_argv(self.out, **changes))
        self.assertEqual(status, 1)
        self.assertEqual(state['launches'], 0)
        self.assertEqual(state['commands'], 20)
        self.assertEqual(state['attempts'], ['exec'] * 20)
        summary = json.loads((self.out / 'results.json').read_text())
        self.assertEqual(summary['status'], 'failed')
        self.assertEqual({row['name'] for row in summary['cases']},
                         set(runner.NAMED_CASES))
        self.assertTrue(all(
            row.get('error', {}).get('type', '').endswith('LaunchBlocked')
            for row in summary['cases']))

    def test_main_dispatch_paired_with_launch_blocked(self):
        binding, example, reference = self.fixture_files()
        state = self.block_launches()
        changes = dict(
            root=REPO_ROOT, binding=binding, example_binding=example,
            reference=reference,
            candidate_cli=Path('/candidate/woozyc'),
            api_driver=Path('/candidate/pythonobs'),
            strace=Path('/tools/strace'))
        self.assertEqual(runner.dispatch_mode(make_args(self.out, **changes)),
                         'paired')
        status = runner.main(main_argv(self.out, **changes))
        self.assertEqual(status, 1)
        self.assertEqual(state['launches'], 0)
        self.assertEqual(state['commands'], 20)
        self.assertEqual(state['attempts'], ['transport'] * 20)
        summary = json.loads((self.out / 'results.json').read_text())
        self.assertEqual({row['name'] for row in summary['cases']},
                         set(runner.NAMED_CASES))

    def test_invalid_role_rejected_before_launch(self):
        binding, example, reference = self.fixture_files()
        state = self.block_launches()
        cases = [
            dict(root=REPO_ROOT, binding=binding, example_binding=example,
                 reference=reference,
                 api_driver=Path('/candidate/pythonobs')),
            dict(root=REPO_ROOT, binding=binding, example_binding=example,
                 reference=reference,
                 candidate_cli=Path('/candidate/woozyc')),
            dict(root=REPO_ROOT, binding=binding, example_binding=example,
                 reference=reference,
                 candidate_cli=Path('/candidate/woozyc'),
                 api_driver=Path('/candidate/pythonobs')),
        ]
        for changes in cases:
            with self.subTest(changes=sorted(changes)):
                with self.assertRaises(AssertionError):
                    runner.main(main_argv(self.out, **changes))
        self.assertEqual(state['commands'], 0)
        self.assertEqual(state['launches'], 0)

    def test_invalid_budget_rejected_before_launch(self):
        binding, example, reference = self.fixture_files()
        state = self.block_launches()
        for budget in ('inf', '-inf', 'nan', '0', '-1', 'abc',
                       '180.1', '181', '86400'):
            with self.subTest(budget=budget):
                with self.assertRaises(SystemExit):
                    runner.main(main_argv(
                        self.out, binding=binding, example_binding=example,
                        reference=reference,
                        candidate_cli=Path('/candidate/woozyc'),
                        api_driver=Path('/candidate/pythonobs'),
                        strace=Path('/tools/strace')) +
                        ['--compile-timeout', budget])
        self.assertEqual(state['commands'], 0)
        self.assertEqual(state['launches'], 0)

    def test_nonempty_strict_output_rejected_before_launch(self):
        binding, example, reference = self.fixture_files()
        self.out.mkdir(parents=True)
        (self.out / 'previous.json').write_text('{}\n')
        state = self.block_launches()
        runner.prepare_output(self.out, strict=False)
        with self.assertRaisesRegex(AssertionError, 'refusing to overwrite'):
            runner.main(main_argv(
                self.out, root=REPO_ROOT, binding=binding,
                example_binding=example, reference=reference,
                candidate_cli=Path('/candidate/woozyc'),
                api_driver=Path('/candidate/pythonobs'),
                strace=Path('/tools/strace')))
        self.assertEqual(state['commands'], 0)
        self.assertEqual(state['launches'], 0)

    def test_strict_process_flags(self):
        runner.require_process(process())
        for key, values in {
                'exit': [False, True, 0.0, None, -9],
                'timed_out': [0, 1, None],
                'cleanup_complete': [1, 0, None],
                'abandoned_pipes': [0, 1, None]}.items():
            for value in values:
                with self.subTest(key=key, value=value):
                    with self.assertRaises(AssertionError):
                        runner.require_process(process(**{key: value}))
        with self.assertRaises(AssertionError):
            runner.require_process(process(exit=1))
        for dirty in (process(timed_out=True),
                      process(cleanup_complete=False),
                      process(abandoned_pipes=True)):
            with self.assertRaises(AssertionError):
                runner.require_process(dirty)

    def test_candidate_cli_failure_does_not_fall_back(self):
        args = make_args(self.out,
                         candidate_cli=Path('/candidate/woozyc'),
                         api_driver=Path('/candidate/pythonobs'),
                         strace=Path('/tools/strace'))
        r = runner.Runner(args)
        project = self.project()
        tools = []

        def fake_transport(argv, cwd, env, timeout):
            tools.append(write_trace(argv))
            return transport_record(1, b'{}', b'blocked')

        with unittest.mock.patch.object(runner, '_run_process_impl',
                                        fake_transport):
            row = {'name': 'candidate', 'status': 'failed', 'commands': []}
            with self.assertRaises(AssertionError):
                r.emit(row, project, 'binding', expect_success=True)
        # The CLI failed first; the helper was never consulted.
        self.assertEqual(tools, [str(Path('/candidate/woozyc').resolve())])
        self.assertEqual([record['label'] for record in row['commands']],
                         ['candidate-cli'])
        self.assertFalse((project / 'out' / 'program.py').exists())
        self.assertFalse((project / 'api-out').exists())

    def test_api_bytes_and_symbols_gate_metadata(self):
        self.assertEqual(runner.api_projection(api_document())['entry_symbol'],
                         200)
        broken = [
            api_document(symbols=None),
            api_document(symbols=[]),
            api_document(entry=999),
            api_document(entry='200'),
            api_document(entry=201, symbols=[
                {'symbol': 201, 'source_name': 'other',
                 'generated': '_f201'}]),
            api_document(entry=200, symbols=[
                {'symbol': 200, 'source_name': 'main',
                 'generated': '_f201'}]),
            api_document(symbols=[
                {'symbol': 1, 'source_name': 'a', 'generated': '_f1'},
                {'symbol': 2, 'source_name': 'b', 'generated': '_f1'}]),
            api_document(symbols=[
                {'symbol': 1, 'source_name': 'main', 'generated': '_f1'},
                {'symbol': 2, 'source_name': 'main', 'generated': '_f2'},
                {'symbol': 3, 'source_name': 'x', 'generated': '_f3'}]),
            api_document(symbols=[
                {'symbol': 1, 'source_name': 'x', 'generated': '_f1'},
                {'symbol': 2, 'source_name': 'y', 'generated': ''}]),
        ]
        with self.assertRaises(AssertionError):
            runner.api_projection(api_document(status='error'))
        for document in broken:
            with self.subTest(entry=document.get('entry')):
                with self.assertRaises(AssertionError):
                    runner.api_projection(document)

        first = self.root / 'cli'
        second = self.root / 'api'
        for directory in (first, second):
            directory.mkdir(parents=True, exist_ok=True)
            (directory / 'program.py').write_text('print(1)\n')
            (directory / 'langrt.py').write_text('RUNTIME = 1\n')
        runner.compare_generated(first, second)
        (second / 'langrt.py').write_text('RUNTIME = 2\n')
        with self.assertRaisesRegex(AssertionError, 'langrt.py'):
            runner.compare_generated(first, second)

        good_hir = 'compiler-dev/1 scalar-v1 module=0 backend=2 word_bits=64\n'
        for api_text, document in (
                ('print("API")\n', api_document(verified_hir=good_hir)),
                ('print(1)\n', api_document())):
            with self.subTest(api_hir='verified_hir' in document):
                args = make_args(
                    self.out, candidate_cli=Path('/candidate/woozyc'),
                    api_driver=Path('/candidate/pythonobs'),
                    strace=Path('/tools/strace'))
                r = runner.Runner(args)
                project = self.project(
                    'project-' + ('hir' if 'verified_hir' in document
                                  else 'no-hir'))
                tools = []

                def fake_transport(argv, cwd, env, timeout):
                    tool = write_trace(argv)
                    tools.append(tool)
                    directory = Path(cwd) / (
                        'out' if tool.endswith('/woozyc')
                        else 'reference-out' if 'lang_python_emit' in tool
                        else 'api-out')
                    directory.mkdir(parents=True, exist_ok=True)
                    text = api_text if 'pythonobs' in tool else 'print(1)\n'
                    (directory / 'program.py').write_text(text)
                    (directory / 'langrt.py').write_text(text)
                    stdout = (json.dumps(document).encode()
                              if 'pythonobs' in tool
                              else b'{"status":"ok"}')
                    return transport_record(0, stdout, b'')

                with unittest.mock.patch.object(
                        runner, '_run_process_impl', fake_transport):
                    row = {'name': 'candidate', 'status': 'failed',
                           'commands': []}
                    with self.assertRaises(AssertionError):
                        r.emit(row, project, 'binding', expect_success=True)
                self.assertEqual(len(tools), 3)
                self.assertFalse((project / 'out' / 'metadata.json').exists())
                self.assertFalse((project / 'out' / 'verified.hir').exists())

    def test_real_hir_required(self):
        good = ('compiler-dev/1 scalar-v1 module=0 backend=2 word_bits=64\n'
                'type 0 i32\n')
        self.assertEqual(runner.require_hir({'verified_hir': good}), good)
        for value in (None, '', '   ', 0, {}, ['x'], 'not a dump'):
            with self.subTest(value=value):
                with self.assertRaises(AssertionError):
                    runner.require_hir({'verified_hir': value})
        with self.assertRaises(AssertionError):
            runner.require_hir({})

    def test_trace_failure_keeps_raw_process(self):
        args = make_args(self.out, candidate_cli=Path('/candidate/woozyc'),
                         api_driver=Path('/candidate/pythonobs'),
                         strace=Path('/tools/strace'))
        r = runner.Runner(args)
        row = {'name': 'candidate', 'status': 'failed', 'commands': []}

        def fake_transport(argv, cwd, env, timeout):
            trace = Path(argv[argv.index('-o') + 1])
            trace.write_text(
                '42  execveat(3, "/bin/sh", ["sh"], 0) = 0\n')
            return transport_record(0, b'raw-out', b'raw-err')

        with unittest.mock.patch.object(runner, '_run_process_impl',
                                        fake_transport):
            with self.assertRaises(AssertionError):
                r.command(row, ['/candidate/woozyc', 'build'], self.out,
                          audit_allowed=runner.exec_allowlist(
                              '/candidate/woozyc'), label='candidate-cli')
        self.assertEqual(len(row['commands']), 1)
        self.assertEqual(len(r.commands), 1)
        record = row['commands'][0]
        self.assertEqual(record['exit'], 0)
        self.assertEqual(record['stdout'], 'raw-out')
        self.assertEqual(record['stderr'], 'raw-err')
        self.assertTrue(Path(record['stdout_path']).is_file())
        self.assertEqual(Path(record['stdout_path']).read_bytes(), b'raw-out')
        self.assertTrue(Path(record['trace_path']).is_file())
        self.assertNotIn('trace_audit', record)

    def test_compile_and_observer_whitelists_are_separate(self):
        self.assertNotEqual(runner.exec_allowlist('/candidate/woozyc'),
                            runner.exec_allowlist('/venv/bin/python'))
        trace = self.root / 'compile.trace'
        trace.write_text('1  execve("/candidate/woozyc", ["woozyc"], 0) = 0\n')
        record = {'trace_path': str(trace)}
        runner.require_trace(record, runner.exec_allowlist('/candidate/woozyc'))
        self.assertIn('trace_audit', record)
        with self.assertRaises(AssertionError):
            runner.require_trace({'trace_path': str(trace)},
                                 runner.exec_allowlist('/venv/bin/python'))
        observer_only = self.root / 'observer.trace'
        observer_only.write_text(
            '2  execve("/venv/bin/python", ["python"], 0) = 0\n')
        with self.assertRaises(AssertionError):
            runner.require_trace({'trace_path': str(observer_only)},
                                 runner.exec_allowlist('/candidate/woozyc'))

    def test_rejection_keeps_output_and_no_program(self):
        rejected = {'exit': 1, 'stdout': rejection_document(
            'E_CALL_ARGUMENT')}
        out = self.root / 'reject-out'
        out.mkdir()
        self.assertEqual(
            runner.require_rejection(rejected, out, ['E_CALL_ARGUMENT']),
            json.loads(rejected['stdout']))
        with self.assertRaises(AssertionError):
            runner.require_rejection({'exit': 0, 'stdout': rejected['stdout']},
                                     out, ['E_CALL_ARGUMENT'])
        with self.assertRaises(AssertionError):
            runner.require_rejection({'exit': 1, 'stdout': ''}, out, [])
        with self.assertRaises(AssertionError):
            runner.require_rejection(rejected, out,
                                     ['E_OWNERSHIP_USE_AFTER_MOVE'])
        (out / 'program.py').write_text('generated\n')
        with self.assertRaisesRegex(AssertionError, 'published program.py'):
            runner.require_rejection(rejected, out, [])
        (out / 'program.py').unlink()
        (out / 'langrt.py').write_text('generated runtime\n')
        with self.assertRaisesRegex(AssertionError, 'published langrt.py'):
            runner.require_rejection(rejected, out, [])

    def test_legacy_and_candidate_compile_argv(self):
        project = self.project('argv-project')
        manifest = project / 'lang.toml'

        legacy = runner.Runner(make_args(self.out))
        calls = []

        def legacy_run(argv, **kwargs):
            calls.append([str(value) for value in argv])
            return SimpleNamespace(returncode=1,
                                   stdout=rejection_document(),
                                   stderr='')

        with unittest.mock.patch.object(subprocess, 'run', legacy_run):
            legacy.emit_legacy({'name': 'legacy', 'status': 'failed',
                                'commands': []}, project, 'binding',
                               expect_success=False)
        self.assertEqual(calls, [[
            '/ref/lang_python_emit', '--project', str(manifest),
            '--out-dir', str(project / 'out'), '--std-binding', 'binding']])

        paired = runner.Runner(make_args(
            self.out, candidate_cli=Path('/candidate/woozyc'),
            api_driver=Path('/candidate/pythonobs'),
            strace=Path('/tools/strace')))
        calls = []

        def fake_transport(argv, cwd, env, timeout):
            tool = write_trace(argv)
            calls.append([str(value) for value in
                          argv[argv.index('-o') + 2:]])
            return transport_record(
                1, rejection_document().encode(), b'')

        with unittest.mock.patch.object(runner, '_run_process_impl',
                                        fake_transport):
            paired.emit_paired({'name': 'paired', 'status': 'failed',
                                'commands': []}, project, 'binding',
                               expect_success=False)
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0], [
            '/candidate/woozyc', 'build', '--project', str(manifest),
            '--backend', 'python', '--std-binding', 'binding',
            '--out-dir', str(project / 'out'), '--format', 'json'])
        self.assertEqual(calls[1], [
            '/ref/lang_python_emit', '--project', str(manifest),
            '--out-dir', str(project / 'reference-out'),
            '--std-binding', 'binding'])

    # --- correction controls (F1..F4) ------------------------------------

    def paired_runner(self, root=None, **changes):
        return runner.Runner(make_args(
            self.out, root=root or self.root_with_host(),
            candidate_cli=Path('/candidate/woozyc'),
            api_driver=Path('/candidate/pythonobs'),
            strace=Path('/tools/strace'), **changes))

    def test_paired_reference_helper_uses_bounded_trace_positive(self):
        r = self.paired_runner()
        project = self.project('positive')
        api = api_document(
            verified_hir='compiler-dev/1 scalar-v1 module=0 backend=2 '
                         'word_bits=64\n')

        def fake_transport(argv, cwd, env, timeout):
            tool = write_trace(argv)
            directory = Path(cwd) / (
                'out' if tool.endswith('/woozyc')
                else 'reference-out' if 'lang_python_emit' in tool
                else 'api-out')
            directory.mkdir(parents=True, exist_ok=True)
            body = "_LANG_BINDING_INFO = 1\nprint(_LANG_BINDING_INFO)\n"
            (directory / 'program.py').write_text(body)
            (directory / 'langrt.py').write_text(body)
            stdout = (json.dumps(api).encode() if 'pythonobs' in tool
                      else b'{"status":"ok"}')
            return transport_record(0, stdout, b'')

        row = {'name': 'positive', 'status': 'failed', 'commands': []}
        with unittest.mock.patch.object(runner, '_run_process_impl',
                                        fake_transport):
            out, cli = r.emit_paired(row, project, 'binding',
                                     expect_success=True)
        helper = [record for record in row['commands']
                  if record['label'] == 'reference-helper']
        self.assertEqual(len(helper), 1)
        helper = helper[0]
        for key in ('timed_out', 'cleanup_complete', 'abandoned_pipes'):
            self.assertIs(type(helper[key]), bool)
        self.assertFalse(helper['timed_out'])
        self.assertTrue(helper['cleanup_complete'])
        self.assertFalse(helper['abandoned_pipes'])
        self.assertTrue(Path(helper['trace_path']).is_file())
        self.assertEqual(row['reference_emit']['exit'], 0)
        self.assertTrue((out / 'metadata.json').is_file())
        self.assertTrue((out / 'verified.hir').is_file())

    def test_paired_reference_helper_uses_bounded_trace_rejection(self):
        r = self.paired_runner()
        project = self.project('rejection')

        def fake_transport(argv, cwd, env, timeout):
            write_trace(argv)
            return transport_record(1, rejection_document().encode(), b'')

        row = {'name': 'rejection', 'status': 'failed', 'commands': []}
        with unittest.mock.patch.object(runner, '_run_process_impl',
                                        fake_transport):
            out, cli = r.emit_paired(row, project, 'binding',
                                     expect_success=False)
        self.assertEqual(cli['exit'], 1)
        helper = [record for record in row['commands']
                  if record['label'] == 'reference-helper']
        self.assertEqual(len(helper), 1)
        helper = helper[0]
        for key in ('timed_out', 'cleanup_complete', 'abandoned_pipes'):
            self.assertIs(type(helper[key]), bool)
        self.assertTrue(Path(helper['trace_path']).is_file())
        self.assertFalse((out / 'program.py').exists())
        self.assertFalse((out / 'langrt.py').exists())

    def test_dirty_observer_flags_rejected(self):
        for flag, value in (('timed_out', True),
                            ('cleanup_complete', False),
                            ('abandoned_pipes', True)):
            for method in ('observe', 'real_observe'):
                with self.subTest(flag=flag, method=method):
                    r = self.paired_runner()
                    row = {'name': method, 'status': 'failed',
                           'commands': []}
                    out = self.root / (flag + '-' + method)
                    out.mkdir()

                    def fake_transport(argv, cwd, env, timeout,
                                       flag=flag, value=value):
                        write_trace(argv)
                        return transport_record(
                            0, b'{"pure_mock_observation":true}\n', b'',
                            **{flag: value})

                    with unittest.mock.patch.object(
                            runner, '_run_process_impl', fake_transport):
                        with self.assertRaisesRegex(AssertionError,
                                                    'not clean'):
                            if method == 'observe':
                                r.observe(row, out)
                            else:
                                r.real_observe(row, out, 'objects')
                    self.assertEqual(len(row['commands']), 1)
                    record = row['commands'][0]
                    self.assertIs(record[flag], value)
                    self.assertTrue(Path(record['stdout_path']).is_file())
                    self.assertEqual(
                        Path(record['stdout_path']).read_bytes(),
                        b'{"pure_mock_observation":true}\n')

    # --- optional observer uname -p policy (SH16) ------------------------

    def exec_line(self, pid, path, argv, result=0, detail=''):
        """One real-shaped strace execve line for a supplied raw trace."""
        return ('%d  execve(%s, %s, 0x0 /* 0 vars */) = %d%s\n'
                % (pid, json.dumps(path), json.dumps(argv), result, detail))

    def uname_fixture(self, name='uname', mode=0o755):
        path = self.root / 'bin' / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#!/bin/sh\nexit 0\n')
        path.chmod(mode)
        return path

    def observer_runner(self, env=None):
        """Build the real paired Runner, freezing before any command starts."""
        if env is not None:
            with unittest.mock.patch.dict(os.environ, env):
                return self.paired_runner()
        return self.paired_runner()

    def observer_lookup(self, r):
        """The frozen PATH uname lookup sequence and the frozen tool index."""
        identity = r.uname_identity
        self.assertIsInstance(identity, dict)
        lookup = [str((Path(entry) / 'uname').resolve())
                  for entry in identity['path_entries']]
        self.assertIn(identity['canonical'], lookup)
        return identity, lookup, lookup.index(identity['canonical'])

    def run_observer(self, r, trace_text, row=None):
        """Run the real Runner.real_observe over one supplied raw trace."""
        out = self.root / 'observe-out'
        out.mkdir(exist_ok=True)
        if row is None:
            row = {'name': 'real-observe', 'status': 'failed',
                   'commands': []}

        def fake_transport(argv, cwd, env, timeout):
            trace = Path(argv[argv.index('-o') + 1])
            trace.parent.mkdir(parents=True, exist_ok=True)
            trace.write_text(trace_text)
            return transport_record(0, b'{"mode":"identity"}\n', b'')

        with unittest.mock.patch.object(runner, '_run_process_impl',
                                        fake_transport):
            r.real_observe(row, out, 'identity')
        return row

    def observe_trace(self, trace_text, row=None, env=None):
        return self.run_observer(self.observer_runner(env), trace_text, row)

    def uname_trace(self, r, python_pid=1, uname_pid=2, lookups=None,
                    success=None, argv=None, python_result=None):
        """Build a Python plus legal uname trace for this frozen Runner."""
        python = str(Path('/venv/bin/python').resolve())
        identity, lookup, index = self.observer_lookup(r)
        if python_result is None:
            header = self.exec_line(python_pid, python, ['python'])
        else:
            header = self.exec_line(python_pid, python, ['python'],
                                    python_result,
                                    ' ENOENT (No such file or directory)')
        lines = [header]
        chosen = lookup[:index] if lookups is None else lookups
        argv = argv or ['uname', '-p']
        for path in chosen:
            lines.append(self.exec_line(
                uname_pid, path, argv, -1,
                ' ENOENT (No such file or directory)'))
        lines.append(self.exec_line(
            uname_pid, identity['canonical'] if success is None else success,
            argv))
        return ''.join(lines)

    def test_observer_accepts_python_only_and_optional_uname(self):
        python = str(Path('/venv/bin/python').resolve())
        # The paired Runner freezes /usr/bin/uname before any command starts.
        r = self.observer_runner()
        identity = r.uname_identity
        self.assertEqual(r.commands, [])
        self.assertEqual(identity['path'], runner.OBSERVER_UNAME_PATH)
        self.assertEqual(identity['canonical'],
                         str(Path(runner.OBSERVER_UNAME_PATH).resolve()))
        frozen_sha = hashlib.sha256(
            Path(identity['canonical']).read_bytes()).hexdigest()
        self.assertEqual(identity['sha256'], frozen_sha)
        # Python-only stays exact and does not require the optional probe.
        audit = self.run_observer(
            r, self.exec_line(1, python, ['python']))['commands'][0][
                'trace_audit']
        self.assertEqual(audit['observed'], [python])
        self.assertNotIn('optional_uname', audit)
        self.assertTrue(audit['uname_identity']['unchanged'])
        self.assertEqual(audit['uname_identity']['probe_count'], 0)
        self.assertEqual(
            audit['uname_identity']['sha256_before'], identity['sha256'])
        self.assertEqual(
            audit['uname_identity']['sha256_at_audit'], identity['sha256'])
        # Python-only does not depend on the frozen PATH containing the tool.
        r = self.observer_runner({'PATH': str(self.root / 'only-path')})
        audit = self.run_observer(
            r, self.exec_line(1, python, ['python']))['commands'][0][
                'trace_audit']
        self.assertEqual(audit['observed'], [python])
        self.assertNotIn('optional_uname', audit)
        # The real fixed tool with one failed PATH lookup before its success.
        env = {'PATH': str(self.root / 'first-path')
               + os.pathsep + os.environ['PATH']}
        r = self.observer_runner(env)
        identity, lookup, index = self.observer_lookup(r)
        self.assertGreater(index, 0)
        audit = self.run_observer(r, self.uname_trace(r))['commands'][0][
            'trace_audit']
        probe = audit['optional_uname'][0]
        self.assertEqual(probe['executable'], identity['canonical'])
        self.assertEqual([item['path'] for item in probe['lookup']],
                         lookup[:index])
        self.assertEqual(probe['argv'], ['uname', '-p'])
        self.assertTrue(audit['uname_identity']['unchanged'])
        self.assertEqual(audit['uname_identity']['sha256_before'],
                         identity['sha256'])

    def test_observer_rejects_unverified_uname_and_mixed_execs(self):
        python = str(Path('/venv/bin/python').resolve())
        py_line = self.exec_line(1, python, ['python'])
        helper = str(Path('/ref/lang_python_emit').resolve())
        script = self.uname_fixture()  # same-name executable, not the tool
        plain = self.root / 'plain' / 'uname'
        plain.parent.mkdir(parents=True, exist_ok=True)
        plain.write_text('#!/bin/sh\nexit 0\n')
        plain.chmod(0o644)
        echo = self.uname_fixture('echo')
        front = str(self.root / 'first-path')
        prefix_env = {'PATH': front + os.pathsep + os.environ['PATH']}

        def cases():
            # F1: arbitrary existing executable named uname (never trusted).
            r = self.observer_runner()
            yield ('F1_arbitrary_named_uname', r,
                   py_line + self.exec_line(
                       2, str(script.resolve()), ['uname', '-p']),
                   'not the frozen tool')
            # F2: execveat after a legal execve cannot bypass argv checks.
            r = self.observer_runner()
            yield ('F2_execveat_after_approved_execve', r,
                   self.uname_trace(r) + self.exec_line(
                       3, r.uname_identity['canonical'], ['uname', '-a'])
                   .replace('execve(', 'execveat(3, ', 1),
                   'execveat')
            # F2: a real ENOENT errno token is required, not text.
            r = self.observer_runner(prefix_env)
            identity, lookup, index = self.observer_lookup(r)
            yield ('F2_errno_substring', r,
                   py_line + self.exec_line(
                       2, lookup[0], ['uname', '-p'], -1,
                       ' EACCES (ENOENT is only text)')
                   + self.exec_line(2, identity['canonical'],
                                    ['uname', '-p']),
                   'errno is not ENOENT')
            # F3: a failed Python exec is not a successful appearance.
            r = self.observer_runner()
            yield ('F3_failed_python', r,
                   self.uname_trace(r, python_result=-1),
                   'did not successfully execve')
            # F3: a malformed Python record is rejected, not counted.
            r = self.observer_runner()
            identity, lookup, index = self.observer_lookup(r)
            malformed = ['1  execve(%s, ["python"], 0) = garbage\n'
                         % json.dumps(python)]
            for path in lookup[:index]:
                malformed.append(self.exec_line(
                    2, path, ['uname', '-p'], -1,
                    ' ENOENT (No such file or directory)'))
            malformed.append(self.exec_line(
                2, identity['canonical'], ['uname', '-p']))
            yield ('F3_malformed_python', r, ''.join(malformed),
                   'unparseable required execve record')
            # Other bad forms stay rejected under the same real Runner.
            r = self.observer_runner(prefix_env)
            yield ('unknown_path_lookup', r,
                   py_line + self.exec_line(
                       2, '/unknown/lookup/uname', ['uname', '-p'], -1,
                       ' ENOENT (No such file or directory)')
                   + self.exec_line(
                       2, r.uname_identity['canonical'], ['uname', '-p']),
                   'not frozen PATH entry')
            r = self.observer_runner(prefix_env)
            identity, lookup, index = self.observer_lookup(r)
            yield ('dangling_lookup', r,
                   py_line + self.exec_line(
                       2, lookup[0], ['uname', '-p'], -1,
                       ' ENOENT (No such file or directory)'),
                   'without an executed child')
            r = self.observer_runner()
            yield ('reused_observer_pid', r,
                   self.uname_trace(r, python_pid=1, uname_pid=1),
                   'reused the traced tool pid')
            r = self.observer_runner({'PATH': '/usr/bin:/bin'})
            identity, lookup, index = self.observer_lookup(r)
            self.assertEqual(index, 0)
            yield ('failed_then_success_same_path', r,
                   py_line + self.exec_line(
                       2, identity['canonical'], ['uname', '-p'], -1,
                       ' ENOENT (No such file or directory)')
                   + self.exec_line(2, identity['canonical'],
                                    ['uname', '-p']),
                   'reported failed lookup and success')
            r = self.observer_runner()
            yield ('wrong_argv', r,
                   self.uname_trace(r, argv=['uname', '-a']), 'argv')
            r = self.observer_runner()
            yield ('extra_argv', r,
                   self.uname_trace(r, argv=['uname', '-p', 'x']), 'argv')
            r = self.observer_runner()
            yield ('relative_uname', r,
                   py_line + '2  execve("uname", ["uname", "-p"], 0) = 0\n',
                   'relative')
            r = self.observer_runner()
            yield ('other_command', r,
                   py_line + self.exec_line(2, str(echo.resolve()),
                                            ['echo']),
                   'unexpected compiler subprocesses')
            r = self.observer_runner()
            yield ('cpp_helper_mixed', r,
                   self.uname_trace(r) + self.exec_line(
                       3, helper, ['lang_python_emit']),
                   'unexpected compiler subprocesses')
            r = self.observer_runner()
            yield ('non_executable_named_uname', r,
                   py_line + self.exec_line(
                       2, str(plain.resolve()), ['uname', '-p']),
                   'not the frozen tool')

        for name, r, text, message in cases():
            with self.subTest(case=name):
                row = {'name': 'real-observe', 'status': 'failed',
                       'commands': []}
                with self.assertRaisesRegex(AssertionError, message):
                    self.run_observer(r, text, row)
                # The raw process record is retained before the policy fails.
                self.assertEqual(len(row['commands']), 1)
                record = row['commands'][0]
                self.assertEqual(record['stdout'], '{"mode":"identity"}\n')
                self.assertEqual(record['exit'], 0)
                self.assertTrue(Path(record['stdout_path']).is_file())
                self.assertEqual(Path(record['stdout_path']).read_bytes(),
                                 b'{"mode":"identity"}\n')
                self.assertNotIn('trace_audit', record)

    def test_observer_rejects_changed_uname_identity(self):
        # Legacy mode never touches the new frozen path.
        with unittest.mock.patch.object(runner, 'OBSERVER_UNAME_PATH',
                                        '/nonexistent/uname'):
            legacy = runner.Runner(make_args(self.out))
            self.assertIsNone(legacy.uname_identity)
        anchor = self.root / 'anchor' / 'uname'
        anchor.parent.mkdir(parents=True)
        # A missing fixed tool fails the paired mode before any command starts.
        with unittest.mock.patch.object(runner, 'OBSERVER_UNAME_PATH',
                                        str(anchor)):
            with self.assertRaisesRegex(AssertionError,
                                        'frozen uname tool is missing'):
                self.paired_runner()
        anchor.write_text('before\n')
        anchor.chmod(0o644)
        with unittest.mock.patch.object(runner, 'OBSERVER_UNAME_PATH',
                                        str(anchor)):
            r = self.paired_runner()
        frozen = dict(r.uname_identity)
        self.assertEqual(frozen['canonical'], str(anchor.resolve()))
        self.assertEqual(frozen['sha256'],
                         hashlib.sha256(b'before\n').hexdigest())
        python = str(Path('/venv/bin/python').resolve())
        text = (self.exec_line(1, python, ['python'])
                + self.exec_line(2, str(anchor.resolve()), ['uname', '-p']))
        # A post-freeze content change is not silently re-read as trusted.
        anchor.write_text('after\n')
        row = {'name': 'real-observe', 'status': 'failed', 'commands': []}
        with self.assertRaisesRegex(AssertionError, 'identity changed'):
            self.run_observer(r, text, row)
        self.assertEqual(len(row['commands']), 1)
        self.assertNotIn('trace_audit', row['commands'][0])
        # A frozen tool that disappears before the audit is rejected too.
        anchor.unlink()
        with self.assertRaisesRegex(AssertionError, 'frozen uname tool is '
                                                    'missing'):
            self.run_observer(r, text)

    def test_non_observer_whitelists_still_reject_uname_and_helper(self):
        uname = str(Path(runner.OBSERVER_UNAME_PATH).resolve())
        candidate = str(Path('/candidate/woozyc').resolve())
        helper = str(Path('/ref/lang_python_emit').resolve())
        python = str(Path('/venv/bin/python').resolve())
        candidate_line = self.exec_line(1, candidate, ['woozyc'])
        uname_line = self.exec_line(2, uname, ['uname', '-p'])
        helper_line = self.exec_line(3, helper, ['lang_python_emit'])
        for name, text, accepted in (
                ('candidate-only', candidate_line, True),
                ('candidate-plus-uname', candidate_line + uname_line, False),
                ('candidate-plus-helper', candidate_line + helper_line,
                 False)):
            with self.subTest(case=name):
                r = runner.Runner(make_args(
                    self.out, candidate_cli=Path('/candidate/woozyc'),
                    api_driver=Path('/candidate/pythonobs'),
                    strace=Path('/tools/strace')))
                row = {'name': 'candidate', 'status': 'failed',
                       'commands': []}

                def fake_transport(argv, cwd, env, timeout, text=text):
                    trace = Path(argv[argv.index('-o') + 1])
                    trace.parent.mkdir(parents=True, exist_ok=True)
                    trace.write_text(text)
                    return transport_record(0, b'{}', b'')

                with unittest.mock.patch.object(
                        runner, '_run_process_impl', fake_transport):
                    if accepted:
                        r.command(row, [candidate, 'build'], self.root,
                                  audit_allowed=runner.exec_allowlist(
                                      candidate), label='candidate-cli')
                    else:
                        with self.assertRaises(AssertionError):
                            r.command(row, [candidate, 'build'], self.root,
                                      audit_allowed=runner.exec_allowlist(
                                          candidate), label='candidate-cli')
                self.assertEqual(len(row['commands']), 1)
                self.assertEqual('trace_audit' in row['commands'][0],
                                 accepted)
        # The observed generated program is not an observer: uname stays
        # outside its exec whitelist as well.
        r = self.paired_runner()
        out = self.root / 'program-out'
        out.mkdir()
        row = {'name': 'program', 'status': 'failed', 'commands': []}

        def direct_transport(argv, cwd, env, timeout):
            trace = Path(argv[argv.index('-o') + 1])
            trace.parent.mkdir(parents=True, exist_ok=True)
            trace.write_text(self.exec_line(1, python, ['python'])
                             + self.exec_line(2, uname, ['uname', '-p']))
            return transport_record(0, b'', b'')

        with unittest.mock.patch.object(runner, '_run_process_impl',
                                        direct_transport):
            with self.assertRaises(AssertionError):
                r.direct(row, out)
        self.assertEqual(len(row['commands']), 1)
        self.assertNotIn('trace_audit', row['commands'][0])

    def test_candidate_rejection_requires_both_files_absent(self):
        for leaked in ('program.py', 'langrt.py'):
            with self.subTest(leaked=leaked):
                r = self.paired_runner()
                project = self.project('leak-' + leaked)
                tools = []

                def fake_transport(argv, cwd, env, timeout, leaked=leaked):
                    tool = write_trace(argv)
                    tools.append(tool)
                    if tool.endswith('/woozyc'):
                        out = Path(cwd) / 'out'
                        out.mkdir(parents=True, exist_ok=True)
                        (out / leaked).write_text('MOCK leaked\n')
                    return transport_record(
                        1, rejection_document().encode(), b'')

                with unittest.mock.patch.object(
                        runner, '_run_process_impl', fake_transport):
                    with self.assertRaisesRegex(AssertionError,
                                                'published ' + leaked):
                        r.emit_paired(
                            {'name': 'leak', 'status': 'failed',
                             'commands': []}, project, 'binding',
                            expect_success=False)
                # The candidate check fails before the reference helper starts.
                self.assertEqual(tools,
                                 [str(Path('/candidate/woozyc').resolve())])

    def test_candidate_rejection_preserves_old_directory(self):
        r = self.paired_runner()
        project = self.project('old-directory')
        out = project / 'out'
        out.mkdir()
        sentinel = out / 'keep.txt'
        sentinel.write_text('old evidence\n')
        os.chmod(sentinel, 0o640)
        before = sentinel.read_bytes()
        mode = stat.S_IMODE(os.stat(sentinel).st_mode)

        def fake_transport(argv, cwd, env, timeout):
            write_trace(argv)
            return transport_record(1, rejection_document().encode(), b'')

        with unittest.mock.patch.object(runner, '_run_process_impl',
                                        fake_transport):
            returned, record = r.emit_paired(
                {'name': 'old', 'status': 'failed', 'commands': []},
                project, 'binding', expect_success=False)
        self.assertEqual(record['exit'], 1)
        self.assertEqual(sentinel.read_bytes(), before)
        self.assertEqual(stat.S_IMODE(os.stat(sentinel).st_mode), mode)
        self.assertFalse((out / 'program.py').exists())
        self.assertFalse((out / 'langrt.py').exists())

    def test_compile_timeout_capped_at_180(self):
        self.assertEqual(runner.finite_positive('180'), 180.0)
        self.assertEqual(runner.finite_positive('0.5'), 0.5)
        for value in ('180.1', '181', '86400'):
            with self.subTest(value=value):
                with self.assertRaises(argparse.ArgumentTypeError):
                    runner.finite_positive(value)
        state = self.block_launches()
        binding, example, reference = self.fixture_files()
        with self.assertRaises(SystemExit):
            runner.main(main_argv(
                self.out, root=self.root_with_host(), binding=binding,
                example_binding=example, reference=reference,
                candidate_cli=Path('/candidate/woozyc'),
                api_driver=Path('/candidate/pythonobs'),
                strace=Path('/tools/strace')) +
                ['--compile-timeout', '86400'])
        self.assertEqual(state['commands'], 0)

    def test_rejection_detects_changed_old_directory(self):
        for change in ('bytes', 'file_mode', 'delete_file', 'add_file',
                       'directory_mode', 'add_directory'):
            with self.subTest(change=change):
                r = self.paired_runner()
                project = self.project('changed-' + change)
                out = project / 'out'
                out.mkdir()
                sentinel = out / 'keep.txt'
                sentinel.write_bytes(b'old evidence\n')
                sentinel.chmod(0o640)
                out.chmod(0o750)
                tools = []

                def fake_transport(argv, cwd, env, timeout):
                    tools.append(write_trace(argv))
                    if change == 'bytes':
                        sentinel.write_bytes(b'changed\n')
                    elif change == 'file_mode':
                        sentinel.chmod(0o600)
                    elif change == 'delete_file':
                        sentinel.unlink()
                    elif change == 'add_file':
                        (out / 'added').write_bytes(b'new\n')
                    elif change == 'directory_mode':
                        out.chmod(0o700)
                    else:
                        (out / 'added').mkdir()
                    return transport_record(1, rejection_document().encode())

                with unittest.mock.patch.object(
                        runner, '_run_process_impl', fake_transport):
                    with self.assertRaisesRegex(AssertionError,
                                                'changed existing output'):
                        r.emit_paired({'commands': []}, project, 'binding',
                                      expect_success=False)
                self.assertEqual(tools,
                                 [str(Path('/candidate/woozyc').resolve())])


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Controls)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    print(f'CONTROLS {result.testsRun} '
          + ('passed' if result.wasSuccessful() else 'failed'))
    raise SystemExit(0 if result.wasSuccessful() else 1)
