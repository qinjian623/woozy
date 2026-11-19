#!/usr/bin/env python3
"""Small self-tests for the test-only wzcc import-closure adapter."""

from __future__ import annotations

import hashlib
import os
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from wzcc_closure import (  # noqa: E402
    ReadEscape,
    SymlinkCycle,
    WriteEscape,
    complete_import_closure,
    imported_packages,
)


def sha(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ClosureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="wzcc-closure-test-")
        base = pathlib.Path(self.tmp.name)
        self.source = base / "source"
        self.project = base / "project" / "src"
        self.outside = base / "outside"
        (self.source).mkdir(parents=True)
        (self.project / "wzcc").mkdir(parents=True)
        (self.outside).mkdir()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def package(self, name: str, files: dict[str, str]) -> None:
        directory = self.source / name
        directory.mkdir(parents=True, exist_ok=True)
        for filename, text in files.items():
            path = directory / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")

    def main(self, text: str) -> None:
        (self.project / "main.wz").write_text(text, encoding="utf-8")

    def test_main_and_transitive_closure(self) -> None:
        self.package("a", {"a.wz": 'package a;\nimport "app/wzcc/b" as b;\n'})
        self.package("b", {"b.wz": "package b;\n"})
        self.main('package app;\nimport "app/wzcc/a" as a;\n')
        copied = complete_import_closure(self.project, self.source)
        self.assertEqual(sorted({record["package"] for record in copied}), ["a", "b"])
        self.assertTrue((self.project / "wzcc/a/a.wz").is_file())
        self.assertTrue((self.project / "wzcc/b/b.wz").is_file())
        for record in copied:
            self.assertEqual(record["sha256"], record["target_sha256"])
            self.assertEqual(sha(pathlib.Path(record["source"])),
                             sha(pathlib.Path(record["target"])))

    def test_existing_empty_package_is_not_filled(self) -> None:
        self.package("a", {"a.wz": "package a;\n"})
        self.main('package app;\nimport "app/wzcc/a" as a;\n')
        empty = self.project / "wzcc/a"
        empty.mkdir()
        skipped: list[dict] = []
        copied = complete_import_closure(self.project, self.source, skipped=skipped)
        self.assertEqual(copied, [])
        self.assertEqual(list(empty.iterdir()), [])
        self.assertEqual(skipped[0]["package"], "a")

    def test_existing_reduced_package_is_not_filled(self) -> None:
        self.package("a", {"a.wz": "package a;\n", "extra.wz": "package a;\n"})
        self.package("b", {"b.wz": "package b;\n"})
        self.main('package app;\nimport "app/wzcc/a" as a;\n')
        reduced = self.project / "wzcc/a"
        reduced.mkdir()
        (reduced / "keep.wz").write_text(
            'package a;\nimport "app/wzcc/b" as b;\n', encoding="utf-8")
        before = sha(reduced / "keep.wz")
        copied = complete_import_closure(self.project, self.source)
        self.assertEqual([record["package"] for record in copied], ["b"])
        self.assertEqual(sorted(path.name for path in reduced.iterdir()), ["keep.wz"])
        self.assertEqual(sha(reduced / "keep.wz"), before)

    def test_production_symlink_read_is_allowed(self) -> None:
        self.package("a", {"a.wz": 'package a;\nimport "app/wzcc/b" as b;\n'})
        self.package("b", {"b.wz": "package b;\n"})
        self.main('package app;\nimport "app/wzcc/a" as a;\n')
        os.symlink(self.source / "a", self.project / "wzcc/a", target_is_directory=True)
        copied = complete_import_closure(self.project, self.source)
        self.assertEqual([record["package"] for record in copied], ["b"])
        self.assertTrue((self.project / "wzcc/a").is_symlink())
        self.assertTrue((self.project / "wzcc/b/b.wz").is_file())

    def test_read_escape_symlink_is_rejected(self) -> None:
        (self.outside / "evil.wz").write_text(
            'package app;\nimport "app/wzcc/a" as a;\n', encoding="utf-8")
        os.symlink(self.outside / "evil.wz", self.project / "main.wz")
        with self.assertRaises(ReadEscape):
            imported_packages(self.project, self.source)

    def test_directory_cycle_is_rejected(self) -> None:
        self.main("package app;\n")
        os.symlink(self.project, self.project / "wzcc/loop", target_is_directory=True)
        with self.assertRaises(SymlinkCycle):
            imported_packages(self.project, self.source)

    def test_write_escape_symlink_is_rejected(self) -> None:
        self.package("a", {"a.wz": "package a;\n"})
        self.main('package app;\nimport "app/wzcc/a" as a;\n')
        (self.project / "wzcc").rmdir()
        os.symlink(self.outside, self.project / "wzcc", target_is_directory=True)
        with self.assertRaises(WriteEscape):
            complete_import_closure(self.project, self.source)
        self.assertEqual(list(self.outside.iterdir()), [])

    def test_missing_production_package_is_rejected(self) -> None:
        self.main('package app;\nimport "app/wzcc/missing" as missing;\n')
        with self.assertRaises(FileNotFoundError):
            complete_import_closure(self.project, self.source)

    def test_source_tree_is_unchanged(self) -> None:
        self.package("a", {"a.wz": 'package a;\nimport "app/wzcc/b" as b;\n'})
        self.package("b", {"b.wz": "package b;\n"})
        self.main('package app;\nimport "app/wzcc/a" as a;\n')
        before = {path.relative_to(self.source).as_posix(): sha(path)
                  for path in sorted(self.source.rglob("*.wz"))}
        complete_import_closure(self.project, self.source)
        after = {path.relative_to(self.source).as_posix(): sha(path)
                 for path in sorted(self.source.rglob("*.wz"))}
        self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main(verbosity=2)
