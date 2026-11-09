#!/usr/bin/env python3
"""Focused tests for generated-file selection in tools/validate_package.py."""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest

PACKAGE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PACKAGE / "tools"))
import validate_package


class PackageFileSelectionTest(unittest.TestCase):
    def test_generated_and_vcs_directories_are_excluded(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "contracts").mkdir()
            (root / "contracts" / "real.json").write_text("{}", encoding="utf-8")
            (root / "build" / "CMakeFiles").mkdir(parents=True)
            (root / "build" / "CMakeCache.txt").write_text("cache", encoding="utf-8")
            (root / "build" / "illegal.json").write_text("{", encoding="utf-8")
            (root / "build" / "CMakeFiles" / "illegal.json").write_text("{", encoding="utf-8")
            (root / "build-debug").mkdir()
            (root / "build-debug" / "illegal.json").write_text("{", encoding="utf-8")
            (root / ".git" / "objects").mkdir(parents=True)
            (root / ".git" / "objects" / "illegal.json").write_text("{", encoding="utf-8")

            original = validate_package.ROOT
            validate_package.ROOT = root
            try:
                files = {path.relative_to(root).as_posix()
                         for path in validate_package.package_files()}
            finally:
                validate_package.ROOT = original

            self.assertIn("contracts/real.json", files)
            for excluded in ("build/illegal.json", "build/CMakeFiles/illegal.json",
                             "build-debug/illegal.json", ".git/objects/illegal.json"):
                self.assertNotIn(excluded, files)


class PackageValidationTest(unittest.TestCase):
    def copy_package(self, destination):
        package = shutil.copytree(
            PACKAGE, destination,
            ignore=shutil.ignore_patterns(".git", "__pycache__", "build*", "reports"))
        self.assertFalse((package / ".git").exists())
        return package

    def validate(self, package):
        run = subprocess.run(
            [sys.executable, "tools/validate_package.py", "--json"],
            cwd=package, text=True, capture_output=True, timeout=60)
        return run, json.loads(run.stdout)

    def test_clean_no_git_package_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = self.copy_package(Path(temporary) / "package")
            run, report = self.validate(package)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(report["status"], "passed")

    def test_cmake_build_noise_is_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = self.copy_package(Path(temporary) / "package")
            build = package / "build"
            (build / "CMakeFiles").mkdir(parents=True)
            (build / "CMakeCache.txt").write_text("cache", encoding="utf-8")
            (build / "illegal.json").write_text('{"broken": true,}\n', encoding="utf-8")
            (build / "dangling.json").symlink_to("../does-not-exist")
            run, report = self.validate(package)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(report["status"], "passed")

    def test_documentation_aliases_are_exact(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = self.copy_package(Path(temporary) / "package")
            (package / "docs" / "explicit-alias.md").write_text(
                "The diagnostic is E_CAP.\n", encoding="utf-8")
            run, report = self.validate(package)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(report["status"], "passed")

        with tempfile.TemporaryDirectory() as temporary:
            package = self.copy_package(Path(temporary) / "package")
            (package / "docs" / "independent-diagnostic.md").write_text(
                "The diagnostic is E_CAPABILITY_UNAVAILABL.\n", encoding="utf-8")
            run, report = self.validate(package)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(report["status"], "failed")
            self.assertIn("docs/independent-diagnostic.md", run.stdout)
            self.assertIn("E_CAPABILITY_UNAVAILABL", run.stdout)

    def test_real_package_errors_still_fail(self):
        def append_unregistered_code(package):
            with (package / "src/backend/native/helper.cpp").open(
                    "a", encoding="utf-8") as source:
                source.write('\nconst char *unregistered = "E_TEST_UNREGISTERED";\n')

        def add_ordinary_untracked_json(package):
            directory = package / "src" / "builders"
            directory.mkdir()
            (directory / "illegal.json").write_text(
                '{"broken": true,}\n', encoding="utf-8")

        mutations = {
            "invalid contracts JSON": lambda package: (
                package / "contracts" / "invalid.json").write_text(
                    '{"broken": true,}\n', encoding="utf-8"),
            "unregistered source code": append_unregistered_code,
            "ordinary untracked JSON": add_ordinary_untracked_json,
            "unregistered documentation code": lambda package: (
                package / "docs" / "invalid-code.md").write_text(
                    "E_DOC_UNREGISTERED\n", encoding="utf-8"),
            "broken documentation link": lambda package: (
                package / "docs" / "broken-link.md").write_text(
                    "[missing](missing.md)\n", encoding="utf-8"),
        }
        for label, mutate in mutations.items():
            with self.subTest(label), tempfile.TemporaryDirectory() as temporary:
                package = self.copy_package(Path(temporary) / "package")
                mutate(package)
                run, report = self.validate(package)
                self.assertNotEqual(run.returncode, 0, label)
                self.assertEqual(report["status"], "failed", label)


if __name__ == "__main__":
    unittest.main()
