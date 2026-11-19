# HIR source-node type index

`run.py` builds two O0 component programs with the supplied `--woozyc`.
The index fixture is compiled inside the staged `hirverify` package, so it
calls the production private functions directly. It checks repeated and
multiple types, the complete module/file/node key, a forced collision chain,
and map growth. A forced collision also checks that the original entry is
found behind a nonmatching head and retains both types. The second program
reuses existing HIR fixtures: it checks malformed source/child rejection,
corrupted throw effect sets through `verify_types`, and erased throw source
constructs by directly calling `source_effect_nodes` with a test-only wrapper. The test-only hook is added only to the staged copy.

Run from any directory, passing a new work directory:

```sh
python3 -B selfhost/tests/hirverify/effect_index/run.py \
  --woozyc /path/to/woozyc --toolchain-root /path/to/toolchains \
  --work-dir /path/to/new-work-directory
```

`--source-root` selects an alternate `wzcc` package directory and
`--stdlib-root` selects its standard library. `--repo-root` selects the
repository containing the shared test runner and fixtures. The work directory
retains command logs, exit status, source hashes and the report.
