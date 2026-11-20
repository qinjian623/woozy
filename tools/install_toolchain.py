#!/usr/bin/env python3
"""Assemble a self-hosted Woozy toolchain from existing Linux x86_64 builds."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
LLVM_TOOLS = ("opt", "llc", "clang", "ld.lld")
RUNTIMES = ("liblang_native_runtime.a", "liblang_hosted_runtime.a")
LLVM_LAUNCHER = """#!/bin/sh
set -eu
tools=$(CDPATH= cd -- "${0%/*}" && pwd -P)
export LD_LIBRARY_PATH='$ORIGIN/../lib'"${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$tools/bin/${0##*/}" "$@"
"""


def output(command, env):
    result = subprocess.run(
        command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, timeout=15,
    )
    if result.returncode:
        raise ValueError(f"{command[0]}: {result.stdout.strip()}")
    return result.stdout.strip()


def check_llvm(directory, env, version):
    pattern = re.compile(r"\b(?:version|LLD)\s+" + re.escape(version) + r"(?:\s|$)")
    for name in LLVM_TOOLS:
        result = output([directory / name, "--version"], env)
        if not pattern.search(result):
            raise ValueError(f"{directory / name} must report LLVM {version}")


def publish(staged, prefix):
    # Linux renameat2 can publish atomically without replacing even an empty directory.
    rename = getattr(ctypes.CDLL(None, use_errno=True), "renameat2", None)
    if rename is None:
        raise ValueError("The installer needs libc renameat2 support")
    rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
    at_fdcwd, rename_noreplace = -100, 1
    if rename(at_fdcwd, os.fsencode(staged), at_fdcwd, os.fsencode(prefix), rename_noreplace) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), prefix)


def install(args):
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        raise ValueError("The native toolchain is available only on Linux x86_64")
    prefix = args.prefix.expanduser().absolute()
    if os.path.lexists(prefix):
        raise ValueError(f"Install destination already exists: {prefix}")
    prefix = prefix.parent.resolve() / prefix.name
    compiler = args.compiler.expanduser().resolve(strict=True)
    llvm_bin = args.llvm_bin.expanduser().resolve(strict=True)
    runtime_dir = (args.runtime_dir or compiler.parent / "runtime").expanduser()
    version = json.loads((ROOT / "contracts/toolchain.lock.json").read_text())["llvm_version"]
    clean_env = {name: value for name, value in os.environ.items()
                 if name not in {"LD_LIBRARY_PATH", "LD_PRELOAD", "WOOZY_LLVM_BIN"}}
    if not compiler.is_file():
        raise ValueError(f"Expected an ELF compiler binary: {compiler}")
    with compiler.open("rb") as source:
        if source.read(4) != b"\x7fELF":
            raise ValueError(f"Expected an ELF compiler binary: {compiler}")
    compiler_version = output([compiler, "--version"], clean_env)
    if not re.match(r"woozyc [^\s]+-selfhost(?:\s|$)", compiler_version):
        raise ValueError("Use a Woozy self-hosted compiler binary for --compiler")

    libraries = {}
    for entry in args.compat_lib:
        path = entry.expanduser().absolute()
        if not re.fullmatch(r"lib[^/]+\.so(?:\.[0-9]+)*", path.name) or not path.is_file():
            raise ValueError(f"Expected a shared library named by its SONAME: {path}")
        if path.name in libraries:
            raise ValueError(f"Duplicate compatibility library: {path.name}")
        libraries[path.name] = path
    input_env = {**clean_env, "LD_LIBRARY_PATH":
                 os.pathsep.join(dict.fromkeys(str(path.parent) for path in libraries.values()))}
    check_llvm(llvm_bin, input_env, version)
    resource = Path(output([llvm_bin / "clang", "-print-resource-dir"], input_env))
    if not (resource / "include").is_dir():
        raise ValueError(f"Clang's builtin headers were not found in {resource}")
    for source_dir in (ROOT / "stdlib", resource):
        if prefix.resolve().is_relative_to(source_dir.resolve()):
            raise ValueError(f"Install destination must be outside {source_dir}")

    for name in RUNTIMES:
        archive = runtime_dir / name
        if not archive.is_file():
            raise ValueError(f"Expected a regular static archive: {archive}")
        with archive.open("rb") as source:
            header = source.read(68)
            if len(header) != 68 or not header.startswith(b"!<arch>\n") or not header.endswith(b"`\n"):
                raise ValueError(f"Expected a nonempty static archive: {archive}")

    prefix.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".woozy-install-", dir=prefix.parent) as temporary:
        staged = Path(temporary) / "bundle"
        toolchain = staged / "toolchain"
        (toolchain / "bin").mkdir(parents=True)
        (staged / "runtime").mkdir()
        shutil.copy2(compiler, staged / "woozyc")
        shutil.copytree(ROOT / "stdlib", staged / "stdlib",
                        ignore=shutil.ignore_patterns(".git", "build", "__pycache__"))
        for name in RUNTIMES:
            shutil.copy2(runtime_dir / name, staged / "runtime" / name)
        (staged / "runtime/python").mkdir()
        for name in ("langrt.py", "unicode18.py"):
            shutil.copy2(ROOT / "runtime/python" / name, staged / "runtime/python" / name)
        shutil.copytree(resource, toolchain / "lib/clang" / resource.name)
        for name, path in libraries.items():
            shutil.copy2(path, toolchain / "lib" / name)
        for name in LLVM_TOOLS:
            shutil.copy2(llvm_bin / name, toolchain / "bin" / name)
            launcher = toolchain / name
            launcher.write_text(LLVM_LAUNCHER)
            launcher.chmod(0o755)

        check_llvm(toolchain, clean_env, version)
        staged_resource = Path(output([toolchain / "clang", "-print-resource-dir"], clean_env))
        if staged_resource != toolchain / "lib/clang" / resource.name:
            raise ValueError(f"Copied Clang could not find its bundled headers: {staged_resource}")
        if output([staged / "woozyc", "--version"], clean_env) != compiler_version:
            raise ValueError("Copied compiler version changed")
        publish(staged, prefix)


def add_bundle_inputs(parser):
    parser.add_argument("--compiler", required=True, type=Path, help="built self-hosted woozyc")
    parser.add_argument("--runtime-dir", type=Path, help="runtime archive directory, defaults to compiler sibling runtime/")
    parser.add_argument("--llvm-bin", required=True, type=Path, help="directory with the locked LLVM tools")
    parser.add_argument("--compat-lib", type=Path, action="append", default=[],
                        help="extra shared library under its requested name, may be repeated")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", required=True, type=Path, help="new installation directory")
    add_bundle_inputs(parser)
    args = parser.parse_args()
    try:
        install(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Woozy installation failed: {error}\n")
    print(args.prefix.expanduser().absolute() / "woozyc")


if __name__ == "__main__":
    main()
