# Source-level Make packages

This directory is a self-test fixture for:

```sh
woozyc build --project selfhost/tests/source_make/lang.toml \
  --out-dir OUT --source-make --cache-dir CACHE --opt 0|2 --format json
```

The fixture has three canonical `model.Module` packages:

* `app` (entry, one file)
* `app/libcore` (two `.wz` files in one package)
* `app/libmath` (one file)

`app.main` calls `app/libcore.combine` and `app/libmath.twice` across package
boundaries.  The program exits with 19.

## First-slice support

* Linux x86_64, Native/64, O0/O2.
* Ordinary non-generic functions, including scalar and non-generic struct/enum examples.
* Multiple files in one canonical package.
* Cross-package `pub` ordinary function calls with stable source-derived names.
* Imported functions are verified external declarations; their definitions are
  emitted only by their own package.

Other ordinary language features may work through the existing checker and
backend, but are not claimed by these tests. Generics, traits/impls, async,
lambdas, foreign/C bindings and `std/` package imports are
rejected explicitly with `E_CAPABILITY_UNAVAILABLE`.

## Caches and logs

The cache key binds the canonical package, every current source byte, real
transitive dependency interfaces, the compiler/LLVM/runtime/make fingerprints,
target and optimization.  Only a package object is cached, after its checks
and object build pass.  A package whose private body changes is rebuilt while
its importers reuse their objects.  A public interface change invalidates the
actual importers.

`Makefile`, `make.log`, `preflight.log`, `combine.log`, `link.log`,
`make-report.json`, `cache-statistics.json`, and one `module-<fixed>.log`
per package are published for inspection.  Real paths are stored in the
private length-prefixed state record; Make targets use fixed-width package ids.

## Run

```sh
python3 -B selfhost/tests/source_make/run.py --woozyc /path/to/woozyc
```

The compiler needs its normal `toolchain/`, `runtime/` and `stdlib/` resources,
LLVM 22.1.8, and GNU Make.  If the local LLVM build needs the legacy libxml2,
inherit the appropriate `LD_LIBRARY_PATH`.
