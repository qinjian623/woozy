#!/usr/bin/env python3
"""Extract the LLVM SDK and tools needed by the Linux release build."""

import argparse
from pathlib import Path
import re
import subprocess


def extract(archive, destination, version):
    root = f"LLVM-{version}-Linux-X64"
    exports_path = f"{root}/lib/cmake/llvm/LLVMExports-release.cmake"
    exports = subprocess.check_output(
        ["tar", "-xJOf", archive, "--occurrence", exports_path], text=True,
    )
    exported_tools = set(re.findall(r'\$\{_IMPORT_PREFIX\}/bin/([\w.+-]+)"', exports))
    if not {"llc", "opt"} <= exported_tools:
        raise ValueError("LLVM CMake exports do not list the expected tools")
    # CMake verifies that every exported executable is present, even if unused.
    tools = exported_tools | {"clang", f"clang-{version.split('.')[0]}", "clang++", "lld", "ld.lld"}
    destination.mkdir(parents=True)
    subprocess.run([
        "tar", "-xJf", archive, "--no-same-owner", "--strip-components=1",
        "-C", destination, "--", f"{root}/include", f"{root}/lib",
        *(f"{root}/bin/{name}" for name in sorted(tools)),
    ], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("version")
    args = parser.parse_args()
    extract(args.archive, args.destination, args.version)


if __name__ == "__main__":
    main()
