"""Private observer: holds Python references but never language owners."""
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
directory = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(directory))
import langrt as rt
import program

metadata = json.loads((directory / 'metadata.json').read_text())
names = {s['generated']: s['source_name'] for s in metadata['symbols']}
row = dict(allocations=[], frees=[], operations=[], events=[], errors=[], returns=[], snapshots={})
objects = []
frames = []


def encode(value):
    if value is rt._DEAD:
        return '<inactive>'
    if isinstance(value, (rt.Owner, rt.Cell)):
        return encode(value.value)
    if isinstance(value, rt.Vec):
        return dict(object=objects.index(value), items=[encode(x) for x in value.items])
    if isinstance(value, tuple):
        return [encode(x) for x in value]
    if callable(value):
        return '<function>'
    return value


original_create = rt.Vec.create
original_release = rt.Vec.release
original_drop = rt.ValueType.drop


def create(element, values):
    result = original_create(element, values)
    objects.append(result)
    index = len(objects) - 1
    row['allocations'].append(dict(object=index, items=encode(tuple(values))))
    row['operations'].append(['allocate', index])
    return result


def release(value):
    index = objects.index(value)
    row['operations'].append(['release', index, value.refs])
    assert value.refs > 0, ('duplicate release', index)
    original_release(value)
    if value.refs == 0:
        assert index not in row['frees'], ('duplicate free', index)
        row['frees'].append(index)
        row['operations'].append(['free', index])


def drop(ops, value):
    if ops.kind == 'trivial':
        row['operations'].append(['drop_value', encode(value)])
    original_drop(ops, value)


rt.Vec.create = staticmethod(create)
rt.Vec.release = release
rt.ValueType.drop = drop


def wrap(original, source):
    def call(*args):
        if source == 'mark':
            row['events'].append(encode(args[-1]))
            row['operations'].append(['event', encode(args[-1])])
        try:
            result = original(*args)
        except rt.TypedError as error:
            row['errors'].append(dict(function=source, type=error.type_key, value=encode(error.value)))
            raise
        row['returns'].append(dict(function=source, value=encode(result)))
        return result
    return call


for name, source in names.items():
    if name.startswith('_f'):
        setattr(program, name, wrap(getattr(program, name), source))


def trace(frame, event, arg):
    if frame.f_code.co_filename == str(directory / 'program.py'):
        language_frame = frame.f_locals.get('_frame')
        if language_frame is not None and all(language_frame is not old for old in frames):
            frames.append(language_frame)
        for key, value in frame.f_locals.items():
            if key in names and not key.startswith('_f'):
                encoded = encode(value)
                if encoded != '<inactive>':
                    if isinstance(value, rt.Cell) and row['snapshots'].get(names[key]) != encoded:
                        row['operations'].append(['binding', names[key], encoded])
                    row['snapshots'][names[key]] = encoded
    return trace


main = getattr(program, '_f' + str(metadata['entry_symbol']))
sys.settrace(trace)
try:
    row['main_return'] = main()
except rt.Panic as error:
    row['panic'] = str(error)
except rt.TypedError as error:
    row['typed'] = dict(type=error.type_key, value=encode(error.value))
    error.drop()
finally:
    sys.settrace(None)
row['alive'] = [dict(object=i, refs=obj.refs) for i, obj in enumerate(objects) if obj.refs]
if 'panic' not in row:
    assert not row['alive'], row
    assert len(row['frees']) == len(objects), row
    assert not any(frame.borrows for frame in frames), 'unended call borrow'
    assert not any(owner.value is not rt._DEAD for frame in frames
                   for owner in frame.owners.values()), 'live language owner after completion'
    row['frames_clean'] = len(frames)
print(json.dumps(row, ensure_ascii=False))
