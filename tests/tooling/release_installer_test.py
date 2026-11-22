import contextlib
import hashlib
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import install


TARGET = "linux-x86_64"
ARCHIVE = f"woozy-{TARGET}.tar.gz"
COMMIT = "0123456789abcdef" * 2 + "01234567"


def package(tag, unsafe=False):
    version = tag[1:] + "-selfhost"
    files = {
        "woozyc": f"#!/bin/sh\necho 'woozyc {version} (compiler-dev/1, commit {COMMIT})'\n",
        "release-metadata": f"version={version}\ncommit={COMMIT}\n",
        "LICENSE": "Example license\n",
        "stdlib/example.wz": "package example;\n",
        "runtime/liblang_native_runtime.a": "native runtime\n",
        "runtime/liblang_hosted_runtime.a": "hosted runtime\n",
        **{f"toolchain/{tool}": "tool\n" for tool in ("clang", "opt", "llc", "ld.lld")},
    }
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        for name in ("woozy", "woozy/stdlib", "woozy/runtime", "woozy/toolchain"):
            directory = tarfile.TarInfo(name)
            directory.type = tarfile.DIRTYPE
            directory.mode = 0o755
            archive.addfile(directory)
        for name, content in files.items():
            file = tarfile.TarInfo("woozy/" + name)
            data = content.encode()
            file.size = len(data)
            file.mode = 0o755 if name == "woozyc" else 0o644
            archive.addfile(file, io.BytesIO(data))
        if unsafe:
            member = tarfile.TarInfo("../outside")
            member.size = 1
            archive.addfile(member, io.BytesIO(b"x"))
    return buffer.getvalue()


class ReleaseInstallerTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.prefix = Path(temporary.name).resolve() / "prefix"
        self.command = self.prefix / "bin/woozyc"
        self.releases = []
        self.assets = {}
        self.download = mock.Mock(side_effect=self.download_asset)
        self.confirm = mock.Mock(return_value=True)

    def publish(self, tag, fault=None):
        archive = package(tag, unsafe=fault == "unsafe archive")
        digest = hashlib.sha256(archive).hexdigest()
        checksum = ("0" * 64 if fault == "checksum" else digest) + f"  {ARCHIVE}\n"
        assets = []
        for name, content in ((ARCHIVE, archive), (ARCHIVE + ".sha256", checksum.encode())):
            url = f"https://github.com/qinjian623/woozy/releases/download/{tag}/{name}"
            self.assets[url] = content
            asset_digest = hashlib.sha256(content).hexdigest()
            asset = {"name": name, "browser_download_url": url, "size": len(content),
                     "state": "uploaded"}
            if name == ARCHIVE and fault == "GitHub digest":
                asset_digest = "0" * 64
            if not (name == ARCHIVE and fault == "checksum"):
                asset["digest"] = f"sha256:{asset_digest}"
            assets.append(asset)
        self.releases.insert(0, {"tag_name": tag, "draft": False, "prerelease": "-rc." in tag,
                                 "assets": assets})

    def download_asset(self, url, destination, limit):
        content = self.assets[url]
        self.assertLessEqual(len(content), limit)
        destination.write_bytes(content)

    def invoke(self, *args):
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(install, "host_target", return_value=TARGET), \
             mock.patch.object(install, "release_pages", return_value=iter(self.releases)), \
             mock.patch.object(install, "download", self.download), \
             mock.patch.object(install, "confirm", self.confirm), \
             contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = install.main(["--prefix", str(self.prefix), *args])
        return result, stdout.getvalue(), stderr.getvalue()

    def test_check_and_decline_leave_prefix_absent(self):
        self.publish("v0.1.0-rc.2")
        result, stdout, stderr = self.invoke("--check")
        self.assertEqual(result, 0, stderr)
        self.assertIn("0.1.0-rc.2", stdout)
        self.confirm.assert_not_called()
        self.assertFalse(self.prefix.exists())

        self.confirm.return_value = False
        result, _, stderr = self.invoke()
        self.assertEqual(result, 0, stderr)
        self.confirm.assert_called_once()
        self.download.assert_not_called()
        self.assertFalse(self.prefix.exists())

    def test_prompt_reads_terminal_and_defaults_to_no(self):
        for reply, accepted in (("yes\n", True), ("\n", False)):
            with self.subTest(reply=reply), contextlib.redirect_stderr(io.StringIO()), \
                 mock.patch("builtins.open", return_value=io.StringIO(reply)) as terminal:
                self.assertEqual(install.confirm("Install Woozy"), accepted)
                terminal.assert_called_once_with("/dev/tty", encoding="utf-8")

    def test_upgrade_compares_rc_numbers_and_keeps_old_toolchain(self):
        self.publish("v0.1.0-rc.9")
        result, _, stderr = self.invoke()
        self.assertEqual(result, 0, stderr)
        self.confirm.assert_called_once()
        self.assertTrue(self.command.is_symlink())
        old_compiler = self.command.resolve()
        old_directory = self.prefix / "opt/woozy-0.1.0-rc.9"
        self.assertEqual(old_compiler, old_directory / "woozyc")

        self.confirm.reset_mock()
        self.publish("v0.1.0-rc.10")
        self.publish("v0.1.0-rc.8")
        result, _, stderr = self.invoke("--yes")
        self.assertEqual(result, 0, stderr)
        self.confirm.assert_not_called()
        self.assertTrue(self.command.is_symlink())
        self.assertEqual(self.command.resolve(), self.prefix / "opt/woozy-0.1.0-rc.10/woozyc")
        self.assertTrue(old_compiler.is_file())
        self.assertTrue((old_directory / "toolchain/clang").is_file())

        self.download.reset_mock()
        result, _, stderr = self.invoke("--yes")
        self.assertEqual(result, 0, stderr)
        self.download.assert_not_called()

    def test_release_selection_respects_channel_and_platform(self):
        self.publish("v0.1.0")
        self.publish("v0.2.0-rc.1")
        self.publish("v0.3.0")
        self.releases[0]["draft"] = True
        self.publish("v0.4.0")
        self.releases[0]["assets"].pop()
        self.assertEqual(install.select_release(self.releases, TARGET).tag, "v0.1.0")
        self.assertEqual(install.select_release(self.releases, TARGET, "v0.1.0-rc.9").tag,
                         "v0.2.0-rc.1")
        self.assertIsNone(install.select_release(self.releases, "macos-arm64"))

    def test_invalid_upgrade_never_switches_current_version(self):
        self.publish("v0.1.0-rc.2")
        result, _, stderr = self.invoke("--yes")
        self.assertEqual(result, 0, stderr)
        old_target = self.command.readlink()
        for fault, message in (("checksum", "SHA-256"), ("GitHub digest", "GitHub asset digest"),
                               ("unsafe archive", "Woozy archive")):
            with self.subTest(fault=fault):
                self.publish("v0.1.0-rc.3", fault=fault)
                result, _, stderr = self.invoke("--yes")
                self.assertNotEqual(result, 0, stderr)
                self.assertIn(message, stderr)
                self.assertEqual(self.command.readlink(), old_target)
                self.assertTrue(self.command.is_file())
                self.assertFalse((self.prefix / "opt/woozy-0.1.0-rc.3").exists())
                self.assertEqual(list(self.prefix.rglob("outside")), [])
                self.releases.pop(0)

    def test_unmanaged_compiler_is_preserved(self):
        self.publish("v0.1.0-rc.2")
        self.command.parent.mkdir(parents=True)
        self.command.write_text("User compiler\n")
        result, stdout, stderr = self.invoke("--check")
        self.assertEqual(result, 0, stderr)
        self.assertIn("v0.1.0-rc.2", stdout)
        result, _, stderr = self.invoke("--yes")
        self.assertNotEqual(result, 0, stderr)
        self.assertFalse(self.command.is_symlink())
        self.assertEqual(self.command.read_text(), "User compiler\n")
        self.download.assert_not_called()


if __name__ == "__main__":
    unittest.main()
