#!/usr/bin/env python3
"""Test the released Woozy archive and the tools it contains on Apple Silicon."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile

from build_release import METADATA_NAME, checksum_text, metadata_text
from install_toolchain import DARWIN_RUNTIME, ROOT, host_target

sys.path.insert(0, str(ROOT / "tests/native"))
from run_chat import Chat, exercise


def run(argv, *, cwd, timeout=180):
    subprocess.run(argv, cwd=cwd, timeout=timeout, check=True)


def check_chat(compiler, work):
    examples = ROOT / "examples/chat"
    for opt in (0, 2):
        programs = {}
        for role in ("server", "client"):
            output = work / f"chat-{opt}-{role}"
            run([compiler, "build", "--project", examples / f"{role}.toml",
                 "--opt", str(opt), "--out-dir", output], cwd=work)
            programs[role] = output / "program"
        chat = Chat(work / f"chat-{opt}-logs")
        cases = []
        try:
            exercise(chat, programs["server"], programs["client"], opt, cases)
        finally:
            chat.cleanup()
        print(f"Chat O{opt}: {len(cases)} cases passed", flush=True)


def verify(release, version, commit):
    if host_target() != "macos-arm64":
        raise ValueError("The release must be tested on macOS arm64")
    archive = "woozy-macos-arm64.tar.gz"
    checksum = release / (archive + ".sha256")
    if checksum.read_text(encoding="ascii") != checksum_text(release, (archive,)):
        raise ValueError("macOS release checksum differs")
    metadata = metadata_text(version, commit)
    for name in tuple(os.environ):
        if (name.startswith(("DYLD_", "WOOZY_")) or name in
                {"LD_LIBRARY_PATH", "LD_PRELOAD", "SDKROOT", "MAKEFLAGS", "GNUMAKEFLAGS",
                 "MFLAGS", "MAKEFILES", "MAKEOVERRIDES", "MAKELEVEL", "MAKE_RESTARTS"}):
            del os.environ[name]

    with tempfile.TemporaryDirectory(prefix="woozy-macos-release-") as temporary:
        work = Path(temporary)
        with tarfile.open(release / archive, "r:gz") as content:
            content.extractall(work, filter="data")
        bundle = work / "woozy"
        if (bundle / METADATA_NAME).read_text(encoding="ascii") != metadata:
            raise ValueError("macOS package has unexpected release metadata")
        for name in ("LICENSE", "tools/lldb/woozy.py"):
            if (bundle / name).read_bytes() != (ROOT / name).read_bytes():
                raise ValueError(f"macOS package has an unexpected {name}")
        if sorted(path.name for path in (bundle / "runtime").glob("*.a")) != [DARWIN_RUNTIME]:
            raise ValueError("The macOS archive must contain the Darwin runtime archive")
        compiler = bundle / "woozyc"
        release_version = metadata.splitlines()[0].removeprefix("version=")
        reported = subprocess.check_output([compiler, "--version"], cwd=work,
                                           timeout=15, text=True).strip()
        if reported != f"woozyc {release_version} (compiler-dev/1, commit {commit.lower()})":
            raise ValueError(f"macOS package reported unexpected version {reported!r}")

        tooling = ROOT / "tests/tooling"
        for name in ("installed_bundle_test.py", "dependency_lock_cli.py", "project_test_cli.py"):
            run([sys.executable, "-B", tooling / name, compiler], cwd=work, timeout=300)
        run([sys.executable, "-B", tooling / "editor_semantics_test.py", "--cli", compiler],
            cwd=work, timeout=300)
        run([sys.executable, "-B", ROOT / "selfhost/tests/macos/run.py", "--woozyc", compiler],
            cwd=work, timeout=600)
        for suite in ("sort", "text", "regex"):
            manifest = ROOT / f"tests/stdlib_{suite}/lang.toml"
            for mode in ("native-o0", "native-o2", "python"):
                run([sys.executable, "-B", tooling / "stdlib_project_test.py", compiler,
                     manifest, mode], cwd=work, timeout=240)
        check_chat(compiler, work)
        http_runner = ROOT / "tests/native/http_server/run_http_server.py"
        # Allow 600 s each for the server and control builds, plus 300 s for wire tests.
        http_timeout = 2 * 600 + 300
        run([sys.executable, "-B", http_runner, "--cli", compiler, "--opt", "0",
             "--out", work / "http-o0"], cwd=work, timeout=http_timeout)
        run([sys.executable, "-B", http_runner, "--cli", compiler, "--opt", "2",
             "--out", work / "http-o2"], cwd=work, timeout=http_timeout)

    print("macOS release archive passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-dir", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    verify(args.release_dir.resolve(strict=True), args.version, args.commit)


if __name__ == "__main__":
    main()
