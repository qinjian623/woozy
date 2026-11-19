# Sema B runtime tests and evidence

Operation: `SH06k-sema-b-utf8-runtime-20261113` (continuation of
`woozy-SH06k-sema-B-20261113`).  This directory owns only the B-side files
`selfhost/src/wzcc/sema/b_*.wz` plus these tests/evidence.

## Production fixes

1. **UTF-8 validation** (`b_expr.wz`, `b_valid_utf8`): the old hand-written
   FSM indexed a Woozy string by Unicode scalar while looping to
   `text.byte_len`, and its two-byte lower bound checked `second` instead of
   `first`.  It rejected legal `é`, `中`, `🙂` and Panicked on `À`/`π`.
   It is replaced by the existing literal-package approach:
   `text.from_bytes(text.bytes(value))`, byte-length equality, and
   `catch text.Error { false }`.  Invalid UTF-8 rejection is retained.
   `b_expression_literal` still consumes the lexer's decoded
   `Token.string_value` after `has_string_value`, matching CPP.
2. **Mutually exclusive effect traversal** (`b_effects.wz`, `annotate`):
   Switch/ForIn/Try/Defer are one `if/else if` chain.  The old four
   independent switches re-walked special children through the
   `defer_data == None` fallback.  Try re-walked its body after the handler
   stack was popped; Switch re-walked arms without re-binding patterns.
   Error/exit summaries after traversal are unchanged.
3. **Foreign property marker** (`b_expr.wz`): the `ForeignOpaque` property
   branch emits `Some(ForeignAccessKind.PropertyRead)` (CPP 1921-1925).
   The `None` access for module values/methods/callables is unchanged.

The old UTF-8 failure is preserved in Root's
`checks/selfhost-sema-b-copy-review-20261113/utf8-helper/` and
`utf8-oracle/`.  Patches against both frozen review snapshots and all hashes
are under `build/sema-b-runtime-final5/evidence/`.

## Case driver

`src/main_slim.wz` is the executed body/effect driver.  It constructs real
`model.Project` values from source text through accepted
source/lexer/parser/resolve, then calls the real constructor, `declarations`,
top-level constants and `check_body` through `helpers.wz`.  It does not use a
stub Checker or fake Verified HIR.  The public `check.wz`/HIR verifier are
deliberately omitted in this mode; this is a complete body-algorithm test,
not a public verified-check claim.

`src/main.wz` is the full-facade driver for a later accepted HIR verifier.

Cases include:

- all scalar/constant fold paths, named arguments, struct fields;
- enum switch with pattern binding, Vec indexing/for-in;
- closure capture, try/throw/catch, try-caught and switch-binding review
  regressions;
- let assignment, non-exhaustive switch, implicit move, overlapping inout,
  bool condition and literal range rejects;
- Unicode literals: `abc`, `é`, `À`, `π`, `中`, `🙂`, escaped newline/tab,
  `\u{1F642}`, NUL;
- a raw invalid UTF-8 source-byte case that must be rejected before Checker;
- a test-only Python binding fixture for `demo.Point.size`:
  `Some(PropertyRead)` and a plain value `ForeignRead` with `None` are both
  asserted in the generated HIR.

`compare_cpp.py` extracts the same source strings, writes real C++ projects,
runs the accepted fast seed's public `check --project`, and compares the first
diagnostic code/phase/primary byte range (and accept/reject status).
The property case is not run against a plain C++ project because it needs the
binding; its Wz binding/marker/None checks do run.

## Actual results (2026-11-13)

Accepted fast seed and helper used:

- seed `5caa3550ab489e404cde9ea1bcc13fef39ce3e386dc1ef2e6e5bc3b8723ed03e`
- helper `0df0cea068d1bf1d123f5738c7561028f6334d2fdd63d3031d6f2afe7f3ee120`

Real A was taken from Root `<repo>` at `selfhost/src/wzcc/sema`; shared six
files matched Root hashes.  HIR verifier
was not used (its four formal fixes are not accepted yet).

- Per-file syntax checks: pass (8/8 B files).
- Staged A/B/shared slim `check --project`: `status=ok`.
- O0 build/run: **27/27 checks passed**, real `out-o0/program`.
- O2 build/run: **27/27 checks passed**, distinct `out-o2/program`.
- Wz O0 and O2 both accepted all Unicode literals; raw invalid UTF-8 source
  bytes were rejected before Checker; no Panic.  The accepted C++ public
  check on the same bytes reports `E_INVALID_SOURCE`/`lex` (evidence
  `invalid-utf8-cpp.json`); the Wz driver asserts rejection, not yet a
  field-by-field diagnostic comparison for this pre-Checker boundary.
- CPP comparison O0: `compared=17 skipped=1(property binding) failed=0`.
- CPP comparison O2: `compared=17 skipped=1(property binding) failed=0`.
- Full public `check_types`/`check_project`/HIR verifier: **not_run** for this
  correction because the HIR verifier is still in formal correction.  No
  Verified/self-host completion is claimed.

Evidence:

- `build/sema-b-runtime-final5/run.stdout`
- `build/sema-b-runtime-final5/sema-b-0s2c20mo/out-o{0,2}/program`
- `build/sema-b-runtime-final5/evidence/run-o{0,2}.stdout`
- `build/sema-b-runtime-final5/evidence/cpp-compare-o{0,2}.stdout`
- `build/sema-b-runtime-final5/evidence/{b_effects,b_expr}*.patch`
- `build/sema-b-runtime-final5/evidence/{b,root-input,seed,program,test}-hashes.txt`
- `build/sema-b-runtime-final5/evidence/invalid-utf8-cpp.json`

Earlier failed runtime and long-build/interruption records are retained under
`build/sema-b-runtime-20261113/`, `build/sema-b-runtime-debug/`, and
`build/sema-b-lite/`; they were not overwritten.

## Reproduce

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
python3 selfhost/tests/sema_b/run.py --slim --run \
  --woozyc /absolute/path/to/accepted-seed/woozyc \
  --a-dir selfhost/src/wzcc/sema \
  --toolchain-root "${WOOZY_TOOLCHAIN_ROOT}" \
  --work-dir build/sema-b-repro --timeout 1200

python3 selfhost/tests/sema_b/compare_cpp.py \
  --woozyc /absolute/path/to/accepted-seed/woozyc \
  --wz-results build/sema-b-repro/.../run-o0.stdout \
  --work-dir build/sema-b-cpp-repro --timeout 120
```

`--slim` omits only public `check.wz`/HIR verifier.  It does not stub
`declarations`, `annotate`, `flow`, `check_body`, `instantiate` or any B
algorithm.

All runner subprocesses use the corpus finite process-group deadline/cleanup
helper, retain each command's stdout, stderr and JSON process record, and
reject NaN, infinite, zero or negative `--timeout` before staging.
