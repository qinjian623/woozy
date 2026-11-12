#!/usr/bin/env python3
"""Execute the FFI01 C ABI contract.

Every external process has a timeout and its argv/stdout/stderr/exit status is
recorded in the report. The script never evaluates generated IR or source.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from datetime import datetime, timezone


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--cli", type=Path, required=True)
    p.add_argument("--root", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--clang", type=Path, required=True)
    p.add_argument("--ar", type=Path, required=True)
    p.add_argument("--hosted-runtime", type=Path, required=True)
    p.add_argument("--native-runtime", type=Path, required=True)
    p.add_argument("--forged", type=Path)
    a = p.parse_args()
    root = a.root.resolve()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = {
        "task": "FFI01",
        "contract": "compiler-dev/1",
        "checked_at": datetime.now(timezone.utc).isoformat(),
        "runs": [],
        "failed": [],
    }
    env = os.environ.copy()
    env["WOOZY_FFI_ENV"] = "1"

    def run(argv, timeout=30):
        args = [str(x) for x in argv]
        try:
            r = subprocess.run(args, capture_output=True, text=True, env=env,
                               timeout=timeout)
            return {"argv": args, "status": "exited", "exit": r.returncode,
                    "stdout": r.stdout, "stderr": r.stderr}
        except subprocess.TimeoutExpired:
            return {"argv": args, "status": "timeout", "exit": None,
                    "stdout": "", "stderr": ""}
        except OSError as e:
            return {"argv": args, "status": "launch_failed", "exit": None,
                    "stdout": "", "stderr": str(e)}

    def record(name, **kw):
        entry = {"name": name, **kw}
        report["runs"].append(entry)
        return entry

    def require(condition, name, detail=""):
        if not condition:
            report["failed"].append(name + (": " + detail if detail else ""))

    def ok(result):
        return result["status"] == "exited" and result["exit"] == 0

    def build(project, opt, label):
        target = out / label
        r = run([a.cli, "build", "--project", project, "--backend", "native",
                 "--opt", str(opt), "--out-dir", target, "--format", "json"],
                timeout=180)
        passed = ok(r)
        document = None
        if passed:
            try:
                document = json.loads(r["stdout"].strip().splitlines()[-1])
                passed = document.get("status") in ("ok", "passed")
            except (ValueError, IndexError):
                passed = False
        record("build:" + label, build=passed, target=str(target), result=r)
        require(passed, "build:" + label)
        return target / "program" if passed else None

    def execute(label, program):
        r = run([program], timeout=10)
        record("run:" + label, result=r)
        require(ok(r), "run:" + label, json.dumps(r["stderr"][-300:]))
        return r

    def elf(program):
        r = run(["readelf", "-l", "-d", program], timeout=10)
        record("readelf:" + program.name, result=r)
        require(ok(r), "readelf:" + str(program))
        return r["stdout"]

    try:
        report["head"] = run(["git", "-C", root, "rev-parse", "HEAD"])["stdout"].strip()
        fixture = root / "tests/native/c_ffi/fixture/ffi_fixture.c"
        fixture_obj = out / "ffi_fixture.o"
        fixture_lib = out / "libffi_fixture.a"
        compile = run([a.clang, "--target=x86_64-unknown-linux-gnu", "-O2",
                       "-fPIC", "-c", fixture, "-o", fixture_obj], timeout=60)
        record("fixture:compile", result=compile)
        require(ok(compile), "fixture:compile", compile["stderr"][-300:])
        archive = run([a.ar, "rcD", fixture_lib, fixture_obj], timeout=60)
        record("fixture:archive", result=archive)
        require(ok(archive), "fixture:archive", archive["stderr"][-300:])
        if report["failed"]:
            raise RuntimeError("fixture build failed")
        fixture_link_dir = root / "tests/native/c_ffi/.build"
        fixture_link_dir.mkdir(parents=True, exist_ok=True)
        for artifact in (fixture_obj, fixture_lib):
            (fixture_link_dir / artifact.name).write_bytes(artifact.read_bytes())

        projects = ["scalars", "libc", "threads", "unicode"]
        for name in projects:
            project = root / f"tests/native/c_ffi/projects/{name}/lang.toml"
            for opt in (0, 2):
                label = f"{name}:O{opt}"
                program = build(project, opt, f"{name}-o{opt}")
                if not program:
                    continue
                elf_text = elf(program)
                require("INTERP" in elf_text, "hosted-interp:" + label)
                require("libstdc++" not in elf_text, "hosted-no-libstdcxx:" + label)
                if name == "libc":
                    require("libcrypto" in elf_text, "hosted-crypto:" + label)
                execute(label, program)

        bounds = build(root / "tests/native/c_ffi/projects/bounds/lang.toml",
                       0, "bounds-panic")
        if bounds:
            r = run([bounds], timeout=10)
            record("run:bounds-panic", result=r)
            passed = (r["status"] == "exited" and r["exit"] != 0 and
                      "LANG_NATIVE_PANIC:v1:vec-index:" in r["stderr"])
            require(passed, "bounds-panic", r["stderr"][-300:])

        # The default freestanding core must keep the old no-INTERP/no-NEEDED
        # executable shape.
        core = build(root / "tests/fixtures/core/lang.toml", 0, "core-freestanding")
        if core:
            text = elf(core)
            require("INTERP" not in text, "core:no-interp")
            require("NEEDED" not in text, "core:no-needed")
            execute("core-freestanding", core)

        negative = {
            "no-hosted": "E_CAPABILITY_UNAVAILABLE",
            "unsafe-required": "E_TYPE_MISMATCH",
            "readonly-write": "E_TYPE_MISMATCH",
            "buffer-from-vec": "E_TYPE_MISMATCH",
            "bad-binding": "E_BINDING_INVALID",
            "unknown-key": "E_BINDING_INVALID",
            "varargs": "E_BINDING_INVALID",
            "buffer-field-size": "E_BINDING_INVALID",
        }
        for name, code in negative.items():
            project = root / f"tests/native/c_ffi/negative/{name}/lang.toml"
            r = run([a.cli, "check", "--project", project, "--backend",
                     "native", "--format", "json"], timeout=60)
            document = None
            try:
                document = json.loads(r["stdout"].strip().splitlines()[-1])
            except (ValueError, IndexError):
                pass
            codes = [d.get("code") for d in (document or {}).get("diagnostics", [])]
            passed = (document is not None and document.get("status") == "error"
                      and code in codes)
            record("negative:" + name, passed=passed, expected=code, result=r)
            require(passed, "negative:" + name, json.dumps(codes))

        # A failed adapter/link must not leave a program that could be run as
        # if the build succeeded.
        missing = out / "link-failure"
        shutil.rmtree(missing, ignore_errors=True)
        r = run([a.cli, "build", "--project",
                 root / "tests/native/c_ffi/negative/link-failure/lang.toml",
                 "--backend", "native", "--opt", "0", "--out-dir", missing,
                 "--format", "json"], timeout=120)
        document = None
        try:
            document = json.loads(r["stdout"].strip().splitlines()[-1])
        except (ValueError, IndexError):
            pass
        codes = [d.get("code") for d in (document or {}).get("diagnostics", [])]
        passed = (not ok(r) and document is not None and
                  document.get("status") == "error" and
                  "E_LINK_FAILED" in codes and not (missing / "program").exists())
        record("negative:link-failure", passed=passed, result=r)
        require(passed, "negative:link-failure", json.dumps(codes))

        forged = a.forged
        if forged and forged.is_file():
            r = run([forged, root / "tests/native/c_ffi/forged-project/lang.toml"],
                    timeout=60)
            record("forged:HIR+IR", result=r)
            require(ok(r), "forged:HIR+IR", r["stderr"][-300:])
        else:
            record("forged:HIR+IR", status="not_run",
                   reason="forged target not supplied")

        nm = a.clang.with_name("llvm-nm")
        if not nm.is_file():
            nm = Path("/usr/bin/nm")
        hosted_symbols = run([nm, "-g", "--defined-only", a.hosted_runtime], timeout=60)
        native_symbols = run([nm, "-g", "--defined-only", a.native_runtime], timeout=60)
        record("archive:hosted", result=hosted_symbols)
        record("archive:native", result=native_symbols)
        require(ok(hosted_symbols) and ok(native_symbols), "archive:symbol-dump")
        if ok(hosted_symbols) and ok(native_symbols):
            h = hosted_symbols["stdout"]
            n = native_symbols["stdout"]
            require("lang_hosted_startup_v1" in h, "hosted:startup-symbol")
            require(" _start" not in h, "hosted:no-native-start")
            require("__cxa_" not in h and "_Unwind" not in h and
                    "_ZSt9terminatev" not in h, "hosted:no-cxx-runtime")
            require("lang_thread_spawn_v1" in n, "native:thread-symbol")

        report["status"] = "failed" if report["failed"] else "passed"
    except Exception as e:  # noqa: BLE001 - evidence runner
        report["failed"].append("runner exception: " + repr(e))
        report["status"] = "failed"
    finally:
        report["artifact_hashes"] = {
            str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(out.rglob("*")) if path.is_file()
        }
        report_path = out / "report.json"
        report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
        print(json.dumps({"status": report.get("status", "failed"),
                          "failed": report["failed"],
                          "report": str(report_path)}, ensure_ascii=False))
    return 0 if report.get("status") == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
