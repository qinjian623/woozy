#!/usr/bin/env python3
"""Acceptance tests for the self-hosted exact number package.

Every case is compiled and executed as a real Woozy program with the stage 0
compiler (`woozyc`) at -O0 and -O2 and compared with an expectation that is
never produced by the implementation under test:

  * integer spellings, rational arithmetic, float->integer truncation and
    fixed-width checked/wrapped integers use Python ``int``/``Fraction``;
  * IEEE-754 decoding uses an independent Python decode;
  * IEEE-754 encodings from decimal spellings, exact rationals, integers and
    f64<->f32 conversion use the test-only GMP/MPFR oracle ``oracle.cpp``
    (see that file for why MPFR is a valid reference here).  Every f64
    expectation is cross-checked against Python ``float``/``struct`` so a
    misconfigured oracle fails the run instead of silently agreeing.

Coverage includes 2^-1074 and 2^-149, subnormal/normal boundaries, ties to
even, overflow to infinity, the -400/+400 decimal magnitude shortcut with
1000-digit literals, signed zero, NaN/Infinity, min/max float/int, integer
rounding to f32/f64, and f64->f32 narrowing (including decimal double-rounding
traps).

Usage:
    python3 selfhost/tests/number/run_number.py [--woozyc PATH]
        [--ld-library-path PATH] [--keep] [--verbose]

The compiler is an external stage 0 tool: this script never rebuilds the C++
compiler, never imports its constant evaluator and never links the oracle into
generated programs.
"""
import argparse
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DEFAULT_WOOZYC = REPO / 'build' / 'woozy-stdlib-native' / 'woozyc'
TOOLCHAIN = REPO.parent / 'toolchains'
DEFAULT_LD_LIBRARY_PATH = ':'.join((
    str(TOOLCHAIN / 'llvm-22.1.8' / 'lib'),
    str(TOOLCHAIN / 'libxml2-legacy-2.13.9' / 'usr' / 'lib'),
))
MODULE_SOURCES = (
    ('wzcc/bigint/bigint.wz', 'wzcc/bigint/bigint.wz'),
    ('wzcc/number/number.wz', 'wzcc/number/number.wz'),
)
FLOAT_WIDTHS = (32, 64)
INTEGER_WIDTHS = (8, 16, 32, 64)
SIGN_MASK = {32: 0x80000000, 64: 0x8000000000000000}
# Maximum number of cases in one generated main; see the chunking comment in
# main().
CHUNK = 60


# --------------------------------------------------------------------------
# Expectation helpers.
# --------------------------------------------------------------------------
def quote(text):
    return json.dumps(text)


def canonical_decimal(text):
    return str(int(text, 10))


def fraction_text(value):
    return f'{value.numerator}/{value.denominator}'


def decode_bits(bits, width):
    fraction_bits = 23 if width == 32 else 52
    exponent_bits = 8 if width == 32 else 11
    bias = 127 if width == 32 else 1023
    negative = ((bits >> (width - 1)) & 1) != 0
    exponent = (bits >> fraction_bits) & ((1 << exponent_bits) - 1)
    fraction = bits & ((1 << fraction_bits) - 1)
    if exponent == (1 << exponent_bits) - 1:
        return dict(kind='nan' if fraction else 'infinity', negative=negative,
                    significand=0, shift=0)
    if exponent == 0:
        return dict(kind='finite', negative=negative, significand=fraction,
                    shift=(1 - bias) - fraction_bits)
    return dict(kind='finite', negative=negative,
                significand=(1 << fraction_bits) | fraction,
                shift=exponent - bias - fraction_bits)


def decoded_integer(decoded):
    magnitude = decoded['significand']
    shift = decoded['shift']
    if shift >= 0:
        magnitude <<= shift
    else:
        magnitude >>= -shift
    return -magnitude if decoded['negative'] else magnitude


def python_ratio_f64_bits(value):
    try:
        result = float(value)
    except OverflowError:
        result = float('inf')
    return int.from_bytes(struct.pack('>d', result), 'big')


def python_spelling_f64_bits(spelling, negative):
    result = float(spelling.replace('_', ''))
    if negative:
        result = -result
    return int.from_bytes(struct.pack('>d', result), 'big')


def python_integer_f64_bits(value):
    try:
        result = float(value)
    except OverflowError:
        result = -float('inf') if value < 0 else float('inf')
    return int.from_bytes(struct.pack('>d', result), 'big')


def cpp_integer_parse(spelling):
    """Independent model of the frozen integer spelling decoding."""
    text = spelling.replace('_', '')
    if not text:
        return None
    base = 10
    body = text
    if len(text) > 2 and text[0] == '0':
        marker = text[1]
        if marker in 'xX':
            base, body = 16, text[2:]
        elif marker in 'bB':
            base, body = 2, text[2:]
        elif marker in 'oO':
            base, body = 8, text[2:]
    if not body:
        return None
    digits = body
    negative = False
    if digits[0] in '+-':
        negative = digits[0] == '-'
        digits = digits[1:]
    if not digits:
        return None
    allowed = set('0123456789abcdefghijklmnopqrstuvwxyz'[:base])
    if any(character.lower() not in allowed for character in digits):
        return None
    value = int(digits, base)
    return -value if negative else value


def decimal_of_dyadic(numerator, denominator):
    """Exact finite decimal spelling of numerator/denominator (power of 2)."""
    assert denominator > 0 and denominator & (denominator - 1) == 0
    scale = denominator.bit_length() - 1
    scaled = numerator * 5 ** scale
    digits = str(scaled).zfill(scale + 1)
    if scale == 0:
        return digits
    return digits[:-scale] + '.' + digits[-scale:]


def f64_bits(value):
    return int.from_bytes(struct.pack('>d', value), 'big')


def f32_bits(value):
    return int.from_bytes(struct.pack('>f', value), 'big')


# --------------------------------------------------------------------------
# Oracle protocol wrapper.
# --------------------------------------------------------------------------
class Oracle:
    def __init__(self):
        self.requests = []
        self.answers = None

    def ask(self, line):
        self.requests.append(line)
        return len(self.requests) - 1

    def run(self, executable):
        payload = ''.join(line + '\n' for line in self.requests)
        result = subprocess.run([str(executable)], input=payload, text=True,
                                capture_output=True, timeout=120)
        if result.returncode != 0:
            raise RuntimeError(f'oracle failed: {result.stderr}')
        answers = result.stdout.splitlines()
        if len(answers) != len(self.requests):
            raise RuntimeError(
                f'oracle answered {len(answers)} of {len(self.requests)}')
        self.answers = answers

    def answer(self, index):
        return self.answers[index]


ORACLE = Oracle()
CASES = []
SEEN = set()


@dataclass
class Case:
    identifier: str
    group: str
    statements: list = field(default_factory=list)
    # ('text', line) | ('oracle', index, suffix) |
    # ('oracle-f64', index, python_bits, suffix)
    expect: tuple = ('text', '')


def add(identifier, group, statements, expect):
    assert identifier not in SEEN, identifier
    SEEN.add(identifier)
    CASES.append(Case(identifier, group, list(statements), expect))


def add_success(case_id, group, statements, expect):
    add(case_id, group, statements, expect)


def add_error(case_id, group, statements, variant):
    if isinstance(statements, str):
        statements = [statements]
    statements = list(statements)
    statements.append(f'io.println("{case_id} no-error");')
    add(case_id, group, statements,
        ('text', f'{case_id} error {variant}'))


def tf(value):
    return 'true' if value else 'false'


# --------------------------------------------------------------------------
# Integer spelling cases (Python int oracle).
# --------------------------------------------------------------------------
def build_parse_cases():
    spellings = [
        ('zero', '0'),
        ('one', '1'),
        ('negative', '-123456789012345678901234567890'),
        ('plus', '+00042'),
        ('underscores', '1_000_000'),
        ('hex', '0x7f'),
        ('hex-upper', '0XDEAD_BEEF'),
        ('binary', '0b1010'),
        ('octal', '0o777'),
        ('legacy-leading-zero', '0777'),
        ('underscore-prefix', '0x_1'),
        ('underline-zero', '0_0'),
        ('double-underscore', '1__2'),
        ('prefix-sign-inside', '0x-10'),
        ('prefix-plus-inside', '0x+2a'),
        ('big-1000-digits', '9' * 1000),
        ('negative-big', '-' + '8' * 500),
        ('hex-big', '0x' + 'f' * 300),
        ('pow2-1074', str(2 ** 1074)),
        ('pow2-63', str(2 ** 63)),
    ]
    for name, spelling in spellings:
        expected = cpp_integer_parse(spelling)
        assert expected is not None, (name, spelling)
        case_id = f'parse-{name}'
        statements = [
            f'io.println("{case_id} " + bigint.format_decimal('
            f'number.parse_integer_spelling({quote(spelling)})));']
        add_success(case_id, 'integer-rational', statements,
                    ('text', f'{case_id} {expected}'))
    invalid = [
        ('empty', ''),
        ('underscore-only', '_'),
        ('sign-only', '+'),
        ('negative-sign-only', '-'),
        ('space', '1 2'),
        ('hex-empty', '0x'),
        ('hex-invalid-digit', '0xg'),
        ('binary-invalid-digit', '0b2'),
        ('octal-invalid-digit', '0o8'),
        ('signed-prefix', '-0x10'),
        ('double-point', '1.5'),
        ('trailing-letter', '12a'),
    ]
    for name, spelling in invalid:
        assert cpp_integer_parse(spelling) is None, (name, spelling)
        case_id = f'parse-reject-{name}'
        add_error(case_id, 'integer-rational',
                  f'let rejected = number.parse_integer_spelling('
                  f'{quote(spelling)});', 'InvalidLiteral')


# --------------------------------------------------------------------------
# Rational cases (Python Fraction oracle).
# --------------------------------------------------------------------------
def rational_statements(identifier, left, right):
    (ln, ld), (rn, rd) = left, right
    return [
        f'let l = number.make_ratio(dec("{ln}"), dec("{ld}"));',
        f'let r = number.make_ratio(dec("{rn}"), dec("{rd}"));',
    ]


def rational_result(case_id, operation, left, right):
    (ln, ld), (rn, rd) = left, right
    left_value = Fraction(int(ln), int(ld))
    right_value = Fraction(int(rn), int(rd))
    statements = rational_statements(case_id, left, right)
    inputs = (f'{fraction_text(left_value)} {fraction_text(right_value)}')
    if operation == 'compare':
        statements.append(
            f'io.println("{case_id} " + '
            'strconv.format_i64(i64(number.ratio_compare(l, r))) + " " + '
            'bigint.format_decimal(l.numerator) + "/" + '
            'bigint.format_decimal(l.denominator) + " " + '
            'bigint.format_decimal(r.numerator) + "/" + '
            'bigint.format_decimal(r.denominator));')
        order = (left_value > right_value) - (left_value < right_value)
        add_success(case_id, 'integer-rational', statements,
                    ('text', f'{case_id} {order} {inputs}'))
        return
    calls = {
        'add': ('number.ratio_add', left_value + right_value),
        'subtract': ('number.ratio_subtract', left_value - right_value),
        'multiply': ('number.ratio_multiply', left_value * right_value),
    }
    if operation in calls:
        call, result = calls[operation]
        statements.append(f'let out = {call}(l, r);')
        statements.append(
            f'io.println("{case_id} " + '
            'bigint.format_decimal(out.numerator) + "/" + '
            'bigint.format_decimal(out.denominator) + " " + '
            'bigint.format_decimal(l.numerator) + "/" + '
            'bigint.format_decimal(l.denominator) + " " + '
            'bigint.format_decimal(r.numerator) + "/" + '
            'bigint.format_decimal(r.denominator));')
        add_success(case_id, 'integer-rational', statements,
                    ('text', f'{case_id} {fraction_text(result)} {inputs}'))
        return
    assert operation == 'divide', operation
    statements.append('let out = number.ratio_divide(l, r);')
    if right_value == 0:
        add_error(case_id, 'integer-rational', statements, 'ZeroDenominator')
        return
    statements.append(
        f'io.println("{case_id} " + '
        'bigint.format_decimal(out.numerator) + "/" + '
        'bigint.format_decimal(out.denominator) + " " + '
        'bigint.format_decimal(l.numerator) + "/" + '
        'bigint.format_decimal(l.denominator) + " " + '
        'bigint.format_decimal(r.numerator) + "/" + '
        'bigint.format_decimal(r.denominator));')
    add_success(case_id, 'integer-rational', statements,
                ('text', f'{case_id} {fraction_text(left_value / right_value)}'
                         f' {inputs}'))


def build_rational_cases():
    make_ratio_inputs = [
        ('reduce', ('6', '-8'), (6, -8)),
        ('zero-negative-denominator', ('0', '-5'), (0, -5)),
        ('unit', ('-7', '1'), (-7, 1)),
        ('double-negative', ('-6', '-8'), (-6, -8)),
        ('gcd-large', (str(2 ** 300 * 3), str(2 ** 299 * 9)),
         (2 ** 300 * 3, 2 ** 299 * 9)),
        ('big', (str(10 ** 1000 + 1), str(3 * 10 ** 999)),
         (10 ** 1000 + 1, 3 * 10 ** 999)),
        ('pow2', (str(2 ** 1074), str(2 ** 1074)), (2 ** 1074, 2 ** 1074)),
        ('one-negative', ('1', '-1'), (1, -1)),
    ]
    for name, (top, bottom), values in make_ratio_inputs:
        case_id = f'ratio-make-{name}'
        expected = Fraction(*values)
        statements = [
            f'let top = dec("{top}");',
            f'let bottom = dec("{bottom}");',
            'let out = number.make_ratio(top, bottom);',
            f'io.println("{case_id} " + '
            'bigint.format_decimal(out.numerator) + "/" + '
            'bigint.format_decimal(out.denominator) + " " + '
            'bigint.format_decimal(top) + "/" + '
            'bigint.format_decimal(bottom));',
        ]
        add_success(case_id, 'integer-rational', statements,
                    ('text', f'{case_id} {fraction_text(expected)} '
                             f'{canonical_decimal(top)}/'
                             f'{canonical_decimal(bottom)}'))
    add_error('ratio-make-zero-denominator', 'integer-rational',
              'let rejected = number.make_ratio(dec("1"), dec("0"));',
              'ZeroDenominator')
    pairs = [
        ('simple', ('1', '3'), ('1', '6')),
        ('cancel', ('1', '2'), ('-1', '2')),
        ('zero', ('0', '7'), ('5', '11')),
        ('negatives', ('-5', '12'), ('7', '-18')),
        ('big', (str(10 ** 400 + 3), str(7 * 10 ** 399)),
                (str(-10 ** 399), '11')),
        ('pow2', (str(2 ** 1074), '3'), ('1', str(2 ** 1073))),
    ]
    for name, left, right in pairs:
        for operation in ('add', 'subtract', 'multiply'):
            rational_result(f'ratio-{operation}-{name}', operation,
                            left, right)
        rational_result(f'ratio-compare-{name}', 'compare', left, right)
        rational_result(f'ratio-divide-{name}', 'divide', left, right)
    rational_result('ratio-compare-equal', 'compare', ('7', '-9'),
                    ('-14', '18'))
    rational_result('ratio-divide-by-zero', 'divide', ('1', '2'), ('0', '3'))
    rational_result('ratio-divide-sign', 'divide', ('1', '2'), ('3', '-4'))


# --------------------------------------------------------------------------
# IEEE literal cases (MPFR oracle; f64 cross-checked with Python struct).
# --------------------------------------------------------------------------
def literal_case(case_id, spelling, negative, width, expect_error=False):
    if expect_error:
        add_error(case_id, 'literals',
                  'let rejected = number.ieee_float_bits('
                  f'{quote(spelling)}, {tf(negative)}, {width});',
                  'InvalidLiteral')
        return
    oracle_spelling = spelling.replace('_', '')
    index = ORACLE.ask(f'L {width} {1 if negative else 0} {oracle_spelling}')
    statements = [
        f'io.println("{case_id} " + strconv.format_u64('
        f'number.ieee_float_bits({quote(spelling)}, {tf(negative)}, '
        f'{width})));']
    if width == 64:
        add_success(case_id, 'literals', statements,
                    ('oracle-f64', index,
                     python_spelling_f64_bits(spelling, negative), ''))
    else:
        add_success(case_id, 'literals', statements, ('oracle', index, ''))


def build_literal_cases():
    simple = [
        ('zero', '0.0', False),
        ('negative-zero', '0.0', True),
        ('negative-zero-fraction', '0.000', True),
        ('zero-huge-exponent', '0e999999999999999999999999999999', False),
        ('negative-zero-huge-exponent', '0.0e-99999999999999999999', True),
        ('one', '1.0', False),
        ('one-tenth', '0.1', False),
        ('one-fifth', '0.2', False),
        ('negative-one-tenth', '0.1', True),
        ('dot-prefix', '.5', False),
        ('dot-suffix', '5.', False),
        ('exponent', '1e10', False),
        ('exponent-plus', '1e+10', False),
        ('exponent-negative', '1e-10', False),
        ('underscored', '1_0.5e1_0', False),
        ('max-f64', '1.7976931348623157e308', False),
        ('max-f64-minus', '1.7976931348623155e308', False),
        ('over-max-f64', '1.7976931348623159e308', False),
        ('huge-f64', '1e400', False),
        ('tiny-f64', '1e-400', False),
        ('zero-exponent-digits', '1.5e0000000000000000005', False),
    ]
    for name, spelling, negative in simple:
        for width in FLOAT_WIDTHS:
            literal_case(f'literal-{name}-{width}', spelling, negative, width)
    exact = [
        ('f64-min-subnormal', 1, 2 ** 1074, 64, False),
        ('f64-half-min-subnormal', 1, 2 ** 1075, 64, False),
        ('f64-subnormal-tie', 3, 2 ** 1075, 64, False),
        ('f64-normal-boundary-tie', 2 ** 53 - 1, 2 ** 1075, 64, False),
        ('f32-min-subnormal', 1, 2 ** 149, 32, False),
        ('f32-half-min-subnormal', 1, 2 ** 150, 32, False),
        ('f32-subnormal-tie', 3, 2 ** 150, 32, False),
        ('f32-normal-boundary-tie', 2 ** 24 - 1, 2 ** 150, 32, False),
        ('f64-one-tie', 2 ** 53 + 1, 2 ** 53, 64, False),
        ('f32-one-tie', 2 ** 24 + 1, 2 ** 24, 32, False),
        ('f32-double-rounding', (2 ** 24 + 1) * 2 ** 51 + 1, 2 ** 75, 32,
         False),
    ]
    for name, numerator, denominator, width, negative in exact:
        spelling = decimal_of_dyadic(numerator, denominator)
        literal_case(f'literal-exact-{name}', spelling, negative, width)
    # 1000-digit cancellation: the value stays small while the digit count and
    # the decimal exponent are large.  Trailing zeros must be stripped before
    # the +-400 magnitude shortcut and before building 10^exponent.
    long_cases = [
        ('shift-cancel', '1' + '0' * 999 + 'e-999', False),
        ('leading-zeros', '0.' + '0' * 999 + '1e999', False),
        ('trailing-zeros', '1.' + '0' * 999 + 'e-400', False),
        ('mixed', '1' + '2' * 499 + '0' * 500 + 'e-500', False),
        ('negative-shift-cancel', '1' + '0' * 999 + 'e-999', True),
    ]
    for name, spelling, negative in long_cases:
        literal_case(f'literal-long-{name}', spelling, negative, 64)
    invalid = [
        ('empty', ''),
        ('dot-only', '.'),
        ('exponent-empty', '1e'),
        ('exponent-sign-only', '1e+'),
        ('double-dot', '1.2.3'),
        ('double-exponent', '1e5e5'),
        ('letter', '1x'),
        ('space', '1 2'),
    ]
    for name, spelling in invalid:
        literal_case(f'literal-reject-{name}-64', spelling, False, 64,
                     expect_error=True)
    add_error('literal-reject-width', 'literals',
              'let rejected = number.ieee_float_bits("1.0", false, 16);',
              'InvalidWidth')
    add_error('literal-reject-width-zero', 'literals',
              'let rejected = number.ieee_float_bits("1.0", false, 0);',
              'InvalidWidth')


# --------------------------------------------------------------------------
# Exact ratio -> IEEE cases (MPFR oracle; f64 cross-checked with Fraction).
# --------------------------------------------------------------------------
def ratio_ieee_case(case_id, numerator, denominator, negative, width,
                    expect_error=None):
    num_text, den_text = str(numerator), str(denominator)
    if expect_error:
        add_error(case_id, 'exact-ratios',
                  f'let rejected = number.exact_ratio_to_ieee('
                  f'{tf(negative)}, dec("{num_text}"), dec("{den_text}"), '
                  f'{width});',
                  expect_error)
        return
    index = ORACLE.ask(f'R {width} {1 if negative else 0} {num_text} '
                       f'{den_text}')
    statements = [
        f'let n = dec("{num_text}");',
        f'let d = dec("{den_text}");',
        f'io.println("{case_id} " + strconv.format_u64('
        f'number.exact_ratio_to_ieee({tf(negative)}, n, d, {width})) + '
        f'" {num_text}/{den_text}");',
    ]
    suffix = f' {num_text}/{den_text}'
    if width == 64:
        value = Fraction(int(num_text), int(den_text))
        expected = python_ratio_f64_bits(value)
        if negative:
            expected |= SIGN_MASK[64]
        add_success(case_id, 'exact-ratios', statements,
                    ('oracle-f64', index, expected, suffix))
    else:
        add_success(case_id, 'exact-ratios', statements,
                    ('oracle', index, suffix))


def build_ratio_ieee_cases():
    for value in (1, 2, 3, 4, 2 ** 51, 2 ** 52 - 1, 2 ** 52, 2 ** 52 + 1,
                  2 ** 52 + 2, 2 ** 53 - 1, 2 ** 53, 2 ** 53 + 1, 2 ** 63,
                  2 ** 64 - 1):
        ratio_ieee_case(f'ratio-ieee-f64-den2-1074-{value}', value,
                        2 ** 1074, False, 64)
    for value in (1, 2, 3, 2 ** 23 - 1, 2 ** 23, 2 ** 23 + 1, 2 ** 24 - 1,
                  2 ** 24, 2 ** 24 + 1):
        ratio_ieee_case(f'ratio-ieee-f32-den2-149-{value}', value,
                        2 ** 149, False, 32)
    ratio_ieee_case('ratio-ieee-f64-half-ulp-boundary', 2 ** 53 + 1,
                    2 ** 53, False, 64)
    ratio_ieee_case('ratio-ieee-f32-half-ulp-boundary', 2 ** 24 + 1,
                    2 ** 24, False, 32)
    ratio_ieee_case('ratio-ieee-f64-max-finite', 2 ** 1024 - 2 ** 971, 1,
                    False, 64)
    ratio_ieee_case('ratio-ieee-f64-max-finite-minus-half-ulp',
                    2 ** 1024 - 2 ** 971 - 2 ** 970, 1, False, 64)
    ratio_ieee_case('ratio-ieee-f64-overflow-tie', 2 ** 1024 - 2 ** 970, 1,
                    False, 64)
    ratio_ieee_case('ratio-ieee-f64-overflow', 2 ** 1024, 1, False, 64)
    ratio_ieee_case('ratio-ieee-f64-overflow-big', 2 ** 1100, 1, False, 64)
    ratio_ieee_case('ratio-ieee-f64-negative-overflow', 2 ** 1024, 1, True, 64)
    ratio_ieee_case('ratio-ieee-f64-negative-zero', 0, 7, True, 64)
    ratio_ieee_case('ratio-ieee-f32-negative-zero', 0, 7, True, 32)
    ratio_ieee_case('ratio-ieee-f64-negative-subnormal', 1, 2 ** 1074, True, 64)
    ratio_ieee_case('ratio-ieee-f64-third', 1, 3, False, 64)
    ratio_ieee_case('ratio-ieee-f64-two-thirds', 2, 3, False, 64)
    ratio_ieee_case('ratio-ieee-f64-tenth', 1, 10, False, 64)
    ratio_ieee_case('ratio-ieee-f32-seventh', 1, 7, False, 32)
    ratio_ieee_case('ratio-ieee-f64-cancel-1000', 10 ** 1000 + 1, 10 ** 999,
                    False, 64)
    ratio_ieee_case('ratio-ieee-f64-cancel-half', 10 ** 500 - 1,
                    2 * 10 ** 500, False, 64)
    ratio_ieee_case('ratio-ieee-f32-cancel-1000', 10 ** 1000 + 1, 10 ** 999,
                    False, 32)
    rng = random.Random(20261113)
    for index in range(10):
        bits = 20 + 7 * index
        numerator = rng.getrandbits(bits) * 2 + 1
        denominator = rng.getrandbits(bits + 3) * 2
        width = 32 if index % 2 else 64
        ratio_ieee_case(f'ratio-ieee-random-{index}', numerator, denominator,
                        index % 3 == 0, width)
    ratio_ieee_case('ratio-ieee-reject-zero-denominator', 1, 0, False, 64,
                    expect_error='ZeroDenominator')
    add_error('ratio-ieee-reject-negative-numerator', 'exact-ratios',
              'let rejected = number.exact_ratio_to_ieee(false, dec("-1"), '
              'dec("3"), 64);', 'InvalidLiteral')
    add_error('ratio-ieee-reject-negative-denominator', 'exact-ratios',
              'let rejected = number.exact_ratio_to_ieee(false, dec("1"), '
              'dec("-3"), 64);', 'InvalidLiteral')
    add_error('ratio-ieee-reject-width', 'exact-ratios',
              'let rejected = number.exact_ratio_to_ieee(false, dec("1"), '
              'dec("3"), 16);', 'InvalidWidth')


# --------------------------------------------------------------------------
# decode_ieee / truncate_ieee / convert_ieee cases.
# --------------------------------------------------------------------------
def interesting_patterns(width):
    fraction_bits = 23 if width == 32 else 52
    exponent_bits = 8 if width == 32 else 11
    sign = SIGN_MASK[width]
    infinity = ((1 << exponent_bits) - 1) << fraction_bits
    quiet = infinity | (1 << (fraction_bits - 1))
    maximum = (((1 << exponent_bits) - 2) << fraction_bits) | (
        (1 << fraction_bits) - 1)
    patterns = [
        0, sign, 1, sign | 1, 2, sign | 2,
        (1 << fraction_bits) - 1, sign | ((1 << fraction_bits) - 1),
        1 << fraction_bits, sign | (1 << fraction_bits),
        (1 << fraction_bits) + 1,
        ((1 << exponent_bits) - 2) << fraction_bits,
        maximum, sign | maximum,
        infinity, sign | infinity,
        quiet, sign | quiet, quiet | 0x123, sign | (quiet | 0x123),
        infinity | 1, sign | (infinity | 1), (infinity | 1) | 0x123,
    ]
    return list(dict.fromkeys(patterns))


def build_decode_cases():
    for width in FLOAT_WIDTHS:
        for pattern in interesting_patterns(width):
            case_id = f'decode-{width}-{pattern:0{width // 4}x}'
            decoded = decode_bits(pattern, width)
            statements = [
                f'let value = number.decode_ieee(0x{pattern:x}, {width});',
                f'io.println("{case_id} " + kind_name(value.kind) + " " + '
                'tf(value.negative) + " " + '
                'bigint.format_decimal(value.significand) + " " + '
                'strconv.format_i64(value.shift));',
            ]
            expected = (f'{case_id} {decoded["kind"]} '
                        f'{int(decoded["negative"])} '
                        f'{decoded["significand"]} {decoded["shift"]}')
            add_success(case_id, 'ieee-bits', statements, ('text', expected))
    add_error('decode-reject-width', 'ieee-bits',
              'let rejected = number.decode_ieee(0, 16);', 'InvalidWidth')
    add_error('decode-reject-width-zero', 'ieee-bits',
              'let rejected = number.decode_ieee(0, 0);', 'InvalidWidth')


def truncate_case(case_id, bits, width, expect_error=False):
    if expect_error:
        add_error(case_id, 'ieee-bits',
                  f'let rejected = number.truncate_ieee(0x{bits:x}, {width});',
                  expect_error)
        return
    value = decoded_integer(decode_bits(bits, width))
    statements = [
        f'io.println("{case_id} " + bigint.format_decimal('
        f'number.truncate_ieee(0x{bits:x}, {width})));']
    add_success(case_id, 'ieee-bits', statements,
                ('text', f'{case_id} {value}'))


def build_truncate_cases():
    f64_values = [
        0.0, -0.0, 1.0, 0.5, 0.9, -0.9, 1.5, -1.9, 2.5, -2.5,
        float(2 ** 52), float(2 ** 53), 1e300, 1.7976931348623157e308,
        5e-324, 2.2250738585072014e-308,
    ]
    for index, value in enumerate(f64_values):
        truncate_case(f'truncate-f64-{index}', f64_bits(value), 64)
    f32_values = [0.0, -0.0, 1.0, 0.5, 0.9, -0.9, 1.5, -1.9,
                  float(2 ** 23), 1e30]
    for index, value in enumerate(f32_values):
        truncate_case(f'truncate-f32-{index}', f32_bits(value), 32)
    for width in FLOAT_WIDTHS:
        fraction_bits = 23 if width == 32 else 52
        exponent_bits = 8 if width == 32 else 11
        infinity = ((1 << exponent_bits) - 1) << fraction_bits
        for name, bits in (('infinity', infinity),
                           ('negative-infinity', SIGN_MASK[width] | infinity),
                           ('nan', infinity | (1 << (fraction_bits - 1))),
                           ('signaling-nan', infinity | 1)):
            truncate_case(f'truncate-{width}-{name}', bits, width,
                          expect_error='NonFiniteInteger')
    add_error('truncate-reject-width', 'ieee-bits',
              'let rejected = number.truncate_ieee(0, 16);', 'InvalidWidth')


def build_convert_cases():
    for width in FLOAT_WIDTHS:
        for pattern in interesting_patterns(width):
            case_id = f'convert-identity-{width}-{pattern:0{width // 4}x}'
            statements = [
                f'io.println("{case_id} " + strconv.format_u64('
                f'number.convert_ieee(0x{pattern:x}, {width}, {width})));']
            add_success(case_id, 'ieee-bits', statements,
                        ('text', f'{case_id} {pattern}'))
    for pattern in interesting_patterns(32):
        index = ORACLE.ask(f'C 32 64 {pattern}')
        case_id = f'convert-widen-32-64-{pattern:08x}'
        statements = [
            f'io.println("{case_id} " + strconv.format_u64('
            f'number.convert_ieee(0x{pattern:x}, 32, 64)));']
        add_success(case_id, 'ieee-bits', statements, ('oracle', index, ''))
    infinity64 = 0x7ff0000000000000
    narrow = [
        ('zero', 0x0000000000000000),
        ('negative-zero', 0x8000000000000000),
        ('one', f64_bits(1.0)),
        ('one-midpoint', f64_bits(1.0 + 2.0 ** -24)),
        ('one-above-midpoint', f64_bits(1.0 + 2.0 ** -24) + 1),
        ('one-below-midpoint', f64_bits(1.0 + 2.0 ** -24) - 1),
        ('third', 0x3fd5555555555555),
        ('min-subnormal', 0x0000000000000001),
        ('half-min-subnormal', f64_bits(2.0 ** -150)),
        ('subnormal-tie', f64_bits(3.0 * 2.0 ** -150)),
        ('max-subnormal-f64', 0x000fffffffffffff),
        ('min-normal-f64', 0x0010000000000000),
        ('min-normal-f32', f64_bits(2.0 ** -126)),
        ('below-min-normal-f32', f64_bits(2.0 ** -126) - 1),
        ('max-f32', f64_bits(float(0x7f7fffff))),
        ('just-above-max-f32', f64_bits(float(0x7f7fffff)) + 1),
        ('overflow-midpoint', f64_bits(2.0 ** 128 - 2.0 ** 103)),
        ('overflow-above', f64_bits(2.0 ** 128 - 2.0 ** 103) + 1),
        ('overflow-below', f64_bits(2.0 ** 128 - 2.0 ** 103) - 1),
        ('infinity', infinity64),
        ('nan', 0x7ff8000000000000),
        ('signaling-nan', infinity64 | 1),
        ('negative-nan', 0xfff8000000000000),
        ('double-rounding-would-collapse',
         f64_bits(1.0 + 2.0 ** -24)),
    ]
    for name, pattern in narrow:
        index = ORACLE.ask(f'C 64 32 {pattern}')
        case_id = f'convert-narrow-{name}'
        statements = [
            f'io.println("{case_id} " + strconv.format_u64('
            f'number.convert_ieee(0x{pattern:x}, 64, 32)));']
        add_success(case_id, 'ieee-bits', statements, ('oracle', index, ''))
    add_error('convert-reject-source-width', 'ieee-bits',
              'let rejected = number.convert_ieee(0, 16, 64);', 'InvalidWidth')
    add_error('convert-reject-target-width', 'ieee-bits',
              'let rejected = number.convert_ieee(0, 64, 16);', 'InvalidWidth')


# --------------------------------------------------------------------------
# integer_to_ieee cases (MPFR oracle; f64 cross-checked with float(int)).
# --------------------------------------------------------------------------
def int_ieee_case(case_id, value, width):
    text = str(value)
    index = ORACLE.ask(f'N {width} {text}')
    statements = [
        f'let v = dec("{text}");',
        f'io.println("{case_id} " + strconv.format_u64('
        f'number.integer_to_ieee(v, {width})) + " " + '
        'bigint.format_decimal(v));',
    ]
    if width == 64:
        add_success(case_id, 'integer-float', statements,
                    ('oracle-f64', index, python_integer_f64_bits(value),
                     f' {text}'))
    else:
        add_success(case_id, 'integer-float', statements,
                    ('oracle', index, f' {text}'))


def build_integer_float_cases():
    values = [
        0, 1, -1, 2, 16777215, 16777216, 16777217, 16777218, 16777219,
        -(2 ** 24 + 1), 2 ** 53 - 1, 2 ** 53, 2 ** 53 + 1, 2 ** 53 + 3,
        2 ** 63 - 1, -(2 ** 63), 2 ** 64 - 1, -(2 ** 64), 10 ** 20,
        2 ** 128, 2 ** 1024 - 2 ** 971, 2 ** 1024 - 2 ** 970, 2 ** 1024,
        -(2 ** 1024), 10 ** 1000 + 1, -(10 ** 300), 2 ** 150 + 2 ** 100,
    ]
    for index, value in enumerate(values):
        for width in FLOAT_WIDTHS:
            int_ieee_case(f'int-ieee-{index}-{width}', value, width)
    rng = random.Random(777001)
    for index in range(6):
        value = rng.getrandbits(60 + 20 * index) | 1
        for width in FLOAT_WIDTHS:
            int_ieee_case(f'int-ieee-random-{index}-{width}', value, width)
    add_error('int-ieee-reject-width', 'integer-float',
              'let rejected = number.integer_to_ieee(dec("1"), 16);',
              'InvalidWidth')


# --------------------------------------------------------------------------
# checked_integer / wrap_integer cases (Python integer oracle).
# --------------------------------------------------------------------------
def integer_width_bounds(width, signed):
    modulus = 1 << width
    if signed:
        return -(modulus >> 1), modulus >> 1
    return 0, modulus


def checked_case(case_id, value, width, signed):
    low, high = integer_width_bounds(width, signed)
    text = str(value)
    definitions = [f'let v = dec("{text}");']
    if low <= value < high:
        statements = definitions + [
            f'let result = number.checked_integer(v, {width}, {tf(signed)});',
            f'io.println("{case_id} " + '
            'bigint.format_decimal(result.value) + " " + '
            'strconv.format_u64(result.bits) + " " + '
            'bigint.format_decimal(v));',
        ]
        add_success(case_id, 'fixed-width', statements,
                    ('text', f'{case_id} {value} {value % (1 << width)} '
                             f'{value}'))
    else:
        add_error(case_id, 'fixed-width',
                  definitions + [f'let rejected = number.checked_integer('
                                 f'v, {width}, {tf(signed)});'],
                  'IntegerRange')


def wrap_case(case_id, value, width, signed):
    modulus = 1 << width
    residue = value % modulus
    mathematical = residue
    if signed and residue >= (modulus >> 1):
        mathematical = residue - modulus
    text = str(value)
    statements = [
        f'let v = dec("{text}");',
        f'let result = number.wrap_integer(v, {width}, {tf(signed)});',
        f'io.println("{case_id} " + bigint.format_decimal(result.value) + '
        '" " + strconv.format_u64(result.bits) + " " + '
        'bigint.format_decimal(v));',
    ]
    add_success(case_id, 'fixed-width', statements,
                ('text', f'{case_id} {mathematical} {residue} {value}'))


def build_fixed_width_cases():
    for width in INTEGER_WIDTHS:
        for signed in (True, False):
            low, high = integer_width_bounds(width, signed)
            values = [low - 1, low, low + 1, -1, 0, 1,
                      high - 2, high - 1, high, high + 1,
                      2 * high, -2 * high + 1, 10 ** 40, -(10 ** 40),
                      2 ** 100 + 1, -(2 ** 100 + 1)]
            for index, value in enumerate(values):
                tag = 's' if signed else 'u'
                checked_case(f'checked-{tag}{width}-{index}', value, width,
                             signed)
                wrap_case(f'wrap-{tag}{width}-{index}', value, width, signed)
    rng = random.Random(424242)
    for index in range(8):
        width = INTEGER_WIDTHS[index % len(INTEGER_WIDTHS)]
        signed = index % 2 == 0
        value = rng.getrandbits(80) - rng.getrandbits(79)
        tag = 's' if signed else 'u'
        checked_case(f'checked-random-{tag}{width}-{index}', value, width,
                     signed)
        wrap_case(f'wrap-random-{tag}{width}-{index}', value, width, signed)
    huge = 10 ** 1000 + 123456789
    for width in INTEGER_WIDTHS:
        for signed in (True, False):
            tag = 's' if signed else 'u'
            checked_case(f'checked-huge-{tag}{width}', huge, width, signed)
            wrap_case(f'wrap-huge-{tag}{width}', huge, width, signed)
    for width in (0, 1, 7, 12, 128):
        add_error(f'checked-reject-width-{width}', 'fixed-width',
                  f'let rejected = number.checked_integer(dec("1"), {width}, '
                  'true);', 'InvalidWidth')
        add_error(f'wrap-reject-width-{width}', 'fixed-width',
                  f'let rejected = number.wrap_integer(dec("1"), {width}, '
                  'true);', 'InvalidWidth')


# --------------------------------------------------------------------------
# Source generation and execution.
# --------------------------------------------------------------------------
PRELUDE = '''package app;

import "app/wzcc/bigint" as bigint;
import "app/wzcc/number" as number;
import "std/io" as io;
import "std/os" as os;
import "std/strconv" as strconv;

func dec(text string) bigint.BigInt throws number.Error {
    try {
        return bigint.parse_digits(text, 10);
    } catch error bigint.Error {
        throw number.Error.InvalidLiteral("test input");
    }
}

func tf(value bool) string {
    if value { return "1"; }
    return "0";
}

func kind_name(value number.IeeeKind) string {
    switch value {
        case number.IeeeKind.Finite: return "finite";
        case number.IeeeKind.Infinity: return "infinity";
        case number.IeeeKind.NaN: return "nan";
    }
}

func error_name(value number.Error) string {
    switch value {
        case number.Error.InvalidLiteral(message): return "InvalidLiteral";
        case number.Error.InvalidWidth: return "InvalidWidth";
        case number.Error.ZeroDenominator: return "ZeroDenominator";
        case number.Error.NonFiniteInteger: return "NonFiniteInteger";
        case number.Error.IntegerRange: return "IntegerRange";
    }
}

func main() i32 throws os.Error {
'''

MANIFEST = '''schema_version = 1
[package]
name = "app"
source_root = "src"
entry = "app"
[build]
backend = "native"
word_bits = 64
'''


def render_cases(rows):
    lines = [PRELUDE]
    for case in rows:
        lines.append(f'    // {case.identifier}\n')
        lines.append('    try {\n')
        for statement in case.statements:
            lines.append('        ' + statement + '\n')
        lines.append('    } catch error number.Error {\n')
        lines.append(f'        io.println("{case.identifier} error " + '
                     'error_name(error));\n')
        lines.append('    }\n')
    lines.append('    return 0;\n')
    lines.append('}\n')
    return ''.join(lines)


def run(argv, cwd=None, env=None, timeout=900):
    return subprocess.run(argv, cwd=None if cwd is None else str(cwd),
                          capture_output=True, text=True, timeout=timeout,
                          env=env)


def status_json(result):
    try:
        return json.loads(result.stdout)
    except ValueError:
        return {'status': 'not-run', 'stderr': result.stderr[-2000:],
                'raw': result.stdout[-2000:]}


def compile_oracle(directory):
    executable = directory / 'oracle'
    result = run(['g++', '-O2', '-std=c++17',
                  str(Path(__file__).resolve().parent / 'oracle.cpp'),
                  '-lmpfr', '-lgmp', '-o', str(executable)])
    if result.returncode != 0:
        raise RuntimeError('cannot build the GMP/MPFR oracle:\n' +
                           result.stderr)
    return executable


def prepare_project(directory, source):
    (directory / 'src' / 'wzcc' / 'bigint').mkdir(parents=True, exist_ok=True)
    (directory / 'src' / 'wzcc' / 'number').mkdir(parents=True, exist_ok=True)
    for relative, repo_relative in MODULE_SOURCES:
        shutil.copyfile(REPO / 'selfhost' / 'src' / repo_relative,
                        directory / 'src' / relative)
    (directory / 'lang.toml').write_text(MANIFEST)
    (directory / 'src' / 'main.wz').write_text(source)


def resolve_expectations():
    expectations = {}
    for case in CASES:
        kind = case.expect[0]
        if kind == 'text':
            expectations[case.identifier] = case.expect[1]
            continue
        answer = ORACLE.answer(case.expect[1])
        suffix = case.expect[-1]
        if kind == 'oracle':
            expectations[case.identifier] = (
                f'{case.identifier} {answer}{suffix}')
            continue
        python_answer = str(case.expect[2])
        if answer != python_answer:
            raise RuntimeError(
                f'oracle/Python f64 disagreement for {case.identifier}: '
                f'MPFR={answer} Python={python_answer}')
        expectations[case.identifier] = (
            f'{case.identifier} {answer}{suffix}')
    return expectations


def main(argv=None):
    parser = argparse.ArgumentParser(
        description='Run the self-hosted number acceptance suite.')
    parser.add_argument('--woozyc', default=os.environ.get(
        'WOOZYC', str(DEFAULT_WOOZYC)))
    parser.add_argument('--ld-library-path', default=os.environ.get(
        'LD_LIBRARY_PATH', DEFAULT_LD_LIBRARY_PATH))
    parser.add_argument('--keep', action='store_true')
    parser.add_argument('--verbose', action='store_true')
    args = parser.parse_args(argv)

    build_parse_cases()
    build_rational_cases()
    build_literal_cases()
    build_ratio_ieee_cases()
    build_decode_cases()
    build_truncate_cases()
    build_convert_cases()
    build_integer_float_cases()
    build_fixed_width_cases()

    environment = dict(os.environ, LD_LIBRARY_PATH=args.ld_library_path,
                       PYTHONDONTWRITEBYTECODE='1', LC_ALL='C')
    directory = Path(tempfile.mkdtemp(prefix='wz-number-tests-'))
    if args.keep:
        print(f'workspace: {directory}')
    failures = []
    not_run = []
    passed = 0
    try:
        ORACLE.run(compile_oracle(directory))
        expectations = resolve_expectations()
        groups = {}
        for case in CASES:
            groups.setdefault(case.group, []).append(case)
        # The stage 0 native backend slows down sharply on very large single
        # functions, so each group is built in bounded chunks.  Splitting by
        # case keeps every build cheap and isolates a failing chunk.
        chunks = []
        for group, rows in groups.items():
            for start in range(0, len(rows), CHUNK):
                label = f'{group}-{start // CHUNK}'
                chunks.append((group, label, rows[start:start + CHUNK]))
        chunk_results = []
        for group, label, rows in chunks:
            source = render_cases(rows)
            project = directory / label
            prepare_project(project, source)
            check = run([args.woozyc, 'check', '--project',
                         str(project / 'lang.toml'), '--backend', 'native',
                         '--format', 'json'], env=environment)
            check_json = status_json(check)
            if args.verbose:
                print(f'[{label}] check {check_json.get("status")} '
                      f'({len(rows)} cases)')
            if check.returncode != 0 or check_json.get('status') != 'ok':
                detail = (check_json.get('diagnostics') or
                          check_json.get('stderr') or check.stdout[-2000:])
                for case in rows:
                    not_run.append((case.identifier, group,
                                    'check failed: ' + str(detail)[:500]))
                continue
            outputs = {}
            for opt in (0, 2):
                out = project / f'out{opt}'
                build = run([args.woozyc, 'build', '--project',
                             str(project / 'lang.toml'), '--backend',
                             'native', '--out-dir', str(out), '--opt',
                             str(opt), '--format', 'json'],
                            env=environment)
                build_json = status_json(build)
                if build.returncode != 0 or build_json.get('status') != 'ok':
                    detail = (build_json.get('diagnostics') or
                              build_json.get('stderr') or
                              build.stdout[-2000:])
                    for case in rows:
                        not_run.append((case.identifier, group,
                                        f'build O{opt} failed: '
                                        + str(detail)[:500]))
                    outputs[opt] = None
                    continue
                execution = run([str(out / 'program')], env=environment)
                if args.verbose:
                    print(f'[{label}] run O{opt} exit '
                          f'{execution.returncode}')
                outputs[opt] = execution
            for case in rows:
                expected = expectations[case.identifier]
                actuals = []
                broken = False
                for opt in (0, 2):
                    execution = outputs.get(opt)
                    if execution is None:
                        broken = True
                        continue
                    if execution.returncode != 0:
                        broken = True
                        failures.append(
                            (case.identifier, expected,
                             f'O{opt} exited {execution.returncode}: '
                             f'{execution.stdout[-500:]}'
                             f'{execution.stderr[-500:]}'))
                        continue
                    matched = [line for line in execution.stdout.splitlines()
                               if line.startswith(case.identifier + ' ')]
                    actuals.append(matched)
                if broken:
                    continue
                if any(len(lines) != 1 for lines in actuals):
                    failures.append(
                        (case.identifier, expected,
                         f'expected one output line per run, got {actuals}'))
                    continue
                if actuals[0] != actuals[1]:
                    failures.append(
                        (case.identifier, expected,
                         f'O0/O2 disagree: {actuals[0]} vs {actuals[1]}'))
                    continue
                if actuals[0][0] == expected:
                    passed += 1
                else:
                    failures.append((case.identifier, expected, actuals[0][0]))
        total = len(CASES)
        print(f'number acceptance: {total} cases, {passed} passed, '
              f'{len(failures)} failed, {len(not_run)} not_run, '
              f'unsupported 0')
        for identifier, expected, actual in failures:
            print(f'FAIL {identifier}\n  expected: {expected}\n'
                  f'  actual:   {actual}')
        for identifier, group, reason in not_run:
            print(f'NOT_RUN {identifier} [{group}]: {reason}')
        return 0 if not failures and not not_run else 1
    finally:
        if not args.keep:
            shutil.rmtree(directory, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
