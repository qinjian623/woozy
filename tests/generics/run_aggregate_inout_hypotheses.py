#!/usr/bin/env python3
"""Execute the root's frozen aggregate-inout correction hypotheses.

These inputs are intentionally outside the repository.  The script does not
copy or rewrite them; it records their SHA-256 values and runs both backends.
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

CASES = (
    'same-nominal-finite',
    'distinct-nominal-control',
    'fixed-callback-same-self',
    'fixed-callback-other-self-control',
    'fixed-callback-numeric-self',
    'wrapped-trait-self',
    'wrapped-fixed-control',
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hashes(project):
    return {str(path.relative_to(project)): digest(path)
            for path in sorted(project.rglob('*')) if path.is_file()}


def run(argv, timeout=120, library=None):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    if library and library.is_dir():
        current = env.get('LD_LIBRARY_PATH')
        env['LD_LIBRARY_PATH'] = str(library) + (':' + current if current else '')
    completed = subprocess.run(argv, capture_output=True, text=True,
                               env=env, timeout=timeout)
    return dict(argv=argv, cwd=str(Path.cwd()),
                env={key: env.get(key) for key in
                     ('LD_LIBRARY_PATH', 'PYTHONDONTWRITEBYTECODE',
                      'PYTHONNOUSERSITE', 'LANG')},
                exit=completed.returncode, stdout=completed.stdout,
                stderr=completed.stderr)


def ok(row, what):
    assert row['exit'] == 0, (what, row)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--hypotheses', type=Path, required=True)
    parser.add_argument('--python', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.cli = args.cli.resolve()
    args.helper = args.helper.resolve()
    args.hypotheses = args.hypotheses.resolve()
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
        task='aggregate-inout-correction-hypotheses',
        operation_id='language-aggregate-inout-20261106',
        status='failed',
        hypotheses=str(args.hypotheses),
        tools={
            'cli': {'path': str(args.cli), 'sha256': digest(args.cli)},
            'helper': {'path': str(args.helper),
                       'sha256': digest(args.helper)},
            'python': {'path': str(args.python),
                       'version': run([args.python, '--version'])},
        },
        runs=[],
    )
    try:
        for case in CASES:
            project = args.hypotheses / case
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
                ok(check, 'check ' + backend)
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
                     '--out-dir', destination, '--format', 'json'],
                    library=library)
                ok(build, 'native build')
                assert json.loads(build['stdout'])['status'] == 'ok', build
                row['execute'] = executed = run([destination / 'program'],
                                                library=library)
                ok(executed, 'native execute')
                assert not executed['stdout'] and not executed['stderr'], executed
                row['program_sha256'] = digest(destination / 'program')
                row['object_sha256'] = digest(destination / 'program.o')
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
            ok(build, 'python build')
            assert json.loads(build['stdout'])['status'] == 'ok', build
            row['execute'] = executed = run(
                [args.python, '-s', destination / 'program.py'],
                library=library)
            ok(executed, 'python execute')
            assert not executed['stdout'] and not executed['stderr'], executed
            row['program_sha256'] = digest(destination / 'program.py')
            row['runtime_sha256'] = digest(destination / 'langrt.py')
            row['status'] = 'passed'
        report['status'] = 'passed'
    finally:
        (args.out / 'results.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], runs=len(report['runs']),
                          report=str(args.out / 'results.json'))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
