#!/usr/bin/env python3
"""Reproduce and verify the root's function-key and defer-depth findings."""
import argparse
import json
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True
from cleanup_regression_cases import flat_cases, key_cases
from run_cleanup import command, sha


def main():
    parser = argparse.ArgumentParser()
    for name in ('helper', 'cli', 'root', 'out', 'python'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    fixture = args.root / 'tests/python/fixtures/function-key-collision/src/main.lang'
    original = fixture.read_text()
    assert sha(fixture) == '310c4df4d72549f1743a30139ea1907b2a14833da15f9dabba16858c56060061'
    rows = []
    report = dict(operation_id='language-t08-python-execution-20261104', correction='1/1',
                  status='failed', cases=rows, failures=[],
                  source_sha256={str(p.relative_to(args.root)): sha(p)
                    for p in [args.root / 'src/backend/python/emitter.cpp',
                              args.root / 'include/lang/backend/python.hpp',
                              args.root / 'runtime/python/langrt.py', fixture,
                              Path(__file__), Path(__file__).with_name('cleanup_regression_cases.py'),
                              Path(__file__).with_name('observe_cleanup_regressions.py')]},
                  shared_test_sha256={name: sha(args.root / name)
                    for name in ('src/hir/hir.cpp', 'tests/cleanup/static_test.cpp')},
                  not_run=['final integrated CLI/ASan/LLVM-off (root)', 'native T08 (native actor)'])
    report['python'] = version = command([args.python, '--version'])
    assert version['returncode'] == 0 and version['stdout'].strip() == 'Python 3.12.15', version
    trap = args.out / 'no-python'
    trap.mkdir(exist_ok=True)
    for name in ('python', 'python3', 'python3.12'):
        file = trap / name
        file.write_text('#!/bin/sh\necho unexpected-python-during-compilation >&2\nexit 118\n')
        file.chmod(0o755)

    def compile_command(argv):
        return command(['/usr/bin/env', f'PATH={trap}', *argv])

    for ident, source, expected in [*key_cases(original), *flat_cases()]:
        directory = args.out / 'sources' / ident
        (directory / 'src').mkdir(parents=True, exist_ok=True)
        file = directory / 'src/main.lang'
        file.write_text(source)
        manifest = directory / 'lang.toml'
        manifest.write_text('schema_version=1\n[package]\nname="app"\nsource_root="src"\n'
                            'entry="app"\n[build]\nbackend="python"\nword_bits=64\n')
        generated = args.out / 'generated' / ident
        row = dict(id=ident, expected=expected, source_sha256=sha(file),
                   manifest_sha256=sha(manifest), status='failed', runtime='not_run')
        rows.append(row)
        try:
            row['check'] = checked = compile_command([args.cli, 'check', '--project', manifest,
                                                      '--backend', 'python', '--format', 'json'])
            assert checked['returncode'] == 0 and not checked['stderr'], checked
            row['emit'] = emitted = compile_command([args.helper, '--project', manifest,
                                                     '--out-dir', generated])
            data = json.loads(emitted['stdout'])
            if expected.get('unsupported'):
                assert emitted['returncode'] == 1 and not emitted['stderr'], emitted
                assert any(d['code'] == 'E_CAPABILITY_UNAVAILABLE' and d.get('primary') and
                           'generated Python' in d['message'] for d in data['diagnostics']), data
                assert not generated.exists(), 'rejected emitter wrote files'
                output = Path(tempfile.mkdtemp(prefix='cli-refused-' + ident + '-',
                                               dir=args.out)) / 'out'
                row['build'] = result = compile_command([args.cli, 'build', '--project', manifest,
                    '--backend', 'python', '--format', 'json', '--out-dir', output])
                assert result['returncode'] == 1 and not result['stderr'], result
                assert any(d['code'] == 'E_CAPABILITY_UNAVAILABLE'
                           for d in json.loads(result['stdout'])['diagnostics']), result
                assert not output.exists(), 'failed build created output'
                output.mkdir(parents=True)
                for name in ('program.py', 'langrt.py', 'program', 'program.o', 'unrelated'):
                    (output / name).write_text('preserve-' + name)
                before = {p.name: sha(p) for p in output.iterdir()}
                row['build_existing'] = result = compile_command([args.cli, 'build', '--project', manifest,
                    '--backend', 'python', '--format', 'json', '--out-dir', output])
                assert result['returncode'] == 1 and not result['stderr'], result
                row['preserved_hashes'] = {p.name: sha(p) for p in output.iterdir()}
                assert before == row['preserved_hashes'], 'failed build changed existing output'
                row['status'], row['runtime'] = 'unsupported', 'not_run (documented generated-control profile)'
                continue
            assert emitted['returncode'] == 0 and not emitted['stderr'] and data['status'] == 'ok', emitted
            row['generated_sha256'] = {name: sha(generated / name)
                for name in ('program.py', 'langrt.py', 'verified.hir')}
            # Compile the complete emitted module with locked CPython even if
            # source main would not execute a deeply nested branch.
            code = 'from pathlib import Path;import sys;compile(Path(sys.argv[1]).read_bytes(),sys.argv[1],"exec")'
            row['cpython_compile'] = compiled = command([args.python, '-B', '-s', '-c', code,
                                                         generated / 'program.py'])
            row['runner'] = ran = command([args.python, '-B', '-s', generated / 'program.py'])
            row['runtime'] = 'failed'
            assert compiled['returncode'] == 0 and not compiled['stderr'], compiled
            if 'panic' in expected:
                assert ran['returncode'] == 101 and ran['stderr'] == 'LANG_PANIC: ' + expected['panic'] + '\n', ran
            elif 'typed' in expected:
                assert ran['returncode'] == 1 and ran['stderr'] == 'LANG_TYPED_ERROR: ' + expected['typed'] + '\n', ran
            else:
                assert ran['returncode'] == (expected.get('main_return') or 0) and not ran['stderr'], ran
            assert not ran['stdout'], ran
            row['observe'] = observed = command([args.python, '-B', '-s',
                Path(__file__).with_name('observe_cleanup_regressions.py'), generated])
            assert observed['returncode'] == 0 and not observed['stderr'], observed
            row['actual'] = actual = json.loads(observed['stdout'])
            for name in ('main_return', 'typed', 'panic', 'events', 'probe_return', 'probe_log', 'probe_typed'):
                if name in expected:
                    assert actual.get(name) == expected[name], (name, actual, expected)
            if 'raised_key' in expected:
                assert actual['raised_keys'] and all(key == expected['raised_key']
                                                     for key in actual['raised_keys']), actual
            if 'main_cell' in expected:
                assert expected['main_cell'] in actual['main_locals'].values(), actual
            if 'panic' in expected:
                assert actual['probe_panic'] == expected['panic'], actual
                assert actual['probe_events'] == expected['events'], actual
            row['status'], row['runtime'] = 'passed', 'passed'
        except (AssertionError, ValueError, OSError) as error:
            row['failure'] = str(error)
            report['failures'].append(ident)
    # Shifted and original keys must stay equal despite distinct HIR identities.
    for ident, _, key in __import__('cleanup_regression_cases').FUNCTION_TYPES:
        pair = [r for r in rows if r['id'] in ('key-' + ident, 'key-' + ident + '-shifted')]
        if all(r['status'] == 'passed' for r in pair):
            assert pair[0]['actual']['typed'] == pair[1]['actual']['typed'] == 'app.Token[' + key + ']'
            assert pair[0]['generated_sha256']['verified.hir'] != pair[1]['generated_sha256']['verified.hir']
    report['status'] = 'passed' if not report['failures'] else 'failed'
    report_file = args.out / 'cleanup-regressions.json'
    report_file.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], cases=len(rows),
                         passed=sum(r['status'] == 'passed' for r in rows),
                         unsupported=sum(r['status'] == 'unsupported' for r in rows),
                         failures=report['failures'], report=str(report_file))))
    return 0 if report['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
