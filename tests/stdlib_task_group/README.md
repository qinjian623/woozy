# std/async scoped gather

`stdlib/async/async.wz` exposes one batch API:

```wz
pub func gather[T,E](tasks Vec[Task[T,E]])
    Task[result.Result[Vec[T],E],Unit]
```

Call it as `await a.gather[i32,Failure](tasks)`. The call is a normal function,
not an async declaration, so it snapshots the input vector and enrolls every
member in the runtime scoped group **before** the returned `Task` handle can be
cancelled. `join_all` is unchanged.

`or_throw(await gather(...))` converts the settled `Result` to a typed `throws
E` at the call site. The Result form is required because this compiler still
rejects an async function declaring `throws E` for a type parameter.

## Contract

- The input `Vec` is snapshotted synchronously. The exact snapshot is used for
  both scope enrollment and the body, so the caller may clear, resize or replace
  its own `Vec` before the group is awaited or spawned.
- Members are activated by their actual runtime kind before the first wait:
  coroutine Tasks use the ordinary spawn path, timer Tasks are already
  scheduled, and a descriptor Task is registered once even when the identical
  handle is enrolled repeatedly. A precompleted member is a no-op.
- Values are returned in input order. The body waits on a runtime completion
  notification, never on a periodic `sleep(1)` status scan.
- A typed failure cancels every unfinished member and the group returns `Err`
  only after all member defers/cleanup have run. Sibling cancellations caused by
  that failure never replace the typed error. A terminal typed error found in
  the initial scan also stops unstarted siblings from being started.
- A member cancellation does **not** erase a real typed error observed before
  or during the drain. The first observed error wins; if several members are
  already complete in one scan, the lowest input index wins. Member
  cancellation is propagated only when no real typed `E` exists.
- If the initial scan already observed a member cancellation, every pending
  member is cancelled rather than started. An already-running member can still
  publish a real typed error during that drain, and that error keeps priority.
- External cancellation of the group or its parent cancels and drains every
  member, then propagates cancellation instead of a result.
- `Unit` values and an empty batch work. `Ok([])` is returned for an empty batch.
- A `Task` may be enrolled in multiple groups and may appear more than once in
  one group. Each enrollment is one runtime edge and contributes exactly one
  completion decrement.

## Scoped cancellation semantics

The runtime additions are additive and opt-in:

- `lang_async_scope_begin_v1(group)`
- `lang_async_scope_add_v1(group, member)`
- `lang_async_scope_activate_v1(member)` (kind-aware, idempotent activation)
- `lang_async_scope_wait_v1()` (completion notification for the running group)
- existing `lang_async_status_v1` for the Wz member-status builtin

Only a group marked by `scope_begin` gets scoped behaviour. Ordinary shared
`Task` await/cancellation semantics are unchanged.

Each completion bumps a group generation under the executor lock and completes
or wakes the linked notification leaf. The body consumes the generation when it
registers its wait, so a completion between its status scan and the wait call is
observed immediately instead of being lost. A linked notification leaf owns no
frame, is registered only so shutdown can cancel it, and is detached and
completed when the group finalises or is cancelled.

The runtime retains one member reference per enrollment. The member only keeps a
borrowed back-link to observing groups. On cancellation the group requests every
pending member's cancellation and stays pending until its own frame and all
member frames finish their cancel exit plans. If a caller is suspended on such a
group and is cancelled, the runtime cancels the group but leaves the caller
linked; the caller is woken only after group and member cleanup finish. A group
that never started is marked terminal without resuming its frame and follows
the same child-cleanup wait.

Dropping the last source handle of an unstarted, uncancelled group releases its
member edges but does not implicitly cancel member Tasks, matching ordinary last
handle behaviour. Cancelling the group requests cancellation of its pending
members. Cancelling a caller waiting on the group does the same and waits
for member cleanup. The GC trace callback reports every group-to-member edge; the
borrowed member-to-group back-links are not traced or counted as references.

Operation numbers are `222` (`scope_begin`), `223` (`scope_add`), `224`
(`scope_activate`), `225` (`scope_wait`) and `226` (member status). Main reserves
`216`-`221` for Linux descriptor-relative FS operations, so the scope block
starts at `222` and leaves that range untouched. The C++ enum pins the same
values and the Wz table is in `selfhost/src/wzcc/system/system.wz`; lowering is
in `selfhost/src/wzcc/nativeemit/e_system.wz` and the C++ backend case is in
`src/backend/native/emit.cpp`.

## Tests

`run_tests.py` creates an isolated compiler prefix from the candidate compiler,
replaces its stdlib with this checkout's stdlib, optionally installs a supplied
native runtime archive, and builds every Source fixture at O0 and O2.

```bash
python3 tests/stdlib_task_group/run_tests.py \
  --compiler /path/to/new/woozyc \
  --runtime /path/to/liblang_native_runtime.a \
  --out /tmp/stdlib_task_group-evidence \
  --stage-dir /tmp/stdlib_task_group-stage
```

Each fixture is built with:

```bash
<stage>/bin/woozyc build --project <fixture>/lang.toml \
  --backend native --opt <0|2> --out-dir <evidence>/builds/<case>_O<opt>
```

and executed at `<evidence>/builds/<case>_O<opt>/program`.

Coverage:

- success order, empty group, `Unit` values
- slow-first/fast-fail in both input orders; first typed error preserved across
  group-initiated sibling cancellation; sibling defers run before the caller
  catches `E`
- mixed success and failure; completion-order and lowest-index tie policies
- caller mutates the input Vec after `gather` (success and unstarted cancel)
- external member cancellation drains the remaining members
- two overlapping groups and duplicate enrollment, including a cancellation
  case that would expose a double completion decrement
- raw timer members, mixed timer/coroutine batches, descriptor (pipe) members,
  and duplicate task/fd handles
- a member completed before `gather` plus a pending member activated afterwards
- terminal-failure or terminal-cancellation activation policy: an unstarted
  sibling is cancelled rather than started
- both error-vs-cancel input orders, including a running sibling cancellation
  and a real typed error observed in the same drain scan
- a first-scan cancellation cancels an unstarted coroutine sibling without
  running its body
- outer cancellation before a would-be error, and cancellation-only propagation
- `run_until(parent)` alone waits for member cleanup before the parent completes
- ordinary `Task` await cancellation still completes the parent before an
  ordinary child's cleanup
- runtime proof that awaiting a cancelled ordinary task cancels the waiter
- compiler reject for async generic `throws E`

Linux x86_64 is the executed target. Darwin arm64 remains `not_run` in this
environment: execution needs a Darwin host, and the Linux bundle has no
`ld64.lld`. The source-make object path also rejects async functions.
