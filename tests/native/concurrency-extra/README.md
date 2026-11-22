# CON01 concurrency-extra evidence

This directory is owned by CON01. It contains:

- `rwlock/` and `rwlock-misuse-*/`: pure-Wz tests for `std/rwlock`.
- `cancel/`: pure-Wz tests for `std/cancel`.
- `runtime_timeout_test.cpp`: host C++ evidence for the two new low-level
  timeout entry points, compiled together with the real freestanding runtime.
- `static_timeout_fixture.cpp`: `-nostdlib -static` direct ABI fixture.
- `run.py`: finite-timeout O0/O2 build/run driver for all of the above.

## Low-level ABI (implemented here)

```c++
extern "C" int64_t lang_thread_wait_for_v1(LangHandle *thread,
                                           int64_t timeout_ns) noexcept;
extern "C" bool lang_condvar_wait_for_v1(LangHandle *condvar,
                                         LangHandle *mutex,
                                         int64_t timeout_ns) noexcept;
```

`timeout_ns == -1` waits forever, `0` checks once, values below `-1` are invalid.
The thread call returns `0` on completion, `-110` on timeout, `-22` for an
invalid timeout, and otherwise preserves the existing wait errors (`-3` for a
thread from another process, `-35` for self-join, and raw syscall errors).
The condvar call returns `true` when notified and `false` only on timeout;
misuse and runtime errors Panic. It always releases the external mutex before
waiting and reacquires it before returning, including timeout/error cleanup.

Both use `CLOCK_MONOTONIC` absolute deadlines. Each retry recomputes the
remaining duration, so EINTR/EAGAIN/spurious wakeups do not restart the full
timeout. Huge positive values saturate at a kernel-safe futex duration.

`lang_thread_wait_v1` and `lang_condvar_wait_v1` now delegate to their
`_wait_for_v1` counterparts with timeout `-1`; their existing observable
behavior is unchanged.

## Wz integration status: not_run

Root still owns the public HIR registry and edits to `std/thread`/`std/sync`.
The prepared mapping is:

| Intrinsic | Result | Owner |
| --- | --- | --- |
| `__woozy_thread_wait_for(__woozy_Thread, i64)` | `i64` | `std/thread` |
| `__woozy_condvar_wait_for(__woozy_Condvar, __woozy_Mutex, i64)` | `bool` | `std/sync` |

Suggested source wrappers (Root may choose final spellings):

```wz
pub func wait_for(worker Thread, timeout_ns i64) bool throws os.Error {
    let status = __woozy_thread_wait_for(worker.state,timeout_ns);
    if status == -110 { return false; }
    if status < 0 { throw os.Error.System(code:i32(-status),completed:0); }
    return true;
}

pub func wait_for(c Condvar, m Mutex, timeout_ns i64) bool {
    return __woozy_condvar_wait_for(c.state,m.state,timeout_ns);
}
```

Until those registry entries and source wrappers exist, the Wz timeout cases
are `not_run`; the C++ runtime test is the executed evidence for the ABI.

## Run

```sh
python3 tests/native/concurrency-extra/run.py \
  --cli /path/to/woozyc --root . --out /tmp/con01-results \
  --clang /path/to/clang++
```

At both O0 and O2, the driver checks static ELFs (no `INTERP`/`NEEDED`), normal
exits, and expected misuse Panics (`LANG_NATIVE_PANIC:v1:runtime-handle-misuse`);
it also builds and runs `runtime_timeout_test.cpp` and the `-nostdlib -static`
`static_timeout_fixture.cpp`.
