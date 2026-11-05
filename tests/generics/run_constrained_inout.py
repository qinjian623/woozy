#!/usr/bin/env python3
"""Execute constrained free-generic scalar inout fixtures on both backends."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True

CASES = ('positive', 'errors')
EXPECTED_EVENTS = {
    'positive': [8, 3, 7, 3, 5, 8, 2, 1, 4, 1],
    'errors': [4, 6, 3, 4, 3, 8, 2, 3],
}
EXPECTED_ALLOCATIONS = {'positive': 2, 'errors': 2}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, timeout=60, library=None):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    if library and library.is_dir():
        current = env.get('LD_LIBRARY_PATH')
        env['LD_LIBRARY_PATH'] = str(library) + (':' + current if current else '')
    completed = subprocess.run(argv, capture_output=True, text=True,
                               env=env, timeout=timeout)
    return dict(argv=argv, exit=completed.returncode,
                stdout=completed.stdout, stderr=completed.stderr)


def check_ok(row, what):
    assert row['exit'] == 0, (what, row)


def native_observations(text):
    """Return ordered observations and allocation tokens from stderr.

    Addresses may be reused after a free, so tokens identify allocation
    events in creation order rather than raw pointers.
    """
    seen = []
    allocations = []
    live = {}
    for line in text.splitlines():
        fields = line.split(':')
        if fields[0] == 'OBS':
            seen.append(('obs', int(fields[2]), int(fields[3])))
        elif fields[0] == 'VEC':
            event = int(fields[1])
            identity = fields[2]
            if event == 1:
                assert identity not in live, line
                token = len(allocations)
                allocations.append(token)
                live[identity] = token
            elif event == 5:
                assert identity in live, line
                seen.append(('free', live.pop(identity)))
            else:
                assert identity in live, line
    assert not live, ('still live', live)
    return seen, allocations


def assert_native(case, observations, allocations):
    events = [item[1] for item in observations if item[0] == 'obs']
    logs = [item[2] for item in observations if item[0] == 'obs']
    assert events == EXPECTED_EVENTS[case], observations
    assert len(allocations) == EXPECTED_ALLOCATIONS[case], allocations
    frees = [item[1] for item in observations if item[0] == 'free']
    assert sorted(frees) == sorted(allocations), observations
    assert len(set(frees)) == len(frees), observations
    if case == 'positive':
        assert logs == [8, 83, 837, 8373, 83735, 837358, 8373582,
                        83735821, 837358214, 8373582141], logs
        assert frees == [1, 0], frees
    else:
        assert logs == [4, 46, 463, 4634, 46343, 463438, 4634382,
                        46343823], logs
        # cleanup_fail registers its local defer before the throw: the deferred
        # event 6 is observed before the catch event 3, and both the deferred
        # local and the moved Vec are released before that catch.
        assert frees == [1, 0], frees
        first_catch = next(i for i, item in enumerate(observations)
                           if item[0] == 'obs' and item[1] == 3)
        assert all(i < first_catch for i, item in enumerate(observations)
                   if item[0] == 'free'), observations


def assert_python(case, data):
    assert data['main_return'] == 0, data
    assert not data['alive'], data
    assert data.get('frames_clean', 0) >= 1, data
    assert data['events'] == EXPECTED_EVENTS[case], data
    assert len(data['allocations']) == EXPECTED_ALLOCATIONS[case], data
    assert sorted(data['frees']) == sorted(item['object']
                                          for item in data['allocations']), data
    assert len(set(data['frees'])) == len(data['frees']), data
    # Scalar inout has no runtime borrow registration in these backends; the
    # observer still checks that every language frame has an empty borrow list
    # and no live owner. Do not count an empty list as a dynamic registration.
    snapshots = data.get('snapshots', {})
    if case == 'positive':
        assert snapshots['ax'] == 2 and snapshots['ay'] == 1, data
        assert snapshots['hx'] == 9 and snapshots['old'] == 8, data
        assert snapshots['qx'] == 99, data
        assert snapshots['sf'] == 1 and snapshots['sx'] == 2, data
        assert snapshots['wx'] == 10 and snapshots['wold'] == 9, data
        assert snapshots['xx'] == 10 and snapshots['xy'] == 20, data
    else:
        assert snapshots, data
        assert snapshots['first'] == 7 and snapshots['second'] == 8, data
        assert snapshots['third'] == 3 and snapshots['fourth'] == 5, data
        assert snapshots['old'] == 4, data
        errors = {(item['function'], item['type'])
                  for item in data['errors']}
        assert ('bad', 'app.E') in errors, data
        assert ('fail_after', 'app.E') in errors, data
        assert ('cleanup_fail', 'app.E') in errors, data


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'probe', 'helper', 'root', 'clang', 'runtime',
                 'python', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--only', action='append', choices=CASES)
    args = parser.parse_args()
    for name in ('cli', 'helper', 'root', 'python', 'out'):
        setattr(args, name, getattr(args, name).resolve())
    args.probe = args.probe.resolve()
    args.clang = args.clang.resolve()
    args.runtime = args.runtime.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    library = next((candidate for candidate in (
        args.clang.parent.parent / 'libxml2-legacy-2.13.9/usr/lib',
        args.clang.parent.parent.parent / 'libxml2-legacy-2.13.9/usr/lib')
        if candidate.is_dir()), None)
    observer = args.root / 'tests/python/observe_ownership.py'
    vec_observer = args.root / 'tests/native/vec_observer.cpp'
    report = dict(task='T07 constrained free generic scalar inout', status='failed',
                  python=run([args.python, '--version']), runs=[],
                  sanitizer_scope='generated Native programs are not sanitizer-instrumented')
    cases = tuple(args.only) if args.only else CASES
    try:
        for case in cases:
            manifest = args.root / f'tests/generics/inout/constrained/{case}/lang.toml'
            sources = {str(path.relative_to(manifest.parent)): digest(path)
                       for path in sorted(manifest.parent.rglob('*'))
                       if path.is_file()}
            for backend in ('native', 'python'):
                check = run([args.cli, 'check', '--project', manifest,
                             '--backend', backend, '--format', 'json'],
                            library=library)
                row = dict(case=case, backend=backend, check=check,
                           source_sha256=sources, status='failed')
                report['runs'].append(row)
                check_ok(check, 'check ' + backend)
                assert json.loads(check['stdout'])['status'] == 'ok', check
                row['status'] = 'passed'

            for opt in (0, 2):
                row = dict(case=case, opt=opt, source_sha256=sources,
                           status='failed')
                report['runs'].append(row)
                native = args.out / case / f'O{opt}'
                build = run([args.cli, 'build', '--project', manifest,
                             '--backend', 'native', '--opt', opt,
                             '--out-dir', native / 'cli', '--format', 'json'],
                            library=library)
                row['build'] = build
                check_ok(build, 'native build')
                assert json.loads(build['stdout'])['status'] == 'ok', build
                row['execute'] = executed = run([native / 'cli/program'],
                                                library=library)
                check_ok(executed, 'native execute')
                assert not executed['stdout'] and not executed['stderr'], executed
                row['probe'] = probed = run([args.probe, manifest,
                                             native / 'probe', opt],
                                            library=library)
                check_ok(probed, 'native probe')
                row['probe_checks'] = json.loads(probed['stdout'])
                observe = run([args.clang, f'-O{opt}', native / 'probe/observe.ll',
                               args.runtime, vec_observer,
                               '-o', native / 'observe'], library=library)
                row['observe_build'] = observe
                check_ok(observe, 'observe build')
                row['observe'] = observed = run([native / 'observe'],
                                                library=library)
                check_ok(observed, 'observe')
                ordered, allocations = native_observations(observed['stderr'])
                assert_native(case, ordered, allocations)
                row['native_allocations'] = len(allocations)
                row['native_order'] = ordered
                row['program_sha256'] = digest(native / 'cli/program')
                row['object_sha256'] = digest(native / 'cli/program.o')
                row['status'] = 'passed'

            python = args.out / case / 'python'
            build = run([args.cli, 'build', '--project', manifest,
                         '--backend', 'python', '--out-dir', python,
                         '--format', 'json'], library=library)
            row = dict(case=case, python=True, source_sha256=sources,
                       build=build, status='failed')
            report['runs'].append(row)
            check_ok(build, 'python build')
            assert json.loads(build['stdout'])['status'] == 'ok', build
            row['execute'] = executed = run([args.python, '-s',
                                             python / 'program.py'],
                                            library=library)
            check_ok(executed, 'python execute')
            assert not executed['stdout'] and not executed['stderr'], executed
            metadata = args.out / case / 'python-metadata'
            emit = run([args.helper, '--project', manifest,
                        '--out-dir', metadata], library=library)
            row['emit'] = emit
            check_ok(emit, 'python helper')
            assert digest(python / 'program.py') == digest(metadata / 'program.py'), \
                'helper differs from CLI output'
            assert digest(python / 'langrt.py') == digest(metadata / 'langrt.py'), \
                'helper runtime differs from CLI output'
            (python / 'metadata.json').write_bytes(
                (metadata / 'metadata.json').read_bytes())
            observed = run([args.python, '-s', observer, python],
                           library=library)
            row['observe'] = observed
            check_ok(observed, 'python observe')
            data = json.loads(observed['stdout'])
            assert_python(case, data)
            row['python_observation'] = data
            row['generated_sha256'] = {
                name: digest(python / name)
                for name in ('program.py', 'langrt.py', 'metadata.json')
            }
            row['status'] = 'passed'

        if not args.only:
            manifest = args.root / 'tests/generics/inout/constrained/reject/lang.toml'
            sources = {str(path.relative_to(manifest.parent)): digest(path)
                       for path in sorted(manifest.parent.rglob('*'))
                       if path.is_file()}
            fresh = args.out / 'reject-new'
            before = run([args.cli, 'build', '--project', manifest,
                          '--backend', 'native', '--opt', 0,
                          '--out-dir', fresh, '--format', 'json'], library=library)
            row = dict(case='reject-new', source_sha256=sources, build=before,
                       status='failed')
            report['runs'].append(row)
            assert before['exit'] != 0, before
            assert json.loads(before['stdout'])['status'] == 'error', before
            assert not (fresh / 'program').exists(), 'rejected build wrote program'
            assert not (fresh / 'program.o').exists(), 'rejected build wrote object'
            row['outputs'] = sorted(str(path.relative_to(fresh))
                                    for path in fresh.rglob('*') if path.is_file())
            row['status'] = 'passed'

            stale = args.out / 'reject-old'
            stale.mkdir(parents=True, exist_ok=True)
            sentinels = {name: b'previous output ' + name.encode()
                         for name in ('program', 'program.o')}
            for name, content in sentinels.items():
                (stale / name).write_bytes(content)
            old_hashes = {name: digest(stale / name) for name in sentinels}
            after = run([args.cli, 'build', '--project', manifest,
                         '--backend', 'native', '--opt', 0,
                         '--out-dir', stale, '--format', 'json'], library=library)
            row = dict(case='reject-old', source_sha256=sources, build=after,
                       before_sha256=old_hashes, status='failed')
            report['runs'].append(row)
            assert after['exit'] != 0, after
            assert json.loads(after['stdout'])['status'] == 'error', after
            row['after_sha256'] = {name: digest(stale / name)
                                   for name in sentinels}
            assert row['after_sha256'] == old_hashes, row
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
