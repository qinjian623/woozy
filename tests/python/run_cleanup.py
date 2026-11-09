#!/usr/bin/env python3
"""Emit real VerifiedHIR and observe cleanup events, values and error payloads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from cleanup_cases import CASES, CROSS_MODULE

UNSAFE = {
    'unsafe-scalar-lambda': dict(events=[], main_return=0),
    'unsafe-return-owned-defer': dict(events=[1, 2, 3], log=123,
                                      main_return=0),
    'unsafe-loop-owned-defer': dict(events=[11, 21, 32], log=1342,
                                    main_return=0),
    'unsafe-caught-throw-owned-defer': dict(events=[1, 2, 3], log=123,
                                            main_return=0),
    'unsafe-inner-catch-owned-defer': dict(events=[1, 2, 3, 4], log=1234,
                                           main_return=0),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, **kwargs):
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    result = subprocess.run(list(map(str, argv)), capture_output=True, text=True,
                            env=kwargs.pop('env', env), timeout=60, **kwargs)
    return dict(argv=list(map(str, argv)), returncode=result.returncode,
                stdout=result.stdout, stderr=result.stderr)


def project(directory, files):
    directory.mkdir(parents=True, exist_ok=True)
    manifest = directory / 'lang.toml'
    manifest.write_text('schema_version=1\n[package]\nname="main"\n'
                        'source_root="src"\nentry="app"\n[build]\n'
                        'backend="python"\nword_bits=64\n')
    for name, source in files.items():
        file = directory / 'src' / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(source)
    return manifest


def copied_project(directory, source):
    directory.mkdir(parents=True, exist_ok=True)
    manifest = directory / 'lang.toml'
    manifest.write_text('schema_version=1\n[package]\nname="main"\n'
                        'source_root="src"\nentry="app"\n[build]\n'
                        'backend="python"\nword_bits=64\n')
    for file in source.rglob('*'):
        if not file.is_file() or file.name == 'lang.toml':
            continue
        target = directory / file.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(file.read_bytes())
    return manifest


def validate(ident, actual, expected):
    if 'events' in expected:
        assert actual['events'] == expected['events'], (ident, actual['events'], expected)
    if 'panic' in expected:
        assert actual['panic'] == expected['panic'], actual
        assert actual['probe_panic']['reason'] == expected['panic'], actual
        assert actual['probe_panic']['log'] == expected['log'], actual
        assert actual['probe_events'] == expected['events'], actual
        assert 'main_return' not in actual, actual
    elif 'typed' in expected:
        assert actual['typed']['type_key'] == expected['typed'], actual
        assert 'main_return' not in actual, actual
    else:
        assert actual['main_return'] == expected.get('main_return', 0), actual
    for name, result in expected.get('function_returns', {}).items():
        returns = [r['value'] for r in actual['returns'] if r['function'] == name]
        assert returns and all(x == result for x in returns), (name, returns)
    for forbidden in expected.get('unexecuted', []):
        assert not any(r['function'] == forbidden for r in actual['calls']), actual
    for event in expected.get('forbidden_events', []):
        assert event not in actual['events'], actual


def main():
    parser = argparse.ArgumentParser()
    for key in ('helper', 'root', 'out', 'python'):
        parser.add_argument('--' + key, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(operation_id='language-t08-python-execution-20261104',
                  contract='compiler-dev/1', status='failed', cases=[], extra=[],
                  not_run=['native T08 execution (native actor)',
                           'final integrated CLI acceptance (root)',
                           'ASan/UBSan and LLVM-off (root)'],
                  unsupported=['dynamic ARC/Vec', 'foreign bindings', 'strict FP'])
    immutable = [args.root / 'tests/cleanup/cases.json', args.root / 'tests/generics/cases.json']
    for directory in ('tests/cleanup/fixtures', 'tests/generics/fixtures',
                      'baseline', 'tests/floating'):
        immutable += sorted(p for p in (args.root / directory).rglob('*') if p.is_file())
    before = {str(p.relative_to(args.root)): sha(p) for p in immutable}
    report['immutable_sha256'] = before
    report['source_sha256'] = {str(p.relative_to(args.root)): sha(p)
                              for directory in ('src/backend/python', 'runtime/python', 'tests/python')
                              for p in (args.root / directory).rglob('*') if p.is_file()}
    report['shared_test_sha256'] = {name: sha(args.root / name)
        for name in ('src/hir/hir.cpp', 'tests/cleanup/static_test.cpp')}
    trap = args.out / 'no-python'
    trap.mkdir(exist_ok=True)
    for name in ('python', 'python3', 'python3.12'):
        file = trap / name
        file.write_text('#!/bin/sh\necho unexpected-python-during-compilation >&2\nexit 118\n')
        file.chmod(0o755)

    def compile_run(ident, manifest, expected, group, reject=None):
        directory = args.out / 'generated' / ident
        row = dict(id=ident, expected=expected, status='failed', manifest=str(manifest))
        report[group].append(row)
        row['source_sha256'] = {str(p.relative_to(manifest.parent)): sha(p)
            for p in manifest.parent.rglob('*') if p.is_file()}
        row['emit'] = emitted = command(['/usr/bin/env', f'PATH={trap}', args.helper,
            '--project', manifest, '--out-dir', directory])
        parsed = json.loads(emitted['stdout'])
        if reject:
            assert emitted['returncode'] == 1 and not emitted['stderr'], emitted
            assert parsed['status'] == 'error' and any(d['code'] == reject
                for d in parsed['diagnostics']), parsed
            assert not directory.exists(), 'rejected input wrote output'
            row['runtime'] = 'not_run (source rejection)'
            row['status'] = 'passed'
            return
        assert emitted['returncode'] == 0 and not emitted['stderr'], emitted
        assert parsed['status'] == 'ok', parsed
        row['runner'] = result = command([args.python, '-B', '-s', directory / 'program.py'])
        if 'panic' in expected:
            assert result['returncode'] == 101 and result['stderr'] == (
                'LANG_PANIC: ' + expected['panic'] + '\n'), result
        elif 'typed' in expected:
            assert result['returncode'] == 1 and result['stderr'] == (
                'LANG_TYPED_ERROR: ' + expected['typed'] + '\n'), result
        else:
            assert result['returncode'] == (expected.get('main_return', 0) or 0), result
            assert not result['stderr'], result
        assert not result['stdout'], result
        row['observe'] = result = command([args.python, '-B', '-s',
            Path(__file__).with_name('observe_cleanup.py'), directory, ident])
        assert result['returncode'] == 0 and not result['stderr'], result
        row['actual'] = actual = json.loads(result['stdout'])
        validate(ident, actual, expected)
        # Inspect source bindings, not merely main's return code.
        metadata = json.loads((directory / 'metadata.json').read_text())
        source_names = {s['generated']: s['source_name'] for s in metadata['symbols']}
        main_locals = {source_names[k]: v for k, v in actual['locals'].get('main', {}).items()}
        if 'log' in expected and 'panic' not in expected:
            assert main_locals['log'] == expected['log'], main_locals
        if ident == 'case-09':
            assert main_locals['found'] is True, main_locals
        if ident in ('case-04', 'case-06', 'case-08'):
            # Payload seen at the throw/call boundary equals the catch binding.
            caught = [v for k, v in actual['locals']['main'].items()
                      if source_names[k] == 'err']
            thrown = [e['value'] for e in actual['errors'] + actual['raised']]
            assert caught and all(v in thrown for v in caught), actual
        if ident == 'case-08':
            assert any(r == dict(function='apply', value=4) for r in actual['returns']), actual
            assert any(e['value'][1] == [-1] for e in actual['errors']), actual
        if ident == 'errors-original':
            returned = [r['value'] for r in actual['returns'] if r['function'] == 'read_value']
            assert returned == [7] and main_locals['cleaned'] == 2, actual
            writes = [c['args'][1] for c in actual['calls'] if c['function'] == 'read_value']
            assert writes == [0, 1], actual
            assert actual['counter_events'] == [1, 2], actual
        row['generated_sha256'] = {name: sha(directory / name)
                                   for name in ('program.py', 'langrt.py', 'verified.hir')}
        row['status'] = 'passed'

    try:
        report['python'] = version = command([args.python, '--version'])
        assert version['stdout'].strip() == 'Python 3.12.15' and version['returncode'] == 0, version
        published = json.loads((args.root / 'tests/cleanup/cases.json').read_text())
        assert published['case_count'] == len(published['cases']) == 19
        for case in published['cases']:
            expected = dict(case.get('runtime_expected', {}))
            if case['id'] == 'case-10':
                expected.update(panic='integer division by zero', log=expected['log_at_panic'])
            elif case['id'] == 'errors-original':
                # This sample's events are counter writes, observed separately.
                expected.pop('events')
            compile_run(case['id'], args.root / case['manifest'], expected, 'cases',
                        case['expected_static'].get('code'))
        for ident, expected in CASES.items():
            manifest = project(args.out / 'sources' / ident,
                               {'main.lang': 'package main;' + expected['source']})
            compile_run(ident, manifest, expected, 'extra')
        manifest = project(args.out / 'sources/cross-module', CROSS_MODULE)
        compile_run('cross-module', manifest, dict(main_return=7), 'extra')
        for ident, expected in UNSAFE.items():
            manifest = copied_project(
                args.out / 'sources' / ident,
                args.root / 'tests/native/cleanup' / ident)
            compile_run(ident, manifest, expected, 'extra')
        # Renumber the HIR type arena with unrelated declarations: the same
        # generic error must still have exactly the same logical key.
        source = CASES['unhandled-generic']['source']
        manifest = project(args.out / 'sources/shifted-type-ids', {'main.lang':
            'package main;struct Noise {x u64;y bool;}enum Earlier {A;B(value Noise);}' + source})
        compile_run('shifted-type-ids', manifest, dict(typed='app.Err[i32]'), 'extra')
        # True no-runner API path for the Panic/Cell observation.
        no_runner = args.out / 'no-runner'
        report['no_runner_emit'] = result = command([args.helper, '--project',
            args.root / 'tests/cleanup/fixtures/case-10/lang.toml',
            '--out-dir', no_runner, '--no-runner'])
        assert result['returncode'] == 0, result
        assert "__main__" not in (no_runner / 'program.py').read_text()
        report['no_runner_observe'] = result = command([args.python, '-B', '-s',
            Path(__file__).with_name('observe_cleanup.py'), no_runner, 'case-10'])
        assert result['returncode'] == 0 and not result['stderr'], result
        assert json.loads(result['stdout'])['probe_panic']['log'] == 12, result
        report['immutable_after_sha256'] = {str(p.relative_to(args.root)): sha(p) for p in immutable}
        assert before == report['immutable_after_sha256'], 'published input changed'
        report['status'] = 'passed'
    finally:
        report_file = args.out / 'cleanup-results.json'
        report_file.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], cases=len(report['cases']),
                         extra=len(report['extra']), report=str(report_file))))


if __name__ == '__main__':
    main()
