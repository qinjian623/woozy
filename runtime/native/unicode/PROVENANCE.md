# utf8proc vendoring provenance

- Upstream project: <https://github.com/JuliaStrings/utf8proc>
- Upstream site: <https://juliastrings.github.io/utf8proc/>
- Pinned tag: `v2.12.0`
- Pinned commit: `79cdd5ab40c79afa559f689ffc13b76812dee1ac`
- Upstream commit date: 2026-09-25
- Release archive:
  <https://codeload.github.com/JuliaStrings/utf8proc/tar.gz/refs/tags/v2.12.0>
  (SHA-256 `f564011d38b2888d583d510b08e69ffa15aa117155db1b9b49ef1dfe1fa25111`)
- Retrieval date: 2026-11-11
- Unicode data version reported by this release: **18.0.0**
- License: MIT/Expat for the code and the Unicode data license for
  `utf8proc_data.c`. Both are retained verbatim in `LICENSE.md`.
- Test vectors: the matching official Unicode 18.0.0 files under
  `tests/native/unicode/data/` (see that directory's `PROVENANCE.md`).

## Files

The four upstream files below are vendored byte-for-byte unchanged. They were
compared with the checked-out tag before commit.

| Vendored file | Upstream path | SHA-256 (upstream and vendored) |
| --- | --- | --- |
| `utf8proc.c` | `utf8proc.c` | `f38438eb986ba7eedad0f2e820a70fae1aaf05197364ba0381a5e84775099ef8` |
| `utf8proc.h` | `utf8proc.h` | `08da85e820efaa3613500946c26b2dc427a07f76001ed7c16bc90f298b814c35` |
| `utf8proc_data.c` | `utf8proc_data.c` | `b4911d4162d216afa612317a9b981851e15d672f4d93b25f3a1eb986f8c0aedb` |
| `LICENSE.md` | `LICENSE.md` | `3b510150d34f248a221bb88e1d811238d6c6c18b51231822c42974c39bb07256` |

## Freestanding adaptation (outside the vendored files)

`utf8proc_adapter.c` compiles the unchanged `utf8proc.c` in one C translation
unit. `utf8proc_freestanding.h` is included after the upstream `<stdlib.h>`
declarations and before `utf8proc.c`; it redirects `malloc`, `free`, `realloc`,
`strlen`, `memcpy`, `memmove`, and `memset` to static hooks. The allocation
hooks call the existing `lang_alloc_v1`/`lang_free_v1` and use a 16-byte
private size header for `realloc`. No global `malloc`, `free`, `realloc`, or
libc string/memory symbol is introduced.

`UTF8PROC_STATIC` is defined by the adapter so the upstream header does not
request a shared-library export attribute. The adapter object has only
`lang_alloc_v1` and `lang_free_v1` as undefined symbols; the normal runtime
also supplies weak freestanding `memcpy`/`memset`/`memmove` implementations for
any compiler-generated calls.

The public wrapper is `unicode.cpp`/`unicode.hpp`; it calls `utf8proc_map` with
explicit byte lengths (never `UTF8PROC_NULLTERM`), copies the result into a
normal owned `LangText`, and always frees the temporary utf8proc buffer.
