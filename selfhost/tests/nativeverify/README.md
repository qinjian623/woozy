# SH07d: Native CFG verifier and shared layout

Scope: `selfhost/src/wzcc/nativeverify/**`,
`selfhost/src/wzcc/nativelayout/**`, `selfhost/tests/nativeverify/**`.

Production sources:

| file | C++ counterpart |
| --- | --- |
| `nativelayout/nativelayout.wz` | `src/ir/layout.hpp` |
| `nativeverify/support.wz` | ordered std::map/std::set replacements, `Bad`, `SupportError` conversions |
| `nativeverify/copies.wz` | C++ `Module`/`PhaseResult` value copies |
| `nativeverify/verify_function.wz` | `Verifier::run`/`function` setup, definitions, address roots, reachability/dominators, enum facts, slot initialization, system-call order/dataflow |
| `nativeverify/verify_ops.wz` | the complete per-Op switch, all 9 terminator kinds, impossible-variant and bounds-check checks |
| `nativeverify/cleanup.wz` | exit/defer dataflow and required-exit obligations |
| `nativeverify/obligations.wz` | `src/ir/obligations.hpp` |
| `nativeverify/ownership.wz` | `src/ir/ownership_verify.inc` |
| `nativeverify/nativeverify.wz` | private `VerifiedModule`, `VerifyResult`, `verify`, `snapshot` |

All 53 `Op` values and all 9 `Exit` values are handled; a final
`need(false, "unknown native operation")` never runs for a valid enum.  The
tail `lang::ir::dump` is intentionally not ported here; `nativeirdump` owns it.

## Contract points

* `verify` deep-copies the CFG first, calls
  `hirverify.verify(input.hir, VerifyMode.Backend)` (the HIR verifier itself
  deep-copies the raw HIR), then checks the complete CFG against
  `hirverify.snapshot(verified)`.  It never trusts raw builder records.
* `hirverify.snapshot` now throws `model.SupportError`
  (SELFHOST_CONTRACT.md line 182).  A snapshot failure inside `verify` is
  classified like any other shared-algorithm invalid IR: `Failed`/`Internal`,
  one `E_IR_INVALID` diagnostic with the original message and no primary span.
  The public `nativeverify.snapshot` keeps `throws model.SupportError` and
  propagates the original error.
* `VerifiedModule` fields are package-private; only `verify` constructs one.
  There is no verifiable flag, no second IR, and `snapshot` deep-copies the
  HIR (through `hirverify.snapshot`) and every native Vec again.
* `nativelayout.AggregateTypes` owns an independent `clone_context` of the
  public type table.  Field substitution may intern storage-only types in that
  private copy; the public `HirModule.types` is never appended to or rewritten.
  Returned declarations/fields are independent values, so a caller cannot
  mutate `module.nominals` through a shared Vec either.
* Map/set iteration order is the C++ key order (`std::map`/`std::set`,
  tuple-lexicographic keys and `optional` with `None < Some`).  The pure helper
  self test checks the order for definitions, instructions, parameters, element
  roots, owner maps, counters, handlers, instances, catches, symbols, flags and
  obligation keys, and it caught a real insertion-position bug during
  development.

## Deliberate, documented deviations

* `layout.hpp` dereferences `types.find(id)` in `fields` before `declaration`
  (C++ UB on an absent TypeId) and uses `type_args.at(p)` (libstdc++ message on
  a short argument list).  The Wz port reports
  `SupportError.Invalid("unknown aggregate declaration")` and
  `SupportError.Invalid("aggregate type argument count")` instead of crashing.
  For every input where the C++ code is defined, values match the oracle.
* C++ `nodes.at`/`slots.at`/`blocks.at`/`defers.at` failures become
  libstdc++ `out_of_range` texts.  The Wz port returns
  `SupportError.Invalid` with a stable message (e.g. `invalid HIR node index`).
  These paths are unreachable for HIR-verified data; they only keep malformed
  builder data from Panicking.
* C++ ownership `root`/`receiver` recursion has no cycle guard; the Wz port
  detects such a cycle and returns `SupportError.Invalid` (unreachable for a
  CFG whose definitions dominate their uses).

## Tests

`layouttest/` builds the same type table and nominal declarations as
`oracle.cpp` (which includes the real `src/ir/layout.hpp` and links the
accepted `liblang_hir.a`), then compares stdout byte-for-byte after stage0
check, O0 and O2 builds.  The Wz program also asserts that a returned
declaration cannot rewrite `module.nominals` and that the private context never
appends to the public table.

`packagecheck/` stages the accepted `model`, `hir`, `types`, `nativeir`,
`foreign`, `pattern` packages from the repository (or `--extra-source` for
packages still landing elsewhere) plus the complete `nativeverify` package and
runs a package-internal self test.  When `hirverify` is absent, only the
facade file `nativeverify.wz` is skipped (single-file syntax check instead);
all other verifier logic is still type-checked, and the shared containers,
copy and obligation helpers are executed.  The self test covers ordered
containers, obligation keys/counts, both-direction CFG deep-copy isolation and
`E_IR_INVALID` diagnostic shape.

`obligationstest/` builds a hand-written HIR control tree (call error exit,
nested defers, return, loop continue/break, exhaustive switch, try/catch and
await error/cancel) and compares the real `required_exits` helper with
`src/ir/obligations.hpp`.  It needs no `hirverify`: the C++ oracle calls
`lang::ir::detail::RequiredExits` on the same builder tree.  Stdout must match
byte-for-byte at O0 and O2; the current fixture produces 7 lines.

`driver/` is the real end-to-end driver for Root integration.  It runs
`project.load -> resolve -> sema.check_project -> nativelower.lower` on real
fixtures (cleanup/ownership/async), verifies the unmodified lowered snapshot,
checks that metadata/owner/error-edge/slot/await mutations are rejected with
`E_IR_INVALID`, and checks snapshot isolation.  While `hirverify` is absent it
reports `not_run` with the missing packages and never fabricates a verified
value.

## Dependency snapshots used for algorithm checks

`packagecheck/` and `obligationstest/` can load accepted packages from another
tree through `--extra-source <tree>/selfhost/src`.  The latest accepted
inputs are `valueops/valueops.wz`
`19149ef4e0cb4a76d6dfd52425eb4c3a552004bf04d84577aa9dd7785723a460`
and `pattern/pattern.wz`
`311c0c03b08d8878250e99267f24ba307e24cbadcbec6e5f355d59e3ba3c1080`
(the accepted ownership analysis is
`a2a9ed16b403088967b159ac4aa5f99c6574f73300350342c30ac7cb503b4f1e`,
but the Native CFG ownership verifier is the port in this package, not that
analysis package).  These runs are algorithm/package checks only; they do not
claim that the public `nativeverify.verify` facade is integrated.

## Known not_run in this branch

* Full `nativeverify` package check including the facade, and any real
  `nativeverify.verify` behavior, need `hirverify` (Root has not added it to the
  main tree yet).  The facade is only syntax-checked and statically matched to
  the frozen contract and to `nativelower`'s calls.
* `driver/` is `not_run` until `hirverify`, `valueops`, `hirclone`, `project`,
  `sema`, `nativelower`, `nativeirdump` are integrated.
* `packagecheck/` and `obligationstest/` were run with the accepted
  `valueops`/`pattern` inputs above.  The obsolete `f52ac...` valueops
  candidate is no longer used by either test.
