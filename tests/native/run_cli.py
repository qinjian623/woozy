#!/usr/bin/env python3
"""Verify the public compiler CLI by executing the resulting ELF files."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from run import panic_record


def command(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE):
    run = subprocess.run([str(x) for x in args], stdout=stdout, stderr=stderr,
                         text=True, timeout=30)
    return dict(argv=[str(x) for x in args], status='exited', exit=run.returncode,
                stdout=run.stdout, stderr=run.stderr)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'root', 'clang', 'lld', 'out', 'helper', 'invalid-json-cli'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(status='failed', runs=[], controls=[], tools={})
    try:
        for name in ('clang', 'lld'):
            tool = command([getattr(args, name), '--version'])
            report['tools'][name] = tool
            assert tool['exit'] == 0 and '22.1.8' in tool['stdout'], tool
        published = json.loads((args.root / 'tests/backend_common/cases.json').read_text())['cases']
        cases = [(c['id'], args.root / c['manifest'], c['expected']['status'],
                  c['expected'].get('main_return', 0)) for c in published]
        cases += [(n, args.root / f'tests/fixtures/{n}/lang.toml', 'return', 0) for n in ('core', 'numeric')]
        cases += [(n, args.root / f'tests/modules/fixtures/{p}/lang.toml', 'return', 0)
                  for n, p in (('mutual-recursion', 'same-package-mutual-recursion'),
                               ('cross-package-call', 'cross-package-public'))]
        cases += [(p.parent.name, p, 'panic' if p.parent.name.startswith('panic-') else 'return',
                   7 if p.parent.name == 'nonzero-main' else 0)
                  for p in sorted((args.root / 'tests/native/fixtures').glob('*/lang.toml'))]
        full = Path('/dev/full')
        assert full.exists(), 'output-write controls require /dev/full'
        helper_root = args.out / 'helper'
        help_ok = command([args.helper, '--help'])
        assert help_ok['exit'] == 0 and not help_ok['stderr'], help_ok
        assert help_ok['stdout'].startswith('lang_native_helper --project'), help_ok
        with full.open('wb') as sink:
            help_full = command([args.helper, '--help'], stdout=sink)
        assert help_full['exit'] == 2 and not help_full['stderr'], help_full
        report['controls'].append(dict(id='helper-help-output-error',
                                       normal=help_ok, failed_write=help_full))
        valid_manifest = args.root / 'tests/fixtures/core/lang.toml'
        object_path = helper_root / 'metadata' / 'program.o'
        object_path.parent.mkdir(parents=True, exist_ok=True)
        metadata = command([args.helper, '--project', valid_manifest, '--out-object',
                            object_path, '--format', 'json'])
        assert metadata['exit'] == 0 and not metadata['stderr'], metadata
        metadata_json = json.loads(metadata['stdout'])
        assert metadata_json['status'] == 'passed' and metadata_json['phase'] == 'object', metadata
        assert object_path.read_bytes()[:4] == b'\x7fELF'
        full_object = helper_root / 'metadata-full' / 'program.o'
        full_object.parent.mkdir(parents=True, exist_ok=True)
        with full.open('wb') as sink:
            metadata_full = command([args.helper, '--project', valid_manifest,
                                     '--out-object', full_object, '--format', 'json'],
                                    stdout=sink)
        assert metadata_full['exit'] == 2 and not metadata_full['stderr'], metadata_full
        assert full_object.read_bytes()[:4] == b'\x7fELF'
        report['controls'].append(dict(id='helper-metadata-output-error',
                                       normal=metadata, failed_write=metadata_full))
        normal_out = helper_root / 'executable'
        normal = command([args.helper, '--project', valid_manifest, '--out-dir',
                          normal_out, '--format', 'json'])
        assert normal['exit'] == 0 and not normal['stderr'], normal
        assert json.loads(normal['stdout'])['status'] == 'ok', normal
        normal_program = normal_out / 'program'
        normal_run = command([normal_program.resolve()])
        assert normal_run['exit'] == 0 and not normal_run['stdout'] and not normal_run['stderr'], normal_run
        full_out = helper_root / 'executable-full'
        with full.open('wb') as sink:
            full_build = command([args.helper, '--project', valid_manifest, '--out-dir',
                                  full_out, '--format', 'json'], stdout=sink)
        assert full_build['exit'] == 2 and not full_build['stderr'], full_build
        full_program = full_out / 'program'
        full_run = command([full_program.resolve()])
        assert full_run['exit'] == 0 and not full_run['stdout'] and not full_run['stderr'], full_run
        report['controls'].append(dict(id='helper-output-error-keeps-published-elf',
                                       normal=normal, failed_write=full_build,
                                       normal_sha256=digest(normal_program),
                                       full_sha256=digest(full_program)))
        for ident, manifest, expected, main_return in cases:
            for opt in (0, 2):
                out = args.out / ident / f'O{opt}'
                built = command([args.cli, 'build', '--project', manifest, '--backend', 'native',
                                 '--opt', opt, '--out-dir', out, '--format', 'json'])
                row = dict(id=ident, opt=opt, build=built, expected_main_return=main_return,
                           source_sha256={str(p.relative_to(args.root)): digest(p)
                                          for p in sorted(manifest.parent.rglob('*'))
                                          if p.is_file() and (p.suffix == '.wz' or p.name == 'lang.toml')})
                report['runs'].append(row)
                assert built['exit'] == 0 and not built['stderr'], built
                assert json.loads(built['stdout'])['status'] == 'ok', built
                program = out / 'program'
                data = program.read_bytes()
                assert data[:4] == b'\x7fELF' and data[4] == 2 and data[18:20] == b'\x3e\x00'
                row['sha256'] = digest(program)
                row['execute'] = ran = command([program.resolve()])
                row['panic'] = panic_record(ran)
                if expected == 'panic':
                    cause = ('division-by-zero' if 'division' in ident or 'remainder' in ident else
                             'invalid-shift' if 'shift' in ident else 'integer-conversion')
                    assert row['panic'] and f':{cause}:' in ran['stderr'], ran
                else:
                    assert ran['exit'] == main_return and not row['panic'] and not ran['stdout'] and not ran['stderr'], ran
                row['status'] = 'passed'
        for opt in (0, 2):
            out = args.out / 'ordinary-return' / f'O{opt}'
            built = command([args.cli, 'build', '--project', args.root / 'tests/native/fixtures/nonzero-main/lang.toml',
                             '--backend', 'native', '--opt', opt, '--out-dir', out, '--format', 'json'])
            assert built['exit'] == 0, built
            ran = command([(out / 'program').resolve()])
            assert ran['exit'] == 7 and not panic_record(ran) and not ran['stdout'] and not ran['stderr'], ran
            report['controls'].append(dict(id='ordinary-return7', opt=opt, build=built, execute=ran))
        project = args.out / 'invalid-project'
        (project / 'src').mkdir(parents=True, exist_ok=True)
        manifest = project / 'lang.toml'
        manifest.write_text('schema_version=1\n[package]\nname="cli"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
        (project / 'src/main.wz').write_text('package app; func main() i32 {return true;}')
        out = args.out / 'core/O0'
        before = {f: digest(out / f) for f in ('program', 'program.o')}
        failed = command([args.cli, 'build', '--project', manifest, '--backend', 'native', '--out-dir', out, '--format', 'json'])
        assert failed['exit'] == 1 and not failed['stderr'], failed
        assert json.loads(failed['stdout'])['diagnostics'][0]['code'] == 'E_TYPE_MISMATCH'
        assert before == {f: digest(out / f) for f in before}, 'failed build changed existing outputs'
        report['controls'].append(dict(id='source-error-preserves-output', build=failed))
        helper_source = helper_root / 'source-error'
        source_normal = command([args.helper, '--project', manifest, '--out-dir',
                                 helper_source, '--format', 'json'])
        assert source_normal['exit'] == 1 and not source_normal['stderr'], source_normal
        assert json.loads(source_normal['stdout'])['diagnostics'][0]['code'] == 'E_TYPE_MISMATCH'
        with full.open('wb') as sink:
            source_full = command([args.helper, '--project', manifest, '--out-dir',
                                   helper_root / 'source-error-full', '--format', 'json'],
                                  stdout=sink)
        assert source_full['exit'] == 2 and not source_full['stderr'], source_full
        with full.open('wb') as err:
            source_stderr_full = command([args.helper, '--project', manifest, '--out-dir',
                                          helper_root / 'source-error-stderr-full'], stderr=err)
        assert source_stderr_full['exit'] == 2 and not source_stderr_full['stdout'], source_stderr_full
        report['controls'].append(dict(id='helper-source-error-output-error',
                                       normal=source_normal, failed_write=source_full,
                                       stderr_failed_write=source_stderr_full))
        invalid_json = command([args.invalid_json_cli, 'build', '--project', manifest,
                                '--backend', 'native', '--out-dir',
                                args.out / 'invalid-helper-json', '--format', 'json'])
        assert invalid_json['exit'] == 3 and not invalid_json['stderr'], invalid_json
        invalid_diagnostic = json.loads(invalid_json['stdout'])['diagnostics'][0]
        assert invalid_diagnostic['code'] == 'E_INTERNAL_ERROR', invalid_json
        assert 'invalid native helper diagnostics' in invalid_diagnostic['message'], invalid_json
        assert 'helper exit code 2' in invalid_diagnostic['message'], invalid_json
        report['controls'].append(dict(id='driver-invalid-helper-json-exit-context',
                                       build=invalid_json))
        (project / 'src/main.wz').write_text('package app; func library() {}')
        no_main = command([args.cli, 'build', '--project', manifest, '--backend', 'native', '--out-dir', args.out / 'no-main', '--format', 'json'])
        assert no_main['exit'] == 1 and json.loads(no_main['stdout'])['diagnostics'][0]['code'] == 'E_CAPABILITY_UNAVAILABLE', no_main
        report['controls'].append(dict(id='no-entry', build=no_main))
        (project / 'src/main.wz').write_text('package app; func main() {}')
        blocked = args.out / 'blocked'
        (blocked / 'program').mkdir(parents=True, exist_ok=True)
        # Clear only files owned by this repeated failure test.
        (blocked / 'program.o').unlink(missing_ok=True)
        (blocked / 'unrelated').unlink(missing_ok=True)
        io = command([args.cli, 'build', '--project', manifest, '--backend', 'native', '--out-dir', blocked, '--format', 'json'])
        assert io['exit'] == 2 and not io['stderr'] and json.loads(io['stdout'])['diagnostics'][0]['code'] == 'E_INPUT_IO', io
        assert (blocked / 'program').is_dir() and not list(blocked.glob('.woozyc-native-*'))
        report['controls'].append(dict(id='io-error-cleans-stage', build=io))
        assert not (blocked / 'program.o').exists()
        (blocked / 'program.o').write_bytes(b'old-object-sentinel')
        (blocked / 'unrelated').write_bytes(b'keep')
        io = command([args.cli, 'build', '--project', manifest, '--backend', 'native', '--out-dir', blocked, '--format', 'json'])
        assert io['exit'] == 2 and not io['stderr'] and json.loads(io['stdout'])['diagnostics'][0]['code'] == 'E_INPUT_IO', io
        assert (blocked / 'program.o').read_bytes() == b'old-object-sentinel'
        assert (blocked / 'unrelated').read_bytes() == b'keep'
        assert (blocked / 'program').is_dir() and not list(blocked.glob('.woozyc-*'))
        (blocked / 'program').rmdir()
        (blocked / 'program.o').unlink()
        (blocked / 'unrelated').unlink()
        deps = command(['ldd', args.cli])
        assert deps['exit'] == 0 and 'libLLVM' not in deps['stdout'] and 'libpython' not in deps['stdout'], deps
        report['frontend_dependencies'] = deps
        report['status'] = 'passed'
    finally:
        (args.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], executions=len(report['runs']) + 2,
                          report=str(args.out / 'results.json'))))


if __name__ == '__main__':
    main()
