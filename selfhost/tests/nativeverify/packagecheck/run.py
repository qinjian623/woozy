#!/usr/bin/env python3
"""Type-check the nativeverify/nativelayout packages against real dependencies.

The project is staged from this repository's accepted packages plus optional
`--extra-source` trees for packages that are still landing on other branches
(for example valueops and pattern).  Nothing is stubbed: every staged file is
the real worker output.  If a required package is absent, the stage reports
not_run with the exact missing paths.

When hirverify is absent, nativeverify/nativeverify.wz is not staged (its only
dependency is the contract-frozen hirverify API); every other nativeverify file
is still checked, which covers the complete CFG/ownership/exit verification
logic.  A single-file syntax check of the facade is reported separately.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[3]
sys.path.insert(0, str(HERE.parents[1]))
from wzcc_closure import complete_import_closure  # noqa: E402

DEFAULT_STAGE0 = ROOT / "build/woozy-stdlib-native/woozyc"
DEFAULT_TIMEOUT = 300.0

PACKAGES = ["model", "hir", "types", "nativeir", "foreign", "nativelayout",
            "nativeverify", "valueops", "pattern"]
OPTIONAL = ["hirverify"]
IMPORTS = ["foreign", "nativelayout", "nativeverify", "pattern", "valueops"]


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Type-check nativeverify/nativelayout packages.")
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0))
    parser.add_argument("--toolchain-root", default=str(ROOT.parent / "toolchains"))
    parser.add_argument("--extra-source", action="append", default=[],
                        help="directory containing wzcc/<package> trees to "
                             "use when this repository lacks them")
    parser.add_argument("--work-dir", default="")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    return parser.parse_args(argv)


def find_package(package: str, extras: list[pathlib.Path]) -> pathlib.Path:
    local = ROOT / "selfhost/src/wzcc" / package
    if local.is_dir() and any(local.glob("*.wz")):
        return local
    for extra in extras:
        candidate = extra / "wzcc" / package
        if candidate.is_dir() and any(candidate.glob("*.wz")):
            return candidate
    return local


def environment(toolchains: pathlib.Path) -> dict[str, str]:
    env = dict(os.environ)
    libs = [
        str(toolchains / "llvm-22.1.8" / "lib"),
        str(toolchains / "libxml2-legacy-2.13.9" / "usr" / "lib"),
    ]
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        libs.append(previous)
    env["LD_LIBRARY_PATH"] = ":".join(libs)
    return env


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    toolchains = pathlib.Path(args.toolchain_root).expanduser().resolve()
    if not (toolchains / "llvm-22.1.8/bin/clang").is_file():
        fail(f"toolchain clang missing under {toolchains}")
    extras = [pathlib.Path(item).expanduser().resolve()
              for item in args.extra_source]
    sources = {}
    missing = []
    for package in PACKAGES + OPTIONAL:
        path = find_package(package, extras)
        if path.is_dir() and any(path.glob("*.wz")):
            sources[package] = path
        else:
            missing.append(package)
    blocking = [name for name in missing if name not in OPTIONAL]
    if blocking:
        print("status: not_run")
        print("reason: required packages are absent")
        for name in blocking:
            print(f"  missing wzcc/{name}")
        return 0
    if args.work_dir:
        work = pathlib.Path(args.work_dir).expanduser().resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        work = pathlib.Path(tempfile.mkdtemp(prefix="woozy-nativeverify-check-"))
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    wizard = project / "src" / "wzcc"
    for package, path in sources.items():
        target = wizard / package
        target.mkdir(parents=True)
        for file in sorted(path.glob("*.wz")):
            if package == "nativeverify" and "hirverify" not in sources and \
                    file.name == "nativeverify.wz":
                continue
            shutil.copy2(file, target / file.name)
    # The self test is package-internal test scaffolding from this test
    # directory; it uses the real private helpers and is never installed.
    if "nativeverify" in sources:
        shutil.copy2(HERE / "selftest.wz", wizard / "nativeverify/selftest.wz")
    imports = [name for name in IMPORTS if name in sources]
    lines = ["package app;"]
    for name in imports:
        lines.append(f'import "app/wzcc/{name}" as {name};')
    lines.append("import \"std/os\" as os;")
    lines.append('func main() i32 throws os.Error '
                 '{ return nativeverify.selftest(); }')
    (project / "src").mkdir(parents=True, exist_ok=True)
    (project / "src/main.wz").write_text("\n".join(lines) + "\n")
    # hirverify/nativeverify pull in constant/system/ownership/literal/bigint/
    # signature.  Complete the real import closure after main.wz is staged.
    complete_import_closure(project / "src", ROOT / "selfhost/src/wzcc")
    (project / "lang.toml").write_text(
        'schema_version = 1\n\n[package]\nname = "nativeverify_check"\n'
        'source_root = "src"\nentry = "app"\n\n[build]\n'
        'backend = "native"\nword_bits = 64\n')
    env = environment(toolchains)
    print(f"stage0: {stage0}")
    print(f"work-dir: {work}")
    for package in sorted(sources):
        print(f"package {package}: {sources[package]}")
    if "hirverify" not in sources:
        print("note: hirverify absent; nativeverify.wz facade is syntax-only")
        syntax = subprocess.run(
            [str(stage0), "check", "--syntax-only",
             str(ROOT / "selfhost/src/wzcc/nativeverify/nativeverify.wz")],
            env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False, timeout=args.timeout)
        if syntax.returncode != 0:
            fail(f"facade syntax check failed:\n{syntax.stdout}{syntax.stderr}")
        print("PASS: nativeverify.wz syntax")
    command = [str(stage0), "check", "--project", str(project / "lang.toml"),
               "--backend", "native"]
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False, timeout=args.timeout)
    if result.returncode != 0:
        fail(f"package check failed:\n{result.stdout}{result.stderr}")
    print(f"PASS: {result.stdout.strip()}")
    env["PATH"] = (str(toolchains / "llvm-22.1.8" / "bin") + os.pathsep +
                   env.get("PATH", ""))
    for level in (0, 2):
        out_dir = work / f"opt{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        built = subprocess.run(
            [str(stage0), "build", "--project", str(project / "lang.toml"),
             "--backend", "native", "--opt", str(level), "--out-dir",
             str(out_dir)], env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False, timeout=args.timeout)
        if built.returncode != 0:
            fail(f"build O{level} failed:\n{built.stdout}{built.stderr}")
        program = out_dir / "program"
        if not program.is_file():
            fail(f"O{level}: missing {program}")
        values = subprocess.run([str(program)], env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                check=False, timeout=args.timeout)
        if values.returncode != 0:
            fail(f"O{level} selftest failed:\n{values.stdout}"
                 f"{values.stderr}")
        if "selftest failures=0" not in values.stdout:
            fail(f"O{level} selftest output:\n{values.stdout}")
        print(f"PASS: O{level} package selftest ({values.stdout.strip()})")
    print("status: passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
