"""Build the low-limit fixture and run it under a small RLIMIT_NOFILE."""

import argparse
import os
from pathlib import Path
import resource
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("mode", choices=("native-o0", "native-o2"))
    parser.add_argument("--limit", type=int, default=64)
    parser.add_argument("--libxml")
    args = parser.parse_args()
    env = dict(os.environ)
    if args.libxml and Path(args.libxml).is_dir():
        env["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, (
            str(Path(args.libxml).resolve()), env.get("LD_LIBRARY_PATH"),
        )))
    with tempfile.TemporaryDirectory(prefix="woozy-stdlib-low-") as temporary:
        out = Path(temporary) / "result"
        command = [str(args.compiler.resolve()), "build", "--project",
                   str(args.manifest.resolve()), "--backend", "native",
                   "--out-dir", str(out), "--opt", args.mode[-1]]
        subprocess.run(command, cwd=temporary, env=env, check=True, timeout=180)

        def lower_limit():
            resource.setrlimit(resource.RLIMIT_NOFILE,
                               (args.limit, args.limit))

        subprocess.run([str(out / "program")], cwd=out, env=env,
                       preexec_fn=lower_limit, check=True, timeout=45)


if __name__ == "__main__":
    main()
