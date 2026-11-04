"""Value runtime. Generated programs pass explicit target widths."""
import math
import struct
import sys

RUNTIME_VERSION = 'compiler-dev/1-python-values/3'
PANIC_MARKER = 'LANG_PANIC: '
PANIC_EXIT_CODE = 101
TYPED_ERROR_MARKER = 'LANG_TYPED_ERROR: '


class Panic(BaseException):
    pass


class TypedError(Exception):
    """Language error only. The key is logical, never a session TypeId."""
    __slots__ = ('type_key', 'value')

    def __init__(self, type_key, value):
        super().__init__(type_key)
        self.type_key, self.value = type_key, value


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
        if isinstance(base, Cell):
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
        raise SystemExit(1) from None
    raise SystemExit(0 if result is None else result)
