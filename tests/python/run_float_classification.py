"""Exercise emitted f64 classifiers and bit-preserving sign operations."""

import json
from pathlib import Path
import random
import runpy
import struct
import subprocess
import sys
import tempfile


class HostFloat(float):
    __slots__ = ()

    def __eq__(self, other):
        raise AssertionError("classification invoked host equality")

    def __ne__(self, other):
        raise AssertionError("classification invoked host inequality")

    def __lt__(self, other):
        raise AssertionError("classification invoked host ordering")

    def __le__(self, other):
        raise AssertionError("classification invoked host ordering")

    def __gt__(self, other):
        raise AssertionError("classification invoked host ordering")

    def __ge__(self, other):
        raise AssertionError("classification invoked host ordering")

    def __float__(self):
        raise AssertionError("classification invoked host float conversion")

    def __neg__(self):
        raise AssertionError("negation invoked host override")

    def __bool__(self):
        raise AssertionError("classification invoked host truth conversion")

    def __abs__(self):
        raise AssertionError("sign operation invoked host absolute value")

    def __neg__(self):
        raise AssertionError("sign operation invoked host negation")


class CountingProxy:
    __slots__ = ("value", "calls")

    def __init__(self, value):
        self.value = value
        self.calls = 0

    def __float__(self):
        self.calls += 1
        return self.value

    def __eq__(self, other):
        raise AssertionError("sign operation invoked proxy equality")

    def __bool__(self):
        raise AssertionError("sign operation invoked proxy truth conversion")


def f64_from_bits(bits):
    return struct.unpack(">d", bits.to_bytes(8, "big"))[0]


def f64_bits(value):
    return int.from_bytes(struct.pack(">d", value), "big")


def generated_symbols(metadata):
    symbols = {}
    for row in metadata["symbols"]:
        symbols.setdefault(row["source_name"], row["generated"])
    return symbols


def check_program(program, symbols):
    sys.path.insert(0, str(program.parent))
    try:
        namespace = runpy.run_path(str(program))
        classify = namespace[symbols["classify"]]
        check_signbit = namespace[symbols["check_signbit"]]
        check_isinf = namespace[symbols["check_isinf"]]
        check_isfinite = namespace[symbols["check_isfinite"]]
        check_negation = namespace[symbols["check_negation"]]
        check_fabs = namespace[symbols["check_fabs"]]
        check_copysign = namespace[symbols["check_copysign"]]

        class HostUInt(int):
            def __or__(self, other):
                return (1 << 100) | 13

        check_unknown_or = namespace[symbols["check_unknown_or"]]
        assert check_unknown_or((1 << 100) | 8, 1) == 9
        assert check_unknown_or(HostUInt(8), 1) == 13
        # These functions must return ordinary floats even without a sign flip.
        for value in (HostFloat(1.0), HostFloat(-1.0)):
            absolute = check_fabs(value)
            assert type(absolute) is float and absolute == 1.0
            for sign in (1.0, -1.0):
                signed = check_copysign(value, HostFloat(sign))
                assert type(signed) is float and signed == sign
        cases = (
            ("nan", 1), ("-nan", 1), ("inf", 2), ("-inf", 2),
            ("0.0", 3), ("-0.0", 3), ("5e-324", 3), ("-42.0", 3),
        )
        for value, expected in cases:
            actual = classify(HostFloat(value))
            assert actual == expected, (value, expected, actual)

        edge_bits = [
            0x0000000000000000,  # positive zero
            0x8000000000000000,  # negative zero
            0x0000000000000001,  # minimum positive subnormal
            0x8000000000000001,  # minimum negative subnormal
            0x000fffffffffffff,  # maximum positive subnormal
            0x800fffffffffffff,  # maximum negative subnormal
            0x0010000000000000,  # minimum positive normal
            0x8010000000000000,  # minimum negative normal
            0x7fefffffffffffff,  # maximum positive finite
            0xffefffffffffffff,  # maximum negative finite
            0x7ff0000000000000,  # positive infinity
            0xfff0000000000000,  # negative infinity
            0x7ff0000000000001,  # positive signaling NaN
            0xfff0000000000001,  # negative signaling NaN
            0x7ff8000000000000,  # positive quiet NaN
            0xfff8000000000000,  # negative quiet NaN
            0x7fffffffffffffff,  # positive NaN with all payload bits set
            0xffffffffffffffff,  # negative NaN with all payload bits set
        ]
        sign_bits = (0x0000000000000000, 0x8000000000000000,
                     0x3ff0000000000000, 0xbff0000000000000,
                     0x7ff0000000000001, 0xfff8000000000123)
        abs_mask = 0x7fffffffffffffff
        sign_mask = 0x8000000000000000
        sign_patterns = edge_bits + [
            (index * 0x9e3779b97f4a7c15) & 0xffffffffffffffff
            for index in range(512)
        ]
        for bits in sign_patterns:
            value = f64_from_bits(bits)
            absolute = check_fabs(HostFloat(value))
            assert type(absolute) is float
            assert f64_bits(absolute) == bits & abs_mask, hex(bits)
            for other in sign_bits:
                signed = check_copysign(HostFloat(value), HostFloat(f64_from_bits(other)))
                assert type(signed) is float
                expected_bits = (bits & abs_mask) | (other & sign_mask)
                assert f64_bits(signed) == expected_bits, (hex(bits), hex(other))

        # A foreign numeric proxy supplies its declared f64 value through
        # __float__ exactly once per argument, never through host float calls
        # in the sign algorithm itself.
        for bits in edge_bits:
            proxy = CountingProxy(f64_from_bits(bits))
            absolute = check_fabs(proxy)
            assert proxy.calls == 1, (hex(bits), proxy.calls)
            assert type(absolute) is float and f64_bits(absolute) == bits & abs_mask
        for value_bits in edge_bits:
            for sign_value_bits in sign_bits:
                value = CountingProxy(f64_from_bits(value_bits))
                sign = CountingProxy(f64_from_bits(sign_value_bits))
                signed = check_copysign(value, sign)
                assert value.calls == 1 and sign.calls == 1, (
                    hex(value_bits), hex(sign_value_bits), value.calls, sign.calls)
                assert type(signed) is float
                assert f64_bits(signed) == (
                    (value_bits & abs_mask) | (sign_value_bits & sign_mask))

        # The private decoder maps encoding failures to Panic, not to a host
        # traceback or a numeric conversion.
        runtime = namespace["_rt"]
        assert type(runtime._float_from_bits(0)) is float
        for invalid in (1 << 64, -1):
            try:
                runtime._float_from_bits(invalid)
            except runtime.Panic:
                pass
            else:
                raise AssertionError(("decoder accepted invalid bits", invalid))
        saved_unpack = runtime._F64_UNPACK
        def fail_unpack(_data):
            raise MemoryError("synthetic decoder failure")
        runtime._F64_UNPACK = fail_unpack
        try:
            try:
                runtime._float_from_bits(0)
            except runtime.Panic:
                pass
            else:
                raise AssertionError("decoder did not map MemoryError to Panic")
        finally:
            runtime._F64_UNPACK = saved_unpack

        random_bits = random.Random(0x2776F6B)
        patterns = edge_bits + [
            random_bits.getrandbits(64) for _ in range(65536)
        ]
        for bits in patterns:
            value = f64_from_bits(bits)
            expected = (bits >> 63) != 0
            for candidate in (value, HostFloat(value)):
                actual = check_signbit(candidate)
                assert actual is expected, (hex(bits), expected, actual)
                negated = check_negation(candidate)
                assert type(negated) is float
                assert struct.pack(">d", negated) == (bits ^ (1 << 63)).to_bytes(8, "big"), hex(bits)
                magnitude = bits & 0x7fffffffffffffff
                infinity = 0x7ff0000000000000
                expected_class = 1 if magnitude > infinity else 2 if magnitude == infinity else 3
                assert classify(candidate) == expected_class, hex(bits)
                assert check_isinf(candidate) is (magnitude == infinity), hex(bits)
                assert check_isfinite(candidate) is (magnitude < infinity), hex(bits)
    finally:
        sys.path.pop(0)


def main():
    helper, fixture = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="woozy-float-classification-") as directory:
        out = Path(directory)
        subprocess.run([helper, "--project", fixture, "--out-dir", out],
                       check=True, timeout=90)
        metadata = json.loads((out / "metadata.json").read_text())
        check_program(out / "program.py", generated_symbols(metadata))


if __name__ == "__main__":
    main()
