# FFI01 C ABI runner

This directory keeps the original `tests/native/c_ffi` contract, fixtures,
bindings and manifests unchanged.  `run_c_ffi.py` is the executable FFI01
runner; `runner_controls.py` is its pure control suite.

No product Source, stdlib, gold, fixture, binding, CMake, catalog, PLAN or
accepted old forged component is modified by these files.

## Legacy mode (default, original CMake argv)

```sh
python3 tests/native/c_ffi/run_c_ffi.py \
  --cli BUILD/woozyc \
  --root /path/to/language-dev \
  --out /path/to/c-ffi-results \
  --clang /path/to/llvm-22.1.8/bin/clang \
  --ar /path/to/llvm-22.1.8/bin/llvm-ar \
  --hosted-runtime BUILD/native/liblang_hosted_runtime.a \
  --native-runtime BUILD/native/liblang_native_runtime.a \
  --forged BUILD/tests/native/lang_c_ffi_forged_test
```

The original arguments remain valid.  Optional additions are `--strace`,
`--readelf`, `--nm`, `--toolchain-root`, `--work-dir`, `--report` and
`--timeout` (informational outer value only; per-command budgets are fixed).

Every legacy invocation writes a new evidence directory:

```text
OUT/runs/<UTC timestamp>-<pid>/
  report.json
  commands.jsonl
  inputs-before.json
  logs/
  traces/
  mirror/tests/native/c_ffi/...
  mirror/tests/fixtures/core/...
  out/...
  work/tmp/
```

Reusing the same `--out` therefore keeps earlier reports, raw bytes, traces,
programs and logs.  The runner also replaces only `OUT/latest-run.json` with a
pointer to the newest run.

The normal legacy branch has the original 43 report rows:

* 8 hosted builds, 8 `readelf` checks, 8 program executions;
* bounds build + bounds program execution;
* core build + core `readelf` + core program execution;
* 8 negative `check` commands;
* 1 link-failure build;
* 1 reference C++ forged execution;
* C fixture compile + archive;
* hosted/native archive `nm` checks (2).

The original extra `git -C ROOT rev-parse HEAD` is kept outside the rows, so a
normal branch performs 44 external commands.  The outer 600 s CMake timeout
and the separate 60 s forged CMake timeout remain Root-controlled.

## Formal public-CLI mode

```sh
python3 tests/native/c_ffi/run_c_ffi.py \
  --mode formal \
  --root /path/to/language-dev \
  --out /fresh/empty/evidence-directory \
  --clang /path/to/llvm-22.1.8/bin/clang \
  --ar /path/to/llvm-22.1.8/bin/llvm-ar \
  --hosted-runtime BUILD/native/liblang_hosted_runtime.a \
  --native-runtime BUILD/native/liblang_native_runtime.a \
  --reference-cli /path/to/reference/woozyc \
  --reference-cli-record /path/to/reference-cli-build.json \
  --reference-source-root /path/to/reference/source/snapshot \
  --reference-native-helper /path/to/reference/native/lang_native_helper \
  --reference-forged BUILD/tests/native/lang_c_ffi_forged_test \
  --candidate-cli /path/to/current/woozyc \
  --candidate-cli-record /path/to/current-cli-build.json \
  --candidate-source-root /path/to/current/source/snapshot \
  --candidate-forged-component /path/to/root-prepared/wz-component \
  --candidate-component-record /path/to/root-prepared/component-build.json \
  --candidate-stdlib /path/to/root-prepared/stdlib \
  --toolchain-root /path/to/toolchains \
  --strace /path/to/strace-7.2 \
  --readelf /usr/bin/readelf
```

A formal `--out` must be new or empty.  A second formal invocation with the
same non-empty `--out` is refused; it never reuses old evidence.

Formal mode runs both roles explicitly:

* reference: the original C++ CLI and the original `forged_test.cpp` target;
  `--reference-native-helper` must be the real `lang_native_helper` that the
  reference CLI launches for native builds.  Its SHA-256 is frozen before any
  command and checked before/after the reference role and at the end; a
  missing or changed helper fails the invocation.  The helper is admitted only
  to the reference build trace allowlist and is never added to the shared or
  candidate allowlist;
* candidate: the current Wz CLI and a Root-prepared, same-Source Wz forged
  analysis component.  Candidate traces still reject every C++ seed, helper or
  unlisted executable.

The candidate component is an input.  This runner never builds it.  Preparing
that component is a separate Root-authorized O2 build with its own 1800 s
budget, outside the original 60 s forged budget.  The old `ed1...` component
and its pinned `run.py` are historical evidence only and are rejected as a
current candidate substitute.

A normal formal branch has 84 external commands / 84 report rows:

* 2 shared C-fixture commands (compile + archive);
* 41 commands per role: 11 builds, 8 checks, 10 program executions,
  9 `readelf` checks, 1 forged execution, 2 `nm` checks.

Reference and candidate logs, traces, programs and rows are separate.

### CLI build-record fields

The record may use any of these source-map spellings:

```json
{
  "status": "passed",
  "cli": {"sha256": "<actual CLI SHA-256>"},
  "source_map_sha256": "<source snapshot map SHA-256>"
}
```

Supported equivalents include `source.map_sha256`,
`source116_map_sha256`, `source116.map_sha256` and
`inputs_before.source116.map_sha256`.  The CLI SHA may also be under
`binary`, `artifact`, `woozyc` or top-level `cli_sha256`.

The runner rejects the role unless the record status is `passed`/`ok`, the
recorded CLI SHA equals the actual file SHA, and the recorded Source map equals
the supplied Source snapshot.

### Candidate component build-record fields

```json
{
  "status": "passed",
  "component": {"sha256": "<actual component SHA-256>"},
  "source116_map_sha256": "<same Source map as the candidate CLI>",
  "stdlib": {"map_sha256": "<supplied stdlib .wz map>"},
  "forged_project": {"manifest": {"sha256": "<forged-project lang.toml>"}},
  "historical": false
}
```

`driver.sha256` or `component_sha256` may replace `component.sha256`.
Stdlib map equivalents include `stdlib_map_sha256` and
`engine_resources.stdlib.map_sha256`.  `historical: true` is rejected.

The candidate CLI, component build record and supplied Source snapshot must
all name the same Source map.  A mismatch is rejected before candidate Source
compilation.  The component receives the forged-project manifest and the
stdlib as its two positional arguments:

```text
<component> <mirror>/tests/native/c_ffi/forged-project/lang.toml <stdlib>
```

The accepted Wz component parser is reused read-only.  Its output must contain
all eight exact CASE lines, both legal baseline lines, positive HIR and
NativeIR coverage, first-code `E_IR_INVALID`, and exactly one
`TOTAL 8passed 0failed`.  A fake `8/8`, duplicate CASE/coverage, zero coverage,
missing mutation target, old C++ output, nonzero exit, timeout or non-empty
stderr is a failure.

If no same-Source component is supplied, the candidate forged row is
`not_run`/pending and the invocation is `incomplete`, never a complete pass.

## Original assertions and budgets

| Branch | Assertions | Budget |
|---|---|---|
| hosted positives (scalars/libc/threads/unicode, O0/O2) | exit 0; ELF has `INTERP`; no `libstdc++`; libc also has `libcrypto` | build 180 s, `readelf` 10 s, program 10 s |
| bounds | nonzero exit and `LANG_NATIVE_PANIC:v1:vec-index:` in stderr; no fixed exit 101 | build 180 s, program 10 s |
| core | exit 0; no `INTERP`; no `NEEDED` | build 180 s, `readelf` 10 s, program 10 s |
| 8 negatives | strict JSON `status=error` and the original diagnostic code | check 60 s |
| link failure | clean nonzero process, strict `error`, `E_LINK_FAILED`, no `out/program` | build 120 s |
| reference forged | original C++ target exits 0; no new stdout/stderr gold | 60 s |
| candidate forged | 8 exact Wz cases and coverage/baseline/TOTAL rules | 60 s |
| fixture / archive / symbols | fixed Clang compile, `ar rcD`, hosted/native symbol rules | 60 s each |
| git HEAD | original extra command, outside report rows | 30 s |

Normal programs have no invented stdout/stderr gold.  Raw bytes and SHA-256
values are retained for every command.  Timeout, signal, launch/`OSError`,
incomplete cleanup or abandoned pipes never count as an expected rejection.

The C fixture and C ABI adapter are compiled with Clang/LLVM and use
libc/crypto/pthread as in the original interop test.  Candidate Woozy Source is
compiled only by the real candidate Wz CLI.  A candidate compiler trace that
contains a C++ seed, helper or any unlisted executable (including through
`execveat`) is rejected; using only a subset of the fixed LLVM tools is
fine.  The reference role keeps its original C++ forged target and
parameters, and its separately supplied native helper is the only extra path
allowed in reference build traces.

## Private work mirror

Before any command, the runner copies the original
`tests/native/c_ffi` tree and `tests/fixtures/core` bytes into its private
run/work mirror.  It excludes old `.build` directories and creates a fresh
private `tests/native/c_ffi/.build`.  Project manifests keep their original
`../../.build` meaning, but it resolves inside the private mirror.  The runner
never writes the owner repository or Root `.build`.  Original fixture,
binding, manifest and Source bytes are compared before/after; an added,
removed or changed input fails the invocation after every already-known
process record has been saved.

Immediately after the shared C fixture object/archive are copied into the
private `.build`, the runner saves the SHA-256 of every shared original and
actual private copy in `fixture-artifacts.json`, and requires each pair to be
byte-identical.  The same shared/private pair is compared before and after
each role and again at the end.  A rewrite or disappearance fails the run and
sets `inputs_stable` to false; the saved initial hashes are never replaced by
the final artifact hashes.

`commands.jsonl` is written through a same-directory `.pending` file and
`replace`.  Each record contains raw stdout/stderr, SHA-256, argv, cwd, actual
timeout, native integer exit, strict booleans, trace path/SHA and
launch/trace-error state before assertions run.  A failed record rewrite keeps
the previous JSON records and the new raw output files.

## Pure controls

```sh
python3 tests/native/c_ffi/runner_controls.py \
  --json-out /tmp/cffi-controls.json
```

Controls block `subprocess`, `os.exec*`/`spawn*`/`fork*`, and socket entry
points before importing the runner.  They call the real runner helpers and
`main` refusal path, and require `external_launch_attempts == []`.  They cover:

* strict native-int exit, strict booleans, timeout, signal and `OSError`;
* NaN/Infinity/`1e1000`/bad UTF-8/missing status/missing diagnostic code;
* missing target artifact and fake build success;
* wrong ELF `INTERP`/`NEEDED`/`libcrypto`/`libstdc++` and archive symbols;
* link failure leaving a program;
* `execveat` helper injection and retained failed-process evidence;
* fake `8/8`, duplicate coverage/CASE, missing target and old C++ forged output;
* CLI/component Source, stdlib and manifest mismatch; historical component;
* input-set addition/removal/change and a Source file disappearing;
* legacy same-`--out` reuse versus formal non-empty refusal;
* record-write failure preserving old JSON and raw bytes;
* post-process resource failure preserving the known process record;
* successful `nm` checks adding both report rows;
* a full legacy orchestration with faked in-memory processes keeping exactly
  43 rows / 44 commands on two runs into the same `--out`, with the first
  report/commands bytes unchanged;
* a full formal orchestration with faked in-memory processes and traces
  keeping exactly 84 rows / 84 commands, including component identity, and
  refusing a second run into the same non-empty `--out`;
* the reference CLI -> `lang_native_helper` trace being accepted and the
  helper SHA recorded, the same helper trace being rejected for the candidate,
  and a helper changed during the reference role failing with
  `inputs_stable=false`;
* shared and private fixture object/archive rewrites and missing files between
  roles all failing with `inputs_stable=false` while keeping the known process
  rows and the initial fixture hashes.

This delivery is a pending-execution migration: writing it did not run any
Source check, compiler, C compile, component build, language program, tool
version or network operation.  Only the pure controls above were executed.
