#!/usr/bin/env python3
"""Run dyn Trait object acceptance cases on both available backends.

Positive projects are checked with both frontend backends, then built and run.
Native outputs are verified as self-contained x86_64 ELF executables at O0 and
O2. Reject projects must produce their declared diagnostic and leave no output.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.dont_write_bytecode = True


def run(command, cwd=None, timeout=300):
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                          timeout=timeout)


def diagnostics(result):
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return set()
    return {item.get("code") for item in payload.get("diagnostics", [])}


def elf_check(path):
    data = path.read_bytes()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise AssertionError("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise AssertionError("not a little-endian ELF64 file")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_type != 2:
        raise AssertionError("ELF type is not ET_EXEC")
    if e_machine != 62:
        raise AssertionError("ELF machine is not x86_64")
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize = struct.unpack_from("<H", data, 54)[0]
    phnum = struct.unpack_from("<H", data, 56)[0]
    if phentsize < 56:
        raise AssertionError("invalid ELF program header size")
    for index in range(phnum):
        offset = phoff + index * phentsize
        if offset + 4 > len(data):
            raise AssertionError("truncated ELF program header")
        p_type = struct.unpack_from("<I", data, offset)[0]
        if p_type == 3:
            raise AssertionError("ELF contains PT_INTERP; not freestanding")
    nm = shutil.which("nm")
    if nm:
        result = run([nm, "-u", str(path)])
        if result.returncode == 0:
            for line in result.stdout.splitlines():
                fields = line.split()
                if fields and fields[0] == "U":
                    raise AssertionError(
                        "native ELF has strong undefined symbol: " + line)
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--python")
    parser.add_argument("--native", action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    cli = str(Path(args.cli).resolve())
    out = Path(args.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    document = json.loads((root / "tests/trait_objects/cases.json").read_text())
    rows = []
    failed = False
    for case in document["cases"]:
        row = {"id": case["id"], "expected": case["expected"]}
        manifest = root / case["manifest"]
        work = out / case["id"]
        work.mkdir()
        codes_by_backend = {}
        for backend in ("python", "native"):
            checked = run([cli, "check", "--project", str(manifest),
                           "--backend", backend, "--format", "json"])
            codes_by_backend[backend] = sorted(diagnostics(checked))
            if case["expected"] == "accept":
                if checked.returncode != 0:
                    row["status"] = "failed"
                    row["detail"] = f"{backend} check failed"
                    row["diagnostic_codes"] = codes_by_backend[backend]
                    failed = True
                    break
            else:
                expected = set(case["diagnostic_codes"])
                if checked.returncode != 1 or not expected.issubset(
                        set(codes_by_backend[backend])):
                    row["status"] = "failed"
                    row["detail"] = f"{backend} rejection code mismatch"
                    row["diagnostic_codes"] = codes_by_backend[backend]
                    failed = True
                    break
        if row.get("status") == "failed":
            rows.append(row)
            continue
        row["diagnostic_codes"] = codes_by_backend

        if case["expected"] == "reject":
            # A rejected project must leave no output on either built backend.
            rejected = [("python", [])]
            if args.native:
                rejected.append(("native", ["--opt", "0"]))
            for backend, extra in rejected:
                rejected_dir = work / ("rejected-" + backend)
                built = run([cli, "build", "--project", str(manifest),
                             "--backend", backend, "--out-dir",
                             str(rejected_dir), "--format", "json"] + extra)
                produced = (rejected_dir / "program.py").exists() or (
                    rejected_dir / "program").exists()
                if built.returncode == 0 or produced:
                    row["status"] = "failed"
                    row["detail"] = (f"rejected project produced {backend} output")
                    failed = True
                    break
            if row.get("status") != "failed":
                row["status"] = "passed"
            rows.append(row)
            continue

        expectations = []
        if args.native:
            for opt, name in ((0, "native-o0"), (2, "native-o2")):
                native_dir = work / name
                built = run([cli, "build", "--project", str(manifest),
                             "--backend", "native", "--opt", str(opt),
                             "--out-dir", str(native_dir),
                             "--format", "json"])
                if built.returncode != 0:
                    row["status"] = "failed"
                    row["detail"] = f"native O{opt} build failed"
                    row["diagnostic_codes"] = sorted(diagnostics(built))
                    failed = True
                    break
                program = native_dir / "program"
                if not program.exists() or not (native_dir / "program.o").exists():
                    row["status"] = "failed"
                    row["detail"] = f"native O{opt} outputs missing"
                    failed = True
                    break
                try:
                    elf_check(program)
                except AssertionError as error:
                    row["status"] = "failed"
                    row["detail"] = f"native O{opt} ELF: {error}"
                    failed = True
                    break
                executed = run([str(program)], cwd=str(native_dir))
                expectations.append((f"native-o{opt}", executed.returncode))
                if executed.returncode != case.get("main_return", 0):
                    row["status"] = "failed"
                    row["detail"] = f"native O{opt} returned {executed.returncode}"
                    failed = True
                    break
        if row.get("status") == "failed":
            rows.append(row)
            continue

        if args.python:
            python_dir = work / "python-ok"
            built = run([cli, "build", "--project", str(manifest),
                         "--backend", "python", "--out-dir", str(python_dir),
                         "--format", "json"])
            if built.returncode != 0:
                row["status"] = "failed"
                row["detail"] = "python build failed"
                row["diagnostic_codes"] = sorted(diagnostics(built))
                failed = True
            else:
                observer = case.get("python_observer")
                if observer:
                    executed = run([args.python, "-s",
                                    str(root / "tests/trait_objects" / observer),
                                    str(python_dir)], cwd=str(python_dir))
                else:
                    executed = run([args.python, "-s",
                                    str(python_dir / "program.py")],
                                   cwd=str(python_dir))
                expectations.append(("python", executed.returncode))
                if executed.returncode != case.get("main_return", 0):
                    row["status"] = "failed"
                    row["detail"] = f"python returned {executed.returncode}"
                    failed = True
                else:
                    row["status"] = "passed"
        else:
            row["status"] = "passed"
        row["runtime_returns"] = dict(expectations)
        rows.append(row)

    document = {
        "schema": "compiler-dev/trait-objects-results/1",
        "status": "failed" if failed else "passed",
        "native": bool(args.native),
        "python": bool(args.python),
        "cases": rows,
    }
    (out / "results.json").write_text(json.dumps(document, indent=2) + "\n")
    print(json.dumps(document, ensure_ascii=False))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
