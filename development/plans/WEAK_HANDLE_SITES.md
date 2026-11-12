# Weak handle sites and ownership map

Status: pre-implementation map, retained as the change inventory. The real
Vec/Handle slot, final-release and fork changes are now integrated; Root
full CTest 136/136 passed. Source Weak and automatic Native GC remain
unimplemented. The older sizes below describe the measured pre-change layout.

This maps the Native ownership sites a future
source-level `Weak[T]` must use. It does not implement `Weak[T]`, change the
runtime, or claim MEM01/MEM02 completion.
Operation: `woozy-shared-handle-weak-map-20261112`.
Scope: `runtime/native/weak.*`, `gc*`, `handle.hpp`, `vec.hpp`, Native codegen,
hosted replacements, and std wrappers that hide runtime ownership.

## 1. Evidence baseline
The accepted standalone primitives were re-run, not inferred from old reports:

- `tests/native/weak/run.sh`: `STATUS=0`; O0/O2 host, ASAN/UBSAN, and O0/O2
  `-nostdlib` static ELF passed. `weak.o` undefined symbols remain exactly
  `lang_alloc_v1`, `lang_free_v1`, `lang_native_panic_v1`.
- `tests/native/cycle_gc/run_gc_weak.sh`: `STATUS=0`; five
  `GC_WEAK_ALL_PASS` runs (host O0/O2, ASAN/UBSAN, static O0/O2).
- Header probe: `LangVec size=72, refs=0, capacity=64, aligned header=80`;
  `LangHandle size=24, refs=0, kind=8, destroy=16`. A trailing eight-byte slot
  projects to `LangVec size=80` (header still 80) and `LangHandle size=32`.
Hashes and logs are in `outputs/tmp/woozy-weak-handle-sites/`.

## 2. Accepted contracts that must not change
`runtime/native/weak.hpp` is authoritative: `LangWeakControl` is 16 bytes
(atomic `u64 weak_refs`, `void *target`); the strong count is always the
target's offset-0 atomic `u64`; `owner_slot` is null initially, points to one
real control, or holds permanent closed tag `1`; downgrade requires a strong
reference; final release calls `lang_weak_detach_v1` once before free; closed
tag is a harmless no-op; `lang_weak_invalidate_slot_v1` permanently closes the
slot and clears the target without allocation, free, or user code under lock;
fork uses `lang_weak_fork_prepare/parent/child_v1`.
`runtime/native/gc.hpp` is also authoritative: `LangGcOps.invalidate_weak` is
the trailing optional callback. The collector pins every candidate, calls every
`invalidate_weak`, and only then calls the first `clear` (`gc.cpp` pinned /
invalidate / clear loops). `gc.hpp`, `gc.cpp`, and `weak.cpp` need no semantic
change for the sites below.
## 3. Eligible actual targets
| Source type | Actual object and strong header | Current final path | Weak slot |
| --- | --- | --- | --- |
| `Vec[T]` | `LangVec`, `refs` offset 0, size 72 | `lang_vec_release_v1`: CAS to 0, acquire fence, iteration check, drop initialized payload, free data then descriptor | append at 72; `&vec->weak_control` |
| `Box[T]` | private one-element `LangVec` cell, not the wrapper | same `LangVec` path | slot on the cell Vec; all Box copies share it |
| `thread.Thread` | `LangHandle` kind `thread`, `refs` offset 0, size 24 | native release then `destroy_thread` unmaps state; hosted release then join/detach and free | append at 24; `&handle->weak_control` |
| `sync.Mutex` / `sync.Condvar` / `sync.AtomicI64` | `LangHandle` in `MutexObject` / `CondvarObject` / `AtomicI64Object` | release then kind-specific validation and `lang_free_v1` | same `LangHandle` slot |
| `ipc.Memory` | `LangHandle` kind `ipc` in `Mapping` | release then munmap and free | same `LangHandle` slot |

`ipc.Mutex`, `ipc.Condvar`, and `ipc.Semaphore` are views over `ipc.Memory`
plus an offset; they are not independent ARC identities and are not targets.
The projected `LangVec` slot keeps the aligned header at 80, so payload offsets
and the observer ABI do not move. `LangHandle` grows 24 to 32; text payloads and
embedded handle-first objects shift by eight bytes but keep `refs` 0, `kind` 8,
`destroy` 16.
## 4. Not eligible or not implemented
- `string`: `LangText` is a four-word value whose `owner` is a `LangHandle` of
  kind `text`. It has a strong count, but the confirmed list excludes strings.
- `os.File` and `net.Socket`: plain `descriptor i32` values, no ARC and no
  finalizer; not weak targets.
- `Slice[T]`: a borrowed 24-byte view retaining/pinning a Vec. Its identity is
  the owner Vec, not the descriptor; do not give the descriptor a slot.
- By-value structs/enums: inline values, possibly owning fields, with no stable
  heap identity; excluded.
- `CForeignResource`, `CForeignOpaque`, C pointers/buffers, Python foreign
  objects: externally managed or non-ARC; excluded by rule.
- `Function`: current `TypeKind::Function` lowers to a bare LLVM function
  pointer and is trivial storage. There is no ARC header, finalizer, or slot.
  Function Weak support waits for the separate Fn ABI/closure planner, which
  must supply an offset-0 strong count and canonical slot. Do not build an
  address-keyed function registry.
- `Channel`, `Task`, `Map`, `Future`, `Pool`, `cancel.Token`, `io.Reader`,
  `io.Writer`: nominal structs whose state lives in Vec cells. They are not in
  the confirmed list. Do not unwrap arbitrary single-Vec structs; that would
  silently make every library wrapper weakable. `Box[T]` is the exception.
## 5. Canonical slot lifecycle
1. Construction writes the canonical slot to null before publication.
   `lang_vec_new_v1` must write it; `allocate_object` (sync), native/hosted
   thread state, IPC `Mapping`, and text owner allocation must also do so.
2. Copying a Vec or handle copies the pointer/header, not the slot value. The
   control block is shared; Weak copies call `lang_weak_retain_v1`.
3. Downgrade is generated only where the caller owns a strong reference, and
   passes `&object->weak_control`; the runtime installs one control or observes
   an existing/closed slot.
4. Final strong release, after CAS to zero and acquire fence, loads the slot and
   calls `lang_weak_detach_v1` before `destroy`/free. A GC-closed slot is a
   no-op. The target must not be freed while detach can read it.
5. Do not clear or reinstall the slot in `clear`, element replacement, Vec
   resize, handle destroy, or any finalizer. Reusing storage requires the new
   constructor to write null, replacing a closed tag.
6. `lang_vec_set_v1`, `lang_vec_resize_v1`, and `lang_vec_clear_v1` change
   elements, not Vec identity. They must not detach or invalidate the Vec slot;
   replaced elements use their own final paths.
The accepted order (drop payload, detach, free) is safe because the strong
count is already zero: upgrade fails at `refs == 0` before detach. Detach must
not be after free or skipped on the final branch. For handles, detach belongs
in the shared release function before `handle->destroy`, covering caller-side
and runner-side final release.
## 6. GC white candidates and clear
Preserve the accepted order: snapshot; trace/count edges; mark external roots;
retain every candidate (pin) before any clear; call `invalidate_weak` for every
candidate before the first clear; clear and re-trace, panicking on surviving
edges; release pins and count `collected` only when the pin was the last strong
count.
For a `LangVec` or `LangHandle` candidate, `invalidate_weak` is exactly
`lang_weak_invalidate_slot_v1(&object->weak_control)`, with no user code or
allocation. After this, no downgrade can install a new control and no upgrade
can return the white target. `clear` must not reinstall or null the slot. A
callback that tries to upgrade a white peer must fail; unsafe self-resurrection
leaves the slot permanently closed. Box needs no separate GC type: its cell Vec
is the traced node and its `invalidate_weak` closes the cell slot. Nominal
wrappers are not independent weak nodes.
## 7. Fork, thread self-reference, and cancellation
`gc_runtime.hpp` fixes the full order:
    alloc prepare -> weak prepare -> gc prepare -> world prepare ->
    runtime prepare -> fork
    parent: runtime -> world -> gc -> weak -> alloc
    child:  alloc -> weak -> gc -> world -> runtime
Native `process.cpp` currently calls only allocator hooks. Root must add
Weak/GC hooks in that order for both `fork_process` paths and release the parent
side on failure. Hosted libc-fork atfork prepare/parent/child in
`runtime/native/process.cpp` and `runtime/hosted/process.cpp` must bracket Weak
and GC in the same order. Only the surviving child resets Weak ticket state; it
must not fabricate ARC drops for vanished threads.
A thread handle starts with `refs == 2`: caller handle plus runner self-reference.
The runner (`lang_thread_run_v1` native, `run_thread` hosted) or the
caller/waiter may perform the final release. Detach therefore cannot live only
in `wait`; it belongs in common `lang_handle_release_v1` before
`destroy_thread`. Hosted self-detach and native stack unmap happen after that
release. No Weak call may happen after the runner unmaps its stack.

`cancel.Token` is cooperative cancellation, not thread cancellation. It owns no
Thread handle and has no finalizer; its `Registry` is reached through a Vec
cell. `request` notifies gates and does not transfer/release a Weak owner. No
slot belongs in `cancel`; a future `Weak[Vec]` on its private state is still a
Vec-slot operation, not a Token handle.

## 8. Smallest shared hook/header extension
Use one trailing `LangWeakControl *` per ARC-capable header, forward-declared so
`vec.hpp` and `handle.hpp` need not include `weak.hpp`:

- `LangVec`: append after `capacity`. Size 72 becomes 80; aligned header stays
  80, so payload offsets, element stride, and the observer header stay
  unchanged. This preserves the fast primitive-Vec layout and all old offsets.
- `LangHandle`: append after `destroy`. Size 24 becomes 32; all old offsets
  remain. Update native/hosted thread and sync size assertions, and initialize
  the field at every allocation site.
- No new runtime API: use existing `lang_weak_downgrade_v1`,
  `lang_weak_retain/release_v1`, `lang_weak_upgrade_v1`, `lang_weak_alive_v1`,
  `lang_weak_detach_v1`, and `lang_weak_invalidate_slot_v1`.
- No address-keyed side table, generation map, or global Weak registry. The
  canonical slot is already the identity and address-reuse protection; an
  address registry would add ABA/lifetime problems without covering wrappers.
- Keep `refs` as atomic `u64` at offset 0. Keep `weak.cpp` free of
  libc/pthreads; it allocates only through `lang_alloc_v1` and frees only
  through `lang_free_v1`.

This is a shared header/ABI extension, not a Weak feature implementation. Root
must add `weak.cpp` (and accepted GC units when wired) to native and hosted
runtime archives; that build wiring is outside this analysis.

## 9. Root change inventory: finalizers and inline RC

| Site | Required change |
| --- | --- |
| `panic.cpp` `lang_vec_new_v1` | write trailing Weak slot null before returning a published Vec |
| `panic.cpp` `lang_vec_release_v1` | after final CAS/acquire, load slot and detach before freeing data/descriptor |
| `vec.hpp`, `handle.hpp` | add trailing slot; keep `refs` offset 0 and existing old offsets |
| native/hosted `lang_handle_release_v1` | detach before `handle->destroy`; covers runner-side final release |
| native/hosted `lang_thread_spawn_v1` | initialize handle slot null before clone/pthread create |
| native `lang_thread_run_v1`, hosted `run_thread`/`fail_spawn` | keep final release through `lang_handle_release_v1`; no direct final free |
| `sync.cpp` `allocate_object` and destroy functions | initialize slot null; keep `lang_free_v1` after release-path detach; no second detach |
| `ipc.cpp` `Mapping`, `text.cpp` `allocate` | initialize handle slot null; text is not a Weak target but shares `LangHandle` |
| `slice.cpp`; `process.cpp`/`dns.cpp`/`network.cpp`/`unicode.cpp` releases | no slot and no new Weak call; ordinary releases reach the changed finalizers |
| `value_ops.inc` `vec_rc` | keep fast release only for `refs >= 2`; final `refs == 1` must call `lang_vec_release_v1`. Never inline a final decrement that bypasses detach |
| `value_ops.inc` `value_op` | no slot edit; Vec/handle copy/drop continue through runtime RC entry points |
| `gc.cpp`, `gc.hpp` | no semantic change; generated `invalidate_weak` targets the canonical slot |
| native/hosted `process.cpp` fork hooks | add Weak/GC prepare/parent/child in accepted order; parent release on failure |
| `src/backend/native/CMakeLists.txt`, hosted `sources.cmake` | Root wiring: compile `weak.cpp` into native/hosted archives; add GC units when integrated |

The LLVM inline path bypasses only non-final decrements. It is safe because
`vec_rc` sends `refs < 2` to the runtime finalizer. If that condition, the
observer path, or a future fast final-release optimization changes, Root must
add detach there or the slot stays unclosed. `LangHandle` has no inline RC path
today, so all handle releases already reach the runtime.

## 10. TypeId-based identity projection

Do not use `type_key` strings or module/name text. Use resolved `TypeId`,
`TypeKind`, declaration `SymbolId`, and `classify_value`:

- `TypeKind::Vec` -> Vec header; source type remains the `Vec[T]` TypeId.
- `ThreadHandle`, `MutexHandle`, `CondvarHandle`, `AtomicI64Handle`,
  `IpcHandle` -> `LangHandle` header.
- A nominal wrapper whose only field is one of those handle TypeKinds may
  project to the handle; the private runtime type cannot be spelled outside its
  std module. Preserve the wrapper TypeId for upgrade/copy/drop.
- `Box[T]`: identify the resolved `std/box.Box` declaration by `SymbolId` and
  verify its single field is `Vec`; project to that field TypeId. No name
  heuristic; do not project arbitrary single-Vec structs.
- `Function`: not weakable until the Fn ABI supplies a closure environment with
  an offset-0 strong count and canonical slot; the current bare pointer has no
  identity.
- Everything else is rejected with an explicit capability diagnostic. No silent
  fallback to `Any`, address registry, or per-value wrapper slot.

The projection selects the header/slot only. The source Weak value still stores
the original `TypeId`, so upgrade returns the same wrapper/container type and
follows that type's normal ARC release.

## 11. Races probeable now, and acceptance criteria

Already probeable without Source Weak, using runtime headers and accepted tests:

- Empty/downgrade/retain/release/upgrade lifecycle; concurrent downgrade on one
  slot; upgrade racing final strong release; invalidate racing downgrade and
  upgrade; closed-slot no-reinstall; address reuse; fork hooks; panic causes.
  Fresh Weak suite passes every phase and ends `control blocks live=0`.
- GC white ordering: pin all, invalidate all before first clear, white
  upgrade/downgrade fail, close is permanent, candidate pins release correctly.
  Fresh GC+Weak suite prints `GC_WEAK_ALL_PASS` in all five configurations.
- Layout/ABI: current and projected header sizes are measured above.

After the header extension, C++ runtime probes can cover real objects without
`Weak[T]` syntax:

- Real `LangVec`/`LangHandle` downgrade, last strong release, upgrade/alive
  failure, and control allocation count returning to zero.
- Upgrade concurrently with `lang_vec_release_v1` and both thread-handle final
  release orders (runner last, waiter last); reject UAF with ASAN/observers.
- GC with real Vec slots: every white slot closed before first clear; a clear
  callback cannot resurrect a white peer.
- Fork before/after slot installation; child upgrade only for living targets;
  parent/child Weak and allocator locks paired on success and failure.
- Box copies share one cell pointer and one control; mutation is visible; last
  drop closes the slot.
- Function values rejected as Weak targets until a closure environment with a
  real header exists.

Acceptance for eventual integration: O0/O2, ASAN/UBSAN, and `-nostdlib` static
ELF; no libc/libstdc++ dependency in default freestanding programs; no target
freed before detach; no control leak after all Weak owners drop; no white
upgrade after invalidation; no address-reuse ABA; fork failure releases parent
hooks; no change to atomic `u64` ARC semantics or the fast primitive-Vec inline
path.

## 12. Open items owned elsewhere

- Fn ABI/closure environment layout and strong header belong to the separate
  Function planner; this document only records the missing identity.
- `Box[T]` source declaration and its private one-element Vec are not yet in
  the tree; the projection is provisional until that task lands.
- Wiring GC/Weak into automatic collection, safepoints, and source `Weak[T]`
  syntax remains MEM01/MEM02 work. This map is not completion evidence.

## Root review, 2026-11-12

This map was accepted as an analysis, not as source Weak implementation. The
slot/final-release/fork implementation is assigned to task
`fb413a68-fe4c-4ac0-9364-79bf9dc7e709`. This step adds only Weak hooks to the
existing fork paths; GC/world hooks still require their later coordinated
integration. Fn uses a shared Vec environment in the reviewed representation;
its source implementation and Weak projection remain pending.
