#!/usr/bin/env python3
"""Execute a test-only host that returns dynamic immutable Python strings."""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

EXPECTED = '宿\0主😀'.encode('utf-8')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def command(argv, cwd=None):
    result = subprocess.run(list(map(str, argv)), cwd=cwd, capture_output=True,
                            timeout=90)
    return dict(argv=list(map(str, argv)), cwd=None if cwd is None else str(cwd),
                returncode=result.returncode, stdout_hex=result.stdout.hex(),
                stderr=result.stderr.decode('utf-8'))


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'helper', 'root', 'python', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    project = args.root / 'tests/python_bindings/string-aggregate'
    binding = project / 'binding.json'
    host = args.root / 'tests/python_bindings/string_aggregate_host.py'
    report = dict(operation_id='string-aggregate-binding-20261109',
                  status='failed', commands=[], values=[])
    try:
        report['python'] = command([args.python, '--version'])
        assert report['python']['returncode'] == 0 and \
            bytes.fromhex(report['python']['stdout_hex']).decode().strip() == 'Python 3.12.15'
        cli_out = args.out / 'cli'
        for phase in ('check', 'build'):
            argv = [args.cli, phase, '--project', project / 'lang.toml',
                    '--backend', 'python', '--std-binding', binding,
                    '--format', 'json']
            if phase == 'build':
                argv += ['--out-dir', cli_out]
            row = command(argv)
            report['commands'].append(row)
            assert row['returncode'] == 0 and not row['stderr'], row
            assert json.loads(bytes.fromhex(row['stdout_hex']))['status'] == 'ok', row
        shutil.copyfile(host, cli_out / 'strhost.py')
        row = command([args.python, '-B', '-s', cli_out / 'program.py'], cwd=cli_out)
        report['commands'].append(row)
        assert row['returncode'] == 0 and not row['stdout_hex'] and not row['stderr'], row

        helper_out = args.out / 'helper'
        row = command([args.helper, '--project', project / 'lang.toml',
                       '--out-dir', helper_out, '--std-binding', binding])
        report['commands'].append(row)
        assert row['returncode'] == 0 and not row['stderr'], row
        shutil.copyfile(host, helper_out / 'strhost.py')
        metadata = json.loads((helper_out / 'metadata.json').read_text())
        names = {s['source_name']: s['generated'] for s in metadata['symbols']
                 if s['source_name'].startswith('probe_host')}
        assert set(names) == {'probe_host', 'probe_host_enum', 'probe_host_vec',
                              'probe_host_release'}, names
        report['generated_identical'] = all(
            (cli_out / name).read_bytes() == (helper_out / name).read_bytes()
            for name in ('program.py', 'langrt.py'))
        assert report['generated_identical'], 'CLI and helper Python differ'
        code = ('import json,program\n'
                'out={}\n'
                f'for name,generated in {names!r}.items():\n'
                ' value=getattr(program,generated)()\n'
                ' assert type(value) is str\n'
                ' data=value.encode("utf-8")\n'
                ' out[name]={"hex":data.hex(),"byte_length":len(data),"text":value}\n'
                'print(json.dumps(out,ensure_ascii=True))\n')
        row = command([args.python, '-B', '-s', '-c', code], cwd=helper_out)
        report['commands'].append(row)
        assert row['returncode'] == 0 and not row['stderr'], row
        observed = json.loads(bytes.fromhex(row['stdout_hex']))
        for name in sorted(names):
            value = observed[name]
            result = dict(id=name, actual_hex=value['hex'],
                          actual_byte_length=value['byte_length'],
                          expected_hex=EXPECTED.hex(),
                          expected_byte_length=len(EXPECTED), status='failed')
            report['values'].append(result)
            assert value['hex'] == EXPECTED.hex(), result
            assert value['byte_length'] == len(EXPECTED), result
            assert value['text'] == EXPECTED.decode('utf-8'), result
            result['status'] = 'passed'
        report['sources'] = {str(p.relative_to(project)): sha(p)
                             for p in sorted(project.rglob('*')) if p.is_file()}
        report['host_sha256'] = sha(host)
        report['status'] = 'passed'
    finally:
        path = args.out / 'binding-results.json'
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        print(json.dumps(dict(status=report['status'],
                              values=len(report['values']), report=str(path))))
    return report['status'] != 'passed'


if __name__ == '__main__':
    raise SystemExit(main())
