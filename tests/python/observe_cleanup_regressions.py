"""Observe generated code without replacing source-language behavior."""
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
out = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(out))
import langrt as rt
import program

metadata = json.loads((out / 'metadata.json').read_text())
names = {s['generated']: s['source_name'] for s in metadata['symbols']}
row = dict(events=[], raised_keys=[], main_locals={})


def trace(frame, event, arg):
    if frame.f_code.co_filename != str(out / 'program.py'):
        return trace
    if event == 'exception' and isinstance(arg[1], rt.TypedError):
        row['raised_keys'].append(arg[1].type_key)
    if event == 'return' and frame.f_code.co_name == '_f' + str(metadata['entry_symbol']):
        row['main_locals'] = {names[key]: value.value if isinstance(value, rt.Cell) else value
            for key, value in frame.f_locals.items()
            if key in names and (isinstance(value, (rt.Cell, int, bool)) or value is None)}
    return trace


def observe_events(original):
    def call(*args):
        row['events'].append(args[-1])
        return original(*args)
    return call


for generated, name in names.items():
    if name in ('event', 'mark') and generated.startswith('_f'):
        setattr(program, generated, observe_events(getattr(program, generated)))
main = getattr(program, '_f' + str(metadata['entry_symbol']))
sys.settrace(trace)
try:
    row['main_return'] = main()
except rt.TypedError as error:
    row['typed'] = error.type_key
except rt.Panic as error:
    row['panic'] = str(error)
finally:
    sys.settrace(None)
for generated, name in names.items():
    if name == 'probe' and generated.startswith('_f'):
        log = rt.Cell(0)
        normal_events = row['events']
        row['events'] = []
        try:
            row['probe_return'] = getattr(program, generated)(log)
        except rt.TypedError as error:
            row['probe_typed'] = error.type_key
        except rt.Panic as error:
            row['probe_panic'] = str(error)
        row['probe_log'] = log.value
        row['probe_events'], row['events'] = row['events'], normal_events
print(json.dumps(row, sort_keys=True))
