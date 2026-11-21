#!/usr/bin/env python3
"""Extract the LLVM SDK and tools needed by a Woozy release build."""

import argparse
from pathlib import Path
import re
import subprocess


def unpack(archive, destination, root, paths):
    destination.mkdir(parents=True)
    subprocess.run([
        "tar", "-xJf", archive, "--no-same-owner", "--strip-components=1",
        "-C", destination, "--", *(f"{root}/{path}" for path in paths),
    ], check=True)


def extract(archive, destination, version, target="linux-x86_64"):
    major = version.split(".")[0]
    if target == "macos-arm64":
        tools = ("clang", f"clang-{major}", "dsymutil", "ld.lld", "ld64.lld", "llc",
                 "lld", "llvm-ar", "llvm-readobj", "opt")
        unpack(archive, destination, f"LLVM-{version}-macOS-ARM64",
               ("lib/clang", *(f"bin/{name}" for name in tools)))
        return
    root = f"LLVM-{version}-Linux-X64"
    exports_path = f"{root}/lib/cmake/llvm/LLVMExports-release.cmake"
    exports = subprocess.check_output(
        ["tar", "-xJOf", archive, "--occurrence", exports_path], text=True,
    )
    exported_tools = set(re.findall(r'\$\{_IMPORT_PREFIX\}/bin/([\w.+-]+)"', exports))
    if not {"llc", "opt"} <= exported_tools:
        raise ValueError("LLVM CMake exports do not list the expected tools")
    # CMake verifies that every exported executable is present, even if unused.
    tools = exported_tools | {"clang", f"clang-{major}", "clang++", "lld", "ld.lld"}
    unpack(archive, destination, root,
           ("include", "lib", *(f"bin/{name}" for name in sorted(tools))))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("version")
    parser.add_argument("--target", choices=("linux-x86_64", "macos-arm64"), default="linux-x86_64")
    args = parser.parse_args()
    extract(args.archive, args.destination, args.version, args.target)


if __name__ == "__main__":
    main()
