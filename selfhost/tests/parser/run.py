#!/usr/bin/env python3
"""Build and run the pretokenized parser tests with the stage-0 compiler.

The test program imports the real app/wzcc/model and app/wzcc/parser packages.
A temporary project is staged with symlinks so the repository stays the single
source of truth.  Each optimization level is built and executed separately.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PARSER = ROOT / "selfhost" / "src" / "wzcc" / "parser"
MODEL = ROOT / "selfhost" / "src" / "wzcc" / "model"
TEST_MAIN = HERE / "src" / "main.wz"

MANIFEST = """schema_version = 1
[package]
name = "parser_pretokenized_test"
source_root = "src"
entry = "app"
[build]
backend = "native"
word_bits = 64
"""


def stage_project(work_dir: Path) -> Path:
    project = work_dir / "project"
    source = project / "src"
    wzcc = source / "wzcc"
    wzcc.mkdir(parents=True)
    for name, target in (("parser", PARSER), ("model", MODEL)):
        link = wzcc / name
        try:
            link.symlink_to(target, target_is_directory=True)
        except OSError:
            shutil.copytree(target, link)
    shutil.copyfile(TEST_MAIN, source / "main.wz")
    manifest = project / "lang.toml"
    manifest.write_text(MANIFEST, encoding="utf-8")
    return project


def run_command(command: list[str], timeout: int, cwd: Path,
                env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    print("+ " + " ".join(command), flush=True)
    return subprocess.run(command, cwd=cwd, env=env, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=timeout)


def parse_json_output(stdout: str, stderr: str) -> dict:
    for line in reversed([line for line in stdout.splitlines() if line.strip()]):
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and "status" in value:
            return value
    raise ValueError(
        "stage-0 build did not emit a JSON status\n"
        f"stdout:\n{stdout}\nstderr:\n{stderr}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--woozyc", required=True,
                        help="stage-0 woozyc executable")
    parser.add_argument("--work-dir", default="",
                        help="staging directory (default: a new temp dir)")
    parser.add_argument("--timeout", type=int, default=180,
                        help="timeout in seconds for each command")
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2),
                        help="optimization level (default: 0 and 2)")
    args = parser.parse_args()

    woozyc = Path(args.woozyc).resolve()
    if not woozyc.is_file():
        print(f"stage-0 compiler not found: {woozyc}", file=sys.stderr)
        return 2
    levels = args.opt or [0, 2]
    if args.work_dir:
        work_dir = Path(args.work_dir).resolve()
        work_dir.mkdir(parents=True, exist_ok=True)
    else:
        work_dir = Path(tempfile.mkdtemp(prefix="woozy-parser-test-"))
    project = stage_project(work_dir)
    manifest = str(project / "lang.toml")
    env = os.environ.copy()

    for level in levels:
        out_dir = project / f"out-o{level}"
        build = run_command([
            str(woozyc), "build", "--project", manifest,
            "--backend", "native", "--out-dir", str(out_dir),
            "--opt", str(level), "--format", "json",
        ], args.timeout, project, env)
        report = parse_json_output(build.stdout, build.stderr)
        if build.returncode != 0 or report.get("status") != "ok":
            print(build.stdout, end="")
            print(build.stderr, end="", file=sys.stderr)
            print(f"FAIL: parser test build at O{level}", file=sys.stderr)
            return 1
        program = out_dir / "program"
        if not program.is_file():
            print(f"FAIL: missing built program {program}", file=sys.stderr)
            return 1
        test = run_command([str(program)], args.timeout, project, env)
        print(test.stdout, end="")
        if test.stderr:
            print(test.stderr, end="", file=sys.stderr)
        if test.returncode != 0:
            print(f"FAIL: parser tests at O{level} exited "
                  f"{test.returncode}", file=sys.stderr)
            return test.returncode
        print(f"PASS: parser pretokenized tests at O{level}")

    print(f"PASS: parser O{'/O'.join(str(level) for level in levels)}; "
          f"work dir {work_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.TimeoutExpired as error:
        print(f"FAIL: timeout: {error}", file=sys.stderr)
        raise SystemExit(124)
