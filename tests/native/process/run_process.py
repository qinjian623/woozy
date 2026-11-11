#!/usr/bin/env python3
"""Exercise process/signal/shared-memory Source APIs in standalone ELF files."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def command(argv, *, expected=0, env=None):
    result = subprocess.run([str(x) for x in argv], env=env, capture_output=True, timeout=60)
    row = dict(argv=[str(x) for x in argv], exit=result.returncode,
               stdout_hex=result.stdout.hex(), stderr=result.stderr.decode('utf-8', 'backslashreplace'))
    assert result.returncode == expected, row
    return row


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'root', 'out'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(status='failed', runs=[])
    try:
        for opt in (0, 2):
            out = args.out/f'process-o{opt}'
            row = dict(opt=opt)
            report['runs'].append(row)
            row['build'] = command([args.cli, 'build', '--project',
                args.root/'tests/native/process/lang.toml', '--backend', 'native',
                '--opt', opt, '--out-dir', out, '--format', 'json'])
            row['elf'] = command(['readelf', '-l', '-d', out/'program'])
            headers = bytes.fromhex(row['elf']['stdout_hex']).decode()
            assert 'INTERP' not in headers and '(NEEDED)' not in headers, row
            row['run'] = command([out/'program', 'probe😀'])
            assert not row['run']['stdout_hex'] and not row['run']['stderr'], row
            invalid = os.environ.copy()
            invalid['WOOZY_INVALID_UTF8'] = '\udcff'
            row['invalid_environment'] = command([out/'program', 'probe😀'], expected=90, env=invalid)
            row['invalid_argument'] = command([out/'program', '\udcff'], expected=90)
            misuse = args.out/f'misuse-o{opt}'
            row['misuse_build'] = command([args.cli, 'build', '--project',
                args.root/'tests/native/process/misuse/lang.toml', '--backend', 'native',
                '--opt', opt, '--out-dir', misuse, '--format', 'json'])
            row['misuse'] = command([misuse/'program'], expected=101)
            assert 'LANG_NATIVE_PANIC:v1:runtime-handle-misuse:' in row['misuse']['stderr'], row
        rejected = command([args.cli, 'build', '--project',args.root/'tests/native/process/lang.toml',
            '--backend', 'python', '--out-dir',args.out/'python-rejected','--format','json'],expected=1)
        report['python_rejected'] = rejected
        assert 'E_CAPABILITY_UNAVAILABLE' in bytes.fromhex(rejected['stdout_hex']).decode(), rejected
        assert not (args.out/'python-rejected/program.py').exists(), rejected
        report['status'] = 'passed'
    finally:
        path = args.out/'process-source-results.json'
        path.write_text(json.dumps(report,ensure_ascii=True,indent=2)+'\n')
    print(json.dumps(dict(status=report['status'],runs=len(report['runs']),report=str(path))))


if __name__ == '__main__':
    main()
