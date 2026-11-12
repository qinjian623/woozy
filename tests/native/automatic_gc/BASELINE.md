# Current Native baseline — corrected automatic-GC tests

Operation: `woozy-native-auto-gc-tests-correction-20261112`
Recorded: 2026-11-12 03:54 +0800 (Asia/Shanghai)
Runtime under test: **plain ARC, no pure-Woozy strong-cycle collection**.

Result of the pinned baseline run:

```text
suite_scope: full (9 cases, O0/O2, 18 runs)
coverage: controls + cycles + pressure
status: expected_failure
gc_test_result: not_implemented
expected_failure: true
not_implemented: true
mem02_complete: false
feature_claim: cycles_not_reclaimed
control_failures: 0
cycle_target_failures: 14 / 14
cycle_baseline_failures: 0 / 14
```

This is the documented no-collection leak, not a GC pass. The full
per-command summary and raw observation streams are external:

```text
/tmp/native-auto-gc-correction-20261112/baseline/summary.json
/tmp/native-auto-gc-correction-20261112/baseline/raw/...
/tmp/native-auto-gc-correction-20261112/target/summary.json
```

The candidate tree keeps only this compact report and
`baseline/pins.json` (tool/source hashes and per-case metrics).

## Tool pin

`baseline/pins.json` contains the complete pin. The run used a copied CLI
snapshot at `/tmp/woozy-auto-gc-pin-20261112/woozyc` with an explicit expected
hash, plus the Root helper/runtime hashes:

| item | sha256 |
| --- | --- |
| `woozyc` (pinned copy) | `63727015aa4f68c701c386f3b2fc35ed25bc2ca67bff57970bc7974e1a4d0391` |
| `native/lang_native_helper` | `ebc252fa4b315fd3f737ae8aa022c07a197d2411751be0178fe3a92cd814e52f` |
| `native/liblang_native_runtime.a` | `5ef6fdc9c9e1b9a308e4723d9e93eedaca635f45f1635d269c536b3dacd0f71b` |
| `llvm-22.1.8/bin/clang` | `31a40dc31f3c15a47aa119aa148f339bf363d8f61202ddbdacbd3f24b71ba113` |
| `llvm-22.1.8/bin/llvm-readelf` | `338c233bc2419b0abdb8f0344912504c26eeab1cd671d6651f4bb8405bb48385` |

`clang --version` is 22.1.8. The legacy libxml2 prefix was prepended to
`LD_LIBRARY_PATH` for the fixed `ld.lld`. Root may rebuild `woozyc` while
running Python GC tests; the copied CLI and explicit `--expect-*` hashes keep
this evidence pinned. The helper/runtime hash must remain stable; a change
makes a run `inconclusive_tool_pin`, not a GC verdict.

## Baseline per program (O0 and O2 identical)

`alloc/free/live` count Vec objects. Payload is live Vec payload bytes
(`ALLOC size - runtime Vec header`, header 80 here); headers and raw metadata
are excluded.

| case | opt | Vec alloc | Vec free | live Vecs | block/raw | peak payload | final payload |
| --- | --- | ---: | ---: | ---: | --- | ---: | ---: |
| `control-empty` | 0,2 | 0 | 0 | 0 | 0/0 | 0 | 0 |
| `control-acyclic` | 0,2 | 4 | 4 | 0 | 4/0 | 32 | 0 |
| `self-cycle` | 0,2 | 2 | 1 | 1 | 2/0 | 16 | 16 |
| `duplicate-edges` | 0,2 | 3 | 2 | 1 | 3/0 | 24 | 24 |
| `two-cycle` | 0,2 | 4 | 2 | 2 | 4/0 | 32 | 32 |
| `enum-nested-cycle` | 0,2 | 1 | 0 | 1 | 1/0 | 24 | 24 |
| `dead-cycle-live` | 0,2 | 32786 | 16388 | 16398 | 32786/0 | 8585520 | 8585456 |
| `loop-continue` | 0,2 | 32769 | 16385 | 16384 | 32769/0 | 8585280 | 8585216 |
| `typed-throw-defer` | 0,2 | 2 | 1 | 1 | 2/0 | 16 | 16 |

The two pressure programs create 8192 rounds of roughly 1 KiB garbage per
round. The no-collection baseline reaches about **8.19 MiB** of live Vec
payload at peak and at exit, well above the 2 MiB target budget. The controls
balance exactly. Small cycle fixtures keep the expected dead cycle(s).

## Corrections in this baseline

1. **Pressure policy, not eager scope timing.** `loop-continue` now runs 8192
   rounds with a fixed live root and checks `peak/final live Vec payload <=
   2 MiB`; it no longer requires collection every eight objects. `dead-cycle-live`
   runs the same pressure between two source markers and checks final cleanup,
   edge release, root readability and the budget, not collection before a
   marker. This follows Root's pressure plan (128 initial registrations, then
   `max(128, 2*live)`, stable function/loop heads, normal main-return final
   collection).
2. **Generation identity.** Vec identity is `(address, generation)`. A legal
   FREE followed by a new ALLOC at the same address keeps both histories. The
   independent review's address-reuse counterexample is now a selftest and
   passes; a real leak still fails.
3. **Metadata separation.** Raw allocator blocks without `VEC:1` are collector
   metadata: excluded from Vec payload budget and from `live Vec freed last`.
   A synthetic metadata-freed-after-live counterexample passes.
4. **Scope/coverage result.** There is no `automatic_gc_implemented` boolean.
   The report separates:
   - `control_only_passed` for a controls-only selection (no GC claim);
   - `selected_scope_passed` for a passing selected cycle subset;
   - `full_suite_passed` for the full 9-case O0/O2 single-mutator suite;
   - `not_implemented` when reclamation checks are the only failures;
   - `inconclusive_tool_or_build`, `inconclusive_observer_abi` and
     `inconclusive_tool_pin` for build/tool/observer failures;
   - `failed` for source/root-content or metric mismatches.
   `mem02_complete` is always false from this suite.

## Verification of the correction

Pinned commands and results (full logs external):

```text
# Baseline (recorded here): exit 0, expected_failure / not_implemented
run_automatic_gc.py ... --baseline-only --timeout 120 \
  --compact-out .../baseline/compact.json

# Target: exit 1, failed / not_implemented, controls 0 failures
run_automatic_gc.py ... --timeout 120 \
  --compact-out .../target/compact.json

# Local control: exit 0, control_only_passed, coverage.cycles=false,
# feature_claim=none, no automatic_gc_implemented field
run_automatic_gc.py ... --case control-empty --opt 0

# Deliberate pin mismatch: exit 2, inconclusive_tool_pin
run_automatic_gc.py ... --expect-cli-sha256 deadbeef ...
```

`tests/native/automatic_gc/selftest_checks.py` passes all synthetic checks:
small collected programs, real-leak rejection, address reuse generations,
metadata free order, payload budget, control-only scope, full-suite scope and
harness-failure classification.

For all 18 baseline runs the real CLI program and the static observation
binary have no `INTERP` and no `NEEDED`.

## Limits

- No `Weak`, capturing-closure, concurrent-GC, thread, fork, channel or
  foreign-cycle behavior is verified.
- No RSS/page-cache observation is used as evidence.
- The pressure budget and final-collection behavior follow the Root Native
  implementation plan; they are not a source-language timing guarantee.
- A full-suite pass would be single-mutator Native cycle evidence only, not
  MEM02 completion.
- This baseline is history. When Native GC is integrated, keep the target
  checks, regenerate the baseline, and do not mark it as a GC pass.
