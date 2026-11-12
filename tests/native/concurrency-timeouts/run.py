#!/usr/bin/env python3
"""Exercise the public Wz thread/condvar timed waits in real O0/O2 executables."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, cwd, env, timeout):
    begin = time.monotonic_ns()
    try:
        result = subprocess.run([str(x) for x in argv], cwd=cwd, env=env,
                                stdin=subprocess.DEVNULL, capture_output=True,
                                timeout=timeout)
        return dict(argv=[str(x) for x in argv], status="exited",
                    exit=result.returncode,
                    stdout=result.stdout.decode("utf-8", "backslashreplace"),
                    stderr=result.stderr.decode("utf-8", "backslashreplace"),
                    elapsed_ns=time.monotonic_ns() - begin)
    except subprocess.TimeoutExpired as error:
        return dict(argv=[str(x) for x in argv], status="timeout", exit=None,
                    stdout=(error.stdout or b"").decode("utf-8", "backslashreplace"),
                    stderr=(error.stderr or b"").decode("utf-8", "backslashreplace"),
                    elapsed_ns=time.monotonic_ns() - begin)


def require_success(row):
    assert row["status"] == "exited" and row["exit"] == 0, row
    assert not row["stderr"], row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("cli", "root", "out"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--repeat", type=int, default=2)
    args = parser.parse_args()
    args.cli, args.root, args.out = (p.resolve() for p in
                                     (args.cli, args.root, args.out))
    if args.repeat < 1 or args.repeat > 20:
        parser.error("--repeat must be between 1 and 20")
    args.out.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    legacy = args.root.parent / "toolchains/libxml2-legacy-2.13.9/usr/lib"
    if legacy.is_dir():
        env["LD_LIBRARY_PATH"] = str(legacy) + (
            ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    source = args.root / "tests/native/concurrency-timeouts"
    inputs = [source / "project.toml", source / "src/main.wz",
              source / "run.py", args.root / "stdlib/thread/thread.wz",
              args.root / "stdlib/sync/sync.wz"]
    report = dict(status="failed", cli=str(args.cli), cli_sha256=sha(args.cli),
                  input_sha256={str(p): sha(p) for p in inputs}, runs=[])
    try:
        for opt in (0, 2):
            out = args.out / f"O{opt}"
            row = dict(opt=opt, executions=[])
            report["runs"].append(row)
            row["build"] = command([
                args.cli, "build", "--project", source / "project.toml",
                "--backend", "native", "--opt", str(opt), "--out-dir", out,
                "--format", "json"], args.root, env, 120)
            require_success(row["build"])
            assert json.loads(row["build"]["stdout"])["status"] == "ok"
            program = out / "program"
            row["program_sha256"] = sha(program)
            row["object_sha256"] = sha(out / "program.o")
            row["elf"] = command(["readelf", "-l", "-d", program], out, env, 10)
            require_success(row["elf"])
            assert "INTERP" not in row["elf"]["stdout"]
            assert "NEEDED" not in row["elf"]["stdout"]
            row["undefined"] = command(["nm", "-u", program], out, env, 10)
            require_success(row["undefined"])
            assert all(line.strip().startswith("w ") for line in
                       row["undefined"]["stdout"].splitlines())
            for _ in range(args.repeat):
                result = command([program], out, env, 20)
                row["executions"].append(result)
                require_success(result)
                assert not result["stdout"], result
        assert report["cli_sha256"] == sha(args.cli), "CLI changed during tests"
        assert all(sha(Path(p)) == h for p, h in report["input_sha256"].items()), \
            "input changed during tests"
        report["status"] = "passed"
    finally:
        (args.out / "summary.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(f"{report['status']}: 2 public-interface O0/O2 builds, "
          f"{2 * args.repeat} executions, static ELF")


if __name__ == "__main__":
    main()
