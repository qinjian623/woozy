# async_memory Native regression tests

These are real-source Native checks for async Task/frame lifetime and GC. They
are permanent expectations, not a snapshot of the current implementation.

## Cases

- `async-param-lifetime`: a normal `Vec` async parameter stays owned and valid
  after the caller's block drops its handle.
- `task-unstarted`, `task-completed`, `task-typed-error`, `task-cancel`,
  `task-shutdown`: all Future, frame and promise/error allocations are released.
  The typed-error fixture carries a `Vec[i32]` payload, catches `42`, re-awaits
  the same errored Task and reads `42` again.
- `double-await-vec`: repeated await returns ordinary ARC copies of an owning
  Vec result without transferring the promise value twice.
- `double-await-struct`: scalar fields copy by value while a `Vec[i32]` field
  remains shared; changing `first.values[0]` is visible through `second`.
- `double-await-string`: `get` calls `strconv.format_i64` and then `+` to build
  a dynamic owning string. The first handle leaves scope before the second
  await; both must remain readable and the allocations must balance.
- `task-live-root`: an executor/source-held Task is a GC root while running;
  GC pressure must not collect its frame or result.
- `task-vec-cycle`, `task-closure-cycle`: never-started pure Vec<Task> and
  Task/closure/Vec cycles are unreachable garbage and must be reclaimed.

The runner also requires the LLVM for the dynamic owning payload fixtures to
call the owning constructor inside the same async frame function: string uses
`lang_text_concat_v1`, and struct/error fixtures use `lang_vec_new_v1`. This
prevents a literal or scalar-only fixture from passing vacuously.

Every case is run at O0 and O2. The runner first checks the app exit code,
stdout and stderr. It then emits LLVM through the Native helper and links the
existing `tests/native/language_gc_source/observe_gc.cpp` observer. The observer
must report zero live heap allocations, zero live Vec payload, balanced
ALLOC/FREE and VEC:1/VEC:5, with no duplicate or unknown free. Missing frees fail
the final heap balance. Each command runs in its own process group; on timeout
the runner kills the group so CLI-spawned helper/compiler descendants do not
survive.

## Running

```bash
python3 tests/native/async_memory/run_async_memory.py \
  --cli "$BUILD/woozyc" \
  --helper "$BUILD/native/lang_native_helper" \
  --runtime "$BUILD/native/liblang_native_runtime.a" \
  --toolchain "$LLVM/bin" \
  --root "$REPO" \
  --out "$OUT" \
  [--libxml2-lib "$LIBXML2/usr/lib"] [--only task-vec-cycle] [--opt 0]

# parser + timeout process-group self-test, no build artifacts required
python3 tests/native/async_memory/run_async_memory.py --self-test
```

`--out` is explicit and is never a fixed `/tmp` path. The CMake fragment in
`CMakeLists.txt` registers `native.async_memory` without modifying the root
CMake files and needs only `LANG_TEST_PYTHON` plus the Native targets.

## Current baseline

The ordinary-parameter copy candidate passes 20 of the 24 O0/O2 runs in the
independent 2026-11-13 review. `task-vec-cycle` and `task-closure-cycle` still
fail at both levels because Future/frame nodes are not yet traced by GC. The
closure environment now stays alive, so its full cycle actually forms. These
failures stay failures in the runner.

The CMake fragment is not included by the Root build until those production
changes pass this suite. Existing full CTest results do not cover these pending
cycle checks. The separate async_parameter_copy suite uses this runner's
allocation parser and requires its own source cases to balance every allocation.
