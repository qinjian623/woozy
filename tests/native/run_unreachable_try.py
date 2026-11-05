#!/usr/bin/env python3
"""Native execution, Python control and runtime allocation observation for the
unreachable-try slot cleanup regression."""
import argparse
import hashlib
import json
import os
import subprocess
import traceback
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    for key in ("cli", "probe", "root", "python", "cxx", "runtime", "out"):
        parser.add_argument("--" + key, type=Path, required=True)
    parser.add_argument("--case", action="append")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = {"status": "failed", "runs": [], "commands": [], "probe": None}
    fixture_root = args.root / "tests/native/unreachable-try"
    cases = json.loads((fixture_root / "cases.json").read_text())["cases"]
    if args.case:
        cases = [c for c in cases if c["id"] in args.case]
        assert len(cases) == len(set(args.case)), "unknown case"

    def command(argv):
        argv = list(map(str, argv))
        result = subprocess.run(argv, capture_output=True, text=True, timeout=90)
        row = {
            "argv": argv,
            "cwd": os.getcwd(),
            "exit": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
        }
        report["commands"].append(row)
        return row

    def ok(row):
        assert row["exit"] == 0, row

    probe = command([args.probe, args.root, args.out / "probe"])
    ok(probe)
    report["probe"] = json.loads(probe["stdout"])

    for case in cases:
        ident = case["id"]
        manifest = fixture_root / ident / "lang.toml"
        row = {
            "id": ident,
            "status": "failed",
            "source_hashes": {
                str(p.relative_to(args.root)): digest(p)
                for p in sorted(manifest.parent.rglob("*"))
                if p.is_file()
            },
        }
        report["runs"].append(row)
        try:
            for optimization in (0, 2):
                native = args.out / ident / f"O{optimization}"
                native.mkdir(parents=True, exist_ok=True)
                build = command(
                    [
                        args.cli,
                        "build",
                        "--project",
                        manifest,
                        "--backend",
                        "native",
                        "--opt",
                        optimization,
                        "--out-dir",
                        native / "cli",
                        "--format",
                        "json",
                    ]
                )
                row[f"build-O{optimization}"] = build
                ok(build)
                assert json.loads(build["stdout"])["status"] == "ok", build
                execute = command([native / "cli/program"])
                row[f"execute-O{optimization}"] = execute
                assert execute["exit"] == case["main_return"], execute
                assert not execute["stdout"] and not execute["stderr"], execute

                observe = command(
                    [
                        args.cxx,
                        f"-O{optimization}",
                        args.out / "probe" / ident
                        / f"program-O{optimization}.ll",
                        args.runtime,
                        args.root / "tests/native/vec_observer.cpp",
                        "-o",
                        native / "observe",
                    ]
                )
                row[f"observe-build-O{optimization}"] = observe
                ok(observe)
                observed = command([native / "observe"])
                row[f"observe-O{optimization}"] = observed
                assert observed["exit"] == case["main_return"], observed
                assert not observed["stdout"], observed
                live, allocations, frees, drops = {}, [], [], {}
                for line in observed["stderr"].splitlines():
                    assert line.startswith("VEC:"), line
                    _, event, identity, detail = line.split(":")
                    event, detail = int(event), int(detail)
                    if event == 1:
                        assert identity not in live, line
                        live[identity] = detail
                        allocations.append([identity, detail])
                        drops[identity] = []
                    elif event == 5:
                        assert identity in live, line
                        length = live.pop(identity)
                        assert drops[identity] == list(range(length - 1, -1, -1)), (
                            "element drop order",
                            drops[identity],
                            length,
                        )
                        frees.append([identity, length])
                    elif event == 4:
                        assert identity in live, line
                        drops[identity].append(detail)
                    else:
                        assert identity in live, line
                assert [length for _, length in allocations] == case["allocations"], (
                    "executed Vec allocations",
                    allocations,
                )
                assert len(frees) == len(allocations) and not live, (frees, live)
                row[f"observations-O{optimization}"] = {
                    "allocations": allocations,
                    "frees": frees,
                    "live": live,
                }

            python_out = args.out / ident / "python"
            python_out.mkdir(parents=True, exist_ok=True)
            python_build = command(
                [
                    args.cli,
                    "build",
                    "--project",
                    manifest,
                    "--backend",
                    "python",
                    "--out-dir",
                    python_out,
                    "--format",
                    "json",
                ]
            )
            row["python-build"] = python_build
            ok(python_build)
            assert json.loads(python_build["stdout"])["status"] == "ok", python_build
            python_run = command([args.python, "-s", python_out / "program.py"])
            row["python-run"] = python_run
            assert python_run["exit"] == case["main_return"], python_run
            assert not python_run["stdout"] and not python_run["stderr"], python_run

            row["hashes"] = {
                str(p.relative_to(args.out)): digest(p)
                for p in sorted((args.out / ident).rglob("*"))
                if p.is_file()
            }
            row["status"] = "passed"
        except Exception as error:
            row["failure"] = str(error)
            row["traceback"] = traceback.format_exc()
            break

    report["passed"] = sum(run["status"] == "passed" for run in report["runs"])
    report["failed"] = len(report["runs"]) - report["passed"]
    report["status"] = "passed" if report["failed"] == 0 else "failed"
    (args.out / "summary.json").write_text(json.dumps(report, indent=2))
    print(
        json.dumps(
            {
                "status": report["status"],
                "passed": report["passed"],
                "failed": report["failed"],
                "failures": [
                    {"id": run["id"], "failure": run.get("failure")}
                    for run in report["runs"]
                    if run["status"] != "passed"
                ],
            }
        )
    )
    return report["status"] != "passed"


if __name__ == "__main__":
    raise SystemExit(main())
