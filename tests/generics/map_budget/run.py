#!/usr/bin/env python3
"""Check ordinary Map combinations and finite generic expansion limits."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
INSTANCE_LIMIT = 4096


def project(out, name, source):
    path = out / name
    (path / 'src').mkdir(parents=True, exist_ok=True)
    (path / 'lang.toml').write_text(
        (Path(__file__).parent / 'combinations/lang.toml').read_text())
    (path / 'src/main.wz').write_text('package app;\n' + source)
    return path / 'lang.toml'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--python', type=Path, help='also build and run Python')
    parser.add_argument('--native', action='store_true', help='also build and run Native O0/O2')
    args = parser.parse_args()
    args.cli, args.out = args.cli.resolve(), args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(status='failed', runs=[])
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)

    def run(argv, cwd=None):
        result = subprocess.run(list(map(str, argv)), capture_output=True,
                                text=True, timeout=180, env=env, cwd=cwd)
        row = dict(argv=list(map(str, argv)), exit=result.returncode,
                   stdout=result.stdout, stderr=result.stderr)
        report['runs'].append(row)
        return row

    def check(manifest, backend, diagnostic=None, message=None):
        row = run([args.cli, 'check', '--project', manifest, '--backend', backend,
                   '--format', 'json'])
        response = json.loads(row['stdout'])
        assert not row['stderr'], row
        if diagnostic:
            assert row['exit'] == 1 and response['status'] == 'error', row
            assert len(response['diagnostics']) == 1, row
            item = response['diagnostics'][0]
            assert item['code'] == diagnostic and item.get('primary'), row
            assert message in item['message'], row
        else:
            assert row['exit'] == 0 and response['status'] == 'ok', row

    try:
        combinations = Path(__file__).parent / 'combinations/lang.toml'
        count_source = '\n'.join(
            f'func f{i}[T]() i32 {{ return 0; }}' for i in range(INSTANCE_LIMIT + 1))
        count_source += '\nfunc main() i32 {\n' + '\n'.join(
            f'f{i}[i32]();' for i in range(INSTANCE_LIMIT + 1)) + '\nreturn 0; }'
        count = project(args.out, 'instance-count', count_source)
        depth = {}
        for size in (48, 49):
            chain = '\n'.join(
                f'func f{i}[T]() i32 {{ return ' +
                (f'f{i + 1}[T](); }}' if i + 1 < size else '0; }')
                for i in range(size))
            depth[size] = project(args.out, f'depth-{size}',
                                  chain + '\nfunc main() i32 { return f0[i32](); }')
        for backend in ('native', 'python'):
            check(combinations, backend)
            check(depth[48], backend)
            check(depth[49], backend, 'E_INSTANTIATION_LIMIT', 'depth budget exceeded (48)')
            check(count, backend, 'E_INSTANTIATION_LIMIT',
                  f'count budget exceeded ({INSTANCE_LIMIT})')
            check(ROOT / 'tests/generics/fixtures/expanding-recursion/lang.toml',
                  backend, 'E_INSTANTIATION_LIMIT', 'depth budget exceeded (48)')
            check(ROOT / 'tests/ownership/fixtures/recursive-generic-growth/lang.toml',
                  backend, 'E_CAPABILITY_UNAVAILABLE', 'ownership member/type expansion budget')
        builds = [('native', str(opt)) for opt in (0, 2)] if args.native else []
        if args.python:
            builds.append(('python', None))
        for backend, opt in builds:
            output = args.out / (backend + (opt or ''))
            command = [args.cli, 'build', '--project', combinations, '--backend', backend,
                       '--out-dir', output, '--format', 'json']
            if opt:
                command += ['--opt', opt]
            row = run(command)
            assert row['exit'] == 0 and json.loads(row['stdout'])['status'] == 'ok', row
            command = [output / 'program'] if backend == 'native' else [
                args.python.resolve(), output / 'program.py']
            row = run(command, cwd=output)
            assert row['exit'] == 0 and not row['stdout'] and not row['stderr'], row
        report['status'] = 'passed'
    finally:
        (args.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], runs=len(report['runs']))))


if __name__ == '__main__':
    main()
