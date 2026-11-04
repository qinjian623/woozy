#!/usr/bin/env python3
"""Real CLI value programs, independent execution and unsupported-operation checks."""
import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from run_cases import classify, command, sha, source_project


# These four sources are unchanged from the old aggregate rejection tests.
# The field-read program really returns 1; successful execution need not be 0.
ORIGINAL = {
    'aggregate-construction': ('struct Box {value i32;} func main() {let b=Box(value:1);}', 0),
    'aggregate-field-read': ('struct Box {value i32;} func main() i32 {let b=Box(value:1);return b.value;}', 1),
    'aggregate-field-write': ('struct Box {value i32;} func main() {var b=Box(value:1);b.value=2;}', 0),
    'enum-switch': ('enum E {A;B;} func main() i32 {switch E.A {case E.A:return 0;case E.B:return 1;}}', 0),
}
SOURCES = {
    'value-parameters-return': '''
struct P {x i32;y i32;}
struct O {p P;other P;}
func pass(x O) O {return x;}
func edited(x O) O {var y=x;y.p.x+=10;y.other=y.p;return y;}
func main() i32 {
    var a=O(p:P(x:1,y:2),other:P(x:3,y:4));
    let b=pass(a);var c=edited(a);let saved=c;
    a.p.x=40;c.p.y=90;
    if b.p.x!=1 || b.p.y!=2 || b.other.x!=3 {return 1;}
    if saved.p.x!=11 || saved.p.y!=2 || saved.other.y!=2 {return 2;}
    if c.other.x!=11 || c.other.y!=2 || a.other.y!=4 {return 3;}
    return 0;
}''',
    'nested-fields-compound': '''
struct P {x i32;y i32;}
struct O {a P;b P;}
func main() i32 {
    var p=O(a:P(x:2,y:3),b:P(x:5,y:7));let old=p;
    p.a.x+=p.b.x;p.b=p.a;p.a.x<<=1;
    if p.a.x!=14 || p.a.y!=3 || p.b.x!=7 || p.b.y!=3 {return 1;}
    if old.a.x!=2 || old.a.y!=3 || old.b.x!=5 || old.b.y!=7 {return 2;}
    return 0;
}''',
    'function-field-callee-once': '''
struct H {f func(i32,i32) i32;}
func tick(n inout i32) i32 {let old=n;n+=1;return old;}
func pair(a i32,b i32) i32 {return a*10+b;}
func sum(a i32,b i32) i32 {return a+b;}
func choose(n inout i32) H {n+=1;return H(f:pair);}
func main() i32 {
    var n i32=0;
    let value=choose(inout n).f(tick(inout n),tick(inout n));
    var h=H(f:pair);let old=h;h.f=sum;
    if value!=12 || n!=3 || h.f(2,3)!=5 || old.f(2,3)!=23 {return 1;}
    return 0;
}''',
    'constructor-source-order': '''
struct P {x i32;y i32;}
struct H {f func(i32) i32;p P;}
func tick(n inout i32) i32 {n+=1;return n;}
func identity(x i32) i32 {return x;}
func callback(n inout i32) (func(i32) i32) {n+=1;return identity;}
func main() i32 {
    var n i32=0;
    let h=H(p:P(y:tick(inout n),x:tick(inout n)),f:callback(inout n));
    if h.p.x!=2 || h.p.y!=1 || h.f(4)!=4 || n!=3 {return 1;}
    return 0;
}''',
    'enum-source-order': '''
enum E {Empty;Pair(x i32,y i32);}
func tick(n inout i32) i32 {n+=1;return n;}
func main() i32 {
    var n i32=0;let e=E.Pair(y:tick(inout n),x:tick(inout n));
    switch e {case E.Empty:return 1;case E.Pair(x,y):
        if x!=2 || y!=1 || n!=2 {return 2;}}
    return 0;
}''',
    'enum-payload-copy-and-return': '''
struct P {x i32;y i32;}
enum E {Empty;Single(x i32);Pair(p P,f func(i32) i32);}
func inc(x i32) i32 {return x+1;}
func pass(e E) E {return e;}
func read(e E) i32 {
    switch e {case E.Empty:return 3;case E.Single(x):return x;
        case E.Pair(p,f):var local=p;local.x+=10;return f(local.x)+p.y;}
}
func main() i32 {
    var p=P(x:4,y:2);var e=E.Pair(p:p,f:inc);let old=pass(e);
    p.x=50;e=E.Empty;
    if read(old)!=17 || read(e)!=3 || read(E.Single(x:9))!=9 {return 1;}
    switch old {case E.Pair(p,_):if p.x!=4 || p.y!=2 {return 2;}
        default:return 3;}
    return 0;
}''',
    'pattern-shadow-wildcard-default': '''
enum E {A(x i32);B(x i32,y i32);Empty;}
func match(e E) i32 {
    let x i32=40;var result i32=0;
    switch e {case E.A(x):{let x=x+1;result=x;}
        case E.B(x,_):result=x*2;default:result=3;}
    if x!=40 {return 90;}return result;
}
func whole(e E) i32 {
    let x i32=99;
    switch e {case x:switch x {case E.A(x):return x;case _:return 0;}}
}
func main() i32 {
    if match(E.A(x:4))!=5 || match(E.B(x:4,y:9))!=8 || match(E.Empty)!=3 {return 1;}
    if whole(E.A(x:7))!=7 || whole(E.Empty)!=0 {return 2;}return 0;
}''',
    'generic-pattern-identities': '''
struct Box[T] {value T;}
enum E[T] {Empty;Some(value Box[T]);}
func value[T](e E[T],fallback T) T {
    switch e {case E.Some(value):{let value=value.value;return value;}
        case E.Empty:return fallback;}
}
func main() i32 {
    let a i32=4;let b i64=9;
    if value(E[i32].Some(value:Box[i32](value:a)),a)!=4 {return 1;}
    if value(E[i64].Some(value:Box[i64](value:b)),b)!=9 {return 2;}
    return 0;
}''',
    'switch-continue-for-step': '''
enum E {Go;Skip;Stop;}
func kind(i i32) E {if i==1 {return E.Skip;}if i==2 {return E.Stop;}return E.Go;}
func step(i inout i32,n inout i32) {i+=1;n+=1;}
func main() i32 {
    var total i32=0;var steps i32=0;
    for var i i32=0;i<4;step(inout i,inout steps) {
        switch kind(i) {case E.Skip:continue;case E.Stop:break;case E.Go:total+=10;}
        total+=1;
    }
    if total!=23 || steps!=4 {return 1;}return 0;
}''',
    'nested-switch-loops-return': '''
enum E {Go;Skip;Stop;}
func kind(i i32) E {if i==1 {return E.Skip;}if i==2 {return E.Stop;}return E.Go;}
func step(i inout i32,n inout i32) {i+=1;n+=1;}
func early() i32 {
    for {switch E.Go {case E.Go:switch E.Stop {case E.Stop:return 42;default:break;}
        default:break;}}
}
func main() i32 {
    var sum i32=0;var steps i32=0;var outersteps i32=0;
    for var i i32=0;i<3;step(inout i,inout outersteps) {
        for var j i32=0;j<4;step(inout j,inout steps) {
            switch kind(j) {case E.Skip:continue;case E.Stop:break;case E.Go:
                switch E.Stop {case E.Stop:break;default:return 8;}
                if j==0 {sum+=10;} else {
                    for var k i32=0;k<3;k+=1 {if k==1 {break;}sum+=100;}
                }
            }
            sum+=1;
        }
    }
    if sum!=339 || steps!=12 || outersteps!=3 || early()!=42 {return 1;}
    var no_steps i32=0;
    for var i i32=0;i<2;step(inout i,inout no_steps) {switch E.Go {default:break;}break;}
    if no_steps!=0 {return 2;}return 0;
}''',
    'switch-scrutinee-once': '''
enum E {Some(x i32);Empty;}
func choose(n inout i32) E {n+=1;return E.Some(x:n);}
func main() i32 {
    var n i32=0;var seen i32=0;
    switch choose(inout n) {case E.Empty:return 1;case E.Some(x):seen=x;}
    if n!=1 || seen!=1 {return 2;}return 0;
}''',
    'empty-unit-bool-values': '''
struct Empty {}
struct P {e Empty;u Unit;b bool;}
enum E {A;B(e Empty,u Unit,b bool);}
func unit() {}
func pass(p P) P {return p;}
func main() i32 {
    var p=P(b:true,u:unit(),e:Empty());let old=pass(p);p.b=false;
    if !old.b || p.b {return 1;}
    switch E.B(b:old.b,u:p.u,e:p.e) {case E.A:return 2;case E.B(_,_,b):if !b {return 3;}}
    return 0;
}''',
    'aggregate-float-bits': '''
struct P {small f32;wide f64;}
enum E {Empty;Some(p P);}
func identity(p P) P {return p;}
func probe() P {
    var p=P(small:1.401298464324817070923729583289916131280e-45,
            wide:4.940656458412465441765687928682213723651e-324);
    let old=identity(p);p.small=1.0;p.wide=1.0;
    switch E.Some(p:old) {case E.Some(p):return p;case E.Empty:return p;}
}
func main() i32 {let p=probe();return 0;}
''',
    'enum-panic-is-not-switch-break': '''
enum E {A;B;}
func main() i32 {switch E.A {case E.A:var zero i32=0;let n=1/zero;case E.B:break;}return 0;}
''',
}
REJECTED = {
    'struct-owned-string': 'struct S {s string;} func main() {let s=S(s:"x");}',
    'enum-owned-string': 'enum E {A;B(s string);} func main() {let e=E.B(s:"x");}',
    'struct-owned-vec': 'struct S {v Vec[i32];} func main() {let s=S(v:[1,2]);}',
    'field-inout': 'struct S {x i32;} func f(x inout i32) {x+=1;} func main() {var s=S(x:1);f(inout s.x);}',
    'nested-variant-pattern': 'enum E {A;B(x i32);} enum O {A(e E);} func main() {switch O.A(e:E.A) {case O.A(E.A):return;default:return;}}',
    'aggregate-float-operation': 'struct P {x f32;} func main() {let p=P(x:1.0);let x=p.x+p.x;}',
    'float-negation': 'struct P {x f64;} func main() {let p=P(x:-0.0);}',
}


def main():
    parser = argparse.ArgumentParser()
    for name in ('langc', 'helper', 'root', 'out', 'python'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    for name, value in vars(args).items():
        setattr(args, name, value.resolve())
    args.out.mkdir(parents=True, exist_ok=True)
    version = command([args.python, '--version'])
    if version['stdout'].strip() != 'Python 3.12.15' or version['returncode']:
        raise RuntimeError(f'Python 3.12.15 required: {version}')
    trap_bin = args.out / 'no-python'
    trap_bin.mkdir(exist_ok=True)
    for name in ('python', 'python3', 'python3.12'):
        path = trap_bin / name
        path.write_text('#!/bin/sh\necho unexpected-python-during-compilation >&2\nexit 118\n')
        path.chmod(0o755)
    report = {'operation_id': 'language-python-pod-aggregates-20261104',
              'contract': 'compiler-dev/1', 'python': version,
              'source_head': command(['git', '-C', args.root, 'rev-parse', 'HEAD'])['stdout'].strip(),
              'cases': [], 'controls': [], 'unsupported': [], 'failures': [],
              'not_run': ['native execution', 'Clang ASan/UBSan (integrator)',
                          'ARC/Vec/string/foreign/throws/defer', 'strict FP arithmetic']}

    def compile(phase, manifest, directory):
        argv = [args.langc, phase, '--project', manifest, '--backend', 'python', '--format', 'json']
        if phase == 'build':
            argv += ['--out-dir', directory]
        return command(['/usr/bin/env', f'PATH={trap_bin}', *argv])

    generated = {}

    def run(ident, manifest, expected=0, panic=False):
        directory = args.out / 'generated' / ident
        row = {'id': ident, 'manifest': str(manifest),
               'source_sha256': {str(p.relative_to(manifest.parent)): sha(p)
                                 for p in sorted(manifest.parent.rglob('*')) if p.is_file()},
               'expected': {'returncode': expected,
                            'classification': 'panic' if panic else 'ordinary_nonzero' if expected else 'return'}}
        report['cases'].append(row)
        for phase in ('check', 'build'):
            result = row[phase] = compile(phase, manifest, directory)
            if result['returncode'] or result['stderr'] or json.loads(result['stdout'])['status'] != 'ok':
                row['status'] = 'failed'
                report['failures'].append(f'{ident}: {phase}')
                return
        row['generated_sha256'] = {name: sha(directory / name) for name in ('program.py', 'langrt.py')}
        row['execute'] = result = command([args.python, '-s', directory / 'program.py'])
        row['actual'] = classify(result)
        passed = result['returncode'] == expected and not result['stdout']
        passed &= (row['actual'] == 'panic' and result['stderr'] == 'LANG_PANIC: integer division by zero\n'
                   if panic else row['actual'] in ('return', 'ordinary_nonzero') and not result['stderr'])
        row['status'] = 'passed' if passed else 'failed'
        if not passed:
            report['failures'].append(f'{ident}: execution')
        generated[ident] = directory

    for ident, (body, expected) in ORIGINAL.items():
        run(ident, source_project(args.out, ident, body), expected)
    for ident, body in SOURCES.items():
        panic = ident == 'enum-panic-is-not-switch-break'
        run(ident, source_project(args.out, ident, body), 101 if panic else 0, panic)
    run('finite-callback', args.root / 'tests/python/fixtures/finite-callback/lang.toml')
    for manifest in sorted((args.root / 'tests/generics/positional').glob('*/lang.toml')):
        run('positional-' + manifest.parent.name, manifest)

    def control(ident, result, passed, **details):
        report['controls'].append(dict(id=ident, execute=result, status='passed' if passed else 'failed', **details))
        if not passed:
            report['failures'].append(ident)

    floats = generated.get('aggregate-float-bits')
    if floats:
        # The helper publishes SymbolId/source maps. Its Python output must be
        # byte-identical to the formal CLI output used for independent running.
        mapping = args.out / 'float-mapping'
        result = command([args.helper, '--project', args.out / 'sources/aggregate-float-bits/lang.toml',
                          '--out-dir', mapping])
        if result['returncode']:
            control('float-mapping', result, False)
        else:
            metadata = json.loads((mapping / 'metadata.json').read_text())
            probe = next(s['generated'] for s in metadata['symbols'] if s['source_name'] == 'probe')
            same = (floats / 'program.py').read_bytes() == (mapping / 'program.py').read_bytes()
            result = command([args.python, '-s', '-c',
                              f"import program,struct; p=program.{probe}(); "
                              "assert struct.pack('>f',p[0]).hex()=='00000001'; "
                              "assert struct.pack('>d',p[1]).hex()=='0000000000000001'"], cwd=floats)
            control('aggregate-float-exact-bits', result,
                    same and not result['returncode'] and not result['stdout'] and not result['stderr'],
                    helper_code_identical=same, expected_f32='00000001', expected_f64='0000000000000001')
        result = command([args.python, '-s', '-c',
                          "import langrt;langrt.RUNTIME_VERSION='wrong';import program"], cwd=floats)
        control('runtime-version-mismatch', result,
                classify(result) == 'python_exception' and 'incompatible langrt version' in result['stderr']
                and 'LANG_PANIC:' not in result['stderr'])
        result = command([args.python, '-s', '-c',
                          "import langrt;assert issubclass(langrt.SwitchBreak,BaseException);"
                          "assert not issubclass(langrt.SwitchBreak,Exception);"
                          "assert not issubclass(langrt.SwitchBreak,langrt.Panic)"], cwd=floats)
        control('switch-control-classification', result,
                result['returncode'] == 0 and not result['stdout'] and not result['stderr'])

    for ident, body in REJECTED.items():
        manifest = source_project(args.out, ident, body)
        directory = args.out / 'rejected' / ident / 'fresh'
        row = {'id': ident, 'source_sha256': sha(manifest.parent / 'src/main.lang')}
        passed = True
        for phase in ('check', 'build'):
            result = row[phase] = compile(phase, manifest, directory)
            parsed = json.loads(result['stdout'])
            source = manifest.parent / 'src/main.lang'
            passed &= (result['returncode'] == 1 and not result['stderr'] and parsed['status'] == 'error'
                       and any(d['code'] == 'E_CAPABILITY_UNAVAILABLE' and d['primary'] is not None
                               and Path(d['primary']['path']).resolve() == source.resolve()
                               and 0 <= d['primary']['start_byte'] < d['primary']['end_byte'] <= source.stat().st_size
                               for d in parsed['diagnostics']) and not directory.exists())
        # Also prove that a failed formal build preserves pre-existing output.
        directory = args.out / 'rejected' / ident / 'existing'
        directory.mkdir(parents=True, exist_ok=True)
        for name in ('program.py', 'langrt.py', 'unrelated'):
            (directory / name).write_text('preserve-' + name)
        before = {p.name: sha(p) for p in directory.iterdir()}
        row['build_existing'] = compile('build', manifest, directory)
        passed &= (row['build_existing']['returncode'] == 1
                   and before == {p.name: sha(p) for p in directory.iterdir()})
        row['status'] = 'unsupported' if passed else 'failed'
        report['unsupported'].append(row)
        if not passed:
            report['failures'].append('unsupported: ' + ident)

    report['status'] = 'failed' if report['failures'] else 'passed'
    report['summary'] = {group: {status: sum(row['status'] == status for row in report[group])
                                for status in ('passed', 'failed', 'unsupported')}
                         for group in ('cases', 'controls', 'unsupported')}
    path = args.out / 'aggregate-results.json'
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], summary=report['summary'],
                          failures=report['failures'], report=str(path)), ensure_ascii=False))
    return bool(report['failures'])


if __name__ == '__main__':
    sys.exit(main())
