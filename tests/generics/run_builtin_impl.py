#!/usr/bin/env python3
"""Run built-in Vec/string/Unit impls on both backends.

The Native side inspects actual generated Vec allocations/frees and string
bytes through the existing private probes. The Python side imports the
generated program and uses the existing ownership observer. Rejected projects
must leave fresh output empty and preserve stale output bytes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True

POSITIVE = ('vec-fixed', 'vec-generic', 'string', 'unit', 'cross-package')
VEC_CASES = ('vec-fixed', 'vec-generic')
VEC_EVENTS = {
    'vec-fixed': [1, 4, 5, 2, 9, 2, 9, 1],
    'vec-generic': [1, 4, 5, 2, 9, 2, 9, 1],
}
VEC_ALLOCATIONS = {'vec-fixed': 3, 'vec-generic': 3}
VEC_LOG = 14529291
UNIT_EVENTS = [3, 3, 4, 4, 8, 6, 5, 6]
UNIT_LOG = 33448656
STRING_EXPECTED = {
    'probe_echo': '值\0😀'.encode('utf-8'),
    'probe_choose_true': '真\0值'.encode('utf-8'),
    'probe_choose_false': '替\0换😀'.encode('utf-8'),
    'probe_generic': '通\0用😀'.encode('utf-8'),
    'probe_type': '类\0型'.encode('utf-8'),
    'probe_trait': '特\0征'.encode('utf-8'),
    'probe_trait_type': '显\0式'.encode('utf-8'),
    'probe_static': '静\0态'.encode('utf-8'),
}
REJECT = {
    'reject-ambiguous': {'E_TYPE_MISMATCH'},
    'reject-overlap': {'E_TYPE_MISMATCH'},
}
BEFORE = ('vec', 'string', 'unit')
FORBIDDEN = {'E_IR_INVALID', 'E_INTERNAL_ERROR'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, timeout=180, library=None, cwd=None):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    if library and library.is_dir():
        current = env.get('LD_LIBRARY_PATH')
        env['LD_LIBRARY_PATH'] = str(library) + (':' + current if current else '')
    completed = subprocess.run(argv, capture_output=True, text=True,
                               env=env, timeout=timeout, cwd=cwd)
    return dict(argv=argv, cwd=str(cwd or Path.cwd()),
                env={key: env.get(key) for key in
                     ('LD_LIBRARY_PATH', 'PYTHONDONTWRITEBYTECODE',
                      'PYTHONNOUSERSITE')},
                exit=completed.returncode, stdout=completed.stdout,
                stderr=completed.stderr)


def run_raw(argv, timeout=180, library=None, cwd=None):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    if library and library.is_dir():
        current = env.get('LD_LIBRARY_PATH')
        env['LD_LIBRARY_PATH'] = str(library) + (':' + current if current else '')
    completed = subprocess.run(argv, capture_output=True, env=env,
                               timeout=timeout, cwd=cwd)
    return dict(argv=argv, exit=completed.returncode,
                stdout_hex=completed.stdout.hex(),
                stderr=completed.stderr.decode('utf-8', 'replace'))


def ok(row, what):
    assert row['exit'] == 0, (what, row)


def codes(row):
    try:
        return [item.get('code') for item in
                json.loads(row['stdout']).get('diagnostics', [])]
    except (json.JSONDecodeError, TypeError):
        return []


def assert_reject(build, expected, what):
    assert build['exit'] != 0, (what, build)
    assert json.loads(build['stdout'])['status'] == 'error', (what, build)
    found = codes(build)
    assert found and set(found) <= expected, (what, found, build)
    assert not (set(found) & FORBIDDEN), (what, found, build)


def source_hashes(project):
    return {str(path.relative_to(project)): digest(path)
            for path in sorted(project.rglob('*')) if path.is_file()}


def native_observations(text):
    seen, allocations, live = [], [], {}
    for line in text.splitlines():
        fields = line.split(':')
        if fields[0] == 'OBS':
            kind = int(fields[1])
            seen.append(('mark' if kind == 1 else 'probe',
                         int(fields[2]), int(fields[3])))
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


def assert_native_vec(case, seen, allocations):
    marks = [item[1] for item in seen if item[0] == 'mark']
    probes = [item[1] for item in seen if item[0] == 'probe']
    frees = [item[1] for item in seen if item[0] == 'free']
    assert marks == VEC_EVENTS[case], (case, marks, seen)
    assert probes == [0], (case, probes, seen)
    assert len(allocations) == VEC_ALLOCATIONS[case], (case, allocations, seen)
    assert sorted(frees) == sorted(allocations), (case, frees, allocations)
    assert len(set(frees)) == len(frees), (case, frees)
    assert seen[-1] == ('probe', 0, VEC_LOG), (case, seen[-1])


def assert_native_unit(seen, allocations):
    marks = [item[1] for item in seen if item[0] == 'mark']
    probes = [item[1] for item in seen if item[0] == 'probe']
    assert marks == UNIT_EVENTS, (marks, seen)
    assert probes == [0], (probes, seen)
    assert not allocations, allocations
    assert seen[-1] == ('probe', 0, UNIT_LOG), seen[-1]


def parse_string_observation(data, metadata):
    width = metadata['target_word_bits'] // 8
    offset, result = 0, {}
    for name in metadata['probes']:
        assert offset + width <= len(data), ('missing length', name)
        length = int.from_bytes(data[offset:offset + width], 'little')
        offset += width
        result[name] = data[offset:offset + length]
        offset += length
    assert offset == len(data), ('extra observation bytes', offset, len(data))
    return result


def elf_checks(program):
    readelf = shutil.which('readelf')
    assert readelf, 'readelf is required for ELF checks'
    program_header = run([readelf, '-l', program])
    dynamic = run([readelf, '-d', program])
    ok(program_header, 'readelf program headers')
    ok(dynamic, 'readelf dynamic section')
    assert 'INTERP' not in program_header['stdout'], program_header
    assert 'NEEDED' not in dynamic['stdout'], dynamic
    return dict(program_headers=program_header, dynamic=dynamic)


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'probe', 'helper', 'root', 'clang', 'runtime',
                 'python', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--string-probe', type=Path, required=True)
    parser.add_argument('--before', type=Path)
    args = parser.parse_args()
    for name in ('cli', 'probe', 'string_probe', 'helper', 'root', 'clang',
                 'runtime', 'python', 'out'):
        setattr(args, name, getattr(args, name).resolve())
    if args.before:
        args.before = args.before.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    library = next((candidate for candidate in (
        args.cli.parent.parent / 'toolchains/libxml2-legacy-2.13.9/usr/lib',
        args.cli.parent.parent.parent / 'toolchains/libxml2-legacy-2.13.9/usr/lib',
    ) if candidate.is_dir()), None)
    if library is None:
        library = next((Path(entry) for entry in
                        os.environ.get('LD_LIBRARY_PATH', '').split(':')
                        if 'libxml2-legacy' in entry and Path(entry).is_dir()),
                       None)
    observer = args.root / 'tests/python/observe_ownership.py'
    vec_observer = args.root / 'tests/native/vec_observer.cpp'
    report = dict(
        task='built-in Vec/string/Unit trait and inherent impl dispatch',
        operation_id='builtin-vec-string-unit-impl-4b99042-20261109',
        status='failed',
        tools={
            'cli': {'path': str(args.cli), 'sha256': digest(args.cli)},
            'ownership_probe': {'path': str(args.probe),
                                'sha256': digest(args.probe)},
            'string_probe': {'path': str(args.string_probe),
                             'sha256': digest(args.string_probe)},
            'helper': {'path': str(args.helper),
                       'sha256': digest(args.helper)},
            'clang': {'path': str(args.clang),
                      'version': run([args.clang, '--version'])},
            'python': {'path': str(args.python),
                       'version': run([args.python, '--version'])},
        },
        sanitizer_scope='generated Native programs are not sanitizer-instrumented',
        runs=[],
    )
    cases = args.root / 'tests/generics/builtin-impl'
    try:
        for case in POSITIVE:
            project = cases / case
            manifest = project / 'lang.toml'
            hashes = source_hashes(project)
            for backend in ('native', 'python'):
                row = dict(case=case, kind='check', backend=backend,
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                row['check'] = check = run(
                    [args.cli, 'check', '--project', manifest,
                     '--backend', backend, '--format', 'json'],
                    library=library)
                ok(check, 'check ' + backend)
                assert json.loads(check['stdout'])['status'] == 'ok', check
                row['status'] = 'passed'

            for opt in (0, 2):
                row = dict(case=case, kind='native', opt=opt,
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                native = args.out / case / f'O{opt}'
                shutil.rmtree(native, ignore_errors=True)
                row['build'] = build = run(
                    [args.cli, 'build', '--project', manifest,
                     '--backend', 'native', '--opt', opt,
                     '--out-dir', native / 'cli', '--format', 'json'],
                    library=library)
                ok(build, 'native build')
                assert json.loads(build['stdout'])['status'] == 'ok', build
                row['execute'] = executed = run([native / 'cli/program'],
                                                library=library)
                ok(executed, 'native execute')
                assert not executed['stdout'] and not executed['stderr'], executed
                row['elf'] = elf_checks(native / 'cli/program')
                row['program_sha256'] = digest(native / 'cli/program')
                row['object_sha256'] = digest(native / 'cli/program.o')

                if case in VEC_CASES or case == 'unit':
                    row['probe'] = probed = run(
                        [args.probe, manifest, native / 'probe', opt],
                        library=library)
                    ok(probed, 'native ownership probe')
                    row['probe_result'] = json.loads(probed['stdout'])
                    observe = run(
                        [args.clang, f'-O{opt}', native / 'probe/observe.ll',
                         args.runtime, vec_observer, '-o', native / 'observe'],
                        library=library)
                    row['observe_build'] = observe
                    ok(observe, 'native observe build')
                    row['observe'] = observed = run([native / 'observe'],
                                                    library=library)
                    ok(observed, 'native observe execute')
                    seen, allocations = native_observations(observed['stderr'])
                    if case in VEC_CASES:
                        assert_native_vec(case, seen, allocations)
                    else:
                        assert_native_unit(seen, allocations)
                    row['native_allocations'] = len(allocations)
                    row['native_observations'] = seen
                elif case == 'string':
                    row['probe'] = probed = run(
                        [args.string_probe, manifest, native / 'probe', opt],
                        library=library)
                    ok(probed, 'native string probe')
                    metadata = json.loads(
                        (native / 'probe/metadata.json').read_text())
                    assert metadata['status'] == 'passed', metadata
                    observe = run(
                        [args.clang, f'-O{opt}', native / 'probe/observe.ll',
                         args.runtime, '-o', native / 'observe'],
                        library=library)
                    row['observe_build'] = observe
                    ok(observe, 'native string observe build')
                    row['observe'] = observed = run_raw([native / 'observe'],
                                                        library=library)
                    ok(observed, 'native string observe execute')
                    actual = parse_string_observation(
                        bytes.fromhex(observed['stdout_hex']), metadata)
                    assert set(actual) == set(STRING_EXPECTED), actual.keys()
                    row['string_bytes'] = {}
                    for name, expected in STRING_EXPECTED.items():
                        assert actual[name] == expected, (
                            name, actual[name].hex(), expected.hex())
                        row['string_bytes'][name] = dict(
                            hex=actual[name].hex(), length=len(actual[name]))
                    nir = (native / 'probe/strings.nir').read_text()
                    llvm = (native / 'probe/before.ll').read_text()
                    assert 'retain' not in nir and 'release' not in nir, nir
                    assert 'lang_vec_retain' not in llvm, llvm
                    assert 'lang_vec_release' not in llvm, llvm
                    row['string_no_retain'] = True
                row['status'] = 'passed'

            row = dict(case=case, kind='python', source_sha256=hashes,
                       status='failed')
            report['runs'].append(row)
            python = args.out / case / 'python'
            shutil.rmtree(python, ignore_errors=True)
            row['build'] = build = run(
                [args.cli, 'build', '--project', manifest,
                 '--backend', 'python', '--out-dir', python,
                 '--format', 'json'], library=library)
            ok(build, 'python build')
            assert json.loads(build['stdout'])['status'] == 'ok', build
            row['execute'] = executed = run(
                [args.python, '-s', python / 'program.py'], library=library)
            ok(executed, 'python execute')
            assert not executed['stdout'] and not executed['stderr'], executed
            metadata_dir = args.out / case / 'python-metadata'
            shutil.rmtree(metadata_dir, ignore_errors=True)
            row['emit'] = emit = run(
                [args.helper, '--project', manifest,
                 '--out-dir', metadata_dir], library=library)
            ok(emit, 'python helper')
            row['generated_sha256'] = {
                name: digest(python / name)
                for name in ('program.py', 'langrt.py')}
            for name in ('program.py', 'langrt.py'):
                assert row['generated_sha256'][name] == digest(metadata_dir / name), \
                    'helper differs from CLI output: ' + name
            metadata = json.loads((metadata_dir / 'metadata.json').read_text())
            (python / 'metadata.json').write_bytes(
                (metadata_dir / 'metadata.json').read_bytes())

            if case == 'string':
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
                row['observe_import'] = imported = run(
                    [args.python, '-s', '-c', code], cwd=python,
                    library=library)
                ok(imported, 'python string import')
                actual = json.loads(imported['stdout'])
                row['string_bytes'] = actual
                assert set(actual) == set(STRING_EXPECTED), actual
                for name, expected in STRING_EXPECTED.items():
                    assert actual[name]['hex'] == expected.hex(), (
                        name, actual[name], expected.hex())
                    assert actual[name]['byte_length'] == len(expected), (
                        name, actual[name], len(expected))

            row['observe'] = observed = run(
                [args.python, '-s', observer, python], library=library)
            ok(observed, 'python ownership observer')
            data = json.loads(observed['stdout'])
            row['python_observation'] = data
            assert data['main_return'] == 0 and not data['alive'], data
            if case in VEC_CASES:
                assert data['events'] == VEC_EVENTS[case], data
                assert len(data['allocations']) == VEC_ALLOCATIONS[case], data
                assert sorted(data['frees']) == sorted(
                    item['object'] for item in data['allocations']), data
                assert len(set(data['frees'])) == len(data['frees']), data
            elif case == 'unit':
                assert data['events'] == UNIT_EVENTS, data
                assert not data['allocations'] and not data['frees'], data
            elif case == 'string':
                assert not data['allocations'] and not data['frees'], data
                assert data['events'] == [], data
            row['status'] = 'passed'

        for case, expected in REJECT.items():
            project = cases / case
            manifest = project / 'lang.toml'
            hashes = source_hashes(project)
            for backend in ('native', 'python'):
                row = dict(case=case, kind='reject-check', backend=backend,
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                row['check'] = check = run(
                    [args.cli, 'check', '--project', manifest,
                     '--backend', backend, '--format', 'json'],
                    library=library)
                assert_reject(check, expected, (case, backend))
                row['diagnostic_codes'] = codes(check)
                row['status'] = 'passed'

            for backend in ('native', 'python'):
                fresh = args.out / 'reject' / case / (backend + '-new')
                shutil.rmtree(fresh, ignore_errors=True)
                row = dict(case=case, kind='reject-fresh', backend=backend,
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                argv = [args.cli, 'build', '--project', manifest,
                        '--backend', backend, '--out-dir', fresh,
                        '--format', 'json']
                if backend == 'native':
                    argv += ['--opt', '0']
                row['build'] = build = run(argv, library=library)
                assert_reject(build, expected, (case, backend, 'fresh'))
                row['outputs'] = sorted(
                    str(path.relative_to(fresh))
                    for name in ('program', 'program.o', 'program.py',
                                 'langrt.py', 'metadata.json')
                    for path in [fresh / name] if path.exists())
                assert not row['outputs'], row
                row['diagnostic_codes'] = codes(build)
                row['status'] = 'passed'

                stale = args.out / 'reject' / case / (backend + '-old')
                stale.mkdir(parents=True, exist_ok=True)
                names = (('program', 'program.o') if backend == 'native'
                         else ('program.py', 'langrt.py', 'metadata.json'))
                sentinels = {name: b'previous output ' + name.encode()
                             for name in names}
                for name, content in sentinels.items():
                    (stale / name).write_bytes(content)
                before = {name: digest(stale / name) for name in sentinels}
                row = dict(case=case, kind='reject-stale', backend=backend,
                           source_sha256=hashes, before_sha256=before,
                           status='failed')
                report['runs'].append(row)
                argv = [args.cli, 'build', '--project', manifest,
                        '--backend', backend, '--out-dir', stale,
                        '--format', 'json']
                if backend == 'native':
                    argv += ['--opt', '0']
                row['build'] = build = run(argv, library=library)
                assert_reject(build, expected, (case, backend, 'stale'))
                row['after_sha256'] = {name: digest(stale / name)
                                       for name in sentinels}
                assert row['after_sha256'] == before, row
                row['diagnostic_codes'] = codes(build)
                row['status'] = 'passed'

        if args.before:
            for case in BEFORE:
                project = args.before / case
                manifest = project / 'lang.toml'
                hashes = source_hashes(project)
                for backend in ('native', 'python'):
                    row = dict(case='before-' + case, kind='check',
                               backend=backend, source_sha256=hashes,
                               status='failed')
                    report['runs'].append(row)
                    row['check'] = check = run(
                        [args.cli, 'check', '--project', manifest,
                         '--backend', backend, '--format', 'json'],
                        library=library)
                    ok(check, 'before check ' + backend)
                    assert json.loads(check['stdout'])['status'] == 'ok', check
                    row['status'] = 'passed'
                for opt in (0, 2):
                    row = dict(case='before-' + case, kind='native', opt=opt,
                               source_sha256=hashes, status='failed')
                    report['runs'].append(row)
                    native = args.out / 'before' / case / f'O{opt}'
                    shutil.rmtree(native, ignore_errors=True)
                    row['build'] = build = run(
                        [args.cli, 'build', '--project', manifest,
                         '--backend', 'native', '--opt', opt,
                         '--out-dir', native / 'cli', '--format', 'json'],
                        library=library)
                    ok(build, 'before native build')
                    row['execute'] = executed = run(
                        [native / 'cli/program'], library=library)
                    ok(executed, 'before native execute')
                    assert not executed['stdout'] and not executed['stderr'], executed
                    row['elf'] = elf_checks(native / 'cli/program')
                    row['status'] = 'passed'
                row = dict(case='before-' + case, kind='python',
                           source_sha256=hashes, status='failed')
                report['runs'].append(row)
                python = args.out / 'before' / case / 'python'
                shutil.rmtree(python, ignore_errors=True)
                row['build'] = build = run(
                    [args.cli, 'build', '--project', manifest,
                     '--backend', 'python', '--out-dir', python,
                     '--format', 'json'], library=library)
                ok(build, 'before python build')
                row['execute'] = executed = run(
                    [args.python, '-s', python / 'program.py'],
                    library=library)
                ok(executed, 'before python execute')
                assert not executed['stdout'] and not executed['stderr'], executed
                row['status'] = 'passed'

        report['status'] = 'passed'
    finally:
        (args.out / 'results.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(
        status=report['status'], runs=len(report['runs']),
        failed=[(row['case'], row.get('kind'), row.get('backend'))
                for row in report['runs'] if row['status'] != 'passed'],
        report=str(args.out / 'results.json'))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
