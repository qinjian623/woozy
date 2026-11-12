#!/usr/bin/env python3
"""Build, run and observe real Source Weak programs on Python/64.

Every positive program is compiled through the public ``woozyc`` driver with
``--backend python`` and the private ``lang_python_emit`` helper.  The generated
``program.py``/``langrt.py`` hashes must match, then the program runs under
``observe.py`` with CPython's automatic collector disabled.  Rejections are
required to report the stated Source/capability diagnostic and to emit no
program.  The captured-Fn case is recorded as unsupported, never as a pass.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

# Reference-value cases already in the accepted Root integration snapshot.
ROOT_POSITIVE = (
    ('weak-vec-lifecycle', 'tests/reference_values/fixtures/weak-vec-lifecycle/lang.toml', 0),
    ('weak-box-lifecycle', 'tests/reference_values/fixtures/weak-box-lifecycle/lang.toml', 0),
    ('weak-empty-upgrade-none', 'tests/reference_values/fixtures/weak-empty-upgrade-none/lang.toml', 0),
    ('weak-fn-immortal', 'tests/reference_values/fixtures/weak-fn-immortal/lang.toml', 0),
    ('weak-fn-captured-dead', 'tests/reference_values/fixtures/weak-fn-captured-dead/lang.toml', 0),
)

LOCAL_POSITIVE = (
    ('combo-fields-vec-enum', 0),
    ('held-some-keeps-alive', 0),
    ('generic-target', 0),
)

# Selected old Source projects: function values, a generic function argument,
# and shared ownership through a struct copy.
OLD_SOURCE = (
    ('old-function-values', 'tests/backend_common/fixtures/function-values/lang.toml', 0),
    ('old-generic-function-argument', 'tests/generics/fixtures/function-argument/lang.toml', 5),
    ('old-struct-copy-shares-vec', 'tests/ownership/fixtures/struct-copy-shares-vec/lang.toml', 0),
)

REJECT = {
    'reject-user-struct': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-same-shape-struct': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-string': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-file': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-slice': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-foreign': ('E_TYPE_MISMATCH', 'Weak target must be'),
    'reject-native-handle': ('E_CAPABILITY_UNAVAILABLE',
                             'backend does not support this concrete function signature'),
}

CAPTURED_FN = 'tests/reference_values/fixtures/weak-fn-captured-dead/lang.toml'


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, timeout, log):
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    env.pop('PYTHONPATH', None)
    done = subprocess.run(
        [str(value) for value in argv], capture_output=True, text=True,
        timeout=timeout, env=env)
    log.append({'argv': [str(value) for value in argv],
                'exit': done.returncode,
                'stdout': done.stdout,
                'stderr': done.stderr})
    return done


def payload(result):
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f'expected JSON, got {result.stdout!r}: {error}')


def build_both(manifest, out, timeout, log, cli, helper):
    cli_result = command(
        [cli, 'build', '--project', manifest, '--backend', 'python',
         '--out-dir', out / 'cli', '--format', 'json'], timeout, log)
    cli_doc = payload(cli_result)
    helper_result = command(
        [helper, '--project', manifest, '--out-dir', out / 'helper'],
        timeout, log)
    helper_doc = payload(helper_result)
    return cli_result, cli_doc, helper_result, helper_doc


def build_positive(name, manifest, expected_return, args, out_root, log, hashes,
                   require_weak=True):
    out = out_root / 'positive' / name
    out.mkdir(parents=True, exist_ok=True)
    cli_result, cli_doc, helper_result, helper_doc = build_both(
        manifest, out, args.timeout, log, args.cli, args.helper)
    if cli_result.returncode != 0 or cli_doc.get('status') != 'ok':
        raise RuntimeError(f'{name}: CLI build failed: {cli_doc}')
    if cli_doc.get('diagnostics'):
        raise RuntimeError(f'{name}: CLI diagnostics: {cli_doc}')
    if helper_result.returncode != 0 or helper_doc.get('status') != 'ok':
        raise RuntimeError(f'{name}: helper build failed: {helper_doc}')
    for generated in ('program.py', 'langrt.py'):
        if digest(out / 'cli' / generated) != digest(out / 'helper' / generated):
            raise RuntimeError(f'{name}: CLI/helper disagree on {generated}')
    program = (out / 'cli' / 'program.py').read_text(encoding='utf-8')
    if require_weak and "_rt.ValueType('weak')" not in program:
        raise RuntimeError(f'{name}: emitted program has no Weak descriptor')
    observed = command(
        [args.python, '-s', Path(__file__).with_name('observe.py'),
         out / 'cli', str(expected_return)], args.timeout, log)
    document = payload(observed)
    if observed.returncode != 0 or document.get('status') != 'passed':
        raise RuntimeError(f'{name}: observer rejected {document}')
    for path in sorted(manifest.parent.rglob('*')):
        if path.is_file():
            hashes[str(path.relative_to(args.root))] = digest(path)
    for generated in ('program.py', 'langrt.py'):
        path = out / 'cli' / generated
        hashes[str(path.relative_to(args.out))] = digest(path)
    return {'manifest': str(manifest.relative_to(args.root)),
            'main_return': expected_return,
            'observation': document,
            'generated': {name: digest(out / 'cli' / name)
                          for name in ('program.py', 'langrt.py')}}


def build_reject(name, code, message, args, out_root, log):
    manifest = args.root / 'tests/python/weak_source/fixtures' / name / 'lang.toml'
    out = out_root / 'negative' / name
    out.mkdir(parents=True, exist_ok=True)
    cli_result, cli_doc, helper_result, helper_doc = build_both(
        manifest, out, args.timeout, log, args.cli, args.helper)
    for label, result, document in (('CLI', cli_result, cli_doc),
                                    ('helper', helper_result, helper_doc)):
        if result.returncode == 0 or document.get('status') != 'error':
            raise RuntimeError(f'{name}: {label} did not reject: {document}')
        diagnostics = document.get('diagnostics', [])
        codes = [item.get('code') for item in diagnostics]
        messages = ' '.join(item.get('message', '') for item in diagnostics)
        if code not in codes:
            raise RuntimeError(f'{name}: {label} expected {code}, got {codes}')
        if message not in messages:
            raise RuntimeError(f'{name}: {label} missing message {message!r}: {messages}')
    if (out / 'cli' / 'program.py').exists() or (out / 'helper' / 'program.py').exists():
        raise RuntimeError(f'{name}: rejected case wrote a program')
    return {'manifest': str(manifest.relative_to(args.root)),
            'expected_code': code,
            'expected_message': message,
            'diagnostics': [item.get('code') for item in cli_doc['diagnostics']],
            'helper_diagnostics': [item.get('code') for item in helper_doc['diagnostics']]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--python', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=120)
    args = parser.parse_args()
    for key in ('cli', 'helper', 'root', 'out', 'python'):
        value = getattr(args, key)
        if not value.is_absolute():
            found = shutil.which(str(value))
            value = Path(found) if found else value
        setattr(args, key, value.resolve())
    args.out.mkdir(parents=True, exist_ok=True)
    log = []
    hashes = {}
    summary = {'status': 'failed', 'python': None,
               'root_positive': {}, 'local_positive': {}, 'old_source': {},
               'rejects': {}, 'unsupported': {}, 'commands': log,
               'hashes': hashes}

    version = command([args.python, '--version'], 20, log)
    if version.stdout.strip() != 'Python 3.12.15':
        raise RuntimeError(f'Python 3.12.15 required: {version.stdout}')
    summary['python'] = version.stdout.strip()

    try:
        for name, manifest_text, expected in ROOT_POSITIVE:
            summary['root_positive'][name] = build_positive(
                name, args.root / manifest_text, expected, args,
                args.out / 'root', log, hashes)
        for name, expected in LOCAL_POSITIVE:
            manifest = (args.root / 'tests/python/weak_source/fixtures' / name /
                        'lang.toml')
            summary['local_positive'][name] = build_positive(
                name, manifest, expected, args, args.out / 'local', log, hashes)
        for name, manifest_text, expected in OLD_SOURCE:
            summary['old_source'][name] = build_positive(
                name, args.root / manifest_text, expected, args,
                args.out / 'old', log, hashes, require_weak=False)
        for name, (code, message) in REJECT.items():
            summary['rejects'][name] = build_reject(
                name, code, message, args, args.out, log)

        summary['status'] = 'passed'
    except Exception as error:
        summary['error'] = f'{type(error).__name__}: {error}'

    (args.out / 'report.json').write_text(
        json.dumps(summary, indent=1, sort_keys=True) + '\n')
    print(json.dumps({
        'status': summary['status'],
        'python': summary['python'],
        'root_positive': sorted(summary['root_positive']),
        'local_positive': sorted(summary['local_positive']),
        'old_source': sorted(summary['old_source']),
        'rejects': sorted(summary['rejects']),
        'unsupported': summary['unsupported'],
        'error': summary.get('error'),
    }, sort_keys=True))
    return 0 if summary['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
