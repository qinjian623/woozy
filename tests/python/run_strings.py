#!/usr/bin/env python3
"""Run CLI-generated Python independently and inspect actual str/UTF-8 values."""
import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from string_cases import (STRING_FIXTURES, STRING_FIXTURE_PATHS, command,
                           rejection_checks, runtime_checks, sha, succeeded)


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'helper', 'root', 'python', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--fixture', choices=STRING_FIXTURES, default='strings')
    args = parser.parse_args()
    expected_values = STRING_FIXTURES[args.fixture]
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = args.root / STRING_FIXTURE_PATHS[args.fixture] / 'lang.toml'
    report = dict(operation_id='language-constant-strings-20261104', backend='python',
                  status='failed', values=[], rejections=[],
                  source_head=command(['git', '-C', args.root, 'rev-parse', 'HEAD']),
                  sources={str(p.relative_to(manifest.parent)): sha(p)
                           for p in sorted(manifest.parent.rglob('*')) if p.is_file()})
    try:
        report['python'] = version = command([args.python, '--version'])
        succeeded(version)
        assert bytes.fromhex(version['stdout_hex']).decode().strip() == 'Python 3.12.15', version
        out = args.out / 'cli'
        report['check'] = result = command([args.cli, 'check', '--project', manifest,
            '--backend', 'python', '--format', 'json'])
        succeeded(result)
        report['build'] = result = command([args.cli, 'build', '--project', manifest,
            '--backend', 'python', '--out-dir', out, '--format', 'json'])
        succeeded(result)
        report['execute'] = result = command([args.python, '-B', '-s', out / 'program.py'])
        succeeded(result)
        assert not result['stdout_hex'], result
        report['mapping'] = result = command([args.helper, '--project', manifest,
            '--out-dir', args.out / 'mapping'])
        succeeded(result)
        metadata = json.loads((args.out / 'mapping/metadata.json').read_text())
        report['helper_code_identical'] = (out / 'program.py').read_bytes() == (args.out / 'mapping/program.py').read_bytes()
        assert report['helper_code_identical'], 'formal CLI and helper Python differ'
        names = {s['source_name']: s['generated'] for s in metadata['symbols'] if s['source_name'] in expected_values}
        assert set(names) == set(expected_values), names
        code = ('import program,json\nresult={}\n'
                f'for name,generated in {names!r}.items():\n'
                ' value=getattr(program,generated)()\n'
                ' assert type(value) is str\n'
                ' data=value.encode("utf-8")\n'
                ' result[name]={"hex":data.hex(),"byte_length":len(data),"text":value}\n'
                'print(json.dumps(result,ensure_ascii=True))\n')
        report['observation'] = result = command([args.python, '-B', '-s', '-c', code], cwd=out)
        succeeded(result)
        actual = json.loads(bytes.fromhex(result['stdout_hex']))
        for name, expected in expected_values.items():
            observed = actual[name]
            row = dict(id=name, actual=observed, expected_hex=expected.hex(),
                       expected_byte_length=len(expected), status='failed')
            report['values'].append(row)
            assert observed['hex'] == expected.hex() and observed['byte_length'] == len(expected), row
            assert observed['text'] == expected.decode('utf-8'), row
            row['status'] = 'passed'
        report['rejections'] = rejection_checks(args.cli, 'python', args.out)
        report['runtime_cases'] = runtime_checks(args.cli, 'python', args.out, args.python)
        report['generated_sha256'] = {name: sha(out / name) for name in ('program.py', 'langrt.py')}
        report['status'] = 'passed'
    finally:
        path = args.out / 'string-results.json'
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], observations=len(report['values']),
                          rejections=len(report['rejections']), report=str(path))))


if __name__ == '__main__':
    main()
