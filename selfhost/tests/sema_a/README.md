# Sema A-side tests

This directory owns only the A-side checker algorithms:

- `a_helpers.wz` — copied-record/table helpers and trait identity equality.
- `a_types.wz` — CPP `check.cpp` 208-413, 507-1464, 2865-3020, 3269-3457,
  and the declaration-side foreign wrappers 3623-3662.
- `a_declarations.wz` — CPP 5496-5893, `duplicate_symbol`, and the ordered
  `declarations` pass.
- `a_instantiate.wz` — CPP 6189-6684 (`constraint_method_arguments`,
  `transform_node`, `instantiate`) plus generic instance cloning.

`run.py` stages the production shared files plus `a_helpers.wz`,
`a_types.wz` and `a_declarations.wz`, then runs 47 checks at O0 and O2. It
deliberately excludes `a_instantiate.wz`: `instantiate` calls B's `annotate`
and `flow`, which are not present in this one-sided package. It also makes no
claim about a complete Checker, Root facade or self-hosting.

```sh
python3 selfhost/tests/sema_a/run.py \
  --woozyc /absolute/path/to/woozyc
```

The runner writes its temporary project/build output under
`build/sema-a-runs/` by default and sets `TMPDIR` there. `--work-dir` can
retain a run elsewhere. Every subprocess uses the corpus runner's finite
process-group deadline and cleanup helper (`selfhost/corpus.py`); each command
retains `label.stdout`, `label.stderr` and `label.json` with its real exit,
timeout and cleanup fields. `--timeout` rejects NaN, infinity, zero and
negative values before staging or running anything.

The checks cover real parser/resolver input, nominal shape/fields, trait
declaration/prototype identity, trait-application arity, inherent and trait
impl method registration, concrete `Self` substitution versus the raw trait
`Self`, generic inference/conflict/finish, `parse_type`, unique method
selection, trait constraints and rejection of an unsatisfied bound, inout
graph acceptance/refusal, and complete Weak target validation.

Three review regressions are also covered explicitly:

- static `T.method` source/move helpers accept non-Self-first inout and move
  parameters, while value-form bounded methods still require the raw trait
  `Self` first parameter;
- declarations restore both an empty Root environment and a pre-existing `Saved` environment around distinct `Holder[T]` and
  `Probe[P]`, plus the minimal empty `Holder[T]` environment;
- an invalid `[T i32]` bound reports `E_TYPE_MISMATCH` at the whole
  `GenericParameter` (`primary` bytes 20:25).

See `FUNCTION_MAP.md` for the per-function CPP line map.

Full Checker type checking and `instantiate`/`transform_node` execution remain
`not_run` until B and the Root facade are present. A type-check-only staging
that omits only the two documented B calls (`annotate`, `flow`) is recorded in
`checks/sema-a-sh06i-20261113/`; it is not a runtime or whole-Checker result.

Root integration adds the explicit move parameter in the static-source fixture and a discriminating saved-environment regression. The repaired helper project passed check/build/run at O0 and O2, with 47 checks each; full Checker and instantiate runtime remain untested.
