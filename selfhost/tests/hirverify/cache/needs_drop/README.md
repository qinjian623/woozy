# HIR verifier needs_drop bool-cache component test

This private test stages the real `selfhost/src/wzcc` package tree, copies the
real `hirverify` package, and appends only `needs_drop_probe.wz` to that same
package.  It builds one native O2 component with the fixed stage-0 compiler.
It does not add a public product probe API and does not replace the product
classifier with a Python model.

`needs_drop_probe.wz` calls the production `make_verifier` and
`classify_needs_drop` plus the real `valueops.classify_value` path.  It checks:

- a successful `false` result is stored separately from `known` and a later
  query still returns `false`;
- a successful `true` result is cached;
- ids `0`, `size+1` and `u64.max` repeat the original classifier error text,
  do not resize either bool vector and do not set `known`;
- the same numeric TypeId in two independently constructed modules has
  different results, and alternating two Verifiers does not share cache bits;
- a value copy of one Verifier with the same module sees the same cache
  vectors (the production constant callback probe pattern);
- replacing the original module's TypeContext and making a new Verifier
  recomputes instead of reusing the old Verifier's cache.

`driver.wz` loads the checked-in concrete-counter fixture and a generated
40-function project through `project.load -> resolve -> sema.check_types`, then:

- preserves the previous NodeRef key identity control;
- verifies both real modules repeatedly and alternately;
- warms each module with two `verify_types` calls, then requires a cloned
  missing Source node, an out-of-range TypeId and an out-of-range ScopeId to
  be rejected with `E_IR_INVALID`;
- runs the cache probe in the same binary.

The benchmark in the probe uses the same unchanged module, ids `1..32`, 64
passes and three trials.  A cached Verifier is warmed before the timed cached
pass; the timed uncached pass calls the actual
`valueops.classify_value(...).needs_drop` for the same ids.  It checks equal
results and positive monotonic nanoseconds.  This is only a component
repeated-query comparison, not a whole-compiler before/after claim.

The runner freezes source, staged modules, test files, stdlib, fixtures, the
stage-0 seed, helper, runtimes, LLVM tools, strace, libxml2, the actual Python
interpreter, `corpus.py` and the runner itself before the first command.  It
builds the component once at O2, runs it once for correctness and the
benchmark, then does a separate tiny trace-only execution.  The trace uses
`strace -e trace=execve,execveat` and must contain only the already-built
program; the benchmark is not measured under strace.

Run with the fixed Python 3.12.15 interpreter:

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
MAIN="${WOOZY_WORK_DIR:-$(dirname "$(git rev-parse --show-toplevel)")}"
"${WOOZY_TOOLCHAIN_ROOT}/t10-python-cpu/venv/bin/python" \
  -B selfhost/tests/hirverify/cache/needs_drop/run.py \
  --work-dir "$MAIN/checks/SH15-hir-needs-drop-cache-20261114"
```

Every command has a finite deadline and uses the shared
`selfhost/corpus._run_process_impl` process-group cleanup helper.  Raw
stdout/stderr and JSON records are retained before a failure is raised.

Root review: strict JSON rejects non-finite values; exec traces use the shared corpus audit for both execve and execveat. Run test_controls.py for pure parser controls. Original failed checks/builds/deep benchmark timeout remain separate historical records.
