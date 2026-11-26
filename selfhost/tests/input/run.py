#!/usr/bin/env python3
"""Build and run the isolated selfhost input test project at O0 and O2.

Fixtures are real temporary files. The driver checks SourceManager retention,
diagnostic phase/failure and source error byte ranges; the host C++ compiler is
only used as the already-built woozyc stage0, never rebuilt or called through
the old frontend API. A temporary project links the current selfhost modules.
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

FIXTURES = {
    "valid-bom-crlf.bin": bytes(
        [0xEF, 0xBB, 0xBF, 0x61, 0x0D, 0x0A, 0x62, 0x0D, 0x0A, 0xC3, 0xA9, 0x0A]
    ),
    "valid-utf8.bin": bytes([0xF0, 0x9F, 0x98, 0x80, 0x0A, 0x5A]),
    "empty.bin": b"",
    "bom-only.bin": bytes([0xEF, 0xBB, 0xBF]),
    "invalid-nul.bin": bytes([0x61, 0x62, 0x00, 0x63, 0x64]),
    "invalid-nul-offset.bin": bytes(
        [0xEF, 0xBB, 0xBF, 0x61, 0x0D, 0x0A, 0x00, 0x62]
    ),
    "invalid-cr.bin": bytes([0x61, 0x0D, 0x62]),
    "invalid-continuation.bin": bytes([0xC3, 0x28]),
    "invalid-truncated.bin": bytes([0xE2, 0x82]),
    "invalid-overlong.bin": bytes([0xC0, 0x80]),
    "invalid-scalar.bin": bytes([0xED, 0xA0, 0x80]),
    "file.bin": b"not a directory\n",
    "unreadable.bin": b"secret\n",
    "dir with space/\u00fcn\u00efcode \u6c49.bin": b"spaced",
    "multi-chunk.bin": (
        bytes([0xEF, 0xBB, 0xBF])
        + b"0123456789abcdef\xc3\xa9\r\n" * 3500
    ),
}

# Permission-restricted fixtures are restored even when a check fails.
RESTORE: list[tuple[Path, int]] = []


def restrict(path: Path) -> None:
    RESTORE.append((path, path.stat().st_mode & 0o7777))
    os.chmod(path, 0o000)


def restore_permissions() -> None:
    for path, mode in RESTORE:
        try:
            os.chmod(path, mode)
        except OSError:
            pass
    RESTORE.clear()


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


def run(opt: str) -> tuple[int, int]:
    root = Path(tempfile.mkdtemp(prefix=f"selfhost-input-o{opt}-"))
    try:
        out_dir = root / "build"
        base = root / "fs"
        base.mkdir()
        for name, data in FIXTURES.items():
            path = base / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (base / "directory").mkdir()
        (base / "unreadable-directory").mkdir()
        os.symlink("loop.bin", base / "loop.bin")
        os.symlink("absent.bin", base / "dangling.bin")
        restrict(base / "unreadable.bin")
        restrict(base / "unreadable-directory")
        permission_checks = os.geteuid() != 0
        if not permission_checks:
            print("skip: unreadable fixtures are readable as root", file=sys.stderr)
        # The oracle is the read loop of src/support/input.cpp, so the expected
        # open/read wording is produced by the real std::ifstream.
        oracle = root / "oracle"
        cxx = os.environ.get("CXX", "g++")
        built = subprocess.run(
            [cxx, "-std=c++20", "-O2", str(HERE / "oracle.cpp"), "-o", str(oracle)],
            text=True, capture_output=True, env=dict(os.environ, LC_ALL="C"), timeout=120,
        )
        if built.returncode != 0:
            raise RuntimeError(f"input oracle build failed: {built.stderr}")
        oracle_run = subprocess.run(
            [str(oracle), str(base)], cwd=base, text=True, capture_output=True,
            env=dict(os.environ, LC_ALL="C"), timeout=60,
        )
        if oracle_run.returncode != 0:
            raise RuntimeError(f"input oracle failed: {oracle_run.stderr}")
        wanted = {line for line in oracle_run.stdout.splitlines() if line.startswith("IO\t")}
        project = stage_project(root)
        build(opt, out_dir, project)
        result = subprocess.run(
            [str(out_dir / "program"), str(base),
             *([] if permission_checks else ["skip-permissions"])],
            cwd=base,
            text=True,
            capture_output=True,
            env=dict(os.environ, LD_LIBRARY_PATH=LD_LIBRARY_PATH, LC_ALL="C"), timeout=60,
        )
        passed = failed = 0
        summary_seen = False
        actual = set()
        for line in result.stdout.splitlines():
            if line.startswith("SUMMARY\t"):
                _, p, f = line.split("\t")
                passed += int(p)
                failed += int(f)
                summary_seen = True
            elif line.startswith("FAIL\t"):
                print(line)
            elif line.startswith("IO\t"):
                actual.add(line)
        for line in sorted(wanted - actual):
            failed += 1
            print(f"FAIL\tC++ oracle I/O message missing: {line}")
        for line in sorted(actual - wanted):
            failed += 1
            print(f"FAIL\tI/O message differs from the C++ oracle: {line}")
        if not summary_seen:
            failed += 1
            print("FAIL\tmissing SUMMARY")
        if result.stderr:
            print(result.stderr, file=sys.stderr, end="")
        if result.returncode != 0 and failed == 0:
            failed += 1
            print(f"FAIL\tprogram exit {result.returncode}")
        print(
            f"input --opt {opt}: passed={passed} failed={failed} "
            f"checks={passed + failed}"
        )
        return passed, failed
    finally:
        restore_permissions()
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
        f"input total: passed={total_passed} failed={total_failed} "
        f"checks={total_passed + total_failed}"
    )
    return 0 if total_failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
