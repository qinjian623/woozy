#!/usr/bin/env python3
"""Run the 24 declared reference-value cases through the public Python CLI.

Each case is decided by ``tests/reference_values/cases.json``:
``expected.compile`` is the accepted/rejected decision and, for accepted
runtime cases, ``expected.main_return`` is the program exit that must hold.
The runner no longer keeps a hard-coded "feature unavailable" baseline.

The JSON does not store diagnostic codes for its six rejects.  The three
closure reject codes asserted here are the ones the existing
``tests/reference_closure_frontend/run_reference_closure_cases.py`` uses and
that both current CLIs publish (``E_OWNERSHIP_USE_AFTER_MOVE``,
``E_TYPE_MISMATCH``, ``E_CALL_ARGUMENT``).  The three Weak rejects use
``E_TYPE_MISMATCH`` plus the ``Weak target must be`` message, as recorded by
``tests/python/weak_source/run_weak_source.py`` and
``tests/native/weak_source``.  An unrelated capability error is a failure.

The case table can declare `backends` per case.  The only current exception is
`thread-capture-scalar-arg`, which declares `["native"]` because std/thread is
Native-only by user decision; the Python runner records it `not_run` from that
metadata (never from an arbitrary E_CAPABILITY_UNAVAILABLE), with the case's
Native-only reason, and does not count it as passed.  Cases without `backends`
keep the default both-backend rule.

Part 1 (the three old function-value sources) keeps its original real Python
execution assertions.  Every subprocess goes through
``selfhost/corpus.py::_run_process_impl`` so it has a finite deadline and
process-group cleanup; a timeout or unclean cleanup is a failure, never a pass.
"""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
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

# Source of each code is documented in the module docstring; cases.json only
# records the reason and the "no unrelated unsupported failure" rule.
REJECT_CODES = {
    'closure-move-owning-read-after': 'E_OWNERSHIP_USE_AFTER_MOVE',
    'closure-readonly-assign': 'E_TYPE_MISMATCH',
    'bound-inout-reject': 'E_CALL_ARGUMENT',
    'weak-reject-user-struct': 'E_TYPE_MISMATCH',
    'weak-reject-string': 'E_TYPE_MISMATCH',
    'weak-reject-file': 'E_TYPE_MISMATCH',
}
WEAK_REJECT_MESSAGE = 'Weak target must be'
COMMAND_TIMEOUT = 180.0
BACKEND = 'python'


class CaseFailed(Exception):
    pass


class CommandFailed(Exception):
    def __init__(self, message, argv=None, returncode=None):
        super().__init__(message)
        self.argv = list(argv) if argv is not None else []
        self.returncode = returncode


class Result:
    """Small completed-process record compatible with the old call sites."""

    def __init__(self, args, returncode, stdout, stderr):
        self.args = list(args)
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr

    def __repr__(self):
        return (f"CompletedProcess(args={self.args!r}, "
                f"returncode={self.returncode}, stdout={self.stdout!r}, "
                f"stderr={self.stderr!r})")


class Context:
    def __init__(self, root, cli, python):
        self.root = root
        self.cli = cli
        self.python = python
        self.cwd = Path.cwd()
        self.env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
                        PYTHONNOUSERSITE='1')
        self.env.pop('PYTHONPATH', None)
        spec = importlib.util.spec_from_file_location(
            'reference_values_corpus', root / 'selfhost/corpus.py')
        self.corpus = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.corpus)


def checked_timeout(timeout):
    if (isinstance(timeout, bool) or
            not isinstance(timeout, (int, float)) or
            not math.isfinite(timeout) or timeout <= 0):
        raise ValueError(f'timeout must be finite and positive: {timeout!r}')
    return float(timeout)


def command(context, argv, timeout=COMMAND_TIMEOUT):
    timeout = checked_timeout(timeout)
    process = context.corpus._run_process_impl(
        [str(part) for part in argv], context.cwd, context.env, timeout)
    result = Result([str(part) for part in argv], process['exit'],
                    process['stdout'].decode('utf-8', 'replace'),
                    process['stderr'].decode('utf-8', 'replace'))
    if process['timed_out']:
        raise CommandFailed(f'timed out after {timeout:g}s', result.args,
                            result.returncode)
    if not process['cleanup_complete'] or process['abandoned_pipes']:
        raise CommandFailed('process tree was not cleaned up', result.args,
                            result.returncode)
    return result


def parsed_stdout(result):
    try:
        payload = json.loads(result.stdout or '{}')
    except json.JSONDecodeError:
        return None
    return payload if isinstance(payload, dict) else None


def case_scope(case, backend=BACKEND):
    """Return (kind, reason) for declared backend metadata.

    None means the case is in scope for this backend.  A declared but invalid
    metadata value is a failure; an explicit list without this backend is the
    only accepted not_run path.
    """
    backends = case.get('backends')
    if backends is None:
        return None, None
    if (not isinstance(backends, list) or not backends
            or any(item not in ('native', 'python') for item in backends)):
        return 'failed', f'invalid backends metadata: {backends!r}'
    if backend not in backends:
        note = case.get('backend_note')
        if not isinstance(note, str) or not note.strip():
            note = (f'declared backends {backends}; {backend} is not a '
                    f'declared target')
        return 'not_run', note
    return None, None


def accept_detail(case, built, executed):
    """Return a failure detail, or None when an accept case is satisfied."""
    expected_return = case['expected'].get('main_return')
    if (isinstance(expected_return, bool) or
            not isinstance(expected_return, int)):
        return f'declared main_return is missing or invalid: {expected_return!r}'
    if built.returncode != 0:
        return f'build exit {built.returncode}: {built.stdout}{built.stderr}'
    if built.stderr:
        return f'build wrote stderr: {built.stderr!r}'
    parsed = parsed_stdout(built)
    if parsed is None:
        return f'build stdout is not a JSON object: {built.stdout!r}'
    if parsed.get('status') != 'ok':
        codes = [item.get('code') for item in parsed.get('diagnostics', [])]
        return f'build status {parsed.get("status")!r}, codes {codes}'
    if executed.returncode != expected_return:
        return (f'program exit {executed.returncode}, declared '
                f'{expected_return}: {executed.stdout}{executed.stderr}')
    if executed.stdout or executed.stderr:
        return (f'program wrote stdout/stderr: {executed.stdout!r} '
                f'{executed.stderr!r}')
    return None


def reject_detail(case, built, out_dir_exists):
    """Return a failure detail, or None when a reject case is satisfied."""
    expected_code = REJECT_CODES.get(case['id'])
    if expected_code is None:
        return f'no confirmed reject code for {case["id"]}'
    if built.returncode != 1:
        return f'reject build exit {built.returncode}, expected 1'
    if built.stderr:
        return f'reject build wrote stderr: {built.stderr!r}'
    parsed = parsed_stdout(built)
    if parsed is None:
        return f'reject build stdout is not a JSON object: {built.stdout!r}'
    if parsed.get('status') != 'error':
        return f'reject build status {parsed.get("status")!r}'
    diagnostics = parsed.get('diagnostics', [])
    codes = [item.get('code') for item in diagnostics]
    if expected_code not in codes:
        return f'expected {expected_code}, got {codes}'
    if case['id'].startswith('weak-reject-'):
        if not any(WEAK_REJECT_MESSAGE in item.get('message', '')
                   for item in diagnostics):
            return f'{expected_code} without the Weak target message'
    if out_dir_exists:
        return 'rejected case wrote an output directory'
    return None


def old_source_rows(context, report, out_root):
    for ident, manifest, expected_return in OLD_SOURCE:
        row = dict(id=ident, manifest=manifest, status='failed')
        report['old_source'].append(row)
        try:
            out_dir = out_root / 'old-source' / ident
            built = command(context, (
                context.cli, 'build', '--project',
                context.root / manifest, '--backend', 'python',
                '--out-dir', out_dir, '--format', 'json'))
            if built.returncode != 0 or built.stderr:
                raise CaseFailed(f'build: {built!r}')
            parsed = parsed_stdout(built)
            if parsed is None or parsed.get('status') != 'ok':
                raise CaseFailed(f'build status: {built.stdout!r}')
            program_path = out_dir / 'program.py'
            if not program_path.is_file():
                raise CaseFailed('program.py missing')
            program = program_path.read_text()
            if "_rt.ValueType('function')" not in program:
                raise CaseFailed('program has no function descriptor')
            if '_rt.FunctionValue(' in program:
                raise CaseFailed('old source unexpectedly uses FunctionValue')
            if '_bound' in program:
                raise CaseFailed('old source unexpectedly emits a bound adapter')
            result = command(context, (
                context.python, '-B', '-s', program_path))
            if result.returncode != expected_return:
                raise CaseFailed(f'program exit {result.returncode}, '
                                 f'expected {expected_return}')
            if result.stdout or result.stderr:
                raise CaseFailed('program wrote stdout/stderr')
        except (CaseFailed, CommandFailed, OSError) as error:
            row['detail'] = str(error)
            continue
        row['program_has_function_descriptor'] = True
        row['main_return'] = result.returncode
        row['status'] = 'passed'


def reference_value_rows(context, report, out_root):
    cases = json.loads(
        (context.root / 'tests/reference_values/cases.json').read_text())['cases']
    for case in cases:
        ident = case['id']
        expected = case.get('expected', {})
        row = dict(id=ident, kind=case['kind'],
                   expected={'compile': expected.get('compile'),
                             'main_return': expected.get('main_return')},
                   status='failed')
        report['reference_values'].append(row)
        try:
            source = context.root / case['manifest']
            source = source.parent / 'src'
            files = sorted(source.rglob('*.wz'))
            if len(files) != 1:
                raise CaseFailed(f'expected one source file, got {files}')
            syntax = command(context, (
                context.cli, 'check', '--syntax-only', files[0],
                '--format', 'json'))
            if syntax.returncode != 0 or syntax.stderr:
                raise CaseFailed(f'syntax check: {syntax!r}')
            syntax_parsed = parsed_stdout(syntax)
            if syntax_parsed is None or syntax_parsed.get('status') != 'ok':
                raise CaseFailed(f'syntax status: {syntax.stdout!r}')
            scope_kind, scope_reason = case_scope(case)
            if scope_kind == 'not_run':
                row['status'] = 'not_run'
                row['reason'] = scope_reason
                row['backends'] = list(case['backends'])
                continue
            if scope_kind == 'failed':
                raise CaseFailed(scope_reason)
            out_dir = out_root / 'reference-values' / ident
            built = command(context, (
                context.cli, 'build', '--project',
                context.root / case['manifest'], '--backend', 'python',
                '--out-dir', out_dir, '--format', 'json'))
            compile_kind = expected.get('compile')
            if compile_kind == 'accept':
                # Never use a program.py left by an earlier run: a failed
                # build must fail before any artifact is read.
                if built.returncode != 0:
                    payload = parsed_stdout(built) or {}
                    codes = [item.get('code') for item in
                             payload.get('diagnostics', [])]
                    raise CaseFailed(
                        f'build failed: exit {built.returncode}, status '
                        f'{payload.get("status")!r}, codes {codes}')
                if built.stderr:
                    raise CaseFailed(f'build wrote stderr: {built.stderr!r}')
                payload = parsed_stdout(built)
                if payload is None or payload.get('status') != 'ok':
                    raise CaseFailed(f'build status: {built.stdout!r}')
                program_path = out_dir / 'program.py'
                if not program_path.is_file():
                    raise CaseFailed(
                        f'build did not publish program.py: {built!r}')
                executed = command(context, (
                    context.python, '-B', '-s', program_path))
                detail = accept_detail(case, built, executed)
                if detail:
                    raise CaseFailed(detail)
                row['status'] = 'passed'
                row['main_return'] = executed.returncode
            elif compile_kind == 'reject':
                detail = reject_detail(case, built, out_dir.exists())
                if detail:
                    raise CaseFailed(detail)
                row['diagnostics'] = [
                    item.get('code') for item in
                    parsed_stdout(built).get('diagnostics', [])]
                row['status'] = 'rejected'
            else:
                raise CaseFailed(
                    f'declared compile is not accept/reject: {compile_kind!r}')
        except (CaseFailed, CommandFailed, OSError,
                json.JSONDecodeError) as error:
            row['detail'] = str(error)
    return cases


def summary(report, cases):
    rows = report['reference_values']
    declared = {case['id']: case for case in cases}
    passed = {row['id'] for row in rows if row['status'] == 'passed'}
    rejected = {row['id'] for row in rows if row['status'] == 'rejected'}
    failed = {row['id'] for row in rows if row['status'] == 'failed'}
    not_run = {row['id'] for row in rows if row['status'] == 'not_run'}
    missing = set(declared) - {row['id'] for row in rows}
    out_of_scope = {case_id for case_id, case in declared.items()
                    if case_scope(case)[0] == 'not_run'}
    unexpected_not_run = not_run - out_of_scope
    for case_id, case in declared.items():
        if case_id in out_of_scope:
            continue
        if case['expected']['compile'] == 'accept' and case_id not in passed:
            failed.add(case_id)
        if case['expected']['compile'] == 'reject' and case_id not in rejected:
            failed.add(case_id)
    old_passed = sum(row['status'] == 'passed' for row in report['old_source'])
    old_failed = len(OLD_SOURCE) - old_passed
    return {
        'cases': len(cases),
        'expected_accept': sum(case['expected']['compile'] == 'accept'
                               for case in cases),
        'expected_reject': sum(case['expected']['compile'] == 'reject'
                               for case in cases),
        'accepted': len(passed & set(declared)),
        'rejected': len(rejected & set(declared)),
        'not_run': len(not_run & set(declared)),
        'unexpected_not_run': len(unexpected_not_run),
        'failed': len(failed) + old_failed,
        'missing': len(missing),
        'old_source_passed': old_passed,
        'old_source_total': len(OLD_SOURCE),
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--python', type=str, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(operation_id='python-reference-values-20261114',
                  contract='compiler-dev/1', status='failed',
                  old_source=[], reference_values=[],
                  notes=[
                      'Part 1 keeps the old-source Python execution assertions.',
                      'Part 2 follows tests/reference_values/cases.json '
                      'expected.compile/main_return for all 24 cases; reject '
                      'codes are the confirmed closure/Weak source rules, not '
                      'a generic unsupported baseline.',
                      'A command timeout or unclean process tree is a failure, '
                      'never a pass.',
                      'Cases may declare backends; thread-capture-scalar-arg '
                      'is explicitly Native-only and is reported not_run on '
                      'Python, never passed. No other case is skipped.',
                  ])
    cases = []
    try:
        context = Context(args.root.resolve(), Path(args.cli).resolve(),
                          args.python)
        old_source_rows(context, report, args.out)
        cases = reference_value_rows(context, report, args.out)
    except Exception as error:  # runner-level, still write the report
        report['runner_error'] = f'{type(error).__name__}: {error}'
    finally:
        if not cases:
            try:
                cases = json.loads(
                    (args.root / 'tests/reference_values/cases.json'
                     ).read_text())['cases']
            except (OSError, json.JSONDecodeError):
                cases = []
        report['summary'] = summary(report, cases) if cases else {}
        report['status'] = ('passed' if cases
                            and report['summary']['failed'] == 0
                            and report['summary']['missing'] == 0
                            and report['summary']['unexpected_not_run'] == 0
                            and report['summary']['old_source_passed'] ==
                            len(OLD_SOURCE) else 'failed')
        (args.out / 'report.json').write_text(
            json.dumps(report, indent=1) + '\n')
        print(json.dumps({'status': report['status'],
                          **report['summary']}))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
