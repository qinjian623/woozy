#!/usr/bin/env python3
"""Compile the real Wz nativebuild package and exercise its fixed LLVM tools."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import signal
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
parser.add_argument('--work-dir')
parser.add_argument('--timeout', type=float, default=240)
args = parser.parse_args()
if args.timeout <= 0:
    parser.error('--timeout must be positive')
parent = Path(args.work_dir).expanduser().resolve() if args.work_dir else None
if parent:
    parent.mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(prefix='woozy-nativebuild-', dir=parent))
SOURCE = Path(args.source_root).expanduser().resolve()
STAGE0 = Path(args.woozyc).expanduser().resolve()
TOOLCHAIN = Path(args.toolchain_root).expanduser().resolve()
TOOLS = TOOLCHAIN / 'llvm-22.1.8/bin'
STRACE = TOOLCHAIN / 'strace-7.2/usr/bin/strace'
ENV = dict(os.environ)
compat = str(TOOLCHAIN / 'libxml2-legacy-2.13.9/usr/lib')
ENV['LD_LIBRARY_PATH'] = compat + (':' + ENV['LD_LIBRARY_PATH'] if ENV.get('LD_LIBRARY_PATH') else '')
TRIPLE = 'x86_64-unknown-linux-gnu'
records, checks, inputs = [], [], []


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(label, argv, expected=0, environment=None):
    with subprocess.Popen([str(x) for x in argv], env=ENV | (environment or {}), stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, start_new_session=True) as process:
        try:
            stdout, stderr = process.communicate(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate()
            (WORK / (label + '.stdout')).write_bytes(stdout)
            (WORK / (label + '.stderr')).write_bytes(stderr)
            raise RuntimeError(f'{label}: timeout after {args.timeout}s')
    (WORK / (label + '.stdout')).write_bytes(stdout)
    (WORK / (label + '.stderr')).write_bytes(stderr)
    records.append({'label': label, 'argv': [str(x) for x in argv], 'exit': process.returncode,
                    'expected_exit': expected, 'stdout_bytes': len(stdout),
                    'stdout_sha256': hashlib.sha256(stdout).hexdigest(), 'stderr_bytes': len(stderr)})
    if process.returncode != expected:
        raise RuntimeError(f'{label}: exit {process.returncode}; {stderr.decode(errors="replace")} {stdout.decode(errors="replace")}')
    return stdout


def bundle(binary, name, missing=None):
    root = WORK / name
    root.mkdir()
    shutil.copy2(binary, root / 'driver')  # /proc/self/exe must point inside this resource bundle.
    (root / 'toolchain').mkdir()
    for tool in ['opt', 'llc', 'clang', 'clang++', 'ld.lld']:
        if tool != missing:
            (root / 'toolchain' / tool).symlink_to(TOOLS / tool)
    (root / 'runtime').mkdir()
    for archive in ['liblang_native_runtime.a', 'liblang_hosted_runtime.a']:
        (root / 'runtime' / archive).symlink_to(STAGE0.parent / 'native' / archive)
    return root


def trace_check(label, trace, root, level, async_ir, hosted):
    value = trace.read_text()
    executed = re.findall(r'execve\("([^"\n]+)".*?= 0', value)
    allowed = {str(root / 'driver')}
    allowed.update(str(root / 'toolchain' / tool) for tool in ['opt', 'llc', 'clang', 'clang++', 'ld.lld'])
    allowed.update(str(TOOLS / tool) for tool in ['opt', 'llc', 'clang', 'clang++', 'ld.lld'])
    assert executed and set(executed) <= allowed, (label, executed)
    assert str(root / 'toolchain/ld.lld') in executed, (label, executed)
    assert '/usr/bin/ld' not in value
    passes = 'verify,default<O2>,verify' if level == 2 else ('verify,default<O0>,verify' if async_ir else 'verify')
    assert '"-passes=' + passes + '"' in value
    assert ('/verified.bc"' in value) == async_ir
    assert '"-filetype=obj"' in value and '"-O=' + str(level) + '"' in value
    assert '"-relocation-model=pic"' in value and '"-code-model=small"' in value
    assert '"--ld-path=' + str(root / 'toolchain/ld.lld') + '"' in value
    if hosted:
        assert '"-Wl,-u,lang_hosted_startup_v1"' in value and '"-pthread"' in value
    else:
        assert '"-nostdlib"' in value and '"-static"' in value
    checks.append({'name': label + '-fixed-tools', 'status': 'passed', 'executed': executed})


def build_case(label, root, level, ir=None, hosted=False, async_ir=False, adapter=None,
               library=None, triple=TRIPLE, expected=0, code=None, phase=None,
               output=None, trace=False, tool_mode=None):
    out = output or (WORK / (label + '-out'))
    argv = [root / 'driver', ir or HERE / 'simple.ll', out, str(level),
            '1' if hosted else '0', '1' if async_ir else '0', adapter or '', library or '', triple]
    trace_path = WORK / (label + '.strace')
    if trace:
        argv = [STRACE, '-f', '-s', '65535', '-e', 'trace=execve', '-o', trace_path, *argv]
    environment = {'WOOZY_NATIVEBUILD_TEST_TOOL': tool_mode} if tool_mode else None
    result = json.loads(command(label, argv, expected, environment))
    if expected == 0:
        assert result['status'] == 'ok' and result['diagnostics'] == [], result
        assert sorted(p.name for p in out.iterdir()) == ['program', 'program.o']
        command(label + '-execute', [out / 'program'], expected=42)
        if trace:
            trace_check(label, trace_path, root, level, async_ir, hosted)
    else:
        assert result['status'] == ('internal_error' if expected == 3 else 'error'), result
        assert len(result['diagnostics']) == 1, result
        diagnostic = result['diagnostics'][0]
        assert diagnostic['code'] == code and diagnostic['phase'] == phase, diagnostic
        assert diagnostic['primary'] is None
        if code in ['E_IR_INVALID', 'E_LINK_FAILED']:
            assert diagnostic['notes'] and diagnostic['notes'][0]
        if out.exists() and out.is_dir():
            assert not list(out.glob('.woozyc-native-*'))
    checks.append({'name': label, 'status': 'passed', 'driver_exit': expected,
                   'tool_bundle': 'diagnostic fixture' if tool_mode else 'fixed LLVM 22.1.8 resources',
                   'diagnostics': result['diagnostics']})
    return result


result = {'status': 'failed', 'work_dir': str(WORK)}
try:
    project = WORK / 'project'
    (project / 'src/wzcc').mkdir(parents=True)
    (project / 'lang.toml').write_text('schema_version=1\n[package]\nname="nativebuild_check"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
    packages = ['model', 'types', 'hir', 'nativeir', 'resources', 'toolio', 'nativebuild', 'render']
    sources = [p for package in packages for p in sorted((SOURCE / package).glob('*.wz'))]
    sources.append(SOURCE / 'nativeemit/api.wz')
    for source in sources:
        target = project / 'src/wzcc' / source.relative_to(SOURCE)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())
        inputs.append({'source': str(source), 'sha256': sha(target)})
    shutil.copy2(HERE / 'main.wz', project / 'src/main.wz')
    # Stage the nativebuild dependency packages without replacing the reduced
    # nativeemit API already copied above.
    skipped_existing = []
    for record in complete_import_closure(project / 'src', SOURCE,
                                           skipped=skipped_existing):
        inputs.append({'source': record['source'], 'sha256': record['sha256']})
    binary_inputs = [TOOLS / name for name in ['opt', 'llc', 'clang', 'ld.lld', 'llvm-ar']]
    binary_inputs += [STRACE, *(STAGE0.parent / 'native' / name for name in
                      ['liblang_native_runtime.a', 'liblang_hosted_runtime.a'])]
    for source in [STAGE0, STAGE0.parent / 'native/lang_native_helper', *binary_inputs, *sorted(HERE.glob('*.wz')),
                   *sorted(HERE.glob('*.ll')), *sorted(HERE.glob('*.c')), Path(__file__).resolve()]:
        inputs.append({'source': str(source), 'sha256': sha(source)})
    (WORK / 'inputs.json').write_text(json.dumps(inputs, indent=2))
    command('wz-check', [STAGE0, 'check', '--project', project / 'lang.toml', '--backend', 'native', '--format', 'json'])
    fixture = ROOT / 'tests/native/c_ffi/fixture/ffi_fixture.c'
    inputs.append({'source': str(fixture), 'sha256': sha(fixture)})
    library = WORK / 'external-library'
    library.mkdir()
    command('external-library-compile', [TOOLS / 'clang', '--target=' + TRIPLE, '-std=c11', '-O2', '-fPIC',
                                      '-c', fixture, '-o', library / 'fixture.o'])
    command('external-library-archive', [TOOLS / 'llvm-ar', 'rcs', library / 'libfixture.a', library / 'fixture.o'])
    diagnostic_tool = WORK / 'diagnostic-tool'
    command('diagnostic-tool-compile', [TOOLS / 'clang', '--ld-path=' + str(TOOLS / 'ld.lld'),
                                      '-std=c11', '-O2', HERE / 'diagnostic_tool.c', '-o', diagnostic_tool])
    bad_ir = WORK / 'invalid.ll'
    bad_ir.write_text('define i32 @main() { ret i64 42 }\n')
    undefined = WORK / 'undefined.ll'
    undefined.write_text('declare i32 @missing_symbol()\ndefine i32 @main() { %r = call i32 @missing_symbol()\n ret i32 %r }\n')
    bad_adapter = WORK / 'invalid.c'
    bad_adapter.write_text('int main( { invalid C\n')
    plain_adapter = WORK / 'plain-adapter.c'
    plain_adapter.write_text('int build_adapter(void) { return 42; }\n')
    for level in [0, 2]:
        output = WORK / f'wz-O{level}'
        command(f'wz-build-O{level}', [STAGE0, 'build', '--project', project / 'lang.toml', '--backend', 'native',
                                      '--opt', level, '--out-dir', output, '--format', 'json'])
        root = bundle(output / 'program', f'bundle-O{level}')
        build_case(f'native-O{level}', root, level, trace=True)
        build_case(f'hosted-O{level}', root, level, hosted=True, trace=True)
        build_case(f'adapter-native-O{level}', root, level, ir=HERE / 'adapter.ll', adapter=plain_adapter, trace=True)
        build_case(f'adapter-external-O{level}', root, level, ir=HERE / 'adapter.ll', adapter=HERE / 'adapter.c',
                   hosted=True, library=library, trace=True)
        build_case(f'coroutine-O{level}', root, level, ir=HERE / 'coroutine.ll', adapter=HERE / 'coroutine.c',
                   hosted=True, async_ir=True, trace=True)
        previous = WORK / f'previous-O{level}'
        previous.mkdir()
        (previous / 'program.o').write_bytes(b'previous object\x00')
        (previous / 'program').write_bytes(b'previous executable\x00')
        build_case(f'bad-ir-O{level}', root, level, ir=bad_ir, output=previous,
                   expected=3, code='E_IR_INVALID', phase='codegen')
        assert (previous / 'program.o').read_bytes() == b'previous object\x00'
        assert (previous / 'program').read_bytes() == b'previous executable\x00'
        build_case(f'bad-adapter-O{level}', root, level, adapter=bad_adapter,
                   expected=1, code='E_LINK_FAILED', phase='link')
        build_case(f'bad-link-O{level}', root, level, ir=undefined,
                   expected=1, code='E_LINK_FAILED', phase='link')
        missing = bundle(output / 'program', f'missing-opt-O{level}', missing='opt')
        failure = build_case(f'missing-tool-O{level}', missing, level,
                             expected=2, code='E_TOOLCHAIN_MISSING', phase='codegen')
        assert 'os error 2,' in failure['diagnostics'][0]['message']
        invalid = WORK / f'invalid-target-O{level}'
        invalid.mkdir()
        (invalid / 'program.o').write_bytes(b'old object')
        (invalid / 'program').mkdir()
        failure = build_case(f'invalid-publish-O{level}', root, level, output=invalid,
                             expected=2, code='E_INPUT_IO', phase='codegen')
        assert 'os error 22,' in failure['diagnostics'][0]['message']
        assert (invalid / 'program.o').read_bytes() == b'old object' and (invalid / 'program').is_dir()
        out_file = WORK / f'out-file-O{level}'
        out_file.write_bytes(b'keep file')
        build_case(f'out-file-O{level}', root, level, output=out_file,
                   expected=2, code='E_INPUT_IO', phase='codegen')
        assert out_file.read_bytes() == b'keep file'
        build_case(f'bad-triple-O{level}', root, level, triple='aarch64-unknown-linux-gnu',
                   expected=1, code='E_TARGET_UNSUPPORTED', phase='codegen')
        build_case(f'bad-optimization-O{level}', root, 9,
                   expected=1, code='E_CAPABILITY_UNAVAILABLE', phase='codegen')
        diagnostic_bundle = bundle(output / 'program', f'diagnostics-O{level}')
        fake_opt = diagnostic_bundle / 'toolchain/opt'
        fake_opt.unlink()
        fake_opt.symlink_to(diagnostic_tool)
        build_case(f'version-prefix-O{level}', diagnostic_bundle, level, tool_mode='wrong-version',
                   expected=2, code='E_TOOLCHAIN_MISSING', phase='codegen')
        failure = build_case(f'invalid-output-bytes-O{level}', diagnostic_bundle, level, tool_mode='invalid-bytes',
                             expected=3, code='E_IR_INVALID', phase='codegen')
        assert failure['diagnostics'][0]['notes'] == ['raw tool output bytes: 41 00 FF 42']
        fake_opt.unlink()
        fake_opt.write_bytes(b'not executable')
        fake_opt.chmod(0o600)
        failure = build_case(f'exec-permission-O{level}', diagnostic_bundle, level,
                             expected=2, code='E_TOOLCHAIN_MISSING', phase='codegen')
        assert 'os error 13,' in failure['diagnostics'][0]['message']
        fake_opt.unlink()
        fake_opt.symlink_to(TOOLS / 'opt')
        fake_clang = diagnostic_bundle / 'toolchain/clang'
        fake_clang.unlink()
        fake_clang.symlink_to(diagnostic_tool)
        for mode, adapter in [('signal', plain_adapter), ('exit126', None), ('exit127', plain_adapter)]:
            failure = build_case(f'{mode}-O{level}', diagnostic_bundle, level, adapter=adapter, tool_mode=mode,
                                 expected=2, code='E_TOOLCHAIN_MISSING', phase='link')
            expected_message = 'C adapter compilation failed' if adapter else 'native linker exited'
            assert failure['diagnostics'][0]['message'].startswith(expected_message)
    (WORK / 'inputs.json').write_text(json.dumps(inputs, indent=2))
    result = {'status': 'passed', 'work_dir': str(WORK), 'input_manifest_sha256': sha(WORK / 'inputs.json'),
              'scope': 'real Wz nativebuild tool driver; handmade LLVM inputs',
              'skipped_existing_packages': skipped_existing,
              'not_run': ['publish recovery itself fails (toolio Rollback branch)', 'full language emitter and new CLI integration'],
              'checks': checks, 'records': records}
except Exception as error:
    result.update(error=str(error), checks=checks, records=records)
    raise
finally:
    (WORK / 'results.json').write_text(json.dumps(result, indent=2))
print(json.dumps({key: value for key, value in result.items() if key not in ['records', 'checks']}, indent=2))
