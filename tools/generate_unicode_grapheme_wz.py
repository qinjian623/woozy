#!/usr/bin/env python3
"""Generate the Woozy grapheme property table from the pinned Unicode runtime.

The data source is runtime/python/unicode18.py, itself generated from the fixed
utf8proc 2.12.0 sources and validated against the official Unicode 18.0.0
NormalizationTest, CaseFolding and GraphemeBreakTest files by
tests/python/unicode18_test.py.  This generator turns the captured packed
(bound class, Indic_Conjunct_Break) property into one balanced decision tree so
the package-internal scanner behind std/unicode.boundaries needs no per-call
table decode or allocation.

The official UCD inputs are pinned below with their SHA-256 hashes.  The
normal generation path uses the captured utf8proc runtime so unassigned code
points keep the existing native behavior; --ucd-dir additionally verifies the
pinned official files before generating.

Usage:
    python3 tools/generate_unicode_grapheme_wz.py
    python3 tools/generate_unicode_grapheme_wz.py --check
    python3 tools/generate_unicode_grapheme_wz.py --ucd-dir /path/to/ucd-18.0.0
"""

import argparse
import hashlib
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "runtime/python/unicode18.py"
OUTPUT = ROOT / "stdlib/unicode/unicode_grapheme_tables.wz"
LICENSE = ROOT / "stdlib/unicode/UNICODE-LICENSE.txt"

# Pinned official inputs for the captured Unicode 18.0.0 runtime.  Relative
# paths are resolved under --ucd-dir; the generator only needs them for the
# optional hash verification because the captured runtime is self-contained.
UCD_INPUTS = (
    ("auxiliary/GraphemeBreakProperty.txt",
     "https://www.unicode.org/Public/18.0.0/ucd/auxiliary/GraphemeBreakProperty.txt",
     "0839dcb79e4ac639ecd538b1abf7c9d22e3f9dd265b7e182d33627aa4d75b45a",
     "Grapheme_Cluster_Break"),
    ("emoji/emoji-data.txt",
     "https://www.unicode.org/Public/18.0.0/ucd/emoji/emoji-data.txt",
     "80d00f8e616a0ef27fd6b8de3b758c06383b5d917e2977709578e68baf733bf1",
     "Extended_Pictographic"),
    ("DerivedCoreProperties.txt",
     "https://www.unicode.org/Public/18.0.0/ucd/DerivedCoreProperties.txt",
     "09c928886a178fcafd93c29e4bd59073a058e5a100b716d425cb563ab50f68c9",
     "InCB (Indic_Conjunct_Break)"),
    ("UnicodeData.txt",
     "https://www.unicode.org/Public/18.0.0/ucd/UnicodeData.txt",
     "0736451de439ae7baf1425136617da495e09ee5afbe6e394374db7009ea08950",
     "general categories and names"),
)
EXPECTED_LICENSE_SHA256 = "e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96"

# utf8proc bound classes; the packed value is bound | indic << 8.
NAMES = {
    1: "other", 2: "cr", 3: "lf", 4: "control", 5: "extend", 6: "l",
    7: "v", 8: "t", 9: "lv", 10: "lvt", 11: "regional_indicator",
    12: "spacingmark", 13: "prepend", 14: "zwj", 19: "extended_pictographic",
    20: "e_zwg",
}
ICB = {0: "none", 1: "linker", 2: "consonant", 3: "extend"}


def load_reference():
    spec = importlib.util.spec_from_file_location("unicode18_reference", REFERENCE)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load %s" % REFERENCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if module._unicode18_unicode_version != "18.0.0":
        raise RuntimeError("unexpected Unicode version")
    if module._unicode18_utf8proc_version != "2.12.0":
        raise RuntimeError("unexpected utf8proc version")
    tables = module._unicode18_load_tables()
    grapheme = tables[7]
    return module, grapheme


def property_runs(grapheme):
    """Return sorted non-default (start, end, packed) runs over all scalars."""
    runs = []
    start = 0
    current = grapheme.get(0, 1)
    for cp in range(1, 0x110000):
        value = grapheme.get(cp, 1)
        if value != current:
            if current != 1:
                runs.append((start, cp - 1, current))
            start = cp
            current = value
    if current != 1:
        runs.append((start, 0x10FFFF, current))
    # Adjacent non-default runs with the same value cannot occur by
    # construction, and default gaps separate different values.
    for left, right in zip(runs, runs[1:]):
        assert left[1] < right[0], (left, right)
    return runs


def emit_tree(runs, low, high, indent, lines):
    pad = "    " * indent
    if low > high:
        lines.append("%sreturn grapheme_other;" % pad)
        return
    middle = (low + high) // 2
    start, end, packed = runs[middle]
    lines.append("%sif offset < 0x%x {" % (pad, start))
    emit_tree(runs, low, middle - 1, indent + 1, lines)
    lines.append("%s} else {" % pad)
    lines.append("%s    if offset <= 0x%x {" % (pad, end))
    lines.append("%s        return 0x%x;" % (pad, packed))
    lines.append("%s    }" % pad)
    emit_tree(runs, middle + 1, high, indent + 1, lines)
    lines.append("%s}" % pad)


def block_runs(runs):
    """Split non-default runs at 256-code-point block boundaries."""
    blocks = {}
    for start, end, packed in runs:
        for block in range(start >> 8, (end >> 8) + 1):
            low = max(start, block << 8) & 0xff
            high = min(end, (block << 8) + 0xff) & 0xff
            blocks.setdefault(block, []).append((low, high, packed))
    return blocks


def verify_ucd_inputs(directory):
    for relative, _url, expected, what in UCD_INPUTS:
        path = directory / relative
        if not path.is_file():
            raise SystemExit("missing pinned UCD input: %s" % path)
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise SystemExit("hash mismatch for %s (%s): %s != %s" % (
                relative, what, actual, expected))


def table_source(runs, reference_sha, license_sha):
    blocks = block_runs(runs)
    lines = [
        "// Generated by tools/generate_unicode_grapheme_wz.py. Do not edit by hand.",
        "//",
        "// Unicode 18.0.0 UAX #29 extended grapheme cluster data for the",
        "// package-internal scanner behind std/unicode.boundaries.",
        "//",
        "// Source of truth: runtime/python/unicode18.py (Unicode 18.0.0,",
        "// utf8proc 2.12.0), SHA-256 %s." % reference_sha,
        "// That captured runtime is generated from the fixed utf8proc sources and",
        "// checked against the official GraphemeBreakTest-18.0.0.txt,",
        "// NormalizationTest-18.0.0.txt and CaseFolding-18.0.0.txt by",
        "// tests/python/unicode18_test.py.",
        "//",
        "// Pinned official inputs behind that runtime (verified by --ucd-dir):",
    ]
    for relative, url, expected, what in UCD_INPUTS:
        lines.append("//   %s (%s)" % (relative, what))
        lines.append("//     %s" % url)
        lines.append("//     SHA-256 %s" % expected)
    lines += [
        "//",
        "// A property value packs (bound class | indic_conjunct_break << 8) using",
        "// the utf8proc class numbers so the language-side implementation stays",
        "// aligned with the existing native runtime.  Unassigned code points are",
        "// Other/None to match utf8proc.",
        "//",
        "// Lookup first selects a 256-code-point block; only %d blocks contain a" % len(blocks),
        "// non-default run and each block is a small balanced range tree over the",
        "// low byte.  This keeps the language-side scanner allocation-free and",
        "// avoids decoding a table page on every call.",
        "//",
        "// Data license: UNICODE-LICENSE.txt in this directory (SHA-256 %s)." % license_sha,
        "package unicode;",
        "",
        "// utf8proc bound-class numbers, used by the generated blocks.",
    ]
    for value, name in sorted(NAMES.items()):
        lines.append("const grapheme_%s u32 = %d;" % (name, value))
    lines += [
        "",
        "// Indic_Conjunct_Break values, packed above the bound class.",
        "const grapheme_icb_linker u32 = 1;",
        "const grapheme_icb_consonant u32 = 2;",
        "const grapheme_icb_extend u32 = 3;",
        "",
        "// Property lookup for one scalar: bound class in the low byte and",
        "// Indic_Conjunct_Break above it.",
        "func grapheme_property(rune u32) u32 {",
        "    let offset = rune & 0xff;",
        "    switch rune >> 8 {",
    ]
    for block in sorted(blocks):
        lines.append("        case %d: {" % block)
        emit_tree(blocks[block], 0, len(blocks[block]) - 1, 3, lines)
        lines.append("        }")
    lines += [
        "        default: return grapheme_other;",
        "    }",
        "    return grapheme_other;",
        "}",
        "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="compare the generated text with the committed file")
    parser.add_argument("--ucd-dir", type=Path,
                        help="verify the pinned Unicode 18.0.0 UCD inputs in DIR")
    args = parser.parse_args()
    if args.ucd_dir is not None:
        if not args.ucd_dir.is_dir():
            raise SystemExit("not a directory: %s" % args.ucd_dir)
        verify_ucd_inputs(args.ucd_dir)
    reference_sha = hashlib.sha256(REFERENCE.read_bytes()).hexdigest()
    license_sha = hashlib.sha256(LICENSE.read_bytes()).hexdigest()
    if license_sha != EXPECTED_LICENSE_SHA256:
        raise SystemExit("UNICODE-LICENSE.txt changed: %s != %s" % (
            license_sha, EXPECTED_LICENSE_SHA256))
    module, grapheme = load_reference()
    runs = property_runs(grapheme)
    text = table_source(runs, reference_sha, license_sha)
    if args.check:
        current = OUTPUT.read_text()
        if current != text:
            raise SystemExit("generated table differs from %s" % OUTPUT)
        print("table ok: %d runs, %d bytes, sha256 %s" % (
            len(runs), len(text.encode()), hashlib.sha256(text.encode()).hexdigest()))
        return
    OUTPUT.write_text(text)
    print("wrote %s: %d runs, %d bytes, sha256 %s" % (
        OUTPUT, len(runs), len(text.encode()),
        hashlib.sha256(text.encode()).hexdigest()))


if __name__ == "__main__":
    main()
