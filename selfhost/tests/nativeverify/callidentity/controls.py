#!/usr/bin/env python3
"""Test-runner process controls for `run.py`.

These are **test-script controls only**. They exercise the runner's own process,
work-directory and exit-status policy with small fake commands; they are not
Source, IR, compiler or verifier evidence and must never be counted as such.

Covered:

1. a normal command keeps its stdout/stderr and per-command record;
2. NaN / +-Inf / 0 / negative / non-numeric timeouts are rejected before use;
3. a non-empty `--work-dir` (nested files plus a symlink) is refused before any
   write and every byte stays identical;
4. an OSError still leaves a command record with the exception;
5. a timeout kills the whole process group (the spawned helper is gone);
6. a parent that exits 0 while a child keeps the pipe is rejected
   (cleanup_complete False / abandoned_pipes True);
7. a signal exit (-9) and a nonzero exit (42) are rejected even when the output
   contains PASS lines, while exit 0 with the same output is accepted;
8. the real CLI refuses a NaN timeout and a non-empty work dir before writing,
   and the check→build→run plumbing accepts exit 0 / rejects PASS text + exit 42
   using a fake seed and fake driver (runner plumbing only).
"""

from __future__ import annotations

import hashlib
import importlib.util
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

HERE = pathlib.Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("callidentity_run", HERE / "run.py")
CI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CI)

FAKE_OUTPUT = "\n".join((
    "PASS accept pristine-lowering",
    "PASS reject unknown-function direct#0",
    "PASS reject wrong-type-correct-signature direct#0",
    "PASS accept unrelated-signature direct#0",
    "failures=0",
)) + "\n"

failures = 0


def check(name: str, condition: bool, detail: str = "") -> None:
    global failures
    if condition:
        print(f"PASS control {name}")
    else:
        failures += 1
        print(f"FAIL control {name}{(': ' + detail) if detail else ''}")


def expect_raise(name: str, error_type, function, *args) -> None:
    global failures
    try:
        function(*args)
    except error_type:
        print(f"PASS control {name}")
        return
    failures += 1
    print(f"FAIL control {name}: no {error_type.__name__}")


def tree_bytes(path: pathlib.Path) -> dict[str, str]:
    record = {}
    for entry in sorted(path.rglob("*")):
        key = str(entry.relative_to(path))
        if entry.is_symlink():
            record[key] = "symlink:" + os.readlink(entry)
        elif entry.is_file():
            record[key] = "file:" + hashlib.sha256(entry.read_bytes()).hexdigest()
        else:
            record[key] = "dir"
    return record


def normal_command(work: pathlib.Path) -> None:
    record = CI.run(["/bin/sh", "-c", "printf hello; printf boom >&2; exit 0"],
                    "control-normal", work, dict(os.environ), 30.0, work)
    CI.require_clean(record, 0)
    check("normal-output-preserved",
          record["stdout"] == b"hello" and record["stderr"] == b"boom",
          f"{record['stdout']!r} {record['stderr']!r}")
    saved = work / "control-normal.command.json"
    check("normal-record-saved",
          saved.is_file() and '"stdout_sha256"' in saved.read_text())


def oserror_record(work: pathlib.Path) -> None:
    expect_raise("oserror-judged-failure", SystemExit, CI.run,
                 ["/nonexistent/callidentity-control"], "control-oserror",
                 work, dict(os.environ), 10.0, work)
    saved = work / "control-oserror.command.json"
    check("oserror-record-saved", saved.is_file())
    if saved.is_file():
        text = saved.read_text()
        check("oserror-record-has-exception",
              '"exception_type": "FileNotFoundError"' in text or
              '"exception_type": "OSError"' in text, text[:200])


def invalid_timeouts() -> None:
    for value in (float("nan"), float("inf"), float("-inf"), 0.0, -1.0):
        expect_raise(f"timeout-rejected-{value}", ValueError,
                     CI.validate_timeout, value)
    expect_raise("timeout-rejected-text", ValueError,
                 CI.validate_timeout, "abc")
    check("timeout-accepted-positive", CI.validate_timeout(0.5) == 0.5)


def work_dir_refusal() -> None:
    root = pathlib.Path(tempfile.mkdtemp(prefix="control-workdir-"))
    (root / "nested").mkdir()
    (root / "nested/deep.txt").write_text("keep me\n")
    os.symlink("../nested/deep.txt", root / "nested/link.txt")
    before = tree_bytes(root)
    expect_raise("work-dir-refused", ValueError,
                 CI.prepare_work_dir, str(root))
    after = tree_bytes(root)
    check("work-dir-bytes-unchanged", before == after,
          f"{before} != {after}")
    check("work-dir-no-new-entries", before.keys() == after.keys())
    empty = pathlib.Path(tempfile.mkdtemp(prefix="control-empty-"))
    check("work-dir-empty-accepted",
          CI.prepare_work_dir(str(empty)) == empty.resolve())
    fresh = root / "does-not-exist"
    check("work-dir-fresh-created", CI.prepare_work_dir(str(fresh)) == fresh)


def timeout_kills_group(work: pathlib.Path) -> None:
    pid_file = work / "timeout-helper.pid"
    script = (f"sleep 60 & echo $! > {pid_file}; wait")
    record = CI.run(["/bin/sh", "-c", script], "control-timeout", work,
                    dict(os.environ), 0.8, work)
    check("timeout-flag", record["timed_out"] is True, str(record["timed_out"]))
    helper = int(pid_file.read_text().strip()) if pid_file.is_file() else 0
    gone = False
    for _ in range(50):
        if helper and not pathlib.Path(f"/proc/{helper}").exists():
            gone = True
            break
        time.sleep(0.1)
    check("timeout-kills-helper-group", gone, f"helper pid {helper} alive")
    expect_raise("timeout-judged-failure", SystemExit,
                 CI.require_clean, record, 0)


def parent_exit_child_pipe(work: pathlib.Path) -> None:
    pid_file = work / "pipe-helper.pid"
    script = f"sleep 20 & echo $! > {pid_file}; exit 0"
    record = CI.run(["/bin/sh", "-c", script], "control-abandoned-pipe", work,
                    dict(os.environ), 30.0, work)
    abandoned = (record["cleanup_complete"] is not True or
                 bool(record["abandoned_pipes"]))
    check("parent-exit0-child-pipe-rejected", abandoned,
          f"cleanup_complete={record['cleanup_complete']} "
          f"abandoned_pipes={record['abandoned_pipes']}")
    expect_raise("abandoned-pipe-judged-failure", SystemExit,
                 CI.require_clean, record, 0)
    if pid_file.is_file():
        pid = int(pid_file.read_text().strip())
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        print(f"note: cleaned up control child pid {pid}")


def exit_status_policy(work: pathlib.Path) -> None:
    def fake(exit_code: int) -> dict:
        return {"label": "control-driver", "argv": ["fake"],
                "cwd": str(work), "timeout": 30.0, "exit": exit_code,
                "timed_out": False, "cleanup_complete": True,
                "abandoned_pipes": False,
                "stdout": FAKE_OUTPUT.encode(), "stderr": b""}

    accepted = CI.judge_driver("driver-exit0", fake(0), FAKE_OUTPUT, "pass",
                               None, None)
    check("pass-text-exit0-accepted", accepted == 0, str(accepted))
    flagged = CI.judge_driver("driver-exit42", fake(42), FAKE_OUTPUT, "pass",
                              None, None)
    check("pass-text-exit42-rejected", flagged > 0, str(flagged))
    signalled = CI.judge_driver("driver-exit-9", fake(-9), FAKE_OUTPUT,
                                "pass", None, None)
    check("pass-text-signal-rejected", signalled > 0, str(signalled))
    timed = fake(0)
    timed["timed_out"] = True
    check("pass-text-timeout-rejected",
          CI.judge_driver("driver-timeout", timed, FAKE_OUTPUT, "pass",
                          None, None) > 0)
    prefix_text = ("FAIL lowering did not verify\n"
                   "E_IR_INVALID lowering error: "
                   "unknown function identity/signature\n")
    prefix_record = fake(1)
    prefix_record["stderr"] = prefix_text.encode()
    check("prefix-exit1-accepted",
          CI.judge_driver("prefix-exit1", prefix_record, prefix_text,
                          "fail", None, None) == 0)
    prefix_zero = fake(0)
    prefix_zero["stderr"] = prefix_text.encode()
    check("prefix-exit0-rejected",
          CI.judge_driver("prefix-exit0", prefix_zero, prefix_text,
                          "fail", None, None) > 0)
    prefix_signal = fake(-9)
    prefix_signal["stderr"] = prefix_text.encode()
    check("prefix-signal-rejected",
          CI.judge_driver("prefix-signal", prefix_signal,
                          prefix_text, "fail", None, None) > 0)




PLUMBING_OUTPUT = "\n".join((
    "PASS accept pristine-lowering",
    "PASS reject unknown-function direct#0",
    "PASS reject wrong-type-correct-signature direct#0",
    "PASS accept unrelated-signature direct#0",
    "failures=0",
)) + "\n"


def plumbing_tree(work: pathlib.Path, name: str, exit_code: int):
    """Fake seed + fake driver for runner plumbing only (no compiler involved)."""
    root = work / name
    (root / "src/wzcc/nativeverify").mkdir(parents=True)
    (root / "src/wzcc/nativeverify/verify_ops.wz").write_text("// fake\n")
    program = root / "fake-program"
    program.write_text("#!/bin/sh\ncat <<'OUT'\n" + PLUMBING_OUTPUT +
                       "OUT\nexit " + str(exit_code) + "\n")
    program.chmod(0o755)
    stage0 = root / "fake-woozyc"
    stage0.write_text(
        "#!/bin/sh\n"
        'case "$1" in\n'
        "  check) exit 0 ;;\n"
        "  build)\n"
        '    out=""\n'
        '    prev=""\n'
        '    for a in "$@"; do\n'
        '      if [ "$prev" = "--out-dir" ]; then out="$a"; fi\n'
        '      prev="$a"\n'
        "    done\n"
        '    mkdir -p "$out" || exit 3\n'
        '    cp ' + str(program) + ' "$out/program" || exit 3\n'
        '    printf \'{"status":"ok"}\'\n'
        "    exit 0 ;;\n"
        "esac\n"
        "exit 2\n")
    stage0.chmod(0o755)
    return root, stage0


def build_path_plumbing(work: pathlib.Path) -> None:
    toolchain = pathlib.Path("/home/qin/Work/language-dev-20261103/toolchains")
    accepted_root, accepted_stage0 = plumbing_tree(work, "plumbing-ok", 0)
    code = CI.main(["--woozyc", str(accepted_stage0),
                    "--source-root", str(accepted_root / "src"),
                    "--work-dir", str(accepted_root / "w"),
                    "--fixture", "enumclosure", "--no-reference",
                    "--reference", str(accepted_stage0),
                    "--toolchain-root", str(toolchain), "--timeout", "60"])
    check("plumbing-check-build-run-accepted", code == 0, f"exit {code}")
    failing_root, failing_stage0 = plumbing_tree(work, "plumbing-exit42", 42)
    code = CI.main(["--woozyc", str(failing_stage0),
                    "--source-root", str(failing_root / "src"),
                    "--work-dir", str(failing_root / "w"),
                    "--fixture", "enumclosure", "--no-reference",
                    "--reference", str(failing_stage0),
                    "--toolchain-root", str(toolchain), "--timeout", "60"])
    check("plumbing-pass-text-exit42-rejected", code != 0, f"exit {code}")


def cli_refusals(work: pathlib.Path) -> None:
    """The real CLI must refuse before any write."""
    run_py = str(HERE / "run.py")
    fresh = work / "cli-timeout-target"
    result = subprocess.run([sys.executable, run_py, "--timeout", "nan",
                             "--work-dir", str(fresh)],
                            capture_output=True, text=True, timeout=120)
    check("cli-nan-timeout-rejected",
          result.returncode != 0 and not fresh.exists(),
          f"exit {result.returncode} exists {fresh.exists()}")
    occupied = work / "cli-occupied"
    (occupied / "nested").mkdir(parents=True)
    (occupied / "nested/file.txt").write_text("bytes stay\n")
    os.symlink("file.txt", occupied / "nested/link")
    before = tree_bytes(occupied)
    result = subprocess.run([sys.executable, run_py, "--work-dir",
                             str(occupied)],
                            capture_output=True, text=True, timeout=120)
    check("cli-nonempty-work-dir-rejected",
          result.returncode != 0, f"exit {result.returncode}")
    check("cli-nonempty-work-dir-bytes-unchanged",
          tree_bytes(occupied) == before)


def main() -> int:
    work = pathlib.Path(tempfile.mkdtemp(prefix="callidentity-controls-"))
    print(f"controls work dir: {work}")
    normal_command(work)
    oserror_record(work)
    invalid_timeouts()
    work_dir_refusal()
    exit_status_policy(work)
    timeout_kills_group(work)
    parent_exit_child_pipe(work)
    build_path_plumbing(work)
    cli_refusals(work)
    print(f"controls failures={failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
