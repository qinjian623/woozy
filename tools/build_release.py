#!/usr/bin/env python3
"""Package a built Linux x86_64 Woozy toolchain and its SHA-256 checksum."""

import argparse
import gzip
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

import install_toolchain


ARCHIVE_NAME = "woozy-linux-x86_64.tar.gz"
METADATA_NAME = "release-metadata"
MAX_VERSION_BYTES = 128
VERSION_PATTERN = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?-selfhost")
COMMIT_PATTERN = re.compile(r"[0-9a-fA-F]{40}")
VENDOR = install_toolchain.ROOT / "runtime/native"
NOTICES = (
    VENDOR / "math/COPYRIGHT",
    VENDOR / "unicode/vendor/utf8proc/LICENSE.md",
    *sorted((VENDOR / "vendor/fast_float").glob("LICENSE-*")),
    *sorted((VENDOR / "vendor/ryu").glob("LICENSE-*")),
)


def metadata_text(version, commit):
    if version is None and commit is None:
        return None
    if version is None or commit is None:
        raise ValueError("--version and --commit must be provided together")
    if version.startswith("v"):
        version = version[1:]
    if not version.endswith("-selfhost"):
        version += "-selfhost"
    if (not version.isascii() or len(version) > MAX_VERSION_BYTES or
            VERSION_PATTERN.fullmatch(version) is None):
        raise ValueError(f"Invalid release version: {version}")
    if COMMIT_PATTERN.fullmatch(commit) is None:
        raise ValueError(f"Invalid release commit: {commit}")
    return f"version={version}\ncommit={commit.lower()}\n"


def write_metadata(bundle, text):
    if text is not None:
        (bundle / METADATA_NAME).write_text(text, encoding="ascii")


def archive_bundle(bundle, destination):
    with destination.open("xb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as zipped:
            with tarfile.open(mode="w|", fileobj=zipped) as archive:
                for path in (bundle, *sorted(bundle.rglob("*"))):
                    member = archive.gettarinfo(path, str(Path("woozy") / path.relative_to(bundle)))
                    if not (member.isdir() or member.isfile()):
                        raise ValueError(f"Unexpected file type in bundle: {path}")
                    member.uid = member.gid = member.mtime = 0
                    member.uname = member.gname = ""
                    member.mode = 0o755 if member.isdir() or member.mode & 0o111 else 0o644
                    if member.isfile():
                        with path.open("rb") as source:
                            archive.addfile(member, source)
                    else:
                        archive.addfile(member)


def build(args):
    metadata = metadata_text(args.version, args.commit)
    output = args.out_dir.expanduser().absolute()
    if os.path.lexists(output):
        raise ValueError(f"Output directory already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".woozy-release-", dir=output.parent) as temporary:
        temporary = Path(temporary)
        args.prefix = temporary / "bundle"
        install_toolchain.install(args)
        write_metadata(args.prefix, metadata)
        shutil.copyfile(install_toolchain.ROOT / "LICENSE", args.prefix / "LICENSE")
        notices = args.prefix / "licenses"
        notices.mkdir()
        for source in NOTICES:
            shutil.copyfile(source, notices / f"{source.parent.name}-{source.name}")
        for source in args.notice:
            destination = notices / source.name
            if destination.exists():
                raise ValueError(f"Duplicate license notice filename: {source.name}")
            shutil.copyfile(source, destination)
        staged = temporary / "release"
        staged.mkdir()
        asset = staged / ARCHIVE_NAME
        archive_bundle(args.prefix, asset)
        with asset.open("rb") as data:
            digest = hashlib.file_digest(data, "sha256").hexdigest()
        (staged / (ARCHIVE_NAME + ".sha256")).write_text(f"{digest}  {ARCHIVE_NAME}\n")
        install_toolchain.publish(staged, output)
    return output / ARCHIVE_NAME


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", required=True, type=Path, help="new directory for the archive and checksum")
    parser.add_argument("--notice", type=Path, action="append", default=[],
                        help="license notice for bundled LLVM or compatibility libraries, may be repeated")
    parser.add_argument("--version",
                        help="release tag or version; a leading v and the -selfhost suffix are optional")
    parser.add_argument("--commit", help="full 40-character source commit for the package")
    install_toolchain.add_bundle_inputs(parser)
    args = parser.parse_args()
    try:
        print(build(args))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Woozy release build failed: {error}\n")


if __name__ == "__main__":
    main()
