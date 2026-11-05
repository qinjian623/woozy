#!/usr/bin/env python3
"""Execute inherent-method move fixtures on both backends and inspect ownership."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True

CASES = ('positive', 'error-before', 'error-after', 'receiver-fails',
         'callee-throws', 'enum-static', 'generic-explicit', 'generic-impl',
         'generic-impl-errors', 'generic-impl-own',
         'generic-impl-own-errors', 'constrained-inherent', 'trait-modes',
         'trait-type-call-factory', 'trait-type-call-owned',
         'trait-type-call-self', 'trait-generic-phantom',
         'trait-generic-multi')
EXPECTED_EVENTS = {
    'positive': [9, 3, 2],
    'error-before': [9, 2, 3],
    'error-after': [9, 2, 3],
    'receiver-fails': [2, 3],
    'callee-throws': [],
    'enum-static': [],
    'generic-explicit': [],
    'generic-impl': [9, 3, 2],
    'generic-impl-errors': [9, 2, 3, 9, 2, 3, 2, 3, 3],
    'generic-impl-own': [9, 3, 2],
    'generic-impl-own-errors': [9, 2, 3, 9, 2, 3, 2, 3, 3],
    'constrained-inherent': [1, 2, 5, 6, 7, 8, 9, 3, 2],
    'trait-modes': [7, 1, 3, 2, 7, 1, 3, 2, 7, 1, 3, 2],
    'trait-type-call-factory': [3, 2, 4, 9],
    'trait-type-call-owned': [],
    'trait-type-call-self': [],
    'trait-generic-phantom': [],
    'trait-generic-multi': [9, 3],
}
EXPECTED_ALLOCATIONS = {
    'positive': 11,
    'error-before': 2,
    'error-after': 2,
    'receiver-fails': 2,
    'callee-throws': 5,
    'enum-static': 4,
    'generic-explicit': 13,
    'generic-impl': 24,
    'generic-impl-errors': 16,
    'generic-impl-own': 23,
    'generic-impl-own-errors': 16,
    'constrained-inherent': 20,
    'trait-modes': 6,
    'trait-type-call-factory': 0,
    'trait-type-call-owned': 8,
    'trait-type-call-self': 0,
    'trait-generic-phantom': 0,
    'trait-generic-multi': 16,
}


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
    assert events == EXPECTED_EVENTS[case], observations
    assert len(allocations) == EXPECTED_ALLOCATIONS[case], allocations
    frees = [item[1] for item in observations if item[0] == 'free']
    assert sorted(frees) == sorted(allocations), observations
    assert len(set(frees)) == len(frees), observations
    if case == 'error-before':
        # The receiver temporary is created after the main Vec. A later
        # argument fails before the move, so the receiver is released first;
        # the untouched binding is released only at normal function exit.
        assert frees == [allocations[1], allocations[0]], observations
        first = observations.index(('free', allocations[1]))
        assert observations.index(('obs', 2, 92)) < first, observations
        assert first < observations.index(('obs', 3, 923)), observations
        assert observations.index(('free', allocations[0])) > \
            observations.index(('obs', 3, 923)), observations
    elif case == 'error-after':
        # The move temporary was created after the receiver temporary. Both
        # are released in reverse creation order before the catch event.
        assert frees == [allocations[0], allocations[1]], observations
        first = observations.index(('free', allocations[0]))
        second = observations.index(('free', allocations[1]))
        assert observations.index(('obs', 2, 92)) < first < second, observations
        assert second < observations.index(('obs', 3, 923)), observations
    elif case == 'receiver-fails':
        # The receiver throws before any argument or move is evaluated. Its
        # local Vec is released before catch; the untouched caller binding is
        # released only at normal function exit.
        assert frees == [allocations[1], allocations[0]], observations
        first = observations.index(('free', allocations[1]))
        assert observations.index(('obs', 2, 2)) < first, observations
        assert first < observations.index(('obs', 3, 23)), observations
        assert observations.index(('free', allocations[0])) > \
            observations.index(('obs', 3, 23)), observations
    elif case in ('generic-impl-errors', 'generic-impl-own-errors'):
        # The generic-impl receiver, moved argument and throwing callee all use
        # the existing cleanup records. The first receiver temp frees before
        # catch, the moved temp frees before its receiver in reverse creation
        # order, and the throwing callee's local plus moved argument free before
        # the catch event.
        assert frees == [2, 1, 3, 5, 4, 8, 7, 13, 12, 11, 15, 14, 10, 9,
                         6, 0], observations
        assert observations.index(('obs', 2, 92)) < \
            observations.index(('free', 2)) < \
            observations.index(('free', 1)) < \
            observations.index(('obs', 3, 923)), observations
        assert observations.index(('obs', 2, 92392)) < \
            observations.index(('free', 3)) < \
            observations.index(('free', 5)) < \
            observations.index(('free', 4)) < \
            observations.index(('obs', 3, 923923)), observations
        assert observations.index(('obs', 2, 9239232)) < \
            observations.index(('free', 8)) < \
            observations.index(('free', 7)) < \
            observations.index(('obs', 3, 92392323)), observations
        assert observations.index(('free', 13)) < \
            observations.index(('free', 12)) < \
            observations.index(('free', 11)) < \
            observations.index(('obs', 3, 923923233)), observations
    elif case == 'trait-modes':
        # Each receiver temporary frees before the following section. The
        # generic trait impl receiver/moved arguments then free before the
        # earlier Box moved argument and the caller's shared aliases.
        assert frees == [1, 3, 5, 4, 2, 0], observations
        assert observations.index(('obs', 2, 7132)) < \
            observations.index(('free', 1)) < \
            observations.index(('obs', 1, 1)), observations
        second_end = observations.index(('obs', 2, 732))
        free_second = observations.index(('free', 3))
        third_start = observations.index(('obs', 1, 1), free_second)
        assert second_end < free_second < third_start, observations
        third_end = observations.index(('obs', 2, 732), third_start)
        assert third_end < observations.index(('free', 5)) < \
            observations.index(('free', 4)) < \
            observations.index(('free', 2)) < \
            observations.index(('free', 0)), observations


def assert_python(case, data):
    assert data['main_return'] == 0, data
    assert not data['alive'], data
    assert data.get('frames_clean', 0) >= 1, data
    assert data['events'] == EXPECTED_EVENTS[case], data
    assert len(data['allocations']) == EXPECTED_ALLOCATIONS[case], data
    assert sorted(data['frees']) == sorted(item['object']
                                          for item in data['allocations']), data
    assert len(set(data['frees'])) == len(data['frees']), data
    sequence = [item for item in data['operations']
                if item[0] in ('event', 'free')]
    if case == 'positive':
        snapshots = data.get('snapshots', {})
        assert snapshots['alias']['object'] == snapshots['result']['object'], data
        assert snapshots['static_alias']['object'] == \
            snapshots['static_result']['object'], data
        assert snapshots['box_alias'] == snapshots['box_result'], data
        assert snapshots['kept_alias'] == snapshots['kept_result'], data
    elif case == 'error-before':
        assert sequence == [['event', 9], ['event', 2], ['free', 1],
                            ['event', 3], ['free', 0]], sequence
    elif case == 'error-after':
        assert sequence == [['event', 9], ['event', 2], ['free', 0],
                            ['free', 1], ['event', 3]], sequence
    elif case == 'receiver-fails':
        assert sequence == [['event', 2], ['free', 1], ['event', 3],
                            ['free', 0]], sequence
        assert any(error['function'] == 'bad_make' and
                   error['type'] == 'app.E' for error in data['errors']), data
    elif case == 'enum-static':
        snapshots = data.get('snapshots', {})
        assert snapshots['alias']['object'] == snapshots['result']['object'], data
        assert snapshots['box_alias'] == snapshots['box_result'], data
        assert snapshots['payload_alias'] == snapshots['payload_result'], data
    elif case == 'generic-explicit':
        snapshots = data.get('snapshots', {})
        assert snapshots['alias']['object'] == snapshots['result']['object'], data
        assert snapshots['box_alias'] == snapshots['box_result'], data
        assert snapshots['kept_alias'] == snapshots['kept_result'], data
    elif case == 'generic-impl':
        snapshots = data.get('snapshots', {})
        assert snapshots['alias']['object'] == snapshots['result']['object'], data
        assert snapshots['static_alias']['object'] == \
            snapshots['static_result']['object'], data
        assert snapshots['kept_alias']['object'] == snapshots['kept_result']['object'], data
        assert snapshots['maybe_alias']['object'] == \
            snapshots['maybe_result']['object'], data
        assert snapshots['inner_alias'][1]['object'] == \
            snapshots['inner'][1]['object'], data
        assert snapshots['moved_box'][1]['object'] == \
            snapshots['inner'][1]['object'], data
    elif case == 'generic-impl-own':
        snapshots = data.get('snapshots', {})
        for alias, result in (('y_alias', 'ry'), ('z_alias', 'rz'),
                              ('s0_alias', 'rs0'), ('s2_alias', 'rs2'),
                              ('m2_alias', 'rm'), ('fw_alias', 'fwr'),
                              ('named_alias', 'named'), ('kept_alias',
                                                         'kept_result'),
                              ('e1_alias', 're')):
            assert snapshots[alias]['object'] == snapshots[result]['object'], data
        assert snapshots['y_alias']['items'] == [15], data
        assert snapshots['z_alias']['items'] == [16], data
        assert snapshots['s0_alias']['items'] == [17], data
        assert snapshots['s2_alias']['items'] == [18], data
        assert snapshots['m2_alias']['items'] == [19], data
        assert snapshots['fw_alias']['items'] == [20], data
        assert snapshots['named_alias']['items'] == [21], data
        assert snapshots['kept_alias']['items'] == [22], data
        assert snapshots['e1_alias']['items'] == [27], data
    elif case == 'generic-impl-own-errors':
        assert sequence == [
            ['event', 9], ['event', 2], ['free', 2], ['free', 1],
            ['event', 3], ['event', 9], ['event', 2], ['free', 3],
            ['free', 5], ['free', 4], ['event', 3], ['event', 2],
            ['free', 8], ['free', 7], ['event', 3], ['free', 13],
            ['free', 12], ['free', 11], ['event', 3], ['free', 15],
            ['free', 14], ['free', 10], ['free', 9], ['free', 6],
            ['free', 0]], sequence
        assert any(error['function'] == 'bad' and error['type'] == 'app.E'
                   for error in data['errors']), data
        assert any(error['function'] == 'bad_make' and
                   error['type'] == 'app.E' for error in data['errors']), data
        assert any(error['function'] == 'fail' and error['type'] == 'app.E'
                   for error in data['errors']), data
    elif case == 'trait-modes':
        snapshots = data.get('snapshots', {})
        assert snapshots['log'] == 7132 and snapshots['flog'] == 732, data
        assert snapshots['rlog'] == 1, data
        assert snapshots['r'] == 32 and snapshots['br'] == 32, data
        assert snapshots['alias'] == [{'object': 0, 'items': [8]}], data
        assert snapshots['balias'] == [7, {'object': 2, 'items': [8]}], data
        assert snapshots['qlog'] == 732 and snapshots['qrlog'] == 1, data
        assert snapshots['q'] == -30, data
        assert snapshots['qalias'] == [{'object': 4, 'items': [8]}], data
        assert data['frees'] == [1, 3, 5, 4, 2, 0], data
    elif case == 'constrained-inherent':
        snapshots = data.get('snapshots', {})
        assert snapshots['a'][0]['items'] == [5], data
        assert snapshots['b'][1][0]['items'] == [16], data
        assert snapshots['b_alias'][1][0]['object'] == \
            snapshots['b'][1][0]['object'], data
        assert snapshots['c'][0]['items'] == [17], data
        assert snapshots['i2_alias'][0]['object'] == \
            snapshots['c'][0]['object'], data
        assert snapshots['d'][1][0]['items'] == [19], data
        assert snapshots['u1_alias'][1][0]['object'] == \
            snapshots['d'][1][0]['object'], data
        assert snapshots['e'][1][0]['items'] == [10], data
        assert snapshots['f'][1][0]['items'] == [11], data
        assert snapshots['g'] == 33 and snapshots['h'] == 24, data
        assert snapshots['k'] == 15, data
        assert snapshots['n'][1]['items'] == [18], data
        assert snapshots['bi_alias'][1]['object'] == \
            snapshots['n'][1]['object'], data
        assert snapshots['m'] == 1, data
        assert snapshots['z'][0]['items'] == [16], data
        assert snapshots['st'][1][0]['items'] == [18], data
        assert snapshots['sti'][0]['items'] == [19], data
        assert snapshots['ordered'][1][0]['items'] == [17], data
        assert snapshots['log'] == 1256789, data
        assert snapshots['order'] == 32, data
    elif case == 'generic-impl-errors':
        assert sequence == [
            ['event', 9], ['event', 2], ['free', 2], ['free', 1],
            ['event', 3], ['event', 9], ['event', 2], ['free', 3],
            ['free', 5], ['free', 4], ['event', 3], ['event', 2],
            ['free', 8], ['free', 7], ['event', 3], ['free', 13],
            ['free', 12], ['free', 11], ['event', 3], ['free', 15],
            ['free', 14], ['free', 10], ['free', 9], ['free', 6],
            ['free', 0]], sequence
        assert any(error['function'] == 'bad' and error['type'] == 'app.E'
                   for error in data['errors']), data
        assert any(error['function'] == 'bad_make' and error['type'] == 'app.E'
                   for error in data['errors']), data
        assert any(error['function'] == 'fail' and error['type'] == 'app.E'
                   for error in data['errors']), data
    elif case == 'trait-type-call-factory':
        snapshots = data.get('snapshots', {})
        assert snapshots['log'] == 32 and snapshots['ok_log'] == 4, data
        assert snapshots['ordered_value'] == [23], data
        assert any(error['function'] == 'checked_bad' and
                   error['type'] == 'app.E' for error in data['errors']), data
    elif case == 'trait-type-call-owned':
        snapshots = data.get('snapshots', {})
        assert snapshots['values_alias']['items'] == [11], data
        assert snapshots['base_alias'][0]['items'] == [7], data
        assert snapshots['r2'][1] == 3, data
        assert snapshots['ok'][1] == 12, data
        assert any(error['function'] == 'check' and
                   error['type'] == 'app.E' for error in data['errors']), data
    elif case == 'trait-type-call-self':
        snapshots = data.get('snapshots', {})
        assert snapshots['a'] == [7] and snapshots['b'] == [8], data
        assert snapshots['c'] == [9] and snapshots['d'] == [10], data
        assert not data['errors'], data
    elif case == 'trait-generic-phantom':
        snapshots = data.get('snapshots', {})
        assert snapshots['forwarded'] == 66, data
        assert snapshots['direct'] == 66, data
        assert not data['errors'], data
        assert data['events'] == [], data
        assert data['allocations'] == [] and data['frees'] == [], data
    elif case == 'trait-generic-multi':
        snapshots = data.get('snapshots', {})
        for alias, result in (('relayed', 'va'), ('made', 'ma'),
                              ('ordered', 'ordered_alias'),
                              ('saved', 'saved_alias')):
            assert snapshots[alias]['object'] == snapshots[result]['object'], data
        assert snapshots['relayed']['items'] == [17], data
        assert snapshots['made']['items'] == [18], data
        assert snapshots['ordered']['items'] == [19], data
        assert snapshots['saved']['items'] == [21], data
        assert snapshots['picked'] == 3 and snapshots['log'] == 93, data
        assert snapshots['early_alias']['items'] == [10], data
        assert data['events'] == [9, 3], data
        assert [(error['function'], error['value'][1][0])
                for error in data['errors']] == [
                    ('failing_vec', 4), ('failing_one', 3), ('relay', 7),
                    ('make', 8), ('relay', 7), ('via', 7)], data
    else:
        assert any(error['function'] == 'consume' and
                   error['type'] == 'app.E' for error in data['errors']), data


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
    report = dict(task='T07 inherent method move', status='failed',
                  python=run([args.python, '--version']), runs=[])
    cases = tuple(args.only) if args.only else CASES
    try:
        for case in cases:
            manifest = args.root / f'tests/generics/method-move/{case}/lang.toml'
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
            manifest = args.root / 'tests/generics/method-move/reject/lang.toml'
            sources = {str(path.relative_to(manifest.parent)): digest(path)
                       for path in sorted(manifest.parent.rglob('*'))
                       if path.is_file()}
            fresh = args.out / 'reject-new'
            shutil.rmtree(fresh, ignore_errors=True)
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
