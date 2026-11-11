#!/usr/bin/env python3
"""Build and run freestanding thread/synchronization programs at O0 and O2."""
import argparse
import json
import re
from pathlib import Path

from run_os import command, digest, no_dynamic_libraries, successful


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('cli', 'root', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.cli, args.root, args.out = (p.resolve() for p in (args.cli, args.root, args.out))
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(status='failed', cli_sha256=digest(args.cli), runs=[])
    cases = dict(basic=0, shared=0, owned=0, detached=0,
                 **{'reentrant-lock': 101, 'unowned-unlock': 101, 'wrong-owner': 101,
                    'docs-thread': 0, 'docs-mutex': 0})
    blocks = re.findall(r'```wz\n(.*?)```', (args.root / 'docs/CONCURRENCY.md').read_text(), re.S)
    for name, block in zip(('docs-thread', 'docs-mutex'), blocks):
        assert block == (args.root / 'tests/native/concurrency' / name / 'src/main.wz').read_text()
    try:
        for name, expected in cases.items():
            source = args.root / 'tests/native/concurrency' / name
            for opt in (0, 2):
                out = args.out / name / f'O{opt}'
                out.mkdir(parents=True, exist_ok=True)
                row = dict(case=name, opt=opt, expected_exit=expected,
                           source_sha256=digest(source / 'src/main.wz'))
                report['runs'].append(row)
                row['build'] = built = command([
                    args.cli, 'build', '--project', source / 'lang.toml', '--backend', 'native',
                    '--opt', opt, '--out-dir', out, '--format', 'json'], args.root)
                successful(built)
                assert json.loads(built['stdout'])['status'] == 'ok', built
                program = out / 'program'
                row['elf'] = no_dynamic_libraries(program, out)
                row['program_sha256'] = digest(program)
                row['executions'] = []
                for _ in range(3 if name in ('shared', 'owned', 'detached') else 1):
                    result = command([program], out)
                    row['executions'].append(result)
                    assert result['status'] == 'exited' and result['exit'] == expected, result
                    assert result['stdout_hex'] == '', result
                    if expected == 101:
                        assert result['stderr'] == 'LANG_NATIVE_PANIC:v1:runtime-handle-misuse:0:0:0\n', result
                    else:
                        assert result['stderr_hex'] == '', result
        for backend in ('python',):
            result = command([args.cli, 'check', '--project',
                              args.root / 'tests/native/concurrency/basic/lang.toml',
                              '--backend', backend, '--format', 'json'], args.root)
            report[backend] = result
            assert result['exit'] != 0 and 'E_CAPABILITY_UNAVAILABLE' in result['stdout'], result
        report['status'] = 'passed'
    finally:
        (args.out / 'summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(f"passed: {len(report['runs'])} O0/O2 builds; "
          f"{sum(len(r['executions']) for r in report['runs'])} executions; no libc; Python rejected")


if __name__ == '__main__':
    main()
