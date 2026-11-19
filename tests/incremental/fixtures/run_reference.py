#!/usr/bin/env python3
"""Materialize one source-package fixture variant and run the existing reference.

The fixtures are data, not a cache implementation.  This driver copies a
fixture's base project to a new work directory, applies the machine-readable
mutation exactly once, then either:

  * build-and-run: invokes ``woozyc build --backend native --opt 0`` and runs
    the generated program; or
  * check: invokes ``woozyc check`` for an expected compile-time failure.

It refuses to overwrite an existing variant result.  All build output and raw
stdout/stderr live under the caller-provided work/raw directories.  The fixtures
themselves contain only Wz/TOML/JSON/Markdown source data.

Typical invocation (the caller supplies the frozen reference and its runtime
environment):

  python3 run_reference.py \
    --reference /path/to/woozyc \
    --work /path/to/D/build/fixtures \
    --raw /path/to/D/raw/fixtures \
    --report /path/to/D/reference-results.json \
    --commands /path/to/D/reference/commands.json

The driver does not retry a failed command and does not run Ninja.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.dont_write_bytecode = True

ENV_KEYS = (
    "PATH",
    "LD_LIBRARY_PATH",
    "LANG",
    "LC_ALL",
    "TZ",
    "PYTHONDONTWRITEBYTECODE",
    "PYTHONNOUSERSITE",
)
FORBIDDEN_CODES = ("E_INTERNAL_ERROR", "E_IR_INVALID")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_path(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def clean_env() -> dict:
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONNOUSERSITE"] = "1"
    env.pop("PYTHONPATH", None)
    return env


def inside(root: Path, candidate: Path) -> bool:
    root = root.resolve()
    candidate = candidate.resolve()
    return candidate == root or str(candidate).startswith(str(root) + os.sep)


def safe_path(root: Path, relative: str) -> Path:
    if Path(relative).is_absolute():
        raise ValueError("absolute path is not allowed: %s" % relative)
    candidate = root / relative
    if not inside(root, candidate):
        raise ValueError("path escapes project: %s" % relative)
    return candidate


def write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def run_command(argv, cwd, env, timeout, raw_dir, label):
    started = time.monotonic()
    timed_out = False
    try:
        completed = subprocess.run(
            [str(item) for item in argv],
            cwd=str(cwd),
            env=env,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        exit_code = completed.returncode
        stdout = completed.stdout
        stderr = completed.stderr
    except subprocess.TimeoutExpired as error:
        timed_out = True
        exit_code = None
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode("utf-8", "replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", "replace")
    elapsed = time.monotonic() - started
    raw_dir.mkdir(parents=True, exist_ok=True)
    (raw_dir / (label + ".stdout")).write_text(stdout)
    (raw_dir / (label + ".stderr")).write_text(stderr)
    record = {
        "label": label,
        "argv": [str(item) for item in argv],
        "cwd": str(cwd),
        "timeout_seconds": timeout,
        "exit": exit_code,
        "timed_out": timed_out,
        "elapsed_seconds": round(elapsed, 6),
        "stdout_sha256": sha256_bytes(stdout.encode()),
        "stderr_sha256": sha256_bytes(stderr.encode()),
        "stdout_file": str(raw_dir / (label + ".stdout")),
        "stderr_file": str(raw_dir / (label + ".stderr")),
        "stdout": stdout,
        "stderr": stderr,
    }
    return record


def materialize(fixture_dir: Path, destination: Path, mutation: dict | None):
    base = fixture_dir / "base"
    if not (base / "lang.toml").is_file():
        raise ValueError("missing base manifest: %s" % (base / "lang.toml"))
    if destination.exists():
        raise ValueError("refusing to reuse existing work directory: %s" % destination)
    shutil.copytree(base, destination)
    applied = []
    if mutation is not None:
        for action in mutation["actions"]:
            op = action["op"]
            target = safe_path(destination, action["path"])
            old = action.get("old")
            new = action.get("new")
            if op in ("replace", "remove"):
                if old is None or not target.is_file():
                    raise ValueError("missing old file for %s: %s" % (op, action["path"]))
                old_bytes = target.read_bytes()
                if sha256_bytes(old_bytes) != old["sha256"]:
                    raise ValueError("old sha mismatch: %s" % action["path"])
                old_ref = fixture_dir / "variants" / mutation["variant"] / old["file"]
                if old_ref.read_bytes() != old_bytes:
                    raise ValueError("old reference differs: %s" % old["file"])
            elif op == "add":
                if target.exists():
                    raise ValueError("add target already exists: %s" % action["path"])
            else:
                raise ValueError("unknown mutation op: %s" % op)
            if op in ("replace", "add"):
                if new is None:
                    raise ValueError("missing new file for %s: %s" % (op, action["path"]))
                new_ref = fixture_dir / "variants" / mutation["variant"] / new["file"]
                new_bytes = new_ref.read_bytes()
                if sha256_bytes(new_bytes) != new["sha256"]:
                    raise ValueError("new reference sha mismatch: %s" % new["file"])
                write_bytes(target, new_bytes)
            elif op == "remove":
                target.unlink()
            applied.append(
                {
                    "op": op,
                    "path": action["path"],
                    "old_sha256": old["sha256"] if old else None,
                    "new_sha256": new["sha256"] if new else None,
                }
            )
    files = {}
    for path in sorted(destination.rglob("*")):
        if path.is_file():
            files[path.relative_to(destination).as_posix()] = sha256_path(path)
    return applied, files


def parse_status(record):
    try:
        return json.loads(record["stdout"])
    except json.JSONDecodeError as error:
        return {"_parse_error": str(error)}


def error_codes(payload):
    return [
        item.get("code")
        for item in payload.get("diagnostics", [])
        if item.get("severity") == "error"
    ]


def compare(expect: dict, actual: dict, payloads: dict):
    failures = []

    def require(condition, message):
        if not condition:
            failures.append(message)

    if expect["mode"] == "check":
        require(actual["check_exit"] == expect["check_exit"],
                "check exit %r != %r" % (actual["check_exit"], expect["check_exit"]))
        require(payloads["check"].get("status") == expect["status"],
                "check status %r != %r" % (payloads["check"].get("status"), expect["status"]))
        codes = error_codes(payloads["check"])
        expected_codes = expect.get("error_codes_any", [])
        require(any(code in codes for code in expected_codes),
                "expected one of %r in error codes %r" % (expected_codes, codes))
        forbidden = [code for code in codes if code in FORBIDDEN_CODES]
        require(not forbidden, "forbidden internal diagnostics: %r" % forbidden)
    else:
        require(actual["build_exit"] == expect["build_exit"],
                "build exit %r != %r" % (actual["build_exit"], expect["build_exit"]))
        require(payloads["build"].get("status") == expect["status"],
                "build status %r != %r" % (payloads["build"].get("status"), expect["status"]))
        run = actual.get("run")
        require(run is not None, "missing program run")
        if run is not None:
            require(run["exit"] == expect["run_exit"],
                    "run exit %r != %r" % (run["exit"], expect["run_exit"]))
            require(run["stdout"] == expect["stdout"],
                    "run stdout %r != %r" % (run["stdout"], expect["stdout"]))
            require(run["stderr"] == expect["stderr"],
                    "run stderr %r != %r" % (run["stderr"], expect["stderr"]))
            require(not run["timed_out"], "program timed out")
        codes = error_codes(payloads["build"])
        forbidden = [code for code in codes if code in FORBIDDEN_CODES]
        require(not forbidden, "forbidden internal diagnostics: %r" % forbidden)
    return failures


def run_one(fixture_dir, variant, fixture, mutation, args, env):
    fixture_id = fixture["id"]
    variant_dir = args.raw / fixture_id / variant
    if (variant_dir / "result.json").exists():
        raise ValueError("refusing to replay existing result: %s" % (variant_dir / "result.json"))
    work_dir = args.work / fixture_id / variant
    project = work_dir / "project"
    out_dir = work_dir / "out"
    if variant == "base":
        expect = fixture["base"]["expect"]
    else:
        expect = mutation["expect"]
    base = fixture_dir / "base"
    base_files = {
        path.relative_to(base).as_posix(): sha256_path(path)
        for path in sorted(base.rglob("*"))
        if path.is_file()
    }
    if base_files != fixture["base"]["file_sha256"]:
        raise ValueError("base file hashes do not match fixture.json")
    applied, files = materialize(fixture_dir, project, mutation)
    commands = []
    payloads = {}
    actual = {"mode": expect["mode"]}
    result = {
        "schema": "woozy-incremental-reference-run/1",
        "fixture": fixture_id,
        "variant": variant,
        "reference": str(args.reference),
        "reference_sha256": sha256_path(args.reference),
        "expect": expect,
        "applied_actions": applied,
        "project_files_sha256": files,
        "project_dir": str(project),
        "out_dir": str(out_dir),
        "commands": commands,
        "env": {key: env.get(key) for key in ENV_KEYS},
    }
    try:
        if expect["mode"] == "check":
            check = run_command(
                [args.reference, "check", "--project", project / "lang.toml",
                 "--format", "json"],
                project, env, args.timeout, variant_dir, "check")
            commands.append(check)
            payloads["check"] = parse_status(check)
            actual["check_exit"] = check["exit"]
        else:
            out_dir.mkdir(parents=True, exist_ok=False)
            build = run_command(
                [args.reference, "build", "--project", project / "lang.toml",
                 "--backend", "native", "--opt", "0", "--out-dir", out_dir,
                 "--format", "json"],
                project, env, args.timeout, variant_dir, "build")
            commands.append(build)
            payloads["build"] = parse_status(build)
            actual["build_exit"] = build["exit"]
            program = out_dir / "program"
            if program.is_file():
                result["program_sha256"] = sha256_path(program)
                run = run_command([program], project, env, args.run_timeout,
                                  variant_dir, "run")
                commands.append(run)
                actual["run"] = run
        result["actual"] = actual
        result["payloads"] = payloads
        failures = compare(expect, actual, payloads)
        result["verdict"] = "passed" if not failures else "failed"
        result["failures"] = failures
    except Exception as error:  # Keep the failure record; never retry automatically.
        result["actual"] = actual
        result["payloads"] = payloads
        result["verdict"] = "failed"
        result["failures"] = ["driver error: %s: %s" % (type(error).__name__, error)]
    variant_dir.mkdir(parents=True, exist_ok=True)
    (variant_dir / "result.json").write_text(
        json.dumps(result, indent=2, ensure_ascii=False) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--commands", type=Path, required=True)
    parser.add_argument("--only", action="append", default=[],
                        help="fixture id or fixture_id:variant; default all")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--run-timeout", type=float, default=20.0)
    args = parser.parse_args()
    args.reference = args.reference.resolve()
    if not args.reference.is_file():
        parser.error("reference does not exist: %s" % args.reference)
    if args.timeout <= 0 or args.run_timeout <= 0:
        parser.error("timeouts must be positive")
    env = clean_env()
    fixtures = []
    for fixture_json in sorted(args.fixtures.glob("*/fixture.json")):
        fixture = json.loads(fixture_json.read_text())
        fixture_dir = fixture_json.parent
        mutation_paths = sorted((fixture_dir / "variants").glob("*/mutation.json"))
        variants = {path.parent.name: json.loads(path.read_text())
                    for path in mutation_paths}
        if sorted(fixture["variants"]) != sorted(variants):
            raise SystemExit("variant list mismatch for %s" % fixture["id"])
        fixtures.append((fixture, fixture_dir, variants))
    selected = set(args.only)
    if selected:
        wanted = {item.split(":", 1)[0] for item in selected}
        fixtures = [item for item in fixtures if item[0]["id"] in wanted]
    all_results = []
    all_commands = []
    for fixture, fixture_dir, variants in fixtures:
        targets = [("base", None)]
        targets += [(name, variants[name]) for name in fixture["variants"]]
        if selected:
            for item in selected:
                parts = item.split(":", 1)
                if parts[0] == fixture["id"] and len(parts) == 2:
                    if parts[1] == "base":
                        targets = [target for target in targets if target[0] == "base"]
                    else:
                        targets = [target for target in targets if target[0] == parts[1]]
        for variant, mutation in targets:
            print("running %s:%s" % (fixture["id"], variant), flush=True)
            result = run_one(fixture_dir, variant, fixture, mutation, args, env)
            all_results.append(result)
            all_commands.extend(result["commands"])
            print("  %s %s" % (result["verdict"], result.get("failures", [])),
                  flush=True)
    args.commands.parent.mkdir(parents=True, exist_ok=True)
    args.commands.write_text(json.dumps({
        "reference": str(args.reference),
        "reference_sha256": sha256_path(args.reference),
        "commands": all_commands,
    }, indent=2, ensure_ascii=False) + "\n")
    summary = {
        "schema": "woozy-incremental-reference-report/1",
        "status": "passed" if all(r["verdict"] == "passed" for r in all_results) else "failed",
        "reference": str(args.reference),
        "reference_sha256": sha256_path(args.reference),
        "runs": len(all_results),
        "passed": sum(r["verdict"] == "passed" for r in all_results),
        "failed": sum(r["verdict"] == "failed" for r in all_results),
        "not_run": "cache/Ninja/compiler implementation and performance are intentionally not run",
        "results": [
            {
                "fixture": r["fixture"],
                "variant": r["variant"],
                "verdict": r["verdict"],
                "expect": r["expect"],
                "actual": r["actual"],
                "failures": r.get("failures", []),
                "result_file": str(args.raw / r["fixture"] / r["variant"] / "result.json"),
                "project_files_sha256": r["project_files_sha256"],
            }
            for r in all_results
        ],
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({key: summary[key] for key in
                      ("status", "runs", "passed", "failed", "reference_sha256")},
                     indent=2))
    return 0 if summary["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
