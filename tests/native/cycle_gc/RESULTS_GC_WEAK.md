# GC + Weak invalidation integration results

Date: 2026-11-12 (Asia/Shanghai)
Scope: accepted GC kernel + accepted 16-byte Weak control block, plus the new
white-candidate Weak invalidation ordering.  This does not claim automatic GC,
source-level `Weak[T]`, Root safepoints, or the whole MEM02 goal.

## Fixed toolchain

```text
clang++: ${WOOZY_TOOLCHAIN_ROOT}/llvm-22.1.8/bin/clang++
         clang version 22.1.8 (ca7933e47d3a3451d81e72ac174dcb5aa28b59d1)
ld.lld:  ${WOOZY_TOOLCHAIN_ROOT}/llvm-22.1.8/bin/ld.lld
         LLD 22.1.8 (same LLVM build)
legacy:  ${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9
```

## Command and result

```sh
CYCLE_GC_OUT=/tmp/gcweak-final2-kernel \
CYCLE_GC_WEAK_OUT=/tmp/gcweak-final2-int \
LANG_WEAK_BUILD_DIR=/tmp/gcweak-final2-weak \
LANG_WEAK_KEEP_BUILD=1 \
timeout 900s tests/native/cycle_gc/run_all.sh
```

Exit code `0`.  Full log: `/tmp/gcweak-final2-all.log`.

- GC kernel: host O0/O2, static O0/O2, ASAN/UBSAN all `CYCLE_GC_ALL_PASS`.
- Weak: host O2/O0, ASAN/UBSAN, static O2/O0 all pass; new invalidation phases
  pass and final `weak_test: PASS (control blocks live=0)`.
- GC+Weak integration: host O0/O2, ASAN/UBSAN, static O0/O2 all
  `GC_WEAK_ALL_PASS`.

Static ELF checks for every static artifact:

```text
Type: EXEC
no INTERP
no NEEDED
no strong undefined symbols
```

## Regression evidence

The original `/tmp/cycle-gc-weak-integration.cpp` with the old Weak source and
four-field `LangGcOps` still prints:

```text
status=0 candidates=2 collected=1 destroyed=1 weak_upgrade_some=1 before_clear=0 after_gc=1
```

and exits 42, as before.  Adapting the same program to the new trailing
`invalidate_weak` callback and current Weak source prints:

```text
status=0 candidates=2 collected=2 destroyed=2 weak_upgrade_some=0 before_clear=-1 after_gc=-1
```

The first-clear probe in `gc_weak_integration_test.cpp` additionally asserts
that both candidates were already invalidated and pinned (`refs == 2`) before
clear #1, that upgrade/downgrade of the white peer return null, and that the
slot is permanently closed.

## Source hashes

```text
f4293e8ee2c983f725a07300a7f18221b0762a38ae70b4b65142893ee8760774  runtime/native/gc.hpp
ea330276634e86721f135520543089e25d6b9afab95d9ade827df371f6339a25  runtime/native/gc.cpp
690a88b8667f0cf85f914191e088eaa5b1f7a110cba2ce037bc64b0abeb700bb  runtime/native/weak.hpp
7e8d887bdc9a88a148baca1dc5f1f32fd51ae709686464801ed215785c4b80e8  runtime/native/weak.cpp
731b8d720dee6b065cf90df93dd3368d1067823119bffc6c0864f79be0c04e78  tests/native/cycle_gc/CONTRACT.md
f7ae04dab29533c4392f2b0b2854ce8d55108b5e2cf21a367e710339a72960cc  tests/native/cycle_gc/cycle_gc_test.cpp
b2143ab3145019d685aef879afaefeb7fbabdeb8a92d16d3a47ea8847177cf61  tests/native/cycle_gc/gc_weak_integration_test.cpp
5c72a9e558bb17b036952edb9dce472507ed27139f0a90e60243fc58df0e6ca5  tests/native/cycle_gc/run_tests.sh
7766e1c37510a5c6137c12a6c5569ab6b9a9d3570f25a32d1aa8ecedeed5665c  tests/native/cycle_gc/run_gc_weak.sh
b9da7ec00edb45e918f74d98be92335186a5e04bbf76278abf349cd6b3108e51  tests/native/cycle_gc/run_all.sh
8878852756b38d3532dbf63aed39f638a75772ed82bb74bf0cf451290b1f6a4a  tests/native/cycle_gc/sanitizer_alloc.cpp
72a1917d368b4fe9ab7aaca2cb788558a421c493d658d37d85748abae8edc41d  tests/native/weak/weak_test.cpp
936685aab27aea576eccf03bf52cdf4a364207a7bad148f289755ada5e24fd39  tests/native/weak/run.sh
```
