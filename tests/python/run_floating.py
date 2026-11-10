#!/usr/bin/env python3
"""Real CLI build, complete CPython compile, execution and bit observations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from floating_cases import dynamic_source, supplemental


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, **kwargs):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    p = subprocess.run(argv, capture_output=True, text=True, timeout=90, env=env, **kwargs)
    return dict(argv=argv, returncode=p.returncode, stdout=p.stdout, stderr=p.stderr)


def ok(row):
    assert row['returncode'] == 0 and not row['stderr'], row


def matches(actual, expected):
    for key, value in expected.items():
        if key == 'payload_compared':
            assert value is False  # fixture policy, not an observed value
            continue
        assert actual.get(key) == value, (key, actual, expected)


def vector_probes(file):
    probes, inputs, outputs = [], [], []
    for line in file.read_text().splitlines():
        if line.startswith('#') or not line:
            continue
        width, op, a, b, category, bits, sign = line.split('\t')
        typ = 'f' + width
        result = dict(type='bool', value=bits == 'true') if category == 'bool' else dict(
            type=typ, **{'class': category}, bits=None if bits == '-' else bits,
            sign=None if sign == '-' else sign)
        arguments = [dict(type=typ, bits=a)]
        if op != 'neg': arguments.append(dict(type=typ, bits=b))
        probes.append(dict(function=op + width, arguments=arguments, result=result))
        inputs.append(' '.join((width, op, a, b)) + '\n')
        outputs.append('\t'.join((category, bits, sign)) + '\n')
    return probes, ''.join(inputs), ''.join(outputs)


def main():
    p = argparse.ArgumentParser()
    for name in ('cli', 'helper', 'python', 'root', 'out'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--oracle', type=Path)
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rows = []
    report = dict(operation_id='language-strict-float-python-20261104', contract='compiler-dev/1',
                  status='failed', cases=rows, failures=[], controls=[],
                  not_run=['root final ASan/UBSan, LLVM-off and cross-backend comparison'])
    shared = ('src/hir/hir.cpp', 'tests/sema/check_test.cpp', 'tests/generics/type_test.cpp')
    report['shared_test_sha256'] = {f: sha(args.root / f) for f in shared}
    report['implementation_sha256'] = {f: sha(args.root / f) for f in (
        'runtime/python/langrt.py', 'src/backend/python/emitter.cpp', 'include/lang/backend/python.hpp')}

    def protected():
        return {str(f.relative_to(args.root)): sha(f) for folder in (
            'baseline', 'tests/floating', 'tests/generics/fixtures', 'tests/cleanup/fixtures')
            for f in sorted((args.root / folder).rglob('*')) if f.is_file() and '__pycache__' not in f.parts}

    before = protected()
    report['protected_before'] = before
    report['python'] = version = command([args.python, '--version'])
    ok(version)
    assert version['stdout'].strip() == 'Python 3.12.15', version
    trap = args.out / 'no-python'
    trap.mkdir(exist_ok=True)
    for name in ('python', 'python3', 'python3.12'):
        f = trap / name
        f.write_text('#!/bin/sh\necho unexpected-python-during-compilation >&2\nexit 118\n')
        f.chmod(0o755)

    def compiler(argv):
        return command(['/usr/bin/env', f'PATH={trap}', *argv])

    def source(ident, body):
        project = args.out / 'sources' / ident
        (project / 'src').mkdir(parents=True, exist_ok=True)
        (project / 'src/main.wz').write_text('package app;\n' + body)
        manifest = project / 'lang.toml'
        manifest.write_text('schema_version=1\n[package]\nname="app"\nsource_root="src"\n'
                            'entry="app"\n[build]\nbackend="python"\nword_bits=64\n')
        return manifest

    def accepted(ident, manifest, expected, probes):
        generated = args.out / 'generated' / ident
        row = dict(id=ident, manifest=str(manifest), expected=expected, status='failed',
                   runtime='not_run', source_sha256=sha(manifest.parent / 'src/main.wz'),
                   manifest_sha256=sha(manifest))
        rows.append(row)
        try:
            for phase in ('check', 'build'):
                argv = [args.cli, phase, '--project', manifest, '--backend', 'python', '--format', 'json']
                if phase == 'build': argv += ['--out-dir', generated]
                row[phase] = r = compiler(argv)
                ok(r)
                assert json.loads(r['stdout'])['status'] == 'ok', r
            mapping = args.out / 'mapping' / ident
            row['metadata_build'] = r = compiler([args.helper, '--project', manifest, '--out-dir', mapping])
            ok(r)
            for name in ('program.py', 'langrt.py'):
                assert (generated / name).read_bytes() == (mapping / name).read_bytes(), name
            # Observe the CLI files, with metadata supplied separately by the helper.
            (generated / 'metadata.json').write_bytes((mapping / 'metadata.json').read_bytes())
            (generated / 'verified.hir').write_bytes((mapping / 'verified.hir').read_bytes())
            row['generated_sha256'] = {name: sha(generated / name) for name in (
                'program.py', 'langrt.py', 'metadata.json', 'verified.hir')}
            code = 'from pathlib import Path;import sys;[compile(Path(p).read_bytes(),p,"exec") for p in sys.argv[1:]]'
            row['cpython_compile'] = r = command([args.python, '-B', '-s', '-c', code,
                                                  generated / 'program.py', generated / 'langrt.py'])
            ok(r)
            row['execute'] = r = command([args.python, '-B', '-s', generated / 'program.py'])
            assert not r['stdout'], r
            if 'panic' in expected:
                assert r['returncode'] == 101 and r['stderr'] == 'LANG_PANIC: ' + expected['panic'] + '\n', r
            elif 'typed' in expected:
                assert r['returncode'] == 1 and r['stderr'] == 'LANG_TYPED_ERROR: ' + expected['typed'] + '\n', r
            else:
                assert r['returncode'] == expected.get('main_return', 0) and not r['stderr'], r
            row['runtime'] = 'passed'
            probe_file = generated / 'probes.json'
            probe_file.write_text(json.dumps(probes, indent=2) + '\n')
            row['expected_probes_sha256'] = sha(probe_file)
            row['observe'] = r = command([args.python, '-B', '-s', Path(__file__).with_name('observe_floating.py'),
                                          generated, probe_file])
            ok(r)
            row['actual'] = observed = json.loads(r['stdout'])
            assert len(observed) == len(probes), (len(observed), len(probes))
            for actual, want in zip(observed, probes):
                if 'result' in want:
                    assert actual['outcome'] == 'return', actual
                    matches(actual['result'], want['result'])
                    assert actual['signature']['result'] == want['result']['type']
                elif 'error_key' in want:
                    assert actual['outcome'] == 'typed' and actual['error_key'] == want['error_key'], actual
                    matches(actual['error_value'], want['error_value'])
                else:
                    assert actual['outcome'] == 'panic' and actual['panic'] == want['panic'], actual
                cells = want.get('cells', [])
                assert len(actual['cells']) == len(cells), actual
                for a, w in zip(actual['cells'], cells): matches(a, w)
            row['status'] = 'passed'
            return generated
        except (AssertionError, ValueError, OSError, KeyError) as error:
            row['failure'] = str(error)
            report['failures'].append(ident)
            return None

    published = json.loads((args.root / 'tests/floating/cases.json').read_text())
    assert sha(args.root / published['oracle']['vectors']) == published['oracle']['vectors_sha256']
    for case in published['cases']:
        assert sha(args.root / case['source']) == case['source_sha256']
        assert sha(args.root / case['manifest']) == case['manifest_sha256']
        manifest = args.root / case['manifest']
        if case['expected_static']['status'] == 'accept':
            expected = case['runtime_expected']
            accepted(case['id'], manifest, dict(main_return=expected['main_return']), expected['probes'])
            continue
        row = dict(id=case['id'], status='failed', runtime='not_run (source rejection)',
                   source_sha256=case['source_sha256'], manifest_sha256=case['manifest_sha256'])
        rows.append(row)
        try:
            code = 'E_CAPABILITY_UNAVAILABLE' if 'conversion' in case['id'] else 'E_TYPE_MISMATCH'
            output = Path(tempfile.mkdtemp(prefix=case['id'] + '-', dir=args.out)) / 'out'
            for phase in ('check', 'build', 'existing'):
                if phase == 'existing':
                    output.mkdir()
                    for name in ('program.py', 'langrt.py', 'program.o', 'program', 'unrelated'):
                        (output / name).write_text('preserve-' + name)
                    previous = {f.name: sha(f) for f in output.iterdir()}
                argv = [args.cli, 'check' if phase == 'check' else 'build', '--project', manifest,
                        '--backend', 'python', '--format', 'json']
                if phase != 'check': argv += ['--out-dir', output]
                row[phase] = r = compiler(argv)
                assert r['returncode'] == 1 and not r['stderr'], r
                diagnostics = json.loads(r['stdout'])['diagnostics']
                assert any(d['code'] == code and d.get('primary') and
                    Path(d['primary']['path']).resolve() == (args.root / case['source']).resolve()
                    for d in diagnostics), diagnostics
                if phase == 'existing':
                    row['preserved_sha256'] = {f.name: sha(f) for f in output.iterdir()}
                    assert previous == row['preserved_sha256']
                else: assert not output.exists()
            row['status'] = 'unsupported' if code == 'E_CAPABILITY_UNAVAILABLE' else 'passed'
        except (AssertionError, ValueError, OSError, KeyError) as error:
            row['failure'] = str(error)
            report['failures'].append(case['id'])

    for ident, body, expected, probes in supplemental():
        accepted(ident, source(ident, body), expected, probes)

    const_fixture = args.root / 'tests/native/floating/const-comparison/src/main.wz'
    const_body = const_fixture.read_text()
    prefix = 'package app;\n'
    assert const_body.startswith(prefix)
    accepted('const-float-comparison',
             source('const-float-comparison', const_body[len(prefix):]),
             dict(main_return=0), [])

    vector_file = Path(__file__).with_name('floating_vectors.tsv')
    probes, oracle_input, oracle_expected = vector_probes(vector_file)
    report['dynamic_vectors_sha256'] = sha(vector_file)
    report['dynamic_vector_count'] = len(probes)
    if args.oracle:
        report['oracle_version'] = r = command([args.oracle, '--version'])
        ok(r)
        assert r['stdout'].strip() == '22.1.8', r
        report['oracle'] = r = command([args.oracle], input=oracle_input)
        ok(r)
        assert r['stdout'] == oracle_expected, 'frozen vectors differ from APFloat'
    else:
        report['oracle'] = dict(status='not_run', reason='LLVM disabled; using frozen APFloat 22.1.8 vectors')
    generated = accepted('dynamic-vectors', source('dynamic-vectors', dynamic_source()), {}, probes)
    if generated:
        # Exact midpoint +/- a sticky bit outside binary64. This tests the
        # integer rounder separately; these are not claimed to be f32 operands.
        code = '''import langrt,struct
for width,p,bias in ((32,23,127),(64,52,1023)):
 pack=lambda v: struct.pack('>f' if width==32 else '>d',v).hex()
 for odd in (0,1):
  # Between 1+odd*ulp and its successor, with 200 extra exact bits.
  d=1<<(p+201); n=d+((2*odd+1)<<200)
  lower=(bias<<p)+odd; upper=lower+1
  for delta,want in ((-1,lower),(0,lower if odd==0 else upper),(1,upper)):
   assert int(pack(langrt._float_round(n+delta,d,width)),16)==want
'''
        report['controls'].append(r := command([args.python, '-B', '-s', '-c', code], cwd=generated))
        ok(r)
        for error in ('ZeroDivisionError', 'OverflowError'):
            code = f'''import program,langrt
def bug(*args): raise {error}('controlled host failure')
langrt.float_binary=bug
langrt.run(lambda:program.{{name}}(1.0,0.0))
'''
            metadata = json.loads((generated / 'metadata.json').read_text())
            name = next(f['generated'] for f in metadata['functions'] if f['source_name'] == 'div32')
            r = command([args.python, '-B', '-s', '-c', code.format(name=name)], cwd=generated)
            report['controls'].append(r)
            assert r['returncode'] == 1 and error in r['stderr'] and 'LANG_PANIC:' not in r['stderr'], r
        r = command([args.python, '-B', '-s', '-c',
                     "import langrt;langrt.RUNTIME_VERSION='compiler-dev/1-python-values/2';import program"], cwd=generated)
        report['controls'].append(r)
        assert r['returncode'] == 1 and 'incompatible langrt version' in r['stderr'] and 'LANG_PANIC:' not in r['stderr'], r
    report['protected_after'] = protected()
    assert before == report['protected_after'], 'protected input changed'
    report['original_probes_observed'] = sum(len(r.get('actual', [])) for r in rows
        if r['id'] in {c['id'] for c in published['cases']})
    report['status'] = 'passed' if not report['failures'] else 'failed'
    path = args.out / 'floating-results.json'
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], original_probes=report['original_probes_observed'],
        dynamic_probes=len(probes), cases=len(rows), failures=report['failures'], report=str(path))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
