#!/usr/bin/env python3
"""Check cold, warm and corrupt arm64 Mach-O object caching in source Make."""

import argparse
import hashlib
import json
import os
import platform
import struct
import subprocess
import tempfile
from pathlib import Path

FIXTURE = Path(__file__).resolve().parents[1] / "source_make" / "lang.toml"
TARGET = "arm64-apple-macosx11.0.0"


def execute(command, env, cwd, expected=0):
    result = subprocess.run(command, env=env, cwd=cwd, capture_output=True,
                            text=True, timeout=600)
    assert result.returncode == expected, (command, result.returncode, result.stdout, result.stderr)
    return result.stdout


def count_build(out, expected):
    stats = json.loads((out / "cache-statistics.json").read_text())
    counts = tuple(stats[key] for key in ("object_hits", "opt_jobs", "llc_jobs"))
    assert counts == expected, stats


def record_field(data, cursor):
    colon = data.index(b":", cursor)
    start = colon + 1
    end = start + int(data[cursor:colon])
    assert end <= len(data)
    return data[start:end], end


def object_record(path):
    data = path.read_bytes()[65:]
    version, cursor = record_field(data, 0)
    name, cursor = record_field(data, cursor)
    object_data, end = record_field(data, cursor)
    assert version == b"wz-native-object/1" and name.decode() == path.name and end == len(data)
    return data[:cursor], object_data


def corruptions(data):
    def change(offset, value):
        return data[:offset] + struct.pack("<I", value) + data[offset + 4:]

    yield "wrong architecture", change(4, 0x01000007)
    yield "bad command size", change(36, 0)
    yield "unexpected load command", change(32, 0x2D)
    offset = 32
    commands = struct.unpack_from("<I", data, 16)[0]
    for _ in range(commands):
        command, size = struct.unpack_from("<II", data, offset)
        if command == 2:
            yield "symbol table overlaps header", change(offset + 8, 32)
        if command == 0x19:
            for section in range(offset + 72, offset + size, 80):
                relocs, count = struct.unpack_from("<II", data, section + 56)
                if count:
                    yield "relocation outside section", change(relocs, 0x7FFFFFFF)
                    break
        offset += size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--native", action="store_true", help="also link and run on macOS arm64")
    args = parser.parse_args()
    if args.native and (platform.system(), platform.machine()) != ("Darwin", "arm64"):
        parser.error("--native requires macOS arm64")

    woozyc = args.woozyc.resolve()
    llvm = args.llvm_bin.resolve()
    env = dict(os.environ, WOOZY_LLVM_BIN=str(llvm))
    with tempfile.TemporaryDirectory(prefix="woozy-macho-cache-") as temporary:
        root = Path(temporary)
        cache = root / "cache"
        for optimization, debug in (("0", False), ("2", False), ("2", True)):
            suffix = f"O{optimization}{'-g' if debug else ''}"
            out = root / f"out-{suffix}"
            command = [str(woozyc), "build", "--project", str(FIXTURE), "--source-make",
                       "--out-dir", str(out), "--opt", optimization, "--cache-dir", str(cache),
                       "--format", "json"]
            if not args.native:
                command += ["--target", TARGET, "--objects-only"]
                env["SDKROOT"] = str(root / "absent-sdk")
            if debug:
                command.append("-g")

            def build(expected):
                execute(command, env, root)
                count_build(out, expected)
                if args.native:
                    execute([str(out / "program")], env, root, expected=19)

            build((0, 3, 3))
            archive = out / "program.a"
            original_archive = archive.read_bytes()
            headers = execute([str(llvm / "llvm-readobj"), "--file-headers", str(archive)], env, root)
            assert headers.count("Format: Mach-O arm64") == 3, headers
            build((3, 0, 0))
            assert archive.read_bytes() == original_archive
            if optimization == "2" and not debug:
                packages = json.loads((out / "make-report.json").read_text())["packages"]
                cases = set()
                for pkg in packages:
                    path = cache / "frontend" / pkg["object_key"]
                    prefix, obj = object_record(path)
                    for name, broken in corruptions(obj):
                        if name in cases:
                            continue
                        # Recompute the outer digest so only the object validator can reject it.
                        body = prefix + str(len(broken)).encode() + b":" + broken
                        path.write_bytes(hashlib.sha256(body).hexdigest().encode() + b"\n" + body)
                        build((2, 1, 1))
                        assert object_record(path)[1] == obj, name
                        assert archive.read_bytes() == original_archive, name
                        cases.add(name)
                assert len(cases) == 5, cases
            print(f"{suffix} Mach-O cache passed", flush=True)


if __name__ == "__main__":
    main()
