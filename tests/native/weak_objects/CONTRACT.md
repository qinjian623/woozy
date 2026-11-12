# Weak real-object runtime contract

Status: runtime foundation only.

This directory tests the canonical Weak slot on the actual Native runtime
objects that the accepted map (`development/plans/WEAK_HANDLE_SITES.md`)
classifies as ARC-shared:

* `LangVec` carries one trailing `LangWeakControl *` at offset 72. Its size is
  80 and the aligned payload header remains 80, so element offsets and the
  observer ABI do not move.
* `LangHandle` carries one trailing `LangWeakControl *` at offset 24. Its size
  is 32; `refs`, `kind` and `destroy` keep offsets 0, 8 and 16.
* Thread, Mutex, Condvar, AtomicI64, IPC and the internal Text owner use the
  `LangHandle` slot. Text is not a source-level Weak target; its owner only
  initializes the slot because it shares the handle layout.
* `Box[T]` is represented by its private one-element `LangVec` cell. This
  suite models that projection with Vec aliases; it does not claim a Box source
  declaration that is not in this tree.

## Lifecycle rule

1. Every constructor writes the slot to null before publication.
2. Copies share the descriptor/header and therefore the one control block.
3. `lang_weak_downgrade_v1` is called with a strong owner and
   `&object->weak_control`. Concurrent/repeated calls converge on one control.
4. The unique final strong release calls `lang_weak_detach_v1` before
   `destroy`/unmap/free. For `LangVec` it is called after the final CAS and
   acquire fence, before payload cleanup and descriptor free. For every
   `LangHandle` subclass it is in the shared release function, so either the
   caller/waiter or the thread runner can be last.
5. `lang_weak_invalidate_slot_v1` permanently tags the slot `1`. Later
   downgrades return null and never install a new control. Final detach treats
   the tag as a no-op. New storage gets null from its constructor, never the
   old tag.
6. Element mutation, growth, clear and Slice operations never touch the Vec
   slot. They change payload, not Vec identity.
7. Fork only adds Weak hooks after the allocator hooks:
   `alloc prepare -> weak prepare -> fork`, parent
   `weak parent -> alloc parent`, child `alloc child -> weak child`.
   The native raw-fork branch and the hosted atfork hooks use the same order.
   A failed fork releases the parent hooks. The child resets only the Weak
   ticket state and creates no ARC drops for vanished threads.
8. No address registry, side table, generation map, or automatic GC
   registration is added. `weak.cpp` algorithms are unchanged.

## Explicit non-goals

* No source `Weak[T]` syntax, sema/HIR/IR/native emit/value_ops change.
* No Function/closure Weak target. The current bare function pointer has no
  ARC header or canonical slot.
* No automatic cycle-collector registration or world/GC fork hooks. A future
  collector callback closes a white slot through
  `lang_weak_invalidate_slot_v1`; this task does not call it automatically.
* Strings, ordinary structs/enums, file descriptors, foreign resources and
  borrowed Slice descriptors are not Weak targets.

## Verification

`run.sh` builds the test source against the native and hosted runtimes and
checks:

* real Vec, Box-cell alias, Mutex, Condvar, AtomicI64 and IPC handle
  lifecycles;
* repeated downgrade on one slot and closed-slot no-reinstall;
* address reuse begins with a null slot;
* a permanent 4 x 20000 allocator-observer stress phase whose shared
  counters use atomic builtins and whose delta must be exactly 80000/80000;
* a gated runner-last release: the user strong reference is dropped before the
  runner is allowed to return, so only the runner can perform final release;
* waiter-last final release, bounded weak-death and allocator-balance waits
  after release, without reading a freed handle;
* an upgrade/final-release handshake that requires at least 128 successful
  upgrades, holds one upgraded reference across the main thread's final
  release, and then lets the holder perform the final release;
* fork before and after slot installation;
* fork failure with seccomp-denied fork, proving parent Weak/allocator hooks
  were released;
* allocator allocation/free balance and zero live allocations after all tests;
* host O0/O2 native + hosted, hosted ASAN/UBSAN, and native `-nostdlib`
  static O0/O2 with no strong undefined symbols, `NEEDED`, `INTERP`, and an
  `EXEC` header.

The tests use the private `lang_weak_*` ABI directly. A test-side allocator
wrapper is observation only; it does not detach or free product objects. Its
shared counters use `__atomic_*` builtins and are reset only when no worker
exists. The stress phase asserts exact deltas instead of the weaker
`alloc == free` condition.

## Test registration

Root owns `tests/native/CMakeLists.txt`. Apply
`tests/native/weak_objects/register.patch` (or add the two targets manually);
this task does not edit the root test CMake file. The standalone `run.sh`
requires no CMake changes.

Root final observer amendment: update free/live counters only after the real
free returns; live uses RELEASE and readers use ACQUIRE. Reset occurs before
workers exist. Root reran all seven configurations after this amendment.
