#!/usr/bin/env python3
"""Use a native macOS arm64 Woozy seed to build and compare stages 2 and 3."""

import argparse
import filecmp
from functools import partial
import json
import os
from pathlib import Path
import platform
import re
import resource
import shutil
import sys

from bootstrap import (
    ROOT, SELFHOST, _all_tree_hashes, _required_file, _snapshot_digest, copy_sources, digest,
)
from corpus import _run_process_impl, _timeout_number, load_json, write_json


OBJECT_TARGET = "arm64-apple-macosx11.0.0"
LINK_TARGET = "arm64-apple-macosx14.4"
LLVM_TOOLS = ("opt", "llc", "llvm-ar", "llvm-readobj")


def run_command(argv, *, cwd, env, timeout, prefix=(), log=None):
    result = _run_process_impl([*prefix, *argv], cwd, env, timeout)
    if log is not None:
        log.write_bytes(result["stdout"] + result["stderr"])
    if result["timed_out"] or not result["cleanup_complete"] or result["exit"]:
        reason = f'exit {result["exit"]}'
        if result["timed_out"]:
            reason = "timed out"
        if not result["cleanup_complete"]:
            reason += ", subprocess cleanup incomplete"
        evidence = f"; see {log}" if log is not None else ""
        raise RuntimeError(f"{argv[0]}: {reason}{evidence}")
    return result["stdout"].decode("utf-8")


def require_ok(output, operation):
    result = json.loads(output)
    if not isinstance(result, dict) or result.get("status") != "ok":
        raise RuntimeError(f"{operation} did not report success")


def arm64_members(archive, readobj, command):
    data = command([readobj, "--file-headers", archive])
    formats = re.findall(r"^Format: (.*)$", data, re.M)
    types = re.findall(r"^  FileType: (.*)$", data, re.M)
    if (not formats or formats != ["Mach-O arm64"] * len(formats)
            or types != ["Relocatable (0x1)"] * len(formats)):
        raise RuntimeError(f"expected a nonempty arm64 Mach-O object archive: {archive}")
    return len(formats)


def verify_objects(objects, opt, llvm, command):
    report = load_json(objects / "make-report.json")
    packages = report["packages"]
    if (report.get("schema_version") != 1 or report.get("phase") != "source-make"
            or not packages or report["target"] != OBJECT_TARGET or report["opt"] != opt
            or report["cache_dir"] or any(row["action"] != "compiled" or row["opt_jobs"] != 1
                                          or row["llc_jobs"] != 1 for row in packages)):
        raise RuntimeError(f"incomplete verified source-package report: {objects}")
    archive = objects / "program.a"
    expected = [f"pkg-{row['package']}.o" for row in packages]
    members = command([llvm / "llvm-ar", "t", archive]).splitlines()
    if (len(set(expected)) != len(expected) or sorted(members) != sorted(expected)
            or arm64_members(archive, llvm / "llvm-readobj", command) != len(expected)):
        raise RuntimeError(f"archive members differ from source-package report: {archive}")
    return len(members)


def verify_compiler(compiler, check_project, llvm, command):
    info = command([llvm / "llvm-readobj", "--file-headers", "--needed-libs", compiler])
    if ("\nFormat: Mach-O arm64\n" not in info or "\n  FileType: Executable (0x2)\n" not in info
            or "\nNeededLibraries [\n  /usr/lib/libSystem.B.dylib\n]\n" not in info):
        raise RuntimeError(f"expected an arm64 Mach-O executable linked with libSystem: {compiler}")
    version = command([compiler, "--version"]).strip()
    if not re.fullmatch(r"woozyc \S+-selfhost \(compiler-dev/1\)", version):
        raise RuntimeError(f"not a Woozy-written compiler: {compiler}")
    log = compiler.parent / "check.log"
    require_ok(command([compiler, "check", "--project", check_project, "--format", "json"],
                       log=log), f"compiler check; see {log}")
    return version


def link_compiler(compiler, archive, runtime, clang, sdk, command):
    command([clang, "-nostdlib++", "-target", LINK_TARGET, "-isysroot", sdk, "-Wl,-dead_strip",
             "-Xlinker", "-force_load", "-Xlinker", archive, runtime,
             "-Wl,-U,_lang_vec_observe_v1", "-Wl,-u,_lang_darwin_startup_v1", "-o", compiler],
            log=compiler.parent / "link.log")


def freeze_project(manifest, destination):
    manifest = _required_file(manifest, "Woozy project manifest")
    copy_sources(manifest.parent / "src", destination / "src")
    return Path(shutil.copyfile(manifest, destination / "lang.toml"))


def bootstrap_stack():
    # A supplied seed can be O0 even when the next stages will use O2.
    minimum = 16 * 1024 * 1024
    soft, hard = resource.getrlimit(resource.RLIMIT_STACK)
    if soft != resource.RLIM_INFINITY and soft < minimum:
        if hard != resource.RLIM_INFINITY and hard < minimum:
            raise RuntimeError("native macOS bootstrap requires a stack limit of at least 16 MiB")
        # Darwin can reject raising a running Python's stack limit. Raise it
        # in the launcher before exec so the compiler starts with 16 MiB.
        return minimum, ("/bin/sh", "-c", 'ulimit -S -s 16384 && exec "$@"', "woozy-bootstrap")
    return soft, ()


def bootstrap(args):
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise RuntimeError("native bootstrap requires macOS arm64")
    stack_bytes, prefix = bootstrap_stack()
    output = args.out_dir.resolve()
    seed = _required_file(args.seed, "native Woozy seed")
    stdlib = (args.stdlib or seed.parent / "stdlib").resolve()
    inputs = [stdlib, args.project.resolve().parent / "src",
              args.check_project.resolve().parent / "src"]
    if any(output.is_relative_to(path) for path in inputs):
        raise RuntimeError("output directory must be outside the source trees")
    output.mkdir(parents=True, exist_ok=False)
    result = {"scope": "macos_native_compiler_bootstrap", "opt": args.opt, "stages": []}
    try:
        env = os.environ.copy()
        for name in ("MAKEFLAGS", "GNUMAKEFLAGS", "MAKEFILES", "MAKEOVERRIDES",
                     "MFLAGS", "MAKELEVEL", "MAKE_RESTARTS", "WOOZY_MAKE", "WOOZYC"):
            env.pop(name, None)
        llvm = args.llvm_bin.resolve()
        env["WOOZY_LLVM_BIN"] = str(llvm)
        command = partial(run_command, cwd=output, env=env, timeout=args.timeout, prefix=prefix)
        version = load_json(ROOT / "contracts/toolchain.lock.json")["llvm_version"]
        for tool in LLVM_TOOLS:
            binary = _required_file(llvm / tool, f"LLVM {tool}")
            version_lines = command([binary, "--version"]).splitlines()
            if f"LLVM version {version}" not in [line.strip() for line in version_lines]:
                raise RuntimeError(f"{binary} requires LLVM version {version}")
        sdk = command(["xcrun", "--sdk", "macosx", "--show-sdk-path"]).strip()
        clang = command(["xcrun", "--sdk", "macosx", "--find", "clang"]).strip()
        if not Path(sdk).is_dir() or not Path(clang).is_file():
            raise RuntimeError("macOS SDK and its Clang are required")
        frozen = output / "inputs"
        project = freeze_project(args.project, frozen / "compiler")
        check_project = freeze_project(args.check_project, frozen / "check")
        copy_sources(stdlib, frozen / "stdlib")
        runtime = frozen / "runtime" / "liblang_darwin_core.a"
        runtime.parent.mkdir()
        shutil.copyfile(_required_file(args.runtime_archive, "Darwin runtime archive"), runtime)
        arm64_members(runtime, llvm / "llvm-readobj", command)
        frozen_inputs = _all_tree_hashes(frozen)

        def stage_binary(generation):
            stage = output / f"stage{generation}"
            stage.mkdir()
            for name in ("stdlib", "runtime"):
                (stage / name).symlink_to(frozen / name, target_is_directory=True)
            (stage / "toolchain").symlink_to(llvm, target_is_directory=True)
            return stage / "woozyc"

        compiler = Path(shutil.copy2(seed, stage_binary(1)))
        result.update(seed_sha256=digest(compiler), runtime_sha256=digest(runtime),
                      llvm=version, sdk=sdk, input_sha256=_snapshot_digest(frozen_inputs),
                      stack_bytes=stack_bytes)
        if digest(seed) != result["seed_sha256"]:
            raise RuntimeError(f"seed changed while freezing inputs: {seed}")
        verify_compiler(compiler, check_project, llvm, command)
        for number in (2, 3):
            objects = output / f"objects{number}"
            log = output / f"stage{number}-build.log"
            record = {"generation": number, "compiler_sha256": digest(compiler), "status": "running"}
            result["stages"].append(record)
            print(f"Building macOS stage {number}", flush=True)
            require_ok(command([compiler, "build", "--project", project, "--out-dir", objects,
                                "--source-make", "--jobs", str(args.jobs), "--objects-only",
                                "--target", OBJECT_TARGET,
                                "--opt", args.opt, "--format", "json"], log=log),
                       f"stage {number} build; see {log}")
            if _all_tree_hashes(frozen) != frozen_inputs or digest(compiler) != record["compiler_sha256"]:
                raise RuntimeError(f"frozen inputs changed during stage {number}")
            members = verify_objects(objects, args.opt, llvm, command)
            compiler = stage_binary(number)
            link_compiler(compiler, objects / "program.a", runtime, clang, sdk, command)
            record.update(version=verify_compiler(compiler, check_project, llvm, command),
                          sha256=digest(compiler), packages=members, status="ok")
            print(f"macOS stage {number}: {members} verified objects, {record['sha256']}", flush=True)
        result["stage23_binary_equal"] = filecmp.cmp(
            output / "stage2/woozyc", output / "stage3/woozyc", shallow=False)
        if not result["stage23_binary_equal"]:
            raise RuntimeError("macOS stage 2 and stage 3 binaries differ")
        result["status"] = "ok"
    except Exception as error:
        if result["stages"] and result["stages"][-1]["status"] == "running":
            result["stages"][-1]["status"] = "failed"
        result.update(status="failed", error=str(error))
        raise
    finally:
        write_json(output / "result.json", result)
        print(f"Bootstrap result: {output / 'result.json'}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=Path, required=True,
                        help="native arm64 Woozy-written compiler with --source-make --jobs support")
    parser.add_argument("--llvm-bin", type=Path, required=True, help="pinned LLVM tools directory")
    parser.add_argument("--runtime-archive", type=Path, required=True, help="liblang_darwin_core.a")
    parser.add_argument("--out-dir", type=Path, required=True, help="a new output directory")
    parser.add_argument("--project", type=Path, default=SELFHOST / "lang.toml")
    parser.add_argument("--stdlib", type=Path, help="defaults to stdlib beside the seed")
    parser.add_argument("--check-project", type=Path, default=ROOT / "examples/hello/lang.toml")
    parser.add_argument("--opt", choices=("0", "2"), default="0")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=7200, help="seconds per command")
    args = parser.parse_args()
    if args.jobs < 1 or _timeout_number(args.timeout) is None:
        parser.error("--jobs and --timeout must be positive, and --timeout must be finite")
    try:
        bootstrap(args)
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
