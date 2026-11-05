# Native u32 sort comparisons

`tests/performance/run_sort.py --algorithm heap` (the default) generates and
validates one Source program and one hand-written C++ program that both:

1. fill the same `Vec[u32]` / `std::vector<uint32_t>` from the fixed 32-bit LCG
   `state = state * 1664525 + 1013904223` (wrapping) with seed `0x6d2b79f5`;
2. sort it in place with the same max-heap `sift_down` + `heapsort` algorithm;
3. compare every element with an independently generated Python `sorted` golden
   array.

The fixed default is `N=65536`, `16` rounds.  `--algorithm quick` selects the
separate in-place three-way quicksort fixture in
`tests/performance/quicksort/`; it uses the same LCG inputs, golden array,
round count, target flags, and prepare/report checks.  The two selections have
distinct task names, operation IDs, report schemas, templates, and manifests,
and measurement refuses a prepare report made for the other algorithm. Each
round refills all `N` elements from the same seed before sorting, so an already
sorted array from the previous round is never used as the next input. The
previous array is never re-used as the golden array.

Both Source programs use only the existing literal `Vec` allocation and plain
`Vec` parameters whose elements are mutated. They do not use `inout Vec`,
`len`, `push`, indexing `inout`, FFI, or a source-level clock. The heap fixture
uses no recursion; the quicksort fixture uses ordinary function recursion for
the smaller partition and loops on the larger partition.

## Files

- `heap.lang.in`: Source heap-sort template (`{{ARRAY}}`, `{{EXPECTED}}`,
  `{{N}}`, `{{SEED}}`, `{{ROUNDS}}`).
- `reference.cpp.in`: C++20 heap-sort reference template with the same
  placeholders.
- `../quicksort/quick.lang.in`: Source quicksort template with the same
  placeholders.
- `../quicksort/reference.cpp.in`: C++20 quicksort reference template.
- `run_sort.py`: algorithm selector, generator, prepare/verification harness,
  and measurement harness.

The generated source, arrays, IR, objects, binaries, and reports go under
`build/sort-prepare/**` and are not committed.

## Prepare

Use a new output directory. The prepare stage writes `prepare.json`, builds and
runs both languages at O0/O2, exports Native IR plus LLVM IR before/after
optimization, verifies the IR with `opt`, and requires the formal `program.o`
to equal the helper-exported object byte-for-byte.

```sh
export PY=/home/qin/Work/language-dev-20261103/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12
export MAIN=/home/qin/Work/language-dev-20261103
export RELEASE="$MAIN/builds/t10-python-backend-root-integration/release"
export LLVM="$MAIN/toolchains/llvm-22.1.8/bin"
export XML="$MAIN/toolchains/libxml2-legacy-2.13.9/usr/lib"

"$PY" tests/performance/run_sort.py --prepare-only \
  --out build/sort-prepare/prepare-release-final \
  --langc "$RELEASE/langc" \
  --helper "$RELEASE/native/lang_native_helper" \
  --runtime "$RELEASE/native/liblang_native_runtime.a" \
  --llvm-bin "$LLVM" \
  --compiler-root "$MAIN/compiler" \
  --ld-library-path "$XML" \
  --cpu 4
```

Prepare verifies the CMake/Ninja `Release` build tree and the four actual
`LANG_NATIVE_*` tool definitions before using the tools. It records script,
`run_native.py`, template, generated source, array, object, binary, and tool
SHA-256 values, plus source sizes and command records.

The primary program is run four times (Source O0/O2 and C++ O0/O2). All four
must exit 0 with no output. The program itself checks every element of the full
array against the golden array and also exercises empty, one-element,
two-element, all-equal, duplicate, already-sorted, reverse-sorted, zero,
`0x80000000`, and `0xffffffff` cases.

For O2, the Source and C++ IR must still contain at least two loops and real
load/store/compare operations. The C++ O2 LLVM IR is saved and verified
separately. No LLVM source changes, exported sort function, or source-level FFI
is used to make this work.

## Wrong-golden control

Prepare also generates a small derived program from the same templates with one
golden element's highest bit flipped while preserving its low 8 bits. The
program must reject it through its normal per-element comparison: both Source
and C++ at O0/O2 must exit exactly 1 with empty stdout/stderr. A crash, Panic,
or unexpected successful exit fails prepare. These four deliberately failing
commands and their full argv/exit/output records remain in `prepare.json`.

`run_sort.py --self-test` checks the LCG/golden/wrong-bit helpers, renders the
selected algorithm templates, verifies the default/heap and explicit algorithm
selection, checks that a matching prepare report is accepted and a
cross-algorithm report is rejected, and verifies that a failed command and a
timed-out command are retained in the command log.  Run it once with the
default (heap) selection and once with `--algorithm quick`.

## Formal measurement

Run the measurement stage only after preparation has stopped and the host is
otherwise idle. It refuses to run unless the prepare report is `passed`, belongs to the same
`--algorithm`, and the script, common `run_native.py`, templates, generated
source/golden arrays, tool paths/SHA-256, build-tree definitions, objects, and
binaries still match.

```sh
"$PY" tests/performance/run_sort.py \
  --out build/sort-prepare/measure-release-idle \
  --prepare-report build/sort-prepare/prepare-release-final/prepare.json \
  --langc "$RELEASE/langc" \
  --helper "$RELEASE/native/lang_native_helper" \
  --runtime "$RELEASE/native/liblang_native_runtime.a" \
  --llvm-bin "$LLVM" \
  --compiler-root "$MAIN/compiler" \
  --ld-library-path "$XML" \
  --cpu 4
```

By default, measurement performs two warmups and seven measured executions per
implementation and O0/O2, alternating candidate and reference order. It records
each run's `elapsed_ns`, exit status, stdout, stderr, and command index, then
reports median/min/max. A missing, failed, timed-out, or noisy run fails the
stage; missing samples are never counted as zero. A minimum below 50 ms is
flagged as startup/loader-sensitive.

The measured duration is the whole ELF process: process startup, loader,
container allocations, fixed-input generation, the selected sort, full
per-element verification, and cleanup. It includes all 16 rounds. It is **not** a pure
sort-function time and no source-level clock is inserted. An optional
`whole_process_ns_per_round` is the whole-process median divided by the round
count, not a pure sorting cost.

## Explicitly not measured

- Cold file-cache behavior, cache flushing, governor changes, CPU isolation, or
  killing other processes.
- Hardware/software performance counters.
- Targets, CPU models, input sizes, or algorithms other than the selected fixed
  heap or quicksort fixture.
- In-process sort-only timing or a separate exported sort kernel.
