#!/usr/bin/env python3
"""SH08e independent probe runner.

Builds a fresh project from the frozen Root joint candidate, overlays only the
E files from this worktree, adds the read-only review.wz probe and the extra
test-only probes, then:

  * checks, builds and runs at O0 and O2;
  * writes every generated module and runs LLVM 22.1.8 opt -passes=verify;
  * links the generated bundle with the real native runtime and a test-only
    scalar ABI wrapper, then runs the Result/getenv/field/IPC checks.

No generated IR is edited to make a check pass.  Every subprocess is bounded
and killed as a process group on timeout so no helper is left behind.
"""
from __future__ import annotations

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
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORKTREE = HERE.parents[2]
DEFAULT_E_SOURCE = WORKTREE / "selfhost/src/wzcc/nativeemit"

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--woozyc', default=str(WORKTREE / 'build/woozy-stdlib-native/woozyc'))
parser.add_argument('--toolchain-root', default=str(WORKTREE.parent / 'toolchains'))
parser.add_argument('--root-joint', default=str(WORKTREE / 'selfhost/src/wzcc'))
parser.add_argument('--review-source', default=str(HERE / 'review.wz'))
parser.add_argument('--runtime-archive')
parser.add_argument('--e-source', default=str(DEFAULT_E_SOURCE))
parser.add_argument('--work-dir', default=None)
parser.add_argument('--results', default=None)
parser.add_argument('--timeout', type=float, default=600)
parser.add_argument('--skip-o2', action='store_true')
args = parser.parse_args()

if args.timeout <= 0:
    parser.error('--timeout must be positive')
if args.work_dir:
    Path(args.work_dir).expanduser().resolve().mkdir(parents=True, exist_ok=True)
    WORK = Path(args.work_dir).expanduser().resolve()
else:
    WORK = Path(tempfile.mkdtemp(prefix='woozy-sh08e-e-',
                                 dir=os.environ.get('TMPDIR') or None))
PROJECT = WORK / 'project'
SOURCE = Path(args.e_source).expanduser().resolve()
TOOLCHAIN = Path(args.toolchain_root).expanduser().resolve()
TOOLS = TOOLCHAIN / 'llvm-22.1.8/bin'
CLANG = TOOLS / 'clang'
LLC = TOOLS / 'llc'
OPT = TOOLS / 'opt'
LD_LLD = TOOLS / 'ld.lld'
STAGE0 = Path(args.woozyc).expanduser().resolve()
ROOT_JOINT = Path(args.root_joint).expanduser().resolve()
REVIEW = Path(args.review_source).expanduser().resolve()
RUNTIME = (Path(args.runtime_archive).expanduser().resolve() if args.runtime_archive
           else STAGE0.parent / 'native/liblang_native_runtime.a')
ENV = dict(os.environ)
compat = ':'.join(str(TOOLCHAIN / item) for item in
                  ('llvm-22.1.8/lib', 'libxml2-legacy-2.13.9/usr/lib'))
ENV['LD_LIBRARY_PATH'] = compat + (':' + ENV['LD_LIBRARY_PATH'] if ENV.get('LD_LIBRARY_PATH') else '')
ENV['TMPDIR'] = str(WORK)
ENV.setdefault('LC_ALL', 'C')

records: list[dict] = []
inputs: list[dict] = []
result: dict = {'status': 'failed', 'work_dir': str(WORK)}


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def record_file(kind: str, path: Path) -> None:
    if path.is_file():
        inputs.append({'kind': kind, 'path': str(path), 'sha256': sha(path),
                       'bytes': path.stat().st_size})


def run(label: str, argv: list[str | Path], expected: int = 0,
        timeout: float | None = None) -> subprocess.CompletedProcess:
    shown = [str(item) for item in argv]
    limit = args.timeout if timeout is None else timeout
    started = time.time()
    proc = subprocess.Popen(shown, cwd=str(WORKTREE), env=ENV,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            start_new_session=True)
    timed_out = False
    try:
        stdout, stderr = proc.communicate(timeout=limit)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = proc.communicate()
    elapsed = round(time.time() - started, 3)
    (WORK / (label + '.stdout')).write_bytes(stdout)
    (WORK / (label + '.stderr')).write_bytes(stderr)
    record = {'label': label, 'argv': shown, 'exit': proc.returncode,
              'expected_exit': expected, 'timed_out': timed_out,
              'timeout_seconds': limit, 'elapsed_seconds': elapsed,
              'stdout_bytes': len(stdout), 'stderr_bytes': len(stderr),
              'stdout_sha256': hashlib.sha256(stdout).hexdigest(),
              'stderr_tail': stderr.decode(errors='replace')[-800:]}
    records.append(record)
    if timed_out:
        raise RuntimeError(f'{label}: timed out after {limit}s; process group killed')
    if proc.returncode != expected:
        raise RuntimeError(f'{label}: exit {proc.returncode}, expected {expected}: '
                           f'{stderr.decode(errors="replace")[-1200:]}')
    return subprocess.CompletedProcess(shown, proc.returncode, stdout, stderr)


CASE_RE = re.compile(
    r'===CASE ([^\n]+)===\n===EXPECTED ([^\n]*)\n===ACTUAL ([^\n]*)\n'
    r'(.*?)===ENDCASE===\n', re.S)


def parse_cases(text: str) -> dict[str, dict]:
    cases = {}
    for match in CASE_RE.finditer(text):
        cases[match.group(1)] = {
            'expected': match.group(2),
            'actual': match.group(3),
            'llvm': match.group(4),
        }
    return cases


def main() -> None:
    global result
    if not STAGE0.is_file():
        raise RuntimeError(f'missing stage0: {STAGE0}')
    if not ROOT_JOINT.is_dir():
        raise RuntimeError(f'missing Root joint source: {ROOT_JOINT}')
    if not REVIEW.is_file():
        raise RuntimeError(f'missing read-only review.wz: {REVIEW}')
    if not RUNTIME.is_file():
        raise RuntimeError(f'missing runtime archive: {RUNTIME}')

    (PROJECT / 'src/wzcc').mkdir(parents=True, exist_ok=True)
    for source in sorted(ROOT_JOINT.rglob('*.wz')):
        relative = source.relative_to(ROOT_JOINT)
        target = PROJECT / 'src/wzcc' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())
        record_file('root-joint', source)

    # Replace every E file with this worktree's production fix.  The frozen
    # Root B/C/D/layout/valueops sources stay untouched.
    for source in sorted(SOURCE.glob('e_*.wz')):
        target = PROJECT / 'src/wzcc/nativeemit' / source.name
        target.write_bytes(source.read_bytes())
        record_file('e-source', source)
    review_target = PROJECT / 'src/wzcc/nativeemit/review.wz'
    review_target.write_bytes(REVIEW.read_bytes())
    record_file('review', REVIEW)
    for name in ('e_review_extra.wz', 'probe_main.wz'):
        target = PROJECT / 'src/wzcc/nativeemit' / name
        if name == 'probe_main.wz':
            target = PROJECT / 'src/main.wz'
        target.write_bytes((HERE / name).read_bytes())
        record_file('test-source', HERE / name)
    (PROJECT / 'lang.toml').write_text(
        'schema_version = 1\n[package]\nname = "woozyc_selfhost"\n'
        'source_root = "src"\nentry = "app"\n[build]\nbackend = "native"\n'
        'word_bits = 64\n')

    check = run('wz-check', [STAGE0, 'check', '--project', PROJECT / 'lang.toml',
                             '--backend', 'native', '--format', 'json'])
    check_json = json.loads(check.stdout.decode().strip().splitlines()[-1])
    if check_json.get('status') != 'ok':
        raise RuntimeError('check did not report ok')

    builds = {}
    for optimization in (0, 2):
        if optimization == 2 and args.skip_o2:
            continue
        out_dir = WORK / f'out-o{optimization}'
        run(f'wz-build-O{optimization}', [STAGE0, 'build', '--project',
            PROJECT / 'lang.toml', '--backend', 'native', '--opt',
            str(optimization), '--out-dir', out_dir, '--format', 'json'],
            timeout=args.timeout)
        program = out_dir / 'program'
        emitted = run(f'wz-run-O{optimization}', [program], timeout=120).stdout
        (WORK / f'emitted-O{optimization}.txt').write_bytes(emitted)
        builds[optimization] = {'out_dir': str(out_dir), 'program': str(program),
                                'emitted_sha256': hashlib.sha256(emitted).hexdigest(),
                                'emitted_bytes': len(emitted)}

    cases = parse_cases((WORK / 'emitted-O0.txt').read_text())
    required = ['result', 'getenv', 'field0', 'field1', 'wait', 'runtime_bundle']
    missing = [name for name in required if name not in cases]
    if missing:
        raise RuntimeError('missing probe cases: ' + ', '.join(missing))
    for name in required[:5]:
        case = cases[name]
        if case['expected'] != case['actual']:
            raise RuntimeError(f'{name}: expected {case["expected"]} but got {case["actual"]}')

    if args.skip_o2:
        pass
    else:
        if (WORK / 'emitted-O0.txt').read_bytes() != (WORK / 'emitted-O2.txt').read_bytes():
            raise RuntimeError('O0 and O2 emitted text differ')

    cases_dir = WORK / 'cases'
    cases_dir.mkdir(exist_ok=True)
    opt_records = []
    for name, case in cases.items():
        path = cases_dir / f'{name}.ll'
        path.write_text(case['llvm'])
        opt = run(f'opt-{name}', [OPT, '-passes=verify', '-disable-output', path],
                  timeout=120)
        opt_records.append({'case': name, 'path': str(path), 'exit': opt.returncode,
                            'sha256': sha(path), 'bytes': path.stat().st_size})

    wrapper = HERE / 'runtime_wrapper.ll'
    driver = HERE / 'runtime_driver.c'
    opt_wrapper = run('opt-runtime-wrapper', [OPT, '-passes=verify',
        '-disable-output', wrapper], timeout=120)
    link_dir = WORK / 'link'
    link_dir.mkdir(exist_ok=True)
    run('llc-bundle', [LLC, '-filetype=obj', '-O0',
        '-mtriple=x86_64-unknown-linux-gnu', '-mcpu=generic',
        '-relocation-model=pic', '-code-model=small',
        cases_dir / 'runtime_bundle.ll', '-o', link_dir / 'bundle.o'], timeout=300)
    run('llc-wrapper', [LLC, '-filetype=obj', '-O0',
        '-mtriple=x86_64-unknown-linux-gnu', '-mcpu=generic',
        '-relocation-model=pic', '-code-model=small', wrapper,
        '-o', link_dir / 'wrapper.o'], timeout=300)
    run('clang-driver', [CLANG, '--target=x86_64-unknown-linux-gnu',
        '-std=c11', '-O2', '-ffreestanding', '-fno-builtin',
        '-fno-stack-protector', '-fPIC', '-c', driver,
        '-o', link_dir / 'driver.o'], timeout=300)
    runtime_record = {'archive': str(RUNTIME), 'exists': RUNTIME.is_file()}
    if RUNTIME.is_file():
        runtime_record['sha256'] = sha(RUNTIME)
        run('link-runtime-probe', [CLANG, '--target=x86_64-unknown-linux-gnu',
            '-nostdlib', '-static', f'--ld-path={LD_LLD}',
            link_dir / 'bundle.o', link_dir / 'wrapper.o', link_dir / 'driver.o',
            RUNTIME, '-o', link_dir / 'runtime_probe'], timeout=300)
        runtime_run = run('run-runtime-probe', [link_dir / 'runtime_probe'],
                          timeout=120)
        runtime_record['program'] = str(link_dir / 'runtime_probe')
        runtime_record['exit'] = runtime_run.returncode
    else:
        raise RuntimeError(f'runtime archive disappeared: {RUNTIME}')

    result = {
        'status': 'passed',
        'work_dir': str(WORK),
        'builds': builds,
        'cases': {name: {'expected': case['expected'], 'actual': case['actual'],
                         'llvm_sha256': hashlib.sha256(case['llvm'].encode()).hexdigest()}
                  for name, case in cases.items()},
        'opt': opt_records,
        'opt_wrapper_exit': opt_wrapper.returncode,
        'runtime': runtime_record,
        'adapter_note': '3835-byte adapter equality is checked by run.py; '
                        'see adapter_results.json. This harness does not regenerate it.',
        'not_run': [
            'public nativeemit.emit/VerifiedModule feed for the whole corpus',
            'all 213 system operations executed at runtime',
            'retained/static callback thread/GC/fork lifecycle',
        ],
    }


if __name__ == '__main__':
    try:
        main()
    except Exception as error:  # noqa: BLE001 - report exact failure evidence
        result = {'status': 'failed', 'work_dir': str(WORK), 'error': repr(error),
                  'records': records[-12:]}
    finally:
        result['records'] = records
        result['inputs'] = inputs
        payload = json.dumps(result, indent=2)
        (WORK / 'results.json').write_text(payload)
        if args.results:
            destination = Path(args.results).expanduser().resolve()
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(payload)
        print(json.dumps({key: value for key, value in result.items()
                          if key not in ('records', 'inputs')}, indent=2))
        if result.get('status') != 'passed':
            raise SystemExit(1)
