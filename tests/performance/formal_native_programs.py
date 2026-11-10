"""Build one preserved O2 Source program for a fixed formal workload.

The two program builders in ``formal_sort_programs`` and
``formal_algorithm_programs`` only freeze rendered Source/C++ inputs and build
the C++ side.  This module adds the shared Source build path needed after core
completion: it calls the builder's existing ``render(R)`` once, uses the
original manifest/source, and writes only ``r{R}/source-O2``.

This is an artifact builder.  It does not execute the generated program, review
the actual R/2R work, freeze the final production tools, verify an idle host, or
start calibration/sampling.  Its sidecar therefore keeps
``actual_work_review=pending`` and ``formal_timing=false``.
"""
from pathlib import Path
import re
from types import SimpleNamespace

import formal_algorithm_programs
import formal_sampling
import formal_sort_programs

common = formal_sort_programs.common
SCHEMA = "compiler-dev/formal-native-source-build/1"
OPERATION_ID = "language-formal-native-program-artifacts-20261110"
TARGET = "x86_64-unknown-linux-gnu"
TIMEOUT = 600
LLVM_TOOLS = ("clang", "clang++", "ld.lld", "opt", "llvm-readelf", "llvm-nm")


def _file_record(path, base=None):
    path = Path(path).resolve()
    record = {"path": str(path), "sha256": common.sha256_file(path),
              "size_bytes": path.stat().st_size}
    if base is not None:
        record["rel"] = str(path.relative_to(Path(base).resolve()))
    return record


def _require_supported(programs, rounds):
    if not isinstance(programs,
                      (formal_sort_programs.SortPrograms,
                       formal_algorithm_programs.AlgorithmPrograms)):
        raise common.PerfError(
            "build_source requires a SortPrograms or AlgorithmPrograms object")
    if (programs.algorithm, programs.n) not in formal_sampling.GRID:
        raise common.PerfError(
            "only the fixed formal GRID algorithms and sizes are supported")
    if (type(rounds) is not int or rounds < 1
            or rounds > 2 * formal_sampling.MAX_ROUNDS
            or rounds & (rounds - 1)):
        raise common.PerfError("R must be a supported power of two")


def _tree(langc, helper, runtime, llvm_bin, env, commands=None):
    args = SimpleNamespace(langc=Path(langc), helper=Path(helper),
                           runtime=Path(runtime), llvm_bin=Path(llvm_bin),
                           cpu=None)
    # build_tree_info probes actual tools with version/help commands.  Keep
    # those commands with the build, as run_native's own reports do.
    previous = common._COMMAND_LOG
    if commands is not None:
        common._COMMAND_LOG = commands
    try:
        return common.build_tree_info(args, env, None)
    finally:
        common._COMMAND_LOG = previous


def _hash_paths(paths):
    result = {}
    for name, path in paths.items():
        path = Path(path).resolve()
        if not path.is_file():
            raise common.PerfError(f"missing frozen tool/metadata: {path}")
        result[name] = {"path": str(path),
                        "sha256": common.sha256_file(path)}
    return result


def _paths(tree, langc, llvm_bin):
    defines = tree["defines"]
    paths = {
        "langc": Path(langc),
        "helper": Path(defines["LANG_NATIVE_HELPER"]),
        "runtime": Path(defines["LANG_NATIVE_RUNTIME"]),
        "clang": Path(defines["LANG_NATIVE_CLANG"]),
        "ld.lld": Path(defines["LANG_NATIVE_LLD"]),
    }
    for name in LLVM_TOOLS:
        paths.setdefault(name, Path(llvm_bin) / name)
    return paths


def _metadata_paths(tree):
    return {
        "build.ninja": Path(tree["build_ninja"]["path"]),
        "CMakeCache.txt": Path(tree["cmake_cache"]["path"]),
    }


def _parse_undefined(text):
    strong, weak = [], []
    for line in text.splitlines():
        match = re.match(r"^\s*([A-Za-z])\s+(\S+)\s*$", line)
        if match is None:
            continue
        kind, symbol = match.group(1), match.group(2)
        if kind == "U":
            strong.append(symbol)
        elif kind in ("w", "v"):
            weak.append(symbol)
    return strong, weak


def _static_elf(program, llvm_bin, run, label):
    elf = common.checked_elf(program, executable=True, label=label)
    read_l = run([Path(llvm_bin) / "llvm-readelf", "-l", program],
                 f"{label} llvm-readelf -l")
    read_d = run([Path(llvm_bin) / "llvm-readelf", "-d", program],
                 f"{label} llvm-readelf -d")
    nm_u = run([Path(llvm_bin) / "llvm-nm", "-u", program],
               f"{label} llvm-nm -u")
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]",
                        read_d["stdout"])
    interp = re.search(r"Requesting program interpreter: ([^\]]+)",
                       read_l["stdout"])
    strong, weak = _parse_undefined(nm_u["stdout"])
    row = {
        "elf": elf,
        "readelf_l": read_l["argv"],
        "readelf_d": read_d["argv"],
        "nm_u": nm_u["argv"],
        "interp": interp.group(1) if interp else None,
        "needed": needed,
        "strong_undefined": strong,
        "weak_undefined": weak,
    }
    if row["interp"] is not None:
        raise common.PerfError(f"{label}: Source ELF has INTERP {row['interp']}")
    if needed:
        raise common.PerfError(f"{label}: Source ELF has NEEDED {needed}")
    if strong:
        raise common.PerfError(
            f"{label}: Source ELF has strong undefined symbols {strong}")
    row["source_static_ok"] = True
    return row


def build_source(programs, rounds, langc, helper, runtime, llvm_bin, env):
    """Compile one Source manifest to O2 and return its real program path.

    The caller must pass the frozen Release ``woozyc``, its embedded helper and
    runtime, and the matching LLVM tool directory.  The returned path is the
    ELF linked by public ``woozyc build``; the helper object must match
    ``program.o`` byte-for-byte.  All other artifacts and every command are in
    ``r{R}/source-O2/build.json``.
    """
    _require_supported(programs, rounds)
    out = Path(programs.out).resolve()
    round_root = out / f"r{rounds}"
    directory = round_root / "source-O2"
    if directory.exists():
        raise common.PerfError(
            f"Source O2 output already exists; refusing to overwrite: {directory}")

    programs.unchanged()
    case = programs.render(rounds)
    expected = {}
    for key in ("source", "reference", "manifest"):
        info = case[key]
        path = Path(info["path"]).resolve()
        if round_root not in path.parents:
            raise common.PerfError(
                f"R{rounds} {key} is not under its own round directory: {path}")
        if not path.is_file() or common.sha256_file(path) != info["sha256"]:
            raise common.PerfError(f"R{rounds} {key} hash changed: {path}")
        expected[key] = {"path": str(path), "sha256": info["sha256"],
                         "size_bytes": path.stat().st_size}

    directory.mkdir(parents=True, exist_ok=False)
    langc_dir = directory / "langc"
    llvm_dir = directory / "llvm"
    llvm_dir.mkdir(exist_ok=False)
    row = {
        "schema": SCHEMA,
        "contract": "compiler-dev/1",
        "operation_id": OPERATION_ID,
        "algorithm": programs.algorithm,
        "n": programs.n,
        "rounds": rounds,
        "variant": "correct",
        "target": {"triple": TARGET, "optimization": 2},
        "directory": str(directory),
        "source_case": expected,
        "commands": [],
        "status": "running",
        "actual_work_review": "pending",
        "formal_timing": False,
        "scope": {
            "core_completion_required": True,
            "freeze_required": True,
            "idle_host_required": True,
            "executed_program": False,
            "calibration_started": False,
            "sampling_started": False,
        },
        "limitations": [
            "Builds one O2 Source artifact; it does not execute the program.",
            "actual_work_review remains pending; R/2R work is not audited here.",
            "Does not replace core completion, final tool freeze, or idle-host checks.",
            "Not a formal timing or performance result.",
        ],
    }

    def save():
        common.write_json(directory / "build.json", row)

    def run(argv, label):
        result = common.command([str(value) for value in argv],
                                timeout=TIMEOUT, env=env)
        row["commands"].append(result)
        save()
        common.require_success(result, label)
        return result

    save()
    try:
        tree_before = _tree(langc, helper, runtime, llvm_bin, env,
                            row["commands"])
        row["toolchain_before"] = tree_before
        before_paths = _paths(tree_before, langc, llvm_bin)
        before_meta = _metadata_paths(tree_before)
        row["hashes"] = {
            "before": _hash_paths({**before_paths, **before_meta}),
            "after": None, "unchanged": None,
        }
        save()

        manifest = expected["manifest"]["path"]
        check = run([Path(langc), "check", "--project", manifest,
                     "--backend", "native", "--format", "json"],
                    f"Source R{rounds} langc check")
        check_json = common.parse_json_stdout(
            check, f"Source R{rounds} langc check")
        if check_json.get("status") != "ok":
            raise common.PerfError(
                f"Source R{rounds} langc check status is not ok: {check_json}")
        row["check"] = check_json

        langc_dir.mkdir(exist_ok=False)
        build = run([Path(langc), "build", "--project", manifest,
                     "--backend", "native", "--opt", "2",
                     "--out-dir", langc_dir, "--format", "json"],
                    f"Source R{rounds} langc O2 build")
        build_json = common.parse_json_stdout(
            build, f"Source R{rounds} langc O2 build")
        if build_json.get("status") != "ok":
            raise common.PerfError(
                f"Source R{rounds} langc O2 build status is not ok: {build_json}")
        row["build"] = build_json
        program = (langc_dir / "program").resolve()
        program_object = (langc_dir / "program.o").resolve()
        if not program.is_file() or not program_object.is_file():
            raise common.PerfError(
                f"Source R{rounds} langc build did not produce program/program.o")
        row["program"] = _file_record(program, directory)
        row["object"] = _file_record(program_object, directory)
        row["object_elf"] = common.checked_elf(
            program_object, executable=False, label=f"Source R{rounds} object")
        save()

        before_ir = llvm_dir / "before.ll"
        after_ir = llvm_dir / "after.ll"
        helper_object = llvm_dir / "program.o"
        native_ir = llvm_dir / "program.nir"
        helper_run = run([Path(helper), "--project", manifest, "--opt", "2",
                          "--out-object", helper_object,
                          "--out-llvm", after_ir,
                          "--out-llvm-before", before_ir,
                          "--out-native", native_ir, "--format", "json"],
                         f"Source R{rounds} helper O2")
        helper_json = common.parse_json_stdout(
            helper_run, f"Source R{rounds} helper O2")
        row["helper"] = helper_json
        if helper_json.get("status") not in ("ok", "passed"):
            raise common.PerfError(
                f"Source R{rounds} helper status is not ok/passed: {helper_json}")
        if not (helper_json.get("llvm_verified_before")
                and helper_json.get("llvm_verified_after")):
            raise common.PerfError(
                f"Source R{rounds} helper did not verify both IR forms")
        for path in (helper_object, before_ir, after_ir, native_ir):
            if not Path(path).is_file():
                raise common.PerfError(
                    f"Source R{rounds} helper did not write {path}")
        row["helper_object"] = _file_record(helper_object, directory)
        row["llvm_before"] = _file_record(before_ir, directory)
        row["llvm_after"] = _file_record(after_ir, directory)
        row["native_ir"] = _file_record(native_ir, directory)
        row["object_match"] = (
            helper_object.read_bytes() == program_object.read_bytes())
        if not row["object_match"]:
            raise common.PerfError(
                f"Source R{rounds}: program.o and helper object differ")
        row["object_elf_match"] = common.checked_elf(
            helper_object, executable=False,
            label=f"Source R{rounds} helper object")["sha256"] == row["object_elf"]["sha256"]
        if not row["object_elf_match"]:
            raise common.PerfError(
                f"Source R{rounds}: helper object ELF differs from program.o")
        save()

        row["verify"] = []
        for kind, path in (("before", before_ir), ("after", after_ir)):
            record = run([Path(llvm_bin) / "opt", "-passes=verify",
                          "-disable-output", path],
                         f"Source R{rounds} {kind} IR verify")
            row["verify"].append({"kind": kind,
                                  "path": str(Path(path).resolve()),
                                  "sha256": common.sha256_file(path),
                                  "command_index": len(row["commands"]) - 1})
            save()

        row["static_elf"] = _static_elf(
            program, llvm_bin, run, f"Source R{rounds} O2")
        row["program"]["executable_elf"] = row["static_elf"]["elf"]
        save()

        if programs.algorithm in ("mandelbrot", "matmul"):
            checker_name = ("float_policy" if programs.algorithm == "mandelbrot"
                            else "float_flags")
            checker = getattr(getattr(programs, "generator", None),
                              checker_name, None)
            if checker is None:
                raise common.PerfError(
                    f"{programs.algorithm} generator lacks {checker_name}")
            row["float_policy"] = checker(
                after_ir, f"{programs.algorithm} n={programs.n} R{rounds} Source O2")
        save()

        tree_after = _tree(langc, helper, runtime, llvm_bin, env,
                           row["commands"])
        row["toolchain_after"] = tree_after
        if tree_after.get("defines") != tree_before.get("defines"):
            raise common.PerfError(
                f"Source R{rounds}: build-tree tool definitions changed")
        after_paths = _paths(tree_after, langc, llvm_bin)
        after_meta = _metadata_paths(tree_after)
        after = _hash_paths({**after_paths, **after_meta})
        row["hashes"]["after"] = after
        row["hashes"]["unchanged"] = after == row["hashes"]["before"]
        if not row["hashes"]["unchanged"]:
            changed = sorted(name for name in set(after) | set(row["hashes"]["before"])
                             if after.get(name) != row["hashes"]["before"].get(name))
            raise common.PerfError(
                f"Source R{rounds}: frozen tools or build metadata changed: {changed}")

        programs.unchanged()
        for key, info in expected.items():
            if common.sha256_file(info["path"]) != info["sha256"]:
                raise common.PerfError(
                    f"Source R{rounds}: generated {key} changed during build: "
                    f"{info['path']}")
        row["status"] = "built"
        return program
    except Exception as error:
        row["status"] = "failed"
        row["failure"] = {"type": type(error).__name__, "message": str(error)}
        raise
    finally:
        save()
