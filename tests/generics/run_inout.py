#!/usr/bin/env python3
"""Execute the unconstrained generic inout fixture on both backends."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, timeout=60):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    completed = subprocess.run(argv, capture_output=True, text=True,
                               env=env, timeout=timeout)
    return dict(argv=argv, exit=completed.returncode,
                stdout=completed.stdout, stderr=completed.stderr)


def check_ok(row, what):
    assert row['exit'] == 0, (what, row)


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'helper', 'root', 'python', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    for name in ('cli', 'helper', 'root', 'python', 'out'):
        setattr(args, name, getattr(args, name).resolve())
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = args.root / 'tests/generics/inout/positive/lang.toml'
    sources = {str(path.relative_to(manifest.parent)): digest(path)
               for path in sorted(manifest.parent.rglob('*'))
               if path.is_file()}
    report = dict(task='T07 unconstrained generic inout', status='failed',
                  python=run([args.python, '--version']), runs=[])
    try:
        for backend in ('native', 'python'):
            check = run([args.cli, 'check', '--project', manifest,
                         '--backend', backend, '--format', 'json'])
            row = dict(backend=backend, check=check, source_sha256=sources,
                       status='failed')
            report['runs'].append(row)
            check_ok(check, 'check ' + backend)
            assert json.loads(check['stdout'])['status'] == 'ok', check
            row['status'] = 'passed'

        for opt in (0, 2):
            native = args.out / 'native' / f'O{opt}'
            build = run([args.cli, 'build', '--project', manifest,
                         '--backend', 'native', '--opt', opt,
                         '--out-dir', native, '--format', 'json'])
            row = dict(backend='native', opt=opt, build=build,
                       source_sha256=sources, status='failed')
            report['runs'].append(row)
            check_ok(build, 'native build')
            assert json.loads(build['stdout'])['status'] == 'ok', build
            row['execute'] = executed = run([native / 'program'])
            check_ok(executed, 'native execute')
            assert not executed['stdout'] and not executed['stderr'], executed
            row['program_sha256'] = digest(native / 'program')
            row['object_sha256'] = digest(native / 'program.o')
            row['status'] = 'passed'

        python = args.out / 'python'
        build = run([args.cli, 'build', '--project', manifest,
                     '--backend', 'python', '--out-dir', python,
                     '--format', 'json'])
        row = dict(backend='python', build=build, source_sha256=sources,
                   status='failed')
        report['runs'].append(row)
        check_ok(build, 'python build')
        assert json.loads(build['stdout'])['status'] == 'ok', build
        row['execute'] = executed = run([args.python, '-s',
                                         python / 'program.py'])
        check_ok(executed, 'python execute')
        assert not executed['stdout'] and not executed['stderr'], executed
        metadata = args.out / 'python-metadata'
        emit = run([args.helper, '--project', manifest, '--out-dir', metadata])
        row['emit'] = emit
        check_ok(emit, 'python helper')
        assert digest(python / 'program.py') == digest(metadata / 'program.py')
        assert digest(python / 'langrt.py') == digest(metadata / 'langrt.py')
        (python / 'metadata.json').write_bytes(
            (metadata / 'metadata.json').read_bytes())
        row['generated_sha256'] = {
            name: digest(python / name)
            for name in ('program.py', 'langrt.py', 'metadata.json')
        }
        row['status'] = 'passed'
        report['status'] = 'passed'
    finally:
        (args.out / 'results.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(
        status=report['status'], runs=len(report['runs']),
        failed=[row for row in report['runs'] if row['status'] != 'passed'],
        report=str(args.out / 'results.json'))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
