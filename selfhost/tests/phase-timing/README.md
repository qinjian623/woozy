# Private phase timing (test-only)

This directory is a **private diagnostic component**, not a product CLI and not
a replacement for `selfhost/tests/nativeobs` or any public test.

## What it does

`main.wz` performs the real public Native success route in order:

```text
project.load
  -> resolve.resolve_project
  -> sema.check_types
  -> hirverify.verify(Backend)
  -> nativelower.lower
  -> nativeemit.emit
  -> nativebuild.build
```

`sema.check_project` is exactly `check_types` followed by `hirverify.verify` on
success.  This driver repeats those two public calls with the same success and
failure forwarding; it does not remove validation, add a snapshot, or add a
`nativeirdump` step.

Before and after each phase it reads `std/time.monotonic` and writes an
immediate JSON line to stderr:

```text
PHASE_TIMING {"schema":1,...,"phase":"project.load","event":"begin","ns":...,"status":"begin"}
PHASE_TIMING {"schema":1,...,"event":"end","ns":...,"status":"ok","begin_ns":...,"elapsed_ns":...,"counts":{...}}
```

stdout carries one independent strict JSON summary.  If a timeout kills the
process, the last `begin` line identifies the phase that was running; unentered
phases are never inferred.

## Runner

`run.py` stages the current `selfhost/src/wzcc` (the frozen da69 Source116
production map) plus this directory's `main.wz` and `lang.toml`, then:

1. verifies the fixed Source116, stdlib, seed, helper and runtime SHA values;
2. builds the private driver **once** at O2 with the fixed C++ seed, budget
   `1800s`, under `strace` with an explicit allowlist that may include the
   matching C++ helper only for this build stage;
3. places fixed `stdlib`, `runtime`, and LLVM `toolchain` links beside the
   driver to select fixed resources; the measurement exec allowlist separately
   rejects C++ seed/helper execution (the shared runtime contains the helper);
4. invokes the driver **once** on the unmodified
   `tests/native/stdlib-io-errors/lang.toml` with a separate `60s` budget,
   under an allowlist containing only the Wz driver and fixed LLVM tools;
5. hashes the generated `program`/`program.o`, but never executes them.

A fresh unique work directory and report path are required.  The runner refuses
non-empty output directories and never retries a failed build.

This is a diagnostic experiment.  It is **not** a pass of the original
20-second test, not a full CLI build, and not a claim that performance is fixed.
All earlier 20s / 1200s / 21600s failures and budgets remain unchanged.

Root 审查副本补充阶段顺序、单调时间和 begin/end 配对检查，并修正 C++ helper/seed 路径比较。原始两条命令及原始 runner 未覆盖；此副本只做纯脚本检查，未重跑构建或计时。
