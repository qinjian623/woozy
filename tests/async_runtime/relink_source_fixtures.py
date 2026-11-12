#!/usr/bin/env python3
"""Optional end-to-end parity check for runtime/native/async.cpp.

The compiler links generated programs against its prebuilt
`liblang_native_runtime.a`. This script copies that archive, replaces only the
`async.cpp.o` member with a fresh build of this working tree's `async.cpp`, and
relinks the real `tests/async` positive fixtures with it at -O0 and -O2. Every
fixture must produce the exact exit code and stdout recorded in
`tests/async/cases.json`.

Usage (all paths are explicit; nothing is guessed):

    python3 tests/async_runtime/relink_source_fixtures.py \
        --woozyc /path/build/woozy-stdlib-native/woozyc \
        --runtime /path/build/woozy-stdlib-native/native/liblang_native_runtime.a \
        --toolchain /path/toolchains/llvm-22.1.8 \
        --out /tmp/async-source-parity

The fixed LLVM toolchain's ld.lld needs the legacy libxml2 runtime; pass
`--libxml2-lib` (or set LD_LIBRARY_PATH yourself).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
FREESTANDING_FLAGS = (
    "-std=c++20", "-O2", "-ffreestanding", "-fno-builtin", "-fno-exceptions",
    "-fno-rtti", "-fno-sanitize=all", "-fno-builtin-memcpy",
    "-fno-builtin-memset", "-fno-builtin-memmove",
)


def run(argv, env, timeout=120):
    return subprocess.run([str(value) for value in argv], capture_output=True,
                          text=True, env=env, timeout=timeout)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--woozyc", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--libxml2-lib", type=Path, default=None)
    args = parser.parse_args()

    env = dict(os.environ)
    if args.libxml2_lib is not None:
        prefix = str(args.libxml2_lib)
        env["LD_LIBRARY_PATH"] = prefix + (
            ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")

    clangxx = args.toolchain / "bin" / "clang++"
    lld = args.toolchain / "bin" / "ld.lld"
    ar = args.toolchain / "bin" / "llvm-ar"
    for tool in (args.woozyc, clangxx, lld, ar, args.runtime):
        if not tool.exists():
            print(f"missing required tool/archive: {tool}", file=sys.stderr)
            return 2

    out = args.out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    async_object = out / "async.cpp.o"
    build = run([clangxx, *FREESTANDING_FLAGS, "-c",
                 ROOT / "runtime" / "native" / "async.cpp", "-o", async_object],
                env)
    if build.returncode != 0:
        print(build.stderr, file=sys.stderr)
        return 1
    archive = out / "runtime-with-worktree-async.a"
    shutil.copyfile(args.runtime, archive)
    added = run([ar, "r", archive, async_object], env)
    if added.returncode != 0:
        print(added.stderr, file=sys.stderr)
        return 1

    catalog = json.loads((ROOT / "tests" / "async" / "cases.json").read_text())
    failures = 0
    for opt in ("0", "2"):
        for case in catalog["positive"]:
            name = case["name"]
            directory = out / f"{name}_O{opt}"
            directory.mkdir(parents=True, exist_ok=True)
            manifest = ROOT / "tests" / "async" / name / "lang.toml"
            compiled = run([args.woozyc, "build", "--project", manifest,
                            "--backend", "native", "--opt", opt,
                            "--out-dir", directory], env)
            if compiled.returncode != 0:
                print(f"[FAIL] build {name} O{opt}: {compiled.stdout}"
                      f"{compiled.stderr}")
                failures += 1
                continue
            program = directory / "program.worktree"
            linked = run([clangxx, "--target=x86_64-unknown-linux-gnu",
                          "-nostdlib", "-static", f"--ld-path={lld}",
                          directory / "program.o", archive, "-o", program],
                         env)
            if linked.returncode != 0:
                print(f"[FAIL] link {name} O{opt}: {linked.stderr}")
                failures += 1
                continue
            executed = run([program], env, timeout=30)
            ok = (executed.returncode == case["exit"] and
                  executed.stdout == case["stdout"])
            print(f"[{'PASS' if ok else 'FAIL'}] source {name} O{opt}: "
                  f"exit={executed.returncode} stdout={executed.stdout!r}")
            if not ok:
                print(f"  expected exit={case['exit']} "
                      f"stdout={case['stdout']!r} stderr={executed.stderr!r}")
                failures += 1
    print(f"failures: {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
