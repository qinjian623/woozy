# stdlib-io-errors runner

`run_io_errors.py` keeps the original public invocation:

```sh
python run_io_errors.py --cli ROLE_CLI --root SOURCE_ROOT --out OUT
```

It builds the O0 and O2 programs with the original 20-second per-process
budgets, runs each published program in its own private cwd, and checks the
original empty stdout/stderr, exit 0, `destination` bytes
`636f7069656400f09f9880`, and absent `absent-destination`.

For strict records add a role:

```sh
python run_io_errors.py --cli ROLE_CLI --root SOURCE_ROOT --out OUT \
  --role candidate --record \
  --expect-cli-sha256 CANDIDATE_SHA \
  --watch CANDIDATE_SOURCE_116 --watch CANDIDATE_RESOURCES
```

Reference runs may add `--helper` and `--expect-helper-sha256`; a candidate
run rejects `--helper`. `--record` also requires the fixed LLVM, strace, and
libxml paths (overridable with `--toolchain`, `--strace`, `--libxml`).

Every command is written immediately under `OUT/commands/` with the exact
argv/cwd/budget/elapsed, native int exit, strict boolean flags, raw
stdout/stderr and SHA-256, and the exec-only trace. The role/opt build and
program cwds stay under `OUT/builds/` and `OUT/programs/`; fixture bytes are
recorded before exit. Non-empty record directories are refused and failed
commands are not retried.

Without `--record`, reusing the same `--out` keeps earlier command records,
build products, and program working directories. Each invocation writes
`OUT/commands/`, `OUT/builds/`, and `OUT/programs/` entries to unused names (or
reuses an existing empty directory) while `OUT/io-errors-results.json` remains
at the original top-level report path. `--record` still refuses a non-empty
`--out`.

`--self-test` runs only pure controls (strict process fields, timeout/signal,
JSON NaN/Infinity, destination/absent-destination, and exec allowlist). A
normal run executes the same controls first and records any control failure in
the same report.
