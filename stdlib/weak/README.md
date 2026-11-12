# std/weak

`Weak[T]` observes a canonical shared identity without a strong reference.
The frontend accepts only these targets, selected by their resolved
declaration, not by name or layout:

- `Vec[T]`
- function values whose source form is currently a non-capturing `Fn`.
  The type-eligibility contract includes function values generally; captured
  Fn targets remain rejected by the current capture rule until LANG01 source
  capture validation lands.
- `std/box.Box[T]` (the shared cell Vec)
- `std/thread.Thread`, `std/sync.Mutex`, `std/sync.Condvar`,
  `std/sync.AtomicI64`, `std/ipc.Memory`

Ordinary structs/enums, `string`, file/socket descriptors, `Slice`, foreign
Python/C objects and `ipc.Mutex`/`Condvar`/`Semaphore` wrappers are rejected.

The three operations are compiler builtins owned by this module and lower to
the existing system-call path; there is no new Native IR opcode. `upgrade`
returns the canonical `std/option.Option[T]`. Copy/drop of a `Weak` only
retains/releases the weak control block. Backend execution and runtime slots
are implemented by the separate runtime/Native value-ops owner.
