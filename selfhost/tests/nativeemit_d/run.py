#!/usr/bin/env python3
"""Check the D async/GC share of nativeemit with real NativeIR fixtures.

The syntax pass always runs against every production d_*.wz file.  The raw
executable pass uses a frozen real B/C/E package as the base (never a
VerifiedModule stub), overlays the production nativeemit files under test,
builds a NativeIR driver project at O0 and O2, then verifies every generated
LLVM module, runs the LLVM 22 coroutine pipeline, compiles with llc, links
against the real native runtime and executes the runnable cases.

The raw pass does not require the independent nativeverify package.  A missing
real VerifiedModule public entry is reported as public_emit=not_run; it is never
reported as a pass.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / 'tests/native'))
from gc_ir_contract import (  # noqa: E402
    ContractError, check_gc_poll_contract, expected_safepoints,
)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--woozyc', default=str(ROOT / 'build/woozy-stdlib-native/woozyc'))
parser.add_argument('--toolchain-root', default=str(ROOT.parent / 'toolchains'))
parser.add_argument('--runtime', default=None,
                    help='liblang_native_runtime.a path; defaults next to --woozyc')
parser.add_argument('--source-root', default=str(ROOT / 'selfhost/src/wzcc'))
parser.add_argument('--nativeemit-source', default=None,
                    help='production nativeemit package to test (default source-root/nativeemit)')
parser.add_argument('--candidate-source', default=None,
                    help='read-only frozen source root with real B/C/E/nativelayout/valueops')
parser.add_argument('--work-dir', default=None,
                    help='parent for a fresh results directory; existing files are not removed')
parser.add_argument('--timeout', type=float, default=600)
parser.add_argument('--only', choices=('all', 'raw', 'contract'), default='all')
parser.add_argument('--force-run', action='store_true',
                    help='attempt the raw pass even when the candidate scan looks incomplete')
args = parser.parse_args()
if args.timeout <= 0:
    parser.error('--timeout must be positive')
if args.work_dir:
    Path(args.work_dir).expanduser().resolve().mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(prefix='woozy-nativeemit-d-',
    dir=Path(args.work_dir).expanduser().resolve() if args.work_dir else None))
SOURCE_ROOT = Path(args.source_root).expanduser().resolve()
NATIVEEMIT = (Path(args.nativeemit_source).expanduser().resolve()
              if args.nativeemit_source else SOURCE_ROOT / 'nativeemit')
CANDIDATE = Path(args.candidate_source).expanduser().resolve() if args.candidate_source else None
TOOLCHAIN = Path(args.toolchain_root).expanduser().resolve()
TOOLS = TOOLCHAIN / 'llvm-22.1.8/bin'
RUNTIME = (Path(args.runtime).expanduser().resolve() if args.runtime
           else Path(args.woozyc).expanduser().resolve().parent / 'native/liblang_native_runtime.a')
STAGE0 = Path(args.woozyc).expanduser().resolve()
ENV = dict(os.environ)
compat = str(TOOLCHAIN / 'libxml2-legacy-2.13.9/usr/lib')
ENV['LD_LIBRARY_PATH'] = compat + (':' + ENV['LD_LIBRARY_PATH'] if ENV.get('LD_LIBRARY_PATH') else '')
ENV['TMPDIR'] = str(WORK)

SELFHOST_HIR_BLOCK = re.compile(r'^b[0-9]+\.[0-9]+$')
SELFHOST_ASYNC_FUNCTION = re.compile(r'^lang\.async\.[0-9]+$')
SELFHOST_ASYNC_DEFINE = re.compile(
    r'@(?:"lang\.async\.[0-9]+"|lang\.async\.[0-9]+)\s*\('
)
SAFEPOINT_SEQUENCE = re.compile(
    r'(?m)^[ \t]*%[-\w.$]+ = load atomic volatile i64, ptr '
    r'@"lang_gc_safepoint_word_v1" acquire, align 8\n'
    r'[ \t]*%[-\w.$]+ = icmp sle i64 [^\n]+, 0\n'
    r'[ \t]*br i1 [^\n]+, label %[^\n]+, label %[^\n]+, !prof [^\n]+\n'
)

REQUIRED_D = [
    'scan_c_resource_flags', 'type_owns_c_resource', 'promise_type',
    'coroutine_signature', 'declare_async', 'async_begin',
    'copy_async_ordinary_parameters', 'build_async_frame_gc',
    'async_publish_and_suspend', 'emit_async_terminator', 'async_finish',
    'emit_async_main', 'has_strong_edge', 'gc_trace_value',
    'gc_register_vec', 'gc_register_fields', 'gc_poll',
    'gc_loop_safepoints', 'gc_collect_loop_exit', 'gc_enter_critical',
    'gc_leave_critical', 'gc_enter_safe', 'gc_leave_safe',
]

RAW_CASES = [
    # index, name, expected exit, stderr substring (empty means exactly empty)
    (0, 'async-main-normal-42', 42, ''),
    (1, 'async-main-cancel', 1, ''),
    (2, 'async-main-typed-error-i32', 1, 'LANG_TYPED_ERROR: i32'),
    (3, 'await-unit-unit', 0, ''),
    (4, 'await-i32-unit', 0, ''),
    (5, 'vec-strong-edge-trace-clear', 0, ''),
    (6, 'owning-task-payload-normal-repeat-clear', 31, ''),
    (7, 'owning-task-payload-error-repeat-clear', 32, ''),
    (8, 'owning-task-payload-cancel-repeat-clear', 33, ''),
]

records = []

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def selfhost_safepoint_contract(text):
    """Check the Wz emitter's real HIR blocks, not the C++ b<N> convention."""
    if not SELFHOST_ASYNC_DEFINE.search(text):
        return None
    return check_gc_poll_contract(
        text, target_pattern=SELFHOST_ASYNC_FUNCTION,
        hir_block=SELFHOST_HIR_BLOCK)

def selfhost_contract_negative_checks(text):
    """Minimal counterexamples: a missing poll and an extra misplaced poll."""
    mutated, count = SAFEPOINT_SEQUENCE.subn('', text, count=1)
    if count != 1:
        raise RuntimeError('safepoint counterexample setup: no request sequence')
    try:
        selfhost_safepoint_contract(mutated)
    except ContractError:
        missing_rejected = True
    else:
        missing_rejected = False
    if not missing_rejected:
        raise RuntimeError('IR contract accepted a missing safepoint poll')

    sequence = SAFEPOINT_SEQUENCE.search(text)
    if sequence is None:
        raise RuntimeError('safepoint counterexample setup: no poll sequence')
    lines = text.splitlines(keepends=True)
    labels = []
    for index, line in enumerate(lines):
        match = re.match(r'^(?P<name>"?[-\w.$]+"?):(?:\s*;.*)?\n?$', line)
        if match is not None:
            labels.append((index, match.group('name').strip('"')))
    insert_at = None
    for position, (index, name) in enumerate(labels):
        if name.startswith('gc.poll.') or name.startswith('entry.'):
            continue
        end = labels[position + 1][0] if position + 1 < len(labels) else len(lines)
        body = ''.join(lines[index + 1:end])
        if 'lang_gc_safepoint_word_v1' in body:
            continue
        insert_at = index + 1
        break
    if insert_at is None:
        raise RuntimeError('safepoint counterexample setup: no target block')
    misplaced = ''.join(lines[:insert_at]) + sequence.group(0) + ''.join(
        lines[insert_at:])
    try:
        selfhost_safepoint_contract(misplaced)
    except ContractError:
        misplaced_rejected = True
    else:
        misplaced_rejected = False
    if not misplaced_rejected:
        raise RuntimeError('IR contract accepted a misplaced safepoint poll')
    return {'missing_rejected': missing_rejected,
            'misplaced_rejected': misplaced_rejected}

def selfhost_loop_position_check():
    """The Wz block names b<N>.<label> must still expose loop header/exit."""
    text = (
        'define void @"lang.async.0"(i1 %c) {\n'
        '"entry.0":\n'
        '  br label %"b0.1"\n'
        '"b0.1":\n'
        '  br label %"b1.2"\n'
        '"b1.2":\n'
        '  br i1 %c, label %"b1.2", label %"b2.3"\n'
        '"b2.3":\n'
        '  ret void\n'
        '}\n'
    )
    got = expected_safepoints(text, SELFHOST_HIR_BLOCK)['lang.async.0']
    want = {'b0.1', 'b1.2', 'b2.3'}
    if got != want:
        raise RuntimeError(
            f'loop safepoint positions: expected {sorted(want)}, got {sorted(got)}')
    return {'entry': 'b0.1', 'header': 'b1.2', 'exit': 'b2.3'}

def write_records():
    (WORK / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')

def command(label, argv, expected=0, timeout=None):
    result = subprocess.run([str(x) for x in argv], env=ENV,
                            capture_output=True,
                            timeout=timeout if timeout is not None else args.timeout)
    (WORK / (label + '.stdout')).write_bytes(result.stdout)
    (WORK / (label + '.stderr')).write_bytes(result.stderr)
    records.append({'label': label, 'argv': [str(x) for x in argv],
                    'exit': result.returncode, 'expected_exit': expected,
                    'stdout_bytes': len(result.stdout),
                    'stdout_sha256': hashlib.sha256(result.stdout).hexdigest(),
                    'stderr_bytes': len(result.stderr),
                    'stderr_sha256': hashlib.sha256(result.stderr).hexdigest()})
    write_records()
    if result.returncode != expected:
        raise RuntimeError(f'{label}: exit {result.returncode} != {expected}: '
                           + result.stderr.decode(errors='replace')[:2000])
    return result

def command_allow(label, argv, expected=0, timeout=None):
    """Record a command without aborting the case on a non-expected exit."""
    result = subprocess.run([str(x) for x in argv], env=ENV,
                            capture_output=True,
                            timeout=timeout if timeout is not None else args.timeout)
    (WORK / (label + '.stdout')).write_bytes(result.stdout)
    (WORK / (label + '.stderr')).write_bytes(result.stderr)
    records.append({'label': label, 'argv': [str(x) for x in argv],
                    'exit': result.returncode, 'expected_exit': expected,
                    'stdout_bytes': len(result.stdout),
                    'stdout_sha256': hashlib.sha256(result.stdout).hexdigest(),
                    'stderr_bytes': len(result.stderr),
                    'stderr_sha256': hashlib.sha256(result.stderr).hexdigest()})
    write_records()
    return result

def copy_package(source_root, project, package):
    target = project / 'src/wzcc' / package
    target.mkdir(parents=True, exist_ok=True)
    for source in sorted((source_root / package).rglob('*.wz')):
        relative = source.relative_to(source_root / package)
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)

def build_base_project(label):
    project = WORK / ('project-' + label)
    for package_dir in sorted(CANDIDATE.iterdir()):
        if package_dir.is_dir() and any(package_dir.glob('*.wz')):
            copy_package(CANDIDATE, project, package_dir.name)
    # Overlay only the production files under test.  B/C/E stay the frozen real
    # candidate; no other worker's source is copied back into production.
    native = project / 'src/wzcc/nativeemit'
    native.mkdir(parents=True, exist_ok=True)
    for source in sorted(NATIVEEMIT.glob('*.wz')):
        shutil.copy2(source, native / source.name)
    (project / 'lang.toml').write_text(
        'schema_version=1\n[package]\nname="nativeemit_d_' + label + '"\n'
        'source_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
    return project

def prerequisite_reason():
    missing = []
    if not NATIVEEMIT.is_dir():
        missing.append(str(NATIVEEMIT))
    for name in ['state.wz', 'text.wz']:
        if not (NATIVEEMIT / name).is_file():
            missing.append(str(NATIVEEMIT / name))
    if not list(NATIVEEMIT.glob('d_*.wz')):
        missing.append(str(NATIVEEMIT / 'd_*.wz'))
    if CANDIDATE is None:
        for pattern, label in [('b_*.wz', 'B'), ('c_*.wz', 'C'), ('e_*.wz', 'E')]:
            if not list(NATIVEEMIT.glob(pattern)):
                missing.append(f'production nativeemit {label} ({pattern})')
        for package in ['nativelayout', 'valueops']:
            if not list((SOURCE_ROOT / package).glob('*.wz')):
                missing.append(str(SOURCE_ROOT / package))
    else:
        for pattern, label in [('b_*.wz', 'B'), ('c_*.wz', 'C'), ('e_*.wz', 'E')]:
            if not list((CANDIDATE / 'nativeemit').glob(pattern)):
                missing.append(f'candidate nativeemit {label} ({pattern})')
        for package in ['nativelayout', 'valueops']:
            if not list((CANDIDATE / package).glob('*.wz')):
                missing.append(str(CANDIDATE / package))
    if not RUNTIME.is_file():
        missing.append(str(RUNTIME))
    for tool in ['opt', 'llc', 'clang', 'ld.lld']:
        if not (TOOLS / tool).is_file():
            missing.append(str(TOOLS / tool))
    return None if not missing else 'missing real inputs: ' + ', '.join(missing)

def split_cases(data):
    cases = {}
    chunks = data.split(b'== CASE ')
    for chunk in chunks[1:]:
        head, body = chunk.split(b'\n', 1)
        cases[int(head)] = body
    return cases

def verify_case(label, ll, expected_exit, expected_stderr):
    result = {'label': label, 'status': 'failed'}
    if ll.read_bytes().startswith(b'ERROR '):
        result['error'] = ll.read_text().strip()
        return result
    verify = command_allow(label + '-verify',
        [TOOLS / 'opt', '-passes=verify', '-disable-output', ll])
    if verify.returncode != 0:
        result['error'] = 'LLVM verify failed: ' + verify.stderr.decode(errors='replace').strip()
        return result
    coro = command_allow(label + '-coro',
        [TOOLS / 'opt', '-S', '-passes=verify,default<O' + str(opt) + '>,verify',
         '-verify-each', '-mtriple=x86_64-unknown-linux-gnu', '-mcpu=generic',
         ll, '-o', WORK / (label + '.post.ll')])
    if coro.returncode != 0:
        result['error'] = 'coroutine pipeline failed: ' + coro.stderr.decode(errors='replace').strip()
        return result
    obj = WORK / (label + '.o')
    llc = command_allow(label + '-llc',
        [TOOLS / 'llc', '-filetype=obj', '-O=' + str(opt),
         '-mtriple=x86_64-unknown-linux-gnu', '-mcpu=generic',
         '-relocation-model=pic', WORK / (label + '.post.ll'), '-o', obj])
    if llc.returncode != 0:
        result['error'] = 'llc failed: ' + llc.stderr.decode(errors='replace').strip()
        return result
    binary = WORK / (label + '.program')
    link = command_allow(label + '-link',
        [TOOLS / 'clang', '--target=x86_64-unknown-linux-gnu', '-nostdlib',
         '-static', '--ld-path=' + str(TOOLS / 'ld.lld'), obj, RUNTIME,
         '-o', binary])
    if link.returncode != 0:
        result['error'] = 'link failed: ' + link.stderr.decode(errors='replace').strip()
        return result
    execute = command_allow(label + '-execute', [binary], expected=expected_exit)
    stderr_text = execute.stderr.decode(errors='replace')
    if execute.returncode != expected_exit:
        result['error'] = f'execute exit {execute.returncode} != {expected_exit}'
        return result
    if expected_stderr:
        if expected_stderr not in stderr_text:
            result['error'] = f'stderr does not contain {expected_stderr!r}'
            return result
    elif stderr_text != '':
        result['error'] = f'stderr is not empty: {stderr_text.strip()}'
        return result
    result['status'] = 'passed'
    result['exit'] = execute.returncode
    result['stderr'] = stderr_text
    return result

result = {'status': 'failed', 'work_dir': str(WORK), 'd_files': [],
          'syntax': 'not_run', 'contract': 'not_run', 'raw': 'not_run',
          'public_emit': 'not_run', 'cases': [], 'records_file': str(WORK / 'commands.json')}
opt = 0
try:
    d_files = sorted(NATIVEEMIT.glob('d_*.wz'))
    if not d_files:
        raise RuntimeError('no d_*.wz files found in ' + str(NATIVEEMIT))
    for source in d_files:
        result['d_files'].append({'path': str(source), 'sha256': sha(source),
                                  'bytes': source.stat().st_size})
    all_text = ''.join(source.read_text() for source in d_files)
    missing_functions = [name for name in REQUIRED_D
                         if ('func ' + name + '(') not in all_text.replace(' ', ' ')
                         and ('func ' + name + ' (') not in all_text]
    # A permissive textual check: the real compile below is authoritative.
    if missing_functions:
        result['missing_d_functions_note'] = missing_functions
    if 'verified = true' in all_text:
        raise RuntimeError('D file contains a fake verification claim')

    for source in d_files:
        command('syntax-' + source.name,
                [STAGE0, 'check', '--syntax-only', source, '--format', 'json'])
    result['syntax'] = 'passed'

    reason = prerequisite_reason()
    if reason and not args.force_run:
        result['status'] = 'not_run'
        result['execution_reason'] = reason
    else:
        # 1. Real contract driver (existing main.wz): promise/signature, pure
        # safepoint walk and the no-async guard.
        contract_status = 'not_run'
        if args.only in ('all', 'contract'):
            try:
                project = build_base_project('contract')
                shutil.copy2(HERE / 'main.wz', project / 'src/main.wz')
                command('contract-check', [STAGE0, 'check', '--project',
                        project / 'lang.toml', '--backend', 'native', '--format', 'json'])
                for level in (0, 2):
                    out_dir = WORK / f'contract-O{level}'
                    command(f'contract-build-O{level}', [STAGE0, 'build', '--project',
                            project / 'lang.toml', '--backend', 'native', '--opt', level,
                            '--out-dir', out_dir, '--format', 'json'])
                    command(f'contract-run-O{level}', [out_dir / 'program'])
                contract_status = 'passed'
            except Exception as error:
                contract_status = 'failed: ' + str(error)
        result['contract'] = contract_status

        # 2. Raw NativeIR driver, O0 and O2.
        raw_status = 'not_run'
        if args.only in ('all', 'raw'):
            project = build_base_project('raw')
            shutil.copy2(HERE / 'raw/driver.wz', project / 'src/wzcc/nativeemit/review_d.wz')
            shutil.copy2(HERE / 'raw/main.wz', project / 'src/main.wz')
            command('raw-check', [STAGE0, 'check', '--project', project / 'lang.toml',
                    '--backend', 'native', '--format', 'json'])
            raw_failed = False
            contract_checks = 0
            negative_checks = None
            loop_check = None
            for level in (0, 2):
                opt = level
                out_dir = WORK / f'raw-O{level}'
                command(f'raw-build-O{level}', [STAGE0, 'build', '--project',
                        project / 'lang.toml', '--backend', 'native', '--opt', level,
                        '--out-dir', out_dir, '--format', 'json'])
                emitted = command(f'raw-emit-O{level}', [out_dir / 'program'])
                cases = split_cases(emitted.stdout)
                for index, name, expected_exit, expected_stderr in RAW_CASES:
                    label = f'O{level}-case{index}-{name}'
                    if index not in cases:
                        result['cases'].append({'label': label,
                            'status': 'failed', 'error': 'driver emitted no case'})
                        raw_failed = True
                        continue
                    ll = WORK / (label + '.ll')
                    ll.write_bytes(cases[index])
                    text = ll.read_text(encoding='utf-8', errors='replace')
                    try:
                        contract = selfhost_safepoint_contract(text)
                        if level == 0 and contract is not None:
                            negative_checks = selfhost_contract_negative_checks(text)
                            loop_check = selfhost_loop_position_check()
                        contract_checks += 1 if contract is not None else 0
                    except (ContractError, RuntimeError) as error:
                        result['cases'].append({
                            'label': label, 'opt': level, 'status': 'failed',
                            'error': f'IR safepoint contract: {error}'})
                        raw_failed = True
                        continue
                    case = verify_case(label, ll, expected_exit, expected_stderr)
                    case['opt'] = level
                    if contract is not None:
                        case['safepoint_contract'] = contract
                    result['cases'].append(case)
                    if case['status'] != 'passed':
                        raw_failed = True
                write_records()
            raw_status = 'failed' if raw_failed else 'passed'
            result['safepoint_contracts_checked'] = contract_checks
            result['safepoint_negative_checks'] = negative_checks
            result['safepoint_loop_check'] = loop_check
        result['raw'] = raw_status

        # 3. Full public Verified emit entry.  This worktree has no
        # nativeverify package; never claim it from raw fixtures.
        verify_pkg = SOURCE_ROOT / 'nativeverify'
        if list(verify_pkg.glob('*.wz')) and args.only in ('all', 'contract'):
            result['public_emit'] = 'not_run: Verified entry integration is outside this raw driver'
        else:
            result['public_emit'] = 'not_run: no real nativeverify VerifiedModule entry in ' + str(verify_pkg)

        if result['raw'] == 'passed' and result['contract'] in ('passed', 'not_run'):
            result['status'] = 'passed'
        elif result['raw'] == 'failed' or str(result['contract']).startswith('failed'):
            result['status'] = 'failed'
        else:
            result['status'] = 'not_run'
except Exception as error:
    result['status'] = 'failed'
    result['error'] = str(error)
    raise
finally:
    write_records()
    (WORK / 'results.json').write_text(json.dumps(result, indent=2) + '\n')

print(json.dumps({k: v for k, v in result.items() if k != 'cases'}, indent=2))
for case in result['cases']:
    print(json.dumps(case, sort_keys=True))
