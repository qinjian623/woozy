#!/usr/bin/env python3
"""Build the real Wz CLI, lexer tests and diagnostic tests with stage0."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(command, expected=0):
    result = subprocess.run([str(item) for item in command], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=1800)
    if result.returncode != expected:
        raise RuntimeError(f"{command}: exit {result.returncode}, expected {expected}\n"
                           f"{result.stdout}\n{result.stderr}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage0", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    work = (args.work_dir or Path(tempfile.mkdtemp(prefix="woozy-selfhost-smoke-"))).resolve()
    work.mkdir(parents=True, exist_ok=True)
    stage0 = args.stage0.resolve()
    for opt in (0, 2):
        programs = {}
        for name, project in (("cli", ROOT / "selfhost"),
                              ("lexer", ROOT / "selfhost/tests/lexer"),
                              ("render", ROOT / "selfhost/tests/render")):
            output = work / f"{name}-o{opt}"
            run([stage0, "build", "--project", project / "lang.toml",
                 "--backend", "native", "--opt", opt, "--out-dir", output])
            programs[name] = output / "program"
        run([programs["lexer"]])
        rendered = json.loads(run([programs["render"]]).stdout)
        diagnostic = rendered["diagnostics"][0]
        assert rendered["status"] == "error", rendered
        assert diagnostic["message"] == 'line\n"quoted"', diagnostic
        assert diagnostic["primary"]["path"] == 'quote"/中文.wz', diagnostic
        assert diagnostic["notes"] == ["nul:\0"], diagnostic
        assert diagnostic["expected_type"] == "i32", diagnostic
        assert len(diagnostic["related"]) == 1 and len(diagnostic["fixits"]) == 1
        cli = programs["cli"]
        assert "selfhost" in run([cli, "--version"]).stdout
        run([cli, "--help"])
        for arguments in ([], ["unknown"], ["--help", "extra"],
                          ["--version", "extra"], ["check"], ["build"],
                          ["check", "--format"], ["check", "--format", "text"],
                          ["check", "--project", ""],
                          ["check", "--project", "-bad"],
                          ["check", "--syntax-only", "one", "two"],
                          ["build", "--syntax-only", "one", "--out-dir", "out"],
                          ["check", "--project", "one", "--project", "two"],
                          ["build", "--project", "one", "--out-dir", "out", "--opt", "3"],
                          ["capabilities", "--format", "json"]):
            expected = run([stage0, *arguments], expected=1 if arguments and
                           arguments[0] == "capabilities" else 2)
            actual = run([cli, *arguments], expected=expected.returncode)
            assert (actual.stdout, actual.stderr) == (expected.stdout, expected.stderr), arguments
        editor = run([sys.executable, ROOT / "tests/tooling/editor_cli_test.py", "--cli", cli])
        semantics = run([sys.executable, ROOT / "tests/tooling/editor_semantics_test.py", "--cli", cli])
        run([cli, "check", "--syntax-only"], expected=2)
        missing = json.loads(run([cli, "check", "--syntax-only", "--format", "json",
                                  work / "missing.wz"], expected=2).stdout)
        assert missing["diagnostics"][0]["code"] == "E_INPUT_IO", missing
        syntax = run([sys.executable, ROOT / "tests/syntax/run_cases.py", cli])
        print(f"O{opt}: lexer, JSON rendering, CLI arguments/I/O and syntax cases passed")
        print(editor.stdout.strip())
        print(semantics.stdout.strip())
        print(syntax.stdout.strip())
        if sys.platform.startswith("linux"):
            elf = run(["readelf", "-l", "-d", cli]).stdout
            assert "INTERP" not in elf and "NEEDED" not in elf, elf
    print("PASS: selfhost source smoke O0/O2 (project/type checking and build not covered)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
