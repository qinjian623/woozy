#!/usr/bin/env python3
"""Run old function-value sources and the implemented Source Weak subset.

The remaining closure/bound fixtures still record unsupported diagnostics.
They are never counted as passing runtime or expected-negative cases.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True

OLD_SOURCE = (
    ('function-values',
     'tests/backend_common/fixtures/function-values/lang.toml', 0),
    ('throws-function-value',
     'tests/cleanup/fixtures/case-08/lang.toml', 0),
    ('generic-function-argument',
     'tests/generics/fixtures/function-argument/lang.toml', 5),
)

# Published current baseline families from tests/reference_values/CONTRACT.md.
BLOCKED_BASELINE = {
    'bound': 'E_TYPE_MISMATCH',            # method values are not implemented
    'closure': 'E_CAPABILITY_UNAVAILABLE',  # capturing a local is unavailable
}


def command(argv):
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    return subprocess.run([str(a) for a in argv], capture_output=True,
                          text=True, env=env, timeout=180)


def baseline_for(case_id):
    if case_id == 'weak-fn-captured-dead':
        return BLOCKED_BASELINE['closure']
    if case_id.startswith('weak-reject-'):
        return 'E_TYPE_MISMATCH'
    if case_id.startswith('weak-'):
        return None
    if case_id.startswith('bound-'):
        return BLOCKED_BASELINE['bound']
    return BLOCKED_BASELINE['closure']


def build(helper, manifest, out_dir):
    return command((helper, 'build', '--project', manifest, '--backend',
                    'python', '--out-dir', out_dir, '--format', 'json'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--python', type=str, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(operation_id='python-closure-backend-20261112',
                  contract='compiler-dev/1', status='failed',
                  old_source=[], reference_values=[],
                  notes=[
                      'Part 1 is real old-source Python execution; no capture '
                      'or bound-method source is accepted at this revision.',
                      'Part 2 executes supported Weak cases and checks their '
                      'specific invalid targets. Remaining closure/bound cases '
                      'are recorded as unsupported, not as passes.',
                  ])

    for ident, manifest, expected_return in OLD_SOURCE:
        row = dict(id=ident, manifest=manifest, status='failed')
        report['old_source'].append(row)
        out_dir = args.out / 'old-source' / ident
        built = build(args.cli, args.root / manifest, out_dir)
        parsed = json.loads(built.stdout or '{}')
        assert built.returncode == 0 and not built.stderr, (ident, built)
        assert parsed.get('status') == 'ok', (ident, parsed)
        program = (out_dir / 'program.py').read_text()
        assert "_rt.ValueType('function')" in program, ident
        assert '_rt.FunctionValue(' not in program, ident
        assert '_bound' not in program, ident
        result = command((args.python, '-B', '-s', out_dir / 'program.py'))
        assert result.returncode == expected_return, (ident, result)
        assert result.stdout == '' and result.stderr == '', (ident, result)
        row['program_has_function_descriptor'] = True
        row['main_return'] = result.returncode
        row['status'] = 'passed'

    cases = json.loads(
        (args.root / 'tests/reference_values/cases.json').read_text())['cases']
    for case in cases:
        ident = case['id']
        row = dict(id=ident, kind=case['kind'], status='failed',
                   expected_blocked=baseline_for(ident))
        report['reference_values'].append(row)
        # The planned source must still parse (syntax only) at this revision.
        source = args.root / case['manifest']
        source = source.parent / 'src'
        files = sorted(source.rglob('*.wz'))
        assert len(files) == 1, (ident, files)
        syntax = command((args.cli, 'check', '--syntax-only', files[0],
                          '--format', 'json'))
        parsed_syntax = json.loads(syntax.stdout or '{}')
        assert syntax.returncode == 0 and not syntax.stderr, (ident, syntax)
        assert parsed_syntax.get('status') == 'ok', (ident, parsed_syntax)
        row['syntax_only'] = 'ok'
        out_dir = args.out / 'reference-values' / ident
        built = build(args.cli, args.root / case['manifest'], out_dir)
        parsed = json.loads(built.stdout or '{}')
        if row['expected_blocked'] is None:
            assert case['kind'] == 'runtime', ident
            assert built.returncode == 0 and not built.stderr, (ident, built)
            assert parsed.get('status') == 'ok', (ident, parsed)
            result = command((args.python, '-B', '-s', out_dir / 'program.py'))
            expected_return = case['expected']['main_return']
            assert result.returncode == expected_return, (ident, result)
            assert result.stdout == '' and result.stderr == '', (ident, result)
            row.update(status='passed', main_return=result.returncode)
            continue
        assert built.returncode == 1 and not built.stderr, (ident, built)
        assert parsed.get('status') == 'error', (ident, parsed)
        assert not out_dir.exists(), (ident, 'rejected case wrote output')
        codes = [d['code'] for d in parsed.get('diagnostics', [])]
        assert row['expected_blocked'] in codes, (ident, codes)
        row['diagnostics'] = codes
        if ident.startswith('weak-reject-'):
            assert any('Weak target must be' in d.get('message', '')
                       for d in parsed.get('diagnostics', [])), (ident, parsed)
            row['status'] = 'rejected (specific Weak target)'
            continue
        row['status'] = ('blocked (not a rejection pass)'
                         if case['kind'] == 'reject'
                         else 'blocked (source feature unavailable)')

    blocked = [r for r in report['reference_values']
               if r['status'].startswith('blocked')]
    passed = [r for r in report['reference_values'] if r['status'] == 'passed']
    rejected = [r for r in report['reference_values']
                if r['status'].startswith('rejected')]
    assert len(report['reference_values']) == 24, len(cases)
    assert (len(passed), len(rejected), len(blocked)) == (4, 3, 17)
    report['status'] = 'passed'
    report['summary'] = dict(
        old_source_passed=len(report['old_source']),
        reference_values_blocked=len(blocked),
        reference_values_passed=len(passed),
        reference_values_rejected=len(rejected))
    (args.out / 'report.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps(report['summary']))


if __name__ == '__main__':
    main()
