"""Test-only host module for T10 Python generation and py.Error bridging.

It is deliberately small and records observable events on stdout.  It is not
a NumPy/PyTorch conformance implementation.
"""
import langrt


class HostInner(Exception):
    pass


class HostOuter(Exception):
    pass


_MISSING = object()


def _log(*parts):
    print('T10HOST', *parts, flush=True)


class Probe:
    def __init__(self, value=0, broken=False):
        object.__setattr__(self, 'value', value)
        object.__setattr__(self, 'broken', broken)

    def __getattribute__(self, name):
        if name.startswith('__'):
            return object.__getattribute__(self, name)
        _log('READ', name)
        if name == 'broken':
            _log('READFAIL', name)
            try:
                raise HostInner('inner read cause')
            except HostInner as inner:
                raise HostOuter('broken read') from inner
        return object.__getattribute__(self, name)

    def ping(self, x):
        _log('CALL', 'ping', x)
        return None

    def fail_call(self):
        _log('CALL', 'fail_call')
        raise RuntimeError('call failed')

    def __call__(self, x):
        _log('CALL', 'call', x)
        return object.__getattribute__(self, 'value') + x


def make():
    _log('MAKE')
    return Probe(0)


def make_bad():
    _log('MAKE_BAD')
    return Probe(0, broken=True)


def arg(value):
    _log('ARG', value)
    return value


def arg_fail(value):
    _log('ARG_FAIL', value)
    raise ValueError('argument failed')


def same(a, b):
    result = a is b
    _log('SAME', result)
    return result


def set_value(probe, value):
    object.__setattr__(probe, 'value', value)
    _log('SET', value)


def get_value(probe):
    value = object.__getattribute__(probe, 'value')
    _log('GET', value)
    return value


def optional(a, b=_MISSING, c=_MISSING):
    _log('OPTIONAL', 'a=' + str(a),
         'b=' + ('MISSING' if b is _MISSING else str(b)),
         'c=' + ('MISSING' if c is _MISSING else str(c)))


def note(value):
    _log('NOTE', value)


def check_marker(probe):
    _log('MARKER', type(probe).__name__, probe is marker)


def inspect(error):
    cause = error.cause
    cause_text = 'None' if cause is None else (
        type(cause).__module__ + '.' + type(cause).__qualname__ + ':' + str(cause))
    _log('ERR', 'type=' + error.type_name,
         'message=' + str(error.message).replace('\n', '\\n'),
         'cause=' + cause_text.replace('\n', '\\n'),
         'traceback=' + ('yes' if 'HostOuter' in error.traceback_text else 'no'),
         'file=' + error.file,
         'begin=' + str(error.begin_byte),
         'end=' + str(error.end_byte),
         'operation=' + error.operation)


def error_source():
    try:
        raise HostInner('inner cause')
    except HostInner as inner:
        raise HostOuter('outer failure') from inner


def raise_internal(kind):
    if kind == 0:
        raise langrt.TypedError('t10.internal', 7, langrt.ValueType())
    raise langrt.TypedError('t10.internal', 8, langrt.ValueType())


def raise_base(kind):
    if kind == 0:
        raise KeyboardInterrupt('host interrupt')
    if kind == 1:
        raise SystemExit(7)
    if kind == 2:
        raise langrt.Panic('host panic')
    raise RuntimeError('unknown base kind')


def take_i64(value):
    _log('TAKE', value)
    return None


def return_none():
    return None


def return_bad_unit():
    return 1


marker = Probe(99)
