#!/usr/bin/env python3
"""Low-RLIMIT cleanup regression for std/fs.copy_dir.

The Wz program runs with RLIMIT_NOFILE soft=64 against a fresh tree deeper than
the preflight descriptor budget but deeper than the copy phase can hold. The
copy must fail with EMFILE, close the open traversal descriptors, and leave the
already-created partial destination in place. No destination path exists before
the run.
"""
import argparse
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import tempfile


def scratch_parent():
    shm = Path("/dev/shm")
    if shm.is_dir() and os.access(shm, os.W_OK):
        return shm
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("mode", choices=("native-o0", "native-o2"))
    parser.add_argument("--libxml")
    args = parser.parse_args()
    hard = resource.getrlimit(resource.RLIMIT_NOFILE)[1]
    if hard != resource.RLIM_INFINITY and hard < 64:
        print(f"SKIP fs.copy_dir low-RLIMIT: hard limit {hard} is below 64",
              file=sys.stderr)
        return 77

    temporary = Path(tempfile.mkdtemp(prefix="woozy-fs-copy-rlimit-",
                                      dir=scratch_parent()))
    try:
        source = temporary / "source"
        path = source
        path.mkdir(parents=True)
        for _ in range(48):
            path = path / "d"
            path.mkdir()
        (temporary / "parent").mkdir()
        destination = temporary / "parent" / "destination"
        if destination.exists():
            raise RuntimeError("destination unexpectedly exists before copy")

        env = dict(os.environ)
        if args.libxml:
            env["LD_LIBRARY_PATH"] = os.pathsep.join(
                filter(None, (args.libxml, env.get("LD_LIBRARY_PATH"))))
        out = temporary / "out"
        subprocess.run([str(args.compiler.resolve()), "build", "--project",
                        str(args.manifest.resolve()), "--backend", "native",
                        "--out-dir", str(out), "--opt", args.mode[-1]],
                       cwd=temporary, env=env, check=True, timeout=180)
        program = out / "program"
        soft = 64
        def limit_fds():
            resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))

        completed = subprocess.run([str(program)], cwd=temporary, env=env,
                                   preexec_fn=limit_fds, capture_output=True,
                                   text=True, timeout=60)
        if completed.returncode != 0:
            print("FAIL fs.copy_dir low-RLIMIT cleanup", file=sys.stderr)
            print(completed.stdout, file=sys.stderr)
            print(completed.stderr, file=sys.stderr)
            return 1
        if "copy_low_rlimit result=24" not in completed.stdout:
            print("FAIL fs.copy_dir low-RLIMIT did not report EMFILE",
                  file=sys.stderr)
            print(completed.stdout, file=sys.stderr)
            return 1
        if "copy_low_rlimit partial\ntrue" not in completed.stdout:
            print("FAIL fs.copy_dir low-RLIMIT lacked partial destination",
                  file=sys.stderr)
            print(completed.stdout, file=sys.stderr)
            return 1
        print(f"fs.copy_dir low-RLIMIT cleanup passed (soft={soft})")
        return 0
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
