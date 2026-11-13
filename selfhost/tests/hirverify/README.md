# HIRVerifier component and flow exit-identity regression

This directory reproduces two independent component fixtures for the accepted
HIR verifier:

- the original six Wz modes (`unit`, `nonunit`, `template0`, `template1`,
  `trait`, `throw`) assembled from real lexer/parser ASTs, at O0 and O2;
- the accepted generic `identity[i32]` / `identity[bool]` shared-source
  regression whose HIR builder is emitted by the C++ `resolve` +
  `sema::check_types` oracle on every run.

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
python3 selfhost/tests/hirverify/run.py \
  --woozyc /home/qin/Work/language-dev-20261103/compiler/build/woozy-stdlib-native/woozyc \
  --build-dir /home/qin/Work/language-dev-20261103/compiler/build/woozy-stdlib-native \
  --toolchain-root /home/qin/Work/language-dev-20261103/toolchains \
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

## Scope

The six-mode Wz tests use real lexer/parser AST positions but intentionally
hand-build the HIR records; they do not run Wz Sema on arbitrary source. The
exit-identity test uses the real C++ resolve/Sema HIR export, not Wz Sema. This
suite does not prove a complete frontend, Native/Python backend, full corpus,
or self-hosting completion.
