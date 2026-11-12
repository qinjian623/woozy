#!/usr/bin/env python3
"""Build and run the standalone Unicode runtime checks.

The final executable is linked with -nostdlib -static against an archive made
from the current runtime/native sources plus the new Unicode objects. Every
subprocess has an explicit timeout and the report retains the exact commands
and captured results. Python only drives the compiler; it never implements or
substitutes the Unicode operations under test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone

VENDOR_SHA256 = {
    "runtime/native/unicode/vendor/utf8proc/utf8proc.c":
        "f38438eb986ba7eedad0f2e820a70fae1aaf05197364ba0381a5e84775099ef8",
    "runtime/native/unicode/vendor/utf8proc/utf8proc.h":
        "08da85e820efaa3613500946c26b2dc427a07f76001ed7c16bc90f298b814c35",
    "runtime/native/unicode/vendor/utf8proc/utf8proc_data.c":
        "b4911d4162d216afa612317a9b981851e15d672f4d93b25f3a1eb986f8c0aedb",
    "runtime/native/unicode/vendor/utf8proc/LICENSE.md":
        "3b510150d34f248a221bb88e1d811238d6c6c18b51231822c42974c39bb07256",
}
DATA_SHA256 = {
    "NormalizationTest.txt":
        "25a50d816764b04abfb4a646d3eb2b2a803284c3873d9a06757b94fe4513dde3",
    "CaseFolding.txt":
        "a004797658a457bec4dc11683e39f69249ea3b595b752dbea6721c4c9f587b0d",
    "GraphemeBreakTest.txt":
        "b0cf047ee94485bbdc846de2b902f5f8a815f6b674f9d04223cddadd91c9df31",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--out", type=Path,
                        default=Path(__file__).resolve().parent / "results")
    parser.add_argument("--clang", default=os.environ.get("CLANG", "clang"))
    parser.add_argument("--clangxx", default=os.environ.get("CLANGXX", "clang++"))
    parser.add_argument("--ar", default=os.environ.get("AR", "ar"))
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()

    root = args.root.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    build = (args.build_dir or Path(tempfile.mkdtemp(prefix="woozy-unicode-"))).resolve()
    build.mkdir(parents=True, exist_ok=True)
    objects = build / "obj"
    objects.mkdir(exist_ok=True)
    data_dir = (root / "tests/native/unicode/data").resolve()
    report = {
        "status": "failed",
        "checked_at": datetime.now(timezone.utc).isoformat(),
        "root": str(root),
        "build_dir": str(build),
        "data_dir": str(data_dir),
        "commands": [],
        "checks": {},
        "results": {},
    }

    def command(label, argv, timeout=120, cwd=None):
        started = time.monotonic()
        try:
            process = subprocess.run([str(x) for x in argv], cwd=str(cwd or root),
                                     stdin=subprocess.DEVNULL, capture_output=True,
                                     text=True, timeout=timeout)
            row = {
                "label": label,
                "argv": [str(x) for x in argv],
                "cwd": str(cwd or root),
                "timeout_seconds": timeout,
                "status": "exited",
                "exit": process.returncode,
                "stdout": process.stdout,
                "stderr": process.stderr,
                "duration_seconds": round(time.monotonic() - started, 3),
            }
        except subprocess.TimeoutExpired as error:
            row = {
                "label": label,
                "argv": [str(x) for x in argv],
                "cwd": str(cwd or root),
                "timeout_seconds": timeout,
                "status": "timed_out",
                "exit": None,
                "stdout": (error.stdout or "").decode("utf-8", "backslashreplace")
                          if isinstance(error.stdout, bytes) else (error.stdout or ""),
                "stderr": (error.stderr or "").decode("utf-8", "backslashreplace")
                          if isinstance(error.stderr, bytes) else (error.stderr or ""),
                "duration_seconds": round(time.monotonic() - started, 3),
            }
        report["commands"].append(row)
        if row["status"] != "exited":
            raise RuntimeError(f"{label} timed out")
        if row["exit"] != 0:
            raise RuntimeError(f"{label} exited {row['exit']}: {row['stderr']}")
        return row

    try:
        for name, path in (("clang", args.clang), ("clang++", args.clangxx), ("ar", args.ar)):
            found = shutil.which(path)
            if not found:
                raise RuntimeError(f"missing tool: {path}")
            report.setdefault("tools", {})[name] = command(
                "version:" + name, [found, "--version"], timeout=20)["stdout"].splitlines()[0]
        report["vendor_sha256"] = {
            name: digest(root / name) for name in VENDOR_SHA256
        }
        report["data_sha256"] = {
            name: digest(data_dir / name) for name in DATA_SHA256
        }
        if report["vendor_sha256"] != VENDOR_SHA256:
            raise RuntimeError("vendored utf8proc files do not match the pinned SHA-256 list")
        if report["data_sha256"] != DATA_SHA256:
            raise RuntimeError("Unicode test data does not match the pinned SHA-256 list")

        cxx_flags = [
            "-std=c++20", "-O2", "-ffreestanding", "-fno-builtin", "-fno-exceptions",
            "-fno-rtti", "-fno-sanitize=all", "-fno-builtin-memcpy",
            "-fno-builtin-memset", "-fno-builtin-memmove",
            "-fno-stack-protector", "-fPIC", f"-I{root / 'runtime/native'}",
        ]
        c_flags = [
            "-std=c11", "-O2", "-ffreestanding", "-fno-builtin", "-fno-sanitize=all",
            "-fno-stack-protector", "-fPIC", "-fvisibility=hidden",
        ]

        runtime_sources = sorted((root / "runtime/native").glob("*.cpp"))
        unicode_cpp = root / "runtime/native/unicode/unicode.cpp"
        unicode_c = root / "runtime/native/unicode/utf8proc_adapter.c"
        built_objects = []
        unicode_object = objects / "unicode.o"
        for source in runtime_sources + [unicode_cpp]:
            obj = unicode_object if source == unicode_cpp else objects / (source.stem + ".o")
            command("compile:" + source.name, [args.clangxx, *cxx_flags, "-c", source, "-o", obj])
            built_objects.append(obj)
        vendor_object = objects / "utf8proc_adapter.o"
        command("compile:utf8proc_adapter.c", [args.clang, *c_flags, "-c", unicode_c,
                                               "-o", vendor_object])
        built_objects.append(vendor_object)

        archive = build / "liblang_native_unicode_test.a"
        command("archive", [args.ar, "rcs", archive, *built_objects], timeout=60)
        symbol_audit = command("audit:archive-symbols", [
            sys.executable, root / "tests/native/check_runtime_symbols.py",
            "--archive", archive, "--nm", "nm",
        ], timeout=30)
        report["checks"]["archive_symbol_audit"] = json.loads(symbol_audit["stdout"])
        if report["checks"]["archive_symbol_audit"].get("status") != "passed":
            raise RuntimeError("runtime archive has external symbols")

        vendor_undefined = command("audit:vendor-undefined",
                                   ["nm", "--undefined-only", "--format=posix", vendor_object],
                                   timeout=20)["stdout"]
        forbidden = {"malloc", "free", "realloc", "calloc", "strlen", "memcpy",
                     "memmove", "memset"}
        vendor_names = set()
        for line in vendor_undefined.splitlines():
            parts = line.split()
            if parts:
                vendor_names.add(parts[0])
        leaked = sorted(vendor_names & forbidden)
        report["checks"]["vendor_undefined"] = sorted(vendor_names)
        report["checks"]["vendor_forbidden_runtime_calls"] = leaked
        if leaked:
            raise RuntimeError(f"vendored adapter still references hosted functions: {leaked}")

        host_smoke = build / "host_smoke"
        command("link:host-smoke", [
            args.clangxx, "-std=c++20", "-O2", f"-I{root / 'runtime/native'}",
            root / "tests/native/unicode/host_smoke_test.cpp", archive, "-o", host_smoke,
        ])
        report["results"]["host_smoke"] = command("run:host-smoke", [host_smoke], timeout=30)

        vector_source = root / "tests/native/unicode/vector_test.cpp"
        data_macro = f'-DLANG_UNICODE_TEST_DATA_DIR="{data_dir}"'
        vector_host_object = objects / "vector_test_host.o"
        command("compile:vector-host", [args.clangxx, *cxx_flags, data_macro, "-c",
                                        vector_source, "-o", vector_host_object])
        host_vector = build / "vector_host"
        command("link:host-vector", [args.clangxx, vector_host_object, archive,
                                    "-o", host_vector])
        report["results"]["host_vector"] = command(
            "run:host-vector", [host_vector], timeout=120)

        vector_static_object = objects / "vector_test_static.o"
        command("compile:vector-static", [args.clangxx, *cxx_flags, data_macro, "-c",
                                          vector_source, "-o", vector_static_object])
        static_vector = build / "vector_static"
        command("link:static-vector", [
            args.clangxx, "-nostdlib", "-static", "-Wl,-e,_start",
            "-Wl,--build-id=none", vector_static_object, archive, "-o", static_vector,
        ])
        elf_headers = command("audit:readelf-headers", ["readelf", "-l", static_vector],
                              timeout=20)["stdout"]
        elf_dynamic = command("audit:readelf-dynamic", ["readelf", "-d", static_vector],
                              timeout=20)["stdout"]
        elf_type = command("audit:readelf-type", ["readelf", "-h", static_vector],
                           timeout=20)["stdout"]
        undefined = command("audit:undefined-symbols", [
            "nm", "--undefined-only", "--format=posix", static_vector], timeout=20)["stdout"]
        strong_undefined = []
        for line in undefined.splitlines():
            parts = line.split()
            if len(parts) >= 2 and not parts[1].islower():
                strong_undefined.append(parts[0])
        report["checks"].update({
            "elf_type_exec": bool(re.search(r"Type:\s+EXEC", elf_type)),
            "has_interp": "INTERP" in elf_headers,
            "has_needed": "(NEEDED)" in elf_dynamic,
            "dynamic_section": elf_dynamic.strip(),
            "undefined_symbols": undefined.splitlines(),
            "strong_undefined_symbols": strong_undefined,
        })
        if not report["checks"]["elf_type_exec"]:
            raise RuntimeError("static test is not an ET_EXEC executable")
        if report["checks"]["has_interp"]:
            raise RuntimeError("static test contains an INTERP program header")
        if report["checks"]["has_needed"]:
            raise RuntimeError("static test contains a NEEDED entry")
        if strong_undefined:
            raise RuntimeError(f"static test has strong undefined symbols: {strong_undefined}")

        report["results"]["static_vector"] = command(
            "run:static-vector", [static_vector], timeout=120)
        expected = "PASS normalization lines=20171 checks=403420 casefold lines=1640 " \
                   "cf=1606 skipped=34 grapheme lines=853 smoke=11"
        if expected not in report["results"]["static_vector"]["stdout"]:
            raise RuntimeError("static vector summary differs from the complete official-vector run")

        leak_source = root / "tests/native/unicode/leak_test.cpp"
        leak_object = objects / "leak_test.o"
        command("compile:leak-test", [args.clangxx, *cxx_flags, "-c", leak_source,
                                      "-o", leak_object])
        leak_exe = build / "leak_static"
        command("link:static-leak", [
            args.clangxx, "-nostdlib", "-static", "-Wl,-e,_start",
            "-Wl,--build-id=none", "-Wl,--wrap=lang_alloc_v1",
            "-Wl,--wrap=lang_free_v1", leak_object, archive, "-o", leak_exe,
        ])
        leak_headers = command("audit:leak-readelf-headers", ["readelf", "-l", leak_exe],
                               timeout=20)["stdout"]
        leak_dynamic = command("audit:leak-readelf-dynamic", ["readelf", "-d", leak_exe],
                               timeout=20)["stdout"]
        leak_undefined = command("audit:leak-undefined-symbols", [
            "nm", "--undefined-only", "--format=posix", leak_exe], timeout=20)["stdout"]
        if "INTERP" in leak_headers or "(NEEDED)" in leak_dynamic or leak_undefined.strip():
            raise RuntimeError("leak test static link is not self-contained")
        report["results"]["static_leak"] = command(
            "run:static-leak", [leak_exe], timeout=30)
        report["status"] = "passed"
    except Exception as error:
        report["error"] = str(error)
    finally:
        (out / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
        print(json.dumps({
            "status": report["status"],
            "error": report.get("error"),
            "report": str(out / "report.json"),
            "results": {name: row.get("stdout", "").strip()
                        for name, row in report.get("results", {}).items()},
            "checks": report.get("checks", {}),
        }, indent=2, ensure_ascii=False))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
