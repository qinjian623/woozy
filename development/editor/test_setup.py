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

    def test_classic_vim_and_emacs(self):
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp)
            config = home / "config"
            env = {**os.environ, "HOME": temp, "XDG_CONFIG_HOME": str(config), "NVIM_APPNAME": "nvim"}
            # A usable Vim package makes this test independent of the network.
            env.update(https_proxy="http://127.0.0.1:1", no_proxy="")
            compiler = home / "woozyc with spaces"
            compiler.write_text("#!/bin/sh\nprintf 'woozyc 0.1\\n'\n")
            compiler.chmod(0o755)
            setup = Path(__file__).with_name("setup.py")
            command = [sys.executable, setup, "--editor", "vim-emacs", "--compiler", compiler]

            init = home / ".emacs.d/init.el"
            init.parent.mkdir()
            original_init = b";; Existing Emacs settings\n(setq inhibit-startup-screen t)\n"
            init.write_bytes(original_init)
            vimrc = home / ".vimrc"
            original_vimrc = b'" Existing Vim settings\nset number\n'
            vimrc.write_bytes(original_vimrc)
            vim = home / ".vim"
            existing_lsp = vim / "pack/existing/start/vim-lsp"
            for filename in ("plugin/lsp.vim", "autoload/lsp.vim"):
                file = existing_lsp / filename
                file.parent.mkdir(parents=True)
                file.write_bytes(b'" Existing vim-lsp\n')

            plugin = vim / "plugin/woozy-lsp.vim"
            plugin.parent.mkdir()
            plugin.write_bytes(b'" User config\n')
            rejected = subprocess.run(command, env=env, capture_output=True, timeout=10)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn(b"woozy-lsp.vim", rejected.stderr)
            self.assertEqual(plugin.read_bytes(), b'" User config\n')
            self.assertEqual(init.read_bytes(), original_init)
            self.assertFalse((init.parent / "lisp").exists())

            plugin.unlink()
            subprocess.run(command, env=env, capture_output=True, timeout=10, check=True)
            installed = init.read_bytes()
            vim_plugin = plugin.read_bytes()
            self.assertTrue(installed.startswith(original_init))
            self.assertIn(str(compiler).encode(), vim_plugin)
            self.assertNotIn(b"@WOOZYC@", vim_plugin)
            self.assertEqual((vim / "syntax/woozy.vim").read_bytes(),
                             (setup.parent / "nvim/syntax/woozy.vim").read_bytes())
            self.assertEqual(vimrc.read_bytes(), original_vimrc)
            self.assertFalse((config / "nvim").exists())
            self.assertFalse((vim / "pack/woozy").exists())
            self.assertEqual((existing_lsp / "plugin/lsp.vim").read_bytes(), b'" Existing vim-lsp\n')
            repeated = subprocess.run(command, env=env, capture_output=True, timeout=10, check=True)
            self.assertIn(b"up to date", repeated.stdout)
            self.assertEqual(plugin.read_bytes(), vim_plugin)
            self.assertEqual(init.read_bytes(), installed)


if __name__ == "__main__":
    unittest.main()
