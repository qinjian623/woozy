"""Build the race fixture and drive deterministic rename points under ptrace."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


SCENARIOS = ("child-open", "child-rmdir", "root-replace", "root-away")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("mode", choices=("native-o0", "native-o2"))
    parser.add_argument("--harness", type=Path)
    parser.add_argument("--libxml")
    args = parser.parse_args()
    env = dict(os.environ)
    if args.libxml and Path(args.libxml).is_dir():
        env["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, (
            str(Path(args.libxml).resolve()), env.get("LD_LIBRARY_PATH"),
        )))
    with tempfile.TemporaryDirectory(prefix="woozy-stdlib-race-build-") as temporary:
        out = Path(temporary) / "result"
        command = [str(args.compiler.resolve()), "build", "--project",
                   str(args.manifest.resolve()), "--backend", "native",
                   "--out-dir", str(out), "--opt", args.mode[-1]]
        subprocess.run(command, cwd=temporary, env=env, check=True, timeout=180)
        harness = args.harness
        if harness is None:
            harness = Path(temporary) / "race-harness"
            source = Path(__file__).resolve().parent / "race_harness.c"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2",
                            "-Wall", "-Wextra", "-o", str(harness), str(source)],
                           cwd=temporary, env=env, check=True, timeout=60)
        program = out / "program"
        for scenario in SCENARIOS:
            with tempfile.TemporaryDirectory(
                    prefix=f"woozy-stdlib-race-{scenario}-") as work:
                subprocess.run([str(harness.resolve()), str(program), scenario],
                               cwd=work, env=env, check=True, timeout=45)


if __name__ == "__main__":
    main()
