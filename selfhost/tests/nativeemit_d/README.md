# nativeemit D (async/GC) checks

This directory has two independent checks for the D share of `nativeemit`:

1. `main.wz` + the contract pass in `run.py` exercise the D helper surface
   (promise layout, coroutine signature, pure loop safepoints, ownership
   probes, the non-async terminator guard and GC critical/SAFE helpers).
2. `raw/driver.wz` builds real `nativeir.Module` values and calls the
   production `declare_runtime_types`, `declare_source_functions`,
   `emit_function` and `emit_main` algorithms. It does not copy D's algorithms
   and does not create a fake `VerifiedModule`.

The raw pass is the regression check for the D async/GC fixes. It is not
blocked by a missing `nativeverify` package: the raw fixtures use the real
B/C/E candidate plus the production nativeemit files. A missing real
VerifiedModule public entry is reported as `public_emit=not_run`; it is never
reported as a pass.

## Running

```sh
python3 selfhost/tests/nativeemit_d/run.py \
  --woozyc /abs/path/to/stage0/woozyc \
  --toolchain-root /abs/path/to/toolchains \
  --runtime /abs/path/to/liblang_native_runtime.a \
  --candidate-source /read-only/frozen/project/src/wzcc \
  --nativeemit-source selfhost/src/wzcc/nativeemit \
  --work-dir /disk/tmp/nativeemit-d \
  --only raw
```

`--candidate-source` is a read-only frozen tree containing the real B/C/E
files and the independent `nativelayout` and `valueops` packages. The runner
copies it to the work directory, overlays only the production `state.wz`,
`text.wz` and `d_*.wz` under test, and never writes back to the candidate or to
the production tree.

`--only contract` runs the helper driver; `--only all` runs both. The compiler
build timeout defaults to 600 seconds per command. `--force-run` attempts the
pass even when the prerequisite scan is incomplete; the real diagnostics are
still reported.

## Raw cases

Each case is emitted into one module and split by the `== CASE n` marker. Every
module is parsed by `opt -passes=verify`, lowered with the LLVM 22 coroutine
pipeline (`opt -passes=verify,default<O0|O2>,verify -verify-each`), compiled by
`llc`, statically linked with the real native runtime, and executed.

| case | fixture | expected result |
| --- | --- | --- |
| 0 | async main returns i32 42 | exit 42, stderr empty |
| 1 | async main cancel return | exit 1, stderr empty |
| 2 | async main i32 error return | exit 1, stderr `LANG_TYPED_ERROR: i32` |
| 3 | `await Task[Unit,Unit]` | exit 0 |
| 4 | `await Task[i32,Unit]` | exit 0 |
| 5 | `Vec[Vec[i32]]` strong-edge trace/clear registered with GC | exit 0 |
| 6 | completed Task with an owning Task result; GC trace then the generated frame clear twice | exit 31 (claimed=3, status=1) |
| 7 | completed Task with an owning Task error; same double clear | exit 32 (claimed=3, status=2) |
| 8 | suspended Task with an owning Task result cancelled by a second task; real cancel exit sets status 3, then the clear is a no-op | exit 33 (claimed=3, status=3) |

Case 5 calls `lang_vec_push_v1` so the traced prefix has one initialized element;
the generated `lang.gc.trace.vN` loop therefore executes its `icmp`, `mul` and
`add` body. Cases 6-8 call the generated `lang.async.frame.clear.<symbol>`
callback twice against the typed metadata, and read the promise claimed/status
words to prove the second call is a no-op. The child frame is still released
afterwards, so the runtime's own clear runs a second time on the same frame.

## Results and limits

`results.json` and `commands.json` in the work directory record every command,
exit code, output hash and the per-case status. `passed`, `failed`, `not_run`
and `unsupported` follow `development/AGENTS.md`.

Known independent B candidate issue: the frozen B tree still writes Vec value-op
TBAA metadata with a C byte string (`c"..."`) where LLVM metadata requires a
metadata string (`!"..."`). The raw fixtures deliberately use the runtime
`lang_vec_*` trace/clear path for the Vec strong-edge case and Task handles for
the owning-payload cases, so the D fix is checked without changing any generated
IR to hide that B defect. A fixture that drops an owning Vec through B's value
op will still fail `opt` until the B worker lands its fix; that failure must be
reported as B, not worked around in the test IR.
