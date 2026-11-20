"""Check project test discovery, execution and reporting with a self-host compiler."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def project(directory, backend, source):
    (directory / "src").mkdir(parents=True)
    (directory / "src/main.wz").write_text(source)
    (directory / "lang.toml").write_text(
        'schema_version = 1\n[package]\nname = "fixture"\nsource_root = "src"\n'
        'entry = "app"\n[build]\nbackend = "' + backend + '"\nword_bits = 64\n'
    )


def main():
    compiler = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="woozy-project-test-") as temporary:
        sandbox = Path(temporary)
        root = sandbox / "project"
        launcher = sandbox / "bin/python3.12"
        launcher.parent.mkdir()
        launcher.write_text('#!/bin/sh\nprintf "%s" "$1" > "$WOOZY_TEST_MARKER"\n'
                            'exec "$WOOZY_TEST_PYTHON" "$@"\n')
        launcher.chmod(0o755)
        marker = sandbox / "python-invoked"
        python = shutil.which("python3.12")
        assert python, "python3.12 is required for this integration test"
        env = {**os.environ, "WOOZY_TEST_PYTHON": python, "WOOZY_TEST_MARKER": str(marker),
               "PATH": str(launcher.parent) + os.pathsep + os.environ.get("PATH", "")}
        app = "package app; func main() i32 { return 0; }"
        project(root, "native", app)

        def run(code):
            result = subprocess.run(
                [compiler, "test", "--project", root / "lang.toml", "--format", "json"],
                cwd=sandbox, env=env, capture_output=True, text=True, timeout=180,
            )
            assert result.returncode == code, (result.returncode, result.stdout, result.stderr)
            return json.loads(result.stdout)

        assert run(2)["suites"] == []
        for name, backend, source in (
            ("d_run", "native",
             'package app; import "std/io" as io; import "std/os" as os; '
             'func main() i32 throws os.Error { io.println("run failure"); return 7; }'),
            ("c_build", "native", "invalid source"),
            ("b_python", "python", app),
            ("a native", "native",
             'package app; import "std/fs" as fs; import "std/os" as os; '
             'func main() i32 throws os.Error { if fs.read_text("fixture.txt") == "hello" { '
             'return 0; } return 9; }'),
        ):
            suite = root / "tests" / name
            project(suite, backend, source)
            (suite / "out").mkdir()
            (suite / "out/program").write_text("keep")
            (suite / "fixture.txt").write_text("hello")
        (root / "tests/README.txt").write_text("not a suite")
        before = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
        rows = run(1)["suites"]
        assert [(row["suite"], row["status"]) for row in rows] == [
            ("a native", "passed"), ("b_python", "passed"),
            ("c_build", "build_failed"), ("d_run", "run_failed"),
        ], rows
        assert "E_" in rows[2]["output"]
        assert (rows[3]["exit_code"], rows[3]["output"]) == (7, "run failure\n")
        assert Path(marker.read_text()).name == "program.py"
        assert before == {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
        for name in ("c_build", "d_run"):
            shutil.rmtree(root / "tests" / name)
        assert len(run(0)["suites"]) == 2
    print("project test CLI: passed")


if __name__ == "__main__":
    main()
