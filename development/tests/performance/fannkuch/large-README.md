# Fannkuch-redux large-input and rounds preparation

This file records pre-release measurements made with the old `langc` / `.wz` names. The archived hashes and observations below retain those names. Current releases use `woozyc`, `.wz`, and the runner option `--woozyc`; see the current [performance summary](../../../docs/PERFORMANCE.md).

This directory now has a separate preparation entry point for n=9/10:

```text
tests/performance/fannkuch/large_prepare.py
```

It reuses the existing `prepare.py`, `run_native.py`, and Sieve preparation
helpers for builds, commands, hashing, ELF inspection, metadata, LLVM exports,
verification, and JSON writing. The existing n=3..7 templates, runner, Oracle,
and delivery files are inputs and are not modified.

This is a correctness and compiler-artifact preparation step. It performs no
timing, records no throughput, and makes no speedup or `<=2x` claim.

## Fixed large expectations

The only expected values are read from the hash-pinned frozen oracle at
`/home/qin/Work/language-dev-20261103/checks/fannkuch-large-oracle-20261110`.
The script verifies `results.json`, `inputs.json`, `verify.py`, the oracle's
`source_sha256` map, all four n=7..10 records, the ordered trace SHA-256 values,
and the permuted-record comparison flags before and after the run.

| n | permutations | checksum | maximum |
| --- | --- | --- | --- |
| 7 | 5040 | 228 | 16 |
| 8 | 40320 | 1616 | 22 |
| 9 | 362880 | 8629 | 30 |
| 10 | 3628800 | 73196 | 38 |

The oracle states that two independent methods produced identical
index/permutation/flips records for every index: recursive prefix rotations
with slice flips, and itertools permutations with inverse mixed-radix ranking
and pair-swap flips. This preparation task did **not** re-run that full Python
trace and did not dynamically observe every n=9/10 permutation. It only checks
the frozen per-index evidence and the compiled program's complete
checksum/maximum value.

Pinned oracle hashes:

| File | SHA-256 |
| --- | --- |
| `results.json` | `67803f179ff1b50134d88b1def7553fe5ccd5ea2445529d9a46164d709489c5d` |
| `inputs.json` | `70fd05c73f054919619128a93a4d572f44a287416a4be8353839581e61a60da6` |
| `verify.py` | `0990beebb7d54d6869f512ef32d5c1c39065ceb1bdf907dadca383b5894a220c` |

## Rounds wrapper

`large_prepare.py` reads the old Source/C++ template text, locates the old
`main` as one exact unique match, and replaces only that text. It then fills the
old placeholders. The algorithm function prefix is checked against the old
`prepare.py` renderer byte-for-byte; the generated program's algorithm prefix
hashes are recorded separately from the new main hash.

The new main is:

```text
for round in 0 .. rounds-1:
    result = fannkuch(n)          # complete call, three working arrays allocated inside
    result.checksum == expected checksum ? else exit 1
    result.maximum   == expected maximum  ? else exit 2
exit 0
```

There is no total accumulator, so arbitrary positive `--rounds` do not create a
new overflow requirement. Every round performs a complete `fannkuch(n)` call;
the three working arrays are allocated and released inside that call. No asm,
volatile, noinline, optnone, explicit optimization attribute, SIMD, threads, or
input-algorithm change is added.

Command line:

```text
--n 9 | --n 10
--rounds <integer greater than 0>   (default 1)
```

## Final runs

All four final runs used the same script and tool hashes, CPU 4, and the
dedicated production Release tree. Output root:

`/home/qin/Work/language-dev-20261103/checks/fannkuch-large-prepare-20261110`

| Run | n | rounds | Report SHA-256 | Status |
| --- | --- | --- | --- | --- |
| `n9-r1-final` | 9 | 1 | `e18b9f8f8c5af1e056bdd3c6c2f92dda60e2745e7b3ce201ace0d2e8ab0bfac3` | passed |
| `n9-r2-final` | 9 | 2 | `2c9aa8dee06c57dbc543a99814c6a217f3e61f979d18845b24be3caa6dca9d10` | passed |
| `n10-r1` | 10 | 1 | `d2ba5569647a2eb66f24f66a5c4e1c3846e3364d88da4f597f16bba32b4ffe60` | passed |
| `n10-r2` | 10 | 2 | `8f3828162e6c4a280d98c39f4f5b358e0c7af59e64fc26740fa43cebf61c367a` | passed |

A combined index is at
`/home/qin/Work/language-dev-20261103/checks/fannkuch-large-prepare-20261110/summary.json`
(SHA-256 `b8435c377fe17e0ef2e9f9079a0b0fdd4117e8a6ecb2047b15a608199504d5e7`).

Exact command template (one run shown):

```sh
export MAIN=/home/qin/Work/language-dev-20261103
export RELEASE="$MAIN/compiler/build/native-perf-release"
export LLVM="$MAIN/toolchains/llvm-22.1.8/bin"
export XML="$MAIN/toolchains/libxml2-legacy-2.13.9/usr/lib"
export PY="$MAIN/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12"
export PYHELPER="$MAIN/compiler/build/native-cli-release/tests/python/lang_python_emit"

"$PY" -B -s tests/performance/fannkuch/large_prepare.py \
  --out "$MAIN/checks/fannkuch-large-prepare-20261110/n9-r1-final" \
  --n 9 --rounds 1 \
  --oracle-dir "$MAIN/checks/fannkuch-large-oracle-20261110" \
  --python "$PY" --python-helper "$PYHELPER" \
  --langc "$RELEASE/langc" --helper "$RELEASE/native/lang_native_helper" \
  --runtime "$RELEASE/native/liblang_native_runtime.a" \
  --llvm-bin "$LLVM" --compiler-root "$MAIN/compiler" \
  --ld-library-path "$XML" --cpu 4
```

The other three runs use the same arguments with `--n/--rounds` and their own
new `--out` directories (n9/r2, n10/r1, n10/r2).

Each final report records every subprocess `argv`, status, exit, stdout,
stderr, timeout, CPU, and elapsed nanoseconds. The R1 reports have 61 commands
(53 exit 0, four expected exit 1, four expected exit 2); the R2 reports have 62
commands (54 exit 0, four expected exit 1, four expected exit 2). No command
timed out. The exit-1/exit-2 commands are the intended wrong-checksum and
wrong-maximum controls.

## Result matrix

Every final run performed the same correctness matrix:

| Check | Actual per run |
| --- | --- |
| Source O0/O2 and C++ O0/O2 correct executions | 4 / 4 quiet exit 0 |
| Wrong checksum in both implementations and both optimization levels | 4 / 4 quiet exit 1 |
| Wrong maximum in both implementations and both optimization levels | 4 / 4 quiet exit 2 |
| Source helper O0/O2 exports | 2 |
| Source before/after LLVM verifier runs | 4 |
| C++ O0/O2 LLVM verifier runs | 2 |
| All LLVM verifier runs | 6 |
| Helper object versus CLI `program.o` byte matches | 2 / 2 |
| Source O0 positive visit-call cases | 1 |
| Source O2 zero visit-call cases | 1 |
| C++ O0 positive visit-call cases | 1 |
| C++ O2 zero visit-call cases | 1 |
| ELF checks (Source O0/O2, C++ O0/O2) | 4 |

Source `visit` is identified through the generated Python metadata symbol, not
a hard-coded number and not a text search. In these runs it is symbol 21 in
every case, with O0 call counts 1 before/1 after, and O2 1 before/0 after.
C++ uses the O0 debug metadata linkage
`_ZL5visitmRKSt6vectorIiSaIiEEi`, with O0 count 1 and O2 count 0.

The C++ O0 IR still has the real `fannkuch` function with eight loops and three
vector constructors. The O2 IR inlines it into `main`; the recorded debug
locations attributed to `fannkuch` are 143 for n=9 and 151 for n=10, with three
heap allocations still present. Source O2 likewise inlines the algorithm into
`main` and removes the empty visit call.

## R2 round-loop evidence

The R2 check is not based only on the presence of a loop or on the checksum.
For each O2 program, the script locates a depth-1 loop and inspects its actual
blocks:

| Run | Impl | Mode | Blocks | Nested loops | Per-round evidence |
| --- | --- | --- | --- | --- | --- |
| n9/r2 | Source | inlined, no fannkuch definition | 45 | 7 | 3 `lang_vec_new_v1`, 27 releases, 27 inits |
| n9/r2 | C++ | inlined debug body | 25 | 5 | 3 `_Znwm`, 3 `_ZdlPvm`, 138 fannkuch debug locations |
| n10/r2 | Source | inlined, no fannkuch definition | 46 | 7 | 3 `lang_vec_new_v1`, 28 releases, 30 inits |
| n10/r2 | C++ | inlined debug body | 26 | 5 | 3 `_Znwm`, 3 `_ZdlPvm`, 146 fannkuch debug locations |

Both Source and C++ reports have four real correct executions (O0/O2), so the
R2 requirement for at least one real correct exit on both ends is exceeded.
The round-loop proof is static IR evidence; it is not a dynamic all-permutation
trace. If the proof did not match, the script would record `not_verified`
rather than fall back to a weaker loop/checksum assertion; it does not add
optimization-disabling source.

## ELF checks

Native Source programs have no `INTERP`, no `NEEDED` entries, and no strong
undefined symbols. C++ programs are dynamic executables. Their separately
listed NEEDED libraries are:

```text
libstdc++.so.6
libm.so.6
libgcc_s.so.1
libc.so.6
```

The C++ dynamic undefined symbols in the reports are the expected standard
library/runtime imports (for example `_Znwm`, `_ZdlPvm`, exception/runtime
symbols, and `__libc_start_main`). No Python library appears in either ELF.

## Inputs, tools, and hashes

Source inputs were snapshotted before and after every run and were unchanged:
the two old templates, `prepare.py`, `visit_trace.py`, `large_prepare.py`,
`run_native.py`, the frozen oracle files, and all tool binaries.

| Input | SHA-256 |
| --- | --- |
| `large_prepare.py` | `ec2f1ca2f14dbaa6aea03670dacd0d53375523c9b42c4aa6ed979e8ec2b8ada9` |
| `fannkuch.wz.in` | `3d2b2c1de4ef9894dcff3645c7fe7cf0a420ec024d46cf19b49a31e8f68028a5` |
| `reference.cpp.in` | `085dcac906d1db8d1432d8e8175c16bf6a198271c119c88ab84113cc35f75ae2` |
| `prepare.py` | `733c2da0a63154f6a952e9d3a96162c71ce95e8cca581266f76ed2838a6d2474` |
| `visit_trace.py` | `e3685f2d1daf0f523c56624fb6165247c3ed0d8b0d9195a5253011fbd5bfb1fe` |
| `run_native.py` | `110f8ed89807f2f7d137bfd0677710badf0fb67a526f48befe06f349a51e00fd` |

Actual tools used by the final runs:

| Tool | SHA-256 |
| --- | --- |
| `langc` | `a17e1cc3753371a3588db3b44a5fab36e3caff1440f9889997e416008bab73ec` |
| `lang_native_helper` | `a4fcd602f221562d52c9a4b2f44d7730ec9041ed210be362732aac225beae780` |
| `liblang_native_runtime.a` | `f0c676c60a6982c5d0497e0f685ed58e3d57cc961e62ab9c62bf8579a1376d02` |
| `lang_python_emit` | `ec507db4297fbf9ca17844649040da888d60ca2468e1db5e1c864b7650b93116` |
| `clang` / `clang++` | `31a40dc31f3c15a47aa119aa148f339bf363d8f61202ddbdacbd3f24b71ba113` |
| `ld.lld` | `a37ea59e863b4c1fd95ead6a905f7ddb461f7d90d4f30d2d76bcbb2ae9588b1d` |
| `opt` | `fc19bcd0ab81c35746c93aad355e8bb81f34ab070e98014cf06ce8433e91d4b1` |
| `llvm-readelf` | `338c233bc2419b0abdb8f0344912504c26eeab1cd671d6651f4bb8405bb48385` |
| `llvm-nm` | `3f2d9af2cd8640a62796191afe29fca84530a2ecdd1b1995f34e1ee235d9ec88` |

The product baseline requested for this operation is `797c382`. The worktree
HEAD during the final runs was `f9b3e5c`; the compiler root HEAD was
`47a64c8`, with later unrelated test commits present and the pre-existing
untracked `not a tty` entry. These HEAD records are informational only. The
actual tool binary hashes above are the evidence used for these runs; no
rebuild was performed and no tool change was inferred from a HEAD delta.

## Generated main hashes

The algorithm-prefix hashes are independent of the wrapper and match the old
renderer. The new-main hashes show the rounds wrapper actually changed only
`main`:

| Run | Source prefix | Source main | C++ prefix | C++ main |
| --- | --- | --- | --- | --- |
| n9/r1 | `63dfe2c3cd95955c86be6ef6f02fb68963059e5db1a2e3734470e070a0210e91` | `bd3a2e1aae9b22c79391e221e3556aa6bbb845773a8c3ef2ffec9d8e146ef5ad` | `08a2127e5622c4dbfa7a020acc83756ec6b588e24761023012f201ac0c7c46e1` | `62f36e327989e5996c6439b014ab50dc130f703210fbde6cd81c7fa07ce6a9b4` |
| n9/r2 | `63dfe2c3cd95955c86be6ef6f02fb68963059e5db1a2e3734470e070a0210e91` | `6d57da924d5af6c9c95bd8b77099dec6f44422cd5192feb1572cff0508acf88f` | `08a2127e5622c4dbfa7a020acc83756ec6b588e24761023012f201ac0c7c46e1` | `284fe6b87ab38c835f46a93947a7409ef831ecbefe26d42f02996a319ef4c8dc` |
| n10/r1 | `59697291649b1d6115fd0395d1bd320e39eb18cd58c543032a0fee82c9b50501` | `50389fd24f8ca5389c96a95398fda813015f19a10878cda3234cb6a60246d7e0` | `f324e009182f55af63dde2a4753beebcbbf4333609461cd14957bf803e9f4035` | `ac5f41f3c880f73c7b3b4b2edbfff1651a68289dc32786a136094692b498c49a` |
| n10/r2 | `59697291649b1d6115fd0395d1bd320e39eb18cd58c543032a0fee82c9b50501` | `5c2dd077b14ee39542188bfec58a804f2b109e5e76c12da771643eec1011ab70` | `f324e009182f55af63dde2a4753beebcbbf4333609461cd14957bf803e9f4035` | `2c00cdec4caf16aff30f8e902e8386707e9b34fce6553ffe0639e40c839844ba` |

## Rejected or not run

- No formal timing, throughput, speedup, cold-cache study, or `<=2x`
  comparison was performed.
- The full n=7..10 Python oracle trace was not re-executed; the hash-pinned
  frozen oracle supplies its existing per-index evidence.
- The old n=3..7 prepare behavior was not re-run in this operation. Its files
  and input bytes are unchanged and are recorded by hash.
- No n=8 execution was added; the script accepts n=9 or n=10.
- R2 work evidence is static IR analysis, not dynamic instrumentation of every
  round or permutation.
- No asm/volatile/noinline/optnone, SIMD, threads, LTO, fast-math, or
  `-march=native` was added.

## Development attempts

Pre-delivery R2 validation exposed two script-only failures before the final
runs: the first n9/r2 report failed with `KeyError: 'after'` because the helper
row exposes the after-IR path through its verifier record, and its correction
failed with a list/string block-join `TypeError`. Those directories are kept
for evidence but are not used as passing final runs. The four final reports
above use the same final script hash and all completed with `status: passed`.
