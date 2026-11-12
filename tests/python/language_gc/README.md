# Python language ownership GC checks

This directory is the MEM02/Python low-level acceptance slice.  It does not
define source closure syntax or Sema/HIR.  The generated program uses the real
helper/API path and the bundled `runtime/python/langrt.py`.

Run the private runtime checks:

```sh
python3.12 tests/python/language_gc/runtime_gc_test.py
```

Run the real Wz programs through a helper built from this tree:

```sh
python3.12 tests/python/language_gc/run_language_gc.py \
  --helper BUILD/tests/python/lang_python_emit \
  --root . --out /tmp/language-gc-results \
  --python /path/to/cpython-3.12.15/bin/python3.12
```

Each case owns a separate generated project and observer process with a finite
timeout.  The observer imports the generated `program.py`/`langrt.py`, wraps
`Vec.create` to retain Weak handles, calls the generated source functions, and
checks `_gc_stats()` plus real Weak upgrade results.  It never calls
Python `gc.collect()` or treats CPython collection as language ARC cleanup.

Cases cover a struct/`Node` Vec self-cycle, two linked cycles, duplicate strong
edges, a live root plus an unreachable cycle pointing to a live child, nested
owned values, automatic loop-header collection (including an always-continue
loop), and typed-error/defer cleanup.  Runtime checks also cover captured
FunctionValue Weak upgrade, stale wrapper retirement, collector allocation
failure state restoration, and foreign-finalizer reentry.  Source capture
syntax, Native, threads, fork and the full MEM02 scope remain out of this
slice.
