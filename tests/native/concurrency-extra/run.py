#!/usr/bin/env python3
"""Build/run CON01 pure-Wz rwlock+cancel cases at O0/O2 and the runtime ABI host test."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, cwd, timeout=30, env=None):
    process = subprocess.Popen([str(x) for x in argv], cwd=cwd,
                               stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               env=env)
    status = 'exited'
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
        status = 'timed_out'
    return dict(argv=[str(x) for x in argv], cwd=str(cwd), status=status,
                exit=process.returncode,
                stdout=stdout.decode('utf-8', 'backslashreplace'),
                stderr=stderr.decode('utf-8', 'backslashreplace'),
                stdout_hex=stdout.hex(), stderr_hex=stderr.hex())


def successful(row):
    assert row['status'] == 'exited' and row['exit'] == 0 and not row['stderr_hex'], row


def static_elf(program, cwd):
    headers = command(['readelf', '-l', program], cwd)
    dynamic = command(['readelf', '-d', program], cwd)
    successful(headers)
    successful(dynamic)
    assert 'INTERP' not in headers['stdout'], headers
    assert '(NEEDED)' not in dynamic['stdout'], dynamic
    return dict(program_headers=headers, dynamic_section=dynamic,
                has_interp=False, has_needed=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('cli', 'root', 'out', 'clang'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--lld', type=Path)
    args = parser.parse_args()
    args.cli, args.root, args.out = (
        p.resolve() for p in (args.cli, args.root, args.out))
    # Keep the clang++ spelling: resolving the symlink to clang-22 selects C mode.
    args.clang = args.clang.absolute()
    args.lld = (args.lld or (args.clang.parent / 'ld.lld')).absolute()
    args.out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    legacy_libxml = Path('/home/qin/Work/language-dev-20261103/toolchains/'
                         'libxml2-legacy-2.13.9/usr/lib')
    if legacy_libxml.is_dir():
        old = env.get('LD_LIBRARY_PATH')
        env['LD_LIBRARY_PATH'] = str(legacy_libxml) + (':' + old if old else '')
    cases = [
        ('rwlock', 0, 3),
        ('cancel', 0, 3),
        ('rwlock-misuse-read-recursive', 101, 1),
        ('rwlock-misuse-write-recursive', 101, 1),
        ('rwlock-misuse-upgrade', 101, 1),
        ('rwlock-misuse-downgrade', 101, 1),
        ('rwlock-misuse-unowned-read-unlock', 101, 1),
        ('rwlock-misuse-unowned-write-unlock', 101, 1),
        ('rwlock-misuse-wrong-thread-unlock', 101, 1),
    ]
    report = dict(status='failed', cli_sha256=digest(args.cli),
                  runs=[], runtime=[], static=[], not_run=[])
    try:
        for name, expected, repeats in cases:
            source = args.root / 'tests/native/concurrency-extra' / name
            for opt in (0, 2):
                out = args.out / name / f'O{opt}'
                out.mkdir(parents=True, exist_ok=True)
                row = dict(case=name, opt=opt, expected_exit=expected,
                           source_sha256={
                               str(p.relative_to(args.root)): digest(p)
                               for p in sorted(source.rglob('*')) if p.is_file()})
                report['runs'].append(row)
                row['build'] = built = command([
                    args.cli, 'build', '--project', source / 'lang.toml',
                    '--backend', 'native', '--opt', opt, '--out-dir', out,
                    '--format', 'json'], args.root, timeout=120, env=env)
                successful(built)
                assert json.loads(built['stdout'])['status'] == 'ok', built
                program = out / 'program'
                row['elf'] = static_elf(program, out)
                row['program_sha256'] = digest(program)
                row['executions'] = []
                for _ in range(repeats):
                    result = command([program], out, timeout=30, env=env)
                    row['executions'].append(result)
                    assert result['status'] == 'exited' and result['exit'] == expected, result
                    if expected == 101:
                        assert result['stderr'] == (
                            'LANG_NATIVE_PANIC:v1:runtime-handle-misuse:0:0:0\n'), result
                    else:
                        assert result['stdout_hex'] == '' and result['stderr_hex'] == '', result
                row['status'] = 'passed'

        runtime_source = (args.root / 'tests/native/concurrency-extra' /
                          'runtime_timeout_test.cpp')
        runtime_sources = [args.root / 'runtime/native' / name for name in
                           ('thread.cpp', 'sync.cpp', 'panic.cpp', 'alloc.cpp')]
        for opt in (0, 2):
            executable = args.out / 'runtime' / f'timeout-O{opt}'
            executable.parent.mkdir(parents=True, exist_ok=True)
            row = dict(opt=opt, source_sha256={
                str(p.relative_to(args.root)): digest(p)
                for p in [runtime_source] + runtime_sources})
            report['runtime'].append(row)
            row['compile'] = compiled = command([
                args.clang, '-std=c++20', f'-O{opt}', '-pthread',
                '-I', args.root / 'runtime/native', runtime_source,
                *runtime_sources, '-o', executable], args.root, timeout=180,
                env=env)
            successful(compiled)
            row['program_sha256'] = digest(executable)
            row['executions'] = []
            for _ in range(3):
                result = command([executable], args.out, timeout=120, env=env)
                row['executions'].append(result)
                assert result['status'] == 'exited' and result['exit'] == 0, result
                assert result['stderr_hex'] == '', result
            row['status'] = 'passed'

        static_source = (args.root / 'tests/native/concurrency-extra' /
                         'static_timeout_fixture.cpp')
        static_sources = [args.root / 'runtime/native' / name for name in
                          ('thread.cpp', 'sync.cpp', 'panic.cpp', 'alloc.cpp',
                           'start.cpp', 'freestanding.cpp')]
        for opt in (0, 2):
            executable = args.out / 'static' / f'timeout-O{opt}'
            executable.parent.mkdir(parents=True, exist_ok=True)
            row = dict(opt=opt, source_sha256={
                str(p.relative_to(args.root)): digest(p)
                for p in [static_source] + static_sources})
            report['static'].append(row)
            row['compile'] = compiled = command([
                args.clang, '--target=x86_64-unknown-linux-gnu', '-std=c++20',
                f'-O{opt}', '-ffreestanding', '-fno-builtin', '-fno-exceptions',
                '-fno-rtti', '-fno-stack-protector', '-fno-pic', '-fno-pie',
                '-nostdlib', '-static', f'--ld-path={args.lld}',
                '-Wl,-e,_start', static_source, *static_sources,
                '-o', executable], args.root, timeout=180, env=env)
            successful(compiled)
            row['elf'] = static_elf(executable, args.out)
            row['program_sha256'] = digest(executable)
            row['executions'] = []
            for _ in range(3):
                result = command([executable], args.out, timeout=30, env=env)
                row['executions'].append(result)
                assert result['status'] == 'exited' and result['exit'] == 0, result
                assert result['stdout_hex'] == '' and result['stderr_hex'] == '', result
            row['status'] = 'passed'

        # Root has not yet registered the prepared wait_for intrinsics or edited
        # std/thread and std/sync. Record this as not_run rather than claiming it.
        report['not_run'].append(dict(
            item='Wz thread.wait_for / sync.wait_for',
            reason='Root registry and source API integration pending; low-level ABI '
                   'is exercised by runtime_timeout_test.cpp',
            status='not_run'))
        report['status'] = 'passed'
    finally:
        (args.out / 'summary.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(f"passed: {len(report['runs'])} Wz O0/O2 builds, "
          f"{sum(len(r['executions']) for r in report['runs'])} executions, "
          f"{len(report['runtime'])} runtime O0/O2 builds, "
          f"{sum(len(r['executions']) for r in report['runtime'])} runtime runs, "
          f"{len(report['static'])} -nostdlib O0/O2 builds, "
          f"{sum(len(r['executions']) for r in report['static'])} static runs")


if __name__ == '__main__':
    main()
