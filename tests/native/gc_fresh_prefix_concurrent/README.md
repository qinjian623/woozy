# Fresh Vec strong prefix vs. a pending collection request (SH15)

This directory adds one small real-source concurrency probe for the fresh
strong-edge `Vec[Vec[Node]]` constructor.  It answers exactly one question:

> When a normal `lang_gc_runtime_collect_v1` request is already pending while
> the generated code writes the fresh Vec's own direct prefix and then
> registers the object, does the collector keep the object alive, and does the
> request really stay pending until the registration succeeded?

The probe shows that the fresh direct prefix does not need its own
critical region: the object is not registered yet, so the collector cannot
reach it, and the request cannot start the collection kernel until the mutator
parks.  Nothing here claims performance, and nothing here is exhaustive
multi-thread correctness.

No production, runtime, CMake, Root, PLAN/STATE/catalog or existing fixture is
changed.  No keyword, no collect API, no runtime ABI and no platform layer is
added.  The CPP helper, the Wz Nativeobs entry, the C++ CLI, the hosted runtime
archive, strace and the LLVM toolchain are Root-supplied test components.  This
directory does not build any of them and never substitutes one emitter for the
other.

## Files

| File | Purpose |
|---|---|
| `fixtures/fresh-prefix/src/main.wz` | one real Wz program: three just-returned self-cycle `Vec[Node]` rings in a length-3 `Vec[Vec[Node]]`, one empty `Vec[Vec[Node]]` control, one already-garbage ring as positive reclaim control, and the Weak liveness + final-reclamation assertions |
| `fixtures/fresh-prefix/lang.toml` | hosted Native manifest (`[build.native] runtime = "hosted"`) |
| `observe_fresh_prefix.cpp` | test-only observer/shim: wraps `lang_vec_new_v1` and `lang_gc_program_register_v1`, identifies the target by allocation generation, runs the real collector pthread, emits the observation record |
| `run.py` | real emitter IR checks, real program build/run, observer link/run, the fixed exec trace/audit and the typed command records |
| `generation_control.cpp` | small hosted C++ control that includes the real shim translation unit and exercises its `note_new`/`note_register` generation bookkeeping against real `lang_vec_new_v1` address reuse (no collector, no thread, no Wz source) |
| `pure_controls.py` | pure-Python controls; zero external calls |
| `README.md` | this file |

## Fixture shape

```
struct Node { marker i32; children Vec[Node]; }          // stride 16
make_ring(marker)            -> fresh self-cycle Vec[Node]
build_rings()                -> [make_ring(11), make_ring(22), make_ring(33)]
build_empty()                -> []                       // Vec[Vec[Node]]
main()                       -> garbage ring, rings + spare in one block,
                                Weak on rings[1], 512-iteration pressure loop,
                                final Weak upgrade must be None
```

`build_rings` is the only place a `Vec[Vec[Node]]` of length three is created,
and `build_empty` is the only place a `Vec[Vec[Node]]` of length zero is
created.  Their element stride is one pointer, so the
`(length, pointer-stride)` pair is the object identity the shim selects.

Because the three arguments of `build_rings` are calls to `make_ring`, the
emitter takes the fresh direct-prefix path (not the constant-table path): the
arguments are evaluated and owned before `lang_vec_new_v1`, the prefix copies
each pointer and publishes `initialized = 1, 2, 3`, and only then registers the
same object.

## Target identity is an allocation generation, not a raw address

The allocator legitimately reuses a released descriptor address.  In this
fixture the 512-iteration pressure loop creates empty `Vec[Node]` children
(80-byte request, size class 96, stride 16) and one-element `Vec[Node]` rings
(96-byte request, size class 128, stride 16).  Those classes overlap the
empty `Vec[Vec[Node]]` (80 bytes) and the length-three target (104 bytes), so a
`Vec[Vec[Node]]` descriptor address can later belong to an unrelated Vec.

The shim keeps one tracked address *generation* per selection: the allocation
serial of the `lang_vec_new_v1` return that produced it.  On every constructor
return it retires any earlier tracked generation that occupied the same raw
address, even when the new shape is different.  A later registration is the
target only while that generation is still live.  A second registration with
no intervening constructor is still the same generation, so it is counted and
fails (`duplicate_target_registration`) instead of being ignored.  Preserving
the earlier counts and not retaining the object means a legal reuse can neither
create a false duplicate-registration failure nor hide a real one.

`generation_control.cpp` demonstrates exactly that with the real shim
functions and the real hosted allocator: a differently shaped Vec that the
allocator places at a released target address retires the old generation and is
not counted; two registrations of one object with no `new` between them are
still counted.  Each mode's test starts with separate selection counters, just
as the real observer runs one mode per process.  It starts no collector or
thread and links no source program.
The delivered single-shot result of that control is recorded in the companion
report; a real observer run is still required before F1 can be called passed.

## What the shim observes

Two existing entry points are wrapped, and nothing else:
`lang_vec_new_v1` and `lang_gc_program_register_v1`.

1. At the selected constructor return the shim checks `initialized == 0`,
   `refs == 1` and `lang_gc_world_can_collect_v1() == true` (the mutator is
   registered, ACTIVE, and outside SAFE/critical), then starts one host pthread.
2. That thread registers itself with `lang_gc_world_register_v1` and calls the
   ordinary `lang_gc_runtime_collect_v1(5 s, &stats)`.  The runtime sets its
   phase to `STOPPING` and `lang_gc_world_stop_v1` publishes the real request
   word, then waits for the still-ACTIVE mutator.
3. The shim waits bounded (2 s) for `lang_gc_world_request_pending_v1()` — the
   actual request bit, not a "started" flag — and requires the collector to be
   unfinished and the phase to still be `STOPPING`.  It does not poll the world,
   does not enter SAFE, does not call any Source/C blocking function and returns
   the original pointer unchanged.
4. The generated code now writes its own prefix while the request is pending.
5. At the registration call the shim requires the same live generation and
   pointer, `initialized == length`, `refs == 1`, the request still pending, the
   collector unfinished, phase `STOPPING` (not `KERNEL`) and the expected
   markers/self-cycle pointers.  Only then does it call the real
   `lang_gc_program_register_v1`.
6. Only after that real call returned `true` does the shim call
   `lang_gc_world_poll_v1()`.  That single poll is the only parking point; a
   request still pending afterwards means the mutator did not park (SAFE,
   critical, fork-stable, or an emitter that already parked) and fails with
   `poll_did_not_park`.
7. After the collector finishes, the shim re-reads the target without retaining
   it, joins the collector thread (checking the `pthread_join` result), and
   emits the record.  The collector unregisters its own world node before
   publishing completion so a later pressure/final collection is not blocked by
   a dead ACTIVE node.

The source program itself then reads every marker, checks the Weak while the
strong owner still exists, drops the rings, runs the existing 512-iteration
garbage-cycle pressure rounds, and requires the final `weak.upgrade` to be
`None`.  The allocation observer
(`tests/native/language_gc_source/observe_gc.cpp`, unchanged) additionally
requires balanced `lang_alloc_v1`/`lang_free_v1` and Vec events, zero live
heap/payload, and rejects a duplicate or unknown `FREE`.

## IR assertions

`run.py` asks each emitter for its own `--out-llvm-before` and `--out-native`.
Each `build_rings` and `build_empty` declaration is mapped to its real NIR
`SymbolId` through NIR source spans, then to `@lang.fn.<SymbolId>`.  The
reviewed structural assertion from
`tests/native/gc_critical_emission` is reused unchanged:

* the `lang_vec_new_v1` result is the object identity;
* the prefix ends at a registration of that same object;
* the initialized-count stores are exactly `1..N` and precede it;
* every element copy targets that object's data;
* no call other than `llvm.memcpy` and that registration appears inside the
  prefix (so no poll, SAFE, critical, callback, throw/exit or panic call can be
  there);
* an empty Vec must write no element copy and no initialized-count store.

In addition this runner requires the recorded element stride to be exactly one
pointer, requires the length to be 3 for `build_rings` and 0 for
`build_empty`, and explicitly scans the interval for the named poll/SAFE/
critical/callback/exit/panic symbols.

The pointer stride has two real printed forms and both are accepted, for the
length and the element stride *together*:

* the Cpp emitter's integer literal `i64 8`;
* the Wz text backend's canonical Native64 allocation-size expression
  `i64 ptrtoint (ptr getelementptr (ptr, ptr null, i32 1) to i64)`
  (`selfhost/src/wzcc/nativeemit/text.wz:allocation_size`).

The platform is confirmed before the expression is valued: Linux, x86_64 and
`sizeof(void *) == 8`; a different platform is refused instead of guessed.
Every other stride spelling — another literal, another `ptrtoint`/`getelementptr`
shape, a quoted literal, a quoted symbol name — is rejected with
`unrecognized lang_vec_new_v1 element stride expression`.  A missing target, an
empty NIR/LLVM, or zero matched targets is a failure.

## Link class (both artifacts are hosted)

The fixture manifest requests `[build.native] runtime = "hosted"`.  The public
CLI program built from that manifest is therefore a **hosted** libc program,
and the observation binary linked from the pre-optimization LLVM plus
`observe_fresh_prefix.cpp`, the unchanged allocation observer and the hosted
runtime is hosted too.  Neither artifact is a freestanding core program and
neither is described as one.

`run.py` runs a real `readelf -l -d` on **both** artifacts and requires
`INTERP`/`NEEDED` for each, recording the per-file link class, the real argv and
the exec-log SHA.  The shim mirrors the fixture's `struct Node` (`int32_t`
marker plus one pointer, size 16) only to read the freshly written prefix; it
fails on a stride mismatch instead of guessing another layout.

## Budgets

| Bound | Value |
|---|---|
| handshake wait for the request bit | 2 s, `sched_yield` only |
| collector stop/collect timeout | 5 s (passed to `lang_gc_runtime_collect_v1`) |
| wait for the collector completion flag | 6 s |
| `pthread_join` itself | no separate deadline; bounded only by the outer program budget, and its return value is checked (`collector_join_failed`) |
| each test program / observed program | `--timeout`, default 60 s |
| each emit/build/link | `--tool-timeout`, default 300 s, Root to confirm |
| each readelf | 60 s |

The 6 s bound covers only the completion flag; it is not a join deadline.  No
non-portable deadline API is added.  Every wait is finite; a missed handshake, a
missing hook, a timeout or an unfinished cleanup is a failure.

## Failure classification

* `poll_did_not_park`, `collector_finished_before_register`,
  `collector_finished_before_new_return`: the mutator parked earlier than the
  registration, so the pending-request interval the probe targets did not
  exist in that emitter.  The old guarded emitter parked in the outermost
  `leave_critical` before registering.  Such a result is an **ordering-scope
  mismatch for this specific no-guard interval, not evidence of an old GC
  safety defect**, and must not be reported as one.
* `phase_not_stopping_*`, `collector_not_busy_at_register`,
  `request_missing_at_*`: the request did not span the interval.
* `initialized_prefix_incomplete`, `initialized_at_new_not_zero`,
  `target_extra_retain`: the constructor or ownership shape changed.
* `duplicate_target_registration`: two registrations for one allocation
  generation; a legal address reuse retires the generation instead and does not
  produce this failure.
* `collector_join_failed`: `pthread_join` returned non-zero.
* `kernel-stats`, `alive-after`, `collector-status`: the collection did no
  reclaim work, reclaimed a live object, or failed.
* exec-audit failures: the traced process executed something outside its
  declared executable set (for example a Wz emit that really ran the Cpp helper
  or an LLVM tool).  The record and the log are kept before the rejection.

## Reused dependencies (no new exported interface)

* `tests/native/gc_critical_emission/run_gc_critical_emission.py` — the
  reviewed `parse_nir`, `parse_llvm`, `scan_source_functions`,
  `fixture_snapshot`, `map_source_target`, `assert_prefix_case`,
  `call_indices`, `run`, `ok`, `assert_execution_completed`,
  `_strict_process` and `write_json_atomic`.
* `tests/native/async_memory/run_async_memory.py::parse_observer` — the real
  observer parser/schema that `hosted_thread_gc` also uses (heap *and* Vec
  balance, live heap/payload at exit).  `language_gc_source.parse_events` is a
  different schema and is not used here.
* `tests/native/language_gc_source/observe_gc.cpp` — the unchanged allocation
  and Vec observer.
* `selfhost/corpus.py::_run_process_impl`, `parse_trace` and `audit_trace` —
  the one bounded process transport and the real exec-log parser/auditor.
* The `hosted_thread_gc` pattern for a hosted observer link with `--wrap`.

Nothing is copied into a new framework, no new runtime symbol is referenced,
and no exported runtime API is invented.

## Command records, inputs and exec audit

Every real command goes through the one common process entry, which adds the
fixed `strace --seccomp-bpf -f -qq -s 16384 -e trace=execve,execveat -o <log>`
prefix.  Each command is recorded with: trace label, the logical `argv`, the
`traced_argv`, the exec log path plus SHA-256 and the audit result
(`observed`/`allowed`/`status`), cwd, actual timeout, exact `int` exit (a
boolean exit is rejected), the three exact booleans, raw stdout/stderr as hex
plus SHA-256, the environment delta (only `GC_FRESH_PREFIX_MODE`), and the
SHA-256 of every configured input before and after the command.

The executable allowlist is per role and exact:

| Role | May execute |
|---|---|
| Cpp emit | the Cpp helper and the declared LLVM tools |
| Wz emit | **the Wz driver only** |
| public CLI build | the CLI, the helper, the declared LLVM tools and the declared linker |
| link | the supplied clang driver, `ld.lld` and the declared linker |
| readelf | the supplied readelf |
| public program / observed program | that artifact only |

so a Wz emission that shells out to the Cpp CLI/helper or to an LLVM tool to
stand in for the Source frontend is a recorded failure, not a label.

The configured input set is resolved once from the actual argv-selected
locations — helper, Wz driver, CLI, hosted runtime archive, clang, readelf,
strace, the fixed LLVM tool set (`clang`, `clang++`, `opt`, `llc`, `ld.lld`),
the optional linker, `selfhost/corpus.py`, this runner, the controls, the
observer parser, the allocation observer, the shim, the generation control, the
fixture manifest/source and every file of the actual `--stdlib` tree.  The tree
is enumerated again for each snapshot so added and removed files also fail.
A missing
declared input is an error, not a silently skipped entry.  `inputs.before.json`,
`inputs.after.json` and the final comparison are all recorded.  The internal
toolchain selection of the supplied CLI is *not* resolved by this runner; the
exec audit records what that CLI really executed instead of pretending the
provenance is known.

The typed/raw process record is handed to the shared `commands.json` save point
before the after-input snapshot is read, so an unreadable after-snapshot keeps
the already executed command and its raw bytes and records the snapshot error.
A changed or unreadable input is a failure.  `commands.json` and `summary.json`
are written through a same-directory `.pending` file and `os.replace`, with
non-finite JSON refused.  The output directory must be new or truly empty; an
existing non-empty directory is refused before any write or command, and no
`rmtree` path exists.

## Run (future, after Root confirms the components)

```sh
python3 -B tests/native/gc_fresh_prefix_concurrent/run.py \
  --helper /path/to/root-supplied/native/lang_native_helper \
  --wz-nativeobs /path/to/root-supplied/nativeobs-driver \
  --cli /path/to/root-supplied/woozyc \
  --runtime /path/to/root-supplied/native/liblang_hosted_runtime.a \
  --clang /path/to/llvm/bin/clang \
  --readelf /path/to/llvm/bin/llvm-readelf \
  --strace /path/to/toolchains/strace-7.2/usr/bin/strace \
  --ld /path/to/linker-driver \
  --root /path/to/repo \
  --out /path/to/new-or-empty/evidence-dir
```

`--stdlib` defaults to `<root>/stdlib`.  `--opt all` (default) is the exact
matrix: 2 emit groups per O-level (4), 1 public CLI program group per O-level
(with its build and run commands, 2 groups), and 4 observed groups per O-level
(link + readelf + observed for `nonempty`/`empty` on both emitters, 8 groups);
14 logical groups and 32 real commands in total.  `--opt 0` or `--opt 2` runs
the 7-group / 16-command half.  The runner refuses to report `passed` unless
every expected command in the matrix really ran.

## Pure controls

```sh
python3 -B tests/native/gc_fresh_prefix_concurrent/pure_controls.py
```

The spawn/exec/socket guards are installed before `run.py` is imported and
`external_launch_attempts` must stay `[]`.  90 controls cover the original set
of 68 plus: the Wz printed pointer-stride form through the real `check_engine`,
a quoted stride spelling, exact protocol integers/booleans (boolean-as-integer,
float, integer-as-boolean, counts and stats), non-finite `Infinity`/`NaN`,
positive and negative exponent overflow, and
duplicate protocol keys, a float observer metric, a kept launch-failure record,
and seven whole-main controls — a complete successful `--opt 0` run including the
first observed handshake with the real allocation parser, the known
after-snapshot failure (the executed command and its raw bytes are kept),
added and removed standard-library files, a
changed frozen tool input, and a Wz emit that really executes the Cpp helper
(rejected by the exec audit with the log preserved), plus real soft links for
`clang++` and `llvm-readelf`. Their invocation names are preserved because LLVM
uses those names to choose its driver and output format; exec auditing still
compares the resolved executable targets.

## Current status

The 90 pure controls, 25 C++ allocation-generation checks, and all 14 logical
groups / 32 real commands passed. The eight O0/O2, C++/Wz, nonempty/empty
observations kept a pending stop request across constructor return and
registration, then completed the poll and join. Prefix markers, self-cycles,
Weak assertions, and heap/Vec allocation balance passed with zero final live
allocations. Each emitter's own LLVM was used for its observation programs.

Both observed and reference programs are hosted links. The public CLI builds
use the C++ reference compiler; Wz emission uses the Nativeobs component.
This checks the selected pending-stop overlaps and does not exhaust
multithreaded behavior or establish complete Wz CLI parity or performance.
