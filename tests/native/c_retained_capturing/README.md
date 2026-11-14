# Retained capturing C callbacks

This directory keeps the original independent C callback runner.  The default
invocation is still the one referenced by `CMakeLists.txt`: it compiles the
user-owned C fixture, runs the source matrix against the compiler named by
`--cli`, and records JSON diagnostics.  The strict paired evidence mode adds
explicit role, trace, persistent command/case records and before/after input
snapshots without changing the compiler, Source, gold, fixtures, headers,
lifecycle test, stdlib or CMake.

## The original route

For one role the runner executes 70 top-level commands:

| Phase | Count | Original budget |
| --- | ---: | ---: |
| 13 positive projects at O0 and O2 | 26 builds | 240 s each |
| `bad_metadata` and `static_capture` no-output O0 builds | 2 builds | 240 s each |
| freestanding `tests/fixtures/core` O0 build | 1 build | 240 s each |
| integration probe check | 1 check | 120 s |
| seven rejection checks | 7 checks | 120 s |
| positive program executions | 26 programs | 30 s each |
| freestanding core program execution | 1 program | 30 s each |
| C fixture shared-library compile | 1 | 120 s |
| standalone C++ lifecycle execution | 1 | 60 s |
| runtime archive `nm` | 2 | 60 s |
| core `readelf -l -d` | 1 | 30 s |
| `git rev-parse HEAD` | 1 | 30 s |
| **total** | **70** | |

Two roles therefore have 140 expected top-level commands.  A matching
standalone C++ lifecycle executable is built once for the whole paired run and
is accounted separately; its build is not one of the 140.

The 13 positive projects, in original order, are `probe`, `move_capture`,
`captures_escape`, `userdata_positions`, `returns_and_units`, `sync_register`,
`two_resource_borrow`, `c_thread`, `failure_cleanup`, `close_modes`,
`typed_errors`, `gc_cleanup` and `cleanup_observation`.  Each program is
required to exit 0.  The original runner does **not** require empty stdout or
stderr, so this runner does not add such a gold value.  The programs' own
assertions remain the source of the positive checks.

The seven rejections and their exact codes are:

| Project | Backend | Expected code |
| --- | --- | --- |
| `throws` | native | `E_CALL_ARGUMENT` |
| `signature` | native | `E_CALL_ARGUMENT` |
| `mode` | native | `E_CAPABILITY_UNAVAILABLE` |
| `bad_metadata` | native | `E_BINDING_INVALID` |
| `python_unsupported` | python | `E_CAPABILITY_UNAVAILABLE` |
| `static_capture` | native | `E_CAPABILITY_UNAVAILABLE` |
| `python_static_unsupported` | python | `E_CAPABILITY_UNAVAILABLE` |

A rejection passes only with a real integer exit 1, JSON `status = error`, the
exact original code, no timeout, no signal, complete process cleanup and no
published `program`/`program.py`.  A timeout, a signal, a spawn error or a
different exit code is never accepted as a rejection.  The two Python rows are
check-only; no Python program is built or executed.  `bad_metadata` and
`static_capture` additionally get one O0 build that must fail with exit 1, the
same code, and no output executable.

The C fixture is compiled per role into that role's private directory with the
original flags and `soname libc_capturing.so`.  The role's
`LD_LIBRARY_PATH` starts with its own fixture directory, so the lifecycle test
and every generated program load the same bytes.  The hosted archive must
define all eight `lang_c_registration_*_v1` symbols.  The native archive is
recorded for information only and has no new symbol requirement.  The
freestanding core must remain free of `INTERP` and `NEEDED` and must exit 0.

`gc_cleanup` keeps only its original assertions: explicit close releases the
external registration root, the still-live graph remains usable, allocation
pressure leaves no C token, and it does **not** assert that the collector has
already reclaimed a cycle.  `cleanup_observation` keeps its separate Weak
upgrade checks.

## Strict paired evidence mode

Build the lifecycle binary once from the original files (the command is
recorded separately in the evidence report):

```bash
mkdir -p /path/to/evidence/lifecycle/link
"$LLVM/bin/clang" --target=x86_64-unknown-linux-gnu -std=c11 -O2 -fPIC \
  -pthread -Wall -Wextra -shared -Wl,-soname,libc_capturing.so \
  tests/native/c_retained_capturing/fixture/capturing_fixture.c \
  -o /path/to/evidence/lifecycle/link/libc_capturing.so

"$LLVM/bin/clang" --target=x86_64-unknown-linux-gnu -std=c++20 -O2 -pthread \
  -I tests/native/c_retained_capturing/fixture \
  tests/native/c_retained_capturing/fixture_lifecycle_test.cpp \
  -L/path/to/evidence/lifecycle/link -lc_capturing \
  -o /path/to/evidence/lifecycle/fixture_lifecycle_test
```

The executable is linked against the same `libc_capturing.so` soname but has
no RPATH.  Each role runs it with `LD_LIBRARY_PATH` beginning with that role's
private fixture directory and with `LD_DEBUG=libs`; the runner parses the
complete pathname after `calling init:` and compares its normalized absolute
path with the role-private file.  A longer filename (for example `.so.wrong`),
a file in another directory, or a missing line is rejected.  The parsed path
and its SHA are recorded.  This is how "the lifecycle really loaded the same
copy" is proven without changing the original C++ assertions.  The C++ test
itself keeps the pre-trampoline reservation, unregister wait, same/different
token, 16 pthread call and all original failure/close assertions.
The Woozy `c_thread` case is not used as a substitute for this C-only
lifecycle test.  This lifecycle also does not exercise Woozy typed-error
unwinding; those assertions remain in the unchanged `typed_errors.wz` Source
case.

Run each role into a new directory:

```bash
python3 tests/native/c_retained_capturing/run_capturing.py \
  --cli /path/to/role/woozyc \
  --root /path/to/repo \
  --out /path/to/evidence/role-run \
  --clang "$LLVM/bin/clang" \
  --toolchain "$LLVM/bin" \
  --hosted-runtime /path/to/liblang_hosted_runtime.a \
  --native-runtime /path/to/liblang_native_runtime.a \
  --stdlib /path/to/actual/stdlib \
  --fixture-test /path/to/evidence/lifecycle/fixture_lifecycle_test \
  --role candidate|reference \
  --strace "$STRACE" --require-trace \
  --source-tests require \
  --proposal-out /path/to/proposal-role.json \
  --extra-input "role-cli=/path/to/role/woozyc"
```

`--source-tests require` runs every Source command even if the probe fails; it
does not turn a failure into `not_run`.  `--fixture-test` is mandatory in that
mode; a missing or non-ELF lifecycle binary is a failure, not a skip.  The
legacy `auto` mode still uses the original integration-missing diagnostic
criterion, but only a clean completed exit 1 with strict process flags, no
spawn error, a clean/absent trace, and a complete finite error JSON can become
a `not_run`.  That parsed error document is retained.  `require` still fails
on the same input; unrelated errors, exit 0 with an error document, malformed
or non-finite JSON, timeout, signal, start failure or a bad trace never become
a skip.  `skip` retains the old "source tests not run" behavior.  Neither
legacy mode is used to claim the strict two-role result.

For the candidate role the compiler trace may contain only the candidate CLI
and the fixed LLVM tools (`clang-22`, `opt`, `llc`, `lld`).  A C++ helper or
frontend in a candidate Source trace fails the run.  For the reference role
the trace may additionally contain its matching `lang_native_helper` and the
system linker used by that helper (`/usr/bin/ld`).  Fixture/lifecycle compiles
have their own exact clang + linker set.  Program, nm and readelf executions
have one-executable sets.  The observed process list and raw trace are saved
for every command.

Every command records its real argv, cwd, integer exit, exact
`timed_out`/`cleanup_complete`/`abandoned_pipes` booleans, timeout, elapsed
time, raw `stdout.bin`/`stderr.bin`, their SHA-256 values and the exec trace.
Compiler JSON is parsed completely and rejects non-standard constants and
non-finite numbers.  Each case writes its result immediately under `cases/`;
the command log is appended immediately; a non-empty output directory is
refused and partial evidence is never deleted.  The report includes the
expected 70/140 counts, the actual counts, the 20 original manifests and their
35 per-role case records (26 positive O0/O2 cases, 7 rejections and 2
no-output builds), the lifecycle/archive/core records, and the cleanup status.

Before the first command the runner snapshots the 20 Source/manifest files,
the three bindings, the C fixture/header, the lifecycle C++ and the shared
scripts, plus the CLI, helper, native/hosted runtimes, fixed LLVM tools,
libxml2, stdlib tree and (for the candidate) its resource and Source trees.
The same files and trees are hashed again after the run; a change fails the
run.  Generated `libc_capturing.so`, its link copy, the lifecycle binary and
every published `program`/`program.o` tree are hashed right after creation and
again at the end.

The proposal written by `--proposal-out` contains only the original 20 family
rows, all `enabled=false`.  The two Python rows are corrected from the
catalog's `build_native`/`native` placeholder to `check`/`python`, because the
original runner only invokes `check --backend python`.  The proposal does not
write or append to the Root catalog.  The freestanding core is runner-only
evidence for this family; its catalog mapping is a separate task (091c984),
and this run does not claim that task's result.  The runner does not add a
Nativeobs driver, a product API, a helper Source build or a relink of the
published `program.o`.

## Legacy invocation

Without the strict flags the runner keeps the old behavior: `--source-tests`
defaults to `auto`, `--role` defaults to `reference`, `--fixture-test` is
optional and tracing is only enabled when `--strace` is supplied.  The old
`auto`/`skip` not-run records are retained for historical compatibility.

## Recorded historical routes

The records below are historical evidence, not commands rerun by every later
runner edit.  This correction did not rerun Source, the compiler, a generated
program, the fixture C/C++ or LLVM.  The rows are kept separate rather than
being presented as one new run.

| Recorded route | Commands | Runner / mode | Meaning and limits |
| --- | ---: | --- | --- |
| Main two-role family result | 140 (70 per role) | `73a2d991…`, strict trace | The original complete candidate/reference result; retained, not rerun here. |
| One-time lifecycle construction | 4 | separate lifecycle build | C shared library, C++ test ELF, `readelf`, staging smoke; not part of the 140. |
| missing-fixture control | 43 | pre-correction `5eb11af…`, O0 | Continued the remaining O0 Source route after the lifecycle was unavailable; it did **not** stop early.  It finally failed because the C lifecycle is required. |
| CMake-like default | 70 | `73a2d991…`, no trace | Default invocation passed; not strict traced evidence. |
| legacy skip | 7 | `73a2d991…`, no trace | 33 `not_run` records, no generated programs, `passed_with_not_run`. |
| version probes | 11 | separate version script | Environment/version record, not family commands. |

The selected recorded routes total 275 commands.  That is not the whole
session: earlier Source probes/smokes, the non-empty-output runner control and
pure Python control launches are additional, so no exact all-session total is
claimed.  The 20 proposal IDs remain unchanged and every proposal row stays
`enabled=false`; the runner proposal evidence is still runner-only and does
not write the Root catalog.
