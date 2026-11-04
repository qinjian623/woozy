#!/usr/bin/env python3
"""Execute unchanged floating fixtures and observe each actual LLVM function.

The CLI runs main. A separate typed LLVM harness calls the generated probe
bodies, writes raw return bits, and runs at O0/O2. No C++ aggregate ABI guesses.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from float_expected import operation, result


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, env=None):
    completed = subprocess.run(list(map(str, argv)), capture_output=True, timeout=90, env=env)
    return dict(argv=list(map(str, argv)), exit=completed.returncode,
                stdout_hex=completed.stdout.hex(), stderr_hex=completed.stderr.hex())


def success(row):
    assert row['exit'] == 0 and not row['stderr_hex'], row


def source_hashes(project):
    return {str(p.relative_to(project)): sha(p) for p in sorted(project.rglob('*'))
            if p.is_file() and (p.suffix == '.lang' or p.name == 'lang.toml')}


def additional():
    probes = []
    rng = random.Random(20261104)
    for width in (32, 64):
        frac, exp = (23, 8) if width == 32 else (52, 11)
        infinity = ((1 << exp) - 1) << frac
        sign = 1 << (width - 1)
        one = 0x3f800000 if width == 32 else 0x3ff0000000000000
        two = one + (1 << frac)
        edges = [0, sign, 1, sign | 1, 2, 3, (1 << frac) - 1, 1 << frac,
                 one, sign | one, one + 1, two, infinity - 1, sign | (infinity - 1),
                 infinity, sign | infinity, infinity | (1 << (frac - 1))]
        pairs = [(a, b) for a in edges for b in (0, sign, 1, two, infinity)]
        pairs += [(rng.getrandbits(width), rng.getrandbits(width)) for _ in range(64)]
        # Non-power-of-two divisors; their exact rational quotient requires RNE.
        pairs += [(0x3f9e0652, 0x40e6b74d), (0x007fffff, 0x3f800001),
                  (0x00800001, 0x3fc00001), (0x7f7fffff, 0x3f000001)] if width == 32 else [
                  (0x3ff3c0ca428c59dd, 0x401cd6e9e1b089a0),
                  (0xfffffffffffff, 0x3ff0000000000001)]
        for op in ('add', 'sub', 'mul', 'div', 'eq', 'ne', 'lt', 'le', 'gt', 'ge'):
            for a, b in pairs:
                raw = operation(op, [a, b], width)
                expected = dict(type='bool', **{'class': 'bool'}, value=raw) if isinstance(raw, bool) else result(raw, width)
                probes.append(dict(function=f'{op}{width}', arguments=[dict(type=f'f{width}', bits=f'{x:0{width // 4}x}') for x in (a, b)],
                                   result=expected, expected_source='exact Fraction arithmetic and integer RNE (float_expected.py)'))
        for a in edges:
            probes.append(dict(function=f'neg{width}', arguments=[dict(type=f'f{width}', bits=f'{a:0{width // 4}x}')],
                               result=result(operation('neg', [a], width), width), expected_source='IEEE sign-bit inversion'))
        for a, b in pairs[:30]:
            probes.append(dict(function=f'compound{width}', arguments=[dict(type=f'f{width}', bits=f'{x:0{width // 4}x}') for x in (a, b)],
                               result=result(operation('compound', [a, b], width), width), expected_source='four exact ratios rounded after each assignment'))
        half_ulp = 0x33800000 if width == 32 else 0x3ca0000000000000
        triples = [('step', [one, half_ulp, half_ulp]),
                   ('nofma', [one + 1, one + 1, sign | (one + 2)])]
        for op, values in triples:
            probes.append(dict(function=f'{op}{width}', arguments=[dict(type=f'f{width}', bits=f'{x:0{width // 4}x}') for x in values],
                               result=result(operation(op, values, width), width), expected_source='exact ratios with separate RNE steps'))
    return probes


def observations(args, case, opt, row, probes, env):
    out = args.out / case['id'] / f'O{opt}'
    out.mkdir(parents=True, exist_ok=True)
    requests = out / 'requests.tsv'
    requests.write_text(''.join(p['function'] + '\t' + p['result']['type'] + ''.join('\t' + a['bits'] for a in p.get('arguments', [])) + '\n' for p in probes))
    row['probe_build'] = r = command([args.probe, args.root / case['manifest'], out / 'probe', opt, requests], env)
    success(r)
    row['probe_metadata'] = json.loads((out / 'probe/metadata.json').read_text())
    assert row['probe_metadata']['probes'] == len(probes)
    row['object_identical_to_cli'] = (out / 'cli/program.o').read_bytes() == (out / 'probe/generated.o').read_bytes()
    assert row['object_identical_to_cli'], 'CLI/probe objects differ'
    row['observe_build'] = r = command([args.clang, f'-O{opt}', '-ffp-contract=off', out / 'probe/observe.ll', args.runtime, '-o', out / 'observe'], env)
    success(r)
    row['observe_execute'] = r = command([out / 'observe'])
    success(r)
    data = bytes.fromhex(r['stdout_hex'])
    mxcsr = int.from_bytes(data[:4], 'little')
    row['floating_environment'] = dict(mxcsr=f'{mxcsr:08x}', rounding=(mxcsr >> 13) & 3,
                                      ftz=bool(mxcsr & (1 << 15)), daz=bool(mxcsr & (1 << 6)),
                                      exceptions_masked=(mxcsr & 0x1f80) == 0x1f80)
    assert len(data) >= 4 and (mxcsr & 0xe040) == 0 and (mxcsr & 0x1f80) == 0x1f80, row['floating_environment']
    offset = 4
    for index, p in enumerate(probes):
        expected = p['result']
        width = 1 if expected['type'] == 'bool' else int(expected['type'][1:]) // 8
        assert offset + width <= len(data), 'missing observation bytes'
        raw = int.from_bytes(data[offset:offset + width], 'little')
        offset += width
        actual = dict(type='bool', **{'class': 'bool'}, value=bool(raw), bits=f'{raw:02x}') if width == 1 else result(raw, width * 8)
        # Preserve actual NaN bits for debugging, without comparing payload/sign.
        if width != 1:
            actual['observed_bits'] = f'{raw:0{width * 2}x}'
        matched = all(actual.get(k) == v for k, v in expected.items() if k in ('type', 'class', 'bits', 'sign', 'value'))
        observed = dict(index=index, function=p['function'], arguments=p.get('arguments', []), expected=expected,
                        actual=actual, expected_source=p.get('expected_source', 'unchanged tests/floating/cases.json and LLVM22 APFloat oracle'),
                        status='passed' if matched else 'failed')
        row['values'].append(observed)
        assert matched, observed
    assert offset == len(data), 'extra observation bytes'


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'probe', 'root', 'clang', 'runtime', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    published = json.loads((args.root / 'tests/floating/cases.json').read_text())
    report = dict(operation_id='language-strict-float-native-20261104', status='failed',
                  contract='compiler-dev/1', runs=[], rejections=[], tools={},
                  unchanged_floating_hashes=source_hashes(args.root / 'tests/floating'),
                  expected_sources={p: sha(args.root / p) for p in ('tests/floating/cases.json', 'tests/floating/oracle/vectors.tsv',
                                    'tests/floating/tools/apfloat_oracle.cpp', 'tests/native/float_expected.py')})
    failbin = args.out / 'fail-python'
    failbin.mkdir(exist_ok=True)
    marker = failbin / 'called'
    marker.unlink(missing_ok=True)
    for name in ('python', 'python3', 'python3.12', 'python3.13', 'python3.14'):
        p = failbin / name
        p.write_text('#!/bin/sh\nprintf "%s\\n" "$0" >> "$FAIL_PYTHON_LOG"\nexit 74\n')
        p.chmod(0o700)
    env = os.environ.copy()
    env['PATH'] = str(failbin.resolve()) + os.pathsep + env.get('PATH', '')
    env['FAIL_PYTHON_LOG'] = str(marker.resolve())
    report['compile_environment'] = {k: env[k] for k in ('PATH', 'FAIL_PYTHON_LOG')}
    try:
        report['tools']['clang'] = r = command([args.clang, '--version'])
        success(r); assert '22.1.8' in bytes.fromhex(r['stdout_hex']).decode()
        report['tools']['python'] = dict(executable=sys.executable, version=sys.version)
        assert sys.version_info[:3] == (3, 12, 15), sys.version
        cases = [c for c in published['cases'] if c['expected_static']['status'] == 'accept']
        assert len(cases) == 12 and sum(len(c['runtime_expected']['probes']) for c in cases) == 76
        cases += [dict(id='dynamic', manifest='tests/native/floating/dynamic/lang.toml', runtime_expected=dict(main_return=0, probes=additional()))]
        values = [('probe_values32', 32, 0x41280000), ('probe_values64', 64, 0x4008000000000000),
                  ('probe_order', 32, 0x42240000), ('probe_cleanup', 32, 0x40b80000),
                  ('probe_same_type', 64, 0x400e000000000000), ('probe_inout', 32, 0x40400000)]
        cases += [dict(id='values-cleanup', manifest='tests/native/floating/values-cleanup/lang.toml',
                       runtime_expected=dict(main_return=0, probes=[dict(function=n, result=result(raw, w), expected_source='independent source arithmetic and decimal event sequence assertions') for n, w, raw in values]))]
        for case in cases:
            manifest = args.root / case['manifest']
            for opt in (0, 2):
                out = args.out / case['id'] / f'O{opt}'
                row = dict(id=case['id'], opt=opt, status='failed', values=[], sources=source_hashes(manifest.parent))
                report['runs'].append(row)
                row['check'] = r = command([args.cli, 'check', '--project', manifest, '--backend', 'native', '--format', 'json'], env)
                row['build'] = build = command([args.cli, 'build', '--project', manifest, '--backend', 'native', '--opt', opt, '--out-dir', out / 'cli', '--format', 'json'], env)
                # An original baseline may still use a documented unsupported feature.
                if build['exit'] and case['id'].startswith('baseline-'):
                    diagnostics = json.loads(bytes.fromhex(build['stdout_hex']))['diagnostics']
                    assert set(d['code'] for d in diagnostics) == {'E_CAPABILITY_UNAVAILABLE'}, build
                    assert not (out / 'cli').exists()
                    row.update(status='unsupported', execution='not_run', diagnostics=diagnostics)
                    continue
                success(r); success(build)
                row['execute'] = ran = command([out / 'cli/program'])
                assert ran['exit'] == case['runtime_expected']['main_return'] and not ran['stdout_hex'] and not ran['stderr_hex'], ran
                probes = case['runtime_expected']['probes']
                if probes:
                    observations(args, case, opt, row, probes, env)
                row['artifacts'] = {str(p.relative_to(out)): sha(p) for p in sorted(out.rglob('*')) if p.is_file()}
                row['status'] = 'passed'
        # Panic, ordinary exit101, and an uncaught typed float error stay distinct.
        for ident, exit_code, marker_text in [('panic', 101, 'LANG_NATIVE_PANIC:v1:division-by-zero:'),
                                             ('return101', 101, ''), ('uncaught', 1, 'LANG_TYPED_ERROR: f64\n'),
                                             ('reject-move', 0, ''), ('reject-vec', 0, '')]:
            manifest = args.root / f'tests/native/floating/{ident}/lang.toml'
            for opt in (0, 2):
                out = args.out / ident / f'O{opt}'
                row = dict(id=ident, opt=opt, sources=source_hashes(manifest.parent), status='failed', values=[])
                report['runs'].append(row)
                row['build'] = r = command([args.cli, 'build', '--project', manifest, '--backend', 'native', '--opt', opt, '--out-dir', out, '--format', 'json'], env)
                success(r)
                row['execute'] = r = command([out / 'program'])
                stderr = bytes.fromhex(r['stderr_hex']).decode()
                assert r['exit'] == exit_code and not r['stdout_hex'], r
                assert (stderr.startswith(marker_text) if marker_text else stderr == ''), r
                row['observed_category'] = 'panic' if ident == 'panic' else 'typed_error' if ident == 'uncaught' else 'ordinary_return'
                row['artifacts'] = {str(p.relative_to(out)): sha(p) for p in sorted(out.rglob('*')) if p.is_file()}
                row['status'] = 'passed'
        codes = {'reject-dynamic-float-int-conversion': 'E_CAPABILITY_UNAVAILABLE'}
        rejection_root = Path(tempfile.mkdtemp(prefix='rejected-', dir=args.out))
        rejects = [c for c in published['cases'] if c['expected_static']['status'] == 'reject']
        for path in sorted((args.root / 'tests/native/floating').glob('reject-*/lang.toml')):
            if path.parent.name in ('reject-move', 'reject-vec'):
                continue
            rejects.append(dict(id=path.parent.name, manifest=str(path.relative_to(args.root))))
            codes[path.parent.name] = 'E_CAPABILITY_UNAVAILABLE'
        for case in rejects:
            manifest = args.root / case['manifest']
            for opt in (0, 2):
                row = dict(id=case['id'], opt=opt, status='failed', commands=[])
                report['rejections'].append(row)
                out = rejection_root / case['id'] / f'O{opt}'
                for phase in ('check', 'build', 'preserve'):
                    if phase == 'preserve':
                        out.mkdir(parents=True, exist_ok=True)
                        for n in ('program', 'program.o', 'unrelated'):
                            (out / n).write_bytes(('old-' + n).encode())
                        before = {p.name: sha(p) for p in out.iterdir()}
                    argv = [args.cli, 'check' if phase == 'check' else 'build', '--project', manifest, '--backend', 'native', '--format', 'json']
                    if phase != 'check':
                        argv += ['--opt', opt, '--out-dir', out]
                    r = command(argv, env); row['commands'].append(r)
                    parsed = json.loads(bytes.fromhex(r['stdout_hex']))
                    assert r['exit'] == 1 and not r['stderr_hex'], r
                    expected = codes.get(case['id'], 'E_TYPE_MISMATCH')
                    assert {d['code'] for d in parsed['diagnostics']} == {expected}, parsed
                    if phase == 'preserve':
                        assert before == {p.name: sha(p) for p in out.iterdir()}, 'failed build changed files'
                    else:
                        assert not out.exists(), 'failed build created output'
                row.update(status='unsupported' if expected == 'E_CAPABILITY_UNAVAILABLE' else 'passed', old_output_preserved=True)
        assert not marker.exists(), 'compiler invoked Python'
        report['compiler_started_python'] = False
        report['status'] = 'passed'
    finally:
        report['counts'] = dict(original_probes_observed=sum(len(r['values']) for r in report['runs'] if r['id'].startswith(('f32-', 'f64-'))),
                               additional_probes_observed=sum(len(r['values']) for r in report['runs'] if r['id'] in ('dynamic', 'values-cleanup')),
                               cli_executions=sum('execute' in r for r in report['runs']),
                               unsupported_runs=sum(r['status'] == 'unsupported' for r in report['runs']),
                               bad_ir_rejected=sum(r.get('probe_metadata', {}).get('bad_ir_rejected', 0) for r in report['runs']))
        report['test_source_hashes'] = source_hashes(args.root / 'tests/native/floating')
        path = args.out / 'results.json'
        serialized = json.dumps(report, ensure_ascii=False, indent=2) + '\n'
        path.write_text(serialized)
        path.with_name(f'results-attempt-{time.time_ns()}.json').write_text(serialized)
    print(json.dumps(dict(status=report['status'], **report['counts'], report=str(path))))


if __name__ == '__main__':
    main()
