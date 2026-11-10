#!/usr/bin/env python3
"""Formal CLI execution plus independent language-owner observations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
from ownership_cases import CASES


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, timeout=60):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    p = subprocess.run(argv, capture_output=True, text=True, env=env, timeout=timeout)
    return dict(argv=argv, exit=p.returncode, stdout=p.stdout, stderr=p.stderr)


def main():
    parser = argparse.ArgumentParser()
    for key in ('cli', 'helper', 'root', 'out', 'python'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--allow-stage-rejection', action='store_true')
    parser.add_argument('--case', action='append')
    args = parser.parse_args()
    for key in ('cli', 'helper', 'root', 'out', 'python'):
        setattr(args, key, getattr(args, key).resolve())
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(cases=[], status='failed')
    original = json.loads((args.root / 'tests/ownership/cases.json').read_text())
    inputs = []
    for c in original['cases']:
        inputs.append((c['id'], args.root / c['manifest'], c['expected_static'], c.get('runtime_expected', {})))
        assert sha(args.root / c['source']) == c['source_sha256'], c
    for ident, (source, expected) in CASES.items():
        root = args.out / 'sources' / ident
        (root / 'src').mkdir(parents=True, exist_ok=True)
        manifest = root / 'lang.toml'
        manifest.write_text('schema_version=1\n[package]\nname="main"\nsource_root="src"\nentry="app"\n[build]\nbackend="python"\nword_bits=64\n')
        (root / 'src/main.wz').write_text('package main;\n' + source + '\n')
        inputs.append((ident, manifest, dict(status='accept'), expected))
    if args.case:
        inputs = [row for row in inputs if row[0] in args.case]
        assert len(inputs) == len(set(args.case)), 'unknown case'
    for ident, manifest, static, expected in inputs:
        row = dict(id=ident, commands=[], status='failed', source_hashes={str(p):sha(p) for p in manifest.parent.rglob('*.wz')})
        report['cases'].append(row)
        try:
            out = args.out / ident
            limit = 20 if ident in ('recursive-generic-growth',
                                    'recursive-generic-growth-unused') else 60
            build = command([args.cli, 'build', '--project', manifest, '--backend', 'python', '--out-dir', out, '--format', 'json'], timeout=limit)
            row['commands'].append(build)
            if args.allow_stage_rejection and not report['cases'][:-1] and build['exit'] == 1:
                diagnostics = json.loads(build['stdout'])['diagnostics']
                if diagnostics and all(d['code'] == 'E_CAPABILITY_UNAVAILABLE' and d['phase'] != 'codegen' for d in diagnostics):
                    report.update(status='not_run', reason='shared backend support still rejects Vec', total=0, passed=0)
                    row['status'] = 'not_run'
                    (args.out / 'results.json').write_text(json.dumps(report, ensure_ascii=False, indent=2))
                    print(json.dumps(report))
                    return 77
            if static['status'] == 'reject':
                assert build['exit'] == 1, build
                data = json.loads(build['stdout'])
                codes = [d['code'] for d in data['diagnostics']]
                if static.get('code'):
                    assert static['code'] in codes, codes
                assert not out.exists(), out
                if ident == 'recursive-generic-growth':
                    diagnostic = data['diagnostics'][0]
                    assert 'budget' in diagnostic['message'], diagnostic
                    assert diagnostic['primary']['end_byte'] > diagnostic['primary']['start_byte'], diagnostic
                row['status'] = 'passed'
                continue
            assert build['exit'] == 0, build
            row['generated_hashes'] = {str(p):sha(p) for p in out.iterdir() if p.is_file()}
            if expected.get('build_only'):
                row['status'] = 'passed'
                row['execution'] = 'not_run: intentionally infinite cleanup'
                continue
            execution = command([args.python, '-s', out / 'program.py'])
            row['commands'].append(execution)
            if expected.get('panic'):
                assert execution['exit'] == 101 and 'LANG_PANIC:' in execution['stderr'], execution
                assert 'LANG_TYPED_ERROR' not in execution['stderr'], execution
            elif expected.get('typed'):
                assert execution['exit'] == 1 and 'LANG_TYPED_ERROR:' in execution['stderr'], execution
                assert 'LANG_PANIC' not in execution['stderr'], execution
            else:
                assert execution['exit'] == expected.get('main_return', 0), execution
                assert not execution['stdout'] and not execution['stderr'], execution
            helper_out = args.out / 'metadata' / ident
            metadata = command([args.helper, '--project', manifest, '--out-dir', helper_out])
            row['commands'].append(metadata)
            assert metadata['exit'] == 0, metadata
            assert sha(out / 'program.py') == sha(helper_out / 'program.py'), 'helper differs from formal CLI'
            assert sha(out / 'langrt.py') == sha(helper_out / 'langrt.py'), 'embedded runtime differs'
            (out / 'metadata.json').write_bytes((helper_out / 'metadata.json').read_bytes())
            if expected.get('cycle'):
                observed = command([args.python, '-s', Path(__file__).with_name('observe_cycle.py'), out])
                row['commands'].append(observed)
                assert observed['exit'] == 0, observed
                data = json.loads(observed['stdout'])
                row['observation'] = data
                assert data['main_return'] == 0, data
                assert data['self_cycle'] and len(data['alive']) == 1, data
                assert data['alive'][0]['refs'] > 0, data
                assert data['alive'][0]['object'] not in data['frees'], data
                assert data['frames_clean'] > 0, data
                row['status'] = 'passed'
                continue
            observed = command([args.python, '-s', Path(__file__).with_name('observe_ownership.py'), out])
            row['commands'].append(observed)
            assert observed['exit'] == 0, observed
            data = json.loads(observed['stdout'])
            row['observation'] = data
            if expected.get('panic'):
                assert 'panic' in data, data
            else:
                assert not data['alive'], data
            for key in ('events', 'frees'):
                if key in expected:
                    assert data[key] == expected[key], (key, data[key], expected[key])
            for event in expected.get('forbidden_events', []):
                assert event not in data['events'], data
            # Source events and final frees share one ordered observation.
            if ident == 'multi-defer-output':
                sequence = [x for x in data['operations'] if x[0] in ('event', 'free')]
                assert sequence == [['event', 3], ['event', 4], ['free', 0],
                                    ['event', 3], ['event', 4], ['free', 2],
                                    ['free', 3], ['free', 1]], sequence
            if ident == 'defer-before-destroy':
                assert data['operations'].index(['binding', 'log', 17]) < data['operations'].index(['free', 0]), data
            if ident == 'error-arg-interrupt':
                sequence = [x for x in data['operations'] if x[0] in ('event', 'free')]
                assert sequence == [['event', 2], ['free', 0], ['event', 3]], sequence
            if ident == 'recursive-for-in-early-exits':
                assert data['events'] == [10, 20, 10, 20, 10, 20], data
                assert len(data['allocations']) == 7 and len(data['frees']) == 7, data
                for name in ('break_log', 'return_log', 'throw_log'):
                    assert ['binding', name, 1020] in data['operations'], data
                events = [i for i, x in enumerate(data['operations']) if x[0] == 'event']
                frees = [i for i, x in enumerate(data['operations']) if x[0] == 'free']
                assert len(events) == 6 and len(frees) == 7, data
                assert min(frees) > max(events), data
            if 'sequence' in expected:
                sequence = [x for x in data['operations'] if x[0] in ('event', 'free')]
                assert sequence == expected['sequence'], sequence
            # Every final Vec release drops scalar elements in reverse order.
            for ident_alloc, alloc in enumerate(data['allocations']):
                if alloc['items'] == [11] or alloc['items'] == [22]:
                    free = data['operations'].index(['free', ident_alloc])
                    assert data['operations'][free - 1] == ['drop_value', alloc['items'][0]], data
            row['status'] = 'passed'
        except (AssertionError, Exception) as error:
            row['error'] = repr(error)
        (args.out / 'results.json').write_text(json.dumps(report, ensure_ascii=False, indent=2))
    failures = [c['id'] for c in report['cases'] if c['status'] != 'passed']
    report.update(status='failed' if failures else 'passed', total=len(report['cases']), passed=len(report['cases'])-len(failures), failures=failures)
    (args.out / 'results.json').write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print(json.dumps({k:v for k,v in report.items() if k != 'cases'}))
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
