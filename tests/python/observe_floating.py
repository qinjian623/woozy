"""Call unmodified CLI output using metadata from its concrete verified HIR."""
import importlib.util
import inspect
import json
import math
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
directory, spec_file = map(Path, sys.argv[1:])
sys.path.insert(0, str(directory))
import langrt
spec = importlib.util.spec_from_file_location('floating_program', directory / 'program.py')
program = importlib.util.module_from_spec(spec)
spec.loader.exec_module(program)
metadata = json.loads((directory / 'metadata.json').read_text())
functions = {f['source_name']: f for f in metadata['functions']}


def decode(spec):
    if spec['type'] in ('f32', 'f64'):
        return struct.unpack('>f' if spec['type'] == 'f32' else '>d', bytes.fromhex(spec['bits']))[0]
    return spec.get('value')


def encode(value, typ):
    if typ in ('f32', 'f64'):
        assert type(value) is float, (type(value), typ)
        fmt = '>f' if typ == 'f32' else '>d'
        raw = struct.pack(fmt, value)
        # Packing alone could hide an incorrectly unrounded f32 return.
        assert math.isnan(value) or struct.unpack(fmt, raw)[0] == value, value
        category = ('nan' if math.isnan(value) else 'infinity' if math.isinf(value)
                    else 'zero' if value == 0 else 'finite')
        return dict(type=typ, **{'class': category}, bits=None if category == 'nan' else raw.hex(),
                    sign=None if category == 'nan' else 'negative' if math.copysign(1.0, value) < 0 else 'positive')
    assert (typ == 'bool' and type(value) is bool) or (typ == 'i32' and type(value) is int) or (typ == 'Unit' and value is None), (value, typ)
    if typ == 'bool':
        return dict(type=typ, **{'class': 'bool'}, value=value)
    return dict(type=typ, value=value)


rows = []
for probe in json.loads(spec_file.read_text()):
    f = functions[probe['function']]
    supplied = probe.get('arguments', [])
    assert f['parameters'] == [a['type'] for a in supplied], (probe, f)
    fn = getattr(program, f['generated'])
    assert len(inspect.signature(fn).parameters) == len(supplied)
    args = [langrt.Cell(decode(a)) if a.get('cell') else decode(a) for a in supplied]
    row = dict(function=probe['function'], signature=f, arguments=supplied)
    try:
        result = fn(*args)
        row['result'] = encode(result, f['result'])
        row['outcome'] = 'return'
    except langrt.TypedError as error:
        row.update(outcome='typed', error_key=error.type_key)
        row['error_value'] = encode(error.value[1][0], probe['error_value']['type'])
    except langrt.Panic as error:
        row.update(outcome='panic', panic=str(error))
    row['cells'] = [encode(arg.value, a['type']) for arg, a in zip(args, supplied) if a.get('cell')]
    rows.append(row)
print(json.dumps(rows, allow_nan=False))
