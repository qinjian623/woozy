#!/usr/bin/env python3
"""Component regression for direct-callee `Op.Function` verification.

Runs the real self-hosted pipeline (project.load -> resolve ->
sema.check_project -> nativelower.lower) on the Source fixtures and exercises the
published nativeverify API:

  * the unmodified lowered module must verify,
  * every no-origin Function instruction must keep `signature` 0 and carry its
    own instance signature in `type_id`,
  * a nonexistent function symbol is rejected with "unknown function identity",
  * a wrong `type_id` is rejected with "unknown function identity/signature"
    even when `signature` holds the correct symbol signature,
  * an unrelated `signature` with the correct `type_id` is accepted,
  * the two concrete instances of the same generic cannot exchange identities.

For the generic fixture the runner additionally resolves the two `swap` call
spans in the real NativeIR dump, requires two distinct symbols with two
distinct signatures there, and requires the driver's
`swapped-instance-identity 0/1` rejection line: a skipped or
different-generic swap is not accepted as passing.

The Wz `nativeirdump` of the lowered module is compared line-by-line with the
real C++ `ir::dump` of the same Source (`WZ_DUMP_NATIVEIR` on the reference
compiler).

Process policy (test-script behaviour, not Source/IR evidence):

  * every command runs through `selfhost/corpus.py::_run_process_impl`, i.e. a
    fresh process group with a bounded group kill and bounded drain;
  * argv, cwd, the real exit code and the three flags (timed_out,
    cleanup_complete, abandoned_pipes) plus the captured stdout/stderr and their
    hashes are written *before* the command is judged; an OSError is recorded
    the same way;
  * a timeout, incomplete cleanup, abandoned pipe, signal or an unexpected exit
    code fails the run;
  * an explicit `--work-dir` must not exist or be an empty directory; otherwise
    the run is refused before any copy, delete, compile or log write.

--expect=fail runs the identical checks against a source tree with the port bug
restored; the driver must then exit 1 (not a signal, not a timeout) and the
lowering must report `unknown function identity/signature`.

--reuse-program runs an already built driver ELF instead of building one, so the
existing O0/O2/prefix programs can be replayed against corrected fixtures.
--stage1 adds an independent Native O0/O2 build+run of each fixture Source with
a real self-hosted stage1 compiler (no driver involved).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import re
import shutil
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SELFHOST = ROOT / "selfhost"
if str(SELFHOST) not in sys.path:
    sys.path.insert(0, str(SELFHOST))
from corpus import _run_process_impl  # noqa: E402

DEFAULT_STAGE0 = ROOT / "build/woozy-stdlib-native/woozyc"
DEFAULT_TOOLCHAIN = ROOT.parent / "toolchains"
DEFAULT_TIMEOUT = 7200.0

FIXTURES = ["generic", "enumclosure"]
BEGIN = "=== NATIVEIR BEGIN ==="
END = "=== NATIVEIR END ==="
REFERENCE_TAIL = re.compile(r"^(build ok:|E_[A-Z_]+:)")
FUNCTION_LINE = re.compile(
    r"^\s*%\d+ = function type=(\d+) @(\d+) source=(\d+):(\d+)-(\d+)")
SWAP_CALLS = ("swap[i32](7, 9)", "swap[bool](true, false)")

REQUIRED_PASS_LINES = (
    "PASS accept pristine-lowering",
    "PASS reject unknown-function direct#0",
    "PASS reject wrong-type-correct-signature direct#0",
    "PASS accept unrelated-signature direct#0",
)


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_timeout(value: float, what: str = "--timeout") -> float:
    """Finite, strictly positive seconds; rejects NaN, +-Inf, 0 and negatives."""
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{what} is not a number: {value!r} ({error})")
    if not math.isfinite(number):
        raise ValueError(f"{what} must be finite, got {number!r}")
    if number <= 0:
        raise ValueError(f"{what} must be > 0, got {number!r}")
    return number


def timeout_argument(text: str) -> float:
    return validate_timeout(float(text))


def prepare_work_dir(text: str) -> pathlib.Path:
    """New or empty explicit work dir, or a fresh mkdtemp.

    A non-empty directory is refused *before* any write, so its bytes (including
    nested files and symlinks) are untouched.
    """
    if not text:
        return pathlib.Path(tempfile.mkdtemp(prefix="woozy-callidentity-"))
    work = pathlib.Path(text).expanduser().resolve()
    if work.exists():
        if not work.is_dir():
            raise ValueError(f"--work-dir exists and is not a directory: {work}")
        entries = sorted(entry.name for entry in work.iterdir())
        if entries:
            raise ValueError(
                f"--work-dir is not empty ({len(entries)} entries: "
                f"{', '.join(entries[:5])}); refusing before any write")
        return work
    work.mkdir(parents=True)
    return work


def environment(toolchain: pathlib.Path) -> dict[str, str]:
    env = dict(os.environ)
    parts = [
        str(toolchain / "llvm-22.1.8" / "lib"),
        str(toolchain / "libxml2-legacy-2.13.9" / "usr" / "lib"),
    ]
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        parts.append(previous)
    env["LD_LIBRARY_PATH"] = ":".join(parts)
    env["PATH"] = (str(toolchain / "llvm-22.1.8" / "bin") + os.pathsep +
                   env.get("PATH", ""))
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    return env


def save_command(log_dir: pathlib.Path, record: dict) -> None:
    """Persist argv/exit/flags/output and hashes before any judgement."""
    log_dir.mkdir(parents=True, exist_ok=True)
    stdout = record.pop("stdout", b"")
    stderr = record.pop("stderr", b"")
    record["stdout_bytes"] = len(stdout)
    record["stdout_sha256"] = hashlib.sha256(stdout).hexdigest()
    record["stderr_bytes"] = len(stderr)
    record["stderr_sha256"] = hashlib.sha256(stderr).hexdigest()
    (log_dir / f"{record['label']}.stdout.bin").write_bytes(stdout)
    (log_dir / f"{record['label']}.stderr.bin").write_bytes(stderr)
    text = json.dumps(record, indent=2) + "\n"
    (log_dir / f"{record['label']}.command.json").write_text(text)
    record["stdout"] = stdout
    record["stderr"] = stderr


def run(command, label: str, cwd: pathlib.Path, env: dict[str, str],
        timeout: float, log_dir: pathlib.Path) -> dict:
    """Run one command through the project's bounded process-group helper."""
    print("+", " ".join(str(part) for part in command), flush=True)
    record = {
        "label": label,
        "argv": [str(part) for part in command],
        "cwd": str(cwd),
        "timeout": timeout,
    }
    try:
        result = _run_process_impl(command, cwd, env, timeout)
    except (OSError, ValueError) as error:
        record.update({
            "exit": None, "timed_out": False, "cleanup_complete": None,
            "abandoned_pipes": None,
            "exception": repr(error), "exception_type": type(error).__name__,
            "stdout": b"", "stderr": b"",
        })
        save_command(log_dir, record)
        fail(f"{label}: {type(error).__name__}: {error}")
    record.update({
        "exit": result["exit"],
        "timed_out": result["timed_out"],
        "cleanup_complete": result["cleanup_complete"],
        "abandoned_pipes": result["abandoned_pipes"],
        "stdout": result["stdout"], "stderr": result["stderr"],
    })
    save_command(log_dir, record)
    return record


def require_clean(record: dict, expected_exit: int | None = None) -> None:
    """Timeouts, incomplete cleanup, abandoned pipes and signals are failures."""
    label = record["label"]
    if record["timed_out"]:
        fail(f"{label}: timed out after {record['timeout']:g}s")
    if record["cleanup_complete"] is not True:
        fail(f"{label}: cleanup_complete={record['cleanup_complete']}")
    if record["abandoned_pipes"]:
        fail(f"{label}: abandoned stdout/stderr pipes")
    if record["exit"] is None:
        fail(f"{label}: no exit status")
    if record["exit"] < 0:
        fail(f"{label}: terminated by signal {-record['exit']}")
    if expected_exit is not None and record["exit"] != expected_exit:
        fail(f"{label}: exit {record['exit']} != {expected_exit}")


def decoded(record: dict) -> str:
    return (record["stdout"] + record["stderr"]).decode("utf-8", "replace")


def nativeir_block(text: str) -> list[str]:
    lines = text.splitlines()
    try:
        begin = lines.index(BEGIN)
        end = lines.index(END)
    except ValueError:
        fail("driver output has no NATIVEIR markers")
    block = lines[begin + 1:end]
    while block and not block[-1].strip():
        block.pop()
    return block


def stage_project(work: pathlib.Path, source_root: pathlib.Path) -> pathlib.Path:
    project = work / "project"
    if project.exists():
        fail(f"work dir was expected empty but {project} exists")
    shutil.copytree(source_root, project / "src")
    shutil.copy2(HERE / "main.wz", project / "src/main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def reference_dump(reference: pathlib.Path, fixture: pathlib.Path,
                   out_dir: pathlib.Path, work: pathlib.Path,
                   env: dict[str, str], timeout: float) -> dict:
    """Real C++ verifier dump of the same Source, with split statuses.

    `ir_status` is "verified-dump" only when the dump is present, the process
    was not signalled/timed out and cleanup completed.  The later link/build
    status is reported separately and is never treated as the IR result.
    """
    if out_dir.exists():
        fail(f"reference out dir already exists: {out_dir}")
    name = fixture.parent.name
    record = run([str(reference), "build", "--project", str(fixture),
                  "--backend", "native", "--opt", "0",
                  "--out-dir", str(out_dir)],
                 f"{name}-reference-build", work,
                 dict(env) | {"WZ_DUMP_NATIVEIR": "1"}, timeout, work)
    if record["timed_out"] or record["cleanup_complete"] is not True \
            or record["abandoned_pipes"] or (record["exit"] or 0) < 0:
        fail(f"reference {name}: no trustworthy IR dump "
             f"(exit={record['exit']}, timed_out={record['timed_out']}, "
             f"cleanup_complete={record['cleanup_complete']}, "
             f"abandoned_pipes={record['abandoned_pipes']})")
    text = record["stderr"].decode("utf-8", "replace")
    if "native-scalars compiler-dev/1 native64" not in text:
        text = record["stdout"].decode("utf-8", "replace")
    lines = []
    for line in text.splitlines():
        if REFERENCE_TAIL.match(line):
            break
        lines.append(line)
    while lines and not lines[-1].strip():
        lines.pop()
    if not lines or lines[0] != "native-scalars compiler-dev/1 native64":
        fail(f"reference {name}: no NativeIR dump (exit {record['exit']})")
    status = {
        "ir_status": "verified-dump",
        "ir_lines": len(lines),
        "build_exit": record["exit"],
        "link_status": "ok" if record["exit"] == 0 else "failed",
    }
    print(f"reference {name}: {status['ir_status']} ({len(lines)} lines), "
          f"build_exit={status['build_exit']} link_status={status['link_status']}")
    return {"lines": lines, "status": status}


def build_and_run(reference: pathlib.Path, fixture: pathlib.Path, opt: int,
                  work: pathlib.Path, env: dict[str, str], timeout: float,
                  tag: str) -> dict:
    """Build one fixture Source and execute the produced program."""
    name = fixture.parent.name
    out_dir = work / f"{tag}-{name}-opt{opt}"
    if out_dir.exists():
        fail(f"output dir already exists: {out_dir}")
    record = run([str(reference), "build", "--project", str(fixture),
                  "--backend", "native", "--opt", str(opt),
                  "--out-dir", str(out_dir), "--format", "json"],
                 f"{tag}-{name}-build-O{opt}", work, env, timeout, work)
    require_clean(record, 0)
    program = out_dir / "program"
    if not program.is_file():
        fail(f"{tag} {name} O{opt}: missing {program}")
    execution = run([str(program)], f"{tag}-{name}-run-O{opt}", out_dir, env,
                    timeout, work)
    require_clean(execution, 0)
    print(f"PASS {tag} {name} O{opt}: build ok, program exit 0, "
          f"stdout={len(execution['stdout'])}B")
    return {"build": record, "run": execution}


def check_pass(name: str, output: str, reference_lines: list[str] | None,
               expect: str) -> int:
    problems = 0
    lines = output.splitlines()
    failure_lines = [line for line in lines if line.startswith("FAIL")]
    totals = [line for line in lines if line.startswith("failures=")]
    if len(totals) != 1:
        if expect == "pass":
            print(f"FAIL {name}: expected one failures= line, "
                  f"got {len(totals)}")
            problems += 1
        count = -1
    else:
        count = int(totals[0].split("=", 1)[1])
    if expect == "pass":
        if count != 0 or failure_lines:
            print(f"FAIL {name}: driver reported failures ({count})")
            for line in failure_lines:
                print("  " + line)
            problems += 1
        for required in REQUIRED_PASS_LINES:
            if required not in output:
                print(f"FAIL {name}: missing '{required}'")
                problems += 1
    else:
        lowering_rejected = (
            "FAIL lowering did not verify" in output and
            "unknown function identity/signature" in output)
        mutation_failed = (
            "FAIL wrong-type-correct-signature direct#0:" in output and
            "FAIL unrelated-signature direct#0:" in output)
        if lowering_rejected:
            print(f"PASS {name}: pre-fix tree rejects the real lowering with "
                  f"unknown function identity/signature")
        elif mutation_failed:
            print(f"PASS {name}: pre-fix tree fails the discriminating "
                  f"mutations")
        elif count <= 0:
            print(f"FAIL {name}: pre-fix tree did not fail (failures={count})")
            problems += 1
        else:
            print(f"FAIL {name}: pre-fix tree failed, but not on the "
                  f"discriminating cases")
            problems += 1
    if reference_lines is not None:
        got = nativeir_block(output)
        if got != reference_lines:
            print(f"FAIL {name}: Wz NativeIR dump differs from C++ ir::dump")
            for index, (left, right) in enumerate(zip(got, reference_lines)):
                if left != right:
                    print(f"  line {index}: wz={left!r} cpp={right!r}")
                    break
            print(f"  lengths: wz={len(got)} cpp={len(reference_lines)}")
            problems += 1
        else:
            print(f"PASS {name}: NativeIR dump equals C++ ir::dump "
                  f"({len(got)} lines)")
    return problems


def swap_span_problems(name: str, output: str, fixture: pathlib.Path) -> int:
    """The generic fixture's 0/1 targets must be its two real swap instances."""
    source = (fixture.parent / "src/main.wz").read_bytes()
    spans = []
    for call in SWAP_CALLS:
        needle = call.encode()
        if source.count(needle) != 1:
            print(f"FAIL {name}: fixture has {source.count(needle)} "
                  f"occurrences of {call!r}")
            return 1
        start = source.index(needle)
        spans.append((start, start + len(needle)))
    dump = nativeir_block(output)
    found = []
    for line in dump:
        match = FUNCTION_LINE.match(line)
        if not match:
            continue
        type_id, symbol, snapshot, begin, end = (int(g) for g in match.groups())
        if snapshot != 2:
            continue
        found.append({"type": type_id, "symbol": symbol,
                      "span": (begin, end)})
    problems = 0
    selected = []
    for span in spans:
        matches = [entry for entry in found if entry["span"] == span]
        if len(matches) != 1:
            print(f"FAIL {name}: dump has {len(matches)} direct Function "
                  f"instructions at source bytes {span[0]}-{span[1]}")
            problems += 1
            continue
        selected.append(matches[0])
    if problems:
        return problems
    if selected[0]["symbol"] == selected[1]["symbol"] or \
            selected[0]["type"] == selected[1]["type"]:
        print(f"FAIL {name}: the two swap instances share symbol or signature: "
              f"{selected}")
        problems += 1
    if "PASS reject swapped-instance-identity 0/1 " not in output:
        print(f"FAIL {name}: no 'swapped-instance-identity 0/1' rejection")
        problems += 1
    if not problems:
        print(f"PASS {name}: swap[i32] @{selected[0]['symbol']} "
              f"type={selected[0]['type']} and swap[bool] "
              f"@{selected[1]['symbol']} type={selected[1]['type']} are the "
              f"rejected 0/1 pair")
    return problems


def judge_driver(name: str, record: dict, output: str, expect: str,
                 reference_lines: list[str] | None,
                 generic_fixture: pathlib.Path | None) -> int:
    """One driver execution: process status first, then the PASS/fail text."""
    problems = 0
    expected_exit = 0 if expect == "pass" else 1
    if record["timed_out"] or record["cleanup_complete"] is not True \
            or record["abandoned_pipes"] or record["exit"] is None \
            or record["exit"] < 0:
        problems += 1
        print(f"FAIL {name}: untrustworthy process record "
              f"(exit={record['exit']}, timed_out={record['timed_out']}, "
              f"cleanup_complete={record['cleanup_complete']}, "
              f"abandoned_pipes={record['abandoned_pipes']})")
    elif record["exit"] != expected_exit:
        problems += 1
        print(f"FAIL {name}: driver exit {record['exit']} != "
              f"{expected_exit} (a PASS line cannot override the process "
              f"status)")
    problems += check_pass(name, output, reference_lines, expect)
    if expect == "pass" and generic_fixture is not None \
            and record["exit"] == 0 and not record["timed_out"]:
        problems += swap_span_problems(name, output, generic_fixture)
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0))
    parser.add_argument("--reference", default=str(DEFAULT_STAGE0))
    parser.add_argument("--stage1", default="")
    parser.add_argument("--toolchain-root", default=str(DEFAULT_TOOLCHAIN))
    parser.add_argument("--source-root", default=str(ROOT / "selfhost/src"))
    parser.add_argument("--stdlib", default=str(ROOT / "stdlib"))
    parser.add_argument("--work-dir", default="")
    parser.add_argument("--timeout", type=timeout_argument,
                        default=DEFAULT_TIMEOUT)
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2))
    parser.add_argument("--source-opt", action="append", type=int,
                        choices=(0, 2), default=[])
    parser.add_argument("--expect", choices=("pass", "fail"), default="pass")
    parser.add_argument("--fixture", action="append", default=[])
    parser.add_argument("--reuse-program", default="")
    parser.add_argument("--program-label", default="reuse")
    parser.add_argument("--no-reference", action="store_true")
    args = parser.parse_args(argv)
    validate_timeout(args.timeout)
    try:
        work = prepare_work_dir(args.work_dir)
    except ValueError as error:
        fail(str(error))
    stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    reference = pathlib.Path(args.reference).expanduser().resolve()
    toolchain = pathlib.Path(args.toolchain_root).expanduser().resolve()
    source_root = pathlib.Path(args.source_root).expanduser().resolve()
    stdlib = pathlib.Path(args.stdlib).expanduser().resolve()
    reuse = (pathlib.Path(args.reuse_program).expanduser().resolve()
             if args.reuse_program else None)
    stage1 = (pathlib.Path(args.stage1).expanduser().resolve()
              if args.stage1 else None)
    required = [reference, reuse, stage1]
    if reuse is None:
        required.append(stage0)
    for path in required:
        if path is not None and not path.is_file():
            fail(f"missing file: {path}")
    if not (toolchain / "llvm-22.1.8/bin/clang").is_file():
        fail(f"toolchain clang missing under {toolchain}")
    if reuse is None and not (
            source_root / "wzcc/nativeverify/verify_ops.wz").is_file():
        fail(f"source root is not the selfhost tree: {source_root}")
    if reuse is not None and args.opt:
        fail("--opt cannot be combined with --reuse-program: the replayed "
             "program's build level is fixed; label it with --program-label")
    levels = args.opt or ([0] if reuse is not None else [0, 2])
    tag = args.program_label if reuse is not None else "driver"
    source_levels = args.source_opt or [0, 2]
    fixtures = []
    for name in (args.fixture or FIXTURES):
        manifest = HERE / "fixtures" / name / "lang.toml"
        if not manifest.is_file():
            fail(f"fixture missing: {manifest}")
        fixtures.append(manifest)

    print(f"work-dir: {work}")
    print(f"reference: {reference} sha256={sha256(reference)}")
    if reuse is not None:
        print(f"reuse-program: {reuse} sha256={sha256(reuse)}")
    else:
        print(f"stage0: {stage0} sha256={sha256(stage0)}")
        print(f"source-root: {source_root}")
        print(f"verify_ops: sha256="
              f"{sha256(source_root / 'wzcc/nativeverify/verify_ops.wz')}")
    if stage1 is not None:
        print(f"stage1: {stage1} sha256={sha256(stage1)}")
    print(f"timeout: {args.timeout:g}s expect={args.expect}")

    env = environment(toolchain)
    problems = 0
    programs = {}
    if reuse is not None:
        for level in levels:
            programs[level] = reuse
    else:
        project = stage_project(work, source_root)
        manifest = str(project / "lang.toml")
        checked = run([str(stage0), "check", "--project", manifest,
                       "--backend", "native"], "driver-check", work, env,
                      args.timeout, work)
        require_clean(checked, 0)
        for level in levels:
            out_dir = work / f"driver-opt{level}"
            built = run([str(stage0), "build", "--project", manifest,
                         "--backend", "native", "--opt", str(level),
                         "--out-dir", str(out_dir), "--format", "json"],
                        f"driver-build-O{level}", work, env, args.timeout, work)
            require_clean(built, 0)
            program = out_dir / "program"
            if not program.is_file():
                fail(f"O{level}: missing {program}")
            programs[level] = program
            print(f"PASS driver build O{level}: {program} "
                  f"sha256={sha256(program)}")

    reference_lines = {}
    if args.expect == "pass" and not args.no_reference:
        for fixture in fixtures:
            name = fixture.parent.name
            reference_lines[name] = reference_dump(
                reference, fixture, work / f"reference-{name}", work, env,
                args.timeout)["lines"]

    for level in levels:
        program = programs[level]
        label_tag = tag if reuse is not None else f"{tag}O{level}"
        for fixture in fixtures:
            name = fixture.parent.name
            record = run([str(program), str(fixture), str(stdlib)],
                         f"{label_tag}-{name}", work, env, args.timeout, work)
            output = decoded(record)
            print(output, end="")
            problems += judge_driver(f"{label_tag}/{name}", record, output,
                                     args.expect,
                                     reference_lines.get(name)
                                     if args.expect == "pass" else None,
                                     fixture if name == "generic" else None)

    if args.expect == "pass" and not args.no_reference:
        for fixture in fixtures:
            for level in source_levels:
                build_and_run(reference, fixture, level, work, env,
                              args.timeout, "reference")
    if stage1 is not None:
        for fixture in fixtures:
            for level in source_levels:
                build_and_run(stage1, fixture, level, work, env,
                              args.timeout, "stage1")

    if problems:
        print(f"FAIL: {problems} problem(s); work dir {work}")
        return 1
    print(f"PASS: callidentity component regression ({args.expect}); "
          f"work dir {work}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
