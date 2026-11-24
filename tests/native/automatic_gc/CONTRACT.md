# Native automatic-GC tests — contract

Status: **tests only, current Native GC not integrated**.
Operation: `woozy-native-auto-gc-tests-correction-20261112`.
This directory does not implement collection and cannot mark MEM02 done. It
pins what a future pure-Woozy collector must demonstrate on real Native
programs and records the current reference-counting leak as an expected
failure.

## Scope and non-goals

In scope:

- Real `.wz` projects built by the Native CLI at `--opt 0` and `--opt 2`.
- Pure-Woozy strong cycles from the existing surface only: `struct`, `enum`,
  `Vec[T]`, indexed/field assignment, loops with explicit `continue`, typed
  `throws`, `catch` and `defer`.
- Exact observation through existing `lang_alloc_v1` / `lang_free_v1` and the
  weak `lang_vec_observe_v1` stream.
- Root survival, dead-cycle reclamation, dead-cycle-to-live-edge reference
  count decrease with the live object still readable, and bounded live Vec
  payload under sustained allocation pressure.
- Static ELF checks (`no INTERP`, `no NEEDED`) for the real program and the
  observation binary.

Out of scope:

- `Weak` downgrade/upgrade and liveness (MEM01).
- Capturing closures and closure environments (LANG01 is planned).
- Concurrent collection, thread stop-the-world, runtime blocking-call SAFE,
  threads, fork, channels and foreign Python/C cycles.
- Source-level `collect`, `gc`, `retain`, `release`, `Weak` or closure APIs.
  None is invented or called.
- RSS or page-cache behavior as evidence. Only exact allocator/Vec events count.
- A source guarantee that collection happens in every scope or before a
  particular marker. The Root Native policy is the reference:
  initial pressure budget 128 successful registrations, then
  `max(128, 2 * live)`; requests are handled at stable function/loop heads;
  a normal single-thread `main` return performs one final collection; Panic
  and failed thread stop are not language guarantees.
- Production pause bounds, throughput or a complete MEM02 acceptance.

## Object identity and observer

`observe_gc.cpp` is a test-only freestanding shim:

```text
CONFIG:vec_header_bytes:<n>       before the first allocation
ALLOC:0x<user pointer>:<bytes>    successful lang_alloc_v1 request
FREE:0x<user pointer>             entry to lang_free_v1
VEC:<event>:0x<object>:<detail>   runtime lang_vec_observe_v1 stream
```

- The shim allocates nothing (fixed stack buffer, raw `write(2)`), never
  retains/releases the observed object and never writes its fields. Its own
  tracking cannot recurse through the allocator it observes.
- `VEC` events use the current live generation of an address.
- The runner assigns a **generation on every ALLOC**: identity is
  `(address, generation)`. A legal FREE followed by a new ALLOC at the same
  address must not overwrite the earlier generation's length/ref/free history.
  This is exercised by an address-reuse synthetic counterexample.
- `CONFIG` reports the runtime Vec header size (currently 80 bytes, aligned
  from `sizeof(LangVec)`); the runner derives a Vec's payload as
  `ALLOC size - header`. Raw collector metadata allocations have no `VEC:1`
  and are excluded from the application payload budget and from live-Vec free
  ordering. They are still counted as blocks and may be freed after the live
  Vec.
- A Vec allocation must satisfy `size >= header`, empty Vecs have exactly the
  header size, and non-empty payloads are an exact multiple of the length.
  Violations are reported as `inconclusive_observer_abi`, not as a GC leak.

Only Vec objects and their backing blocks are measured. Retain/release call
counts are not pinned; only the specific edge transition is checked.

## Target requirements per case

| case | source shape | target evidence |
| --- | --- | --- |
| `control-empty` | no owning values | 0 Vec allocations, 0 live Vecs, 0 payload bytes |
| `control-acyclic` | nested tree, no cycle | all 4 Vecs freed; rooted reads/writes preserved |
| `self-cycle` | Vec element points at its container | cycle Vec freed; non-empty Vec freed; root reads preserved |
| `duplicate-edges` | same Vec stored twice in its own element | both edges tolerated; cycle freed exactly once |
| `two-cycle` | two Vecs point at each other | both cycles freed |
| `enum-nested-cycle` | struct field holds enum payload holding the cycle edge | `struct -> enum -> Vec` edge traced and freed |
| `dead-cycle-live` | dead cycle holds an edge to a still-rooted Vec; 8192-round pressure phase between markers | live payload peak `<= 2 MiB`; dead cycle freed; live Vec count reaches 1 and is not freed early; live root is read throughout/after pressure; final 0 |
| `loop-continue` | 8192 rounds, each ~1 KiB dead self-cycle + payload, explicit `continue`, no collect | live Vec payload peak `<= 2 MiB`; all source Vecs freed at final collection; fixed root checksum correct |
| `typed-throw-defer` | dead cycle created before a typed throw; `defer` and `catch` run | cycle freed; defer/error log and source exit value unchanged |

### Pressure policy and budget

`loop-continue` creates 8192 rounds. Each round allocates:

- a `Vec[Node]` (length 1) whose element points back at the container
  (`self_ref`), forming the cycle;
- a `Vec[i64]` of length 128 (1024 payload bytes) owned by that element;
- two transient empty Vecs that are replaced and freed.

The fixed live root is a `Vec[i64]` of length 8, read every round, checked at
every 1024th round and after the loop, with a final checksum. There is no
collect call and no source-level GC trigger.

The target measures **live Vec payload bytes** over the whole event stream:
`peak_live_payload_bytes <= 2 * 1024 * 1024`, `final_live_payload_bytes == 0`,
and all source Vecs freed. It does **not** require collection in every round,
per eight objects, or before any marker. A no-collection baseline leaves about
8.19 MiB of payload plus headers, far above the budget.

`dead-cycle-live` runs the same 8192-round pressure between `marker_a` and
`marker_b`, while reading a still-rooted length-9 `Vec[i64]`. The watched dead
cycle is a length-7 `Vec[Node]` holding the edge to the live root. A normal
single-thread final collection at `main` return may reclaim it; the target
requires:

- the dead cycle and live root are freed (final cleanup);
- the dead cycle's Vec free precedes the live root's Vec free;
- the live root has a release to count 1 (edge destroyed) and then 0/free;
- the live root is the **last Vec** freed (raw metadata may follow);
- the live root is not freed before the post-pressure marker/read, and the
  source returns the checked value;
- the payload peak is `<= 2 MiB`.

No collection before a particular marker is required. "Still readable" is
proven by the source reading the live root throughout the pressure phase and
after it; a premature free breaks the program's exit value. The event stream
shows the edge release and final Vec ordering.

## Modes and exit codes

Default target mode:

- Builds/runs every selected case at selected opts.
- Compares actual metrics with `cases.json` target and applies named checks.
- On the current no-GC runtime it returns **1**, `status=failed`,
  `gc_test_result=not_implemented`, `expected_failure=true`,
  `not_implemented=true`, with exact expected/actual per failed check.
- A build/tool/observer failure returns **2** and an `inconclusive_*` result;
  it is not evidence that GC is missing.
- A control-only selection returns **0** with
  `gc_test_result=control_only_passed`, `coverage.cycles=false`,
  `feature_claim=none`. It is not a GC pass.
- A selected cycle subset that passes returns `selected_scope_passed`.
- The full 9-case set at O0/O2 that passes returns `full_suite_passed`; this is
  the full single-mutator cycle suite, still not MEM02 complete
  (`mem02_complete=false`).

`--baseline-only`:

- Records the existing leak. A matching full baseline returns **0** with
  `status=expected_failure`, `gc_test_result=not_implemented`,
  `expected_failure=true`, `not_implemented=true`; it is never a GC pass.
- A mismatch returns **1** (`baseline_mismatch`); do not weaken the target.
- Controls-only baseline is `control_only_passed`, not `expected_failure`.

Result taxonomy (no ambiguous `automatic_gc_implemented` boolean):

- `gc_test_result`: `not_run`, `inconclusive_tool_pin`,
  `inconclusive_tool_or_build`, `inconclusive_observer_abi`,
  `control_only_passed`, `selected_scope_passed`, `full_suite_passed`,
  `not_implemented`, `failed`, `baseline_mismatch`.
- `suite_scope`: full vs selected case ids and opt levels.
- `coverage`: controls, cycles, pressure, full-case-set, opt levels and the
  explicit `not_covered` list.
- `feature_claim`: `none`, `cycles_not_reclaimed`,
  `single_mutator_selected_scope_cycles_passed`, or
  `single_mutator_full_suite_cycles_passed`.
- `mem02_complete`: always `false` from this suite.

## Reference and candidate roles

`--role reference` is the default and keeps the original route: the public
CLI builds and runs the program, the supplied `--helper` emits LLVM, and the
observer links that LLVM. `--role candidate` never runs the helper. It links
the unchanged `observe_gc.cpp` from the `cli/program.o` published by the same
successful public CLI build, with the role's `--runtime`, fixed `--clang` and
`--lld`:

```text
clang -O<0|2> -std=c++20 <case/O*/cli/program.o> observe_gc.cpp
      <runtime> -o observe -nostdlib -static --ld-path=<ld.lld>
      -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1
```

Candidate mode requires `--strace FILE` and `--lld FILE`; `--helper` is not
required (a wrapper may still pass it, but it is never executed),
`--expect-helper-sha256` is rejected, and the helper path is not pinned or
checked as embedded in the CLI. The candidate link preserves the absolute
alias in argv (`Path.absolute()`), including `--ld-path=<.../ld.lld>`; only
the SHA-256 and exec-trace allowlist canonicalize symlinks. It never substitutes
a generic `lld` target. Candidate builds, links, readelf and program runs are
traced with separate exec allowlists: the CLI plus its fixed LLVM toolchain
for the build, the selected clang/LLD for the observer link, the built program
for its run, the observer only for its run, and readelf only for ELF checks.
The `clang --version` check uses the same bounded command layer,
raw output/SHA records, and process JSON as case commands; candidate version
tracing allows only clang. An unexpected exec, missing trace, timeout, signal,
non-int exit, non-bool cleanup flag, abandoned pipe, missing `program.o`, or
build failure stops that case before a later observer can mask it.

The full nine cases at O0/O2 are 18 runs. Reference uses 7 commands per run
(126 case commands); candidate uses 6 (108) because it has no helper emit.
Each role also performs one `clang --version` check. Reference additionally
runs one bounded `woozyc --__resource-paths` query, so the full planned counts
are 128 and 109 (237 for both), excluding preparation. The `command_plan` field
reports these pre-run expectations; it is not backfilled from a driver exit
status.

## Tool and input pinning

- `--expect-cli-sha256`, `--expect-helper-sha256` and
  `--expect-runtime-sha256` pin the exact build products. Without them the
  hashes observed at run start are the pin.
- Build JSON is parsed with strict finite JSON. NaN, Infinity and exponent
  overflow such as `1e999` are rejected, not stringified. `summary.json`,
  compact reports and process JSON are written with `allow_nan=False` and
  recursive finite-number checks.
- In reference mode the runner asks the CLI for its bounded
  `--__resource-paths` document and requires the resolved helper and native
  runtime to be the supplied `--helper` and `--runtime`. The current CLI
  resolves resources relative to its executable; it intentionally does not
  embed an absolute helper path. The query itself is recorded as a bounded
  process with raw streams and hashes. A different resolved resource is
  `inconclusive_tool_pin`, not a GC result. Candidate mode does not check the
  helper and pins the CLI, runtime, clang, readelf, strace and LLD.
- CLI/helper/runtime (reference) or CLI/runtime/strace/LLD (candidate) are
  re-hashed after the run. Any change makes the run `inconclusive_tool_pin`.
  Root may rebuild the CLI while running Python GC tests; the helper/runtime
  pin is what matters for Native evidence. A copied CLI with the same
  executable-relative resource layout can be used to keep the CLI stable.
- Every command has a finite timeout (`--timeout`, per-case override for the
  pressure cases). Tool output, exit codes, source hashes and the pin are in
  the external full summary.
- Before the run, the runner snapshots the selected CLI resource stdlib and
  runtime, the linked runtime, root stdlib, observer source, all fixtures, and
  tools. The Vec header is `HERE.parents[2]/runtime/native/vec.hpp`, the actual
  file selected by `observe_gc.cpp`'s relative include; it is independent of
  `--root`. The snapshot and both before/after path displays use that same
  actual header. After the run, it recomputes the same set from the stored
  before paths. Any change or missing required input makes the run
  `inconclusive_tool_pin` with a non-zero exit; it is not merely recorded as
  `false` while still passing. Missing required inputs are never silently
  skipped.

## Output hygiene

- Candidate mode still requires a new or empty `--out`; a non-empty directory
  is refused. Default/reference mode preserves the legacy repeatable entry:
  a non-empty `--out` gets a unique `run-*` child, old materials are kept, and
  the report records both `requested_out` and the actual `output_dir`. This is
  the existing NativeSlice pattern; it is not claimed to be byte-identical
  default behavior. A `--compact-out` requested inside that legacy `--out` is
  remapped into the new child run directory.
- The full `summary.json` is written under the actual `--out`. Every command's
  stdout/stderr bytes are atomically saved under `OUT/raw/...` (or
  `OUT/tool/raw/...` for the version call) with sizes and SHA-256. Candidate
  exec traces are saved under `OUT/<case>/O*/traces/` or
  `OUT/tool/traces/`.
- Immediately after the transport returns, before trace audit or any
  assertion, each command writes `OUT/<case>/O*/process/<name>.json` (or
  `OUT/tool/process/clang-version.json`) with argv, timeout budget, exact
  four process fields, raw paths/SHA and trace status. The same JSON is
  updated after trace audit. Private raw text is never copied into JSON, so
  an interrupted later command still leaves earlier process records readable.
- The candidate tree keeps only source, `cases.json`, this contract,
  `BASELINE.md`, and a compact baseline report (`baseline/pins.json`) with
  tool/source hashes and per-case metrics. Raw logs and full summaries are not
  committed.

## Current expected state and limits

`BASELINE.md` records the current plain-ARC runtime: O0/O2 keep one dead cycle
and its payload per pressure round, so the pressure baseline is about 8.58 MB
of live Vec payload (over the 2 MiB budget), the two controls pass, the small
cycle cases keep their expected live objects, and all programs/observation
binaries are static ELF without `INTERP`/`NEEDED`.

Not verified here: `Weak`, capturing closures, concurrent collection, thread
stop/fork, foreign cycles, or the complete MEM02 integration. A full-suite pass
in this directory would be strong evidence for the single-mutator Native cycle
path only; it would not by itself mark MEM02 done.

## Running

```sh
# Default target checks (currently exits 1: not implemented)
tests/native/automatic_gc/run.sh /tmp/woozy-automatic-gc-target \
  --compact-out /tmp/woozy-automatic-gc-target/compact.json

# Record the existing leak (exits 0 only as expected_failure/not_implemented)
tests/native/automatic_gc/run.sh /tmp/woozy-automatic-gc-baseline \
  --baseline-only

# Selected control must not claim GC
tests/native/automatic_gc/run.sh /tmp/woozy-automatic-gc-control \
  --case control-empty --opt 0

# Candidate role: append these flags to the reference wrapper invocation.
# The wrapper may still pass --helper; candidate mode ignores it.
tests/native/automatic_gc/run.sh /tmp/woozy-automatic-gc-candidate \
  --role candidate --strace /path/to/strace \
  --lld "$WOOZY_LLVM/bin/ld.lld"

# Synthetic checker logic only, not GC evidence
python3 tests/native/automatic_gc/selftest_checks.py

# Pure transport/argv controls only, not GC evidence
python3 tests/native/automatic_gc/test_public_port_controls.py
```

Default/reference reuse of an existing output directory writes a new `run-*`
child and reports the actual `output_dir`; candidate mode still refuses a
non-empty output directory.

`run.sh` defaults to the fixed Root build products under
`build/woozy-stdlib-native` and
LLVM 22.1.8; override with `WOOZY_NATIVE_BUILD`, `WOOZY_LLVM`, `WOOZY_LIBXML2`.
The legacy libxml2 directory is prepended to `LD_LIBRARY_PATH` for `ld.lld`.

The suite is intentionally not registered in Root CTest until collection is
implemented and this contract is reviewed by the runtime owner.
