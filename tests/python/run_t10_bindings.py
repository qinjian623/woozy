#!/usr/bin/env python3
"""Generate, execute and assert T10 Python binding/runtime cases.

The helper uses the real public load/resolve/check/verify/emit API.  Generated
programs are executed only by the explicitly supplied CPython path; this
runner does not import NumPy/PyTorch itself except through the independent
original-04, real-object and reference subprocesses.
"""
import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
THREAD_ENV = ('OMP_NUM_THREADS', 'MKL_NUM_THREADS', 'OPENBLAS_NUM_THREADS',
              'NUMEXPR_NUM_THREADS', 'VECLIB_MAXIMUM_THREADS')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


class Runner:
    def __init__(self, args):
        self.args = args
        self.rows = []
        self.registered = set()
        self.commands = []
        self.env = dict(os.environ)
        for name in THREAD_ENV:
            self.env[name] = '1'
        self.env['PYTHONDONTWRITEBYTECODE'] = '1'
        self.env['PYTHONNOUSERSITE'] = '1'
        self.env.pop('PYTHONPATH', None)

    def command(self, row, argv, cwd, timeout=180):
        argv = [str(value) for value in argv]
        proc = subprocess.run(argv, cwd=str(cwd), env=self.env,
                              capture_output=True, text=True, timeout=timeout)
        record = {
            'argv': argv,
            'cwd': str(Path(cwd).resolve()),
            'exit': proc.returncode,
            'stdout': proc.stdout,
            'stderr': proc.stderr,
        }
        row['commands'].append(record)
        self.commands.append(record)
        return record

    def project(self, name, files, binding):
        root = self.args.out / 'projects' / name
        if root.exists():
            shutil.rmtree(root)
        for relative, text in files.items():
            write(root / relative, text)
        write(root / 'lang.toml', f'''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "python"
word_bits = 64
[dependencies.t10host]
kind = "python"
binding = "{binding}"
host_package = "t10host"
''')
        return root

    def emit(self, row, project, binding, expect_success=True):
        out = project / 'out'
        record = self.command(
            row,
            [self.args.helper, '--project', project / 'lang.toml',
             '--out-dir', out, '--std-binding', binding],
            project)
        if expect_success:
            assert record['exit'] == 0, record
            assert (out / 'program.py').exists(), record
            assert (out / 'langrt.py').exists(), record
            # The declared host module is copied alongside generated files.
            host = self.args.root / 'tests/python/t10_host.py'
            shutil.copyfile(host, out / 't10host.py')
            row['generated_files'] = {
                'program.py': sha(out / 'program.py'),
                'langrt.py': sha(out / 'langrt.py'),
                'verified.hir': sha(out / 'verified.hir'),
                'metadata.json': sha(out / 'metadata.json'),
            }
            code = (out / 'program.py').read_text()
            assert '_LANG_BINDING_INFO =' in code
            assert 'import std.py' not in code
        return out, record

    def observe(self, row, out):
        record = self.command(row, [self.args.python, self.args.observe, out],
                              out)
        assert record['exit'] == 0, record
        lines = record['stdout'].splitlines()
        assert lines, record
        observed = json.loads(lines[-1])
        events = [line[len('T10HOST '):] for line in lines[:-1]
                  if line.startswith('T10HOST ')]
        return observed, events

    def real_observe(self, row, out, mode):
        record = self.command(
            row, [self.args.python, self.args.real_observer, out, mode], out)
        assert record['exit'] == 0, record
        lines = record['stdout'].splitlines()
        assert lines, record
        return json.loads(lines[-1])

    def direct(self, row, out, timeout=180):
        return self.command(row, [self.args.python, '-s', out / 'program.py'],
                            out, timeout=timeout)

    def add(self, name, fn):
        self.registered.add(name)
        if self.args.only and name not in self.args.only:
            return None
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


def non_error_events(events):
    return [event for event in events if not event.startswith('ERR ')]


def error_events(events):
    return [event for event in events if event.startswith('ERR ')]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--helper', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--python', type=Path, required=True)
    parser.add_argument('--binding', type=Path, required=True)
    parser.add_argument('--example-binding', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--observe', type=Path, required=True)
    parser.add_argument('--original-observer', type=Path, required=True)
    parser.add_argument('--reference-observer', type=Path, required=True)
    parser.add_argument('--real-observer', type=Path)
    parser.add_argument('--only', nargs='+', default=[],
                        help='run only these named cases in this invocation')
    args = parser.parse_args()
    # Keep the venv interpreter path itself: resolving its symlink selects the
    # base CPython and loses the dedicated NumPy/PyTorch site-packages.
    args.python = Path(os.path.abspath(str(args.python)))
    for key in ('helper', 'root', 'out', 'binding', 'example_binding',
                'reference', 'observe', 'original_observer', 'reference_observer'):
        setattr(args, key, getattr(args, key).resolve())
    if args.real_observer is None:
        args.real_observer = args.root / 'tests/python/t10_real_objects.py'
    else:
        args.real_observer = args.real_observer.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    r = Runner(args)
    binding = str(args.binding)
    example_binding = str(args.example_binding)

    order_source = '''package main;
import "std/py" as py;
import "t10host" as host;
func pass(p host.Probe) host.Probe { return p; }
func main() i32 throws py.Error {
    let p = host.make();
    let q = p;
    host.set_value(q, 42);
    if host.get_value(p) != 42 { return 1; }
    host.same(p, q);
    let r = pass(p);
    host.same(p, r);
    host.optional(1, c: 3);
    host.optional(c: 3, a: 1);
    host.optional(7);
    let m = host.marker;
    host.check_marker(m);
    if p(0) != 42 { return 2; }
    host.return_none();
    host.make().ping(host.arg(1));
    try { host.make_bad().broken(); return 10; } catch e py.Error { let saved = e; host.inspect(saved); }
    try { host.make().ping(host.arg_fail(2)); return 11; } catch e py.Error { host.inspect(e); }
    try { host.make().fail_call(); return 12; } catch e py.Error { host.inspect(e); }
    try { host.error_source(); return 13; } catch e py.Error { host.inspect(e); }
    return 0;
}
'''
    expected_nonerror = [
        'MAKE', 'SET 42', 'GET 42', 'SAME True', 'SAME True',
        'OPTIONAL a=1 b=MISSING c=3', 'OPTIONAL a=1 b=MISSING c=3',
        'OPTIONAL a=7 b=MISSING c=MISSING', 'MARKER Probe True',
        'CALL call 0',
        'MAKE', 'READ ping', 'ARG 1', 'CALL ping 1',
        'MAKE_BAD', 'READ broken', 'READFAIL broken',
        'MAKE', 'READ ping', 'ARG_FAIL 2',
        'MAKE', 'READ fail_call', 'CALL fail_call',
    ]
    expected_errors = [
        ('t10host.HostOuter', 't10host.Probe.broken',
         't10host.HostInner:inner read cause', 'traceback=yes'),
        ('builtins.ValueError', 't10host.arg_fail', 'cause=None',
         'traceback=no'),
        ('builtins.RuntimeError', 't10host.Probe.fail_call', 'cause=None',
         'traceback=no'),
        ('t10host.HostOuter', 't10host.error_source',
         't10host.HostInner:inner cause', 'traceback=yes'),
    ]

    def order_case(row):
        project = r.project('order-objects-errors', {'src/main.lang': order_source}, binding)
        out, _ = r.emit(row, project, binding)
        observed, events = r.observe(row, out)
        require(observed['main_return'] == 0, observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        require(observed['frames'] >= 1, observed)
        require(non_error_events(events) == expected_nonerror, events)
        errors = error_events(events)
        require(len(errors) == len(expected_errors), events)
        for event, (kind, operation, cause, traceback) in zip(errors, expected_errors):
            require('type=' + kind in event, event)
            require('operation=' + operation in event, event)
            require(cause in event, event)
            require(traceback in event, event)
            require('file=' in event and event.split('begin=')[1].split()[0].isdigit(),
                    event)
        info = observed['binding_info']
        bundle = info['bundles'][0]
        require(info['runtime_version'] == 'compiler-dev/1-python-values/7', info)
        require(bundle['contract'] == 'compiler-dev/1', bundle)
        require(bundle['sha256'] == sha(args.binding), bundle)
        require(bundle['review_status'] == 'draft_requires_conformance', bundle)
        require(any(m['logical_path'] == 't10host' and m['host_module'] == 't10host'
                    for m in bundle['modules']), bundle)
        require(any(r['package'] == 't10host' for r in bundle['package_requirements']),
                bundle)
        row['observation'] = observed
        row['events'] = events

    r.add('order-objects-errors', order_case)

    foreign_ownership_source = '''package main;
import "std/py" as py;
import "t10host" as host;

struct Box[T] { value T; }
struct Inner { p host.Probe; }
struct Outer { inner Inner; v Vec[i32]; }
enum Choice { One(p host.Probe); None; }
enum Maybe[T] { Some(value T); None; }
trait Sink { func keep(self Self, p move host.Probe) host.Probe; }
struct Holder { n i32; }
impl Sink for Holder {
  func keep(self Holder, p move host.Probe) host.Probe { return p; }
}
struct GenericHolder { n i32; }
impl GenericHolder {
  func keep[T](self GenericHolder, value move T) T { return move value; }
}
struct GenericBox[T] { value T; }
impl[T] GenericBox[T] {
  func keep[U](self Self, value move U) U { return move value; }
}
func pass(p host.Probe) host.Probe { return p; }
func take(p move host.Probe) host.Probe { return p; }
func relay[T](value move T) T { return move value; }
func apply(cb func(move host.Probe) host.Probe, p move host.Probe) host.Probe {
  return cb(move p);
}
func wrap(p host.Probe) Outer {
  return Outer(inner: Inner(p: p), v: [4]);
}
func pack(p host.Probe) Vec[host.Probe] {
  return [p];
}
func main() i32 throws py.Error {
  let x = host.make();
  let y = move x;
  if host.same(y, y) != true { return 1; }
  let values Vec[host.Probe] = [y];
  if host.same(y, values[0]) != true { return 2; }
  host.set_value(values[0], 42);
  if host.get_value(y) != 42 { return 3; }
  let part = values[0:1];
  if host.same(y, part[0]) != true { return 4; }
  let inner = Inner(p: pass(y));
  let outer = Outer(inner: inner, v: [1]);
  let wrapped = wrap(y);
  if host.same(wrapped.inner.p, y) != true { return 28; }
  let packed = pack(y);
  if host.same(packed[0], y) != true { return 29; }
  if host.same(y, outer.inner.p) != true { return 5; }
  host.set_value(outer.inner.p, 7);
  if host.get_value(y) != 7 { return 6; }
  var changed = outer;
  let q = host.make();
  changed.inner.p = q;
  if host.same(changed.inner.p, q) != true { return 7; }
  if host.get_value(y) != 7 { return 8; }
  if changed.v[0] != 1 { return 9; }
  var outer_move = outer;
  let moved_outer = move outer_move;
  if host.same(y, moved_outer.inner.p) != true { return 19; }
  var vec_move = values;
  let moved_vec = move vec_move;
  if host.same(y, moved_vec[0]) != true { return 20; }
  let direct_outer = move Outer(inner: Inner(p: y), v: [3]);
  if host.same(y, direct_outer.inner.p) != true { return 21; }
  let made = move host.make();
  host.set_value(made, 3);
  if host.get_value(made) != 3 { return 22; }
  let made_vec Vec[host.Probe] = [move made];
  if host.get_value(made_vec[0]) != 3 { return 23; }
  let choice = Choice.One(p: y);
  switch choice {
  case Choice.One(p):
    if host.same(p, y) != true { return 10; }
  case Choice.None: return 11;
  }
  let maybe = Maybe[host.Probe].Some(value: y);
  switch maybe {
  case Maybe.Some(p):
    if host.same(p, y) != true { return 26; }
  case Maybe.None: return 27;
  }
  let boxed = Box[host.Probe](value: y);
  if host.same(boxed.value, y) != true { return 12; }
  let moved = take(move q);
  if host.same(moved, moved) != true { return 13; }
  let relayed = relay[host.Probe](move moved);
  if host.same(relayed, relayed) != true { return 14; }
  let held = Holder(n: 1);
  let kept = held.keep(move relayed);
  if host.same(kept, kept) != true { return 15; }
  let temp = pass(y);
  let functional = apply(take, move temp);
  if host.same(functional, y) != true { return 16; }
  let result = apply(take, move functional);
  if host.same(result, y) != true { return 17; }
  for item in values {
    if host.same(item, y) != true { return 18; }
  }
  let gh = GenericHolder(n: 2);
  let generic_kept = gh.keep[host.Probe](move result);
  if host.same(generic_kept, y) != true { return 24; }
  let gb = GenericBox[host.Probe](value: y);
  let gb_kept = gb.keep[host.Probe](move generic_kept);
  if host.same(gb_kept, y) != true { return 25; }
  return 0;
}
'''

    def foreign_ownership_case(row):
        project = r.project('foreign-ownership',
                            {'src/main.lang': foreign_ownership_source}, binding)
        out, _ = r.emit(row, project, binding)
        observed, events = r.observe(row, out)
        require(observed['main_return'] == 0, observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        require(not error_events(events), events)
        metadata = json.loads((out / 'metadata.json').read_text())
        entry = '_f' + str(metadata['entry_symbol'])
        x_states = [state for state in observed['owner_states']
                    if state['function'] == entry and state['name'] == 'x']
        require(any(state['active'] for state in x_states), x_states)
        require(x_states and not x_states[-1]['active'], x_states)
        require(len(observed['foreign_copies']) > 0, observed)
        require(len(observed['foreign_drops']) > 0, observed)
        makes = [op for op in observed['operations'] if op == ['host', 'MAKE']]
        require(len(makes) == 3, observed['operations'])
        for event in (['host', 'SET', 42], ['host', 'GET', 42],
                      ['host', 'SET', 7], ['host', 'GET', 7]):
            require(event in observed['operations'], (event, observed['operations']))
        row['observation'] = observed

    r.add('foreign-ownership-move-aggregate', foreign_ownership_case)

    foreign_cleanup_source = '''package main;
import "std/py" as py;
import "t10host" as host;

enum Failure { Bad(value Vec[host.Probe]); }
enum Bad { Stop; }
struct Pair { p host.Probe; n i32; }
func consume(p move host.Probe, n i64) i64 throws py.Error {
  if host.same(p, p) != true { return -1; }
  return n;
}
func fail_move(p move host.Probe) i32 throws Bad { throw Bad.Stop; }
func fail_agg(seed host.Probe) Vec[host.Probe] throws Failure {
  let values Vec[host.Probe] = [seed];
  throw Failure.Bad(value: values);
}
func fail_partial() i32 throws Bad { throw Bad.Stop; }
func keep_deferred(p move host.Probe) host.Probe throws py.Error {
  defer { try { host.same(p, p); host.note(7); } catch e py.Error { } }
  return p;
}
func main() i32 throws py.Error {
  let a = host.make();
  if consume(move a, host.arg(1)) != 1 { return 1; }
  let b = host.make();
  try { consume(move b, host.arg_fail(2)); return 2; }
  catch e py.Error { host.note(2); }
  let c = host.make();
  try { fail_move(move c); return 3; }
  catch e Bad { host.note(3); }
  try { let values = fail_agg(host.make()); return 4; }
  catch e Failure {
    switch e {
    case Failure.Bad(values):
      if host.same(values[0], values[0]) != true { return 5; }
    }
  }
  try { let pair = Pair(p: host.make(), n: fail_partial()); return 6; }
  catch e Bad { host.note(6); }
  let d = host.make();
  let deferred = keep_deferred(move d);
  if host.same(deferred, deferred) != true { return 7; }
  host.note(9);
  return 0;
}
'''

    def foreign_cleanup_case(row):
        project = r.project('foreign-cleanup',
                            {'src/main.lang': foreign_cleanup_source}, binding)
        out, _ = r.emit(row, project, binding)
        observed, events = r.observe(row, out)
        require(observed['main_return'] == 0, observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        require(not error_events(events), events)
        metadata = json.loads((out / 'metadata.json').read_text())
        entry = '_f' + str(metadata['entry_symbol'])
        for name in ('a', 'b', 'c', 'd'):
            states = [state for state in observed['owner_states']
                      if state['function'] == entry and state['name'] == name]
            require(any(state['active'] for state in states), states)
            require(states and not states[-1]['active'], states)
        operations = [op for op in observed['operations']
                      if op[0] in ('host', 'foreign_drop')]
        def index_of(value, start=0):
            for index in range(start, len(operations)):
                if operations[index] == value:
                    return index
            raise AssertionError((value, operations))
        failed_arg = index_of(['host', 'ARG_FAIL', 2])
        note_after_arg = index_of(['host', 'NOTE', 2], failed_arg)
        require(any(op == ['foreign_drop']
                    for op in operations[failed_arg + 1:note_after_arg]),
                operations)
        made_b = index_of(['host', 'MAKE'], index_of(['host', 'NOTE', 2]))
        note_after_c = index_of(['host', 'NOTE', 3], made_b)
        require(any(op == ['foreign_drop']
                    for op in operations[made_b + 1:note_after_c]), operations)
        note_after_partial = index_of(['host', 'NOTE', 6], note_after_c)
        partial_make = max(index for index, op in enumerate(operations)
                           if op == ['host', 'MAKE'] and index < note_after_partial)
        require(any(op == ['foreign_drop']
                    for op in operations[partial_make + 1:note_after_partial]),
                operations)
        note_after_defer = index_of(['host', 'NOTE', 7], note_after_partial)
        defer_make = max(index for index, op in enumerate(operations)
                         if op == ['host', 'MAKE'] and index < note_after_defer)
        require(['host', 'SAME', True] in operations[defer_make + 1:note_after_defer],
                operations)
        note_after_return = index_of(['host', 'NOTE', 9], note_after_defer)
        require(any(op == ['foreign_drop']
                    for op in operations[note_after_defer + 1:note_after_return]),
                operations)
        require(len(observed['foreign_copies']) > 0, observed)
        require(len(observed['foreign_drops']) >= 5, observed)
        row['observation'] = observed

    r.add('foreign-ownership-cleanup', foreign_cleanup_case)

    def foreign_rejections_case(row):
        cases = [
            ('inout', 'func f(x inout host.Probe) Unit { } '
             'func main() i32 { return 0; }', 'E_CAPABILITY_UNAVAILABLE'),
            ('partial-move', 'struct S { p host.Probe; } '
             'func main() i32 throws py.Error { let s = S(p: host.make()); '
             'let p = move s.p; return 0; }', 'E_CAPABILITY_UNAVAILABLE'),
            ('undeclared-member', 'func main() i32 throws py.Error { '
             'let p = host.make(); p.missing(); return 0; }',
             'E_CAPABILITY_UNAVAILABLE'),
            ('read-after-move', 'func main() i32 throws py.Error { '
             'let p = host.make(); let q = move p; host.same(p, q); '
             'return 0; }', 'E_OWNERSHIP_USE_AFTER_MOVE'),
            ('double-move', 'func main() i32 throws py.Error { '
             'let p = host.make(); let q = move p; let r = move p; '
             'return 0; }', 'E_OWNERSHIP_USE_AFTER_MOVE'),
            ('borrow-alias', 'func f(a host.Probe, b move host.Probe) i32 '
             '{ return 0; } func main() i32 throws py.Error { '
             'let p = host.make(); return f(p, move p); }', 'E_CALL_ARGUMENT'),
            ('move-borrowed-parameter', 'func f(p host.Probe) i32 { '
             'let q = move p; return 0; } func main() i32 throws py.Error { '
             'return f(host.make()); }', 'E_CALL_ARGUMENT'),
        ]
        for name, body, code in cases:
            source = ('package main; import "std/py" as py; '
                      'import "t10host" as host; ' + body)
            project = r.project('foreign-reject-' + name,
                                {'src/main.lang': source}, binding)
            out, result = r.emit(row, project, binding, expect_success=False)
            require(result['exit'] == 1, result)
            data = json.loads(result['stdout'])
            codes = [d['code'] for d in data['diagnostics']]
            require(code in codes, (name, data))
            require(not (out / 'program.py').exists(), result)
            require(not (out / 'langrt.py').exists(), result)
        source = ('package main; import "std/py" as py; '
                  'import "t10host" as host; '
                  'func main() i32 throws py.Error { let p = host.make(); '
                  'let values Vec[host.Probe] = [p]; return 0; }')
        project = r.project('foreign-reject-32bit',
                            {'src/main.lang': source}, binding)
        manifest = (project / 'lang.toml').read_text().replace(
            'word_bits = 64', 'word_bits = 32')
        (project / 'lang.toml').write_text(manifest)
        out, result = r.emit(row, project, binding, expect_success=False)
        require(result['exit'] == 1, result)
        data = json.loads(result['stdout'])
        require(any(d['code'] == 'E_CAPABILITY_UNAVAILABLE'
                    for d in data['diagnostics']), data)
        require(not (out / 'program.py').exists(), result)
        row['rejections'] = cases

    r.add('foreign-ownership-rejections', foreign_rejections_case)

    internal_helper = '''package helper;
import "std/py" as py;
import "t10host" as host;
pub func fail() Unit throws py.Error { host.error_source(); }
'''
    internal_main = '''package main;
import "std/py" as py;
import "t10host" as host;
import "app/helper" as helper;
enum E { Bad; }
func bad_i64() i64 throws E { throw E.Bad; }
func main() i32 throws py.Error {
    var seen i32 = 0;
    try { host.take_i64(bad_i64()); return 1; } catch e E { host.note(1); seen += 1; }
    try { helper.fail(); return 2; } catch e py.Error { let copy = e; host.inspect(copy); seen += 2; }
    if seen != 3 { return 3; }
    return 0;
}
'''

    def internal_case(row):
        project = r.project('internal-cross-module',
                            {'src/main.lang': internal_main,
                             'src/helper/main.lang': internal_helper}, binding)
        out, _ = r.emit(row, project, binding)
        observed, events = r.observe(row, out)
        require(observed['main_return'] == 0, observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        require(non_error_events(events) == ['NOTE 1'], events)
        errors = error_events(events)
        require(len(errors) == 1, events)
        require('type=t10host.HostOuter' in errors[0], errors)
        require('operation=t10host.error_source' in errors[0], errors)
        require('cause=t10host.HostInner:inner cause' in errors[0], errors)
        require('/src/helper/main.lang' in errors[0], errors)
        require('TAKE' not in non_error_events(events), events)
        row['observation'] = observed
        row['events'] = events

    r.add('internal-cross-module', internal_case)

    escape_main = '''package main;
import "std/py" as py;
import "t10host" as host;
func internal_escape() Unit throws py.Error { host.raise_internal(0); }
func main() i32 {
    try { internal_escape(); } catch e py.Error { return 1; }
    return 0;
}
'''

    def internal_escape_case(row):
        project = r.project('internal-escape', {'src/main.lang': escape_main}, binding)
        out, _ = r.emit(row, project, binding)
        observed, events = r.observe(row, out)
        require('typed' in observed, observed)
        require(observed['typed']['type'] == 't10.internal', observed)
        require(observed['owners'] == [], observed)
        require(all(not borrows for borrows in observed['borrows']), observed)
        row['observation'] = observed

    r.add('internal-escape', internal_escape_case)

    def base_case(row, kind, expected_exit, marker, forbidden=()):
        source = f'''package main;
import "std/py" as py;
import "t10host" as host;
func main() Unit throws py.Error {{ host.raise_base({kind}); }}
'''
        project = r.project('base-' + str(kind), {'src/main.lang': source}, binding)
        out, _ = r.emit(row, project, binding)
        result = r.direct(row, out)
        if expected_exit is not None:
            require(result['exit'] == expected_exit, result)
        require(marker in result['stderr'], result)
        require('LANG_TYPED_ERROR' not in result['stderr'], result)
        for text in forbidden:
            require(text not in result['stderr'], result)
        row['result'] = result

    def keyboard_case(row):
        base_case(row, 0, None, 'KeyboardInterrupt')
        require(row['result']['exit'] != 0, row['result'])
    r.add('base-keyboard-interrupt', keyboard_case)
    r.add('base-system-exit', lambda row: base_case(
        row, 1, 7, '', forbidden=('LANG_PANIC',)))
    r.add('base-panic', lambda row: base_case(
        row, 2, 101, 'LANG_PANIC: host panic'))

    def bad_unit_case(row):
        source = '''package main;
import "std/py" as py;
import "t10host" as host;
func main() Unit throws py.Error { host.return_bad_unit(); }
'''
        project = r.project('bad-unit', {'src/main.lang': source}, binding)
        out, _ = r.emit(row, project, binding)
        result = r.direct(row, out)
        require(result['exit'] == 101, result)
        require('LANG_PANIC: foreign callable declared Unit returned a non-None value'
                in result['stderr'], result)
        require('LANG_TYPED_ERROR' not in result['stderr'], result)
        row['result'] = result

    r.add('bad-unit-contract', bad_unit_case)

    def no_throws_case(row):
        variant = json.loads(args.binding.read_text())
        for callable in variant['callables']:
            if callable['id'] == 't10host.note':
                callable['error_type'] = None
        variant_path = args.out / 'variants' / 'no-throws-binding.json'
        variant_path.parent.mkdir(parents=True, exist_ok=True)
        variant_path.write_text(json.dumps(variant, ensure_ascii=False, indent=2) + '\n')
        source = '''package main;
import "std/py" as py;
import "t10host" as host;
func main() Unit throws py.Error { host.note(1); }
'''
        project = r.project('no-throws', {'src/main.lang': source}, variant_path)
        out, result = r.emit(row, project, variant_path, expect_success=False)
        require(result['exit'] == 1, result)
        data = json.loads(result['stdout'])
        codes = [d['code'] for d in data['diagnostics']]
        messages = [d['message'] for d in data['diagnostics']]
        require('E_CAPABILITY_UNAVAILABLE' in codes, data)
        require(any('requires declared lang.py.Error' in message for message in messages),
                data)
        require(not (out / 'program.py').exists(), result)
        row['diagnostics'] = data['diagnostics']

    r.add('no-throws-external-callable', no_throws_case)

    for name, declaration in [
        ('foreign-value-error', 'func fail(x host.Probe) Unit throws host.Probe { throw x; }'),
        ('function-foreign-error', 'func fail(g func() host.Probe) Unit throws func() host.Probe { throw g; }'),
    ]:
        def unsupported_error_case(row, name=name, declaration=declaration):
            source = ('package main; import "std/py" as py; '
                      'import "t10host" as host; ' + declaration + ' func main() Unit {}')
            project = r.project(name, {'src/main.lang': source}, binding)
            out, result = r.emit(row, project, binding, expect_success=False)
            require(result['exit'] == 1, result)
            data = json.loads(result['stdout'])
            require(any(d['code'] == 'E_CAPABILITY_UNAVAILABLE' and d['primary'] is not None
                        and 'runtime_error declaration' in d['message']
                        for d in data['diagnostics']), data)
            require(not (out / 'program.py').exists() and not (out / 'langrt.py').exists(), result)
            row['diagnostics'] = data['diagnostics']
        r.add(name, unsupported_error_case)

    def missing_host_case(row):
        variant = json.loads(args.binding.read_text())
        variant['package_requirements'].append(
            {'package': 't10_missing_host', 'version_policy': 'test-only missing host'})
        variant['modules'].append(
            {'logical_path': 'missinghost', 'host_module': 't10_missing_host',
             'provider': 'python_package'})
        variant['callables'].append({
            'id': 'missinghost.ping', 'module': 'missinghost', 'name': 'ping',
            'call_kind': 'function', 'owner_type': None,
            'host_attribute': ['ping'], 'parameters': [],
            'return_type': 'Unit', 'error_type': 'lang.py.Error',
            'ownership': {'return': 'unit', 'storage': 'no_tensor_storage'},
            'effects': [], 'receiver_escapes': 'not_applicable'})
        variant_path = args.out / 'variants' / 'missing-host-binding.json'
        variant_path.parent.mkdir(parents=True, exist_ok=True)
        variant_path.write_text(json.dumps(variant, ensure_ascii=False, indent=2) + '\n')
        source = '''package main;
import "std/py" as py;
import "missinghost" as missing;
func main() Unit throws py.Error { missing.ping(); }
'''
        project = r.project('missing-host', {'src/main.lang': source}, variant_path)
        with (project / 'lang.toml').open('a') as manifest:
            manifest.write(f'''[dependencies.missinghost]
kind = "python"
binding = "{variant_path}"
host_package = "t10_missing_host"
''')
        out, _ = r.emit(row, project, variant_path)
        result = r.direct(row, out)
        require(result['exit'] != 0, result)
        require('HostEnvironmentError' in result['stderr'], result)
        require("cannot import declared host module 't10_missing_host'" in result['stderr'],
                result)
        require('LANG_TYPED_ERROR' not in result['stderr'], result)
        require('LANG_PANIC' not in result['stderr'], result)
        row['result'] = result

    r.add('missing-host-environment-error', missing_host_case)

    real_objects_source = '''package main;
import "std/py" as py;
import "numpy" as np;
import "torch" as torch;

// Ordinary host references: let copy and return must preserve identity.
func pass_array(value np.ndarray) np.ndarray {
    let copy = value;
    return copy;
}
func pass_tensor(value torch.Tensor) torch.Tensor {
    let copy = value;
    return copy;
}
func main() Unit throws py.Error {
    let values = np.linspace(0.0, 1.0, num: 3, dtype: np.float32);
    let array_copy = pass_array(values);
    let tensor = torch.tensor(array_copy, dtype: torch.float32, device: "cpu");
    let tensor_copy = pass_tensor(tensor);
    return;
}
'''
    real_error_source = '''package main;
import "std/py" as py;
import "numpy" as np;

func fail_negative_num() Unit throws py.Error {
    let values = np.linspace(0.0, 1.0, num: -1, dtype: np.float32);
    return;
}
func main() Unit throws py.Error {
    try { fail_negative_num(); } catch e py.Error { let saved = e; return; }
    return;
}
'''

    def real_project(name, source, packages):
        project = args.out / 'projects' / name
        if project.exists():
            shutil.rmtree(project)
        write(project / 'src/main.lang', source)
        dependencies = ''.join(f'''[dependencies.{package}]
kind = "python"
binding = "{args.example_binding}"
host_package = "{package}"
''' for package in packages)
        write(project / 'lang.toml', f'''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "python"
word_bits = 64
''' + dependencies)
        return project

    def real_objects_case(row):
        project = real_project('real-objects', real_objects_source,
                               ('numpy', 'torch'))
        out, _ = r.emit(row, project, args.example_binding)
        observed = r.real_observe(row, out, 'identity')
        require(observed['mode'] == 'identity', observed)
        require(observed['status'] == 'passed', observed)
        require(all(observed['acceptance'].values()), observed)
        require(observed['generated']['pass_array'] != observed['generated']['pass_tensor'],
                observed)
        row['observation'] = observed

    r.add('real-ndarray-tensor-identity', real_objects_case)

    def real_error_case(row):
        project = real_project('real-host-exception', real_error_source,
                               ('numpy',))
        out, _ = r.emit(row, project, args.example_binding)
        observed = r.real_observe(row, out, 'error')
        require(observed['mode'] == 'error', observed)
        require(observed['status'] == 'passed', observed)
        require(all(observed['acceptance'].values()), observed)
        require(observed['payload']['type_key'] == 'lang.py.Error', observed)
        require(observed['payload']['type_name'] == 'builtins.ValueError', observed)
        require(observed['payload']['operation'] == 'numpy.linspace.scalar', observed)
        row['observation'] = observed

    r.add('real-host-exception', real_error_case)

    def original04_case(row):
        source = args.root / 'baseline/samples/04_numpy_torch.lang'
        expected_sha = 'beb672124e6ccc697c8c129d2f7aa3bf21640de0b4af7a058eefa907fc6e6797'
        require(sha(source) == expected_sha, sha(source))
        project = args.out / 'projects' / 'original-04'
        if project.exists():
            shutil.rmtree(project)
        write(project / 'src/main.lang', source.read_text())
        write(project / 'lang.toml', f'''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "python"
word_bits = 64
[dependencies.numpy]
kind = "python"
binding = "{example_binding}"
host_package = "numpy"
[dependencies.torch]
kind = "python"
binding = "{example_binding}"
host_package = "torch"
''')
        out, emit_result = r.emit(row, project, args.example_binding)
        generated = json.loads((out / 'metadata.json').read_text())
        names = {s['source_name']: s['generated'] for s in generated['symbols']}
        require('train' in names, names)
        generated_result = r.command(
            row, [args.python, args.original_observer, out], out, timeout=300)
        require(generated_result['exit'] == 0, generated_result)
        generated_row = json.loads(generated_result['stdout'].splitlines()[-1])
        require(generated_row['status'] == 'passed', generated_row)
        require(all(generated_row['acceptance'].values()), generated_row)
        reference_result = r.command(
            row, [args.python, args.reference_observer, args.reference], out,
            timeout=300)
        require(reference_result['exit'] == 0, reference_result)
        reference_row = json.loads(reference_result['stdout'].splitlines()[-1])
        require(reference_row['status'] == 'passed', reference_row)
        require(all(reference_row['acceptance'].values()), reference_row)
        require(reference_row['sha256'] == sha(args.reference), reference_row)
        generated_loss = generated_row['result']['value']
        reference_loss = reference_row['result']['value']
        require(math.isclose(generated_loss, reference_loss, rel_tol=1e-5,
                             abs_tol=1e-6),
                (generated_loss, reference_loss))
        row['source_sha256'] = expected_sha
        row['reference_sha256'] = reference_row['sha256']
        row['generated_observation'] = generated_row
        row['reference'] = reference_row
        row['comparison'] = {
            'generated_loss': generated_loss,
            'reference_loss': reference_loss,
            'rtol': 1e-5,
            'atol': 1e-6,
            'passed': True,
        }

    r.add('original-04-cpu', original04_case)

    # shortcut: matched cases may run before an unknown-name error; collect
    # cases first if rejection before execution is needed.
    unknown_only = [name for name in args.only if name not in r.registered]
    if unknown_only:
        parser.error('unknown --only case(s): ' + ', '.join(unknown_only))

    status = 'passed' if all(row['status'] == 'passed' for row in r.rows) else 'failed'
    summary = {
        'status': status,
        'cases': r.rows,
        'commands': r.commands,
        'environment': {
            'python': str(args.python),
            'helper': str(args.helper),
            'binding_sha256': sha(args.binding),
            'example_binding_sha256': sha(args.example_binding),
            'reference_sha256': sha(args.reference),
        },
    }
    (args.out / 'results.json').write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'status': status,
                      'cases': {row['name']: row['status'] for row in r.rows},
                      'results': str(args.out / 'results.json')},
                     ensure_ascii=False))
    return 0 if status == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
