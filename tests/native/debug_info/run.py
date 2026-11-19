#!/usr/bin/env python3
"""Exercise -g through the real native compiler, linker, and GDB."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile


def run(*args, status=0, cwd=None):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=90)
    if result.returncode != status:
        raise AssertionError((args, result.returncode, result.stdout, result.stderr))
    return result.stdout + result.stderr


def debug_program(args, executable, line, project):
    output = run(
        args.gdb, "-q", "-nx", "-batch", executable,
        "-ex", "set disable-randomization off",
        "-ex", "set debuginfod enabled off",
        "-ex", f"break values.wz:{line}",
        "-ex", "run",
        "-ex", "backtrace",
        "-ex", "print seed",
        "-ex", "print answer",
        "-ex", "next",
        "-ex", "print answer",
        cwd=project,
    )
    assert f"values.wz:{line}" in output and "answer += 2;" in output, output
    assert re.search(r"#0\s+(?:0x[0-9a-f]+ in )?compute\s*\(", output), output
    assert re.search(r"#1\s+.* at .*main\.wz:", output), output
    assert not re.search(r"\.wz:0\b", output), output
    assert not re.search(r"#2[^\n]*\.wz:\d+", output), output
    assert re.findall(r"^\$\d+ = (\d+)$", output, re.MULTILINE) == ["39", "40", "42"], output
    print(output, end="")


def check(args, root):
    project = root / "project with spaces"
    source = project / "src" / "values"
    source.mkdir(parents=True)
    values = source / "values.wz"
    values.write_text(
        "package values;\n"
        "pub func compute(seed i32) i32 {\n"
        "    var answer i32 = seed + 1;\n"
        "    answer += 2;\n"
        "    return answer;\n"
        "}\n"
    )
    (project / "src" / "main.wz").write_text(
        'package app;\nimport "app/values" as values;\nimport "std/os" as os;\n\n'
        "func main() i32 {\n"
        "    var seed i32 = 39;\n"
        "    if os.getpid() <= 0 { seed = 11; }\n"
        "    return values.compute(seed);\n"
        "}\n"
    )
    manifest = project / "lang.toml"
    manifest.write_text(
        'schema_version=1\n[package]\nname="debug_fixture"\nsource_root="src"\n'
        'entry="app"\n[build]\nbackend="native"\nword_bits=64\n'
    )
    build = (args.cli, "build", "--project", manifest)
    assert "E_DRIVER_ARGUMENT" in run(args.cli, "check", "--project", manifest, "-g", status=2)
    assert "E_DRIVER_ARGUMENT" in run(*build, "--backend", "python", "--out-dir", root / "python", "-g", status=2)
    python_manifest = project / "python.toml"
    python_manifest.write_text(manifest.read_text().replace('backend="native"', 'backend="python"'))
    assert "E_DRIVER_ARGUMENT" in run(
        args.cli, "build", "--project", python_manifest, "--out-dir", root / "python", "-g", status=2,
    )

    modes = [(), ("--package-objects",)]
    if args.selfhost:
        modes.append(("--source-make",))
    for mode in modes:
        label = mode[0][2:] if mode else "whole"
        out = root / label
        executable = out / "program"
        command = (*build, "--out-dir", out, *mode)
        if mode:
            command += ("--cache-dir", root / (label + "-cache"))

        def stats():
            return json.loads((out / "cache-statistics.json").read_text())

        run(*command, "--opt", "0")
        package_count = stats()["opt_jobs"] if mode else 0
        if mode:
            assert package_count >= 2
        plain_lines = run(args.dwarfdump, "--debug-line", executable)
        assert "values.wz" not in plain_lines, plain_lines
        run(executable, status=42)

        run(*command, "--opt", "0", "-g")
        if mode:
            assert stats()["object_hits"] == 0
        run(args.dwarfdump, "--verify", executable)
        debug_program(args, executable, 4, project)
        run(executable, status=42)

        if mode:
            run(*command, "--opt", "0", "-g")
            assert stats()["object_hits"] == package_count
            debug_program(args, executable, 4, project)
        if mode == ("--source-make",):
            original = values.read_text()
            values.write_text(original.replace("    var answer", "\n    var answer"))
            run(*command, "--opt", "0", "-g")
            assert stats()["object_hits"] == 0
            debug_program(args, executable, 5, project)
            values.write_text(original)

        run(*command, "--opt", "2", "-g")
        run(args.dwarfdump, "--verify", executable)
        lines = run(args.dwarfdump, "--debug-line", executable)
        assert "values.wz" in lines and "main.wz" in lines, lines
        dump = run(args.dwarfdump, "--debug-info", "--debug-line", executable)
        assert "src/values/values.wz" in dump and "src/main.wz" in dump, dump
        assert not re.search(r'"(/[^"]+)"', dump), dump
        run(executable, status=42)
        print(f"{label}: -g checks passed")

    manifest = Path(__file__).resolve().parents[2] / "async/repeat-await/lang.toml"
    out = root / "async"
    for opt in ("0", "2"):
        run(args.cli, "build", "--project", manifest, "--out-dir", out, "--opt", opt, "-g")
        run(args.dwarfdump, "--verify", out / "program")
        assert run(out / "program") == "42\n42\n"
    print("async: -g checks passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cli", "gdb", "dwarfdump"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--selfhost", action="store_true")
    with tempfile.TemporaryDirectory(prefix="woozy-debug-") as directory:
        check(parser.parse_args(), Path(directory))


if __name__ == "__main__":
    main()
