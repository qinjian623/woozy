# Native Unicode runtime

This directory is self-contained and does not modify the existing text,
allocator, Vec, CMake, frontend, standard-library, or documentation files.
It provides the requested C ABI:

```cpp
int64_t lang_unicode_normalize_v1(LangText *out, const LangText *input,
                                  int32_t form) noexcept; // NFC/NFD/NFKC/NFKD
int64_t lang_unicode_casefold_v1(LangText *out, const LangText *input) noexcept;
LangVec *lang_unicode_grapheme_boundaries_v1(const LangText *text) noexcept;
```

Optional helpers `lang_unicode_rune_to_upper_v1/lower_v1` implement simple
one-code-point UnicodeData mappings only. They are not locale-sensitive string
case conversion.

## Semantics

- Normalization forms are `0=NFC`, `1=NFD`, `2=NFKC`, `3=NFKD`. Invalid form or
  invalid UTF-8 returns `-EINVAL` (`-22`) and initializes `out` to empty.
- Case folding is the full default Unicode mapping from CaseFolding statuses
  `C + F`, including one-to-many expansions such as `ß -> ss` and
  `İ -> i + U+0307`. Turkic (`T`) and simple-only (`S`) mappings are not applied.
- Inputs use the explicit `LangText.bytes` length. Embedded NUL is a normal
  scalar and is never a terminator. Outputs follow the existing ownership rule:
  non-empty results own one `LangHandle`; empty results have no owner. If
  `out == input`, the old reference is released only after the replacement is
  complete.
- Allocation failure follows the existing runtime convention: `Panic(5)` for
  allocation failure and `Panic(6)` for size overflow. Temporary utf8proc map
  buffers are always returned through the runtime free path.
- Grapheme boundaries are extended grapheme-cluster boundaries (UAX #29 with
  GB9c/GB11/GB12/GB13 state). The result is a `LangVec` with `stride=8` and
  code-point positions. It always includes `0` and the code-point length; an
  empty text returns `[0]`.
- The existing string code-point length/index semantics are untouched.

The implementation is built from unmodified utf8proc 2.12.0 sources, which
report Unicode 18.0.0. See `PROVENANCE.md` for the pinned commit, licenses,
and SHA-256 list.

## Suggested CMake integration

The existing runtime target already has `enable_language(C)` and the required
freestanding flags. Add only the wrapper and adapter; do **not** add
`vendor/utf8proc/utf8proc.c` separately, because the adapter must install the
memory hooks first:

```cmake
enable_language(C)
target_sources(lang_native_runtime PRIVATE
  ${LANG_NATIVE_ROOT}/runtime/native/unicode/unicode.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/unicode/utf8proc_adapter.c)
# Optional: keep vendored implementation symbols out of dynamic symbol tables.
set_source_files_properties(
  ${LANG_NATIVE_ROOT}/runtime/native/unicode/utf8proc_adapter.c
  PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")
```

No new include directory is needed: the wrapper and adapter use paths relative
to their own directory. The existing target flags
`-ffreestanding -fno-builtin -fno-exceptions -fno-rtti -fno-sanitize=all`
apply to both files; the adapter's internal macros change only the vendored
implementation calls, not the target. No `libm`, libc, or libstdc++ dependency
is added.

For a CTest entry, the included `run.py` compiles an archive from every current
`runtime/native/*.cpp` plus the new objects, runs a hosted smoke test, runs the
complete official vectors hosted, then links and runs the same vectors with
`-nostdlib -static`. It also runs a wrapped-allocation leak/ownership test.

```cmake
if(LANG_TEST_PYTHON)
  add_test(NAME native.unicode COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/native/unicode/run.py
    --root ${LANG_SOURCE_ROOT}
    --out ${CMAKE_CURRENT_BINARY_DIR}/unicode-results
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --clangxx ${LLVM_TOOLS_BINARY_DIR}/clang++
    --ar ${CMAKE_AR})
  set_tests_properties(native.unicode PROPERTIES TIMEOUT 300)
endif()
```

## Python-side same-version reference

The checked-in official Unicode 18.0.0 files are the authoritative reference
for all three operations:

- normalization: `NormalizationTest.txt` invariants;
- full default case folding: `CaseFolding.txt` statuses `C` and `F`;
- extended grapheme clusters: `GraphemeBreakTest.txt`.

For an independent Python normalization oracle, use PyPI package
`unicodedata2==18.0.0` (its `unidata_version` is `18.0.0`; verified with
CPython 3.14.8 on x86_64). The Python 3.14 stdlib build in this environment
reports Unicode 16.0.0, so `unicodedata`/`str.casefold()` are **not** a
same-version oracle here. Use `unicodedata2.normalize(...)` for normalization
and the official CaseFolding `C/F` lines for full case folding. Python's
`str.upper/lower` likewise follow the interpreter's Unicode version and are
not a substitute for the simple rune helpers.
