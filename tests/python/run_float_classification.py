"""Exercise the emitted f64 function on Python float subclasses."""

import json
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile


class HostFloat(float):
    def __eq__(self, other):
        raise AssertionError("classification invoked host equality")

    def __ne__(self, other):
        raise AssertionError("classification invoked host inequality")


def check_program(program, generated):
    sys.path.insert(0, str(program.parent))
    try:
        classify = runpy.run_path(str(program))[generated]
        cases = (
            ("nan", 1), ("-nan", 1), ("inf", 2), ("-inf", 2),
            ("0.0", 3), ("-0.0", 3), ("5e-324", 3), ("-42.0", 3),
        )
        for value, expected in cases:
            actual = classify(HostFloat(value))
            assert actual == expected, (value, expected, actual)
    finally:
        sys.path.pop(0)


def main():
    helper, fixture = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="woozy-float-classification-") as directory:
        out = Path(directory)
        subprocess.run([helper, "--project", fixture, "--out-dir", out],
                       check=True, timeout=90)
        metadata = json.loads((out / "metadata.json").read_text())
        generated, = (row["generated"] for row in metadata["symbols"]
                      if row["source_name"] == "classify")
        check_program(out / "program.py", generated)


if __name__ == "__main__":
    main()
