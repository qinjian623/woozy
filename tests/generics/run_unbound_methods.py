#!/usr/bin/env python3
"""Execute unbound Type/T method-value fixtures on Native and Python."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True

NATIVE_CASES = {'core', 'move-inout', 'throws'}
SIMPLE_CASES = {'cross-package', 'public-const-phantom', 'local-const-aliases',
                'local-const-call-controls'}
EXPECTED_ALLOCATIONS = {'core': 2, 'move-inout': 6, 'throws': 6}
EXPECTED_VEC_ALLOCATIONS = {'core': 1, 'move-inout': 3, 'throws': 3}
EXPECTED_NATIVE_IR_REJECTIONS = {'core': 11, 'move-inout': 13, 'throws': 11}
EXPECTED_MUTATIONS = {
    'core': set(),
    'move-inout': {'actual-self-inout-parameter-slot-not-inout'},
    'throws': set(),
}
EXPECTED_EVENTS = {
    'core': [],
    'move-inout': [],
    'throws': [(8, 8), (9, 89), (1, 1), (6, 16), (2, 162), (1, 1621),
               (6, 16216), (3, 162163), (4, 1621634), (7, 16216347)],
}
EXPECTED_PYTHON_EVENTS = {
    'core': [],
    'move-inout': [],
    'throws': [8, 9, 1, 6, 2, 1, 6, 3, 4, 7],
}
STRING_EXPECTED = {
    'probe_value': '值\0😀'.encode(),
    'probe_argument': '替\0换😀'.encode(),
    'probe_generic': '通\0用😀'.encode(),
}
LOCAL_STRING_EXPECTED = {
    'probe_alias_value': '值\0😀'.encode(),
    'probe_alias_argument': '替\0换😀'.encode(),
    'probe_alias_generic': '通\0用😀'.encode(),
}
STRING_CASES = {
    'strings': STRING_EXPECTED,
    'local-const-alias-strings': LOCAL_STRING_EXPECTED,
}
REJECTS = {
    'reject-const-private-result': {'E_PRIVATE_TYPE_LEAK'},
    'reject-const-private-parameter': {'E_PRIVATE_TYPE_LEAK'},
    'reject-const-private-throws': {'E_PRIVATE_TYPE_LEAK'},
    'reject-ambiguous': {'E_TYPE_MISMATCH'},
    'reject-phantom': {'E_TYPE_MISMATCH'},
    'reject-expected-mode': {'E_TYPE_MISMATCH'},
    'reject-private': {'E_PRIVATE_ACCESS'},
    'reject-capture': {'E_TYPE_MISMATCH'},
    'reject-named': {'E_CALL_ARGUMENT'},
    'reject-constructor': {'E_TYPE_MISMATCH'},
    'reject-vec-inout': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-local-const-ordinary': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-local-const-call': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-local-const-field': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-local-const-index': {'E_CAPABILITY_UNAVAILABLE'},
    'reject-local-const-forward': {'E_NAME_NOT_FOUND'},
    'reject-local-const-out-of-scope': {'E_NAME_NOT_FOUND'},
    'reject-local-const-readonly': {'E_TYPE_MISMATCH'},
    'reject-local-const-cycle': {'E_CONSTANT_CYCLE'},
}
BEFORE = ('ordinary-self', 'static-factory', 'impl-own-generics',
          'constrained-full-trait')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, timeout=180, library=None, cwd=None, raw=False):
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


def check_source(cli, manifest, backend, library):
    row = run([cli, 'check', '--project', manifest, '--backend', backend,
               '--format', 'json'], library=library)
    ok(row, 'check ' + backend)
    assert json.loads(row['stdout'])['status'] == 'ok', row
    return row


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
        assert line.split()[-1] == 'lang_vec_observe_v1', line
    return dict(program_headers=headers, dynamic=dynamic,
                undefined=undefined, strong_undefined=strong)


def parse_observer(text):
    live, allocations, frees = {}, [], []
    vec_live, vec_allocations, vec_frees = {}, [], []
    events = []
    for line in text.splitlines():
        fields = line.split(':')
        if fields[0] == 'ALLOC':
            pointer = fields[1]
            assert pointer != '(nil)' and pointer not in live, line
            token = len(allocations)
            live[pointer] = token
            allocations.append(dict(token=token, pointer=pointer))
        elif fields[0] == 'FREE':
            pointer = fields[1]
            assert pointer in live, line
            frees.append(live.pop(pointer))
        elif fields[0] == 'VEC':
            event, identity, detail = int(fields[1]), fields[2], int(fields[3])
            if event == 1:
                assert identity not in vec_live, line
                vec_live[identity] = dict(token=len(vec_allocations),
                                          length=detail, drops=[])
                vec_allocations.append(identity)
            elif event == 5:
                value = vec_live.pop(identity)
                assert value['drops'] == list(range(value['length'] - 1, -1, -1)), value
                vec_frees.append(value['token'])
            else:
                assert identity in vec_live, line
                if event == 4:
                    vec_live[identity]['drops'].append(detail)
        elif fields[0] == 'OBS':
            events.append((int(fields[2]), int(fields[3])))
        else:
            raise AssertionError(line)
    return dict(allocations=allocations, frees=frees, live=live,
                vec_allocations=vec_allocations, vec_frees=vec_frees,
                vec_live=vec_live, events=events)


def assert_observer(case, data):
    assert not data['live'] and not data['vec_live'], (case, data)
    assert len(data['allocations']) == EXPECTED_ALLOCATIONS[case], data
    assert sorted(data['frees']) == list(range(len(data['allocations']))), data
    assert len(data['vec_allocations']) == EXPECTED_VEC_ALLOCATIONS[case], data
    assert sorted(data['vec_frees']) == list(range(len(data['vec_allocations']))), data
    assert data['events'] == EXPECTED_EVENTS[case], data


def native_cases(args, library, case, manifest, sources, report):
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
        assert row['probe_checks']['rejected'] == \
            EXPECTED_NATIVE_IR_REJECTIONS[case], row['probe_checks']
        mutations = (target / 'probe/mutations.txt').read_text().splitlines()
        names = {line.split(':', 1)[0] for line in mutations}
        assert EXPECTED_MUTATIONS[case] <= names, (case, names)
        row['native_ir_mutations'] = mutations
        observe = run(
            [args.clang, f'-O{opt}', target / 'probe/observe.ll',
             args.root / 'tests/native/vec_observer.cpp',
             args.root / 'tests/native/alloc_observer.cpp', args.runtime,
             '-Wl,--wrap=lang_alloc_v1', '-Wl,--wrap=lang_free_v1',
             '-o', target / 'observe'], library=library)
        row['observe_build'] = observe
        ok(observe, 'native observer build')
        row['observe'] = observed = run([target / 'observe'], library=library)
        ok(observed, 'native observer execute')
        data = parse_observer(observed['stderr'])
        assert_observer(case, data)
        row['observation'] = data
        row['status'] = 'passed'


def python_cases(args, library, case, manifest, sources, report):
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
        [args.python, '-s', args.root / 'tests/python/observe_ownership.py',
         target], library=library)
    ok(observed, 'python ownership observer')
    data = json.loads(observed['stdout'])
    assert data['main_return'] == 0 and not data['alive'], data
    assert data.get('frames_clean', 0) >= 1, data
    assert data['events'] == EXPECTED_PYTHON_EVENTS[case], data
    assert len(data['allocations']) == EXPECTED_VEC_ALLOCATIONS[case], data
    assert sorted(data['frees']) == sorted(item['object']
                                          for item in data['allocations']), data
    row['python_observation'] = data
    row['generated_sha256'] = {
        name: digest(target / name)
        for name in ('program.py', 'langrt.py', 'metadata.json')}
    row['status'] = 'passed'


def simple_case(args, library, case, manifest, sources, report):
    for backend in ('native', 'python'):
        row = dict(case=case, kind='check', backend=backend,
                   check=check_source(args.cli, manifest, backend, library),
                   source_sha256=sources, status='passed')
        report['runs'].append(row)
    for opt in (0, 2):
        row = dict(case=case, kind='native', opt=opt,
                   source_sha256=sources, status='failed')
        report['runs'].append(row)
        target = args.out / case / f'O{opt}'
        shutil.rmtree(target, ignore_errors=True)
        row['build'] = build = run(
            [args.cli, 'build', '--project', manifest, '--backend', 'native',
             '--opt', opt, '--out-dir', target, '--format', 'json'],
            library=library)
        ok(build, 'simple native build')
        assert json.loads(build['stdout'])['status'] == 'ok', build
        row['execute'] = executed = run([target / 'program'], library=library)
        ok(executed, 'simple native execute')
        assert not executed['stdout'] and not executed['stderr'], executed
        row['elf'] = elf_checks(target / 'program')
        row['status'] = 'passed'
    row = dict(case=case, kind='python', source_sha256=sources, status='failed')
    report['runs'].append(row)
    target = args.out / case / 'python'
    shutil.rmtree(target, ignore_errors=True)
    row['build'] = build = run(
        [args.cli, 'build', '--project', manifest, '--backend', 'python',
         '--out-dir', target, '--format', 'json'], library=library)
    ok(build, 'simple python build')
    assert json.loads(build['stdout'])['status'] == 'ok', build
    row['execute'] = executed = run([args.python, '-s', target / 'program.py'],
                                    library=library)
    ok(executed, 'simple python execute')
    assert not executed['stdout'] and not executed['stderr'], executed
    row['status'] = 'passed'


def string_case(args, library, case, manifest, sources, expected, report):
    for backend in ('native', 'python'):
        row = dict(case=case, kind='check', backend=backend,
                   check=check_source(args.cli, manifest, backend, library),
                   source_sha256=sources, status='passed')
        report['runs'].append(row)
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
        ok(build, 'string native build')
        assert json.loads(build['stdout'])['status'] == 'ok', build
        row['execute'] = executed = run([target / 'cli/program'], library=library)
        ok(executed, 'string native execute')
        assert not executed['stdout'] and not executed['stderr'], executed
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
        data = bytes.fromhex(observed['stdout_hex'])
        offset, actual = 0, {}
        for name in metadata['probes']:
            assert offset + 8 <= len(data), ('missing length', name)
            length = int.from_bytes(data[offset:offset + 8], 'little')
            offset += 8
            actual[name] = data[offset:offset + length]
            offset += length
        assert offset == len(data), ('extra observation bytes', offset, len(data))
        assert set(actual) == set(expected), actual
        row['string_bytes'] = {}
        for name, value in expected.items():
            assert actual[name] == value, (name, actual[name].hex(),
                                           value.hex())
            row['string_bytes'][name] = dict(hex=actual[name].hex(),
                                             length=len(actual[name]))
        row['status'] = 'passed'
    row = dict(case=case, kind='python', source_sha256=sources,
               status='failed')
    report['runs'].append(row)
    target = args.out / case / 'python'
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
    metadata_dir = args.out / case / 'python-metadata'
    shutil.rmtree(metadata_dir, ignore_errors=True)
    row['emit'] = emit = run(
        [args.helper, '--project', manifest, '--out-dir', metadata_dir],
        library=library)
    ok(emit, 'string python helper')
    metadata = json.loads((metadata_dir / 'metadata.json').read_text())
    symbols = {item['source_name']: item['generated']
               for item in metadata['symbols']
               if item['source_name'] in expected}
    assert set(symbols) == set(expected), symbols
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
    for name, value in expected.items():
        assert actual[name]['hex'] == value.hex(), (name, actual[name],
                                                   value.hex())
        assert actual[name]['byte_length'] == len(value), (name, actual)
    row['status'] = 'passed'


def reject_case(args, library, name, expected, report):
    manifest = args.root / 'tests/generics/unbound-method-values' / name / 'lang.toml'
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
                     '--backend', backend,
                     '--out-dir', fresh, '--format', 'json'], library=library)
        row['build'] = build
        assert build['exit'] == 1, (name, backend, build)
        found = diagnostic_codes(build)
        assert found and found <= expected, (name, backend, found, build)
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
                     '--backend', backend,
                     '--out-dir', stale, '--format', 'json'], library=library)
        row['build'] = build
        assert build['exit'] == 1, (name, backend, build)
        found = diagnostic_codes(build)
        assert found and found <= expected, (name, backend, found, build)
        assert json.loads(build['stdout'])['status'] == 'error', build
        row['after_sha256'] = {
            suffix: digest(stale / suffix) for suffix in sentinels}
        assert row['after_sha256'] == before, (name, backend, row)
        row['status'] = 'passed'
    assert source_hashes(manifest.parent) == sources, 'reject source changed'


def before_case(args, library, report):
    if not args.before:
        return
    frozen = json.loads((args.before / 'source-sha256.json').read_text())
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
        for relative, expected in frozen.items():
            if relative.startswith(name + '/'):
                assert sources[relative.replace(name + '/', '', 1)] == expected, \
                    'frozen source differs: ' + relative
        row['status'] = 'passed'


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'probe', 'string_probe', 'helper', 'root', 'clang',
                 'runtime', 'python', 'out'):
        parser.add_argument('--' + name.replace('_', '-'), type=Path,
                            required=True)
    parser.add_argument('--before', type=Path)
    parser.add_argument('--only', action='append',
                        choices=tuple(NATIVE_CASES | SIMPLE_CASES |
                                      set(STRING_CASES)) + tuple(REJECTS) +
                        tuple('before-' + x for x in BEFORE))
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
    report = dict(task='uncaptured Type/T method function values',
                  status='failed', python=run([args.python, '--version']),
                  runs=[])
    selected = set(args.only) if args.only else None
    try:
        base = args.root / 'tests/generics/unbound-method-values'
        for case in sorted(NATIVE_CASES | SIMPLE_CASES):
            if selected is not None and case not in selected:
                continue
            manifest = base / case / 'lang.toml'
            sources = source_hashes(manifest.parent)
            if case in NATIVE_CASES:
                for backend in ('native', 'python'):
                    row = dict(case=case, kind='check', backend=backend,
                               check=check_source(args.cli, manifest, backend,
                                                  library),
                               source_sha256=sources, status='passed')
                    report['runs'].append(row)
                native_cases(args, library, case, manifest, sources, report)
                python_cases(args, library, case, manifest, sources, report)
            else:
                simple_case(args, library, case, manifest, sources, report)
            assert source_hashes(manifest.parent) == sources, \
                'positive source changed: ' + case
        for case, expected in STRING_CASES.items():
            if selected is not None and case not in selected:
                continue
            manifest = base / case / 'lang.toml'
            string_case(args, library, case, manifest,
                        source_hashes(manifest.parent), expected, report)
        for name, expected in REJECTS.items():
            if selected is None or name in selected:
                reject_case(args, library, name, expected, report)
        if selected is None or any(x.startswith('before-') for x in selected):
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
