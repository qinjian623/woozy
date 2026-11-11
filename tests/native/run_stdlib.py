#!/usr/bin/env python3
"""Run source-level libraries, including short-write recovery and real sockets."""
import argparse
import json
from pathlib import Path
import subprocess


def command(argv, data=None):
    result = subprocess.run([str(x) for x in argv], input=b"" if data is None else data, capture_output=True, timeout=60)
    row = dict(argv=[str(x) for x in argv], exit=result.returncode,
               stdout_hex=result.stdout.hex(), stderr=result.stderr.decode('utf-8', 'backslashreplace'))
    assert result.returncode == 0, dict(argv=row['argv'], exit=row['exit'], stdout=result.stdout.decode('utf-8','backslashreplace'), stderr=row['stderr'])
    return row


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'root', 'out', 'clang', 'lld', 'runtime', 'python'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(status='failed', runs=[])
    try:
        for case in ('stdlib', 'stdlib-pure', 'stdlib-writer-error', 'stdlib-stdin', 'json-thread'):
            for backend, opt in [('native', 0), ('native', 2)] + ([('python', None)] if case == 'stdlib-pure' else []):
                out = args.out/f'{case}-{backend}-{opt}'
                argv = [args.cli, 'build', '--project', args.root/f'tests/native/{case}/lang.toml',
                        '--backend', backend, '--out-dir', out, '--format', 'json']
                if opt is not None:
                    argv += ['--opt', opt]
                row = dict(case=case, backend=backend, opt=opt, build=command(argv))
                report['runs'].append(row)
                program = out/'program'
                if case == 'stdlib-writer-error':
                    mock = out/'mock.o'
                    row['mock_compile'] = command([args.clang, '-std=c++20', '-O2', '-ffreestanding',
                        '-fno-exceptions', '-fno-rtti', '-fno-stack-protector', '-c',
                        args.root/'tests/native/stdlib-writer-error/mock_write.cpp', '-o', mock])
                    program = out/'program-mock'
                    row['mock_link'] = command([args.clang, '-nostdlib', '-static', f'--ld-path={args.lld}',
                        '-Wl,--wrap=lang_os_write_v1', out/'program.o', mock, args.runtime, '-o', program])
                if backend == 'native':
                    row['elf'] = headers = command(['readelf', '-l', '-d', program])
                    text = bytes.fromhex(headers['stdout_hex']).decode()
                    assert 'INTERP' not in text and '(NEEDED)' not in text, row
                data = '中😀\r\n\nlast\0end'.encode() if case == 'stdlib-stdin' else None
                row['run'] = executed = command([program] if backend == 'native'
                    else [args.python, '-B', '-s', out/'program.py'], data)
                expected = {'stdlib': b'stdlib: ok\n', 'stdlib-pure': b'',
                    'stdlib-writer-error': b'ABCDEFG', 'json-thread': b'',
                    'stdlib-stdin': '中😀\n\nlast\0end\n-8\n-16\n-32\n-64\n-1\n8\n16\n32\n64\n1\n1.5\n-0\ntrue\n'.encode()}[case]
                assert executed['stdout_hex'] == expected.hex() and not executed['stderr'], row
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        path = args.out/'stdlib-results.json'
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
    print(json.dumps(dict(status=report['status'], runs=len(report['runs']), report=str(path))))


if __name__ == '__main__':
    main()
