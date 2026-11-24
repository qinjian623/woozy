"""Exercise immutable u32/u64 proof reuse and constant shifts in generated Python."""

import json
from pathlib import Path
import random
import re
import runpy
import subprocess
import sys
import tempfile

M32 = (1 << 32) - 1
M64 = (1 << 64) - 1


def u32(value):
    return value & M32


def u64(value):
    return value & M64


def generated_symbols(metadata):
    symbols = {}
    for row in metadata["functions"]:
        symbols.setdefault(row["source_name"], row["generated"])
    return symbols


def function_text(code, generated):
    start = code.find("def " + generated + "(")
    assert start >= 0, generated
    match = re.search(r"(?m)^def ", code[start + 1:])
    end = start + 1 + match.start() if match else len(code)
    return code[start:end]


def check_program(program, symbols):
    code = program.read_text()
    sys.path.insert(0, str(program.parent))
    try:
        namespace = runpy.run_path(str(program))
        runtime = namespace["_rt"]
        call = {name: namespace[generated] for name, generated in symbols.items()}

        positive = ("u32_bits", "u32_shifts", "u64_bits", "u64_shifts")
        for name in positive:
            text = function_text(code, symbols[name])
            assert "_rt.binary(" not in text, (name, text)
        for name in ("u32_bits", "u64_bits"):
            text = function_text(code, symbols[name])
            for operator in (" & ", " | ", " ^ "):
                assert operator in text, (name, operator, text)
        for name in ("u32_shifts", "u64_shifts"):
            text = function_text(code, symbols[name])
            assert " >> " in text and " << " in text, (name, text)

        assert "4294967295" in function_text(code, symbols["u32_shifts"])
        assert "18446744073709551615" in function_text(
            code, symbols["u64_shifts"])

        assert call["u32_bits"]() == 1161888005
        assert call["u32_shifts"]() == 1610612737
        assert call["u64_bits"]() == 6148914691236517204
        assert call["u64_shifts"]() == 1

        # The independent oracle is ordinary Python integer arithmetic with
        # explicit target-width wrapping, not the emitter's helper functions.
        assert call["u32_bits"]() == u32(
            u32(u32(4294967295 & 2863311530) | 305419896) ^ 4294967295)
        assert call["u32_shifts"]() == u32(
            u32(u32(3758096384 << 1) >> 1) | u32(3758096384 >> 31))
        assert call["u64_bits"]() == u64(
            u64(u64(18446744073709551615 & 12297829382473034410) | 1) ^
            18446744073709551615)
        assert call["u64_shifts"]() == u64(
            u64(u64(18446744073709551615 << 63) >> 63) |
            u64(18446744073709551615 >> 63))

        # Branch-local bindings keep their proofs, and both arms agree with
        # the integer oracle.
        branch_text = function_text(code, symbols["branch_u32"])
        assert "_rt.binary(" not in branch_text, branch_text
        assert call["branch_u32"](True) == 305419896
        assert call["branch_u32"](False) == 305419897

        # A capture local is not the creator's Binding, so it keeps the
        # wrapped helper path rather than inheriting outer-function proof.
        captured = [text for text in re.split(r"(?m)^(?=def )", code)
                    if "def " in text and "_env" in text and
                    "_rt.binary('and'" in text]
        assert captured, code
        assert call["closure_u32"]() == 305419896

        for name in ("dynamic_u32", "dynamic_u64", "writable_u32"):
            text = function_text(code, symbols[name])
            assert "_rt.binary(" in text, (name, text)

        rng = random.Random(0x1_0B_A6A)
        for _ in range(256):
            value = rng.getrandbits(80)
            count = rng.randrange(32)
            expected = u32(u32(value & M32) >> count)
            assert call["dynamic_u32"](value, count) == expected
            value = rng.getrandbits(80)
            count = rng.randrange(64)
            expected = u64(u64(value | 1) << count)
            assert call["dynamic_u64"](value, count) == expected
        assert call["writable_u32"]() == 38177487

        # An untrusted host int subclass reaches its overridden operation only
        # through the wrapped helper, never through a direct proof path.
        class HostUInt(int):
            calls = 0

            def __or__(self, other):
                type(self).calls += 1
                return int.__or__(self, other)

        assert call["dynamic_u64"](HostUInt(2), 3) == 24
        assert HostUInt.calls == 1

        for name in ("invalid_high_u32", "invalid_negative_u64"):
            text = function_text(code, symbols[name])
            assert "_rt.binary('" in text, (name, text)
            try:
                call[name]()
            except runtime.Panic as error:
                assert "invalid shift count" in str(error)
            else:
                raise AssertionError((name, "invalid count did not Panic"))
    finally:
        sys.path.pop(0)


def main():
    helper, fixture = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="woozy-immutable-uint-") as directory:
        out = Path(directory)
        subprocess.run([helper, "--project", fixture, "--out-dir", out],
                       check=True, timeout=90)
        metadata = json.loads((out / "metadata.json").read_text())
        check_program(out / "program.py", generated_symbols(metadata))


if __name__ == "__main__":
    main()
