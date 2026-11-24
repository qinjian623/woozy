#!/usr/bin/env python3
"""Build and run tests/stdlib_fs_helpers in isolated Native bundles.

The compiler resolves stdlib relative to its real executable path. This driver
therefore builds a temporary bundle containing a hard link (or copy) of the
compiler, links to its other resources, and a copy of the repository's stdlib.
That keeps the test on the edited sources without changing the release install
or the central CMake test list.

Temporary work is removed after the run. Without --out it is created below
$TMPDIR when set, otherwise below $HOME; --out selects a caller-owned workspace.
"""

import argparse
import errno
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
MANIFEST = HERE / "lang.toml"


def make_bundle(compiler, directory):
    source = compiler.resolve().parent
    bundle = directory / "bundle"
    shutil.rmtree(bundle, ignore_errors=True)
    bundle.mkdir()
    try:
        os.link(compiler.resolve(), bundle / "woozyc")
    except OSError:
        shutil.copy2(compiler.resolve(), bundle / "woozyc")
    for name in ("native", "runtime", "toolchain", "release-metadata"):
        entry = source / name
        if entry.exists():
            (bundle / name).symlink_to(entry.resolve(),
                                       target_is_directory=entry.is_dir())
    shutil.copytree(ROOT / "stdlib", bundle / "stdlib")
    return bundle


def remove_any(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    except IsADirectoryError:
        shutil.rmtree(path)


def make_fixture(scratch):
    fixture = scratch / "encoded"
    fixture.mkdir()
    raw = os.fsencode(fixture)
    with open(os.path.join(raw, b"good.txt"), "wb") as stream:
        stream.write(b"good")
    os.symlink(b"good.txt", os.path.join(raw, b"link"))

    bad_file = os.path.join(raw, b"bad-\xff")
    bad_directory = os.path.join(raw, b"dir-\xfe")
    try:
        with open(bad_file, "xb"):
            pass
        os.mkdir(bad_directory)
    except OSError as error:
        if sys.platform != "darwin" or error.errno != errno.EILSEQ:
            raise
        # Only the invalid-byte-name portion is platform dependent. Remove any
        # partially created fixture entries so the remaining checks see the
        # same tree on filesystems that reject such names.
        remove_any(bad_file)
        remove_any(bad_directory)
        return fixture, "skip", f"errno {error.errno} ({error.strerror})"
    with open(os.path.join(bad_directory, b"hidden"), "wb") as stream:
        stream.write(b"hidden")
    return fixture, "invalid", ""


def deep_depth(scratch):
    try:
        path_max = os.pathconf(scratch, "PC_PATH_MAX")
    except (OSError, ValueError):
        path_max = 4096
    if path_max is None or path_max <= 0:
        path_max = 4096
    # create_temp_dir adds "deep-" plus sixteen hex digits to scratch.
    root_length = len(os.fsencode(str(scratch))) + 1 + len("deep-") + 16
    available = path_max - root_length - 64
    return max(1, available // 2)


def run_mode(bundle, work, mode, libxml, overflow):
    opt = mode[-1]
    out = work / ("out-" + mode)
    shutil.rmtree(out, ignore_errors=True)
    environment = dict(os.environ)
    library = libxml or bundle / "toolchain" / "lib"
    if library.is_dir():
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, (
            str(library), environment.get("LD_LIBRARY_PATH"),
        )))
    build = [str(bundle / "woozyc"), "build", "--project", str(MANIFEST),
             "--backend", "native", "--out-dir", str(out), "--opt", opt]
    print("build", mode, ":", " ".join(build), flush=True)
    subprocess.run(build, cwd=work, env=environment, check=True, timeout=240)

    scratch = work / ("scratch-" + mode)
    shutil.rmtree(scratch, ignore_errors=True)
    scratch.mkdir()
    fixture, invalid_names, detail = make_fixture(scratch)
    depth = deep_depth(scratch)
    command = [str(out / "program"), str(scratch), str(fixture),
               invalid_names, str(depth), "1" if overflow else "0"]
    print("run", mode, ": depth", depth, "invalid-names", invalid_names,
          flush=True)
    if invalid_names == "skip":
        print("skip", mode, ": invalid UTF-8 byte names refused by filesystem:",
              detail, flush=True)
    subprocess.run(command, cwd=work, env=environment, check=True, timeout=240)


def workspace_parent():
    configured = os.environ.get("TMPDIR")
    parent = Path(configured) if configured else Path.home()
    parent.mkdir(parents=True, exist_ok=True)
    return parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path,
                        default=(Path(os.environ["WOOZYC"])
                                 if "WOOZYC" in os.environ else None))
    parser.add_argument("--out", type=Path,
                        help="empty persistent workspace instead of a temporary one")
    parser.add_argument("--libxml", type=Path)
    parser.add_argument("--overflow", action="store_true",
                        help="also run the 100001-entry overflow test")
    args = parser.parse_args()
    if args.compiler is None:
        parser.error("pass --compiler or set WOOZYC")
    compiler = args.compiler.resolve()
    if not compiler.is_file():
        parser.error(f"missing compiler: {compiler}")

    with tempfile.TemporaryDirectory(prefix="woozy-fs-helpers-",
                                     dir=workspace_parent()) as temporary:
        work = Path(args.out).resolve() if args.out else Path(temporary)
        work.mkdir(parents=True, exist_ok=True)
        if any(work.iterdir()):
            parser.error(f"work directory is not empty: {work}")
        bundle = make_bundle(compiler, work)
        for mode in ("native-o0", "native-o2"):
            run_mode(bundle, work, mode, args.libxml, args.overflow)
    print("stdlib_fs_helpers: passed native-o0 native-o2", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
