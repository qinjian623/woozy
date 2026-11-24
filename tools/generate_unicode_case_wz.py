#!/usr/bin/env python3
"""Generate the pure-Woozy Unicode default case-conversion tables.

The generated module contains only the data needed by ``std/unicode.upper`` and
``std/unicode.lower``: full default Upper/Lowercase_Mapping entries, the Cased
and Case_Ignorable properties used by Final_Sigma, and a small UTF-8 append
helper.  It is intentionally generated from the official Unicode Character
Database rather than from an interpreter's ``str.upper``/``str.lower``, whose
Unicode version may differ.

The table is split into small constant strings (64 code points for mapping
pages, 128 for property pages).  A lookup selects one page, then binary-searches
that page with ``text.at``.  This keeps the generated source compact for the
compiler while avoiding a large Vec allocation on every call.

Only the Python standard library is used.  The input UCD files are not part of
this repository; pass ``--ucd-dir`` to a directory containing them, or allow
the tool to download the pinned release into a temporary directory.  The
SHA-256 values are part of the generation contract.

Usage:
    python3 tools/generate_unicode_case_wz.py --ucd-dir /tmp/ucd18
    python3 tools/generate_unicode_case_wz.py --check --ucd-dir /tmp/ucd18
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import tempfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUTPUT = ROOT / "stdlib" / "unicode" / "unicode_case_tables.wz"
UNICODE_VERSION = "18.0.0"
BASE_URL = f"https://www.unicode.org/Public/{UNICODE_VERSION}/ucd/"

FILES = {
    "UnicodeData.txt": "0736451de439ae7baf1425136617da495e09ee5afbe6e394374db7009ea08950",
    "SpecialCasing.txt": "8538dea57c184f1ef3783885ea79677b10f6efa06423717157e63712f14d1ad2",
    "DerivedCoreProperties.txt": "09c928886a178fcafd93c29e4bd59073a058e5a100b716d425cb563ab50f68c9",
}

SIGMA = 0x03A3
I_WITH_DOT = 0x0130
MAPPING_SHIFT = 6     # 64 code points per mapping page
PROPERTY_SHIFT = 7    # 128 code points per property page
MAPPING_FIELDS = 7    # start, end, abs(delta), sign, target1..3
PROPERTY_FIELDS = 2   # start, end


@dataclass(frozen=True)
class Record:
    start: int
    end: int
    delta: int = 0
    expansion: tuple[int, ...] = ()

    @property
    def is_expansion(self) -> bool:
        return bool(self.expansion)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download_ucd(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name in FILES:
        target = directory / name
        if target.exists():
            continue
        with urllib.request.urlopen(BASE_URL + name, timeout=120) as response:
            target.write_bytes(response.read())


def verify_ucd(directory: Path) -> None:
    missing = [name for name in FILES if not (directory / name).is_file()]
    if missing:
        raise SystemExit(
            "missing UCD input: " + ", ".join(missing) +
            "\nPass --ucd-dir or --download to fetch the pinned release."
        )
    for name, expected in FILES.items():
        actual = sha256(directory / name)
        if actual != expected:
            raise SystemExit(
                f"{name}: SHA-256 {actual} does not match pinned {expected}"
            )


def parse_unicode_data(path: Path) -> tuple[dict[int, tuple[int, ...]], dict[int, tuple[int, ...]]]:
    upper: dict[int, tuple[int, ...]] = {}
    lower: dict[int, tuple[int, ...]] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split(";")
        if len(fields) != 15:
            raise SystemExit(f"{path}:{number}: expected 15 fields")
        cp = int(fields[0], 16)
        if fields[12]:
            upper[cp] = (int(fields[12], 16),)
        if fields[13]:
            lower[cp] = (int(fields[13], 16),)
    return upper, lower


def parse_special_casing(path: Path, upper: dict[int, tuple[int, ...]],
                         lower: dict[int, tuple[int, ...]]) -> None:
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        fields = [field.strip() for field in body.split(";")]
        while len(fields) > 5 and not fields[-1]:
            fields.pop()
        if len(fields) == 4:
            fields.append("")
        if len(fields) != 5:
            raise SystemExit(f"{path}:{number}: expected 5 fields")
        if fields[4]:
            # Locale and contextual mappings are not part of the default
            # conversion.  Final_Sigma is applied by the string function.
            continue
        cp = int(fields[0], 16)
        lower[cp] = tuple(int(value, 16) for value in fields[1].split())
        upper[cp] = tuple(int(value, 16) for value in fields[3].split())


def parse_properties(path: Path) -> dict[str, list[tuple[int, int]]]:
    wanted = {"Cased", "Case_Ignorable"}
    ranges: dict[str, list[tuple[int, int]]] = {name: [] for name in wanted}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        fields = [field.strip() for field in body.split(";")]
        if len(fields) < 2 or fields[1] not in wanted:
            continue
        bounds = fields[0].split("..")
        start = int(bounds[0], 16)
        end = int(bounds[-1], 16)
        if start > end:
            raise SystemExit(f"{path}:{number}: descending range")
        ranges[fields[1]].append((start, end))
    for name, items in ranges.items():
        items.sort()
        merged: list[tuple[int, int]] = []
        for start, end in items:
            if merged and start <= merged[-1][1] + 1:
                merged[-1] = (merged[-1][0], max(merged[-1][1], end))
            else:
                merged.append((start, end))
        ranges[name] = merged
    return ranges


def mapping_records(mapping: dict[int, tuple[int, ...]]) -> list[Record]:
    """Remove identities and compress affine one-code-point mapping runs."""
    records: list[Record] = []
    run: list[tuple[int, int]] = []

    def flush() -> None:
        nonlocal run
        if not run:
            return
        start, delta = run[0]
        end = start
        for cp, current_delta in run[1:]:
            if cp == end + 1 and current_delta == delta:
                end = cp
            else:
                records.append(Record(start, end, delta=delta))
                start = end = cp
                delta = current_delta
        records.append(Record(start, end, delta=delta))
        run = []

    for cp in sorted(mapping):
        targets = mapping[cp]
        if len(targets) != 1:
            flush()
            records.append(Record(cp, cp, expansion=targets))
            continue
        target = targets[0]
        if target == cp:
            flush()
            continue
        delta = target - cp
        if run and cp == run[-1][0] + 1 and delta == run[-1][1]:
            run.append((cp, delta))
        else:
            flush()
            run = [(cp, delta)]
    flush()
    records.sort(key=lambda record: record.start)
    return records


def split_mapping_pages(records: list[Record]) -> dict[int, list[tuple[int, Record]]]:
    size = 1 << MAPPING_SHIFT
    pages: dict[int, list[tuple[int, Record]]] = {}
    for record in records:
        for page in range(record.start >> MAPPING_SHIFT,
                          (record.end >> MAPPING_SHIFT) + 1):
            start = max(record.start, page * size)
            end = min(record.end, (page + 1) * size - 1)
            pages.setdefault(page, []).append((start, Record(
                start, end, delta=record.delta, expansion=record.expansion)))
    for items in pages.values():
        items.sort(key=lambda item: item[1].start)
    return pages


def split_property_pages(ranges: list[tuple[int, int]]) -> dict[int, list[tuple[int, int]]]:
    size = 1 << PROPERTY_SHIFT
    pages: dict[int, list[tuple[int, int]]] = {}
    for start, end in ranges:
        for page in range(start >> PROPERTY_SHIFT, (end >> PROPERTY_SHIFT) + 1):
            first = max(start, page * size)
            last = min(end, (page + 1) * size - 1)
            pages.setdefault(page, []).append((first, last))
    for items in pages.values():
        items.sort()
    return pages


def woozy_string(values: list[int]) -> str:
    """Quote a rune sequence as an ASCII-only Woozy string literal."""
    pieces: list[str] = ['"']
    for value in values:
        if value == 0x22:
            pieces.append('\\"')
        elif value == 0x5C:
            pieces.append("\\\\")
        elif 0x20 <= value <= 0x7E:
            pieces.append(chr(value))
        else:
            pieces.append(f"\\u{{{value:x}}}")
    pieces.append('"')
    return "".join(pieces)


def mapping_page_string(items: list[tuple[int, Record]]) -> str:
    values: list[int] = []
    for start, record in items:
        if record.is_expansion:
            targets = list(record.expansion)
            if len(targets) > 3:
                raise SystemExit("case expansion longer than three code points")
            targets += [0] * (3 - len(targets))
            values.extend([start, record.end, 0, 0, *targets])
        else:
            delta = record.delta
            values.extend([
                start, record.end, abs(delta), 0 if delta > 0 else 1, 0, 0, 0,
            ])
    return woozy_string(values)


def property_page_string(items: list[tuple[int, int]]) -> str:
    values: list[int] = []
    for start, end in items:
        values.extend([start, end])
    return woozy_string(values)


def emit_page_selector(name: str, pages: dict[int, list],
                       render, out: list[str]) -> None:
    keys = sorted(pages)

    def tree(lo: int, hi: int, indent: str) -> None:
        if lo > hi:
            return
        mid = (lo + hi) // 2
        key = keys[mid]
        if lo < mid:
            out.append(f"{indent}if page < {key} {{")
            tree(lo, mid - 1, indent + "    ")
            out.append(f"{indent}}} else {{")
            out.append(f'{indent}    if page == {key} {{ return {render(key)}; }}')
            tree(mid + 1, hi, indent + "    ")
            out.append(f"{indent}}}")
        else:
            out.append(f'{indent}if page == {key} {{ return {render(key)}; }}')
            tree(mid + 1, hi, indent)

    out.append(f"func {name}(page u32) string {{")
    tree(0, len(keys) - 1, "    ")
    out.append('    return "";')
    out.append("}")


def generate(ucd: Path) -> str:
    upper, lower = parse_unicode_data(ucd / "UnicodeData.txt")
    parse_special_casing(ucd / "SpecialCasing.txt", upper, lower)
    properties = parse_properties(ucd / "DerivedCoreProperties.txt")

    upper_pages = split_mapping_pages(mapping_records(upper))
    lower_pages = split_mapping_pages(mapping_records(lower))
    cased_pages = split_property_pages(properties["Cased"])
    ignorable_pages = split_property_pages(properties["Case_Ignorable"])

    # The table stores U+03A3's ordinary lowercase mapping.  The string
    # function selects U+03C2 when Final_Sigma is true.
    for pages in (upper_pages, lower_pages):
        for items in pages.values():
            for _, record in items:
                if record.is_expansion and record.start == SIGMA:
                    raise SystemExit("unexpected SpecialCasing entry for U+03A3")
                if record.start == I_WITH_DOT and not record.is_expansion:
                    raise SystemExit("U+0130 must use its full two-code-point mapping")

    out: list[str] = []
    out.append("// Generated by tools/generate_unicode_case_wz.py. Do not edit by hand.")
    out.append("//")
    out.append(f"// Unicode {UNICODE_VERSION} full default case mappings and Final_Sigma")
    out.append("// properties for std/unicode.upper and std/unicode.lower.")
    out.append("// Sources are the official UCD files, not vendored here:")
    for name, digest in FILES.items():
        out.append(f"//   {BASE_URL}{name}")
        out.append(f"//     SHA-256 {digest}")
    out.append("//")
    out.append("// Data license: UNICODE-LICENSE.txt in this directory.")
    out.append("//")
    out.append("// Each page is a small constant string of rune-sized fields.  Mapping")
    out.append("// records are (start, end, abs(delta), sign, target1, target2, target3)")
    out.append("// and property records are (start, end).  A selector returns only the")
    out.append("// relevant page, so lookups allocate no table Vec and scan at most one")
    out.append("// page.  No locale-specific mappings are present.")
    out.append("package unicode;")
    out.append("")
    out.append('import "std/text" as text;')
    out.append('import "std/vec" as vec;')
    out.append("")
    out.append("func case_append_rune(out Vec[u8], rune u32) {")
    out.append("    if rune < 0x80 {")
    out.append("        vec.push(out, u8(rune));")
    out.append("    } else if rune < 0x800 {")
    out.append("        vec.push(out, u8(0xc0 | (rune >> 6)));")
    out.append("        vec.push(out, u8(0x80 | (rune & 0x3f)));")
    out.append("    } else if rune < 0x10000 {")
    out.append("        vec.push(out, u8(0xe0 | (rune >> 12)));")
    out.append("        vec.push(out, u8(0x80 | ((rune >> 6) & 0x3f)));")
    out.append("        vec.push(out, u8(0x80 | (rune & 0x3f)));")
    out.append("    } else {")
    out.append("        vec.push(out, u8(0xf0 | (rune >> 18)));")
    out.append("        vec.push(out, u8(0x80 | ((rune >> 12) & 0x3f)));")
    out.append("        vec.push(out, u8(0x80 | ((rune >> 6) & 0x3f)));")
    out.append("        vec.push(out, u8(0x80 | (rune & 0x3f)));")
    out.append("    }")
    out.append("}")
    out.append("")
    out.append("func case_apply_record(data string, base usize, rune u32, out Vec[u8]) {")
    out.append("    let delta = data[base+2];")
    out.append("    if delta != 0 {")
    out.append("        if data[base+3] == 0 {")
    out.append("            case_append_rune(out, rune + delta);")
    out.append("        } else {")
    out.append("            case_append_rune(out, rune - delta);")
    out.append("        }")
    out.append("        return;")
    out.append("    }")
    out.append("    let first = data[base+4];")
    out.append("    if first != 0 { case_append_rune(out, first); }")
    out.append("    let second = data[base+5];")
    out.append("    if second != 0 { case_append_rune(out, second); }")
    out.append("    let third = data[base+6];")
    out.append("    if third != 0 { case_append_rune(out, third); }")
    out.append("}")
    out.append("")
    out.append("func case_mapping(rune u32, out Vec[u8], data string) {")
    out.append("    if data != \"\" {")
    out.append(f"        var low usize = 0;")
    out.append(f"        var high usize = text.len(data) / {MAPPING_FIELDS};")
    out.append("        for low < high {")
    out.append("            let middle = (low+high)/2;")
    out.append(f"            let base = middle*{MAPPING_FIELDS};")
    out.append("            let start = data[base];")
    out.append("            let end = data[base+1];")
    out.append("            if rune < start {")
    out.append("                high = middle;")
    out.append("            } else if rune > end {")
    out.append("                low = middle+1;")
    out.append("            } else {")
    out.append("                case_apply_record(data, base, rune, out);")
    out.append("                return;")
    out.append("            }")
    out.append("        }")
    out.append("    }")
    out.append("    case_append_rune(out, rune);")
    out.append("}")
    out.append("")
    out.append("func case_property_lookup(data string, rune u32) bool {")
    out.append("    if data == \"\" { return false; }")
    out.append("    var low usize = 0;")
    out.append(f"    var high usize = text.len(data) / {PROPERTY_FIELDS};")
    out.append("    for low < high {")
    out.append("        let middle = (low+high)/2;")
    out.append(f"        let base = middle*{PROPERTY_FIELDS};")
    out.append("        let start = data[base];")
    out.append("        let end = data[base+1];")
    out.append("        if rune < start { high = middle; }")
    out.append("        else if rune > end { low = middle+1; }")
    out.append("        else { return true; }")
    out.append("    }")
    out.append("    return false;")
    out.append("}")
    out.append("")

    emit_page_selector("case_upper_page", upper_pages, lambda key: mapping_page_string(upper_pages[key]), out)
    out.append("")
    emit_page_selector("case_lower_page", lower_pages, lambda key: mapping_page_string(lower_pages[key]), out)
    out.append("")
    emit_page_selector("case_cased_page", cased_pages, lambda key: property_page_string(cased_pages[key]), out)
    out.append("")
    emit_page_selector("case_ignorable_page", ignorable_pages,
                       lambda key: property_page_string(ignorable_pages[key]), out)
    out.append("")
    out.append("func case_upper(rune u32, out Vec[u8]) {")
    out.append(f"    case_mapping(rune, out, case_upper_page(rune >> {MAPPING_SHIFT}));")
    out.append("}")
    out.append("")
    out.append("func case_lower(rune u32, out Vec[u8]) {")
    out.append(f"    case_mapping(rune, out, case_lower_page(rune >> {MAPPING_SHIFT}));")
    out.append("}")
    out.append("")
    out.append("func case_is_cased(rune u32) bool {")
    out.append(f"    return case_property_lookup(case_cased_page(rune >> {PROPERTY_SHIFT}), rune);")
    out.append("}")
    out.append("")
    out.append("func case_is_case_ignorable(rune u32) bool {")
    out.append(f"    return case_property_lookup(case_ignorable_page(rune >> {PROPERTY_SHIFT}), rune);")
    out.append("}")
    out.append("")
    out.append(f'const CASE_UNICODE_VERSION string = "{UNICODE_VERSION}";')
    out.append("const CASE_DATA_SHA256 string =")
    out.append('    "' + sha256(ucd / "UnicodeData.txt") + ":" +
               sha256(ucd / "SpecialCasing.txt") + ":" +
               sha256(ucd / "DerivedCoreProperties.txt") + '";')
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ucd-dir", type=Path,
                        help="directory containing the pinned UCD text files")
    parser.add_argument("--download", action="store_true",
                        help="download missing files into --ucd-dir or a temp dir")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true",
                        help="fail if --output differs from the generated text")
    args = parser.parse_args()

    temporary: tempfile.TemporaryDirectory[str] | None = None
    try:
        if args.ucd_dir is None:
            if not args.download:
                parser.error("--ucd-dir is required unless --download is used")
            temporary = tempfile.TemporaryDirectory(prefix="woozy-ucd18-")
            ucd = Path(temporary.name)
        else:
            ucd = args.ucd_dir
        if args.download:
            download_ucd(ucd)
        verify_ucd(ucd)
        generated = generate(ucd)
        if args.check:
            try:
                existing = args.output.read_text(encoding="utf-8")
            except FileNotFoundError:
                print(f"missing generated file: {args.output}", file=sys.stderr)
                return 1
            if existing != generated:
                print(f"generated file is stale: {args.output}", file=sys.stderr)
                return 1
            print(f"up to date: {args.output}")
            return 0
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(generated, encoding="utf-8")
        print(f"wrote {args.output}")
        return 0
    finally:
        if temporary is not None:
            temporary.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
