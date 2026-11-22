#!/usr/bin/env python3
"""Check for and install published Woozy toolchains."""

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile


REPO = "qinjian623/woozy"
TAG_PATTERN = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?")
METADATA = re.compile(r"version=([0-9A-Za-z.-]+)-selfhost\ncommit=([0-9a-f]{40})\n")
MAX_TAG_LENGTH = 120  # The compiler accepts up to 128 bytes including -selfhost.


@dataclass(frozen=True)
class Release:
    tag: str
    archive: str
    size: int
    digest: str | None


def host_target():
    host = (platform.system(), platform.machine())
    if host == ("Linux", "x86_64"):
        return "linux-x86_64"
    if host == ("Darwin", "arm64"):
        return "macos-arm64"
    raise ValueError("Woozy releases support Linux x86_64 and Apple Silicon")


def version_key(tag):
    if not isinstance(tag, str) or len(tag) > MAX_TAG_LENGTH:
        return None
    match = TAG_PATTERN.fullmatch(tag)
    if match is None:
        return None
    major, minor, patch, preview = match.groups()
    identifiers = []
    for word in preview.split(".") if preview else ():
        if word.isdigit():
            if len(word) > 1 and word.startswith("0"):
                return None
            identifiers.append((0, int(word)))
        else:
            identifiers.append((1, word))
    return (int(major), int(minor), int(patch), not preview, tuple(identifiers))


def fetch(url, limit, *, destination=None):
    command = ["curl", "-q", "--proto", "=https", "--proto-redir", "=https", "-fsSL",
               "--connect-timeout", "10", "--retry", "2", "--max-filesize", str(limit)]
    if destination is not None:
        command.extend(("--max-time", "900", "--output", os.fspath(destination)))
    else:
        command.extend(("--max-time", "30"))
    return subprocess.run([*command, url], check=True, stdout=subprocess.PIPE).stdout


def release_pages():
    page = 1
    while True:
        url = f"https://api.github.com/repos/{REPO}/releases?per_page=100&page={page}"
        data = json.loads(fetch(url, 8 * 1024 * 1024))
        if not isinstance(data, list):
            raise ValueError("GitHub returned an invalid release list")
        yield from data
        if len(data) < 100:
            return
        page += 1


def select_release(releases, target, current_tag=None):
    archive = f"woozy-{target}.tar.gz"
    candidates = []
    for entry in releases:
        if not isinstance(entry, dict) or entry.get("draft"):
            continue
        tag = entry.get("tag_name")
        key = version_key(tag)
        if key is None:
            continue
        assets = {asset.get("name"): asset for asset in (entry.get("assets") or [])
                  if isinstance(asset, dict) and asset.get("state", "uploaded") == "uploaded"}
        if archive not in assets or f"{archive}.sha256" not in assets:
            continue
        size = assets[archive].get("size")
        if not isinstance(size, int) or size <= 0:
            continue
        stable = key[3] and not entry.get("prerelease")
        candidates.append((key, stable, Release(tag, archive, size, assets[archive].get("digest"))))
    if current_tag is None or version_key(current_tag)[3]:
        stable = [candidate for candidate in candidates if candidate[1]]
        candidates = stable or candidates
    return max(candidates, key=lambda candidate: candidate[0])[2] if candidates else None


def download(url, destination, limit):
    fetch(url, limit, destination=destination)
    if destination.stat().st_size > limit:
        raise ValueError(f"Release asset exceeds the expected size: {destination.name}")


def bundle_tag(bundle, target, expected=None):
    compiler = bundle / "woozyc"
    metadata = bundle / "release-metadata"
    if not metadata.is_file() or metadata.is_symlink() or metadata.stat().st_size > 190:
        raise ValueError(f"Missing or invalid release metadata in {bundle}")
    match = METADATA.fullmatch(metadata.read_text(encoding="ascii"))
    if match is None:
        raise ValueError(f"Invalid release metadata in {bundle}")
    version, commit = match.groups()
    tag = f"v{version}"
    if version_key(tag) is None or (expected is not None and tag != expected):
        raise ValueError(f"Bundle version {tag} does not match the expected Woozy release")
    if not compiler.is_file() or compiler.is_symlink() or not os.access(compiler, os.X_OK):
        raise ValueError(f"Missing executable {compiler}")
    tools = ["clang", "opt", "llc", "ld.lld"]
    runtimes = ["liblang_native_runtime.a", "liblang_hosted_runtime.a"]
    if target == "macos-arm64":
        tools.extend(("ld64.lld", "llvm-ar"))
        runtimes = ["liblang_darwin_core.a"]
    required = [bundle / "LICENSE", *(bundle / "runtime" / name for name in runtimes),
                *(bundle / "toolchain" / name for name in tools)]
    if not (bundle / "stdlib").is_dir() or not all(path.is_file() for path in required):
        raise ValueError(f"Incomplete Woozy release bundle at {bundle}")
    output = subprocess.check_output([compiler, "--version"], text=True, timeout=15).strip()
    version_output = rf"woozyc {re.escape(version)}-selfhost \([^()\r\n]+, commit {commit}\)"
    if re.fullmatch(version_output, output) is None:
        raise ValueError(f"Compiler version does not match the release metadata in {bundle}")
    return tag


def current_install(link, target):
    if not os.path.lexists(link):
        return None, None
    if not link.is_symlink():
        raise ValueError(f"Cannot manage {link}. Use a different --prefix for this installer.")
    try:
        file = link.resolve(strict=True)
        if file.name != "woozyc":
            raise ValueError(f"Expected a woozyc executable at {file}")
        return bundle_tag(file.parent, target), os.readlink(link)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        raise ValueError(f"Cannot manage {link}. Use a different --prefix. ({error})") from error


def verify_and_extract(release, directory):
    base = f"https://github.com/{REPO}/releases/download/{release.tag}"
    sums = directory / f"{release.archive}.sha256"
    download(f"{base}/{sums.name}", sums, 1024)
    line = re.fullmatch(r"([0-9a-fA-F]{64})  " + re.escape(release.archive) + r"\n?",
                        sums.read_text(encoding="ascii"))
    if line is None:
        raise ValueError("Invalid release checksum file")
    expected = line[1].lower()
    if release.digest is not None and release.digest != f"sha256:{expected}":
        raise ValueError("Release checksum does not match the GitHub asset digest")
    archive = directory / release.archive
    download(f"{base}/{archive.name}", archive, release.size)
    with archive.open("rb") as stream:
        if archive.stat().st_size != release.size or hashlib.file_digest(stream, "sha256").hexdigest() != expected:
            raise ValueError("Woozy archive failed SHA-256 verification")
    with tarfile.open(archive, "r:gz") as content:
        members = content.getmembers()
        for member in members:
            path = PurePosixPath(member.name)
            if (not path.parts or path.parts[0] != "woozy" or ".." in path.parts or
                    not (member.isdir() or member.isfile())):
                raise ValueError(f"Unexpected path or file type in Woozy archive: {member.name!r}")
        content.extractall(directory, members=members, filter="data")
    return directory / "woozy"


def install(release, prefix, link, target):
    import fcntl

    opt = prefix / "opt"
    opt.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(opt / ".woozy-install.lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, "rb+") as lock:
        if not stat.S_ISREG(os.fstat(lock.fileno()).st_mode):
            raise ValueError("Woozy install lock must be a regular file")
        fcntl.flock(lock, fcntl.LOCK_EX)
        current, old_link = current_install(link, target)
        if current is not None and version_key(current) >= version_key(release.tag):
            print(f"Woozy {current} is up to date.")
            return
        bundle = opt / f"woozy-{release.tag[1:]}"
        if os.path.lexists(bundle):
            if bundle.is_symlink() or not bundle.is_dir():
                raise ValueError(f"Conflicting install directory: {bundle}")
            bundle_tag(bundle, target, release.tag)
        else:
            print(f"Downloading and verifying {release.archive} for {release.tag}…", flush=True)
            with tempfile.TemporaryDirectory(prefix=".woozy-release-", dir=opt) as temporary:
                staged = verify_and_extract(release, Path(temporary))
                bundle_tag(staged, target, release.tag)
                if os.path.lexists(bundle):
                    raise ValueError(f"Conflicting install directory: {bundle}")
                staged.rename(bundle)
        link.parent.mkdir(parents=True, exist_ok=True)
        destination = os.path.relpath(bundle / "woozyc", link.parent)
        if old_link is None:
            link.symlink_to(destination)
        else:
            with tempfile.TemporaryDirectory(prefix=".woozy-link-", dir=link.parent) as temporary:
                replacement = Path(temporary) / link.name
                replacement.symlink_to(destination)
                if not link.is_symlink() or os.readlink(link) != old_link:
                    raise ValueError(f"Woozy executable changed during installation: {link}")
                replacement.replace(link)
    print(f"Installed Woozy {release.tag} at {link}.")


def confirm(action):
    try:
        terminal = open("/dev/tty", encoding="utf-8")
    except OSError as error:
        raise ValueError("Confirmation requires a terminal. Use --yes for an unattended install.") from error
    with terminal:
        print(f"{action}? [y/N] ", end="", file=sys.stderr, flush=True)
        return terminal.readline().strip().lower() in {"y", "yes"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="report the available release without installing")
    mode.add_argument("--yes", action="store_true", help="install without prompting")
    parser.add_argument("--prefix", type=Path, default=Path.home() / ".local",
                        help="install prefix containing bin and opt (default: ~/.local)")
    args = parser.parse_args(argv)
    try:
        target = host_target()
        if not hasattr(tarfile, "data_filter"):
            raise ValueError("Use Python 3.12 or later to verify and extract Woozy releases")
        prefix = args.prefix.expanduser().resolve()
        link = prefix / "bin" / "woozyc"
        unknown = False
        try:
            current, _ = current_install(link, target)
        except ValueError:
            if not args.check:
                raise
            current = None
            unknown = True
            print(f"Installed Woozy version is unknown at {link}.")
        release = select_release(release_pages(), target, current)
        if release is None:
            raise ValueError(f"No Woozy release with an archive and checksum for {target}")
        up_to_date = current is not None and version_key(current) >= version_key(release.tag)
        action = f"Upgrade Woozy from {current} to {release.tag}" if current else f"Install Woozy {release.tag}"
        if up_to_date:
            print(f"Woozy {current} is up to date.")
        elif args.check:
            print(f"Available Woozy release {release.tag} ({target})." if unknown else f"{action} ({target}).")
        elif not args.yes and not confirm(action):
            print("Installation cancelled.")
            return 0
        else:
            install(release, prefix, link, target)
        if not args.check:
            on_path = shutil.which("woozyc")
            if on_path is None or Path(on_path).absolute() != link:
                print(f"Put {link.parent} on PATH to run this Woozy installation, or use {link} directly.")
        return 0
    except (OSError, ValueError, RuntimeError, tarfile.TarError, subprocess.SubprocessError) as error:
        print(f"Woozy installation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
