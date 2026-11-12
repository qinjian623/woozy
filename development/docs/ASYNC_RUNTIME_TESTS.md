# Async runtime lifetime tests

Focused regression tests for `runtime/native/async.cpp` (the `lang-async/1`
executor). They compile the real translation unit into the test binary and
drive it through the same entry points the Native emitter emits, so no runtime
archive, LLVM helper or compiler build is required.

Run everything (default output directory `tests/async_runtime/.build`):

```bash
tests/async_runtime/run_tests.sh
```

The script uses the fixed LLVM 22.1.8 toolchain by default and exports the
legacy libxml2 runtime needed by that `ld.lld`; override with `CXX`,
`LANG_LLVM_ROOT`, `LANG_LIBXML2_ROOT` and `ASYNC_RUNTIME_OUT`. It builds and
runs:

* `async_runtime_lifetime_test` with ASan+UBSan at `-O1` (primary evidence),
* the same test at `-O0` and `-O2` without sanitizers,
* `review_counterexamples.cpp`, the literal copy of the external review's ASan
  harness (`runtime_lifetime_asan.cpp`) in both of its modes,
* a freestanding `-c` build of `async.cpp` with the exact `lang_native_runtime`
  flags plus an undefined-symbol check.

A single scenario can also be run directly:

```bash
clang++ -std=c++20 -O1 -g -fsanitize=address,undefined \
  -fno-exceptions -fno-rtti tests/async_runtime/async_runtime_lifetime_test.cpp \
  -o /tmp/async_lifetime
/tmp/async_lifetime cancel-waiter-then-child-complete
```

CMake registration is owned by the Root integration:
`add_subdirectory(tests/async_runtime)` inside `BUILD_TESTING`. This directory
already contains the `CMakeLists.txt` target (`native.async_runtime_lifetime`).

Optional end-to-end parity against the real `tests/async` fixtures: copy the
compiler's runtime archive, replace only `async.cpp.o` with a fresh build of
this tree's executor, and relink the fixtures:

```bash
python3 tests/async_runtime/relink_source_fixtures.py \
  --woozyc <build>/woozyc \
  --runtime <build>/native/liblang_native_runtime.a \
  --toolchain /path/to/llvm-22.1.8 \
  --libxml2-lib /path/to/libxml2-legacy-2.13.9/usr/lib \
  --out /tmp/async-source-parity
```

## Scenarios

| Scenario | What it pins down |
| --- | --- |
| `unstarted-release-then-shutdown` | review counterexample 1: `new` -> `release` -> `shutdown` removes the registry node |
| `cancel-waiter-then-child-complete` | review counterexample 2: a cancelled, completed and released waiter is detached from the child's list |
| `cancel-suspended-task` | cancel wakes a suspended frame exactly once and runs the cancel exit plan |
| `drop-handle-while-suspended` | the executor reference keeps a started frame alive after its `Task` handle is dropped; shutdown still cancels it |
| `cancel-queued-before-first-run` | a queued task cancelled before its first resume never runs its body and is unlinked from the ready queue |
| `drop-queued-task-then-shutdown` | shutdown traversal survives the registry successor being freed by the current cancel |
| `multiple-waiters` | every waiter of one target is woken once and the list is cleared |
| `multiple-waiters-cancel-one` | cancelling one waiter does not disturb the others |
| `repeated-cancel-and-release` | repeated cancel is a no-op after completion; the frame is destroyed once |
| `task-copies-and-cancel` | `retain`/`release` copies plus cancel stay balanced |
| `completed-task-release` | completion unregisters once; the final release does not unregister again |
| `unspawned-drop-without-shutdown` | an unpublished (never resumed) frame is destroyed without releasing an uninitialized promise |
| `await-cancelled-task` | cancelling a task target wakes and cancels the waiting parent |
| `sequential-awaits` | a frame that awaits two targets keeps both links balanced |
| `fd-waiter-cancel` | epoll registration is dropped when the descriptor wait is cancelled |
| `fd-ready` | a ready descriptor completes the wait and unregisters the fd |
| `timer-dropped-before-expiry` | a dropped pending timer leaves no registry node |
| `timer-expires-then-release` | the timer deadline is driven through the blocking wait |
| `shutdown-twice` | shutdown is idempotent |
| `promise-readable-after-completion` | the promise stays readable until the last handle drops (what the emitter's await re-check needs) |
| `source-cancel-fixture-ownership` | the real `tests/async/cancel` ownership shape leaves no registry residue |
| `gc-safe-around-epoll-wait` | the blocking `epoll_wait` enters the runtime SAFE guard, with an empty ready queue and no current frame |

Every scenario ends with an invariant check: only pending futures are
registered, the pending-task counter matches the registry, waiter links are
bidirectional and never reference completed frames, the ready queue only holds
pending futures, the ready tail is consistent, and all allocations created by
the scenario were released.

## Leak accounting

LeakSanitizer reports nothing on this host (a plain `malloc` leak also goes
unreported), so the test allocator counts live blocks itself. All scenarios
release their handles and require zero live blocks.

The one documented residual is in `source-cancel-fixture-ownership`: the
emitter still does not drop ordinary source `Task` values (only the entry
task's handle is released at process exit). Six source-owned futures remain
allocated after `shutdown` — the two task futures and the four timers, i.e. the
same six 120-byte `Future` records the external observer saw for
`tests/async/cancel`. They are *not* registry residue: `g_all` is empty after
shutdown, so no later executor pass can walk them. The reference counts are
exact, which the scenario proves by releasing the six handles and reaching zero
live blocks. Source-level `Task` copy/drop integration is a separate backend
task (Root integration); this directory must be updated to expect zero live
blocks once that lands.
