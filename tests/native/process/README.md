# process public runner

`run_process.py` keeps the original route and assertions: O0/O2 native builds,
`readelf -l -d`, a normal `probe😀` run, raw-`0xff` environment and argv runs,
misuse build/run, and one Python rejection build.  Every command keeps the
original 60 second budget.  The existing CMake invocation still works:

```sh
python3 tests/native/process/run_process.py \
  --cli <woozyc> --root <repository> --out <fresh-output>
```

The output directory for a fresh legacy run is used directly.  The default
`--cli/--root/--out` invocation may be repeated with the same argv: when
`out` is already non-empty the runner allocates a private `out/rerun-N`
directory for the new raw records and programs, so the previous raw bytes and
programs are left untouched.  The latest top-level
`out/process-source-results.json` is written at its original location rather
than being moved into the private run directory.  `--record` and the
`candidate`/`reference` roles are strict: both `out` and the record directory
must be empty, and a partial failure is left in place and never overwritten by
a same-name rerun.

## Evidence options

The optional flags add records; they do not change the command route or
assertions:

- `--role {legacy,candidate,reference}`: labels the role and selects the
  compiler-process allowlist.  `legacy` is the CMake/default mode.
- `--trace <strace>`: wraps every command with
  `strace -f -qq -s 16384 -e trace=execve,execveat` and stores the raw trace.
- `--record <directory>`: record root (defaults to `--out`).
- `--peer-cli <woozyc>`: the other role's CLI, hashed before/after.
- `--candidate-source <source/src>`: the candidate's frozen 116-file Source.
- `--llvm-bin <llvm/bin>`, `--libxml <libxml2.so>`, `--python <python>`:
  fixed resource identities.
- `--expect-cli-sha256`, `--expect-candidate-source-sha256`: fail closed when
  the supplied identity is not the expected one.

A candidate run does not allow a C++ helper or frontend in its traced
subprocesses.  A reference run allows its matching `lang_native_helper` plus
the fixed LLVM tools.  The runner records every command's actual argv (with
raw bytes as hex), cwd, timeout, elapsed time, integer exit, strict
timeout/cleanup/pipe flags, raw stdout/stderr, SHA-256 values, output artifacts,
input before/after snapshots and the complete exec trace.  The input snapshot
includes the actual `selfhost/corpus.py` loaded by `load_corpus(root)` and the
actual GNU readelf resolved from `PATH`/`/usr/bin/readelf` (path and SHA-256).
An LLVM `llvm-readelf` from `--llvm-bin` is recorded as a separate
`llvm-llvm-readelf` resource and is never used as proof of the GNU tool.

`strace` records failed execs too.  For the normal program the runner requires
two successful `/bin/sh` execs and three failed `/definitely/missing/woozy`
execs with `ENOENT`; the actual shell path and SHA are recorded rather than
assuming a particular distribution.  Invalid UTF-8 is recorded as raw `0xff`
bytes and is never replaced by U+FFFD.  The misuse program must exit 101 with
`LANG_NATIVE_PANIC:v1:runtime-handle-misuse:` on stderr.  The Python rejection
must be exit 1 with complete JSON containing `E_CAPABILITY_UNAVAILABLE` and no
`program.py`; timeout, signal and spawn failure are not accepted.

## Controls

`test_controls.py` is pure Python and does not build or run Source, start a
subprocess or use the network.  It keeps the original 33 checks (strict
process flags, panic marker, raw UTF-8 bytes, complete JSON with NaN/Infinity
rejection, trace success/error parsing and strict non-empty output refusal)
and adds exact-int checks (`False`/`True` are rejected through the real
`command` record branch), repeated legacy `--cli/--root/--out` calls with old
raw/program byte preservation, strict `--record` non-empty refusal, and
before/after resource keys for the actual `corpus.py` and GNU readelf.

```sh
python3 tests/native/process/test_controls.py
```
