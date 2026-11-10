# Binarytrees preparation fixture

This directory contains a correctness and LLVM/object preparation check for the
official single-threaded binary-trees workload written in Source and
hand-written C++20. It does **not** measure time, throughput, or speedup, and it
does not claim a `<=2x` result.

The fixed algorithm is:

```text
maxDepth   = max(N, 6)
stretch    = full tree of depth maxDepth + 1; check; release
long-lived = full tree of depth maxDepth
for depth = 4, 6, ..., maxDepth:
    trees = 2**(maxDepth - depth + 4)
    repeat trees times:
        tree = full tree of depth depth
        check(tree)
        release tree
check(long-lived); release
```

`check(node)` is `1` for a leaf, otherwise
`1 + check(left) + check(right)`. Every tree is completely created before it is
visited, and completely released before the next tree is created. The
long-lived tree remains alive through all bucket batches.

There is no output, FFI, thread, SIMD, timing call, LTO, fast-math, FMA,
`-march=native`, benchmark-specific arena/pool/free-list, cached node, shared
leaf, or formula-only replacement for building/visiting the trees.

## Fixed inputs and independent expected values

| N | maxDepth | stretch depth/check | long-lived depth/check | bucket depth: trees / check |
| ---: | ---: | --- | --- | --- |
| 6 | 6 | 7 / 255 | 6 / 127 | 4: 64 / 1984; 6: 16 / 2032 |
| 8 | 8 | 9 / 1023 | 8 / 511 | 4: 256 / 7936; 6: 64 / 8128; 8: 16 / 8176 |
| 10 | 10 | 11 / 4095 | 10 / 2047 | 4: 1024 / 31744; 6: 256 / 32512; 8: 64 / 32704; 10: 16 / 32752 |

The values come from an independent integer perfect-tree formula in
`prepare.py` (`nodes(d) = 2**(d+1) - 1`). They are never read from either
implementation. For N=10 the complete six-line formatted text is compared
byte-for-byte with the official 223-byte file:

```text
/home/qin/Work/language-dev-20261103/checks/algorithm-reference-inputs-20261109/binarytrees-n10.txt
SHA-256 b7f92c56b5d8aeb0a4d698842d1d87a57b4909865c3c84e5e10313e16663c3cb
```

The N=6/8/10 values are also checked against
`binarytrees-oracle-check.json` in the same directory. The programs print
nothing; each computed line is compared inside the program with the embedded
independent constants.

## Source and C++ representation

Source uses the existing recursive type without new syntax or standard library
APIs:

```text
struct Node {
    leaf bool;
    children Vec[Node];
}
```

Every logical node owns a real `Vec[Node]` object. Leaves use a real empty Vec,
not a constant sentinel; internal nodes own exactly two child Node values in
their Vec. Creation is post-order: left subtree, right subtree, then the parent
Vec. The Source runtime allocates its descriptor and element storage together,
once per logical node, including independent empty leaf Vecs. Object sizes still
differ from C++ and are recorded as implementation costs.

C++ uses one separately allocated `Node` per logical node, including leaves:

```cpp
struct Node {
    bool leaf;
    std::unique_ptr<Node> left;
    std::unique_ptr<Node> right;
};
```

Leaves have null owning children but still allocate the same Node object.
C++ therefore performs one real `new` per logical node and one `delete` per
node. No `std::vector`, arena, pool, free-list, or shared leaf is used.

Both sides perform the same creation/traversal/release order and use the same
left-then-right child order.

## Correct and wrong-expected controls

Every N is rendered in five variants:

| variant | change | expected quiet exit |
| --- | --- | --- |
| `correct` | none | 0 |
| `wrong-stretch` | stretch check + 1 | 1 |
| `wrong-bucket` | first bucket check + 1 | 2 |
| `wrong-long` | long-lived check + 1 | 3 |
| `wrong-tree-count` | first bucket tree count + 1 | 4 |

All variants are built and run as Source Native O0/O2 and C++ O0/O2. Wrong
variants must exit quietly with the listed nonzero code; a crash, Panic, output,
or exit 0 fails the control.

## Real lifecycle observers

The formal programs contain no observer code. Separate test-only binaries are
linked from the same prepared Source object and the same C++ reference source.

### Source

`prepare.py` links the helper `program.o` (byte-identical to the formal
`langc build` `program.o`) with `native_observer.cpp` and the project runtime
using `--wrap=lang_alloc_v1` / `--wrap=lang_free_v1`. `native_observer.cpp`
defines the runtime's existing weak `lang_vec_observe_v1` ABI and reads the real
`LangVec` layout from `runtime/native/vec.hpp`; it does not guess the ABI. The
trace records:

- every real allocator pointer/size/alignment and free pointer;
- every Vec create/free identity, length, stride and refs;
- every element drop index and final refs.

The trace is written to the `BT_TRACE_PATH` file. The observer ELF depends on
host libc and is not a formal Native performance program.

The checker validates:

- each new Vec has one real allocation of `64 + length * stride` bytes,
  including a distinct 64-byte descriptor for each zero-length leaf;
- every node is a length 0 leaf or length 2 internal Vec;
- leaf count = internal count + 1 for each full tree;
- element drops occur in reverse index order;
- every Vec identity and every heap pointer is physically freed;
- the exact tree-run sequence: stretch tree, complete long-lived tree, then each
  bucket tree completely allocated before any free, and completely freed before
  the next new tree;
- peak live nodes equals the stretch-tree node count;
- the long-lived tree identities survive every bucket run and are released in
  the final run;
- total logical nodes match the independent formula.

### C++

`prepare.py` compiles the same `reference.cpp` with
`-Dmain=tree_program_main` and links `cpp_observer.cpp`, which replaces global
`operator new`/`delete`. It records every Node pointer and allocation size plus
the final `allocations/frees/live/peak` line after `tree_program_main` returns.
It does not rely on host static-destructor order.

The checker validates the same tree-run sequence, exact node count, one stable
Node size for every allocation, peak live nodes, long-lived survival, and final
live count zero. The observer binary is test-only.

## LLVM and object checks

For every correct N and O0/O2:

- public `langc check` JSON;
- Source Native O0/O2 build, static ELF checks, quiet execution;
- C++ O0/O2 build, dynamic ELF checks, quiet execution;
- Source LLVM `before.ll` / `after.ll` from `lang_native_helper`;
- two explicit `opt -passes=verify` runs;
- helper-object SHA-256 and byte equality with the formal `langc build`
  `program.o`;
- C++ O0/O2 `-g -S -emit-llvm` IR export and `opt -passes=verify`;
- O2 work checks that real recursive tree construction/traversal remains:
  Source must retain recursive constructor/traversal functions with
  `lang_vec_new_v1`, `lang_vec_at_v1`, `lang_vec_release_v1`, leaf-flag loads,
  pointer loads and node-count addition; C++ must retain `make_tree` and
  `check`, real `operator new`/`delete` calls, recursive calls, child pointer
  loads, leaf-flag loads and node-count addition. A constant-return or
  allocation-free artifact is rejected.

Formal Source CLI ELFs must have no `INTERP`, no `NEEDED`, and no strong
undefined symbol. Formal C++ ELFs must be dynamic with non-empty `NEEDED`.
Observer ELFs are recorded separately as host-libc-dependent test binaries.

All commands are recorded with complete `argv/status/exit/stdout/stderr/
elapsed_ns/timeout_s`. The large lifecycle traces are retained as files and
their SHA-256/size/line counts are recorded; they are not embedded in the JSON.

## Running

The production tools used by the recorded preparation run are under
`/home/qin/Work/language-dev-20261103`; `--compiler-root` must point at the main
compiler checkout. The script may run from that checkout or from a worker
worktree, but the supplied `langc`, helper and runtime must be under
`--compiler-root`. The CMake cache must record `BUILD_TESTING=OFF`.

```sh
export MAIN=/home/qin/Work/language-dev-20261103
export PY="$MAIN/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12"
export RELEASE="$MAIN/compiler/build/native-perf-release"
export LLVM="$MAIN/toolchains/llvm-22.1.8/bin"
export XML="$MAIN/toolchains/libxml2-legacy-2.13.9/usr/lib"

"$PY" -B -s tests/performance/binarytrees/prepare.py \
  --out checks/binarytrees-preparation-<new-name> \
  --langc "$RELEASE/langc" \
  --helper "$RELEASE/native/lang_native_helper" \
  --runtime "$RELEASE/native/liblang_native_runtime.a" \
  --llvm-bin "$LLVM" \
  --compiler-root "$MAIN/compiler" \
  --ld-library-path "$XML" \
  --cpu 4
```

The output directory must not already exist. A passed run is a correctness and
artifact check, not a formal performance measurement; no measurement stage is
provided by this fixture.
