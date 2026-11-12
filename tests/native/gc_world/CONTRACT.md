# lang-gc-world/1 contract

Status: MEM02 independent runtime implementation, including nested critical
(graph-modification) regions. This directory owns the stop-the-world registry
only. It does **not** claim that objects are automatically collected, that the
compiler already emits polls or critical regions, that runtime blocking
operations call the SAFE API, or that `fork` is wired into `process.cpp`.

Platform: Linux x86_64, freestanding-capable C++20. The runtime translation
unit uses raw syscalls, atomics, futexes, `lang_alloc_v1`, and `lang_free_v1`.
It does not use libc, libstdc++, pthreads, TLS, or dynamic initialization.

## 1. Fixed C ABI

Declared in `runtime/native/gc_world.hpp`, implemented in
`runtime/native/gc_world.cpp`.

| Symbol | Signature | Meaning |
| --- | --- | --- |
| `lang_gc_world_register_v1` | `bool(void)` | Register current gettid. Returns true only when this call created the registration. Idempotent otherwise. During an active stop a new registration is inserted PARKED and the call waits for resume. |
| `lang_gc_world_unregister_v1` | `bool(void)` | Remove the current thread's registration. Returns true only when this call removed it. False if the thread was not registered. Panic for the active collector or from inside SAFE or critical. |
| `lang_gc_world_poll_v1` | `void(void)` | Safepoint poll. No-request fast path is one atomic load. Slow path looks up gettid. With a request it parks and waits for resume unless the thread is in SAFE or a critical region; critical defers the stop. |
| `lang_gc_world_enter_safe_v1` | `void(void)` | Enter one nesting level of a SAFE region. Panic while any critical region is active. |
| `lang_gc_world_leave_safe_v1` | `void(void)` | Leave one nesting level. The outermost leave reacquires mutator eligibility; if a request is active it parks instead of becoming ACTIVE. |
| `lang_gc_world_enter_critical_v1` | `void(void)` | Enter one nested graph-modification region. Allowed only for a registered ACTIVE thread outside SAFE and not parked; Panic on misuse or depth overflow. |
| `lang_gc_world_leave_critical_v1` | `void(void)` | Leave one critical level. Inner leaves never park. The outermost leave is a safepoint and parks if a request is still active. |
| `lang_gc_world_can_collect_v1` | `bool(void)` | Query only. True only when no stop is owned, the current thread is registered and ACTIVE, and safe_depth and critical_depth are zero. False for unregistered threads and never auto-registers. |
| `lang_gc_world_stop_v1` | `int64_t(int64_t timeout_ns)` | Stop all other registered mutators. Caller must be registered and outside SAFE and critical. On success the caller is the recorded collector. A critical caller gets -16 without any request change. |
| `lang_gc_world_resume_v1` | `void(void)` | Only the successful stop caller may resume. Others Panic. |
| `lang_gc_world_fork_prepare_v1` | `void(void)` | Acquire the registry lock and validate fork preconditions. Lock stays held across `fork()`. |
| `lang_gc_world_fork_parent_v1` | `void(void)` | Release the inherited/held registry lock in the parent. |
| `lang_gc_world_fork_child_v1` | `void(void)` | Prune the child registry to the surviving thread, remap its tid, clear stop state, reset the inherited lock, free removed nodes. |
| `lang_gc_world_request_word_v1` | `uint32_t` global | Atomic request word. Bit 0 is `LANG_GC_WORLD_REQUEST_BIT`. |
| `lang_gc_world_request_pending_v1` | `int(void)` | Out-of-line atomic check, no syscall. |
| `lang_gc_world_poll_required_v1` | `static inline int(void)` | Header inline atomic acquire load for compiler loop branches. |

`bool` is the SysV x86_64 C/C++ bool. The header is usable from C as well as
C++.

## 2. Compiler fast path

The backend must not call a function on every loop iteration if it can avoid
it. Inline this check:

```llvm
%word = load atomic i32, ptr @lang_gc_world_request_word_v1 acquire, align 4
%pending = and i32 %word, 1
%need = icmp ne i32 %pending, 0
br i1 %need, label %slow, label %continue
slow:
call void @lang_gc_world_poll_v1()
```

or use `lang_gc_world_poll_required_v1()`. The load is an atomic builtin; the
compiler must not replace it with a non-atomic cached bool. `poll` checks the
same atomic word again before entering its slow path, so an inlined branch and
the out-of-line call are both sound.

Unregistered threads are not detected on the idle fast path (detecting them
would require gettid, which defeats the purpose). If the request bit is set and
such a thread calls `poll`, the slow path looks up the registry and Panics with
`poll-unregistered`; it cannot silently pass a stop.

## 3. Node state machine

Each registered kernel tid has one node:

* `ACTIVE` - may touch the Wz graph. A critical region is still ACTIVE.
* `SAFE` - inside one or more SAFE regions, must not touch the graph.
* `PARKED` - stopped at a poll/register/safe-leave/critical-leave, waiting on
  the resume epoch.
* `safe_depth` counts nested SAFE entries; `critical_depth` counts nested
  critical entries. They are never both nonzero.

Transitions are serialized by the registry lock:

| Operation | From | To |
| --- | --- | --- |
| new registration, no request | absent | ACTIVE |
| new registration, request active | absent | PARKED, then wait, then ACTIVE |
| repeated registration, request active, ACTIVE, critical=0 | ACTIVE | PARKED, wait, return false |
| repeated registration, request active, ACTIVE, critical>0 | ACTIVE | unchanged, return false (critical must finish) |
| poll, request active, critical=0 | ACTIVE | PARKED, wait |
| poll, request active, critical>0 | ACTIVE | unchanged (defer; outer leave will park) |
| poll, request active | SAFE | unchanged (returns; leave will park) |
| enter SAFE, depth 0 -> 1 | ACTIVE | SAFE |
| enter SAFE, nested | SAFE | SAFE, depth++ |
| leave SAFE, depth > 0 | SAFE | SAFE |
| leave SAFE, depth 1 -> 0, request active | SAFE | PARKED, wait |
| leave SAFE, depth 1 -> 0, no request | SAFE | ACTIVE |
| enter critical, depth 0 -> 1 | ACTIVE | ACTIVE, critical=1 |
| enter critical, nested | ACTIVE | ACTIVE, critical++ |
| leave critical, depth > 1 | ACTIVE | ACTIVE, critical-- (never parks) |
| leave critical, depth 1 -> 0, request active | ACTIVE | PARKED, wait |
| leave critical, depth 1 -> 0, no request | ACTIVE | ACTIVE |
| resume wake | PARKED | ACTIVE, under the registry lock only if no request is active |

The resume-to-ACTIVE transition is performed while holding the registry lock
and rechecking the request. A new stop cannot therefore set the request, scan a
parked node as stopped, and then observe that node becoming ACTIVE.

## 4. Poll, SAFE, and graph ordering

`enter_safe` and the ACTIVE->PARKED transition publish prior graph writes
through the registry lock release. The collector's scan acquires the same lock,
so after a successful stop it sees all mutator writes that preceded the poll or
SAFE entry.

`leave_safe` (outermost) rechecks the request under the lock. If a stop is
pending, it publishes PARKED and waits. It never jumps directly from SAFE to
ACTIVE during a collection. The caller may touch the graph only after
`leave_safe` returns. Nested SAFE entries only adjust the depth; the thread
remains exempt from the stop.

SAFE is a statement about this thread and the Wz graph. It is not a blanket
annotation for every syscall, every allocator lock, or every foreign call. A
thread that holds an allocator/runtime lock across a SAFE region still has to
make that lock protocol safe. The runtime does not automatically mark syscalls
or allocator critical sections as SAFE.

Critical regions are the opposite assertion: the thread is in the middle of a
multi-word graph modification. `enter_critical` publishes the region under the
registry lock and increments `critical_depth`. While it is nonzero, a `poll`
that sees the request returns without changing ACTIVE or parking, and a
repeated `register` also refuses to park an already-registered ACTIVE thread.
The collector's scan therefore keeps seeing ACTIVE and wait/timeout. Inner
`leave_critical` calls only decrement the depth and never become safepoints.
The outermost leave decrements to zero and, if the request is still active,
publishes PARKED under the lock, notifies outside the lock, and waits for
resume.

`enter_safe` while `critical_depth != 0` is a terminal Panic: SAFE claims the
graph is quiescent, while critical claims a partial write is in progress.
`unregister` inside critical is also Panic. `stop` inside critical returns -16
without setting a request. None of these operations silently reinterpret one
exclusion region as the other.

## 5. Register and unregister ownership

Registration is per kernel tid and idempotent. A repeated `register` returns
false and creates no new node. A foreign callback attached to an already
registered language thread must not call `unregister`: the ABI has no separate
token, and `unregister` is thread-scoped. The bool return exists so a callback
can know whether it created the registration and only then pair it with an
`unregister`.

Consequences, stated so no caller relies on an unprovided guarantee:

* `register` cannot distinguish a nested callback registration from the
  language thread's own registration, because both run on the same tid.
* `unregister` removes the tid's one registration regardless of which nested
  caller invokes it. A callback that ignores the bool can therefore tear down a
  language registration. This is a caller contract violation, not an
  automatically detected condition.
* A thread must call `unregister` before it exits. There is no TLS, destructor,
  or atexit hook, and the runtime does not guess whether a reused tid belongs to
  a dead registration. The process exit path is the only implicit cleanup.
* `unregister` from inside SAFE is Panic, because the thread is still claiming
  graph-exemption and the callback has not paired leave. Leaving SAFE first may
  park during a stop; the callback then unregisters after resume.
* `unregister` from inside critical is Panic for the same reason: the graph is
  still being modified. Leave the outermost critical level first. That leave
  may park during a stop; unregister only after it returns.
* Repeated `register` on a thread that is already registered and critical does
  not park that thread, even if a stop request is active. It returns false and
  the critical region must finish normally.
* The active collector cannot unregister during its stop; doing so is Panic.
  It must `resume` first.

## 6. Stop and resume

`timeout_ns`:

* `-1`: wait without a deadline.
* `0`: perform one scan; succeed if already stopped, otherwise retract and
  return `-110`.
* `> 0`: absolute CLOCK_MONOTONIC deadline `now + timeout_ns`. All futex waits
  use `FUTEX_WAIT_BITSET_PRIVATE` with that absolute timespec. `EINTR` retries
  do not recompute or extend it.
* `<-1`: return `-22` without changing state.

The caller must be registered and outside SAFE and critical. It looks up its
own node, then under the registry lock:

1. If another collector owns an active stop (including a self-reentrant call),
   return `-16` without changing state.
2. If the caller's `critical_depth != 0`, return `-16` without setting the
   request or changing an existing stop. A critical region cannot become a
   collector.
3. Record the collector tid, set `g_stop_active`, atomically set the request
   bit, and notify waiters.
4. Scan the registry. A stop is successful only when every node other than the
   collector is SAFE or PARKED. Registrations that happen after the request are
   inserted PARKED before returning, so they count as stopped.
5. Keep the request set on success. The collector may allocate and use the
   registry; it is exempt from its own stop.

On timeout, the collector clears the request and stop ownership under the lock,
then wakes all futex waiters. Parked mutators, registration waiters, leave-SAFE
waiters, and outermost leave-critical waiters recheck and become ACTIVE. A
timeout cannot leave `g_stop_active` or a parked thread behind. A thread still
inside critical is not parked; it will observe the retracted request when its
outer leave runs.

Every wake goes through `notify_waiters`. It checks the `FUTEX_WAKE_PRIVATE`
result: a nonnegative value (including zero woken) is success, `EINTR` retries,
and any other negative result is a terminal `futex-wake-failed` Panic. A failed
wake means parked threads may never observe the epoch change, so stop, retract,
and resume must not return success in that state.

Only the recorded collector can `resume`. Resume clears the request and stop
state under the lock and wakes every waiter. It does not run ARC drops or
otherwise mutate user objects. If the wake fails, resume Panics instead of
returning a false success.

If the collector exits without resume, the world remains stopped by design.
The collector must pair every successful stop with resume.

A critical mutator can make a positive stop timeout expire. Retraction clears
the request and stop state while the mutator is still critical. When its outer
leave later runs, the request is already gone, so it returns ACTIVE without
parking on a stale request. A successful stop cannot occur while any other node
is still ACTIVE-with-critical-depth; the collector keeps scanning until the
outer leave publishes PARKED (or the timeout retracts).

### 6.1 `can_collect`

`lang_gc_world_can_collect_v1` is a read-only query. It returns true exactly
when all of these hold under the registry lock:

* no stop is currently owned (`g_stop_active == 0`);
* the current tid has a registered node;
* that node is ACTIVE;
* `safe_depth == 0`;
* `critical_depth == 0`.

Unregistered threads return false and are not auto-registered. The query does
not set the request, does not change state, and never resumes, releases, or
consumes a successful stop. In particular, the collector holding a successful
stop gets false, and calling this function does not let it skip its explicit
`lang_gc_world_resume_v1`. All threads are false while another collector owns a
stop, because no second collection can start in that state.

The intended future use is an automatic entry poll deciding whether a
collection may be considered at a critical function entry. It is advisory: a
true result is a snapshot that can become stale immediately, and callers still
need the ordinary request-bit poll for stop participation. It is not a
registration API and must not be used to attach an unknown foreign thread.

## 7. Registry internals and lifetime

* Global state is zero-initialized POD: a singly linked node list, a ticket
  lock, the request word, an epoch futex word, stop/collector fields, and fork
  state. There are no static constructors.
* Nodes come from `lang_alloc_v1(sizeof(Node), alignof(Node))`. Each node has an
  atomic `refs` count, tid, state, safe depth, critical depth, and next pointer.
  The critical depth is registry-local state; no TLS, Root TCB layout, or
  per-thread native TLS is added.
* The registry owns one reference. Lookups under the lock add a reference;
  `unregister` removes the node under the lock and drops the registry reference
  after unlocking. The last release calls `lang_free_v1` outside the lock.
* The registry lock is never held across allocation, free, futex wait, or a user
  callback. The fork-child hook is the one special case: it inherits the lock
  held, resets it, and only then frees obsolete nodes.
* The stop scan uses node fields only while holding the lock and never keeps a
  node pointer across the lock. It therefore cannot race a free. Poll, SAFE
  enter/leave, stop, and resume hold a lookup reference for their own duration.
* Notification is `g_epoch` plus `FUTEX_WAKE` of all waiters. A waiter captures
  the epoch under the lock, rechecks the request under the same lock, then waits
  on the captured value. `EAGAIN` and `EINTR` loop.

## 8. Fork hooks

These hooks are **not** wired into `process.cpp` in this task. They are separate
functions with invocation preconditions.

Required ordering:

```
lang_alloc_fork_prepare_v1();      // existing allocator lock
lang_gc_world_fork_prepare_v1();   // registry lock + fork state
pid = fork();
parent:
  lang_gc_world_fork_parent_v1();  // release registry lock
  lang_alloc_fork_parent_v1();     // release allocator lock
child:
  lang_alloc_fork_child_v1();      // reset allocator lock first
  lang_gc_world_fork_child_v1();   // prune/free registry nodes
```

The allocator must be bracketed because the child hook frees inherited registry
nodes with `lang_free_v1`. Calling only the GC hooks while another thread holds
the allocator lock would deadlock in the child. If `fork()` itself fails, the
caller must still call the parent hooks.

Prepare preconditions and constraints:

* The caller must be registered.
* It must not already be parked.
* A non-collector caller must not fork while another thread owns an active
  stop unless the caller is inside a critical region. The non-critical case
  deliberately rejects the ambiguous "safe foreign thread forks during
  collection" scenario with Panic `fork-active-stop`.
* A caller inside a critical region is allowed, including while another
  collector's stop is pending. The intended future fork path enters critical
  before taking launch/environment/allocator locks so a stop cannot park it
  mid-acquisition. The child preserves the surviving thread's critical depth
  and clears the inherited stop; the parent keeps its own depth and the
  collector's pending stop.
* The collector may fork during its own stop. The child clears the inherited
  stop; the parent keeps it.
* Forking from inside SAFE is allowed only when no foreign collector owns a
  stop. The child keeps the surviving thread's SAFE depth.
* Prepare keeps the registry lock held until exactly one of the parent/child
  hooks runs. Do not call registry or allocator entry points between prepare
  and that matching hook; they would wait on the held locks.

Child behavior:

* The forking thread's node is identified by the parent tid recorded in
  prepare. All other nodes (threads that do not exist in the child) are removed
  from the list.
* The surviving node's tid is changed to the child's new `gettid()`. Keeping the
  parent tid would make every later lookup fail.
* The surviving node's `critical_depth` is copied unchanged. The state remains
  ACTIVE when `safe_depth == 0`; critical is not a SAFE state.
* Stop/collector state and the request word are cleared; the epoch is reset; the
  inherited ticket lock is reset to next=serving=0. No inherited lock remains.
* The allocator fork-child hook has already reset the allocator lock, so removed
  nodes can be freed. The surviving node keeps its one registry reference.
* The child does not simulate ARC drops for vanished threads. It only reclaims
  registry bookkeeping memory. Object graph cleanup for vanished threads is
  explicitly out of scope.
* Calling the parent hook in the child or the child hook in the parent is
  detected (the child's tid differs from the recorded parent tid) and Panics
  with `fork-hook-misuse`.

## 9. Panic and error results

Misuse Panic writes a terminal record to stderr and exits 101 through
`exit_group`:

```
LANG_NATIVE_PANIC:v1:gc-world:<reason>:<tid>:0:0
```

Reasons implemented here: `allocation-failed`, `clock-failed`,
`futex-failed`, `futex-wake-failed`, `gettid-failed`, `state-corruption`,
`register-parked`, `poll-unregistered`, `safe-unregistered`,
`safe-depth-overflow`, `safe-while-parked`, `leave-without-enter`,
`critical-unregistered`, `critical-in-safe`, `critical-while-parked`,
`critical-depth-overflow`, `safe-in-critical`, `unregister-in-critical`,
`leave-critical-without-enter`, `stop-unregistered`, `stop-in-safe`,
`stop-parked`, `unregister-collector`, `unregister-in-safe`,
`unregister-parked`, `resume-unregistered`, `resume-not-owner`,
`fork-unregistered`, `fork-active-stop`, `fork-parked`, `fork-hook-misuse`,
`fork-no-current`.

`stop` returns `-16` (EBUSY) for collector competition/self-reentry or when the
caller is inside a critical region, `-22` (EINVAL) for `timeout_ns < -1`, and
`-110` (ETIMEDOUT) after a complete retraction. The critical `-16` path does not
set or change the request. Unexpected futex/clock failures are terminal Panic
rather than a silent spin.

`can_collect` is a boolean query, not a Panic path for unregistered threads:
an unknown tid returns false and is left unregistered. A gettid failure is still
the same terminal `gettid-failed` Panic as the other entry points.

`current_tid` uses a raw gettid syscall. If it fails or returns a nonpositive
value, it calls `gc_panic_with_tid("gettid-failed", 0)`. That helper writes the
record and calls `exit_group` without using gettid, so a seccomp-denied gettid
produces an ordinary Panic record with tid 0 instead of SIGILL. The general
`gc_panic` helper also treats a failed diagnostic gettid as tid 0 and never
recurses through the API path.

## 10. Explicitly not implemented by this task

The following are `not_run` and must not be reported as integrated behavior:

* LLVM code generation of function-entry polls, loop-backedge polls, and
  inlined request branches into real compiled Wz programs.
* Compiler or source-level insertion of `lang_gc_world_enter_critical_v1` /
  `leave_critical_v1` around graph multi-word writes. This task provides the
  registry support and ABI; the write sites are not instrumented.
* Automatic SAFE annotation around runtime blocking operations (file I/O,
  network, condition waits, process waits, etc.).
* A hosted foreign-C-thread auto-attach entry point. Foreign threads must call
  `lang_gc_world_register_v1` explicitly, and unregistered slow-path misuse
  Panics.
* Wiring the fork hooks into `runtime/native/process.cpp` or any language
  `fork` API. The hooks are tested directly.
* Actual tracing collection, object graph traversal, ARC drop for dead threads,
  or claims that memory is automatically reclaimed.
* Non-Linux or non-x86_64 ports.
* Async-signal-safe use of the registry. A signal handler must not call
  registry/SAFE APIs while the interrupted thread may hold the registry lock.

## 11. Verification evidence

`tests/native/gc_world/run_all.sh` is the single entry point. It applies a
per-process `timeout`, builds and runs:

* Hosted multi-thread suite at `-O0` and `-O2` with the real runtime allocator,
  wrapping `lang_alloc_v1`/`lang_free_v1` to prove live allocation count is zero
  at the end.
* The same suite under ASAN+UBSAN with a libc-heap allocation shim so
  LeakSanitizer sees all registry nodes.
* All misuse panic modes in fresh processes, checking exit 101 and the exact
  panic reason, including critical/SAFE confusion, unmatched leave, unregister
  inside critical, and an overflow build with a lowered test-only depth limit.
* Seccomp fault-injection modes in fresh processes under a per-process timeout:
  100,000 idle fast-path polls with gettid denied; register with gettid denied,
  expecting exit 101 and `gc-world:gettid-failed:0`; and resume with
  `FUTEX_WAKE_PRIVATE` denied after a worker is parked, expecting exit 101 and
  `gc-world:futex-wake-failed` with no `RESUME_RETURNED` output.
* A production `-ffreestanding` object, plus a `ld -static` ELF with no libc and
  no dynamic section. The ELF creates two raw clone threads, stops/resumes them,
  and verifies the graph counter.
* A GCC freestanding object compile as a second compiler check when GCC is
  available.

The host suite covers: multiple mutators and stable counter during a successful
stop; resume continuation; hundreds of consecutive stops/resumes; SAFE blocking
without delaying the stop; outermost leave parking until resume; nested SAFE;
registration during an active stop; registration timeout wakeup; register/stop
and unregister/stop races; timeout retraction; EINTR-resistant monotonic
deadline; two-collector competition and self-reentry; repeated same-tid
register/unregister; thread exit cycles; normal fork pruning/tid remap; and
collector fork during its own stop.

Critical-region coverage additionally includes: a mutator holding a
half-written graph flag and nested critical levels while another thread's stop
must not succeed until the outer leave; inner leave not becoming a safepoint;
poll deferring without parking; a long critical region causing stop timeout and
retraction without parking on the stale request at exit; stop returning -16
from critical without setting a request; `can_collect` false for critical,
SAFE, unregistered, active-collector, and another-pending-collector states;
`can_collect` not releasing a successful stop; fork from a nested critical
parent preserving both levels and remapping the child tid; fork from a critical
thread while another collector's stop is pending (child clears the stop, parent
parks at outer leave); and an overflow Panic through a test build whose
critical-depth limit is lowered to 2. A simple 100,000-pair enter/leave
benchmark prints `GC_WORLD_CRITICAL_PAIR_NS`; it measures the existing
gettid+registry path and does not change Root TCB/TLS layout. The fork child
reports its post-prune live allocation count through a pipe.

Not covered in this evidence: true kernel pid reuse after pid-space wrap (the
suite exercises repeated exit and same-tid re-registration instead), a real GC
collector traversing a Wz graph, and compiler/runtime integration listed in
section 10.

The full suite (now 42 checks: the original 30, four seccomp-related checks,
six critical misuse modes, and a critical-overflow build/runtime check) was
rerun with the fixed toolchain:

```bash
CXX=/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/clang++ \
LD_LIBRARY_PATH=/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9/usr/lib \
GC_WORLD_BUILD_DIR=/tmp/gc-world-critical-20261112 \
bash tests/native/gc_world/run_all.sh
```

## 12. Correction history

2026-11-12: An independent seccomp review found two error paths that were not
terminal Panic:

* `notify_waiters` ignored a denied `FUTEX_WAKE_PRIVATE`; a resume could return
  while the parked worker remained asleep. The result is now checked, `EINTR`
  retries, and other negatives Panic with `futex-wake-failed`.
* An unavailable gettid used `__builtin_trap` and produced SIGILL. It now uses
  a gettid-independent terminal panic path with `gettid-failed` and tid 0.

New `gc_world_seccomp_test.cpp` modes reproduce both faults in isolated
processes and also verify the 100,000-iteration idle fast path never issues
gettid.

## 13. Critical-region integration step

2026-11-12: Added the registry-side nested critical region described in
sections 3, 4, 6.1, 8 and 9. `Node.critical_depth`, the three critical ABI
entry points, poll deferral, leave-critical parking, stop's critical BUSY
result, SAFE/critical mutual exclusion, unregister rejection, fork depth
preservation, and the read-only `can_collect` query are implemented and tested.

This step is still registry support only. It does not instrument Wz source
writes, does not emit critical regions from the compiler, does not claim that a
half-written graph is automatically repaired, and does not change the Root TCB
or add thread-local storage. The critical depth lives in the allocated registry
node and is reached by the same gettid lookup as the rest of the registry.
