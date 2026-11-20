"""Check portable, repeatable locks for the inputs the self-host compiler loads."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)


def main():
    compiler = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="woozy-lock-cli-") as temporary:
        root = Path(temporary) / "project"
        manifest = """schema_version = 1
[package]
name = "project"
source_root = "src"
entry = "app"
[dependencies.lib]
kind = "source"
path = "dep space"
"""
        main_source = 'package app; import "lib/lib" as lib; func main() i32 { return lib.value(); }\n'
        write(root / "lang.toml", manifest)
        write(root / "src/main.wz", main_source)
        dep = root / "dep space"
        write(dep / "lang.toml", """schema_version = 1
[package]
name = "local"
source_root = "src"
[dependencies.extra]
kind = "source"
path = "extra"
""")
        write(dep / "src/lib/lib.wz",
              'package lib; import "app/inside" as inside; import "extra/value" as extra; '
              'pub func value() i32 { return inside.value() + extra.value(); }\n')
        inner = dep / "src/inside/inside.wz"
        leaf = dep / "extra/value/value.wz"
        original_source = "package inside; pub func value() i32 { return 0; }\n"
        write(inner, original_source)
        write(leaf, "package value; pub func value() i32 { return 0; }\n")

        def run(directory, *args, code=0, diagnostic=None, name="lang.toml"):
            result = subprocess.run(
                [compiler, *args, "--project", directory / name],
                capture_output=True, text=True, timeout=60,
            )
            assert result.returncode == code, (result.args, result.stdout, result.stderr)
            if diagnostic:
                assert diagnostic in result.stderr, result.stderr

        run(root, "check")
        run(root, "lock", "--check", code=1, diagnostic="E_DEPENDENCY_LOCK")
        run(root, "lock")
        lock = root / "lang.lock"
        baseline = lock.read_bytes()
        assert str(root).encode() not in baseline
        assert b'"dep space/src/inside/inside.wz"' in baseline
        assert b'"dep space/extra/value/value.wz"' in baseline
        run(root, "lock")
        assert lock.read_bytes() == baseline

        copy = Path(temporary) / "relocated"
        shutil.copytree(root, copy)
        run(copy, "lock", "--check")
        run(copy, "lock")
        assert (copy / "lang.lock").read_bytes() == baseline
        write(root / "src/main.wz", main_source + "// application-only edit\n")
        run(root, "check")
        write(inner, original_source + "// drift\n")
        run(root, "check", code=1, diagnostic="E_DEPENDENCY_LOCK")
        output = root / "out"
        write(output / "program", "old output")
        run(root, "build", "--out-dir", str(output), code=1, diagnostic="E_DEPENDENCY_LOCK")
        assert (output / "program").read_text() == "old output"
        write(inner, original_source)
        added = leaf.with_name("added.wz")
        write(added, "package value; pub func new_value() i32 { return 5; }")
        run(root, "lock", "--check", code=1, diagnostic="E_DEPENDENCY_LOCK")
        added.unlink()
        run(root, "check")
        leaf.unlink()
        run(root, "lock", code=2)
        assert lock.read_bytes() == baseline

        copied_lock = copy / "lang.lock"
        copied_lock.write_bytes(b"bad\xff\n")
        run(copy, "check", code=1, diagnostic="E_DEPENDENCY_LOCK")
        copied_lock.unlink()
        os.mkfifo(copied_lock)
        run(copy, "check", code=2, diagnostic="E_INPUT_IO")
        copied_lock.unlink()
        copied_lock.symlink_to("missing.lock")
        run(copy, "check", code=2, diagnostic="E_INPUT_IO")
        copied_lock.unlink()
        (copy / "lang.toml").rename(copy / "alternate.toml")
        run(copy, "lock", name="alternate.toml")
        assert b'"alternate.toml"' in copied_lock.read_bytes()
        run(copy, "lock", "--check", name="alternate.toml")
        copied_lock.unlink()
        config = copy / "alternate.toml"
        config.write_text(config.read_text() + '[dependencies.host]\n'
                          'kind = "python"\nbinding = "missing.json"\nhost_package = "host"\n')
        run(copy, "check", name="alternate.toml")
        run(copy, "lock", name="alternate.toml", code=1, diagnostic="E_CAPABILITY_UNAVAILABLE")
        assert not copied_lock.exists()
    print("dependency lock CLI: passed")


if __name__ == "__main__":
    main()
