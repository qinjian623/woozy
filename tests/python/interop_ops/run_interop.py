#!/usr/bin/env python3
"""FFI02 declared property/index/slice conformance runner.

The runner builds real Python-backend projects with the public `woozyc` CLI,
executes them with the dedicated CPython/NumPy/PyTorch environment, and checks
the existing T10 binding path as a compatibility control.  It does not import
NumPy/PyTorch itself.
"""
import argparse
import copy
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
THREAD_ENV = ('OMP_NUM_THREADS', 'MKL_NUM_THREADS', 'OPENBLAS_NUM_THREADS',
              'NUMEXPR_NUM_THREADS', 'VECLIB_MAXIMUM_THREADS')


class Runner:
    def __init__(self, args):
        self.args = args
        self.rows = []
        self.commands = []
        self.env = dict(os.environ)
        for name in THREAD_ENV:
            self.env[name] = '1'
        self.env['PYTHONDONTWRITEBYTECODE'] = '1'
        self.env['PYTHONNOUSERSITE'] = '1'
        self.env.pop('PYTHONPATH', None)
        self.env.pop('PYTHONOPTIMIZE', None)
        self.root = args.root.resolve()
        self.binding = args.binding.resolve()
        self.host = Path(__file__).resolve().parent / 'host.py'

    def command(self, row, argv, cwd, timeout=180):
        argv = [str(value) for value in argv]
        proc = subprocess.run(argv, cwd=str(cwd), env=self.env,
                              capture_output=True, text=True, timeout=timeout)
        record = {'argv': argv, 'cwd': str(Path(cwd).resolve()),
                  'exit': proc.returncode, 'stdout': proc.stdout,
                  'stderr': proc.stderr}
        row['commands'].append(record)
        self.commands.append(record)
        return record

    def project(self, name, source, binding=None):
        root = self.args.out / 'projects' / name
        if root.exists():
            shutil.rmtree(root)
        (root / 'src').mkdir(parents=True)
        (root / 'src/main.wz').write_text(source)
        binding = Path(binding or self.binding).resolve()
        (root / 'lang.toml').write_text(f'''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "python"
word_bits = 64
[dependencies.interophost]
kind = "python"
binding = "{binding}"
host_package = "interophost"
[dependencies.numpy]
kind = "python"
binding = "{binding}"
host_package = "numpy"
[dependencies.torch]
kind = "python"
binding = "{binding}"
host_package = "torch"
''')
        return root

    def build(self, row, project, binding=None, backend='python', out=None):
        binding = Path(binding or self.binding).resolve()
        out = out or project / 'out'
        record = self.command(
            row,
            [self.args.woozyc, 'build', '--project', project / 'lang.toml',
             '--backend', backend, '--out-dir', out,
             '--std-binding', binding],
            project)
        return record, out

    def run_program(self, row, out):
        shutil.copyfile(self.host, out / 'interophost.py')
        return self.command(row, [self.args.python, '-s', 'program.py'], out)

    def run_ok(self, row, out, expected_return=0):
        record = self.run_program(row, out)
        require(record['exit'] == expected_return,
                'program returned ' + str(record['exit']) + '\n' +
                record['stdout'] + record['stderr'])
        return record

    def check(self, row, project, binding=None, backend='python'):
        binding = Path(binding or self.binding).resolve()
        record = self.command(
            row,
            [self.args.woozyc, 'check', '--project', project / 'lang.toml',
             '--backend', backend, '--std-binding', binding, '--format',
             'json'],
            project)
        return record

    def add(self, name, fn):
        row = {'name': name, 'status': 'failed', 'commands': []}
        self.rows.append(row)
        try:
            fn(row)
            row['status'] = 'passed'
        except BaseException as error:
            row['error'] = {
                'type': type(error).__module__ + '.' + type(error).__qualname__,
                'message': str(error),
            }
        return row


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def fail(record, message):
    raise AssertionError(message + ': ' + json.dumps(record, indent=2)[-6000:])


def diagnostics(record):
    try:
        return json.loads(record['stdout']).get('diagnostics', [])
    except Exception:
        return []


def built(row, record, out):
    if record['exit'] != 0:
        fail(record, 'build failed')
    require((out / 'program.py').exists(), 'program.py was not generated')
    require((out / 'langrt.py').exists(), 'langrt.py was not generated')




PROPERTY_SOURCE = '''package main;
import "std/py" as py;
import "interophost" as host;

struct Holder { p host.Probe; values Vec[host.Probe]; }
struct ErrorBox { error py.Error; }

func pass(p host.Probe) host.Probe { return p; }

func explode_after(p host.Probe) i64 throws py.Error {
    let owned = "defer me";
    var caught = 0;
    defer { try { p.value = 7.0; } catch e py.Error { } }
    try { let bad = p.explode; } catch e py.Error { caught = 1; }
    if caught != 1 { return 9; }
    return 0;
}

func main() i32 throws py.Error {
    let p = host.make();
    p.value = 1.5;
    if p.value != 1.5 { return 1; }
    if p.nested_value != 5 { return 2; }
    if host.probe_counts(p) != 1010000 { return 3; }
    try { let bad = p.explode; return 4; } catch e py.Error {
        if host.inspect(e) != 31 { return 5; }
        let boxed = ErrorBox(error: e);
        let errors Vec[py.Error] = [e];
        if host.inspect(boxed.error) != 31 { return 6; }
        if host.inspect(errors[0]) != 31 { return 7; }
    }
    try { p.explode_set = 9; return 8; } catch e py.Error {
        if host.inspect(e) != 31 { return 9; }
    }
    try { let bad = p.bad_type; return 10; } catch e py.Error {
        if host.inspect(e) != 24 { return 11; }
    }
    if host.explode_count(p) != 2 { return 12; }
    let d = host.make();
    try { let v = explode_after(d); if v != 0 { return 13; } }
    catch e py.Error { return 14; }
    if d.value != 7.0 { return 15; }
    if host.probe_counts(d) != 1010000 { return 16; }
    let h = Holder(p: host.make(), values: [host.make(), host.make()]);
    h.p.value = 2.0;
    h.values[host.note_i64(0)].value = host.note_f64(3.5);
    if h.p.value != 2.0 { return 17; }
    if h.values[0].value != 3.5 { return 18; }
    p.text = "hello";
    if p.text != "hello" { return 19; }
    if p.child.value != 1.5 { return 20; }
    pass(p).value = 3.25;
    if pass(p).value != 3.25 { return 21; }
    return 0;
}
'''

INDEX_SOURCE = '''package main;
import "std/py" as py;
import "interophost" as host;

func main() i32 throws py.Error {
    let p = host.make();
    let k = host.make_key(1);
    if p[k] != 20 { return 1; }
    p[host.make_key(2)] = host.note_i64(42);
    if p[host.make_key(2)] != 42 { return 2; }
    try { let bad = p[host.make_key(9)]; return 3; } catch e py.Error {
        if host.inspect(e) != 24 { return 4; }
    }
    if host.probe_counts(p) != 301 { return 5; }
    let b = host.make_readonly();
    if b[1] != 20 { return 6; }
    if b[0:2] != 30 { return 7; }
    return 0;
}
'''

NUMPY_SOURCE = '''package main;
import "std/py" as py;
import "interophost" as host;
import "numpy" as np;

func main() i32 throws py.Error {
    let a = np.linspace(0.0, 3.0, num: 4, dtype: np.float32);
    if a.ndim != 1 { return 1; }
    if a[1] != 1.0 { return 2; }
    let b = a[host.note_i64(1):host.note_i64(3)];
    if b[0] != 1.0 { return 3; }
    if b[1] != 2.0 { return 4; }
    let open_start = a[:2];
    if open_start[1] != 1.0 { return 5; }
    let open_end = a[1:];
    if open_end[0] != 1.0 { return 6; }
    a[0] = 0.5;
    if a[0] != 0.5 { return 7; }
    let c = np.linspace(7.0, 8.0, num: 2, dtype: np.float32);
    a[host.note_i64(1):host.note_i64(3)] = c;
    if a[1] != 7.0 { return 8; }
    if a[2] != 8.0 { return 9; }
    let d = np.linspace(5.0, 5.0, num: 4, dtype: np.float32);
    a[:] = d;
    if a[0] != 5.0 { return 10; }
    if a[3] != 5.0 { return 11; }
    return 0;
}
'''

TORCH_SOURCE = '''package main;
import "std/py" as py;
import "numpy" as np;
import "torch" as torch;

func main() i32 throws py.Error {
    let a = np.linspace(0.0, 3.0, num: 4, dtype: np.float32);
    let t = torch.tensor(a, dtype: torch.float32, device: "cpu");
    if t.ndim != 1 { return 1; }
    if t[0].item() != 0.0 { return 2; }
    let s = t[0:2];
    if s[1].item() != 1.0 { return 3; }
    let open_start = t[:2];
    if open_start[0].item() != 0.0 { return 4; }
    let open_end = t[1:];
    if open_end[0].item() != 1.0 { return 5; }
    t[0] = t[1];
    if t[0].item() != 1.0 { return 6; }
    t[0:2] = t[2:4];
    if t[0].item() != 2.0 { return 7; }
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--woozyc', type=Path, required=True)
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--python', type=Path, required=True)
    parser.add_argument('--binding', type=Path, required=True)
    parser.add_argument('--only', nargs='+', default=[])
    args = parser.parse_args()
    args.python = Path(os.path.abspath(str(args.python)))
    for key in ('woozyc', 'helper', 'root', 'out', 'binding'):
        setattr(args, key, getattr(args, key).resolve())
    args.out.mkdir(parents=True, exist_ok=True)
    r = Runner(args)

    def pick(name):
        return not args.only or name in args.only

    def index_case(row):
        project = r.project('index-key-order', INDEX_SOURCE)
        record, out = r.build(row, project)
        built(row, record, out)
        result = r.run_ok(row, out)
        lines = result['stdout'].splitlines()
        expected = [
            'INTEROP MAKE', 'INTEROP MAKE_KEY 1', 'INTEROP KEY 1',
            'INTEROP GETITEM 1 1', 'INTEROP MAKE_KEY 2',
            'INTEROP NOTE_I64 42', 'INTEROP KEY 2',
            'INTEROP SETITEM 2 1 42', 'INTEROP MAKE_KEY 2',
            'INTEROP KEY 2', 'INTEROP GETITEM 2 2',
            'INTEROP MAKE_KEY 9', 'INTEROP KEY 9',
            'INTEROP GETITEM 9 3',
        ]
        require(lines[:len(expected)] == expected, (lines, expected))
        require(lines[len(expected)].startswith(
            'INTEROP ERR type=builtins.IndexError '
            'message=list index out of range cause=none '
            'operation=interophost.Probe.index file='), lines)
        require(lines[len(expected) + 1:] == [
            'INTEROP COUNTS 0 0 3 1', 'INTEROP MAKE_READONLY',
            'INTEROP READONLY_GET 1', 'INTEROP READONLY_SLICE 0 2',
        ], lines)
        row['stdout'] = result['stdout']

    def properties_case(row):
        project = r.project('properties-errors', PROPERTY_SOURCE)
        record, out = r.build(row, project)
        built(row, record, out)
        result = r.run_ok(row, out)
        lines = result['stdout'].splitlines()
        require(sum(line.startswith('INTEROP ERR type=interophost.HostOuter '
                                    'message=property getter failed '
                                    'cause=HostInner '
                                    'operation=interophost.Probe.explode '
                                    'file=') for line in lines) >= 1, lines)
        require(any('operation=interophost.Probe.explode_set' in line for line in lines),
                lines)
        require(any('operation=interophost.Probe.bad_type' in line for line in lines),
                lines)
        require('INTEROP EXPLODE_COUNT 2' in lines, lines)
        require(lines.count('INTEROP NOTE_I64 0') == 1, lines)
        require(lines.count('INTEROP NOTE_F64 3.5') == 1, lines)
        # Nested property evaluation order: key, then value, then setter.
        i_key = lines.index('INTEROP NOTE_I64 0')
        i_value = lines.index('INTEROP NOTE_F64 3.5')
        i_set = lines.index('INTEROP SET value 1 3.5')
        require(i_key < i_value < i_set, lines)
        row['stdout'] = result['stdout']

    def numpy_case(row):
        project = r.project('numpy-index-slice', NUMPY_SOURCE)
        record, out = r.build(row, project)
        built(row, record, out)
        result = r.run_ok(row, out)
        notes = [line for line in result['stdout'].splitlines()
                 if line.startswith('INTEROP NOTE_I64 ')]
        require(notes == ['INTEROP NOTE_I64 1', 'INTEROP NOTE_I64 3',
                          'INTEROP NOTE_I64 1', 'INTEROP NOTE_I64 3'], notes)

    def torch_case(row):
        project = r.project('torch-index-slice', TORCH_SOURCE)
        record, out = r.build(row, project)
        built(row, record, out)
        r.run_ok(row, out)

    if pick('properties-errors'):
        r.add('properties-errors', properties_case)
    if pick('index-key-order'):
        r.add('index-key-order', index_case)
    if pick('numpy-index-slice'):
        r.add('numpy-index-slice', numpy_case)
    if pick('torch-index-slice'):
        r.add('torch-index-slice', torch_case)

    negative_sources = {
        'undeclared-property': ('''let p = host.make(); let x = p.missing;''',
                                'E_CAPABILITY_UNAVAILABLE',
                                'external property is not declared'),
        'readonly-property': ('''let p = host.make(); p.readonly = 1;''',
                              'E_CAPABILITY_UNAVAILABLE',
                              'external property is read-only'),
        'undeclared-index': ('''let n = host.make_no_index(); let x = n[0];''',
                             'E_CAPABILITY_UNAVAILABLE',
                             'external indexing is not declared'),
        'wrong-index-type': ('''let p = host.make(); let x = p["bad"];''',
                             'E_TYPE_MISMATCH', None),
        'undeclared-slice': ('''let p = host.make(); let x = p[0:1];''',
                             'E_CAPABILITY_UNAVAILABLE',
                             'external slicing is not declared'),
        'wrong-slice-type': ('''let a = np.linspace(0.0, 1.0, num: 2, dtype: np.float32);
                               let x = a["a":"b"];''',
                             'E_TYPE_MISMATCH', None),
        'wrong-property-type': ('''let p = host.make(); p.value = "bad";''',
                                'E_TYPE_MISMATCH',
                                'value type does not match'),
        'compound-property': ('''let p = host.make(); p.value += 1.0;''',
                              'E_CAPABILITY_UNAVAILABLE',
                              'compound assignment'),
        'readonly-index': ('''let b = host.make_readonly(); b[0] = 1;''',
                           'E_CAPABILITY_UNAVAILABLE',
                           'external indexing is read-only'),
        'readonly-slice': ('''let b = host.make_readonly(); b[0:1] = 2;''',
                           'E_CAPABILITY_UNAVAILABLE',
                           'external slicing is read-only'),
        'multi-index': ('''let a = np.linspace(0.0, 1.0, num: 2, dtype: np.float32);
                           let x = a[0,1];''',
                        'E_TYPE_MISMATCH',
                        'exactly one index'),
        'undeclared-property-write': ('''let n = host.make_no_index(); n.value = 1;''',
                                      'E_CAPABILITY_UNAVAILABLE',
                                      'external property is not declared'),
    }

    def negative(name, body, code, message):
        def case(row):
            source = f'''package main;
import "std/py" as py;
import "interophost" as host;
import "numpy" as np;

func main() i32 throws py.Error {{
    {body}
    return 0;
}}
'''
            project = r.project('negative-' + name, source)
            record = r.check(row, project)
            require(record['exit'] != 0, record)
            found = diagnostics(record)
            require(found, record)
            require(any(d.get('code') == code for d in found), found)
            if message is not None:
                require(any(message in d.get('message', '') for d in found),
                        found)
        return case

    for name, (body, code, message) in negative_sources.items():
        if pick('negative-' + name):
            r.add('negative-' + name, negative(name, body, code, message))

    def native_case(row):
        project = r.project('native-rejected', PROPERTY_SOURCE)
        record = r.check(row, project, backend='native')
        require(record['exit'] != 0, record)
        found = diagnostics(record)
        require(any(d.get('code') == 'E_CAPABILITY_UNAVAILABLE' and
                    'external module is unavailable' in d.get('message', '')
                    for d in found), found)
    if pick('native-rejected'):
        r.add('native-rejected', native_case)

    def binding_error_case(row, name, mutate):
        variant = json.loads(r.binding.read_text())
        mutate(variant)
        path = args.out / 'bindings' / (name + '.json')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(variant, indent=2) + '\n')
        source = '''package main;
import "std/py" as py;
import "interophost" as host;
func main() i32 throws py.Error { let p = host.make(); return 0; }
'''
        project = r.project('binding-' + name, source, path)
        record = r.check(row, project, binding=path)
        require(record['exit'] != 0, record)
        found = diagnostics(record)
        require(any(d.get('code') == 'E_BINDING_INVALID' for d in found), found)

    def unknown_field(variant):
        variant['properties'][0]['unknown_ffi02_field'] = 1

    def wrong_error_type(variant):
        variant['properties'][0]['error_type'] = 'lang.py.NotError'

    def duplicate_property(variant):
        duplicate = copy.deepcopy(variant['properties'][0])
        duplicate['id'] = 'interophost.Probe.value.duplicate'
        variant['properties'].append(duplicate)

    def duplicate_indexer(variant):
        duplicate = copy.deepcopy(variant['indexers'][0])
        duplicate['id'] = 'interophost.Probe.index.duplicate'
        variant['indexers'].append(duplicate)

    def duplicate_id(variant):
        variant['indexers'].append(copy.deepcopy(variant['indexers'][0]))

    def unknown_top_field(variant):
        variant['unknown_ffi02_top'] = []

    def null_properties(variant):
        variant['properties'] = None

    def null_indexers(variant):
        variant['indexers'] = None

    def null_slices(variant):
        variant['slices'] = None

    for name, mutate in (
            ('unknown-field', unknown_field),
            ('wrong-error-type', wrong_error_type),
            ('duplicate-property', duplicate_property),
            ('duplicate-indexer', duplicate_indexer),
            ('duplicate-id', duplicate_id),
            ('unknown-top-field', unknown_top_field),
            ('null-properties', null_properties),
            ('null-indexers', null_indexers),
            ('null-slices', null_slices)):
        if pick('binding-' + name):
            r.add('binding-' + name,
                  lambda row, n=name, m=mutate: binding_error_case(row, n, m))

    def legacy_case(row):
        out = args.out / 'legacy-t10'
        record = r.command(
            row,
            [args.python, args.root / 'tests/python/run_t10_bindings.py',
             '--helper', args.helper,
             '--woozyc', args.woozyc,
             '--root', args.root,
             '--out', out,
             '--python', args.python,
             '--binding', args.root / 'tests/python/t10_binding.json',
             '--example-binding', args.root / 'contracts/python_bindings.example.json',
             '--reference', args.root / 'baseline/samples/numpy_torch_reference.py',
             '--observe', args.root / 'tests/python/observe_t10.py',
             '--original-observer', args.root / 'tests/python/t10_original04.py',
             '--reference-observer', args.root / 'tests/python/t10_reference04.py',
             '--only', 'order-objects-errors',
             'foreign-ownership-move-aggregate',
             'foreign-ownership-cleanup'],
            args.root, timeout=600)
        require(record['exit'] == 0, record)
        summary = json.loads(record['stdout'].strip().splitlines()[-1])
        require(summary['status'] == 'passed', summary)
        row['summary'] = summary
    if pick('legacy-t10-control'):
        r.add('legacy-t10-control', legacy_case)

    results = {
        'status': 'passed' if all(row['status'] == 'passed' for row in r.rows)
                  else 'failed',
        'cases': {row['name']: row['status'] for row in r.rows},
        'rows': r.rows,
        'commands': r.commands,
    }
    (args.out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    for row in r.rows:
        print(row['name'], row['status'])
        if row['status'] != 'passed':
            print(json.dumps(row.get('error'), indent=2))
            for command in row['commands']:
                print('command:', ' '.join(command['argv']))
                print('exit:', command['exit'])
                print(command['stdout'][-4000:])
                print(command['stderr'][-4000:])
    print(results['status'], args.out / 'results.json')
    return 0 if results['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
