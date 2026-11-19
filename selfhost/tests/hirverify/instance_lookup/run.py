#!/usr/bin/env python3
"""Check early instance lookup using real source and the existing HIR oracle.

All generated sources, compiler output and raw command logs stay in --work-dir.
This is a focused verifier test, not a corpus or bootstrap run.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import time

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('hirverify_runner', HERE.parent / 'run.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--woozyc', required=True, type=Path)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--toolchain-root', required=True, type=Path)
    parser.add_argument('--work-dir', required=True, type=Path)
    parser.add_argument('--timeout', default=1800, type=float)
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=False)
    commands = []
    report = {'status': 'running', 'commands': commands}
    report_path = args.work_dir / 'report.json'

    def sha(path):
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()

    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')

    def run(label, argv):
        start = time.monotonic()
        record = {'label': label, 'argv': list(map(str, argv))}
        commands.append(record)
        save()
        env = base.environment(args.toolchain_root, args.work_dir)
        try:
            result = base.command(record['argv'], label, args.work_dir, args.timeout, env)
        finally:
            detail = args.work_dir / (label + '.json')
            if detail.is_file():
                record.update(json.loads(detail.read_text()))
            record['seconds'] = time.monotonic() - start
            save()
        return result

    try:
        project = args.work_dir / 'project'
        base.stage_project(project, HERE / 'driver.wz', HERE.parent / 'exit_identity/lang.toml', generated=True)
        report['compiler_before'] = sha(args.woozyc)
        oracle = args.work_dir / 'oracle'
        run('oracle-build', ['g++', '-std=c++20', '-O0', '-g0', '-I', base.ROOT / 'include',
            HERE.parent / 'exit_identity/oracle.cpp', '-Wl,--start-group',
            *[args.build_dir / x for x in (*base.ORACLE_LIBS, 'liblang_driver_process.a')], '-Wl,--end-group',
            '-ljson-c', '-ltomlplusplus', '-lgmpxx', '-lgmp', '-lm', '-o', oracle])
        run('oracle-generate', [oracle, HERE / 'fixture.wz', project / 'src/generated.wz'])
        files = [p for p in (project / 'src').rglob('*') if p.is_file()]
        report['sources_before'] = {str(p): sha(p) for p in sorted(files)}
        for opt in (0, 2):
            output = args.work_dir / ('O' + str(opt))
            run('build-O' + str(opt), [args.woozyc, 'build', '--project', project / 'lang.toml',
                '--backend', 'native', '--opt', str(opt), '--out-dir', output, '--package-objects', '--format', 'json'])
            result = run('run-O' + str(opt), [output / 'program', HERE / 'fixture.wz'])
            if not result.stdout.endswith('instance lookup controls passed\n'):
                raise RuntimeError('missing control summary')
        report['compiler_after'] = sha(args.woozyc)
        report['source_changes'] = [p for p, value in report['sources_before'].items() if sha(p) != value]
        assert report['compiler_before'] == report['compiler_after'] and not report['source_changes']
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        save()


if __name__ == '__main__':
    main()
