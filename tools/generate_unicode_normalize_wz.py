#!/usr/bin/env python3
"""Generate stdlib/unicode/unicode_normalize_tables.wz from repo Unicode 18 data.

The source of truth is runtime/python/unicode18.py, the checked-in Unicode
18.0.0 helper generated from the vendored utf8proc 2.12.0 sources.  The
generator imports only that Python module's embedded tables; it does not
compile C, call the native runtime, or read external Unicode files.

Usage:
    python3 tools/generate_unicode_normalize_wz.py
    python3 tools/generate_unicode_normalize_wz.py --check
"""

import argparse
import hashlib
import importlib.util
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / "runtime" / "python" / "unicode18.py"
DEFAULT_OUTPUT = ROOT / "stdlib" / "unicode" / "unicode_normalize_tables.wz"
LICENSE_PATH = ROOT / "runtime" / "native" / "unicode" / "vendor" / "utf8proc" / "LICENSE.md"

UNICODE_VERSION = "18.0.0"
HANGUL_SBASE = 0xAC00
HANGUL_SCOUNT = 11172
HANGUL_LBASE = 0x1100
HANGUL_VBASE = 0x1161
HANGUL_TBASE = 0x11A7
HANGUL_VCOUNT = 21
HANGUL_TCOUNT = 28

# (WZ name, source table, maximum decomposition length, description)
TABLES = (
    ("nfd", "nfd", 4, "NFD"),
    ("nfkd", "nfkd", 18, "NFKD"),
)

# One page stores every scalar in a 16-scalar window.  The page starts with a
# 32-byte header holding two 6-bit offset digits per slot, high digit first,
# all biased by 32.  Offset zero means no decomposition.  A nonzero offset
# points from the page start to a record containing one 6-bit target count
# followed by four 6-bit digits per target.  Records are packed after the
# header; offsets must fit in 12 bits.  Pages are small enough that direct
# string indexing stays cheap, while the generated source has only a few
# hundred string literals.
#
# Page fields are printable ASCII so native string indexing is constant time
# instead of scanning preceding UTF-8.  JSON string escaping quotes and
# backslashes for the generated literals.
PAGE_SHIFT = 4
PAGE_SIZE = 1 << PAGE_SHIFT
PAGE_BIAS = 32
PAGE_DIGITS = 4
PAGE_DIGIT_BITS = 6
PAGE_DIGIT_MASK = (1 << PAGE_DIGIT_BITS) - 1
PAGE_HEADER_BYTES = PAGE_SIZE * 2
PAGE_OFFSET_BITS = 12
PAGE_OFFSET_LIMIT = 1 << PAGE_OFFSET_BITS


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def load_source(path):
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location("_woozy_unicode18_source", path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load " + str(path))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if module._unicode18_unicode_version != UNICODE_VERSION:
        raise RuntimeError("source Unicode version is not 18.0.0")
    return module, module._unicode18_load_tables()


def hangul_decompose(cp):
    sindex = cp - HANGUL_SBASE
    lead = HANGUL_LBASE + sindex // (HANGUL_VCOUNT * HANGUL_TCOUNT)
    vowel = HANGUL_VBASE + (sindex % (HANGUL_VCOUNT * HANGUL_TCOUNT)) // HANGUL_TCOUNT
    trail = sindex % HANGUL_TCOUNT
    result = [lead, vowel]
    if trail:
        result.append(HANGUL_TBASE + trail)
    return tuple(result)


def scalar(cp):
    return 0 <= cp <= 0x10FFFF and not 0xD800 <= cp <= 0xDFFF


def validate_mapping(mapping, label, max_length):
    if not mapping:
        raise RuntimeError(label + " table is empty")
    for cp, targets in mapping.items():
        if not scalar(cp) or cp == 0:
            raise RuntimeError(label + " has an invalid source")
        if not 1 <= len(targets) <= max_length:
            raise RuntimeError("%s row U+%04X has %d targets" % (label, cp, len(targets)))
        for target in targets:
            if not scalar(target) or target == 0:
                raise RuntimeError("%s row U+%04X has an invalid target" % (label, cp))
        for target in targets:
            if target in mapping:
                raise RuntimeError("%s row U+%04X is not fully decomposed" % (label, cp))


def validate_hangul(nfd, nfkd):
    for cp in range(HANGUL_SBASE, HANGUL_SBASE + HANGUL_SCOUNT):
        expected = hangul_decompose(cp)
        if nfd.get(cp) != expected or nfkd.get(cp) != expected:
            raise RuntimeError("Hangul decomposition mismatch at U+%04X" % cp)


def normalize_rows(mapping):
    return {
        cp: tuple(targets)
        for cp, targets in mapping.items()
        if not HANGUL_SBASE <= cp < HANGUL_SBASE + HANGUL_SCOUNT
    }


def validate_composition(composition):
    seen = set()
    for (first, second), composite in composition.items():
        if not (scalar(first) and scalar(second) and scalar(composite)):
            raise RuntimeError("composition pair has a non-scalar")
        if first == 0 or second == 0 or composite == 0:
            raise RuntimeError("composition pair contains U+0000")
        key = (first, second)
        if key in seen:
            raise RuntimeError("duplicate composition pair")
        seen.add(key)


def ccc_ranges(ccc):
    ranges = []
    for cp, value in sorted(ccc.items()):
        if not scalar(cp) or not 1 <= value <= 255:
            raise RuntimeError("bad combining class at U+%04X" % cp)
        if ranges and ranges[-1][1] + 1 == cp and ranges[-1][2] == value:
            ranges[-1] = (ranges[-1][0], cp, value)
        else:
            ranges.append((cp, cp, value))
    return ranges


def quick_check_ranges(module, form, decomposition, composition):
    """Return scalars that are not NFC/NFKC quick-check Yes.

    A scalar is No when normalizing the single scalar for this form changes it.
    A scalar is Maybe when it can combine with a preceding character.  Hangul
    V and T jamo are added explicitly because their algorithmic compositions
    are not rows in the composition table.  Treating Maybe as "not Yes" gives
    the conservative Quick_Check identity test: all-Yes plus canonical order
    is sufficient, while the converse is not assumed.
    """
    maybe = {second for _first, second in composition}
    maybe.update(range(0x1161, 0x1176))
    maybe.update(range(0x11a8, 0x11c3))
    bad = set(maybe)
    for cp, targets in decomposition.items():
        if form == 0:
            changed = module._unicode18_normalize(0, chr(cp)) != chr(cp)
        else:
            changed = module._unicode18_normalize(2, chr(cp)) != chr(cp)
        # A stable precomposed scalar can still expose its first decomposed
        # component to the preceding scalar.  If that component is a possible
        # composition second, this scalar is not safely Quick_Check Yes.
        if changed or (len(targets) > 1 and targets[0] in maybe):
            bad.add(cp)
    ranges = []
    for cp in sorted(bad):
        if not scalar(cp):
            raise RuntimeError("quick-check range has an invalid scalar")
        if ranges and ranges[-1][1] + 1 == cp:
            ranges[-1] = (ranges[-1][0], cp)
        else:
            ranges.append((cp, cp))
    return ranges


def emit_bad_ranges(lines, ranges, name):
    records = [(start, end, 1) for start, end in ranges]
    emit_range_selector(lines, records, name)


def encode_page(slots, width):
    offsets = [0] * PAGE_SIZE
    records = []
    for slot in range(PAGE_SIZE):
        targets = slots.get(slot)
        if targets is None:
            continue
        if not 1 <= len(targets) < width:
            raise RuntimeError("page target count exceeds allowed width")
        if len(targets) > PAGE_DIGIT_MASK:
            raise RuntimeError("page target count does not fit in 6 bits")
        offset = PAGE_HEADER_BYTES + len(records)
        if offset >= PAGE_OFFSET_LIMIT:
            raise RuntimeError("page record offset does not fit in 12 bits")
        offsets[slot] = offset
        records.append(chr(PAGE_BIAS + len(targets)))
        for value in targets:
            if not 0 <= value < (1 << (PAGE_DIGIT_BITS * PAGE_DIGITS)):
                raise RuntimeError("page scalar does not fit in four 6-bit digits")
            for shift in range(PAGE_DIGIT_BITS * (PAGE_DIGITS - 1), -1,
                               -PAGE_DIGIT_BITS):
                records.append(chr(PAGE_BIAS + ((value >> shift) & PAGE_DIGIT_MASK)))
    header = []
    for offset in offsets:
        if not 0 <= offset < PAGE_OFFSET_LIMIT:
            raise RuntimeError("page slot offset does not fit in 12 bits")
        header.append(chr(PAGE_BIAS + ((offset >> PAGE_DIGIT_BITS) & PAGE_DIGIT_MASK)))
        header.append(chr(PAGE_BIAS + (offset & PAGE_DIGIT_MASK)))
    return "".join(header + records)


def page_literals(rows, width):
    pages = {}
    for cp, targets in rows.items():
        page = cp >> PAGE_SHIFT
        slot = cp & (PAGE_SIZE - 1)
        pages.setdefault(page, {})[slot] = targets
    return {page: encode_page(slots, width)
            for page, slots in pages.items()}


# Large generated selectors are emitted as bounded named helpers.  The
# sorted entries are split into contiguous balanced chunks of at most
# SELECTOR_CHUNK_ENTRIES leaves; every chunk becomes its own function and the
# public selector performs a numeric binary search over the chunk boundaries
# before calling the matching helper.  The total comparison count stays
# O(log n) (log chunks + log chunk size), the comparisons remain integer
# branches, and no selector scans linearly.
SELECTOR_CHUNK_ENTRIES = 32


def selector_chunk_bounds(count):
    if count == 0:
        return []
    chunks = (count + SELECTOR_CHUNK_ENTRIES - 1) // SELECTOR_CHUNK_ENTRIES
    base, extra = divmod(count, chunks)
    bounds = []
    start = 0
    for index in range(chunks):
        size = base + (1 if index < extra else 0)
        bounds.append((start, start + size))
        start += size
    return bounds


def emit_key_tree(lines, keyed, lo, hi, indent, key_var, default, low, high,
                  leaf):
    pad = "    " * indent
    if lo >= hi:
        lines.append(pad + "return %s;" % default)
        return
    middle = (lo + hi) // 2
    entry = keyed[middle]
    lines.append(pad + "if %s < %d {" % (key_var, low(entry)))
    emit_key_tree(lines, keyed, lo, middle, indent + 1, key_var, default,
                  low, high, leaf)
    lines.append(pad + "} else if %s > %d {" % (key_var, high(entry)))
    emit_key_tree(lines, keyed, middle + 1, hi, indent + 1, key_var, default,
                  low, high, leaf)
    lines.append(pad + "} else {")
    lines.append(pad + "    return %s;" % leaf(entry))
    lines.append(pad + "}")


def emit_key_dispatch(lines, keyed, bounds, chunk_names, lo, hi, indent,
                      key_var, default, low, high):
    pad = "    " * indent
    if lo >= hi:
        lines.append(pad + "return %s;" % default)
        return
    middle = (lo + hi) // 2
    start, end = bounds[middle]
    lines.append(pad + "if %s < %d {" % (key_var, low(keyed[start])))
    emit_key_dispatch(lines, keyed, bounds, chunk_names, lo, middle,
                      indent + 1, key_var, default, low, high)
    lines.append(pad + "} else if %s > %d {" % (key_var,
                                                high(keyed[end - 1])))
    emit_key_dispatch(lines, keyed, bounds, chunk_names, middle + 1, hi,
                      indent + 1, key_var, default, low, high)
    lines.append(pad + "} else {")
    lines.append(pad + "    return %s(%s);" % (chunk_names[middle], key_var))
    lines.append(pad + "}")


def emit_key_selector(lines, keyed, name, default, low, high, leaf, ret):
    bounds = selector_chunk_bounds(len(keyed))
    chunk_names = ["%s_chunk_%d" % (name, index)
                   for index in range(len(bounds))]
    for index, (start, end) in enumerate(bounds):
        lines.append("func %s(rune u32) %s {" % (chunk_names[index], ret))
        emit_key_tree(lines, keyed, start, end, 1, "rune", default,
                      low, high, leaf)
        lines.append("    return %s;" % default)
        lines.append("}")
        lines.append("")
    lines.append("func %s(rune u32) %s {" % (name, ret))
    emit_key_dispatch(lines, keyed, bounds, chunk_names, 0, len(bounds), 1,
                      "rune", default, low, high)
    lines.append("    return %s;" % default)
    lines.append("}")


def emit_page_selector(lines, pages, name):
    keyed = [(page, pages[page]) for page in sorted(pages)]
    emit_key_selector(lines, keyed, name, '""',
                      lambda entry: entry[0], lambda entry: entry[0],
                      lambda entry: json.dumps(entry[1]), "string")


def emit_range_selector(lines, ranges, name):
    emit_key_selector(lines, ranges, name, "0",
                      lambda entry: entry[0], lambda entry: entry[1],
                      lambda entry: str(entry[2]), "u32")


def emit_pair_tree(lines, pairs, lo, hi, indent):
    pad = "    " * indent
    if lo >= hi:
        lines.append(pad + "return 0;")
        return
    middle = (lo + hi) // 2
    first, second, composite = pairs[middle]
    before = "first < %d || (first == %d && second < %d)" % (
        first, first, second)
    after = "first > %d || (first == %d && second > %d)" % (
        first, first, second)
    lines.append(pad + "if " + before + " {")
    emit_pair_tree(lines, pairs, lo, middle, indent + 1)
    lines.append(pad + "} else if " + after + " {")
    emit_pair_tree(lines, pairs, middle + 1, hi, indent + 1)
    lines.append(pad + "} else {")
    lines.append(pad + "    return %d;" % composite)
    lines.append(pad + "}")


def emit_pair_dispatch(lines, pairs, bounds, chunk_names, lo, hi, indent):
    pad = "    " * indent
    if lo >= hi:
        lines.append(pad + "return 0;")
        return
    middle = (lo + hi) // 2
    start, end = bounds[middle]
    first, second = pairs[start][0], pairs[start][1]
    lines.append(pad + "if first < %d || (first == %d && second < %d) {" % (
        first, first, second))
    emit_pair_dispatch(lines, pairs, bounds, chunk_names, lo, middle,
                       indent + 1)
    first, second = pairs[end - 1][0], pairs[end - 1][1]
    lines.append(pad + "} else if first > %d || (first == %d && second > %d) {" % (
        first, first, second))
    emit_pair_dispatch(lines, pairs, bounds, chunk_names, middle + 1, hi,
                       indent + 1)
    lines.append(pad + "} else {")
    lines.append(pad + "    return %s(first,second);" % chunk_names[middle])
    lines.append(pad + "}")


def emit_compose_selector(lines, pairs, name):
    pairs = sorted(pairs)
    bounds = selector_chunk_bounds(len(pairs))
    chunk_names = ["%s_chunk_%d" % (name, index)
                   for index in range(len(bounds))]
    for index, (start, end) in enumerate(bounds):
        lines.append("func %s(first u32, second u32) u32 {" % chunk_names[index])
        emit_pair_tree(lines, pairs, start, end, 1)
        lines.append("    return 0;")
        lines.append("}")
        lines.append("")
    lines.append("func %s(first u32, second u32) u32 {" % name)
    emit_pair_dispatch(lines, pairs, bounds, chunk_names, 0, len(bounds), 1)
    lines.append("    return 0;")
    lines.append("}")


def license_notice():
    data = LICENSE_PATH.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    lines = [
        "// Embedded utf8proc and Unicode data license notices.",
        "// Source: runtime/native/unicode/vendor/utf8proc/LICENSE.md",
        "// SHA-256: " + digest,
        "// --- begin embedded LICENSE.md ---",
    ]
    text = data.decode("utf-8")
    for line in text.splitlines():
        lines.append("// " + line if line else "//")
    lines.append("// --- end embedded LICENSE.md ---")
    return lines


def render(source_digest, payload_digest, nfd, nfkd, ccc, composition,
           nfc_bad, nfkc_bad):
    lines = [
        "// Generated by tools/generate_unicode_normalize_wz.py. Do not edit by hand.",
        "//",
        "// Unicode 18.0.0 canonical/compatibility decomposition, canonical",
        "// composition, canonical combining classes and conservative NFC/NFKC",
        "// quick-check identity sets for std/unicode.  Hangul syllables are",
        "// decomposed algorithmically and are not stored in the tables.  This",
        "// is a modified extraction of the Unicode 18.0.0 data shipped with the",
        "// repository.",
        "//",
        "// Source: runtime/python/unicode18.py",
        "//   SHA-256 " + source_digest,
        "//   embedded payload SHA-256 " + payload_digest,
        "// The source helper is generated from the vendored utf8proc 2.12.0",
        "// sources under runtime/native/unicode/vendor/utf8proc.",
        "//",
    ]
    lines.extend(license_notice())
    lines.extend([
        "package unicode;",
        "",
        'const WZ_NORMALIZE_UNICODE_VERSION string = "18.0.0";',
        "const WZ_NORMALIZE_PAGE_SHIFT u32 = %d;" % PAGE_SHIFT,
        "const WZ_NORMALIZE_PAGE_MASK u32 = %d;" % (PAGE_SIZE - 1),
        "",
    ])

    for label, attr, max_length, title in TABLES:
        rows = {"nfd": nfd, "nfkd": nfkd}[attr]
        width = max_length + 1
        pages = page_literals(rows, width)
        lines.append("// %s: %d non-Hangul entries, %d occupied %d-scalar pages."
                     % (title, len(rows), len(pages), PAGE_SIZE))
        lines.append("// Each page has a 32-byte ASCII header of two 6-bit "
                     "record-offset digits per slot; records hold one 6-bit "
                     "count and four 6-bit digits per target, all biased by 32.")
        emit_page_selector(lines, pages, "wz_normalize_%s_page" % label)
        lines.append("")

    lines.append("// Canonical combining classes: %d nonzero ranges." % len(ccc))
    emit_range_selector(lines, ccc, "wz_normalize_ccc")
    lines.append("")

    lines.append("// NFC quick-check Not-Yes scalars: %d ranges." % len(nfc_bad))
    emit_bad_ranges(lines, nfc_bad, "wz_normalize_nfc_bad")
    lines.append("")
    lines.append("// NFKC quick-check Not-Yes scalars: %d ranges." % len(nfkc_bad))
    emit_bad_ranges(lines, nfkc_bad, "wz_normalize_nfkc_bad")
    lines.append("")

    pairs = [(first, second, composite)
             for (first, second), composite in composition.items()]
    lines.append("// Canonical composition pairs: %d." % len(pairs))
    emit_compose_selector(lines, pairs, "wz_normalize_compose_pair")
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)

    source = args.source.resolve()
    module, tables = load_source(source)
    nfd_raw, nfkd_raw, _casefold, _upper, _lower, composition, ccc, _grapheme = tables

    expected = {"nfd": 13253, "nfkd": 17154, "composition": 961}
    for name, actual in (("nfd", len(nfd_raw)), ("nfkd", len(nfkd_raw)),
                         ("composition", len(composition))):
        if actual != expected[name]:
            raise RuntimeError("%s table has %d rows, expected %d"
                               % (name, actual, expected[name]))
    validate_mapping(nfd_raw, "NFD", 4)
    validate_mapping(nfkd_raw, "NFKD", 18)
    validate_hangul(nfd_raw, nfkd_raw)
    validate_composition(composition)

    nfd = normalize_rows(nfd_raw)
    nfkd = normalize_rows(nfkd_raw)
    if len(nfd) != 2081 or len(nfkd) != 5982:
        raise RuntimeError("unexpected non-Hangul entry counts")
    ranges = ccc_ranges(ccc)
    nfc_bad = quick_check_ranges(module, 0, nfd_raw, composition)
    nfkc_bad = quick_check_ranges(module, 2, nfkd_raw, composition)
    text = render(sha256_file(source), module._unicode18_payload_sha256,
                  nfd, nfkd, ranges, composition, nfc_bad, nfkc_bad)

    if args.check:
        current = args.output.read_text(encoding="utf-8")
        if current != text:
            raise SystemExit("generated table differs from " + str(args.output))
        print("ok " + str(args.output))
        return
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8", newline="\n")
    print("wrote %s (%d bytes)" % (args.output, len(text.encode("utf-8"))))


if __name__ == "__main__":
    main()
