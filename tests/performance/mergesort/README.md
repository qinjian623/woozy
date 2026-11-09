# Merge sort fixture

Fixed Source/C++20 pair for `tests/performance/run_sort.py --algorithm merge`.

- `merge.lang.in` and `reference.cpp.in` are rendered from the same LCG inputs,
  Python `sorted` golden array, round count, and per-element check.
- Both programs implement the same hand-written stable **bottom-up** merge
  sort: identical width/start/merge/copy loop order, `<=` left-run comparison,
  and one full-size work buffer.
- The Source program creates the buffer with `values[:]`; the C++ reference uses
  `std::vector<uint32_t> buffer(values)`.  Each width pass merges from `values`
  into `buffer`, then copies all `n` entries back to `values`.  Buffer allocation
  and every copy stay inside the timed program.
- Small embedded vectors cover empty, one, two, duplicates, all-equal, sorted,
  and reverse inputs.  `run_sort.py --prepare-only` additionally builds and runs
  both languages at O0/O2 and requires the high-bit-wrong golden programs to
  exit 1 quietly.

This directory contains no timing results; prepare only proves correctness and
artifact consistency.
