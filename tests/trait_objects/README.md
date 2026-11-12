# Dynamic trait object acceptance

`cases.json` lists the original and review-correction fixtures. Positive cases
are checked with both frontends, executed on Python 3.12 and, when LLVM is
enabled, built at Native O0 and O2. Native outputs are verified as
self-contained ELF64 x86_64 executables with no `PT_INTERP` and no strong
undefined symbols. Rejection cases declare their required diagnostic code and
must not leave output on either built backend.

`dyn-cycle` is run through `observe_dyn_cycle.py`, which disables CPython GC and
forbids `gc.collect`, then verifies that the pure-Woozy `Vec[dyn Trait]` cycle
is retired by the language collector without duplicate Vec releases or live
DynObject wrappers.

```sh
python3 tests/trait_objects/run_trait_cases.py \
  --cli build/llvm-clang/woozyc \
  --root . \
  --out /tmp/trait-results \
  --python /path/to/python3.12 \
  --native
```

CTest target: `trait_objects.cases`.
