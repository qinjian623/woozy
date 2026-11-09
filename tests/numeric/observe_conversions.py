"""Call unchanged Python conversion probes and expose their concrete result values.

The compiler output and metadata come from the test-only Python helper. Inputs
are rebuilt from raw IEEE bits with struct, and float results are packed at
their declared width so an unrounded f32 value cannot hide behind binary64.
"""
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
spec = importlib.util.spec_from_file_location('conversion_program',
                                              directory / 'program.py')
program = importlib.util.module_from_spec(spec)
spec.loader.exec_module(program)
metadata = json.loads((directory / 'metadata.json').read_text())
functions = {f['source_name']: f for f in metadata['functions']}


def decode(row):
    typ = row['type']
    if typ == 'f32':
        return struct.unpack('>f', bytes.fromhex(row['bits']))[0]
    if typ == 'f64':
        return struct.unpack('>d', bytes.fromhex(row['bits']))[0]
    value = row['value']
    if typ in ('i8', 'i16', 'i32', 'i64', 'isize'):
        bits = 64 if typ == 'isize' else int(typ[1:])
        if value >= 1 << (bits - 1):
            value -= 1 << bits
    return value


def encode(value, typ):
    if typ in ('f32', 'f64'):
        assert type(value) is float, (type(value), typ)
        fmt = '>f' if typ == 'f32' else '>d'
        raw = struct.pack(fmt, value)
        if math.isnan(value):
            return dict(type=typ, **{'class': 'nan'})
        assert struct.unpack(fmt, raw)[0] == value, value
        category = ('infinity' if math.isinf(value)
                    else 'zero' if value == 0 else 'finite')
        return dict(type=typ, **{'class': category}, bits=raw.hex(),
                    sign='negative' if math.copysign(1.0, value) < 0
                    else 'positive')
    assert typ in ('i8', 'u8', 'i16', 'u16', 'i32', 'u32', 'i64', 'u64',
                   'isize', 'usize'), typ
    return dict(type=typ, value=value)


rows = []
for probe in json.loads(spec_file.read_text()):
    f = functions[probe['name']]
    supplied = probe['arguments']
    assert len(f['parameters']) == len(supplied), (probe, f)
    args = [decode(dict(type=f['parameters'][i],
                        bits=f'{value:0{width // 4}x}'
                        if f['parameters'][i] in ('f32', 'f64')
                        else None,
                        value=value))
            for i, (value, width) in enumerate(zip(supplied,
                                                   probe['arg_widths']))]
    fn = getattr(program, f['generated'])
    assert len(inspect.signature(fn).parameters) == len(args)
    row = dict(function=probe['name'], signature=f, arguments=probe['arguments'])
    try:
        row['result'] = encode(fn(*args), f['result'])
        row['outcome'] = 'return'
    except langrt.Panic as error:
        row.update(outcome='panic', panic=str(error))
    rows.append(row)
print(json.dumps(rows, allow_nan=False))
