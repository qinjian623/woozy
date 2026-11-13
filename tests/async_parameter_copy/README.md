# async parameter copy Native tests

These are real-source Native checks for ordinary async owning parameters. The
frame takes a private owning copy during Task creation, before the initial
suspension, so the caller's block may end before the Task is started or resumed.

## Cases

- `param-vec-late-start`: `make` owns a `Vec[i32]`, creates the Task and returns
  it. The caller starts the Task only after `make`'s block has ended.
- `param-string-await`: a dynamic `string` built by `format_i64` and `+` survives
  the caller block and the `await` suspension.
- `param-struct-enum-await`: owning `struct` and `enum` parameters copy their
  `Vec` fields recursively.
- `param-fn-capture-await`: a capturing function parameter retains its closure
  environment.
- `param-weak-await`: a `Weak[Vec[i32]]` parameter retains its control block
  while a strong `Vec` parameter is also frame-owned.
- `param-dyn-await`: a `dyn Trait` parameter clones its boxed concrete value.
- `param-throw`: the typed-error exit drops the frame-owned parameter once and
  keeps the promise-owned error payload separate.
- `param-cancel`: after suspension, cancel runs the source defer and then drops
  both ordinary parameters exactly once.
- `param-cancel-queued`: cancel a queued Task before its body runs; the
  first-resume cancel entry drops the copied parameter through the common final.
- `param-unstarted`: destroying a never-started Task releases the private
  parameter copy through the initial-suspend destroy edge.
- `reject-resource-copy`: a C resource with no `retain_symbol` has no ordinary
  copy operation and is rejected with `E_CAPABILITY_UNAVAILABLE` instead of
  borrowing the resource across suspend.

Every case is built by the published Native CLI at O0 and O2, executed, then
emitted as LLVM and linked with `tests/native/language_gc_source/observe_gc.cpp`.
The observer must report zero live heap allocations, balanced ALLOC/FREE and
VEC:1/VEC:5, and no duplicate or unknown free. Missing frees fail the case.

## Running

```bash
python3 tests/async_parameter_copy/run_parameter_copy.py \
  --cli "$BUILD/woozyc" \
  --helper "$BUILD/native/lang_native_helper" \
  --runtime "$BUILD/native/liblang_native_runtime.a" \
  --toolchain "$LLVM/bin" \
  --root "$REPO" \
  --out "$OUT" \
  [--libxml2-lib "$LIBXML2/usr/lib"] [--only param-weak-await] [--opt 0]
```

The CMake fragment registers `native.async_parameter_copy` without modifying the
root CMake files. It needs only `LANG_TEST_PYTHON` plus the Native targets.

## Current baseline

On the ordinary-parameter-copy implementation, all 22 O0/O2 runs pass. In the
separate `native.async_memory` suite, 20 of 24 runs pass: `async-param-lifetime`
now passes at O0/O2, while `task-vec-cycle` and `task-closure-cycle` remain
failed at O0/O2 because Native Task/frame nodes are not yet traced by GC. The
closure cycle now really retains the function environment; this suite does not
mask that missing GC integration.
