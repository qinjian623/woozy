"""Compile and run a standard-library API fixture."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("mode", choices=("native-o0", "native-o2", "python"))
    parser.add_argument("--libxml")
    parser.add_argument("--panic", help="expected Native Panic reason")
    parser.add_argument("--expect-panic")
    args = parser.parse_args()
    env = dict(os.environ)
    if args.libxml and Path(args.libxml).is_dir():
        env["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, (
            str(Path(args.libxml).resolve()), env.get("LD_LIBRARY_PATH"),
        )))
    native = args.mode.startswith("native")
    if args.panic and not native:
        parser.error("--panic requires a Native mode")
    backend = "native" if native else "python"
    with tempfile.TemporaryDirectory(prefix="woozy-stdlib-") as temporary:
        out = Path(temporary) / "result"
        command = [args.compiler.resolve(), "build", "--project", args.manifest.resolve(),
                   "--backend", backend, "--out-dir", out]
        if native:
            command.extend(("--opt", args.mode[-1]))
        subprocess.run(command, cwd=temporary, env=env, check=True, timeout=180)
        run = [out / "program"] if native else [sys.executable, "-B", out / "program.py"]
        if args.panic:
            result = subprocess.run(run, cwd=out, env=env, text=True,
                                    capture_output=True, timeout=10)
            marker = f"LANG_NATIVE_PANIC:v1:{args.panic}:"
            if result.returncode != 101 or marker not in result.stderr:
                raise SystemExit(f"Expected {marker}; exit={result.returncode}\n{result.stderr}")
        elif args.expect_panic:
            result = subprocess.run(run, cwd=out, env=env, capture_output=True,
                                    text=True, timeout=45)
            if (result.returncode != 101 or result.stdout or
                    args.expect_panic not in result.stderr):
                raise AssertionError((run, result.returncode, result.stdout, result.stderr))
        else:
            subprocess.run(run, cwd=out, env=env, check=True, timeout=45)


if __name__ == "__main__":
    main()
