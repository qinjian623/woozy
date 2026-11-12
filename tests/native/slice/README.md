# Borrowed Slice runtime validation

Scope: the private `LangSlice` ABI in `runtime/native/slice.hpp`/`slice.cpp`.
This is the low-level borrowed view only. It does not implement source-level
`WzSlice`, typed `Ptr`, or change the existing copying `v[start:end]` Vec slice.

## ABI

`LangSlice` is 24 bytes, 8-byte aligned:

| offset | field |
| ---: | --- |
| 0 | `LangVec *owner` |
| 8 | `uint64_t start` |
| 16 | `uint64_t length` |

`LangVec` remains at its published 72-byte layout. The only new exported
symbols are:

```
lang_slice_new_v1
lang_slice_copy_v1
lang_slice_drop_v1
lang_slice_sub_v1
lang_slice_at_v1
lang_slice_length_v1
```

`slice.cpp` calls only `lang_vec_retain_v1`, `lang_vec_release_v1`,
`lang_vec_begin_v1`, `lang_vec_end_v1`, `lang_vec_at_v1`, and the panic entry;
it performs no libc allocation and does not invoke element copy/drop callbacks.

## Rules verified here

- `new` checks `start <= end <= length <= initialized`, then retains and pins.
  Empty ranges still retain and pin their owner.
- `copy` adheres a bit-copied live handle with one additional retain and pin.
  A zeroed `LangSlice` is the only owner-less state: copying it is a no-op and
  dropping it repeatedly is safe.
- `drop` clears the descriptor before ending the pin and releasing the owner.
  Malformed non-empty null-owner handles and invalid ranges Panic.
- `sub` validates relative bounds and absolute overflow, then creates a new
  independently pinned view of the same owner.
- `at` checks the slice-relative index and absolute overflow, then uses the
  existing `lang_vec_at_v1` slot access. The returned pointer is for typed
  backend use, not an unrestricted source-language raw pointer.
- While any slice is alive, Vec `reserve` (when capacity actually grows),
  `push`, non-empty `pop`, `resize`, and `clear` Panic with
  `active-iteration` before changing length or storage. `reserve` inside the
  existing capacity is allowed. After the final slice drops, structural
  mutation succeeds.
- Slices do not copy or drop elements. Owned Vec/string-like payloads are
  dropped exactly once, at the final owner release.

## Concurrency prerequisites

The ABI is atomic only for the existing `refs` and `iterations` counters. It
does not make descriptor bit-copies or element accesses safe by itself:

- A caller that bit-copies a `LangSlice` descriptor between threads must
  synchronize that bit-copy with the source descriptor (for example with the
  existing mutex), then call `lang_slice_copy_v1` on the destination before
  use.
- The destination bit-copy must be retained/pinned while the source handle or
  another strong owner reference is still live. Copying a descriptor after the
  owner was finally released cannot resurrect it.
- Structural Vec changes require the existing caller lock. The runtime does
  not repair data races, and a malformed unsynchronized handle can still be
  unsafe.
- Concurrent element reads/writes through aliasing slices require caller
  synchronization; the iteration pin permits element writes, not races.
- Every bit-copy needs exactly one `lang_slice_copy_v1`; every live handle
  needs exactly one `lang_slice_drop_v1`. The exact ABI has no per-handle
  marker to detect a missing or duplicate copy call.

## Build and run

Use the repository's pinned LLVM 22 toolchain and the legacy libxml2 runtime for
CTest JUnit output:

```sh
cmake -S tests/native/slice -B /tmp/lang-slice-build -G Ninja \
  -DCMAKE_CXX_COMPILER=/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/clang++ \
  -DCMAKE_BUILD_TYPE=
cmake --build /tmp/lang-slice-build -j2
LD_LIBRARY_PATH=/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9/usr/lib \
  ctest --test-dir /tmp/lang-slice-build --output-on-failure \
  --output-junit /tmp/slice-ctest.xml
```

The harness tests `slice.runtime.O0`, `slice.runtime.O2`, a no-libc static
`slice_freestanding` run, and static ELF properties (no `PT_INTERP`, no
`DT_NEEDED`, no strong undefined symbol). The hosted tests wrap
`lang_alloc_v1`/`lang_free_v1` to require zero live allocations and no
double/unknown free.
