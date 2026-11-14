#!/usr/bin/env python3
"""Check, build and run the LANG03 owning (non-POD) inout fixture set.

Accepted fixtures must pass `check` on Native and Python, build with the Native
backend at O0 and O2, run with the expected main return, and produce a static
x86_64 ELF without PT_INTERP or DT_NEEDED. The Python backend must build and
execute the same source with the same observable result. Rejected fixtures must
fail both frontends with the recorded source diagnostic and leave no program.

Every accepted case encodes its own drop/identity observation in the program
exit status: each replaced owner is released exactly once (a Weak handle turns
dead), shared identities stay alive while another copy exists, and sibling
owning fields keep their values.
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


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv, timeout=120):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONNOUSERSITE="1")
    env.pop("PYTHONPATH", None)
    completed = subprocess.run(
        argv, capture_output=True, text=True, timeout=timeout, env=env
    )
    return {
        "argv": argv,
        "exit": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def diagnostics(row):
    try:
        return json.loads(row["stdout"]).get("diagnostics", [])
    except (json.JSONDecodeError, TypeError):
        return []


def require_ok(row, what):
    if row["exit"] != 0:
        raise RuntimeError(f"{what} failed: {row}")


def require_gnu_readelf(text, what):
    """Fail closed unless the tool really produced GNU readelf -l output.

    llvm-readelf chooses GNU output from argv[0].  Calling the resolved
    llvm-readobj binary still exits 0, but its own format contains neither
    INTERP nor NEEDED, so the negative checks below would pass vacuously.
    """
    if "Elf file type is" not in text or "Program Headers:" not in text:
        raise RuntimeError(
            f"{what}: expected GNU readelf output ('Elf file type is', "
            "'Program Headers:'); invoke llvm-readelf (argv[0] selects GNU "
            "mode), not llvm-readobj")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--readelf", type=Path)
    args = parser.parse_args()
    args.cli = args.cli.resolve()
    args.root = args.root.resolve()
    args.python = args.python.resolve()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    readelf = args.readelf or shutil.which("llvm-readelf") or shutil.which("readelf")
    report = {
        "task": "LANG03 owning inout",
        "contract": "compiler-dev/1",
        "status": "failed",
        "tools": {
            "cli": {"path": str(args.cli), "sha256": digest(args.cli)},
            "python": {
                "path": str(args.python),
                "version": run([args.python, "--version"])["stdout"].strip(),
            },
            "readelf": str(readelf) if readelf else None,
        },
        "cases": [],
    }

    # Direct runtime coverage for _rt.replace_place: every real place shape
    # (Owner, Cell, Field-over-Owner, Field-over-Cell, Index, Field-over-Index)
    # must publish first, release the replaced value exactly once, and keep the
    # new owner at refs=1. Generated Source programs cannot reach every shape,
    # so this minimal unit check always runs with the fixture suite.
    places = run(
        [
            args.python,
            "-s",
            args.root / "tests/owning_inout/python_places_test.py",
            "--runtime",
            args.root / "runtime/python/langrt.py",
            "--format",
            "json",
        ]
    )
    report["places"] = places
    require_ok(places, "runtime replace_place unit check")
    place_body = json.loads(places["stdout"])
    assert place_body["status"] == "passed", places
    assert {entry["kind"] for entry in place_body["places"]} == {
        "Owner", "Cell", "FieldOwner", "FieldCell", "Index", "FieldIndex",
        "OwnerInactive",
    }, place_body

    catalog = json.loads((args.root / "tests/owning_inout/cases.json").read_text())
    try:
        for case in catalog["cases"]:
            row = {
                "id": case["id"],
                "kind": case["kind"],
                "status": "failed",
                "checks": {},
                "builds": [],
            }
            report["cases"].append(row)
            manifest = args.root / case["manifest"]
            project = manifest.parent
            row["source_sha256"] = {
                str(path.relative_to(project)): digest(path)
                for path in sorted(project.rglob("*"))
                if path.is_file()
            }

            for backend in ("native", "python"):
                check = run(
                    [
                        args.cli,
                        "check",
                        "--project",
                        manifest,
                        "--backend",
                        backend,
                        "--format",
                        "json",
                    ]
                )
                row["checks"][backend] = check
                if case["kind"] == "accept":
                    require_ok(check, f"{case['id']} {backend} check")
                    body = json.loads(check["stdout"])
                    assert body["status"] == "ok", check
                else:
                    assert check["exit"] == 1, (case["id"], backend, check)
                    codes = [item.get("code") for item in diagnostics(check)]
                    assert case["expected_code"] in codes, (
                        case["id"],
                        backend,
                        codes,
                        check,
                    )

            destination = args.out / case["id"]
            shutil.rmtree(destination, ignore_errors=True)
            if case["kind"] == "reject":
                for backend in ("native", "python"):
                    fresh = destination / f"{backend}-reject"
                    argv = [
                        args.cli,
                        "build",
                        "--project",
                        manifest,
                        "--backend",
                        backend,
                        "--out-dir",
                        fresh,
                        "--format",
                        "json",
                    ]
                    if backend == "native":
                        argv += ["--opt", "0"]
                    build = run(argv)
                    row["builds"].append({"backend": backend, "build": build})
                    assert build["exit"] == 1, (case["id"], backend, build)
                    codes = [item.get("code") for item in diagnostics(build)]
                    assert case["expected_code"] in codes, (
                        case["id"],
                        backend,
                        codes,
                        build,
                    )
                    outputs = ["program", "program.o", "program.py", "langrt.py"]
                    assert not any((fresh / name).exists() for name in outputs), (
                        case["id"],
                        backend,
                        "rejected build left outputs",
                    )
                row["status"] = "passed"
                continue

            expected = case.get("expected_main_return", 0)
            for opt in (0, 2):
                native = destination / f"native-O{opt}"
                build = run(
                    [
                        args.cli,
                        "build",
                        "--project",
                        manifest,
                        "--backend",
                        "native",
                        "--opt",
                        opt,
                        "--out-dir",
                        native,
                        "--format",
                        "json",
                    ]
                )
                row["builds"].append({"backend": "native", "opt": opt, "build": build})
                require_ok(build, f"{case['id']} native O{opt} build")
                assert json.loads(build["stdout"])["status"] == "ok", build
                executed = run([native / "program"])
                require_ok(executed, f"{case['id']} native O{opt} execute")
                assert executed["exit"] == expected, (case["id"], opt, executed)
                assert not executed["stdout"] and not executed["stderr"], executed
                data = (native / "program").read_bytes()
                assert data[:4] == b"\x7fELF" and data[4] == 2 and data[18:20] == b"\x3e\x00", (
                    case["id"],
                    "program is not ELF64 x86_64",
                )
                assert (native / "program.o").read_bytes()[:4] == b"\x7fELF", (
                    case["id"],
                    "object is not ELF",
                )
                assert readelf, "readelf is required for freestanding ELF checks"
                elf = run([readelf, "-l", "-d", native / "program"])
                require_ok(elf, f"{case['id']} readelf")
                text = elf["stdout"]
                require_gnu_readelf(text, f"{case['id']} native O{opt} readelf")
                assert "INTERP" not in text, (case["id"], text)
                assert "NEEDED" not in text, (case["id"], text)

            python = destination / "python"
            build = run(
                [
                    args.cli,
                    "build",
                    "--project",
                    manifest,
                    "--backend",
                    "python",
                    "--out-dir",
                    python,
                    "--format",
                    "json",
                ]
            )
            row["builds"].append({"backend": "python", "build": build})
            require_ok(build, f"{case['id']} python build")
            assert json.loads(build["stdout"])["status"] == "ok", build
            executed = run([args.python, "-s", python / "program.py"])
            require_ok(executed, f"{case['id']} python execute")
            assert executed["exit"] == expected, (case["id"], "python", executed)
            assert not executed["stdout"] and not executed["stderr"], executed
            row["status"] = "passed"

        report["status"] = "passed"
    finally:
        (args.out / "results.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n"
        )
    failed = [case["id"] for case in report["cases"] if case["status"] != "passed"]
    print(
        json.dumps(
            {
                "status": report["status"],
                "cases": len(report["cases"]),
                "places": place_body["status"],
                "failed": failed,
                "report": str(args.out / "results.json"),
            }
        )
    )
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
