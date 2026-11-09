#!/usr/bin/env python3
"""Execute inherent-method scalar inout fixtures on both backends."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True

CASES = ('positive', 'errors', 'constrained-inherent', 'trait-modes',
         'trait-self', 'trait-self-callback', 'trait-generic-inout',
         'trait-qualified-inout', 'trait-qualified-own',
         'trait-ordinary-holder-control', 'trait-ordinary-return-control',
         'trait-function-cycle-reject', 'trait-type-inout-return-reject')
REJECT_CASES = ('reject', 'trait-generic-nonscalar-reject')
EXPECTED_EVENTS = {
    'positive': [9, 3, 5],
    'errors': [9, 2, 3, 9, 8, 2, 3, 9, 4, 3, 9, 4, 6, 3, 9, 5, 6],
    'constrained-inherent': [23, 14, 3, 5, 4, 3, 4, 6, 3],
    'trait-modes': [1, 5, 1, 3, 7, 3, 3, 9, 2, 8, 4, 2],
    'trait-self': [9, 2, 3, 2, 3, 9, 2, 3, 9, 3, 2, 4, 4, 6, 3, 5, 6, 4],
    'trait-self-callback': [2, 1, 2, 3, 1, 2, 4, 2, 1, 2, 3, 1, 2, 4,
                            5, 4, 6, 9, 3, 4, 6, 9],
    'trait-generic-inout': [1, 1, 2, 3, 4, 1, 1, 4, 9, 4, 6, 9, 5, 6, 9],
    'trait-qualified-inout': [1, 1, 3, 3, 4, 2, 5, 5, 7, 3],
    'trait-qualified-own': [],
    'trait-ordinary-holder-control': [],
    'trait-ordinary-return-control': [],
    'trait-function-cycle-reject': [],
    'trait-type-inout-return-reject': [],
}
EXPECTED_LOGS = {
    'trait-generic-inout': [1, 11, 112, 1123, 11234, 112341, 1123411,
                            11234114, 112341149, 1123411494, 11234114946,
                            112341149469, 1123411494695, 11234114946956,
                            112341149469569],
    'trait-qualified-inout': [1, 11, 113, 1133, 11334, 113342, 1133425,
                              11334255, 7, 113342553],
    'trait-qualified-own': [],
    'trait-ordinary-holder-control': [],
    'trait-ordinary-return-control': [],
    'trait-function-cycle-reject': [],
    'trait-type-inout-return-reject': [],
}
EXPECTED_ALLOCATIONS = {'positive': 8, 'errors': 13,
                        'constrained-inherent': 8, 'trait-modes': 2,
                        'trait-self': 9, 'trait-self-callback': 4,
                        'trait-generic-inout': 4,
                        'trait-qualified-inout': 2,
                        'trait-qualified-own': 0,
                        'trait-ordinary-holder-control': 0,
                        'trait-ordinary-return-control': 0,
                        'trait-function-cycle-reject': 0,
                        'trait-type-inout-return-reject': 0}


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
        assert logs == [9, 93, 935], logs
    elif case == 'constrained-inherent':
        # The trait-constrained receiver and both constrained own parameters
        # use the ordinary scalar inout path. Named arguments are evaluated in
        # source order, typed throw keeps the written scalar, and the deferred
        # write in cleanup_fail runs after the return value is saved.
        assert logs == [23, 244, 3, 35, 354, 3543, 35434, 354346,
                        3543463], logs
        assert frees == [4, 5, 7, 6, 3, 2, 1, 0], frees
    elif case == 'trait-modes':
        # Concrete and generic trait impl calls use ordinary self and concrete
        # scalar inout places. Named arguments are evaluated in written order;
        # the typed throw preserves the callee's write, and the deferred write
        # happens after the old scalar result has been saved.
        assert logs == [1, 5, 11, 3, 7, 33, 113, 1139, 11392, 113928, 4,
                        1139282], logs
        assert frees == [1, 0], frees
    elif case == 'trait-self':
        # `slot inout Self` writes back through concrete and constrained
        # calls. The receiver factory marker appears once per invocation, so
        # the receiver is evaluated once. Named arguments follow source order
        # (second=3 before first=2). A throwing callee and a throwing later
        # argument preserve the documented write rules; defer runs after the
        # returned scalar is saved. Vec moves and locals are each freed once
        # in the observed cleanup order.
        assert logs == [9, 92, 923, 2, 23, 9, 92, 923, 9, 93, 932, 9324,
                        4, 46, 463, 5, 56, 564], logs
        assert frees == [0, 2, 1, 3, 4, 6, 5, 8, 7], frees
    elif case == 'trait-generic-inout':
        # Method-own U/V and trait Self inout use the ordinary scalar
        # writeback path. A later throwing argument never enters the callee;
        # the callee's own throw keeps its write and runs defer; the saved
        # call returns before its deferred write. Two owned Vec arguments and
        # the two deferred locals are each freed once.
        assert logs == EXPECTED_LOGS[case], logs
        assert frees == [1, 0, 3, 2], frees
    elif case == 'trait-qualified-inout':
        # Concrete Type and bounded T entries pass every declared parameter
        # explicitly; no synthetic receiver is created. Non-method-generic
        # inout and generic method inout coexist with move owned arguments.
        assert logs == EXPECTED_LOGS[case], logs
        assert frees == [0, 1], frees
    elif case == 'trait-qualified-own':
        # A method-own first parameter named self stays static even when its
        # concrete type equals the impl type. No receiver is fabricated.
        assert logs == EXPECTED_LOGS[case], logs
        assert frees == [], frees
    elif case in ('trait-ordinary-holder-control',
                  'trait-ordinary-return-control',
                  'trait-function-cycle-reject',
                  'trait-type-inout-return-reject'):
        # Legacy ordinary-only Type/T calls and the newly opened dependent
        # callback entry keep their Self callbacks and allocate no heap.
        assert logs == EXPECTED_LOGS[case], logs
        assert frees == [], frees
    elif case == 'trait-self-callback':
        # The i32 and bool blocks each invoke a callback returned by the
        # factory, a concrete receiver method with a free function value, and
        # the same constraint forwarded through two generic templates. The
        # saved/failed methods show the callback write, return-before-defer,
        # and moved-owned/local cleanup. The callback itself is proven by the
        # changed slot and returned old values, not only by the marks around it.
        assert logs == [2, 21, 212, 2123, 21231, 212312, 2123124,
                        2, 21, 212, 2123, 21231, 212312, 2123124,
                        5, 54, 546, 5469, 3, 34, 346, 3469], logs
        assert frees == [1, 0, 3, 2], frees
    else:
        # Receiver temporaries are released before their catch events. The
        # throwing callee releases its deferred local and moved argument before
        # the caller receiver temporary. The successful receiver is released
        # before the following marks; the last holder survives to normal exit.
        assert logs == [
            9, 92, 923, 9239, 92398, 923982, 9239823, 9, 92398234,
            923982343, 99, 9239823434, 92398234346, 923982343463,
            9239823434639, 92398234346395, 923982343463956,
        ], logs
        assert frees == [1, 0, 3, 2, 5, 4, 9, 6, 8, 7, 11, 10, 12], frees


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
    if case == 'positive':
        snapshots = data.get('snapshots', {})
        assert snapshots['v'] == 5 and snapshots['oldv'] == 26, data
        assert snapshots['ad'] == 44 and snapshots['oldac'] == 40, data
        assert snapshots['w'] == 33 and snapshots['x'] is True, data
        assert snapshots['aa'] == 37 and snapshots['ab'] == 39, data
    elif case == 'trait-modes':
        snapshots = data.get('snapshots', {})
        assert snapshots['log'] == 1139282 and snapshots['order'] == 5, data
        assert snapshots['flog'] == 33 and snapshots['forder'] == 7, data
        assert snapshots['r'] == 2 and snapshots['x'] == 5, data
        assert snapshots['y'] == 7 and snapshots['br'] == 4, data
        assert snapshots['oldb'] == 8 and snapshots['z'] == 9, data
        assert snapshots['t'] == 11, data
        assert snapshots['delayed'] == 20 and snapshots['d'] == 21, data
        assert snapshots['u'] == 30 and snapshots['order2'] == 4, data
        errors = {(item['function'], item['type'])
                  for item in data['errors']}
        assert ('fail', 'app.E') in errors, data
        assert ('bad', 'app.E') in errors, data
    elif case == 'constrained-inherent':
        snapshots = data.get('snapshots', {})
        assert snapshots['a'] == 3 and snapshots['olda'] == 1, data
        assert snapshots['b'] == 5 and snapshots['oldb'] == 4, data
        assert snapshots['c'] == 7 and snapshots['oldc'] == 6, data
        assert snapshots['d'] == 9 and snapshots['oldd'] == 8, data
        assert snapshots['e'] is True and snapshots['olde'] is False, data
        assert snapshots['f'] == 2.5 and snapshots['oldf'] == 1.5, data
        assert snapshots['g'] == 4.5 and snapshots['oldg'] == 3.5, data
        assert snapshots['h'] == 11 and snapshots['oldh'] == 10, data
        assert snapshots['i'] == 13 and snapshots['oldi'] == 12, data
        assert snapshots['j'] == 15 and snapshots['k'] is True, data
        assert snapshots['paired'] == 1, data
        assert snapshots['l'] == 17 and snapshots['oldl'] == 16, data
        assert snapshots['m'] == 19 and snapshots['oldm'] == 18, data
        assert snapshots['n'] == 21 and snapshots['oldn'] == 20, data
        assert snapshots['o'] == 23 and snapshots['p'] == 22, data
        assert snapshots['q'] == 25 and snapshots['oldq'] == 24, data
        assert snapshots['left_pick'] == 23, data
        assert snapshots['right_pick'] == 14, data
        assert snapshots['r'] == 27 and snapshots['s'] == 29, data
        assert snapshots['oldt'] == 28 and snapshots['t'] == 29, data
        assert snapshots['oldu'] == 30 and snapshots['u'] == 31, data
        assert snapshots['ub'] is True and snapshots['oldub'] is False, data
        assert snapshots['v'] == 35 and snapshots['oldv'] == 34, data
        assert snapshots['oldw'] == 36 and snapshots['w'] == 5, data
        assert snapshots['oldx'] == 40 and snapshots['x'] == 41, data
        assert snapshots['y'] == 43 and snapshots['z'] == 45, data
        assert snapshots['log'] == 3543463, data
        assert snapshots['plog'] == 244, data
        errors = {(item['function'], item['type'])
                  for item in data['errors']}
        assert ('fail_after', 'app.E') in errors, data
        assert ('cleanup_fail', 'app.E') in errors, data
    elif case == 'trait-self':
        # The final block keeps the bool ConstraintCall result, its deferred
        # write, and the later ordinary inout. The i32 block's moved Vec was
        # released and never restored to the moved binding. Python records the
        # typed failures from the throwing argument, receiver, and callee; the
        # scalar inout path leaves the frame borrow list empty.
        snapshots = data.get('snapshots', {})
        assert snapshots['log'] == 564, data
        assert snapshots['slot'] is True and snapshots['value'] is True, data
        assert snapshots['receiver'] is False and snapshots['saved'] is True, data
        assert snapshots['old'] is False and snapshots['next'] is True, data
        assert snapshots['owned']['items'] == [6], data
        assert snapshots['local']['items'] == [61], data
        assert snapshots['read'] == 61, data
        assert snapshots['first'] == 2 and snapshots['second'] == 3, data
        errors = {(item['function'], item['type'])
                  for item in data['errors']}
        assert ('bad', 'app.Failure') in errors, data
        assert ('bad_make', 'app.Failure') in errors, data
        assert ('fail', 'app.Failure') in errors, data
        assert ('relay_fail', 'app.Failure') in errors, data
    elif case == 'trait-generic-inout':
        snapshots = data.get('snapshots', {})
        assert snapshots['log'] == 112341149469569, data
        assert snapshots['x'] == 2 and snapshots['p'] == 20, data
        assert snapshots['q'] is False and snapshots['s'] == 41, data
        assert snapshots['saved_slot'] == 101, data
        errors = {(item['function'], item['type'])
                  for item in data['errors']}
        assert ('bad', 'app.Failure') in errors, data
        assert ('failed', 'app.Failure') in errors, data
    elif case == 'trait-qualified-inout':
        snapshots = data.get('snapshots', {})
        assert snapshots['log'] == 113342553, data
        assert snapshots['a'] == 2 and snapshots['b'] == 4, data
        assert snapshots['c'] == 6 and snapshots['d'] == 8, data
        assert snapshots['e'] is True and snapshots['h'] == 14, data
        assert snapshots['receiver_log'] == 7, data
    elif case == 'trait-qualified-own':
        snapshots = data.get('snapshots', {})
        assert snapshots['log'] == 77788, data
        assert snapshots['x'] == 2 and snapshots['a'] == 1, data
        assert snapshots['y'] == 4 and snapshots['b'] == 3, data
        assert snapshots['w'] == 6 and snapshots['c'] == 5, data
        assert snapshots['p'] == 7 and snapshots['q'] is False, data
        assert snapshots['r'] == 8 and snapshots['s'] is True, data
    elif case in ('trait-ordinary-holder-control',
                  'trait-ordinary-return-control',
                  'trait-function-cycle-reject',
                  'trait-type-inout-return-reject'):
        assert not data['allocations'], data
        assert data['main_return'] == 0, data
    elif case == 'trait-self-callback':
        # Factory-returned and directly passed free functions both write the
        # slot; the constrained forwarding path returns the old scalar in both
        # impls. The final throwing call preserves its deferred slot update but
        # frees the moved Vec and local Vec once each. Python metadata names the
        # final saved/failed locals, which are the last observed bindings.
        snapshots = data.get('snapshots', {})
        assert snapshots['saved'] == 20, data
        assert snapshots['slot'] == 51, data
        assert snapshots['log'] == 3469, data
        assert snapshots['direct'] is True and snapshots['forwarded'] is False, data
        errors = {(item['function'], item['type'])
                  for item in data['errors']}
        assert ('failed', 'app.Failure') in errors, data
    else:
        snapshots = data.get('snapshots', {})
        assert snapshots, data
        assert snapshots['third'] == 30 and snapshots['fourth'] == 40, data
        assert snapshots['sixth'] == 6 and snapshots['fifth'] == 55, data
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
    report = dict(task='T07 inherent method scalar inout', status='failed',
                  python=run([args.python, '--version']), runs=[],
                  sanitizer_scope='generated Native programs are not sanitizer-instrumented')
    cases = tuple(args.only) if args.only else CASES
    try:
        for case in cases:
            manifest = args.root / f'tests/generics/method-inout/{case}/lang.toml'
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
            for reject_case in REJECT_CASES:
                manifest = (args.root / 'tests/generics/method-inout' /
                            reject_case / 'lang.toml')
                sources = {str(path.relative_to(manifest.parent)): digest(path)
                           for path in sorted(manifest.parent.rglob('*'))
                           if path.is_file()}
                fresh = args.out / f'{reject_case}-new'
                shutil.rmtree(fresh, ignore_errors=True)
                before = run([args.cli, 'build', '--project', manifest,
                              '--backend', 'native', '--opt', 0,
                              '--out-dir', fresh, '--format', 'json'],
                             library=library)
                row = dict(case=f'{reject_case}-new', source_sha256=sources,
                           build=before, status='failed')
                report['runs'].append(row)
                assert before['exit'] != 0, before
                assert json.loads(before['stdout'])['status'] == 'error', before
                assert not (fresh / 'program').exists(), 'rejected build wrote program'
                assert not (fresh / 'program.o').exists(), 'rejected build wrote object'
                row['outputs'] = sorted(str(path.relative_to(fresh))
                                        for path in fresh.rglob('*') if path.is_file())
                row['status'] = 'passed'

                stale = args.out / f'{reject_case}-old'
                stale.mkdir(parents=True, exist_ok=True)
                sentinels = {name: b'previous output ' + name.encode()
                             for name in ('program', 'program.o')}
                for name, content in sentinels.items():
                    (stale / name).write_bytes(content)
                old_hashes = {name: digest(stale / name) for name in sentinels}
                after = run([args.cli, 'build', '--project', manifest,
                             '--backend', 'native', '--opt', 0,
                             '--out-dir', stale, '--format', 'json'],
                            library=library)
                row = dict(case=f'{reject_case}-old', source_sha256=sources,
                           build=after, before_sha256=old_hashes, status='failed')
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
