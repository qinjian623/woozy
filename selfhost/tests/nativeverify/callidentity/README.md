# `callidentity` — direct-callee `Op.Function` component regression

Scope: `nativeverify`'s no-origin `Op.Function` check
(`verify_ops.wz` `check_function_instruction`). The C++ verifier compares the
registered function type of the instruction's symbol with the instruction's
result type (`src/ir/verify.cpp:845-847`: `functions.at(i.function) == i.type`);
`nativelower/expr.wz:612-618` builds such an instruction with
`type_id = data.signature`, `function = direct_symbol` and leaves `signature`
at 0.

## Files

| file | purpose |
| --- | --- |
| `main.wz` | test driver: real `project.load -> resolve -> sema.check_project(Backend) -> nativelower.lower`, then only the published `nativeverify` API (snapshot/verify). No `VerifiedModule`/`VerifiedHirModule` is constructed and no mutation touches the published module. |
| `lang.toml` | project manifest for the driver build (entry `app`, `src/main.wz`). |
| `fixtures/generic/` | `case03_generic` copy (`9a160e9f…`) **plus** one `swap[bool]` instance. The two `swap` calls come first, so direct callee instructions `0`/`1` really are the two instances of the same generic; `identity[string]` still covers another instance. |
| `fixtures/enumclosure/` | byte copy of `case04_enumclosure` (`ae811cd9…`), the `make(Op.Add(3))` source. |
| `run.py` | runner: builds (or replays) the driver, runs both fixtures, checks the driver's own exit status, the accept/reject lines, the swap-instance pair and the NativeIR dump parity with the C++ `ir::dump`. |
| `controls.py` | **test-runner controls only** (see below). Never Source/IR evidence. |

## What the driver checks per real no-origin `Function` instruction

* `signature == 0` (what lowering actually produces);
* nonexistent symbol → `unknown function identity`;
* wrong `type_id` while `signature` holds the symbol's correct function type →
  `unknown function identity/signature` (the port bug accepted this);
* `signature = 987654321` with the correct `type_id` → accepted (the branch must
  not read `signature`);
* two concrete instances of one generic cannot exchange identities;
* for `fixtures/generic` the runner locates both `swap` call spans in the dump
  and requires two distinct symbols with two distinct signatures there, plus
  the driver's `swapped-instance-identity 0/1` rejection. A skipped swap or a
  swap of two different generic functions is not accepted.

`--expect fail` requires the pre-fix tree to reject the real lowering with
`unknown function identity/signature` and the driver to exit **1**.

## Runner policy (test-script behaviour)

* Every command goes through `selfhost/corpus.py::_run_process_impl`: fresh
  process group, group kill on timeout, bounded drain. There is no second
  spawn/kill implementation here.
* For each command, `argv`, `cwd`, timeout, the real exit code and the three
  flags (`timed_out`, `cleanup_complete`, `abandoned_pipes`) plus captured
  stdout/stderr and their SHA-256 are written (`*.command.json`,
  `*.stdout.bin`, `*.stderr.bin`) **before** the command is judged. An `OSError`
  is recorded the same way.
* A timeout, incomplete cleanup, abandoned pipe, signal (`exit < 0`) or an
  unexpected exit code fails the run. In the positive mode the replayed driver
  must exit 0; in the negative mode exactly 1. PASS text can never override the
  process status.
* The C++ reference build that supplies `ir::dump` reports two separate
  statuses: `ir_status` (`verified-dump`) and `build_exit`/`link_status`. A
  timeout or signal is never accepted as a verified dump. The *program* build
  and run of each fixture is a separate step and requires exit 0.
* An explicit `--work-dir` must not exist or be empty. Otherwise the run is
  refused before any copy, delete, compile or log write, and the directory is
  left byte-identical. The default is a fresh `mkdtemp`.
* `--timeout` must be finite and `> 0` (NaN, ±Inf, 0 and negatives are rejected
  before anything runs). The project default is 7200 s.

## Usage

```text
# build a fresh driver and run both fixtures at O0 and O2
python3 run.py --woozyc <stage0>/woozyc --reference <cpp-compiler> \
  --toolchain-root <toolchains> --timeout 600

# replay an existing driver ELF (no rebuild); one program per run
python3 run.py --reuse-program <driver>/program --program-label driverO0 \
  --reference <cpp-compiler> --toolchain-root <toolchains> --timeout 600

# negative half with the pre-fix verifier program
python3 run.py --reuse-program <prefix-program>/program --program-label prefixO0 \
  --expect fail --no-reference --timeout 600

# additionally build+run each fixture Source with a real stage1 compiler
python3 run.py --reuse-program <driver>/program --program-label driverO2 \
  --stage1 <stage1>/woozyc --timeout 600
```

## Controls (`controls.py`)

Small fake commands and a fake seed/driver exercise the runner itself: normal
output recorded, `OSError` record, NaN/±Inf/0/negative timeouts rejected, a
non-empty `--work-dir` (nested files + symlink) refused with identical bytes,
timeout killing the whole helper group, a parent exiting 0 while a child keeps
the pipe rejected, and `PASS` text with exit 42 / SIGKILL / timeout rejected
while exit 0 is accepted. The expected rejections print `FAIL <name>: …` lines;
each is followed by `PASS control <name>`. These prove nothing about Source,
IR, the compiler or the verifier.
