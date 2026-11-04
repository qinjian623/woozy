#!/usr/bin/env python3
"""Verify the public compiler CLI by executing the resulting ELF files."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from run import panic_record


def command(args):
    run = subprocess.run([str(x) for x in args], capture_output=True, text=True, timeout=30)
    return dict(argv=[str(x) for x in args], status='exited', exit=run.returncode,
                stdout=run.stdout, stderr=run.stderr)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'root', 'clang', 'lld', 'out'):
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
        cases = [(c['id'], args.root / c['manifest'], c['expected']['status']) for c in published]
        cases += [(n, args.root / f'tests/fixtures/{n}/lang.toml', 'return') for n in ('core', 'numeric')]
        for ident, manifest, expected in cases:
            for opt in (0, 2):
                out = args.out / ident / f'O{opt}'
                built = command([args.cli, 'build', '--project', manifest, '--backend', 'native',
                                 '--opt', opt, '--out-dir', out, '--format', 'json'])
                row = dict(id=ident, opt=opt, build=built)
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
                    cause = ('division-by-zero' if 'division' in ident else
                             'invalid-shift' if 'shift' in ident else 'integer-conversion')
                    assert row['panic'] and f':{cause}:' in ran['stderr'], ran
                else:
                    assert ran['exit'] == 0 and not ran['stdout'] and not ran['stderr'], ran
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
        (project / 'src/main.lang').write_text('package app; func main() i32 {return true;}')
        out = args.out / 'core/O0'
        before = {f: digest(out / f) for f in ('program', 'program.o')}
        failed = command([args.cli, 'build', '--project', manifest, '--backend', 'native', '--out-dir', out, '--format', 'json'])
        assert failed['exit'] == 1 and not failed['stderr'], failed
        assert json.loads(failed['stdout'])['diagnostics'][0]['code'] == 'E_TYPE_MISMATCH'
        assert before == {f: digest(out / f) for f in before}, 'failed build changed existing outputs'
        report['controls'].append(dict(id='source-error-preserves-output', build=failed))
        (project / 'src/main.lang').write_text('package app; func library() {}')
        no_main = command([args.cli, 'build', '--project', manifest, '--backend', 'native', '--out-dir', args.out / 'no-main', '--format', 'json'])
        assert no_main['exit'] == 1 and json.loads(no_main['stdout'])['diagnostics'][0]['code'] == 'E_CAPABILITY_UNAVAILABLE', no_main
        report['controls'].append(dict(id='no-entry', build=no_main))
        (project / 'src/main.lang').write_text('package app; func main() {}')
        blocked = args.out / 'blocked'
        (blocked / 'program').mkdir(parents=True, exist_ok=True)
        io = command([args.cli, 'build', '--project', manifest, '--backend', 'native', '--out-dir', blocked, '--format', 'json'])
        assert io['exit'] == 2 and not io['stderr'] and json.loads(io['stdout'])['diagnostics'][0]['code'] == 'E_INPUT_IO', io
        assert (blocked / 'program').is_dir() and not list(blocked.glob('.langc-native-*'))
        report['controls'].append(dict(id='io-error-cleans-stage', build=io))
        (blocked / 'program').rmdir()
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
