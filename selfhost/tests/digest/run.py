#!/usr/bin/env python3
"""Build the isolated digest test project with stage0 at O0/O2 and check hashes."""

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HEX64 = re.compile(r"^[0-9a-f]{64}$")
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def pattern(length):
    return bytes((index * 37 + 11) % 256 for index in range(length))


def expected_hashes():
    cases = {
        "empty": b"",
        "abc": b"abc",
        "nul": b"\x00",
        "nul-embedded": b"a\x00b",
        "invalid-utf8": bytes([0xFF, 0xFE, 0x80, 0xC0, 0xAF]),
        "multiblock": pattern(1024),
        "million-a": b"a" * 1_000_000,
    }
    for length in (55, 56, 63, 64, 65, 119, 120, 127, 128, 129):
        cases[f"len{length}"] = pattern(length)
    return {name: hashlib.sha256(data).hexdigest() for name, data in cases.items()}


def run(command, env, cwd):
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=600,
    )
    print(f"$ {' '.join(command)}")
    if completed.returncode != 0:
        print(completed.stdout, end="")
        print(completed.stderr, end="", file=sys.stderr)
    return completed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--woozyc", type=Path, default=Path(os.environ.get("WOOZYC", ROOT / "build/woozy-stdlib-native/woozyc")))
    args = parser.parse_args()
    env = os.environ.copy()
    expected = expected_hashes()
    failures = []
    outputs = {}
    manifest = HERE / "lang.toml"
    with tempfile.TemporaryDirectory(prefix="woozy-digest-") as temporary:
        root = Path(temporary)
        for opt in (0, 2):
            out_dir = root / f"o{opt}"
            build = run(
                [
                    str(args.woozyc.resolve()),
                    "build",
                    "--project",
                    str(manifest),
                    "--backend",
                    "native",
                    "--opt",
                    str(opt),
                    "--out-dir",
                    str(out_dir),
                    "--format",
                    "json",
                ],
                env,
                HERE,
            )
            if build.returncode != 0:
                failures.append(f"O{opt} build exit {build.returncode}")
                continue
            binary = out_dir / "program"
            if not binary.is_file():
                failures.append(f"O{opt} build did not produce {binary}")
                continue
            executed = run([str(binary)], env, HERE)
            if executed.returncode != 0:
                failures.append(f"O{opt} run exit {executed.returncode}")
                continue
            outputs[opt] = executed.stdout

    for opt, stdout in outputs.items():
        hashes = {}
        aliases = {}
        for line in stdout.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[0] == "HASH":
                hashes[parts[1]] = parts[2]
            elif len(parts) == 3 and parts[0] == "ALIAS":
                aliases[parts[1]] = parts[2]
        for name, want in expected.items():
            got = hashes.get(name)
            if got is None:
                failures.append(f"O{opt} missing hash {name}")
            elif got != want:
                failures.append(f"O{opt} hash {name}: {got} != {want}")
            elif not HEX64.match(got):
                failures.append(f"O{opt} hash {name} is not lowercase 64 hex")
            if aliases.get(name) != "ok":
                failures.append(f"O{opt} alias {name}: {aliases.get(name)!r}")
        if len(hashes) != len(expected):
            failures.append(f"O{opt} hash count {len(hashes)} != {len(expected)}")
        if len(aliases) != len(expected):
            failures.append(f"O{opt} alias count {len(aliases)} != {len(expected)}")

    if 0 in outputs and 2 in outputs and outputs[0] != outputs[2]:
        failures.append("O0 and O2 output differ")

    total = len(expected) * len(outputs)
    print(f"cases={len(expected)} opt_levels={len(outputs)} checks={total} failures={len(failures)} not_run={2-len(outputs)}")
    for failure in failures:
        print(f"FAIL {failure}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
