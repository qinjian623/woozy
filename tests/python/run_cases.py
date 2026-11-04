#!/usr/bin/env python3
"""Real frontend -> VerifiedHirModule -> C++ emitter -> explicit Python 3.12.

No .lang evaluator lives here. Extra source projects are written under --out;
all compilation goes through the C++ helper. Existing fixtures are read-only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
from datetime import datetime, timezone

PANIC_EXIT = 101
PANIC_MARKER = 'LANG_PANIC: '
CAUSES = {
    'panic-division-by-zero': 'integer division by zero',
    'panic-invalid-shift': 'invalid shift count',
    'panic-negative-shift': 'invalid shift count',
    'panic-dynamic-conversion': 'integer conversion overflow',
    'panic-remainder-zero': 'integer division by zero',
    'panic-shift-u64-count': 'invalid shift count',
    'panic-unsigned-conversion': 'integer conversion overflow',
}

EXTRA_SOURCES = {
    # Declarations alone are not executable aggregate/generic operations.
    'unused-aggregate-declaration': 'struct Box {value i32;} func main() {}',
    'unused-generic-declaration': 'func f[T](x T) T {return x;} func main() {}',
    'callee-once-before-arguments': '''
func tick(x inout i32) i32 { let old=x; x+=1; return old; }
func pair(a i32,b i32) i32 {return a*10+b;}
func choose(x inout i32) (func(i32,i32) i32) {x+=1; return pair;}
func main() i32 {
    var counter i32=0;
    let value=choose(inout counter)(tick(inout counter),tick(inout counter));
    if value!=12 || counter!=3 {return 1;}
    return 0;
}''',
    'lambda-recursion-discard': '''
const answer i32=42;
func even(x i32) bool {if x==0 {return true;} return odd(x-1);}
func odd(x i32) bool {if x==0 {return false;} return even(x-1);}
func factory() (func(i32) i32) {return func(x i32) i32 {return x*2;};}
func ignored(_ i32,_ i32) {}
func apply(f func(i32) i32,x i32) i32 {return f(x);}
func main() i32 {
    ignored(1,2);
    let discard=func(_ i32,_ i32) {};
    discard(3,4);
    let result=func(x i32) i32 {return x+1;}(41);
    let nested=func() (func(i32) i32) {return func(x i32) i32 {return x;};};
    if result!=answer || factory()(21)!=42 || nested()(17)!=17 {return 1;}
    if apply(func(x i32) i32 {return x+2;},40)!=42 {return 2;}
    if !even(8) || !odd(9) {return 3;}
    return 0;
}''',
    'inout-identity-and-copies': '''
func bump(x inout i32) {x+=1;}
func relay(x inout i32) {bump(inout x);}
func copied(x i32) i32 {var local=x; relay(inout local); return local;}
func flip(x inout bool) {x=!x;}
func mutate_then_panic(x inout i32) {x+=1;var zero i32=0;let result=x/zero;}
func main() i32 {
    var x i32=3;
    if copied(x)!=4 || x!=3 {return 1;}
    relay(inout x);
    if x!=4 {return 2;}
    let f=func(x inout i32) {bump(inout x);};
    f(inout x);
    var b bool=false;
    flip(inout b);
    if x!=5 || !b {return 3;}
    return 0;
}''',
    'short-circuit-effects': '''
func hit(x inout i32) bool {x+=1;return true;}
func main() i32 {
    var x i32=0;
    let a=false && hit(inout x);
    let b=true || hit(inout x);
    let c=true && hit(inout x);
    let d=false || hit(inout x);
    let e=(false && hit(inout x)) || (true && hit(inout x));
    if a || !b || !c || !d || !e || x!=3 {return 1;}
    return 0;
}''',
    'nested-loop-step-and-early-return': '''
func early() i32 {for {for {return 42;}}}
func main() i32 {
    var total i32=0;
    var steps i32=0;
    for var outer i32=0;outer<3;outer+=1 {
        for var inner i32=0;inner<5;inner+=1 {
            if inner==1 {continue;}
            if inner==3 {break;}
            total+=inner;
        }
        steps+=1;
        if outer==1 {continue;}
        total+=10;
    }
    if total!=26 || steps!=3 || early()!=42 {return 1;}
    return 0;
}''',
    'unit-main-and-variable-function': '''
func ping() {}
func pong() {}
func main() {
    var f=ping;
    f();
    f=pong;
    f();
    if true {} else {}
}''',
    'panic-remainder-zero': '''
func main() i32 {var a i64=-9223372036854775808;var b i64=0;let x=a%b;return 0;}
''',
    'panic-shift-u64-count': '''
func main() i32 {var x u8=1;var n u64=18446744073709551615;let y=x>>n;return 0;}
''',
    'panic-unsigned-conversion': '''
func main() i32 {var x i64=-1;let y=u64(x);return 0;}
''',
    'ordinary-nonzero': 'func main() i32 {return 101;}',
    'ordinary-other-nonzero': 'func main() i32 {var x i32=1;return x+6;}',
    'python-keywords': '''
func with(def i32,lambda i32) i32 {return def+lambda;}
func main() i32 {
    let __name__ i32=40;
    let _rt i32=2;
    if with(lambda:_rt,def:__name__)!=42 {return 1;}
    return 0;
}''',
}

UNSUPPORTED_SOURCES = {
    'float-arithmetic': 'func main() i32 {var x f32=1.0;let y=x+x;return 0;}',
    'float-comparison': 'func main() i32 {var x f64=1.0;let y=x==x;return 0;}',
    'ordinary-parameter-rebinding': 'func f(x i32) {x=2;} func main() {}',
    'string': 'func main() {let x="hello";}',
    'vec': 'func main() {let x=[1,2];}',
    'defer': 'func main() {defer {}}',
    'throws': 'func main() throws i32 {}',
    'capturing-lambda': 'func main() {let x i32=1;let f=func() i32 {return x;};}',
}


def command(argv, **kwargs):
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    # Avoid inherited import hooks changing generated-program behavior.
    env.pop('PYTHONPATH', None)
    env.pop('PYTHONOPTIMIZE', None)
    p = subprocess.run([str(a) for a in argv], text=True, capture_output=True,
                       timeout=30, env=env, **kwargs)
    return {'command': [str(a) for a in argv], 'returncode': p.returncode,
            'stdout': p.stdout, 'stderr': p.stderr}


def classify(result):
    if (result['returncode'] == PANIC_EXIT and not result['stdout'] and
            result['stderr'].startswith(PANIC_MARKER) and
            len(result['stderr'].splitlines()) == 1):
        return 'panic'
    if result['returncode'] == 0:
        return 'return'
    if 'SyntaxError:' in result['stderr']:
        return 'syntax_error'
    if 'Traceback (most recent call last):' in result['stderr']:
        return 'python_exception'
    return 'ordinary_nonzero'


def source_project(out, ident, body, *, bits=64):
    root = out / 'sources' / ident
    (root / 'src').mkdir(parents=True, exist_ok=True)
    (root / 'src/main.lang').write_text('package app;\n' + body + '\n')
    (root / 'lang.toml').write_text(
        'schema_version = 1\n[package]\nname = "python_test"\n'
        'source_root = "src"\nentry = "app"\n[build]\n'
        f'backend = "python"\nword_bits = {bits}\n')
    return root / 'lang.toml'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def integer_width_project():
    statements = []
    error = 1
    for signed in (True, False):
        for typ, bits in [(('i' if signed else 'u') + str(n), n) for n in (8, 16, 32, 64)] + [
                ('isize' if signed else 'usize', 64)]:
            lower = -(1 << (bits - 1)) if signed else 0
            upper = (1 << (bits - int(signed))) - 1
            statements.append(f'{{var high {typ}={upper};var low {typ}={lower};'
                              f'if high+1!={lower} || low-1!={upper} {{return {error};}}')
            if signed:
                statements.append(f'var neg {typ}=-1;if low/neg!={lower} || low%neg!=0 '
                                  f'{{return {error + 1};}}')
            statements.append('}')
            error += 2
    statements += ['var a i64=9223372036854775807;var b i64=3;',
                   f'if a/b!=3074457345618258602 || a%b!=1 {{return {error};}}',
                   'var u u64=18446744073709551615;var two u64=2;',
                   f'if u/two!=9223372036854775807 || u%two!=1 {{return {error + 1};}}',
                   'return 0;']
    return 'func main() i32 {' + '\n'.join(statements) + '}'


# Direct target-precision literals and value/inout transmission. Float
# observations use struct.pack on generated functions, never source evaluation.
FLOAT_LITERALS = {
    'zero32': ('f32', '0.0', '00000000'),
    'one32': ('f32', '1.0', '3f800000'),
    'tie32': ('f32', '1.000000059604644775390625', '3f800000'),
    'sub32': ('f32', '1.401298464324817070923729583289916131280e-45', '00000001'),
    'under32': ('f32', '1e-100', '00000000'),
    'over32': ('f32', '1e100', '7f800000'),
    'zero64': ('f64', '0.0', '0000000000000000'),
    'one64': ('f64', '1.0', '3ff0000000000000'),
    'tie64': ('f64', '1.00000000000000011102230246251565404236316680908203125', '3ff0000000000000'),
    'sub64': ('f64', '4.940656458412465441765687928682213723651e-324', '0000000000000001'),
    'under64': ('f64', '1e-1000', '0000000000000000'),
    'over64': ('f64', '1e1000', '7ff0000000000000'),
}


def float_project():
    parts = ['func pass32(x f32) f32 {return x;}',
             'func pass64(x f64) f64 {return x;}',
             'func set32(x inout f32) {x=1.5;}',
             'func set64(x inout f64) {x=1.5;}',
             'func probe32() f32 {var x f32=0.0;set32(inout x);return x;}',
             'func probe64() f64 {var x f64=0.0;set64(inout x);return x;}']
    for ident, (typ, literal, _) in FLOAT_LITERALS.items():
        bits = typ[1:]
        parts.append(f'func {ident}() {typ} {{let x {typ}={literal};'
                     f'var copy=x;copy=pass{bits}(copy);return copy;}}')
    parts.append('func main() i32 {return 0;}')
    return '\n'.join(parts)


def main():
    parser = argparse.ArgumentParser()
    for arg in ('helper', 'root', 'out', 'python'):
        parser.add_argument('--' + arg, type=Path, required=True)
    args = parser.parse_args()
    args.helper = args.helper.resolve()
    args.root = args.root.resolve()
    args.python = args.python.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    version = command([args.python, '--version'])
    if version['returncode'] or not version['stdout'].startswith('Python 3.12.'):
        raise RuntimeError(f'Python 3.12 required: {version}')
    cases_file = args.root / 'tests/backend_common/cases.json'
    published = json.loads(cases_file.read_text())
    if published['case_count'] != 15 or len(published['cases']) != 15:
        raise RuntimeError('expected all 15 published backend cases')
    immutable = [cases_file]
    for case in published['cases']:
        manifest = args.root / case['manifest']
        immutable.extend([manifest, manifest.parent / 'src/main.lang'])
    for ident in ('core', 'numeric'):
        immutable.extend([args.root / f'tests/fixtures/{ident}/lang.toml',
                          args.root / f'tests/fixtures/{ident}/src/main.lang'])
    before = {str(path): sha(path) for path in immutable}
    report = {'task': 'T06', 'operation_id': 'language-t06-python-scalars-20261103',
              'contract': 'compiler-dev/1', 'profile': 'python64',
              'checked_at': datetime.now(timezone.utc).isoformat(),
              'python': version, 'platform': platform.platform(),
              'source_head': command(['git', '-C', args.root, 'rev-parse', 'HEAD'])['stdout'].strip(),
              'fixture_sha256': before, 'cases': [], 'extra': [], 'negative_controls': [],
              'unsupported': [], 'failures': [],
              'not_run': ['official build CLI integration (integrator)',
                          'native backend comparison (T05)', 'typed throws/defer (T08)',
                          'ARC/leak checks (T09)', 'foreign NumPy/PyTorch (T10)']}

    def compile_case(ident, manifest, group, expected='return', status=0):
        directory = args.out / 'generated' / ident
        built = command([args.helper, '--project', manifest, '--out-dir', directory])
        row = {'id': ident, 'manifest': str(manifest), 'expected': expected,
               'expected_returncode': PANIC_EXIT if expected == 'panic' else status, 'compile': built,
               'pipeline': 'load_project -> resolve_project -> check_project -> hir::verify -> python::emit'}
        report[group].append(row)
        if built['returncode']:
            row['status'] = 'failed'
            report['failures'].append(ident + ': frontend/emitter failed')
            return None
        result = command([args.python, '-s', directory / 'program.py'])
        row.update(execute=result, actual=classify(result))
        row['generated_sha256'] = {n: sha(directory / n) for n in ('program.py', 'langrt.py', 'verified.hir', 'metadata.json')}
        if expected == 'panic':
            passed = (row['actual'] == 'panic' and
                      result['stderr'] == PANIC_MARKER + CAUSES[ident] + '\n')
        else:
            passed = (row['actual'] in ('return', 'ordinary_nonzero') and
                      result['returncode'] == status and not result['stdout'] and not result['stderr'])
        row['status'] = 'passed' if passed else 'failed'
        if not passed:
            report['failures'].append(ident + ': unexpected generated-program behavior')
        return directory

    for case in published['cases']:
        compile_case(case['id'], args.root / case['manifest'], 'cases', case['expected']['status'])
        report['cases'][-1]['published_expected'] = case['expected']
    for ident in ('core', 'numeric'):
        compile_case(ident, args.root / f'tests/fixtures/{ident}/lang.toml', 'cases')
    sources = dict(EXTRA_SOURCES, **{'all-integer-widths': integer_width_project(),
                                    'float-bits-and-transmission': float_project()})
    generated = {}
    for ident, body in sources.items():
        expected = 'panic' if ident in CAUSES else 'return'
        status = 101 if ident == 'ordinary-nonzero' else 7 if ident == 'ordinary-other-nonzero' else 0
        group = 'negative_controls' if status else 'extra'
        generated[ident] = compile_case(ident, source_project(args.out, ident, body), group, expected, status)

    module_manifest = source_project(args.out, 'cross-module', '''
import "app/math";
func main() i32 {if math.twice(21)!=42 {return 1;}return 0;}
''')
    math_dir = module_manifest.parent / 'src/math'
    math_dir.mkdir(parents=True, exist_ok=True)
    (math_dir / 'value.lang').write_text('package math; pub func twice(x i32) i32 {return x*2;}\n')
    (module_manifest.parent / 'src/other.lang').write_text('package app; func unused() {}\n')
    compile_case('cross-module', module_manifest, 'extra')

    # Actual generated module + runtime, with an external harness injecting a
    # host bug. This must remain a Python traceback and cannot count as Panic.
    normal = generated['ordinary-other-nonzero']
    if normal:
        entry = json.loads((normal / 'metadata.json').read_text())['entry_symbol']
        injected = command([args.python, '-s', '-c',
            'import program, langrt\n'
            'def host_bug(*args):\n    raise RuntimeError("controlled host bug")\n'
            f'langrt.binary=host_bug\nlangrt.run(program._f{entry})'], cwd=normal)
        passed = (classify(injected) == 'python_exception' and injected['returncode'] == 1 and
                  'RuntimeError: controlled host bug' in injected['stderr'] and PANIC_MARKER not in injected['stderr'])
        report['negative_controls'].append({'id': 'non-panic-python-exception', 'execute': injected,
                                             'actual': classify(injected),
                                             'status': 'passed' if passed else 'failed'})
        if not passed: report['failures'].append('host Python exception misclassified')
        syntax = command([args.python, '-s', '-c', 'def broken('], cwd=normal)
        passed = classify(syntax) != 'panic' and 'SyntaxError' in syntax['stderr']
        report['negative_controls'].append({'id': 'python-syntax-error', 'execute': syntax,
                                             'actual': classify(syntax),
                                             'status': 'passed' if passed else 'failed'})
        if not passed: report['failures'].append('syntax error misclassified')
        controls = command([args.python, '-s', '-c',
            "import langrt; assert issubclass(langrt.Panic, BaseException); "
            "assert not issubclass(langrt.Panic, Exception)"], cwd=normal)
        report['negative_controls'].append({'id': 'panic-outside-exception', 'execute': controls,
                                             'status': 'passed' if controls['returncode'] == 0 else 'failed'})
        if controls['returncode']: report['failures'].append('Panic Exception inheritance')

    inout = generated['inout-identity-and-copies']
    if inout:
        symbols = json.loads((inout / 'metadata.json').read_text())['symbols']
        mutate = next(s['generated'] for s in symbols if s['source_name'] == 'mutate_then_panic')
        observation = command([args.python, '-s', '-c',
            'import program, langrt\nx=langrt.Cell(4)\n'
            f'try:\n    program.{mutate}(x)\n'
            'except langrt.Panic:\n    assert x.value==5\n'
            'else:\n    raise AssertionError("missing Panic")'], cwd=inout)
        report['extra'].append({'id': 'inout-mutation-before-panic', 'execute': observation,
                                'status': 'passed' if observation['returncode'] == 0 else 'failed'})
        if observation['returncode']: report['failures'].append('inout copies back only on normal return')

    floats = generated['float-bits-and-transmission']
    if floats:
        metadata = json.loads((floats / 'metadata.json').read_text())
        functions = {s['source_name']: s['generated'] for s in metadata['symbols'] if s['generated'].startswith('_f')}
        probes = ["import program, struct"]
        checks = dict(FLOAT_LITERALS, probe32=('f32', '', '3fc00000'), probe64=('f64', '', '3ff8000000000000'))
        for ident, (typ, _, hex_bits) in checks.items():
            fmt = '>f' if typ == 'f32' else '>d'
            probes.append(f"assert struct.pack('{fmt}', program.{functions[ident]}()).hex() == '{hex_bits}', '{ident}'")
        observation = command([args.python, '-s', '-c', '\n'.join(probes)], cwd=floats)
        report['extra'].append({'id': 'float-exact-bits-observed', 'execute': observation,
                                'checks': {k: v[2] for k, v in checks.items()},
                                'status': 'passed' if observation['returncode'] == 0 else 'failed'})
        if observation['returncode']: report['failures'].append('float bits/transmission')

    foreign = source_project(args.out, 'unsupported-foreign',
                             'import "py" as p;func main() {p.run();}')
    with foreign.open('a') as file:
        file.write('[dependencies.py]\nkind="python"\nbinding="binding.json"\nhost_package="host"\n')
    foreign.parent.joinpath('binding.json').write_text(json.dumps({
        'schema_version': 1, 'contract': 'compiler-dev/1',
        'package_requirements': [{'package': 'host'}],
        'modules': [{'logical_path': 'py', 'host_module': 'host'}],
        'types': [], 'values': [], 'callables': [
            {'module': 'py', 'name': 'run', 'call_kind': 'function', 'id': 'host.run'}]}))
    unavailable = [(ident, source_project(args.out, 'unsupported-' + ident, body))
                   for ident, body in UNSUPPORTED_SOURCES.items()] + [('foreign', foreign)]
    for ident, manifest in unavailable:
        directory = args.out / 'generated' / ('unsupported-' + ident)
        result = command([args.helper, '--project', manifest, '--out-dir', directory])
        parsed = json.loads(result['stdout'])
        source = manifest.parent / 'src/main.lang'
        passed = (result['returncode'] == 1 and parsed['status'] == 'error' and
                  any(d['code'] == 'E_CAPABILITY_UNAVAILABLE' and d['primary'] is not None and
                      Path(d['primary']['path']).resolve() == source.resolve() and
                      0 <= d['primary']['start_byte'] < d['primary']['end_byte'] <= source.stat().st_size
                      for d in parsed['diagnostics']) and not directory.exists())
        report['unsupported'].append({'id': ident, 'compile': result,
                                      'status': 'unsupported' if passed else 'failed'})
        if not passed: report['failures'].append('unsupported diagnostic: ' + ident)

    # The checker supports Python/32, but this emitter's first profile is 64.
    width32 = source_project(args.out, 'profile32', 'func main() {}', bits=32)
    rejected = command([args.helper, '--project', width32, '--out-dir', args.out / 'generated/profile32'])
    parsed = json.loads(rejected['stdout'])
    passed = (rejected['returncode'] == 1 and any(d['phase'] == 'codegen' and
              d['code'] == 'E_CAPABILITY_UNAVAILABLE' for d in parsed['diagnostics']))
    report['unsupported'].append({'id': 'python32-profile', 'compile': rejected,
                                  'status': 'unsupported' if passed else 'failed'})
    if not passed: report['failures'].append('Python32 emitter diagnostic')
    if before != {str(path): sha(path) for path in immutable}:
        report['failures'].append('published fixture changed')
    report['status'] = 'failed' if report['failures'] else 'passed'
    report['summary'] = {group: {status: sum(row['status'] == status for row in report[group])
                                  for status in ('passed', 'failed', 'unsupported')}
                         for group in ('cases', 'extra', 'negative_controls', 'unsupported')}
    (args.out / 'results.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    for group in ('cases', 'extra', 'negative_controls', 'unsupported'):
        for row in report[group]: print(f"{group}/{row['id']}: {row['status']}")
    print(json.dumps({'status': report['status'], 'summary': report['summary'],
                      'failures': report['failures'], 'report': str(args.out / 'results.json')}, ensure_ascii=False))
    return 1 if report['failures'] else 0


if __name__ == '__main__':
    sys.exit(main())
