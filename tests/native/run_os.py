#!/usr/bin/env python3
"""Run std/os against Linux files and separately labelled write-result mocks."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, cwd):
    process = subprocess.Popen([str(x) for x in argv], cwd=cwd, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    status = 'exited'
    try:
        stdout, stderr = process.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
        status = 'timed_out'
    return dict(argv=[str(x) for x in argv], cwd=str(cwd), pid=process.pid,
                status=status, exit=process.returncode,
                stdout=stdout.decode('utf-8', 'backslashreplace'),
                stderr=stderr.decode('utf-8', 'backslashreplace'),
                stdout_hex=stdout.hex(), stderr_hex=stderr.hex())


def successful(row):
    assert row['status'] == 'exited' and row['exit'] == 0 and not row['stderr_hex'], row


def no_dynamic_libraries(program, cwd):
    headers = command(['readelf', '-l', program], cwd)
    dynamic = command(['readelf', '-d', program], cwd)
    successful(headers)
    successful(dynamic)
    assert 'INTERP' not in headers['stdout'] and '(NEEDED)' not in dynamic['stdout'], (headers, dynamic)
    return dict(program_headers=headers, dynamic_section=dynamic,
                has_interp=False, has_needed=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('cli', 'root', 'out', 'clang', 'runtime'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--lld', type=Path)
    args = parser.parse_args()
    for name in ('cli', 'root', 'out', 'clang', 'runtime'):
        setattr(args, name, getattr(args, name).resolve())
    # lld chooses its driver from argv[0], so keep the ld.lld symlink name.
    args.lld = (args.lld or args.clang.parent / 'ld.lld').absolute()
    args.out.mkdir(parents=True, exist_ok=True)
    source_root = args.root / 'tests/native/os'
    mock_source = source_root / 'mock_write.cpp'
    cases = [
        dict(id='hello', stdout=b'Hello, world!\n'),
        dict(id='file-io', stdout=b'A\0B\n', files={'captured.bin': b'A\0B\n'}, absent=['io.bin']),
        dict(id='directories', stdout=b'', absent=['made']),
        dict(id='getpid', stdout=None),
        dict(id='exit', stdout=b'before\n', exit=23),
        dict(id='errors', stdout=b'', files={'closed.bin': b'', 'counts.bin': b''}),
        dict(id='write-all-success', mock=0, stdout=b'A\0BC\n', stderr=b'text\0!\nMOCK-OK\n',
             sequences={'stdout': [-4, 2, 1, 2], 'stderr': [-4, 3, -4, 4]}),
        dict(id='write-all-no-progress', mock=1, stdout=b'A\0', stderr=b'texMOCK-OK\n',
             sequences={'stdout': [-4, 2, 0], 'stderr': [-4, 3, 0]}, completed={'stdout': 2, 'stderr': 3}),
        dict(id='write-all-would-block', mock=2, stdout=b'A\0B', stderr=b'teMOCK-OK\n',
             sequences={'stdout': [-4, 1, 2, -11], 'stderr': [2, -4, -11]}, completed={'stdout': 3, 'stderr': 2}),
    ]
    report = dict(status='failed', planned_executions=len(cases) * 2, runs=[], tools={},
                  mock_scope='Only fd1/fd2 results are simulated. Positive byte counts are forwarded to real Linux writes; all pointers, remaining lengths and bytes are checked.',
                  mock_exit_codes={'201': 'extra call', '202': 'wrong remaining length',
                                   '203': 'wrong data pointer', '204': 'wrong bytes',
                                   '205': 'positive forwarding mismatch', '206': 'incomplete sequence'},
                  simulated_results={'-4': 'EINTR', '0': 'no progress', '-11': 'EAGAIN'})
    row = None
    try:
        for name in ('clang', 'lld'):
            report['tools'][name] = result = command([getattr(args, name), '--version'], args.root)
            successful(result)
            assert '22.1.8' in result['stdout'], result
        for name in ('cli', 'runtime'):
            assert getattr(args, name).is_file(), getattr(args, name)
            report['tools'][name] = dict(path=str(getattr(args, name)), sha256=digest(getattr(args, name)))
        report['stdlib_sha256'] = digest(args.root / 'stdlib/os/os.wz')
        mock_objects = {}
        for case in cases:
            manifest = source_root / case['id'] / 'lang.toml'
            for opt in (0, 2):
                out = args.out / case['id'] / f'O{opt}'
                out.mkdir(parents=True, exist_ok=True)
                row = dict(id=case['id'], opt=opt, status='not_run',
                           expected_exit=case.get('exit', 0),
                           expected_stdout_hex=case['stdout'].hex() if case['stdout'] is not None else 'little-endian child PID',
                           expected_stderr_hex=case.get('stderr', b'').hex(),
                           source_sha256={str(p.relative_to(args.root)): digest(p)
                                          for p in sorted(manifest.parent.rglob('*')) if p.is_file()})
                report['runs'].append(row)
                row['check'] = checked = command([args.cli, 'check', '--project', manifest,
                                                  '--backend', 'native', '--format', 'json'], args.root)
                successful(checked)
                assert json.loads(checked['stdout'])['status'] == 'ok', checked
                row['status'] = 'checked'
                row['build'] = built = command([args.cli, 'build', '--project', manifest, '--backend', 'native',
                                                '--opt', opt, '--out-dir', out, '--format', 'json'], args.root)
                successful(built)
                assert json.loads(built['stdout'])['status'] == 'ok', built
                row['status'] = 'built'
                program = out / 'program'
                if 'mock' in case:
                    mode = case['mock']
                    if mode not in mock_objects:
                        mock_object = args.out / f'mock-write-{mode}.o'
                        compiled = command([args.clang, '-std=c++20', '-O2', '-ffreestanding', '-fno-builtin',
                                            '-fno-exceptions', '-fno-rtti', '-fno-stack-protector', '-fno-pic', '-fno-pie',
                                            f'-DMOCK_MODE={mode}', '-c', mock_source, '-o', mock_object], args.root)
                        row['mock_compile'] = compiled
                        successful(compiled)
                        mock_objects[mode] = mock_object
                    row['mock_sequences'] = case['sequences']
                    row['expected_completed'] = case.get('completed', {'stdout': 5, 'stderr': 7})
                    row['mock_sha256'] = digest(mock_source)
                    program = out / 'program-mock'
                    row['mock_link'] = linked = command([
                        args.clang, '--target=x86_64-unknown-linux-gnu', '-nostdlib', '-static',
                        '--ld-path=' + str(args.lld), '-Wl,-e,_start', '-Wl,--wrap=lang_os_write_v1',
                        out / 'program.o', mock_objects[mode], args.runtime, '-o', program], args.root)
                    successful(linked)
                    row['status'] = 'linked'
                row['elf'] = no_dynamic_libraries(program, args.root)
                row['program_sha256'] = digest(program)
                sandbox = Path(tempfile.mkdtemp(prefix='run-', dir=out))
                row['execute'] = ran = command([program], sandbox)
                expected_stdout = struct.pack('<I', ran['pid']) if case['id'] == 'getpid' else case['stdout']
                row['expected_stdout_hex'] = expected_stdout.hex()
                row['files'] = files = {}
                for name, expected in case.get('files', {}).items():
                    path = sandbox / name
                    actual = path.read_bytes() if path.is_file() else None
                    files[name] = dict(path=str(path), expected_hex=expected.hex(),
                                       actual_hex=actual.hex() if actual is not None else None,
                                       sha256=digest(path) if actual is not None else None)
                row['absent'] = {name: not (sandbox / name).exists() for name in case.get('absent', [])}
                assert ran['status'] == 'exited' and ran['exit'] == row['expected_exit'], ran
                assert ran['stdout_hex'] == expected_stdout.hex() and ran['stderr_hex'] == row['expected_stderr_hex'], ran
                assert all(item['actual_hex'] == item['expected_hex'] for item in files.values()), files
                assert all(row['absent'].values()), row['absent']
                row['status'] = 'passed'
        assert report['tools']['cli']['sha256'] == digest(args.cli), 'compiler changed during test'
        assert report['tools']['runtime']['sha256'] == digest(args.runtime), 'runtime changed during test'
        assert report['stdlib_sha256'] == digest(args.root / 'stdlib/os/os.wz'), 'stdlib changed during test'
        report['status'] = 'passed'
    except Exception as error:
        if row is not None:
            row['status'] = 'failed'
        report['error'] = str(error)
        raise
    finally:
        report['executions'] = sum('execute' in item for item in report['runs'])
        report['passed'] = sum(item['status'] == 'passed' for item in report['runs'])
        (args.out / 'results.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        print(json.dumps(dict(status=report['status'], passed=report['passed'],
                              planned=report['planned_executions'], executions=report['executions'],
                              report=str(args.out / 'results.json'))))


if __name__ == '__main__':
    main()
