#!/usr/bin/env python3
"""Generate runtime/python/unicode18.py from the fixed utf8proc 2.12 data.

The vendored utf8proc translation unit is compiled as a host program that
enumerates every scalar value.  The resulting tables are stored in the Python
module itself, compressed and base85 encoded, so the embedded runtime has no
data file, package, pip, ctypes or network dependency.

Only the Python standard library is used.  The generated source is
deterministic: the same vendor files produce byte-identical output.

Usage:
    python3 tools/generate_unicode18.py
    python3 tools/generate_unicode18.py --output /tmp/unicode18.py
"""

import argparse
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

SCHEMA = 1
ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "runtime" / "native" / "unicode" / "vendor" / "utf8proc"
LICENSE_PATH = VENDOR / "LICENSE.md"
LICENSE_SOURCE = "runtime/native/unicode/vendor/utf8proc/LICENSE.md"
DEFAULT_OUTPUT = ROOT / "runtime" / "python" / "unicode18.py"

C_PROGRAM = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define UTF8PROC_STATIC 1
#include "utf8proc.c"

/* Decode the direct (non-recursive) canonical decomposition sequence that
   utf8proc stores for a codepoint.  This is the same sequence encoding used
   by seqindex_write_char_decomposed in the vendored translation unit. */
static int direct_canonical_decomposition(int32_t cp, int32_t *out, int cap) {
  const utf8proc_property_t *property = utf8proc_get_property(cp);
  const utf8proc_uint16_t *entry;
  int length, count = 0;
  if (property->decomp_seqindex == UINT16_MAX || property->decomp_type != 0)
    return 0;
  entry = &utf8proc_sequences[property->decomp_seqindex & 0x3FFF];
  length = property->decomp_seqindex >> 14;
  if (length >= 3) {
    length = *entry;
    entry++;
  }
  for (; length >= 0; entry++, length--) {
    int32_t value = seqindex_decode_entry(&entry);
    if (count >= cap)
      return -1;
    out[count++] = value;
  }
  return count;
}

static int emit_mapping(int32_t cp, unsigned int options, char tag) {
  int32_t buffer[512];
  utf8proc_ssize_t length = utf8proc_decompose_char(
      cp, buffer, (utf8proc_ssize_t)(sizeof(buffer) / sizeof(buffer[0])),
      options, NULL);
  int i;
  if (length < 0 ||
      length > (utf8proc_ssize_t)(sizeof(buffer) / sizeof(buffer[0]))) {
    fprintf(stderr, "decomposition overflow for U+%04X\n", (unsigned)cp);
    return 1;
  }
  if (length == 1 && buffer[0] == cp)
    return 0;
  printf("%c %d", tag, cp);
  for (i = 0; i < length; ++i)
    printf(" %d", buffer[i]);
  putchar('\n');
  return 0;
}

int main(void) {
  int32_t cp;
  int previous_boundclass = -1, previous_indic = -1;
  int previous_ccc = -1;

  printf("V %s %s\n", utf8proc_unicode_version(), utf8proc_version());

  for (cp = 0; cp < 0x110000; ++cp) {
    const utf8proc_property_t *property = utf8proc_get_property(cp);
    if (property->boundclass != previous_boundclass ||
        property->indic_conjunct_break != previous_indic) {
      printf("G %d %d\n", cp,
             property->boundclass | (property->indic_conjunct_break << 8));
      previous_boundclass = property->boundclass;
      previous_indic = property->indic_conjunct_break;
    }
    if (property->combining_class != previous_ccc) {
      printf("A %d %d\n", cp, property->combining_class);
      previous_ccc = property->combining_class;
    }
  }

  for (cp = 0; cp < 0x110000; ++cp) {
    const utf8proc_property_t *property;
    int32_t direct[16];
    int length;

    /* Surrogates are not scalar values.  The native bridge rejects their
       UTF-8 forms; leaving them untouched is the least surprising bridge
       behavior for Python strings that happen to contain one. */
    if (cp >= 0xD800 && cp <= 0xDFFF)
      continue;

    if (emit_mapping(cp, UTF8PROC_DECOMPOSE, 'D') != 0)
      return 2;
    if (emit_mapping(cp, UTF8PROC_DECOMPOSE | UTF8PROC_COMPAT, 'K') != 0)
      return 2;
    if (emit_mapping(cp, UTF8PROC_CASEFOLD, 'F') != 0)
      return 2;

    {
      int32_t upper = utf8proc_toupper(cp);
      int32_t lower = utf8proc_tolower(cp);
      if (upper != cp)
        printf("U %d %d\n", cp, upper);
      if (lower != cp)
        printf("L %d %d\n", cp, lower);
    }

    /* Candidate composition pairs are direct two-codepoint canonical
       decompositions.  Probe the actual normalizer so composition
       exclusions and unreachable equal-combining-class pairs are omitted,
       exactly as utf8proc would behave. */
    property = utf8proc_get_property(cp);
    length = direct_canonical_decomposition(cp, direct, 16);
    if (length < 0) {
      fprintf(stderr, "direct decomposition overflow for U+%04X\n",
              (unsigned)cp);
      return 2;
    }
    if (length == 2 && !property->comp_exclusion) {
      int32_t pair[2];
      int32_t normalized;
      pair[0] = direct[0];
      pair[1] = direct[1];
      normalized = utf8proc_normalize_utf32(pair, 2, UTF8PROC_COMPOSE);
      if (normalized == 1 && pair[0] == cp)
        printf("C %d %d %d\n", direct[0], direct[1], cp);
    }
  }

  return 0;
}
"""


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def compile_probe(directory):
    compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not compiler:
        raise RuntimeError("a host C compiler (cc or gcc) is required")
    source = Path(directory) / "unicode18_probe.c"
    binary = Path(directory) / "unicode18_probe"
    source.write_text(C_PROGRAM, encoding="ascii")
    command = [
        compiler,
        "-O2",
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-I",
        str(VENDOR),
        str(source),
        "-o",
        str(binary),
    ]
    result = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    if result.returncode != 0:
        raise RuntimeError(
            "failed to compile utf8proc probe:\n" + (result.stderr or "")
        )
    return binary


def read_probe(binary):
    result = subprocess.run(
        [str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    if result.returncode != 0:
        raise RuntimeError(
            "utf8proc probe failed:\n" + (result.stderr or "exit %d" % result.returncode)
        )
    return result.stdout


def checked_ranges(changes, label):
    if not changes or changes[0][0] != 0:
        raise RuntimeError(label + " ranges must start at U+0000")
    previous = -1
    for start, _ in changes:
        if start <= previous or start > 0x10FFFF:
            raise RuntimeError(label + " ranges are not strictly increasing")
        previous = start
    return changes


def parse_probe(text):
    unicode_version = None
    utf8proc_version = None
    grapheme_changes = []
    ccc_changes = []
    nfd = []
    nfkd = []
    casefold = []
    upper = []
    lower = []
    composition = []
    seen = set()

    for line in text.splitlines():
        if not line:
            continue
        fields = line.split()
        tag = fields[0]
        if tag == "V":
            unicode_version, utf8proc_version = fields[1], fields[2]
            continue
        values = [int(field) for field in fields[1:]]
        if tag == "G":
            grapheme_changes.append((values[0], values[1]))
        elif tag == "A":
            ccc_changes.append((values[0], values[1]))
        elif tag == "D":
            _check_cp(values, "NFD")
            nfd.append(values)
        elif tag == "K":
            _check_cp(values, "NFKD")
            nfkd.append(values)
        elif tag == "F":
            _check_cp(values, "casefold")
            casefold.append(values)
        elif tag == "U":
            _check_cp(values, "uppercase")
            upper.append(values)
        elif tag == "L":
            _check_cp(values, "lowercase")
            lower.append(values)
        elif tag == "C":
            if len(values) != 3:
                raise RuntimeError("bad composition row: " + line)
            key = (values[0], values[1])
            if key in seen:
                raise RuntimeError("duplicate composition pair: " + line)
            seen.add(key)
            composition.append(values)
        else:
            raise RuntimeError("unknown probe line: " + line)

    if unicode_version != "18.0.0":
        raise RuntimeError(
            "vendored utf8proc reports Unicode %r, expected 18.0.0"
            % (unicode_version,)
        )
    if utf8proc_version != "2.12.0":
        raise RuntimeError(
            "vendored utf8proc reports version %r, expected 2.12.0"
            % (utf8proc_version,)
        )
    checked_ranges(grapheme_changes, "grapheme")
    checked_ranges(ccc_changes, "combining class")
    expected_counts = {
        "NFD": (nfd, 13253),
        "NFKD": (nfkd, 17154),
        "casefold": (casefold, 1606),
        "uppercase": (upper, 1526),
        "lowercase": (lower, 1508),
        "composition": (composition, 961),
    }
    for label, (rows, expected) in expected_counts.items():
        if len(rows) != expected:
            raise RuntimeError(
                "%s table has %d rows, expected %d for Unicode 18.0.0"
                % (label, len(rows), expected)
            )
    for rows, label in (
        (nfd, "NFD"),
        (nfkd, "NFKD"),
        (casefold, "casefold"),
        (upper, "uppercase"),
        (lower, "lowercase"),
    ):
        keys = [row[0] for row in rows]
        if keys != sorted(keys):
            raise RuntimeError(label + " rows are not sorted")
        if len(keys) != len(set(keys)):
            raise RuntimeError(label + " rows contain duplicate code points")
    return {
        "unicode_version": unicode_version,
        "utf8proc_version": utf8proc_version,
        "grapheme_changes": grapheme_changes,
        "ccc_changes": ccc_changes,
        "nfd": nfd,
        "nfkd": nfkd,
        "casefold": casefold,
        "upper": upper,
        "lower": lower,
        "composition": composition,
    }


def _check_cp(values, label):
    if len(values) < 2:
        raise RuntimeError(label + " row has no mapping")
    cp = values[0]
    if not 0 <= cp <= 0x10FFFF or 0xD800 <= cp <= 0xDFFF:
        raise RuntimeError("%s row has invalid source U+%04X" % (label, cp))
    for target in values[1:]:
        if not 0 <= target <= 0x10FFFF or 0xD800 <= target <= 0xDFFF:
            raise RuntimeError("%s row has invalid target %d" % (label, target))


def build_payload(extracted):
    sources = {}
    for name in ("utf8proc.c", "utf8proc.h", "utf8proc_data.c", "LICENSE.md"):
        path = VENDOR / name
        sources[name] = sha256_file(path)
    return {
        "schema": SCHEMA,
        "unicode_version": extracted["unicode_version"],
        "utf8proc_version": extracted["utf8proc_version"],
        "sources_sha256": sources,
        "ccc": extracted["ccc_changes"],
        "grapheme": extracted["grapheme_changes"],
        "nfd": extracted["nfd"],
        "nfkd": extracted["nfkd"],
        "casefold": extracted["casefold"],
        "upper": extracted["upper"],
        "lower": extracted["lower"],
        "composition": extracted["composition"],
    }


def encode_payload(payload):
    raw = json.dumps(
        payload, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    ).encode("ascii")
    packed = base64.b85encode(zlib.compress(raw, 9)).decode("ascii")
    return raw, packed


def chunk_literal(data, width=96):
    chunks = [data[i : i + width] for i in range(0, len(data), width)]
    if not chunks:
        chunks = [""]
    lines = []
    for chunk in chunks:
        if '"' in chunk or "\\" in chunk:
            raise RuntimeError("base85 alphabet unexpectedly contains quotes")
        lines.append('    "%s"' % chunk)
    return "\n".join(lines)


def render_license_notice():
    """Return the complete vendored LICENSE.md as Python comments."""
    license_bytes = LICENSE_PATH.read_bytes()
    license_text = license_bytes.decode("utf-8")
    digest = hashlib.sha256(license_bytes).hexdigest()
    lines = [
        "# Embedded utf8proc and Unicode data license notices.",
        "# Source: " + LICENSE_SOURCE,
        "# SHA256: " + digest,
        "# --- begin embedded utf8proc LICENSE.md ---",
    ]
    for line in license_text.splitlines():
        lines.append("# " + line if line else "#")
    lines.append("# --- end embedded utf8proc LICENSE.md ---")
    return "\n".join(lines)


MODULE_HEAD = '''"""Unicode 18.0.0 normalization, case folding and grapheme boundaries.

Generated by tools/generate_unicode18.py from the fixed utf8proc 2.12.0
sources under runtime/native/unicode/vendor/utf8proc.  The full utf8proc MIT
license and Unicode data license notices are embedded below as comments, so
this helper remains self-contained when it is concatenated into langrt.py
without the vendor directory.  The tables are embedded
in this file; importing it reads no external data files and needs no packages,
network access or ctypes.  All public and private names use the _unicode18
prefix so this source can be embedded as a prefix into the single langrt.py
runtime without collisions.
"""
%(license_notice)s


_unicode18_unicode_version = "%(unicode_version)s"
_unicode18_utf8proc_version = "%(utf8proc_version)s"
_unicode18_payload_sha256 = "%(payload_sha256)s"

# Compressed (zlib level 9), base85 encoded, deterministic JSON payload.
_unicode18_payload = (
%(payload_literal)s
)

_unicode18_tables = None

# UAX #29 Extended Grapheme Cluster bound classes (utf8proc names).
_unicode18_start = 0
_unicode18_other = 1
_unicode18_cr = 2
_unicode18_lf = 3
_unicode18_control = 4
_unicode18_extend = 5
_unicode18_l = 6
_unicode18_v = 7
_unicode18_t = 8
_unicode18_lv = 9
_unicode18_lvt = 10
_unicode18_regional_indicator = 11
_unicode18_spacingmark = 12
_unicode18_prepend = 13
_unicode18_zwj = 14
_unicode18_extended_pictographic = 19
_unicode18_e_zwg = 20

_unicode18_icb_none = 0
_unicode18_icb_linker = 1
_unicode18_icb_consonant = 2
_unicode18_icb_extend = 3

# Hangul constants used by canonical composition.
_unicode18_hangul_sbase = 0xAC00
_unicode18_hangul_lbase = 0x1100
_unicode18_hangul_vbase = 0x1161
_unicode18_hangul_tbase = 0x11A7
_unicode18_hangul_lcount = 19
_unicode18_hangul_vcount = 21
_unicode18_hangul_tcount = 28
_unicode18_hangul_scount = 11172


def _unicode18_load_tables():
    global _unicode18_tables
    if _unicode18_tables is not None:
        return _unicode18_tables
    # Imported lazily so embedding this prefix into langrt.py does not add
    # module import cost to programs that never call a Unicode helper.
    import base64 as _unicode18_base64
    import hashlib as _unicode18_hashlib
    import json as _unicode18_json
    import zlib as _unicode18_zlib
    try:
        raw = _unicode18_zlib.decompress(
            _unicode18_base64.b85decode(_unicode18_payload.encode("ascii"))
        )
        data = _unicode18_json.loads(raw.decode("ascii"))
    except Exception as error:
        raise RuntimeError("corrupt embedded Unicode 18.0.0 tables") from error
    if data.get("schema") != %(schema)d:
        raise RuntimeError("unsupported embedded Unicode 18.0.0 table schema")
    if data.get("unicode_version") != _unicode18_unicode_version:
        raise RuntimeError("embedded Unicode version mismatch")
    if data.get("utf8proc_version") != _unicode18_utf8proc_version:
        raise RuntimeError("embedded utf8proc version mismatch")
    if _unicode18_hashlib.sha256(raw).hexdigest() != _unicode18_payload_sha256:
        raise RuntimeError("embedded Unicode payload checksum mismatch")

    nfd = {row[0]: tuple(row[1:]) for row in data["nfd"]}
    nfkd = {row[0]: tuple(row[1:]) for row in data["nfkd"]}
    casefold = {row[0]: tuple(row[1:]) for row in data["casefold"]}
    upper = {row[0]: row[1] for row in data["upper"]}
    lower = {row[0]: row[1] for row in data["lower"]}
    composition = {(row[0], row[1]): row[2] for row in data["composition"]}

    # Only nonzero canonical combining classes are stored in the hot lookup.
    ccc = {}
    ccc_rows = data["ccc"]
    for index, (start, value) in enumerate(ccc_rows):
        if not value:
            continue
        end = ccc_rows[index + 1][0] - 1 if index + 1 < len(ccc_rows) else 0x10FFFF
        for cp in range(start, end + 1):
            ccc[cp] = value

    # Expand only non-default grapheme runs.  The default is OTHER/NONE.
    grapheme = {}
    grapheme_rows = data["grapheme"]
    for index, (start, packed) in enumerate(grapheme_rows):
        if packed == _unicode18_other:
            continue
        end = (
            grapheme_rows[index + 1][0] - 1
            if index + 1 < len(grapheme_rows)
            else 0x10FFFF
        )
        for cp in range(start, end + 1):
            grapheme[cp] = packed

    _unicode18_tables = (
        nfd,
        nfkd,
        casefold,
        upper,
        lower,
        composition,
        ccc,
        grapheme,
    )
    return _unicode18_tables


def _unicode18_decompose(mapping, value):
    result = []
    append = result.append
    extend = result.extend
    for cp in map(ord, value):
        mapped = mapping.get(cp)
        if mapped is None:
            append(cp)
        else:
            extend(mapped)
    return result


def _unicode18_reorder(buffer, ccc):
    # Same adjacent-swap loop as utf8proc_normalize_utf32's canonical
    # ordering pass: swap only when the second character is itself a mark and
    # the first class is strictly greater.
    position = 0
    limit = len(buffer) - 1
    while position < limit:
        first = buffer[position]
        second = buffer[position + 1]
        first_class = ccc.get(first, 0)
        second_class = ccc.get(second, 0)
        if first_class > second_class and second_class > 0:
            buffer[position] = second
            buffer[position + 1] = first
            if position > 0:
                position -= 1
            else:
                position += 1
        else:
            position += 1


def _unicode18_compose(buffer, ccc, composition):
    # Port of the composition loop in utf8proc_normalize_utf32.
    starter = None
    maximum_class = -1
    output = []
    append = output.append
    for current in buffer:
        current_class = ccc.get(current, 0)
        if starter is not None and current_class > maximum_class:
            first = output[starter]
            lindex = first - _unicode18_hangul_lbase
            if 0 <= lindex < _unicode18_hangul_lcount:
                vindex = current - _unicode18_hangul_vbase
                if 0 <= vindex < _unicode18_hangul_vcount:
                    output[starter] = (
                        _unicode18_hangul_sbase
                        + (lindex * _unicode18_hangul_vcount + vindex)
                        * _unicode18_hangul_tcount
                    )
                    continue
            sindex = first - _unicode18_hangul_sbase
            if (
                0 <= sindex < _unicode18_hangul_scount
                and sindex %% _unicode18_hangul_tcount == 0
            ):
                tindex = current - _unicode18_hangul_tbase
                if 0 < tindex < _unicode18_hangul_tcount:
                    output[starter] = first + tindex
                    continue
            composite = composition.get((first, current))
            if composite is not None:
                output[starter] = composite
                continue
        append(current)
        if current_class:
            if current_class > maximum_class:
                maximum_class = current_class
        else:
            starter = len(output) - 1
            maximum_class = -1
    return output


def _unicode18_normalize(form, value):
    """Return NFC=0, NFD=1, NFKC=2 or NFKD=3 of a Python string."""
    if not isinstance(form, int):
        raise TypeError("normalization form must be an int")
    if form < 0 or form > 3:
        raise ValueError("normalization form must be 0 (NFC), 1 (NFD), 2 (NFKC) or 3 (NFKD)")
    if not isinstance(value, str):
        raise TypeError("value must be a str")
    nfd, nfkd, _casefold, _upper, _lower, composition, ccc, _grapheme = (
        _unicode18_load_tables()
    )
    if form == 1:
        buffer = _unicode18_decompose(nfd, value)
        _unicode18_reorder(buffer, ccc)
        return "".join(map(chr, buffer))
    if form == 3:
        buffer = _unicode18_decompose(nfkd, value)
        _unicode18_reorder(buffer, ccc)
        return "".join(map(chr, buffer))
    mapping = nfd if form == 0 else nfkd
    buffer = _unicode18_decompose(mapping, value)
    _unicode18_reorder(buffer, ccc)
    buffer = _unicode18_compose(buffer, ccc, composition)
    return "".join(map(chr, buffer))


def _unicode18_casefold(value):
    """Return the full default Unicode 18.0.0 case folding of a string."""
    if not isinstance(value, str):
        raise TypeError("value must be a str")
    _nfd, _nfkd, casefold, _upper, _lower, _composition, _ccc, _grapheme = (
        _unicode18_load_tables()
    )
    result = []
    append = result.append
    extend = result.extend
    for cp in map(ord, value):
        mapped = casefold.get(cp)
        if mapped is None:
            append(cp)
        else:
            extend(mapped)
    return "".join(map(chr, result))


def _unicode18_grapheme_break_simple(left, right):
    # GB1 through GB11 and GB999 in the same order as utf8proc.c.  GB9c and
    # the stateful RI/emoji rules are applied by the caller.
    if left == _unicode18_start:
        return True
    if left == _unicode18_cr and right == _unicode18_lf:
        return False
    if _unicode18_cr <= left <= _unicode18_control:
        return True
    if _unicode18_cr <= right <= _unicode18_control:
        return True
    if left == _unicode18_l and right in (
        _unicode18_l,
        _unicode18_v,
        _unicode18_lv,
        _unicode18_lvt,
    ):
        return False
    if left in (_unicode18_lv, _unicode18_v) and right in (
        _unicode18_v,
        _unicode18_t,
    ):
        return False
    if left in (_unicode18_lvt, _unicode18_t) and right == _unicode18_t:
        return False
    if right in (
        _unicode18_extend,
        _unicode18_zwj,
        _unicode18_spacingmark,
    ):
        return False
    if left == _unicode18_prepend:
        return False
    if (
        left == _unicode18_e_zwg
        and right == _unicode18_extended_pictographic
    ):
        return False
    if (
        left == _unicode18_regional_indicator
        and right == _unicode18_regional_indicator
    ):
        return False
    return True


def _unicode18_grapheme_break(left, right, left_icb, right_icb, state):
    if state == 0:
        state_class = left
        state_icb = left_icb
    else:
        state_class = state & 0xFF
        state_icb = state >> 8
    permitted = _unicode18_grapheme_break_simple(
        state_class, right
    ) and not (
        state_icb == _unicode18_icb_linker
        and right_icb == _unicode18_icb_consonant
    )
    if not (
        state_icb == _unicode18_icb_linker
        and right_icb == _unicode18_icb_extend
    ):
        state_icb = right_icb
    if state_class == right == _unicode18_regional_indicator:
        state_class = _unicode18_other
    elif state_class == _unicode18_extended_pictographic:
        if right == _unicode18_extend:
            state_class = _unicode18_extended_pictographic
        elif right == _unicode18_zwj:
            state_class = _unicode18_e_zwg
        else:
            state_class = right
    else:
        state_class = right
    return permitted, state_class | (state_icb << 8)


def _unicode18_boundaries(value):
    """Return code-point boundaries including 0 and the end; empty is [0]."""
    if not isinstance(value, str):
        raise TypeError("value must be a str")
    if not value:
        return [0]
    _nfd, _nfkd, _casefold, _upper, _lower, _composition, _ccc, grapheme = (
        _unicode18_load_tables()
    )
    codepoints = list(map(ord, value))
    result = [0]
    state = 0
    previous = grapheme.get(codepoints[0], _unicode18_other)
    left_class = previous & 0xFF
    left_icb = previous >> 8
    for index in range(1, len(codepoints)):
        current = grapheme.get(codepoints[index], _unicode18_other)
        right_class = current & 0xFF
        right_icb = current >> 8
        boundary, state = _unicode18_grapheme_break(
            left_class, right_class, left_icb, right_icb, state
        )
        if boundary:
            result.append(index)
        left_class = right_class
        left_icb = right_icb
    result.append(len(codepoints))
    return result


def _unicode18_simple_rune_upper(rune):
    """Return the simple one-code-point uppercase mapping of a scalar."""
    if not isinstance(rune, int):
        raise TypeError("rune must be an int")
    if rune < 0 or rune > 0x10FFFF or 0xD800 <= rune <= 0xDFFF:
        return rune
    _nfd, _nfkd, _casefold, upper, _lower, _composition, _ccc, _grapheme = (
        _unicode18_load_tables()
    )
    return upper.get(rune, rune)


def _unicode18_simple_rune_lower(rune):
    """Return the simple one-code-point lowercase mapping of a scalar."""
    if not isinstance(rune, int):
        raise TypeError("rune must be an int")
    if rune < 0 or rune > 0x10FFFF or 0xD800 <= rune <= 0xDFFF:
        return rune
    _nfd, _nfkd, _casefold, _upper, lower, _composition, _ccc, _grapheme = (
        _unicode18_load_tables()
    )
    return lower.get(rune, rune)


# Names used by the native bridge use "rune_to_*"; keep both spellings.
_unicode18_rune_to_upper = _unicode18_simple_rune_upper
_unicode18_rune_to_lower = _unicode18_simple_rune_lower
'''


def render_module(payload):
    raw, packed = encode_payload(payload)
    digest = hashlib.sha256(raw).hexdigest()
    source = MODULE_HEAD % {
        "unicode_version": payload["unicode_version"],
        "utf8proc_version": payload["utf8proc_version"],
        "payload_sha256": digest,
        "schema": SCHEMA,
        "payload_literal": chunk_literal(packed),
        "license_notice": render_license_notice(),
    }
    return source, raw, digest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help="output module path (default: runtime/python/unicode18.py)",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the existing output equals freshly generated bytes",
    )
    args = parser.parse_args(argv)

    with tempfile.TemporaryDirectory(prefix="unicode18-generate-") as directory:
        binary = compile_probe(directory)
        extracted = parse_probe(read_probe(binary))
    payload = build_payload(extracted)
    source, raw, digest = render_module(payload)
    encoded = source.encode("ascii")

    if args.check:
        try:
            existing = args.output.read_bytes()
        except FileNotFoundError:
            print("missing %s" % args.output, file=sys.stderr)
            return 1
        if existing != encoded:
            print("stale or non-deterministic %s" % args.output, file=sys.stderr)
            return 1
        print(
            "ok %s: Unicode %s, utf8proc %s, %d JSON bytes, sha256 %s"
            % (
                args.output,
                payload["unicode_version"],
                payload["utf8proc_version"],
                len(raw),
                digest,
            )
        )
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_name(args.output.name + ".tmp")
    temporary.write_bytes(encoded)
    os.replace(temporary, args.output)
    print(
        "wrote %s: Unicode %s, utf8proc %s, %d source bytes, %d JSON bytes, sha256 %s"
        % (
            args.output,
            payload["unicode_version"],
            payload["utf8proc_version"],
            len(encoded),
            len(raw),
            digest,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
