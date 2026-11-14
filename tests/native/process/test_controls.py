#!/usr/bin/env python3
"""Pure controls for the process public-record runner.

These checks do not build Source, execute a program, start a subprocess or
touch a compiler.  They exercise the runner's pure decisions: exact integer
exit codes, strict process flags, panic marker, raw UTF-8 bytes, complete JSON,
trace success/failure parsing, legacy repeatability vs strict record refusal,
and the before/after resource snapshot keys.
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "sh15_process_runner", HERE / "run_process.py")
RUNNER = importlib.util.module_from_spec(spec)
spec.loader.exec_module(RUNNER)

CHECKS = []


def check(name, condition, detail=""):
    CHECKS.append({"name": name, "status": "passed" if condition else "failed",
                   "detail": "" if condition else str(detail)})


def process_result(exit_code, stdout=b"", stderr=b""):
    return {"exit": exit_code, "timed_out": False,
            "cleanup_complete": True, "abandoned_pipes": False,
            "stdout": stdout, "stderr": stderr}


class FakeCaller:
    """Reuse the same in-memory caller for command and main controls."""

    def __init__(self, result):
        self._result = result

    def _run_process_impl(self, argv, cwd, env, timeout):
        return self._result(argv, env)


def route_fake(argv, env):
    parts = [str(part) for part in argv]
    if "python" in parts:
        document = {"status": "error", "diagnostics": [
            {"code": "E_CAPABILITY_UNAVAILABLE"}]}
        return process_result(
            1, json.dumps(document).encode("utf-8"))
    if any(part.endswith("/misuse-o0/program") or
           part.endswith("/misuse-o2/program") for part in parts):
        return process_result(
            101, b"", (RUNNER.PANIC + "0:0:0\n").encode("utf-8"))
    if env and "WOOZY_INVALID_UTF8" in env:
        return process_result(90)
    if any(part == RUNNER.INVALID_UTF8 for part in parts):
        return process_result(90)
    return process_result(0)


def run_main_in_process(argv, caller):
    stdout = io.StringIO()
    stderr = io.StringIO()
    old_argv = sys.argv
    old_loader = RUNNER.load_corpus
    RUNNER.load_corpus = lambda root: caller
    sys.argv = [str(HERE / "run_process.py")] + [str(part) for part in argv]
    try:
        with contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(stderr):
            code = RUNNER.main()
    finally:
        sys.argv = old_argv
        RUNNER.load_corpus = old_loader
    return code, stdout.getvalue(), stderr.getvalue()


def fixture_root(base):
    root = base / "repo"
    (root / "selfhost/src").mkdir(parents=True)
    (root / "selfhost/src/main.wz").write_text(
        "// fixture source\n", encoding="utf-8")
    (root / "selfhost/corpus.py").write_text(
        "# fixture corpus\n", encoding="utf-8")
    cli = base / "bin/woozyc"
    cli.parent.mkdir(parents=True)
    cli.write_bytes(b"fixture-woozyc")
    (cli.parent / "stdlib").mkdir()
    (cli.parent / "stdlib/fixture.wz").write_text(
        "// fixture stdlib\n", encoding="utf-8")
    llvm_bin = base / "llvm/bin"
    llvm_bin.mkdir(parents=True)
    (llvm_bin / "llvm-readelf").write_bytes(b"fixture-llvm-readelf")
    return root, cli, llvm_bin


def check_command_bool(exit_code, expected):
    with tempfile.TemporaryDirectory() as temporary:
        record = Path(temporary) / "record"
        ctx = {
            "corpus": FakeCaller(
                lambda argv, env, code=exit_code: process_result(code)),
            "record": record,
            "commands": [],
            "strace": None,
            "cwd": Path(temporary),
            "role": "candidate",
        }
        try:
            RUNNER.command(["/never-executed"], label="bool-exit",
                           phase="build", ctx=ctx, expected=expected)
            rejected = False
        except RUNNER.EvidenceError:
            rejected = True
        command = json.loads(
            (record / "commands/00-bool-exit/command.json").read_text(
                encoding="utf-8"))
        return rejected, command


def main():
    strict = RUNNER.process_problems(1, False, True, False, None, 1)
    check("strict:expected rejection accepted", strict == [], strict)
    check("strict:timeout is not a rejection",
          RUNNER.process_problems(1, True, True, False, None, 1),
          "timeout accepted")
    check("strict:signal is not a rejection",
          RUNNER.process_problems(-9, False, True, False, None, 1),
          "signal accepted")
    check("strict:spawn failure is not a rejection",
          RUNNER.process_problems(None, None, None, None,
                                  "OSError: missing", 1),
          "spawn failure accepted")
    check("strict:incomplete cleanup is not a rejection",
          RUNNER.process_problems(1, False, False, True, None, 1),
          "cleanup accepted")
    check("strict:wrong exit is not a rejection",
          RUNNER.process_problems(0, False, True, False, None, 1),
          "wrong exit accepted")

    for native_exit in (0, 1, 90, 101):
        check(f"exit:int {native_exit} still accepted",
              RUNNER.process_problems(
                  native_exit, False, True, False, None, native_exit) == [],
              native_exit)

    rejected, command = check_command_bool(False, 0)
    check("exit:bool False expected 0 rejected by command",
          rejected and command["exit"] is False and
          command["exit_is_int"] is False and
          "exit is not an integer" in " ".join(command["problems"]),
          command)
    rejected, command = check_command_bool(True, 1)
    check("exit:bool True expected 1 rejected by command",
          rejected and command["exit"] is True and
          command["exit_is_int"] is False and
          "exit is not an integer" in " ".join(command["problems"]),
          command)

    check("panic:marker required",
          RUNNER.panic_problems(RUNNER.PANIC + "0:0:0") == [])
    check("panic:empty stderr rejected", RUNNER.panic_problems("") != [])

    raw = RUNNER.INVALID_UTF8
    check("utf8:raw byte is 0xff", RUNNER.argv_hex([raw]) == ["ff"],
          RUNNER.argv_hex([raw]))
    check("utf8:display is reversible",
          RUNNER.argv_display([raw]) == ["\\xff"],
          RUNNER.argv_display([raw]))
    check("utf8:no replacement character",
          "efbfbd" not in RUNNER.argv_hex([raw])[0])
    check("utf8:emoji bytes preserved",
          RUNNER.argv_hex(["probe😀"]) == ["70726f6265f09f9880"],
          RUNNER.argv_hex(["probe😀"]))

    try:
        json.loads('{"x": NaN}', parse_constant=RUNNER._reject_json_constant)
        check("json:NaN rejected", False, "NaN accepted")
    except ValueError:
        check("json:NaN rejected", True)
    try:
        json.loads('{"x": Infinity}',
                   parse_constant=RUNNER._reject_json_constant)
        check("json:Infinity rejected", False, "Infinity accepted")
    except ValueError:
        check("json:Infinity rejected", True)
    document = {"status": "error", "diagnostics": [
        {"code": "E_CAPABILITY_UNAVAILABLE"}]}
    check("json:complete error accepted",
          RUNNER.python_rejection(document) == [])
    check("json:wrong status rejected",
          RUNNER.python_rejection({"status": "ok", "diagnostics": []}) != [])
    check("json:missing code rejected",
          RUNNER.python_rejection({"status": "error", "diagnostics": []}) != [])

    with tempfile.TemporaryDirectory() as temporary:
        trace = Path(temporary) / "exec.log"
        trace.write_text(
            '10 execve("/prog", ["/prog", "probe\\360\\237\\230\\200"], '
            '0x1 /* 2 vars */) = 0\n'
            '11 execve("/bin/sh", ["sh", "-c", "exit 7"], '
            '0x2 /* 1 var */) = 0\n'
            '11 execve("/definitely/missing/woozy", ["missing"], '
            '0x3 /* 0 vars */) = -1 ENOENT (No such file or directory)\n'
            '11 execveat(3, "/bin/sh", ["sh"], 0x4 /* 0 vars */, 0) '
            '= -1 ENOENT (No such file or directory)\n',
            encoding="utf-8")
        records = RUNNER.parse_exec_trace(trace)
        check("trace:four records", len(records) == 4, len(records))
        check("trace:success result", records[0]["success"] is True)
        check("trace:emoji argv bytes",
              records[0]["argv_hex"][1] == "70726f6265f09f9880",
              records[0]["argv_hex"])
        check("trace:failed result", records[2]["success"] is False)
        check("trace:failed errno", records[2]["errno"] == "ENOENT",
              records[2]["errno"])
        shell_resolved = str(Path("/bin/sh").resolve())
        check("trace:execveat parsed",
              records[3]["call"] == "execveat" and
              records[3]["path_abs"] == shell_resolved, records[3])
        check("trace:pid parsed", records[1]["pid"] == 11, records[1]["pid"])

    program = "/tmp/program"
    shell = str(Path("/bin/sh").resolve())
    initial = {"path_abs": program, "argv_hex": ["2f746d702f70726f6772616d"],
               "success": True, "pid": 1}
    shell_exec = {"path_abs": shell, "success": True, "pid": 2}
    missing = {"path_abs": RUNNER.MISSING, "success": False,
               "errno": "ENOENT", "pid": 3}
    observation, problems = RUNNER.analyze_program(
        [initial, shell_exec, shell_exec] + [missing] * 3,
        program, [program], require_spawn=True)
    check("program:two shells and three ENOENT accepted",
          problems == [], problems)
    check("program:shell count recorded",
          observation["shell"]["count"] == 2)
    _, problems = RUNNER.analyze_program(
        [initial, shell_exec] + [missing] * 3,
        program, [program], require_spawn=True)
    check("program:one shell rejected", problems != [])
    _, problems = RUNNER.analyze_program(
        [initial, shell_exec, shell_exec] + [missing] * 2,
        program, [program], require_spawn=True)
    check("program:two missing rejected", problems != [])
    raw_initial = {"path_abs": program,
                   "argv_hex": ["2f70726f6772616d", "ff"],
                   "success": True, "pid": 1}
    _, problems = RUNNER.analyze_program(
        [raw_initial], program, ["/program", raw], require_spawn=False)
    check("program:raw 0xff initial argv accepted", problems == [], problems)
    replacement = {"path_abs": program,
                   "argv_hex": ["2f70726f6772616d", "efbfbd"],
                   "success": True, "pid": 1}
    _, problems = RUNNER.analyze_program(
        [replacement], program, ["/program", raw], require_spawn=False)
    check("program:U+FFFD initial argv rejected", problems != [])

    # Original strict non-empty refusal, now without a spawned Python.
    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        root, cli, llvm_bin = fixture_root(base)
        nonempty = base / "out"
        nonempty.mkdir()
        (nonempty / "keep.txt").write_text("keep\n", encoding="utf-8")
        process = run_main_in_process(
            ["--cli", cli, "--root", root, "--out", nonempty,
             "--record", base / "strict-record"],
            FakeCaller(route_fake))
        code, stdout, _ = process
        check("out:non-empty refused", code == 2, code)
        check("out:refusal names directory",
              "not empty" in stdout, stdout)
        check("out:existing file preserved",
              (nonempty / "keep.txt").read_text(encoding="utf-8") == "keep\n")

    # F2: strict --record refuses a non-empty record directory.
    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        root, cli, llvm_bin = fixture_root(base)
        empty_out = base / "out"
        empty_out.mkdir()
        record = base / "record"
        record.mkdir()
        (record / "keep.txt").write_text("keep\n", encoding="utf-8")
        code, stdout, _ = run_main_in_process(
            ["--cli", cli, "--root", root, "--out", empty_out,
             "--record", record],
            FakeCaller(route_fake))
        check("record:non-empty refused", code == 2, code)
        check("record:existing file preserved",
              (record / "keep.txt").read_text(encoding="utf-8") == "keep\n")

    # F2/F3: actual main twice with the same legacy argv, fake caller only.
    with tempfile.TemporaryDirectory() as temporary:
        base = Path(temporary)
        root, cli, llvm_bin = fixture_root(base)
        out = base / "out"
        argv = ["--cli", cli, "--root", root, "--out", out,
                "--llvm-bin", llvm_bin]
        code, stdout, stderr = run_main_in_process(
            argv, FakeCaller(route_fake))
        check("legacy:first run on empty out succeeds", code == 0,
              (code, stdout, stderr))

        before = json.loads(
            (out / "inputs.before.json").read_text(encoding="utf-8"))
        after = json.loads(
            (out / "inputs.after.json").read_text(encoding="utf-8"))
        files = before["files"]
        corpus = root / "selfhost/corpus.py"
        expected_readelf = Path(
            shutil.which("readelf") or "/usr/bin/readelf").resolve()
        check("snapshot:corpus resource key",
              files.get("corpus", {}).get("sha256") ==
              RUNNER.digest(corpus), files.get("corpus"))
        check("snapshot:GNU readelf resource key",
              files.get("gnu-readelf", {}).get("path") ==
              str(expected_readelf) and
              files["gnu-readelf"].get("sha256") ==
              RUNNER.digest(expected_readelf), files.get("gnu-readelf"))
        check("snapshot:LLVM readelf kept separate",
              "llvm-llvm-readelf" in files and
              files["llvm-llvm-readelf"]["path"] !=
              files["gnu-readelf"]["path"], files.get("llvm-llvm-readelf"))
        check("snapshot:before after same corpus and GNU readelf",
              before["snapshot_sha256"] == after["snapshot_sha256"] and
              after["files"]["corpus"] == files["corpus"] and
              after["files"]["gnu-readelf"] == files["gnu-readelf"])

        first_raw = out / "commands/00-build-o0/stdout.bin"
        check("legacy:first run wrote a raw record", first_raw.is_file())
        first_raw.write_bytes(b"old-raw-bytes\n")
        old_program = out / "process-o0/program"
        old_program.parent.mkdir(parents=True, exist_ok=True)
        old_program.write_bytes(b"old-program-bytes\n")
        keep = out / "keep.txt"
        keep.write_text("keep\n", encoding="utf-8")

        code, stdout, stderr = run_main_in_process(
            argv, FakeCaller(route_fake))
        check("legacy:second run same argv succeeds", code == 0,
              (code, stdout, stderr))
        check("legacy:old raw bytes preserved",
              first_raw.read_bytes() == b"old-raw-bytes\n")
        check("legacy:old program bytes preserved",
              old_program.read_bytes() == b"old-program-bytes\n")
        check("legacy:unrelated file preserved",
              keep.read_text(encoding="utf-8") == "keep\n")
        check("legacy:private run dir allocated",
              (out / "rerun-1").is_dir())
        check("legacy:top-level result kept",
              (out / "process-source-results.json").is_file() and
              not (out / "rerun-1/process-source-results.json").exists())

        collision = out / "rerun-2"
        collision.write_bytes(b"keep-collision\n")
        code, stdout, stderr = run_main_in_process(
            argv, FakeCaller(route_fake))
        check("legacy:existing rerun filename skipped", code == 0,
              (code, stdout, stderr))
        check("legacy:rerun collision bytes preserved",
              collision.read_bytes() == b"keep-collision\n")
        check("legacy:next unused rerun name selected",
              (out / "rerun-3").is_dir())

    failed = [item for item in CHECKS if item["status"] == "failed"]
    result = {"status": "passed" if not failed else "failed",
              "checks": CHECKS, "passed": len(CHECKS) - len(failed),
              "failed": failed}
    print(json.dumps(result, ensure_ascii=True, indent=2))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
