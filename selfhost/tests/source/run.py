#!/usr/bin/env python3
"""Run the pure-Wz source snapshot and manager tests in a copied project.

The repository tree is never modified. The canonical module and the frozen
model are copied into <work-dir>/project together with this directory's main.wz
and lang.toml, then the selected stage0 compiler checks, builds at O0 and O2,
runs the value tests, and runs every panic mode in a separate process.

--woozyc and --work-dir let a verifier use an absolute stage0 binary and a fresh
output directory without rebuilding the C++ compiler or reusing old outputs.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_STAGE0 = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
DEFAULT_WORK = ROOT / "build" / "source-tests"
DEFAULT_TIMEOUT = 120.0

PANIC_MODES = [
    "byte-inside-char",
    "position-inside-crlf",
    "span-interior-byte",
    "span-invalid-source",
    "position-invalid-source",
    "byte-invalid-source",
    "rawspan-order",
    "rawspan-bounds",
    "manager-update-zero",
    "manager-update-unknown",
    "manager-get-zero",
    "manager-get-unknown",
    "manager-empty-path",
]


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def environment() -> dict[str, str]:
    env = dict(os.environ)
    toolchain = env.get("WOOZY_TOOLCHAIN_ROOT")
    if not toolchain:
        return env
    parts = [
        f"{toolchain}/llvm-22.1.8/lib",
        f"{toolchain}/libxml2-legacy-2.13.9/usr/lib",
    ]
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        parts.append(previous)
    env["LD_LIBRARY_PATH"] = ":".join(parts)
    return env


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check, build and execute the Wz source snapshot tests.")
    parser.add_argument(
        "--woozyc", default=str(DEFAULT_STAGE0),
        help=f"stage0 compiler (default: {DEFAULT_STAGE0})")
    parser.add_argument(
        "--work-dir", default=str(DEFAULT_WORK),
        help=f"output/copy directory (default: {DEFAULT_WORK})")
    parser.add_argument(
        "--timeout", type=float, default=DEFAULT_TIMEOUT,
        help=f"timeout in seconds for each command (default: {DEFAULT_TIMEOUT:g})")
    return parser.parse_args(argv)


def run(command: list[str], label: str, work: pathlib.Path,
        timeout: float) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    try:
        return subprocess.run(
            command, cwd=work, env=environment(), text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
            timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"{label}: timed out after {timeout:g}s")
    except OSError as error:
        fail(f"{label}: {error}")


def copy_project(work: pathlib.Path) -> pathlib.Path:
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    (project / "src" / "wzcc" / "model").mkdir(parents=True)
    (project / "src" / "wzcc" / "source").mkdir(parents=True)
    shutil.copy2(ROOT / "selfhost" / "src" / "wzcc" / "model" / "model.wz",
                 project / "src" / "wzcc" / "model" / "model.wz")
    shutil.copy2(ROOT / "selfhost" / "src" / "wzcc" / "source" / "source.wz",
                 project / "src" / "wzcc" / "source" / "source.wz")
    shutil.copy2(HERE / "main.wz", project / "src" / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def expect_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label}: exit {result.returncode}\nstdout:\n{result.stdout}"
             f"stderr:\n{result.stderr}")
    print(f"PASS: {label}")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    work = pathlib.Path(args.work_dir).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")
    work.mkdir(parents=True, exist_ok=True)
    print(f"stage0: {stage0}")
    print(f"work-dir: {work}")
    project = copy_project(work)
    manifest = str(project / "lang.toml")

    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "check", work, args.timeout)
    expect_ok(checked, "stage0 check")

    for opt in ("0", "2"):
        out = work / f"opt{opt}"
        if out.exists():
            shutil.rmtree(out)
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", opt,
                     "--out-dir", str(out)], f"build O{opt}", work,
                    args.timeout)
        expect_ok(built, f"stage0 build O{opt}")
        program = out / "program"
        if not program.is_file():
            fail(f"O{opt}: missing {program}")
        values = run([str(program)], f"values O{opt}", work, args.timeout)
        expect_ok(values, f"value tests O{opt}")
        for mode in PANIC_MODES:
            panic = run([str(program), mode], f"panic {mode} O{opt}", work,
                        args.timeout)
            if panic.returncode != 101:
                fail(f"O{opt} {mode}: exit {panic.returncode}, want 101\n"
                     f"stdout:\n{panic.stdout}stderr:\n{panic.stderr}")
            record = "LANG_NATIVE_PANIC:v1:vec-index:"
            if record not in panic.stderr:
                fail(f"O{opt} {mode}: missing panic record\n"
                     f"stderr:\n{panic.stderr}")
            print(f"PASS: panic {mode} O{opt} exits 101 with vec-index record")
    print(f"PASS: source snapshot and manager tests (check, O0, O2, "
          f"{len(PANIC_MODES)} panic modes per opt level)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
