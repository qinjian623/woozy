#!/usr/bin/env python3
"""Controlled post-preflight target-move race for std/fs.copy_dir.

The Wz program copies fixture ``source`` to ``parent/destination``. As soon as
the destination root appears, this harness stops the whole process with
SIGSTOP, moves ``parent`` under ``source/aaa/zzz/moved``, and resumes it with
SIGCONT. That makes the move happen after copy_dir's bounded preflight and
before the source scanner reaches zzz. The target-root pin must return EINVAL,
leave the partial destination at the moved path, and leave the process fd count
unchanged.

The fixture prefers /dev/shm (tmpfs), where readdir is reverse-insertion order:
zzz is created first and the large/filler entries after it, so zzz is scanned
last. The script checks that ordering before running and refuses to run with a
misleading fixture on another filesystem.
"""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def wait_stopped(pid, deadline):
    status = Path(f"/proc/{pid}/status")
    while time.monotonic() < deadline:
        try:
            text = status.read_text(errors="replace")
        except OSError:
            return False
        for line in text.splitlines():
            if line.startswith("State:"):
                if "T" in line.split(":", 1)[1]:
                    return True
                break
        time.sleep(0.001)
    return False


def scratch_parent():
    shm = Path("/dev/shm")
    if shm.is_dir() and os.access(shm, os.W_OK):
        return shm
    return None


def run_attempt(compiler, manifest, mode, libxml):
    temporary = Path(tempfile.mkdtemp(prefix="woozy-fs-copy-race-",
                                      dir=scratch_parent()))
    try:
        source = temporary / "source"
        aaa = source / "aaa"
        zzz = aaa / "zzz"
        parent = temporary / "parent"
        aaa.mkdir(parents=True)
        zzz.mkdir()
        names = []
        for index in range(1000):
            path = aaa / f"f{index:04d}"
            path.write_bytes(b"race")
            names.append(path.name)
        slow = aaa / "slow.bin"
        with slow.open("wb") as stream:
            stream.truncate(64 * 1024 * 1024)
        names.append(slow.name)
        # tmpfs readdir is reverse insertion order; zzz must be last.
        if os.listdir(aaa)[-1] != "zzz":
            return "fixture-order", None
        parent.mkdir()

        out = temporary / "out"
        env = dict(os.environ)
        if libxml:
            env["LD_LIBRARY_PATH"] = os.pathsep.join(
                filter(None, (libxml, env.get("LD_LIBRARY_PATH"))))
        command = [str(compiler.resolve()), "build", "--project",
                   str(manifest.resolve()), "--backend", "native",
                   "--out-dir", str(out), "--opt", mode[-1]]
        subprocess.run(command, cwd=temporary, env=env, check=True, timeout=180)
        program = out / "program"
        process = subprocess.Popen([str(program)], cwd=temporary, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True)

        destination = parent / "destination"
        moved = zzz / "moved"
        deadline = time.monotonic() + 30.0
        stopped = False
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            if destination.is_dir():
                try:
                    process.send_signal(signal.SIGSTOP)
                    stopped = wait_stopped(process.pid, deadline)
                except ProcessLookupError:
                    stopped = False
                if stopped:
                    parent.rename(moved)
                    process.send_signal(signal.SIGCONT)
                break
            time.sleep(0.001)
        try:
            stdout, stderr = process.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
            return "timeout", (stdout, stderr)
        if not stopped:
            return "missed-stop", (stdout, stderr)
        if process.returncode != 0:
            return f"program-exit-{process.returncode}", (stdout, stderr)
        if "copy_race partial\n" not in stdout or "\ntrue\n" not in stdout:
            return "partial-check", (stdout, stderr)
        return None, (stdout, stderr)
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("mode", choices=("native-o0", "native-o2"))
    parser.add_argument("--libxml")
    args = parser.parse_args()
    last = None
    for attempt in range(3):
        problem, output = run_attempt(args.compiler, args.manifest, args.mode,
                                      args.libxml)
        if problem is None:
            print(f"fs.copy_dir target-move guard passed (attempt {attempt + 1})")
            return 0
        last = (problem, output)
        if problem in ("fixture-order",):
            break
    problem, output = last
    print(f"FAIL fs.copy_dir target-move guard: {problem}", file=sys.stderr)
    if output:
        for stream in output:
            if stream:
                print(stream, file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
