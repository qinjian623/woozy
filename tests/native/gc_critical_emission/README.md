# GC critical emission checks (SH15)

This directory contains the minimal regression for the fresh-Vec prefix and
strong-edge store/replace emission reduction.  It does not build the CPP
helper or the Wz Nativeobs component; those are Root-supplied test components.

## What is asserted

`run_gc_critical_emission.py` reads the real NIR dump and pre-optimization LLVM
from two independent emitters:

- patched CPP: `lang_native_helper --project ... --opt 0|2 ...`
- patched Wz: the sealed Nativeobs entry with `--project ... --stdlib ...`

It maps each fixture declaration to its real NIR `SymbolId` through NIR source
spans, then to `@lang.fn.<SymbolId>`.  A missing target, zero matched target,
empty NIR/LLVM, or an unreadable target is a failure, not a pass.

### Fresh Vec prefix

Per `vec_init`, the ``lang_vec_new_v1`` result is the object identity.  Its
data field 5 and initialized-count field 2 are followed through the actual
GEP/load chain, and element copies must target that object's data.  The prefix
ends at a registration of that same object, after every required copy and
initialized-count store.  Registering another object is rejected.  An
unregistered/empty prefix ends at the next real slot/return publication (a
critical pair that wraps a real slot store), not at an arbitrary next call; an
extra constructor pair with no such publication is part of the checked prefix
and is rejected.  A legitimate following slot/return guard is not rejected.

### Local store and indirect replace

The NIR store slot ID is mapped to its LLVM alloca. A synchronous local
publication has no guard, even for a strong-edge value. The owner is still
dropped through that slot after publication.

For `replace_indirect`, follow the NIR address to its base and verify the LLVM
publication at the corresponding address. A strong-edge inout destination
or a projected Vec element field has exactly one pair around the publication.
A field rooted in a synchronous local alloca has no pair. The old value must
be loaded from the destination and saved to a distinct scratch address, then
dropped from scratch exactly once after publication. A pair around an
unrelated store or an active flag fails. The non-owning `store_indirect`
control has no guard. An async-owned slot must stay guarded because coroutine
lowering can move it into the traced frame.

The IR checks identify the publication address and old-value scratch; they do
not reconstruct the complete new-value SSA graph from NIR.  An unrelated RHS
can pass this structural check.  The production diff preserves RHS generation;
value correctness still requires the real program assertions and existing
behavior tests.  Both LLVM printers are supported: Wz quoted globals and `%pN`
parameters, CPP aligned memcpy operands, and their different slot/scratch order.

### Real execution

The same fixture is built by the role public CLI and executed at O0/O2.  The
unchanged observer from `tests/native/language_gc_source/observe_gc.cpp` is
linked with **both** the patched-CPP and patched-Wz `before.ll`, and both
observer binaries must exit 0 with zero live Vecs/payload and balanced
ALLOC/FREE.  There is no IR-only or skipped-execution success path.

Every command goes through the existing bounded
`selfhost/corpus.py::_run_process_impl` transport.  Its complete record is
saved before any assertion: `argv`, `cwd`, the actual timeout, trace label,
raw stdout/stderr as hex plus SHA-256, `exit`, and the three exact booleans
(`timed_out`, `cleanup_complete`, `abandoned_pipes`).  `exit` must be an exact
`int` (not `False`), a launch failure or timeout cannot pass, and partial bytes
are always JSON-serialisable.  `commands.json` is written through a
same-directory `.pending` file and `os.replace` after each command, including
every `readelf` call and failed build/link record.  `summary.json` uses the
same replace discipline.

The output directory must be new or truly empty.  An existing non-empty
directory is refused before any external command or write; old summaries,
command logs and raw evidence are never deleted or overwritten.  No
`shutil.rmtree` path exists.

The broader GC lifecycle suites remain separate and authoritative:

- `tests/native/gc_activation/run_gc_activation.py` for graph activation,
  polls, and lifecycle;
- `tests/native/language_gc_source/run_language_gc_source.py` for self/mutual/
  closure/weak cycles, live roots, 8192 pressure, 2 MiB peak, and ownership;
- `tests/native/slice_source/`, `tests/native/weak_source/`, and the C cleanup
  runners for Slice/Weak/C-resource behavior and concurrent handshakes.

The activation and lifecycle suites test real heap edges directly. A runtime
whole-element Vec setter provides its own critical region. Incidental guards
around local owner slots do not demonstrate protection of that update.

## Run (after Root authorizes the components)

```sh
python3 -B tests/native/gc_critical_emission/run_gc_critical_emission.py \
  --helper /path/to/patched-cpp/native/lang_native_helper \
  --wz-nativeobs /path/to/patched-wz/nativeobs-driver \
  --cli /path/to/role/woozyc \
  --runtime /path/to/role/runtime/liblang_native_runtime.a \
  --clang /path/to/llvm/bin/clang \
  --readelf /path/to/llvm/bin/llvm-readelf \
  --root /path/to/repo \
  --out /path/to/new-or-empty/evidence-dir
```

`--stdlib` defaults to `<root>/stdlib`; `--observer` defaults to the existing
`tests/native/language_gc_source/observe_gc.cpp`.  `--opt all` (default) checks
O0 and O2.  The output directory is evidence only; the runner does not build a
component or call the C++ helper from the Wz path.

## Pure controls

```sh
python3 -B tests/native/gc_critical_emission/pure_controls.py
```

The controls install spawn/exec/socket guards before importing the runner and
exercise the actual assertion functions with small textual NIR/LLVM fixtures.
They cover the original 26 controls plus the eight independent
counterexamples: publication outside the pair, dropping the new owner, no old
load/save, an empty nonregistered extra pair, registering another object, a
boolean exit, timeout partial `0xff` bytes, and existing-out deletion.  New
actual-main controls replace only the bounded-process seam and verify
`bool False`, timeout partial bytes, build/link/launch failure, readelf record,
the pre-process refusal of a non-empty output directory, and preservation of
the `clang++`/`llvm-readelf` invocation names through real soft links. LLVM
chooses its driver and output format from these names.
`external_launch_attempts` must be `[]`.

The Wz Nativeobs result covers the current production emission functions
through the test driver. The public CLI build uses the supplied `--cli`
binary. Nativeobs does not exercise the Wz CLI's build orchestration.
