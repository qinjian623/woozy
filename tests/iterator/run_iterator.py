#!/usr/bin/env python3
"""Run the LANG04 user-Iterator fixtures on Native and Python.

Accepted fixtures must pass both frontends, run at Native O0/O2 with the real
main return, and execute again under the existing Native Vec allocation
observer. The observer must show every allocation freed and no live object.
Python must run the same source and import it through the existing ownership
observer, which checks live language owners, frames and Vec frees. Rejected
fixtures must fail both frontends with the recorded diagnostic and leave no
program or Python output.
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


def library_dir(cli: Path, root: Path) -> Path | None:
    candidates = []
    for base in (cli.parent, cli.parent.parent, cli.parent.parent.parent,
                 root.parent, root.parent.parent):
        candidates.append(base / "toolchains/libxml2-legacy-2.13.9/usr/lib")
    for entry in os.environ.get("LD_LIBRARY_PATH", "").split(":"):
        if entry and Path(entry).is_dir():
            candidates.append(Path(entry))
    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    return None


def run(argv, timeout=180, library: Path | None = None, cwd: Path | None = None):
    argv = list(map(str, argv))
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONNOUSERSITE="1")
    env.pop("PYTHONPATH", None)
    if library is not None:
        current = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = str(library) + (":" + current if current else "")
    completed = subprocess.run(
        argv, capture_output=True, text=True, timeout=timeout, env=env, cwd=cwd
    )
    return {
        "argv": argv,
        "exit": completed.returncode,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
    }


def require_ok(row, what):
    if row["exit"] != 0:
        raise RuntimeError(f"{what} failed: {row}")


def diagnostics(row):
    try:
        return json.loads(row["stdout"]).get("diagnostics", [])
    except (json.JSONDecodeError, TypeError):
        return []


def native_observations(text):
    allocations = 0
    live = {}
    frees = 0
    for line in text.splitlines():
        fields = line.split(":")
        if fields[0] != "VEC":
            continue
        event = int(fields[1])
        identity = fields[2]
        if event == 1:
            if identity in live:
                raise AssertionError(f"duplicate Vec allocation: {line}")
            live[identity] = allocations
            allocations += 1
        elif event == 5:
            if identity not in live:
                raise AssertionError(f"Vec free without allocation: {line}")
            del live[identity]
            frees += 1
        elif identity not in live:
            raise AssertionError(f"Vec event on unknown object: {line}")
    if live:
        raise AssertionError(f"live Vec objects after main: {live}")
    return {"allocations": allocations, "frees": frees, "live": 0}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--python-emit", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--observer", type=Path)
    parser.add_argument("--readelf", type=Path)
    args = parser.parse_args()
    for name in ("cli", "python_emit", "root", "python", "clang", "runtime", "out"):
        setattr(args, name, getattr(args, name).resolve())
    args.observer = (args.observer or (args.root / "tests/native/vec_observer.cpp")).resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    library = library_dir(args.cli, args.root)
    report = {
        "task": "LANG04 user Iterator",
        "contract": "compiler-dev/1",
        "status": "failed",
        "tools": {
            "cli": {"path": str(args.cli), "sha256": digest(args.cli)},
            "python_emit": {"path": str(args.python_emit), "sha256": digest(args.python_emit)},
            "python": {"path": str(args.python), "version": run([args.python, "--version"], library=library)["stdout"].strip()},
            "clang": {"path": str(args.clang), "version": run([args.clang, "--version"], library=library)["stdout"].strip()},
            "runtime": {"path": str(args.runtime)},
            "observer": {"path": str(args.observer)},
            "library": str(library) if library else None,
        },
        "cases": [],
    }
    catalog = json.loads((args.root / "tests/iterator/cases.json").read_text())
    try:
        for case in catalog["cases"]:
            row = {"id": case["id"], "kind": case["kind"], "status": "failed", "checks": {}, "builds": []}
            report["cases"].append(row)
            manifest = args.root / case["manifest"]
            project = manifest.parent
            row["source_sha256"] = {
                str(path.relative_to(project)): digest(path)
                for path in sorted(project.rglob("*")) if path.is_file()
            }
            for backend in ("native", "python"):
                check = run([args.cli, "check", "--project", manifest,
                             "--backend", backend, "--format", "json"], library=library)
                row["checks"][backend] = check
                if case["kind"] == "accept":
                    require_ok(check, f"{case['id']} {backend} check")
                    assert json.loads(check["stdout"])["status"] == "ok", check
                else:
                    assert check["exit"] == 1, (case["id"], backend, check)
                    codes = [item.get("code") for item in diagnostics(check)]
                    assert case["expected_code"] in codes, (case["id"], backend, codes, check)

            destination = args.out / case["id"]
            shutil.rmtree(destination, ignore_errors=True)
            if case["kind"] == "reject":
                for backend in ("native", "python"):
                    fresh = destination / f"{backend}-reject"
                    argv = [args.cli, "build", "--project", manifest,
                            "--backend", backend, "--out-dir", fresh,
                            "--format", "json"]
                    if backend == "native":
                        argv += ["--opt", "0"]
                    build = run(argv, library=library)
                    row["builds"].append({"backend": backend, "build": build})
                    assert build["exit"] == 1, (case["id"], backend, build)
                    codes = [item.get("code") for item in diagnostics(build)]
                    assert case["expected_code"] in codes, (case["id"], backend, codes, build)
                    outputs = ["program", "program.o", "program.py", "langrt.py", "metadata.json"]
                    assert not any((fresh / name).exists() for name in outputs), (
                        case["id"], backend, "rejected build left outputs")
                row["status"] = "passed"
                continue

            expected = case.get("expected_main_return", 0)
            for opt in (0, 2):
                native = destination / f"native-O{opt}"
                build = run([args.cli, "build", "--project", manifest,
                             "--backend", "native", "--opt", opt,
                             "--out-dir", native, "--format", "json"], library=library)
                row["builds"].append({"backend": "native", "opt": opt, "build": build})
                require_ok(build, f"{case['id']} native O{opt} build")
                assert json.loads(build["stdout"])["status"] == "ok", build
                executed = run([native / "program"], library=library)
                require_ok(executed, f"{case['id']} native O{opt} execute")
                assert executed["exit"] == expected, (case["id"], opt, executed)
                assert not executed["stdout"] and not executed["stderr"], executed
                if args.readelf:
                    elf = run([args.readelf, "-l", "-d", native / "program"], library=library)
                    require_ok(elf, f"{case['id']} readelf")
                    assert "INTERP" not in elf["stdout"], (case["id"], elf)
                    assert "NEEDED" not in elf["stdout"], (case["id"], elf)

                observe_exe = destination / f"observe-O{opt}"
                link = run([args.clang, f"-O{opt}", native / "program.o",
                            args.runtime, args.observer, "-o", observe_exe], library=library)
                row["builds"].append({"backend": "native-observer", "opt": opt, "link": link})
                require_ok(link, f"{case['id']} native O{opt} observer link")
                observed = run([observe_exe], library=library)
                require_ok(observed, f"{case['id']} native O{opt} observer execute")
                assert observed["exit"] == expected, (case["id"], opt, observed)
                metrics = native_observations(observed["stderr"])
                row[f"native_observer_O{opt}"] = metrics
                if case.get("requires_vec_allocations"):
                    assert metrics["allocations"] > 0, (case["id"], opt, metrics)
                assert metrics["frees"] == metrics["allocations"], (case["id"], opt, metrics)

            python = destination / "python"
            build = run([args.cli, "build", "--project", manifest,
                         "--backend", "python", "--out-dir", python,
                         "--format", "json"], library=library)
            row["builds"].append({"backend": "python", "build": build})
            require_ok(build, f"{case['id']} python build")
            assert json.loads(build["stdout"])["status"] == "ok", build
            executed = run([args.python, "-s", python / "program.py"], library=library)
            require_ok(executed, f"{case['id']} python execute")
            assert executed["exit"] == expected, (case["id"], "python", executed)
            assert not executed["stdout"] and not executed["stderr"], executed

            if case.get("python_observer", True):
                emitted = destination / "python-observe"
                emit = run([args.python_emit, "--project", manifest,
                            "--out-dir", emitted], library=library)
                row["builds"].append({"backend": "python-emit", "run": emit})
                require_ok(emit, f"{case['id']} python observer emit")
                observed = run([args.python, "-s", args.root / "tests/python/observe_ownership.py", emitted], library=library)
                require_ok(observed, f"{case['id']} python ownership observer")
                data = json.loads(observed["stdout"])
                row["python_observer"] = data
                assert data["main_return"] == expected, (case["id"], data)
                assert not data["alive"], (case["id"], data)
                assert len(data["frees"]) == len(data["allocations"]), (case["id"], data)
            else:
                row["python_observer"] = {"status": "skipped", "reason": case.get("python_observer_note", "")}
            row["status"] = "passed"

        report["status"] = "passed"
    finally:
        (args.out / "results.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    failed = [case["id"] for case in report["cases"] if case["status"] != "passed"]
    print(json.dumps({"status": report["status"], "cases": len(report["cases"]),
                      "failed": failed, "report": str(args.out / "results.json")}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
