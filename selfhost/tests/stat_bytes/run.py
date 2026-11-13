#!/usr/bin/env python3
"""Differential test for std/os.stat_bytes.

Builds a small Wz program that calls os.stat_bytes with empty, NUL-containing,
overlong, real non-UTF-8, Unicode, missing and ENOTDIR paths. The C++ oracle
calls the same lang_sys_stat_v1 runtime ABI; both outputs must match exactly at
O0 and O2. No source or generated IR is rewritten to make a case pass.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parents[1]))
from corpus import _run_process_impl  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--woozyc', type=Path,
                    default=ROOT / 'build/woozy-stdlib-native/woozyc')
parser.add_argument('--runtime-dir', type=Path,
                    help='directory containing liblang_native_runtime.a '
                         '(default: stage0 parent/native)')
parser.add_argument('--cxx', default=os.environ.get('CXX', 'clang++'))
parser.add_argument('--work-dir', type=Path)
parser.add_argument('--timeout', type=float, default=240)
args = parser.parse_args()
if not math.isfinite(args.timeout) or args.timeout <= 0:
    parser.error('--timeout must be finite and positive')
if args.work_dir:
    args.work_dir.expanduser().resolve().mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(
    prefix='woozy-stat-bytes-',
    dir=args.work_dir.expanduser().resolve() if args.work_dir else None)).resolve()
RUNTIME_DIR = (args.runtime_dir.expanduser().resolve() if args.runtime_dir
               else args.woozyc.resolve().parent / 'native')
RUNTIME = RUNTIME_DIR / 'liblang_native_runtime.a'
ENV = dict(os.environ)
ENV.setdefault('LC_ALL', 'C')
records: list[dict] = []


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(label: str, argv: list[bytes | str], *, timeout: float | None = None,
        cwd: Path | None = None) -> subprocess.CompletedProcess[bytes]:
    shown = [x.decode(errors='backslashreplace') if isinstance(x, bytes) else str(x)
             for x in argv]
    budget = args.timeout if timeout is None else timeout
    # fsdecode preserves arbitrary Linux filename bytes through surrogateescape.
    process = _run_process_impl([os.fsdecode(x) for x in argv],
                                cwd if cwd is not None else Path.cwd(), ENV, budget)
    stdout, stderr = process['stdout'], process['stderr']
    (WORK / f'{label}.stdout').write_bytes(stdout)
    (WORK / f'{label}.stderr').write_bytes(stderr)
    records.append({'label': label, 'argv': shown, 'exit': process['exit'],
                    'timed_out': process['timed_out'],
                    'cleanup_complete': process['cleanup_complete'],
                    'abandoned_pipes': process['abandoned_pipes'],
                    'timeout_seconds': budget,
                    'stdout_sha256': hashlib.sha256(stdout).hexdigest(),
                    'stdout_bytes': len(stdout), 'stderr_bytes': len(stderr)})
    if (process['exit'] != 0 or process['timed_out']
            or not process['cleanup_complete'] or process['abandoned_pipes']):
        raise RuntimeError(f'{label}: process failed: {records[-1]}: '
                           f'{stderr.decode(errors="replace")[-1000:]}')
    return subprocess.CompletedProcess(argv, process['exit'], stdout, stderr)


def parse(stdout: bytes) -> dict[str, tuple[str, int, int]]:
    out: dict[str, tuple[str, int, int]] = {}
    for line in stdout.decode('ascii').splitlines():
        name, rest = line.split(': ', 1)
        if rest.startswith('err '):
            out[name] = ('err', int(rest[4:]), 0)
        else:
            fields = dict(item.split('=', 1) for item in rest[2:].split())
            out[name] = ('ok', int(fields['mode']), int(fields['size']))
    return out


def main() -> int:
    if not args.woozyc.is_file():
        raise RuntimeError(f'missing stage0: {args.woozyc}')
    if not RUNTIME.is_file():
        raise RuntimeError(f'missing runtime archive: {RUNTIME}')

    project = WORK / 'project'
    shutil.copytree(HERE / 'src', project / 'src')
    shutil.copyfile(HERE / 'lang.toml', project / 'lang.toml')

    oracle = WORK / 'oracle'
    run('oracle-build', [args.cxx, '-std=c++20', '-O2',
        '-I', str(ROOT / 'runtime/native'), str(HERE / 'oracle.cpp'),
        str(RUNTIME), '-o', str(oracle)])

    fsdir = WORK / 'fs'
    fsdir.mkdir()
    (fsdir / 'unicode-\u6c49.wz').write_bytes(b'y')
    (fsdir / 'notdir').write_bytes(b'n')
    fd = os.open(os.fsencode(fsdir) + b'/\xff', os.O_CREAT | os.O_WRONLY, 0o640)
    os.write(fd, b'x')
    os.close(fd)

    invalid = os.fsencode(fsdir) + b'/\xff'
    unicode_path = os.fsencode(fsdir) + b'/unicode-\xe6\xb1\x89.wz'
    missing = os.fsencode(fsdir) + b'/missing'
    notdir = os.fsencode(fsdir) + b'/notdir/child'

    oracle_run = run('oracle-run',
                     [os.fsencode(oracle), invalid, unicode_path, missing, notdir])
    expected = parse(oracle_run.stdout)

    builds: dict[str, dict[str, object]] = {}
    for optimization in (0, 2):
        out_dir = WORK / f'out-o{optimization}'
        run(f'build-o{optimization}', [str(args.woozyc), 'build', '--project',
            str(project / 'lang.toml'), '--backend', 'native', '--opt',
            str(optimization), '--out-dir', str(out_dir), '--format', 'json'])
        program = out_dir / 'program'
        actual_run = run(f'run-o{optimization}',
                         [os.fsencode(program), invalid, unicode_path, missing,
                          notdir])
        actual = parse(actual_run.stdout)
        if actual_run.stdout != oracle_run.stdout:
            raise RuntimeError(f'O{optimization} output differs from oracle: '
                               f'{actual_run.stdout!r} != {oracle_run.stdout!r}')
        if actual != expected:
            raise RuntimeError(f'O{optimization} parsed values differ')
        builds[f'O{optimization}'] = {
            'program': str(program), 'program_sha256': sha(program),
            'stdout_sha256': hashlib.sha256(actual_run.stdout).hexdigest()}
    if (WORK / 'run-o0.stdout').read_bytes() != (WORK / 'run-o2.stdout').read_bytes():
        raise RuntimeError('O0 and O2 outputs differ')

    if expected['empty'] != ('err', 2, 0):
        raise RuntimeError(f"empty path: {expected['empty']}")
    if expected['nul'] != ('err', 22, 0):
        raise RuntimeError(f"NUL path: {expected['nul']}")
    if expected['long'] != ('err', 36, 0):
        raise RuntimeError(f"overlong path: {expected['long']}")
    if expected['missing'] != ('err', 2, 0):
        raise RuntimeError(f"missing path: {expected['missing']}")
    if expected['notdir'] != ('err', 20, 0):
        raise RuntimeError(f"ENOTDIR path: {expected['notdir']}")
    for name in ('invalid', 'unicode'):
        kind, mode, size = expected[name]
        if kind != 'ok' or (mode & 0o170000) != 0o100000 or size != 1:
            raise RuntimeError(f'{name} path: {expected[name]}')

    result = {
        'status': 'passed',
        'work_dir': str(WORK),
        'woozyc': {'path': str(args.woozyc), 'sha256': sha(args.woozyc)},
        'runtime_archive': {'path': str(RUNTIME), 'sha256': sha(RUNTIME)},
        'oracle': {'path': str(oracle), 'sha256': sha(oracle)},
        'expected': {k: list(v) for k, v in expected.items()},
        'builds': builds,
        'records': records,
    }
    (WORK / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'records'}, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - exact failure evidence
        result = {'status': 'failed', 'work_dir': str(WORK), 'error': repr(error),
                  'records': records}
        (WORK / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
        raise SystemExit(1)
