#!/usr/bin/env python3
"""Build real Wz programs and run isolated language-GC observations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
CASES = ('self-cycle', 'two-cycles', 'duplicate-edges', 'dead-to-live',
         'nested-owned', 'auto-churn', 'always-continue',
         'exception-defer')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--python', type=Path, required=True)
    parser.add_argument('--timeout', type=float, default=60.0)
    parser.add_argument('--only', action='append', default=[])
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    source = (HERE / 'source.wz').read_text()
    cases = list(CASES)
    if args.only:
        assert set(args.only) <= set(CASES), 'unknown case'
        cases = [case for case in cases if case in args.only]
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
               PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    report = {'status': 'failed', 'cases': []}
    for case in cases:
        project = args.out / 'projects' / case
        (project / 'src').mkdir(parents=True, exist_ok=True)
        (project / 'src/main.wz').write_text(source)
        (project / 'lang.toml').write_text(
            'schema_version=1\n[package]\nname="app"\n'
            'source_root="src"\nentry="app"\n[build]\n'
            'backend="python"\nword_bits=64\n')
        generated = args.out / 'generated' / case
        row = {'case': case, 'status': 'failed', 'commands': [],
               'source_sha256': digest(project / 'src/main.wz')}
        report['cases'].append(row)
        build = subprocess.run(
            [str(args.helper), '--project', str(project / 'lang.toml'),
             '--out-dir', str(generated)],
            capture_output=True, text=True, env=env, timeout=args.timeout)
        row['commands'].append({'argv': ['helper', project.name, case],
                                'exit': build.returncode,
                                'stdout': build.stdout,
                                'stderr': build.stderr})
        if build.returncode != 0:
            row['error'] = 'helper rejected real Wz project'
            (args.out / 'results.json').write_text(
                json.dumps(report, indent=2, ensure_ascii=False))
            print(json.dumps({'status': 'failed', 'case': case,
                              'diagnostics': build.stdout}))
            return 1
        assert (generated / 'program.py').exists()
        assert (generated / 'langrt.py').exists()
        assert (generated / 'metadata.json').exists()
        row['generated'] = {
            name: digest(generated / name)
            for name in ('program.py', 'langrt.py', 'metadata.json')}
        run = subprocess.run(
            [str(args.python), '-s', str(HERE / 'observe_language_gc.py'),
             str(generated), case],
            capture_output=True, text=True, env=env, timeout=args.timeout)
        row['commands'].append({'argv': ['python', case],
                                'exit': run.returncode,
                                'stdout': run.stdout,
                                'stderr': run.stderr})
        if run.returncode != 0:
            row['error'] = 'observer failed'
            (args.out / 'results.json').write_text(
                json.dumps(report, indent=2, ensure_ascii=False))
            print(json.dumps({'status': 'failed', 'case': case,
                              'stderr': run.stderr}))
            return 1
        row['observation'] = json.loads(run.stdout.splitlines()[-1])
        row['status'] = 'passed'
    report['status'] = 'passed'
    report['total'] = len(cases)
    report['passed'] = len(cases)
    (args.out / 'results.json').write_text(
        json.dumps(report, indent=2, ensure_ascii=False))
    print(json.dumps({'status': 'passed', 'cases': cases}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
