#!/usr/bin/env python3
"""Compile the Wz identity implementation and compare its bytes with C++."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cli', required=True, type=Path)
parser.add_argument('--oracle', required=True, type=Path)
parser.add_argument('--source-root', required=True, type=Path)
parser.add_argument('--out', required=True, type=Path)
parser.add_argument('--libxml', required=True, type=Path)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
project = args.out / 'project'
(project / 'src').mkdir(parents=True)
(project / 'src/wzcc').symlink_to(args.source_root.resolve(), target_is_directory=True)
(project / 'src/main.wz').write_bytes(Path(__file__).with_name('identity.wz').read_bytes())
(project / 'lang.toml').write_text('schema_version=1\n[package]\nname="identity_probe"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
environment = os.environ.copy()
environment['LD_LIBRARY_PATH'] = str(args.libxml) + (':' + environment['LD_LIBRARY_PATH'] if environment.get('LD_LIBRARY_PATH') else '')
commands = []

def command(label, argv):
    start = time.monotonic()
    result = subprocess.run([str(arg) for arg in argv], capture_output=True, env=environment, timeout=300)
    (args.out / (label + '.stdout')).write_bytes(result.stdout)
    (args.out / (label + '.stderr')).write_bytes(result.stderr)
    commands.append({'label': label, 'argv': [str(arg) for arg in argv], 'exit': result.returncode,
        'seconds': time.monotonic() - start, 'stdout_sha256': hashlib.sha256(result.stdout).hexdigest(),
        'stderr_sha256': hashlib.sha256(result.stderr).hexdigest()})
    assert result.returncode == 0, (label, result.stdout, result.stderr)
    return result.stdout

try:
    expected = command('cpp', [args.oracle, '--dump'])
    command('build', [args.cli, 'build', '--project', project / 'lang.toml', '--out-dir', args.out / 'native', '--opt', '0', '--format', 'json'])
    actual = command('wz', [args.out / 'native/program'])
    assert actual == expected, (actual, expected)
    report = {'status': 'passed', 'matching_lines': len(actual.splitlines()), 'commands': commands}
except Exception as error:
    report = {'status': 'failed', 'error': str(error), 'commands': commands}
    raise
finally:
    (args.out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'out': str(args.out)}))
