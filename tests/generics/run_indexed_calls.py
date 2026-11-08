#!/usr/bin/env python3
"""Execute indexed function-value calls on Native O0/O2 and Python.

The cases exercise Vec element calls, struct Vec fields, nested and
parenthesized suffixes, constructor/function-result suffixes, existing
function-value modes and typed errors, evaluation order, temporary Vec cleanup,
runtime bounds Panic and static rejection controls.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


sys.dont_write_bytecode = True

POSITIVE = ('direct-runtime', 'field-modes-runtime',
            'evaluation-cleanup-runtime')
PANIC = 'panic-index'
REJECTS = {
    'reject-nonfunction': {'E_CALL_ARGUMENT'},
    'reject-bad-index': {'E_TYPE_MISMATCH'},
    'reject-field-bad-index': {'E_TYPE_MISMATCH'},
    'reject-generic-type': {'E_NAME_KIND'},
    'reject-generic-arity': {'E_TYPE_MISMATCH'},
    'reject-generic-bound': {'E_TYPE_MISMATCH'},
    'reject-method-type': {'E_NAME_KIND'},
}
EXPECTED_EVENTS = {'evaluation-cleanup-runtime': [1, 2, 3, 4, 5, 6, 7]}
EXPECTED_ALLOCATIONS = {'evaluation-cleanup-runtime': 3}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, timeout=90, library=None):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    if library and library.is_dir():
        current = env.get('LD_LIBRARY_PATH')
        env['LD_LIBRARY_PATH'] = str(library) + (':' + current if current else '')
    completed = subprocess.run(argv, capture_output=True, text=True,
                               env=env, timeout=timeout)
    return dict(argv=argv, exit=completed.returncode,
                stdout=completed.stdout, stderr=completed.stderr)


def check_ok(row, what):
    assert row['exit'] == 0, (what, row)


def native_observations(text):
    merged = []
    live = {}
    allocations = []
    for line in text.splitlines():
        fields = line.split(':')
        if fields[0] == 'OBS':
            merged.append(('obs', int(fields[2]), int(fields[3])))
        elif fields[0] == 'VEC':
            event = int(fields[1])
            identity = fields[2]
            merged.append(('vec', event, identity))
            if event == 1:
                assert identity not in live, line
                token = len(allocations)
                allocations.append(token)
                live[identity] = token
            elif event == 5:
                assert identity in live, line
                merged.append(('free', live.pop(identity)))
            else:
                assert identity in live, line
    assert not live, ('still live', live)
    return merged, allocations


def assert_native(case, merged, allocations):
    events = [item[1] for item in merged if item[0] == 'obs']
    if case in EXPECTED_EVENTS:
        assert events == EXPECTED_EVENTS[case], merged
    if case in EXPECTED_ALLOCATIONS:
        assert len(allocations) == EXPECTED_ALLOCATIONS[case], allocations
        # The first temporary Holder.fs Vec dies before the first catch; the
        # second temporary argument Vec dies before the second catch.
        first_free = merged.index(('free', 0))
        second_free = merged.index(('free', 2))
        assert first_free < merged.index(('obs', 4, 1234)), merged
        assert second_free < merged.index(('obs', 7, 1234567)), merged


def assert_python(case, data):
    assert data['main_return'] == 0, data
    assert not data['alive'], data
    assert data.get('frames_clean', 0) >= 1, data
    if case in EXPECTED_EVENTS:
        assert data['events'] == EXPECTED_EVENTS[case], data
    if case in EXPECTED_ALLOCATIONS:
        objects = [item['object'] for item in data['allocations']]
        assert len(objects) == EXPECTED_ALLOCATIONS[case], data
        assert sorted(data['frees']) == sorted(objects), data


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'probe', 'helper', 'root', 'clang', 'runtime',
                 'python', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    for name in ('cli', 'probe', 'helper', 'root', 'clang', 'runtime',
                 'python', 'out'):
        setattr(args, name, getattr(args, name).resolve())
    args.out.mkdir(parents=True, exist_ok=True)
    library = next((candidate for candidate in (
        args.clang.parent.parent / 'libxml2-legacy-2.13.9/usr/lib',
        args.clang.parent.parent.parent / 'libxml2-legacy-2.13.9/usr/lib')
        if candidate.is_dir()), None)
    observer = args.root / 'tests/python/observe_ownership.py'
    vec_observer = args.root / 'tests/native/vec_observer.cpp'
    report = dict(task='indexed function-value calls', status='failed',
                  python=run([args.python, '--version']), runs=[],
                  rejections=[], panics=[])
    try:
        for case in POSITIVE:
            manifest = args.root / f'tests/generics/indexed-calls/{case}/lang.toml'
            sources = {str(path.relative_to(manifest.parent)): digest(path)
                       for path in sorted(manifest.parent.rglob('*'))
                       if path.is_file()}
            for backend in ('native', 'python'):
                check = run([args.cli, 'check', '--project', manifest,
                             '--backend', backend, '--format', 'json'],
                            library=library)
                row = dict(case=case, backend=backend, check=check,
                           source_sha256=sources, status='failed')
                report['runs'].append(row)
                check_ok(check, 'check ' + backend)
                assert json.loads(check['stdout'])['status'] == 'ok', check
                row['status'] = 'passed'

            for opt in (0, 2):
                row = dict(case=case, opt=opt, source_sha256=sources,
                           status='failed')
                report['runs'].append(row)
                native = args.out / case / f'O{opt}'
                build = run([args.cli, 'build', '--project', manifest,
                             '--backend', 'native', '--opt', opt,
                             '--out-dir', native / 'cli', '--format', 'json'],
                            library=library)
                row['build'] = build
                check_ok(build, 'native build')
                assert json.loads(build['stdout'])['status'] == 'ok', build
                row['execute'] = executed = run([native / 'cli/program'],
                                                library=library)
                check_ok(executed, 'native execute')
                assert not executed['stdout'] and not executed['stderr'], executed
                row['probe'] = probed = run([args.probe, manifest,
                                             native / 'probe', opt],
                                            library=library)
                check_ok(probed, 'native probe')
                row['probe_checks'] = json.loads(probed['stdout'])
                observe = run([args.clang, f'-O{opt}', native / 'probe/observe.ll',
                               args.runtime, vec_observer,
                               '-o', native / 'observe'], library=library)
                row['observe_build'] = observe
                check_ok(observe, 'observe build')
                row['observe'] = observed = run([native / 'observe'],
                                                library=library)
                check_ok(observed, 'observe')
                merged, allocations = native_observations(observed['stderr'])
                assert_native(case, merged, allocations)
                row['native_allocations'] = len(allocations)
                row['native_order'] = merged
                row['program_sha256'] = digest(native / 'cli/program')
                row['object_sha256'] = digest(native / 'cli/program.o')
                row['status'] = 'passed'

            python = args.out / case / 'python'
            build = run([args.cli, 'build', '--project', manifest,
                         '--backend', 'python', '--out-dir', python,
                         '--format', 'json'], library=library)
            row = dict(case=case, python=True, source_sha256=sources,
                       build=build, status='failed')
            report['runs'].append(row)
            check_ok(build, 'python build')
            assert json.loads(build['stdout'])['status'] == 'ok', build
            row['execute'] = executed = run([args.python, '-s',
                                             python / 'program.py'],
                                            library=library)
            check_ok(executed, 'python execute')
            assert not executed['stdout'] and not executed['stderr'], executed
            metadata = args.out / case / 'python-metadata'
            emit = run([args.helper, '--project', manifest,
                        '--out-dir', metadata], library=library)
            row['emit'] = emit
            check_ok(emit, 'python helper')
            assert digest(python / 'program.py') == digest(metadata / 'program.py'), \
                'helper differs from CLI output'
            assert digest(python / 'langrt.py') == digest(metadata / 'langrt.py'), \
                'helper runtime differs from CLI output'
            observe = run([args.python, '-s', observer, metadata],
                          library=library)
            row['observe'] = observe
            check_ok(observe, 'python observer')
            data = json.loads(observe['stdout'])
            row['python_observation'] = data
            assert_python(case, data)
            row['program_sha256'] = digest(python / 'program.py')
            row['runtime_sha256'] = digest(python / 'langrt.py')
            row['status'] = 'passed'

        manifest = args.root / f'tests/generics/indexed-calls/{PANIC}/lang.toml'
        for backend in ('native', 'python'):
            check = run([args.cli, 'check', '--project', manifest,
                         '--backend', backend, '--format', 'json'],
                        library=library)
            check_ok(check, 'panic check ' + backend)
            assert json.loads(check['stdout'])['status'] == 'ok', check
        for opt in (0, 2):
            row = dict(case=PANIC, opt=opt, status='failed')
            report['panics'].append(row)
            native = args.out / PANIC / f'O{opt}'
            build = run([args.cli, 'build', '--project', manifest,
                         '--backend', 'native', '--opt', opt,
                         '--out-dir', native, '--format', 'json'],
                        library=library)
            row['build'] = build
            check_ok(build, 'panic native build')
            row['execute'] = executed = run([native / 'program'],
                                            library=library)
            assert executed['exit'] == 101 and not executed['stdout'], executed
            assert executed['stderr'].startswith('LANG_NATIVE_PANIC:v1:vec-index:'), \
                executed
            row['status'] = 'passed'
        python = args.out / PANIC / 'python'
        build = run([args.cli, 'build', '--project', manifest,
                     '--backend', 'python', '--out-dir', python,
                     '--format', 'json'], library=library)
        row = dict(case=PANIC, python=True, build=build, status='failed')
        report['panics'].append(row)
        check_ok(build, 'panic python build')
        row['execute'] = executed = run([args.python, '-s',
                                         python / 'program.py'],
                                        library=library)
        assert executed['exit'] == 101 and not executed['stdout'], executed
        assert executed['stderr'].startswith('LANG_PANIC:'), executed
        row['status'] = 'passed'

        for name, expected in REJECTS.items():
            manifest = args.root / f'tests/generics/indexed-calls/{name}/lang.toml'
            for backend in ('native', 'python'):
                check = run([args.cli, 'check', '--project', manifest,
                             '--backend', backend, '--format', 'json'],
                            library=library)
                response = json.loads(check['stdout'])
                codes = {item['code'] for item in response['diagnostics']}
                assert check['exit'] == 1 and not check['stderr'], check
                assert response['status'] == 'error' and codes == expected, check
                assert 'E_IR_INVALID' not in codes, check
                fresh = args.out / 'rejected' / name / backend / 'fresh'
                fresh.parent.mkdir(parents=True, exist_ok=True)
                shutil.rmtree(fresh, ignore_errors=True)
                assert not fresh.exists(), fresh
                argv = [args.cli, 'build', '--project', manifest,
                        '--backend', backend, '--out-dir', fresh,
                        '--format', 'json']
                if backend == 'native':
                    argv += ['--opt', '2']
                fresh_result = run(argv, library=library)
                assert fresh_result['exit'] == 1 and not fresh_result['stderr'], fresh_result
                assert {d['code'] for d in json.loads(fresh_result['stdout'])['diagnostics']} == expected, fresh_result
                assert not fresh.exists(), fresh
                stale = args.out / 'rejected' / name / backend / 'stale'
                shutil.rmtree(stale, ignore_errors=True)
                stale.mkdir(parents=True, exist_ok=True)
                for filename in ('program', 'program.o', 'program.py', 'langrt.py',
                                 'unrelated'):
                    (stale / filename).write_text('preserve-' + filename)
                before = {path.name: digest(path) for path in stale.iterdir()}
                stale_argv = list(argv)
                stale_argv[stale_argv.index('--out-dir') + 1] = stale
                stale_result = run(stale_argv, library=library)
                assert stale_result['exit'] == 1 and not stale_result['stderr'], stale_result
                assert {d['code'] for d in json.loads(stale_result['stdout'])['diagnostics']} == expected, stale_result
                assert before == {path.name: digest(path) for path in stale.iterdir()}, \
                    stale
                report['rejections'].append(dict(
                    case=name, backend=backend, check=check,
                    fresh=fresh_result, stale=stale_result,
                    fresh_absent=True, stale_preserved=True,
                    status='passed'))
        report['status'] = 'passed'
    finally:
        (args.out / 'report.json').write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps(dict(task=report['task'], status=report['status'],
                          runs=len(report['runs']),
                          panics=len(report['panics']),
                          rejections=len(report['rejections']),
                          python=report['python'])))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
