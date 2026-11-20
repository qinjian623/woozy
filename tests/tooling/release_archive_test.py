import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from build_release import archive_bundle


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


if __name__ == "__main__":
    unittest.main()
