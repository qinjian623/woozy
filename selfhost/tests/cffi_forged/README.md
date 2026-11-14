# C-FFI forged HIR/NativeIR Wz component

Test-only component for the eight original forged-metadata rejection
assertions in `tests/native/c_ffi/forged_test.cpp`. It does **not** migrate the
C-FFI runner, enable any catalog case, build/link/run the forged C target, or
build the full product CLI.

## What it runs

`main.wz` imports the real production modules and executes, in-process:

```text
project.load
  -> resolve.resolve_project
  -> sema.check_project(..., hirverify.VerifyMode.Backend)
  -> hirverify.snapshot / hirverify.verify(Backend)
  -> nativelower.lower
  -> nativeverify.snapshot / nativeverify.verify
```

The input is the unchanged
`tests/native/c_ffi/forged-project/lang.toml`, its `src/main.wz`, and the
unchanged `tests/native/c_ffi/bindings/fixture.json`. The component first
requires the legal HIR and NativeIR to verify and to contain C foreign calls,
C field reads and a C pointer write. It then makes these eight independent
mutations, each from a fresh deep snapshot:

| Layer | Mutation name | Change |
| --- | --- | --- |
| HIR | `hir_c_callable` | first C call `c_callable = Some(999999)` |
| HIR | `hir_signature_type_id` | first C call `signature = 999999` |
| HIR | `hir_arg_indices` | swap first argument source/parameter indices, then `parameter_index = 7` |
| HIR | `hir_c_field` | first `CFieldRead.field = 999999` |
| NativeIR | `native_c_callable` | first `foreign_call.c_callable = Some(999999)` |
| NativeIR | `native_origin_foreign_call` | first `foreign_call.origin = None` |
| NativeIR | `native_c_field` | first `c_field_read.c_field = 999999` |
| NativeIR | `native_origin_c_ptr_write` | first `c_ptr_write.origin = None` |

Each mutation must produce `Failed` + `Internal` + no value + a non-empty
diagnostic list whose first code is `E_IR_INVALID`. A missing mutation target
is a failure, never an expected rejection. After every case the untouched legal
snapshot is verified again, proving that the shared Wz `Vec` handles did not
alias the mutant into the next case.

## Runner

`run.py` performs one fixed O2 build of this private component with the pinned
C++ seed, then one run of the component with a 300-second budget. It records:

* the pinned Source116 map (`ed1c...`, 116 `.wz` files), the private component
  map and its file count (117), the private `main.wz` and `lang.toml`;
* the unchanged forged project, binding, C++ test and contract as provenance;
* seed, helper, native/hosted runtime, fixed LLVM tools, strace, actual Python,
  `selfhost/corpus.py`, this runner, libxml, and the 37-file stdlib map;
* raw stdout/stderr bytes and SHA-256, argv, cwd, finite timeout, exact native
  int/bool process flags, and strace exec records;
* before/after resource hashes, build artifacts, and the parsed eight `CASE`
  lines, both coverage lines, both legal-baseline lines, and the exact
  `TOTAL 8passed 0failed` line.

The component run's exact exec allowlist contains only the component itself.
The build allowlist contains the fixed C++ seed, its matching helper, and the
fixed LLVM tools. Existing C++ forged output is provenance only and is not
reused as a result.

Pure negative controls (no external command) exercise the summary parser and
strict process checks:

```sh
python3 selfhost/tests/cffi_forged/run.py --self-test-controls
```

They cover a valid 8/8 summary, 0/8 and 1/8 summaries, duplicate totals and
duplicate case names, a missing mutation target, timeout and signal records,
non-bool flags, non-empty stderr, unknown lines, missing coverage, and a fake
total without case lines.

## Usage

```sh
python3 selfhost/tests/cffi_forged/run.py \
  --work-dir /path/to/new/evidence-dir \
  --report /path/to/new/report.json
```

Defaults pin the source root to this checkout, the forged project and binding
to the unchanged repository files, and the seed/runtime/stdlib/toolchain/strace
to the fixed SH15 resources. The runner refuses an existing work directory or
non-empty report path. A failed build or failed analysis stops immediately with
a nonzero exit; there is no retry or budget change.

## Scope and non-claims

This component claims only the eight forged HIR/NativeIR rejection assertions
running on the actual Wz compiler modules. It does **not** claim the full
15-case C-FFI runner, a complete selfhost pass, C program interoperability, or
that the forged C target was built. It does not modify production Source,
fixtures, bindings, the original C++ test, the original runner, gold data,
assertions, CMake, the catalog, PLAN or STATE.

Root review: each HIR/NativeIR coverage line must occur exactly once; duplicate or replacement coverage is rejected by the pure controls. The original Wz driver and its saved 8-case result were not rebuilt.
