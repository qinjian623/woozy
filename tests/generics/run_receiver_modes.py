#!/usr/bin/env python3
"""Real receiver-mode builds, executions, ARC observations and output rejection."""

import argparse
import json
from pathlib import Path
import shutil
import sys

sys.dont_write_bytecode = True
from receiver_mode_cases import REJECTS
from run_unbound_methods import (
    run,
    ok,
    digest,
    diagnostic_codes,
    parse_observer,
    elf_checks,
    source_hashes,
)

EXPECTED = {
    "core": dict(
        vec=11,
        events=[2, 3, 7, 9, 4, 5],
        native_events=[(2, 2), (3, 23), (7, 237), (9, 2379), (4, 4), (5, 45)],
    ),
    "inout": dict(
        vec=1,
        events=[3, 7, 9, 4, 5, 6, 5],
        native_events=[(3, 3), (7, 7), (9, 79), (4, 4), (5, 45), (6, 6), (5, 65)],
    ),
    "inherent": dict(vec=2, events=[], native_events=[]),
    "full-trait": dict(vec=1, events=[], native_events=[]),
    "owning-move-borrow": dict(vec=2, events=[1, 2, 3], native_events=[(1, 1), (2, 12), (3, 123)]),
}


def main():
    parser = argparse.ArgumentParser()
    for name in ("cli", "probe", "helper", "root", "clang", "runtime", "python", "out"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    for name in vars(args):
        setattr(args, name, getattr(args, name).resolve())
    args.out.mkdir(parents=True, exist_ok=True)
    library = args.clang.parent.parent.parent / "libxml2-legacy-2.13.9/usr/lib"
    report = dict(
        status="failed",
        rows=[],
        tools={str(p): digest(p) for p in (args.cli, args.helper, args.probe, args.runtime)},
    )

    def command(row, key, argv):
        result = run(argv, library=library)
        row[key] = result
        return result

    def row(case, kind, **extra):
        result = dict(case=case, kind=kind, status="failed", **extra)
        report["rows"].append(result)
        return result

    try:
        version = run([args.python, "--version"])
        ok(version, "fixed Python")
        assert version["stdout"].strip() == "Python 3.12.15", version
        for case, wanted in EXPECTED.items():
            manifest = args.root / "tests/generics/receiver-modes" / case / "lang.toml"
            hashes = source_hashes(manifest.parent)
            for backend in ("native", "python"):
                r = row(case, "check", backend=backend, source_sha256=hashes)
                checked = command(
                    r,
                    "check",
                    [
                        args.cli,
                        "check",
                        "--project",
                        manifest,
                        "--backend",
                        backend,
                        "--format",
                        "json",
                    ],
                )
                ok(checked, "check")
                r["status"] = "passed"
            for opt in (0, 2):
                r = row(case, "native", opt=opt, source_sha256=hashes)
                target = args.out / case / f"O{opt}"
                shutil.rmtree(target, ignore_errors=True)
                ok(
                    command(
                        r,
                        "build",
                        [
                            args.cli,
                            "build",
                            "--project",
                            manifest,
                            "--backend",
                            "native",
                            "--opt",
                            opt,
                            "--format",
                            "json",
                            "--out-dir",
                            target / "cli",
                        ],
                    ),
                    "native build",
                )
                executed = command(r, "execute", [target / "cli/program"])
                ok(executed, "native execute")
                assert not executed["stdout"] and not executed["stderr"], executed
                r["elf"] = elf_checks(target / "cli/program")
                ok(
                    command(r, "probe", [args.probe, manifest, target / "probe", opt]),
                    "lower/probe",
                )
                ok(
                    command(
                        r,
                        "observe_build",
                        [
                            args.clang,
                            f"-O{opt}",
                            target / "probe/observe.ll",
                            args.root / "tests/native/vec_observer.cpp",
                            args.root / "tests/native/alloc_observer.cpp",
                            args.runtime,
                            "-Wl,--wrap=lang_alloc_v1",
                            "-Wl,--wrap=lang_free_v1",
                            "-o",
                            target / "observe",
                        ],
                    ),
                    "observer build",
                )
                observed = command(r, "observe", [target / "observe"])
                ok(observed, "observer execution")
                data = parse_observer(observed["stderr"])
                r["observation"] = data
                assert not data["live"] and not data["vec_live"], data
                assert len(data["vec_allocations"]) == len(data["vec_frees"]) == wanted["vec"], data
                assert len(data["allocations"]) == len(data["frees"]) == wanted["vec"], data
                assert data["events"] == wanted["native_events"], data
                if case == "core":
                    sequence = data["sequence"]
                    for a, b, vecs in ((7, 9, 1), (4, 5, 2)):
                        start = next(
                            i for i, x in enumerate(sequence) if x[0] == "obs" and x[1] == a
                        )
                        end = next(i for i, x in enumerate(sequence) if x[0] == "obs" and x[1] == b)
                        middle = sequence[start + 1 : end]
                        assert sum(x[0] == "vec_free" for x in middle) == vecs, middle
                        assert sum(x[0] == "free" for x in middle) == vecs, middle
                if case == "owning-move-borrow":
                    sequence = data["sequence"]
                    first = next(i for i, x in enumerate(sequence) if x[0] == "obs" and x[1] == 1)
                    bad = next(i for i, x in enumerate(sequence) if x[0] == "obs" and x[1] == 2)
                    catch = next(i for i, x in enumerate(sequence) if x[0] == "obs" and x[1] == 3)
                    assert [x for x in sequence[:first] if x[0] == "vec_free"] == [
                        ("vec_free", 0)
                    ], sequence
                    assert [x for x in sequence[bad + 1 : catch] if x[0] == "vec_free"] == [
                        ("vec_free", 1)
                    ], sequence
                    assert sum(x[0] == "free" for x in sequence[bad + 1 : catch]) == 1, sequence
                r["status"] = "passed"
            r = row(case, "python", source_sha256=hashes)
            target = args.out / case / "python"
            shutil.rmtree(target, ignore_errors=True)
            ok(
                command(
                    r,
                    "build",
                    [
                        args.cli,
                        "build",
                        "--project",
                        manifest,
                        "--backend",
                        "python",
                        "--format",
                        "json",
                        "--out-dir",
                        target,
                    ],
                ),
                "Python build",
            )
            ok(
                command(
                    r,
                    "compile",
                    [
                        args.python,
                        "-B",
                        "-s",
                        "-c",
                        'from pathlib import Path;import sys;[compile(p.read_bytes(),str(p),"exec") for p in Path(sys.argv[1]).glob("*.py")]',
                        target,
                    ],
                ),
                "whole module compile",
            )
            executed = command(r, "execute", [args.python, "-B", "-s", target / "program.py"])
            ok(executed, "Python execution")
            assert not executed["stdout"] and not executed["stderr"], executed
            metadata = target.with_name("metadata")
            ok(
                command(r, "emit", [args.helper, "--project", manifest, "--out-dir", metadata]),
                "metadata helper",
            )
            assert all(
                digest(target / name) == digest(metadata / name)
                for name in ("program.py", "langrt.py")
            ), "CLI/helper differ"
            shutil.copyfile(metadata / "metadata.json", target / "metadata.json")
            observed = command(
                r,
                "observe",
                [
                    args.python,
                    "-B",
                    "-s",
                    args.root
                    / (
                        "tests/generics/observe_receiver_modes.py"
                        if case == "owning-move-borrow"
                        else "tests/python/observe_ownership.py"
                    ),
                    target,
                ],
            )
            ok(observed, "Python owner observer")
            data = json.loads(observed["stdout"])
            r["observation"] = data
            assert data["main_return"] == 0 and not data["alive"] and data["frames_clean"] > 0, data
            assert data["events"] == wanted["events"], data
            assert len(data["allocations"]) == len(data["frees"]) == wanted["vec"], data
            if case == "core":
                operations = data["operations"]
                a = operations.index(["event", 7])
                b = operations.index(["event", 9])
                assert [x for x in operations[a + 1 : b] if x[0] == "free"] == [["free", 8]], (
                    operations
                )
                a = operations.index(["event", 4])
                b = operations.index(["event", 5])
                assert [x for x in operations[a + 1 : b] if x[0] == "free"] == [
                    ["free", 10],
                    ["free", 9],
                ], operations
            if case == "owning-move-borrow":
                operations = data["operations"]
                first = operations.index(["event", 1])
                bad, catch = operations.index(["event", 2]), operations.index(["event", 3])
                assert operations.index(["free", 0]) < first, operations
                assert bad < operations.index(["free", 1]) < catch, operations
                calls = data["borrow_calls"]
                assert (
                    sum(x[0] == "begin" for x in calls) == sum(x[0] == "end" for x in calls) == 2
                ), calls
                assert data["active_borrows"] == 0, data
            r["status"] = "passed"
        for case, (source, code) in REJECTS.items():
            project = args.out / "reject-sources" / case
            (project / "src").mkdir(parents=True, exist_ok=True)
            (project / "src/main.wz").write_text("package app;" + source + "\n")
            manifest = project / "lang.toml"
            manifest.write_text(
                'schema_version=1\n[package]\nname="app"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n'
            )
            for backend in ("native", "python"):
                for kind in ("check", "fresh", "stale"):
                    r = row(case, kind, backend=backend, source_sha256=source_hashes(project))
                    argv = [
                        args.cli,
                        "check" if kind == "check" else "build",
                        "--project",
                        manifest,
                        "--backend",
                        backend,
                        "--format",
                        "json",
                    ]
                    output = args.out / "rejects" / case / (backend + "-" + kind)
                    before = None
                    if kind != "check":
                        shutil.rmtree(output, ignore_errors=True)
                        argv += ["--out-dir", output]
                        if kind == "stale":
                            output.mkdir(parents=True)
                            for name in (
                                "program",
                                "program.o",
                                "program.py",
                                "langrt.py",
                                "unrelated",
                            ):
                                (output / name).write_text("keep " + name)
                            before = {p.name: digest(p) for p in output.iterdir()}
                            r["before"] = before
                    result = command(r, "command", argv)
                    assert result["exit"] == 1 and diagnostic_codes(result) == {code}, (
                        case,
                        code,
                        result,
                    )
                    if kind == "fresh":
                        assert not output.exists() or not list(output.rglob("*")), output
                    if kind == "stale":
                        r["after"] = {p.name: digest(p) for p in output.iterdir()}
                        assert r["after"] == before, r
                    r["status"] = "passed"
        report["status"] = "passed"
    except Exception as error:
        report["error"] = repr(error)
        raise
    finally:
        (args.out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            dict(
                status=report["status"],
                records=len(report["rows"]),
                formal_runs=3 * len(EXPECTED),
                observer_runs=3 * len(EXPECTED),
                reject_cases=len(REJECTS),
            )
        )
    )


if __name__ == "__main__":
    main()
