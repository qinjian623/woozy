#!/usr/bin/env python3
"""Execute the recursive-enum-pattern fixtures on both backends.

The probe/observer reuse the existing ownership helpers; this runner only adds
the T07 nested-pattern source cases and expected arm/cleanup sequences.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import shutil

sys.dont_write_bytecode = True

CASES = ('dispatch', 'control', 'owning', 'prefix', 'literal',
         'literal-scalars', 'literal-cross-package', 'literal-full-i8',
         'literal-control')
EXPECTED_EVENTS = {
    'dispatch': [1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 8, 9],
    'control': [1, 7, 2, 7, 3, 7, 6, 4, 0, 9, 4, 0, 9, 8],
    'owning': [3, 4, 5],
    'prefix': [1, 2, 3, 4],
    'literal': [1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 2],
    'literal-scalars': [],
    'literal-cross-package': [],
    'literal-full-i8': [],
    'literal-control': [1, 2, 3, 1, 4, 5, 6, 7, 8, 7, 9, 7, 9, 7, 1],
}
EXPECTED_ALLOCATIONS = {'dispatch': 0, 'control': 0, 'owning': 2, 'prefix': 2,
                        'literal': 3, 'literal-scalars': 0,
                        'literal-cross-package': 0, 'literal-full-i8': 0,
                        'literal-control': 6}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, timeout=60, library=None):
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
    seen = []
    allocations = []
    live = {}
    for line in text.splitlines():
        fields = line.split(':')
        if fields[0] == 'OBS':
            seen.append(('obs', int(fields[2]), int(fields[3])))
        elif fields[0] == 'VEC':
            event = int(fields[1])
            identity = fields[2]
            if event == 1:
                assert identity not in live, line
                token = len(allocations)
                allocations.append(token)
                live[identity] = token
            elif event == 5:
                assert identity in live, line
                seen.append(('free', live.pop(identity)))
            else:
                assert identity in live, line
    assert not live, ('still live', live)
    return seen, allocations


def assert_native(case, observations, allocations):
    events = [item[1] for item in observations if item[0] == 'obs']
    assert events == EXPECTED_EVENTS[case], observations
    assert len(allocations) == EXPECTED_ALLOCATIONS[case], allocations
    frees = [item[1] for item in observations if item[0] == 'free']
    assert sorted(frees) == sorted(allocations), observations
    assert len(set(frees)) == len(frees), observations


def assert_python(case, data):
    assert data['main_return'] == 0, data
    assert not data['alive'], data
    assert data.get('frames_clean', 0) >= 1, data
    assert data['events'] == EXPECTED_EVENTS[case], data
    objects = [item['object'] for item in data['allocations']]
    assert len(objects) == EXPECTED_ALLOCATIONS[case], data
    assert sorted(data['frees']) == sorted(objects), data
    assert len(set(data['frees'])) == len(data['frees']), data


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'probe', 'helper', 'root', 'clang', 'runtime',
                 'python', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--only', action='append', choices=CASES)
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
    report = dict(task='T07 recursive enum patterns', status='failed',
                  python=run([args.python, '--version']), runs=[],
                  rejections=[])
    cases = tuple(args.only) if args.only else CASES
    try:
        rejected = args.root / 'tests/generics/nested-patterns/reject-nonexhaustive'
        for backend in ('native', 'python'):
            fresh = args.out / 'rejected' / backend / 'fresh'
            shutil.rmtree(fresh, ignore_errors=True)
            fresh_result = run([args.cli, 'build', '--project', rejected / 'lang.toml',
                                '--backend', backend, '--out-dir', fresh,
                                '--format', 'json'], library=library)
            parsed = json.loads(fresh_result['stdout'])
            assert fresh_result['exit'] == 1 and not fresh_result['stderr'], fresh_result
            assert parsed['status'] == 'error', fresh_result
            assert any(d['code'] == 'E_TYPE_MISMATCH' for d in parsed['diagnostics']), fresh_result
            assert not fresh.exists(), fresh
            existing = args.out / 'rejected' / backend / 'existing'
            existing.mkdir(parents=True, exist_ok=True)
            for name in ('program', 'program.o', 'program.py', 'langrt.py', 'unrelated'):
                (existing / name).write_text('preserve-' + name)
            before = {p.name: digest(p) for p in existing.iterdir()}
            existing_result = run([args.cli, 'build', '--project', rejected / 'lang.toml',
                                   '--backend', backend, '--out-dir', existing,
                                   '--format', 'json'], library=library)
            assert existing_result['exit'] == 1 and not existing_result['stderr'], existing_result
            assert before == {p.name: digest(p) for p in existing.iterdir()}, existing
            report['rejections'].append(dict(backend=backend,
                                             fresh=fresh_result,
                                             existing=existing_result,
                                             existing_preserved=True))
        for case in cases:
            manifest = args.root / f'tests/generics/nested-patterns/{case}/lang.toml'
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
                ordered, allocations = native_observations(observed['stderr'])
                assert_native(case, ordered, allocations)
                row['native_allocations'] = len(allocations)
                row['native_order'] = ordered
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
        report['status'] = 'passed'
    finally:
        (args.out / 'report.json').write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps(dict(task=report['task'], status=report['status'],
                          runs=len(report['runs']),
                          rejections=len(report['rejections']),
                          python=report['python'])))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
