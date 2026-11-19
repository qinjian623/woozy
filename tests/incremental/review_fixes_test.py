#!/usr/bin/env python3
"""Cache string-source binding and whole-directory publication regression tests."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
for name in ['cli', 'publish-test', 'clang', 'lld', 'runtime', 'out', 'libxml']:
    parser.add_argument('--' + name, required=True, type=Path)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = str(args.libxml) + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
commands = []

def sha(data): return hashlib.sha256(data).hexdigest()
def command(label, argv, expected=0):
    start = time.monotonic()
    result = subprocess.run([str(arg) for arg in argv], capture_output=True, env=env, timeout=180)
    (args.out / (label + '.stdout')).write_bytes(result.stdout)
    (args.out / (label + '.stderr')).write_bytes(result.stderr)
    commands.append({'label': label, 'argv': [str(arg) for arg in argv], 'exit': result.returncode,
                     'expected_exit': expected, 'seconds': time.monotonic() - start,
                     'stdout_sha256': sha(result.stdout), 'stderr_sha256': sha(result.stderr)})
    assert result.returncode == expected, (label, result.returncode, result.stdout, result.stderr)
    return result

def project(name, source):
    path = args.out / name
    (path / 'src').mkdir(parents=True)
    (path / 'lang.toml').write_text('schema_version=1\n[package]\nname="regression"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
    (path / 'src/main.wz').write_text(source)
    return path

def build(label, source, cache, output, opt='2'):
    result = command(label, [args.cli, 'build', '--project', source / 'lang.toml', '--out-dir', output,
                            '--package-objects', '--cache-dir', cache, '--opt', opt, '--format', 'json'])
    assert json.loads(result.stdout)['status'] == 'ok'
    return json.loads((output / 'cache-statistics.json').read_text())

def corrupt(cache, old, new):
    changed = []
    for file in (cache / 'parse').glob('*.ast'):
        previous = file.read_bytes()
        body = previous[65:]
        if old in body:
            altered = body.replace(old, new, 1)
            file.write_bytes(sha(altered).encode() + b'\n' + altered)
            changed.append({'file': str(file), 'before_sha256': sha(previous), 'after_sha256': sha(file.read_bytes())})
    assert len(changed) == 1, changed
    return changed

class Wire:
    def __init__(self, body): self.body, self.cursor = body, 0
    def number(self):
        start = self.cursor
        end = self.body.index(b':', start)
        self.cursor = end + 1
        return int(self.body[start:end])
    def string(self):
        size = self.number(); self.cursor += size
    def type(self):
        kind = self.number(); self.number()
        if kind == 8:
            for _ in range(self.number()): self.number(); self.type()
            self.type()
    def source(self):
        self.string()
        for _ in range(self.number()): self.number()
        self.number(); self.number()

def structure_edits(body, category):
    wire = Wire(body); wire.string(); wire.string()
    nodes, scopes = wire.number(), wire.number()
    for index in range(scopes):
        wire.string(); start = wire.cursor; parent = wire.number()
        if parent: wire.number()
        if category == 'scope-cycle' and index == 0:
            assert parent == 0
            return [(start, wire.cursor, b'1:0:')]
        wire.source()
    for index in range(nodes):
        wire.type(); wire.source(); wire.number()
        children_start = wire.cursor
        children = wire.number()
        for _ in range(children): wire.number()
        children_end = wire.cursor
        tag = wire.number()
        if tag == 2: wire.string(); wire.number()
        elif tag == 20: wire.string()
        elif tag == 15: pass
        elif tag == 14:
            kind_start = wire.cursor; kind = wire.number(); kind_end = wire.cursor
            wire.type()
            direct_start = wire.cursor; direct = wire.number()
            if direct: wire.string()
            direct_end = wire.cursor
            callee_start = wire.cursor; callee = wire.number()
            if callee: wire.number()
            callee_end = wire.cursor
            assert wire.number() == 0
            if category == 'node-cycle':
                assert kind == 0 and direct == 1 and callee == 0 and children == 0
                return [(children_start, children_end, f'1:{index}:'.encode()),
                        (kind_start, kind_end, b'1:'), (direct_start, direct_end, b'0:'),
                        (callee_start, callee_end, f'1:{index}:'.encode())]
        else: raise AssertionError(('unexpected fixture node', tag))
    assert wire.number() > 0
    wire.string(); start = wire.cursor; wire.type()
    assert category == 'nonfunction-signature'
    return [(start, wire.cursor, b'2:32:')]

try:
    imported = project('import-source', 'package main; import "app/pkgA" as dep; func main() i32 { return dep.value(); }')
    for name, value in [('pkgA', 17), ('pkgB', 29)]:
        directory = imported / 'src' / name
        directory.mkdir()
        (directory / 'value.wz').write_text(f'package {name}; pub func value() i32 {{ return {value}; }}')
    cache = args.out / 'import-cache'; output = args.out / 'import-output'
    build('import-cold', imported, cache, output)
    command('import-before', [output / 'program'], 17)
    import_corruption = corrupt(cache, b'8:app/pkgA', b'8:app/pkgB')
    imported_stats = build('import-corrupt', imported, cache, output)
    command('import-after', [output / 'program'], 17)
    assert imported_stats['parse_misses'] == 1 and imported_stats['string_tokens_checked'] >= 1
    literal = project('literal-source', 'package main; func main() i32 { if "alpha" == "alpha" { return 7; } return 9; }')
    cache = args.out / 'literal-cache'; output = args.out / 'literal-output'
    build('literal-cold', literal, cache, output)
    command('literal-before', [output / 'program'], 7)
    literal_corruption = corrupt(cache, b'5:alpha', b'5:bravo')
    literal_stats = build('literal-corrupt', literal, cache, output)
    command('literal-after', [output / 'program'], 7)
    assert literal_stats['parse_misses'] == 1 and literal_stats['string_tokens_checked'] >= 1
    structural = project('structure-source', 'package main; func value() i32 { return 17; } func main() i32 { return value(); }')
    cache = args.out / 'structure-cache'; output = args.out / 'structure-output'
    build('structure-cold', structural, cache, output)
    frontend_key = json.loads((output / 'package-records.json').read_text())[0]['frontend_key']
    cache_file = cache / 'frontend' / (frontend_key + '.hir')
    correct = cache_file.read_bytes()[65:]
    structural_results = []
    for category in ['scope-cycle', 'node-cycle', 'nonfunction-signature']:
        body = correct
        for begin, end, replacement in sorted(structure_edits(correct, category), reverse=True):
            body = body[:begin] + replacement + body[end:]
        corrupted = sha(body).encode() + b'\n' + body
        cache_file.write_bytes(corrupted)
        (args.out / (category + '.hir')).write_bytes(corrupted)
        stats = build(category, structural, cache, output)
        command(category + '-program', [output / 'program'], 17)
        assert stats['rejected_frontend_cache'] > 0 and stats['checked_bodies'] > 0, stats
        structural_results.append({'case': category, 'corrupted_sha256': sha(corrupted), 'stats': stats})
    stress = args.out / 'publish-stress'; stress.mkdir()
    command('publication-stress', [args.publish_test, stress])
    first = project('writer-A', 'package main; func main() i32 { return 17; }')
    second = project('writer-B', 'package main; func main() i32 { return 29; }')
    shared = args.out / 'shared-output'; shared.mkdir()
    alias = args.out / 'shared-output-alias'; alias.symlink_to(shared, target_is_directory=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(build, 'concurrent-A', first, args.out / 'concurrent-cache', shared, '0'),
                   executor.submit(build, 'concurrent-B', second, args.out / 'concurrent-cache', alias, '2')]
        for future in futures: future.result()
    metadata = json.loads((shared / 'package-records.json').read_text())
    identity = metadata[0]['identity']
    expected = 17 if str(first / 'src') in identity else 29
    assert str(first / 'src') in identity or str(second / 'src') in identity
    command('concurrent-program', [shared / 'program'], expected)
    command('relink-published-object', [args.clang, '--target=x86_64-unknown-linux-gnu', '--ld-path=' + str(args.lld),
        '-nostdlib', '-static', shared / 'program.o', args.runtime, '-o', args.out / 'relinked-program'])
    command('relinked-program', [args.out / 'relinked-program'], expected)
    log = (shared / 'package-build.log').read_text()
    assert ('default<O0>' if expected == 17 else 'default<O2>') in log
    report = {'status': 'passed', 'commands': commands, 'import_corruption': import_corruption,
              'literal_corruption': literal_corruption, 'import_stats': imported_stats,
              'literal_stats': literal_stats, 'structure_checks': structural_results, 'published_identity': identity, 'published_program_exit': expected}
except Exception as error:
    report = {'status': 'failed', 'error': str(error), 'commands': commands}
    raise
finally:
    (args.out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'commands': len(commands), 'out': str(args.out)}, ensure_ascii=False))
