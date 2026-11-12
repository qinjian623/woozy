# CON01 verification report

Status: **implemented and checked**, including Root source-wrapper integration.
The low-level ABI, `std/rwlock`, `std/cancel`, and all standalone tests below
are implemented and actually executed.

Environment: Linux x86_64, LLVM/clang/ld.lld 22.1.8, legacy libxml2 2.13.9,
compiler built from this worktree into `/tmp/con01-build`.

## Commands and results

```sh
# Worktree compiler build (LLVM 22.1.8)
cmake -S . -B /tmp/con01-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLANG_ENABLE_LLVM=ON -DBUILD_TESTING=OFF \
  -DCMAKE_C_COMPILER=.../llvm-22.1.8/bin/clang \
  -DCMAKE_CXX_COMPILER=.../llvm-22.1.8/bin/clang++ \
  -DLLVM_DIR=.../llvm-22.1.8/lib/cmake/llvm \
  -DCMAKE_PREFIX_PATH=.../toolchains/build/zstd
ninja -C /tmp/con01-build woozyc lang_native_helper
# passed: 96/96 steps

python3 tests/native/concurrency-extra/run.py \
  --cli /tmp/con01-build/woozyc --root . --out /tmp/con01/runner-final \
  --clang .../llvm-22.1.8/bin/clang++ --lld .../llvm-22.1.8/bin/ld.lld
# passed: 18 Wz O0/O2 builds, 26 executions, 2 runtime O0/O2 builds,
#         6 runtime runs, 2 -nostdlib O0/O2 builds, 6 static runs

python3 tests/native/run_concurrency.py \
  --cli /tmp/con01-build/woozyc --root . --out /tmp/con01/existing-concurrency
# passed: 18 O0/O2 builds; 30 executions; no libc; Python rejected

python3 tests/native/check_runtime_symbols.py \
  --archive /tmp/con01-build/native/liblang_native_runtime.a
# {"status": "passed", "unresolved": ["main", "lang_vec_observe_v1"]}

# Existing host regressions linked against the rebuilt runtime archive:
# thread_runtime_test  -> passed (joins, ARC, self-join, detached mmap cleanup, 5 Panic)
# sync_runtime_test    -> sync-runtime-ok
# process_runtime_test -> process runtime: ok
# text_runtime_test    -> passed (ARC/thread readers/Panic)
```

Wz coverage: parallel readers; writer mutual exclusion and 4000-update
read-modify-write counter; writer waiting blocks a new reader and the writer
acquires first; recursive read/write, upgrade, downgrade, unowned unlock, and
wrong-thread unlock all exit 101 with
`LANG_NATIVE_PANIC:v1:runtime-handle-misuse:0:0:0`. Cancel coverage: repeated
independent requests, copied tokens, repeated `check` throws, cooperative
worker polling, idempotent request, and normal `defer`.

Runtime coverage: completed zero wait, running zero wait, positive timeout,
`-1`, invalid `-2`, huge `INT64_MAX`, concurrent joins, self-join, signal/EINTR
storms (deadline not restarted), condvar timeout/zero/infinite/notify, timeout
reacquires the external mutex, timed waiters can be destroyed cleanly, and
misuse Panics. The `-nostdlib -static` fixture calls the exact C ABI directly.

## Public source integration

Root integrated `thread.wait_for` and `sync.wait_for`. The real Wz O0/O2
fixture in `tests/native/concurrency-timeouts/` passed, including timeout,
completion, invalid duration, self-join, mutex reacquisition and condition
predicate/deadline loops. `native.concurrency_timeouts` is registered in CTest;
the integrated suite passed 122/122. These results supplement the original
standalone worktree commands above.
Weak/GC can rely on the two new exported runtime symbols and the unchanged old
thread/mutex/condvar ABI.
