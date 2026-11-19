#!/usr/bin/env python3
"""Independent conformance tests for selfhost/src/wzcc/toml.

The Woozy parser never starts Python. This harness is the test driver: it
builds a small Woozy program that imports app/wzcc/toml, feeds each TOML
document to the built binary on stdin and compares the canonical dump with
CPython's tomllib, which is used only as an expectation oracle here.

Covered:
- cases/valid/*.toml: tomllib accepts, the Woozy parser must accept and
  produce the same value tree (string escapes, integer/float/date-time kinds,
  array and table order).
- cases/invalid/*.toml: tomllib rejects, the Woozy parser must report a
  TOML syntax error (exit 2) with an offset inside the original input text.
  expected.json may pin an offset or message substring for individual cases.
- cases/compat/*.toml: documents the installed toml++ accepts but Python
  tomllib cannot judge (year 0 is a Gregorian leap year; Python datetime has
  no year 0). Each has a sibling .expected canonical dump and is compared
  byte for byte; tomllib rejection is asserted so the bypass stays visible.
- cases/limits/*.toml: documented resource limits; the Woozy parser must
  report Error.Syntax with the documented message.
- real project manifests (every */lang.toml plus curated dependency-heavy
  files) are compared against tomllib exactly like valid cases.
- the module sources are checked for forbidden host/FFI/exec use.

Both Native O0 and O2 builds of the driver are tested.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    import tomllib
except ImportError:  # pragma: no cover - old interpreter
    print("not_run: Python 3.11+ tomllib is required for the oracle")
    raise SystemExit(1)

REPO = Path(__file__).resolve().parents[3]
MODULE_DIR = REPO / "selfhost/src/wzcc/toml"
DRIVER = REPO / "selfhost/tests/toml/driver.wz"
CASES = REPO / "selfhost/tests/toml/cases"
DEFAULT_CLI = REPO / "build/woozy-stdlib-native/woozyc"
TOOLCHAIN_ROOT = Path(os.environ.get("WOOZY_TOOLCHAIN_ROOT") or
                      (REPO.parent / "toolchains"))
LIBRARY_PATH = (
    f"{TOOLCHAIN_ROOT / 'llvm-22.1.8/lib'}:"
    f"{TOOLCHAIN_ROOT / 'libxml2-legacy-2.13.9/usr/lib'}"
)
CURATED_MANIFESTS = [
    "selfhost/lang.toml",
    "examples/hello/lang.toml",
    "tests/async/basic/lang.toml",
    "tests/native/c_ffi/forged-project/lang.toml",
    "tests/python_bindings/string-aggregate/lang.toml",
]
EXPECTED_LIMITS = {
    "depth_exceeded.toml": "container nesting is too deep",
    "key_path_too_long.toml": "key path is too long",
}
FORBIDDEN_SOURCE = [
    "unsafe",
    "__woozy_",
    "extern ",
    'import "std/ffi"',
    'import "std/process"',
    'import "std/os"',
    'import "std/io"',
    "lang_native_helper",
    "woozyc",
]


class RawFloat(float):
    """Keeps the exact TOML float token so raw text can be compared."""

    def __new__(cls, token: str):
        value = super().__new__(cls, float(token))
        value.raw = token
        return value


def escape(value: str) -> str:
    out = ['"']
    for ch in value:
        code = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif code < 0x20 or code == 0x7F:
            out.append("\\u%04x" % code)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def canonical_time(value: datetime.time) -> str:
    text = "%02d:%02d:%02d" % (value.hour, value.minute, value.second)
    if value.microsecond:
        text += ".%06d" % value.microsecond
    return text


def canonical_datetime(value: datetime.datetime) -> str:
    text = "%04d-%02d-%02dT%s" % (
        value.year, value.month, value.day, canonical_time(value)
    )
    offset = value.utcoffset()
    if offset is not None:
        seconds = int(offset.total_seconds())
        if seconds == 0:
            text += "Z"
        else:
            sign = "+" if seconds >= 0 else "-"
            seconds = abs(seconds)
            text += "%s%02d:%02d" % (sign, seconds // 3600, (seconds % 3600) // 60)
    return text


def dump(value) -> str:
    if isinstance(value, bool):
        return "B1" if value else "B0"
    if isinstance(value, RawFloat):
        return "F" + escape(value.raw)
    if isinstance(value, int):
        return "I" + escape(str(value))
    if isinstance(value, str):
        return "S" + escape(value)
    if isinstance(value, datetime.datetime):
        tag = "ODT" if value.utcoffset() is not None else "LDT"
        return tag + escape(canonical_datetime(value))
    if isinstance(value, datetime.date):
        return "LD" + escape("%04d-%02d-%02d" % (value.year, value.month, value.day))
    if isinstance(value, datetime.time):
        return "LT" + escape(canonical_time(value))
    if isinstance(value, list):
        return "A[" + ",".join(dump(item) for item in value) + "]"
    if isinstance(value, dict):
        return "T[" + ",".join(
            escape(key) + "=" + dump(item) for key, item in value.items()
        ) + "]"
    raise TypeError("unexpected oracle value: %r" % (value,))


def oracle(text: str):
    try:
        return True, dump(tomllib.loads(text, parse_float=RawFloat))
    except Exception as error:  # TOMLDecodeError and Python date ValueError
        return False, str(error)


def run_binary(binary: Path, text: str):
    result = subprocess.run(
        [str(binary)], input=text.encode("utf-8"), capture_output=True, timeout=30
    )
    return result.returncode, result.stdout.decode("utf-8", "replace")


def build_driver(cli: Path, workspace: Path, opt: str, env) -> Path:
    project = workspace / ("project-o" + opt)
    source = project / "src"
    source.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(DRIVER, source / "main.wz")
    link = source / "wzcc"
    if link.is_symlink() or link.exists():
        if link.is_symlink():
            link.unlink()
        else:
            shutil.rmtree(link)
    link.symlink_to(REPO / "selfhost/src/wzcc")
    (project / "lang.toml").write_text(
        'schema_version = 1\n[package]\nname = "toml_tests"\n'
        'source_root = "src"\nentry = "app"\n[build]\n'
        'backend = "native"\nword_bits = 64\n',
        encoding="utf-8",
    )
    out = workspace / ("out-o" + opt)
    result = subprocess.run(
        [str(cli), "build", "--project", str(project / "lang.toml"),
         "--backend", "native", "--out-dir", str(out), "--opt", opt],
        capture_output=True, text=True, env=env, timeout=300,
    )
    if result.returncode != 0:
        raise RuntimeError(
            "build failed (opt %s):\n%s%s" % (opt, result.stdout, result.stderr)
        )
    binary = out / "program"
    if not binary.exists():
        raise RuntimeError("build produced no executable: %s" % binary)
    return binary


def source_purity() -> list[str]:
    problems = []
    for path in sorted(MODULE_DIR.glob("*.wz")):
        text = path.read_text(encoding="utf-8")
        for token in FORBIDDEN_SOURCE:
            if token in text:
                problems.append("%s contains %r" % (path.name, token))
        for line in text.splitlines():
            if line.startswith("import "):
                if 'import "std/text"' in line or 'import "std/vec"' in line:
                    continue
                problems.append("%s imports %r" % (path.name, line.strip()))
    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, default=DEFAULT_CLI)
    parser.add_argument("--workspace", type=Path, default=REPO / "build/toml-tests")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-manifests", action="store_true")
    args = parser.parse_args()

    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = LIBRARY_PATH + (
        ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else ""
    )
    if not args.cli.exists():
        print("not_run: stage0 compiler not found at %s" % args.cli)
        return 1

    workspace = args.workspace
    workspace.mkdir(parents=True, exist_ok=True)
    binaries = {}
    for opt in ("0", "2"):
        binary = workspace / ("out-o" + opt) / "program"
        if not args.skip_build:
            print("build: native O%s driver" % opt)
            binary = build_driver(args.cli, workspace, opt, env)
        binaries[opt] = binary

    expected_path = CASES / "invalid/expected.json"
    expected = (
        json.loads(expected_path.read_text(encoding="utf-8"))
        if expected_path.exists() else {}
    )

    failures: list[str] = []
    counts = {"valid": 0, "invalid": 0, "compat": 0, "manifests": 0, "limits": 0}

    def check_valid(name: str, text: str, category: str) -> None:
        counts[category] += 1
        accepted, want = oracle(text)
        if not accepted:
            failures.append("%s: oracle rejected a valid case: %s" % (name, want))
            return
        for opt, binary in binaries.items():
            code, out = run_binary(binary, text)
            if code != 0:
                failures.append("%s [O%s]: parser rejected: %s" % (name, opt, out.strip()))
            elif out.strip("\n") != want:
                failures.append(
                    "%s [O%s]: value mismatch\n  oracle: %s\n  parser: %s"
                    % (name, opt, want, out.strip())
                )

    def check_invalid(name: str, text: str) -> None:
        counts["invalid"] += 1
        accepted, want = oracle(text)
        if accepted:
            failures.append("%s: tomllib accepts this case, so it is not invalid" % name)
            return
        # Offsets are Unicode code point indices into the original input.
        limit = len(text)
        for opt, binary in binaries.items():
            code, out = run_binary(binary, text)
            if code != 2:
                failures.append(
                    "%s [O%s]: expected syntax error exit 2, got %d: %r"
                    % (name, opt, code, out[:160])
                )
                continue
            line = out.strip()
            if not line.startswith("error offset=") or " message=" not in line:
                failures.append("%s [O%s]: malformed error line: %r" % (name, opt, line))
                continue
            offset_text, message = line[len("error offset="):].split(" message=", 1)
            try:
                offset = int(offset_text)
            except ValueError:
                failures.append("%s [O%s]: non-numeric offset: %r" % (name, opt, offset_text))
                continue
            if offset < 0 or offset > limit:
                failures.append(
                    "%s [O%s]: offset %d outside 0..%d" % (name, opt, offset, limit)
                )
            if not message:
                failures.append("%s [O%s]: empty diagnostic message" % (name, opt))
            rule = expected.get(name, {})
            if "offset" in rule and rule["offset"] != offset:
                failures.append(
                    "%s [O%s]: offset %d, expected %d"
                    % (name, opt, offset, rule["offset"])
                )
            if "message_contains" in rule and rule["message_contains"] not in message:
                failures.append(
                    "%s [O%s]: message %r does not contain %r"
                    % (name, opt, message, rule["message_contains"])
                )

    for path in sorted((CASES / "valid").glob("*.toml")):
        check_valid(path.name, path.read_bytes().decode("utf-8"), "valid")
    for path in sorted((CASES / "invalid").glob("*.toml")):
        check_invalid(path.name, path.read_bytes().decode("utf-8"))
    for path in sorted((CASES / "compat").glob("*.toml")):
        counts["compat"] += 1
        text = path.read_bytes().decode("utf-8")
        expected_file = path.with_suffix(".expected")
        if not expected_file.exists():
            failures.append("%s: missing .expected compat dump" % path.name)
            continue
        want = expected_file.read_text(encoding="utf-8").strip("\n")
        accepted, reason = oracle(text)
        if accepted:
            failures.append(
                "%s: tomllib accepts this compat fixture; it belongs in cases/valid"
                % path.name
            )
        for opt, binary in binaries.items():
            code, out = run_binary(binary, text)
            if code != 0:
                failures.append(
                    "%s [O%s]: parser rejected compat fixture: %s"
                    % (path.name, opt, out.strip())
                )
            elif out.strip("\n") != want:
                failures.append(
                    "%s [O%s]: compat value mismatch\n  expected: %s\n  parser: %s"
                    % (path.name, opt, want, out.strip())
                )
    for path in sorted((CASES / "limits").glob("*.toml")):
        counts["limits"] += 1
        text = path.read_bytes().decode("utf-8")
        wanted = EXPECTED_LIMITS[path.name]
        for opt, binary in binaries.items():
            code, out = run_binary(binary, text)
            if code != 2 or wanted not in out:
                failures.append(
                    "%s [O%s]: expected documented limit %r, got %d %r"
                    % (path.name, opt, wanted, code, out.strip()[:160])
                )

    manifests: list[Path] = []
    for relative in CURATED_MANIFESTS:
        path = REPO / relative
        if path.exists():
            manifests.append(path)
    if not args.skip_manifests:
        for path in sorted(REPO.glob("**/lang.toml")):
            if "build" in path.parts:
                continue
            if path not in manifests:
                manifests.append(path)
    for path in manifests:
        check_valid(str(path.relative_to(REPO)), path.read_text(encoding="utf-8"), "manifests")

    problems = source_purity()
    if problems:
        failures.extend("source purity: " + item for item in problems)

    print(
        "python %s, oracle tomllib, cli %s"
        % (sys.version.split()[0], args.cli)
    )
    print(
        "valid=%d invalid=%d compat=%d manifests=%d limits=%d builds=O0,O2"
        % (counts["valid"], counts["invalid"], counts["compat"],
           counts["manifests"], counts["limits"])
    )
    if failures:
        print("failed=%d" % len(failures))
        for failure in failures:
            print("FAIL " + failure)
        return 1
    print("passed=%d failed=0 not_run=0" % (sum(counts.values()) * 2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
