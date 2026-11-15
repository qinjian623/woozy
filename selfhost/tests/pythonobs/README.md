# Python emission API observer driver

This component is the test-only second entry point for
`tests/python/slice_source/run_slice_source.py`.  It is not a product CLI and
it does not call a C++ helper or a C++ frontend at runtime.

The driver is built once from Wz Source with the C++ seed compiler at `-O0`.
At runtime it:

1. creates `source.manager()`;
2. calls `project.load(ProjectOptions(manifest, stdlib, None), inout manager)`
   by default; test callers may pass `--std-binding BUNDLE`, in which case
   `std_source_root` is `None` and `std_binding_bundle` is the explicit bundle
   (the ordinary stdlib does not contain `std/py`);
3. selects `backend = "python"` after loading and before resolution;
4. calls `resolve.resolve_project`;
5. calls `sema.check_project(..., None, hirverify.VerifyMode.Backend)` and only
   uses the real `VerifiedHirModule` returned there;
6. takes `hirverify.snapshot`, finds the entry `main` using the resolved symbol
   identity/module (never a guessed `_f` number);
7. reads `resources.directory()/runtime/python/unicode18.py + "\n" +
   runtime/python/langrt.py` and calls
   `pythonemit.emit(verified, entry, Options(entrypoint: true), runtime)`;
8. uses the production `toolio.stage`/`cleanup`/`publish_pair` path from
   `cli/project.wz`: writes both files completely in a private staging
   directory, then publishes `langrt.py`/`program.py` as a pair.  A real
   `toolio.Error` is mapped to `E_INPUT_IO` and leaves the previous target
   files untouched; a write failure also cleans the private stage;
9. prints the existing JSON status line with an added `symbols` array.  Each
   element is `{symbol, source_name, generated}`: `source_name` comes from the
   same snapshot `HirModule.resolved.symbols`, and `generated` comes directly
   from `pythonemit.Output.symbols`.  No `_f`/`_s` name is guessed or parsed
   from generated Python.

## Optional real HIR observation (`--emit-hir`)

The product CLI publishes only `program.py`/`langrt.py`; it does not publish
`verified.hir`.  The original T10 runner nevertheless requires a real
`verified.hir` artifact and hashes it.  The test-only `--emit-hir` flag adds an
optional, additive field to the existing JSON status line:

```json
{"status":"ok","entry":...,"symbols":[...],"verified_hir":"..."}
```

`verified_hir` is the exact text of `hirdump.dump_module(module)` for the
`hirverify.snapshot(verified)` module already used for emission.  It is computed
before publishing, so a failed dump returns a diagnostic and publishes nothing.
Without the flag the stdout is unchanged, and the `langrt.py`/`program.py` pair
is still staged and published exactly as before.  The runner writes the
returned text as `verified.hir` itself; this driver adds no product CLI flag and
never fabricates HIR text.

A retained 55d `pythonobs` binary predates this flag.  Changing a Python runner
cannot give that binary the new interface, so it cannot be used as the real-HIR
candidate.  Using this interface requires a separately authorized build of this
component from the current Source.

Failure prints the same diagnostic document as the product CLI and exits
non-zero without creating an output program.  Resource paths are based on the
real executable directory, so the runner stages the candidate's frozen
`stdlib` and `runtime` next to the copied driver binary.

## Reproduce

The runner builds this project automatically when given
`--driver-compiler` (the C++ seed) and no `--api-driver`:

```sh
python3 tests/python/slice_source/run_slice_source.py \
  --candidate-cli /path/to/candidate/stage1/woozyc \
  --driver-compiler /path/to/cpp-seed/woozyc \
  --candidate-resources /path/to/candidate/stage1 \
  --root /path/to/compiler \
  --out /new/evidence/dir \
  --python /path/to/python3.12.15
```

Use `--api-driver /path/to/prebuilt/pythonobs` to reuse an already built
driver.  The runner still copies it into the new evidence directory and links
the candidate resources beside it, and it records the source/build/tool hashes.

## String-binding case

`tests/python/run_string_binding.py --candidate-cli ... --api-driver ...`
uses this driver as the candidate API entry for the test-only string binding
project.  It passes `--std-binding tests/python_bindings/string-aggregate/
binding.json` and reads the published `symbols` array.  The legacy
`--cli`/`--helper` CMake invocation remains the reference mode and does not
use this driver.  Neither mode changes the product CLI surface.
