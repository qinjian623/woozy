#!/usr/bin/env python3
"""Build, run and observe real Wz `Slice[T]` programs on the Python backend.

Every fixture is compiled twice through public entry points: the `woozyc`
driver with `--backend python`, and the private `lang_python_emit` helper.  The
generated `program.py`/`langrt.py` hashes must match.  Programs then run under
`observe.py`, which disables CPython's automatic collector, forbids
`gc.collect`, and checks logical Vec retain/release, Slice pin/end, descriptor
drop/retirement and final live roots.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True

POSITIVE = ("python-slice", "core", "owned", "cleanup", "throws-return",
            "after-drop", "empty", "move", "nested-to-vec", "for-in",
            "compound", "reassign", "gc-cycle")
PANIC = {
    "structural-push": "Vec length changes during iteration",
    "structural-pop": "Vec length changes during iteration",
    "structural-clear": "Vec length changes during iteration",
    "structural-resize": "Vec length changes during iteration",
    "structural-reserve": "Vec storage changes during iteration",
    "empty-still-pins": "Vec length changes during iteration",
    "negative-index": "Slice index out of bounds",
    "huge-index": "Slice index out of bounds",
    "bounds-order": "Slice range out of bounds",
    "bounds-high": "Slice range out of bounds",
    "sub-bounds": "Slice range out of bounds",
    "negative-sub": "Slice start out of bounds",
    "huge-sub": "Slice range out of bounds",
    "huge-view": "Slice range out of bounds",
}
NEGATIVE = {"unsupported-c": "E_CAPABILITY_UNAVAILABLE"}

# Exact logical facts selected per fixture.  The observer already checks the
# global balance invariants for every positive program; these rows add the
# operation counts that distinguish view/copy/to-vec/GC paths.
EXPECTED = {
    "python-slice": {"slice_create": 1},
    "core": {"slice_to_vec": 1, "min_create": 6, "allocations": 4},
    "owned": {"slice_to_vec": 2, "min_create": 4},
    "cleanup": {"min_create": 3},
    "throws-return": {"min_create": 3},
    "after-drop": {"min_create": 2},
    "empty": {"slice_to_vec": 1, "min_create": 3},
    "move": {"min_create": 2},
    "nested-to-vec": {"slice_to_vec": 1, "min_create": 1},
    "for-in": {"min_create": 2},
    "compound": {"min_create": 1},
    "reassign": {"min_create": 2},
    "gc-cycle": {"min_create": 1, "min_retire": 1, "min_collected": 1},
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, timeout, log):
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONNOUSERSITE="1")
    env.pop("PYTHONPATH", None)
    started = subprocess.run(
        [str(value) for value in argv], capture_output=True, text=True,
        timeout=timeout, env=env)
    log.append({"argv": [str(value) for value in argv],
                "exit": started.returncode, "stdout": started.stdout,
                "stderr": started.stderr})
    return started


def payload(result):
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"expected JSON, got {result.stdout!r}: {error}")


def build(manifest: Path, out: Path, timeout, log, cli: Path, helper: Path):
    cli_result = command(
        [cli, "build", "--project", manifest, "--backend", "python",
         "--out-dir", out / "cli", "--format", "json"], timeout, log)
    document = payload(cli_result)
    if cli_result.returncode != 0 or document.get("status") != "ok":
        raise RuntimeError(f"CLI build failed for {manifest}: {document}")
    if document.get("diagnostics"):
        raise RuntimeError(f"CLI diagnostics for {manifest}: {document}")
    helper_result = command(
        [helper, "--project", manifest, "--out-dir", out / "helper"],
        timeout, log)
    helper_document = payload(helper_result)
    if helper_result.returncode != 0 or helper_document.get("status") != "ok":
        raise RuntimeError(f"helper build failed for {manifest}: {helper_document}")
    for name in ("program.py", "langrt.py"):
        if digest(out / "cli" / name) != digest(out / "helper" / name):
            raise RuntimeError(f"CLI/helper disagree on generated {name}")
    runtime = (out / "cli" / "langrt.py").read_text(encoding="utf-8")
    if "RUNTIME_VERSION = 'compiler-dev/1-python-values/9'" not in runtime:
        raise RuntimeError("generated runtime is not the expected version 9")
    return {"program": digest(out / "cli" / "program.py"),
            "runtime": digest(out / "cli" / "langrt.py")}


def run_observer(python: Path, root: Path, out: Path, panic: bool,
                 timeout, log):
    argv = [python, "-s", root / "tests/python/slice_source/observe.py",
            out / "cli"]
    if panic:
        argv.append("panic")
    result = command(argv, timeout, log)
    document = payload(result)
    if result.returncode != 0 or document.get("status") != "passed":
        raise RuntimeError(f"observer rejected {out}: {document}")
    return document


def check_positive(name, observed):
    expected = EXPECTED[name]
    if observed["exit"] != 0:
        raise RuntimeError(f"{name}: unexpected exit {observed['exit']}")
    if observed["program_stdout"] or observed["program_stderr"]:
        raise RuntimeError(f"{name}: program produced output")
    stats, counts = observed["stats"], observed["counts"]
    if stats["live"] != 0 or observed["live_vectors"] != 0:
        raise RuntimeError(f"{name}: live language roots at exit")
    if "slice_to_vec" in expected:
        if counts["slice_to_vec"] != expected["slice_to_vec"]:
            raise RuntimeError(f"{name}: to_vec count {counts}")
    if "min_create" in expected and counts["slice_create"] < expected["min_create"]:
        raise RuntimeError(f"{name}: too few Slice creations {counts}")
    if "min_retire" in expected and counts["slice_retire"] < expected["min_retire"]:
        raise RuntimeError(f"{name}: no collected Slice edge {counts}")
    if "min_collected" in expected and stats["total_collected"] < expected["min_collected"]:
        raise RuntimeError(f"{name}: collector freed nothing {stats}")
    if "allocations" in expected and stats["allocations"] != expected["allocations"]:
        raise RuntimeError(f"{name}: Vec allocation count {stats}")
    for key, value in expected.items():
        if key in ("slice_create", "slice_copy", "slice_drop",
                   "slice_begin", "slice_end"):
            if counts[key] != value:
                raise RuntimeError(f"{name}: {key}={counts[key]} != {value}")


def check_panic(name, observed, message):
    if observed["exit"] != 101:
        raise RuntimeError(f"{name}: expected Panic exit 101, got {observed['exit']}")
    if observed["program_stdout"]:
        raise RuntimeError(f"{name}: Panic wrote stdout")
    if f"LANG_PANIC: {message}\n" != observed["program_stderr"]:
        raise RuntimeError(f"{name}: wrong Panic record {observed['program_stderr']!r}")
    counts = observed["counts"]
    if counts["slice_create"] < 1:
        raise RuntimeError(f"{name}: no attempted view creation {counts}")
    if counts["slice_drop"] or counts["slice_retire"]:
        raise RuntimeError(f"{name}: Panic performed cleanup {counts}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    for key in ("cli", "helper", "root", "out", "python"):
        value = getattr(args, key)
        if not value.is_absolute():
            found = shutil.which(str(value))
            value = Path(found) if found else value
        setattr(args, key, value.resolve())
    args.out.mkdir(parents=True, exist_ok=True)
    fixtures = args.root / "tests/python/slice_source/fixtures"
    log = []
    hashes = {}
    summary = {"status": "failed", "python": None, "positive": {},
               "panics": {}, "negative": {}, "commands": log,
               "hashes": hashes}

    version = command([args.python, "--version"], 20, log)
    if version.stdout.strip() != "Python 3.12.15":
        raise RuntimeError(f"Python 3.12.15 required: {version.stdout}")
    summary["python"] = version.stdout.strip()

    try:
        for name in POSITIVE:
            source = fixtures / name / "lang.toml"
            if not source.exists():
                raise RuntimeError(f"missing fixture {name}")
            out = args.out / "positive" / name
            out.mkdir(parents=True, exist_ok=True)
            generated = build(source, out, args.timeout, log,
                              args.cli, args.helper)
            observed = run_observer(args.python, args.root, out, False,
                                    args.timeout, log)
            check_positive(name, observed)
            summary["positive"][name] = {
                "generated": generated,
                "counts": observed["counts"],
                "stats": observed["stats"],
                "identity_regressions": observed["identity_regressions"],
                "blank_to_vec_typed": observed["blank_to_vec_typed"]}
            for path in sorted((fixtures / name / "src").glob("*.wz")):
                hashes[str(path.relative_to(args.root))] = digest(path)
            for generated_name in ("program.py", "langrt.py"):
                path = out / "cli" / generated_name
                hashes[str(path.relative_to(args.out))] = digest(path)

        for name, message in PANIC.items():
            out = args.out / "panic" / name
            out.mkdir(parents=True, exist_ok=True)
            generated = build(fixtures / name / "lang.toml", out,
                              args.timeout, log, args.cli, args.helper)
            observed = run_observer(args.python, args.root, out, True,
                                    args.timeout, log)
            check_panic(name, observed, message)
            summary["panics"][name] = {"generated": generated,
                                       "message": message,
                                       "counts": observed["counts"]}
            for path in sorted((fixtures / name / "src").glob("*.wz")):
                hashes[str(path.relative_to(args.root))] = digest(path)

        for name, code in NEGATIVE.items():
            manifest = fixtures / name / "lang.toml"
            failed = command(
                [args.cli, "build", "--project", manifest, "--backend",
                 "python", "--out-dir", args.out / "negative" / name,
                 "--format", "json"], args.timeout, log)
            document = payload(failed)
            codes = [item.get("code") for item in document.get("diagnostics", [])]
            if failed.returncode == 0 or code not in codes:
                raise RuntimeError(f"negative {name}: expected {code}, got {codes}")
            if (args.out / "negative" / name / "program.py").exists():
                raise RuntimeError(f"negative {name}: generated a program")
            helper_failed = command(
                [args.helper, "--project", manifest, "--out-dir",
                 args.out / "negative" / name / "helper"], args.timeout, log)
            helper_document = payload(helper_failed)
            helper_codes = [item.get("code") for item in
                            helper_document.get("diagnostics", [])]
            if helper_failed.returncode == 0 or code not in helper_codes:
                raise RuntimeError(
                    f"negative {name}: helper retained no capability gate")
            summary["negative"][name] = {"code": code,
                                         "codes": codes,
                                         "helper_codes": helper_codes}

        for path in sorted((args.root / "stdlib/slice").glob("*.wz")):
            hashes[str(path.relative_to(args.root))] = digest(path)
        summary["status"] = "passed"
        return 0
    except Exception as error:
        summary["error"] = str(error)
        print(f"python slice source test failed: {error}", file=sys.stderr)
        return 1
    finally:
        (args.out / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
