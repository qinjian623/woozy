"""Test-only staging helper: complete the real app/wzcc import closure.

The helper reads every staged Wz file, including ``src/main.wz`` (callers must
stage the entry point before calling it), follows the real
``import "app/wzcc/..."`` edges, and copies only missing production package
directories from the known ``selfhost/src/wzcc`` tree.

Safety rules for this test adapter:

* reads may follow symlinks only when the resolved target stays inside the
  private project or the designated production source tree;
* directory cycles and read escapes are errors, never silently skipped;
* a pre-staged package directory is never filled or overwritten, even if it
  is empty or deliberately reduced; missing nested packages can be staged;
* every write is made below the private project's ``wzcc`` directory; the
  helper refuses a symlinked ``wzcc`` and re-checks every destination real path.
"""

from __future__ import annotations

import hashlib
import os
import re
from pathlib import Path

IMPORT_RE = re.compile(
    r'^\s*import\s+"app/wzcc/([A-Za-z_][A-Za-z0-9_]*(?:/[A-Za-z_][A-Za-z0-9_]*)*)"',
    re.MULTILINE)


class ClosureError(RuntimeError):
    """Base class for staging safety failures."""


class ReadEscape(ClosureError):
    """A staged source symlink resolves outside the allowed read roots."""


class SymlinkCycle(ClosureError):
    """A staged directory graph contains a symlink/directory cycle."""


class WriteEscape(ClosureError):
    """A closure copy would write outside the private project."""


def _real(path: Path) -> Path:
    return Path(os.path.realpath(str(path)))


def _within(path: Path, root: Path) -> bool:
    try:
        return path.resolve(strict=False).is_relative_to(root.resolve())
    except (OSError, RuntimeError):
        return False


def _allowed(path: Path, roots: tuple[Path, ...]) -> bool:
    return any(_within(path, root) for root in roots)


def _read_roots(project_src: Path, source_root: Path,
                additional_read_roots: tuple[Path, ...]) -> tuple[Path, ...]:
    roots = [project_src.resolve(), source_root.resolve()]
    roots.extend(path.resolve() for path in additional_read_roots)
    return tuple(roots)


def _walk_wz(project_src: Path, source_root: Path,
             additional_read_roots: tuple[Path, ...]) -> list[Path]:
    """Return staged .wz files with symlink escape/cycle checks."""
    roots = _read_roots(project_src, source_root, additional_read_roots)
    found: list[Path] = []
    active: set[Path] = set()
    visited: set[Path] = set()

    def walk(directory: Path) -> None:
        real_directory = _real(directory)
        if not _allowed(real_directory, roots):
            raise ReadEscape(
                f"staged source directory escapes the project/source roots: "
                f"{directory} -> {real_directory}")
        if real_directory in active:
            raise SymlinkCycle(f"directory cycle at {directory} -> {real_directory}")
        if real_directory in visited:
            return
        active.add(real_directory)
        try:
            with os.scandir(directory) as scan:
                entries = sorted(scan, key=lambda entry: entry.name)
                for entry in entries:
                    path = Path(directory) / entry.name
                    if entry.is_dir(follow_symlinks=True):
                        walk(path)
                    elif (entry.name.endswith(".wz") and
                          (entry.is_file(follow_symlinks=True) or
                           entry.is_symlink())):
                        target = _real(path)
                        if not _allowed(target, roots):
                            raise ReadEscape(
                                f"staged source symlink escapes the project/"
                                f"source roots: {path} -> {target}")
                        if not target.is_file():
                            raise ClosureError(
                                f"staged source symlink is not a file: {path}")
                        found.append(path)
        finally:
            active.remove(real_directory)
        visited.add(real_directory)

    walk(project_src)
    return found


def imported_packages(project_src: Path, source_root: Path,
                      additional_read_roots: tuple[Path, ...] = ()) -> set[str]:
    """Return every app/wzcc package imported by staged Wz sources."""
    packages: set[str] = set()
    for source in _walk_wz(project_src, source_root, additional_read_roots):
        packages.update(IMPORT_RE.findall(
            source.read_text(encoding="utf-8", errors="replace")))
    return packages


def _check_private_project(project_src: Path, source_root: Path) -> None:
    if project_src.is_symlink():
        raise WriteEscape(f"private project src must not be a symlink: {project_src}")
    if source_root == project_src or _within(project_src, source_root):
        raise WriteEscape(
            f"private project is inside the production source tree: {project_src}")
    if _within(source_root, project_src):
        raise WriteEscape(
            f"production source tree is inside the private project: {source_root}")


def _check_write_path(path: Path, target_root: Path, project_src: Path) -> None:
    current = target_root
    if current.is_symlink():
        raise WriteEscape(f"wzcc staging root must not be a symlink: {current}")
    for part in path.relative_to(target_root).parts:
        current = current / part
        if current.is_symlink():
            raise WriteEscape(f"write path crosses a symlink: {current}")
    real = _real(path)
    if not _within(real, project_src):
        raise WriteEscape(f"write path escapes the private project: {path} -> {real}")
    if _within(real, _real(target_root)):
        return
    raise WriteEscape(f"write path escapes the private wzcc tree: {path} -> {real}")


def complete_import_closure(project_src: Path, source_root: Path, *,
                            additional_read_roots: tuple[Path, ...] = (),
                            skipped: list[dict] | None = None) -> list[dict]:
    """Copy missing app/wzcc packages into the private project.

    ``project_src`` is the project's ``src`` directory (with ``main.wz``
    already staged); ``source_root`` is ``selfhost/src/wzcc``.  Returns one
    record per copied file with source/target SHA-256.  Existing package
    directories are reported through ``skipped`` and left untouched.
    """
    project_src = Path(project_src)
    source_root = Path(source_root).resolve()
    _check_private_project(project_src, source_root)
    project_src = project_src.resolve()
    target_root = project_src / "wzcc"
    if not target_root.is_dir():
        raise FileNotFoundError(f"project has no src/wzcc directory: {target_root}")
    if target_root.is_symlink():
        raise WriteEscape(f"project wzcc directory must not be a symlink: {target_root}")
    if not _within(target_root, project_src):
        raise WriteEscape(f"project wzcc directory escapes the project: {target_root}")
    copied: list[dict] = []
    created_parents: set[Path] = set()
    while True:
        imports = imported_packages(project_src, source_root,
                                    additional_read_roots)
        missing: list[str] = []
        for package in sorted(imports):
            destination = target_root / package
            if destination not in created_parents and (
                    destination.exists() or destination.is_symlink()):
                if skipped is not None:
                    skipped.append({
                        "package": package,
                        "path": str(destination),
                        "reason": "existing package directory; not filled or overwritten",
                        "files": sorted(path.name for path in destination.glob("*.wz"))
                        if destination.is_dir() else [],
                    })
                continue
            missing.append(package)
        if not missing:
            return copied
        for package in missing:
            source_dir = source_root / package
            if not _within(source_dir, source_root):
                raise ReadEscape(
                    f"production package symlink escapes source root: {source_dir}")
            if not source_dir.is_dir():
                raise FileNotFoundError(
                    f"imported production package is missing: {source_dir}")
            sources = [source for source in _walk_wz(source_dir, source_root, ())
                       if source.parent == source_dir]
            if not sources:
                raise FileNotFoundError(
                    f"imported production package has no Wz files: {source_dir}")
            destination = target_root / package
            _check_write_path(destination, target_root, project_src)
            # A later import can name an ancestor created only to hold a child.
            created_parents.update(parent for parent in destination.parents
                                   if not parent.exists())
            destination.mkdir(parents=True, exist_ok=destination in created_parents)
            created_parents.discard(destination)
            for source in sources:
                out = destination / source.name
                _check_write_path(out, target_root, project_src)
                data = source.read_bytes()
                digest = hashlib.sha256(data).hexdigest()
                with out.open("xb") as target:
                    target.write(data)
                _check_write_path(out, target_root, project_src)
                if hashlib.sha256(out.read_bytes()).hexdigest() != digest:
                    raise ClosureError(f"copied source changed bytes: {source}")
                copied.append({
                    "package": package,
                    "source": str(source),
                    "target": str(out),
                    "sha256": digest,
                    "target_sha256": digest,
                })
