# HIRVerifier component and flow exit-identity regression

This directory reproduces three independent component fixtures for the accepted
HIR verifier:

- the original six Wz modes (`unit`, `nonunit`, `template0`, `template1`,
  `trait`, `throw`) assembled from real lexer/parser ASTs, at O0 and O2;
- the accepted generic `identity[i32]` / `identity[bool]` shared-source
  regression whose HIR builder is emitted by the C++ `resolve` +
  `sema::check_types` oracle on every run;
- the focused per-run lookup-index driver in `cache/`, which verifies two real
  projects repeatedly in one process and can compare against a pre-fix
  `hirverify` package.

`run.py` stages only the explicit package/standard-library import closure into
a private work directory. It never writes generated copies into the repository
`src` tree, and it does not use the old private worktree or an old
`generated.wz`.

## Reproduce

Prerequisites are an existing stage-0 build whose `woozyc` contains the real
`lang_native_helper`/runtime paths, the matching C++ static libraries and
headers, and the LLVM/libxml2 toolchain used by that build. Build artifacts are
not committed.

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
python3 selfhost/tests/hirverify/run.py \
  --woozyc build/woozy-stdlib-native/woozyc \
  --build-dir build/woozy-stdlib-native \
  --toolchain-root "${WOOZY_TOOLCHAIN_ROOT}" \
  --work-dir /tmp/hirverify-repro \
  --timeout 900 \
  --program-timeout 30
```

Defaults are repository-relative: `--woozyc` and `--build-dir` point at
`build/woozy-stdlib-native`, `--work-dir` at `build/hirverify-runs`, and the
toolchain defaults to a repository sibling or the checkout sibling of the given
`woozyc`. `--work-dir` is the parent of a fresh `hirverify-*` run directory;
`results.json` and `evidence/` remain there. Every command has a finite
process-group deadline. A timeout, non-zero exit, incomplete cleanup or
abandoned pipe fails the runner after retaining stdout, stderr and the JSON
process record. `--timeout`/`--program-timeout` reject NaN, infinity, zero and
negative values before staging or running commands.

The C++ oracles link the same real stage-0 libraries:

```text
liblang_sema.a liblang_hir.a liblang_modules.a liblang_frontend.a
-ljson-c -ltomlplusplus -lgmpxx -lgmp -lm
```

## Six original modes

One O0 and one O2 build are executed; each binary runs all six modes.

| mode | source | expected report |
| --- | --- | --- |
| unit | `fixture.wz` | 14 actual `PROBE` lines, exact C++-matching values, and independent snapshot checks `snapshot_children=0`, `snapshot_after_builder=0:2`, `snapshot_after_returned=0:2` |
| nonunit | `nonunit.wz` | six original `CHECK` labels, all `ok`, `failures=0` |
| template0 | `template.wz` | six original `CHECK` labels, all `ok`, `failures=0` |
| template1 | `template.wz` | six original `CHECK` labels, all `ok`, `failures=0` |
| trait | `trait-impl.wz` | six original `CHECK` labels including `trait-overlap-negative`, all `ok` |
| throw | `throw.wz` | six original `CHECK` labels, all `ok`, `failures=0` |

The source intentionally prints `SUMMARY probes=13` although 14 `PROBE` lines
are emitted. The runner checks the real 14 lines and does not edit the source to
make the old label look correct. The C++ oracle is also run for every mode; the
trait oracle must still reject duplicate impl patterns with
`E_IR_INVALID:duplicate or overlapping impl patterns`. O0 and O2 reports must be
byte-identical.

## Flow exit identity

`exit_identity/fixture.wz` is the accepted source byte-for-byte:

```woozy
package app;

func identity[T](value T) T { return value; }
func main() i32 {
    let n = identity[i32](7);
    let b = identity[bool](true);
    return n;
}
```

`exit_identity/` contains only the fixture, source driver, manifest and oracle.
There is no checked-in `generated.wz`. Each run:

1. builds `exit_identity/oracle.cpp` against the real C++ libraries;
2. runs it on the fixture, which executes `resolve_project` →
   `sema::check_types` → public verifier and writes a fresh `generated.wz` with
   the current symbol/type/node identities and real source spans;
3. checks the oracle's shared-source aliases and `TARGET_RETURN 16`, accepts the
   three positive API calls, and requires rejection of the bad Return and
   non-trivial bad ExitPlan;
4. builds that generated builder with Wz at O0 and O2 and runs both binaries.

The accepted report for each optimization is exactly:

```text
TYPE complete=1 diag=none
CAP complete=1 diag=none
VERIFY complete=1 has=1 diag=none
BAD_RETURN rejected=1 diag=E_IR_INVALID:lowering:error:primary=0:return target function mismatch
BAD_EXIT rejected=1 diag=E_IR_INVALID:lowering:error:primary=0:exit target/scopes/cleanup order/value preservation mismatch
```

The generated file hash in `results.json` is the hash of the builder emitted by
that run, not a committed/old HIR. With the accepted Root stage-0 build the
current generated SHA-256 was
`4d22fc1c7870c8b3fccf7ddfc622e24ec5d9ed49d5b2412886fd31b845717357`; the
symbol IDs differ from the older proof because the current builtin table has
the accepted `stat` operation.

## Process and report controls

```sh
python3 selfhost/tests/hirverify/test_process_controls.py -v
```

This uses the actual `command` and `finite_timeout` functions extracted from
this runner and from `sema_a/run.py`/`sema_b/run.py`. Tiny Python fake tools are
deliberately used; they are not language or Checker tests. The controls cover:

- normal stdout/stderr retention and cleanup fields;
- timeout output retention;
- non-zero exit rejection;
- a parent exiting 0 while a child holds a pipe, which must still fail with
  `cleanup_complete=false`/`abandoned_pipes=true`;
- NaN/Inf/zero/negative deadline rejection before the command starts, including
  the real CLI exit-2 path;
- mutation controls for missing/duplicate `PROBE`, wrong verified/snapshot
  values, missing/failed `CHECK`, missing exit negatives/aliases, and unexpected
  stderr.

The Sema A/B runners now use the same bounded `selfhost/corpus.py` process
helper and retain `label.stdout`, `label.stderr` and `label.json` for every
command. Their staging, 47/27 assertions, `--run`/`--slim` interfaces and exit
requirements are unchanged.

## Per-run verifier lookup indexes

The focused `cache/` driver checks the two whole-module scans that the HIR
verifier previously repeated:

- function symbol -> its HIR nodes, used by `source_effect_nodes` for both
  `node_types` and the owning-node gate;
- the exact Source `NodeRef` set for every ForIn `iterator.next` node and its
  children, used by `ownership_sources`.

The production check order is preserved statically: the indexes are built once
after `shape`, and the consuming loops keep the original node order.  The
existing six-mode tests pass with their recorded labels/codes, and the current
`BAD_SOURCE` control sets `nodes[0].source.node.node = 999999` and gets
`E_IR_INVALID` with `rejected=1 failed=1`.  This has not dynamically exercised
every span field or every possible error-priority combination.  The entries are
private to one `Verifier`/run.  The synthetic set is keyed by module, file and
node, matching `same_ref`; it is not keyed by a numeric node id or span alone.

`cache/driver.wz` loads two real projects through
`project.load -> resolve -> sema.check_types`, then verifies the published HIR
repeatedly in one process.  The first is the checked-in concrete iterator
fixture; the second is a generated 40-function, 830-node project with the same
real `std/iter` and `std/option` imports.  The driver also clones a module,
breaks a Source node reference and requires `E_IR_INVALID`, and alternates the
two modules on every pass so a stale cross-module cache would be visible.  A
test-only staged wrapper compares NodeRef keys with equal node ids in different
modules and equal module/node ids in different files; both must differ.  That
is a comparator-level control, not a test of a specific collision in the
116-source compiler input.

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
python3 selfhost/tests/hirverify/cache/run.py \
  --woozyc build/woozy-stdlib-native/woozyc \
  --toolchain-root "${WOOZY_TOOLCHAIN_ROOT}" \
  --work-dir /tmp/hirverify-cache-repro \
  --iterations 2
```

Each successful pass reports the two module sizes (`105`/`830` nodes),
`KEY module-file-node distinct`, two `BAD_SOURCE rejected=1 ...` lines, and
exactly one `ITERATIONS 2 MODULES 2 VERIFIES 4 BAD 0` line.  For every round the
runner checks both reports immediately and requires the new/baseline non-timing
output to be equal; a first-round difference cannot be hidden by a later round.
It also requires one positive integer `ELAPSED_NS` per program.  The runner
builds only this focused driver at O0; it does not rebuild the 116-source CLI
or run the complete corpus.  To compare the pre-fix verifier, export that
revision's package and pass it explicitly:

```sh
git archive <pre-fix-rev> selfhost/src/wzcc/hirverify | tar -x -C /tmp/hirverify-old
python3 selfhost/tests/hirverify/cache/run.py ... \
  --baseline-dir /tmp/hirverify-old/selfhost/src/wzcc/hirverify \
  --iterations 2 --repeats 3
```

`results.json` retains every bounded command record, source hashes, raw
stdout/stderr, per-round equality, the exact positive `ELAPSED_NS` values and
their median ratio.  The ratio is only this component driver; it is not a
whole-compiler speedup claim.

Report, elapsed and repeat-loop controls:

```sh
python3 -B selfhost/tests/hirverify/cache/test_controls.py \
  > /tmp/hirverify-cache-controls.json
```

These use the actual runner checker/parser and bounded process helper with tiny
fake programs.  They require first-round mismatch rejection even when later
rounds match, reject zero/wrong/duplicate counts and zero/negative/missing/
duplicate elapsed values, and accept a valid equal pair.  They also check
report paths containing spaces.  They do not compile Woozy source or run the
retained ELF drivers.

## Per-Verifier needs_drop bool cache

The focused `cache/needs_drop/` component adds the same-package cache probe
described in `cache/needs_drop/README.md`.  It checks successful true/false
results, invalid-id failures, independent modules and Verifiers, same-module
sharing, fresh Verifiers after a TypeContext change, real Source alternating
verification and warm-cache HIR tamper rejection.  Its runner builds one O2
component and records the fixed seed/tool/runtime hashes and a separate
exec-only trace.

## Expression-kind gate

`expression_kinds/run.py` is a smaller component check for the production
`hirverify.verifier_expression`/`verifier_statement` helpers.  It injects only a
package-internal forwarder (`expression_kinds/probe.wz`) and builds one O0
driver.  The driver checks the `Await` (42), `TraitObjectInit` (43) and
`TraitDispatch` (44) kinds are expressions; `ExpressionStatement` (16), `Noop`
(19) and `Block` (15) are not; and foreign place accesses remain rejected.  A
pre-fix package directory is run once with `--expect-exit 1` to retain the
observed three failures.  This does not build the 116-source CLI, the C++
oracle/probe, or run the complete corpus.  See `expression_kinds/README.md`.

## Scope

The six-mode Wz tests use real lexer/parser AST positions but intentionally
hand-build the HIR records; they do not run Wz Sema on arbitrary source. The
exit-identity test uses the real C++ resolve/Sema HIR export, not Wz Sema. The
cache driver does run the real Wz project/resolve/Sema pipeline, but only on
its two focused fixtures and only for repeated verifier passes. This suite does
not prove a complete frontend, Native/Python backend, full corpus, or
self-hosting completion.
