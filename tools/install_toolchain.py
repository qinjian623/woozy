#!/usr/bin/env python3
"""Assemble a self-hosted Woozy toolchain on Linux x86_64 or macOS arm64."""

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
DARWIN_RUNTIME = "liblang_darwin_core.a"
DARWIN_LLVM_TOOLS = (*LLVM_TOOLS, "ld64.lld", "llvm-ar")
LLVM_LAUNCHER = """#!/bin/sh
set -eu
tools=$(CDPATH= cd -- "${0%/*}" && pwd -P)
export LD_LIBRARY_PATH='$ORIGIN/../lib'"${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$tools/bin/${0##*/}" "$@"
"""
DARWIN_LAUNCHER_HEADER = """#!/bin/sh
set -eu
tools=$(CDPATH= cd -- "${0%/*}" && pwd -P)
export DYLD_LIBRARY_PATH="$tools/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
"""


def host_target():
    system, machine = platform.system(), platform.machine()
    if (system, machine) == ("Linux", "x86_64"):
        return "linux-x86_64"
    if (system, machine) == ("Darwin", "arm64"):
        return "macos-arm64"
    raise ValueError("The native toolchain is available on Linux x86_64 and macOS arm64")


def output(command, env):
    result = subprocess.run(
        command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, timeout=15,
    )
    if result.returncode:
        raise ValueError(f"{command[0]}: {result.stdout.strip()}")
    return result.stdout.strip()


def check_llvm(directory, env, version, names=LLVM_TOOLS):
    pattern = re.compile(r"\b(?:version|LLD)\s+" + re.escape(version) + r"(?:\s|$)")
    for name in names:
        result = output([directory / name, "--version"], env)
        if not pattern.search(result):
            raise ValueError(f"{directory / name} must report LLVM {version}")


def publish(staged, prefix):
    libc = ctypes.CDLL(None, use_errno=True)
    source, destination = os.fsencode(staged), os.fsencode(prefix)
    if platform.system() == "Darwin":
        rename = getattr(libc, "renamex_np", None)
        if rename is None:
            raise ValueError("The installer needs libc renamex_np support")
        rename.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        rename_excl = 0x4
        result = rename(source, destination, rename_excl)
    else:
        rename = getattr(libc, "renameat2", None)
        if rename is None:
            raise ValueError("The installer needs libc renameat2 support")
        rename.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
        at_fdcwd, rename_noreplace = -100, 1
        result = rename(at_fdcwd, source, at_fdcwd, destination, rename_noreplace)
    if result != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), prefix)


def check_native_binary(binary, darwin, label):
    format_name = "Mach-O arm64" if darwin else "ELF"
    if not binary.is_file():
        raise ValueError(f"Expected a {format_name} {label}: {binary}")
    with binary.open("rb") as source:
        header = source.read(16)
    if darwin:
        valid = (header[:8] == b"\xcf\xfa\xed\xfe\x0c\0\0\x01" and
                 header[12:16] == b"\x02\0\0\0")
    else:
        valid = header.startswith(b"\x7fELF")
    if not valid:
        raise ValueError(f"Expected a {format_name} {label}: {binary}")


def copy_tools(source, toolchain, names, darwin):
    # LLVM ships one linker executable dispatched to ELF or Mach-O by its name.
    shared_lld = darwin and (source / "ld.lld").samefile(source / "ld64.lld")
    if shared_lld:
        shutil.copy2(source / "ld.lld", toolchain / "bin/lld")
    for name in names:
        if shared_lld and name in ("ld.lld", "ld64.lld"):
            flavor = "gnu" if name == "ld.lld" else "darwin"
            script = DARWIN_LAUNCHER_HEADER + f'exec "$tools/bin/lld" -flavor {flavor} "$@"\n'
        else:
            shutil.copy2(source / name, toolchain / "bin" / name)
            script = (DARWIN_LAUNCHER_HEADER + 'exec "$tools/bin/${0##*/}" "$@"\n'
                      if darwin else LLVM_LAUNCHER)
        launcher = toolchain / name
        launcher.write_text(script)
        launcher.chmod(0o755)


def install(args):
    darwin = host_target() == "macos-arm64"
    prefix = args.prefix.expanduser().absolute()
    if os.path.lexists(prefix):
        raise ValueError(f"Install destination already exists: {prefix}")
    prefix = prefix.parent.resolve() / prefix.name
    compiler = args.compiler.expanduser().resolve(strict=True)
    llvm_bin = args.llvm_bin.expanduser().resolve(strict=True)
    runtime_dir = (args.runtime_dir or compiler.parent / "runtime").expanduser()
    runtimes = ((DARWIN_RUNTIME, *(name for name in RUNTIMES if os.path.lexists(runtime_dir / name)))
                if darwin else RUNTIMES)
    tools = DARWIN_LLVM_TOOLS if darwin else LLVM_TOOLS
    if darwin and (llvm_bin / "dsymutil").is_file():
        tools = (*tools, "dsymutil")
    version = json.loads((ROOT / "contracts/toolchain.lock.json").read_text())["llvm_version"]
    clean_env = {name: value for name, value in os.environ.items()
                 if name not in {"LD_LIBRARY_PATH", "LD_PRELOAD", "WOOZY_LLVM_BIN"}
                 and not (darwin and name.startswith("DYLD_"))}
    check_native_binary(compiler, darwin, "compiler binary")
    compiler_version = output([compiler, "--version"], clean_env)
    if not re.match(r"woozyc [^\s]+-selfhost(?:\s|$)", compiler_version):
        raise ValueError("Use a Woozy self-hosted compiler binary for --compiler")

    libraries = {}
    library_pattern = r"lib[^/]+\.dylib" if darwin else r"lib[^/]+\.so(?:\.[0-9]+)*"
    for entry in args.compat_lib:
        path = entry.expanduser().absolute()
        if not re.fullmatch(library_pattern, path.name) or not path.is_file():
            raise ValueError(f"Expected a shared library named by its requested filename: {path}")
        if path.name in libraries:
            raise ValueError(f"Duplicate compatibility library: {path.name}")
        libraries[path.name] = path
    library_env = "DYLD_LIBRARY_PATH" if darwin else "LD_LIBRARY_PATH"
    input_env = {**clean_env, library_env:
                 os.pathsep.join(dict.fromkeys(str(path.parent) for path in libraries.values()))}
    if darwin:
        for name in tools:
            check_native_binary(llvm_bin / name, darwin, "LLVM executable")
    check_llvm(llvm_bin, input_env, version, tools)
    resource = Path(output([llvm_bin / "clang", "-print-resource-dir"], input_env))
    if not (resource / "include").is_dir():
        raise ValueError(f"Clang's builtin headers were not found in {resource}")
    for source_dir in (ROOT / "stdlib", resource):
        if prefix.resolve().is_relative_to(source_dir.resolve()):
            raise ValueError(f"Install destination must be outside {source_dir}")

    for name in runtimes:
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
        for name in runtimes:
            shutil.copy2(runtime_dir / name, staged / "runtime" / name)
        (staged / "runtime/python").mkdir()
        for name in ("langrt.py", "unicode18.py"):
            shutil.copy2(ROOT / "runtime/python" / name, staged / "runtime/python" / name)
        shutil.copytree(resource, toolchain / "lib/clang" / resource.name)
        for name, path in libraries.items():
            shutil.copy2(path, toolchain / "lib" / name)
        copy_tools(llvm_bin, toolchain, tools, darwin)

        check_llvm(toolchain, clean_env, version, tools)
        staged_resource = Path(output([toolchain / "clang", "-print-resource-dir"], clean_env))
        if staged_resource.resolve() != (toolchain / "lib/clang" / resource.name).resolve():
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
