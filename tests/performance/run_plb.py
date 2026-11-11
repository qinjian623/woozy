#!/usr/bin/env python3
"""Local Woozy/C++ comparisons for the five problems on the PLB C++ page.

The website's times are not local measurements. Portable pairs use O2, one
CPU and matching algorithms; unmodified upstream C++ uses the website's O3
flags and four CPUs and is reported separately. Every invocation keeps and
checks stdout. Batch R repeats whole processes, not an altered inner kernel.
"""
import argparse
import csv
import json
import math
import os
from pathlib import Path
import re
import resource
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

import run_native as common
import run_sort

HERE = Path(__file__).resolve().parent / 'plb'
CASES = {'helloworld': ('QwQ',), 'fannkuch': ('10', '11'),
         'nbody': ('500000', '5000000'), 'nsieve': ('10', '12'),
         'spectral-norm': ('2000', '4000', '8000')}
SMALL = {'helloworld': ('QwQ', 'T_T'), 'fannkuch': ('7', '8'),
         'nbody': ('1000', '10000'), 'nsieve': ('4', '5'),
         'spectral-norm': ('100',)}
ORIGINALS = {'helloworld': ('helloworld/1.cpp',),
             'fannkuch': ('fannkuch-redux/1-im.cpp',),
             'nbody': ('nbody/1.cpp', 'nbody/0-i.cpp'),
             'nsieve': ('nsieve/1.cpp',),
             'spectral-norm': ('spectral-norm/8.cpp', 'spectral-norm/6-im.cpp')}
FANNKUCH = {7: (228, 16), 8: (1616, 22), 10: (73196, 38), 11: (556355, 51)}


def parsed(name, value, text):
    if name == 'helloworld':
        if text != f'Hello world {value}!':
            raise common.PerfError(f'wrong greeting: {text!r}')
        return text
    if name == 'fannkuch':
        match = re.fullmatch(r'(-?\d+)\nPfannkuchen\((\d+)\) = (\d+)\n?', text)
        if not match or int(match[2]) != int(value):
            raise common.PerfError(f'wrong fannkuch output: {text!r}')
        return [int(match[1]), int(match[3])]
    if name == 'nsieve':
        rows = [re.fullmatch(r'Primes up to\s+(\d+)\s+(\d+)', line)
                for line in text.splitlines()]
        if len(rows) != 3 or not all(rows):
            raise common.PerfError(f'wrong nsieve output: {text!r}')
        result = [[int(row[1]), int(row[2])] for row in rows]
        if [row[0] for row in result] != [10000 << (int(value) - i) for i in range(3)]:
            raise common.PerfError('nsieve did not compute the three required bounds')
        return result
    rows = text.split()
    if len(rows) != (2 if name == 'nbody' else 1):
        raise common.PerfError(f'wrong floating output: {text!r}')
    result = list(map(float, rows))
    if not all(math.isfinite(item) for item in result):
        raise common.PerfError('non-finite benchmark result')
    return result


def equivalent(name, actual, expected):
    if name in ('nbody', 'spectral-norm'):
        good = len(actual) == len(expected) and all(
            abs(a - b) <= 1e-9 for a, b in zip(actual, expected))
    else:
        good = actual == expected
    if not good:
        raise common.PerfError(f'{name}: {actual!r} != {expected!r}')


def oracle(name, value):
    if name == 'helloworld':
        return f'Hello world {value}!'
    if name == 'fannkuch':
        return list(FANNKUCH[int(value)])
    if name == 'nsieve':
        # Independent Eratosthenes oracle starts at p*p, unlike benchmark 2*p.
        limit = 10000 << int(value)
        flags = bytearray(b'\1') * limit
        flags[:2] = b'\0\0'
        for prime in range(2, math.isqrt(limit - 1) + 1):
            if flags[prime]:
                start = prime * prime
                flags[start:limit:prime] = b'\0' * ((limit - 1 - start) // prime + 1)
        return [[limit >> i, flags.count(1, 0, limit >> i)] for i in range(3)]
    # Upstream bench.yaml's small-input expected outputs, retained in vendor.
    path = HERE / 'vendor/bench/algorithm' / name / f'{value}_out'
    if path.exists():
        return parsed(name, value, path.read_text())
    if value in SMALL[name]:
        raise common.PerfError(f'missing pinned small-input oracle: {path}')
    return None


def selftest():
    assert parsed('fannkuch', '10', '73196\nPfannkuchen(10) = 38\n') == [73196, 38]
    assert parsed('nsieve', '4', 'Primes up to   160000 14683\nPrimes up to 80000 7837\nPrimes up to 40000 4203\n') == oracle('nsieve', '4')
    for name, value, text in [('helloworld', 'QwQ', 'Hello world!'),
                              ('spectral-norm', '100', 'nan'),
                              ('nbody', '1000', '-0.1'),
                              ('fannkuch', '11', '73196\nPfannkuchen(10) = 38\n'),
                              ('nsieve', '4', 'Primes up to 1 1\n')]:
        try:
            parsed(name, value, text)
        except (common.PerfError, ValueError):
            pass
        else:
            raise AssertionError((name, 'invalid output was accepted'))
    try:
        equivalent('spectral-norm', [1.27], [1.28])
    except common.PerfError:
        pass
    else:
        raise AssertionError('wrong finite result was accepted')
    cpus = sorted(os.sched_getaffinity(0))
    if len(cpus) >= 4:
        with tempfile.TemporaryDirectory(prefix='plb-preserve-') as temporary:
            directory = Path(temporary)
            sentinel = '{"status":"sampled","do_not_replace":true}\n'
            path = directory / 'results.json'
            path.write_text(sentinel)
            for phase in ('prepare', 'sample'):
                result = subprocess.run([sys.executable, __file__, phase, '--out', directory,
                                         '--cli', directory / 'unused-compiler',
                                         '--llvm-bin', directory, '--cpu', str(cpus[0])],
                                        capture_output=True, text=True, timeout=10)
                assert result.returncode != 0, (phase, result)
                assert path.read_text() == sentinel, (phase, 'previous results overwritten')
    print('PLB output checks passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('prepare', 'sample', 'selftest'))
    parser.add_argument('--out', type=Path)
    parser.add_argument('--cli', type=Path)
    parser.add_argument('--llvm-bin', type=Path)
    parser.add_argument('--cpu', type=int, default=1)
    args = parser.parse_args()
    if args.phase == 'selftest':
        selftest()
        return
    if not all((args.out, args.cli, args.llvm_bin)):
        parser.error('--out, --cli and --llvm-bin are required')
    args.cli, args.out = args.cli.resolve(), args.out.absolute()
    args.llvm_bin = args.llvm_bin.absolute()  # Preserve the ld.lld symlink name.
    cpus = sorted(os.sched_getaffinity(0))
    if args.cpu not in cpus or len(cpus) < 4:
        parser.error('one selected CPU and at least four available CPUs are required')
    upstream_cpus = [args.cpu] + [cpu for cpu in cpus if cpu != args.cpu][:3]
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1',
                       OMP_THREAD_LIMIT='4', OMP_NUM_THREADS='4', OMP_DYNAMIC='FALSE',
                       OMP_DISPLAY_ENV='FALSE', OMP_DISPLAY_AFFINITY='FALSE')
    for parent in args.cli.parents:
        library = parent / 'toolchains/libxml2-legacy-2.13.9/usr/lib'
        if library.is_dir():
            environment['LD_LIBRARY_PATH'] = str(library) + ':' + environment.get('LD_LIBRARY_PATH', '')
            break
    omp = args.llvm_bin.parent / 'lib/x86_64-unknown-linux-gnu'
    environment['LD_LIBRARY_PATH'] = str(omp) + ':' + environment.get('LD_LIBRARY_PATH', '')
    for which, limit in ((resource.RLIMIT_STACK, 33554432), (resource.RLIMIT_CORE, 0)):
        _, hard = resource.getrlimit(which)
        resource.setrlimit(which, (limit, hard))
    report_path = args.out / 'results.json'
    report = {'status': 'preparing', 'commands': [], 'cases': [], 'originals': []}
    can_write = False

    def save():
        common.write_json(report_path, report)

    def execute(argv, *, upstream=False):
        if upstream:
            argv = ['taskset', '-c', ','.join(map(str, upstream_cpus)), *map(str, argv)]
        record = common.command(argv, timeout=300, env=environment,
                                cpu=None if upstream else args.cpu)
        report['commands'].append(record)
        save()  # Keep failed commands and their real stdout before checking.
        common.require_success(record, str(argv[0]))
        return record

    def checked(program, name, value, expected=None, upstream=False):
        record = execute([program, value], upstream=upstream)
        # LLVM OpenMP reports the expected cap when 8.cpp requests all host CPUs.
        capped = upstream and Path(program).name == 'upstream-8' and re.fullmatch(
            r'OMP: Warning #96: Cannot form a team with \d+ threads, using 4 instead\.\n'
            r'OMP: Hint Consider unsetting KMP_DEVICE_THREAD_LIMIT \(KMP_ALL_THREADS\), '
            r'KMP_TEAMS_THREAD_LIMIT, and OMP_THREAD_LIMIT \(if any are set\)\.\n', record['stderr'])
        if record['stderr'] and not capped:
            raise common.PerfError(f'unexpected stderr: {record["stderr"]}')
        result = parsed(name, value, record['stdout'])
        if expected is not None:
            equivalent(name, result, expected)
        return record, result

    def unchanged():
        for path, digest in report['frozen'].items():
            if common.sha256_file(path) != digest:
                raise common.PerfError(f'frozen file changed: {path}')

    try:
        if args.phase == 'prepare':
            args.out.mkdir(parents=True, exist_ok=False)
            can_write = True
            report.update(host_before=common.host_info(), cpu=args.cpu, upstream_cpus=upstream_cpus,
                          load_before=list(os.getloadavg()), idle_required=False,
                          cpp_flags=[*run_sort.TARGET_FLAGS, '-ffp-contract=off', '-std=c++20', '-O2'],
                          upstream_flags=['-pipe', '-Wall', '-O3', '-fomit-frame-pointer',
                                          '-march=broadwell', '-std=c++20', '-static-libstdc++',
                                          '-fopenmp', '-pthread'],
                          method='whole-process batches, including process launch and stdout',
                          startup_case='helloworld: R=1, no 500ms calibration',
                          website='https://programming-language-benchmarks.vercel.app/cpp')
            save()
            manifest = json.loads((HERE / 'source-manifest.json').read_text())
            for entry in manifest['files']:
                if common.sha256_file(HERE / 'vendor' / entry['path']) != entry['sha256']:
                    raise common.PerfError(f'upstream source changed: {entry["path"]}')
            report['source_manifest'] = manifest
            report['compiler_source_commit'] = execute(
                ['git', '-C', Path(__file__).resolve().parents[2], 'rev-parse', 'HEAD'])['stdout'].strip()
            report['compiler_version'] = execute([args.cli, '--version'])['stdout']
            report['cpp_version'] = execute([args.llvm_bin / 'clang++', '--version'])['stdout']
            cpp_flags = report['cpp_flags']
            for name, values in CASES.items():
                directory = args.out / name
                (directory / 'src').mkdir(parents=True)
                shutil.copyfile(HERE / f'{name}.wz', directory / 'src/main.wz')
                project = directory / 'project.toml'
                project.write_text('schema_version = 1\n[package]\nname = "plb_' +
                                   name.replace('-', '_') + '"\nsource_root = "src"\nentry = "app"\n'
                                   '[build]\nbackend = "native"\nword_bits = 64\n')
                cpp = HERE / (f'{name}.cpp' if name != 'helloworld' else
                              'vendor/bench/algorithm/helloworld/1.cpp')
                programs = {}
                small_results = {}
                for opt in (0, 2):
                    source_dir = directory / f'woozy-O{opt}'
                    execute([args.cli, 'build', '--project', project, '--backend', 'native',
                             '--opt', opt, '--out-dir', source_dir, '--format', 'json'])
                    reference = directory / f'cpp-O{opt}'
                    execute([args.llvm_bin / 'clang++', *cpp_flags[:-1], f'-O{opt}',
                             f'--ld-path={args.llvm_bin / "ld.lld"}', cpp, '-o', reference])
                    for value in SMALL[name]:
                        expected = oracle(name, value)
                        _, result = checked(reference, name, value, expected)
                        checked(source_dir / 'program', name, value, result)
                        small_results[f'O{opt}/{value}'] = result
                    programs = {'woozy': str(source_dir / 'program'), 'cpp': str(reference)}
                # Export and verify both Woozy IR forms, and match CLI/helper objects.
                helper = args.cli.parent / 'native/lang_native_helper'
                execute([helper, '--project', project, '--opt', 2,
                         '--out-object', directory / 'helper.o',
                         '--out-llvm', directory / 'after.ll',
                         '--out-llvm-before', directory / 'before.ll', '--format', 'json'])
                if common.sha256_file(directory / 'helper.o') != common.sha256_file(directory / 'woozy-O2/program.o'):
                    raise common.PerfError(f'{name}: CLI/helper object mismatch')
                execute([args.llvm_bin / 'clang++', *cpp_flags, '-S', '-emit-llvm', cpp,
                         '-o', directory / 'cpp.ll'])
                for filename in ('before.ll', 'after.ll', 'cpp.ll'):
                    execute([args.llvm_bin / 'opt', '-passes=verify', '-disable-output', directory / filename])
                elf = execute([args.llvm_bin / 'llvm-readelf', '-l', '-d', programs['woozy']])
                symbols = execute([args.llvm_bin / 'llvm-nm', '-u', programs['woozy']])
                if 'INTERP' in elf['stdout'] or '(NEEDED)' in elf['stdout'] or re.search(r'^\s*U\s', symbols['stdout'], re.M):
                    raise common.PerfError(f'{name}: Woozy executable has external runtime dependencies')
                for value in values:
                    _, result = checked(programs['cpp'], name, value, oracle(name, value))
                    checked(programs['woozy'], name, value, result)
                    report['cases'].append(dict(name=name, input=value, programs=programs,
                                                expected=result, small=small_results))
                for upstream_file in ORIGINALS[name]:
                    path = HERE / 'vendor/bench/algorithm' / upstream_file
                    output = directory / ('upstream-' + path.name.removesuffix('.cpp'))
                    execute([args.llvm_bin / 'clang++', *report['upstream_flags'],
                             f'--ld-path={args.llvm_bin / "ld.lld"}', path, '-o', output])
                    for value in values:
                        expected = next(row['expected'] for row in report['cases']
                                        if row['name'] == name and row['input'] == value)
                        checked(output, name, value, expected, upstream=True)
                        report['originals'].append(dict(name=name, input=value, source=upstream_file,
                                                       program=str(output), expected=expected))
                print(json.dumps(dict(name=name, status='built-and-checked')), flush=True)
            frozen = [path for path in HERE.rglob('*') if path.is_file()]
            frozen += [Path(__file__), Path(common.__file__), Path(run_sort.__file__), args.cli,
                       args.cli.parent / 'native/lang_native_helper',
                       args.cli.parent / 'native/liblang_native_runtime.a',
                       args.cli.parent / 'CMakeCache.txt', args.cli.parent / 'build.ninja',
                       omp / 'libomp.so']
            frozen += list((Path(__file__).resolve().parents[2] / 'stdlib').rglob('*.wz'))
            frozen += [args.llvm_bin / name for name in ('clang', 'clang++', 'ld.lld', 'opt', 'llvm-readelf', 'llvm-nm')]
            frozen += [path for path in args.out.rglob('*') if path.is_file() and path != report_path]
            report['frozen'] = {str(path): common.sha256_file(path) for path in frozen}
            report['status'] = 'prepared'
        else:
            report = json.loads(report_path.read_text())
            if report['status'] != 'prepared' or report['cpu'] != args.cpu or report['upstream_cpus'] != upstream_cpus:
                raise common.PerfError('matching prepared run required; samples cannot be overwritten')
            unchanged()
            can_write = True
            report['status'] = 'sampling'

            def batch(row, implementation, rounds):
                records = []
                for _ in range(rounds):
                    record, _ = checked(row['programs'][implementation], row['name'], row['input'], row['expected'])
                    records.append(len(report['commands']) - 1)
                return dict(rounds=rounds, commands=records,
                            elapsed_ns=sum(report['commands'][i]['elapsed_ns'] for i in records))

            for row in report['cases']:
                rounds, calibration = 1, []
                row.update(calibration=calibration, load_before=list(os.getloadavg()))
                if row['name'] != 'helloworld':
                    while True:
                        trials = [batch(row, 'cpp', rounds) for _ in range(3)]
                        calibration.append(dict(rounds=rounds, trials=trials))
                        times = [trial['elapsed_ns'] for trial in trials]
                        if statistics.median(times) >= 500_000_000 and min(times) >= 250_000_000:
                            break
                        rounds *= 2
                        if rounds > 1024:
                            raise common.PerfError('C++ batch calibration exceeded 1024 processes')
                    # Retain 2R controls for both implementations without changing inputs.
                    row['double_round_controls'] = {name: batch(row, name, 2 * rounds)
                                                    for name in ('cpp', 'woozy')}
                row.update(rounds=rounds, calibration=calibration, samples=[])
                for phase, count in (('warmup', 2), ('measured', 9)):
                    for pair in range(count):
                        order = ('woozy', 'cpp') if pair % 2 == 0 else ('cpp', 'woozy')
                        for implementation in order:
                            sample = batch(row, implementation, rounds)
                            sample.update(phase=phase, pair=pair, implementation=implementation)
                            row['samples'].append(sample)
                            save()
                row['median_ms'] = {name: statistics.median(
                    sample['elapsed_ns'] for sample in row['samples']
                    if sample['phase'] == 'measured' and sample['implementation'] == name) / rounds / 1e6
                    for name in ('woozy', 'cpp')}
                row['ratio'] = row['median_ms']['woozy'] / row['median_ms']['cpp']
                row['load_after'] = list(os.getloadavg())
                unchanged()
                print(json.dumps(dict(name=row['name'], input=row['input'], ratio=row['ratio'])), flush=True)
            for row in report['originals']:
                row['samples'] = []
                for phase, count in (('warmup', 2), ('measured', 9)):
                    for _ in range(count):
                        record, _ = checked(row['program'], row['name'], row['input'], row['expected'], upstream=True)
                        row['samples'].append(dict(phase=phase, command=len(report['commands']) - 1,
                                                   elapsed_ns=record['elapsed_ns']))
                row['median_ms'] = statistics.median(sample['elapsed_ns'] for sample in row['samples']
                                                     if sample['phase'] == 'measured') / 1e6
                unchanged()
                save()
                print(json.dumps(dict(upstream=row['source'], input=row['input'], ms=row['median_ms'])), flush=True)
            report.update(status='sampled', host_after=common.host_info(), load_after=list(os.getloadavg()))
            with (args.out / 'results.csv').open('w', newline='') as stream:
                writer = csv.writer(stream)
                writer.writerow(['name', 'input', 'cpp_ms', 'woozy_ms', 'woozy/cpp', 'processes_per_batch'])
                for row in report['cases']:
                    writer.writerow([row['name'], row['input'], row['median_ms']['cpp'],
                                     row['median_ms']['woozy'], row['ratio'], row['rounds']])
        report['updated_at_unix'] = time.time()
        save()
    except Exception as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}')
        if can_write:
            save()
        raise
    print(json.dumps(dict(status=report['status'], cases=len(report['cases']), report=str(report_path))))


if __name__ == '__main__':
    main()
