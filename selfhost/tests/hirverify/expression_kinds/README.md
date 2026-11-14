# HIR expression-kind component check

This focused driver reproduces the actual Wz `HIRVerifier` expression gate defect:
`verifier_expression` whitelisted `Await` (42), `TraitObjectInit` (43) and
`TraitDispatch` (44), but an additional `kind_id <= 41` range check rejected
them before the switch could run.  The C++ `expression()` helper lists the same
three kinds without a numeric range gate.

`driver.wz` builds real `hir.HirNode` values and calls the production
`verifier_expression`/`verifier_statement` helpers through a package-internal
forwarder injected by the runner (`probe.wz`).  It does not copy the predicate.
It checks:

- `Await`, `TraitObjectInit`, `TraitDispatch` are expressions with kind ids
  42/43/44;
- `ExpressionStatement` (16), `Noop` (19) and `Block` (15) are not expressions
  and are statements;
- foreign property/index/slice reads are expressions, foreign
  property/index/slice places are not, and a place is not a statement;
- a foreign read with no access kind remains an expression.

The same runner supports a pre-fix package directory.  In that mode the driver
must exit 1 with exactly the three positive checks failing:

```sh
python3 selfhost/tests/hirverify/expression_kinds/run.py \
  --woozyc /path/to/accepted-stage0/woozyc \
  --toolchain-root /path/to/toolchains \
  --work-dir /tmp/hirverify-expression-runs \
  --verifier-dir /tmp/pre-fix/hirverify \
  --expect-exit 1
```

After the fix, omit `--verifier-dir` and `--expect-exit`; every `CHECK` line is
`ok` and the summary is `SUMMARY failures=0`.  The runner builds only this O0
component driver.  It does not build the 116-source CLI, the C++ oracle/probe,
or run the complete corpus.  Every command has a finite deadline and uses the
shared `corpus._run_process_impl` process-group cleanup helper.  `results.json`
retains source/driver hashes, the built program hash, full stdout/stderr, and
each command record.

Check complete baseline/fixed output assertions without building a component:

```sh
python3 selfhost/tests/hirverify/expression_kinds/test_report_controls.py
```
