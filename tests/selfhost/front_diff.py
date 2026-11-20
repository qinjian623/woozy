#!/usr/bin/env python3
"""SH04 differential test: C++ stage-0 frontend vs the Wz frontend.

The C++ oracle is always built and run first.  The Wz driver is built with the
stage-0 compiler only when the real app/wzcc/source, lexer and parser modules
exist.  The Wz driver is a test-only program; it is never linked into or called
by the C++ oracle, and the oracle is never executed by Wz.

Every external command starts in its own process group and is killed as a
group on timeout.  Runtime executions of the Wz driver receive a PATH that
contains only failing trap wrappers, so an accidental exec of the old
frontend or a bare toolchain command is reported instead of passing silently.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
ORACLE_SOURCE = HERE / "stage0_oracle.cpp"
DRIVER_SOURCE = HERE / "front_driver.wz"
SYNTAX_CASES = ROOT / "tests/syntax/cases.json"
SELFHOST_SOURCE_BASELINE = HERE / "frontend_sources.txt"
OPERATION_ID = "woozy-SH04-front-diff-one-correction-20261113"
PROTOCOL = "woozy-selfhost-front/1"
MODULE_NAMES = ("model", "source", "lexer", "parser")
EXPECTED_SYNTAX_CASES = 80
EXPECTED_EDGE_CASES = 28
DEFAULT_JOBS = 4
# Keep the original per-oracle command budget.  The restored inventory is run in
# bounded parallel batches; the CTest command budget remains 1200 s and Wz
# program runs remain 60 s.
DEFAULT_ORACLE_TIMEOUT = 60.0
TRAP_NAMES = (
    "woozyc",
    "lang_native_helper",
    "woozyc-helper",
    "langc",
    "clang",
    "clang++",
    "ld.lld",
    "cc",
    "c++",
    "gcc",
    "as",
    "ld",
    "stage0_oracle",
    "front_diff.py",
)


class CheckFailure(Exception):
    pass


def relative(path: Path) -> str:
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


def parse_env(values: Iterable[str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in values:
        if "=" not in item:
            raise CheckFailure(f"--env expects NAME=VALUE, got {item!r}")
        name, value = item.split("=", 1)
        if not name:
            raise CheckFailure(f"empty environment name in {item!r}")
        result[name] = value
    return result


def run_process(
    command: list[str],
    *,
    env: dict[str, str],
    timeout: float,
    cwd: Path | None = None,
) -> dict[str, Any]:
    """Run one command in a new session/process group with a hard timeout."""
    try:
        process = subprocess.Popen(
            command,
            cwd=str(cwd) if cwd else None,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
    except OSError as error:
        return {
            "command": command,
            "returncode": 127,
            "stdout": b"",
            "stderr": str(error).encode("utf-8", "replace"),
            "timed_out": False,
            "signalled": False,
        }
    try:
        stdout, stderr = process.communicate(timeout=timeout)
        timed_out = False
        signalled = process.returncode is not None and process.returncode < 0
    except subprocess.TimeoutExpired:
        timed_out = True
        signalled = True
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
    return {
        "command": command,
        "returncode": process.returncode,
        "stdout": stdout,
        "stderr": stderr,
        "timed_out": timed_out,
        "signalled": signalled,
    }


def display_command(command: list[str]) -> str:
    return " ".join(repr(item) if " " in item else item for item in command)


def output_text(run: dict[str, Any]) -> str:
    return run["stderr"].decode("utf-8", "replace").strip()


def build_oracle(args: argparse.Namespace, env: dict[str, str]) -> tuple[Path | None, dict[str, Any] | None]:
    binary = args.work_dir / "bin/stage0_oracle"
    binary.parent.mkdir(parents=True, exist_ok=True)
    command = [
        args.cxx,
        "-std=c++20",
        "-O2",
        "-I",
        str(ROOT / "include"),
        str(ORACLE_SOURCE),
        str(ROOT / "src/support/source.cpp"),
        str(ROOT / "src/syntax/lexer.cpp"),
        str(ROOT / "src/syntax/parser.cpp"),
        "-o",
        str(binary),
    ]
    run = run_process(command, env=env, timeout=args.build_timeout)
    (args.work_dir / "logs/oracle-build.stdout").write_bytes(run["stdout"])
    (args.work_dir / "logs/oracle-build.stderr").write_bytes(run["stderr"])
    if run["returncode"] != 0 or run["timed_out"]:
        return None, {
            "status": "failed",
            "command": display_command(command),
            "returncode": run["returncode"],
            "timed_out": run["timed_out"],
            "output": output_text(run),
        }
    return binary, None


def safe_fixture_bytes(path: Path, expected: bytes) -> bytes:
    relative_path = Path(path)
    actual_path = (ROOT / relative_path).resolve()
    if relative_path.is_absolute() or not actual_path.is_relative_to(ROOT.resolve()):
        raise CheckFailure(f"syntax fixture path leaves the repository: {relative_path}")
    actual = actual_path.read_bytes()
    if actual != expected:
        raise CheckFailure(f"syntax fixture differs from inventory: {relative_path}")
    return actual


def load_cases(case_dir: Path) -> list[dict[str, Any]]:
    inventory = json.loads(SYNTAX_CASES.read_text(encoding="utf-8"))
    if inventory.get("schema") != "compiler-dev/syntax-cases/1":
        raise CheckFailure("unexpected tests/syntax/cases.json schema")
    syntax_cases = inventory.get("in_memory_cases", []) + inventory.get("file_cases", [])
    if len(syntax_cases) != EXPECTED_SYNTAX_CASES:
        raise CheckFailure(
            f"expected {EXPECTED_SYNTAX_CASES} syntax cases, found {len(syntax_cases)}"
        )

    cases: list[dict[str, Any]] = []
    syntax_dir = case_dir / "syntax"
    syntax_dir.mkdir(parents=True, exist_ok=True)
    for index, case in enumerate(syntax_cases):
        expected = case["source"].encode("utf-8")
        if "path" in case:
            payload = safe_fixture_bytes(Path(case["path"]), expected)
            path = (ROOT / case["path"]).resolve()
        else:
            payload = expected
            path = (syntax_dir / f"{index:02d}.wz").resolve()
            path.write_bytes(payload)
        cases.append(
            {
                "group": "syntax",
                "name": f"syntax/{case['name']}",
                "path": path,
                "bytes": payload,
                "sha256": hashlib.sha256(payload).hexdigest(),
                "expected_syntax": case["expected_syntax"],
            }
        )

    edge_cases = edge_case_definitions()
    if len(edge_cases) != EXPECTED_EDGE_CASES:
        raise CheckFailure(
            f"expected {EXPECTED_EDGE_CASES} edge cases, found {len(edge_cases)}"
        )
    edge_dir = case_dir / "edge"
    edge_dir.mkdir(parents=True, exist_ok=True)
    for index, (name, payload) in enumerate(edge_cases):
        path = (edge_dir / f"{index:02d}.wz").resolve()
        path.write_bytes(payload)
        cases.append(
            {
                "group": "edge",
                "name": f"edge/{name}",
                "path": path,
                "bytes": payload,
                "sha256": hashlib.sha256(payload).hexdigest(),
                "expected_syntax": None,
            }
        )

    for name, path in selfhost_source_paths():
        payload = path.read_bytes()
        cases.append(
            {
                "group": "selfhost",
                "name": name,
                "path": path.resolve(),
                "bytes": payload,
                "sha256": hashlib.sha256(payload).hexdigest(),
                "expected_syntax": None,
            }
        )
    payload = DRIVER_SOURCE.read_bytes()
    cases.append(
        {
            "group": "selfhost",
            "name": "selfhost/tests/selfhost/front_driver.wz",
            "path": DRIVER_SOURCE.resolve(),
            "bytes": payload,
            "sha256": hashlib.sha256(payload).hexdigest(),
            "expected_syntax": None,
        }
    )
    return cases


def selfhost_source_paths() -> list[tuple[str, Path]]:
    """Read every live compiler/test source; stale copies cannot substitute."""
    root = ROOT / "selfhost"
    # Additional sources enter the test automatically. Removing or renaming
    # an existing source requires a corresponding baseline update.
    baseline = SELFHOST_SOURCE_BASELINE.read_text(encoding="utf-8").splitlines()
    if not baseline or baseline != sorted(set(baseline)):
        raise CheckFailure("selfhost source baseline must be sorted and unique")
    sources = {
        path.relative_to(ROOT).as_posix(): path
        for path in root.rglob("*.wz") if path.is_file()
    }
    missing = sorted(set(baseline) - sources.keys())
    if missing:
        raise CheckFailure(
            f"missing {len(missing)} baseline selfhost sources: {', '.join(missing)}"
        )
    return sorted(sources.items())


def edge_case_definitions() -> list[tuple[str, bytes]]:
    """Support, lexer and parser edges that exercise the published contract."""
    bom = b"\xef\xbb\xbf"
    return [
        ("empty", b""),
        ("bom-only", bom),
        (
            "bom-crlf-utf16",
            bom
            + "// 中文😀\r\npackage p;\r\nfunc main() {}\r\n".encode("utf-8"),
        ),
        (
            "crlf-whitespace-comment",
            b"package p;\r\n\r\nfunc f() {\r\n\tlet x = 1;\r\n}\r\n",
        ),
        (
            "utf16-surrogate-boundaries",
            "package p;\r\n// 😀 𝄞 𐐀\r\nfunc f() {}\r\n".encode("utf-8"),
        ),
        ("raw-nul", b"package p;\x00"),
        ("diagnostic-lex-only-not-recovered", b"package p; /* open"),
        (
            "diagnostic-prefix-and-parser-recovery",
            b"package p; func f() { let x = @; let y = 1 }",
        ),
        ("standalone-cr", b"package p;\rfunc f() {}"),
        ("invalid-utf8", b"package p;\n\xff"),
        ("truncated-utf8", b"package p;\n\xe2\x82"),
        ("overlong-utf8", b"package p;\n\xc0\xaf"),
        ("surrogate-utf8", b"package p;\n\xed\xa0\x80"),
        ("max-codepoint", b"package p;\n// \xf4\x8f\xbf\xbf\nfunc f() {}"),
        (
            "decoded-string-values",
            b'package p; func f() { let a = ""; let b = "\\0"; '
            b'let c = "\\u{1F600}"; let d = "\\n\\r\\t\\"\\\\"; }',
        ),
        (
            "raw-string-newline",
            b"package p; func f() { let a = `line\n/*not trivia*/\\n`; }",
        ),
        ("invalid-escape", b'package p; func f() { let a = "\\x20"; }'),
        (
            "trivia-mix",
            b"package p;\n  // line\n\t/* block */ func f() {}\n",
        ),
        ("unterminated-block-comment", b"package p; /* open"),
        (
            "keyword-and-reserved",
            b"package p; func f() { let a = true; let b = false; "
            b"let c = async; let d = await; let e = x ++ y; let f = x -> y; "
            b"let g = x => y; }",
        ),
        (
            "numeric-tokens",
            b"package p; func f() { let a = 0; let b = 1_000; "
            b"let c = 0b10_01; let d = 0o7_1; let e = 0xAf_01; "
            b"let f = 1.25e-2; let g = 1e3; }",
        ),
        (
            "invalid-numbers",
            b"package p; func f() { let a = 007; let b = 0x_FF; "
            b"let c = 1__0; let d = 1e+; }",
        ),
        (
            "recovery-at-root",
            b"package p; func broken() { let x = 1 }\n"
            b"func later() { return; }\n",
        ),
        (
            "recovery-in-block",
            b"package p; func broken() { let x = 1; func later() { return; } }\n",
        ),
        (
            "nesting-limit",
            b"package p; func f() { let x = "
            + b"(" * 400
            + b"1"
            + b")" * 400
            + b"; } func later() {}\n",
        ),
        (
            "all-declarations",
            b'package p;\nimport "std/io" as io;\n'
            b"pub const N i32 = 1;\n"
            b"pub async func run[T](x T, inout y i32) i32 throws E { return x; }\n"
            b"struct S { pub field i32; }\n"
            b"enum E { A; B(i32); }\n"
            b"trait T { func m(self Self) i32; }\n"
            b"impl T for S { func m(self Self) i32 { return self.field; } }\n",
        ),
        (
            "patterns-and-effects",
            b"package p; func f(x i32) i32 { for y in [1, 2] { switch y { "
            b"case 1: return x; case E.A: break; default: continue; } } "
            b"try { defer { } unsafe { } } catch e Error { throw e; } "
            b"return 0; }\n",
        ),
        (
            "types-and-brackets",
            b"package p; func f() { let a = factory[func(inout i32) i32 throws E](x); "
            b"let b = factory[(func(i32) i32)](x); let c = table[(index) + 1]; "
            b"let d = dyn Trait[i32]; }",
        ),
    ]


def oracle_envelope(document: Any, case_name: str) -> None:
    if not isinstance(document, dict):
        raise CheckFailure(f"{case_name}: oracle output is not an object")
    if document.get("protocol") != PROTOCOL:
        raise CheckFailure(f"{case_name}: wrong oracle protocol")
    if document.get("operation_id") != OPERATION_ID:
        raise CheckFailure(f"{case_name}: wrong oracle operation_id")
    for key in ("source", "lex", "parse"):
        if key not in document:
            raise CheckFailure(f"{case_name}: oracle output is missing {key}")


DIAGNOSTIC_SHAPES = {
    "edge/diagnostic-lex-only-not-recovered": {
        "lex_nonempty": True,
        "parser_nonempty": False,
        "state": "complete",
        "recovered": False,
    },
    "edge/diagnostic-prefix-and-parser-recovery": {
        "lex_nonempty": True,
        "parser_nonempty": True,
        "state": "recovered",
        "recovered": True,
    },
}


def validate_diagnostic_projection(document: dict[str, Any], case_name: str) -> None:
    """Check parse.diagnostics is exactly [lex diagnostics] + parser diagnostics."""
    parsed = document["parse"]
    if parsed.get("state") == "not_run":
        return
    lexed = document["lex"]
    lex_diagnostics = lexed.get("diagnostics", [])
    parse_diagnostics = parsed.get("diagnostics", [])
    if parse_diagnostics[: len(lex_diagnostics)] != lex_diagnostics:
        raise CheckFailure(
            f"{case_name}: parse.diagnostics does not start with lex.diagnostics"
        )
    parser_diagnostics = parse_diagnostics[len(lex_diagnostics) :]
    shape = DIAGNOSTIC_SHAPES.get(case_name)
    if shape is None:
        return
    if bool(lex_diagnostics) != shape["lex_nonempty"]:
        raise CheckFailure(f"{case_name}: unexpected lexical diagnostic count")
    if bool(parser_diagnostics) != shape["parser_nonempty"]:
        raise CheckFailure(f"{case_name}: unexpected parser diagnostic count")
    if parsed.get("state") != shape["state"]:
        raise CheckFailure(f"{case_name}: unexpected projected parse state")
    if bool(parsed.get("recovered")) != shape["recovered"]:
        raise CheckFailure(f"{case_name}: lexical diagnostics changed parser recovery")


# Evidence records retain only these selected, non-credential run variables.
# This is a deliberate subset, not the complete process environment; the
# subprocess still receives the original env object unchanged.
RECORDED_ENV_KEYS = (
    "PATH",
    "LC_ALL",
    "PYTHONDONTWRITEBYTECODE",
    "TMPDIR",
    "LD_LIBRARY_PATH",
    "WOOZY_TOOLCHAIN_ROOT",
    "SH04_TRAP_LOG",
)


def recorded_env(env: dict[str, str]) -> dict[str, str]:
    """Return the allowlisted evidence subset without mutating ``env``."""
    return {key: env[key] for key in RECORDED_ENV_KEYS if key in env}


def write_case_record(path: Path, record: dict[str, Any]) -> None:
    safe_record = dict(record)
    if isinstance(safe_record.get("env"), dict):
        safe_record["env"] = recorded_env(safe_record["env"])
    path.write_text(json.dumps(safe_record, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run_oracle(
    binary: Path,
    cases: list[dict[str, Any]],
    args: argparse.Namespace,
    env: dict[str, str],
) -> tuple[dict[str, Path], list[dict[str, Any]]]:
    """Run independent oracle processes in a bounded batch."""
    outputs: dict[str, Path] = {}
    failures: list[dict[str, Any]] = []
    out_dir = args.work_dir / "oracle"
    out_dir.mkdir(parents=True, exist_ok=True)

    def one(case: dict[str, Any]) -> tuple[str, Path | None, dict[str, Any] | None]:
        run = run_process(
            [str(binary), str(case["path"])],
            env=env,
            timeout=args.oracle_timeout,
        )
        stem = case["name"].replace("/", "__")
        stdout_path = out_dir / f"{stem}.stdout"
        stderr_path = out_dir / f"{stem}.stderr"
        stdout_path.write_bytes(run["stdout"])
        stderr_path.write_bytes(run["stderr"])
        record: dict[str, Any] = {
            "case": case["name"],
            "group": case["group"],
            "input": {"path": str(case["path"]), "sha256": case["sha256"]},
            "argv": run["command"],
            "cwd": os.getcwd(),
            "env": recorded_env(env),
            "timeout_seconds": args.oracle_timeout,
            "exit": run["returncode"],
            "timed_out": run["timed_out"],
            "stdout": {"path": str(stdout_path),
                       "sha256": sha256_bytes(run["stdout"]),
                       "bytes": len(run["stdout"])},
            "stderr": {"path": str(stderr_path),
                       "sha256": sha256_bytes(run["stderr"]),
                       "bytes": len(run["stderr"])},
            "protocol": "oracle",
        }
        if run["returncode"] != 0 or run["timed_out"]:
            record.update({
                "validation": "failed",
                "error": "oracle process failed",
                "output": output_text(run),
            })
            write_case_record(out_dir / f"{stem}.json", record)
            return case["name"], None, {
                "case": case["name"],
                "status": "failed",
                "reason": "oracle process failed",
                "command": display_command(run["command"]),
                "returncode": run["returncode"],
                "timed_out": run["timed_out"],
                "output": output_text(run),
            }
        try:
            document = json.loads(run["stdout"].decode("utf-8"))
            oracle_envelope(document, case["name"])
            validate_diagnostic_projection(document, case["name"])
        except (UnicodeDecodeError, json.JSONDecodeError, CheckFailure) as error:
            record.update({
                "validation": "failed",
                "error": f"invalid oracle output: {error}",
                "output": output_text(run),
            })
            write_case_record(out_dir / f"{stem}.json", record)
            return case["name"], None, {
                "case": case["name"],
                "status": "failed",
                "reason": f"invalid oracle output: {error}",
                "command": display_command(run["command"]),
                "output": output_text(run),
            }
        record["validation"] = "passed"
        write_case_record(out_dir / f"{stem}.json", record)
        return case["name"], stdout_path, None

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(one, case) for case in cases]
        for future in concurrent.futures.as_completed(futures):
            name, output, failure = future.result()
            if failure is not None:
                failures.append(failure)
            else:
                assert output is not None
                outputs[name] = output
    failures.sort(key=lambda failure: failure.get("case", ""))
    return outputs, failures


def module_root() -> Path:
    return ROOT / "selfhost/src/wzcc"


def missing_wz_modules() -> list[str]:
    missing: list[str] = []
    root = module_root()
    for name in MODULE_NAMES:
        directory = root / name
        if not directory.is_dir() or not any(directory.glob("*.wz")):
            missing.append(str(directory.relative_to(ROOT)) + "/*.wz")
    return missing


def find_stage0(args: argparse.Namespace) -> Path | None:
    if args.stage0:
        return args.stage0
    candidate = ROOT / "build/woozy-stdlib-native/woozyc"
    return candidate if candidate.is_file() else None


def make_wz_project(work_dir: Path, modules: Path) -> Path:
    """Create an isolated project whose app/wzcc is the real source tree."""
    project = work_dir / "wz-project"
    source_root = project / "src"
    source_root.mkdir(parents=True, exist_ok=True)
    (source_root / "main.wz").write_bytes(DRIVER_SOURCE.read_bytes())
    link = source_root / "wzcc"
    if link.is_symlink() or link.exists():
        if link.is_dir() and not link.is_symlink():
            shutil.rmtree(link)
        else:
            link.unlink()
    link.symlink_to(modules, target_is_directory=True)
    (project / "lang.toml").write_text(
        "schema_version = 1\n\n"
        "[package]\n"
        'name = "woozy_selfhost_front_diff"\n'
        'source_root = "src"\n'
        'entry = "app"\n\n'
        "[build]\n"
        'backend = "native"\n'
        "word_bits = 64\n\n"
        "[build.native]\n"
        'runtime = "hosted"\n',
        encoding="utf-8",
    )
    return project


def write_traps(directory: Path, log: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name in TRAP_NAMES:
        path = directory / name
        path.write_text(
            "#!/bin/sh\n"
            f'printf "%s\\n" "$0 $*" >> "$SH04_TRAP_LOG"\n'
            "exit 97\n",
            encoding="utf-8",
        )
        path.chmod(0o755)
    log.write_text("", encoding="utf-8")


def build_wz(
    stage0: Path,
    project: Path,
    args: argparse.Namespace,
    env: dict[str, str],
) -> tuple[dict[int, Path], list[dict[str, Any]]]:
    binaries: dict[int, Path] = {}
    failures: list[dict[str, Any]] = []
    for optimization in (0, 2):
        out_dir = args.work_dir / f"wz/o{optimization}"
        out_dir.mkdir(parents=True, exist_ok=True)
        command = [
            str(stage0),
            "build",
            "--project",
            str(project / "lang.toml"),
            "--backend",
            "native",
            "--opt",
            str(optimization),
            "--out-dir",
            str(out_dir),
        ]
        run = run_process(command, env=env, timeout=args.build_timeout)
        (args.work_dir / f"logs/wz-o{optimization}-build.stdout").write_bytes(run["stdout"])
        (args.work_dir / f"logs/wz-o{optimization}-build.stderr").write_bytes(run["stderr"])
        if run["timed_out"]:
            reason = "Wz build timed out"
        elif run["returncode"] != 0:
            reason = f"Wz build failed with status {run['returncode']}"
        else:
            reason = ""
        program = out_dir / "program"
        if not reason and not program.is_file():
            reason = "Wz build did not produce program"
        if reason:
            failures.append(
                {
                    "phase": f"wz-build-o{optimization}",
                    "status": "failed",
                    "reason": reason,
                    "command": display_command(command),
                    "output": output_text(run),
                }
            )
            continue
        binaries[optimization] = program
    return binaries, failures


def run_wz(
    binaries: dict[int, Path],
    cases: list[dict[str, Any]],
    oracle_paths: dict[str, Path],
    args: argparse.Namespace,
) -> tuple[list[dict[str, Any]], list[str]]:
    """Run independent Wz processes in bounded batches and compare on disk."""
    failures: list[dict[str, Any]] = []
    trap_dir = args.work_dir / "runtime-traps"
    trap_log = args.work_dir / "runtime-traps.log"
    write_traps(trap_dir, trap_log)
    out_dir = args.work_dir / "wz/output"
    out_dir.mkdir(parents=True, exist_ok=True)
    all_trap_calls: list[str] = []

    for optimization, binary in binaries.items():
        payload = binary.read_bytes()
        for forbidden in ("woozyc", "lang_native_helper", "stage0_oracle", "front_diff.py"):
            if forbidden.encode("ascii") in payload:
                failures.append(
                    {
                        "phase": f"wz-binary-o{optimization}",
                        "status": "failed",
                        "reason": f"Wz binary contains forbidden symbol {forbidden!r}",
                    }
                )
        optimization_log = args.work_dir / f"runtime-traps-o{optimization}.log"
        optimization_log.write_text("", encoding="utf-8")
        runtime_env = {
            "PATH": str(trap_dir),
            "SH04_TRAP_LOG": str(optimization_log),
            "LC_ALL": "C",
        }

        def one(case: dict[str, Any]) -> dict[str, Any] | None:
            key = f"o{optimization}/{case['name']}"
            run = run_process(
                [str(binary), str(case["path"])],
                env=runtime_env,
                timeout=args.timeout,
            )
            stem = key.replace("/", "__")
            stdout_path = out_dir / f"{stem}.stdout"
            stderr_path = out_dir / f"{stem}.stderr"
            stdout_path.write_bytes(run["stdout"])
            stderr_path.write_bytes(run["stderr"])
            record: dict[str, Any] = {
                "case": case["name"],
                "group": case["group"],
                "optimization": optimization,
                "input": {"path": str(case["path"]),
                          "sha256": case["sha256"]},
                "argv": run["command"],
                "cwd": os.getcwd(),
                "env": recorded_env(runtime_env),
                "timeout_seconds": args.timeout,
                "exit": run["returncode"],
                "timed_out": run["timed_out"],
                "stdout": {"path": str(stdout_path),
                           "sha256": sha256_bytes(run["stdout"]),
                           "bytes": len(run["stdout"])},
                "stderr": {"path": str(stderr_path),
                           "sha256": sha256_bytes(run["stderr"]),
                           "bytes": len(run["stderr"])},
                "protocol": "wz",
                "expected": None,
                "protocol_compare": {"status": "not_run"},
            }
            if run["returncode"] != 0 or run["timed_out"]:
                record.update({
                    "validation": "failed",
                    "error": "Wz driver process failed",
                    "output": output_text(run),
                })
                write_case_record(out_dir / f"{stem}.json", record)
                return {
                    "case": case["name"],
                    "optimization": optimization,
                    "status": "failed",
                    "reason": "Wz driver process failed",
                    "command": display_command(run["command"]),
                    "returncode": run["returncode"],
                    "timed_out": run["timed_out"],
                    "output": output_text(run),
                }
            try:
                document = json.loads(run["stdout"].decode("utf-8"))
                oracle_envelope(document, case["name"])
                validate_diagnostic_projection(document, case["name"])
            except (UnicodeDecodeError, json.JSONDecodeError, CheckFailure) as error:
                record.update({
                    "validation": "failed",
                    "error": f"invalid Wz output: {error}",
                    "output": output_text(run),
                })
                write_case_record(out_dir / f"{stem}.json", record)
                return {
                    "case": case["name"],
                    "optimization": optimization,
                    "status": "failed",
                    "reason": f"invalid Wz output: {error}",
                    "command": display_command(run["command"]),
                    "output": output_text(run),
                }
            oracle_path = oracle_paths.get(case["name"])
            if oracle_path is None:
                record.update({
                    "validation": "failed",
                    "error": "oracle output is missing for this case",
                })
                write_case_record(out_dir / f"{stem}.json", record)
                return {
                    "case": case["name"],
                    "optimization": optimization,
                    "status": "failed",
                    "reason": "oracle output is missing for this case",
                }
            try:
                oracle_bytes = oracle_path.read_bytes()
                expected = json.loads(oracle_bytes.decode("utf-8"))
            except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
                record.update({
                    "validation": "failed",
                    "error": f"cannot read oracle output: {error}",
                })
                write_case_record(out_dir / f"{stem}.json", record)
                return {
                    "case": case["name"],
                    "optimization": optimization,
                    "status": "failed",
                    "reason": f"cannot read oracle output: {error}",
                }
            record["expected"] = {
                "path": str(oracle_path),
                "sha256": sha256_bytes(oracle_bytes),
                "bytes": len(oracle_bytes),
            }
            differences = compare(expected, document)
            if differences:
                record.update({
                    "validation": "protocol_failed",
                    "protocol_compare": {
                        "status": "failed",
                        "differences": differences[:20],
                    },
                })
                write_case_record(out_dir / f"{stem}.json", record)
                return {
                    "case": case["name"],
                    "optimization": optimization,
                    "status": "failed",
                    "reason": "protocol differs",
                    "differences": differences[:20],
                }
            record["validation"] = "passed"
            record["protocol_compare"] = {"status": "passed", "differences": []}
            write_case_record(out_dir / f"{stem}.json", record)
            return None

        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures = [pool.submit(one, case) for case in cases]
            for future in concurrent.futures.as_completed(futures):
                failure = future.result()
                if failure is not None:
                    failures.append(failure)
        trap_calls = optimization_log.read_text(
            encoding="utf-8", errors="replace").strip()
        if trap_calls:
            lines = trap_calls.splitlines()[:20]
            all_trap_calls.extend(lines)
            failures.append(
                {
                    "phase": "wz-runtime-isolation",
                    "optimization": optimization,
                    "status": "failed",
                    "reason": "Wz driver attempted to execute a trap command",
                    "calls": lines,
                }
            )
    failures.sort(key=lambda failure: (
        failure.get("case", ""), failure.get("optimization", -1),
        failure.get("phase", "")))
    return failures, all_trap_calls


def compare(expected: Any, actual: Any, path: str = "$") -> list[str]:
    """Return a bounded list of structural differences with JSON-like paths."""
    differences: list[str] = []

    def visit(left: Any, right: Any, where: str) -> None:
        if len(differences) >= 50:
            return
        if type(left) is not type(right):
            differences.append(
                f"{where}: type {type(left).__name__} != {type(right).__name__}"
            )
            return
        if isinstance(left, dict):
            left_keys = list(left)
            right_keys = list(right)
            if set(left_keys) != set(right_keys):
                missing = sorted(set(left_keys) - set(right_keys))
                extra = sorted(set(right_keys) - set(left_keys))
                differences.append(f"{where}: missing={missing} extra={extra}")
                return
            for key in left_keys:
                visit(left[key], right[key], f"{where}.{key}")
        elif isinstance(left, list):
            if len(left) != len(right):
                differences.append(f"{where}: length {len(left)} != {len(right)}")
                return
            for index, (left_item, right_item) in enumerate(zip(left, right)):
                visit(left_item, right_item, f"{where}[{index}]")
        elif left != right:
            differences.append(f"{where}: {left!r} != {right!r}")

    visit(expected, actual, path)
    return differences


def write_summary(summary: dict[str, Any], work_dir: Path) -> Path:
    path = work_dir / "summary.json"
    path.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"),
                        help="host C++ compiler for the stage-0 oracle")
    parser.add_argument("--stage0", type=Path,
                        help="stage-0 woozyc (default: build/woozy-stdlib-native/woozyc)")
    parser.add_argument("--work-dir", type=Path,
                        default=ROOT / f"tmp/checks/{OPERATION_ID}")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="timeout per Wz program run, seconds")
    parser.add_argument("--oracle-timeout", type=float,
                        default=DEFAULT_ORACLE_TIMEOUT,
                        help="timeout per C++ oracle run, seconds "
                             f"(default: {DEFAULT_ORACLE_TIMEOUT:g})")
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS,
                        help=f"bounded parallel case batch size "
                             f"(default: {DEFAULT_JOBS})")
    parser.add_argument("--build-timeout", type=float, default=600.0,
                        help="timeout per oracle/Wz build, seconds")
    parser.add_argument("--env", action="append", default=[],
                        help="NAME=VALUE added to compiler/build environments")
    parser.add_argument("--require-wz", action="store_true",
                        help="exit nonzero when Wz modules are missing or Wz fails")
    args = parser.parse_args()
    if args.timeout <= 0 or args.oracle_timeout <= 0:
        parser.error("--timeout and --oracle-timeout must be positive")
    if args.jobs <= 0:
        parser.error("--jobs must be positive")

    args.work_dir = args.work_dir.resolve()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    (args.work_dir / "logs").mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.update(parse_env(args.env))

    summary: dict[str, Any] = {
        "operation_id": OPERATION_ID,
        "protocol": PROTOCOL,
        "scope": "SH04 source/lexer/parser differential",
        "environment": {
            "cxx": args.cxx,
            "stage0": None,
            "work_dir": str(args.work_dir),
            "jobs": args.jobs,
            "oracle_timeout": args.oracle_timeout,
            "wz_timeout": args.timeout,
            "fixed_cases": None,
            "inputs_sha256": None,
        },
        "counts": {},
        "oracle": {},
        "wz": {},
        "failures": [],
        "not_run": [],
    }

    try:
        cases = load_cases(args.work_dir / "cases")
    except (OSError, KeyError, ValueError, CheckFailure, json.JSONDecodeError) as error:
        summary["failures"].append({"status": "failed", "reason": f"case loading failed: {error}"})
        write_summary(summary, args.work_dir)
        print(f"FAIL: case loading failed: {error}", file=sys.stderr)
        return 1

    groups: dict[str, int] = {}
    for case in cases:
        groups[case["group"]] = groups.get(case["group"], 0) + 1
    input_records = [
        {"name": case["name"], "group": case["group"],
         "path": str(case["path"]), "sha256": case["sha256"],
         "bytes": len(case["bytes"])}
        for case in cases
    ]
    input_document = {
        "schema": "woozy-front-inputs/1",
        "operation_id": OPERATION_ID,
        "count": len(input_records),
        "cases": input_records,
    }
    input_text = json.dumps(input_document, ensure_ascii=False, sort_keys=True,
                            indent=2) + "\n"
    input_path = args.work_dir / "inputs.json"
    input_path.write_text(input_text, encoding="utf-8")
    input_sha = hashlib.sha256(json.dumps(
        input_records, ensure_ascii=False, sort_keys=True,
        separators=(",", ":")).encode("utf-8")).hexdigest()
    summary["counts"] = {
        "cases": len(cases), "groups": groups,
        "inputs_path": str(input_path), "inputs_sha256": input_sha,
    }
    summary["environment"]["inputs_sha256"] = input_sha
    summary["environment"]["fixed_cases"] = len(cases)

    oracle_binary, oracle_build_failure = build_oracle(args, env)
    if oracle_build_failure:
        summary["oracle"] = oracle_build_failure
        summary["failures"].append({"phase": "oracle-build", **oracle_build_failure})
        write_summary(summary, args.work_dir)
        print("FAIL: C++ stage-0 oracle build failed", file=sys.stderr)
        if oracle_build_failure.get("output"):
            print(oracle_build_failure["output"], file=sys.stderr)
        return 1

    oracle_outputs, oracle_failures = run_oracle(oracle_binary, cases, args, env)
    summary["oracle"] = {
        "status": "passed" if not oracle_failures else "failed",
        "passed": len(oracle_outputs),
        "failed": len(oracle_failures),
        "cases": len(cases),
    }
    summary["failures"].extend(oracle_failures)
    if oracle_failures or len(oracle_outputs) != len(cases):
        write_summary(summary, args.work_dir)
        print(
            f"FAIL: C++ stage-0 oracle {len(oracle_outputs)}/{len(cases)} passed",
            file=sys.stderr,
        )
        for failure in oracle_failures[:20]:
            print(f"  {failure.get('case', '<build>')}: {failure.get('reason')}", file=sys.stderr)
        return 1

    missing = missing_wz_modules()
    stage0 = find_stage0(args)
    modules = module_root()
    summary["environment"]["stage0"] = str(stage0) if stage0 else None
    summary["environment"]["module_root"] = str(modules)
    if missing or stage0 is None:
        reasons = []
        if missing:
            reasons.append("missing real Wz modules: " + ", ".join(missing))
        if stage0 is None:
            reasons.append("stage-0 compiler not found (build/woozy-stdlib-native/woozyc)")
        reason = "; ".join(reasons)
        summary["wz"] = {
            "status": "not_run",
            "reason": reason,
            "missing_modules": missing,
            "o0": {"status": "not_run"},
            "o2": {"status": "not_run"},
        }
        summary["not_run"].append(
            {
                "scope": "Wz O0/O2 differential",
                "status": "not_run",
                "reason": reason,
            }
        )
        summary_path = write_summary(summary, args.work_dir)
        print(f"PASS: C++ stage-0 oracle {len(oracle_outputs)}/{len(cases)} cases")
        print(f"NOT_RUN: Wz O0/O2 differential: {reason}")
        print(f"summary: {summary_path}")
        return 1 if args.require_wz else 0

    try:
        project = make_wz_project(args.work_dir, modules)
    except OSError as error:
        summary["wz"] = {"status": "failed", "reason": f"Wz project setup failed: {error}"}
        summary["failures"].append(summary["wz"])
        write_summary(summary, args.work_dir)
        print(f"FAIL: Wz project setup failed: {error}", file=sys.stderr)
        return 1

    binaries, build_failures = build_wz(stage0, project, args, env)
    summary["failures"].extend(build_failures)
    if build_failures:
        for optimization in (0, 2):
            summary["wz"][f"o{optimization}"] = {
                "status": "passed" if optimization in binaries else "failed"
            }
        summary["wz"]["status"] = "failed"
        write_summary(summary, args.work_dir)
        print("FAIL: Wz O0/O2 build failed", file=sys.stderr)
        for failure in build_failures:
            print(f"  {failure['phase']}: {failure['reason']}", file=sys.stderr)
            if failure.get("output"):
                print(failure["output"], file=sys.stderr)
        return 1

    wz_failures, trap_calls = run_wz(binaries, cases, oracle_outputs, args)
    summary["failures"].extend(wz_failures)
    for optimization in (0, 2):
        total = len(cases)
        passed = sum(
            1
            for case in cases
            if not any(
                failure.get("case") == case["name"]
                and failure.get("optimization") == optimization
                for failure in wz_failures
            )
        )
        summary["wz"][f"o{optimization}"] = {
            "status": "passed" if passed == total and not build_failures else "failed",
            "passed": passed,
            "failed": total - passed,
            "cases": total,
        }
    if trap_calls:
        summary["wz"]["runtime_isolation"] = "failed"
    summary["wz"]["status"] = (
        "passed"
        if len(wz_failures) == 0
        and summary["wz"]["o0"]["status"] == "passed"
        and summary["wz"]["o2"]["status"] == "passed"
        else "failed"
    )
    summary_path = write_summary(summary, args.work_dir)
    if summary["wz"]["status"] == "passed":
        print(f"PASS: C++ stage-0 oracle {len(oracle_outputs)}/{len(cases)} cases")
        print(f"PASS: Wz O0 and O2 match the oracle on {len(cases)} cases")
        print(f"summary: {summary_path}")
        return 0
    print(f"PASS: C++ stage-0 oracle {len(oracle_outputs)}/{len(cases)} cases")
    print(f"FAIL: Wz differential has {len(wz_failures)} failure(s)", file=sys.stderr)
    for failure in wz_failures[:20]:
        where = failure.get("case", failure.get("phase", "<build>"))
        optimization = failure.get("optimization")
        suffix = f" o{optimization}" if optimization is not None else ""
        print(f"  {where}{suffix}: {failure.get('reason')}", file=sys.stderr)
        for difference in failure.get("differences", []):
            print(f"    {difference}", file=sys.stderr)
        if failure.get("output"):
            print(failure["output"], file=sys.stderr)
    print(f"summary: {summary_path}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
