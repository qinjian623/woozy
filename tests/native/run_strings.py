#!/usr/bin/env python3
"""Formal CLI execution plus typed LLVM observation of generated string values."""
import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from string_cases import (STRING_FIXTURES, STRING_FIXTURE_PATHS, command,
                           rejection_checks, runtime_checks, sha, succeeded)


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'probe', 'root', 'clang', 'runtime', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--fixture', choices=STRING_FIXTURES, default='strings')
    args = parser.parse_args()
    expected_values = STRING_FIXTURES[args.fixture]
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = args.root / STRING_FIXTURE_PATHS[args.fixture] / 'lang.toml'
    report = dict(operation_id='language-constant-strings-20261104', backend='native',
                  status='failed', runs=[], rejections=[],
                  source_head=command(['git', '-C', args.root, 'rev-parse', 'HEAD']),
                  sources={str(p.relative_to(manifest.parent)): sha(p)
                           for p in sorted(manifest.parent.rglob('*')) if p.is_file()})
    try:
        report['clang'] = version = command([args.clang, '--version'])
        succeeded(version)
        assert '22.1.8' in bytes.fromhex(version['stdout_hex']).decode(), version
        for opt in (0, 2):
            out = args.out / f'O{opt}'
            row = dict(opt=opt, status='failed', values=[])
            report['runs'].append(row)
            row['check'] = result = command([args.cli, 'check', '--project', manifest,
                '--backend', 'native', '--format', 'json'])
            succeeded(result)
            row['build'] = result = command([args.cli, 'build', '--project', manifest,
                '--backend', 'native', '--opt', opt, '--out-dir', out / 'cli', '--format', 'json'])
            succeeded(result)
            row['execute'] = result = command([out / 'cli/program'])
            succeeded(result)
            assert not result['stdout_hex'], result
            row['probe'] = result = command([args.probe, manifest, out / 'probe', opt])
            succeeded(result)
            metadata = json.loads((out / 'probe/metadata.json').read_text())
            assert metadata['native_ir_rejected'] == 11 and metadata['hir_rejected'] == 1
            assert set(metadata['probes']) == set(expected_values), metadata
            row['metadata'] = metadata
            row['object_identical_to_cli'] = (out / 'cli/program.o').read_bytes() == (out / 'probe/generated.o').read_bytes()
            assert row['object_identical_to_cli'], 'helper and formal CLI generated different objects'
            row['observation_build'] = result = command([args.clang, f'-O{opt}',
                out / 'probe/observe.ll', args.runtime, '-o', out / 'observe'])
            succeeded(result)
            row['observation_execute'] = result = command([out / 'observe'])
            succeeded(result)
            data = bytes.fromhex(result['stdout_hex'])
            width = metadata['target_word_bits'] // 8
            offset = 0
            for name in metadata['probes']:
                assert offset + width <= len(data), 'missing length'
                length = int.from_bytes(data[offset:offset + width], 'little')
                offset += width
                assert offset + width <= len(data), 'missing codepoint length'
                runes = int.from_bytes(data[offset:offset + width], 'little')
                offset += width
                actual = data[offset:offset + length]
                offset += length
                value = dict(id=name, actual_hex=actual.hex(), actual_byte_length=length,
                             expected_hex=expected_values[name].hex(), expected_byte_length=len(expected_values[name]),
                             actual_length=runes, expected_length=len(expected_values[name].decode('utf-8')),
                             status='passed' if actual == expected_values[name] and
                             length == len(expected_values[name]) and
                             runes == len(expected_values[name].decode('utf-8')) else 'failed')
                row['values'].append(value)
                assert value['status'] == 'passed', value
            assert offset == len(data), 'extra observation bytes'
            row['artifacts'] = {str(p.relative_to(out)): sha(p) for p in sorted(out.rglob('*')) if p.is_file()}
            row['status'] = 'passed'
        report['rejections'] = rejection_checks(args.cli, 'native', args.out)
        report['runtime_cases'] = runtime_checks(args.cli, 'native', args.out)
        report['status'] = 'passed'
    finally:
        path = args.out / 'string-results.json'
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], observations=sum(len(r['values']) for r in report['runs']),
                          rejections=len(report['rejections']), report=str(path))))


if __name__ == '__main__':
    main()
