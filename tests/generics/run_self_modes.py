#!/usr/bin/env python3
"""Execute Type/T actual-Self move/inout fixtures and negative controls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True

POSITIVE = ('core', 'inout', 'cross-package', 'controls')
# Approved owning inout: these former reject sources compile and run now.
# They are checked, built and executed on both backends, but the ownership
# probe/allocator observation stays limited to POSITIVE.
OWNING_POSITIVE = ('reject-index-inout', 'reject-owning-self-inout',
                   'reject-trait-owning-inout')
EXPECTED_EVENTS = {
    'core': [2, 3, 7, 9, 4, 5],
    'inout': [3, 7, 9, 4, 5, 6, 5],
    'cross-package': [],
    'controls': [],
}
EXPECTED_ALLOCATIONS = {
    'core': 11,
    'inout': 1,
    'cross-package': 2,
    'controls': 4,
}
REJECTS = {
    'reject-value-move': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-value-inout': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-no-move': {'E_CALL_ARGUMENT'},
    'reject-move-read': {'E_OWNERSHIP_USE_AFTER_MOVE'},
    'reject-let-inout': {'E_CALL_ARGUMENT'},
    'reject-param-inout': {'E_CALL_ARGUMENT'},
    'reject-alias-inout': {'E_CALL_ARGUMENT'},
    'reject-field-inout': {'E_TYPE_MISMATCH'},
}
STRING_EXPECTED = {
    'probe_inherent': '值\0😀'.encode(),
    'probe_trait': '替\0换😀'.encode(),
    'probe_generic': '通\0用😀'.encode(),
}
BEFORE = ('inherent-move', 'inherent-inout', 'trait-move', 'trait-inout')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, timeout=120, library=None, cwd=None, raw=False):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    if library and library.is_dir():
        current = env.get('LD_LIBRARY_PATH')
        env['LD_LIBRARY_PATH'] = str(library) + (':' + current if current else '')
    completed = subprocess.run(argv, capture_output=True, env=env,
                               timeout=timeout, cwd=cwd)
    row = dict(argv=argv, exit=completed.returncode,
               stderr=completed.stderr.decode('utf-8', 'replace'))
    if raw:
        row['stdout_hex'] = completed.stdout.hex()
    else:
        row['stdout'] = completed.stdout.decode('utf-8', 'replace')
    return row


def ok(row, what):
    assert row['exit'] == 0, (what, row)


def diagnostic_codes(row):
    try:
        return {item.get('code') for item in
                json.loads(row['stdout']).get('diagnostics', [])}
    except (json.JSONDecodeError, TypeError):
        return set()


def source_hashes(project):
    return {str(path.relative_to(project)): digest(path)
            for path in sorted(project.rglob('*')) if path.is_file()}


def elf_checks(program):
    readelf = shutil.which('readelf')
    nm = shutil.which('nm')
    assert readelf and nm, 'readelf/nm are required'
    headers = run([readelf, '-l', program])
    dynamic = run([readelf, '-d', program])
    symbols = run([nm, '--undefined-only', program])
    ok(headers, 'readelf program headers')
    ok(dynamic, 'readelf dynamic section')
    ok(symbols, 'nm undefined symbols')
    assert 'INTERP' not in headers['stdout'], headers
    assert 'NEEDED' not in dynamic['stdout'], dynamic
    undefined = [line.strip() for line in symbols['stdout'].splitlines()
                 if line.strip()]
    strong = [line for line in undefined
              if line.split()[0] not in ('w', 'v')]
    assert not strong, ('strong undefined symbols', strong)
    for line in undefined:
        kind, name = line.split()
        assert kind == 'w' and name in {
            'lang_vec_observe_v1', 'lang_process_init_v1'}, line
    return dict(program_headers=headers, dynamic=dynamic,
                undefined=undefined, strong_undefined=strong)


def native_observations(text):
    seen, allocations, live = [], [], {}
    for line in text.splitlines():
        fields = line.split(':')
        if fields[0] == 'OBS':
            seen.append(('obs', int(fields[2]), int(fields[3])))
        elif fields[0] == 'VEC':
            event, identity = int(fields[1]), fields[2]
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


def assert_native(case, seen, allocations):
    events = [item[1] for item in seen if item[0] == 'obs']
    frees = [item[1] for item in seen if item[0] == 'free']
    assert events == EXPECTED_EVENTS[case], (case, events, seen)
    assert len(allocations) == EXPECTED_ALLOCATIONS[case], (case, allocations)
    assert sorted(frees) == sorted(allocations), (case, frees, allocations)
    assert len(set(frees)) == len(frees), (case, frees)


def assert_python(case, data):
    assert data['main_return'] == 0, data
    assert not data['alive'], data
    assert data.get('frames_clean', 0) >= 1, data
    assert data['events'] == EXPECTED_EVENTS[case], data
    assert len(data['allocations']) == EXPECTED_ALLOCATIONS[case], data
    assert sorted(data['frees']) == sorted(item['object']
                                          for item in data['allocations']), data
    assert len(set(data['frees'])) == len(data['frees']), data
    if case == 'core':
        snapshots = data['snapshots']
        assert snapshots['alias']['object'] == snapshots['r'][0]['object'], data
        assert snapshots['alias']['items'] == [7], data
    elif case == 'inout':
        snapshots = data['snapshots']
        assert snapshots['p'] == [7, 8], data
        assert snapshots['q'] == [15, 25], data
        assert snapshots['r'] == [10, 5], data
        assert snapshots['n'] == [23, 23], data
        assert snapshots['e'] == [30, 31], data
        assert snapshots['t'] == [44, 46], data


def parse_string_observation(data, metadata):
    width = metadata['target_word_bits'] // 8
    offset, result = 0, {}
    for name in metadata['probes']:
        assert offset + 2 * width <= len(data), ('missing lengths', name)
        length = int.from_bytes(data[offset:offset + width], 'little')
        offset += width
        runes = int.from_bytes(data[offset:offset + width], 'little')
        offset += width
        assert offset + length <= len(data), ('missing string bytes', name)
        result[name] = data[offset:offset + length]
        assert runes == len(STRING_EXPECTED[name].decode('utf-8')), (name, runes)
        offset += length
    assert offset == len(data), ('extra observation bytes', offset, len(data))
    return result


def check_source(cli, manifest, backend, library):
    row = run([cli, 'check', '--project', manifest, '--backend', backend,
               '--format', 'json'], library=library)
    ok(row, 'check ' + backend)
    assert json.loads(row['stdout'])['status'] == 'ok', row
    return row


def native_case(args, library, case, manifest, sources, report):
    for opt in (0, 2):
        row = dict(case=case, kind='native', opt=opt,
                   source_sha256=sources, status='failed')
        report['runs'].append(row)
        target = args.out / case / f'O{opt}'
        shutil.rmtree(target, ignore_errors=True)
        row['build'] = build = run(
            [args.cli, 'build', '--project', manifest, '--backend', 'native',
             '--opt', opt, '--out-dir', target / 'cli', '--format', 'json'],
            library=library)
        ok(build, 'native build')
        assert json.loads(build['stdout'])['status'] == 'ok', build
        row['execute'] = executed = run([target / 'cli/program'], library=library)
        ok(executed, 'native execute')
        assert not executed['stdout'] and not executed['stderr'], executed
        row['elf'] = elf_checks(target / 'cli/program')
        row['program_sha256'] = digest(target / 'cli/program')
        row['object_sha256'] = digest(target / 'cli/program.o')
        row['probe'] = probed = run(
            [args.probe, manifest, target / 'probe', opt], library=library)
        ok(probed, 'native ownership probe')
        row['probe_checks'] = json.loads(probed['stdout'])
        observe = run([args.clang, f'-O{opt}', target / 'probe/observe.ll',
                       args.runtime, args.vec_observer, '-o',
                       target / 'observe'], library=library)
        row['observe_build'] = observe
        ok(observe, 'native observer build')
        row['observe'] = observed = run([target / 'observe'], library=library)
        ok(observed, 'native observer execute')
        seen, allocations = native_observations(observed['stderr'])
        assert_native(case, seen, allocations)
        row['native_order'] = seen
        row['native_allocations'] = len(allocations)
        if case == 'inout':
            mutations = (target / 'probe/mutations.txt').read_text()
            assert ('actual-self-inout-parameter-slot-not-inout: '
                    'parameter slot type/mode mismatch') in mutations, mutations
            native_ir = (target / 'probe/native.txt').read_text()
            assert re.search(r'slot \$0 type=\d+ parameter=0 inout=1',
                             native_ir), native_ir[:2000]
            assert re.search(r'%0 = address .* \$0', native_ir), native_ir[:2000]
            assert 'store_indirect' in native_ir
            row['native_ir_inout_address'] = True
            row['native_ir_mutation'] = (
                'actual-self-inout-parameter-slot-not-inout')
        row['status'] = 'passed'


def python_case(args, library, case, manifest, sources, report):
    row = dict(case=case, kind='python', source_sha256=sources,
               status='failed')
    report['runs'].append(row)
    target = args.out / case / 'python'
    shutil.rmtree(target, ignore_errors=True)
    row['build'] = build = run(
        [args.cli, 'build', '--project', manifest, '--backend', 'python',
         '--out-dir', target, '--format', 'json'], library=library)
    ok(build, 'python build')
    assert json.loads(build['stdout'])['status'] == 'ok', build
    row['execute'] = executed = run([args.python, '-s', target / 'program.py'],
                                    library=library)
    ok(executed, 'python execute')
    assert not executed['stdout'] and not executed['stderr'], executed
    metadata_dir = args.out / case / 'python-metadata'
    shutil.rmtree(metadata_dir, ignore_errors=True)
    row['emit'] = emit = run(
        [args.helper, '--project', manifest, '--out-dir', metadata_dir],
        library=library)
    ok(emit, 'python helper')
    for name in ('program.py', 'langrt.py'):
        assert digest(target / name) == digest(metadata_dir / name), \
            'helper differs from CLI output: ' + name
    (target / 'metadata.json').write_bytes(
        (metadata_dir / 'metadata.json').read_bytes())
    row['observe'] = observed = run(
        [args.python, '-s', args.python_observer, target], library=library)
    ok(observed, 'python ownership observer')
    data = json.loads(observed['stdout'])
    assert_python(case, data)
    row['python_observation'] = data
    row['generated_sha256'] = {
        name: digest(target / name)
        for name in ('program.py', 'langrt.py', 'metadata.json')}
    if case == 'inout':
        assert '_rt.Cell(' in (target / 'program.py').read_text()
    row['status'] = 'passed'


def string_case(args, library, manifest, sources, report):
    for backend in ('native', 'python'):
        row = dict(case='strings', kind='check', backend=backend,
                   check=check_source(args.cli, manifest, backend, library),
                   source_sha256=sources, status='failed')
        report['runs'].append(row)
        row['status'] = 'passed'
    for opt in (0, 2):
        row = dict(case='strings', kind='native', opt=opt,
                   source_sha256=sources, status='failed')
        report['runs'].append(row)
        target = args.out / 'strings' / f'O{opt}'
        shutil.rmtree(target, ignore_errors=True)
        row['build'] = build = run(
            [args.cli, 'build', '--project', manifest, '--backend', 'native',
             '--opt', opt, '--out-dir', target / 'cli', '--format', 'json'],
            library=library)
        ok(build, 'string native build')
        assert json.loads(build['stdout'])['status'] == 'ok', build
        row['execute'] = executed = run([target / 'cli/program'], library=library)
        ok(executed, 'string native execute')
        assert not executed['stdout'] and not executed['stderr'], executed
        row['elf'] = elf_checks(target / 'cli/program')
        row['probe'] = probed = run(
            [args.string_probe, manifest, target / 'probe', opt],
            library=library)
        ok(probed, 'native string probe')
        metadata = json.loads((target / 'probe/metadata.json').read_text())
        assert metadata['status'] == 'passed', metadata
        observe = run([args.clang, f'-O{opt}', target / 'probe/observe.ll',
                       args.runtime, '-o', target / 'observe'], library=library)
        row['observe_build'] = observe
        ok(observe, 'string observer build')
        row['observe'] = observed = run([target / 'observe'], library=library,
                                        raw=True)
        ok(observed, 'string observer execute')
        actual = parse_string_observation(
            bytes.fromhex(observed['stdout_hex']), metadata)
        assert set(actual) == set(STRING_EXPECTED), actual
        row['string_bytes'] = {}
        for name, expected in STRING_EXPECTED.items():
            assert actual[name] == expected, (name, actual[name].hex(),
                                              expected.hex())
            row['string_bytes'][name] = dict(hex=actual[name].hex(),
                                             length=len(actual[name]))
        row['status'] = 'passed'
    row = dict(case='strings', kind='python', source_sha256=sources,
               status='failed')
    report['runs'].append(row)
    target = args.out / 'strings' / 'python'
    shutil.rmtree(target, ignore_errors=True)
    row['build'] = build = run(
        [args.cli, 'build', '--project', manifest, '--backend', 'python',
         '--out-dir', target, '--format', 'json'], library=library)
    ok(build, 'string python build')
    assert json.loads(build['stdout'])['status'] == 'ok', build
    row['execute'] = executed = run([args.python, '-s', target / 'program.py'],
                                    library=library)
    ok(executed, 'string python execute')
    assert not executed['stdout'] and not executed['stderr'], executed
    metadata_dir = args.out / 'strings' / 'python-metadata'
    shutil.rmtree(metadata_dir, ignore_errors=True)
    row['emit'] = emit = run(
        [args.helper, '--project', manifest, '--out-dir', metadata_dir],
        library=library)
    ok(emit, 'string python helper')
    metadata = json.loads((metadata_dir / 'metadata.json').read_text())
    symbols = {item['source_name']: item['generated']
               for item in metadata['symbols']
               if item['source_name'] in STRING_EXPECTED}
    assert set(symbols) == set(STRING_EXPECTED), symbols
    code = (
        'import program,json\nresult={}\n'
        f'for name,generated in {symbols!r}.items():\n'
        ' value=getattr(program,generated)()\n'
        ' assert type(value) is str\n'
        ' data=value.encode("utf-8")\n'
        ' result[name]={"hex":data.hex(),"byte_length":len(data)}\n'
        'print(json.dumps(result,ensure_ascii=True))\n')
    row['observe'] = imported = run([args.python, '-s', '-c', code],
                                    cwd=target, library=library)
    ok(imported, 'python string import')
    actual = json.loads(imported['stdout'])
    row['string_bytes'] = actual
    for name, expected in STRING_EXPECTED.items():
        assert actual[name]['hex'] == expected.hex(), (name, actual[name],
                                                       expected.hex())
        assert actual[name]['byte_length'] == len(expected), (name, actual)
    row['status'] = 'passed'


def reject_case(args, library, name, expected, report):
    manifest = args.root / 'tests/generics/self-modes' / name / 'lang.toml'
    sources = source_hashes(manifest.parent)
    for backend in ('native', 'python'):
        row = dict(case=name, kind='check', backend=backend,
                   source_sha256=sources, status='failed')
        report['runs'].append(row)
        checked = run([args.cli, 'check', '--project', manifest,
                       '--backend', backend, '--format', 'json'],
                      library=library)
        row['check'] = checked
        assert checked['exit'] != 0, (name, checked)
        assert json.loads(checked['stdout'])['status'] == 'error', checked
        found = diagnostic_codes(checked)
        assert found and found <= expected, (name, found, expected, checked)
        row['diagnostic_codes'] = sorted(found)
        row['status'] = 'passed'
    for backend in ('native', 'python'):
        fresh = args.out / 'rejects' / name / f'{backend}-fresh'
        shutil.rmtree(fresh, ignore_errors=True)
        row = dict(case=name, kind='fresh', backend=backend,
                   source_sha256=sources, status='failed')
        report['runs'].append(row)
        build = run([args.cli, 'build', '--project', manifest,
                     '--backend', backend, '--opt', 0,
                     '--out-dir', fresh, '--format', 'json'], library=library)
        row['build'] = build
        assert build['exit'] != 0, (name, backend, build)
        assert json.loads(build['stdout'])['status'] == 'error', build
        row['outputs'] = sorted(str(path.relative_to(fresh))
                                for path in fresh.rglob('*') if path.is_file())
        assert not row['outputs'], (name, backend, row['outputs'])
        row['status'] = 'passed'

        stale = args.out / 'rejects' / name / f'{backend}-stale'
        stale.mkdir(parents=True, exist_ok=True)
        sentinels = {suffix: b'previous output ' + suffix.encode()
                     for suffix in ('program', 'program.o', 'program.py',
                                    'langrt.py', 'metadata.json')}
        for suffix, content in sentinels.items():
            (stale / suffix).write_bytes(content)
        before = {suffix: digest(stale / suffix) for suffix in sentinels}
        row = dict(case=name, kind='stale', backend=backend,
                   source_sha256=sources, before_sha256=before,
                   status='failed')
        report['runs'].append(row)
        build = run([args.cli, 'build', '--project', manifest,
                     '--backend', backend, '--opt', 0,
                     '--out-dir', stale, '--format', 'json'], library=library)
        row['build'] = build
        assert build['exit'] != 0, (name, backend, build)
        assert json.loads(build['stdout'])['status'] == 'error', build
        row['after_sha256'] = {
            suffix: digest(stale / suffix) for suffix in sentinels}
        assert row['after_sha256'] == before, (name, backend, row)
        row['status'] = 'passed'


def before_case(args, library, report):
    if not args.before:
        return
    for name in BEFORE:
        manifest = args.before / name / 'lang.toml'
        sources = source_hashes(manifest.parent)
        row = dict(case='before-' + name, source_sha256=sources,
                   status='failed')
        report['runs'].append(row)
        row['checks'] = [
            check_source(args.cli, manifest, backend, library)
            for backend in ('native', 'python')]
        for opt in (0, 2):
            target = args.out / 'before' / name / f'O{opt}'
            shutil.rmtree(target, ignore_errors=True)
            row[f'native_O{opt}_build'] = build = run(
                [args.cli, 'build', '--project', manifest, '--backend',
                 'native', '--opt', opt, '--out-dir', target / 'cli',
                 '--format', 'json'], library=library)
            ok(build, 'before native build')
            assert json.loads(build['stdout'])['status'] == 'ok', build
            row[f'native_O{opt}_execute'] = executed = run(
                [target / 'cli/program'], library=library)
            ok(executed, 'before native execute')
            assert not executed['stdout'] and not executed['stderr'], executed
            row[f'native_O{opt}_elf'] = elf_checks(target / 'cli/program')
        target = args.out / 'before' / name / 'python'
        shutil.rmtree(target, ignore_errors=True)
        row['python_build'] = build = run(
            [args.cli, 'build', '--project', manifest, '--backend', 'python',
             '--out-dir', target, '--format', 'json'], library=library)
        ok(build, 'before python build')
        assert json.loads(build['stdout'])['status'] == 'ok', build
        row['python_execute'] = executed = run(
            [args.python, '-s', target / 'program.py'], library=library)
        ok(executed, 'before python execute')
        assert not executed['stdout'] and not executed['stderr'], executed
        assert source_hashes(manifest.parent) == sources, \
            'before source changed: ' + name
        row['status'] = 'passed'


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'probe', 'string_probe', 'helper', 'root', 'clang',
                 'runtime', 'python', 'out'):
        parser.add_argument('--' + name.replace('_', '-'), type=Path,
                            required=True)
    parser.add_argument('--before', type=Path)
    parser.add_argument('--only', action='append',
                        choices=(POSITIVE + OWNING_POSITIVE +
                                 tuple(REJECTS) + ('strings',)))
    args = parser.parse_args()
    for name in ('cli', 'probe', 'string_probe', 'helper', 'root', 'clang',
                 'runtime', 'python', 'out'):
        setattr(args, name, getattr(args, name).resolve())
    if args.before:
        args.before = args.before.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    library = next((candidate for candidate in (
        args.clang.parent.parent / 'libxml2-legacy-2.13.9/usr/lib',
        args.clang.parent.parent.parent / 'libxml2-legacy-2.13.9/usr/lib')
        if candidate.is_dir()), None)
    args.python_observer = args.root / 'tests/python/observe_ownership.py'
    args.vec_observer = args.root / 'tests/native/vec_observer.cpp'
    report = dict(task='explicit Type/T actual Self move/inout',
                  status='failed', python=run([args.python, '--version']),
                  runs=[])
    selected = set(args.only) if args.only else (
        set(POSITIVE) | set(REJECTS) | set(OWNING_POSITIVE) | {'strings'})
    try:
        for case in POSITIVE:
            if case not in selected:
                continue
            manifest = args.root / 'tests/generics/self-modes' / case / 'lang.toml'
            sources = source_hashes(manifest.parent)
            for backend in ('native', 'python'):
                row = dict(case=case, kind='check', backend=backend,
                           check=check_source(args.cli, manifest, backend,
                                              library),
                           source_sha256=sources, status='passed')
                report['runs'].append(row)
            native_case(args, library, case, manifest, sources, report)
            python_case(args, library, case, manifest, sources, report)
        for case in OWNING_POSITIVE:
            if case not in selected:
                continue
            manifest = (args.root / 'tests/generics/self-modes' / case /
                        'lang.toml')
            sources = source_hashes(manifest.parent)
            for backend in ('native', 'python'):
                row = dict(case=case, kind='check', backend=backend,
                           check=check_source(args.cli, manifest, backend,
                                              library),
                           source_sha256=sources, status='passed')
                report['runs'].append(row)
            for opt in (0, 2):
                row = dict(case=case, kind='native', opt=opt,
                           source_sha256=sources, status='failed')
                report['runs'].append(row)
                target = args.out / case / f'O{opt}'
                shutil.rmtree(target, ignore_errors=True)
                row['build'] = build = run(
                    [args.cli, 'build', '--project', manifest,
                     '--backend', 'native', '--opt', opt,
                     '--out-dir', target / 'cli', '--format', 'json'],
                    library=library)
                ok(build, 'native build')
                assert json.loads(build['stdout'])['status'] == 'ok', build
                row['execute'] = executed = run([target / 'cli/program'],
                                                library=library)
                ok(executed, 'native execute')
                assert not executed['stdout'] and not executed['stderr'], executed
                row['status'] = 'passed'
            row = dict(case=case, kind='python', source_sha256=sources,
                       status='failed')
            report['runs'].append(row)
            destination = args.out / case / 'python'
            shutil.rmtree(destination, ignore_errors=True)
            row['build'] = build = run(
                [args.cli, 'build', '--project', manifest,
                 '--backend', 'python', '--out-dir', destination,
                 '--format', 'json'], library=library)
            ok(build, 'python build')
            assert json.loads(build['stdout'])['status'] == 'ok', build
            row['execute'] = executed = run(
                [args.python, '-s', destination / 'program.py'],
                library=library)
            ok(executed, 'python execute')
            assert not executed['stdout'] and not executed['stderr'], executed
            row['status'] = 'passed'
        if 'strings' in selected:
            manifest = args.root / 'tests/generics/self-modes/strings/lang.toml'
            string_case(args, library, manifest, source_hashes(manifest.parent),
                        report)
        for name, expected in REJECTS.items():
            if name in selected:
                reject_case(args, library, name, expected, report)
        before_case(args, library, report)
        report['status'] = 'passed'
    except Exception as error:
        report['error'] = repr(error)
        raise
    finally:
        (args.out / 'results.json').write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    passed = sum(r['status'] == 'passed' for r in report['runs'])
    print(json.dumps(dict(status=report['status'], passed=passed,
                          failed=len(report['runs']) - passed,
                          runs=len(report['runs']))))


if __name__ == '__main__':
    main()
