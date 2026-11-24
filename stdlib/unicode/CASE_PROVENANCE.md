# Unicode 18.0.0 case-conversion provenance

`unicode_case_tables.wz` is generated, not hand-edited. It implements the
Unicode **default** uppercase and lowercase mappings and the Final_Sigma
context rule for `std/unicode.upper` and `std/unicode.lower`.

## Sources

These official Unicode Character Database files are read by
`tools/generate_unicode_case_wz.py`; the upstream files are not committed.

- Unicode release: **18.0.0**
- Base URL: <https://www.unicode.org/Public/18.0.0/ucd/>
- Retrieval date: 2026-11-22
- Data license: `UNICODE-LICENSE.txt` in this directory (Unicode License V3,
  SHA-256 `e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96`)

| File | Upstream URL | SHA-256 |
| --- | --- | --- |
| `UnicodeData.txt` | <https://www.unicode.org/Public/18.0.0/ucd/UnicodeData.txt> | `0736451de439ae7baf1425136617da495e09ee5afbe6e394374db7009ea08950` |
| `SpecialCasing.txt` | <https://www.unicode.org/Public/18.0.0/ucd/SpecialCasing.txt> | `8538dea57c184f1ef3783885ea79677b10f6efa06423717157e63712f14d1ad2` |
| `DerivedCoreProperties.txt` | <https://www.unicode.org/Public/18.0.0/ucd/DerivedCoreProperties.txt> | `09c928886a178fcafd93c29e4bd59073a058e5a100b716d425cb563ab50f68c9` |

The generator pins these hashes and refuses inputs from another release. It
uses `UnicodeData.txt` simple mappings, overrides them with unconditional
`SpecialCasing.txt` entries, and reads the derived `Cased` and `Case_Ignorable`
properties used by Final_Sigma. Conditional language-sensitive entries
(Turkic, Azerbaijani, Lithuanian, and `Not_Before_Dot`/`After_I`/`More_Above`)
are intentionally excluded: these APIs are the locale-independent default
algorithm.

## Generated data

The generated file stores:

- full non-identity uppercase mappings: 1,601 code points, including 103
  multi-code-point expansions (sharp S, dotted I, ligatures, Armenian
  ligatures, and Greek ypogegrammena);
- full non-identity lowercase mappings: 1,508 code points, including
  `U+0130 -> U+0069 U+0307`;
- `Cased`: 4,797 code points;
- `Case_Ignorable`: 2,890 code points.

Mappings are split into constant pages of 64 code points and property ranges
into pages of 128 code points. A page selector returns one small constant
string; the lookup binary-searches that page with string indexing. Therefore a
call does not allocate a table `Vec`, and Final_Sigma context is evaluated in
one forward pass with bounded lookahead over the original string.

Current generated-file SHA-256:
`b6b9e4abdf59597dfa5d0fdb20451f3b7f07dd0e8e37b95fd8dda67668bb670e`.

## Regeneration and checks

```sh
python3 tools/generate_unicode_case_wz.py --ucd-dir /path/to/ucd18
python3 tools/generate_unicode_case_wz.py --check --ucd-dir /path/to/ucd18
python3 tests/stdlib_unicode_case/verify_tables.py --ucd-dir /path/to/ucd18
```

`--download` fetches the pinned files into a temporary directory when they are
not available locally. The generated output is deterministic: the same three
upstream files and generator produce byte-identical text.

## Full default case folding

`unicode_casefold_table.wz` implements the mapping used by
`std/unicode.casefold`. It is generated from the repository's unmodified
Unicode 18.0.0 [CaseFolding.txt](../../tests/native/unicode/data/CaseFolding.txt),
originally retrieved from the [Unicode Character Database](https://www.unicode.org/Public/18.0.0/ucd/CaseFolding.txt)
on 2026-11-11. The input SHA-256 is
`a004797658a457bec4dc11683e39f69249ea3b595b752dbea6721c4c9f587b0d`.
The data is covered by the Unicode License V3 in `UNICODE-LICENSE.txt`.

The table uses the 1,606 full default mappings (status C or F), and the
library handles the 26 ASCII mappings directly. Status S and T entries are
excluded. The lookup groups adjacent or alternating code points with the
same delta and stores each expansion in up to three 21-bit fields. The
operation folds case without applying normalization and preserves embedded
NUL. Native and Python use the same Woozy implementation.

Current generated-file SHA-256:
`256f6c7df18358addb7c930087646522ea9ccf79f41ef2e16d458addf7165f04`.

```sh
python3 tools/generate_unicode_casefold_wz.py
python3 tools/generate_unicode_casefold_wz.py --check
```
