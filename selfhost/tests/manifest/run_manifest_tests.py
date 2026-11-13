#!/usr/bin/env python3
"""Run the pure-Wz SH05 manifest loader tests from a copied project.

The stage0 compiler only builds the copied Wz project; no C++ compiler or
toml++ parser is rebuilt. Each opt level runs the same real-filesystem checks:
temporary manifests, a manifest/source-root symlink, dependency config cases,
whole-original-byte diagnostics, integer limits, and every repository lang.toml
as an existing legal dependency manifest. The only intentionally unchecked
diagnostic text is the TOML syntax wording: the Woozy reader reports its own
message instead of toml++'s description. The code, Environment failure and
whole-source span are compared exactly.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DEFAULT_STAGE0 = ROOT / "build" / "woozy-stdlib-native" / "woozyc"
DEFAULT_WORK = ROOT / "build" / "manifest-tests"
DEFAULT_TIMEOUT = 180.0
MODULES = ("model", "source", "input", "paths", "toml", "manifest")
COUNT_RE = re.compile(r"MANIFEST_COUNTS passed=(\d+) failed=(\d+)")


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def environment() -> dict[str, str]:
    env = dict(os.environ)
    parts = [
        str(ROOT.parent / "toolchains" / "llvm-22.1.8" / "lib"),
        str(ROOT.parent / "toolchains" / "libxml2-legacy-2.13.9" / "usr" / "lib"),
    ]
    previous = env.get("LD_LIBRARY_PATH")
    if previous:
        parts.append(previous)
    env["LD_LIBRARY_PATH"] = ":".join(parts)
    return env


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Check, build and execute the Wz manifest loader tests.")
    parser.add_argument(
        "--woozyc", default=str(DEFAULT_STAGE0),
        help=f"absolute stage0 compiler (default: {DEFAULT_STAGE0})")
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
    (project / "src").mkdir(parents=True)
    for module in MODULES:
        shutil.copytree(ROOT / "selfhost" / "src" / "wzcc" / module,
                        project / "src" / "wzcc" / module)
    shutil.copy2(HERE / "src" / "main.wz", project / "src" / "main.wz")
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    return project


def corpus_list(work: pathlib.Path) -> pathlib.Path:
    manifests = []
    for path in ROOT.rglob("lang.toml"):
        relative = path.relative_to(ROOT)
        if "build" in relative.parts or ".git" in relative.parts:
            continue
        # This harness manifest's source_root is produced by its own runner
        # copy step; it is not directly loadable from the repository tree.
        if relative.as_posix() == "selfhost/tests/source/lang.toml":
            continue
        manifests.append(str(path.resolve()))
    manifests.sort()
    listing = work / "manifest-corpus.txt"
    listing.write_text("\n".join(manifests) + "\n", encoding="utf-8")
    print(f"corpus: {len(manifests)} existing lang.toml files")
    return listing


def expect_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label}: exit {result.returncode}\nstdout:\n{result.stdout}"
             f"stderr:\n{result.stderr}")
    print(f"PASS: {label}")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = pathlib.Path(args.woozyc).expanduser()
    if not stage0.is_absolute():
        fail(f"--woozyc must be absolute: {stage0}")
    if not stage0.is_file():
        fail(f"stage0 compiler missing: {stage0}")
    if args.timeout <= 0:
        fail(f"--timeout must be positive, got {args.timeout:g}")

    work = pathlib.Path(args.work_dir).expanduser().resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    print(f"stage0: {stage0}")
    print(f"work-dir: {work}")
    project = copy_project(work)
    manifest = str(project / "lang.toml")
    listing = corpus_list(work)

    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], "check", work, args.timeout)
    expect_ok(checked, "stage0 check")

    for opt in ("0", "2"):
        out = work / f"opt{opt}"
        built = run([str(stage0), "build", "--project", manifest,
                     "--backend", "native", "--opt", opt,
                     "--out-dir", str(out)], f"build O{opt}", work,
                    args.timeout)
        expect_ok(built, f"stage0 build O{opt}")
        program = out / "program"
        if not program.is_file():
            fail(f"O{opt}: missing {program}")
        case_dir = work / f"cases-opt{opt}"
        case_dir.mkdir()
        values = run([str(program), str(case_dir), str(listing)],
                     f"manifest tests O{opt}", work, args.timeout)
        expect_ok(values, f"manifest tests O{opt}")
        match = COUNT_RE.search(values.stdout)
        if match is None:
            fail(f"O{opt}: missing MANIFEST_COUNTS line")
        passed, failed = (int(value) for value in match.groups())
        if failed != 0 or passed == 0:
            fail(f"O{opt}: passed={passed} failed={failed}")
        print(f"O{opt} counts: passed={passed} failed={failed}")
    print("PASS: manifest loader tests (check, O0, O2, real temp dirs, "
          "symlinks, dependency configs, integer limits, full legal corpus)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
