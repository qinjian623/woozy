#!/usr/bin/env python3
"""Verify stdlib/unicode/unicode_case_tables.wz against the pinned UCD.

This is an independent decode of the generated constant pages.  It checks the
full default Upper/Lowercase_Mapping tables and the Cased/Case_Ignorable
property pages, not just a sample.  The official UCD files are inputs and are
not committed; pass the directory that contains them.

Usage:
    python3 tests/stdlib_unicode_case/verify_tables.py --ucd-dir /tmp/ucd18
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TABLES = ROOT / "stdlib" / "unicode" / "unicode_case_tables.wz"

HASHES = {
    "UnicodeData.txt": "0736451de439ae7baf1425136617da495e09ee5afbe6e394374db7009ea08950",
    "SpecialCasing.txt": "8538dea57c184f1ef3783885ea79677b10f6efa06423717157e63712f14d1ad2",
    "DerivedCoreProperties.txt": "09c928886a178fcafd93c29e4bd59073a058e5a100b716d425cb563ab50f68c9",
}
MAPPING_FIELDS = 7
PROPERTY_FIELDS = 2


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def load_mappings(ucd: Path) -> tuple[dict[int, tuple[int, ...]], dict[int, tuple[int, ...]]]:
    upper: dict[int, tuple[int, ...]] = {}
    lower: dict[int, tuple[int, ...]] = {}
    for line in (ucd / "UnicodeData.txt").read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split(";")
        cp = int(fields[0], 16)
        if fields[12]:
            upper[cp] = (int(fields[12], 16),)
        if fields[13]:
            lower[cp] = (int(fields[13], 16),)
    for line in (ucd / "SpecialCasing.txt").read_text(encoding="utf-8").splitlines():
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        fields = [field.strip() for field in body.split(";")]
        while len(fields) > 5 and not fields[-1]:
            fields.pop()
        if len(fields) == 4:
            fields.append("")
        if fields[4]:
            continue
        cp = int(fields[0], 16)
        lower[cp] = tuple(int(value, 16) for value in fields[1].split())
        upper[cp] = tuple(int(value, 16) for value in fields[3].split())
    return upper, lower


def load_properties(ucd: Path) -> dict[str, set[int]]:
    wanted = {"Cased", "Case_Ignorable"}
    result = {name: set() for name in wanted}
    for line in (ucd / "DerivedCoreProperties.txt").read_text(encoding="utf-8").splitlines():
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        fields = [field.strip() for field in body.split(";")]
        if len(fields) < 2 or fields[1] not in wanted:
            continue
        bounds = fields[0].split("..")
        start = int(bounds[0], 16)
        end = int(bounds[-1], 16)
        result[fields[1]].update(range(start, end + 1))
    return result


def decode_literal(literal: str) -> list[int]:
    values: list[int] = []
    index = 0
    while index < len(literal):
        char = literal[index]
        if char != "\\":
            values.append(ord(char))
            index += 1
            continue
        escape = literal[index + 1]
        if escape == '"':
            values.append(0x22)
            index += 2
        elif escape == "\\":
            values.append(0x5C)
            index += 2
        elif escape == "u":
            if literal[index + 2] != "{":
                raise ValueError("malformed Unicode escape")
            close = literal.index("}", index + 3)
            values.append(int(literal[index + 3:close], 16))
            index = close + 1
        else:
            raise ValueError(f"unsupported escape in generated table: {literal[index:index + 4]}")
    return values


def page_strings(source: str, function: str) -> dict[int, list[int]]:
    match = re.search(rf"func {function}\(page u32\) string \{{(.*?)\n\}}", source, re.S)
    if not match:
        raise ValueError(f"missing generated function {function}")
    pages: dict[int, list[int]] = {}
    for item in re.finditer(
            r'if page == (\d+) \{ return ("(?:[^"\\]|\\.)*"); \}', match.group(1)):
        pages[int(item.group(1))] = decode_literal(item.group(2)[1:-1])
    return pages


def check_mapping(source: str, function: str,
                  expected: dict[int, tuple[int, ...]]) -> int:
    pages = page_strings(source, function)
    actual: dict[int, tuple[int, ...]] = {}
    for page, values in pages.items():
        if len(values) % MAPPING_FIELDS:
            raise ValueError(f"{function}: page {page} is not a whole number of records")
        for offset in range(0, len(values), MAPPING_FIELDS):
            start, end, delta, sign, first, second, third = values[
                offset:offset + MAPPING_FIELDS]
            if page != start >> 6:
                raise ValueError(f"{function}: record {start:#x} is on the wrong page")
            if start > end:
                raise ValueError(f"{function}: descending record {start:#x}")
            if delta:
                if first or second or third:
                    raise ValueError(f"{function}: simple record has expansion fields")
                for cp in range(start, end + 1):
                    actual[cp] = (cp + delta if sign == 0 else cp - delta,)
            else:
                targets = tuple(value for value in (first, second, third) if value)
                if not targets:
                    raise ValueError(f"{function}: empty expansion record")
                actual[start] = targets
    wanted = {cp: targets for cp, targets in expected.items()
              if targets != (cp,)}
    if actual != wanted:
        missing = sorted(set(wanted) - set(actual))
        extra = sorted(set(actual) - set(wanted))
        wrong = [cp for cp in sorted(set(actual) & set(wanted))
                 if actual[cp] != wanted[cp]]
        raise ValueError(
            f"{function}: missing={missing[:3]} extra={extra[:3]} wrong={wrong[:3]}")
    return len(actual)


def check_property(source: str, function: str, expected: set[int]) -> int:
    pages = page_strings(source, function)
    actual: set[int] = set()
    for page, values in pages.items():
        if len(values) % PROPERTY_FIELDS:
            raise ValueError(f"{function}: page {page} is not a whole number of records")
        for offset in range(0, len(values), PROPERTY_FIELDS):
            start, end = values[offset:offset + PROPERTY_FIELDS]
            if page != start >> 7:
                raise ValueError(f"{function}: range {start:#x} is on the wrong page")
            actual.update(range(start, end + 1))
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ValueError(f"{function}: missing={missing[:5]} extra={extra[:5]}")
    return len(actual)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ucd-dir", type=Path, required=True)
    parser.add_argument("--tables", type=Path, default=DEFAULT_TABLES)
    args = parser.parse_args()

    for name, expected in HASHES.items():
        path = args.ucd_dir / name
        if not path.is_file():
            print(f"missing {path}", file=sys.stderr)
            return 1
        actual = sha256(path)
        if actual != expected:
            print(f"{name}: SHA-256 {actual} != {expected}", file=sys.stderr)
            return 1

    upper, lower = load_mappings(args.ucd_dir)
    properties = load_properties(args.ucd_dir)
    source = args.tables.read_text(encoding="utf-8")
    if 'const CASE_UNICODE_VERSION string = "18.0.0";' not in source:
        print("generated file does not declare Unicode 18.0.0", file=sys.stderr)
        return 1
    counts = {
        "uppercase mappings": check_mapping(source, "case_upper_page", upper),
        "lowercase mappings": check_mapping(source, "case_lower_page", lower),
        "Cased code points": check_property(source, "case_cased_page",
                                            properties["Cased"]),
        "Case_Ignorable code points": check_property(source, "case_ignorable_page",
                                                     properties["Case_Ignorable"]),
    }
    for name, count in counts.items():
        print(f"{name}: {count}")
    print("tables match the pinned Unicode 18.0.0 data")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
