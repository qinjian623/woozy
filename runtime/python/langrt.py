"""Value runtime. Generated programs pass explicit target widths."""
import importlib
import math
import re
import struct
import sys
import traceback

RUNTIME_VERSION = 'compiler-dev/1-python-values/8'
PANIC_MARKER = 'LANG_PANIC: '
PANIC_EXIT_CODE = 101
TYPED_ERROR_MARKER = 'LANG_TYPED_ERROR: '
HOST_ERROR_MARKER = 'LANG_HOST_ERROR: '


class Panic(BaseException):
    pass


class HostEnvironmentError(RuntimeError):
    """A declared host module could not be imported.

    This is an execution-environment failure, not a source py.Error.  It is
    raised before the external-operation bridge so it is never converted into
    a typed language error.
    """


class Error:
    """Payload of the fixed lang.py.Error type.

    It keeps the original host exception, its cause and formatted traceback,
    plus the source operation that failed.  Copying language ownership shares
    this payload; it never copies the host exception or takes a TypedError
    payload twice.
    """
    __slots__ = ('exception', 'cause', 'traceback_text', 'file',
                 'begin_byte', 'end_byte', 'operation', 'type_name', 'message')

    def __init__(self, exception, file, begin_byte, end_byte, operation):
        self.exception = exception
        self.cause = exception.__cause__
        self.traceback_text = ''.join(traceback.format_exception(
            type(exception), exception, exception.__traceback__))
        self.file = file
        self.begin_byte = begin_byte
        self.end_byte = end_byte
        self.operation = operation
        cls = type(exception)
        self.type_name = cls.__module__ + '.' + cls.__qualname__
        self.message = str(exception)

    def copy(self):
        return self

    def drop(self):
        # Python owns the actual exception reference.  The language owner
        # only releases this payload slot; it does not deep-copy or destroy
        # the host exception.
        pass

    def describe(self):
        message = self.message.replace('\n', '\\n')
        return (f"{self.type_name}: {message} at {self.file}:"
                f"{self.begin_byte}:{self.end_byte} ({self.operation})")


class TypedError(Exception):
    """Language error only. The key is logical, never a session TypeId."""
    __slots__ = ('type_key', 'value', 'ops')

    def __init__(self, type_key, value, ops=None):
        super().__init__(type_key)
        self.type_key, self.value, self.ops = type_key, value, ops


    def take(self):
        if self.value is _DEAD:
            raise RuntimeError('typed error payload already transferred')
        value, self.value = self.value, _DEAD
        self.ops = None
        return value

    def drop(self):
        if self.value is not _DEAD and self.ops is not None:
            value, self.value = self.value, _DEAD
            self.ops.drop(value)
            self.ops = None


class LoopContinue(BaseException):
    """Unwind the iteration's lexical cleanups before running its step."""
    __slots__ = ('target',)

    def __init__(self, target):
        self.target = target


class Cell:
    __slots__ = ('value',)

    def __init__(self, value):
        self.value = value


class Field:
    """Short-lived field place; never stored in a source-language value."""
    __slots__ = ('root', 'path')

    def __init__(self, base, index):
        if isinstance(base, Index):
            self.root, self.path = base, (index,)
        elif isinstance(base, (Cell, Owner)):
            self.root, self.path = base, (index,)
        else:
            self.root, self.path = base.root, base.path + (index,)

    @property
    def value(self):
        result = self.root.value
        for index in self.path:
            result = result[index]
        return result

    @value.setter
    def value(self, replacement):
        if isinstance(self.root, Index):
            # The indexed element remains the real shared storage. Replace
            # only this field path and copy the sibling fields that stay in
            # the object, so the old element's drop does not release them.
            self.root.replace_field(self.path, replacement)
            return
        def replace(value, depth):
            if depth == len(self.path):
                return replacement
            index = self.path[depth]
            return value[:index] + (replace(value[index], depth + 1),) + value[index + 1:]
        self.root.value = replace(self.root.value, 0)


class SwitchBreak(BaseException):
    """Internal control transfer, separate from Panic and typed errors."""
    __slots__ = ('target',)

    def __init__(self, target):
        self.target = target


def wrap(value, bits, signed):
    value &= (1 << bits) - 1
    if signed and value >= (1 << (bits - 1)):
        value -= 1 << bits
    return value


def convert(value, bits, signed):
    lower = -(1 << (bits - 1)) if signed else 0
    upper = (1 << (bits - int(signed))) - 1
    if not lower <= value <= upper:
        raise Panic('integer conversion overflow')
    return value


def float_to_int(value, bits, signed):
    """Truncate a finite f32/f64 value towards zero, then range-check it."""
    if not math.isfinite(value):
        raise Panic('float to integer conversion of NaN or infinity')
    # The source value is an exact IEEE value; math.trunc returns its exact
    # mathematical integer (arbitrary precision), never a second rounding.
    truncated = math.trunc(value)
    lower = -(1 << (bits - 1)) if signed else 0
    upper = (1 << (bits - int(signed))) - 1
    if not lower <= truncated <= upper:
        raise Panic('float to integer conversion overflow')
    return truncated


def int_to_float(value, bits):
    """Round one exact integer directly to the destination IEEE precision."""
    return _float_round(abs(value), 1, bits, value < 0)


def float_to_float(value, bits):
    """Round an IEEE source value once to the destination IEEE precision.

    The source is an exact Python float (binary64) produced by the existing
    f32/f64 runtime. NaN/infinity keep their class; a finite ratio is rounded
    with RNE, including subnormals, signed zero and overflow to signed
    infinity. This is a conversion, never the float-to-integer Panic path.
    """
    if bits not in (32, 64):
        raise ValueError('unsupported IEEE width')
    if math.isnan(value) or math.isinf(value):
        return value
    negative = math.copysign(1.0, value) < 0
    if value == 0:
        return ieee(int(negative) << (bits - 1), bits)
    numerator, denominator = value.as_integer_ratio()
    return _float_round(abs(numerator), denominator, bits, negative)


def unary(op, value, bits, signed):
    if op == 'not':
        return not value
    if op == 'neg':
        return wrap(-value, bits, signed)
    if op == 'inv':
        return wrap(~value, bits, signed)
    raise ValueError('unknown unary operation')


def binary(op, a, b, bits, signed):
    if op == 'eq': return a == b
    if op == 'ne': return a != b
    if op == 'lt': return a < b
    if op == 'le': return a <= b
    if op == 'gt': return a > b
    if op == 'ge': return a >= b
    if op in ('shl', 'shr'):
        if b < 0 or b >= bits:
            raise Panic('invalid shift count')
        value = a << b if op == 'shl' else a >> b
    elif op in ('div', 'rem'):
        if b == 0:
            raise Panic('integer division by zero')
        quotient = abs(a) // abs(b)
        if (a < 0) != (b < 0):
            quotient = -quotient
        value = quotient if op == 'div' else a - quotient * b
    elif op == 'add': value = a + b
    elif op == 'sub': value = a - b
    elif op == 'mul': value = a * b
    elif op == 'and': value = a & b
    elif op == 'or': value = a | b
    elif op == 'xor': value = a ^ b
    else: raise ValueError('unknown binary operation')
    return wrap(value, bits, signed)


def ieee(pattern, bits):
    if bits == 32:
        return struct.unpack('>f', pattern.to_bytes(4, 'big'))[0]
    if bits == 64:
        return struct.unpack('>d', pattern.to_bytes(8, 'big'))[0]
    raise ValueError('unsupported IEEE width')


def _float_round(numerator, denominator, bits, negative=False):
    """Round a nonnegative exact ratio directly to the target IEEE format.

    The quantum is 2**(max(floor(log2(n/d)), emin) - fraction_bits).
    Integer divmod retains the entire remainder, including division sticky
    bits. One ties-to-even decision covers normal, subnormal and overflow
    results; there is no intermediate binary64 rounding.
    """
    fraction, bias = {32: (23, 127), 64: (52, 1023)}[bits]
    sign = int(negative) << (bits - 1)
    if numerator == 0:
        return ieee(sign, bits)
    exponent = numerator.bit_length() - denominator.bit_length()
    if exponent >= 0:
        exponent -= numerator < (denominator << exponent)
    else:
        exponent -= (numerator << -exponent) < denominator
    exponent = max(exponent, 1 - bias)
    quantum = exponent - fraction
    if quantum >= 0:
        denominator <<= quantum
    else:
        numerator <<= -quantum
    significand, remainder = divmod(numerator, denominator)
    twice = remainder << 1
    if twice > denominator or (twice == denominator and significand & 1):
        significand += 1
    if significand == 1 << (fraction + 1):
        significand >>= 1
        exponent += 1
    if exponent > bias:
        pattern = ((1 << (bits - fraction - 1)) - 1) << fraction
    elif significand < 1 << fraction:
        pattern = significand
    else:
        pattern = ((exponent + bias) << fraction) | (significand - (1 << fraction))
    return ieee(sign | pattern, bits)


def float_neg(value, bits):
    # Changing the sign needs no arithmetic or rounding, including for -0.
    fmt = {32: '>f', 64: '>d'}[bits]
    pattern = int.from_bytes(struct.pack(fmt, value), 'big')
    return ieee(pattern ^ (1 << (bits - 1)), bits)


def float_binary(op, a, b, bits):
    if bits not in (32, 64):
        raise ValueError('unsupported IEEE width')
    if op in ('eq', 'ne', 'lt', 'le', 'gt', 'ge'):
        return binary(op, a, b, 0, False)
    if op == 'sub':
        b = float_neg(b, bits)
        op = 'add'
    if op not in ('add', 'mul', 'div'):
        raise ValueError('unknown floating operation')
    if math.isnan(a) or math.isnan(b):
        return math.nan
    aneg = math.copysign(1.0, a) < 0
    bneg = math.copysign(1.0, b) < 0
    negative = aneg != bneg
    ai, bi = math.isinf(a), math.isinf(b)
    if op == 'add':
        if ai or bi:
            return math.nan if ai and bi and negative else (a if ai else b)
        an, ad = a.as_integer_ratio()
        bn, bd = b.as_integer_ratio()
        numerator, denominator = an * bd + bn * ad, ad * bd
        negative = numerator < 0 or (numerator == 0 and aneg and bneg)
    else:
        if op == 'mul':
            if (ai and b == 0) or (bi and a == 0):
                return math.nan
            if ai or bi:
                return -math.inf if negative else math.inf
        else:
            if (ai and bi) or (a == 0 and b == 0):
                return math.nan
            if ai or b == 0:
                return -math.inf if negative else math.inf
            if bi:
                return _float_round(0, 1, bits, negative)
        an, ad = a.as_integer_ratio()
        bn, bd = b.as_integer_ratio()
        if op == 'mul':
            numerator, denominator = an * bn, ad * bd
        else:
            numerator, denominator = an * bd, ad * bn
    return _float_round(abs(numerator), abs(denominator), bits, negative)


def run(main):
    try:
        result = main()
    except Panic as error:
        print(PANIC_MARKER + str(error), file=sys.stderr)
        raise SystemExit(PANIC_EXIT_CODE) from None
    except TypedError as error:
        print(TYPED_ERROR_MARKER + error.type_key, file=sys.stderr)
        if isinstance(error.value, Error):
            print(HOST_ERROR_MARKER + error.value.describe(), file=sys.stderr)
        error.drop()
        raise SystemExit(1) from None
    raise SystemExit(0 if result is None else result)


# These are language owners. Python references and garbage collection do not
# increment/decrement the language count, and no __del__ performs language work.
_DEAD = object()
_MAX_COUNT = (1 << 64) - 1


class ValueType:
    __slots__ = ('kind', 'fields', 'variants', 'stride')

    def __init__(self, kind='trivial', fields=(), variants=None, stride=8):
        self.kind, self.fields = kind, fields
        self.variants, self.stride = variants or {}, stride

    def copy(self, value):
        if self.kind == 'vec':
            value.retain()
            return value
        if self.kind in ('foreign', 'error'):
            # A language copy keeps the same host object.  For errors this is
            # the declared error_payload_copy relation, not a deep copy.
            return value
        if self.kind in ('struct', 'enum'):
            pending, copied = [(self, value, None)], []
            while pending:
                ops, item, count = pending.pop()
                if count is not None:
                    fields = tuple(copied[-count:]) if count else ()
                    if count:
                        del copied[-count:]
                    copied.append((item[0], fields) if ops.kind == 'enum'
                                  else fields)
                elif isinstance(ops, ValueType) and ops.kind in ('struct', 'enum'):
                    fields, values = (ops.fields, item) if ops.kind == 'struct' else (
                        ops.variants[item[0]], item[1])
                    pairs = tuple(zip(fields, values))
                    pending.append((ops, item, len(pairs)))
                    pending.extend((t, v, None) for t, v in reversed(pairs))
                else:
                    copied.append(ops.copy(item))
            return copied[0]
        return value

    def drop(self, value):
        if self.kind == 'vec':
            value.release()
        elif self.kind == 'error':
            value.drop()
        elif self.kind == 'foreign':
            # Dropping a language owner does not promise immediate host
            # destruction or release a host reference held elsewhere.
            pass
        elif self.kind in ('struct', 'enum'):
            pending = [(self, value)]
            while pending:
                ops, item = pending.pop()
                if isinstance(ops, ValueType) and ops.kind in ('struct', 'enum'):
                    fields, values = (ops.fields, item) if ops.kind == 'struct' else (
                        ops.variants[item[0]], item[1])
                    pending.extend(zip(fields, values))
                else:
                    ops.drop(item)


class Owner:
    __slots__ = ('ops', 'value')

    def __init__(self, ops, value=_DEAD):
        self.ops, self.value = ops, value

    def take(self):
        if self.value is _DEAD:
            raise RuntimeError('read of inactive language owner')
        value, self.value = self.value, _DEAD
        return value

    def drop(self):
        if self.value is not _DEAD:
            value = self.take()
            self.ops.drop(value)

    def install(self, value):
        self.drop()
        self.value = value


class Frame:
    __slots__ = ('owners', 'borrows')

    def __init__(self):
        self.owners, self.borrows = {}, []

    def slot(self, key, ops):
        if key not in self.owners:
            self.owners[key] = Owner(ops)
        return self.owners[key]

    def put(self, key, ops, value):
        slot = self.slot(key, ops)
        if slot.value is not _DEAD:
            raise RuntimeError('overwriting live temporary')
        slot.value = value
        return value

    def take(self, key):
        return self.owners[key].take()

    def drop(self, key):
        slot = self.owners.get(key)
        if slot is not None:
            slot.drop()

    def borrow(self, key):
        self.borrows.append(key)

    def end_borrow(self, key):
        # Branches may never start some candidate borrow in an error plan.
        for i in range(len(self.borrows) - 1, -1, -1):
            if self.borrows[i] == key:
                del self.borrows[i]
                break


ERROR_TYPE_KEY = 'lang.py.Error'
ERROR_OPS = ValueType('error')
_HOST_MODULES = {}


def host_module(logical_path, host_module_name):
    """Import only a declared host module; keep import failures separate.

    The cache key includes both declaration identity and the declared host
    module.  Two logical modules may intentionally map to different Python
    modules; a logical-path-only key would silently return the wrong host
    object and turn a later operation into a misleading py.Error.
    """
    key = (logical_path, host_module_name)
    module = _HOST_MODULES.get(key)
    if module is not None:
        return module
    try:
        module = importlib.import_module(host_module_name)
    except Exception as exc:
        raise HostEnvironmentError(
            f"cannot import declared host module {host_module_name!r} "
            f"for {logical_path!r}: "
            f"{type(exc).__module__}.{type(exc).__qualname__}: {exc}"
        ) from exc
    _HOST_MODULES[key] = module
    return module


def _host_typed_error(exception, file, begin_byte, end_byte, operation):
    payload = Error(exception, file, begin_byte, end_byte, operation)
    return TypedError(ERROR_TYPE_KEY, payload, ERROR_OPS)


def _host_text(value):
    if not isinstance(value, str):
        raise TypeError('foreign value declared string is not str')
    # Use the builtin encoder, preserving NUL and combining marks while
    # rejecting isolated surrogate code points without replacement.
    str.encode(value, 'utf-8', 'strict')
    return str.__str__(value)


def foreign_read(base, attribute_path, file, begin_byte, end_byte, operation,
                 returns_text=False):
    """Read exactly one declared host attribute path.

    Only the declared path is consulted; unknown names never fall back to a
    general attribute search.  Internal typed errors and BaseException
    subclasses keep their original propagation.
    """
    try:
        value = base
        for attribute in attribute_path:
            value = getattr(value, attribute)
        return _host_text(value) if returns_text else value
    except TypedError:
        raise
    except Exception as exc:
        raise _host_typed_error(
            exc, file, begin_byte, end_byte, operation) from exc


def foreign_call(function, positional, keywords, file, begin_byte, end_byte,
                 operation, returns_unit, returns_text=False):
    """Call one already-read declared host target.

    The caller evaluated the target and every source argument before this
    function.  Omitted optional parameters are absent from both containers;
    they are never replaced with None.
    """
    try:
        value = function(*positional, **keywords)
        if returns_text:
            value = _host_text(value)
    except TypedError:
        raise
    except Exception as exc:
        raise _host_typed_error(
            exc, file, begin_byte, end_byte, operation) from exc
    if returns_unit and value is not None:
        raise Panic('foreign callable declared Unit returned a non-None value')
    return value


def _allocation_size(count, stride):
    if count < 0 or stride < 0 or (stride and count > _MAX_COUNT // stride):
        raise Panic('Vec allocation size overflow')
    size = count * stride
    if size > sys.maxsize:
        raise Panic('Vec allocation size overflow')
    return size


def _allocate_elements(values):
    return list(values)


def _allocate_vec():
    return object.__new__(Vec)


class Vec:
    __slots__ = ('element', 'items', 'refs', 'iterations', 'capacity', '_reserved')

    @staticmethod
    def create(element, values):
        _allocation_size(len(values), element.stride)
        try:
            result = _allocate_vec()
            result.items = _allocate_elements(values)
            result.element, result.refs, result.iterations = element, 1, 0
            result.capacity, result._reserved = len(values), []
            return result
        except (MemoryError, OverflowError):
            raise Panic('Vec allocation failed') from None

    def retain(self):
        if not 0 < self.refs < _MAX_COUNT:
            raise Panic('Vec reference count overflow or inactive object')
        self.refs += 1

    def release(self):
        if self.refs <= 0:
            raise RuntimeError('duplicate Vec release')
        self.refs -= 1
        if self.refs == 0:
            for value in reversed(self.items):
                self.element.drop(value)
            self.items.clear()
            self._reserved.clear()

    def index(self, index):
        if not 0 <= index < len(self.items):
            raise Panic('Vec index out of bounds')
        return index

    def get(self, index):
        return self.items[self.index(index)]

    def set(self, index, value):
        index = self.index(index)
        old, self.items[index] = self.items[index], value
        self.element.drop(old)

    def slice(self, start, end):
        start = 0 if start is None else start
        end = len(self.items) if end is None else end
        if not 0 <= start <= end <= len(self.items):
            raise Panic('Vec slice out of bounds')
        # The new object exists before any element is copied. Allocation
        # failure is Panic; it does not promise cleanup of initialized values.
        try:
            result = Vec.create(self.element, self.items[start:end])
            result.items.clear()
            for index in range(start, end):
                result.items.append(self.element.copy(self.items[index]))
            return result
        except (MemoryError, OverflowError):
            raise Panic('Vec allocation failed') from None

    def begin(self):
        self.iterations += 1
        return len(self.items)

    def end(self):
        if self.iterations <= 0:
            raise RuntimeError('inactive Vec iteration')
        self.iterations -= 1

    def reserve(self, minimum):
        if minimum <= self.capacity:
            return
        if self.iterations:
            raise Panic('Vec storage changes during iteration')
        capacity = self.capacity or 1
        while capacity < minimum:
            capacity = minimum if capacity > _MAX_COUNT // 2 else capacity * 2
        _allocation_size(capacity, self.element.stride)
        try:
            reserved = _allocate_elements([None] * capacity)
        except (MemoryError, OverflowError):
            raise Panic('Vec allocation failed') from None
        self.capacity, self._reserved = capacity, reserved

    def push(self, value):
        if self.iterations:
            raise Panic('Vec length changes during iteration')
        if len(self.items) == _MAX_COUNT:
            raise Panic('Vec allocation size overflow')
        self.reserve(len(self.items) + 1)
        try:
            self.items.append(value)
        except (MemoryError, OverflowError):
            raise Panic('Vec allocation failed') from None

    def pop(self):
        if not self.items:
            return False, None
        if self.iterations:
            raise Panic('Vec length changes during iteration')
        return True, self.items.pop()

    def resize(self, length, fill):
        previous = len(self.items)
        if length == previous:
            return
        if self.iterations:
            raise Panic('Vec length changes during iteration')
        if length < previous:
            for value in reversed(self.items[length:]):
                self.element.drop(value)
            del self.items[length:]
            return
        self.reserve(length)
        try:
            for _ in range(previous, length):
                self.items.append(self.element.copy(fill))
        except (MemoryError, OverflowError):
            raise Panic('Vec allocation failed') from None

    def clear(self):
        self.resize(0, None)

    def _replace_length(self, values):
        # Private runtime test entry. No source-language length API exists.
        if self.iterations:
            raise Panic('Vec length changes during iteration')
        _allocation_size(len(values), self.element.stride)
        try:
            replacement = _allocate_elements(values)
        except (MemoryError, OverflowError):
            raise Panic('Vec allocation failed') from None
        old, self.items = self.items, replacement
        self.capacity = max(self.capacity, len(replacement))
        for value in reversed(old):
            self.element.drop(value)


class Index:
    """Receiver and mathematical index, never an element address."""
    __slots__ = ('receiver', 'index')

    def __init__(self, receiver, index):
        self.receiver, self.index = receiver, receiver.index(index)

    @property
    def value(self):
        return self.receiver.get(self.index)

    @value.setter
    def value(self, replacement):
        self.receiver.set(self.index, replacement)

    def replace_field(self, path, replacement):
        """Replace one field path inside this element with a real writeback."""
        def replace(ops, value, depth):
            if depth == len(path):
                return replacement
            field = path[depth]
            if (ops.kind != 'struct' or field < 0 or
                    field >= len(ops.fields)):
                raise RuntimeError('field place does not match element storage')
            values = list(value)
            for index in range(len(values)):
                if index != field:
                    values[index] = ops.fields[index].copy(value[index])
            values[field] = replace(ops.fields[field], value[field], depth + 1)
            return tuple(values)

        current = self.value
        self.receiver.set(
            self.index, replace(self.receiver.element, current, 0))


def text_binary(op, a, b):
    try:
        if op == 'add': return a + b
        if op == 'eq': return a == b
        if op == 'ne': return a != b
        if op == 'lt': return a < b
        if op == 'le': return a <= b
        if op == 'gt': return a > b
        if op == 'ge': return a >= b
    except (MemoryError, OverflowError):
        raise Panic('String allocation failed') from None
    raise RuntimeError('invalid checked String operator')


def text_at(value, index):
    if not 0 <= index < len(value):
        raise Panic('String index out of bounds')
    return ord(value[index])


def text_slice(value, start, end):
    start = 0 if start is None else start
    end = len(value) if end is None else end
    if not 0 <= start <= end <= len(value):
        raise Panic('String slice out of bounds')
    try:
        return value[start:end]
    except (MemoryError, OverflowError):
        raise Panic('String allocation failed') from None


def _parse_number(value, kind):
    unsigned = kind == 'u64'
    if kind in ('i64', 'u64'):
        if not re.fullmatch(r'[+]?[0-9]+' if unsigned else r'[+-]?[0-9]+', value):
            return 22, None
        negative = value.startswith('-')
        digits = value.lstrip('+-').lstrip('0') or '0'
        if len(digits) > 20:
            return 34, None
        result = int(digits) * (-1 if negative else 1)
        lower, upper = (0, (1 << 64)-1) if unsigned else (-(1 << 63), (1 << 63)-1)
        return (0, result) if lower <= result <= upper else (34, None)
    negative = value.startswith('-')
    body = value.lstrip('+-') if value[:1] in ('+', '-') else value
    if body.lower() in ('nan', 'inf') and value[:2] not in ('++', '--', '+-', '-+'):
        return 0, math.copysign(float(body), -1 if negative else 1)
    match = re.fullmatch(r'[+-]?([0-9]+(?:[.][0-9]*)?|[.][0-9]+)(?:[eE]([+-]?[0-9]+))?', value)
    if match is None:
        return 22, None
    mantissa, exponent = match.groups()
    exponent = exponent or '0'
    exponent_digits = exponent.lstrip('+-').lstrip('0') or '0'
    power = 1000000000 if len(exponent_digits) > 9 else int(exponent_digits)
    if exponent.startswith('-'):
        power = -power
    if '.' in mantissa:
        power -= len(mantissa)-mantissa.index('.')-1
    digits = mantissa.replace('.', '').lstrip('0')
    bits = 32 if kind == 'f32' else 64
    if not digits:
        return 0, _float_round(0, 1, bits, negative)
    adjusted = power + len(digits)-1
    if adjusted > (39 if bits == 32 else 309):
        return 34, None
    if adjusted < (-47 if bits == 32 else -325):
        return 0, _float_round(0, 1, bits, negative)
    trimmed = digits.rstrip('0')
    power += len(digits)-len(trimmed)
    coefficient = 0
    # Chunk conversion avoids Python's process-wide decimal digit limit.
    for start in range(0, len(trimmed), 9):
        chunk = trimmed[start:start+9]
        coefficient = coefficient * (10 ** len(chunk)) + int(chunk)
    numerator, denominator = (coefficient * 10**power, 1) if power >= 0 else (coefficient, 10**-power)
    result = _float_round(numerator, denominator, bits, negative)
    return (34, None) if math.isinf(result) else (0, result)


def _format_number(value, kind):
    if kind in ('i64', 'u64'):
        return str(value)
    if math.isnan(value): return 'nan'
    if math.isinf(value): return '-inf' if value < 0 else 'inf'
    if value == 0: return '-0' if math.copysign(1.0, value) < 0 else '0'
    if kind == 'f32':
        for precision in range(1, 10):
            candidate = format(value, f'.{precision}g')
            if _parse_number(candidate, kind) == (0, value):
                break
    else:
        candidate = repr(value)
    negative = candidate.startswith('-')
    mantissa, marker, exponent = candidate.lstrip('-').lower().partition('e')
    whole, point, fraction = mantissa.partition('.')
    digits = whole + fraction
    power = (int(exponent) if marker else 0) - len(fraction)
    digits = digits.lstrip('0')
    exponent = power + len(digits)-1
    digits = digits.rstrip('0')
    if -6 <= exponent < 21:
        position = exponent + 1
        if position <= 0:
            result = '0.' + '0'*(-position) + digits
        elif position >= len(digits):
            result = digits + '0'*(position-len(digits))
        else:
            result = digits[:position] + '.' + digits[position:]
    else:
        result = digits[0] + ('.'+digits[1:] if len(digits)>1 else '') + 'e' + ('+' if exponent>=0 else '-') + str(abs(exponent))
    return ('-' if negative else '') + result


def _math_call(name, args):
    if name == 'signbit': return math.copysign(1.0, args[0]) < 0
    if name in ('floor', 'ceil', 'trunc', 'round'):
        value = args[0]
        if not math.isfinite(value) or value == 0:
            return value
        if name == 'round':
            fraction, integral = math.modf(abs(value))
            return math.copysign(integral + (1.0 if fraction >= 0.5 else 0.0), value)
        result = float(getattr(math, name)(value))
        return math.copysign(result, value) if result == 0 else result
    if name == 'fma':
        a, b, c = args
        if math.isnan(a) or math.isnan(b) or math.isnan(c): return math.nan
        product_negative = (math.copysign(1.0, a) < 0) != (math.copysign(1.0, b) < 0)
        if (math.isinf(a) and b == 0) or (math.isinf(b) and a == 0): return math.nan
        if math.isinf(a) or math.isinf(b):
            if math.isinf(c) and product_negative != (c < 0): return math.nan
            return -math.inf if product_negative else math.inf
        if math.isinf(c): return c
        an, ad = a.as_integer_ratio()
        bn, bd = b.as_integer_ratio()
        cn, cd = c.as_integer_ratio()
        numerator = an*bn*cd + cn*ad*bd
        denominator = ad*bd*cd
        negative = numerator < 0 or (numerator == 0 and product_negative and math.copysign(1.0, c) < 0)
        return _float_round(abs(numerator), denominator, 64, negative)
    try:
        return getattr(math, name)(*args)
    except ValueError:
        if name in ('log', 'log2', 'log10') and args[0] == 0: return -math.inf
        if name == 'log1p' and args[0] == -1: return -math.inf
        if name == 'pow' and args[0] == 0 and args[1] < 0:
            negative = math.copysign(1.0, args[0]) < 0 and math.isfinite(args[1]) and args[1].is_integer() and args[1] % 2 != 0
            return -math.inf if negative else math.inf
        return math.nan
    except OverflowError:
        if name == 'ldexp': return math.copysign(math.inf, args[0])
        if name == 'pow' and args[0] < 0 and args[1].is_integer() and args[1] % 2 != 0:
            return -math.inf
        return math.inf


def value_call(operation, args, element=None, tags=None):
    try:
        if operation.startswith('math_'):
            return _math_call(operation[5:], args)
        if operation.startswith('num_parse_'):
            error, value = _parse_number(args[0], operation[10:])
            return (tags['Err'], (error,)) if error else (tags['Ok'], (value,))
        if operation.startswith('num_format_'):
            return _format_number(args[0], operation[11:])
        if operation == 'text_len': return len(args[0])
        if operation == 'text_byte_len': return len(args[0].encode('utf-8'))
        if operation == 'text_from_bytes':
            try:
                return tags['Ok'], (bytes(args[0].items).decode('utf-8'),)
            except UnicodeDecodeError:
                return tags['Err'], (22,)
        if operation == 'text_from_rune':
            rune = args[0]
            if not 0 <= rune <= 0x10ffff or 0xd800 <= rune <= 0xdfff:
                return tags['Err'], (22,)
            return tags['Ok'], (chr(rune),)
        if operation == 'text_bytes':
            return Vec.create(element, args[0].encode('utf-8'))
        if operation == 'vec_empty': return Vec.create(element, [])
        if operation == 'vec_make':
            result = Vec.create(element, [])
            result.resize(args[0], args[1])
            return result
        if operation == 'vec_len': return len(args[0].items)
        if operation == 'vec_capacity': return args[0].capacity
        if operation == 'vec_reserve': return args[0].reserve(args[1])
        if operation == 'vec_push':
            return args[0].push(args[0].element.copy(args[1]))
        if operation == 'vec_pop':
            present, value = args[0].pop()
            return (tags['Some'], (value,)) if present else (tags['None'], ())
        if operation == 'vec_resize': return args[0].resize(args[1], args[2])
        if operation == 'vec_clear': return args[0].clear()
    except (MemoryError, OverflowError):
        raise Panic('value library allocation failed') from None
    raise RuntimeError('invalid checked value operation')
