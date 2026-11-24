# fast_float Eisel-Lemire provenance

`float_parse.wz` contains a Woozy translation of the Eisel-Lemire decimal
conversion used by the vendored fast_float headers. `float_parse_pow5.wz`
contains the normalized 5^q table copied from the same vendor revision.
`float_parse_big.wz` is original Woozy code and has no fast_float input.

## Source revision

- Upstream: <https://github.com/fastfloat/fast_float>
- Pinned commit: `8ec59686a1430867ba609c2777ca15143dd81003`
- Upstream commit date: 2026-11-09
- Retrieved: 2026-11-11
- Vendored version: fast_float 8.3.3
- License: triple Apache-2.0 / Boost-1.0 / MIT. This project uses the
  **Boost Software License 1.0** (`LICENSE-BOOST` in this directory).
  `AUTHORS` in this directory is the exact upstream copyright list.
- The complete vendor copy, including all three license texts, remains under
  `runtime/native/vendor/fast_float/` in the repository.

## Files used

| Vendored file | Upstream path | Upstream SHA-256 | Vendored SHA-256 |
| --- | --- | --- | --- |
| `decimal_to_binary.h` | `include/fast_float/decimal_to_binary.h` | `d9fdec8e0be092aab9f9dd0d3837ea131b4a4e9ea30facc3841fcb0ca1285ee3` | `a64a4a314eefccb82bbd954d5e884596e1b5fb66bfd6aa761a6c9ff76637f401` |
| `fast_table.h` | `include/fast_float/fast_table.h` | `c18e0a0a2513215730557cc1dd53541cacd72855ddc0287934e78b364f989edb` | `c18e0a0a2513215730557cc1dd53541cacd72855ddc0287934e78b364f989edb` |
| `float_common.h` | `include/fast_float/float_common.h` | `1e6bdad315871aea2790cc5a73113f3734404f64cc0ded748f3d2314f943b7de` | `46629074549c7d84d43591c525062e2a5424b4edf49c88a6ffa69e38e1ef5e97` |
| `parse_number.h` | `include/fast_float/parse_number.h` | `a3577b03643b9f7db3248c9240a2c98d65a4dfa316521ebe32ee74deb145b6d3` | `2a6baed8e26d59b1a1e7987fa5918ff676de1f6d961633a27c2de5f7bb81af53` |
| `AUTHORS` | `AUTHORS` | `ed249424bce4e318fa190dd6d8becf60cfc37287132ad3f89e16e0f28d878dcf` | `ed249424bce4e318fa190dd6d8becf60cfc37287132ad3f89e16e0f28d878dcf` |
| `LICENSE-BOOST` | `LICENSE-BOOST` | `8d8291caf1cee26d23acf3eb67c9f9a2d58f1c681b16a4fbe8cbfb9e3c0b5a9b` | `8d8291caf1cee26d23acf3eb67c9f9a2d58f1c681b16a4fbe8cbfb9e3c0b5a9b` |

## Translation boundary

`float_parse.wz` translates the `compute_float`,
`compute_product_approximation`, `power`, normalization, subnormal, overflow,
and ties-to-even logic from `decimal_to_binary.h`, using the format constants
from `float_common.h` and the short-spelling assumptions from
`parse_number.h`. The 64x64-to-128 multiplication is written with four 32-bit
partial products because Woozy has no 128-bit integer type. The input scanner,
Clinger path, exact big-integer fallback, and error mapping are original
Woozy code.

`float_parse_pow5.wz` copies each `power_of_five_128` `{hi, lo}` pair from
`fast_table.h` in table order. No value was regenerated from a different
revision or recomputed.

`AUTHORS` and `LICENSE-BOOST` are duplicated here, next to the translated
source, so a package that installs only `stdlib/` still carries the required
attribution. The full release already copies the vendor `LICENSE-*` files into
its `licenses/` directory; the repository vendor directory is not removed.
