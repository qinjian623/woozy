import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class EditorSetupTest(unittest.TestCase):
    def test_install_repeat_and_conflict(self):
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp)
            config = home / "config"
            env = {**os.environ, "HOME": temp, "XDG_CONFIG_HOME": str(config), "NVIM_APPNAME": "nvim"}
            compiler = home / "woozyc"
            compiler.write_text("#!/bin/sh\nprintf 'woozyc 0.1\\n'\n")
            compiler.chmod(0o755)
            setup = Path(__file__).with_name("setup.py")
            command = [sys.executable, setup, "--compiler", compiler]

            init = home / ".emacs"
            original = b";; Existing settings\n(setq inhibit-startup-screen t)\n"
            init.write_bytes(original)
            plugin = config / "nvim/plugin/woozy.lua"
            plugin.parent.mkdir(parents=True)
            plugin.write_bytes(b"-- User config\n")
            rejected = subprocess.run(command, env=env, capture_output=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertEqual(plugin.read_bytes(), b"-- User config\n")
            self.assertEqual(init.read_bytes(), original)
            self.assertFalse((home / ".emacs.d").exists())

            plugin.unlink()
            subprocess.run(command, env=env, capture_output=True, check=True)
            installed = init.read_bytes()
            self.assertTrue(installed.startswith(original))
            self.assertIn(b"vim.lsp.start", plugin.read_bytes())
            repeated = subprocess.run(command, env=env, capture_output=True, check=True)
            self.assertIn(b"up to date", repeated.stdout)
            self.assertEqual(init.read_bytes(), installed)


if __name__ == "__main__":
    unittest.main()
