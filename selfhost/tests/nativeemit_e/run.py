#!/usr/bin/env python3
"""Check the E nativeemit sources and, when the whole package is present, run
the C adapter generation fixture.

The script never uses the old C++ frontend/backend to perform production work.
It runs stage0 only as the bootstrap compiler and as the test oracle.  When the
sibling B/C/D/layout/verify files have not been merged yet, the check is
reported as `partial` and full-package build/execution is explicitly `not_run`.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
from wzcc_closure import complete_import_closure  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--woozyc', default=str(ROOT / 'build/woozy-stdlib-native/woozyc'))
parser.add_argument('--toolchain-root', default=str(ROOT.parent / 'toolchains'))
parser.add_argument('--source-root', default=str(ROOT / 'selfhost/src/wzcc'))
parser.add_argument('--e-source', default=str(HERE.parents[1] / 'src/wzcc/nativeemit'),
                    help='directory holding the E files under test')
parser.add_argument('--work-dir', default=None)
parser.add_argument('--timeout', type=float, default=240)
args = parser.parse_args()
if args.timeout <= 0:
    parser.error('--timeout must be positive')
if args.work_dir:
    Path(args.work_dir).expanduser().resolve().mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(prefix='woozy-nativeemit-e-',
                            dir=Path(args.work_dir).expanduser().resolve() if args.work_dir else None))
SOURCE_ROOT = Path(args.source_root).expanduser().resolve()
E_SOURCE = Path(args.e_source).expanduser().resolve()
TOOLCHAIN = Path(args.toolchain_root).expanduser().resolve()
TOOLS = TOOLCHAIN / 'llvm-22.1.8/bin'
CLANG = TOOLS / 'clang'
STAGE0 = Path(args.woozyc).expanduser().resolve()
PROJECT = WORK / 'project'
ENV = dict(os.environ)
compat = str(TOOLCHAIN / 'libxml2-legacy-2.13.9/usr/lib')
ENV['LD_LIBRARY_PATH'] = compat + (':' + ENV['LD_LIBRARY_PATH'] if ENV.get('LD_LIBRARY_PATH') else '')
ENV['TMPDIR'] = str(WORK)

# Save only the run settings needed in results.json.
RECORDED_ENV = {
    key: ENV[key]
    for key in ('PATH', 'LC_ALL', 'PYTHONDONTWRITEBYTECODE', 'TMPDIR',
                'LD_LIBRARY_PATH', 'WOOZY_TOOLCHAIN_ROOT')
    if key in ENV
}

# Cross-group names supplied by B/C/D and the layout/verify packages.  They are
# expected to be absent in a standalone E worktree; anything else is an error.
ALLOWED_MISSING = {
    'type_of', 'aggregate_declaration', 'aggregate_fields', 'field_index',
    'variant_index', 'lower_type', 'type_key', 'signature', 'find_hir_function',
    'source_function', 'call_target', 'value', 'value_type', 'constant_value',
    'storage', 'stride', 'element_storage', 'guard', 'checked_index',
    'vec_memory', 'vec_field', 'call', 'runtime', 'owning', 'value_op',
    'copy_value', 'destroy_slot', 'env_record_type', 'env_record_op',
    'make_environment', 'function_thunk', 'bound_adapter', 'function_value',
    'function_value_info', 'trait_box_op', 'trait_method_thunk', 'trait_vtable',
    'thread_bridge', 'thread_spawn', 'weak_system_call',
    'emit_value_instruction', 'scan_c_resource_flags', 'type_owns_c_resource',
    'promise_type', 'coroutine_signature', 'declare_async', 'async_begin',
    'copy_async_ordinary_parameters', 'build_async_frame_gc',
    'async_publish_and_suspend', 'emit_async_terminator', 'async_finish',
    'emit_async_main', 'has_strong_edge', 'gc_trace_value', 'gc_register_vec',
    'gc_register_fields', 'gc_poll', 'gc_loop_safepoints',
    'gc_collect_loop_exit', 'gc_enter_critical', 'gc_leave_critical',
    'gc_enter_safe', 'gc_leave_safe', 'declare_source_functions',
    'emit_function', 'emit_instruction', 'emit_terminator', 'emit_main',
    'AggregateTypes', 'aggregate_types', 'declaration', 'fields',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(label, argv, expected=0, input_bytes=None):
    result = subprocess.run([str(x) for x in argv], input=input_bytes, env=ENV,
                            capture_output=True, timeout=args.timeout)
    (WORK / (label + '.stdout')).write_bytes(result.stdout)
    (WORK / (label + '.stderr')).write_bytes(result.stderr)
    records.append({
        'label': label,
        'argv': [str(x) for x in argv],
        'expected_exit': expected,
        'exit': result.returncode,
        'timeout': args.timeout,
        'env': RECORDED_ENV,
        'stdout': {'path': str(WORK / (label + '.stdout')),
                   'bytes': len(result.stdout),
                   'sha256': hashlib.sha256(result.stdout).hexdigest()},
        'stderr': {'path': str(WORK / (label + '.stderr')),
                   'bytes': len(result.stderr),
                   'sha256': hashlib.sha256(result.stderr).hexdigest()},
    })
    if result.returncode != expected:
        raise RuntimeError(f'{label}: {result.returncode}: '
                           f'{result.stderr.decode(errors="replace")[-1000:]}')
    return result.stdout


records = []
inputs = []
result = {'status': 'failed'}
try:
    (PROJECT / 'src/wzcc').mkdir(parents=True, exist_ok=True)
    (PROJECT / 'lang.toml').write_text(
        'schema_version=1\n[package]\nname="nativeemit_e_check"\n'
        'source_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
    packages = ['model', 'types', 'hir', 'nativeir', 'nativeemit',
                'nativelayout', 'valueops', 'nativeverify', 'hirverify']
    copied = []
    has_backend_deps = (SOURCE_ROOT / 'nativelayout').is_dir() and \
        (SOURCE_ROOT / 'valueops').is_dir()
    for package in packages:
        source_dir = SOURCE_ROOT / package
        if not source_dir.is_dir():
            continue
        target = PROJECT / 'src/wzcc' / package
        target.mkdir(parents=True, exist_ok=True)
        package_sources = []
        for source in sorted(source_dir.glob('*.wz')):
            if package == 'nativeemit' and source.name.startswith('e_'):
                continue
            if package == 'nativeemit' and not has_backend_deps and \
                    source.name[:2] in ('b_', 'c_', 'd_'):
                continue
            package_sources.append(source)
        if package == 'nativeemit':
            package_sources.extend(sorted(E_SOURCE.glob('e_*.wz')))
        for source in package_sources:
            (target / source.name).write_bytes(source.read_bytes())
            inputs.append({'source': str(source), 'sha256': sha(source)})
            copied.append(package + '/' + source.name)
    (PROJECT / 'src/main.wz').write_bytes((HERE / 'main.wz').read_bytes())
    inputs.append({'source': str(HERE / 'main.wz'), 'sha256': sha(HERE / 'main.wz')})
    # SH31 added interface to the shared nativeemit state/emit files.  Copy the
    # real import closure; a missing production package is left for the check
    # stage to report with its normal diagnostics.
    closure_error = None
    skipped_existing = []
    try:
        for record in complete_import_closure(PROJECT / 'src', SOURCE_ROOT,
                                               skipped=skipped_existing):
            inputs.append({'source': record['source'], 'sha256': record['sha256']})
    except FileNotFoundError as error:
        closure_error = str(error)
    (WORK / 'inputs.json').write_text(json.dumps(inputs, indent=2))

    check = subprocess.run([str(STAGE0), 'check', '--project',
                            str(PROJECT / 'lang.toml'), '--backend', 'native',
                            '--format', 'json'], env=ENV, capture_output=True,
                           timeout=args.timeout)
    (WORK / 'check.stdout').write_bytes(check.stdout)
    (WORK / 'check.stderr').write_bytes(check.stderr)
    records.append({
        'label': 'wz-check',
        'argv': [str(STAGE0), 'check', '--project',
                 str(PROJECT / 'lang.toml'), '--backend', 'native',
                 '--format', 'json'],
        'expected_exit': 0,
        'exit': check.returncode,
        'timeout': args.timeout,
        'env': RECORDED_ENV,
        'stdout': {'path': str(WORK / 'check.stdout'),
                   'bytes': len(check.stdout),
                   'sha256': hashlib.sha256(check.stdout).hexdigest()},
        'stderr': {'path': str(WORK / 'check.stderr'),
                   'bytes': len(check.stderr),
                   'sha256': hashlib.sha256(check.stderr).hexdigest()},
    })
    document = None
    try:
        document = json.loads(check.stdout.decode().strip().splitlines()[-1])
    except (ValueError, IndexError):
        document = None
    diagnostics = (document or {}).get('diagnostics', [])
    missing = []
    other = []
    for diagnostic in diagnostics:
        if diagnostic.get('code') == 'E_NAME_NOT_FOUND':
            match = re.search(r'unknown name: ([A-Za-z_][A-Za-z0-9_]*)',
                              diagnostic.get('message', ''))
            if match and match.group(1) in ALLOWED_MISSING:
                missing.append(match.group(1))
                continue
        other.append(diagnostic)
    if document is None:
        result = {'status': 'failed', 'reason': 'check did not print JSON',
                  'check_exit': check.returncode, 'work_dir': str(WORK),
                  'records': records}
    elif check.returncode == 0 and not diagnostics:
        result = {'status': 'passed', 'scope': 'full package; adapter fixture',
                  'closure_error': closure_error,
                  'work_dir': str(WORK), 'copied': copied,
                  'skipped_existing_packages': skipped_existing,
                  'records': records}
        build_dir = WORK / 'build-O0'
        build = run('wz-build-O0', [STAGE0, 'build', '--project',
                    PROJECT / 'lang.toml', '--backend', 'native', '--opt', '0',
                    '--out-dir', build_dir, '--format', 'json'])
        program = build_dir / 'program'
        source = run('wz-run-adapter', [program])
        adapter = WORK / 'adapter.c'
        adapter.write_bytes(source)
        expected = [b'struct wf_rec_1 {', b'_Static_assert(sizeof(struct wf_rec_1) == 4',
                    b'lang_c_adapter_v1_1', b'lang_c_adapter_v1_2',
                    b'lang_c_adapter_v1_3', b'lang_c_callback_trampoline_v1_7_3',
                    b'lang_c_registration_trampoline_v1_3_0',
                    b'lang_c_registration_dispatch_v1_3_0',
                    b'lang_c_registration_unregister_v1_4',
                    b'lang_c_release_v1_2', b'lang_c_retain_v1_2',
                    b'lang_c_release_v1_4']
        absent = [item.decode() for item in expected if item not in source]
        if absent:
            raise RuntimeError('adapter source missing: ' + ', '.join(absent))
        run('clang-adapter-syntax', [CLANG,
            '--target=x86_64-unknown-linux-gnu', '-std=c11', '-fsyntax-only',
            adapter])
        result['adapter_bytes'] = len(source)
        result['adapter_sha256'] = hashlib.sha256(source).hexdigest()
        result['adapter'] = str(adapter)
    elif check.returncode != 0 and missing and not other:
        result = {'status': 'partial',
                  'scope': 'E sources parsed and name-checked; sibling groups missing',
                  'missing_cross_group': sorted(set(missing)),
                  'full_package_check': 'not_run',
                  'build': 'not_run', 'execution': 'not_run',
                  'work_dir': str(WORK), 'copied': copied,
                  'skipped_existing_packages': skipped_existing,
                  'records': records}
    else:
        result = {'status': 'failed',
                  'reason': 'unexpected check diagnostics',
                  'diagnostics': other[:50],
                  'missing_cross_group': sorted(set(missing)),
                  'check_exit': check.returncode, 'work_dir': str(WORK),
                  'records': records}
except Exception as error:
    result = {'status': 'failed', 'error': repr(error), 'work_dir': str(WORK),
              'records': records}
finally:
    (WORK / 'results.json').write_text(json.dumps(result, indent=2))

print(json.dumps({k: v for k, v in result.items() if k != 'records'}, indent=2))
if result.get('status') == 'failed':
    raise SystemExit(1)
