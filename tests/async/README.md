# Async source cases

`run_async_cases.py` has two modes.

## Legacy helper mode (default)

The original mode is unchanged: `woozyc build` for the 19 positive and 6 reject
manifests, then the native helper emits a separate object for each of the 12
observed subsets at O0/O2. The helper source is linked with
`tests/async/async_observer.cpp`, the nine `--wrap` symbols, and the matching
runtime. `--observe auto` still skips observed cases when a prerequisite is
missing; `--observe on` fails instead. `tests/async/CMakeLists.txt` keeps using
this mode.

## Public-object mode

The public-object checks are tied to the frozen 37-file stdlib snapshot used by
the reference and candidate roles. A newer stdlib tree (for example one that
adds `std/http_server`) must be supplied as a frozen snapshot through
`--stdlib-root`; the runner intentionally rejects a different file count so a
mixed frozen/current input set is never accepted.

`--public-object` migrates only the observation source of the object:

1. The selected role CLI performs all 44 public Source builds: 19 positive
   manifests at O0 and O2 plus 6 reject manifests at O0.
2. The 38 positive `program` files are executed with the original exit/stdout
   checks. A `stderr` field is checked only when `cases.json` contains one; no
   empty-stderr gold is invented.
3. The original `async_observer.cpp` is compiled once with the original
   freestanding C++ flags.
4. Each of the 12 observed cases reuses the already-published
   `<case>_O<opt>/program.o` from step 1, links it with the original observer,
   the matching role runtime, `-nostdlib -static --ld-path=...`, and the
   original nine wraps, then executes the observer.
5. The observer must pass the original future/frame/Vec balance checks and
   every exact field in `cases.json` `counts`: `bad_frame_free`,
   `task_retain`/`task_release`, and `vec_retain`/`vec_release` included.

The full mode therefore has 131 top-level commands per role (44 builds + 38
programs + 1 observer compile + 24 links + 24 observer executions) and 68
checks (38 + 6 + 24). It does not compile Source a second time for observation.

The mode is fail-closed:

- `--helper` is rejected; a candidate role also rejects `--trace-allow` and can
  never invoke a C++ helper.
- A reference role must pass exactly one `--trace-allow` pointing at the
  `lang_native_helper` beside its own CLI.
- `--observe auto` is treated as `on` in public-object mode. A missing
  observer, runtime, clang, ld, strace, or tool fails; it does not auto-skip.
- Every Source build runs under `strace --seccomp-bpf -f -qq -s 16384 -e
  trace=execve,execveat` and is audited against the role allowlist. Candidate
  builds allow only the candidate CLI plus the fixed LLVM tools. Reference
  builds additionally allow only the matching helper. Observer compile, link,
  and execution are recorded separately and are not part of the Source
  allowlist.
- The output directory must be new or empty. Failures continue with the
  remaining cases; partial evidence is retained. Every command writes raw
  stdout/stderr bytes, SHA-256, process fields, cwd, timeout, and trace
  immediately. Exits are real ints; `timed_out`, `cleanup_complete`, and
  `abandoned_pipes` are strict bools. Timeouts must be positive finite
  numbers. A killed process keeps its real exit and timeout flags.
- Recording a failure and accepting a pass are separate. A command may be
  retained with its real failure flags, but program, positive-build,
  reject-build, observer compile/link/execute, and the final summary accept a
  success only when `timed_out` is `false`, `cleanup_complete` is `true`, and
  `abandoned_pipes` is `false`. A legal expected exit 1 and a normal nonzero
  reject exit remain valid when those cleanup conditions hold.
- Before/after SHA-256 sets cover all 25 manifests and Sources, `cases.json`,
  the observer, the nine wraps, the runner, the runtime, headers, tools, both
  CLIs and the reference helper, and the role's 37-file stdlib tree. The 116
  self-host compiler sources are recorded separately from the async fixtures.
  Candidate runs require `--candidate-source` to identify the compiler's
  frozen source tree, and its nonempty map must match `$ROOT/selfhost/src`.
  Verify that tree against the CLI's build record as well as its binary SHA.
  `matches_frozen_727b` is a historical comparison field; a new matching source
  pair can pass with a different digest. Before/after input checks still apply.
- Reject builds must exit normally with a nonzero code, report the original
  diagnostic code, and publish neither `program` nor `program.o`.

Example (paths are role-specific):

```bash
python3 tests/async/run_async_cases.py --public-object --role candidate \
  --cli "$CAND/woozyc" --runtime "$CAND/runtime/liblang_native_runtime.a" \
  --toolchain "$LLVM" --strace "$STRACE" --libxml2-lib "$LIBXML" \
  --root "$ROOT" --out "$EVIDENCE" --stdlib-root "$CAND/stdlib" \
  --candidate-source "$FROZEN_SOURCE116" \
  --proposal-out "$REPORT/proposal-candidate.json" --observe on
```

The reference command adds `--trace-allow "$REF/native/lang_native_helper"` and
uses the reference CLI, runtime, and stdlib. `--proposal-out` writes a proposal
that only updates the 25 pre-existing catalog ids and the existing runner row;
it does not modify the catalog or append duplicate cases. Each case's
`adaptation` is derived from its actual required results: positive cases need
both ordinary O0 and O2 results, observed cases additionally need both observer
results, and reject cases need their own reject result. All required results
must be `passed`; a missing or failed result stays `pending`. The exact observer
fields are not representable in the generic corpus schema and remain separate
runner evidence.

## Self-test

`--self-test` exercises the same validators used by public mode without a
compiler: missing observer/counts fields, wrong exact counts, unbalanced
allocations, bad process flag types and unsafe cleanup values, summary
rejection of timeouts or dirty cleanup, ready/pending proposal derivation,
bad timeouts, nonzero exits, legal expected exit 1, wrong object path/opt,
published reject artifacts, and auto-skip refusal. It writes JSON with
`--self-test-out`.
