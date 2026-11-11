# musl math vendoring provenance

- Upstream: <https://git.musl-libc.org/cgit/musl/tree/src/math>
- Pinned commit: `7813e9119eb5fe681b6750b8a033a0490bca0941`
- Upstream commit date: 2026-10-14
- Retrieved: 2026-11-11 from the official git repository (verified against `https://github.com/ifduyue/musl` at the same commit)
- License: musl `COPYRIGHT`, retained verbatim as `runtime/native/math/COPYRIGHT`. Individual source headers retain their exact MIT/BSD/Sun/Arm notices; the Arm data files are MIT.

No vendored source file was modified. All freestanding adaptations live in the new non-vendor headers `math.h`, `features.h`, `libm.h`, `atomic.h`, `math_prefix.h`, and in `support.cpp`.

## Files

| Vendored file | Upstream path | Upstream SHA-256 | Vendored SHA-256 |
| --- | --- | --- | --- |
| `__cos.c` | `src/math/__cos.c` | `3f0f774aab60a18d7ef935ffdf121bcda17cbadabff14096c614da22e31a42c1` | `3f0f774aab60a18d7ef935ffdf121bcda17cbadabff14096c614da22e31a42c1` |
| `__math_divzero.c` | `src/math/__math_divzero.c` | `84aa910bdc5e7ccfed42098c37c44e278aa942d46e5a51d5ee8273336c79e719` | `84aa910bdc5e7ccfed42098c37c44e278aa942d46e5a51d5ee8273336c79e719` |
| `__math_invalid.c` | `src/math/__math_invalid.c` | `b56440ed59fa1e1aaaf6bf8b672c8bb71eaf844457906f7773da1d67d7ef013b` | `b56440ed59fa1e1aaaf6bf8b672c8bb71eaf844457906f7773da1d67d7ef013b` |
| `__math_oflow.c` | `src/math/__math_oflow.c` | `74669de53bc4b250183cfa262a8a03ee8106f33a83e3aca6b217bafc7363b83c` | `74669de53bc4b250183cfa262a8a03ee8106f33a83e3aca6b217bafc7363b83c` |
| `__math_uflow.c` | `src/math/__math_uflow.c` | `9a841197bbe9ade9bbd684f8bc65424a97c481d37dabb9fa1790bafe7959034a` | `9a841197bbe9ade9bbd684f8bc65424a97c481d37dabb9fa1790bafe7959034a` |
| `__math_xflow.c` | `src/math/__math_xflow.c` | `5feafe10347636884a071a5096f912d5ca4a0939844804ca09b88241405648ad` | `5feafe10347636884a071a5096f912d5ca4a0939844804ca09b88241405648ad` |
| `__rem_pio2.c` | `src/math/__rem_pio2.c` | `938329e0939cd83ee2185a52154b41242f618d4a6df1b08395f344da33d4f1e6` | `938329e0939cd83ee2185a52154b41242f618d4a6df1b08395f344da33d4f1e6` |
| `__rem_pio2_large.c` | `src/math/__rem_pio2_large.c` | `b97f19fb5951c83a0c10506fab35646caefde13bd03100ad2af55ef00af0b9fb` | `b97f19fb5951c83a0c10506fab35646caefde13bd03100ad2af55ef00af0b9fb` |
| `__sin.c` | `src/math/__sin.c` | `f3a37ef9bcfb8a2f9c809d8c7f1aa2c17c8c3319ad7fb2fe7c0a6284ea8859d2` | `f3a37ef9bcfb8a2f9c809d8c7f1aa2c17c8c3319ad7fb2fe7c0a6284ea8859d2` |
| `__tan.c` | `src/math/__tan.c` | `e18b65b69fced8dbb2d3ed1a3dc6daea0e9f857f2762f274380bd89a6f89d2c4` | `e18b65b69fced8dbb2d3ed1a3dc6daea0e9f857f2762f274380bd89a6f89d2c4` |
| `acos.c` | `src/math/acos.c` | `cc6f9017e7e2e0ef01516fa12c68aa63865e6415c90dc039dfce9abffae09a4c` | `cc6f9017e7e2e0ef01516fa12c68aa63865e6415c90dc039dfce9abffae09a4c` |
| `asin.c` | `src/math/asin.c` | `9302dd0b2f302f7b7decce82e737ed250bae1bcc65c19eaf3d7ec19010892e63` | `9302dd0b2f302f7b7decce82e737ed250bae1bcc65c19eaf3d7ec19010892e63` |
| `atan.c` | `src/math/atan.c` | `1cffb67a65456ae0bc5f07a94df04a4f18ebc1ac0bffe3219250c4e0555ee01d` | `1cffb67a65456ae0bc5f07a94df04a4f18ebc1ac0bffe3219250c4e0555ee01d` |
| `atan2.c` | `src/math/atan2.c` | `a5f94b09b10b4e72bb15d94ae403c9dfd27d7ad881ecd729000004429acfca63` | `a5f94b09b10b4e72bb15d94ae403c9dfd27d7ad881ecd729000004429acfca63` |
| `ceil.c` | `src/math/ceil.c` | `f616b50efc61eb35f946308b222f5f83ca707b4efaf7ea175969a5b62d2d6637` | `f616b50efc61eb35f946308b222f5f83ca707b4efaf7ea175969a5b62d2d6637` |
| `copysign.c` | `src/math/copysign.c` | `7a5ae1fbedc6da808c69b1769f21624c20709c93eaa846fdf89546ab6bccecae` | `7a5ae1fbedc6da808c69b1769f21624c20709c93eaa846fdf89546ab6bccecae` |
| `cos.c` | `src/math/cos.c` | `3a74e054057d636fa494bdd76a53a40db5cee618264f6ab31cb0538ba63c6bda` | `3a74e054057d636fa494bdd76a53a40db5cee618264f6ab31cb0538ba63c6bda` |
| `exp.c` | `src/math/exp.c` | `7db56f21ca9b8915d36dd5099b707aafeb6a736eae798e19b76b074764e75f34` | `7db56f21ca9b8915d36dd5099b707aafeb6a736eae798e19b76b074764e75f34` |
| `exp2.c` | `src/math/exp2.c` | `b771de769e1b69b931d1f95baa39b429a73c9904e8b5607e1736771c6ace92b1` | `b771de769e1b69b931d1f95baa39b429a73c9904e8b5607e1736771c6ace92b1` |
| `exp_data.c` | `src/math/exp_data.c` | `c55dff64e369fe816cd8ce367064989bb119f85783599680d1a329031c660689` | `c55dff64e369fe816cd8ce367064989bb119f85783599680d1a329031c660689` |
| `exp_data.h` | `src/math/exp_data.h` | `df77fca837edd02b260dd09a2914202775909ae67a059218ae38cfab104e6d16` | `df77fca837edd02b260dd09a2914202775909ae67a059218ae38cfab104e6d16` |
| `expm1.c` | `src/math/expm1.c` | `c4764166cea03420f2dfc1840a6a7615934192d851b96d5d2dcc321cc1e39460` | `c4764166cea03420f2dfc1840a6a7615934192d851b96d5d2dcc321cc1e39460` |
| `fabs.c` | `src/math/fabs.c` | `99880a7802d67077fa54a2bbb5d3388cf42b00c96d3e2b1c16d63f2918157754` | `99880a7802d67077fa54a2bbb5d3388cf42b00c96d3e2b1c16d63f2918157754` |
| `floor.c` | `src/math/floor.c` | `b9ccb9363719a84be218d60c56717f4fb0234e170c305e36ce21c30e1f42f8e1` | `b9ccb9363719a84be218d60c56717f4fb0234e170c305e36ce21c30e1f42f8e1` |
| `fma.c` | `src/math/fma.c` | `f3e328da492e4818f83737bab2b8b7d8cf281c4ce3730773c99ab8c7efc41dc7` | `f3e328da492e4818f83737bab2b8b7d8cf281c4ce3730773c99ab8c7efc41dc7` |
| `fmod.c` | `src/math/fmod.c` | `33fb61093e04783081b9e8f8a8a303456b2f919c215cd0c378c1ed0acf41934c` | `33fb61093e04783081b9e8f8a8a303456b2f919c215cd0c378c1ed0acf41934c` |
| `frexp.c` | `src/math/frexp.c` | `58a2973f80f9def7940708850e97f8080da38be2496cd473f7cb9023e9d59e94` | `58a2973f80f9def7940708850e97f8080da38be2496cd473f7cb9023e9d59e94` |
| `hypot.c` | `src/math/hypot.c` | `a5532b3328310bb3ac9f7cd9074fd26c5fe7288c719c907c5c03a615442f9e7e` | `a5532b3328310bb3ac9f7cd9074fd26c5fe7288c719c907c5c03a615442f9e7e` |
| `ldexp.c` | `src/math/ldexp.c` | `702b608d3a5cf9c5d26c26ef2c528a3003555ee48ececec3b817c817d297bfe8` | `702b608d3a5cf9c5d26c26ef2c528a3003555ee48ececec3b817c817d297bfe8` |
| `log.c` | `src/math/log.c` | `a0694d8587a0d145d0f8ae87cbffba014d7988bc2ebcbfad94cea73b394c9ecc` | `a0694d8587a0d145d0f8ae87cbffba014d7988bc2ebcbfad94cea73b394c9ecc` |
| `log10.c` | `src/math/log10.c` | `ef9b4b1f0918cf1fcb797f9fa4d4ffe177bfa27f668d74a41d613bfbcd83850a` | `ef9b4b1f0918cf1fcb797f9fa4d4ffe177bfa27f668d74a41d613bfbcd83850a` |
| `log1p.c` | `src/math/log1p.c` | `c76bbf714cd456a9f0de6e2c94853c27e7865581a2edfdef6d1f861739bbcb46` | `c76bbf714cd456a9f0de6e2c94853c27e7865581a2edfdef6d1f861739bbcb46` |
| `log2.c` | `src/math/log2.c` | `281e4fc0f737419e245019d8bc8a8ca53032c2d88c31d08561e9e59dcb4b4640` | `281e4fc0f737419e245019d8bc8a8ca53032c2d88c31d08561e9e59dcb4b4640` |
| `log2_data.c` | `src/math/log2_data.c` | `5a62bb213f19dc1364cddb612e3d13831e382f5ae6f7fafd7064362f53478dfb` | `5a62bb213f19dc1364cddb612e3d13831e382f5ae6f7fafd7064362f53478dfb` |
| `log2_data.h` | `src/math/log2_data.h` | `b56e1a56f73a8700caa9787cd399fecf7f96038af56c2a590aeb6f0f8662ea8e` | `b56e1a56f73a8700caa9787cd399fecf7f96038af56c2a590aeb6f0f8662ea8e` |
| `log_data.c` | `src/math/log_data.c` | `e96bcd99d88fc57e0f3686299014f4b949a6212603ce237f39e16cedc155617a` | `e96bcd99d88fc57e0f3686299014f4b949a6212603ce237f39e16cedc155617a` |
| `log_data.h` | `src/math/log_data.h` | `a8dced6f0821f30cad040ba326b03e419fbe0a750ed8f3b0d7145992bee4bccc` | `a8dced6f0821f30cad040ba326b03e419fbe0a750ed8f3b0d7145992bee4bccc` |
| `pow.c` | `src/math/pow.c` | `b06f0a08d74fcff8cced7afa2c0e47e301e3e8a19c0de5becdec28cd7b014027` | `b06f0a08d74fcff8cced7afa2c0e47e301e3e8a19c0de5becdec28cd7b014027` |
| `pow_data.c` | `src/math/pow_data.c` | `2052ec913f9b4a2170a98a6fcbcd7694f5dec220036b00c36106a2b4b65a4c55` | `2052ec913f9b4a2170a98a6fcbcd7694f5dec220036b00c36106a2b4b65a4c55` |
| `pow_data.h` | `src/math/pow_data.h` | `5014345a7958adcf6ff835b5e96afa99dd5a98c40dfe968442288e002b8e01a3` | `5014345a7958adcf6ff835b5e96afa99dd5a98c40dfe968442288e002b8e01a3` |
| `remainder.c` | `src/math/remainder.c` | `1996546b761c35236155ef3c4740ce6c108e83e26c2e610f46e0b78e689a12cb` | `1996546b761c35236155ef3c4740ce6c108e83e26c2e610f46e0b78e689a12cb` |
| `remquo.c` | `src/math/remquo.c` | `7ccf8f5c1376de85a8741dcbdcd9feb69fe8268ddd49aebc796036f80a9eaee9` | `7ccf8f5c1376de85a8741dcbdcd9feb69fe8268ddd49aebc796036f80a9eaee9` |
| `round.c` | `src/math/round.c` | `2349e599e99052603794087193725d28180b5d3466d43d6e6e1b254627bf37ae` | `2349e599e99052603794087193725d28180b5d3466d43d6e6e1b254627bf37ae` |
| `scalbn.c` | `src/math/scalbn.c` | `a2e766283ed30c2b30c71e01dbdabef58faf92cc9aa84a01c1ce1069b6625696` | `a2e766283ed30c2b30c71e01dbdabef58faf92cc9aa84a01c1ce1069b6625696` |
| `sin.c` | `src/math/sin.c` | `24e53a9d85f196f66919cc19247be5480bb607ef38844b1a24bc47c4fe4974ef` | `24e53a9d85f196f66919cc19247be5480bb607ef38844b1a24bc47c4fe4974ef` |
| `sqrt.c` | `src/math/sqrt.c` | `688908f94c3edf7a439cb64094034a596a1fbff08445b9158a22874cb52ead4f` | `688908f94c3edf7a439cb64094034a596a1fbff08445b9158a22874cb52ead4f` |
| `sqrt_data.c` | `src/math/sqrt_data.c` | `590aaadff964dcfc36ef9e5302423a779cda4f1608e15dbeee8e43e6e5f1c6fe` | `590aaadff964dcfc36ef9e5302423a779cda4f1608e15dbeee8e43e6e5f1c6fe` |
| `sqrt_data.h` | `src/math/sqrt_data.h` | `5fa47a8b8c1afbee4dcd10f71d2f6c433a24f9aa5ce7a90924cd1db236cbc03c` | `5fa47a8b8c1afbee4dcd10f71d2f6c433a24f9aa5ce7a90924cd1db236cbc03c` |
| `tan.c` | `src/math/tan.c` | `422d411bac962a1077ae394d14e54313012a6faa3476c20b2a66d3fb3f7a923f` | `422d411bac962a1077ae394d14e54313012a6faa3476c20b2a66d3fb3f7a923f` |
| `trunc.c` | `src/math/trunc.c` | `2f2941542db65a5c3a8a6fa952b978a5ff757f8b14ffdaef8ad28e0cafead644` | `2f2941542db65a5c3a8a6fa952b978a5ff757f8b14ffdaef8ad28e0cafead644` |
| `COPYRIGHT` | `COPYRIGHT` | `b870108ec5e7790e9f9919064f1b9421d62d5f9b0e6c230c6adf7ea2da62e97b` | `b870108ec5e7790e9f9919064f1b9421d62d5f9b0e6c230c6adf7ea2da62e97b` |

## Freestanding adaptations (no source patch)

1. `math_prefix.h` is a forced include for every vendored C unit. It renames every public and internal math/data symbol to `lang_math_*` / `lang_math_internal_*`, so the host oracle's `std::sin`/`std::pow` etc. cannot be shadowed. It also clears `__FP_FAST_FMA*` so the non-FMA table variants are selected; the build uses `-ffp-contract=off`.
2. `math.h` is a minimal isolated `<math.h>` for the C units only: `double_t`, constants, double bit predicates, and declarations. It is not on the host test include path.
3. `features.h` provides only `hidden`, `weak`, and an empty `weak_alias` (so musl's `drem` alias is not emitted).
4. `libm.h` keeps the required musl bit-access/barrier/prediction/error-helper interface for double, drops long-double/architecture code, and temporarily sets `LDBL_MAX_EXP` to 1024 so `__rem_pio2_large.c` selects only the double-precision 2/pi table extension. The source text and the double table are unchanged.
5. `atomic.h` provides only `a_clz_64` using `__builtin_clzll` for `fma.c`.
6. `support.cpp` adds exact-ABI `int32_t` wrappers for `ldexp`/`frexp` and the bitwise `isnan`/`isinf`/`isfinite`/`signbit` predicates. It does not implement any polynomial or approximation.

## Compile flags

The manifest `sources.cmake` sets these flags only on the vendored C sources:

```text
-std=c99 -ffreestanding -fno-builtin -fno-common -fno-strict-aliasing
-fno-math-errno -fno-trapping-math -fno-fast-math -ffp-contract=off
-Wno-parentheses -Wno-shift-op-parentheses -Wno-unknown-pragmas
-I runtime/native/math
-include runtime/native/math/math_prefix.h
```

The host test/oracle is never compiled with this isolated `-I` path or prefix include.

## Excluded upstream code

No float (`*f`) or long-double (`*l`) variants, architecture assembly, whole-library build files, tests, or unrelated math functions are vendored. Only the double files reachable from the required ABI plus their double data tables and internal kernels are present.
