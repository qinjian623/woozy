"""Smoke-test an installed Woozy bundle with no toolchain overrides."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    clean_env = {name: value for name, value in os.environ.items()
                 if name not in {"LD_LIBRARY_PATH", "LD_PRELOAD", "WOOZY_LLVM_BIN"}}

    with tempfile.TemporaryDirectory(prefix="woozy-bundle-test-") as temporary:
        directory = Path(temporary)

        def run(command):
            result = subprocess.run(command, cwd=directory, env=clean_env, timeout=90,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if result.returncode:
                raise SystemExit(f"{command} exited {result.returncode}\n{result.stdout}")
            return result.stdout

        native = directory / "native"
        run([compiler, "build", "--project", ROOT / "examples/hello/lang.toml",
             "--backend", "native", "--out-dir", native])
        if run([native / "program"]) != "Hello, world!\n":
            raise SystemExit("Unexpected Hello World output")
        run([compiler, "check", "--project", ROOT / "examples/http_server/lang.toml",
             "--backend", "native"])

        python = directory / "python"
        iterator = ROOT / "tests/iterator/fixtures/concrete-counter/lang.toml"
        run([compiler, "build", "--project", iterator, "--backend", "python", "--out-dir", python])
        run([sys.executable, python / "program.py"])
    print("Installed Woozy bundle passed Native Hello World, HTTP project check, and Python Iterator.")


if __name__ == "__main__":
    main()
