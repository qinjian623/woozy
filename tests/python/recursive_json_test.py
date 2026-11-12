#!/usr/bin/env python3
"""Recursive type descriptors and JSON cleanup through the real Python backend."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]


def runtime_checks():
    spec = importlib.util.spec_from_file_location('rt', ROOT / 'runtime/python/langrt.py')
    rt = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rt)
    limit = sys.getrecursionlimit()
    events = []

    class Label:
        def drop(self, value):
            events.append(value)

    value = rt.ValueType('enum')
    reference = rt.ValueType('vec')
    member = rt.ValueType('struct', (Label(), value))
    value.variants = {0: (), 1: (reference,), 2: (reference,)}
    for objects in (False, True):
        tree, allocated = (0, ()), []
        for index in range(256):
            item = (index, tree) if objects else tree
            items = rt.Vec.create(member if objects else value, [item])
            allocated.append(items)
            tree = (2 if objects else 1, (items,))
        alias = value.copy(tree)
        assert allocated[-1].refs == 2
        value.drop(tree)
        assert allocated[-1].refs == 1 and not events
        value.drop(alias)
        assert all(item.refs == 0 and not item.items for item in allocated)
        assert events == (list(range(256)) if objects else [])
        events.clear()

    # Aggregate copy remains a value copy; only Vec fields retain aliases.
    ops, item = rt.ValueType(), 7
    for _ in range(2000):
        ops, item = rt.ValueType('struct', (ops,)), (item,)
    copied = ops.copy(item)
    ops.drop(item)
    ops.drop(copied)
    for _ in range(2000):
        copied = copied[0]
    assert copied == 7
    try:
        value.copy((99, ()))
    except KeyError:
        pass
    else:
        raise AssertionError('unknown enum tag accepted')
    assert sys.getrecursionlimit() == limit


DECLARATIONS = '''
enum Value {{ Null; {variants} }}
struct Member {{ name string; value Value; }}
'''
VALUE_PROGRAM = '''
import "std/vec" as vec;
{declarations}
func main() i32 {{
    let children = vec.empty[Value]();
    vec.push(children, Value.Null);
    let members = vec.empty[Member]();
    vec.push(members, Member(name: "x", value: Value.Array(children)));
    let value = Value.Object(members);
    let alias = value;
    switch alias {{
        case Value.Object(fields): {{
            if vec.len(fields) != 1 || fields[0].name != "x" {{ return 1; }}
            switch fields[0].value {{
                case Value.Array(items): if vec.len(items) != 1 {{ return 2; }}
                case _: return 3;
            }}
        }}
        case _: return 4;
    }}
    return 0;
}}
'''
JSON_DEPTH = r'''
import "std/json" as json;
import "std/text" as text;
func roundtrip(source string) bool throws json.Error {
    let value = json.parse(source);
    return json.serialize(value) == source;
}
func rejected(source string) bool {
    try { let value = json.parse(source); return false; }
    catch error json.Error {
        switch error { case json.Error.DepthLimit: return true; case _: return false; }
    }
}
func main() i32 throws json.Error {
    let array = text.repeat("[", 256) + "null" + text.repeat("]", 256);
    let object = text.repeat("{\"x\":", 256) + "null" + text.repeat("}", 256);
    if !roundtrip(array) || !roundtrip(object) { return 1; }
    if !rejected("[" + array + "]") || !rejected("{\"x\":" + object + "}") { return 2; }
    return 0;
}
'''
JSON_CYCLE = '''
import "std/json" as json;
import "std/vec" as vec;
func main() i32 {
    let items = vec.empty[json.Value]();
    let value = json.Value.Array(items);
    vec.push(items, value);
    try { let encoded = json.serialize(value); return 1; }
    catch error json.Error {
        switch error { case json.Error.DepthLimit: return 0; case _: return 2; }
    }
}
'''
OBSERVER = '''
import gc, json, runpy, sys
import langrt as rt
gc.disable()
def forbid_host_collection(*args, **kwargs):
    raise AssertionError('recursive JSON cleanup must not use CPython gc.collect')
gc.collect = forbid_host_collection
allocated = []
create = rt.Vec.create
def observe(element, values):
    result = create(element, values)
    allocated.append(result)
    return result
rt.Vec.create = staticmethod(observe)
limit = sys.getrecursionlimit()
try:
    runpy.run_path(sys.argv[1], run_name='__main__')
except SystemExit as error:
    assert error.code in (None, 0), error.code
assert sys.getrecursionlimit() == limit
alive = sum(item.refs > 0 for item in allocated)
assert alive == int(sys.argv[2]), (alive, [(i, item.refs) for i, item in enumerate(allocated) if item.refs])
assert all(item.refs > 0 or not item.items for item in allocated)
collected = rt._gc_stats()['total_collected']
assert collected >= int(sys.argv[3]), collected
print(json.dumps(dict(allocated=len(allocated), alive=alive,
                      collected=collected, recursionlimit=limit)))
'''


def command(argv, timeout):
    result = subprocess.run(list(map(str, argv)), capture_output=True, text=True,
                            timeout=timeout)
    assert result.returncode == 0, (argv, result.returncode, result.stdout, result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--python', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    version = command([args.python, '-c', 'import sys; print(sys.version_info[:2])'], 20)
    assert version.strip() == '(3, 12)', version
    runtime_checks()
    variants = ('Array(value Vec[Value]); Object(value Vec[Member]);',
                'Object(value Vec[Member]); Array(value Vec[Value]);')
    cases = {}
    for index, order in enumerate(variants):
        declarations = DECLARATIONS.format(variants=order)
        cases[f'variants-{index}'] = VALUE_PROGRAM.format(declarations=declarations), 0
        enum, member = declarations.strip().split('\n')
        cases[f'member-first-{index}'] = VALUE_PROGRAM.format(declarations=member + '\n' + enum), 0
    cases['json-depth-256'] = JSON_DEPTH, 0
    # The normal Python runner now reclaims pure-Woozy strong cycles.
    cases['json-cycle'] = JSON_CYCLE, 0
    report = {}
    for name, (source, alive) in cases.items():
        directory = args.out.resolve() / name
        (directory / 'src').mkdir(parents=True, exist_ok=True)
        (directory / 'src/main.wz').write_text('package app;\n' + source)
        manifest = directory / 'lang.toml'
        manifest.write_text('schema_version = 1\n[package]\nname = "app"\nsource_root = "src"\n'
                            'entry = "app"\n[build]\nbackend = "python"\nword_bits = 64\n')
        output = directory / 'python'
        command([args.cli, 'build', '--project', manifest, '--backend', 'python',
                 '--out-dir', output, '--format', 'json'], 60)
        command([args.python, '-s', output / 'program.py'], 20)
        observer = output / 'observe.py'
        observer.write_text(OBSERVER)
        report[name] = json.loads(command([args.python, '-s', observer,
                                          output / 'program.py', alive,
                                          int(name == 'json-cycle')], 20))
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
