#!/usr/bin/env python3
"""Build all published accept projects with langc and run independent Python files."""
import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from run_cases import classify, command, sha, source_project


EXTRA_SOURCES = {
    'scalar-trait-constraint': '''
trait Ordered {func less(self Self,other Self) bool;}
impl Ordered for i32 {func less(self i32,other i32) bool {return self<other;}}
func less[T Ordered](a T,b T) bool {return a.less(b);}
func main() i32 {
    let a i32=1;let b i32=2;
    if less[i32](a,b) && !less(b,a) {return 0;}
    return 1;
}
''',
    'generic-callback': '''
func apply[T](callback func(T) T,x T) T {return callback(x);}
func identity[T]() (func(T) T) {return func(x T) T {return x;};}
func inc(x i32) i32 {return x+1;}
func main() i32 {
    let twice=func(x i32) i32 {return x*2;};
    let keep=identity[i32]();
    if apply[i32](inc,41)!=42 || apply(twice,21)!=42 || keep(7)!=7 {return 1;}
    return 0;
}
''',
    'unused-aggregate-template': '''
struct Box[T] {value T;}
enum Maybe[T] {None;Some(value T);}
func unpack[T](x T) T {
    var boxed=Box[T](value:x);
    boxed.value=x;
    let e=Maybe[T].Some(value:boxed.value);
    switch e {case Maybe.Some(value):return value;case Maybe.None:return x;}
}
func main() i32 {return 0;}
''',
}


def main():
    parser = argparse.ArgumentParser()
    for name in ('langc', 'root', 'out', 'python'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.langc = args.langc.resolve()
    args.root = args.root.resolve()
    args.out = args.out.resolve()
    args.python = args.python.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    version = command([args.python, '--version'])
    if version['returncode'] or version['stdout'].strip() != 'Python 3.12.15':
        raise RuntimeError(f'Python 3.12.15 required: {version}')
    published_file = args.root / 'tests/generics/cases.json'
    published = json.loads(published_file.read_text())
    indexed = {row['id']: row for row in published['cases']}
    immutable = [published_file] + sorted(
        p for p in (args.root / 'tests/generics/fixtures').rglob('*') if p.is_file())
    before = {str(p.relative_to(args.root)): sha(p) for p in immutable}
    # Compiler subprocesses get only failing interpreter commands in PATH.
    # This does not affect the explicit, absolute Python invocation afterwards.
    trap_bin = args.out / 'no-python'
    trap_bin.mkdir(exist_ok=True)
    for name in ('python', 'python3', 'python3.12'):
        trap = trap_bin / name
        trap.write_text('#!/bin/sh\necho unexpected-python-during-compilation >&2\nexit 118\n')
        trap.chmod(0o755)
    report = {
        'task': 'T07 Python value instances',
        'operation_id': 'language-python-pod-aggregates-20261104',
        'contract': published['contract'], 'python': version,
        'source_head': command(['git', '-C', args.root, 'rev-parse', 'HEAD'])['stdout'].strip(),
        'fixture_sha256': before, 'cases': [], 'extra': [], 'failures': [],
        'not_run': ['native execution and comparison (separate agent)',
                    'Clang ASan/UBSan (integrator)', 'ARC/foreign execution',
                    'strict floating point execution'],
    }

    def run(ident, manifest, expected, group):
        directory = args.out / 'generated' / ident
        sources = sorted(p for p in manifest.parent.rglob('*')
                         if p.is_file() and p.suffix in ('.lang', '.toml', '.json'))
        row = {
            'id': ident, 'manifest': str(manifest),
            'runtime_expected': {'main_return': expected},
            'source_sha256': {str(p.relative_to(manifest.parent)): sha(p) for p in sources},
        }
        report[group].append(row)
        for phase in ('check', 'build'):
            argv = [args.langc, phase, '--project', manifest, '--backend', 'python', '--format', 'json']
            if phase == 'build':
                argv += ['--out-dir', directory]
            row[phase] = command(['/usr/bin/env', f'PATH={trap_bin}', *argv])
            result = row[phase]
            parsed = json.loads(result['stdout'])
            if result['returncode'] or parsed['status'] != 'ok' or result['stderr']:
                row['status'] = 'failed'
                report['failures'].append(f'{ident}: {phase} failed')
                return
        row['generated_sha256'] = {name: sha(directory / name) for name in ('program.py', 'langrt.py')}
        row['execute'] = result = command([args.python, '-s', directory / 'program.py'])
        row['actual'] = classify(result)
        passed = (result['returncode'] == expected and not result['stdout'] and not result['stderr']
                  and row['actual'] in ('return', 'ordinary_nonzero'))
        row['status'] = 'passed' if passed else 'failed'
        if not passed:
            report['failures'].append(f'{ident}: unexpected generated-program behavior')

    accepts = [case for case in indexed.values() if case['expected_static']['status'] == 'accept']
    if len(accepts) != 13 or any('runtime_expected' not in case for case in accepts):
        raise RuntimeError('expected the original 13 accepts with published runtime_expected')
    for case in accepts:
        ident = case['id']
        run(ident, args.root / case['manifest'], case['runtime_expected']['main_return'], 'cases')
    for ident, body in EXTRA_SOURCES.items():
        run(ident, source_project(args.out, ident, body), 0, 'extra')
    if before != {str(p.relative_to(args.root)): sha(p) for p in immutable}:
        report['failures'].append('published generic fixture changed')
    report['status'] = 'failed' if report['failures'] else 'passed'
    report['summary'] = {group: {status: sum(row['status'] == status for row in report[group])
                                for status in ('passed', 'failed')}
                         for group in ('cases', 'extra')}
    path = args.out / 'generic-results.json'
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'status': report['status'], 'summary': report['summary'],
                      'failures': report['failures'], 'report': str(path)}, ensure_ascii=False))
    return 1 if report['failures'] else 0


if __name__ == '__main__':
    sys.exit(main())
