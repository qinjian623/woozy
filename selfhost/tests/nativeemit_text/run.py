#!/usr/bin/env python3
"""Exercise the shared Wz text helpers only, without rebuilding the compiler."""
from pathlib import Path
import argparse
import hashlib
import tempfile
import json
import os
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
from wzcc_closure import complete_import_closure  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--woozyc', default=str(ROOT / 'build/woozy-stdlib-native/woozyc'))
parser.add_argument('--toolchain-root', default=str(ROOT.parent / 'toolchains'))
parser.add_argument('--source-root', default=str(ROOT / 'selfhost/src/wzcc'))
parser.add_argument('--nativeemit-source', default=None,
                    help='override only the nativeemit package for an unmerged draft')
parser.add_argument('--work-dir', default=None,
                    help='parent for a fresh results directory; existing files are not removed')
parser.add_argument('--timeout', type=float, default=180)
args = parser.parse_args()
if args.timeout <= 0:
    parser.error('--timeout must be positive')
if args.work_dir:
    Path(args.work_dir).expanduser().resolve().mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(prefix='woozy-nativeemit-text-',
                           dir=Path(args.work_dir).expanduser().resolve() if args.work_dir else None))
SOURCE_ROOT = Path(args.source_root).expanduser().resolve()
NATIVEEMIT = Path(args.nativeemit_source).expanduser().resolve() if args.nativeemit_source else SOURCE_ROOT / 'nativeemit'
TOOLCHAIN = Path(args.toolchain_root).expanduser().resolve()
TOOLS = TOOLCHAIN / 'llvm-22.1.8/bin'
STAGE0 = Path(args.woozyc).expanduser().resolve()
PROJECT = WORK / 'project'
LINUX = 'x86_64-unknown-linux-gnu'
DARWIN = 'arm64-apple-macosx11.0.0'
ENV = dict(os.environ)
compat = str(TOOLCHAIN / 'libxml2-legacy-2.13.9/usr/lib')
ENV['LD_LIBRARY_PATH'] = compat + (':' + ENV['LD_LIBRARY_PATH'] if ENV.get('LD_LIBRARY_PATH') else '')



def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
        raise RuntimeError(f'{label}: {result.returncode}: stdout={result.stdout.decode(errors="replace")} stderr={result.stderr.decode(errors="replace")}')
    return result.stdout


def object_file(label, target, level, source):
    before = WORK / f'generated-{label}.ll'
    after = WORK / f'verified-{label}.ll'
    output = WORK / f'generated-{label}.o'
    before.write_bytes(source)
    passes = 'verify' if level == 0 else 'verify,default<O2>,verify'
    command(f'opt-{label}', [TOOLS / 'opt', '-S', '-passes=' + passes, '-verify-each',
            '-mtriple=' + target, '-mcpu=generic', before, '-o', after])
    command(f'llc-{label}', [TOOLS / 'llc', '-filetype=obj', f'-O={level}',
            '-mtriple=' + target, '-mcpu=generic', '-relocation-model=pic',
            '-code-model=small', after, '-o', output])
    return output


records = []
inputs = []
try:
    (PROJECT / 'src/wzcc').mkdir(parents=True, exist_ok=True)
    (PROJECT / 'lang.toml').write_text('schema_version=1\n[package]\nname="nativeemit_text_check"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
    for package in ['model', 'types', 'hir', 'nativeir']:
        target = PROJECT / 'src/wzcc' / package
        target.mkdir(exist_ok=True)
        for source in sorted((SOURCE_ROOT / package).glob('*.wz')):
            (target / source.name).write_bytes(source.read_bytes())
            inputs.append({'source': str(source), 'sha256': sha(source)})
    target = PROJECT / 'src/wzcc/nativeemit'
    target.mkdir(exist_ok=True)
    # Test the production text helpers even while other emitter files are incomplete.
    # SH37 made state/text reference the same-package debug.wz.  Package import
    # edges do not expose that dependency, so select debug.wz explicitly, but
    # keep selecting only state/text for trees that predate it.
    uses_debug = ('DebugState' in (NATIVEEMIT / 'state.wz').read_text() or
                  'debug_line(' in (NATIVEEMIT / 'text.wz').read_text())
    names = ['state.wz', 'text.wz'] + (['debug.wz'] if uses_debug else [])
    for filename in names:
        source = NATIVEEMIT / filename
        if not source.is_file():
            raise RuntimeError(f'missing nativeemit dependency {source}')
        (target / source.name).write_bytes(source.read_bytes())
        inputs.append({'source': str(source), 'sha256': sha(source)})
    if uses_debug:
        # debug_scalar calls B's private type_of.  Stage that one production
        # body instead of all of B (or a stand-in type table).
        source = HERE / 'fixture_type_of.wz'
        (target / source.name).write_bytes(source.read_bytes())
        inputs.append({'source': str(source), 'sha256': sha(source)})
        if (NATIVEEMIT / 'debug_types.wz').is_file():
            source = HERE / 'fixture_debug_type.wz'
            (target / source.name).write_bytes(source.read_bytes())
            inputs.append({'source': str(source), 'sha256': sha(source)})
    (PROJECT / 'src/main.wz').write_bytes((HERE / 'main.wz').read_bytes())
    inputs.append({'source': str(HERE / 'main.wz'), 'sha256': sha(HERE / 'main.wz')})
    # SH31 added interface/digest imports to nativeemit/state.wz.  Copy only
    # the real packages that the staged sources import, never a scaffold or a
    # copy of an unrelated main-tree package.
    skipped_existing = []
    for record in complete_import_closure(PROJECT / 'src', SOURCE_ROOT,
                                           skipped=skipped_existing):
        inputs.append({'source': record['source'], 'sha256': record['sha256']})
    for source in [STAGE0, STAGE0.parent / 'native/lang_native_helper']:
        inputs.append({'source': str(source), 'sha256': sha(source)})
    (WORK / 'inputs.json').write_text(json.dumps(inputs, indent=2))

    layouts = {}
    for target in (LINUX, DARWIN):
        probe = command(f'clang-layout-{target}', [TOOLS / 'clang', '--target=' + target,
                        '-S', '-emit-llvm', '-x', 'c', '-', '-o', '-'], input_bytes=b'')
        layouts[target] = re.search(rb'target datalayout = "([^"]+)"', probe).group(1)
    for name in ['clang', 'opt', 'llc', 'ld.lld']:
        version = command(name + '-version', [TOOLS / name, '--version'])
        assert b'22.1.8' in version

    command('wz-check', [STAGE0, 'check', '--project', PROJECT / 'lang.toml',
                        '--backend', 'native', '--format', 'json'])
    sources = []
    darwin_sources = []
    for level in [0, 2]:
        command(f'wz-build-O{level}', [STAGE0, 'build', '--project', PROJECT / 'lang.toml',
                '--backend', 'native', '--opt', level, '--out-dir', WORK / f'O{level}', '--format', 'json'])
        source = command(f'wz-run-O{level}', [WORK / f'O{level}/program'])
        assert source.count(b'define i32 @"main"()') == 1
        assert b'declare i32 @"twice"' not in source
        assert b'%v0 = add i32 20, 1' in source and b'%v0 = mul i32 %p0, 2' in source
        assert b'phi i32' in source and b'!tbaa' in source
        assert b'c"\\00\\22\\5C\\80\\FF"' in source
        assert b'fmul double' in source and b'fadd double' in source
        assert b' fast ' not in source and b' contract ' not in source
        assert re.search(rb'target datalayout = "([^"]+)"', source).group(1) == layouts[LINUX]
        sources.append(source)
        program = WORK / f'generated-O{level}'
        linux_object = object_file(f'O{level}', LINUX, level, source)
        command(f'link-O{level}', [TOOLS / 'clang', '--target=' + LINUX,
                '--ld-path=' + str(TOOLS / 'ld.lld'), linux_object, '-o', program])
        command(f'generated-run-O{level}', [program], expected=42)

        darwin = command(f'wz-darwin-O{level}', [WORK / f'O{level}/program', DARWIN])
        assert f'target triple = "{DARWIN}"'.encode() in darwin
        assert re.search(rb'target datalayout = "([^"]+)"', darwin).group(1) == layouts[DARWIN]
        assert b'define i32 @"main"()' in darwin
        darwin_sources.append(darwin)
        darwin_object = object_file(f'darwin-O{level}', DARWIN, level, darwin)
        header = command(f'readobj-darwin-O{level}', [TOOLS / 'llvm-readobj', '--file-headers',
                         darwin_object])
        assert b'Format: Mach-O arm64' in header and b'FileType: Relocatable' in header
    assert sources[0] == sources[1]
    assert darwin_sources[0] == darwin_sources[1]
    darwin_debug = command('darwin-debug', [WORK / 'O0/program', DARWIN, '-g'])
    object_file('darwin-debug', DARWIN, 0, darwin_debug)
    for name, arguments, message in (
        ('unknown-target', ['x86_64-apple-macosx11.0.0'], b'native LLVM emission supports'),
    ):
        stdout = command(name, [WORK / 'O0/program', *arguments], expected=3)
        assert not stdout
        error = (WORK / (name + '.stderr')).read_bytes()
        assert b'E_CAPABILITY_UNAVAILABLE' in error and message in error
    result = {'status': 'passed', 'scope': 'Wz shared text helpers only; no language emitter bodies',
              'work_dir': str(WORK),
              'llvm_ir_bytes': len(sources[0]), 'llvm_ir_sha256': hashlib.sha256(sources[0]).hexdigest(),
              'data_layout': layouts[LINUX].decode(),
              'darwin_data_layout': layouts[DARWIN].decode(),
              'skipped_existing_packages': skipped_existing,
              'records': records}
except Exception as error:
    result = {'status': 'failed', 'error': str(error), 'records': records}
    raise
finally:
    (WORK / 'results.json').write_text(json.dumps(result, indent=2))

print(json.dumps({k: v for k, v in result.items() if k != 'records'}, indent=2))
