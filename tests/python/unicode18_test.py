#!/usr/bin/env python3
"""Acceptance checks for the embedded Unicode 18.0.0 Python runtime.

The official NormalizationTest, CaseFolding and GraphemeBreakTest files are
the oracle.  No check uses Python 3.12's unicodedata (Unicode 15.0.0).  The
generator determinism check compiles the fixed vendored utf8proc translation
unit twice and compares the generated module bytes.  The generated module's
embedded full license notice is compared byte-for-byte with the vendored
LICENSE.md.

Run with the project Python, for example:

    /home/qin/Work/language-dev-20261103/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12 \
        tests/python/unicode18_test.py
"""

import argparse
import hashlib
import importlib.util
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "runtime/python/unicode18.py"
GENERATOR_PATH = ROOT / "tools/generate_unicode18.py"
LICENSE_PATH = ROOT / "runtime/native/unicode/vendor/utf8proc/LICENSE.md"
DATA_DIR = ROOT / "tests/native/unicode/data"
NORMALIZATION_FILE = DATA_DIR / "NormalizationTest.txt"
CASEFOLD_FILE = DATA_DIR / "CaseFolding.txt"
GRAPHEME_FILE = DATA_DIR / "GraphemeBreakTest.txt"


def load_module(path=MODULE_PATH):
    spec = importlib.util.spec_from_file_location("unicode18_under_test", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load %s" % path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def codepoints(field):
    field = field.strip()
    if not field:
        return []
    values = []
    for item in field.split():
        value = int(item, 16)
        if not 0 <= value <= 0x10FFFF or 0xD800 <= value <= 0xDFFF:
            raise AssertionError("invalid test code point U+%04X" % value)
        values.append(value)
    return values


def text_of(values):
    return "".join(map(chr, values))


def strip_comment(line):
    return line.split("#", 1)[0].strip()


def check_module_surface(module):
    assert module._unicode18_unicode_version == "18.0.0"
    assert module._unicode18_utf8proc_version == "2.12.0"
    for name in dir(module):
        if name.startswith("__"):
            continue
        assert name.startswith("_unicode18"), "unprefixed module name: " + name
    source = MODULE_PATH.read_text(encoding="ascii")
    assert "import unicodedata" not in source
    assert "import ctypes" not in source
    assert "import http" not in source
    assert "import socket" not in source
    assert "import urllib" not in source
    return {"module_names": sum(
        1 for name in dir(module) if not name.startswith("__")
    )}


def check_embedded_license():
    source = MODULE_PATH.read_text(encoding="ascii")
    begin = "# --- begin embedded utf8proc LICENSE.md ---\n"
    end = "# --- end embedded utf8proc LICENSE.md ---\n"
    assert source.count(begin) == 1, "license begin marker missing/duplicated"
    assert source.count(end) == 1, "license end marker missing/duplicated"
    assert "# Source: runtime/native/unicode/vendor/utf8proc/LICENSE.md" in source
    _prefix, rest = source.split(begin, 1)
    block, _suffix = rest.split(end, 1)
    reconstructed = []
    for line in block.splitlines():
        if line == "#":
            reconstructed.append("")
        elif line.startswith("# "):
            reconstructed.append(line[2:])
        else:
            raise AssertionError("license block line is not a comment: %r" % line)
    license_bytes = LICENSE_PATH.read_bytes()
    license_text = license_bytes.decode("utf-8")
    expected = license_text.splitlines()
    assert reconstructed == expected, "embedded license notice differs from LICENSE.md"
    digest = hashlib.sha256(license_bytes).hexdigest()
    assert "# SHA256: " + digest in source, "embedded license hash mismatch"
    assert "Unicode data license" in source
    assert "COPYRIGHT AND PERMISSION NOTICE" in source
    return {
        "license_sha256": digest,
        "license_lines": len(expected),
        "license_bytes": len(license_bytes),
    }


def check_snapshot_headers():
    headers = {}
    for path in (NORMALIZATION_FILE, CASEFOLD_FILE, GRAPHEME_FILE):
        with path.open(encoding="utf-8") as handle:
            first = handle.readline().strip()
        assert "18.0.0" in first, (
            "%s does not identify Unicode 18.0.0: %r" % (path, first)
        )
        headers[path.name] = first
    return {"snapshot_headers": headers}


def check_normalization(module):
    lines = 0
    checks = 0
    with NORMALIZATION_FILE.open(encoding="utf-8") as handle:
        for raw in handle:
            line = strip_comment(raw)
            if not line or line.startswith("@"):
                continue
            fields = line.split(";")
            if len(fields) < 5:
                raise AssertionError("bad NormalizationTest line: " + raw)
            c1, c2, c3, c4, c5 = (
                text_of(codepoints(field)) for field in fields[:5]
            )
            lines += 1
            for source, expected in (
                (c1, c2),
                (c2, c2),
                (c3, c2),
                (c4, c4),
                (c5, c4),
            ):
                actual = module._unicode18_normalize(0, source)
                checks += 1
                assert actual == expected, (
                    "NFC line %d: %r -> %r, expected %r"
                    % (lines, source, actual, expected)
                )
            for source, expected in (
                (c1, c3),
                (c2, c3),
                (c3, c3),
                (c4, c5),
                (c5, c5),
            ):
                actual = module._unicode18_normalize(1, source)
                checks += 1
                assert actual == expected, (
                    "NFD line %d: %r -> %r, expected %r"
                    % (lines, source, actual, expected)
                )
            for source in (c1, c2, c3, c4, c5):
                actual = module._unicode18_normalize(2, source)
                checks += 1
                assert actual == c4, (
                    "NFKC line %d: %r -> %r, expected %r"
                    % (lines, source, actual, c4)
                )
            for source in (c1, c2, c3, c4, c5):
                actual = module._unicode18_normalize(3, source)
                checks += 1
                assert actual == c5, (
                    "NFKD line %d: %r -> %r, expected %r"
                    % (lines, source, actual, c5)
                )
    assert lines == 20171, "unexpected NormalizationTest line count: %d" % lines
    assert checks == 403420, "unexpected normalization check count: %d" % checks
    return {"normalization_lines": lines, "normalization_checks": checks}


def check_casefold(module):
    full = {}
    simple_or_turkic = []
    entries = 0
    with CASEFOLD_FILE.open(encoding="utf-8") as handle:
        for raw in handle:
            line = strip_comment(raw)
            if not line:
                continue
            fields = [field.strip() for field in line.split(";")]
            if len(fields) < 3:
                raise AssertionError("bad CaseFolding line: " + raw)
            cp = int(fields[0], 16)
            status = fields[1]
            mapping = text_of(codepoints(fields[2]))
            entries += 1
            if status in ("C", "F"):
                full[cp] = mapping
            elif status in ("S", "T"):
                simple_or_turkic.append((cp, status, mapping))
            else:
                raise AssertionError("unknown CaseFolding status: " + status)
    checks = 0
    for cp in sorted(full):
        actual = module._unicode18_casefold(chr(cp))
        checks += 1
        assert actual == full[cp], (
            "casefold U+%04X: %r, expected %r"
            % (cp, actual, full[cp])
        )
    for cp, status, mapping in simple_or_turkic:
        if cp in full:
            continue
        actual = module._unicode18_casefold(chr(cp))
        checks += 1
        assert actual == chr(cp), (
            "default casefold must not use %s mapping for U+%04X" % (status, cp)
        )
    assert len(full) == 1606, "unexpected full casefold count: %d" % len(full)
    return {
        "casefold_entries": entries,
        "casefold_full": len(full),
        "casefold_checks": checks,
    }


def parse_grapheme_line(line):
    text = []
    expected = []
    position = 0
    for token in line.split():
        if token == "\u00f7":  # division sign: boundary
            expected.append(position)
        elif token == "\u00d7":  # multiplication sign: no boundary
            pass
        else:
            value = int(token, 16)
            if not 0 <= value <= 0x10FFFF or 0xD800 <= value <= 0xDFFF:
                raise AssertionError("invalid grapheme code point U+%04X" % value)
            text.append(chr(value))
            position += 1
    return "".join(text), expected


def check_grapheme(module):
    tests = 0
    with GRAPHEME_FILE.open(encoding="utf-8") as handle:
        for raw in handle:
            line = strip_comment(raw)
            if not line:
                continue
            text, expected = parse_grapheme_line(line)
            actual = module._unicode18_boundaries(text)
            tests += 1
            assert actual == expected, (
                "grapheme test %d: %r -> %r, expected %r"
                % (tests, text, actual, expected)
            )
            assert actual[0] == 0
            assert actual[-1] == len(text)
    assert tests == 853, "unexpected GraphemeBreakTest count: %d" % tests
    return {"grapheme_tests": tests}


def check_edges(module):
    normalize = module._unicode18_normalize
    casefold = module._unicode18_casefold
    boundaries = module._unicode18_boundaries

    assert normalize(0, "") == ""
    assert normalize(1, "") == ""
    assert normalize(2, "") == ""
    assert normalize(3, "") == ""
    assert casefold("") == ""
    assert boundaries("") == [0]

    for form in range(4):
        assert normalize(form, "\x00") == "\x00"
        assert normalize(form, "a\x00b") == "a\x00b"
        assert normalize(form, "\x00\x00") == "\x00\x00"
        assert normalize(form, "\x00\u0301") == "\x00\u0301"
    assert casefold("\x00A\x00") == "\x00a\x00"
    assert boundaries("\x00") == [0, 1]
    assert boundaries("\x00\x00") == [0, 1, 2]
    assert boundaries("a\x00b") == [0, 1, 2, 3]
    assert boundaries("\x00\u0301") == [0, 1, 2]

    # One-to-many and recursive mappings.
    assert casefold("\u00df") == "ss"
    assert casefold("\ufb03") == "ffi"
    assert casefold("\u0130") == "i\u0307"
    assert casefold("\u03c2") == "\u03c3"
    assert casefold("Stra\u00dfe") == "strasse"
    assert normalize(2, "\ufb03") == "ffi"
    assert normalize(3, "\ufb03") == "ffi"
    assert normalize(2, "\u2460") == "1"
    assert normalize(0, "A\u030a") == "\u00c5"
    assert normalize(1, "\u00c5") == "A\u030a"
    assert normalize(1, "\u1e69") == "s\u0323\u0307"
    assert normalize(0, "\u1e69") == "\u1e69"
    # Equal combining classes do not compose the U+0344 decomposition.
    assert normalize(1, "\u0344") == "\u0308\u0301"
    assert normalize(0, "\u0344") == "\u0308\u0301"
    # Hangul syllable decomposition and composition.
    assert normalize(1, "\uac01") == "\u1100\u1161\u11a8"
    assert normalize(0, "\u1100\u1161\u11a8") == "\uac01"
    assert normalize(0, "\uac00\u11a8") == "\uac01"

    # Regional indicators pair only in twos.
    assert boundaries("\U0001f1e6\U0001f1e7") == [0, 2]
    assert boundaries("\U0001f1e6\U0001f1e7\U0001f1e8") == [0, 2, 3]
    assert boundaries("\U0001f1e6\U0001f1e7\U0001f1e8\U0001f1e9") == [0, 2, 4]
    # Emoji ZWJ sequences and keycaps.
    assert boundaries("\U0001f469\u200d\U0001f4bb") == [0, 3]
    assert boundaries("\U0001f469\U0001f3fd\u200d\U0001f4bb") == [0, 4]
    assert boundaries("\U0001f469\ufe0f\u200d\U0001f4bb") == [0, 4]
    assert boundaries("1\ufe0f\u20e3") == [0, 3]
    assert boundaries("\U0001f3f4\U000e0067\U000e0062\U000e0065\U000e006e"
                       "\U000e0067\U000e007f") == [0, 7]
    # Indic conjunct break: consonant + linker (+ extend) + consonant.
    assert boundaries("\u0915\u094d\u0937") == [0, 3]
    assert boundaries("\u0915\u094d\u200d\u0937") == [0, 4]
    assert boundaries("\u0915\u094d\u0300\u0937") == [0, 4]
    assert boundaries("\u0915\u0937") == [0, 1, 2]

    assert module._unicode18_simple_rune_upper(ord("a")) == ord("A")
    assert module._unicode18_simple_rune_lower(ord("A")) == ord("a")
    assert module._unicode18_simple_rune_upper(0x10428) == 0x10400
    assert module._unicode18_simple_rune_lower(0x10400) == 0x10428
    assert module._unicode18_simple_rune_upper(0x110000) == 0x110000
    assert module._unicode18_rune_to_upper is module._unicode18_simple_rune_upper
    assert module._unicode18_rune_to_lower is module._unicode18_simple_rune_lower

    for bad_form in (-1, 4, 99):
        try:
            normalize(bad_form, "a")
        except ValueError:
            pass
        else:
            raise AssertionError("normalization accepted form %r" % bad_form)
    for function in (
        lambda: normalize("0", "a"),
        lambda: normalize(0, b"a"),
        lambda: casefold(b"a"),
        lambda: boundaries(b"a"),
    ):
        try:
            function()
        except TypeError:
            pass
        else:
            raise AssertionError("non-string input was accepted")

    return {"edge_checks": "passed"}


def run_generator(python, output, timeout):
    completed = subprocess.run(
        [python, str(GENERATOR_PATH), "--output", str(output)],
        cwd=str(ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )
    if completed.returncode != 0:
        raise AssertionError(
            "generator failed with %d:\n%s\n%s"
            % (completed.returncode, completed.stdout, completed.stderr)
        )
    return completed.stdout.strip()


def check_generation(python, timeout):
    with tempfile.TemporaryDirectory(prefix="unicode18-test-") as directory:
        target = Path(directory) / "generated.py"
        first_message = run_generator(python, target, timeout)
        first_bytes = target.read_bytes()
        second_message = run_generator(python, target, timeout)
        second_bytes = target.read_bytes()
        assert first_bytes == second_bytes, "two generator runs differ"
        assert first_message == second_message, "generator message is unstable"
        committed = MODULE_PATH.read_bytes()
        assert first_bytes == committed, (
            "runtime/python/unicode18.py is stale; regenerate it"
        )
        digest = hashlib.sha256(first_bytes).hexdigest()
    return {
        "generator_bytes": len(committed),
        "generator_sha256": digest,
        "generator_message": first_message,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--skip-generation",
        action="store_true",
        help="skip the compile-twice generator determinism check",
    )
    parser.add_argument(
        "--timeout", type=float, default=300.0,
        help="timeout in seconds for each generator invocation",
    )
    args = parser.parse_args(argv)

    for path in (
        MODULE_PATH,
        GENERATOR_PATH,
        LICENSE_PATH,
        NORMALIZATION_FILE,
        CASEFOLD_FILE,
        GRAPHEME_FILE,
    ):
        assert path.is_file(), "missing required file: %s" % path

    module = load_module()
    report = {"python": sys.version.split()[0]}
    report.update(check_module_surface(module))
    report.update(check_embedded_license())
    report.update(check_snapshot_headers())
    report.update(check_normalization(module))
    report.update(check_casefold(module))
    report.update(check_grapheme(module))
    report.update(check_edges(module))
    if not args.skip_generation:
        report.update(check_generation(sys.executable, args.timeout))

    print(
        "unicode18 ok: Unicode %s, utf8proc %s, %d normalization checks, "
        "%d full casefold, %d grapheme tests"
        % (
            module._unicode18_unicode_version,
            module._unicode18_utf8proc_version,
            report["normalization_checks"],
            report["casefold_full"],
            report["grapheme_tests"],
        )
    )
    if not args.skip_generation:
        print(
            "generator deterministic: %d bytes, sha256 %s"
            % (report["generator_bytes"], report["generator_sha256"])
        )
    return report


if __name__ == "__main__":
    main()
