import argparse
import errno
import hashlib
import os
from pathlib import Path
import platform
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import build_release
import install_toolchain
from build_release import METADATA_NAME, archive_bundle, metadata_text, write_metadata


class ReleaseArchiveTest(unittest.TestCase):
    def test_archive_is_reproducible_and_preserves_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = root / "staging"
            bundle.mkdir()
            compiler = bundle / "woozyc"
            compiler.write_bytes(b"compiler")
            compiler.chmod(0o711)
            first, second = root / "first.tar.gz", root / "second.tar.gz"
            archive_bundle(bundle, first)
            os.utime(compiler, (1234, 1234))
            archive_bundle(bundle, second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with tarfile.open(first, "r:gz") as archive:
                self.assertEqual(archive.getnames(), ["woozy", "woozy/woozyc"])
                item = archive.getmember("woozy/woozyc")
                self.assertEqual((item.mode, item.mtime, item.uid, item.gid), (0o755, 0, 0, 0))
                self.assertEqual(archive.extractfile(item).read(), b"compiler")

    def test_release_metadata_is_validated_and_archived(self):
        commit = "0123456789abcdef" * 2 + "01234567"
        for version, expected in (
                ("v0.1.0-rc.1", "0.1.0-rc.1-selfhost"),
                ("v0.1.0", "0.1.0-selfhost"),
                ("0.1.0-selfhost", "0.1.0-selfhost"),
                ("1.2.3-beta.2", "1.2.3-beta.2-selfhost")):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                bundle = root / "staging"
                bundle.mkdir()
                (bundle / "woozyc").write_bytes(b"compiler")
                text = metadata_text(version, commit.upper())
                self.assertEqual(text, f"version={expected}\ncommit={commit}\n")
                write_metadata(bundle, text)
                archive = root / "release.tar.gz"
                archive_bundle(bundle, archive)
                with tarfile.open(archive, "r:gz") as contents:
                    item = contents.getmember("woozy/" + METADATA_NAME)
                    self.assertEqual((item.mode, item.mtime, item.uid, item.gid),
                                     (0o644, 0, 0, 0))
                    self.assertEqual(contents.extractfile(item).read().decode("ascii"), text)

    def test_release_version_length_boundary_matches_wz_rule(self):
        commit = "0" * 40
        prefix, suffix = "1.2.3-", "-selfhost"
        filler = "a" * (build_release.MAX_VERSION_BYTES - len(prefix) - len(suffix))
        at_limit = prefix + filler + suffix
        self.assertEqual(len(at_limit), build_release.MAX_VERSION_BYTES)
        self.assertEqual(metadata_text(at_limit, commit),
                         f"version={at_limit}\ncommit={commit}\n")
        too_long = prefix + filler + "a" + suffix
        self.assertEqual(len(too_long), build_release.MAX_VERSION_BYTES + 1)
        with self.assertRaises(ValueError):
            metadata_text(too_long, commit)

    def test_build_includes_metadata_and_licenses(self):
        commit = "fedcba9876543210" * 2 + "fedcba98"
        for target in ("linux-x86_64", "macos-arm64"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)

                def install(args):
                    args.prefix.mkdir()
                    (args.prefix / "woozyc").write_bytes(b"compiler")

                args = argparse.Namespace(out_dir=root / "release", notice=[],
                                          version="v0.1.0-rc.1", commit=commit)
                with mock.patch.object(install_toolchain, "install", side_effect=install), \
                     mock.patch.object(install_toolchain, "host_target", return_value=target):
                    archive_path = build_release.build(args)
                name = f"woozy-{target}.tar.gz"
                self.assertEqual(archive_path.name, name)
                digest = hashlib.sha256(archive_path.read_bytes()).hexdigest()
                self.assertEqual(archive_path.with_name(name + ".sha256").read_text(),
                                 f"{digest}  {name}\n")
                with tarfile.open(archive_path, "r:gz") as archive:
                    item = archive.getmember("woozy/" + METADATA_NAME)
                    self.assertEqual(
                        archive.extractfile(item).read().decode("ascii"),
                        f"version=0.1.0-rc.1-selfhost\ncommit={commit}\n")
                    for relative in ("LICENSE", "docs/QUICKSTART.md", "tools/gdb/woozy.py",
                                     "development/editor/setup.py",
                                     "development/editor/nvim/plugin/woozy.lua",
                                     "development/editor/emacs/woozy-mode.el"):
                        self.assertEqual(archive.extractfile("woozy/" + relative).read(),
                                         (install_toolchain.ROOT / relative).read_bytes())
                    self.assertEqual(archive.getmember("woozy/development/editor/setup.py").mode,
                                     0o755)
                    lldb = "tools/lldb/woozy.py"
                    if target == "macos-arm64" and (install_toolchain.ROOT / lldb).is_file():
                        self.assertEqual(archive.extractfile("woozy/" + lldb).read(),
                                         (install_toolchain.ROOT / lldb).read_bytes())
                    for notice in build_release.NOTICES:
                        entry = f"woozy/licenses/{notice.parent.name}-{notice.name}"
                        self.assertEqual(archive.extractfile(entry).read(), notice.read_bytes())

    def test_compiler_binary_header_is_native(self):
        with tempfile.TemporaryDirectory() as temporary:
            compiler = Path(temporary) / "woozyc"
            compiler.write_bytes(b"\xcf\xfa\xed\xfe\x0c\0\0\x01" + b"\0" * 4 + b"\x02\0\0\0")
            install_toolchain.check_native_binary(compiler, True, "compiler binary")
            with self.assertRaisesRegex(ValueError, "ELF"):
                install_toolchain.check_native_binary(compiler, False, "compiler binary")
            compiler.write_bytes(b"\xcf\xfa\xed\xfe\x07\0\0\x01" + b"\0" * 4 + b"\x02\0\0\0")
            with self.assertRaisesRegex(ValueError, "Mach-O arm64"):
                install_toolchain.check_native_binary(compiler, True, "LLVM executable")

    @unittest.skipUnless(platform.system() in {"Darwin", "Linux"}, "native installer hosts")
    def test_publish_does_not_clobber_even_empty_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old, staged = root / "existing", root / "staged"
            old.mkdir()
            staged.mkdir()
            (staged / "preserved").write_text("staged content")
            with self.assertRaises(OSError) as failure:
                install_toolchain.publish(staged, old)
            self.assertEqual(failure.exception.errno, errno.EEXIST)
            self.assertEqual(list(old.iterdir()), [])
            self.assertEqual((staged / "preserved").read_text(), "staged content")

    def test_invalid_or_missing_metadata_is_rejected(self):
        self.assertIsNone(metadata_text(None, None))
        for version, commit in (
                (None, "0" * 40),
                ("v0.1.0", None),
                ("v0.1.0\n", "0" * 40),
                ("not-a-version", "0" * 40),
                ("1.2.3-é", "0" * 40),
                ("v0.1.0", "0" * 39),
                ("v0.1.0", "g" * 40),
                ("v0.1.0", "0" * 40 + "\n")):
            with self.subTest(version=version, commit=commit):
                with self.assertRaises(ValueError):
                    metadata_text(version, commit)
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary)
            write_metadata(bundle, None)
            self.assertFalse((bundle / METADATA_NAME).exists())


if __name__ == "__main__":
    unittest.main()
