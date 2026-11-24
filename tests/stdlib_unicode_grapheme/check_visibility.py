#!/usr/bin/env python3
"""Check that the grapheme scanner stays package-private.

Top-level declarations are package-visible by default and `pub` enables
cross-package access.  The focused fixture must pass using only the public
std/unicode.boundaries; an app-package probe naming the internal
wz_grapheme_boundaries must be rejected with E_PRIVATE_ACCESS.
"""

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = Path(__file__).resolve().parent / "project.toml"


def run_check(compiler, project):
    result = subprocess.run(
        [str(compiler), "check", "--project", str(project), "--format", "json"],
        capture_output=True, text=True)
    try:
        report = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        raise SystemExit("invalid compiler JSON: %s\n%s" % (error, result.stdout))
    return result.returncode, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    args = parser.parse_args()
    env = os.environ.copy()
    legacy = ROOT.parent / "toolchains/libxml2-legacy-2.13.9/usr/lib"
    if legacy.is_dir():
        env["LD_LIBRARY_PATH"] = str(legacy) + ":" + env.get("LD_LIBRARY_PATH", "")
    previous = os.environ.get("LD_LIBRARY_PATH")
    os.environ.update(env)
    try:
        code, public = run_check(args.compiler, FIXTURE)
        if code != 0 or public.get("status") != "ok":
            raise SystemExit("public fixture failed:\n%s" % json.dumps(public, indent=2))
        with tempfile.TemporaryDirectory(prefix="grapheme-visibility-") as directory:
            source = Path(directory) / "src"
            source.mkdir()
            (source / "main.wz").write_text(
                'package app;\n'
                'import "std/unicode" as unicode;\n'
                'func main() i32 { return i32(unicode.wz_grapheme_boundaries("abc")[0]); }\n')
            project = Path(directory) / "lang.toml"
            project.write_text(
                'schema_version = 1\n[package]\nname = "app"\nsource_root = "src"\n'
                'entry = "app"\n[build]\nbackend = "native"\nword_bits = 64\n')
            code, private = run_check(args.compiler, project)
        codes = [item.get("code") for item in private.get("diagnostics", [])]
        if code == 0 or "E_PRIVATE_ACCESS" not in codes:
            raise SystemExit("cross-package private access was not rejected:\n%s"
                             % json.dumps(private, indent=2))
        for name, expression in (
                ("read", '__woozy_text_cursor_read("abc",0)'),
                ("slice", '__woozy_text_cursor_slice("abc",0,1)')):
            with tempfile.TemporaryDirectory(
                    prefix="grapheme-cursor-%s-denial-" % name) as directory:
                source = Path(directory) / "src"
                source.mkdir()
                (source / "main.wz").write_text(
                    'package app;\n'
                    'func main() i32 { return i32(%s); }\n' % expression)
                project = Path(directory) / "lang.toml"
                project.write_text(
                    'schema_version = 1\n[package]\nname = "app"\nsource_root = "src"\n'
                    'entry = "app"\n[build]\nbackend = "native"\nword_bits = 64\n')
                code, denied = run_check(args.compiler, project)
            codes = [item.get("code") for item in denied.get("diagnostics", [])]
            if code == 0 or "E_CAPABILITY_UNAVAILABLE" not in codes:
                raise SystemExit("user-module cursor %s access was not rejected:\n%s"
                                 % (name, json.dumps(denied, indent=2)))
        print("visibility ok: public fixture passes; private scanner and "
              "user-module cursor read/slice access are rejected")
    finally:
        if previous is None:
            os.environ.pop("LD_LIBRARY_PATH", None)
        else:
            os.environ["LD_LIBRARY_PATH"] = previous


if __name__ == "__main__":
    main()
