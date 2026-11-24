#!/usr/bin/env python3
"""Check the generated grapheme table against the pinned Unicode runtime.

The committed table backs the package-internal scanner called by
std/unicode.boundaries.  It is a 256-code-point block switch with a small range
tree per block.  This script parses every leaf, expands it back to property
runs, and compares all 1,114,112 code points with runtime/python/unicode18.py
(Unicode 18.0.0, utf8proc 2.12.0).  It also re-runs
tools/generate_unicode_grapheme_wz.py --check for determinism.
"""

import importlib.util
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "stdlib/unicode/unicode_grapheme_tables.wz"
GENERATOR = ROOT / "tools/generate_unicode_grapheme_wz.py"
REFERENCE = ROOT / "runtime/python/unicode18.py"


def load_reference():
    spec = importlib.util.spec_from_file_location("unicode18_reference", REFERENCE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_tree():
    text = TABLE.read_text()
    body = text[text.index("func grapheme_property"):]
    lines = [line.strip() for line in body.splitlines()[1:]]
    if lines[0] != "let offset = rune & 0xff;" or lines[1] != "switch rune >> 8 {":
        raise SystemExit("unexpected function prologue: %r" % lines[:2])

    def parse_node(index):
        line = lines[index]
        if line == "return grapheme_other;":
            return [], index + 1
        match = re.fullmatch(r"if offset < 0x([0-9a-f]+) \{", line)
        if not match:
            raise SystemExit("unexpected tree line: %r" % line)
        start = int(match.group(1), 16)
        left, index = parse_node(index + 1)
        if lines[index] != "} else {":
            raise SystemExit("expected } else {, got %r" % lines[index])
        index += 1
        match = re.fullmatch(r"if offset <= 0x([0-9a-f]+) \{", lines[index])
        if not match:
            raise SystemExit("expected leaf bound, got %r" % lines[index])
        end = int(match.group(1), 16)
        index += 1
        match = re.fullmatch(r"return 0x([0-9a-f]+);", lines[index])
        if not match:
            raise SystemExit("expected leaf value, got %r" % lines[index])
        value = int(match.group(1), 16)
        index += 1
        if lines[index] != "}":
            raise SystemExit("expected leaf close, got %r" % lines[index])
        index += 1
        right, index = parse_node(index)
        if lines[index] != "}":
            raise SystemExit("expected node close, got %r" % lines[index])
        return left + [(start, end, value)] + right, index + 1

    leaves = []
    index = 2
    while True:
        line = lines[index]
        if line == "default: return grapheme_other;":
            index += 1
            break
        match = re.fullmatch(r"case (\d+): \{", line)
        if not match:
            raise SystemExit("unexpected switch line: %r" % line)
        block = int(match.group(1))
        index += 1
        block_leaves, index = parse_node(index)
        if lines[index] != "}":
            raise SystemExit("expected case close, got %r" % lines[index])
        index += 1
        for start, end, value in block_leaves:
            if start > end or end > 0xFF:
                raise SystemExit("invalid block leaf %r" % ((start, end, value),))
            leaves.append(((block << 8) | start, (block << 8) | end, value))
    if lines[index] != "}" or lines[index + 1] != "return grapheme_other;":
        raise SystemExit("unexpected switch tail: %r" % lines[index:index + 2])
    for left, right in zip(leaves, leaves[1:]):
        if left[1] >= right[0]:
            raise SystemExit("leaves overlap or are unsorted: %r %r" % (left, right))
    return text, leaves


def expected_runs(grapheme):
    runs = []
    start = None
    previous = 1
    for cp in range(0x110000):
        value = grapheme.get(cp, 1)
        if value != 1 and start is None:
            start = cp
            previous = value
        elif value != 1 and value != previous:
            runs.append((start, cp - 1, previous))
            start = cp
            previous = value
        elif value == 1 and start is not None:
            runs.append((start, cp - 1, previous))
            start = None
    if start is not None:
        runs.append((start, 0x10FFFF, previous))
    return runs


def split_runs(runs):
    """Split expected runs at the same 256-code-point boundaries as the table."""
    split = []
    for start, end, value in runs:
        for block in range(start >> 8, (end >> 8) + 1):
            split.append((max(start, block << 8), min(end, (block << 8) + 0xFF), value))
    return split


def main():
    text, leaves = parse_tree()
    module = load_reference()
    grapheme = module._unicode18_load_tables()[7]
    expected = split_runs(expected_runs(grapheme))
    if leaves != expected:
        for index, (got, want) in enumerate(zip(leaves, expected)):
            if got != want:
                raise SystemExit("leaf %d differs: got %r want %r" % (index, got, want))
        raise SystemExit("leaf count differs: got %d want %d" % (len(leaves), len(expected)))
    covered = 0
    leaf_index = 0
    for cp in range(0x110000):
        while leaf_index < len(leaves) and cp > leaves[leaf_index][1]:
            leaf_index += 1
        if leaf_index < len(leaves) and leaves[leaf_index][0] <= cp:
            value = leaves[leaf_index][2]
            covered += 1
        else:
            value = 1
        if value != grapheme.get(cp, 1):
            raise SystemExit("code point U+%04X differs" % cp)
    check = subprocess.run([sys.executable, str(GENERATOR), "--check"],
                           capture_output=True, text=True)
    if check.returncode != 0:
        raise SystemExit(check.stdout + check.stderr)
    print("table ok: %d leaves, %d covered code points, %d bytes, %s" % (
        len(leaves), covered, len(text.encode()), check.stdout.strip()))


if __name__ == "__main__":
    raise SystemExit(main())
