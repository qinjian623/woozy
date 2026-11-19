# Fannkuch-redux preparation fixture

This directory prepares the fixed Fannkuch-redux input for the classic algorithm
set. It is a correctness and compiler-artifact check only: it records no timing,
no throughput, and no speedup or `<=2x` conclusion.

The Source and C++20 programs use the same single-threaded iterative `count`
array generation, which is the incremental equivalent of the official recursive
prefix-left-rotation order. Both use the same prefix reversal loop, the same
element-wise permutation copy, and the same alternating checksum by official
zero-based permutation index. The three working arrays are allocated only when a
complete `fannkuch(n)` call starts; no working array is allocated per
permutation.

## Fixed cases

The expected values are read from the frozen oracle at
`<dev-work-dir>/checks/fannkuch-oracle-20261109`. Prepare
verifies the result, input, 5 traces, and 5 expected-output files before and
after the run.

| n | n! | checksum | maximum |
| --- | --- | --- | --- |
| 3 | 6 | 2 | 2 |
| 4 | 24 | 4 | 4 |
| 5 | 120 | 11 | 7 |
| 6 | 720 | 49 | 10 |
| 7 | 5040 | 228 | 16 |

## Files

| File | Purpose |
| --- | --- |
| `fannkuch.wz.in` | renderable Source template; placeholders `{{N}}`, `{{CHECKSUM}}`, `{{MAXIMUM}}`, `{{ZEROS}}`, `{{VISIT_VALUES}}`, `{{VISIT_FLIPS}}` |
| `reference.cpp.in` | renderable C++20 template with the same placeholders |
| `visit_trace.py` | observer of the unmodified generated Source Python module |
| `prepare.py` | prepare driver; reuses `../run_native.py` for commands, hashing, ELF checks, build-tree/tool validation, host records, and report writing |
| `DELIVERY.md` | actual correction run, hashes, commands, results, and remaining limits |

## Correctness interface

Each rendered program has one `FannkuchResult { checksum i64; maximum i32; }`
value. `main` compares both complete values:

- checksum mismatch: quiet exit `1`;
- maximum mismatch: quiet exit `2`;
- both correct: quiet exit `0`.

The programs print nothing. They do not use the official stdout interface
(`checksum` line followed by `Pfannkuchen(n) = maximum`); the full numeric
comparison is the fixture interface.

For each `n`, prepare renders one correct program, one program with
`checksum + 1`, and one with `maximum + 1`. Each wrong variant is built and run
as Source O0/O2 and C++ O0/O2, so every `n` has four wrong-checksum failures and
four wrong-maximum failures.

## Real `visit` observer

Source `visit` is empty and has the real snapshot parameter:

```text
visit(index u64, values Vec[i32], flips i32)
```

C++ uses the corresponding
`static void visit(uint64_t, const std::vector<int32_t>&, int32_t)`. The call
passes the current `perm1` snapshot and the actual flip count. There is no
external observer or per-permutation output in the real programs.

`visit_trace.py` is a real observer of the generated Source program:

1. `prepare.py` runs the actual `lang_python_emit` helper to produce
   `program.py`, `langrt.py`, `verified.hir`, and `metadata.json`.
2. The observer imports that unmodified generated `program.py`.
3. It finds `visit` and `main` by `metadata.json` source names/symbol IDs, not by
   a hard-coded symbol number.
4. It wraps only `visit`, records the actual received `index`, one-based
   `values`, and `flips`, then calls the original empty `visit`.
5. It fully executes the generated `main` and prints the recorded
   `index:one_based_permutation:flips` lines.

Prepare compares those real 5,910 lines with `traces/n3.txt` through
`traces/n7.txt`. The observer does not recompute Fannkuch, permutations, or flip
counts.

Two Source negative controls use the same observer:

- `negative-visit-flips`: `visit` receives `flips + 1`; checksum/max still use
  the real flips, so the programs and generated `main` still succeed. The
  observer must reject the trace with a `flips` mismatch.
- `negative-visit-values`: `visit` receives the flipped `perm` instead of the
  `perm1` snapshot; checksum/max still use the real permutation. The observer
  must reject the trace with a `permutation` mismatch.

## Visit-call identity

Source visit calls are identified through the generated Python metadata symbol
ID for the Source `visit` function. Prepare requires that symbol to exist as a
NativeIR function and as `@lang.fn.<symbol>` in the exported LLVM IR, then counts
actual `call`/`invoke` instructions targeting that symbol. This is not a search
for the letters `visit`.

C++ visit calls are identified from the O0 IR debug metadata's
`DISubprogram(name: "visit", linkageName: ...)`. Prepare counts calls/invokes to
that exact mangled linkage name.

For every n, O0 must have at least one actual target call, and O2 must have zero.
The report records before/after counts and the actual symbol/mangled name.

## Prepare command

Use the dedicated production Release tree with `BUILD_TESTING=OFF`. Output paths
must not already exist.

```sh
REPO="$(git rev-parse --show-toplevel)"
export MAIN="${WOOZY_WORK_DIR:-$(dirname "$REPO")}"
export RELEASE="$REPO/build/native-perf-release"
export LLVM="$MAIN/toolchains/llvm-22.1.8/bin"
export XML="$MAIN/toolchains/libxml2-legacy-2.13.9/usr/lib"
export PY="$MAIN/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12"
export PYHELPER="$REPO/build/native-cli-release/tests/python/lang_python_emit"
OUT="${OUT:?set OUT to a new preparation output directory}"

"$PY" -B -s tests/performance/fannkuch/prepare.py \
  --out "$OUT" \
  --oracle-dir "$MAIN/checks/fannkuch-oracle-20261109" \
  --python "$PY" \
  --python-helper "$PYHELPER" \
  --woozyc "$RELEASE/woozyc" \
  --helper "$RELEASE/native/lang_native_helper" \
  --runtime "$RELEASE/native/liblang_native_runtime.a" \
  --llvm-bin "$LLVM" \
  --compiler-root "$REPO" \
  --ld-library-path "$XML" \
  --cpu 4
```

The C++ reference uses C++20 with
`--target=x86_64-unknown-linux-gnu -march=x86-64 -mtune=generic`,
`-fno-fast-math`, `-fno-lto`, and the fixed LLVM `ld.lld`.

## Output checks

`prepare.json` records every command's complete `argv`, status, exit, stdout,
stderr and timeout. It also contains:

- generated Source/C++/manifest and Python artifact SHA-256 values;
- real observer traces, frozen traces, exact-match results, and negative-control
  first mismatches;
- Source LLVM `before.ll` / `after.ll`, both `opt -passes=verify` results, the
  helper object comparison against the formal `woozyc` `program.o`, and the
  metadata/NativeIR/LLVM visit-symbol mapping;
- C++ O0/O2 IR verifies and mangled-symbol visit call counts;
- wrong-checksum/wrong-maximum and negative-control builds, runs, and exits;
- tool SHA-256/version probes, the production Release build-tree definitions,
  host/compiler HEAD/status records, and frozen-oracle before/after hashes.

The delivered correction run in `DELIVERY.md` passed 20 correct executions,
40 expected failures, 20 negative Source executions with normal main checks,
10 negative observer rejections, 5 real trace files (5,910 rows), 10 Source
LLVM exports, 20 Source IR verifies, 10 object byte matches, and 10 C++ IR
verifies. It did not measure time and does not claim a performance result.
