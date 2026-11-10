#!/usr/bin/env python3
"""Build and execute the negative-constant Vec fixture through the real CLI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True


def command(argv, env, timeout=120):
    try:
        run = subprocess.run([str(x) for x in argv], capture_output=True,
                             text=True, env=env, timeout=timeout)
        return dict(argv=[str(x) for x in argv], status='exited',
                    exit=run.returncode, stdout=run.stdout, stderr=run.stderr)
    except subprocess.TimeoutExpired:
        return dict(argv=[str(x) for x in argv], status='timeout', exit=None,
                    stdout='', stderr='')
    except OSError as error:
        return dict(argv=[str(x) for x in argv], status='launch_failed',
                    exit=None, stdout='', stderr=str(error))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = args.root / 'tests/native/fixtures/vec-negative-constants/lang.toml'
    env = os.environ.copy()
    report = dict(operation_id='language-negative-float-vec-constant-fold-20261110',
                  status='failed', manifest=str(manifest), runs=[],
                  sources={str(path.relative_to(args.root)): digest(path)
                           for path in sorted(manifest.parent.rglob('*'))
                           if path.is_file()})
    try:
        for optimization in (0, 2):
            out = args.out / f'O{optimization}'
            row = dict(optimization=optimization, status='failed')
            report['runs'].append(row)
            row['check'] = command([args.cli, 'check', '--project', manifest,
                                    '--backend', 'native', '--format', 'json'],
                                   env)
            assert row['check']['exit'] == 0 and not row['check']['stderr'], row['check']
            row['build'] = command([args.cli, 'build', '--project', manifest,
                                    '--backend', 'native', '--opt', optimization,
                                    '--out-dir', out / 'cli', '--format', 'json'],
                                   env)
            assert row['build']['exit'] == 0 and not row['build']['stderr'], row['build']
            row['execute'] = command([out / 'cli/program'], env)
            assert row['execute']['exit'] == 0, row['execute']
            assert row['execute']['stdout'] == '' and row['execute']['stderr'] == '', row['execute']
            row['status'] = 'passed'
        report['status'] = 'passed'
        return 0
    except Exception as error:
        report['error'] = str(error)
        return 1
    finally:
        (args.out / 'vec-negative-results.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    raise SystemExit(main())
