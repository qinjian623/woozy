"""Private observer for T10 generated programs; no host-language changes."""
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True
directory = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(directory))
import langrt as rt
import t10host
import program

metadata = json.loads((directory / 'metadata.json').read_text())
row = {'owners': [], 'borrows': 0, 'frames': 0,
       'binding_info': program._LANG_BINDING_INFO,
       'operations': [], 'owner_states': [], 'foreign_copies': [],
       'foreign_drops': []}
frames = []
names = {s['generated']: s['source_name'] for s in metadata['symbols']}
owner_last = {}

original_log = t10host._log


def log(*parts):
    row['operations'].append(['host'] + list(parts))
    return original_log(*parts)


t10host._log = log

original_copy = rt.ValueType.copy
original_drop = rt.ValueType.drop


def copy_value(self, value):
    result = original_copy(self, value)
    if self.kind == 'foreign':
        if result is not value:
            raise AssertionError('foreign copy changed host identity')
        row['foreign_copies'].append(id(value))
        row['operations'].append(['foreign_copy'])
    return result


def drop_value(self, value):
    if self.kind == 'foreign':
        row['foreign_drops'].append(id(value))
        row['operations'].append(['foreign_drop'])
    return original_drop(self, value)


rt.ValueType.copy = copy_value
rt.ValueType.drop = drop_value


def trace(frame, event, arg):
    if frame.f_code.co_filename == str(directory / 'program.py'):
        language_frame = frame.f_locals.get('_frame')
        if language_frame is not None and all(language_frame is not old for old in frames):
            frames.append(language_frame)
        for key, value in frame.f_locals.items():
            if key in names and isinstance(value, rt.Owner):
                state = (frame.f_code.co_name, names[key], id(value))
                active = value.value is not rt._DEAD
                if owner_last.get(state) != active:
                    owner_last[state] = active
                    row['owner_states'].append({
                        'function': state[0], 'name': state[1],
                        'active': active})
    return trace


main = getattr(program, '_f' + str(metadata['entry_symbol']))
sys.settrace(trace)
try:
    row['main_return'] = main()
except rt.Panic as error:
    row['panic'] = str(error)
except rt.TypedError as error:
    row['typed'] = {'type': error.type_key, 'value': repr(error.value)}
    error.drop()
finally:
    sys.settrace(None)
row['frames'] = len(frames)
row['borrows'] = [list(frame.borrows) for frame in frames]
for frame in frames:
    for key, owner in frame.owners.items():
        if owner.value is not rt._DEAD:
            row['owners'].append([key[0], key[1]])
print(json.dumps(row, ensure_ascii=False, default=repr))
