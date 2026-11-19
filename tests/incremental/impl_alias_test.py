#!/usr/bin/env python3
"""Two file-local Q aliases must give two distinct impl method identities."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cli', required=True, type=Path)
parser.add_argument('--out', required=True, type=Path)
parser.add_argument('--libxml', required=True, type=Path)
parser.add_argument('--allow-known-bug', action='store_true')
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
project = args.out / 'project'; source = project / 'src'
source.mkdir(parents=True)
(project / 'lang.toml').write_text('schema_version=1\n[package]\nname="aliases"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
(source / 'main.wz').write_text('''package app;
import "app/model" as model;
import "app/ta" as A;
import "app/tb" as B;
import "app/adapters" as _;
func left[T A.Convert](value T) i32 { return value.convert(); }
func right[T B.Convert](value T) i32 { return value.convert(); }
func main() i32 {
    let value = model.S(value: 0);
    return left[model.S](value) * 10 + right[model.S](value);
}
''')
for name in ['model', 'ta', 'tb', 'adapters']:
    (source / name).mkdir()
(source / 'model/model.wz').write_text('package model; pub struct S { pub value i32; }')
for name in ['ta', 'tb']:
    (source / name / 'trait.wz').write_text(f'package {name}; pub trait Convert {{ func convert(self Self) i32; }}')
for filename, trait, value in [('a.wz', 'ta', 17), ('b.wz', 'tb', 29)]:
    (source / 'adapters' / filename).write_text(f'''package adapters;
import "app/{trait}" as Q;
import "app/model" as model;
impl Q.Convert for model.S {{ func convert(self Self) i32 {{ return {value}; }} }}
''')
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = str(args.libxml) + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
commands = []

def command(label, argv, expected=0):
    begin = time.monotonic()
    result = subprocess.run([str(arg) for arg in argv], env=env, capture_output=True, timeout=180)
    (args.out / (label + '.stdout')).write_bytes(result.stdout)
    (args.out / (label + '.stderr')).write_bytes(result.stderr)
    commands.append({'label': label, 'argv': [str(arg) for arg in argv], 'exit': result.returncode,
        'expected_exit': expected, 'seconds': time.monotonic() - begin,
        'stdout_sha256': hashlib.sha256(result.stdout).hexdigest(), 'stderr_sha256': hashlib.sha256(result.stderr).hexdigest()})
    if expected is not None: assert result.returncode == expected, (label, result.stdout, result.stderr, result.returncode)
    return result

def build(label, cached):
    output = args.out / label
    argv = [args.cli, 'build', '--project', project / 'lang.toml', '--out-dir', output, '--opt', '2', '--format', 'json']
    if cached: argv += ['--package-objects', '--cache-dir', args.out / 'cache']
    result = command(label, argv); assert json.loads(result.stdout)['status'] == 'ok'
    run = command(label + '-program', [output / 'program'], None)
    result = {'exit': run.returncode}
    if cached: result['stats'] = json.loads((output / 'cache-statistics.json').read_text())
    return result

try:
    legacy = build('whole-reference', False); assert legacy['exit'] == 199
    cold = build('cold', True); assert cold['exit'] == 199
    warm = build('warm', True); assert warm['exit'] == 199 and warm['stats']['opt_jobs'] == 0
    (source / 'adapters/a.wz').rename(source / 'adapters/z.wz')
    reordered = build('reordered-impl-files', True)
    if not args.allow_known_bug: assert reordered['exit'] == 199, reordered
    assert reordered['stats']['opt_jobs'] == 1 and reordered['stats']['object_hits'] == 1, reordered
    report = {'status': 'known_bug_reproduced' if reordered['exit'] != 199 else 'passed',
              'expected_exit': 199, 'whole': legacy, 'cold': cold, 'warm': warm,
              'reordered': reordered, 'commands': commands}
except Exception as error:
    report = {'status': 'failed', 'error': str(error), 'commands': commands}
    raise
finally:
    (args.out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'commands': len(commands), 'out': str(args.out)}, ensure_ascii=False))
