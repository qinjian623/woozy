#!/usr/bin/env python3
"""Run complete generic-trait applications on both backends.

Each positive program encodes its observations in its exit status. Negative
projects distinguish source errors from unsupported combinations.
A rejected build must leave a fresh directory empty and must
not overwrite old output bytes.
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

POSITIVE = ('dual-phantom', 'joint-nonoverlap', 'cross-package',
            'phantom-impl-key', 'forward-nominal-runtime',
            'alpha-bound-runtime', 'combined-self-bound-runtime',
            'combined-self-sibling-bound-runtime', 'own-alpha-runtime',
            'own-trait-bound-runtime', 'own-phantom-key-runtime',
            'own-ordinary-errors', 'own-cross-package-lambda',
            'modes-before-move-three-entries',
            'modes-before-pod-inout-writeback',
            'modes-before-static-own-named-self',
            'modes-combinations', 'modes-lifetime',
            'ordinary-receiver-identity')
# Declaration-only controls: both backends must accept, but their main does not
# exercise the bound, so they are not reported as runtime coverage.
# Old reject-* sources keep their bytes. Their declarations are now legal;
# the modes-* projects exercise the newly accepted calls.
STATIC_POSITIVE = ('prototype-bound-self', 'bound-pod-callback-control',
                   'reject-own-generic', 'hidden-fixed-callback-control',
                   'reject-own-generic-move', 'reject-own-generic-inout',
                   'reject-move', 'reject-inout')
OBSERVED = ('modes-combinations', 'modes-lifetime')
EXPECTED_EVENTS = {
    'modes-combinations': [],
    'modes-lifetime': [2, 1, 4, 3, 3, 7, 9, 7, 9, 3],
}
SOURCE_ERROR = {
    'ordinary-receiver-reject-concrete': {'E_CALL_ARGUMENT'},
    'ordinary-receiver-reject-constrained': {'E_CALL_ARGUMENT'},
    'reject-ambiguous': {'E_TYPE_MISMATCH'},
    'reject-bound-order': {'E_TYPE_MISMATCH'},
    'reject-arity': {'E_TYPE_MISMATCH'},
    'reject-overlap': {'E_TYPE_MISMATCH'},
    'reject-result-context': {'E_TYPE_MISMATCH'},
    'reject-alpha-bound-mismatch': {'E_TYPE_MISMATCH'},
    'reject-self-bound-mismatch': {'E_TYPE_MISMATCH'},
    'reject-forward-arity': {'E_TYPE_MISMATCH'},
    'modes-reject-move-marker': {'E_CALL_ARGUMENT'},
    'modes-reject-after-move': {'E_OWNERSHIP_USE_AFTER_MOVE'},
    'modes-reject-inout-alias': {'E_CALL_ARGUMENT'},
}
UNSUPPORTED = {
    'reject-bound-owning-callback': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-own-dependent-callback': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-hidden-own-callback': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-hidden-own-callback-return': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-hidden-own-callback-enum': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-hidden-own-callback-throws': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-hidden-own-callback-function-return': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-hidden-trait-callback': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-hidden-trait-callback-legacy': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-hidden-self-callback': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-hidden-self-callback-legacy': {'E_CAPABILITY_UNAVAILABLE'},
    'modes-reject-inout-owning': {'E_CAPABILITY_UNAVAILABLE'},
    'modes-reject-actual-self-move': {'E_CAPABILITY_UNAVAILABLE'},
    'modes-reject-actual-self-inout': {'E_CAPABILITY_UNAVAILABLE'},
}
FORBIDDEN = {'E_IR_INVALID', 'E_INTERNAL_ERROR'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, timeout=120, library=None, cwd=None):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    if library and library.is_dir():
        current = env.get('LD_LIBRARY_PATH')
        env['LD_LIBRARY_PATH'] = str(library) + (':' + current if current else '')
    completed = subprocess.run(argv, capture_output=True, text=True,
                               env=env, timeout=timeout, cwd=cwd)
    return dict(argv=argv, cwd=str(cwd or Path.cwd()),
                env={key: env.get(key) for key in
                     ('LD_LIBRARY_PATH', 'PYTHONDONTWRITEBYTECODE',
                      'PYTHONNOUSERSITE', 'LANG')},
                exit=completed.returncode,
                stdout=completed.stdout, stderr=completed.stderr)


def require_ok(row, what):
    assert row['exit'] == 0, (what, row)


def source_hashes(project):
    return {str(path.relative_to(project)): digest(path)
            for path in sorted(project.rglob('*')) if path.is_file()}


def codes(row):
    try:
        return [item.get('code') for item in
                json.loads(row['stdout']).get('diagnostics', [])]
    except (json.JSONDecodeError, TypeError):
        return []


def assert_reject(build, expected, what):
    assert build['exit'] != 0, (what, build)
    assert json.loads(build['stdout'])['status'] == 'error', (what, build)
    found = codes(build)
    assert found and set(found) <= expected, (what, found, build)
    assert not (set(found) & FORBIDDEN), (what, found, build)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--python', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.cli = args.cli.resolve()
    args.helper = args.helper.resolve()
    args.root = args.root.resolve()
    args.python = args.python.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    library = next((candidate for candidate in (
        args.cli.parent.parent / 'toolchains/libxml2-legacy-2.13.9/usr/lib',
        args.cli.parent.parent.parent / 'toolchains/libxml2-legacy-2.13.9/usr/lib',
    ) if candidate.is_dir()), None)
    if library is None:
        library = next((Path(entry) for entry in
                        os.environ.get('LD_LIBRARY_PATH', '').split(':')
                        if 'libxml2-legacy' in entry and Path(entry).is_dir()),
                       None)
    report = dict(
        task='generic-trait-applications-and-method-modes',
        operation_id='language-generic-trait-method-modes-20261107',
        status='failed',
        tools={
            'cli': {'path': str(args.cli), 'sha256': digest(args.cli)},
            'helper': {'path': str(args.helper),
                       'sha256': digest(args.helper)},
            'python': {'path': str(args.python),
                       'version': run([args.python, '--version'])},
        },
        sanitizer_scope='generated Native programs are not sanitizer-instrumented',
        runs=[],
    )
    cases = args.root / 'tests/generics/generic-traits'
    try:
        for case in POSITIVE:
            project = cases / case
            manifest = project / 'lang.toml'
            hashes = source_hashes(project)
            for backend in ('native', 'python'):
                row = dict(case=case, kind='check', backend=backend,
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                row['check'] = check = run(
                    [args.cli, 'check', '--project', manifest,
                     '--backend', backend, '--format', 'json'],
                    library=library)
                require_ok(check, 'check ' + backend)
                assert json.loads(check['stdout'])['status'] == 'ok', check
                row['status'] = 'passed'

            for opt in (0, 2):
                row = dict(case=case, kind='native', opt=opt,
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                destination = args.out / case / f'O{opt}'
                shutil.rmtree(destination, ignore_errors=True)
                row['build'] = build = run(
                    [args.cli, 'build', '--project', manifest,
                     '--backend', 'native', '--opt', opt,
                     '--out-dir', destination / 'cli', '--format', 'json'],
                    library=library)
                require_ok(build, 'native build')
                assert json.loads(build['stdout'])['status'] == 'ok', build
                row['execute'] = executed = run([destination / 'cli/program'],
                                                library=library)
                require_ok(executed, 'native execute')
                assert not executed['stdout'] and not executed['stderr'], executed
                row['program_sha256'] = digest(destination / 'cli/program')
                row['object_sha256'] = digest(destination / 'cli/program.o')
                row['status'] = 'passed'

            row = dict(case=case, kind='python', source_sha256=hashes,
                       status='failed')
            report['runs'].append(row)
            destination = args.out / case / 'python'
            shutil.rmtree(destination, ignore_errors=True)
            row['build'] = build = run(
                [args.cli, 'build', '--project', manifest,
                 '--backend', 'python', '--out-dir', destination,
                 '--format', 'json'], library=library)
            require_ok(build, 'python build')
            assert json.loads(build['stdout'])['status'] == 'ok', build
            row['execute'] = executed = run(
                [args.python, '-s', destination / 'program.py'],
                library=library)
            require_ok(executed, 'python execute')
            assert not executed['stdout'] and not executed['stderr'], executed
            metadata = args.out / case / 'python-metadata'
            shutil.rmtree(metadata, ignore_errors=True)
            row['emit'] = emit = run(
                [args.helper, '--project', manifest,
                 '--out-dir', metadata], library=library)
            require_ok(emit, 'python helper')
            row['generated_sha256'] = {
                name: digest(destination / name)
                for name in ('program.py', 'langrt.py')}
            for name in ('program.py', 'langrt.py'):
                assert row['generated_sha256'][name] == digest(metadata / name), \
                    'helper differs from CLI output: ' + name
            row['helper_metadata_sha256'] = digest(metadata / 'metadata.json')
            if case in OBSERVED:
                (destination / 'metadata.json').write_bytes(
                    (metadata / 'metadata.json').read_bytes())
                row['observe'] = observed = run(
                    [args.python, '-s',
                     args.root / 'tests/python/observe_ownership.py', destination],
                    library=library)
                require_ok(observed, 'python ownership observer')
                data = json.loads(observed['stdout'])
                assert data['main_return'] == 0 and not data['alive'], data
                assert data['allocations'] and data['frames_clean'] > 0, data
                assert data['events'] == EXPECTED_EVENTS[case], data
                assert sorted(data['frees']) == sorted(
                    item['object'] for item in data['allocations']), data
                assert len(set(data['frees'])) == len(data['frees']), data
                if case == 'modes-lifetime':
                    snapshots = data['snapshots']
                    assert snapshots['alias']['items'] == [3], data
                    assert snapshots['owned'] == snapshots['copied'], data
                    assert snapshots['packet'] == snapshots['moved_packet'], data
                    assert snapshots['pair'] == [3, False], data
                    assert snapshots['log'] == 2143379793, data
                    assert {'fail', 'pass', 'write'} <= {
                        item['function'] for item in data['errors']}, data
                row['python_observation'] = data
            row['status'] = 'passed'

        for case in STATIC_POSITIVE:
            project = cases / case
            manifest = project / 'lang.toml'
            hashes = source_hashes(project)
            for backend in ('native', 'python'):
                row = dict(case=case, kind='static-check', backend=backend,
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                row['check'] = check = run(
                    [args.cli, 'check', '--project', manifest,
                     '--backend', backend, '--format', 'json'],
                    library=library)
                require_ok(check, 'static check ' + backend)
                assert json.loads(check['stdout'])['status'] == 'ok', check
                row['status'] = 'passed'

        for case, expected in SOURCE_ERROR.items():
            project = cases / case
            manifest = project / 'lang.toml'
            hashes = source_hashes(project)
            for backend in ('native', 'python'):
                row = dict(case=case, kind='reject-check', backend=backend,
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                row['check'] = check = run(
                    [args.cli, 'check', '--project', manifest,
                     '--backend', backend, '--format', 'json'],
                    library=library)
                assert_reject(check, expected, (case, backend))
                row['diagnostic_codes'] = codes(check)
                row['status'] = 'passed'

        for case, expected in UNSUPPORTED.items():
            project = cases / case
            manifest = project / 'lang.toml'
            hashes = source_hashes(project)
            for backend in ('native', 'python'):
                row = dict(case=case, kind='unsupported-check',
                           backend=backend, source_sha256=hashes,
                           status='failed')
                report['runs'].append(row)
                row['check'] = check = run(
                    [args.cli, 'check', '--project', manifest,
                     '--backend', backend, '--format', 'json'],
                    library=library)
                assert_reject(check, expected, (case, backend))
                row['diagnostic_codes'] = codes(check)
                row['status'] = 'passed'

        for case, expected in {**SOURCE_ERROR, **UNSUPPORTED}.items():
            project = cases / case
            manifest = project / 'lang.toml'
            hashes = source_hashes(project)
            for backend in ('native', 'python'):
                fresh = args.out / 'reject' / case / (backend + '-new')
                shutil.rmtree(fresh, ignore_errors=True)
                row = dict(case=case, kind='reject-fresh', backend=backend,
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                argv = [args.cli, 'build', '--project', manifest,
                        '--backend', backend, '--out-dir', fresh,
                        '--format', 'json']
                if backend == 'native':
                    argv += ['--opt', '0']
                row['build'] = build = run(argv, library=library)
                assert_reject(build, expected, (case, backend, 'fresh'))
                row['outputs'] = sorted(
                    str(path.relative_to(fresh))
                    for name in ('program', 'program.o', 'program.py',
                                 'langrt.py', 'metadata.json')
                    for path in [fresh / name] if path.exists())
                assert not row['outputs'], row
                row['diagnostic_codes'] = codes(build)
                row['status'] = 'passed'

                stale = args.out / 'reject' / case / (backend + '-old')
                stale.mkdir(parents=True, exist_ok=True)
                names = (('program', 'program.o') if backend == 'native'
                         else ('program.py', 'langrt.py', 'metadata.json'))
                sentinels = {name: b'previous output ' + name.encode()
                             for name in names}
                for name, content in sentinels.items():
                    (stale / name).write_bytes(content)
                before = {name: digest(stale / name) for name in sentinels}
                row = dict(case=case, kind='reject-stale', backend=backend,
                           source_sha256=hashes, before_sha256=before,
                           status='failed')
                report['runs'].append(row)
                argv = [args.cli, 'build', '--project', manifest,
                        '--backend', backend, '--out-dir', stale,
                        '--format', 'json']
                if backend == 'native':
                    argv += ['--opt', '0']
                row['build'] = build = run(argv, library=library)
                assert_reject(build, expected, (case, backend, 'stale'))
                row['after_sha256'] = {name: digest(stale / name)
                                       for name in sentinels}
                assert row['after_sha256'] == before, row
                row['diagnostic_codes'] = codes(build)
                row['status'] = 'passed'
        report['status'] = 'passed'
    finally:
        (args.out / 'results.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(
        status=report['status'], runs=len(report['runs']),
        failed=[(row['case'], row.get('kind'), row.get('backend'))
                for row in report['runs'] if row['status'] != 'passed'],
        report=str(args.out / 'results.json'))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
