#!/usr/bin/env python3
"""Build and run the isolated selfhost paths test project at O0 and O2.

The canonical oracle calls os.stat before os.path.realpath(strict=True), so
the kernel checks file/. spellings and the symlink limit. The empty spelling
follows std::filesystem::canonical and reports EINVAL, while Python realpath
would return the current directory. A temporary project links the current
selfhost modules; the prebuilt stage0 compiler builds the driver.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
WOOZYC = Path(os.environ.get("WOOZYC", ROOT / "build/woozy-stdlib-native/woozyc"))
LD_LIBRARY_PATH = os.environ.get("LD_LIBRARY_PATH", "")


def stage_project(root: Path) -> Path:
    project = root / "project"
    shutil.copytree(HERE / "project", project)
    (project / "src" / "wzcc").symlink_to(
        ROOT / "selfhost" / "src" / "wzcc", target_is_directory=True)
    return project / "lang.toml"


def build(opt: str, out_dir: Path, project: Path) -> None:
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LD_LIBRARY_PATH
    result = subprocess.run(
        [
            str(WOOZYC),
            "build",
            "--project",
            str(project),
            "--backend",
            "native",
            "--out-dir",
            str(out_dir),
            "--opt",
            opt,
            "--format",
            "json",
        ],
        text=True,
        capture_output=True,
        env=env, timeout=300,
    )
    print(result.stdout.strip())
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="")
    report = json.loads(result.stdout)
    if result.returncode != 0 or report.get("status") != "ok":
        raise RuntimeError(f"woozyc build --opt {opt} failed")


def fixtures(base: Path) -> list[str]:
    a = base / "a"
    (a / "real" / "sub").mkdir(parents=True)
    (base / "b").mkdir()
    (a / "real" / "file").write_bytes(b"payload\n")
    os.symlink("real", a / "link")
    os.symlink("../b", a / "up")
    os.symlink("/tmp", a / "abs")
    os.symlink("file", a / "real" / "filelink")
    os.symlink("real/", a / "trailinglink")
    os.symlink("missing", base / "dangling")
    os.symlink("loop2", base / "loop1")
    os.symlink("loop1", base / "loop2")
    os.symlink("self", base / "self")
    for number in range(1, 42):
        target = "a/real" if number == 1 else f"links{number - 1}"
        os.symlink(target, base / f"links{number}")
    return [
        ".",
        "..",
        "",
        "a/link/..",
        "a/link/../real",
        "a/up",
        "a/up/file",
        "a/link/file",
        "a/link",
        "a/link/",
        "a/link////",
        "a/abs/../tmp",
        "a/real/file/",
        "a/real/file/x",
        "a/real/file/..",
        "a/real/file/.",
        "a/real/filelink",
        "a/real/filelink/",
        "a/trailinglink",
        "a/trailinglink/",
        "a/trailinglink/file",
        "a/trailinglink/..",
        "dangling",
        "dangling/",
        "loop1",
        "self",
        "links40",
        "links41",
        "a//real///./file",
        "a/real/sub/..",
        "a/real/../../a/real",
        "//",
        "/",
        "/tmp/../tmp",
        str(a / "real"),
        str(a / "link" / "file"),
    ]


def expected(base: Path, case: str) -> tuple[str, str]:
    if case == "":
        # std::filesystem::canonical rejects the empty spelling with EINVAL;
        # Python realpath("", strict=True) returns the current directory.
        return ("ERR", "22")
    # str(base / case) would drop trailing separators and dot components before
    # the oracle could observe them.
    target = case if os.path.isabs(case) else str(base) + "/" + case
    try:
        # Python 3.12 realpath skips file/. checks and the kernel's link limit.
        os.stat(target)
        return ("OK", os.path.realpath(target, strict=True))
    except OSError as error:
        return ("ERR", str(error.errno))


def run(opt: str) -> tuple[int, int]:
    root = Path(tempfile.mkdtemp(prefix=f"selfhost-paths-o{opt}-"))
    try:
        out_dir = root / "build"
        base = root / "fs"
        base.mkdir()
        project = stage_project(root)
        build(opt, out_dir, project)
        cases = fixtures(base)
        binary = out_dir / "program"
        result = subprocess.run(
            [str(binary), *cases],
            cwd=base,
            text=True,
            capture_output=True,
            env=dict(os.environ, LD_LIBRARY_PATH=LD_LIBRARY_PATH), timeout=60,
        )
        passed = failed = 0
        canonical_seen = 0
        for line in result.stdout.splitlines():
            if line.startswith("SUMMARY\t"):
                _, p, f = line.split("\t")
                passed += int(p)
                failed += int(f)
            elif line.startswith("FAIL\t"):
                print(line)
                failed += 0
            elif line.startswith("CANON\t"):
                _, case, kind, payload = line.split("\t", 3)
                want = expected(base, case)
                got = (kind, payload)
                canonical_seen += 1
                if got == want:
                    passed += 1
                else:
                    failed += 1
                    print(f"FAIL\tcanonical [{case}] got={got} want={want}")
        if canonical_seen != len(cases):
            failed += len(cases) - canonical_seen
            print(
                f"FAIL\tcanonical count got={canonical_seen} want={len(cases)}"
            )
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        if result.returncode != 0 and failed == 0:
            failed += 1
            print(f"FAIL\tprogram exit {result.returncode}")
        print(
            f"paths --opt {opt}: passed={passed} failed={failed} "
            f"checks={passed + failed} (join=16 canonical={len(cases)})"
        )
        return passed, failed
    finally:
        shutil.rmtree(root)


def main() -> int:
    if not WOOZYC.is_file():
        print(f"not_run: compiler missing: {WOOZYC}", file=sys.stderr)
        return 2
    total_passed = total_failed = 0
    for opt in ("0", "2"):
        passed, failed = run(opt)
        total_passed += passed
        total_failed += failed
    print(
        f"paths total: passed={total_passed} failed={total_failed} "
        f"checks={total_passed + total_failed}"
    )
    return 0 if total_failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
