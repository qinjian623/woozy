# Generic recursion component

This component builds a small driver against the live `app/wzcc` project,
resolve and Sema packages, then checks real project manifests at O0 and O2.

It covers:

- `tests/generics/fixtures/expanding-recursion`: the unchanged 48-level
  instantiation budget must produce `E_INSTANTIATION_LIMIT`, phase `type`,
  message `generic instantiation depth budget exceeded (48)`, span 71..101;
- a finite generic type chain and a same-argument recursive cache hit, which
  must both be accepted;
- the production source still contains the 4096 instance-count guard before
  the 48 depth guard.

Run with a stage-0 compiler:

```sh
python3 selfhost/tests/sema_recursion/run.py \
  --woozyc build/woozy-stdlib-native/woozyc
```

`--reference /path/to/cpp/woozyc` additionally requires reference exit 1 and
compares every serialized diagnostic field, including the exact primary path
and byte range. The driver does not serialize `has_primary`; this component
does not verify that flag. The full CLI JSON tests verify it separately.

The runner saves `report.json` from the start, after each command and on
failure, with argv, exact exit/status types and stdout/stderr digests. Before
and after each command it hashes the core Wz source tree, stdlib, driver,
fixtures and stage-0 compiler, and hashes the executable actually used. It
rejects changed inputs or executables. Evidence is retained under
`build/sema-recursion-runs/`; new hashes do not establish missing historical
hashes for an earlier run. Runtime/toolchain macro provenance is handled by
the bootstrap tooling, not discovered by this runner.

This is a component proof only. It does not build the 116-file CLI and does not
claim full corpus or self-hosting success.

Run the small Python assertion controls without a compiler build:

```sh
python3 selfhost/tests/sema_recursion/test_process_controls.py
```
