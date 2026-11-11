# fast_float vendoring provenance

- Upstream: <https://github.com/fastfloat/fast_float>
- Pinned commit: `8ec59686a1430867ba609c2777ca15143dd81003`
- Upstream commit date: 2026-11-09
- Retrieved: 2026-11-11
- License: triple Apache-2.0 / Boost-1.0 / MIT. This project uses the
  **Boost Software License 1.0** (`LICENSE-BOOST`). The complete Apache-2.0
  and MIT texts and the upstream `AUTHORS` file are retained as well.
- Only the decimal floating-point parse path is used. `numbers.cpp` calls
  `fast_float::from_chars` for `double` or `float` after validating the exact
  language grammar. The vendored header set is header-only; no package
  manager, shared library, `strtod`, `printf`, `libm`, or native
  `std::from_chars` is used by the runtime.

## Files

| Vendored file | Upstream path | Upstream SHA-256 | Vendored SHA-256 |
| --- | --- | --- | --- |
| `ascii_number.h` | `include/fast_float/ascii_number.h` | `53e8fdafbdc6e461bb2fbb9c98192955b6760c61cf761adf574d1b0aa1063724` | `f96fd6db12fb16a5453b9ef805ca6c1bd4a6680ae5251a7e36c0067d91b2df97` |
| `bigint.h` | `include/fast_float/bigint.h` | `ba39c5d8d8c8fe9ca8d7300aea8392a4367fd60689a9b8e3268aadceca729e53` | `ba39c5d8d8c8fe9ca8d7300aea8392a4367fd60689a9b8e3268aadceca729e53` |
| `constexpr_feature_detect.h` | `include/fast_float/constexpr_feature_detect.h` | `2dd2dc5f98a8f1a78813e62d47e82a19ebe42af726356723d60f6eb387c6dd38` | `2dd2dc5f98a8f1a78813e62d47e82a19ebe42af726356723d60f6eb387c6dd38` |
| `decimal_to_binary.h` | `include/fast_float/decimal_to_binary.h` | `d9fdec8e0be092aab9f9dd0d3837ea131b4a4e9ea30facc3841fcb0ca1285ee3` | `a64a4a314eefccb82bbd954d5e884596e1b5fb66bfd6aa761a6c9ff76637f401` |
| `digit_comparison.h` | `include/fast_float/digit_comparison.h` | `69fb10f59005eabf0d44a5caf4733fd97a3838a8ac7090c4dc6e3e9e61a31ac3` | `69fb10f59005eabf0d44a5caf4733fd97a3838a8ac7090c4dc6e3e9e61a31ac3` |
| `fast_float.h` | `include/fast_float/fast_float.h` | `fec4c0dc177fb3986a9007824c81fb93338ef83a6dba23325d430174a8686ce1` | `fec4c0dc177fb3986a9007824c81fb93338ef83a6dba23325d430174a8686ce1` |
| `fast_table.h` | `include/fast_float/fast_table.h` | `c18e0a0a2513215730557cc1dd53541cacd72855ddc0287934e78b364f989edb` | `c18e0a0a2513215730557cc1dd53541cacd72855ddc0287934e78b364f989edb` |
| `float_common.h` | `include/fast_float/float_common.h` | `1e6bdad315871aea2790cc5a73113f3734404f64cc0ded748f3d2314f943b7de` | `46629074549c7d84d43591c525062e2a5424b4edf49c88a6ffa69e38e1ef5e97` |
| `parse_number.h` | `include/fast_float/parse_number.h` | `a3577b03643b9f7db3248c9240a2c98d65a4dfa316521ebe32ee74deb145b6d3` | `2a6baed8e26d59b1a1e7987fa5918ff676de1f6d961633a27c2de5f7bb81af53` |
| `AUTHORS` | `AUTHORS` | `ed249424bce4e318fa190dd6d8becf60cfc37287132ad3f89e16e0f28d878dcf` | `ed249424bce4e318fa190dd6d8becf60cfc37287132ad3f89e16e0f28d878dcf` |
| `LICENSE-APACHE` | `LICENSE-APACHE` | `097a889aa954d04e088b790b10a4014d6189561d0a6013935a73ce3d4ddaaf06` | `097a889aa954d04e088b790b10a4014d6189561d0a6013935a73ce3d4ddaaf06` |
| `LICENSE-BOOST` | `LICENSE-BOOST` | `8d8291caf1cee26d23acf3eb67c9f9a2d58f1c681b16a4fbe8cbfb9e3c0b5a9b` | `8d8291caf1cee26d23acf3eb67c9f9a2d58f1c681b16a4fbe8cbfb9e3c0b5a9b` |
| `LICENSE-MIT` | `LICENSE-MIT` | `e562f3f974ced7e69dd1db77b820b36bcf8f30377f1aa105723fba449c53c4e6` | `e562f3f974ced7e69dd1db77b820b36bcf8f30377f1aa105723fba449c53c4e6` |

## Minimal freestanding patch

The conversion algorithms, table data, rounding, and digit-comparison logic are
unchanged. The patch only removes hosted-runtime dependencies:

1. `float_common.h` / `parse_number.h`: removed the hosted `<system_error>`
   include. `float_common.h` now declares a local `fast_float::errc` with the
   two values used by the parser (`invalid_argument = 22`,
   `result_out_of_range = 34`) and success value zero. All `std::errc` uses in
   `ascii_number.h`, `parse_number.h`, and `float_common.h` were changed to
   that local type.
2. `decimal_to_binary.h`: removed unused `<cmath>` and `<cstdlib>` includes.
   `parse_number.h`: removed the unused `<cmath>` include.
3. `float_common.h`: the automatic x86 SSE2 detection is guarded by
   `!defined(FASTFLOAT_NO_SIMD)`. `numbers.cpp` defines `FASTFLOAT_NO_SIMD`
   because the runtime parses `char`, while the SIMD helpers only cover
   `char16_t`; this avoids pulling hosted `mm_malloc` declarations into a
   freestanding translation unit. The scalar parser path is unchanged.
4. `numbers.cpp` defines `NDEBUG` before including fast_float so assertions do
   not reference `__assert_fail`. The parser's own runtime validation, range
   checks, and exact bigint fallback remain enabled.
