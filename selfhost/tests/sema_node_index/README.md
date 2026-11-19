# Sema NodeKey index component test

This private test stages the real `selfhost/src/wzcc/sema` product files
(all A/B/shared files except the public `check.wz` facade), appends
`node_index_test.wz` to that same `sema` package, and builds one native O2
test program with the fixed stage-0 compiler. It does not replace product
logic with Python or C++, and it does not add a public product API.

`node_index_test.wz` calls the real product code:

- `run_put_node_symbol` for sorted insertion;
- `definition`, `occurrence`, and `b_occurrence_raw` for all three readers;
- `node_symbol_position` for the shared private lower_bound;
- `initialize_checker` for a real resolved project, and `clone_checker` for
  clone ordering and storage independence.

The linear `node_index_oracle_*` functions in the test file are an explicitly
labelled test oracle copied from the pre-change rules. They are not product
code and are used only to compare the same keys, order, first-definition and
last-occurrence results, lookup results, and checksums.

Coverage includes empty/single tables, edge and tuple-missing keys, equal node
IDs across modules/files, increasing/decreasing/interleaved insertion,
duplicate first-definition/last-occurrence substitution, parenthesized
occurrence unwrapping, the original `missing sema definition` error, sorted
clone tables, and clone/original storage independence.

The component benchmark uses ascending keys for both the product and oracle
insertion paths, then two real product lookups per key (`definition` and
`b_occurrence_raw`) against two linear oracle lookups. It reports measured
monotonic nanoseconds, call counts, measured oracle comparison counts, result
checksums, architecture, and compile opt. Reverse/interleaved suffix moves
remain in the product and are tested for correctness, but the benchmark does
not claim overall O(M log M) insertion or a full compiler speedup.

Run with the fixed Python 3.12.15 interpreter:

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
MAIN="${WOOZY_WORK_DIR:-$(dirname "$(git rev-parse --show-toplevel)")}"
"${WOOZY_TOOLCHAIN_ROOT}/t10-python-cpu/venv/bin/python" \
  -B selfhost/tests/sema_node_index/run.py \
  --work-dir "$MAIN/checks/SH15-sema-node-index-lower-bound-20261114"
```

The runner verifies the fixed stage-0/helper/runtime/LLVM inputs, records
before/after source, test, stdlib, and tool hashes, retains command
stdout/stderr and JSON records, and writes `result.json` under the retained
run directory. It performs one frontend type-check preflight and one native O2
component build; it does not run the original 20s/1200s/21600s jobs.

The linear oracle updates comparison counters inside its timed loops. Its
ratios compare this instrumented test oracle with the product helpers; they
are not a matched before/after measurement of the old product implementation
or a measurement of whole-compiler speed. The runner requires exactly one
315/0 summary and positive values for all four elapsed-time fields.
