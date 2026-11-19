#!/usr/bin/env python3
"""Exercise the current package-object and scalar frontend-cache stage."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cli', required=True, type=Path)
parser.add_argument('--out', required=True, type=Path)
parser.add_argument('--libxml', required=True, type=Path)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
environment = os.environ.copy()
environment['LD_LIBRARY_PATH'] = str(args.libxml) + (':' + environment['LD_LIBRARY_PATH'] if environment.get('LD_LIBRARY_PATH') else '')
records = []

def digest(data):
    return hashlib.sha256(data).hexdigest()

def command(label, argv, expected=0):
    start = time.monotonic()
    result = subprocess.run([str(arg) for arg in argv], capture_output=True, env=environment, timeout=180)
    (args.out / (label + '.stdout')).write_bytes(result.stdout)
    (args.out / (label + '.stderr')).write_bytes(result.stderr)
    record = {'label': label, 'argv': [str(arg) for arg in argv], 'exit': result.returncode,
              'expected_exit': expected, 'seconds': time.monotonic() - start,
              'stdout_sha256': digest(result.stdout), 'stderr_sha256': digest(result.stderr)}
    records.append(record)
    if result.returncode != expected:
        raise AssertionError(f'{label}: exit {result.returncode}, expected {expected}: {result.stderr.decode(errors="replace")} {result.stdout.decode(errors="replace")}')
    return result, record

def project(path):
    (path / 'src/a').mkdir(parents=True)
    (path / 'src/b').mkdir()
    (path / 'lang.toml').write_text('schema_version=1\n[package]\nname="three"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
    (path / 'src/main.wz').write_text('package main;\nimport "app/a" as a;\nimport "app/b" as b;\nfunc main() i32 { return a.value() + b.value() - 42; }\n')
    (path / 'src/a/a.wz').write_text('package a;\nfunc secret() i32 { return 12; }\npub func value() i32 { return secret(); }\n')
    (path / 'src/b/b.wz').write_text('package b;\npub func value() i32 { return 30; }\n')
    return path

p = project(args.out / 'project')
cache = args.out / 'cache'

def build(label, *, cache_path=cache, project_path=p, output=None, opt='2', expected=0, program=0):
    output = output or args.out / label
    argv = [args.cli, 'build', '--project', project_path / 'lang.toml', '--out-dir', output,
            '--package-objects', '--opt', opt, '--format', 'json']
    if cache_path is not None:
        argv += ['--cache-dir', cache_path]
    result, record = command(label, argv, expected)
    protocol = json.loads(result.stdout)
    assert protocol['status'] == ('ok' if expected == 0 else 'error'), protocol
    if expected == 0:
        stats = json.loads((output / 'cache-statistics.json').read_text())
        log = (output / 'package-build.log').read_text()
        record['stats'] = stats
        record['program_sha256'] = digest((output / 'program').read_bytes())
        assert stats['opt_jobs'] == len(re.findall(r'^\[\d+/\d+\].* -passes=', log, re.M)), log
        assert stats['llc_jobs'] == len(re.findall(r'^\[\d+/\d+\].* -filetype=obj ', log, re.M)), log
        command(label + '-program', [output / 'program'], program)
        record['source_files'] = {str(file.relative_to(project_path)): digest(file.read_bytes()) for file in sorted((project_path / 'src').rglob('*.wz'))}
        return stats

try:
    hashes = {str(args.cli): digest(args.cli.read_bytes())}
    for file in [args.cli.parent / 'native/lang_native_helper', args.cli.parent / 'native/liblang_native_runtime.a',
                 args.cli.parent / 'native/liblang_hosted_runtime.a']:
        hashes[str(file)] = digest(file.read_bytes())
    (args.out / 'compiler-runtime-inputs.json').write_text(json.dumps(hashes, indent=2))
    cold = build('cold')
    cold_ir = {record['logical_path']: record['llvm_ir_sha256'] for record in json.loads((args.out / 'cold/package-records.json').read_text())}
    assert (cold['parse_misses'], cold['checked_bodies'], cold['opt_jobs'], cold['llc_jobs']) == (3, 4, 3, 3), cold
    warm = build('warm')
    assert (warm['parse_hits'], warm['checked_bodies'], warm['object_hits'], warm['opt_jobs'], warm['llc_jobs']) == (3, 0, 3, 0, 0), warm
    (p / 'src/a/a.wz').write_text('package a;\nfunc secret() i32 { var x i32 = 11; x += 2; return x; }\npub func value() i32 { return secret(); }\n')
    changed = build('private-length-change', program=1)
    assert (changed['parse_misses'], changed['frontend_package_hits'], changed['checked_bodies'], changed['object_hits'], changed['opt_jobs']) == (1, 2, 2, 2, 1), changed
    changed_records = json.loads((args.out / 'private-length-change/package-records.json').read_text())
    for record in changed_records:
        if record['logical_path'] in ('app', 'app/b'):
            assert record['llvm_ir_sha256'] == cold_ir[record['logical_path']] and record['object_cache_hit'], record
        elif record['logical_path'] == 'app/a':
            assert record['llvm_ir_sha256'] != cold_ir['app/a'] and not record['object_cache_hit'], record
    build('private-clean', cache_path=None, program=1)
    (p / 'src/a/a.wz').write_text('package a;\nfunc secret() i32 { return 13; }\npub func value(x i32) i32 { return secret() + x; }\n')
    main = p / 'src/main.wz'
    main.write_text(main.read_text().replace('a.value()', 'a.value(2)'))
    abi = build('public-abi', program=3)
    assert abi['frontend_package_hits'] == 1 and abi['opt_jobs'] >= 2, abi
    # Other packages declare this public function too; a changed signature may
    # change their actual optimizer input and conservatively recompile them.
    # File insertion changes resolver and runtime snapshot numbering. Existing
    # function keys stay stable; objects may rebuild conservatively for spans.
    (p / 'src/a/0-extra.wz').write_text('package a; func extra() i32 { return 99; }\n')
    build('file-add', program=3)
    (p / 'src/a/0-extra.wz').unlink()
    build('file-remove', program=3)
    # An invalid rebuild must leave the previous successful program executable.
    old = digest((args.out / 'file-remove/program').read_bytes())
    source = (p / 'src/a/a.wz').read_text()
    (p / 'src/a/a.wz').write_text(source.replace('return 13;', 'return true;'))
    build('source-failure', output=args.out / 'file-remove', expected=1)
    assert digest((args.out / 'file-remove/program').read_bytes()) == old
    command('old-program-after-failure', [args.out / 'file-remove/program'], 3)
    (p / 'src/a/a.wz').write_text(source)
    # Independently hash-correct, malformed frontend input must be rejected.
    hir_files = sorted((cache / 'frontend').glob('*.hir'))
    mutated = 0
    for file in hir_files:
        body = file.read_bytes()[65:]
        if b'2:1313:' in body:
            body = body.replace(b'2:1313:', b'2:1414:', 1)
            file.write_bytes(digest(body).encode() + b'\n' + body)
            mutated += 1
    assert mutated, 'semantic-corruption fixture literal not found'
    semantic = build('semantic-cache-corruption', program=3)
    assert semantic['rejected_frontend_cache'] > 0 and semantic['checked_bodies'] > 0, semantic
    # Truncated/hash-mismatched parse and object files fall back to real work.
    for file in (cache / 'parse').glob('*.ast'):
        file.write_bytes(file.read_bytes()[:31])
    for file in (cache / 'frontend').glob('*.obj'):
        file.write_bytes(b'not-an-object')
    damaged = build('truncated-cache', program=3)
    assert damaged['parse_misses'] == 3 and damaged['opt_jobs'] == 3, damaged
    build('optimization-change', opt='0', program=3)
    special = project(args.out / '带 空格-$项目')
    build('special-path', project_path=special, cache_path=args.out / '带 $空格-cache', output=args.out / '带 $空格-out')
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(build, 'concurrent-' + str(index), output=args.out / 'concurrent-output', program=3) for index in range(2)]
        for future in futures:
            future.result()
    summary = {'status': 'passed', 'scope': 'native package objects and C++ scalar frontend cache',
               'commands': records, 'not_run': ['Wz frontend cache', 'full language HIR persistence', 'Ninja frontend parallel jobs', 'formal Source bootstrap']}
except Exception as error:
    summary = {'status': 'failed', 'error': str(error), 'commands': records}
    raise
finally:
    (args.out / 'report.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'status': summary['status'], 'commands': len(records), 'out': str(args.out)}, ensure_ascii=False))
