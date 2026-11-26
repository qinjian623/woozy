#!/usr/bin/env python3
"""Exercise the real E system emitter for operation 213 (stat_bytes).

A fresh project is assembled from the production selfhost tree, the E files
from --e-source, and this directory's e_stat_bytes.wz probe. stage0 builds the
probe at O0 and O2; the printed LLVM is verified with opt, compiled with llc,
linked with the real native runtime, and executed by driver.c against a real
non-UTF-8 file and the empty/NUL/overlong/missing/ENOTDIR error cases.

The generated IR is never edited. This checks only the E component; the full
Wz public check/build facade is explicitly not claimed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SELFHOST = HERE.parents[1]
if str(SELFHOST) not in sys.path:
    sys.path.insert(0, str(SELFHOST))
from corpus import _run_process_impl  # noqa: E402
STAT_MARK = re.compile(rb'===STAT_BYTES===\n(.*?)===END_STAT_BYTES===\n', re.S)
FS_MARK = re.compile(rb'===FS_ABI===\n(.*?)===END_FS_ABI===\n', re.S)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--woozyc', type=Path,
                    default=ROOT / 'build/woozy-stdlib-native/woozyc')
parser.add_argument('--root-joint', type=Path,
                    default=ROOT / 'selfhost/src/wzcc')
parser.add_argument('--e-source', type=Path,
                    help='E source override (default root-joint/nativeemit)')
parser.add_argument('--toolchain-root', type=Path,
                    default=Path(os.environ.get('WOOZY_TOOLCHAIN_ROOT',
                                                str(ROOT.parent / 'toolchains'))))
parser.add_argument('--runtime-archive', type=Path)
parser.add_argument('--work-dir', type=Path)
parser.add_argument('--timeout', type=float, default=600)
parser.add_argument('--program-timeout', type=float, default=30)
args = parser.parse_args()
if args.timeout <= 0:
    parser.error('--timeout must be positive')
if args.program_timeout <= 0:
    parser.error('--program-timeout must be positive')
if args.work_dir:
    args.work_dir.expanduser().resolve().mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(
    prefix='woozy-stat-bytes-e-',
    dir=args.work_dir.expanduser().resolve() if args.work_dir else None)).resolve()
TOOLS = args.toolchain_root.expanduser().resolve() / 'llvm-22.1.8/bin'
CLANG = TOOLS / 'clang'
LLC = TOOLS / 'llc'
OPT = TOOLS / 'opt'
LD_LLD = TOOLS / 'ld.lld'
STAGE0 = args.woozyc.expanduser().resolve()
ROOT_JOINT = args.root_joint.expanduser().resolve()
E_SOURCE = (args.e_source.expanduser().resolve()
            if args.e_source else ROOT_JOINT / 'nativeemit')
RUNTIME = (args.runtime_archive.expanduser().resolve() if args.runtime_archive
           else STAGE0.parent / 'native/liblang_native_runtime.a')
ENV = dict(os.environ)
ENV['LD_LIBRARY_PATH'] = ':'.join([
    str(args.toolchain_root.expanduser().resolve() / 'llvm-22.1.8/lib'),
    str(args.toolchain_root.expanduser().resolve() /
        'libxml2-legacy-2.13.9/usr/lib'),
    ENV.get('LD_LIBRARY_PATH', '')])
ENV.setdefault('LC_ALL', 'C')
records: list[dict] = []


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(label: str, argv: list[Path | str], *, cwd: Path | None = None,
        timeout: float | None = None) -> subprocess.CompletedProcess[bytes]:
    shown = [str(item) for item in argv]
    budget = args.timeout if timeout is None else timeout
    process = _run_process_impl(shown, str(cwd) if cwd else str(Path.cwd()),
                                ENV, budget)
    stdout = process['stdout']
    stderr = process['stderr']
    (WORK / f'{label}.stdout').write_bytes(stdout)
    (WORK / f'{label}.stderr').write_bytes(stderr)
    record = {'label': label, 'argv': shown, 'exit': process['exit'],
              'timed_out': process['timed_out'],
              'cleanup_complete': process['cleanup_complete'],
              'abandoned_pipes': process['abandoned_pipes'],
              'timeout_seconds': budget,
              'stdout_sha256': hashlib.sha256(stdout).hexdigest(),
              'stderr_sha256': hashlib.sha256(stderr).hexdigest(),
              'stderr_tail': stderr.decode(errors='replace')[-800:]}
    records.append(record)
    failures = []
    if process['timed_out']:
        failures.append(f'timed out after {budget:g}s')
    if process['exit'] != 0:
        failures.append(f'exit {process["exit"]}')
    if not process['cleanup_complete'] or process['abandoned_pipes']:
        failures.append('process cleanup incomplete (abandoned pipes)')
    if failures:
        raise RuntimeError(f'{label}: ' + '; '.join(failures) + ': '
                           f'{stderr.decode(errors="replace")[-1000:]}')
    return subprocess.CompletedProcess(shown, process['exit'], stdout, stderr)


def main() -> int:
    if not STAGE0.is_file():
        raise RuntimeError(f'missing stage0: {STAGE0}')
    if not ROOT_JOINT.is_dir():
        raise RuntimeError(f'missing selfhost source: {ROOT_JOINT}')
    if not E_SOURCE.is_dir():
        raise RuntimeError(f'missing E source: {E_SOURCE}')
    if not RUNTIME.is_file():
        raise RuntimeError(f'missing runtime archive: {RUNTIME}')
    for tool in (CLANG, LLC, OPT, LD_LLD):
        if not tool.is_file():
            raise RuntimeError(f'missing toolchain tool: {tool}')

    project = WORK / 'project'
    (project / 'src/wzcc').mkdir(parents=True)
    inputs = []
    for source in sorted(ROOT_JOINT.rglob('*.wz')):
        target = project / 'src/wzcc' / source.relative_to(ROOT_JOINT)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())
        inputs.append({'kind': 'root-joint', 'path': str(source),
                       'sha256': sha(source)})
    root_emitter = ROOT_JOINT / 'nativeemit'
    c_layout = Path('core/c_abi_types.wz')
    expected_e = {path.relative_to(root_emitter) for path in root_emitter.rglob('e_*.wz')}
    selected_e = {path.relative_to(E_SOURCE) for path in E_SOURCE.rglob('e_*.wz')}
    if (root_emitter / c_layout).is_file():
        expected_e.add(c_layout)
    if (E_SOURCE / c_layout).is_file():
        selected_e.add(c_layout)
    if not selected_e or not expected_e <= selected_e:
        raise RuntimeError(f'E source paths differ between {E_SOURCE} and {root_emitter}')
    for relative in sorted(selected_e):
        source = E_SOURCE / relative
        target = project / 'src/wzcc/nativeemit' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())
        inputs.append({'kind': 'e-source', 'path': str(source),
                       'sha256': sha(source)})
    shutil.copyfile(HERE / 'e_stat_bytes.wz',
                    project / 'src/wzcc/nativeemit/e_stat_bytes.wz')
    shutil.copyfile(HERE / 'e_fs_abi.wz',
                    project / 'src/wzcc/nativeemit/e_fs_abi.wz')
    shutil.copyfile(HERE / 'src/main.wz', project / 'src/main.wz')
    shutil.copyfile(HERE / 'lang.toml', project / 'lang.toml')
    project_files = {str(path.relative_to(project)): sha(path)
                     for path in sorted(project.rglob('*')) if path.is_file()}
    source_set_sha256 = hashlib.sha256(''.join(
        f'{name}:{digest}\n' for name, digest in project_files.items()
    ).encode()).hexdigest()

    emitted: dict[int, dict[str, bytes]] = {}
    for optimization in (0, 2):
        out_dir = WORK / f'out-o{optimization}'
        run(f'build-o{optimization}', [STAGE0, 'build', '--project',
            project / 'lang.toml', '--backend', 'native', '--opt',
            str(optimization), '--out-dir', out_dir, '--format', 'json'])
        output = run(f'run-o{optimization}', [out_dir / 'program'],
                     timeout=args.program_timeout).stdout
        stat_match = STAT_MARK.search(output)
        fs_match = FS_MARK.search(output)
        if not stat_match or not fs_match:
            raise RuntimeError(f'O{optimization}: markers not found in program output')
        emitted[optimization] = {'stat': stat_match.group(1),
                                 'fs': fs_match.group(1)}
        (WORK / f'emitted-o{optimization}.txt').write_bytes(output)
    if emitted[0] != emitted[2]:
        raise RuntimeError('O0 and O2 emitted LLVM text differ')

    stat_llvm = WORK / 'stat_bytes.ll'
    stat_llvm.write_bytes(emitted[0]['stat'])
    stat_text = stat_llvm.read_text()
    call = re.search(
        r'call i64 @"lang_sys_stat_v1"\(ptr %[^,]+, i64 %[^,]+, ptr %[^)]+\)',
        stat_text)
    if not call:
        raise RuntimeError('generated module has no three-argument '
                           'lang_sys_stat_v1 call')
    if 'getelementptr inbounds %"lang.vec", ptr %p0, i32 0, i32 5' not in stat_text \
            or 'getelementptr inbounds %"lang.vec", ptr %p0, i32 0, i32 1' not in stat_text:
        raise RuntimeError('Vec data/length fields were not read from the descriptor')

    fs_llvm = WORK / 'fs_abi.ll'
    fs_llvm.write_bytes(emitted[0]['fs'])
    fs_text = fs_llvm.read_text()
    signatures = {
        'getcwd': r'call i64 @"lang_sys_getcwd_vec_v1"\(ptr %[^,]+, i64 %[^)]+\)',
        'getdents': r'call i64 @"lang_sys_getdents_vec_v1"\(i32 %[^,]+, ptr %[^,]+, i64 %[^)]+\)',
        'readlink': r'call i64 @"lang_sys_readlink_vec_v1"\(ptr %[^,]+, i64 %[^,]+, ptr %[^,]+, i64 %[^)]+\)',
        'nanosleep': r'call i64 @"lang_sys_nanosleep_vec_v1"\(ptr %[^,]+, ptr %[^)]+\)',
        'getrandom': r'call i64 @"lang_sys_getrandom_vec_v1"\(ptr %[^,]+, i64 %[^,]+, i32 %[^)]+\)',
    }
    calls = {}
    for name, pattern in signatures.items():
        match = re.search(pattern, fs_text)
        if not match:
            raise RuntimeError(f'{name}: descriptor ABI call signature missing')
        calls[name] = match.group(0)
    if len(re.findall(r'call i64 @"lang_sys_getcwd_vec_v1"\(', fs_text)) != 1:
        raise RuntimeError('getcwd call count changed')
    if len(re.findall(r'call i64 @"lang_sys_nanosleep_vec_v1"\(', fs_text)) != 1:
        raise RuntimeError('nanosleep call count changed')

    opt_stat = run('opt-stat', [OPT, '-passes=verify', '-disable-output', stat_llvm])
    opt_fs = run('opt-fs', [OPT, '-passes=verify', '-disable-output', fs_llvm])
    link = WORK / 'link'
    link.mkdir()
    for prefix, llvm, driver in (
            ('stat', stat_llvm, HERE / 'driver.c'),
            ('fs', fs_llvm, HERE / 'fs_abi_driver.c')):
        run(f'llc-{prefix}', [LLC, '-filetype=obj', '-O0',
            '-mtriple=x86_64-unknown-linux-gnu', '-mcpu=generic',
            '-relocation-model=pic', '-code-model=small', llvm,
            '-o', link / f'{prefix}.o'])
        run(f'clang-driver-{prefix}', [CLANG, '--target=x86_64-unknown-linux-gnu',
            '-std=c11', '-O2', '-ffreestanding', '-fno-builtin',
            '-fno-stack-protector', '-fPIC', '-c', driver,
            '-o', link / f'{prefix}-driver.o'])
        run(f'link-{prefix}', [CLANG, '--target=x86_64-unknown-linux-gnu',
            '-nostdlib', '-static', f'--ld-path={LD_LLD}',
            link / f'{prefix}.o', link / f'{prefix}-driver.o', RUNTIME,
            '-o', link / f'{prefix}_probe'])

    fsdir = WORK / 'fs'
    fsdir.mkdir()
    fd = os.open(os.fsencode(fsdir) + b'/\xff', os.O_CREAT | os.O_WRONLY, 0o640)
    os.write(fd, b'x')
    os.close(fd)
    (fsdir / 'notdir').write_bytes(b'n')
    (fsdir / 'target').write_bytes(b't')
    os.symlink('target', fsdir / 'link')
    stat_probe = run('run-stat-probe', [link / 'stat_probe'], cwd=fsdir,
                     timeout=args.program_timeout)
    fs_probe = run('run-fs-probe', [link / 'fs_probe'], cwd=fsdir,
                   timeout=args.program_timeout)

    result = {
        'status': 'passed',
        'work_dir': str(WORK),
        'woozyc': {'path': str(STAGE0), 'sha256': sha(STAGE0)},
        'runtime_archive': {'path': str(RUNTIME), 'sha256': sha(RUNTIME)},
        'emitted': {
            'stat_bytes': {'path': str(stat_llvm), 'sha256': sha(stat_llvm),
                           'bytes': stat_llvm.stat().st_size,
                           'o0_o2_equal': True},
            'fs_abi': {'path': str(fs_llvm), 'sha256': sha(fs_llvm),
                       'bytes': fs_llvm.stat().st_size,
                       'o0_o2_equal': True}},
        'call': call.group(0),
        'old_calls': calls,
        'opt_exit': {'stat': opt_stat.returncode, 'fs': opt_fs.returncode},
        'probe_exit': {'stat': stat_probe.returncode, 'fs': fs_probe.returncode},
        'inputs': inputs,
        'source_set': {'sha256': source_set_sha256, 'files': project_files},
        'records': records,
        'not_run': [
            'full Wz public check/build facade (selfhost CLI reports '
            'E_CAPABILITY_UNAVAILABLE for project build)',
            'all 214 system operations through E',
        ],
    }
    (WORK / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items()
                      if k not in ('records', 'inputs')}, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - keep exact failure evidence
        result = {'status': 'failed', 'work_dir': str(WORK), 'error': repr(error),
                  'failed_record': records[-1] if records else None,
                  'records': records}
        (WORK / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
        raise SystemExit(1)
