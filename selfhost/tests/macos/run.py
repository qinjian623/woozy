#!/usr/bin/env python3
"""Run native macOS arm64 compiler checks with a built Woozy toolchain."""

import argparse
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def command(argv, *, cwd=None, expected=0):
    result = subprocess.run(argv, cwd=cwd, text=True, capture_output=True, timeout=180)
    if result.returncode != expected:
        raise RuntimeError(f"{argv}\n{result.returncode=}\n{result.stdout}\n{result.stderr}")
    return result.stdout


def build(compiler, manifest, output, *options):
    result = json.loads(command([compiler, "build", "--project", manifest,
                                 "--out-dir", output, "--format", "json", *options]))
    assert result["status"] == "ok", result
    program = output / "program"
    assert "Mach-O 64-bit executable arm64" in command(["file", program])
    return program


def check_debugger(program, source, marker, variables, expected_values):
    lines = source.read_text().splitlines()
    line = next(index for index, text in enumerate(lines, 1) if marker in text)
    # Authorize LLDB to launch only this disposable test executable over SSH.
    entitlements = program.parent / "debug-entitlements.plist"
    entitlements.write_bytes(plistlib.dumps({"com.apple.security.get-task-allow": True}))
    command(["codesign", "--force", "--sign", "-", "--entitlements", entitlements, program])
    debugger = command(["xcrun", "--find", "lldb"]).strip()
    stdout = command([
        debugger, "--batch", "--no-lldbinit",
        "-o", f'target create --symfile "{program}.dwarf" "{program}"',
        "-o", f'command script import "{ROOT / "tools/lldb/woozy.py"}"',
        "-o", f"breakpoint set -f {source.name} -l {line}",
        "-o", "run",
        "-o", f"frame variable {variables}",
        "-o", "continue",
    ], cwd=source.parent.parent)
    assert "Breakpoint 1:" in stdout and "no locations" not in stdout, stdout
    for expected in (*expected_values, "exited with status = 42"):
        assert expected in stdout, stdout


def check_c(compiler, work):
    fixtures = work / "c_ffi"
    shutil.copytree(ROOT / "tests/native/c_ffi", fixtures, ignore=shutil.ignore_patterns(".build"))
    library = fixtures / ".build"
    library.mkdir()
    command(["xcrun", "clang", "-std=c11", "-target", "arm64-apple-macosx14.4", "-O2",
             "-c", fixtures / "fixture/ffi_fixture.c", "-o", library / "fixture.o"])
    command(["xcrun", "ar", "rcs", library / "libffi_fixture.a", library / "fixture.o"])
    binding = fixtures / "bindings/fixture.json"
    data = json.loads(binding.read_text())
    data["target"] = "arm64-apple-darwin"
    binding.write_text(json.dumps(data))
    for label in ("scalars", "threads"):
        manifest = fixtures / "projects" / label / "lang.toml"
        command([build(compiler, manifest, work / label, "--opt", "2")])
    data["target"] = "x86_64-linux-gnu"
    binding.write_text(json.dumps(data))
    wrong_target = subprocess.run([compiler, "build", "--project", manifest,
                                   "--out-dir", work / "wrong-target", "--format", "json"],
                                  text=True, capture_output=True, timeout=180)
    diagnostics = json.loads(wrong_target.stdout)["diagnostics"]
    assert wrong_target.returncode and any(row["code"] == "E_TARGET_UNSUPPORTED"
                                           for row in diagnostics), wrong_target.stdout
    print("C scalar, record, callback and foreign-thread fixtures passed", flush=True)


def test(compiler, work):
    hello = ROOT / "examples/hello/lang.toml"
    for optimization in ("0", "2"):
        program = build(compiler, hello, work / f"hello-{optimization}", "--opt", optimization)
        assert command([program]) == "Hello, world!\n"
        libraries = command(["otool", "-L", program]).splitlines()[1:]
        assert len(libraries) == 1 and libraries[0].strip().startswith("/usr/lib/libSystem.B.dylib"), libraries
    print("Default native link passed at O0 and O2", flush=True)

    native = ROOT / "tests/native"
    for name, manifest, expected in (
        ("standard-library", native / "stdlib/lang.toml", "stdlib: ok\n"),
        ("pure-library", native / "stdlib-pure/lang.toml", ""),
        ("unicode", native / "unicode/source/project.toml", ""),
    ):
        assert command([build(compiler, manifest, work / name, "--opt", "2")]) == expected
    print("Existing file, time, random, math, network and Unicode fixtures passed", flush=True)

    manifest = ROOT / "selfhost/tests/source_make/lang.toml"
    for mode in ("--source-make", "--package-objects"):
        out = work / mode.removeprefix("--")
        options = (mode, "--opt", "2", "--cache-dir", work / f"{out.name}-cache")
        expected = 19
        program = build(compiler, manifest, out, *options)
        command([program], expected=expected)
        cold = json.loads((out / "cache-statistics.json").read_text())
        command([build(compiler, manifest, out, *options)], expected=expected)
        warm = json.loads((out / "cache-statistics.json").read_text())
        assert cold["opt_jobs"] == 3 and cold["llc_jobs"] == 3, cold
        assert warm["object_hits"] == 3, warm
        assert warm["opt_jobs"] == 0 and warm["llc_jobs"] == 0, warm
    print("Both package build modes reused all three cached Mach-O objects", flush=True)

    for name, expected in (("basic", "42\n"), ("tcp-loopback", "TCP_OK\n"), ("udp-loopback", "UDP_OK\n")):
        manifest = ROOT / "tests/async" / name / "lang.toml"
        program = build(compiler, manifest, work / name, "--opt", "2")
        assert command([program]) == expected
    print("Async compute, TCP and UDP fixtures passed", flush=True)
    check_c(compiler, work)

    debug_project = work / "debug"
    (debug_project / "src").mkdir(parents=True)
    manifest = debug_project / "lang.toml"
    manifest.write_text('schema_version = 1\n[package]\nname="debug"\nsource_root="src"\nentry="app"\n'
                        '[build]\nbackend="native"\nword_bits=64\n')
    source = debug_project / "src/values.wz"
    shutil.copyfile(ROOT / "tests/native/debug_info/values.wz", source)
    for flag in ((), ("--source-make",), ("--package-objects",)):
        output = work / f"debug-{len(flag)}-{''.join(flag)}"
        check_debugger(build(compiler, manifest, output, "-g", *flag), source,
                       "BREAK_SHADOW", "seed word *numbers window",
                       ('seed = 9', 'word = "你好"', 'length 3', 'length 2', '[0] = 4', '[0] = 9'))
    print("LLDB resolved breakpoints and read scoped variables in all three modes", flush=True)
    source = source.with_name("await.wz")
    (source.parent / "values.wz").unlink()
    shutil.copyfile(ROOT / "tests/native/debug_info/await.wz", source)
    check_debugger(build(compiler, manifest, work / "debug-async", "-g"), source,
                   "BREAK_RESUMED", "seed before", ("seed = 40", "before = 41"))
    print("LLDB read values after await", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        parser.error("these checks require macOS arm64")
    os.environ.setdefault("MAKEFLAGS", "-j3")
    with tempfile.TemporaryDirectory(prefix="woozy-macos-tests-") as work:
        test(args.woozyc.resolve(), Path(work))


if __name__ == "__main__":
    main()
