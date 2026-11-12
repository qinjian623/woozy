#!/usr/bin/env python3
"""Run the same public Unicode API fixture on Native O0/O2 and Python."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('cli', 'root', 'out', 'python'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    legacy = args.root.parent / 'toolchains/libxml2-legacy-2.13.9/usr/lib'
    if legacy.is_dir():
        env['LD_LIBRARY_PATH'] = str(legacy) + ':' + env.get('LD_LIBRARY_PATH', '')
    report = {'status': 'failed', 'commands': []}

    def run(argv):
        result = subprocess.run([str(x) for x in argv], cwd=args.root, env=env,
                                capture_output=True, timeout=180)
        row = dict(argv=[str(x) for x in argv], exit=result.returncode,
                   stdout=result.stdout.decode('utf-8', 'backslashreplace'),
                   stderr=result.stderr.decode('utf-8', 'backslashreplace'))
        report['commands'].append(row)
        assert result.returncode == 0, row
        return row

    try:
        for backend, opt in (('native', 0), ('native', 2), ('python', None)):
            out = args.out / (backend + (str(opt) if opt is not None else ''))
            argv = [args.cli, 'build', '--project',
                    args.root / 'tests/native/unicode/source/project.toml',
                    '--backend', backend, '--out-dir', out, '--format', 'json']
            if opt is not None:
                argv.extend(['--opt', str(opt)])
            assert json.loads(run(argv)['stdout'])['status'] == 'ok'
            if backend == 'native':
                run([out / 'program'])
                elf = run(['readelf', '-l', '-d', out / 'program'])['stdout']
                assert 'INTERP' not in elf and 'NEEDED' not in elf, elf
            else:
                run([args.python, '-B', '-s', out / 'program.py'])
                runtime = (out / 'langrt.py').read_text()
                assert runtime.startswith(
                    (args.root / 'runtime/python/unicode18.py').read_text())
        report['status'] = 'passed'
    finally:
        (args.out / 'report.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(report['status'])


if __name__ == '__main__':
    main()
