#!/usr/bin/env python3
"""GDB checks for aggregate values, scopes, captures and suspended locals."""

import argparse
from pathlib import Path
import re
import shutil
import tempfile

from run import run


HERE = Path(__file__).resolve().parent
PRINTER = HERE.parents[2] / "tools/gdb/woozy.py"


def project(root, name):
    path = root / name
    source = path / "src" / f"{name}.wz"
    source.parent.mkdir(parents=True)
    shutil.copyfile(HERE / source.name, source)
    manifest = path / "lang.toml"
    manifest.write_text(
        'schema_version=1\n[package]\nname="debug_fixture"\nsource_root="src"\n'
        'entry="app"\n[build]\nbackend="native"\nword_bits=64\n'
    )
    return manifest, source


def debugger(args, executable, source, *commands):
    options = ["set disable-randomization off", "set debuginfod enabled off",
               "set print sevenbit-strings off", f"source {PRINTER}"]
    lines = source.read_text().splitlines()
    for marker, _ in commands:
        line, = [index for index, text in enumerate(lines, 1) if "// " + marker in text]
        options.append(f"break {source.name}:{line}")
    options.append("run")
    for index, (_, expressions) in enumerate(commands):
        if index:
            options.append("continue")
        options.extend(f"print {expression}" for expression in expressions)
    options.append("bt")
    return run(args.gdb, "-q", "-nx", "-batch", executable,
               *(argument for option in options for argument in ("-ex", option)),
               cwd=source.parent.parent)


def values(args, executable, source):
    output = debugger(
        args, executable, source,
        ("BREAK_VALUES", ("point.x", "point.y", "payload.item.y", "numbers",
                          "window", "word", "choice", "seed")),
        ("BREAK_SHADOW", ("seed",)),
        ("BREAK_ITERATION", ("item", "seed")),
        ("BREAK_CATCH", ("caught",)),
        ("BREAK_PATTERN", ("value",)),
        ("BREAK_CAPTURE", ("total", "delta")),
        ("BREAK_GENERIC", ("crate.item.y", "item.x")),
        ("BREAK_METHOD", ("self.y",)),
        ("BREAK_BEFORE_MOVE", ("input",)),
        ("BREAK_AFTER_MOVE", ("input", "moved")),
    )
    for name in ("BREAK_VALUES", "BREAK_SHADOW", "BREAK_ITERATION", "BREAK_CATCH",
                 "BREAK_PATTERN", "BREAK_CAPTURE", "BREAK_GENERIC", "BREAK_METHOD",
                 "BREAK_BEFORE_MOVE", "BREAK_AFTER_MOVE"):
        assert name in output, output
    expected = ["5", "80", "80", "5", "9", "4", "5", "13", "22", "5", "80", "5", "80"]
    assert re.findall(r"^\$\d+ = (\d+)$", output, re.M) == expected, output
    assert re.search(r"length 3[^\n]*\{4,\s*9,\s*11\}", output), output
    assert re.search(r"length 2[^\n]*\{9,\s*11\}", output), output
    assert '"你好"' in output and re.search(r"^\$\d+ = .*\bNumber\b", output, re.M), output
    assert re.search(r"^\$\d+ = .*\bBad\b.*\b21\b", output, re.M), output
    assert re.search(r"^\$\d+ = length 1 = \{7\}\n"
                     r"\n.*\n.*BREAK_AFTER_MOVE[^\n]*\n"
                     r"\$\d+ = <optimized out>\n\$\d+ = length 1 = \{7\}$",
                     output, re.M), output
    print(output, end="")


def resumed(args, executable, source):
    output = debugger(args, executable, source, ("BREAK_RESUMED", ("seed", "before")))
    assert "BREAK_RESUMED" in output, output
    assert re.findall(r"^\$\d+ = (\d+)$", output, re.M) == ["40", "41"], output
    print(output, end="")


def deferred(args, root):
    manifest, source = project(root, "deferred")
    out = root / "deferred-program"
    run(args.cli, "build", "--project", manifest, "--out-dir", out, "--opt", "0", "-g")
    executable = out / "program"
    run(args.dwarfdump, "--verify", executable)
    run(executable, status=42)
    line, = [index for index, text in enumerate(source.read_text().splitlines(), 1)
             if "// BREAK_DEFER" in text]
    # A single GDB line breakpoint can select only the first expansion.
    output = run(
        args.gdb, "-q", "-nx", "-batch", executable,
        "-ex", "set disable-randomization off", "-ex", "set debuginfod enabled off",
        "-ex", f"python site = gdb.decode_line('deferred.wz:{line}')[1][0]",
        "-ex", f"python points = {{row.pc for row in site.symtab.linetable().line({line})}}",
        "-ex", "python [gdb.Breakpoint('*' + hex(pc)) for pc in sorted(points)]",
        "-ex", "run", "-ex", "print item", "-ex", "print total",
        "-ex", "continue", "-ex", "print item", "-ex", "print total",
        cwd=source.parent.parent,
    )
    assert output.count("BREAK_DEFER") == 2, output
    assert re.findall(r"^\$\d+ = (\d+)$", output, re.M) == ["11", "0", "21", "11"], output
    print(output, end="")


def thread(args, root):
    manifest = HERE.parent / "concurrency/docs-thread/lang.toml"
    out = root / "thread"
    run(args.cli, "build", "--project", manifest, "--out-dir", out, "--opt", "0", "-g")
    executable = out / "program"
    run(args.dwarfdump, "--verify", executable)
    output = run(
        args.gdb, "-q", "-nx", "-batch", executable,
        "-ex", "set disable-randomization off", "-ex", "set debuginfod enabled off",
        "-ex", "break calculate", "-ex", "run", "-ex", "info threads",
        "-ex", "print value", "-ex", "continue",
        cwd=manifest.parent,
    )
    assert re.search(r"Thread 2 .*hit Breakpoint .*calculate", output), output
    assert re.search(r"^\$1 = 21$", output, re.M) and "exited normally" in output, output
    print(output, end="")


def check(args, root):
    for name, inspect in (("values", values), ("await", resumed)):
        manifest, source = project(root, name)
        modes = [(), ("--package-objects",)]
        if name == "values" and args.selfhost:
            modes.append(("--source-make",))
        for mode in modes:
            label = mode[0][2:] if mode else "whole"
            for level in ("0", "2"):
                out = root / f"{name}-{label}-{level}"
                run(args.cli, "build", "--project", manifest, "--out-dir", out,
                    "--opt", level, "-g", *mode)
                executable = out / "program"
                run(args.dwarfdump, "--verify", executable)
                run(executable, status=42)
                if level == "0":
                    inspect(args, executable, source)
                print(f"{name} {label} O{level}: verified", flush=True)
    deferred(args, root)
    thread(args, root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cli", "gdb", "dwarfdump"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--selfhost", action="store_true")
    with tempfile.TemporaryDirectory(prefix="woozy-full-debug-") as directory:
        check(parser.parse_args(), Path(directory))


if __name__ == "__main__":
    main()
