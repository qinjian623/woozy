# Async fork recovery

This directory covers `std/process.fork` and the internal exec-only spawn path
after the Native async executor has been used. The runtime change keeps the
accepted allocator/Weak/GC fork chain and adds three ordered async steps:

1. A fork reservation acquired outside every internal runtime lock. It makes
   the executor-TID decision atomic with a concurrent first bind.
2. The executor ticket lock, taken last in prepare and released first in the
   parent.
3. A child reset of both ticket words, the gate and the inherited epoll
   descriptor. The child never sends `EPOLL_CTL_DEL/ADD` to the parent's epoll
   instance; `ensure_epoll` lazily creates a private `EPOLL_CLOEXEC` instance
   and re-adds only the pending descriptor waits whose registration intent
   survived the fork.

## Supported

- `std/process.fork` from the executor thread, including from inside a
  coroutine body. Both processes may continue an already-registered descriptor
  wait; the child rebuilds a private epoll and the parent's listener is
  untouched. The child keeps `g_current`, retargets the executor to its
  own raw gettid, resets the ticket words and continues its own copy of the
  ready/await/refs/GC state.
- `std/process.fork` from an unbound thread. The executor stays unbound in the
  child and the first executor call binds there.
- `std/process.fork` on a thread other than a bound executor returns `-EBUSY`
  before creating a child; `std/os.check` reports it as `os.Error.System(16,0)`.
- Hosted user atfork child handlers see the ordinary-fork async state already
  reset. Nested Source and direct libc forks from such a handler are supported
  and covered by `native.async_fork_hosted_nested-user-child`. The runtime
  parent hook releases the Source binding reservation before user parent
  handlers; the parent final-release test also performs a nested direct libc
  fork after that handler runs.
- Internal `process.spawn`/`execve` use an exec-only fork. A foreign executor
  is allowed there, because the child never runs or cleans inherited tasks.
- NoGraph scalar/Unit Tasks take the same async reset path even though the
  program never enables `lang-gc-program/1`.
- A final Task release is independent of the fork binding reservation. It
  unlinks the executor tables while the Task still has one strong reference,
  performs the real GC unregister, and only then publishes zero and runs frame
  cleanup. A fork snapshot may retain a registered Future with a positive
  reference, but never a registered zero-ref node. User prepare/parent callbacks
  may therefore join a worker that drops the last Task reference.

## Explicitly not supported

- Continuing or pausing another thread's executor. A Source fork that would
  require that handoff is rejected with `os.Error`.
- Direct libc `fork()` from a thread other than a bound executor. Its atfork
  prepare is `void`, so it cannot return a Source `os.Error`. It writes
  `LANG_ASYNC_FORK_REFUSED:executor-on-another-thread` and raises a terminal
  runtime Panic instead; no child is created. The same rule applies to
  `pthread_atfork` users that call libc `fork` directly.
- Resuming a task frame that another thread was executing. The runtime only
  copies an executor that belongs to the forking thread, or leaves an unbound
  executor unbound.
- Fabricating ARC drops for threads that disappear in the child. A reference
  held only by a vanished thread remains an ordinary inherited reference; it is
  not collected, and the child does not clear the object on its behalf.
- Running language async code in an internal spawn child before `exec`. The
  exec-only path skips task continuation/cleanup and epoll rebuild. It also
  does not close the inherited async epoll from the unlocked fork window;
  `EPOLL_CLOEXEC` (and `_exit` on exec failure) closes that private descriptor
  without risking a close of an unrelated descriptor whose number changed
  concurrently with the fork.

## Checks

`run_async_fork.py` builds each real-Source fixture at O0 and O2 with the CLI
and requires exact `parent-ok` output plus no panic marker. The
`fork-observer-balance` fixture is also rebuilt through the Native helper and
linked with the existing allocation/Vec observer; the child redirects only its
own stderr after fork, so the runner checks zero live heap/Vec objects and
balanced ALLOC/FREE and VEC events for parent and child separately. The C++ runtime
test has separate process modes for same-thread NoGraph forks, seccomp-denied
fork reservation/ticket rollback, a first-bind/fork race plus foreign-executor
exec-only spawn, and a final release paused just before GC unregister. A test-only linker
wrapper makes the child inherit exactly one registered node and requires its
trace-time refs to equal 1; the parent resumes release and requires exactly one
final frame cleanup. The hosted test covers same-thread
direct libc fork, failed source fork recovery, nested Source/direct forks from a
user child handler, foreign-executor Source rejection, exec-only spawn, the
recorded direct-fork refusal Panic, and permanent user prepare/parent handlers
that join a final-release worker. Those two Source modes run under a real
process-group timeout through `run_hosted_atfork_release.py`; the direct libc
mode is the positive control. Verification rebuilds the hosted test at O0 and
O2 against the same hosted archive for these three modes.
