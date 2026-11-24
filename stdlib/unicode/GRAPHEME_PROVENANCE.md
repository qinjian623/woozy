# Unicode 18.0.0 grapheme-boundary provenance

`unicode_grapheme_tables.wz` is generated, not hand-edited.  It provides the
property data for the package-internal `wz_grapheme_boundaries` scanner in
`unicode_grapheme.wz`.  The only public entry points are the existing
`std/unicode.boundaries`, `grapheme_count` and `graphemes`; the scanner is not
`pub`, so other packages cannot call it.

## Implementation and fixture paths

- Scanner: `stdlib/unicode/unicode_grapheme.wz`
- Generated data: `stdlib/unicode/unicode_grapheme_tables.wz`
- Public delegates: `stdlib/unicode/unicode.wz` (`boundaries`,
  `grapheme_count`, `graphemes`)
- Generator: `tools/generate_unicode_grapheme_wz.py`
- Official conformance runner:
  `tests/stdlib_unicode_grapheme/run_conformance.py`
- Small directory fixture:
  `tests/stdlib_unicode_grapheme/src/main.wz`
- Table verifier: `tests/stdlib_unicode_grapheme/verify_tables.py`

## Pinned sources

The generator consumes `runtime/python/unicode18.py`, the captured Unicode
18.0.0 / utf8proc 2.12.0 runtime, SHA-256:

`3d665f58f129b1e335ac57a2246c4be51be2cf827df007398de329cebdb46775`

That captured runtime was generated from the fixed utf8proc sources and is
validated by `tests/python/unicode18_test.py` against the official
NormalizationTest, CaseFolding and GraphemeBreakTest files.  The official UCD
inputs behind it are pinned below; `--ucd-dir` verifies their hashes.

| File | Upstream URL | SHA-256 | Used for |
| --- | --- | --- | --- |
| `auxiliary/GraphemeBreakProperty.txt` | <https://www.unicode.org/Public/18.0.0/ucd/auxiliary/GraphemeBreakProperty.txt> | `0839dcb79e4ac639ecd538b1abf7c9d22e3f9dd265b7e182d33627aa4d75b45a` | `Grapheme_Cluster_Break` |
| `emoji/emoji-data.txt` | <https://www.unicode.org/Public/18.0.0/ucd/emoji/emoji-data.txt> | `80d00f8e616a0ef27fd6b8de3b758c06383b5d917e2977709578e68baf733bf1` | `Extended_Pictographic` |
| `DerivedCoreProperties.txt` | <https://www.unicode.org/Public/18.0.0/ucd/DerivedCoreProperties.txt> | `09c928886a178fcafd93c29e4bd59073a058e5a100b716d425cb563ab50f68c9` | `InCB` (`Indic_Conjunct_Break`) |
| `UnicodeData.txt` | <https://www.unicode.org/Public/18.0.0/ucd/UnicodeData.txt> | `0736451de439ae7baf1425136617da495e09ee5afbe6e394374db7009ea08950` | general categories and names |

Official grapheme test oracle:
`tests/native/unicode/data/GraphemeBreakTest.txt`
(Unicode 18.0.0, 853 cases, SHA-256
`b0cf047ee94485bbdc846de2b902f5f8a815f6b674f9d04223cddadd91c9df31`).

The data license is the existing `UNICODE-LICENSE.txt` in this directory
(SHA-256 `e7a93b009565cfce55919a381437ac4db883e9da2126fa28b91d12732bc53d96`).
The generator refuses to run if that file changes.

## Generated data

The table stores one packed property per code point:
`boundclass | indic_conjunct_break << 8`, using the utf8proc class numbers.
Unassigned code points are reset to Other/None because that is the observable
behavior of the existing utf8proc 2.12.0-backed native runtime; the official
files assign some unassigned ranges defaults.  The official UCD files remain
the pinned provenance for the assigned properties.

- 1,631 non-default runs before block splitting;
- 143 non-default 256-code-point blocks;
- 1,671 block-local range-tree leaves;
- 2,168 total property runs in the captured runtime (including default runs);
- generated file: 453,911 bytes, SHA-256
  `c3b3736c8499746e8a64c0579ed978b8d67283a1a3e6a5df3ce2b7c74bdcc407`.

The lookup is a `switch` on `rune >> 8` followed by a small balanced range tree
over the low byte.  It allocates nothing and decodes no page.

## Regeneration and checks

```sh
python3 tools/generate_unicode_grapheme_wz.py
python3 tools/generate_unicode_grapheme_wz.py --check
python3 tools/generate_unicode_grapheme_wz.py --check --ucd-dir /path/to/ucd-18.0.0
python3 tests/stdlib_unicode_grapheme/verify_tables.py
python3 tests/stdlib_unicode_grapheme/run_conformance.py \
  --compiler /path/to/frozen/woozyc --work-dir /tmp/grapheme-conformance
```

`--ucd-dir` checks the four pinned files above; it does not re-derive the
unassigned-code-point defaults, which intentionally follow the captured
utf8proc runtime.  `verify_tables.py` parses every generated leaf and compares
all 1,114,112 code points with `runtime/python/unicode18.py`; the conformance
runner uses only the public `std/unicode.boundaries` and `grapheme_count`.
