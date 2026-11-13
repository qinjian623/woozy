#!/usr/bin/env python3
"""Run the reusable Native CFG verifier driver over real source projects.

The driver is a Wz program that executes the real self-hosted pipeline
(project.load -> resolve -> sema.check_project -> nativelower.lower) and then
checks the published nativeverify API:

  * an unmodified lowered snapshot verifies again,
  * metadata/owner/error-edge/slot/await mutations of fresh snapshots are
    rejected with E_IR_INVALID,
  * mutating a returned snapshot does not change the published module.

Every package under selfhost/src is staged as-is; no stub package, fabricated
VerifiedHirModule or private verified flag is created.  If a required package
is still absent the whole stage is reported as not_run with the exact missing
paths.  Until hirverify exists this driver is checked but not executed here;
Root runs it after dependency integration.
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
DEFAULT_STAGE0 = ROOT / "build/woozy-stdlib-native/woozyc"
DEFAULT_TOOLCHAIN = ROOT.parent / "toolchains"
DEFAULT_TIMEOUT = 600.0

REQUIRED_PACKAGES = [
    "model", "hir", "hirclone", "types", "source", "lexer", "parser",
    "project", "resolve", "sema", "hirverify", "valueops", "pattern",
    "foreign", "signature", "nativeir", "nativelayout", "nativeverify",
    "nativelower", "nativeirdump", "system", "input", "paths", "manifest",
    "toml", "jsondoc", "digest", "bigint", "number",
]

SKIP_MARKERS = ("reject-", "-reject", "reject_")


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the Native CFG verifier driver over fixtures.")
    parser.add_argument("--woozyc", default=str(DEFAULT_STAGE0))
    parser.add_argument("--toolchain-root", default=str(DEFAULT_TOOLCHAIN))
    parser.add_argument("--work-dir", default="")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2))
    parser.add_argument(
        "--corpus", action="append", default=[],
        help="project manifest or fixture directory (repeatable); "
             "defaults to the accepted cleanup/ownership/async fixtures")
    return parser.parse_args(argv)


def environment(toolchain: pathlib.Path) -> dict[str, str]:
    env = dict(os.environ)
    parts = [
        str(toolchain / "llvm-22.1.8" / "lib"),
        str(toolchain / "libxml2-legacy-2.13.9" / "usr" / "lib"),
    ]
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        parts.append(previous)
    env["LD_LIBRARY_PATH"] = ":".join(parts)
    env["PATH"] = (str(toolchain / "llvm-22.1.8" / "bin") + os.pathsep +
                   env.get("PATH", ""))
    return env


def run(command: list[str], label: str, cwd: pathlib.Path,
        env: dict[str, str], timeout: float) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    try:
        return subprocess.run(
            command, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False, timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"{label}: timed out after {timeout:g}s")
    except OSError as error:
        fail(f"{label}: {error}")


def expect_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label}: exit {result.returncode}\n"
             f"stdout:\n{result.stdout}stderr:\n{result.stderr}")
    print(f"PASS: {label}")


def missing_packages() -> list[str]:
    missing = []
    for package in REQUIRED_PACKAGES:
        directory = ROOT / "selfhost/src/wzcc" / package
        if not directory.is_dir() or not any(directory.glob("*.wz")):
            missing.append(str(directory))
    return missing


def default_corpus() -> list[pathlib.Path]:
    roots = [
        ROOT / "tests/cleanup/fixtures",
        ROOT / "tests/ownership/fixtures",
        ROOT / "tests/native/async_memory/fixtures",
    ]
    result = []
    for root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.iterdir()):
            if not path.is_dir():
                continue
            if any(marker in path.name for marker in SKIP_MARKERS):
                continue
            if (path / "lang.toml").is_file():
                result.append(path / "lang.toml")
    return result


def stage_project(work: pathlib.Path) -> pathlib.Path:
    project = work / "driver"
    if project.exists():
        shutil.rmtree(project)
    shutil.copytree(ROOT / "selfhost/src", project / "src")
    shutil.copy2(HERE / "main.wz", project / "src/main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = pathlib.Path(args.woozyc).expanduser().resolve()
    toolchain = pathlib.Path(args.toolchain_root).expanduser().resolve()
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    if not (toolchain / "llvm-22.1.8/bin/clang").is_file():
        fail(f"toolchain clang missing under {toolchain}")
    missing = missing_packages()
    if missing:
        print("status: not_run")
        print("reason: required self-host packages are absent")
        for path in missing:
            print(f"  missing {path}")
        print("no stub package, fabricated VerifiedHirModule or private "
              "verified flag was used; Root reruns this driver after "
              "dependency integration")
        return 0
    corpus = ([pathlib.Path(item).expanduser().resolve()
               for item in args.corpus] if args.corpus else default_corpus())
    if not corpus:
        fail("no corpus projects found")
    levels = args.opt or [0, 2]
    if args.work_dir:
        work = pathlib.Path(args.work_dir).expanduser().resolve()
        work.mkdir(parents=True, exist_ok=True)
    else:
        work = pathlib.Path(tempfile.mkdtemp(prefix="woozy-nativeverify-driver-"))
    env = environment(toolchain)
    print(f"stage0: {stage0}")
    print(f"work-dir: {work}")
    project = stage_project(work)
    manifest = str(project / "lang.toml")
    stdlib = str(ROOT / "stdlib")
    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "driver check", work, env,
                  args.timeout)
    expect_ok(checked, "driver check")
    for level in levels:
        out_dir = work / f"opt{level}"
        if out_dir.exists():
            shutil.rmtree(out_dir)
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", str(level),
                     "--out-dir", str(out_dir)], f"driver build O{level}",
                    work, env, args.timeout)
        expect_ok(built, f"driver build O{level}")
        program = out_dir / "program"
        if not program.is_file():
            fail(f"O{level}: missing {program}")
        for fixture in corpus:
            if fixture.is_dir():
                fixture = fixture / "lang.toml"
            result = run([str(program), str(fixture), stdlib],
                         f"O{level} {fixture.parent.name}", work, env,
                         args.timeout)
            expect_ok(result, f"O{level} {fixture.parent.name}")
            if "failures=0" not in result.stdout:
                fail(f"O{level} {fixture.parent.name}: driver reported "
                     f"failures\nstdout:\n{result.stdout}")
            rejected = result.stdout.count(" rejected")
            skipped = result.stdout.count(" skipped")
            print(f"PASS: O{level} {fixture.parent.name}: "
                  f"{rejected} mutations rejected, {skipped} skipped")
    print(f"PASS: Native verifier driver; work dir {work}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
