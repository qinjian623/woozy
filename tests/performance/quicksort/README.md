# Native u32 quicksort comparison

`tests/performance/run_sort.py --algorithm quick` generates and validates one
Source program and one hand-written C++20 program that both:

1. fill the same `Vec[u32]` / `std::vector<uint32_t>` from the fixed 32-bit LCG
   `state = state * 1664525 + 1013904223` (wrapping) with seed `0x6d2b79f5`;
2. sort it in place with the same hand-written three-way quicksort;
3. compare every element with an independently generated Python `sorted` golden
   array.

The fixed default is `N=65536`, `16` rounds. Each round refills all `N`
elements from the same seed before sorting and never reuses the sorted result as
the next input or as the golden array. The C++ reference uses `std::vector` and
does not call `std::sort`.

## Algorithm

Both implementations use the same source-level operations in the same order:

- `[lo, hi)` is a half-open range; the pivot value is `a[lo + (hi - lo) / 2]`.
- A Dutch-national-flag partition keeps `< pivot` on the left, `== pivot` in the
  middle, and `> pivot` on the right. Equal elements are excluded from both
  recursive subranges, so all-equal and highly duplicated inputs terminate.
- After each partition, only the smaller subrange is called recursively; the
  larger subrange is handled by updating the local `left` or `right` bound and
  continuing the loop. Recursion depth is therefore `O(log n)`, not `O(n)`.
- Indices are `usize` / `std::size_t`. The loop condition `hi - lo > 1` is only
  evaluated with `lo <= hi`; decrements of `gt` happen only after `i < gt`, so
  unsigned subtraction does not underflow.
- The normal `Vec[u32]` parameter is written through element indexing. No
  `inout Vec`, new language syntax, safety-check removal, compiler/IR change, or
  source-level clock is used.

Small controls in both programs cover the empty range, one element, two
elements, duplicates, all-equal values, already-ascending input,
descending input, the high u32 bit, and `0xffffffff`. The full default array is
also checked element by element after every round.

## Files

- `quick.lang.in`: Source template (`{{ARRAY}}`, `{{EXPECTED}}`, `{{N}}`,
  `{{SEED}}`, `{{ROUNDS}}`).
- `reference.cpp.in`: C++20 reference template with the same placeholders.
- `../run_sort.py`: shared generator, prepare/verification harness, and
  measurement harness. The `quick` selection uses the task
  `native-quicksort-benchmark`, operation ID
  `language-quicksort-benchmark-20261105`, and its own report schemas. A
  measurement run rejects a prepare report for the heap fixture.

Generated sources, arrays, LLVM/Native IR, objects, binaries, and reports go
under `build/quicksort/**` and are not committed.

## Self-test and prepare

The self-test checks the selected template identity, rendering, and
cross-algorithm prepare-report rejection without building a compiler fixture.

```sh
export PY=/home/qin/Work/language-dev-20261103/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12
"$PY" tests/performance/run_sort.py --self-test --algorithm quick
```

Prepare uses the existing Release tools and a new output directory. It builds
and runs Source and C++ at O0/O2, exports LLVM IR before/after optimization plus
Native IR, verifies the IR with `opt`, requires the formal `program.o` to equal
the helper-exported object byte-for-byte, and runs four wrong-golden rejections.

```sh
export MAIN=/home/qin/Work/language-dev-20261103
export RELEASE="$MAIN/builds/independent-runtime-root-integration/release"
export LLVM="$MAIN/toolchains/llvm-22.1.8/bin"
export XML="$MAIN/toolchains/libxml2-legacy-2.13.9/usr/lib"

"$PY" tests/performance/run_sort.py --prepare-only --algorithm quick \
  --out build/quicksort/prepare-release-final \
  --langc "$RELEASE/langc" \
  --helper "$RELEASE/native/lang_native_helper" \
  --runtime "$RELEASE/native/liblang_native_runtime.a" \
  --llvm-bin "$LLVM" \
  --compiler-root "$MAIN/compiler" \
  --ld-library-path "$XML" \
  --cpu 4
```

Prepare is a correctness/artifact check, not formal timing. The measured stage
is the same whole-process wall-time harness as the heap fixture, including
process startup, input generation, sorting, full verification, and cleanup; it
is not a pure sort-function time. No formal timing result or performance
conclusion is produced by prepare.

## Explicitly not measured

- In-process sort-only time, a separate exported sort kernel, or a source clock.
- Cache/load conditions, CPU governor changes, CPU isolation, or perf counters.
- LTO, fast-math, `-march=native`, other targets, other input sizes, or other
  algorithms.
