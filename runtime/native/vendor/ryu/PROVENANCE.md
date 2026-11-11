# Ryu vendoring provenance

- Upstream: <https://github.com/ulfjack/ryu>
- Pinned commit: `4c0618b0e44f7ef027ebae05d2cc7812048f7c8f`
- Upstream commit date: 2026-02-10
- Retrieved: 2026-11-11
- License: dual Apache-2.0 / Boost-1.0. This project uses the **Boost
  Software License 1.0** (`LICENSE-Boost`). The complete Apache-2.0 text is
  also retained as `LICENSE-Apache2`.
- Only `d2s_buffered_n` and `f2s_buffered_n` are built. `numbers.cpp` includes
  `d2s.c` and `f2s.c` in its own translation unit; no separate vendor source
  list is required by the build.

## Files

| Vendored file | Upstream path | Upstream SHA-256 | Vendored SHA-256 |
| --- | --- | --- | --- |
| `ryu.h` | `ryu/ryu.h` | `b7feab0ba1df5e9ef3d602f386592ad152491405e49b408fb21c0e0e9e6bfb16` | `48ac5b259489090e9eacb51ce7e9040db5dcf7c72cca8e55dccfe3fc093e80ab` |
| `common.h` | `ryu/common.h` | `0bbd71d26da6193e678d0776cf418f43f287c73d6fd6725353df0aadf70f2a19` | `0bbd71d26da6193e678d0776cf418f43f287c73d6fd6725353df0aadf70f2a19` |
| `d2s.c` | `ryu/d2s.c` | `d24323c7eb77d63f1e52c415212b50060b776f0883525d907d04954fcd48cf64` | `91c0cdf6cb9ac927c80530a050c0f337e7c1946bf456ef6bb9f9792c23a2e200` |
| `d2s_intrinsics.h` | `ryu/d2s_intrinsics.h` | `1d05702f2edacce428223d4b43dd3095c1bd1f3ad30128ce1761d84356dddc7d` | `01d9bdaada67b42f2d1bbdebb3765867e6c44e3a181b806c1d7e851b93f9cc21` |
| `d2s_full_table.h` | `ryu/d2s_full_table.h` | `2618f6e5fae6c4443899b184efe3d08295dd267dc9f1a994c983c7caca59ebe6` | `2618f6e5fae6c4443899b184efe3d08295dd267dc9f1a994c983c7caca59ebe6` |
| `digit_table.h` | `ryu/digit_table.h` | `8b782573abc0b8554d74163ae6c02f0beb5c30d1e63eaebdb0ddf2c98d817e01` | `8b782573abc0b8554d74163ae6c02f0beb5c30d1e63eaebdb0ddf2c98d817e01` |
| `f2s.c` | `ryu/f2s.c` | `0a98f4fce6be4c599f9602b8059e95bae6bffbb622dcde52deb4a8f8b51f5e0e` | `bbe6eef12da73d8d402c9178617c3780894eedd8a3cd6ee5bcd3d412a00aa304` |
| `f2s_intrinsics.h` | `ryu/f2s_intrinsics.h` | `662d2903454b8f6c44f18d0567e6d3f8f73f360d0d50a46e91fd245fdfa8fdc0` | `c73722ddeea62d81e2e94c0e2a7aaf8126713a960786f71584904a2a565967f4` |
| `LICENSE-Boost` | `LICENSE-Boost` | `c9bff75738922193e67fa726fa225535870d2aa1059f91452c411736284ad566` | `c9bff75738922193e67fa726fa225535870d2aa1059f91452c411736284ad566` |
| `LICENSE-Apache2` | `LICENSE-Apache2` | `c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4` | `c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4` |

## Minimal patch

1. `ryu.h` keeps only the declarations for `d2s_buffered_n` and
   `f2s_buffered_n`; unused fixed/exp declarations are removed.
2. `d2s.c` / `f2s.c`: removed `<stdlib.h>` and the malloc-backed convenience
   functions `d2s_buffered`, `d2s`, `f2s_buffered`, and `f2s`. Production code
   therefore has no `malloc`/`free` reference. Only the buffered `_n` entry
   points remain.
3. Include paths in `d2s.c`, `f2s.c`, `d2s_intrinsics.h`, and
   `f2s_intrinsics.h` are local filenames (`"ryu.h"`, `"common.h"`, ...)
   instead of upstream repository-root paths, so the files can be included
   from `runtime/native/numbers.cpp` without adding an include directory.
4. `f2s_intrinsics.h` retains the upstream conditional references to
   `f2s_full_table.h` / `d2s_small_table.h`, but neither `RYU_FLOAT_FULL_TABLE`
   nor `RYU_OPTIMIZE_SIZE` is defined by the runtime, so the required
   `d2s_full_table.h` is the only table selected.

No numerical code, lookup table, assertion, or rounding path was changed.
The upstream `RYU_DEBUG` diagnostic `printf` blocks are retained verbatim but
that macro is not defined by the runtime, so the production object has no
`printf` reference. `numbers.cpp` defines `NDEBUG` before including these
sources so freestanding builds do not reference `__assert_fail`; this does not
disable the parser validation in `numbers.cpp`.
