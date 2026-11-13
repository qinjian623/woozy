#!/usr/bin/env python3
"""Run whole-binding POD aggregate inout cases on both backends.

The fixtures intentionally use only named/active fields; no program compares
padding.  Positive programs encode every observation in their exit status so
the Native and Python runs can be compared without an external oracle.
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

POSITIVE = ('values', 'routes', 'correction-positive',
            'reject-correction/generic-dependent-callback',
            'reject-alias/field-place', 'reject-alias/index-place')
# These two former alias-rejection cases are now legal field/index inout places
# and are executed as positive controls instead of being checked as rejects.
PLACE_POSITIVE = {'reject-alias/field-place', 'reject-alias/index-place'}
# Approved owning inout: a concrete Function field inside the inout aggregate is
# an ordinary owning value now, so the former C5 rejection fixture executes as a
# positive program with the other aggregate cases.
OWNING_POSITIVE = ('fixed-callback-function-cycle',
                   'reject-owned/concrete-type-parameter',
                   'reject-owned/enum-owning-payload',
                   'reject-owned/function-root',
                   'reject-owned/nested-owning-field',
                   'reject-owned/string-root', 'reject-owned/vec-root',
                   'reject-correction/owning-wrapped-self')
C5_UNSUPPORTED = ()
# Only Unit stays closed among the former reject-owned roots; every owning root
# (Vec, string, function value, owning struct/enum, concrete generic) is a real
# whole inout value now.
OWNING_REJECT_NAMES = {'concrete-type-parameter', 'enum-owning-payload',
                       'function-root', 'nested-owning-field',
                       'string-root', 'vec-root'}
REJECT_CODES = {'E_CALL_ARGUMENT', 'E_CAPABILITY_UNAVAILABLE'}
# Owning inout no longer masks this fixture's real impl/trait signature
# mismatch: `callback func(inout Own)` against `callback func(inout U)`. The
# owning data itself is legal now, so the precise source error is expected.
CASE_REJECT_CODES = {
    'reject-correction/owning-dependent-callback': {'E_TYPE_MISMATCH'},
}
def reject_codes_for(case):
    return CASE_REJECT_CODES.get(case, REJECT_CODES)
FORBIDDEN_REJECT_CODES = {'E_IR_INVALID', 'E_INTERNAL_ERROR'}


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


def diagnostics(row):
    try:
        return json.loads(row['stdout']).get('diagnostics', [])
    except (json.JSONDecodeError, TypeError):
        return []


def codes(row):
    return [item.get('code') for item in diagnostics(row)]


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
        task='aggregate-whole-binding-inout',
        operation_id='language-aggregate-inout-20261106',
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
    cases = args.root / 'tests/generics/aggregate-inout'
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
            row['status'] = 'passed'

        for case in OWNING_POSITIVE:
            project = cases / case
            manifest = project / 'lang.toml'
            hashes = source_hashes(project)
            for backend in ('native', 'python'):
                row = dict(case=case, kind='owning-check', backend=backend,
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                row['check'] = check = run(
                    [args.cli, 'check', '--project', manifest,
                     '--backend', backend, '--format', 'json'],
                    library=library)
                require_ok(check, 'owning check ' + backend)
                assert json.loads(check['stdout'])['status'] == 'ok', check
                row['status'] = 'passed'
            for opt in (0, 2):
                row = dict(case=case, kind='owning-native', opt=opt,
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                destination = args.out / case / f'O{opt}'
                shutil.rmtree(destination, ignore_errors=True)
                row['build'] = build = run(
                    [args.cli, 'build', '--project', manifest,
                     '--backend', 'native', '--opt', opt,
                     '--out-dir', destination / 'cli', '--format', 'json'],
                    library=library)
                require_ok(build, 'owning native build')
                assert json.loads(build['stdout'])['status'] == 'ok', build
                row['execute'] = executed = run([destination / 'cli/program'],
                                                library=library)
                require_ok(executed, 'owning native execute')
                assert not executed['stdout'] and not executed['stderr'], executed
                row['status'] = 'passed'
            row = dict(case=case, kind='owning-python', source_sha256=hashes,
                       status='failed')
            report['runs'].append(row)
            destination = args.out / case / 'python'
            shutil.rmtree(destination, ignore_errors=True)
            row['build'] = build = run(
                [args.cli, 'build', '--project', manifest,
                 '--backend', 'python', '--out-dir', destination,
                 '--format', 'json'], library=library)
            require_ok(build, 'owning python build')
            assert json.loads(build['stdout'])['status'] == 'ok', build
            row['execute'] = executed = run(
                [args.python, '-s', destination / 'program.py'],
                library=library)
            require_ok(executed, 'owning python execute')
            assert not executed['stdout'] and not executed['stderr'], executed
            row['status'] = 'passed'

        for case in C5_UNSUPPORTED:
            project = cases / case
            manifest = project / 'lang.toml'
            hashes = source_hashes(project)
            for backend in ('native', 'python'):
                row = dict(case=case, kind='c5-unsupported-check',
                           backend=backend, source_sha256=hashes,
                           status='failed')
                report['runs'].append(row)
                row['check'] = check = run(
                    [args.cli, 'check', '--project', manifest,
                     '--backend', backend, '--format', 'json'],
                    library=library)
                assert check['exit'] != 0, check
                found = codes(check)
                assert 'E_CAPABILITY_UNAVAILABLE' in found, (case, backend, found)
                row['diagnostic_codes'] = found
                row['status'] = 'passed'

        for group in ('reject-owned', 'reject-alias', 'reject-correction'):
            for manifest in sorted((cases / group).glob('*/lang.toml')):
                case = f'{group}/{manifest.parent.name}'
                if case == 'reject-correction/generic-dependent-callback':
                    # The former template rejection is now a positive control;
                    # the owning-data rejections below keep this loop covered.
                    continue
                if case in PLACE_POSITIVE:
                    # Field/index inout is covered above as a positive case.
                    continue
                if group == 'reject-owned' and \
                        manifest.parent.name in OWNING_REJECT_NAMES:
                    # Approved owning inout: covered by OWNING_POSITIVE above.
                    continue
                if case == 'reject-correction/owning-wrapped-self':
                    # Approved owning inout: covered by OWNING_POSITIVE above.
                    continue
                project = manifest.parent
                hashes = source_hashes(project)
                for backend in ('native', 'python'):
                    row = dict(case=case, kind='reject-check',
                               backend=backend, source_sha256=hashes,
                               status='failed')
                    report['runs'].append(row)
                    row['check'] = check = run(
                        [args.cli, 'check', '--project', manifest,
                         '--backend', backend, '--format', 'json'],
                        library=library)
                    assert check['exit'] != 0, check
                    found = codes(check)
                    assert found and set(found) <= reject_codes_for(case), \
                        (case, backend, found, check)
                    assert not (set(found) & FORBIDDEN_REJECT_CODES), check
                    row['diagnostic_codes'] = found
                    row['status'] = 'passed'

                for backend in ('native', 'python'):
                    fresh = args.out / 'reject' / group / manifest.parent.name \
                        / (backend + '-new')
                    shutil.rmtree(fresh, ignore_errors=True)
                    row = dict(case=case, kind='reject-fresh',
                               backend=backend, source_sha256=hashes,
                               status='failed')
                    report['runs'].append(row)
                    argv = [args.cli, 'build', '--project', manifest,
                            '--backend', backend, '--out-dir', fresh,
                            '--format', 'json']
                    if backend == 'native':
                        argv += ['--opt', '0']
                    row['build'] = build = run(argv, library=library)
                    assert build['exit'] != 0, build
                    assert json.loads(build['stdout'])['status'] == 'error', build
                    found = codes(build)
                    assert found and set(found) <= reject_codes_for(case), \
                        (case, backend, found, build)
                    assert not (set(found) & FORBIDDEN_REJECT_CODES), build
                    outputs = ['program', 'program.o', 'program.py',
                               'langrt.py', 'metadata.json']
                    row['outputs'] = sorted(
                        str(path.relative_to(fresh))
                        for name in outputs
                        for path in [fresh / name] if path.exists())
                    assert not row['outputs'], row
                    row['diagnostic_codes'] = found
                    row['status'] = 'passed'

                    stale = args.out / 'reject' / group / manifest.parent.name \
                        / (backend + '-old')
                    stale.mkdir(parents=True, exist_ok=True)
                    names = (('program', 'program.o') if backend == 'native'
                             else ('program.py', 'langrt.py', 'metadata.json'))
                    sentinels = {name: b'previous output ' + name.encode()
                                 for name in names}
                    for name, content in sentinels.items():
                        (stale / name).write_bytes(content)
                    before = {name: digest(stale / name) for name in sentinels}
                    row = dict(case=case, kind='reject-stale',
                               backend=backend, source_sha256=hashes,
                               before_sha256=before, status='failed')
                    report['runs'].append(row)
                    argv = [args.cli, 'build', '--project', manifest,
                            '--backend', backend, '--out-dir', stale,
                            '--format', 'json']
                    if backend == 'native':
                        argv += ['--opt', '0']
                    row['build'] = build = run(argv, library=library)
                    assert build['exit'] != 0, build
                    assert json.loads(build['stdout'])['status'] == 'error', build
                    found = codes(build)
                    assert found and set(found) <= reject_codes_for(case), \
                        (case, backend, found, build)
                    assert not (set(found) & FORBIDDEN_REJECT_CODES), build
                    row['after_sha256'] = {name: digest(stale / name)
                                           for name in sentinels}
                    assert row['after_sha256'] == before, row
                    row['diagnostic_codes'] = found
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
