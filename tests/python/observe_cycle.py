"""Observe an intentionally retained language reference cycle without encoding it.

Host Python references are only used to inspect the objects after the generated
program returns.  Language references come from Vec.refs; no extra retain is
performed by this observer.
"""
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
directory = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(directory))
import langrt as rt
import program

metadata = json.loads((directory / 'metadata.json').read_text())
objects = []
frees = []
frames = []

original_create = rt.Vec.create
original_release = rt.Vec.release


def create(element, values):
    result = original_create(element, values)
    objects.append(result)
    return result


def release(value):
    index = objects.index(value)
    original_release(value)
    if value.refs == 0:
        assert index not in frees, ('duplicate free', index)
        frees.append(index)


rt.Vec.create = staticmethod(create)
rt.Vec.release = release


def trace(frame, event, arg):
    if frame.f_code.co_filename == str(directory / 'program.py'):
        language_frame = frame.f_locals.get('_frame')
        if language_frame is not None and all(
                language_frame is not old for old in frames):
            frames.append(language_frame)
    return trace


main = getattr(program, '_f' + str(metadata['entry_symbol']))
sys.settrace(trace)
try:
    result = main()
finally:
    sys.settrace(None)

alive = [dict(object=i, refs=obj.refs) for i, obj in enumerate(objects)
         if obj.refs]
row = dict(main_return=result, alive=alive, frees=frees,
           frames_clean=len(frames), self_cycle=False)
assert len(alive) == 1, row
index = alive[0]['object']
obj = objects[index]
assert obj.refs > 0, row
assert index not in frees, row
assert obj.items, row
assert obj.items[0][1] is obj, row
assert not any(frame.borrows for frame in frames), 'unended call borrow'
assert not any(owner.value is not rt._DEAD for frame in frames
               for owner in frame.owners.values()), 'live frame owner'
row['self_cycle'] = True
print(json.dumps(row, ensure_ascii=False))
