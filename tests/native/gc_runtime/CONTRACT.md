# lang-gc-runtime/1 contract

Status: MEM02 runtime-only automatic controller. It connects the accepted
`lang-gc/1` stopped-world cycle collector, `lang-gc-world/1` mutator registry,
and `weak.hpp` into one pressure-driven runtime path.

This is **not** the completed SourceNativeGC:

* Wz source writes, compiler poll/critical instrumentation, and the main-return
  final collection hook are not connected.
* Blocking runtime operations are not automatically annotated SAFE.
* Main/thread/process startup and shutdown are not modified.
* Foreign-callback guards and hosted callback SAFE restoration remain Root work.
* The controller does not claim that every source program is automatically
  collected; it only provides the runtime APIs the next compiler/root step can
  call.

## 1. Fixed C ABI

Declared in `runtime/native/gc_runtime.hpp`, implemented in
`runtime/native/gc_runtime.cpp`. `LangGcStatus`, `LangGcOps`, `LangGcVisitor`
and `LangGcStats` come from the read-only `gc.hpp` dependency.

| Symbol | Meaning |
| --- | --- |
| `lang_gc_runtime_register_v1(void *, const LangGcOps *)` | Thin `lang_gc_register_v1` wrapper. Only `LANG_GC_OK` increments pressure. DUPLICATE/INVALID do not. It never discovers or registers an unknown world thread. |
| `lang_gc_runtime_unregister_v1(void *)` | Thin `lang_gc_unregister_v1` wrapper. It does not change pressure. |
| `lang_gc_runtime_poll_v1()` | Automatic safepoint/controller poll. |
| `lang_gc_runtime_collect_v1(int64_t, LangGcStats *)` | Explicit controller collection. |
| `lang_gc_runtime_defer_release_v1(void *, void (*)(void *) noexcept)` | Transfer an external payload to the current collector's post-resume release batch, or return false. |
| `lang_gc_runtime_stats_v1(LangGcRuntimeStats *)` | Read-only controller snapshot. |
| `lang_gc_runtime_fork_prepare_v1()` | Controller fork prepare; holds only the short queue lock. |
| `lang_gc_runtime_fork_parent_v1()` | Parent release. |
| `lang_gc_runtime_fork_child_v1()` | Child reset phase/synchronization; no external releases and no fake drops. |

The caller must explicitly call `lang_gc_world_register_v1`. This layer does
not auto-register the current thread and does not treat an incomplete object as
a complete graph.

## 2. Pressure and poll ordering

Initial pressure is 128 successful object registrations. Pressure is a
saturating cumulative counter `pressure_accum` plus a threshold
`pressure_limit`:

* initial `pressure_limit = 128`;
* `register` increments `pressure_accum` only for `LANG_GC_OK`;
* poll considers pressure pending when `pressure_accum >= pressure_limit`;
* after a successful kernel collection:
  * subtract the pressure snapshot captured before the collection from
    `pressure_accum`, using a CAS so registrations that happen during the
    collection remain pending;
  * set `pressure_limit = max(128, 2 * stats.registry_live_after)`, saturating
    at `UINT64_MAX`.

The adaptive threshold avoids rescanning a growing live graph after every fixed
128 allocations. A registration that happens after the collection snapshot is
never lost. A failed stop/kernel collection leaves pressure pending.

`lang_gc_runtime_poll_v1` order:

1. If the world request bit is set, call `lang_gc_world_poll_v1` first.
2. If pressure is not pending, return.
3. If `lang_gc_world_can_collect_v1()` is false, return.
4. Attempt `lang_gc_runtime_collect_v1(10 ms, nullptr)`.

With neither a world request nor pending pressure the function performs only
atomic loads/branches: no gettid, lock, allocation, futex, or kernel call.
Critical/SAFE threads cannot start a collection because
`lang_gc_world_can_collect_v1()` is false. A pending world request is always
handled before the controller considers pressure.

## 3. Collection and status mapping

`lang_gc_runtime_collect_v1` claims a controller busy word with CAS; a
concurrent controller collection returns `-16`. The caller must be explicitly
world-registered and outside SAFE and critical regions. The automatic poll
gates on `lang_gc_world_can_collect_v1`, while direct misuse falls through to
`lang_gc_world_stop_v1` (`-16` inside critical, or Panic when SAFE or
unregistered). The timeout is forwarded unchanged to `lang_gc_world_stop_v1`,
which owns the fixed monotonic deadline and EINTR retries.

The world is stopped before the kernel collector. `lang_gc_collect_stopped_v1`
is called while stopped; it returns only `LANG_GC_OK` or `LANG_GC_BUSY` in this
runtime (other results are a terminal controller Panic).

Status mapping:

| Condition | Controller result |
| --- | --- |
| controller CAS busy, or world stop `-16` | `-16` BUSY |
| world stop timeout `-110` | `-110`; world has retracted and resumed |
| `timeout_ns < -1` / world stop `-22` | `-22` INVALID |
| world stop OK + kernel `LANG_GC_OK` | `0` success |
| world stop OK + kernel `LANG_GC_BUSY` | `-16`; world is resumed first |

On every failure the controller clears its busy state and leaves pending
pressure and the graph unchanged. `out` is zeroed at entry and only populated
on kernel success. A timeout is never recorded as a successful collection.

Successful sequence:

1. claim controller busy, stop world with the requested deadline;
2. open the queue only for this collector, run the kernel collector stopped;
3. detach the queue into a local owned list;
4. update pressure/threshold from the kernel's `registry_live_after`;
5. resume the world;
6. clear controller busy state;
7. drain the local external-release list and invoke callbacks.

External C/release callbacks therefore never run before the world resume call,
and never under the controller queue lock.

## 4. Deferred external releases

`lang_gc_runtime_defer_release_v1(payload, release)` has a no-gettid fast path:
if the controller enqueue window is closed, it returns false immediately. The
caller still owns the reference and must call its C release wrapper. This is the
normal non-GC drop path.

Only the collector that is currently inside the stopped kernel collection can
enqueue. The controller cannot observe the kernel's individual trace, clear, and
release callbacks; source integration must therefore call this helper only from
clear/release, never from trace. The slow path checks the current gettid against
the recorded collector tid under the queue lock. Other SAFE/mutator threads
receive false. Queue node allocation happens before the lock; free happens
after unlock; callbacks run only after detach/resume and never under the lock.

The queue holds the transferred payload/reference without retaining it and
without releasing it early. Each node is released exactly once. On success the
batch is detached before world resume, so a callback that starts a second
collection uses a fresh global queue while the old local batch remains valid.
A release callback may allocate, register, and call
`lang_gc_runtime_collect_v1` again; the old batch is not overwritten.

Pure Wz strong edges must be removed synchronously in the object's `clear`
callback, not deferred. The queue is only for external C/reference payloads
whose release may re-enter Woozy after resume.

## 5. `can_collect` interaction

`lang_gc_runtime_poll_v1` defers to `lang_gc_world_can_collect_v1()`; the world
query is true only when no stop is owned, the calling thread is registered and
ACTIVE, and both SAFE and critical depths are zero. It is false for the
collector holding a stop and for every thread while another stop is active.
The controller never uses this query to release or consume a stop; `resume`
remains the world collector's explicit operation.

## 6. Statistics

`LangGcRuntimeStats` is the minimum test/inspection surface:

* pressure_accum/pressure_limit, last_live_after;
* queue_depth, queue_enqueued, queue_drained;
* collect_ok, collect_busy, collect_timeout, collect_invalid, kernel_busy;
* total_registered_scanned, total_traced_edges;
* phase (`IDLE`, `STOPPING`, `KERNEL`, `DRAINING`), busy, collector_tid,
  enqueue_open;
* fork generation.

`lang_gc_runtime_stats_v1` is a lock-free snapshot and does not change state.

## 7. Fork hooks and order

Root should use one fixed order. The recommended complete sequence is:

```
enter world critical if required by the fork path
lang_alloc_fork_prepare_v1
lang_weak_fork_prepare_v1
lang_gc_fork_prepare_v1
lang_gc_world_fork_prepare_v1
lang_gc_runtime_fork_prepare_v1
fork
parent:
  lang_gc_runtime_fork_parent_v1
  lang_gc_world_fork_parent_v1
  lang_gc_fork_parent_v1
  lang_weak_fork_parent_v1
  lang_alloc_fork_parent_v1
child:
  lang_alloc_fork_child_v1
  lang_weak_fork_child_v1
  lang_gc_fork_child_v1
  lang_gc_world_fork_child_v1
  lang_gc_runtime_fork_child_v1
```

Controller prepare takes only the short queue lock; it never waits for
`busy`, a controller collection, or the world stop. Thus a critical fork can
proceed while another controller stop is pending without waiting on the GC busy
state. Other runtime prepare hooks may still have their own documented
constraints.

The child resets controller queue lock/head/tail, enqueue window, busy,
phase, collector tid, and fork state; it increments the fork generation. It
does not retain, release, or invoke callbacks for inherited external payloads
and does not synthesize ARC drops for vanished threads. An inherited queue is
orphaned (leaked in the child) rather than fake-dropped. Pressure counters are
left inherited; the child does not fabricate drops or re-run parent callbacks.
If the fork syscall fails, the caller must still run the parent hooks.

A fork from the active collector inside a kernel clear/trace callback is
rejected with terminal Panic `fork-in-gc-callback`. A normal foreign destructor
after resume may fork. The child drain detects the generation change after the
current callback and aborts instead of executing more inherited releases; the
parent continues its own local batch.

## 8. Terminal Panic

The controller writes a terminal record and exits 101 through `exit_group`:

```
LANG_NATIVE_PANIC:v1:gc-runtime:<reason>:<tid>:0:0
```

Reasons currently emitted: `fork-in-gc-callback`, `fork-hook-misuse`,
`gettid-failed`, `world-stop-status`, `kernel-status`,
`defer-allocation-failed`. A queue allocation failure on the eligible collector
path is terminal Panic rather than a false result that would force an external
release while the world is stopped.

## 9. Explicitly not implemented / Root handoff

`not_run` / not integrated here:

* Compiler emission of polls, critical regions, or source-level ownership
  writes.
* Main-return final collection and thread/process startup/shutdown hooks.
* Automatic SAFE around blocking runtime/syscall operations.
* Hosted foreign-callback entry/SAFE restoration; the caller must explicitly
  world-register and follow the world callback contract.
* Wiring the complete fork order into `process.cpp`; only hooks and tests are
  provided.
* Any claim that MEM02/SourceNativeGC is complete.

## 10. Verification evidence

`tests/native/gc_runtime/run_all.sh` is the single entry point. It applies a
per-process `timeout` and builds/runs:

* Hosted O0/O2/ASAN+UBSAN suites linking `gc_runtime.cpp`, `gc_world.cpp`,
  `gc.cpp`, `weak.cpp`, `panic.cpp`, and the allocator. The default allocator is
  wrapped to count live allocations; the ASAN build uses a libc-heap shim so
  LeakSanitizer sees every runtime allocation.
* A seccomp fastpoll test with gettid denied and 100,000 idle polls.
* A controller misuse process checking exit 101 and
  `gc-runtime:fork-in-gc-callback`.
* A `-ffreestanding` build of every runtime layer and a `ld -static` ELF with
  no INTERP, no dynamic NEEDED entries, and no undefined symbols. The ELF
  creates a rooted graph plus 128 ~1 KiB garbage cycles, runs automatic
  collection and deferred releases, checks Weak death and final cleanup.
* A GCC freestanding object compile as a second compiler check when available.

Host tests cover:

* 8,192 ~1 KiB unreachable self-cycles with no manual collect in the loop; live
  registry/bytes bounded; rooted payload checked after every poll; Weak upgrade
  dead after collection.
* 4,096 growing rooted objects; adaptive threshold equals
  `max(128, 2 * last_live_after)`; total registered scans linear (observed 5,120
  for 4,096 roots, four auto collections), not the fixed-128 quadratic bound.
* Two critical mutators and a SAFE blocker: automatic stop times out while the
  critical writes are in progress; after their outermost leaves, collection
  succeeds even with the SAFE blocker; 128 deferred external callbacks check
  controller busy == 0, enqueue window closed, world request clear, and no
  half-critical state.
* Queue re-entry: an external callback allocates/registers a new garbage cycle
  and starts a second collection; the old batch drains E1/E2 exactly once and
  the nested queue drains E2 exactly once. No UAF observed under ASAN.
* Late registration during kernel clear preserves pressure; direct non-GC defer
  returns false and the caller releases its own payload. A SAFE non-collector
  inside the stopped kernel clear window is refused enqueue and releases its
  own payload.
* Invalid timeout, controller busy competition, and stop timeout leave graph
  and pressure unchanged. Kernel BUSY from a direct stopped-world collector
  maps to `-16` after world resume.
* Normal fork, critical fork while a controller stop is pending, and fork from
  an external release after resume; child controller reset and no extra
  inherited external release (drain generation abort). No fake drops.

## 11. Final acceptance run

Controller suite with the fixed LLVM 22.1.8 toolchain and legacy libxml2
runtime:

```bash
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
CXX="${WOOZY_TOOLCHAIN_ROOT}/llvm-22.1.8/bin/clang++" \
LD_LIBRARY_PATH="${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9/usr/lib" \
GC_RUNTIME_BUILD_DIR=/tmp/gc-runtime-20261112 \
bash tests/native/gc_runtime/run_all.sh
```

```text
SUMMARY pass=25 fail=0 skip=0
```

Fixed O2 host output:

```text
GC_RUNTIME_AUTO_OK garbage=8192 max_live_objects=100 max_live_bytes=100000 scans=8255
GC_RUNTIME_GROWTH_OK roots=4096 collections=4 scans=5120 limit=6912 live_after=3456
GC_RUNTIME_TIMEOUT_OK timeout=1 ok=71 ext=128
GC_RUNTIME_QUEUE_OK enqueued=131 drained=131
GC_RUNTIME_LATE_REGISTER_OK
GC_RUNTIME_DIRECT_RELEASE_OK
GC_RUNTIME_NONCOLLECTOR_ENQUEUE_OK
GC_RUNTIME_FAIL_OK invalid=1 busy=2 timeout=2
GC_RUNTIME_KERNEL_BUSY_OK kernel_busy=1
GC_RUNTIME_FORK_IDLE_OK child_phase=0
GC_RUNTIME_FORK_STOP_OK collections=80
GC_RUNTIME_FORK_DRAIN_OK child_e2=0
GC_RUNTIME_HOST_OK allocations=25307
```

Static no-libc ELF printed `GC_RUNTIME_STATIC_OK`; the suite also checked
`statically linked`, no `NEEDED`, no `INTERP`, and no undefined symbols.
Fastpoll printed `FAST_OK` with gettid denied. The misuse process exited 101
with `LANG_NATIVE_PANIC:v1:gc-runtime:fork-in-gc-callback`.

The accepted world suite was rerun unchanged for regression:

```bash
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
CXX="${WOOZY_TOOLCHAIN_ROOT}/llvm-22.1.8/bin/clang++" \
LD_LIBRARY_PATH="${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9/usr/lib" \
GC_WORLD_BUILD_DIR=/tmp/gc-world-regression-20261112 \
bash tests/native/gc_world/run_all.sh
# SUMMARY pass=42 fail=0 skip=0
```

This acceptance run covers the runtime controller and world layers only. To
complete MEM02/SourceNativeGC, Root still needs to integrate the items listed
in section 9.
