#!/usr/bin/env python3
"""Build the same frozen Woozy compiler sources through successive generations."""

import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy_sources(source, destination):
    destination.mkdir(parents=True)
    records = {}
    for path in sorted(source.rglob("*.wz")):
        relative = path.relative_to(source)
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        records[str(relative)] = digest(target)
    if not records:
        raise RuntimeError(f"no Woozy sources in {source}")
    current = {str(p.relative_to(source)): digest(p) for p in sorted(source.rglob("*.wz"))}
    if current != records:
        raise RuntimeError(f"sources changed while copying {source}")
    return records


def trace_executables(path, allowed):
    observed = set()
    for line in path.read_text().splitlines():
        match = re.search(r'execve\(("(?:[^"\\]|\\.)*")', line)
        if match:
            executable = ast.literal_eval("b" + match[1]).decode(
                sys.getfilesystemencoding(), "surrogateescape")
            observed.add(str(Path(executable).resolve()))
        elif "execveat(" in line:
            raise RuntimeError(f"unhandled execveat in {path}")
    unexpected = observed - allowed
    if unexpected:
        raise RuntimeError(f"unexpected compiler subprocesses: {sorted(unexpected)}")
    if not observed:
        raise RuntimeError(f"no executable recorded in {path}")
    return sorted(observed)


def run(args):
    output = args.out_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise RuntimeError(f"output directory must be empty: {output}")
    source = ROOT / "selfhost/src"
    if output.is_relative_to(source):
        raise RuntimeError("bootstrap output cannot be inside compiler source")
    output.mkdir(parents=True, exist_ok=True)
    report = {"scope": "compiler_build_stages", "stages": [],
              "corpus_parity_checked": False, "status": "running"}
    report_path = output / "result.json"
    try:
        stage0 = args.stage0.resolve(strict=True)
        toolchain = args.toolchain.resolve(strict=True)
        strace = args.strace
        if args.stages > 1 and not strace:
            raise RuntimeError("strace is required to check later-stage subprocesses")
        resources = output / "resources"
        resources.mkdir()
        report["compiler_sources"] = copy_sources(source, output / "source/src")
        shutil.copyfile(ROOT / "selfhost/lang.toml", output / "source/lang.toml")
        report["manifest_sha256"] = digest(output / "source/lang.toml")
        report["stdlib_sources"] = copy_sources(ROOT / "stdlib", resources / "stdlib")
        runtime = resources / "runtime"
        runtime.mkdir()
        for name in ("liblang_native_runtime.a", "liblang_hosted_runtime.a"):
            shutil.copyfile(args.runtime_dir / name, runtime / name)
        (runtime / "python").mkdir()
        for name in ("langrt.py", "unicode18.py"):
            shutil.copyfile(ROOT / "runtime/python" / name, runtime / "python" / name)
        tools = resources / "toolchain"
        tools.mkdir()
        allowed_tools = set()
        for name in ("opt", "llc", "clang", "clang++", "ld.lld"):
            executable = (toolchain / "bin" / name).resolve(strict=True)
            (tools / name).symlink_to(executable)
            allowed_tools.add(str(executable))
        environment = dict(os.environ)
        libraries = [str(toolchain / "lib"), str(args.libxml_dir.resolve())]
        if environment.get("LD_LIBRARY_PATH"):
            libraries.append(environment["LD_LIBRARY_PATH"])
        environment["LD_LIBRARY_PATH"] = ":".join(libraries)
        compiler = stage0
        for generation in range(1, args.stages + 1):
            stage = output / f"stage{generation}"
            stage.mkdir()
            for name in ("stdlib", "runtime", "toolchain"):
                (stage / name).symlink_to(resources / name, target_is_directory=True)
            command = [str(compiler), "build", "--project",
                       str(output / "source/lang.toml"), "--backend", "native",
                       "--opt", args.opt, "--out-dir", str(stage / "build"),
                       "--format", "json"]
            trace = stage / "exec.log"
            traced = command
            if generation > 1:
                traced = [str(strace), "-f", "-qq", "-s", "16384",
                          "-e", "trace=execve,execveat", "-o", str(trace), *command]
            print(f"building stage{generation} with {compiler}", flush=True)
            with (stage / "stdout.log").open("wb") as stdout, \
                 (stage / "stderr.log").open("wb") as stderr:
                result = subprocess.run(traced, cwd=output / "source", env=environment,
                                        stdout=stdout, stderr=stderr, timeout=args.timeout)
            record = {"generation": generation, "compiler": str(compiler),
                      "command": command, "exit": result.returncode}
            report["stages"].append(record)
            if generation > 1:
                record["executables"] = trace_executables(
                    trace, allowed_tools | {str(compiler.resolve())})
            if result.returncode:
                raise RuntimeError(f"stage{generation} failed; see {stage}")
            status = json.loads((stage / "stdout.log").read_text())
            if status.get("status") != "ok":
                raise RuntimeError(f"stage{generation} did not report a successful build")
            binary = stage / "build/program"
            if not binary.is_file() or not os.access(binary, os.X_OK):
                raise RuntimeError(f"stage{generation} did not create an executable")
            compiler = stage / "woozyc"
            shutil.copy2(binary, compiler)
            record["sha256"] = digest(compiler)
            frozen = {str(p.relative_to(output / "source/src")): digest(p)
                      for p in sorted((output / "source/src").rglob("*.wz"))}
            if frozen != report["compiler_sources"]:
                raise RuntimeError("compiler sources changed during bootstrap")
        if args.stages == 3:
            report["stage23_binary_equal"] = digest(output / "stage2/woozyc") == \
                digest(output / "stage3/woozyc")
        if args.stages == 3 and not report["stage23_binary_equal"]:
            raise RuntimeError("stage2 and stage3 binaries differ; inspect the generated IR")
        report["status"] = "ok"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print(f"bootstrap result: {report_path}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage0", type=Path,
                        default=ROOT / "build/woozy-stdlib-native/woozyc")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--stages", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--opt", choices=("0", "2"), default="2")
    parser.add_argument("--toolchain", type=Path,
                        default=ROOT.parent / "toolchains/llvm-22.1.8")
    parser.add_argument("--runtime-dir", type=Path,
                        default=ROOT / "build/woozy-stdlib-native/native")
    parser.add_argument("--libxml-dir", type=Path,
                        default=ROOT.parent / "toolchains/libxml2-legacy-2.13.9/usr/lib")
    parser.add_argument("--strace", type=Path,
                        default=shutil.which("strace"))
    parser.add_argument("--timeout", type=float, default=7200)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    try:
        run(args)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
