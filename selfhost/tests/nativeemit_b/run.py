#!/usr/bin/env python3
"""SH08b (nativeemit B) check and end-to-end text-emission harness.

This runner never substitutes scaffolds or stubs for production packages.
B's owned files come from ``--b-source-root`` (the worktree under test) while
the real sibling packages (C/D/E, nativelayout and valueops) are copied
read-only from ``--source-root`` into one temporary project.  A missing
package is a hard error naming the absent files, not a skipped or "not_run"
success.

What it runs
------------
1. Parses/resolves/type-checks the combined production package with stage0
   ``woozyc check``.
2. Builds and runs a Wz self-check that exercises B's real pure helpers and
   drives B's production code paths (declare_source_functions, emit_function,
   emit_main, sections) over a synthetic NativeIR module.  The module includes
   real VecInit/VecSet/VecRead/VecLength instructions (emitted by C through
   B's vec_field/vec_memory TBAA helpers), integer identity/widening/narrowing
   conversions, float-to-i64 conversion and the existing scalar/CFG/main
   cases.  The harness exits 84 only when the returned program value is 84.
3. Feeds the emitted pre-optimization LLVM text to LLVM 22.1.8:
   ``opt -passes=verify``, ``llc -O0``, links the existing native runtime and
   runs the generated program, expecting exit code 84.  It also runs
   ``opt -passes=default<O2>,verify``.

The full CPP/Wz differential corpus, the Root public emit wrapper/nativeverify
pipeline, and async/C-adapter execution remain outside this B harness and are
reported as not_run.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--woozyc', default=str(ROOT / 'build/woozy-stdlib-native/woozyc'))
parser.add_argument('--toolchain-root', default=str(ROOT.parent / 'toolchains'))
parser.add_argument('--b-source-root', default=str(ROOT / 'selfhost/src/wzcc'),
                    help='source tree that owns the B files under test')
parser.add_argument('--source-root', default=str(ROOT / 'selfhost/src/wzcc'),
                    help='source tree containing the real C/D/E, nativelayout and valueops packages')
parser.add_argument('--runtime', default=str(ROOT / 'build/woozy-stdlib-native/native/liblang_native_runtime.a'))
parser.add_argument('--groups', choices=['tree', 'scaffold'], default='tree',
                    help='tree: real sibling packages are required. scaffold was removed '
                         'because a stand-in must not be reported as production.')
parser.add_argument('--work-dir', default=None,
                    help='parent for a fresh results directory; existing files are not removed')
parser.add_argument('--timeout', type=float, default=1200,
                    help='per-command timeout in seconds (the full package build can take several minutes)')
args = parser.parse_args()
if args.groups != 'tree':
    parser.error('--groups scaffold was removed: real C/D/E, nativelayout and valueops are required')
if args.timeout <= 0:
    parser.error('--timeout must be positive')
if args.work_dir:
    Path(args.work_dir).expanduser().resolve().mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(prefix='woozy-nativeemit-b-',
                             dir=Path(args.work_dir).expanduser().resolve() if args.work_dir else None))
B_SOURCE_ROOT = Path(args.b_source_root).expanduser().resolve()
SOURCE_ROOT = Path(args.source_root).expanduser().resolve()
TOOLCHAIN = Path(args.toolchain_root).expanduser().resolve()
TOOLS = TOOLCHAIN / 'llvm-22.1.8/bin'
LLVM_LIB = TOOLCHAIN / 'llvm-22.1.8/lib'
XML_LIB = TOOLCHAIN / 'libxml2-legacy-2.13.9/usr/lib'
STAGE0 = Path(args.woozyc).expanduser().resolve()
RUNTIME = Path(args.runtime).expanduser().resolve()
PROJECT = WORK / 'project'
ENV = dict(os.environ)
# The stage0 compiler and the LLVM tools must see both the LLVM 22.1.8 shared
# libraries and the legacy libxml2 used by the frozen bootstrap.
ENV['LD_LIBRARY_PATH'] = str(LLVM_LIB) + ':' + str(XML_LIB) + (
    ':' + ENV['LD_LIBRARY_PATH'] if ENV.get('LD_LIBRARY_PATH') else '')
ENV['TMPDIR'] = str(WORK)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


records = []
inputs = []


def record_input(path):
    inputs.append({'source': str(path), 'sha256': sha(path)})


def command(label, argv, expected=0, input_bytes=None):
    result = subprocess.run([str(x) for x in argv], input=input_bytes, env=ENV,
                            capture_output=True, timeout=args.timeout)
    (WORK / (label + '.stdout')).write_bytes(result.stdout)
    (WORK / (label + '.stderr')).write_bytes(result.stderr)
    records.append({'label': label, 'argv': [str(x) for x in argv],
                    'exit': result.returncode, 'expected_exit': expected,
                    'stdout_bytes': len(result.stdout),
                    'stdout_sha256': hashlib.sha256(result.stdout).hexdigest(),
                    'stderr_bytes': len(result.stderr)})
    if result.returncode != expected:
        raise RuntimeError(f'{label}: exit {result.returncode} '
                           f'!= {expected}: {result.stderr.decode(errors="replace")}'
                           f'{result.stdout.decode(errors="replace")}')
    return result.stdout


def require_files(package, paths):
    missing = [str(path) for path in paths if not path.is_file()]
    if missing:
        raise RuntimeError('missing required production dependency package '
                           f'{package}: ' + ', '.join(missing))


def copy_package(package):
    target = PROJECT / 'src/wzcc' / package
    target.mkdir(parents=True, exist_ok=True)
    sources = sorted((SOURCE_ROOT / package).glob('*.wz'))
    if not sources:
        raise RuntimeError(f'missing required production dependency package {package}: '
                           f'{SOURCE_ROOT / package} has no .wz files')
    for source in sources:
        shutil.copyfile(source, target / source.name)
        record_input(source)


try:
    for label, path in [('stage0', STAGE0), ('runtime', RUNTIME)]:
        if not path.is_file():
            raise RuntimeError(f'missing {label}: {path}')
    for label, path in [('LLVM tools', TOOLS), ('LLVM libs', LLVM_LIB),
                        ('legacy libxml2', XML_LIB)]:
        if not path.is_dir():
            raise RuntimeError(f'missing {label}: {path}')

    (PROJECT / 'src/wzcc').mkdir(parents=True, exist_ok=True)
    (PROJECT / 'lang.toml').write_text(
        'schema_version=1\n[package]\nname="nativeemit_b_harness"\nsource_root="src"\n'
        'entry="app"\n[build]\nbackend="native"\nword_bits=64\n')

    for package in ['model', 'types', 'hir', 'nativeir', 'interface', 'digest',
                    'nativeadapter']:
        copy_package(package)

    # Production Root text/state/debug plus every B file comes from the B owner.
    nativeemit = PROJECT / 'src/wzcc/nativeemit'
    nativeemit.mkdir(exist_ok=True)
    b_dir = B_SOURCE_ROOT / 'nativeemit'
    b_files = [b_dir / 'state.wz', b_dir / 'text.wz'] + sorted(b_dir.glob('b_*.wz'))
    # SH37 made state/text and b_function/b_ops reference nativeemit/debug.wz.
    # It is a same-package dependency, so no package import edge exposes it;
    # stage it when the selected B tree has it and fail early when a debug-using
    # B tree lacks it.  Older debug-free trees keep their original selection.
    debug = b_dir / 'debug.wz'
    if debug.is_file():
        b_files.append(debug)
    debug_types = b_dir / 'debug_types.wz'
    if debug_types.is_file():
        b_files.append(debug_types)
    require_files('nativeemit/B', b_files)
    if not debug.is_file() and any('debug_' in path.read_text() for path in b_files):
        raise RuntimeError(f'missing required production dependency package '
                           f'nativeemit/B: {debug}')
    for source in b_files:
        shutil.copyfile(source, nativeemit / source.name)
        record_input(source)

    # Real sibling C/D/E files.  They are read-only inputs; B never copies a
    # scaffold for them.
    sibling = sorted(list((SOURCE_ROOT / 'nativeemit').glob('c_*.wz')) +
                     list((SOURCE_ROOT / 'nativeemit').glob('d_*.wz')) +
                     list((SOURCE_ROOT / 'nativeemit').glob('e_*.wz')))
    for prefix, label in [('c_*.wz', 'C'), ('d_*.wz', 'D'), ('e_*.wz', 'E')]:
        if not list((SOURCE_ROOT / 'nativeemit').glob(prefix)):
            raise RuntimeError(f'missing required production dependency package '
                               f'nativeemit/{label}: {SOURCE_ROOT / "nativeemit" / prefix}')
    for source in sibling:
        shutil.copyfile(source, nativeemit / source.name)
        record_input(source)

    # Root-owned frozen state/text/debug must be identical in both trees when
    # the sibling tree contains a copy; a mismatch is a merge error, not a
    # reason to silently pick one.
    for name in ['state.wz', 'text.wz', 'debug.wz', 'debug_types.wz']:
        owned = b_dir / name
        other = SOURCE_ROOT / 'nativeemit' / name
        if not owned.is_file():
            continue
        if other.is_file() and sha(other) != sha(owned):
            raise RuntimeError(f'Root-owned nativeemit/{name} differs between '
                               f'B root {owned} and sibling root {other}')

    copy_package('nativelayout')
    copy_package('valueops')

    shutil.copyfile(HERE / 'selfcheck.wz', nativeemit / 'zz_selfcheck.wz')
    record_input(HERE / 'selfcheck.wz')
    shutil.copyfile(HERE / 'main.wz', PROJECT / 'src/main.wz')
    record_input(HERE / 'main.wz')
    record_input(STAGE0)
    record_input(RUNTIME)
    (WORK / 'inputs.json').write_text(json.dumps(inputs, indent=2))

    for name in ['clang', 'opt', 'llc']:
        version = command(name + '-version', [TOOLS / name, '--version'])
        assert b'22.1.8' in version

    # 1. Real syntax/resolve/type check of the combined production package.
    command('wz-check', [STAGE0, 'check', '--project', PROJECT / 'lang.toml',
                         '--backend', 'native', '--format', 'json'])

    # 2. Build and run the self-check; stdout is the emitted pre-opt LLVM text.
    command('wz-build', [STAGE0, 'build', '--project', PROJECT / 'lang.toml',
                         '--backend', 'native', '--opt', '0',
                         '--out-dir', WORK / 'out', '--format', 'json'])
    source = command('harness-run', [WORK / 'out/program'], expected=0)
    llvm = WORK / 'generated.ll'
    llvm.write_bytes(source)
    records.append({'label': 'generated-sha256',
                    'sha256': hashlib.sha256(source).hexdigest(),
                    'bytes': len(source)})

    text = source.decode()
    # B-owned text that must be present.  The sibling files may add helpers.
    present = {
        'define i32 @"main"()': 1,
        'define internal i1 @"lang.fn.10"(ptr %p0, ptr %p1)': 1,
        'define internal i32 @"lang.fn.11"(i32 %p0)': 1,
        'define internal i32 @"lang.fn.12"()': 1,
        'define internal i1 @"lang.fn.13"(ptr %p0, ptr %p1)': 1,
        'define internal i1 @"lang.fn.14"(i32 %p0, ptr %p1, ptr %p2)': 1,
        'define internal i32 @"lang.fn.15"()': 1,
        'define internal i32 @"lang.fn.16"(i32 %p0)': 1,
        'define internal i64 @"lang.fn.17"()': 1,
        'define internal i32 @"lang.fn.18"()': 1,
        'define internal i32 @"lang.fn.19"()': 1,
        'define internal i1 @"lang.fn.20"()': 1,
        'define internal i1 @"lang.fn.21"()': 1,
        'define internal i8 @"lang.fn.22"()': 1,
        'ret i1 false': 1,
        'ret i1 true': 1,
        'ret i8 0': 1,
        'call i32 @"lang.fn.11"(i32 21)': 1,
        'call i32 @"lang.fn.12"()': 1,
        'call i1 @"lang.fn.13"(ptr %v0, ptr %v1)': 1,
        'call i1 @"lang.fn.14"(i32 7, ptr %v0, ptr %v1)': 1,
        'call i32 @"lang.fn.15"()': 1,
        'division.wrap': 6,
        'division.result': 6,
        'phi i32': 2,
        'sdiv i32': 1,
        'srem i32': 1,
        'fneg double': 1,
        'fcmp olt double': 1,
        'fptosi double': 2,
        'icmp eq i32 2, 0': 2,
        'call void @"lang_native_panic_v1"': 4,
        'lang_gc_runtime_poll_v1': 14,
        'lang_gc_program_finish_v1': 3,
        'lang_native_panic_v1': 5,
        'string.bytes.0': 1,
        'icmp ne i64 0, 0': 1,
        'lang_vec_new_v1': 1,
        'llvm.memcpy.p0.p0.i64': 1,
        '!"lang.vec.storage"': 1,
        '!"descriptor"': 1,
        '!"elements"': 1,
        'sext i32 ': 1,
        'trunc i64 ': 1,
        'ret i64 ': 1,
        '= type { i32, i32 }': 1,
        '= type { i32, { i32 }, {} }': 1,
        'insertvalue %"value.': 4,
        'extractvalue %"value.': 4,
    }
    for needle, count in present.items():
        found = text.count(needle)
        records.append({'label': 'text:' + needle, 'count': found,
                        'min_expected': count})
        assert found >= count, f'{needle}: {found} < {count}'
    # Flags the emitter must never add, plus the old invalid metadata-string
    # spelling that produced c"..." inside !{...}.
    for needle in ['fast', 'nsw', 'nuw', 'noalias', 'contract',
                   'c"lang.vec.storage"', 'c"descriptor"', 'c"elements"',
                   'trunc i32 ']:
        found = text.count(needle)
        records.append({'label': 'absent:' + needle, 'count': found})
        assert found == 0, f'{needle}: {found} != 0'
    assert 'bitcast (i64 4611686018427387904 to double)' in text
    assert 'bitcast (i64 14114281232179134464 to double)' in text
    assert 'bitcast (i64 4890909195324358656 to double)' in text
    assert re.search(r'target datalayout = "([^"]+)"', text).group(1) == \
        'e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128'
    assert 'c"a\\22b\\5Cc\\09d\\C3\\A9"' in text
    # No same-width trunc may appear anywhere (the integer/float conversion
    # bug would spell `trunc i32 ... to i32` or `trunc i64 ... to i64`).
    assert not re.search(r'trunc i(\d+) [^\n]+ to i\1\b', text), \
        'same-width trunc emitted'
    # Real Vec instruction bodies were emitted, not only the TBAA definitions.
    assert 'getelementptr inbounds %"lang.vec"' in text
    assert 'load i64, ptr ' in text and '!tbaa !' in text

    # 3. LLVM verification, code generation and execution against the runtime.
    verified = WORK / 'verified.ll'
    command('opt-verify', [TOOLS / 'opt', '-S', '-passes=verify', '-verify-each',
                           '-mtriple=x86_64-unknown-linux-gnu', '-mcpu=generic',
                           llvm, '-o', verified])
    object_file = WORK / 'generated.o'
    command('llc', [TOOLS / 'llc', '-filetype=obj', '-O=0',
                    '-mtriple=x86_64-unknown-linux-gnu', '-mcpu=generic',
                    '-relocation-model=pic', '-code-model=small',
                    verified, '-o', object_file])
    command('opt-O2', [TOOLS / 'opt', '-S', '-passes=default<O2>,verify',
                       '-mtriple=x86_64-unknown-linux-gnu', '-mcpu=generic',
                       verified, '-o', WORK / 'optimized.ll'])
    program = WORK / 'generated'
    command('link', [TOOLS / 'clang', '--target=x86_64-unknown-linux-gnu',
                     '--ld-path=' + str(TOOLS / 'ld.lld'), object_file,
                     RUNTIME, '-lpthread', '-lm', '-o', program])
    command('generated-run', [program], expected=84)

    result = {'status': 'passed', 'groups': args.groups,
              'scope': 'nativeemit B production files + real C/D/E, nativelayout and valueops: '
                       'package check, synthetic NativeIR text emission, LLVM verify/link/run',
              'work_dir': str(WORK),
              'b_source_root': str(B_SOURCE_ROOT),
              'sibling_source_root': str(SOURCE_ROOT),
              'llvm_ir_bytes': len(source),
              'llvm_ir_sha256': hashlib.sha256(source).hexdigest(),
              'scaffolded_packages': [],
              'not_run': [
                  'Root public emit(...) wrapper and nativeverify.VerifiedModule pipeline '
                  '(this harness drives B over a synthetic NativeIR module)',
                  'full CPP/Wz differential corpus, bad-CFG/NativeIR rejection, and all '
                  'source programs through the new nativeemit (Root driver)',
                  'async/coroutine emission, closure/dyn and C adapter end-to-end runtime '
                  'execution (other owners not exercised by this B harness)',
              ],
              'records': records}
except Exception as error:
    result = {'status': 'failed', 'error': str(error), 'records': records,
              'b_source_root': str(B_SOURCE_ROOT),
              'sibling_source_root': str(SOURCE_ROOT)}
    raise
finally:
    (WORK / 'results.json').write_text(json.dumps(result, indent=2))

print(json.dumps({k: v for k, v in result.items() if k != 'records'}, indent=2))
