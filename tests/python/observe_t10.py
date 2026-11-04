"""Private observer for T10 generated programs; no host-language changes."""
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True
directory = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(directory))
import langrt as rt
import program

metadata = json.loads((directory / 'metadata.json').read_text())
row = {'owners': [], 'borrows': 0, 'frames': 0, 'binding_info': program._LANG_BINDING_INFO}
frames = []


def trace(frame, event, arg):
    if frame.f_code.co_filename == str(directory / 'program.py'):
        language_frame = frame.f_locals.get('_frame')
        if language_frame is not None and all(language_frame is not old for old in frames):
            frames.append(language_frame)
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
