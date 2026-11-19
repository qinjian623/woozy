# Capturing C registration runtime tests

C++ runtime harnesses for the private hosted ABI
`runtime/hosted/c_callback.hpp` and the implementation
`runtime/hosted/c_callback.cpp`. They exercise the frozen contract from
[development/docs/C_RETAINED_CAPTURING_CALLBACKS.md](../../../docs/C_RETAINED_CAPTURING_CALLBACKS.md)
without a source compiler, an executable trampoline, libffi or a global
"current closure" slot.

CTest names: `native.c_callback_runtime`, `native.c_callback_process`
(fragment `CMakeLists.txt`; Root registers it).

## Fixture

`c_callback_fixture.hpp` models one ordinary C library registration API:

* `lang_c_registration_new_v1` ACTIVE before the C adapter, then
  `lang_c_registration_finish_v1` with the raw token after it. The C library
  stores the context as userdata and may call back synchronously.
* The dispatch helper has the generated typed-bridge shape: the existing
  32-byte GC callback scope surrounds `Fn.code`, the private 16-byte runtime
  token is separate, `Fn.code` receives `env` first, and runtime leave runs
  after the GC callback scope.
* `fake_unregister` is the quiescent foreign stop. The closure environment is a
  real `LangVec` whose element drop callback records the final external-root
  drop.

## Coverage

`c_callback_runtime_test.cpp`:

* synchronous registration callback and 4 foreign threads x 200 callbacks;
* exactly-once env drop and unregister, explicit close idempotence, and the
  final owner release;
* close blocking on a real active callback (unregister already returned) and on
  a real foreign-call lease (drained before unregister);
* close/release called from an **outer SAFE region**: teardown uses the existing
  GC cleanup scope, so the waits/foreign stop are nested SAFE and the env drop
  (and its destructors, asserted with an enter/leave critical region) is ACTIVE,
  then the caller's prior SAFE depth is restored;
* a concurrent follower closer that waits SAFE while the leader is blocked
  inside the foreign stop, with a real 30 ms stop-the-world collection
  succeeding (`--wrap=pthread_cond_wait` proves the wait was reached);
* nested borrow from an existing callback while close is pending, and a
  callback dispatched from inside the foreign unregister;
* same-thread env-destructor re-entry after detach (no-op, no early free);
* null `finish` failure cleanup (one env drop, no unregister);
* collector deferred release: the retained env stays an external root while the
  world is stopped, and unregister/drop run only after resume with the
  controller busy phase cleared.

`c_callback_process_test.cpp` (each rejected case is forked and its terminal
Panic record is checked):

* inherited implicit release is a total no-op (no reference, env, token or
  unregister work), inherited callback leave restores TLS only, and a new
  child-created context works normally;
* inherited explicit close / borrow / callback entry / successful or failed
  registration finish Panic with the inherited marker before any context lock;
* closing one's own callback Panics before waiting;
* an unrelated new lease while close is pending Panics;
* re-entering close before the foreign stop finishes Panics;
* entering a null or already-closed registration Panics (a callback is never
  silently skipped);
* popping a TLS token out of LIFO order Panics (callback and lease stacks).

## Implementation notes relevant to integration

* `close_v1`/`release_v1` may be invoked ACTIVE or from inside the generated
  adapter's SAFE wrapper. The runtime enters one existing GC cleanup scope
  (ACTIVE work) for the whole teardown: lease/callback waits and the foreign
  unregister nest their own SAFE region, every context mutex is released before
  that SAFE region ends, and the env root is dropped ACTIVE outside every host
  lock. Leaving the cleanup scope restores the caller's prior SAFE depth. A
  release still offers the retained external root to the collector's deferred
  queue before any cleanup scope is created.
* `enter_v1` never returns null: a null context, an unpublished or an
  already-unregistered registration is a terminal misuse Panic. Callbacks stay
  admitted while the foreign unregister runs. The bridge therefore needs no
  skip/null branch.
* `borrow_v1(null, token)` returns null and zeroes the token, matching nullable
  C resource results; `end_borrow_v1` is then a no-op.
* Every nonempty TLS token must be the current top before `leave_v1`/
  `end_borrow_v1` pops it; an out-of-order pop (leaving an outer callback while
  a nested one is active) is a terminal misuse Panic.
* An explicit close tears down once but keeps the private context allocated for
  the Source owner: the final `release_v1` frees it. This makes explicit close
  idempotent and keeps concurrent closers safe.
* Terminal Panic markers (cause `runtime-handle-misuse`, snapshot word):
  `1` inherited registration, `2` self-wait, `3` closed/unavailable registration
  use (enter/borrow), `4` host synchronization failure, `5` close re-entry
  before the foreign stop finished, `6` invalid handle/token misuse,
  `7` counter corruption, `8` out-of-order TLS token pop. Allocation failure
  uses the shared `allocation-failed` cause.

## Running

```sh
timeout 900 tests/native/c_callback_runtime/run_callback_tests.sh
```

The script builds the hosted archive from the published manifest sources plus
`runtime/hosted/c_callback.cpp` with the canonical LLVM 22.1.8 toolchain,
`LD_LIBRARY_PATH=${WOOZY_TOOLCHAIN_ROOT}/llvm-22.1.8/lib:${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9/usr/lib`,
links both harnesses against it and runs them with timeouts. It writes to a
temporary directory by default; set `C_CALLBACK_BUILD_DIR` to keep artifacts.

Source-level checks from the contract (O0/O2 capture escape, Vec contents,
function variable, bound method, userdata positions, bool/scalar/Unit returns,
automatic close, GC-triggered cleanup, rejection of throws/mode/signature/schema
errors, Python rejection, default core ELF independence) need the compiler
integration and are **not_run** here. The root header, the language contract and
the shared C resource/callback ABI are used unchanged.
