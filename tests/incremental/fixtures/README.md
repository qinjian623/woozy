# Source-package incremental fixtures (reference-prepared)

These fixtures prepare source-level inputs for future cold/warm/dirty checks of
the source-package incremental feature. They are **data** for language
semantics, not an implementation of caching, Ninja, or compiler code.

The frozen old reference compiler was used only to establish the expected
language behavior. The runs below do **not** measure cache hits, package
incremental reuse, Ninja scheduling, or performance. The incremental feature was
not run and is not claimed to pass.

## Layout

```
tests/incremental/fixtures/
  manifest.json                 # fixture list + all file SHA-256 values
  run_reference.py              # small verification driver (not a build system)
  01-diamond-private-body/
    fixture.json                # base file hashes, base expectation, variant list
    base/                       # complete lang.toml + source tree
    variants/<name>/
      mutation.json             # actions, old/new content hashes, expectation
      old/<path>                # exact old full files (absent for add)
      new/<path>                # exact new full files (absent for remove)
  02-package-alias-shared/
  ...
  06-package-set-errors/
```

Each `mutation.json` contains:

* `actions`: `add`, `replace`, or `remove`, with the project-relative path and
  references to the exact full old/new file bytes under `old/` and `new/`;
* `expected_invalidation`: which logical packages should be rebuilt/reused and
  which caller/link steps must not serve stale success — a **design
  expectation**, not a measurement;
* `expect`: exact reference command mode, exit/status, diagnostic codes, and
  expected program exit/stdout/stderr.

The base project itself is also run by the driver. Variants are applied to a
fresh copy of `base/` in a new work directory. A variant result is never
rewritten by the driver.

## Fixtures

| Fixture | Base result | Variants | Purpose |
| --- | --- | --- | --- |
| `01-diamond-private-body` | `main` = 40 | `private-body` = 42 | `app -> left/right -> core`; private `core.secret` body changes, public `offset` signature does not |
| `02-package-alias-shared` | 40 | `shared-helper-body` = 41, `alpha-value` = 42, `alias-swap` = 25 | same-package top-level sharing across files; the same alias name is file-local and may resolve to different modules |
| `03-public-const-signature` | 16 | `const-value` = 20, `signature-params` = `E_TYPE_MISMATCH` | public const value changes with the same type; public function signature changes |
| `04-generic-body-dedup` | 44 | `private-helper` = 66, `generic-body` = 66, `caller-literal` = 46 | two packages use the same `dep.choose[i32]`; the generic body calls a package-private bool helper |
| `05-owning-vec-throws` | 4 | `vec-values` = 7, `no-throw` = 6 | cross-package owning `Vec[i32]` return, alias sharing, `Weak` release, and typed throw |
| `06-package-set-errors` | 12 | `add-file` = 14, `remove-file-stale` = `E_NAME_NOT_FOUND`, `add-package` = 17, `remove-package-stale` = `E_IMPORT_NOT_FOUND`, `import-cycle` = `E_IMPORT_CYCLE`, `syntax-failure` = `E_PARSE_EXPECTED_TOKEN`, `type-failure` = `E_TYPE_MISMATCH` | add/remove files and packages, import cycle, syntax and type failures after a known-good base |

`main` returning a nonzero value (for example 4 or 7) is an expected program
result, not a test failure. Failure variants use `woozyc check` and expect exit
1 with the named diagnostic code.

## Reference driver

The driver requires the caller to provide the frozen reference and its runtime
environment; it does not discover a compiler or install dependencies.

```sh
REF=/path/to/woozyc
FIX=/path/to/tests/incremental/fixtures
D=/path/to/checks/SH31-reference-fixtures-20261117

python3 "$FIX/run_reference.py" \
  --fixtures "$FIX" \
  --reference "$REF" \
  --work "$D/build/fixtures" \
  --raw "$D/raw/fixtures" \
  --report "$D/reference-results.json" \
  --commands "$D/reference/commands.json"
```

Successful variants are compiled once with:

```sh
woozyc build --project <project>/lang.toml --backend native --opt 0 \
  --out-dir <out> --format json
```

and the generated `program` is run once. Failure variants use:

```sh
woozyc check --project <project>/lang.toml --format json
```

The driver refuses to overwrite an existing `result.json`. It does not retry a
failed command and does not invoke Ninja.

The reference compiler used for this preparation is the old C++ `woozyc`
(`0.1.0-bootstrap`), SHA-256
`c48984719ccb02e1b00d9e8119d2d3f61bcc4db0507b4aa51d22365b6475012c`.
Its sibling `native/lang_native_helper` is
`a1786592f70940fd814358a5fcc2df25098f07ce7a83eeb5d6e1d5146a82047b`.
Native builds need LLVM 22.1.8 and the legacy libxml2 runtime in
`PATH`/`LD_LIBRARY_PATH`; no `--std-binding` was passed.

## Scope and limitations

* 24 reference runs (6 bases + 18 variants) were executed once each, all passing.
* The source files are deterministic and use no external I/O.
* `expected_invalidation` is not measured. In particular, the generic fixture
  intentionally asks for one `dep.choose[i32]` instance shared by two packages,
  but this reference only verifies language results; it does not prove dedup.
* Failure variants are intended for future stale-output tests. This preparation
  does not build a successful base into the same output directory before the
  failing variant.
* No main-repository source, implementation worktree, or compiler code was
  changed. No commit or push was made.
