#!/usr/bin/env python3
"""Compile with the C++ helper, link with Clang/LLD, execute ELF files directly.

Python only drives subprocesses and records evidence; it never evaluates the
source language or generated LLVM. No shared fixture is rewritten.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone

PANIC = re.compile(r"LANG_NATIVE_PANIC:v1:(division-by-zero|invalid-shift|integer-conversion):[0-9]+:[0-9]+:[0-9]+\n")


def panic_record(result):
    return (result.get("status") == "exited" and result["exit"] != 0
            and PANIC.fullmatch(result["stderr"]) is not None
            and result["stdout"] == "")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--toolchains", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--report", type=Path, required=True)
    a = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(a.toolchains / "libxml2-legacy-2.13.9/usr/lib") + ":" + env.get("LD_LIBRARY_PATH", "")
    llvm = a.toolchains / "llvm-22.1.8/bin"
    helper = a.build.resolve() / "native/lang_native_helper"
    runtime = a.build.resolve() / "native/liblang_native_runtime.a"
    report = {"task": "T05", "contract": "compiler-dev/1", "checked_at": datetime.now(timezone.utc).isoformat(),
              "scope": "native scalar helper object emission, LLVM verification, Clang/LLD link, and ELF execution",
              "preflight": [], "runs": []}
    failed = []

    def command(argv, timeout=30):
        args = [str(x) for x in argv]
        try:
            r = subprocess.run(args, capture_output=True, text=True, env=env, timeout=timeout)
            return {"argv": args, "status": "exited", "exit": r.returncode, "stdout": r.stdout, "stderr": r.stderr}
        except subprocess.TimeoutExpired:
            return {"argv": args, "status": "timeout", "exit": None, "stdout": "", "stderr": ""}
        except OSError as e:
            return {"argv": args, "status": "launch_failed", "exit": None, "stdout": "", "stderr": str(e)}

    def ok(r):
        return r["status"] == "exited" and r["exit"] == 0

    def save():
        report["failed"] = failed
        report["status"] = "failed" if failed else "passed"
        a.report.parent.mkdir(parents=True, exist_ok=True)
        a.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")

    try:
        report["base_head"] = command(["git", "-C", root, "rev-parse", "HEAD"])["stdout"].strip()
        report["implementation_sha256"] = {
            str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest()
            for pattern in ["include/lang/ir/*.hpp", "include/lang/backend/native.hpp", "src/ir/*.cpp",
                            "src/backend/native/*", "runtime/native/*.cpp", "tests/native/*_test.cpp", "tests/native/run.py"]
            for f in sorted(root.glob(pattern)) if f.is_file()}
        report["helper_sha256"] = hashlib.sha256(helper.read_bytes()).hexdigest()
        report["runtime_sha256"] = hashlib.sha256(runtime.read_bytes()).hexdigest()
        report["tools"] = {name: command([llvm / name, "--version"]) for name in ["clang", "ld.lld", "opt", "llvm-readelf", "llvm-nm"]}
        for name, r in report["tools"].items():
            if not ok(r) or "22.1.8" not in r["stdout"]:
                raise RuntimeError("wrong/missing tool version: " + name)
        common = json.loads((root / "tests/backend_common/cases.json").read_text())["cases"]
        cases = [(c["id"], root / c["manifest"], c["expected"]["status"], c["expected"].get("main_return", 0)) for c in common]
        cases += [(x, root / "tests/fixtures" / x / "lang.toml", "return", 0) for x in ["core", "numeric"]]
        cases += [("mutual-recursion", root / "tests/modules/fixtures/same-package-mutual-recursion/lang.toml", "return", 0)]
        cases += [("cross-package-call", root / "tests/modules/fixtures/cross-package-public/lang.toml", "return", 0)]
        cases += [(x.parent.name, x, "panic" if x.parent.name.startswith("panic-") else "return",
                   7 if x.parent.name == "nonzero-main" else 0)
                  for x in sorted((root / "tests/native/fixtures").glob("*/lang.toml"))]
        # All 15 shared native64 prerequisites must pass before emitting any object.
        for name, manifest, _, _ in cases:
            r = command([helper, "--project", manifest, "--check-only", "--format", "json"])
            accepted = ok(r) and json.loads(r["stdout"])["word_bits"] == 64
            report["preflight"].append({"id": name, "result": "passed" if accepted else "failed", "check": r})
            if not accepted:
                failed.append(name + ": static prerequisite")
        if failed:
            raise RuntimeError("static prerequisites failed; no object emitted")
        report["shared_preflight"] = [len(common), len(common)]
        # These three frozen sources were capability rejections in the original
        # runner. They now use the same build/link/execute loop, while their
        # rows stay separate from the original 35-case run set.
        report["previously_rejected"] = []
        supported = [
            ("strict-float", "func f(x f32) f32 {return x+x;} func main() i32{return 0;}"),
            ("string", 'func main() i32 {let text = "hello";return 0;}'),
            ("defer", "func f() {} func main() i32 {defer {f();} return 0;}"),
        ]
        supported_sources = {}
        external_cases = []
        for name, source in supported:
            d = out / "previously-rejected" / name
            (d / "src").mkdir(parents=True, exist_ok=True)
            manifest = d / "lang.toml"
            manifest.write_text('schema_version=1\n[package]\nname="supported"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=64\n')
            (d / "src/main.lang").write_text("package app;\n" + source + "\n")
            supported_sources[name] = source
            external_cases.append((name, manifest, "return", 0))
        for name, manifest, expected, main_return in cases + external_cases:
            external = name in supported_sources
            for optimization in [0, 2]:
                directory = (out / "previously-rejected" / name if external else out / name) / ("O" + str(optimization))
                directory.mkdir(parents=True, exist_ok=True)
                obj, executable = directory / "program.o", directory / "program"
                ll, before, cfg = directory / "program.ll", directory / "before.ll", directory / "program.nir"
                row = {"id": name, "optimization": optimization, "manifest": str(manifest),
                       "source_sha256": {(str(s.relative_to(root)) if s.is_relative_to(root) else str(s)):
                                         hashlib.sha256(s.read_bytes()).hexdigest()
                                         for s in sorted((manifest.parent / "src").rglob("*.lang"))},
                       "expected": {"status": expected, "main_return": main_return}}
                if external:
                    row["source"] = supported_sources[name]
                    report["previously_rejected"].append(row)
                else:
                    report["runs"].append(row)
                row["compile"] = command([helper, "--project", manifest, "--opt", optimization,
                    "--out-object", obj, "--out-llvm", ll, "--out-llvm-before", before, "--out-native", cfg, "--format", "json"])
                if not ok(row["compile"]):
                    row["result"] = "failed"; failed.append(name + ": compile O" + str(optimization)); continue
                if external:
                    row["object_created"] = obj.is_file() and obj.stat().st_size > 0
                    if not row["object_created"]:
                        row["result"] = "failed"; failed.append(name + ": object missing O" + str(optimization)); continue
                row["metadata"] = json.loads(row["compile"]["stdout"])
                row["verify_files"] = [command([llvm / "opt", "-passes=verify", "-disable-output", f]) for f in [before, ll]]
                if not all(ok(x) for x in row["verify_files"]):
                    row["result"] = "failed"; failed.append(name + ": LLVM verify"); continue
                row["link"] = command([llvm / "clang", "--target=x86_64-unknown-linux-gnu",
                    "-nostdlib", "-static",
                    "--ld-path=" + str(llvm / "ld.lld"), obj, runtime, "-o", executable])
                if not ok(row["link"]):
                    row["result"] = "failed"; failed.append(name + ": link"); continue
                data = executable.read_bytes()
                row["elf_x86_64"] = data[:4] == b"\x7fELF" and data[4] == 2 and data[18:20] == b"\x3e\x00"
                row["executable_sha256"] = hashlib.sha256(data).hexdigest()
                row["readelf_program_headers"] = command([llvm / "llvm-readelf", "-lW", executable])
                row["readelf_dynamic"] = command([llvm / "llvm-readelf", "-dW", executable])
                row["undefined_symbols"] = command([llvm / "llvm-nm", "-u", "--format=posix", executable])
                undefined = [line.split() for line in row["undefined_symbols"]["stdout"].splitlines() if line.split()]
                row["strong_undefined_symbols"] = [fields[0] for fields in undefined
                                                   if len(fields) >= 2 and fields[1] == "U"]
                row["static_elf"] = (
                    ok(row["readelf_program_headers"]) and
                    "Elf file type is EXEC" in row["readelf_program_headers"]["stdout"] and
                    "INTERP" not in row["readelf_program_headers"]["stdout"] and
                    ok(row["readelf_dynamic"]) and
                    "NEEDED" not in row["readelf_dynamic"]["stdout"] and
                    ok(row["undefined_symbols"]) and
                    not row["strong_undefined_symbols"])
                if not row["static_elf"]:
                    row["result"] = "failed"; failed.append(name + ": static ELF O" + str(optimization)); continue
                row["execute"] = command([executable], timeout=5)
                row["panic_record"] = panic_record(row["execute"])
                if expected == "panic":
                    passed = row["panic_record"]
                    cause = "division-by-zero" if "division" in name or "remainder" in name else "invalid-shift" if "shift" in name else "integer-conversion"
                    passed = passed and (":" + cause + ":") in row["execute"]["stderr"]
                else:
                    passed = (row["execute"]["status"] == "exited" and row["execute"]["exit"] == main_return
                              and not row["panic_record"] and row["execute"]["stdout"] == "" and row["execute"]["stderr"] == "")
                passed = passed and row["elf_x86_64"]
                row["result"] = ("supported" if external else "passed") if passed else "failed"
                if not passed:
                    failed.append(name + ": execute O" + str(optimization))
                print(f"{name} O{optimization}: {row['result']} exit={row['execute']['exit']} panic={row['panic_record']}", flush=True)
        # A normal nonzero main has actually run above. Exit status alone never
        # qualifies; neither does a marker from a successful or unlaunched run.
        report["panic_classifier_controls"] = all(not panic_record(x) for x in [
            {"status": "exited", "exit": 7, "stdout": "", "stderr": ""},
            {"status": "exited", "exit": -11, "stdout": "", "stderr": ""},
            {"status": "exited", "exit": 0, "stdout": "", "stderr": "LANG_NATIVE_PANIC:v1:invalid-shift:0:1:2\n"},
            {"status": "launch_failed", "exit": 101, "stdout": "", "stderr": ""},
        ])
        report["rejections"] = []
        rejects = [
            ("native32", "func main() i32 {return 0;}", 32,
             "E_CAPABILITY_UNAVAILABLE", "unsupported"),
            ("ordinary-parameter-write",
             "func f(x i32) i32 {x=1;return x;} func main() i32{return 0;}", 64,
             "E_TYPE_MISMATCH", "rejected"),
        ]
        for name, source, width, expected_code, category in rejects:
            d = out / "rejections" / name
            (d / "src").mkdir(parents=True, exist_ok=True)
            manifest = d / "lang.toml"
            manifest.write_text('schema_version=1\n[package]\nname="reject"\nsource_root="src"\nentry="app"\n[build]\nbackend="native"\nword_bits=' + str(width) + '\n')
            (d / "src/main.lang").write_text("package app;\n" + source + "\n")
            r = command([helper, "--project", manifest, "--out-object", d / "must-not-exist.o", "--format", "json"])
            try:
                codes = [x["code"] for x in json.loads(r["stderr"])["diagnostics"]]
            except (ValueError, KeyError):
                codes = []
            absent = not (d / "must-not-exist.o").exists()
            passed = r["exit"] == 1 and expected_code in codes and absent
            report["rejections"].append({"id": name, "expected_code": expected_code,
                                         "object_absent": absent,
                                         "result": category if passed else "failed",
                                         "command": r})
            if not passed: failed.append(name + ": rejection")
        invalid = out / "invalid-llvm.ll"
        invalid.write_text("""define i32 @broken(i1 %c) {
entry: br i1 %c, label %yes, label %no
yes: %v = add i32 1, 2
br label %join
no: br label %join
join: ret i32 %v
}
""")
        report["bad_llvm"] = command([llvm / "opt", "-passes=verify", "-disable-output", invalid])
        if (ok(report["bad_llvm"]) or "does not dominate" not in report["bad_llvm"]["stderr"]
                or not report["panic_classifier_controls"]):
            failed.append("invalid IR/classifier accepted")
        frontend = a.build.resolve() / "frontend/langc"
        report["frontend_dynamic_dependencies"] = command(["ldd", frontend])
        report["frontend_llvm_symbols"] = command(["nm", "-C", "--undefined-only", frontend])
        if "llvm::" in report["frontend_llvm_symbols"]["stdout"] or "libLLVM" in report["frontend_dynamic_dependencies"]["stdout"]:
            failed.append("frontend depends on LLVM")
        report["summary"] = {"attempts": len(report["runs"]),
            "executions": sum("execute" in r for r in report["runs"]),
            "passed": sum(r.get("result") == "passed" for r in report["runs"]),
            "shared_executions": sum("execute" in r and r["id"] in {x["id"] for x in common} for r in report["runs"])}
    except Exception as e:
        failed.append(str(e))
    save()
    print(json.dumps({"status": report["status"], "failed": failed, "report": str(a.report)}))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
