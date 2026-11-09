#!/usr/bin/env python3
"""Observe the unmodified generated Source Python module.

Loads ``program.py`` and ``metadata.json`` from ``--generated-dir``, finds the
generated ``visit`` and entry functions by metadata (never by hard-coded symbol
numbers), wraps ``visit`` so the actual received index/values/flips are recorded,
and then calls the original empty ``visit`` before fully executing ``main``.
No Fannkuch, permutation, or flip value is recomputed here.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    directory = args.generated_dir.resolve()
    metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))

    sys.dont_write_bytecode = True
    sys.path.insert(0, str(directory))
    import langrt as rt  # noqa: F401  (imported from the generated directory)
    import program

    symbols = {int(row["symbol"]): row for row in metadata["symbols"]}
    entry_symbol = int(metadata["entry_symbol"])
    if entry_symbol not in symbols or symbols[entry_symbol]["source_name"] != "main":
        raise SystemExit("metadata entry symbol is not main")
    visits = [
        row for row in metadata["symbols"]
        if row["source_name"] == "visit" and row["generated"].startswith("_f")
    ]
    if len(visits) != 1:
        raise SystemExit(f"expected one generated visit function, got {len(visits)}")
    visit_name = visits[0]["generated"]
    original = getattr(program, visit_name)
    calls: list[tuple[int, tuple[int, ...], int]] = []

    def observed(index, values, flips):
        items = getattr(values, "items", values)
        calls.append((int(index), tuple(int(value) + 1 for value in items), int(flips)))
        return original(index, values, flips)

    setattr(program, visit_name, observed)
    result = getattr(program, symbols[entry_symbol]["generated"])()
    if result != 0:
        raise SystemExit(f"generated main returned {result!r}, expected 0")

    for index, permutation, flips in calls:
        print(f"{index}:{','.join(str(value) for value in permutation)}:{flips}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
