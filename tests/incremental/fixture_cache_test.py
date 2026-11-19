#!/usr/bin/env python3
"""Run reviewed source fixtures through the new package build, retaining every command."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cli', type=Path, required=True)
parser.add_argument('--fixtures', type=Path, default=Path(__file__).parent / 'fixtures')
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--libxml', type=Path, required=True)
args = parser.parse_args()
args.out.mkdir(parents=True, exist_ok=False)
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = str(args.libxml) + (':' + env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
manifest = json.loads((args.fixtures / 'manifest.json').read_text())
commands = []
cases = []

def sha(data):
    return hashlib.sha256(data).hexdigest()

def command(label, argv, expected):
    begin = time.monotonic()
    result = subprocess.run([str(arg) for arg in argv], env=env, capture_output=True, timeout=180)
    (args.out / (label + '.stdout')).write_bytes(result.stdout)
    (args.out / (label + '.stderr')).write_bytes(result.stderr)
    commands.append({'label': label, 'argv': [str(arg) for arg in argv], 'exit': result.returncode,
                     'expected_exit': expected, 'seconds': time.monotonic() - begin,
                     'stdout_sha256': sha(result.stdout), 'stderr_sha256': sha(result.stderr)})
    assert result.returncode == expected, (label, result.returncode, expected, result.stdout, result.stderr)
    return result

try:
    for fixture in manifest['fixtures']:
        name = fixture['id']
        base = args.fixtures / name / 'base'
        project = args.out / name / 'project'
        cache = args.out / name / 'cache'
        output = args.out / name / 'output'
        for label, variant in [('cold', None), ('warm', None)] + [(value['name'], value) for value in fixture['variants']]:
            if project.exists():
                shutil.rmtree(project)
            shutil.copytree(base, project)
            expected = fixture['base_expect'] if variant is None else variant['expect']
            if variant is not None:
                mutation_root = args.fixtures / name / 'variants' / label
                mutation = json.loads((mutation_root / 'mutation.json').read_text())
                for action in mutation['actions']:
                    target = project / action['path']
                    if action['old']:
                        assert sha(target.read_bytes()) == action['old']['sha256']
                    if action['new']:
                        data = (mutation_root / action['new']['file']).read_bytes()
                        assert sha(data) == action['new']['sha256']
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(data)
                    else:
                        target.unlink()
                        parent = target.parent
                        while parent != project and not any(parent.iterdir()):
                            parent.rmdir()
                            parent = parent.parent
            full_label = name + '-' + label
            source_hashes = {str(file.relative_to(project)): sha(file.read_bytes()) for file in sorted(project.rglob('*')) if file.is_file()}
            previous_program = sha((output / 'program').read_bytes()) if (output / 'program').exists() else None
            build_exit = expected.get('build_exit', expected.get('check_exit'))
            result = command(full_label, [args.cli, 'build', '--project', project / 'lang.toml',
                '--out-dir', output, '--package-objects', '--cache-dir', cache, '--opt', '2', '--format', 'json'], build_exit)
            protocol = json.loads(result.stdout)
            assert protocol['status'] == expected['status'], (full_label, protocol)
            case = {'fixture': name, 'variant': label, 'passed': True, 'source_files': source_hashes,
                    'expected': expected, 'actual_status': protocol['status']}
            if build_exit == 0:
                case['stats'] = json.loads((output / 'cache-statistics.json').read_text())
                case['program_sha256'] = sha((output / 'program').read_bytes())
                # Output files are overwritten by later builds. Retain this
                # invocation's real Ninja commands and counters separately.
                (args.out / (full_label + '.ninja.log')).write_bytes((output / 'package-build.log').read_bytes())
                run = command(full_label + '-program', [output / 'program'], expected['run_exit'])
                assert run.stdout.decode() == expected.get('stdout', '')
                assert run.stderr.decode() == expected.get('stderr', '')
                if label == 'warm':
                    assert case['stats']['opt_jobs'] == 0 and case['stats']['llc_jobs'] == 0, case
            else:
                codes = {diagnostic['code'] for diagnostic in protocol['diagnostics']}
                assert codes.intersection(expected['error_codes_any']), (full_label, codes, expected)
                case['diagnostic_codes'] = sorted(codes)
                case['old_program_preserved'] = previous_program == sha((output / 'program').read_bytes())
                assert case['old_program_preserved'], full_label
            cases.append(case)
    report = {'status': 'passed', 'cases': cases, 'commands': commands,
              'scope': 'new package build/cache behavior, using independently reviewed source inputs',
              'not_claimed': ['full-language frontend cache hits', 'Wz frontend cache', 'formal self-host bootstrap']}
except Exception as error:
    report = {'status': 'failed', 'error': str(error), 'cases': cases, 'commands': commands}
    raise
finally:
    (args.out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'cases': len(cases), 'commands': len(commands), 'out': str(args.out)}, ensure_ascii=False))
