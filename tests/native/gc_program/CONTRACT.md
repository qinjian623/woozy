# lang-gc-program/1 — runtime integration contract

Status: **runtime/ABI integration only; not SourceNativeGC complete.**
Operation: `woozy-native-gc-program-runtime-20261112`.
Base: `68b3cde`.

This directory adds the private program lifecycle/guard layer between a
generated Native program and the accepted `lang-gc/1`, `lang-gc-world/1` and
`lang-gc-runtime/1` layers. It does not emit HIR, IR, polls or source
registration, and it does not modify the emitter. The Source pressure target in
`tests/native/automatic_gc` therefore continues to be **RED**; no C++ runtime
test here is Source verification.

Everything in this directory is a C++ runtime test. It uses real `LangVec`,
language threads and real fork paths, but no `.wz` source program.

## API

`runtime/native/gc_program.hpp` is the private header. The compiler's outermost
main wrapper must use the C ABI; generated code must not depend on the inline
C++ hooks.

| Symbol | Meaning |
| --- | --- |
| `lang_gc_program_init_v1()` | Register the main world thread and enable guards. Returns `0` or `LANG_GC_PROGRAM_ERROR_STATE`. |
| `lang_gc_program_finish_v1()` | Finite-stop final collection, then unregister main. Hooks/bookkeeping remain enabled until process exit. Must be after wrapper cleanup, outside SAFE/critical. |
| `lang_gc_program_register_v1(obj, ops)` | Opt-in registration through `lang_gc_runtime_register_v1`, so successful registrations increment controller pressure. `false` when disabled or not `LANG_GC_OK`; never registers a primitive Vec by itself. |
| `lang_gc_program_unregister_v1(obj)` | Cheap no-op while disabled; otherwise `lang_gc_runtime_unregister_v1`. |
| `lang_gc_program_enter_safe_v1()` / `leave_safe(token)` | C ABI SAFE scope for generated foreign calls. |
| `lang_gc_program_enter_critical_v1()` / `leave_critical(token)` | C ABI nested graph-modification scope. |
| `lang_gc_program_thread_enter_v1()` / `leave(registered)` | Runtime thread runner hooks. |
| `lang_gc_program_fork_enter_v1(token)` / `leave(token)` | Stack-owned fork scope; suspends/restores outer SAFE depth. |
| `lang_gc_world_suspend_safe_v1()` / `restore_safe(depth)` | Concrete world extension used by the fork scope; no extra `GraphStableNoPark` API was required. |

`LANG_GC_PROGRAM_FINAL_COLLECT_NS` is 10 ms, the same finite stop-acquisition
budget as the automatic controller; it is not a hard upper bound on arbitrary
trace/clear/foreign-cleanup work. A failed/timed-out final collection is
reported, never recorded as success. Panic performs no GC cleanup.

`finish` does **not** clear the enabled word or the hook table. A detached
language thread may still be registered and may final-release objects after
main returns. Keeping registration/unregistration bookkeeping active until
process exit is required so those final frees remove their registry entries; a
later worker poll/collection must never read freed storage. `finish` is final
for the main wrapper, not a runtime shutdown. A second main `finish` is a state
error.

The wrapper shape is:

```text
if (lang_gc_program_init_v1() != 0) {
    emit typed source error through the normal error channel
    return/exit without running user code
}
run user code and perform every wrapper cleanup
(void)lang_gc_program_finish_v1()
```

The runtime does not print a source typed error. It only returns the state
code; the wrapper owns the message/key and the source error channel. `finish`
must be called after cleanup so element/external destroys do not run after the
registry was torn down.

## Default fast path

Before init, `enabled_word_v1` is zero. Runtime guards perform one atomic load
(and a null hook-pointer check) and return. They do not call `gettid`, inspect
the world registry, allocate, or register a numeric `Vec`. The seccomp
`disabled-fast` test denies `gettid` and exercises Vec final release, SAFE,
critical, thread-enter, and fork-token enter/leave while disabled.

## Threads and roots

`runtime/native/thread.cpp` and `runtime/hosted/thread.cpp` install a
`ThreadGuard` before the language entry and release it after the ThreadState
context_drop. An existing registration is not removed. The context bytes and
the ThreadState itself are never registered; their strong references are an
external root because the collector observes their object `refs` as greater
than the incoming graph edges.

## Blocking SAFE hooks

The runtime enters SAFE only around the blocking operation, after all graph
validation/writes, and while it holds no allocator/environment/launch lock.
Leave happens before returning to graph or caller-visible output writes.

Hooked paths:

* `os.read`, `os.write`;
* `socket.connect`, `accept`, `send`, `recv`, `sendto`, `recvfrom`, `poll`;
* mutex contention wait, condvar wait, IPC mutex/condvar/semaphore futex waits;
* language thread join;
* `waitpid`, `sigtimedwait`, `nanosleep`;
* hosted pthread condvar wait (the pthread mutex is released while waiting).

Allocator-internal `mmap`, `getdents`, `readlink`, DNS parsing, and threads
holding launch/environment/allocator locks were deliberately left unannotated.
Spawn holds launch through its exec-error pipe handshake and is in the fork
graph-stable no-park scope, so it cannot park while holding launch.

## Fork order and SAFE suspension

The program fork scope is entered **before** launch/environment/allocator locks.
If the caller has outer SAFE depth, `lang_gc_world_suspend_safe_v1` leaves every
level. An outermost leave honors a pending stop normally, so the thread parks
without holding runtime locks. The scope then enters the graph-stable fork
no-park interval (`lang_gc_world_enter_fork_stable_v1`), which is distinct from
a graph-critical modification region. Synchronous callbacks and SAFE regions
remain legal inside it. The node stays ACTIVE for stop/collection even when a
nested SAFE depth is nonzero, so the collector can neither park the thread nor
collect while it holds internal fork locks. A genuine graph-critical region
nested inside still rejects callbacks/SAFE exactly as before.

After all locks are released, the saved outer SAFE depth is restored while the
no-park interval is still active, then the outermost
`lang_gc_world_leave_fork_stable_v1` publishes SAFE or PARKED. A stop requested
during the interval is handled there; a stop owned before the interval was
handled by the initial SAFE suspension before any lock was taken.

Full order (implemented in `runtime/native/process.cpp` and
`runtime/hosted/process.cpp`):

```text
program fork scope (suspend SAFE, enter graph-stable no-park)
launch
environment
allocator prepare
Weak prepare
gc prepare
world prepare
controller prepare
fork
parent: controller, world, gc, Weak, allocator, environment, launch,
        restore SAFE, leave no-park
child:  allocator, Weak, gc, world, controller, reset launch/environment,
        thread tid, restore SAFE, leave no-park
```

The child keeps only the calling thread, resets runtime locks, and does not
fabricate ARC drops for vanished threads. Both freestanding raw fork and hosted
libc fork (direct `fork(3)` and `std/process.fork`) use this path. A fork from
an unregistered foreign thread is outside the world contract and Panics.

Tests cover normal fork, fork from an outer SAFE scope, fork from an outer
critical scope, fork while another thread owns a real stop, fork failure with
all parent locks restored, hosted direct libc fork, hosted language fork, and
synchronous callback/SAFE calls from a hosted `pthread_atfork` prepare handler.
The F1 probe also verifies that a genuine graph critical region still Panics on
callback entry, and that a real `lang_os_write_v1` inside graph critical still
Panics with `safe-in-critical`.

## Vec final release (implemented part)

`panic.cpp`'s ordinary final strong release checks the cheap enabled word and
calls the installed `vec_final_release` hook **before** Weak detach and element
payload drops. The hook unregisters the Vec when it was registered. The probe
test registers a Vec and observes `registry_live()==0` inside its drop callback.

A real self-cycle Vec test traces only `initialized` elements, publishes an
empty prefix before dropping the old element, and verifies that slot beyond the
initialized prefix is never visited. The Vec payload is reclaimed by the
collector.

Not implemented in this task: general struct/enum/Vec structural copy, move,
resize, push and clear publish windows. Those operations were not changed. The
emitter/ownership lowering still must wrap their multi-word publish in short
critical regions and must not drop old external values inside a half-modified
graph. No source-level automatic reclamation is claimed.

## Not done / Root handoff

* Emitter registration, polls/critical emission, and the main wrapper are Root
  frontend work in other files. This task provides the callable ABI and tests.
* No `.wz` source program is compiled or measured here. The automatic-GC Source
  target stays RED.
* Closures, generated struct/enum tracing, and the full Source registration
  patch are not implemented.
* No production pause/throughput claim.

## Verification

`tests/native/gc_program/run_all.sh` is the single entry point. It applies a
per-build/per-process `timeout`, uses fixed Clang 22.1.8, `-std=c++20`,
`-pedantic-errors` (extensions off) and the legacy libxml2 path for `ld.lld`.
It builds host O0/O2/ASAN+UBSAN, hosted O0/O2/ASAN+UBSAN, freestanding static
O2, and a default-core static ELF without `INTERP`, `NEEDED`, undefined symbols
or libc/libstdc++ strings. It also runs the accepted world, callback,
controller, Weak, thread and sync suites. The raw-clone thread/mutex modes are
`skipped` only in the ASAN build because that runner has no ASAN TLS block.

## Emitter registration patch points (not applied here)

The Root frontend worker owns `src/backend/native/emit.cpp` and the main
wrapper. The runtime ABI is ready for these emitted sequences:

```text
; outermost generated main
call i64 @lang_gc_program_init_v1()
if nonzero: emit the normal typed error and return
...
; after all generated cleanup and before returning to the CRT/_start
call i64 @lang_gc_program_finish_v1()
```

```text
; allocate a heap object whose static type can hold an owned Wz edge
call i1 @lang_gc_program_register_v1(ptr %object, ptr @ops_for_static_type)
; ignore false: disabled program, duplicate, or invalid
```

```text
; normal final strong release, before payload drops
call void @lang_gc_program_unregister_v1(ptr %object)
```

```text
; function entry and loop backedges: one advisory load, then the controller
%w = load atomic volatile i64, ptr @lang_gc_safepoint_word_v1 acquire, align 8
if (signed %w <= 0) call void @lang_gc_runtime_poll_v1()
```

```text
; blocking foreign/system call after graph writes
%t = call i32 @lang_gc_program_enter_safe_v1()
call ...                     ; syscall/C call
call void @lang_gc_program_leave_safe_v1(i32 %t)
```

Registration must be emitted only for cycle-capable owning types. A numeric
`Vec` or a Vec whose element type has no owning edge must not be registered.
A registered Vec trace must iterate `initialized`, not `length`, and clear must
publish the new/empty prefix before dropping old elements. Until that emitter
work and the main wrapper are integrated and measured on real `.wz` programs,
the Source target remains RED.

## Correction 2026-11-12: F1/F2/F3

* **F1** — Fork no longer enters a graph-critical modification region. It uses
  a distinct graph-stable no-park interval (`enter/leave_fork_stable`), which
  keeps callbacks and SAFE legal and keeps the node ACTIVE for stop/collection
  while internal fork locks are held. A nested genuine critical region still
  rejects callback/SAFE with `callback-in-critical` / `safe-in-critical`.
  Pending-stop suspension remains before lock acquisition; restoration happens
  after all fork locks are released in parent, child and failure paths.
* **F2** — `lang_gc_program_register_v1` now calls
  `lang_gc_runtime_register_v1`, and program/Vec unregistration calls
  `lang_gc_runtime_unregister_v1`. The permanent `pressure` probe checks 128
  successful program registrations produce `pressure_accum == 128`.
* **F3** — `finish` keeps the enabled word and hook table installed for the
  process lifetime. It unregisters the main world node but leaves worker
  bookkeeping live, so a surviving thread's final releases remove registry
  entries and a later poll/collection cannot read freed nodes. Main-tid entry
  hooks become no-ops after finish so host atexit/destructor code does not enter
  the world for an unregistered main. The permanent `finish-survivor` probe
  uses `std::thread`, forces a `-110` final-stop timeout, releases all nodes,
  polls, and checks an empty registry at O0/O2/ASAN.

The 10 ms FINAL_COLLECT value bounds stop acquisition only; it is not claimed
as a wall-clock bound for arbitrary tracing, clearing, or foreign cleanup.

Root added the ACTIVE/zero-SAFE pending-stop case: fork entry polls before the no-park interval, so a request already visible to the caller completes before any fork lock is acquired. The permanent probe checks collector success at scope return in O0/O2/ASAN; true critical and nested no-park still defer through the existing poll rules.
