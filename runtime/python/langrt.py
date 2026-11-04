"""Scalar runtime. Generated programs pass explicit target widths."""
import struct
import sys

RUNTIME_VERSION = 'compiler-dev/1-python-scalars/1'
PANIC_MARKER = 'LANG_PANIC: '
PANIC_EXIT_CODE = 101


class Panic(BaseException):
    pass


class Cell:
    __slots__ = ('value',)

    def __init__(self, value):
        self.value = value


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


def run(main):
    try:
        result = main()
    except Panic as error:
        print(PANIC_MARKER + str(error), file=sys.stderr)
        raise SystemExit(PANIC_EXIT_CODE) from None
    raise SystemExit(0 if result is None else result)
