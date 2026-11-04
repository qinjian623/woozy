"""Test-only IEEE expectations from exact integer ratios; no host FP arithmetic.

Independent of native emission. The production compiler never imports this file.
Round the exact rational once to the requested binary interchange format, RNE.
"""
from fractions import Fraction


def decoded(raw, width):
    frac, exp = (23, 8) if width == 32 else (52, 11)
    sign = raw >> (width - 1)
    e, m = (raw >> frac) & ((1 << exp) - 1), raw & ((1 << frac) - 1)
    if e == (1 << exp) - 1:
        return ('nan' if m else 'infinity'), sign, None
    value = Fraction(m if e == 0 else (1 << frac) + m)
    power = (e if e else 1) - ((1 << (exp - 1)) - 1) - frac
    value *= Fraction(2) ** power
    return ('zero' if value == 0 else 'finite'), sign, -value if sign else value


def result(raw, width):
    category, sign, _ = decoded(raw, width)
    return dict(type=f'f{width}', **{'class': category},
                bits=None if category == 'nan' else f'{raw:0{width // 4}x}',
                sign=None if category == 'nan' else 'negative' if sign else 'positive')


def rounded(value, width, zero_sign=0):
    frac, exp = (23, 8) if width == 32 else (52, 11)
    bias = (1 << (exp - 1)) - 1
    if not value:
        return zero_sign << (width - 1)
    sign = int(value < 0)
    value = abs(value)
    e = value.numerator.bit_length() - value.denominator.bit_length()
    if value < Fraction(2) ** e:
        e -= 1
    scale = max(e, 1 - bias) - frac
    scaled = value / (Fraction(2) ** scale)
    q, r = divmod(scaled.numerator, scaled.denominator)
    q += int(2 * r > scaled.denominator or (2 * r == scaled.denominator and q % 2))
    if q == (1 << (frac + 1)):
        q >>= 1
        scale += 1
    sign_bits = sign << (width - 1)
    if q < (1 << frac):
        return sign_bits | q
    e = scale + frac
    if e > bias:
        return sign_bits | (((1 << exp) - 1) << frac)
    return sign_bits | ((e + bias) << frac) | (q - (1 << frac))


def operation(op, inputs, width):
    a = inputs[0]
    ca, sa, va = decoded(a, width)
    frac = 23 if width == 32 else 52
    exp = 8 if width == 32 else 11
    infinity = ((1 << exp) - 1) << frac
    nan = infinity | (1 << (frac - 1))
    if op == 'neg':
        return a ^ (1 << (width - 1))
    b = inputs[1]
    cb, sb, vb = decoded(b, width)
    if op in ('eq', 'ne', 'lt', 'le', 'gt', 'ge'):
        if 'nan' in (ca, cb):
            return op == 'ne'
        ka = (-1 if sa else 1, 0) if ca == 'infinity' else (0, va)
        kb = (-1 if sb else 1, 0) if cb == 'infinity' else (0, vb)
        return {'eq': ka == kb, 'ne': ka != kb, 'lt': ka < kb,
                'le': ka <= kb, 'gt': ka > kb, 'ge': ka >= kb}[op]
    if 'nan' in (ca, cb):
        return nan
    if op == 'sub':
        return operation('add', [a, b ^ (1 << (width - 1))], width)
    if op == 'add':
        if ca == cb == 'infinity' and sa != sb:
            return nan
        if ca == 'infinity':
            return a
        if cb == 'infinity':
            return b
        return rounded(va + vb, width, int(ca == cb == 'zero' and sa == sb == 1))
    sign = (sa ^ sb) << (width - 1)
    if op == 'mul':
        if {ca, cb} == {'zero', 'infinity'}:
            return nan
        if 'infinity' in (ca, cb):
            return sign | infinity
        return rounded(va * vb, width, sa ^ sb)
    if op == 'div':
        if ca == cb == 'infinity' or ca == cb == 'zero':
            return nan
        if ca == 'infinity' or cb == 'zero':
            return sign | infinity
        if ca == 'zero' or cb == 'infinity':
            return sign
        return rounded(va / vb, width, sa ^ sb)
    if op == 'compound':
        for step in ('add', 'sub', 'mul', 'div'):
            a = operation(step, [a, b], width)
        return a
    if op == 'step':
        return operation('add', [operation('add', [a, b], width), inputs[2]], width)
    if op == 'nofma':
        return operation('add', [operation('mul', [a, b], width), inputs[2]], width)
    raise AssertionError(op)
