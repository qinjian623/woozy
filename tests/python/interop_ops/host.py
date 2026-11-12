"""Test-only host fixture for FFI02 declared property/index/slice operations.

Only ordinary Python attributes, properties and the `__getitem__`/`__setitem__`
protocol are used.  The module is deliberately small and logs every observable
operation so the runner can verify evaluation counts and ordering.
"""
import langrt


class HostInner(Exception):
    pass


class HostOuter(Exception):
    pass


def _log(*parts):
    print('INTEROP', *parts, flush=True)


class Key:
    def __init__(self, value):
        self.value = value

    def __index__(self):
        _log('KEY', self.value)
        return self.value

    def __repr__(self):
        return 'Key(%d)' % self.value


class Inner:
    def __init__(self, value):
        self.value = value


class Probe:
    def __init__(self):
        self._value = 0.0
        self._text = ''
        self.readonly = 7
        self.nested = Inner(5)
        self.get_count = 0
        self.set_count = 0
        self.index_get_count = 0
        self.index_set_count = 0
        self.explode_count = 0
        self.items = [10, 20, 30]

    @property
    def value(self):
        self.get_count += 1
        _log('GET', 'value', self.get_count)
        return self._value

    @value.setter
    def value(self, replacement):
        self.set_count += 1
        _log('SET', 'value', self.set_count, replacement)
        self._value = replacement

    @property
    def child(self):
        _log('GET', 'child')
        return self

    @property
    def text(self):
        _log('GET', 'text')
        return self._text

    @text.setter
    def text(self, replacement):
        _log('SET', 'text', replacement)
        self._text = replacement

    @property
    def explode(self):
        self.explode_count += 1
        _log('EXPLODE', 'get')
        try:
            raise HostInner('inner property cause')
        except HostInner as inner:
            raise HostOuter('property getter failed') from inner

    @property
    def explode_set(self):
        return self._value

    @explode_set.setter
    def explode_set(self, replacement):
        self.explode_count += 1
        _log('EXPLODE', 'set', replacement)
        try:
            raise HostInner('inner setter cause')
        except HostInner as inner:
            raise HostOuter('property setter failed') from inner

    @property
    def bad_type(self):
        # Declared string but deliberately returns a non-str value.
        return 7

    def __getitem__(self, key):
        self.index_get_count += 1
        index = int(key)
        _log('GETITEM', index, self.index_get_count)
        return self.items[index]

    def __setitem__(self, key, replacement):
        self.index_set_count += 1
        index = int(key)
        _log('SETITEM', index, self.index_set_count, replacement)
        self.items[index] = replacement


class NoIndex:
    pass


class ReadOnlyBox:
    def __init__(self):
        self.items = [10, 20, 30]

    def __getitem__(self, key):
        if isinstance(key, slice):
            _log('READONLY_SLICE', key.start, key.stop)
            return sum(self.items[key])
        _log('READONLY_GET', int(key))
        return self.items[int(key)]


def make():
    _log('MAKE')
    return Probe()


def make_no_index():
    _log('MAKE_NO_INDEX')
    return NoIndex()


def make_readonly():
    _log('MAKE_READONLY')
    return ReadOnlyBox()


def make_key(value):
    _log('MAKE_KEY', value)
    return Key(value)


def note_i64(value):
    _log('NOTE_I64', value)
    return value


def note_f64(value):
    _log('NOTE_F64', value)
    return value


def probe_counts(probe):
    counts = (probe.get_count, probe.set_count,
              probe.index_get_count, probe.index_set_count)
    _log('COUNTS', *counts)
    return (counts[0] * 1000000 + counts[1] * 10000 +
            counts[2] * 100 + counts[3])


def explode_count(probe):
    _log('EXPLODE_COUNT', probe.explode_count)
    return probe.explode_count


def inspect(error):
    """Return a bitmask describing the preserved py.Error payload."""
    flags = 0
    if isinstance(error.cause, HostInner):
        flags |= 1
    if 'HostOuter' in error.traceback_text:
        flags |= 2
    if isinstance(error.exception, HostOuter):
        flags |= 4
    if error.operation and error.operation.startswith('interophost.'):
        flags |= 8
    if error.file.endswith('.wz'):
        flags |= 16
    _log('ERR', 'type=' + error.type_name,
         'message=' + str(error.message).replace('\n', '\\n'),
         'cause=' + ('none' if error.cause is None else
                     type(error.cause).__name__),
         'operation=' + error.operation,
         'file=' + error.file)
    return flags
