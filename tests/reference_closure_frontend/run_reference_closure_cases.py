#!/usr/bin/env python3
"""Build and execute the independent reference closure/bound/function cases.

This is the runtime half of tests/reference_closure_frontend. It does not
contain a second .wz interpreter: all checking and code generation goes through
the real woozyc driver. Expected values are the independent checks inside the
source fixtures (main returns 0 only when every assertion holds).

A command timeout or a process-tree cleanup failure is recorded as a failed
case and the runner continues with the remaining cases; the final report is
printed from a ``finally`` block and the process exits 1 when anything failed.
All subprocesses go through ``selfhost/corpus.py::_run_process_impl`` (finite
deadline and process-group cleanup).

Per-case ``backends`` metadata in ``tests/reference_values/cases.json`` is
honoured: the explicit Native-only thread case is run on Native and recorded
``not_run`` on Python with its declared reason.  A case without ``backends``
keeps the default both-backend rule.  No other failure is auto-skipped.

Negative cases are checked with ``check`` and then with real Native O0/O2
builds. The build must reject with the same declared code and must not publish
``program``/``program.py``; a pre-existing output file in the target directory
must survive the rejected build unchanged.
"""
import argparse
import importlib.util
import json
import math
import os
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True

POSITIVE = [
    "closure-copy-at-creation-sibling",
    "closure-creator-scope-escape",
    "closure-generic-two-instances",
    "closure-move-owning-and-scalar",
    "closure-nested-transitive",
    "closure-readonly-shared-mutation",
    "closure-two-capture-types",
    "fn-return-field-vec",
    "fn-throws-defer-capture",
    "bound-ordinary-repeat-copy",
    "bound-move-repeat-copy",
    "indirect-callee-arg-throw",
    "thread-capture-scalar-arg",
]
# Codes are the confirmed rejection rules for these three fixtures; cases.json
# stores the required behavior and the "no unrelated unsupported failure" rule
# but no diagnostic_codes field.
NEGATIVE = {
    "closure-move-owning-read-after": "E_OWNERSHIP_USE_AFTER_MOVE",
    "closure-readonly-assign": "E_TYPE_MISMATCH",
    "bound-inout-reject": "E_CALL_ARGUMENT",
}
COMMAND_TIMEOUT = 120.0


class CommandFailed(Exception):
    def __init__(self, message, argv=None, returncode=None, kind='command'):
        super().__init__(message)
        self.argv = list(argv) if argv is not None else []
        self.returncode = returncode
        self.kind = kind


class Result:
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
    def __init__(self, root, cli, python, opt):
        self.root = root
        self.cli = cli
        self.python = python
        self.opt = opt
        self.cwd = Path.cwd()
        self.env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
                        PYTHONNOUSERSITE='1')
        self.env.pop('PYTHONPATH', None)
        spec = importlib.util.spec_from_file_location(
            'reference_closure_corpus', root / 'selfhost/corpus.py')
        self.corpus = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.corpus)
        cases = json.loads(
            (root / 'tests/reference_values/cases.json').read_text())['cases']
        self.case_backends = {case['id']: case.get('backends')
                              for case in cases}
        self.case_notes = {case['id']: case.get('backend_note')
                           for case in cases}


def checked_timeout(timeout):
    if (isinstance(timeout, bool) or
            not isinstance(timeout, (int, float)) or
            not math.isfinite(timeout) or timeout <= 0):
        raise ValueError(f'timeout must be finite and positive: {timeout!r}')
    return float(timeout)


def command(context, argv, timeout=COMMAND_TIMEOUT):
    timeout = checked_timeout(timeout)
    command_argv = [str(part) for part in argv]
    try:
        process = context.corpus._run_process_impl(
            command_argv, context.cwd, context.env, timeout)
    except OSError as error:
        raise CommandFailed(
            f'OS error {type(error).__name__}: {error}; '
            f'argv={command_argv!r}', command_argv, kind='os_error') from error
    result = Result(command_argv, process['exit'],
                    process['stdout'].decode('utf-8', 'replace'),
                    process['stderr'].decode('utf-8', 'replace'))
    if process['timed_out']:
        raise CommandFailed(f'timed out after {timeout:g}s', result.args,
                            result.returncode, kind='timeout')
    if not process['cleanup_complete'] or process['abandoned_pipes']:
        raise CommandFailed('process tree was not cleaned up', result.args,
                            result.returncode, kind='cleanup')
    return result


def parsed_stdout(result):
    try:
        payload = json.loads(result.stdout or '{}')
    except json.JSONDecodeError:
        return None
    return payload if isinstance(payload, dict) else None


def execute_phase(context, row, key, argv):
    try:
        result = command(context, argv)
    except CommandFailed as error:
        phase = {'failed': True, 'kind': error.kind, 'error': str(error),
                 'argv': error.argv}
        if error.kind == 'timeout':
            phase['timed_out'] = True
        row['phases'][key] = phase
        raise
    row['phases'][key] = {'exit': result.returncode}
    return result


def fail(results, row, message):
    row['status'] = 'failed'
    row['detail'] = message
    results['failed'].append(message)


def positive_case(name, context, results, temp):
    row = {'case': name, 'status': 'running', 'phases': {}}
    results['cases'].append(row)
    opt = context.opt
    native_out = temp / f"native-{opt}-{name}"
    try:
        built = execute_phase(context, row, 'native_build', [
            context.cli, "build", "--project",
            context.root / "tests/reference_values/fixtures" / name / "lang.toml",
            "--backend", "native", "--opt", str(opt),
            "--out-dir", native_out])
    except CommandFailed as error:
        return fail(results, row,
                    f"{name}: native O{opt} build {error}")
    if built.returncode != 0:
        return fail(results, row,
                    f"{name}: native O{opt} build failed: "
                    f"{built.stdout}{built.stderr}")
    program = native_out / "program"
    if not program.is_file():
        return fail(results, row,
                    f"{name}: native O{opt} build published no program")
    native_ok = True
    try:
        executed = execute_phase(context, row, 'native_execute', [program])
    except CommandFailed as error:
        native_ok = False
        fail(results, row, f"{name}: native O{opt} execute {error}")
    else:
        if executed.returncode != 0:
            native_ok = False
            fail(results, row,
                 f"{name}: native O{opt} returned {executed.returncode}: "
                 f"{executed.stdout}{executed.stderr}")
        else:
            results['passed'].append(f"{name}: native O{opt}")

    backends = getattr(context, 'case_backends', {}).get(name)
    if backends is not None:
        if (not isinstance(backends, list) or not backends
                or any(item not in ('native', 'python') for item in backends)):
            return fail(results, row,
                        f"{name}: invalid backends metadata: {backends!r}")
        if 'python' not in backends:
            note = getattr(context, 'case_notes', {}).get(name)
            if not isinstance(note, str) or not note.strip():
                note = (f'declared backends {backends}; python is not a '
                        f'declared target')
            results['not_run'].append(f"{name}: {note}")
            if row['status'] != 'failed':
                row['status'] = 'not_run'
            return None

    pyout = temp / f"python-{name}"
    try:
        built = execute_phase(context, row, 'python_build', [
            context.cli, "build", "--project",
            context.root / "tests/reference_values/fixtures" / name / "lang.toml",
            "--backend", "python", "--out-dir", pyout])
    except CommandFailed as error:
        return fail(results, row, f"{name}: python build {error}")
    if built.returncode != 0:
        return fail(results, row,
                    f"{name}: python build failed: "
                    f"{built.stdout}{built.stderr}")
    try:
        executed = execute_phase(context, row, 'python_execute', [
            context.python, "-s", pyout / "program.py"])
    except CommandFailed as error:
        return fail(results, row, f"{name}: python execute {error}")
    if executed.returncode != 0:
        return fail(results, row,
                    f"{name}: python returned {executed.returncode}: "
                    f"{executed.stdout}{executed.stderr}")
    results['passed'].append(f"{name}: python")
    if native_ok:
        row['status'] = 'passed'
    return None


def negative_case(name, code, context, results, temp):
    row = {'case': name, 'status': 'running', 'phases': {},
           'expected_code': code}
    results['cases'].append(row)
    manifest = (context.root / "tests/reference_values/fixtures" / name /
                "lang.toml")
    try:
        checked = execute_phase(context, row, 'check', [
            context.cli, "check", "--project", manifest, "--format", "json"])
    except CommandFailed as error:
        return fail(results, row, f"{name}: check {error}")
    payload = parsed_stdout(checked) or {}
    diagnostics = payload.get('diagnostics', [])
    codes = [item.get('code') for item in diagnostics]
    row['diagnostics'] = diagnostics
    if (checked.returncode == 0 or payload.get('status') != 'error'
            or code not in codes):
        return fail(results, row,
                    f"{name}: expected rejection {code}, got "
                    f"{checked.returncode} {codes} "
                    f"status={payload.get('status')!r}")

    for opt in (0, 2):
        fresh = temp / f"rejected-native-o{opt}-{name}"
        try:
            built = execute_phase(context, row, f'native_o{opt}_build', [
                context.cli, "build", "--project", manifest,
                "--backend", "native", "--opt", str(opt),
                "--out-dir", fresh, "--format", "json"])
        except CommandFailed as error:
            return fail(results, row,
                        f"{name}: native O{opt} build {error}")
        built_payload = parsed_stdout(built) or {}
        built_codes = [item.get('code') for item in
                       built_payload.get('diagnostics', [])]
        row['phases'][f'native_o{opt}_build']['codes'] = built_codes
        row['phases'][f'native_o{opt}_build']['status'] = \
            built_payload.get('status')
        row['phases'][f'native_o{opt}_build']['output_dir_created'] = \
            fresh.exists()
        if built.returncode == 0:
            return fail(results, row,
                        f"{name}: native O{opt} build accepted a reject case")
        if built_payload.get('status') != 'error' or code not in built_codes:
            return fail(results, row,
                        f"{name}: native O{opt} build expected {code}, got "
                        f"status={built_payload.get('status')!r} "
                        f"codes {built_codes}")
        if (fresh / "program").exists() or (fresh / "program.py").exists():
            return fail(results, row,
                        f"{name}: native O{opt} reject wrote a program")

        kept = temp / f"existing-native-o{opt}-{name}"
        kept.mkdir()
        (kept / "program").write_bytes(b"old-program")
        (kept / "keep.txt").write_bytes(b"keep")
        try:
            built = execute_phase(context, row, f'native_o{opt}_existing', [
                context.cli, "build", "--project", manifest,
                "--backend", "native", "--opt", str(opt),
                "--out-dir", kept, "--format", "json"])
        except CommandFailed as error:
            return fail(results, row,
                        f"{name}: native O{opt} existing-output {error}")
        kept_payload = parsed_stdout(built) or {}
        kept_codes = [item.get('code') for item in
                      kept_payload.get('diagnostics', [])]
        if built.returncode == 0:
            return fail(results, row,
                        f"{name}: native O{opt} existing-output build accepted")
        if kept_payload.get('status') != 'error' or code not in kept_codes:
            return fail(results, row,
                        f"{name}: native O{opt} existing-output expected "
                        f"{code}, got status={kept_payload.get('status')!r} "
                        f"codes {kept_codes}")
        if (not (kept / "program").is_file()
                or (kept / "program").read_bytes() != b"old-program"):
            return fail(results, row,
                        f"{name}: native O{opt} replaced existing program")
        if (not (kept / "keep.txt").is_file()
                or (kept / "keep.txt").read_bytes() != b"keep"):
            return fail(results, row,
                        f"{name}: native O{opt} changed an existing file")
        if (kept / "program.py").exists():
            return fail(results, row,
                        f"{name}: native O{opt} existing output gained program.py")
    results['passed'].append(f"{name}: rejected {code}")
    row['status'] = 'passed'
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--woozyc", required=True)
    parser.add_argument("--root", default=".")
    parser.add_argument("--python", default="python3.12")
    parser.add_argument("--opt", type=int, choices=(0, 2), default=0)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    results = {"passed": [], "not_run": [], "failed": [], "cases": []}
    try:
        context = Context(root, Path(args.woozyc).resolve(), args.python,
                          args.opt)
        with tempfile.TemporaryDirectory(prefix="woozy-closure-run-") as temp:
            temporary = Path(temp)
            for name in POSITIVE:
                try:
                    positive_case(name, context, results, temporary)
                except OSError as error:
                    # File-system OSErrors outside a command still belong to
                    # this case; record it and keep the remaining cases.
                    row = (results['cases'][-1] if results['cases'] else None)
                    if row is None or row.get('case') != name:
                        row = {'case': name, 'status': 'failed', 'phases': {}}
                        results['cases'].append(row)
                    fail(results, row,
                         f"{name}: OS error outside command: "
                         f"{type(error).__name__}: {error}")
            for name, code in NEGATIVE.items():
                try:
                    negative_case(name, code, context, results, temporary)
                except OSError as error:
                    row = (results['cases'][-1] if results['cases'] else None)
                    if row is None or row.get('case') != name:
                        row = {'case': name, 'status': 'failed', 'phases': {}}
                        results['cases'].append(row)
                    fail(results, row,
                         f"{name}: OS error outside command: "
                         f"{type(error).__name__}: {error}")
    except Exception as error:  # keep the partial report and exit non-zero
        results["failed"].append(
            f"runner: uncaught {type(error).__name__}: {error}")
    finally:
        print(json.dumps({"status": "ok" if not results["failed"] else "failed",
                          "opt": args.opt, "results": results}, indent=2))
    return 0 if not results["failed"] else 1


if __name__ == "__main__":
    sys.exit(main())
