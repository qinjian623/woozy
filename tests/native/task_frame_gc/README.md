# Native Task/frame GC checks

Real-source checks for the typed per-async-function frame metadata published by
`lang_async_publish_v2`. These are permanent regression expectations.

## Cases

Positive (O0 and O2, output checked, then rebuilt as LLVM and linked with the
existing `tests/native/language_gc_source/observe_gc.cpp` observer):

- `task-move-vec-cycle`, `task-move-closure-cycle`: never-started `move` frame
  owners form a Task frame -> Vec/closure environment -> Task cycle. The
  observer must see every Future, frame and Vec allocated and freed once.
- `task-result-cycle`, `task-error-cycle`: a completed Task's promise-owned
  result/typed-error payload stores the same Task type, producing a
  Future/promise -> payload -> Task cycle.
- `task-live-move-root`: the executor's `kActive` root must keep a running
  move-parameter frame and its live Vec reachable under GC pressure.
- `task-await-chain`, `task-await-scalar`: the two real await-link references
  keep both frames alive; the scalar variant has no Vec and proves that a
  non-null frame trace activates `lang-gc-program/1`.
- `task-never-started-move-cleanup`: `clear_owners` drops both move parameters
  once, never resumes the body and never runs its defer.
- `task-unit-nograph`, `task-scalar-nograph`: provable leaf Tasks emit no frame
  trace and must keep the no-graph startup path (`lang_gc_program_init_v1` is
  not called).

Rejection:

- `async-c-resource-reject`: a move parameter owns a C resource directly.
- `async-c-closure-dyn-order`, `async-c-closure-dyn-reversed`: a move closure
  captures a dyn value whose concrete payload holds a C resource, in both
  declaration orders.
- `async-c-dyn-closure-order`, `async-c-dyn-closure-reversed`: a move dyn value
  has a concrete payload holding a closure that captures a C resource, in both
  declaration orders.
- `async-c-closure-dyn-cycle`: the real Task-cycle shape, where the move closure
  environment holds both the dyn/C-resource value and the Vec<Task> that owns the
  never-started Task.

Every rejection case must report `E_CAPABILITY_UNAVAILABLE` before producing a
program/object, because stopped-world clear cannot run a C release and the whole
frame must not be deferred. The C FFI binding declarations are reused from
`tests/native/c_ffi/bindings`; no C library call or link succeeds in these
cases.

The two propagated flags (`module_closure_owns_c_resource` and
`module_trait_box_owns_c_resource`) are deliberately module-wide conservative
booleans. They are recomputed to a stable fixpoint so declaration/node order
cannot change the result. If any closure or dyn value in the same module can
reach an external C resource, an unrelated async frame slot of the same class
may also be rejected. This is not a precise per-value analysis and is documented
as such.

The observer checks zero live heap allocations, zero live Vec payload and
balanced ALLOC/FREE and VEC:1/VEC:5 streams, not only exit code 0. The LLVM
checks assert `lang_async_publish_v2`, generated metadata/trace/clear symbols,
and the presence or absence of the `lang_gc_program_init_v1` call.

## Running

```bash
python3 tests/native/task_frame_gc/run_task_frame_gc.py \
  --cli "$BUILD/woozyc" \
  --helper "$BUILD/native/lang_native_helper" \
  --runtime "$BUILD/native/liblang_native_runtime.a" \
  --toolchain "$LLVM/bin" \
  --root "$REPO" \
  --out "$OUT" \
  [--libxml2-lib "$LIBXML2/usr/lib"] [--only task-move-vec-cycle] [--opt 0]
```

`tests/native/task_frame_gc/CMakeLists.txt` is the registration fragment for
Root; it is not included by this worktree.

## Upstream runtime input

The GC-aware runtime ABI copied into `runtime/native/async.cpp` and
`runtime/native/async.hpp` is the reviewed input from
`/home/qin/Work/language-dev-20261103/checks/extensions-20261111/task-gc-runtime-accepted-input/`.
It is not new implementation in this task.

| file | SHA-256 |
| --- | --- |
| `async.cpp` | `15e94df45b8d5306724062b7b8c6ea06f6a72454bbb06a5b97e4d3cbc584fd38` |
| `async.hpp` | `4550ca71ca140f5ef7ce61656593175502c8aac358e74b5dd7057c0e978e2c47` |
