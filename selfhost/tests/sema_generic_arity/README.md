# Generic/method type-argument arity component

This component builds a small driver against the live `app/wzcc` project,
resolve and Sema packages, then checks real project manifests at O0.

It covers the diagnostic-code mapping for explicit type-argument arity:

- the unchanged `tests/generics/indexed-calls/reject-generic-arity` fixture
  must report `E_TYPE_MISMATCH`, phase `type`, message
  `wrong number of generic arguments`, primary 63..77, no related/fixits/notes,
  matching C++;
- a correct generic call and a correct method call must be accepted;
- too many and too few generic type arguments, and too many method type
  arguments for inherent and trait methods, must report `E_TYPE_MISMATCH`;
- a generic function value with the wrong explicit type-argument count must
  report `E_TYPE_MISMATCH`;
- ordinary value-argument count errors must still report `E_CALL_ARGUMENT`
  with message `wrong number of arguments` (function and method calls).

The runner uses the production project loader, resolver and Sema; it does not
reimplement the arity checks. System-operation type-argument arity is not
changed by this component because C++ explicitly uses `E_CALL_ARGUMENT` there.

Run with a stage-0 compiler and the frozen C++ reference:

```sh
python3 selfhost/tests/sema_generic_arity/run.py \
  --woozyc /path/to/woozyc \
  --reference /path/to/cpp/woozyc \
  --toolchain-root /path/to/toolchains
```

`--toolchain-root` must contain `llvm-22.1.8/lib` and
`libxml2-legacy-2.13.9/usr/lib`; it can also be supplied through
`LANG_TOOLCHAIN_ROOT`. The native O0 build is intentionally the only variant
built by this component.

The runner saves `report.json` from the start, after each command and on
failure, with argv, exact exit/status types and stdout/stderr digests. Before
and after each command it hashes the core Wz source tree, stdlib, runtime,
driver, fixtures and the stage-0/reference executables, and hashes the
executable actually used. It rejects changed inputs or executables. Evidence is
retained under `build/sema-generic-arity-runs/`.

This is a component proof only. It does not build the 116-file CLI, does not
rerun the corpus, and does not claim that an already-built stage-1/2/3 binary
contains this fix.

Reference JSON must have status `ok` with exit 0, or `error` with exit 1.
The comparison covers diagnostic code, phase, severity, message, path, primary
byte span and related/fixit/note counts. It does not compare `has_primary`, line
or UTF-16 positions, or the contents of those three arrays.

Run the small reference-result controls without compiling the component:

```sh
python3 selfhost/tests/sema_generic_arity/test_process_controls.py
```
