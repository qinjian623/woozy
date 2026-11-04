#!/usr/bin/env python3
"""Run the generic move fixtures on both backends and inspect ownership order."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True

CASES = ('positive', 'error-before', 'error-after')
EXPECTED_EVENTS = {
    'positive': None,
    'error-before': [2, 3],
    'error-after': [9, 2, 3],
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, timeout=45):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    completed = subprocess.run(argv, capture_output=True, text=True,
                               env=env, timeout=timeout)
    return dict(argv=argv, exit=completed.returncode,
                stdout=completed.stdout, stderr=completed.stderr)


def check_ok(row):
    assert row['exit'] == 0, row


def native_observations(text):
    """Return ordered OBS values and Vec free identities from observer stderr."""
    seen = []
    allocations = set()
    live = set()
    for line in text.splitlines():
        fields = line.split(':')
        if fields[0] == 'OBS':
            seen.append(('obs', int(fields[2]), int(fields[3])))
        elif fields[0] == 'VEC':
            event = int(fields[1])
            identity = fields[2]
            if event == 1:
                assert identity not in allocations, line
                allocations.add(identity)
                live.add(identity)
            elif event == 5:
                assert identity in live, line
                live.remove(identity)
                seen.append(('free', identity))
            else:
                assert identity in allocations, line
    assert not live, ('still live', live)
    return seen, allocations


def assert_native_order(case, observations):
    events = [item for item in observations if item[0] == 'obs']
    assert [item[1] for item in events] == (EXPECTED_EVENTS[case] or []), events
    frees = [item for item in observations if item[0] == 'free']
    if case == 'positive':
        assert not events and frees, observations
        return
    assert len(frees) == 1, observations
    free_at = observations.index(frees[0])
    if case == 'error-before':
        last_event = max(observations.index(item) for item in events)
        assert free_at > last_event, observations
    else:
        first = observations.index(events[1])
        last = observations.index(events[2])
        assert first < free_at < last, observations


def python_observation(case, data):
    assert data['main_return'] == 0, data
    assert not data['alive'], data
    assert data.get('frames_clean', 0) >= 1, data
    assert data['events'] == (EXPECTED_EVENTS[case] or []), data
    if case == 'positive':
        assert data['frees'] and len(data['frees']) == len(data['allocations']), data
        snapshots = data.get('snapshots', {})
        if 'alias' in snapshots and 'result' in snapshots:
            assert snapshots['alias']['object'] == snapshots['result']['object'], data
        return
    sequence = [item for item in data['operations']
                if item[0] in ('event', 'free')]
    if case == 'error-before':
        assert sequence == [['event', 2], ['event', 3], ['free', 0]], sequence
    else:
        assert sequence == [['event', 9], ['event', 2], ['free', 0],
                            ['event', 3]], sequence


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'probe', 'helper', 'root', 'clang', 'runtime',
                 'python', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    for name in ('cli', 'probe', 'helper', 'root', 'clang', 'runtime',
                 'python', 'out'):
        setattr(args, name, getattr(args, name).resolve())
    args.out.mkdir(parents=True, exist_ok=True)
    observer = args.root / 'tests/python/observe_ownership.py'
    vec_observer = args.root / 'tests/native/vec_observer.cpp'
    report = dict(task='T07 unconstrained generic move', status='failed',
                  python=run([args.python, '--version']), runs=[])
    try:
        for case in CASES:
            manifest = args.root / f'tests/generics/move/{case}/lang.toml'
            sources = {str(path.relative_to(manifest.parent)): digest(path)
                       for path in sorted(manifest.parent.rglob('*'))
                       if path.is_file()}
            for opt in (0, 2):
                row = dict(case=case, opt=opt, source_sha256=sources,
                           status='failed')
                report['runs'].append(row)
                native = args.out / case / f'O{opt}'
                build = run([args.cli, 'build', '--project', manifest,
                             '--backend', 'native', '--opt', opt,
                             '--out-dir', native / 'cli', '--format', 'json'])
                row['build'] = build
                check_ok(build)
                assert json.loads(build['stdout'])['status'] == 'ok', build
                row['execute'] = executed = run([native / 'cli/program'])
                check_ok(executed)
                assert not executed['stdout'] and not executed['stderr'], executed
                row['probe'] = probed = run([args.probe, manifest,
                                             native / 'probe', opt])
                check_ok(probed)
                row['probe_checks'] = json.loads(probed['stdout'])
                observe = run([args.clang, f'-O{opt}', native / 'probe/observe.ll',
                               args.runtime, vec_observer,
                               '-o', native / 'observe'])
                row['observe_build'] = observe
                check_ok(observe)
                row['observe'] = observed = run([native / 'observe'])
                check_ok(observed)
                ordered, allocations = native_observations(observed['stderr'])
                assert_native_order(case, ordered)
                row['native_allocations'] = len(allocations)
                row['native_order'] = ordered
                if case == 'positive':
                    assert len(allocations) == 3, allocations
                else:
                    assert len(allocations) == 1, allocations
                row['program_sha256'] = digest(native / 'cli/program')
                row['object_sha256'] = digest(native / 'cli/program.o')
                row['status'] = 'passed'

            python = args.out / case / 'python'
            build = run([args.cli, 'build', '--project', manifest,
                         '--backend', 'python', '--out-dir', python,
                         '--format', 'json'])
            row = dict(case=case, python=True, source_sha256=sources,
                       build=build, status='failed')
            report['runs'].append(row)
            check_ok(build)
            assert json.loads(build['stdout'])['status'] == 'ok', build
            row['execute'] = executed = run([args.python, '-s',
                                             python / 'program.py'])
            check_ok(executed)
            assert not executed['stdout'] and not executed['stderr'], executed
            metadata = args.out / case / 'python-metadata'
            emit = run([args.helper, '--project', manifest,
                        '--out-dir', metadata])
            row['emit'] = emit
            check_ok(emit)
            assert digest(python / 'program.py') == digest(metadata / 'program.py'), \
                'helper differs from CLI output'
            assert digest(python / 'langrt.py') == digest(metadata / 'langrt.py'), \
                'helper runtime differs from CLI output'
            (python / 'metadata.json').write_bytes(
                (metadata / 'metadata.json').read_bytes())
            observed = run([args.python, '-s', observer, python])
            row['observe'] = observed
            check_ok(observed)
            data = json.loads(observed['stdout'])
            python_observation(case, data)
            row['python_observation'] = data
            row['generated_sha256'] = {
                name: digest(python / name)
                for name in ('program.py', 'langrt.py', 'metadata.json')
            }
            row['status'] = 'passed'
        report['status'] = 'passed'
    finally:
        (args.out / 'results.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(
        status=report['status'], runs=len(report['runs']),
        failed=[(row['case'], row.get('opt'), row.get('failure'))
                for row in report['runs'] if row['status'] != 'passed'],
        report=str(args.out / 'results.json'))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
