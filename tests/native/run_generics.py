#!/usr/bin/env python3
"""Build concrete T07 scalar instances with langc and execute the ELF files."""
import argparse
import json
from pathlib import Path
from run_cli import command, digest
from run import panic_record


SCALARS = ('explicit', 'inferred', 'return-context', 'multi-instance',
           'function-argument', 'same-recursion', 'cross-package-alias')
AGGREGATES = ('trait-unique', 'fields-reordered', 'nested-copy', 'enum-empty',
              'enum-payload', 'field-evaluation-order')


def sources(root, manifest):
    # Include vendored source packages, as well as the main source directory.
    return {str(p.relative_to(root)): digest(p)
            for p in sorted(manifest.parent.rglob('*'))
            if p.is_file() and (p.suffix == '.lang' or p.name == 'lang.toml')}


def build(args, manifest, opt, out):
    return command([args.cli, 'build', '--project', manifest, '--backend', 'native',
                    '--opt', opt, '--out-dir', out, '--format', 'json'])


def rejected(result):
    assert result['exit'] == 1 and not result['stderr'], result
    response = json.loads(result['stdout'])
    codes = [d['code'] for d in response['diagnostics']]
    assert response['status'] == 'error' and codes, response
    assert set(codes) == {'E_CAPABILITY_UNAVAILABLE'}, response


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'root', 'clang', 'lld', 'out'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    catalog = args.root / 'tests/generics/cases.json'
    published = {c['id']: c for c in json.loads(catalog.read_text())['cases']}
    report = dict(operation_id='language-native-t07-scalar-instances-20261104',
                  contract='compiler-dev/1', status='failed', runs=[], rejections=[],
                  cases_sha256=digest(catalog), tools={})
    try:
        for name in ('clang', 'lld'):
            result = command([getattr(args, name), '--version'])
            report['tools'][name] = result
            assert result['exit'] == 0 and '22.1.8' in result['stdout'], result
        cases = [(n, args.root / published[n]['manifest'],
                  published[n]['runtime_expected']['main_return']) for n in SCALARS]
        cases += [(n, args.root / f'tests/native/generics/{n}/lang.toml', expected)
                  for n, expected in (('trait-scalar', 9), ('unused-aggregate', 0),
                                      ('callback', 11))]
        for ident, manifest, expected in cases:
            for opt in (0, 2):
                out = args.out / ident / f'O{opt}'
                row = dict(id=ident, opt=opt, expected_main_return=expected,
                           source_sha256=sources(args.root, manifest), status='failed')
                report['runs'].append(row)
                row['build'] = result = build(args, manifest, opt, out)
                assert result['exit'] == 0 and not result['stderr'], result
                assert json.loads(result['stdout'])['status'] == 'ok', result
                program = out / 'program'
                data = program.read_bytes()
                row['elf_x86_64'] = (data[:4] == b'\x7fELF' and data[4:6] == b'\x02\x01'
                                        and data[18:20] == b'\x3e\x00')
                assert row['elf_x86_64'], 'wrong executable format'
                row['program_sha256'] = digest(program)
                row['object_sha256'] = digest(out / 'program.o')
                row['execute'] = ran = command([program.resolve()])
                row['panic'] = panic_record(ran)
                assert ran['exit'] == expected and not row['panic'], ran
                assert not ran['stdout'] and not ran['stderr'], ran
                row['status'] = 'passed'
        for ident in AGGREGATES:
            manifest = args.root / published[ident]['manifest']
            for opt in (0, 2):
                out = args.out / 'unsupported' / ident / f'O{opt}'
                # An empty directory permits repeated runs while detecting any
                # partial object, executable or staging file from this build.
                out.mkdir(parents=True, exist_ok=True)
                assert not list(out.iterdir()), 'unexpected rejection output'
                row = dict(id=ident, opt=opt, source_sha256=sources(args.root, manifest),
                           status='failed')
                report['rejections'].append(row)
                row['build'] = result = build(args, manifest, opt, out)
                rejected(result)
                assert not list(out.iterdir()), 'unsupported build wrote output'
                # Also check that existing successful outputs survive rejection.
                existing = args.out / 'explicit' / f'O{opt}'
                before = {p.name: digest(p) for p in existing.iterdir()}
                row['existing_output_build'] = result = build(args, manifest, opt, existing)
                rejected(result)
                assert before == {p.name: digest(p) for p in existing.iterdir()}
                row['status'] = 'unsupported'
        report['status'] = 'passed'
    finally:
        (args.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], executions=len(report['runs']),
                          unsupported=len(report['rejections']),
                          report=str(args.out / 'results.json'))))


if __name__ == '__main__':
    main()
