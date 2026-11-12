# Unicode 18.0.0 test-vector provenance

These are the complete official Unicode Character Database conformance files
for the same Unicode version as vendored utf8proc 2.12.0. They are stored
verbatim with their original copyright/terms headers.

- Unicode release: **18.0.0**
- Base URL: <https://www.unicode.org/Public/18.0.0/ucd/>
- Retrieval date: 2026-11-11
- License/terms: <https://www.unicode.org/terms_of_use.html>

| File | Upstream URL | SHA-256 |
| --- | --- | --- |
| `NormalizationTest.txt` | <https://www.unicode.org/Public/18.0.0/ucd/NormalizationTest.txt> | `25a50d816764b04abfb4a646d3eb2b2a803284c3873d9a06757b94fe4513dde3` |
| `CaseFolding.txt` | <https://www.unicode.org/Public/18.0.0/ucd/CaseFolding.txt> | `a004797658a457bec4dc11683e39f69249ea3b595b752dbea6721c4c9f587b0d` |
| `GraphemeBreakTest.txt` | <https://www.unicode.org/Public/18.0.0/ucd/auxiliary/GraphemeBreakTest.txt> | `b0cf047ee94485bbdc846de2b902f5f8a815f6b674f9d04223cddadd91c9df31` |

`vector_test.cpp` reads every non-comment line of all three files. It does not
skip any vector: the complete counts are 20,171 normalization lines (403,420
form/invariant comparisons), 1,640 case-folding lines (1,606 default C/F
mappings; 34 Turkic/simple-only entries are deliberately not applied), and 853
grapheme-break lines.
