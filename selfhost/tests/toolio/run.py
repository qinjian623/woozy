#!/usr/bin/env python3
"""Exercise current toolio with real processes and filesystem operations at O0/O2.

Staging links the live production toolio package; no module copy is checked in.
Every build, process capture, and output directory belongs to a fresh run folder.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TOOLCHAINS = ROOT.parent / "toolchains"
EXPECTED = 40


def command(argv: list[str], label: str, work: Path, timeout: float,
            env: dict[str, str], records: list[dict]) -> str:
    print("+", " ".join(argv), flush=True)
    before = time.monotonic()
    process = subprocess.Popen(argv, cwd=work, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True,
                               start_new_session=True)
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        os.killpg(process.pid, signal.SIGKILL)
        stdout, stderr = process.communicate()
    (work / f"{label}.stdout").write_text(stdout)
    (work / f"{label}.stderr").write_text(stderr)
    records.append({"label": label, "argv": argv, "exit": process.returncode,
                    "timed_out": timed_out,
                    "seconds": time.monotonic() - before})
    (work / "commands.json").write_text(json.dumps(records, indent=2))
    if timed_out or process.returncode != 0:
        raise RuntimeError(f"{label}: exit={process.returncode}, timeout={timed_out}\n"
                           f"{stdout}{stderr}")
    if stderr:
        raise RuntimeError(f"{label}: unexpected stderr: {stderr}")
    print(f"PASS {label}" + (f": {stdout.strip()}" if stdout.strip() else ""), flush=True)
    return stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", default=str(ROOT / "build/woozy-stdlib-native/woozyc"))
    parser.add_argument("--cxx", default=str(TOOLCHAINS / "llvm-22.1.8/bin/clang++"))
    parser.add_argument("--work-dir", help="parent for a unique retained run directory")
    parser.add_argument("--timeout", type=float, default=300)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    parent = Path(args.work_dir).expanduser().resolve() if args.work_dir else None
    if parent:
        parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="toolio-", dir=parent))
    print("work-dir:", work, flush=True)
    project = work / "project"
    (project / "src/wzcc").mkdir(parents=True)
    (project / "src/wzcc/toolio").symlink_to(ROOT / "selfhost/src/wzcc/toolio",
                                            target_is_directory=True)
    shutil.copy2(HERE / "lang.toml", project / "lang.toml")
    shutil.copy2(HERE / "src/main.wz", project / "src/main.wz")
    env = dict(os.environ)
    paths = [str(TOOLCHAINS / "llvm-22.1.8/lib"),
             str(TOOLCHAINS / "libxml2-legacy-2.13.9/usr/lib")]
    if env.get("LD_LIBRARY_PATH"):
        paths.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(paths)
    records: list[dict] = []
    compiler = str(Path(args.woozyc).expanduser().resolve())
    cxx = str(Path(args.cxx).expanduser().absolute())
    tool = work / "fixed-tool"
    command([cxx, "-std=c++20", "-O2", str(HERE / "tool.cpp"), "-o", str(tool)],
            "fixture-build", work, args.timeout, env, records)
    command([compiler, "check", "--project", str(project / "lang.toml"),
             "--backend", "native"], "check", work, args.timeout, env, records)
    for opt in (0, 2):
        out = work / f"opt{opt}"
        command([compiler, "build", "--project", str(project / "lang.toml"),
                 "--backend", "native", "--opt", str(opt), "--out-dir", str(out)],
                f"build-O{opt}", work, args.timeout, env, records)
        summary = command([str(out / "program"), str(work / f"O{opt}-all"), str(tool), "all"],
                          f"run-O{opt}", work, args.timeout, env, records)
        if summary != f"TOOLIO passed={EXPECTED} failed=0\n":
            raise RuntimeError(f"unexpected summary: {summary!r}; expected {EXPECTED}")
        for mode in ("low1", "low2", "low3"):
            data = work / f"O{opt}-{mode}"
            command([str(out / "program"), str(data), str(tool), mode],
                    f"run-O{opt}-{mode}", work, args.timeout, env, records)
            if (data / "lowfd-ok").read_text() != "passed":
                raise RuntimeError(f"missing real low-FD result: O{opt}/{mode}")
    (work / "result.md").write_text(
        f"toolio check passed; O0/O2 each {EXPECTED} assertions plus low1/low2/low3 passed.\n"
        "Actual commands, exits, stdout/stderr retained in this directory.\n")
    print(f"PASS toolio: check; O0/O2 each {EXPECTED} assertions + 3 low-FD runs", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError) as error:
        raise SystemExit(str(error))
