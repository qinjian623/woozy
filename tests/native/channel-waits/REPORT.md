# CON02 verification report

Status: **passed** for `send_timeout`, `receive_timeout`, `send_cancel`,
`receive_cancel`, timeout/cancel races, and the existing Channel suite.

## Sources

- Baseline copied read-only from Root: `stdlib/channel/channel.wz` SHA-256
  `613ba7027c04083865f549b1a082e636207065ba06725cba1a0702dde843f357`.
- Final `stdlib/channel/channel.wz` SHA-256
  `816c49dfe33361c720a4b1a04e9e9b785454e7cb203a400b0941483fbe6159d3`.
- Final `stdlib/cancel/cancel.wz` SHA-256
  `9c53f0626fbd1298b47c69404bdf8329e1518c73cd248715700228d31b1d25b0`.
- Contract: `stdlib/channel/CONTRACT.md`.
- This task did not modify native thread/sync, rwlock, or shared compiler
  sources.

## Toolchain

Root source (with the 20261112 `sync.wait_for`/`thread.wait_for` registry) was
copied read-only into `/tmp/con02-src`; only its `stdlib/channel` and
`stdlib/cancel` were replaced with this worktree's versions. A clean compiler
was built into `/tmp/con02-build` with LLVM/clang/ld.lld 22.1.8 and legacy
libxml2 2.13.9. `/tmp/con02-build/woozyc` SHA-256:
`25d90d97890781a9b8cd723778d606becc843ba2af5f4674e439f48ba5f31a3a`.

## Results

Three stable rounds of the new harness:

```text
passed: 18 builds, 28 executions, 0 failures
passed: 18 builds, 28 executions, 0 failures
passed: 18 builds, 28 executions, 0 failures
```

Total: **54 O0/O2 builds, 84 executions, 0 failures**. Every build was checked
as a static ELF with no `INTERP` and no `NEEDED`.

Three stable rounds of Root's existing `tests/native/channels/run_channels.py`
against this `std/channel`:

```text
passed: 18 builds, 46 executions, 0 failures
passed: 18 builds, 46 executions, 0 failures
passed: 18 builds, 46 executions, 0 failures
```

Total: **54 O0/O2 builds, 138 executions, 0 failures**.

The new harness also verifies:

- `deadline-spurious`: ten real cancel notifications wake a timed sender; it
  still times out on the original 600ms deadline and completes under 2.5s.
- registration/request interleaving, both with and without waiting for the
  waiter to enter; no stale subscription remains.
- cancel wakes 8 senders, 8 receivers, shared-token multiple waiters, and close
  wakes cancel waiters; `subscription_count` returns to zero after every join.
- 300 owned payloads with strings/Vecs pass exact count and checksum.
- `<-1` timeout exits 101 with `LANG_NATIVE_PANIC:v1:runtime-handle-misuse:`.
- repeated cancel and close do not deadlock or duplicate/drop items.

Full per-command results from the original check are in
`/tmp/con02/stable-waits*/summary.json` and
`/tmp/con02/stable-existing-*/summary.json`; that is an external check
directory and those logs are not part of the repository. The real runner and
its test matrix are committed in the Root tree as [run.py](run.py) and
[README.md](README.md), and the suite is registered as `native.channel_waits` in
[../CMakeLists.txt](../CMakeLists.txt). To rerun the new timeout/cancel cases:

```sh
python3 tests/native/channel-waits/run.py \
  --cli <woozyc> --root . --out /tmp/channel-waits \
  --ld-library-path <libxml2-legacy-lib-dir>
```

The existing `channel/1` suite remains available through
`tests/native/channels/run_channels.py`.
