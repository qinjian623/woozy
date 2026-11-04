"""Controlled observer of unmodified generated Python, in a fresh process."""
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
functions = {name: source for name, source in names.items() if name.startswith('_f')}
row = dict(events=[], calls=[], returns=[], errors=[], raised=[], locals={}, counter_events=[])


def value(x):
    if isinstance(x, rt.Cell):
        return value(x.value)
    if isinstance(x, tuple):
        return [value(item) for item in x]
    if callable(x):
        return '<function>'
    return x


def wrap(original, source):
    def call(*args):
        row['calls'].append(dict(function=source, args=[value(x) for x in args]))
        if source == 'mark':
            row['events'].append(args[1])
        if source == 'event':
            row['events'].append(args[0])
        try:
            result = original(*args)
        except rt.TypedError as error:
            if source == 'read_value':
                row['counter_events'].append(args[1].value)
            row['errors'].append(dict(function=source, type_key=error.type_key,
                                      value=value(error.value)))
            raise
        row['returns'].append(dict(function=source, value=value(result)))
        if source == 'read_value':
            row['counter_events'].append(args[1].value)
        return result
    return call


def trace(frame, event, arg):
    if frame.f_code.co_filename != str(directory / 'program.py'):
        return trace
    if event == 'exception' and isinstance(arg[1], rt.TypedError):
        error = arg[1]
        row['raised'].append(dict(type_key=error.type_key, value=value(error.value)))
    if event == 'return':
        source = functions.get(frame.f_code.co_name)
        if source:
            row['locals'][source] = {key: value(x) for key, x in frame.f_locals.items()
                                     if key in names}
    return trace


for name, source in functions.items():
    setattr(program, name, wrap(getattr(program, name), source))
main = getattr(program, '_f' + str(metadata['entry_symbol']))
sys.settrace(trace)
try:
    row['main_return'] = value(main())
except rt.Panic as error:
    row['panic'] = str(error)
except rt.TypedError as error:
    row['typed'] = dict(type_key=error.type_key, value=value(error.value))
finally:
    sys.settrace(None)

# Observe Panic's Cell through the public no-entrypoint API as well.
if 'panic' in row:
    normal_events = row['events']
    row['events'] = []
    probe = next(name for name, source in functions.items() if source == 'probe')
    log = rt.Cell(0)
    try:
        getattr(program, probe)(log)
    except rt.Panic as error:
        row['probe_panic'] = dict(reason=str(error), log=log.value)
    else:
        raise AssertionError('probe returned instead of Panic')
    row['probe_events'] = row['events']
    row['events'] = normal_events

if sys.argv[2] == 'typed-boundary':
    signal = next(name for name, source in functions.items() if source == 'signal')
    controls = [rt.Panic('injected'), rt.SwitchBreak(27), rt.LoopContinue(31),
                KeyboardInterrupt(), SystemExit(37), RuntimeError('host-error'),
                rt.TypedError('app.F', (999, ()))]
    row['control_events'] = []
    normal_events = row['events']
    for error in controls:
        def fail():
            raise error
        setattr(program, signal, fail)
        row['events'] = []
        try:
            main()
        except BaseException as escaped:
            assert escaped is error, (type(error), type(escaped))
        else:
            raise AssertionError('typed catch swallowed ' + type(error).__name__)
        expected = [] if isinstance(error, rt.Panic) else [3]
        assert row['events'] == expected, (type(error).__name__, row['events'])
        row['control_events'].append(dict(type=type(error).__name__, events=row['events']))
    row['events'] = normal_events

print(json.dumps(row, sort_keys=True))
