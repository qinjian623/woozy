#!/usr/bin/env python3
"""Verify source Make objects and link a native Apple Silicon Woozy compiler."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from build_release import checksum_text
from install_toolchain import ROOT, host_target, publish

sys.path.insert(0, str(ROOT / "selfhost"))
from bootstrap_macos import arm64_members, link_compiler, verify_compiler, verify_objects


def command(argv, *, log=None):
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, timeout=180)
    if log is not None:
        log.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{argv[0]} exited {result.returncode}\n{result.stdout}{result.stderr}")
    return result.stdout


def assemble(objects, runtime, llvm, output):
    if host_target() != "macos-arm64":
        raise ValueError("The compiler must be linked on macOS arm64")
    names = ("program.a", "make-report.json")
    if (objects / "SHA256SUMS").read_text(encoding="ascii") != checksum_text(objects, names):
        raise ValueError("macOS source Make object checksums differ")
    count = verify_objects(objects, "0", llvm, command)
    arm64_members(runtime, llvm / "llvm-readobj", command)
    sdk = command(["xcrun", "--sdk", "macosx", "--show-sdk-path"]).strip()
    clang = command(["xcrun", "--sdk", "macosx", "--find", "clang"]).strip()
    output.parent.mkdir(parents=True, exist_ok=True)
    if os.path.lexists(output):
        raise ValueError(f"Compiler output already exists: {output}")

    with tempfile.TemporaryDirectory(prefix=".woozy-macos-link-", dir=output.parent) as temporary:
        directory = Path(temporary)
        compiler = directory / "woozyc"
        (directory / "stdlib").symlink_to(ROOT / "stdlib", target_is_directory=True)
        link_compiler(compiler, objects / "program.a", runtime, clang, sdk, command)
        version = verify_compiler(compiler, ROOT / "tests/fixtures/core/lang.toml", llvm, command)
        publish(compiler, output)
    print(f"Linked {count} verified Mach-O objects into {version}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--objects", type=Path, required=True)
    parser.add_argument("--runtime-archive", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assemble(args.objects.resolve(strict=True), args.runtime_archive.resolve(strict=True),
             args.llvm_bin.resolve(strict=True), args.output.absolute())


if __name__ == "__main__":
    main()
