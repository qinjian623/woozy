"""Exercise the emitted f64 classifiers on plain and hostile float subclasses."""

import json
from pathlib import Path
import random
import runpy
import struct
import subprocess
import sys
import tempfile


class HostFloat(float):
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

    def __bool__(self):
        raise AssertionError("classification invoked host truth conversion")


def f64_from_bits(bits):
    return struct.unpack(">d", bits.to_bytes(8, "big"))[0]


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
