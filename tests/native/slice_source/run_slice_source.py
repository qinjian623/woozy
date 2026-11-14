#!/usr/bin/env python3
"""Build, verify, execute and observe the real Wz Slice[T] source interface.

Default (legacy CMake) mode is the frozen C++ reference route: the public CLI
builds and runs the fixtures, ``lang_native_helper`` emits the four observation
files, and the runner links the original allocation/Vec observers against the
public ``program.o``.

Pass ``--wz-native-helper`` (alias ``--driver``) for candidate mode.  The Wz
observation driver performs the real Source -> load/resolve/check/lower/
snapshot/dump/emit pipeline; this runner then performs the fixed LLVM
verify/opt/verify/llc steps, independent Clang/LLD link, ELF checks, observer
runs, and execution assertions.  The candidate driver never invokes the C++
frontend/helper.  The Wz entry is built separately from the frozen ed1 Source
by ``build_nativeobs_ed1.py``; its sealed build report is validated here.

Every actual process is started through ``selfhost/corpus._run_process_impl``
with finite timeouts, exact exec allowlists in candidate mode, raw bytes/SHA,
nativeint exit codes, strict bool cleanup flags, and bounded process-group
cleanup.  A command record is written to disk before any assertion is made.
The output directory must be new; existing records are never overwritten.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone

TARGET_FLAGS = (
    "--target=x86_64-unknown-linux-gnu", "-march=x86-64", "-mtune=generic",
    "-fno-fast-math", "-ffp-contract=off", "-fno-lto",
)
POSITIVE = ("core", "owned", "cleanup", "throws-return", "after-drop")
PANIC = {
    "structural-push": "active-iteration",
    "structural-pop": "active-iteration",
    "structural-clear": "active-iteration",
    "structural-resize": "active-iteration",
    "structural-reserve": "active-iteration",
    "empty-still-pins": "active-iteration",
    "negative-index": "vec-index",
    "huge-index": "vec-index",
    "bounds-order": "vec-slice",
    "bounds-high": "vec-slice",
    "sub-bounds": "vec-slice",
}
# The former "python-slice" entry is now positive Python coverage in
# tests/python/slice_source/fixtures/python-slice.  The manifest is deliberately
# not selected by the Native runner, so this file does not execute it.
NEGATIVE = {
    "type-mismatch": ("native", "E_TYPE_MISMATCH"),
    "wrong-arity": ("native", "E_TYPE_MISMATCH"),
    "wrong-len": ("native", "E_TYPE_MISMATCH"),
}

ED1_SOURCE_MAP_SHA = "ed1c65fb9889606a713f03cc4e02d7bd6d818c58e4937610554cdd3f3e866a09"
STDLIB37_MAP_SHA = "473aa54b5ce5c1bbae4985d0d187528aa056a72ddc1737c3f05d3abb9623bd47"
EXPECTED_COMPONENT_FILES = 117

BUILD_TIMEOUT = 180.0
PROGRAM_TIMEOUT = 60.0
READELF_TIMEOUT = 30.0
STREAM_NAME = re.compile(r"[^A-Za-z0-9_.-]+")
LLVM_TOOL_NAMES = ("opt", "llc", "clang", "clang++", "ld.lld",
                   "llvm-readelf", "llvm-nm")


def utc():
    return datetime.now(timezone.utc).isoformat()


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    return sha256_bytes(Path(path).read_bytes())


def json_hash(value):
    return sha256_bytes(json.dumps(value, sort_keys=True,
                                   separators=(",", ":")).encode())


def tree_map(root, suffix=None):
    result = {}
    for path in sorted(Path(root).rglob("*")):
        if not path.is_file():
            continue
        if suffix is not None and path.suffix != suffix:
            continue
        result[str(path.relative_to(root))] = sha256_file(path)
    return result


def finite_json(value, where="$"):
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError(f"non-finite float at {where}")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            finite_json(item, f"{where}.{key}")
    elif isinstance(value, (list, tuple)):
        for index, item in enumerate(value):
            finite_json(item, f"{where}[{index}]")


def atomic_write_json(path, value):
    path = Path(path)
    finite_json(value)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=path.parent,
                                     prefix=path.name + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2, ensure_ascii=False,
                      allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def strict_json_loads(text):
    """Parse JSON while rejecting constants and float exponent overflow.

    ``json.loads(..., parse_constant=...)`` alone still turns ``1e1000`` into
    infinity, so every parsed float must also be finite.  Integers and finite
    positive/negative/zero decimals remain accepted at any nesting depth.
    """
    def reject(value):
        raise ValueError(f"non-finite JSON constant: {value}")

    def finite_float(value):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError(f"non-finite JSON number: {value}")
        return number

    return json.loads(text, parse_constant=reject, parse_float=finite_float)


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def load_corpus_support(root):
    path = Path(root) / "selfhost/corpus.py"
    spec = importlib.util.spec_from_file_location("woozy_corpus_support", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def clean_process(record):
    return (record.get("status") == "exited"
            and type(record.get("exit")) is int
            and record.get("timed_out") is False
            and record.get("cleanup_complete") is True
            and record.get("abandoned_pipes") is False
            and (not record.get("trace_required")
                 or (record.get("trace") or {}).get("status") == "ok"))


def process_ok(record):
    return clean_process(record) and record.get("exit") == 0


def rejection_ok(record):
    return clean_process(record) and record.get("exit") == 1


def parse_negative(record):
    need(rejection_ok(record),
         f"negative build was not a normal exit-1 rejection: {record}")
    try:
        payload = strict_json_loads(record["stdout"])
    except (ValueError, TypeError) as error:
        raise RuntimeError(f"negative result is not strict JSON: {error}: "
                           f"{record['stdout']!r}") from error
    need(isinstance(payload, dict), f"negative result is not an object: {payload}")
    need(payload.get("status") == "error", f"negative build accepted: {payload}")
    diagnostics = payload.get("diagnostics")
    need(isinstance(diagnostics, list), f"negative diagnostics is not a list: {payload}")
    codes = []
    for diagnostic in diagnostics:
        need(isinstance(diagnostic, dict), f"bad diagnostic: {diagnostic}")
        need(isinstance(diagnostic.get("code"), str), f"bad diagnostic code: {diagnostic}")
        codes.append(diagnostic["code"])
    return codes


def rejection_has_code(record, code):
    return code in parse_negative(record)


def observer_valid(record):
    events = record.get("events") or {}
    return (record.get("allocations") == record.get("frees")
            and all(events.get(index, 0) > 0 for index in (1, 2, 3, 5, 6, 7)))


def parse_observer(stderr):
    heap, vecs = {}, {}
    events = {index: 0 for index in range(1, 8)}
    allocations = frees = 0
    for line in stderr.splitlines():
        if line.startswith("ALLOC:"):
            pointer = line.split(":", 1)[1]
            need(pointer not in heap, f"duplicate live ALLOC {pointer}")
            heap[pointer] = True
            allocations += 1
        elif line.startswith("FREE:"):
            pointer = line.split(":", 1)[1]
            need(pointer in heap, f"unknown FREE {pointer}")
            del heap[pointer]
            frees += 1
        elif line.startswith("VEC:"):
            parts = line.split(":")
            need(len(parts) == 4, f"bad VEC line {line}")
            event = int(parts[1])
            identity = parts[2]
            need(1 <= event <= 7, f"bad VEC event {line}")
            events[event] += 1
            if event == 1:
                need(identity not in vecs, f"duplicate live VEC {identity}")
                vecs[identity] = True
            else:
                need(identity in vecs, f"VEC event on dead object {line}")
                if event == 5:
                    del vecs[identity]
        elif line:
            raise RuntimeError(f"unexpected observer output: {line}")
    need(not heap, f"observer heap live at end: {heap}")
    need(not vecs, f"observer Vec live at end: {vecs}")
    need(allocations == frees,
         f"ALLOC/FREE mismatch {allocations}/{frees}")
    record = {"allocations": allocations, "frees": frees, "events": events}
    need(observer_valid(record), f"weak observer evidence: {record}")
    return record


def ir_paths(directory, opt):
    directory = Path(directory)
    return {
        "before.ll": directory / f"O{opt}.before.ll",
        "after.ll": directory / f"O{opt}.after.ll",
        "module.nir": directory / f"O{opt}.nir",
        "object": directory / f"O{opt}.o",
    }


def four_ir_ok(paths):
    return all(path.is_file() and path.stat().st_size > 0
               for path in paths.values())


def required_symbols(name):
    required = ["lang_slice_new_v1", "lang_slice_length_v1",
                "lang_slice_copy_v1", "lang_slice_drop_v1",
                "lang_slice_at_v1"]
    if name == "core":
        required += ["lang_slice_sub_v1", "lang_vec_slice_v1"]
    if name == "owned":
        required += ["lang_vec_slice_v1", "lang_vec_set_v1"]
    return required


def prepare_out(out):
    """Prepare an explicit fresh output directory; never overwrite records."""
    out = Path(out)
    if out.exists():
        if not out.is_dir():
            raise RuntimeError(f"output path is not a directory: {out}")
        if any(out.iterdir()):
            raise RuntimeError(
                "output directory is not empty; refusing to overwrite records: "
                + str(out))
    out.mkdir(parents=True, exist_ok=True)
    (out / "raw").mkdir(parents=True, exist_ok=True)
    (out / "traces").mkdir(parents=True, exist_ok=True)
    return out


def prepare_legacy_out(out):
    """Repeatable CMake compatibility: keep old files, add one unique run.

    The legacy test passes a fixed ``--out`` and cannot be changed to add a
    fresh option.  If that directory already holds records, create a new
    uniquely named child directory instead of deleting or overwriting them.
    Explicit fresh reference/candidate runs still use ``prepare_out``.
    """
    out = Path(out)
    if out.exists() and out.is_dir() and any(out.iterdir()):
        out = Path(tempfile.mkdtemp(prefix="run-", dir=str(out)))
    return prepare_out(out)


def aliases(*paths):
    result = set()
    for path in paths:
        if path is None:
            continue
        value = Path(path)
        result.add(str(value))
        try:
            result.add(str(value.resolve()))
        except OSError:
            pass
    return sorted(result)


def file_record(path):
    if path is None:
        return None
    path = Path(path)
    if not path.is_file():
        return {"path": str(path), "missing": True}
    return {"path": str(path), "size": path.stat().st_size,
            "sha256": sha256_file(path)}


def tree_record(root, suffix=None, pattern=None):
    if root is None:
        return None
    root = Path(root)
    if not root.is_dir():
        return {"path": str(root), "missing": True}
    if pattern is None:
        return {"path": str(root), "tree": tree_map(root, suffix)}
    files = {}
    for path in sorted(root.glob(pattern)):
        if path.is_file():
            files[path.name] = sha256_file(path)
    return {"path": str(root), "files": files}


def find_libxml_dir(args, env):
    if args.libxml_dir is not None:
        return args.libxml_dir.resolve()
    for entry in env.get("LD_LIBRARY_PATH", "").split(":"):
        if entry and "libxml" in entry and Path(entry).is_dir():
            return Path(entry).resolve()
    if args.toolchains is not None:
        candidate = args.toolchains / "libxml2-legacy-2.13.9/usr/lib"
        if candidate.is_dir():
            return candidate.resolve()
    return None


def validate_candidate_inputs(args):
    need(args.build_report is not None,
         "candidate mode requires --build-report from build_nativeobs_ed1.py")
    need(args.source_root is not None,
         "candidate mode requires --source-root for the ed1 Source identity")
    need(args.stdlib_source is not None,
         "candidate mode requires --stdlib-source for the fixed 37-file stdlib")
    need(args.candidate_ready is not None,
         "candidate mode requires --candidate-ready for the stage1 readiness record")
    report = json.loads(args.build_report.read_text(encoding="utf-8"))
    finite_json(report, "build-report")
    need(report.get("status") == "passed", "Wz entry build report is not passed")
    need(process_ok(report.get("process") or {}),
         "Wz entry build process did not exit 0 with complete cleanup")
    need(report.get("source116_map_sha256") == ED1_SOURCE_MAP_SHA,
         "Wz entry build report does not pin the ed1 Source map")
    need(report.get("stdlib_map_sha256") == STDLIB37_MAP_SHA,
         "Wz entry build report does not pin the fixed 37-file stdlib")
    actual_source = tree_map(args.source_root, ".wz")
    need(len(actual_source) == 116 and json_hash(actual_source) == ED1_SOURCE_MAP_SHA,
         "current source root is not the frozen 116-file ed1 Source")
    actual_stdlib = tree_map(args.stdlib_source, ".wz")
    need(len(actual_stdlib) == 37 and json_hash(actual_stdlib) == STDLIB37_MAP_SHA,
         "current stdlib is not the fixed 37-file source")
    if args.component_source_map is not None:
        component_map = json.loads(
            args.component_source_map.read_text(encoding="utf-8"))
        finite_json(component_map, "component-source-map")
        need(len(component_map) == EXPECTED_COMPONENT_FILES,
             "component source map does not contain exactly 117 files")
        need(json_hash(component_map) == report.get("component_source_map_sha256"),
             "component source map does not match the sealed build report")
    driver = args.wz_native_helper.resolve()
    need(driver.is_file(), f"Wz observation driver missing: {driver}")
    need(sha256_file(driver) == report.get("driver_sha256"),
         "Wz observation driver SHA does not match the sealed build report")
    need(args.cli.resolve().is_file(), f"candidate CLI missing: {args.cli}")
    if args.expect_cli_sha256:
        need(sha256_file(args.cli) == args.expect_cli_sha256,
             "candidate CLI SHA does not match --expect-cli-sha256")
    ready = json.loads(args.candidate_ready.read_text(encoding="utf-8"))
    finite_json(ready, "candidate-ready")
    need(ready.get("status") == "ready",
         "candidate readiness record does not report ready")
    need(ready.get("source_map_sha256") == ED1_SOURCE_MAP_SHA,
         "candidate readiness record does not pin the ed1 Source map")
    need(ready.get("stdlib_map_sha256") == STDLIB37_MAP_SHA,
         "candidate readiness record does not pin the fixed 37-file stdlib")
    stage1 = ready.get("stage1") or {}
    need(stage1.get("sha256") == sha256_file(args.cli),
         "candidate CLI SHA does not match the stage1 readiness record")
    return report, ready


def main(argv=None):
    sys.dont_write_bytecode = True
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--helper", type=Path,
                        help="C++ helper for the legacy reference route")
    parser.add_argument("--wz-native-helper", "--driver", dest="wz_native_helper",
                        type=Path,
                        help="pure-Wz observation driver; selects candidate mode")
    parser.add_argument("--reference-helper", type=Path,
                        help="C++ helper kept only as a recorded reference in candidate mode")
    parser.add_argument("--role", choices=("reference", "candidate"),
                        help="explicit role; default is inferred from --wz-native-helper")
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--lld", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--toolchains", type=Path)
    parser.add_argument("--strace", type=Path)
    parser.add_argument("--readelf", type=Path)
    parser.add_argument("--libxml-dir", type=Path)
    parser.add_argument("--seed", type=Path)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--stdlib-source", type=Path)
    parser.add_argument("--build-report", type=Path)
    parser.add_argument("--component-source-map", type=Path)
    parser.add_argument("--candidate-ready", type=Path)
    parser.add_argument("--expect-cli-sha256")
    parser.add_argument("--ld-library-path", default="")
    parser.add_argument("--fresh", action="store_true",
                        help="explicit fresh run: refuse a non-empty --out")
    parser.add_argument("--timeout", type=float, default=BUILD_TIMEOUT)
    parser.add_argument("--program-timeout", type=float, default=PROGRAM_TIMEOUT)
    parser.add_argument("--readelf-timeout", type=float, default=READELF_TIMEOUT)
    args = parser.parse_args(argv)
    for name in ("timeout", "program_timeout", "readelf_timeout"):
        value = getattr(args, name)
        if not math.isfinite(value) or value <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive and finite")

    root = args.root.resolve()
    requested_out = args.out.resolve()
    candidate = args.wz_native_helper is not None
    if args.role == "candidate" and not candidate:
        parser.error("--role candidate requires --wz-native-helper")
    if args.role == "reference" and candidate:
        parser.error("--role reference does not accept --wz-native-helper")
    if candidate:
        if args.toolchains is None:
            parser.error("candidate mode requires --toolchains")
        if args.helper is None:
            args.helper = None
        if args.source_root is None or args.stdlib_source is None:
            parser.error("candidate mode requires --source-root and --stdlib-source")
        if args.build_report is None or args.candidate_ready is None:
            parser.error("candidate mode requires --build-report and --candidate-ready")
        validate_candidate_inputs(args)
    else:
        if args.helper is None:
            parser.error("reference mode requires --helper")

    # A bare CMake invocation has neither --fresh nor --role.  Treat only that
    # legacy compatibility route as repeatable; explicit formal runs stay fresh.
    fresh_mode = args.fresh or args.role is not None or candidate
    if fresh_mode:
        out = prepare_out(requested_out)
    else:
        out = prepare_legacy_out(requested_out)
    corpus = load_corpus_support(root)
    fixtures = root / "tests/native/slice_source/fixtures"
    cli = args.cli.resolve()
    helper = args.helper.resolve() if args.helper is not None else None
    driver = (args.wz_native_helper.resolve()
              if args.wz_native_helper is not None else None)
    reference_helper = (args.reference_helper.resolve()
                        if args.reference_helper is not None else None)
    runtime = args.runtime.resolve()
    # Keep the caller's leaf names: resolving ``ld.lld`` to the generic ``lld``
    # driver breaks its argv[0] flavour selection.  The trace allowlist records
    # both the given path and its resolved target.
    clang = args.clang.absolute()
    clangxx = (clang.parent / "clang++")
    need(clangxx.is_file(), f"clang++ missing next to {clang}")
    lld = args.lld.absolute()
    toolchains = args.toolchains.resolve() if args.toolchains is not None else None
    llvm = toolchains / "llvm-22.1.8/bin" if toolchains is not None else None
    opt = llvm / "opt" if llvm is not None else None
    llc = llvm / "llc" if llvm is not None else None
    llvm_readelf = llvm / "llvm-readelf" if llvm is not None else None
    if candidate:
        for tool in (opt, llc, llvm_readelf):
            need(tool.is_file(), f"fixed LLVM tool missing: {tool}")
        need(sha256_file(clang) == sha256_file(llvm / "clang"),
             "candidate --clang is not the fixed LLVM clang")
        need(sha256_file(lld) == sha256_file(llvm / "ld.lld"),
             "candidate --lld is not the fixed LLVM ld.lld")
        readelf = args.readelf.resolve() if args.readelf is not None else llvm_readelf
        need(readelf.is_file(), f"llvm-readelf missing: {readelf}")
        strace = (args.strace.resolve() if args.strace is not None
                  else toolchains / "strace-7.2/usr/bin/strace")
        need(strace.is_file(), f"strace 7.2 missing: {strace}")
    else:
        readelf = (args.readelf.resolve() if args.readelf is not None
                   else Path(shutil.which("readelf") or ""))
        need(readelf.is_file(), f"readelf missing: {readelf}")
        strace = args.strace.resolve() if args.strace is not None else None

    env = os.environ.copy()
    if candidate:
        libxml = find_libxml_dir(args, env)
        if libxml is not None:
            env["LD_LIBRARY_PATH"] = str(libxml) + (
                ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    if args.ld_library_path:
        env["LD_LIBRARY_PATH"] = (args.ld_library_path + ":" +
                                  env.get("LD_LIBRARY_PATH", ""))

    source_root = args.source_root.resolve() if args.source_root is not None else None
    stdlib_source = (args.stdlib_source.resolve()
                     if args.stdlib_source is not None else None)
    libxml = find_libxml_dir(args, env)

    def resources():
        return {
            "cli": file_record(cli),
            "helper": file_record(helper),
            "wz_native_helper": file_record(driver),
            "reference_helper": file_record(reference_helper),
            "seed": file_record(args.seed.resolve() if args.seed is not None else None),
            "runtime": file_record(runtime),
            "clang": file_record(clang),
            "clangxx": file_record(clangxx),
            "lld": file_record(lld),
            "opt": file_record(opt),
            "llc": file_record(llc),
            "readelf": file_record(readelf),
            "llvm_readelf": file_record(llvm_readelf),
            "strace": file_record(strace),
            "python": file_record(Path(sys.executable).resolve()),
            "python_version": sys.version,
            "libxml": tree_record(libxml, pattern="libxml2*.so*"),
            "ld_library_path": env.get("LD_LIBRARY_PATH", ""),
            "corpus": file_record(root / "selfhost/corpus.py"),
            "runner": file_record(Path(__file__).resolve()),
            "fixtures": tree_record(fixtures),
            "observers": {
                "vec": file_record(root / "tests/native/vec_observer.cpp"),
                "alloc": file_record(root / "tests/native/alloc_observer.cpp"),
            },
            "runtime_slice": {
                "slice.cpp": file_record(root / "runtime/native/slice.cpp"),
                "slice.hpp": file_record(root / "runtime/native/slice.hpp"),
            },
            "source": tree_record(source_root, ".wz"),
            "stdlib": tree_record(stdlib_source, ".wz"),
            "build_report": file_record(args.build_report),
            "component_source_map": file_record(args.component_source_map),
            "candidate_ready": file_record(args.candidate_ready),
        }

    commands = []
    command_categories = []
    command_index = 0
    failures = []
    planned_items = []
    completed = []
    failed = []
    items = {}
    summary = {
        "task": "SH15-native-slice-public-wz-records",
        "mode": "candidate" if candidate else "reference",
        "role": args.role or ("candidate" if candidate else "reference"),
        "status": "running",
        "started_at_utc": utc(),
        "root": str(root),
        "requested_out": str(requested_out),
        "out": str(out),
        "out_mode": "fresh" if fresh_mode else "legacy-repeat",
        "cli": str(cli),
        "cli_sha256": sha256_file(cli),
        "helper": str(helper) if helper else None,
        "helper_sha256": sha256_file(helper) if helper else None,
        "wz_native_helper": str(driver) if driver else None,
        "wz_native_helper_sha256": sha256_file(driver) if driver else None,
        "reference_helper": str(reference_helper) if reference_helper else None,
        "readelf": str(readelf),
        "toolchains": str(toolchains) if toolchains else None,
        "source_map_sha256": (json_hash(tree_map(source_root, ".wz"))
                              if source_root is not None else None),
        "stdlib_map_sha256": (json_hash(tree_map(stdlib_source, ".wz"))
                              if stdlib_source is not None else None),
        "positive": {name: {"O0": {}, "O2": {}} for name in POSITIVE},
        "panics": {name: {"O0": {}, "O2": {}} for name in PANIC},
        "negative": {},
        "items": items,
        "completed": completed,
        "failed": failed,
        "not_run": [],
        "failures": failures,
        "commands_json": str(out / "commands.json"),
        "raw_dir": str(out / "raw"),
        "traces_dir": str(out / "traces"),
    }
    if candidate:
        summary["sealed_build_report"] = str(args.build_report.resolve())
        summary["sealed_build_report_sha256"] = sha256_file(args.build_report)
        summary["candidate_ready"] = str(args.candidate_ready.resolve())
        summary["candidate_ready_sha256"] = sha256_file(args.candidate_ready)
        summary["component_source_map"] = (
            str(args.component_source_map.resolve())
            if args.component_source_map is not None else None)
    expected_counts = {
        "public_build": 35,
        "private_source_ir": 10,
        "program": 32,
        "readelf": 10,
        "observer_link": 10,
        "observer_run": 10,
        "extra_llvm": 40 if candidate else 0,
    }
    summary["command_counts"] = {
        "expected": expected_counts,
        "expected_total": sum(expected_counts.values()),
        "actual": {key: 0 for key in expected_counts},
        "actual_total": 0,
    }

    for name in POSITIVE:
        for opt_value in (0, 2):
            planned_items.append(f"positive/{name}/O{opt_value}")
    for name in PANIC:
        for opt_value in (0, 2):
            planned_items.append(f"panic/{name}/O{opt_value}")
    for name in NEGATIVE:
        planned_items.append(f"negative/{name}")

    def actual_counts():
        counts = {key: 0 for key in expected_counts}
        for category in command_categories:
            counts[category] = counts.get(category, 0) + 1
        return counts

    def update_summary():
        counts = actual_counts()
        summary["command_counts"]["actual"] = counts
        summary["command_counts"]["actual_total"] = sum(counts.values())
        summary["not_run"] = [item for item in planned_items
                              if item not in completed and item not in failed]
        if summary["status"] == "running":
            summary["status"] = "running"
        atomic_write_json(out / "summary.json", summary)

    def record_ref(record):
        return {
            "index": record["index"],
            "status": record["status"],
            "exit": record["exit"],
            "timed_out": record["timed_out"],
            "cleanup_complete": record["cleanup_complete"],
            "abandoned_pipes": record["abandoned_pipes"],
            "stdout_sha256": record["stdout_sha256"],
            "stderr_sha256": record["stderr_sha256"],
            "timeout_s": record.get("timeout_s"),
            "trace_status": (record.get("trace") or {}).get("status"),
        }

    def command(argv, timeout, label, category, trace=None):
        nonlocal command_index
        command_index += 1
        values = [str(x) for x in argv]
        stem = STREAM_NAME.sub("_", label)[:80]
        prefix = f"{command_index:04d}_{stem}"
        stdout_path = out / "raw" / (prefix + ".stdout")
        stderr_path = out / "raw" / (prefix + ".stderr")
        stdout_path.write_bytes(b"")
        stderr_path.write_bytes(b"")
        trace_path = None
        traced_args = None
        if trace is not None:
            trace_path = out / "traces" / (prefix + ".exec.log")
            traced_args = [str(strace), "--seccomp-bpf", "-f", "-qq",
                           "-s", "16384", "-e", "trace=execve,execveat",
                           "-o", str(trace_path), *values]
            run_args = traced_args
        else:
            run_args = values
        started = time.monotonic()
        try:
            raw = corpus._run_process_impl(run_args, root, env, timeout)
            exit_code = raw["exit"]
            timed_out = raw["timed_out"]
            cleanup_complete = raw["cleanup_complete"]
            abandoned_pipes = raw["abandoned_pipes"]
            stdout = raw["stdout"]
            stderr = raw["stderr"]
            launch_error = None
        except OSError as error:
            exit_code = None
            timed_out = False
            cleanup_complete = True
            abandoned_pipes = False
            stdout = b""
            stderr = b""
            launch_error = str(error)
        elapsed = round(time.monotonic() - started, 6)
        stdout_path.write_bytes(stdout)
        stderr_path.write_bytes(stderr)
        status = ("launch_failed" if launch_error is not None else
                  "timeout" if timed_out else
                  "signalled" if (isinstance(exit_code, int) and exit_code < 0) else
                  "exited")
        record = {
            "index": command_index,
            "label": label,
            "category": category,
            "argv": values,
            "status": status,
            "exit": exit_code,
            "timed_out": timed_out,
            "cleanup_complete": cleanup_complete,
            "abandoned_pipes": abandoned_pipes,
            "stdout": stdout.decode("utf-8", "replace"),
            "stderr": stderr.decode("utf-8", "replace"),
            "stdout_bytes": len(stdout),
            "stderr_bytes": len(stderr),
            "stdout_sha256": sha256_bytes(stdout),
            "stderr_sha256": sha256_bytes(stderr),
            "stdout_path": str(stdout_path.relative_to(out)),
            "stderr_path": str(stderr_path.relative_to(out)),
            "cwd": str(root),
            "elapsed_s": elapsed,
            "timeout_s": timeout,
            "trace_required": trace is not None,
        }
        if launch_error is not None:
            record["launch_error"] = launch_error
        # Persist the known process result before trace auditing or summary
        # updates can fail.  atomic_write_json closes a same-directory pending
        # file and replaces the old list, so a write error cannot truncate it.
        commands.append(record)
        command_categories.append(category)
        atomic_write_json(out / "commands.json", commands)
        update_summary()
        if traced_args is not None:
            record["traced_argv"] = traced_args
            record["trace_path"] = str(trace_path.relative_to(out))
            record["trace_expected"] = sorted(set(trace))
            try:
                record["trace"] = corpus.audit_trace(trace_path,
                                                     sorted(set(trace)))
            except (corpus.TraceError, OSError) as error:
                record["trace"] = {"status": "violation", "error": str(error),
                                   "allowed": sorted(set(trace))}
                failures.append(f"{label}: exec trace violation: {error}")
        else:
            record["trace"] = {"status": "not_required"}
        if launch_error is not None:
            failures.append(f"{label}: launch failed: {launch_error}")
        if timed_out:
            failures.append(f"{label}: timed out after {timeout:g}s")
        if not cleanup_complete:
            failures.append(f"{label}: cleanup incomplete")
        if abandoned_pipes:
            failures.append(f"{label}: abandoned pipes")
        atomic_write_json(out / "commands.json", commands)
        update_summary()
        return record

    def public_allowlist():
        toolchain_dir = cli.parent / "toolchain"
        return aliases(cli, opt, llc, clang, clangxx, lld,
                       toolchain_dir / "opt", toolchain_dir / "llc",
                       toolchain_dir / "clang", toolchain_dir / "clang++",
                       toolchain_dir / "ld.lld")

    def ir_allowlist():
        if candidate:
            return aliases(driver)
        return None

    def build_trace():
        return public_allowlist() if candidate else None

    def readelf_trace():
        return aliases(readelf) if candidate else None

    def observer_link_trace():
        return aliases(clangxx, lld) if candidate else None

    def observer_run_trace(observer):
        return aliases(observer) if candidate else None

    def llvm_trace(tool):
        return aliases(tool)

    def item_record(item):
        value = {"status": "running", "steps": {}, "commands": {},
                 "artifacts": {}, "assertions": []}
        items[item] = value
        return value

    def record_step(item, step, record):
        ref = record_ref(record)
        items[item]["steps"][step] = ref["status"]
        items[item]["commands"][step] = ref

    try:
        summary["resource_sha256_before"] = resources()
        update_summary()
        for name in POSITIVE:
            manifest = fixtures / name / "lang.toml"
            ir_texts = {}
            for opt_value in (0, 2):
                item = f"positive/{name}/O{opt_value}"
                current = item_record(item)
                out_item = out / "positive" / name / f"O{opt_value}"
                try:
                    build = command(
                        [cli, "build", "--project", manifest,
                         "--backend", "native", "--opt", str(opt_value),
                         "--out-dir", out_item / "cli", "--format", "json"],
                        args.timeout, f"{name}-O{opt_value}-public-build",
                        "public_build", trace=build_trace())
                    record_step(item, "public_build", build)
                    need(process_ok(build),
                         f"{name} O{opt_value} public build: {build['stderr']}")
                    payload = strict_json_loads(build["stdout"])
                    need(isinstance(payload, dict) and payload.get("status") == "ok",
                         f"{name} O{opt_value} public build payload: {payload}")
                    need(not any(d.get("code") == "E_CAPABILITY_UNAVAILABLE"
                                 for d in payload.get("diagnostics", [])),
                         f"{name} O{opt_value} capability fallback")
                    program = out_item / "cli" / "program"
                    program_o = out_item / "cli" / "program.o"
                    need(program.is_file() and program_o.is_file(),
                         f"{name} O{opt_value} public build did not publish "
                         f"program/program.o")
                    execute = command([program], args.program_timeout,
                                      f"{name}-O{opt_value}-execute", "program",
                                      trace=observer_run_trace(program))
                    record_step(item, "program", execute)
                    need(process_ok(execute) and not execute["stdout"]
                         and not execute["stderr"],
                         f"{name} O{opt_value} execution: {execute['stderr']}")
                    dyn = command(
                        [readelf, "-dW" if candidate else "-d", program],
                        args.readelf_timeout, f"{name}-O{opt_value}-readelf",
                        "readelf", trace=readelf_trace())
                    record_step(item, "readelf", dyn)
                    need(process_ok(dyn), f"{name} O{opt_value} readelf failed")
                    marker = "NEEDED" if candidate else "(NEEDED)"
                    need(marker not in dyn["stdout"],
                         f"{name} O{opt_value} has a dynamic interpreter dependency")
                    ir_dir = out_item / "ir"
                    ir_dir.mkdir(parents=True, exist_ok=True)
                    paths = ir_paths(ir_dir, opt_value)
                    if candidate:
                        driver_run = command(
                            [driver, "--project", manifest,
                             "--stdlib", stdlib_source,
                             "--opt", str(opt_value),
                             "--out-llvm-before", paths["before.ll"],
                             "--out-native", paths["module.nir"],
                             "--format", "json"],
                            args.timeout, f"{name}-O{opt_value}-wz-source-ir",
                            "private_source_ir", trace=ir_allowlist())
                        record_step(item, "private_source_ir", driver_run)
                        need(process_ok(driver_run),
                             f"{name} O{opt_value} Wz Source IR failed")
                        metadata = strict_json_loads(driver_run["stdout"])
                        need(isinstance(metadata, dict)
                             and metadata.get("status") == "passed"
                             and metadata.get("cfg_verified") is True
                             and type(metadata.get("has_async")) is bool
                             and metadata.get("word_bits") == 64
                             and isinstance(metadata.get("triple"), str),
                             f"{name} O{opt_value} Wz metadata incomplete: {metadata}")
                        passes = ("verify,default<O2>,verify"
                                  if opt_value == 2 else
                                  ("verify,default<O0>,verify"
                                   if metadata["has_async"] else "verify"))
                        verify_before = command(
                            [opt, "-passes=verify", "-disable-output",
                             paths["before.ll"]],
                            args.timeout,
                            f"{name}-O{opt_value}-verify-before",
                            "extra_llvm", trace=llvm_trace(opt))
                        record_step(item, "verify_before", verify_before)
                        need(process_ok(verify_before),
                             f"{name} O{opt_value} pre-optimization verify failed")
                        optimize = command(
                            [opt, "-S", "-passes=" + passes, "-verify-each",
                             "-mtriple=" + metadata["triple"],
                             "-mcpu=generic", paths["before.ll"],
                             "-o", paths["after.ll"]],
                            args.timeout, f"{name}-O{opt_value}-opt",
                            "extra_llvm", trace=llvm_trace(opt))
                        record_step(item, "opt", optimize)
                        need(process_ok(optimize),
                             f"{name} O{opt_value} fixed opt failed")
                        verify_after = command(
                            [opt, "-passes=verify", "-disable-output",
                             paths["after.ll"]],
                            args.timeout,
                            f"{name}-O{opt_value}-verify-after",
                            "extra_llvm", trace=llvm_trace(opt))
                        record_step(item, "verify_after", verify_after)
                        need(process_ok(verify_after),
                             f"{name} O{opt_value} post-optimization verify failed")
                        lower = command(
                            [llc, "-filetype=obj", "-O=" + str(opt_value),
                             "-mtriple=" + metadata["triple"],
                             "-mcpu=generic", "-relocation-model=pic",
                             "-code-model=small", paths["after.ll"],
                             "-o", paths["object"]],
                            args.timeout, f"{name}-O{opt_value}-llc",
                            "extra_llvm", trace=llvm_trace(llc))
                        record_step(item, "llc", lower)
                        need(process_ok(lower),
                             f"{name} O{opt_value} fixed llc failed")
                    else:
                        helper_run = command(
                            [helper, "--project", manifest,
                             "--opt", str(opt_value),
                             "--out-object", paths["object"],
                             "--out-llvm", paths["after.ll"],
                             "--out-llvm-before", paths["before.ll"],
                             "--out-native", paths["module.nir"],
                             "--format", "json"],
                            args.timeout, f"{name}-O{opt_value}-helper",
                            "private_source_ir", trace=None)
                        record_step(item, "private_source_ir", helper_run)
                        need(process_ok(helper_run),
                             f"{name} O{opt_value} helper failed")
                        metadata = strict_json_loads(helper_run["stdout"])
                        need(isinstance(metadata, dict)
                             and metadata.get("status") == "passed",
                             f"{name} O{opt_value} helper rejected verified IR")
                    need(four_ir_ok(paths),
                         f"{name} O{opt_value} did not produce all four IR files")
                    nir_text = paths["module.nir"].read_text(encoding="utf-8")
                    after_text = paths["after.ll"].read_text(encoding="utf-8")
                    before_text = paths["before.ll"].read_text(encoding="utf-8")
                    need("interp" not in nir_text.lower()
                         and "interp" not in after_text.lower(),
                         f"{name} O{opt_value} contains an interpreter marker")
                    if candidate:
                        need("interp" not in before_text.lower(),
                             f"{name} O{opt_value} before.ll contains an "
                             "interpreter marker")
                    ir_texts[opt_value] = nir_text + after_text
                    current["artifacts"] = {
                        "program": str(program),
                        "program_sha256": sha256_file(program),
                        "program_o": str(program_o),
                        "program_o_sha256": sha256_file(program_o),
                        "ir": {key: {"path": str(path),
                                     "sha256": sha256_file(path),
                                     "size": path.stat().st_size}
                               for key, path in paths.items()},
                    }
                    current["positive"] = {
                        "exit": execute["exit"],
                        "stdout_empty": execute["stdout"] == "",
                        "stderr_empty": execute["stderr"] == "",
                        "readelf_no_needed": marker not in dyn["stdout"],
                        "metadata": metadata,
                    }
                    completed.append(item)
                    current["status"] = "passed"
                    summary["positive"][name][f"O{opt_value}"] = current
                except Exception as error:
                    current["status"] = "failed"
                    current["error"] = str(error)
                    failures.append(f"{item}: {error}")
                    failed.append(item)
                    summary["positive"][name][f"O{opt_value}"] = current
                update_summary()
            if len(ir_texts) == 2:
                combined = ir_texts[0] + ir_texts[2]
                for symbol in required_symbols(name):
                    if symbol not in combined:
                        failures.append(f"{name} IR lacks real low-level call {symbol}")
                summary["positive"][name]["required_symbols"] = {
                    symbol: symbol in combined
                    for symbol in required_symbols(name)}
            else:
                summary["positive"][name]["required_symbols"] = {
                    symbol: "not_run" for symbol in required_symbols(name)}
            for opt_value in (0, 2):
                item = f"positive/{name}/O{opt_value}"
                current = items.get(item)
                if current is None or current.get("status") != "passed":
                    continue
                try:
                    out_item = out / "positive" / name / f"O{opt_value}"
                    observer = out_item / "observer"
                    compile_observer = command(
                        [clangxx, *TARGET_FLAGS, "-std=c++20", "-O2",
                         "-I", root / "runtime/native",
                         out_item / "cli" / "program.o",
                         root / "tests/native/vec_observer.cpp",
                         root / "tests/native/alloc_observer.cpp", runtime,
                         "-Wl,--wrap=lang_alloc_v1",
                         "-Wl,--wrap=lang_free_v1",
                         f"--ld-path={lld}", "-o", observer],
                        args.timeout, f"{name}-O{opt_value}-observer-link",
                        "observer_link", trace=observer_link_trace())
                    record_step(item, "observer_link", compile_observer)
                    need(process_ok(compile_observer),
                         f"{name} O{opt_value} observer build failed")
                    observed = command(
                        [observer], args.program_timeout,
                        f"{name}-O{opt_value}-observer-run", "observer_run",
                        trace=observer_run_trace(observer))
                    record_step(item, "observer_run", observed)
                    need(process_ok(observed),
                         f"{name} O{opt_value} observer run failed")
                    need(not observed["stdout"],
                         f"{name} O{opt_value} observer stdout")
                    record = parse_observer(observed["stderr"])
                    current["observer"] = record
                    summary["positive"][name][f"O{opt_value}"]["observer"] = record
                except Exception as error:
                    current["status"] = "failed"
                    current["error"] = str(error)
                    failures.append(f"{item}: observer: {error}")
                    if item not in failed:
                        failed.append(item)
                    if item in completed:
                        completed.remove(item)
                update_summary()

        for name, cause in PANIC.items():
            manifest = fixtures / name / "lang.toml"
            for opt_value in (0, 2):
                item = f"panic/{name}/O{opt_value}"
                current = item_record(item)
                out_item = out / "panic" / name / f"O{opt_value}"
                try:
                    build = command(
                        [cli, "build", "--project", manifest,
                         "--backend", "native", "--opt", str(opt_value),
                         "--out-dir", out_item, "--format", "json"],
                        args.timeout, f"panic-{name}-O{opt_value}-build",
                        "public_build", trace=build_trace())
                    record_step(item, "public_build", build)
                    need(process_ok(build),
                         f"panic {name} O{opt_value} build: {build['stderr']}")
                    program = out_item / "program"
                    need(program.is_file(), f"panic {name} O{opt_value} no program")
                    execute = command([program], args.program_timeout,
                                      f"panic-{name}-O{opt_value}-execute",
                                      "program", trace=observer_run_trace(program))
                    record_step(item, "program", execute)
                    need(clean_process(execute), f"panic {name} O{opt_value} "
                         f"did not terminate normally: {execute}")
                    need(execute["exit"] == 101,
                         f"panic {name} O{opt_value} exit {execute['exit']}")
                    marker = f"LANG_NATIVE_PANIC:v1:{cause}:"
                    need(marker in execute["stderr"],
                         f"panic {name} O{opt_value} wrong record: "
                         f"{execute['stderr']!r}")
                    need(not execute["stdout"],
                         f"panic {name} O{opt_value} stdout not empty")
                    current["panic"] = {"exit": execute["exit"],
                                        "cause": cause,
                                        "marker_found": True,
                                        "stdout_empty": True,
                                        "program_sha256": sha256_file(program)}
                    current["status"] = "passed"
                    completed.append(item)
                    summary["panics"][name][f"O{opt_value}"] = current
                except Exception as error:
                    current["status"] = "failed"
                    current["error"] = str(error)
                    failures.append(f"{item}: {error}")
                    failed.append(item)
                    summary["panics"][name][f"O{opt_value}"] = current
                update_summary()

        for name, (backend, code) in NEGATIVE.items():
            item = f"negative/{name}"
            current = item_record(item)
            manifest = fixtures / name / "lang.toml"
            out_item = out / "negative" / name
            argv = [cli, "build", "--project", manifest,
                    "--backend", backend]
            if backend == "native":
                argv += ["--opt", "0"]
            argv += ["--out-dir", out_item, "--format", "json"]
            try:
                result = command(argv, args.timeout,
                                 f"negative-{name}-build", "public_build",
                                 trace=build_trace())
                record_step(item, "public_build", result)
                codes = parse_negative(result)
                need(rejection_has_code(result, code),
                     f"negative {name} expected {code}, got {codes}")
                need(not (out_item / "program").exists(),
                     f"negative {name} produced a program")
                current["negative"] = {"backend": backend, "codes": codes,
                                       "program_absent": True,
                                       "program_o_absent": not (
                                           out_item / "program.o").exists()}
                current["status"] = "passed"
                completed.append(item)
                summary["negative"][name] = current
            except Exception as error:
                current["status"] = "failed"
                current["error"] = str(error)
                failures.append(f"{item}: {error}")
                failed.append(item)
            update_summary()
    except Exception as error:
        failures.append(f"run aborted: {error}")
        summary["error"] = str(error)
    finally:
        try:
            summary["resource_sha256_after"] = resources()
            summary["resource_sha256_stable"] = (
                summary.get("resource_sha256_before") ==
                summary["resource_sha256_after"])
            if not summary["resource_sha256_stable"]:
                failures.append("resource SHA changed during the run")
        except Exception as error:
            failures.append(f"resource sha check: {error}")
        summary["finished_at_utc"] = utc()
        summary["not_run"] = [item for item in planned_items
                              if item not in completed and item not in failed]
        summary["completed"] = completed
        summary["failed"] = failed
        summary["failures"] = failures
        if failures:
            summary["status"] = "failed"
        elif summary["not_run"]:
            summary["status"] = "partial"
        else:
            summary["status"] = "passed"
        update_summary()
        atomic_write_json(out / "commands.json", commands)
    print(json.dumps({"status": summary["status"],
                      "failed": failures,
                      "not_run": summary["not_run"],
                      "commands": summary["command_counts"],
                      "summary": str(out / "summary.json")}))
    return 0 if summary["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
