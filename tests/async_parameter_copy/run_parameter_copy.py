#!/usr/bin/env python3
"""Real-source Native checks for ordinary async parameter copies.

Every fixture creates a Task with an ordinary owning parameter in a block that
ends before the Task is started or awaited. The build is checked at O0 and O2,
then emitted as LLVM and linked with the existing allocation observer. A case
passes only when the source output matches and every allocation is released
exactly once.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
OBSERVER = "tests/native/language_gc_source/observe_gc.cpp"


def load_memory_runner():
    path = HERE.parent / "native/async_memory/run_async_memory.py"
    spec = importlib.util.spec_from_file_location("async_memory_runner", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


MEMORY = load_memory_runner()
need = MEMORY.need
invoke = MEMORY.invoke
parse_observer = MEMORY.parse_observer
check_static_elf = MEMORY.check_static_elf
record_hash = MEMORY.record_hash
digest = MEMORY.digest

# Permanent source expectations. The allocation minima avoid a vacuous pass if
# the compiler stops constructing the owning value under test.
CASES = (
    {
        "id": "param-vec-late-start",
        "stdout": "42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "ordinary Vec parameter survives make's block and late start",
    },
    {
        "id": "param-string-await",
        "stdout": "value=42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 0,
        "purpose": "dynamic string parameter survives the caller block and await",
    },
    {
        "id": "param-struct-enum-await",
        "stdout": "7\n9\n",
        "min_heap_allocations": 4,
        "min_vec_allocations": 2,
        "purpose": "owning struct and enum fields copy recursively across await",
    },
    {
        "id": "param-fn-capture-await",
        "stdout": "42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 2,
        "purpose": "capturing function parameter retains its closure environment",
    },
    {
        "id": "param-weak-await",
        "stdout": "42\n42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "Weak parameter retains its control block across suspend",
    },
    {
        "id": "param-dyn-await",
        "stdout": "42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "dyn Trait parameter clones its owned box across suspend",
    },
    {
        "id": "param-throw",
        "stdout": "42\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 2,
        "purpose": "typed-error exit drops the frame-owned parameter once",
    },
    {
        "id": "param-cancel",
        "stdout": "cancel-cleanup\n",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "cancel exit drops both frame-owned parameters once",
    },
    {
        "id": "param-cancel-queued",
        "stdout": "",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "queued cancel drops the parameter before the body ran",
    },
    {
        "id": "param-unstarted",
        "stdout": "",
        "min_heap_allocations": 3,
        "min_vec_allocations": 1,
        "purpose": "never-started Task drops the private parameter copy",
    },
)
CASE_BY_ID = {case["id"]: case for case in CASES}

# Negative cases: an async ordinary owning parameter needs a real copy. A C
# resource that declares no retain_symbol cannot be borrowed across suspend.
REJECTS = (
    {
        "id": "reject-resource-copy",
        "code": "E_CAPABILITY_UNAVAILABLE",
        "messages": ("retain_symbol", "external C resource"),
        "purpose": "non-copyable C resource parameter is rejected",
    },
)
REJECT_BY_ID = {case["id"]: case for case in REJECTS}

# ---------------------------------------------------------------------------
# Public-object mode.
#
# The legacy path above compiles every Source twice: once with the public
# ``build`` and once with the reference ``lang_native_helper`` purely to obtain
# before.ll for the observer.  This mode instead takes the ``program.o`` that
# the tested role's own public ``build`` published and relinks the unchanged
# allocation observer from that object.  The candidate Source therefore never
# goes through a C++ frontend or the old helper.
#
# The original string case also needs its private ``before.ll`` assertion
# containing ``lang_text_concat_v1``.  A published object cannot prove that, so
# the public-object mode accepts the case only together with an explicit
# test-only Native observation driver.  The driver is not a product CLI and is
# never used to build ``program`` or ``program.o``.
PUBLIC_POSITIVE_IDS = tuple(case["id"] for case in CASES)
PUBLIC_REJECT_IDS = tuple(case["id"] for case in REJECTS)
PUBLIC_CASE_IDS = PUBLIC_POSITIVE_IDS + PUBLIC_REJECT_IDS
PUBLIC_IR_REQUIRED_IDS = ("param-string-await",)
PUBLIC_MIGRATED_POSITIVE_IDS = tuple(
    case_id for case_id in PUBLIC_POSITIVE_IDS
    if case_id not in PUBLIC_IR_REQUIRED_IDS)
LLVM_TRACE_TOOLS = ("clang", "clang++", "ld.lld", "opt", "llc")
PUBLIC_SCHEMA = "woozy/native-async-parameter-copy-public-object/1"
IR_SCHEMA = "woozy/native-async-parameter-copy-ir-observation/1"


def load_public_corpus(root: Path):
    """Load the existing bounded process runner from the given Root."""
    path = root / "selfhost/corpus.py"
    need(path.is_file(), f"missing corpus driver: {path}")
    spec = importlib.util.spec_from_file_location("woozy_public_corpus", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def public_selection(only, ir_driver=None):
    selected = list(dict.fromkeys(only or ()))
    need(bool(selected),
         "public-object selection is empty; refusing a vacuous pass")
    unknown = sorted(set(selected) - set(PUBLIC_CASE_IDS))
    need(not unknown, "not migrated to public objects: " + ", ".join(unknown))
    needs_ir = sorted(set(selected) & set(PUBLIC_IR_REQUIRED_IDS))
    if needs_ir and ir_driver is None:
        raise RuntimeError(
            "param-string-await keeps its private before.ll "
            "lang_text_concat_v1 assertion; pass --ir-driver with the "
            "test-only Native observation driver: " + ", ".join(needs_ir))
    if ir_driver is not None and not needs_ir:
        raise RuntimeError(
            "--ir-driver is only used by param-string-await; add that case "
            "to --only or remove --ir-driver")
    return selected


def resolve_ir_driver(args):
    """Validate the explicit test-only before.ll emitter for string."""
    path = Path(args.ir_driver)
    need(path.is_file(), f"IR driver does not exist: {path}")
    need(os.access(path, os.X_OK), f"IR driver is not executable: {path}")
    name = path.name.lower()
    need("helper" not in name and "frontend" not in name,
         "IR driver must be the test-only Native observation driver, not a "
         f"C++ helper/frontend: {path}")
    if args.cli is not None:
        need(path.resolve() != Path(args.cli).resolve(),
             "IR driver must not be the public compiler CLI")
    if args.ir_driver_sha256:
        expected = args.ir_driver_sha256.lower()
        need(len(expected) == 64 and all(c in "0123456789abcdef"
                                         for c in expected),
             "IR driver SHA-256 must be 64 lowercase hex digits")
        actual = MEMORY.digest(path)
        need(actual == expected,
             f"IR driver SHA-256 mismatch: expected {expected}, got {actual}")
    return path.resolve()


def text_of(data: bytes) -> str:
    return data.decode("utf-8", "surrogateescape")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def evidence_path(path: Path, out: Path) -> str:
    try:
        return str(path.relative_to(out))
    except ValueError:
        return str(path)


def make_public_context(args, corpus):
    env = os.environ.copy()
    if args.libxml2_lib:
        previous = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = str(args.libxml2_lib) + (
            ":" + previous if previous else "")
    allowed = set()
    if args.cli is not None:
        allowed.add(str(Path(args.cli).resolve()))
    if args.clang is not None:
        allowed.add(str(Path(args.clang).resolve()))
    if args.toolchain is not None:
        for name in LLVM_TRACE_TOOLS:
            path = Path(args.toolchain) / name
            if path.exists():
                allowed.add(str(path.resolve()))
    for name in args.trace_allow:
        if name.exists():
            allowed.add(str(name.resolve()))
    link_allowed = set()
    if args.clang is not None:
        link_allowed.add(str(Path(args.clang).resolve()))
    if args.toolchain is not None:
        for name in ("clang", "clang-22", "clang++", "ld.lld", "lld"):
            path = Path(args.toolchain) / name
            if path.exists():
                link_allowed.add(str(path.resolve()))
    ir_allowed = set()
    if getattr(args, "ir_driver", None) is not None:
        ir_allowed.add(str(Path(args.ir_driver).resolve()))
    return {
        "corpus": corpus,
        "env": env,
        "cwd": args.root,
        "strace": args.strace,
        "role": args.role,
        "allowed_executables": allowed,
        "observer_link_allowed_executables": link_allowed,
        "ir_allowed_executables": ir_allowed,
        "out": args.out,
        "commands": [],
        "hashes": {},
    }


def run_command(ctx, argv, timeout, phase, label, command_root, cwd=None):
    """Run one command through the shared bounded process-group runner."""
    argv = [str(part) for part in argv]
    cwd = Path(cwd or ctx["cwd"])
    started = time.monotonic()
    result = ctx["corpus"]._run_process_impl(argv, cwd, ctx["env"], timeout)
    elapsed = round(time.monotonic() - started, 6)
    stdout, stderr = result["stdout"], result["stderr"]
    index = len(ctx["commands"])
    record_dir = command_root / f"{index:03d}-{phase}"
    record_dir.mkdir(parents=True)
    (record_dir / "stdout.bin").write_bytes(stdout)
    (record_dir / "stderr.bin").write_bytes(stderr)
    record = {
        "index": index,
        "phase": phase,
        "label": label,
        "argv": argv,
        "cwd": str(cwd),
        "exit": result["exit"],
        "signalled": result["exit"] is not None and result["exit"] < 0,
        "status": ("timed_out" if result["timed_out"] else
                   "signalled" if result["exit"] is not None
                   and result["exit"] < 0 else "exited"),
        "timed_out": result["timed_out"],
        "cleanup_complete": result["cleanup_complete"],
        "abandoned_pipes": result["abandoned_pipes"],
        "timeout_s": timeout,
        "elapsed_s": elapsed,
        "stdout_path": evidence_path(record_dir / "stdout.bin", ctx["out"]),
        "stderr_path": evidence_path(record_dir / "stderr.bin", ctx["out"]),
        "stdout_sha256": sha256_bytes(stdout),
        "stderr_sha256": sha256_bytes(stderr),
    }
    (record_dir / "command.json").write_text(
        json.dumps(record, indent=2) + "\n", encoding="utf-8")
    ctx["commands"].append(record)
    return {"record": record, "stdout": stdout, "stderr": stderr}


def require_clean_command(result, label):
    """Require one process record with exact integer/boolean field types.

    Truthiness is deliberately not enough: ``False``, ``0.0``, ``0`` and
    ``None`` must not be accepted as a clean exit or as a clean process flag.
    Missing fields fail explicitly through the same RuntimeError path.
    """
    record = result["record"]
    exit_code = record.get("exit")
    need(type(exit_code) is int,
         f"{label} exit is not an int (missing or wrong type): {exit_code!r}")
    need(exit_code >= 0,
         f"{label} terminated by signal: exit {exit_code}")
    timed_out = record.get("timed_out")
    need(timed_out is False,
         f"{label} timed_out is not exactly False: {timed_out!r}")
    cleanup_complete = record.get("cleanup_complete")
    need(cleanup_complete is True,
         f"{label} cleanup_complete is not exactly True: "
         f"{cleanup_complete!r}")
    abandoned_pipes = record.get("abandoned_pipes")
    need(abandoned_pipes is False,
         f"{label} abandoned_pipes is not exactly False: "
         f"{abandoned_pipes!r}")
    return record


def process_guard_controls():
    """Pure in-memory controls for require_clean_command; no subprocesses."""
    checks = {}
    safe = {
        "exit": 0,
        "timed_out": False,
        "cleanup_complete": True,
        "abandoned_pipes": False,
    }

    def record(**overrides):
        value = dict(safe)
        value.update(overrides)
        return {"record": value}

    def missing(field):
        value = dict(safe)
        del value[field]
        return {"record": value}

    def accepted(name, result):
        try:
            require_clean_command(result, name)
        except Exception as error:  # noqa: BLE001 - control boundary
            checks[name] = {"status": "failed", "error": str(error)}
        else:
            checks[name] = {"status": "passed"}

    def rejected(name, result):
        try:
            require_clean_command(result, name)
        except Exception as error:  # noqa: BLE001 - control boundary
            checks[name] = {"status": "passed", "rejected": str(error)}
        else:
            checks[name] = {"status": "failed",
                            "error": "control was accepted"}

    accepted("guard_valid_exit0_accepted", record())
    accepted("guard_valid_positive_exit_accepted", record(exit=1))
    rejected("guard_false_exit_rejected", record(exit=False))
    rejected("guard_true_exit_rejected", record(exit=True))
    rejected("guard_float_exit_rejected", record(exit=0.0))
    rejected("guard_none_exit_rejected", record(exit=None))
    rejected("guard_negative_exit_rejected", record(exit=-1))
    rejected("guard_missing_exit_rejected", missing("exit"))
    for flag in ("timed_out", "cleanup_complete", "abandoned_pipes"):
        for value in (0, 1):
            rejected(f"guard_{flag}_int_{value}_rejected",
                     record(**{flag: value}))
        rejected(f"guard_{flag}_none_rejected", record(**{flag: None}))
        rejected(f"guard_{flag}_missing_rejected", missing(flag))
    rejected("guard_timed_out_true_rejected", record(timed_out=True))
    rejected("guard_cleanup_complete_false_rejected",
             record(cleanup_complete=False))
    rejected("guard_abandoned_pipes_true_rejected",
             record(abandoned_pipes=True))
    return checks


def audit_exec_trace(ctx, trace_path: Path, label, allowed, policy,
                     forbid_source_helper=False):
    """Fail closed unless one command ran only its exact allowed programs."""
    corpus = ctx["corpus"]
    try:
        observed = corpus.parse_trace(trace_path)
    except corpus.TraceError as error:
        raise RuntimeError(f"{label} trace unparseable: {error}")
    unexpected = sorted(set(observed) - set(allowed))
    need(not unexpected,
         f"{label} trace ran outside the allowed set: {unexpected}")
    if forbid_source_helper:
        forbidden = [path for path in observed
                     if "helper" in Path(path).name.lower()
                     or "frontend" in Path(path).name.lower()]
        need(not forbidden,
             f"{label} candidate Source used a C++ helper/frontend: {forbidden}")
    return {"status": "ok", "observed": sorted(observed),
            "allowed": sorted(allowed),
            "trace_path": evidence_path(trace_path, ctx["out"]),
            "policy": policy}


def audit_public_trace(ctx, trace_path: Path, label):
    """Fail closed unless the Source compile ran only the allowed programs."""
    if ctx["role"] == "candidate":
        policy = "candidate Source compile: this role's CLI + fixed LLVM only"
    else:
        policy = ("reference Source compile: reference CLI + its compiled-in "
                  "stage0 helper + fixed LLVM")
    return audit_exec_trace(
        ctx, trace_path, label, ctx["allowed_executables"], policy,
        forbid_source_helper=ctx["role"] == "candidate")


def run_traced_command(ctx, argv, timeout, phase, label, command_root,
                       trace_path: Path, allowed, policy,
                       forbid_source_helper=False):
    """Run one command under exec-only strace and audit its exact set."""
    trace = None
    wrapped = [str(part) for part in argv]
    if ctx["strace"]:
        trace_path.parent.mkdir(parents=True, exist_ok=True)
        wrapped = [str(ctx["strace"]), "--seccomp-bpf", "-f", "-qq",
                   "-s", "16384", "-e", "trace=execve,execveat",
                   "-o", str(trace_path), *wrapped]
    result = run_command(ctx, wrapped, timeout, phase, label, command_root)
    record = require_clean_command(result, label)
    if ctx["strace"]:
        trace = audit_exec_trace(ctx, trace_path, label, allowed, policy,
                                 forbid_source_helper=forbid_source_helper)
        MEMORY.record_hash(ctx["hashes"], ctx["out"], trace_path)
    return result, record, trace


def public_static_elf(path: Path, args, ctx, command_root, label):
    need(path.is_file(), f"{label}: missing ELF {path}")
    need(args.readelf.is_file(),
         f"{label}: missing GNU readelf {args.readelf}; not_run is not a pass")
    result = run_command(ctx, [args.readelf, "-l", "-d", path],
                         min(args.timeout, 60), "readelf", label,
                         command_root)
    record = require_clean_command(result, f"{label} readelf")
    need(record["exit"] == 0, f"{label} readelf failed: exit {record['exit']}")
    stdout = text_of(result["stdout"])
    need("Elf file type is EXEC" in stdout,
         f"{label} readelf did not describe a real EXEC ELF: {stdout[:200]!r}")
    need("Program Headers:" in stdout,
         f"{label} readelf printed no program headers: {stdout[:200]!r}")
    need("INTERP" not in stdout and "NEEDED" not in stdout,
         f"{label} is not a static freestanding ELF")
    return {"status": "passed", "command": record["index"],
            "readelf": str(args.readelf),
            "stdout_sha256": record["stdout_sha256"]}


def validate_program_streams(exit_code, stdout_text, stderr_text, case, opt):
    """Original program expectations; shared with the short controls."""
    label = f"{case['id']} O{opt}"
    need(exit_code == 0, f"{label} program execution failed: exit {exit_code}")
    need(stdout_text == case["stdout"],
         f"{label} program stdout mismatch: {stdout_text!r}")
    need(stderr_text == "",
         f"{label} program wrote stderr: {stderr_text!r}")


def validate_observer_streams(exit_code, stdout_text, stderr_text, case, opt):
    """Original observer expectations, including the untouched parser."""
    label = f"{case['id']} O{opt}"
    need(exit_code == 0,
         f"{label} observer execution failed: exit {exit_code}")
    need(stdout_text == case["stdout"],
         f"{label} observer stdout mismatch: {stdout_text!r}")
    need("LANG_NATIVE_PANIC" not in stderr_text,
         f"{label} panicked: {stderr_text!r}")
    metrics = parse_observer(stderr_text)
    need(metrics["final_live_heap_count"] == 0,
         f"{label} leaked heap allocations: {metrics}")
    need(metrics["final_live_heap_bytes"] == 0,
         f"{label} leaked heap bytes: {metrics}")
    need(metrics["heap_allocations"] == metrics["heap_frees"],
         f"{label} unbalanced heap allocations: {metrics}")
    need(metrics["vec_allocations"] == metrics["vec_frees"],
         f"{label} unbalanced Vec count: {metrics}")
    need(metrics["live_vecs_at_exit"] == 0,
         f"{label} leaked Vecs: {metrics}")
    need(metrics["final_live_payload_bytes"] == 0,
         f"{label} leaked Vec payload: {metrics}")
    need(metrics["heap_allocations"] >= case["min_heap_allocations"],
         f"{label} did not allocate the object set: {metrics}")
    need(metrics["vec_allocations"] >= case["min_vec_allocations"],
         f"{label} did not allocate the Vec set: {metrics}")
    return metrics


def validate_reject_payload(payload, case, opt):
    label = f"{case['id']} O{opt}"
    need(payload.get("status") == "error", f"{label}: {payload}")
    diagnostics = payload.get("diagnostics", [])
    need(any(diagnostic.get("code") == case["code"] and
             any(message in diagnostic.get("message", "")
                 for message in case["messages"])
             for diagnostic in diagnostics),
         f"{label} rejected with the wrong diagnostic: {diagnostics}")


def validate_reject_outputs(cli_dir: Path, case, opt):
    label = f"{case['id']} O{opt}"
    for output in ("program", "program.o"):
        need(not (cli_dir / output).exists(),
             f"{label} emitted {output} despite rejection")


def public_source_build(case, opt, args, ctx, directory, command_root):
    """Run this role's public build and audit its subprocess set."""
    case_id = case["id"]
    label = f"{case_id} O{opt}"
    manifest = HERE / "fixtures" / case_id / "lang.toml"
    need(manifest.is_file(), f"missing fixture manifest: {manifest}")
    build_argv = [
        args.cli, "build", "--project", manifest, "--backend", "native",
        "--opt", str(opt), "--out-dir", directory / "cli",
        "--format", "json",
    ]
    trace_path = directory / "trace" / f"source-build.O{opt}.exec.log"
    policy = ("candidate Source compile: this role's CLI + fixed LLVM only"
              if ctx["role"] == "candidate" else
              "reference Source compile: reference CLI + its compiled-in "
              "stage0 helper + fixed LLVM")
    built, record, trace = run_traced_command(
        ctx, build_argv, args.timeout, "source-build",
        f"{label} public Source build", command_root, trace_path,
        ctx["allowed_executables"], policy,
        forbid_source_helper=ctx["role"] == "candidate")
    return built, record, trace


def validate_ir_metadata(metadata, label):
    """The original helper check required status=passed; keep it, record all."""
    need(isinstance(metadata, dict), f"{label} emitted no IR metadata object")
    need(metadata.get("status") == "passed",
         f"{label} IR metadata is not passed: {metadata}")
    need(metadata.get("phase") == "object",
         f"{label} IR metadata phase is not object: {metadata}")
    return {
        key: metadata.get(key)
        for key in ("status", "phase", "llvm", "contract", "runtime_abi",
                    "triple", "data_layout", "runtime", "c_adapter",
                    "entry_symbol", "word_bits", "has_async", "cfg_verified")
    }


def validate_before_ir(path: Path, label):
    """Keep the original literal marker assertion, without adding counts."""
    need(path.is_file() and path.stat().st_size > 0,
         f"{label} emitted no nonempty before.ll")
    text = path.read_text(encoding="utf-8")
    need("lang_text_concat_v1" in text,
         f"{label} before.ll lacks the original lang_text_concat_v1 marker")
    return text


def public_ir_emit(case, opt, args, ctx, directory, command_root):
    """Generate the original private before.ll with the sealed Wz driver.

    The driver receives the real manifest and matching stdlib, and executes
    only itself.  Its output is evidence only: ``program`` and ``program.o``
    still come from this role's public build, and the observer links that
    object rather than this before.ll.
    """
    case_id = case["id"]
    label = f"{case_id} O{opt} IR"
    need(case_id in PUBLIC_IR_REQUIRED_IDS,
         f"{label} IR emitter is only defined for param-string-await")
    need(args.ir_driver is not None, f"{label} missing --ir-driver")
    need(args.ir_stdlib is not None, f"{label} missing --ir-stdlib")
    manifest = HERE / "fixtures" / case_id / "lang.toml"
    need(manifest.is_file(), f"{label} missing fixture manifest: {manifest}")
    need(args.ir_stdlib.is_dir(),
         f"{label} IR stdlib is missing: {args.ir_stdlib}")
    ir_dir = directory / "ir"
    need(not ir_dir.exists(), f"{label} IR evidence already exists: {ir_dir}")
    ir_dir.mkdir()
    before_path = ir_dir / f"O{opt}.before.ll"
    native_path = ir_dir / f"O{opt}.nir"
    metadata_path = ir_dir / f"O{opt}.metadata.json"
    driver_argv = [
        args.ir_driver, "--project", manifest, "--stdlib", args.ir_stdlib,
        "--opt", str(opt), "--out-llvm-before", before_path,
        "--out-native", native_path, "--format", "json",
    ]
    trace_path = directory / "trace" / f"ir-emit.O{opt}.exec.log"
    emitted, emit_record, emit_trace = run_traced_command(
        ctx, driver_argv, args.timeout, "ir-emit",
        f"{label} real before.ll emit", command_root, trace_path,
        ctx["ir_allowed_executables"],
        "IR observation: only the sealed test-only Nativeobs driver")
    need(emit_record["exit"] == 0,
         f"{label} driver failed: exit {emit_record['exit']}")
    metadata = None
    if emitted["stdout"]:
        try:
            metadata = json.loads(text_of(emitted["stdout"]))
        except json.JSONDecodeError as error:
            raise RuntimeError(f"{label} metadata is not JSON: {error}")
    metadata_record = validate_ir_metadata(metadata, label)
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n",
                             encoding="utf-8")
    validate_before_ir(before_path, label)
    need(native_path.is_file() and native_path.stat().st_size > 0,
         f"{label} emitted no nonempty Native CFG")
    # The flags must be the real paths this function asked for, not a
    # post-hoc metadata or stdout claim.
    need("--out-llvm-before" in driver_argv
         and str(before_path) == str(driver_argv[driver_argv.index(
             "--out-llvm-before") + 1]),
         f"{label} --out-llvm-before path is not the observed file")
    need("--out-native" in driver_argv
         and str(native_path) == str(driver_argv[driver_argv.index(
             "--out-native") + 1]),
         f"{label} --out-native path is not the observed file")
    opt_path = args.toolchain / "opt"
    need(opt_path.is_file(), f"{label} missing fixed opt: {opt_path}")
    verify_trace = directory / "trace" / f"ir-verify.O{opt}.exec.log"
    verified, verify_record, verify_trace = run_traced_command(
        ctx, [opt_path, "-passes=verify", "-disable-output", before_path],
        args.timeout, "ir-verify", f"{label} LLVM verify", command_root,
        verify_trace, {str(opt_path.resolve())},
        "IR verify: fixed LLVM opt only")
    need(verify_record["exit"] == 0,
         f"{label} opt verify failed: exit {verify_record['exit']}")
    for path in (before_path, native_path, metadata_path):
        MEMORY.record_hash(ctx["hashes"], ctx["out"], path)
    return {
        "schema": IR_SCHEMA,
        "status": "passed",
        "driver": str(args.ir_driver),
        "driver_sha256": MEMORY.digest(args.ir_driver),
        "driver_sha256_expected": args.ir_driver_sha256,
        "stdlib": str(args.ir_stdlib),
        "stdlib_tree_sha256": tree_digest(args.ir_stdlib),
        "manifest": str(manifest),
        "manifest_sha256": MEMORY.digest(manifest),
        "source_sha256": MEMORY.digest(
            HERE / "fixtures" / case_id / "src/main.wz"),
        "opt": opt,
        "driver_argv": [str(part) for part in driver_argv],
        "emit": {"command": emit_record["index"],
                 "exit": emit_record["exit"],
                 "argv": emit_record["argv"],
                 "cwd": emit_record["cwd"],
                 "timeout_s": emit_record["timeout_s"],
                 "elapsed_s": emit_record["elapsed_s"],
                 "timed_out": emit_record["timed_out"],
                 "cleanup_complete": emit_record["cleanup_complete"],
                 "abandoned_pipes": emit_record["abandoned_pipes"],
                 "stdout_sha256": emit_record["stdout_sha256"],
                 "stderr_sha256": emit_record["stderr_sha256"],
                 "trace": emit_trace},
        "verify": {"command": verify_record["index"],
                   "exit": verify_record["exit"],
                   "argv": verify_record["argv"],
                   "trace": verify_trace},
        "metadata": metadata_record,
        "before": {"path": evidence_path(before_path, ctx["out"]),
                   "sha256": MEMORY.digest(before_path),
                   "size": before_path.stat().st_size,
                   "lang_text_concat_v1": True},
        "native": {"path": evidence_path(native_path, ctx["out"]),
                   "sha256": MEMORY.digest(native_path),
                   "size": native_path.stat().st_size},
    }


def public_positive(case, opt, args, ctx):
    case_id = case["id"]
    label = f"{case_id} O{opt}"
    directory = ctx["out"] / case_id / f"O{opt}"
    need(not directory.exists(),
         f"evidence directory already exists: {directory}")
    directory.mkdir(parents=True)
    command_root = directory / "commands"

    built, build_record, trace = public_source_build(
        case, opt, args, ctx, directory, command_root)
    need(build_record["exit"] == 0,
         f"{label} public Source build failed: exit {build_record['exit']}")
    payload = json.loads(text_of(built["stdout"]))
    need(payload.get("status") == "ok", f"{label} build: {payload}")
    need(not any(diagnostic.get("code") == "E_CAPABILITY_UNAVAILABLE"
                 for diagnostic in payload.get("diagnostics", [])),
         f"{label} silently fell back")

    program = directory / "cli" / "program"
    program_object = directory / "cli" / "program.o"
    need(program.is_file(), f"{label} emitted no program")
    need(program_object.is_file(),
         f"{label} emitted no program.o; the observer cannot use this role's object")
    program_elf = public_static_elf(program, args, ctx, command_root,
                                    f"{label} program")

    program_trace = directory / "trace" / f"program-execute.O{opt}.exec.log"
    executed, execute_record, program_trace_record = run_traced_command(
        ctx, [program], args.timeout, "program-execute",
        f"{label} program execution", command_root, program_trace,
        {str(program.resolve())},
        "program execution: the published static ELF only")
    validate_program_streams(execute_record["exit"], text_of(executed["stdout"]),
                             text_of(executed["stderr"]), case, opt)

    observer_dir = directory / "observe"
    observer_dir.mkdir()
    observer = observer_dir / "observe"
    link_argv = [
        args.clang, "--target=x86_64-unknown-linux-gnu",
        f"--ld-path={args.toolchain / 'ld.lld'}",
        f"-O{opt}", "-std=c++20", program_object, args.observer,
        args.runtime, "-o", observer, "-nostdlib", "-static",
        "-Wl,--wrap=lang_alloc_v1", "-Wl,--wrap=lang_free_v1",
    ]
    link_trace_path = directory / "trace" / f"observer-link.O{opt}.exec.log"
    linked, link_record, link_trace = run_traced_command(
        ctx, link_argv, args.timeout, "observer-link",
        f"{label} observer link", command_root, link_trace_path,
        ctx["observer_link_allowed_executables"],
        "test-only observer link: fixed clang + ld.lld only")
    need(link_record["exit"] == 0,
         f"{label} observer link failed: exit {link_record['exit']}")
    observer_elf = public_static_elf(observer, args, ctx, command_root,
                                     f"{label} observer")

    observer_trace = directory / "trace" / f"observer-execute.O{opt}.exec.log"
    observed, observe_record, observer_trace_record = run_traced_command(
        ctx, [observer], args.timeout, "observer-execute",
        f"{label} observer execution", command_root, observer_trace,
        {str(observer.resolve())},
        "observer execution: the linked static observer ELF only")
    metrics = validate_observer_streams(
        observe_record["exit"], text_of(observed["stdout"]),
        text_of(observed["stderr"]), case, opt)

    ir = None
    if case_id in PUBLIC_IR_REQUIRED_IDS:
        ir = public_ir_emit(case, opt, args, ctx, directory, command_root)

    for path in (program, program_object, observer):
        MEMORY.record_hash(ctx["hashes"], ctx["out"], path)
    return {
        "status": "passed",
        "purpose": case["purpose"],
        "source_build": {"command": build_record["index"],
                         "exit": build_record["exit"],
                         "stderr_sha256": build_record["stderr_sha256"],
                         "stderr_path": build_record["stderr_path"]},
        "candidate_source_trace": trace,
        "program": {"command": execute_record["index"],
                    "exit": execute_record["exit"],
                    "trace": program_trace_record},
        "observer_link": {
            "command": link_record["index"],
            "trace_scope": ("test-only clang observer shim link; not part "
                            "of the candidate Source trace whitelist"),
            "trace": link_trace,
            "program_object": evidence_path(program_object, ctx["out"]),
            "program_object_sha256": MEMORY.digest(program_object),
            "observer_source_sha256": MEMORY.digest(args.observer),
            "runtime_sha256": MEMORY.digest(args.runtime),
        },
        "observer": {"command": observe_record["index"],
                     "exit": observe_record["exit"],
                     "trace": observer_trace_record},
        "ir": ir,
        "metrics": metrics,
        "elf": program_elf,
        "observer_elf": observer_elf,
        "program_sha256": MEMORY.digest(program),
        "observer_sha256": MEMORY.digest(observer),
    }


def public_reject(case, opt, args, ctx):
    case_id = case["id"]
    label = f"{case_id} O{opt}"
    directory = ctx["out"] / case_id / f"O{opt}"
    need(not directory.exists(),
         f"evidence directory already exists: {directory}")
    directory.mkdir(parents=True)
    command_root = directory / "commands"

    built, build_record, trace = public_source_build(
        case, opt, args, ctx, directory, command_root)
    need(build_record["exit"] is not None and build_record["exit"] > 0,
         f"{label} was not a normal non-zero rejection: exit {build_record['exit']}")
    payload = json.loads(text_of(built["stdout"]))
    validate_reject_payload(payload, case, opt)
    validate_reject_outputs(directory / "cli", case, opt)
    for path in (HERE / "fixtures" / case_id / "lang.toml",
                 HERE / "fixtures" / case_id / "binding.json"):
        MEMORY.record_hash(ctx["hashes"], ctx["out"], path)
    return {
        "status": "passed",
        "purpose": case["purpose"],
        "code": case["code"],
        "messages": list(case["messages"]),
        "source_build": {"command": build_record["index"],
                         "exit": build_record["exit"],
                         "stderr_sha256": build_record["stderr_sha256"],
                         "stderr_path": build_record["stderr_path"]},
        "candidate_source_trace": trace,
        "published": {"program": False, "program.o": False},
    }


def snapshot_inputs(paths):
    return {key: (MEMORY.digest(path) if path.is_file() else None)
            for key, path in paths}


def public_input_paths(args):
    paths = [
        ("repo/selfhost/corpus.py", args.root / "selfhost/corpus.py"),
        ("runner/run_parameter_copy.py", HERE / "run_parameter_copy.py"),
        ("cli/woozyc", args.cli),
        ("runtime/liblang_native_runtime.a", args.runtime),
        ("observer/observe_gc.cpp", args.observer),
        ("header/runtime/native/vec.hpp",
         args.root / "runtime/native/vec.hpp"),
        ("toolchain/clang", args.clang),
        ("toolchain/ld.lld", args.toolchain / "ld.lld"),
        ("toolchain/opt", args.toolchain / "opt"),
        ("toolchain/llc", args.toolchain / "llc"),
        ("toolchain/llvm-readelf", args.readelf),
    ]
    if args.strace is not None:
        paths.append(("toolchain/strace", args.strace))
    if getattr(args, "ir_driver", None) is not None:
        paths.append(("ir-driver/nativeobs", args.ir_driver))
    for name in args.trace_allow:
        paths.append((f"trace-allow/{name.name}", name))
    for label, path in getattr(args, "extra_input_pairs", []):
        paths.append((f"extra/{label}", path))
    for fixture in sorted((HERE / "fixtures").iterdir()):
        for path in sorted(fixture.rglob("*")):
            if path.is_file():
                paths.append((f"fixture/{path.relative_to(HERE)}", path))
    return paths


def tree_digest(path: Path):
    if not path.exists():
        return None
    entries = []
    for child in sorted(path.rglob("*")):
        if child.is_file():
            entries.append([str(child.relative_to(path)), MEMORY.digest(child)])
    return sha256_bytes(json.dumps(entries, separators=(",", ":")).encode())


def record_stdlib_after(resource_root: Path, resources):
    after = tree_digest(resource_root / "stdlib")
    resources["stdlib_tree_sha256_after"] = after
    before = resources["stdlib_tree_sha256"]
    resources["stdlib_tree_unchanged"] = before is not None and before == after
    need(resources["stdlib_tree_unchanged"],
         "stdlib bytes changed during the run or the tree is missing")


def tree_snapshot(path: Path):
    """Return a real per-file SHA map for a stable source/resource tree."""
    need(path.is_dir(), f"snapshot root is missing: {path}")
    mapping = {}
    for child in sorted(path.rglob("*")):
        if child.is_file():
            mapping[str(child.relative_to(path))] = MEMORY.digest(child)
    need(bool(mapping), f"snapshot root contains no files: {path}")
    return mapping


def snapshot_digest(mapping):
    return sha256_bytes(json.dumps(mapping, sort_keys=True,
                                   separators=(",", ":")).encode())


def write_snapshots(out: Path, snapshots, phase):
    directory = out / "snapshots"
    directory.mkdir(parents=True, exist_ok=True)
    records = {}
    for label, (root, mapping) in snapshots.items():
        path = directory / f"{label}.{phase}.json"
        path.write_text(json.dumps({
            "root": str(root),
            "files": len(mapping),
            "map_sha256": snapshot_digest(mapping),
            "map": mapping,
        }, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        records[label] = {
            "root": str(root),
            "files": len(mapping),
            "map_sha256": snapshot_digest(mapping),
            "path": evidence_path(path, out),
            "sha256": MEMORY.digest(path),
        }
    return records


def write_proposal(path: Path, summary, selected, options):
    rows = []
    for case_id in PUBLIC_CASE_IDS:
        if case_id not in selected:
            continue
        entry = {
            "id": case_id,
            "enabled": False,
            "required": False,
            "operation": "build_native",
            "backend": "native",
            "project": f"tests/async_parameter_copy/fixtures/{case_id}/lang.toml",
            "evidence": {},
        }
        if case_id in CASE_BY_ID:
            case = CASE_BY_ID[case_id]
            entry.update({
                "stdout": case["stdout"],
                "min_heap_allocations": case["min_heap_allocations"],
                "min_vec_allocations": case["min_vec_allocations"],
                "observer": "tests/native/language_gc_source/observe_gc.cpp",
                "observer_scope": ("private runner evidence; the generic "
                                   "corpus schema cannot express observer or "
                                   "static ELF assertions"),
            })
        else:
            case = REJECT_BY_ID[case_id]
            entry.update({"code": case["code"],
                          "messages": list(case["messages"])})
        for opt in options:
            result = summary.get("cases", {}).get(case_id, {}).get(
                f"O{opt}", {})
            entry["evidence"][f"O{opt}"] = {
                "status": result.get("status", "not_run"),
                "detail": result,
            }
        rows.append(entry)
    runner_only = []
    for row in rows:
        case_id = row["id"]
        item = {
            "id": case_id,
            "observer": {
                "source": "tests/native/language_gc_source/observe_gc.cpp",
                "linked_from": "this role's published program.o",
                "assertions": [
                    "observer exit 0 and exact stdout",
                    "parse_observer accepts the complete stderr",
                    "zero final live heap count/bytes and payload",
                    "balanced heap and Vec allocations/frees",
                    "original per-case allocation minima",
                ],
            },
            "static_elf": ["program: EXEC, no INTERP/NEEDED",
                           "observer: EXEC, no INTERP/NEEDED"],
            "not_expressible_by_corpus_schema": True,
        }
        if case_id in PUBLIC_IR_REQUIRED_IDS:
            item["ir"] = {
                "driver": "test-only Nativeobs Wz driver",
                "output": "O{0,2}.before.ll + O{0,2}.nir",
                "assertion": "literal lang_text_concat_v1 in real before.ll",
                "llvm_verify": "fixed opt -passes=verify -disable-output",
                "not_a_product_cli_ir_export": True,
            }
        runner_only.append(item)
    document = {
        "schema": "woozy/native-async-parameter-copy-proposal/1",
        "source": "SH12o async parameter copy public-object run",
        "enabled": False,
        "notes": [
            "Proposal only: update the eleven existing case ids; do not add "
            "duplicate cases and do not enable them from this file.",
            "param-string-await row is backed by the original before.ll "
            "lang_text_concat_v1 observation through the explicit test-only "
            "Nativeobs driver, but it remains disabled here.",
            "The generic corpus schema does not express the observer, "
            "static-ELF or before.ll evidence recorded by this runner.",
            "These rows are not written to Root, catalog or PLAN.",
        ],
        "cases": rows,
        "runner_only_evidence": runner_only,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


def public_self_test(args):
    """Short negative controls for the public-object checks.

    These controls feed the same validators and process runner used by real
    runs; they do not reimplement parse_observer expectations.
    """
    base = Path(tempfile.mkdtemp(prefix="param-copy-public-controls-"))
    root = (args.root or HERE.parent.parent).resolve()
    corpus = load_public_corpus(root)
    case = CASE_BY_ID["param-vec-late-start"]
    reject_case = REJECT_BY_ID["reject-resource-copy"]
    clean = (
        "CONFIG:vec_header_bytes:80\n"
        "ALLOC:0x1:84\n"
        "VEC:1:0x1:1\n"
        "VEC:4:0x1:0\n"
        "VEC:5:0x1:0\n"
        "FREE:0x1\n"
        "ALLOC:0x2:16\n"
        "FREE:0x2\n"
        "ALLOC:0x3:16\n"
        "FREE:0x3\n"
    )
    checks = {}
    checks.update(process_guard_controls())

    def rejected(name, function):
        try:
            function()
        except Exception as error:  # noqa: BLE001 - control boundary
            checks[name] = {"status": "passed", "rejected": str(error)}
        else:
            checks[name] = {"status": "failed",
                            "error": "control was accepted"}

    def accepted(name, function):
        try:
            function()
        except Exception as error:  # noqa: BLE001 - control boundary
            checks[name] = {"status": "failed", "error": str(error)}
        else:
            checks[name] = {"status": "passed"}

    accepted("clean_observer_accepted",
             lambda: validate_observer_streams(0, case["stdout"], clean,
                                               case, 0))
    accepted("program_accepted",
             lambda: validate_program_streams(0, case["stdout"], "", case, 0))
    stdlib = base / "stdlib"
    stdlib.mkdir()
    module = stdlib / "test.wz"
    module.write_text("before\n", encoding="utf-8")
    resources = {"stdlib_tree_sha256": tree_digest(stdlib)}
    accepted("stdlib_unchanged_accepted",
             lambda: record_stdlib_after(base, resources))
    module.write_text("after\n", encoding="utf-8")
    rejected("stdlib_change_rejected",
             lambda: record_stdlib_after(base, resources))
    rejected("empty_selection_rejected", lambda: public_selection([]))
    rejected("string_selection_rejected",
             lambda: public_selection(["param-string-await"]))

    # IR-driver controls.  The driver identity is a real SHA-256 check, not a
    # forbidden-word substitute for the exec trace used by a real run.
    fake_driver = base / "fake-nativeobs"
    fake_driver.write_text(
        """#!/bin/sh
out_llvm=''
out_nir=''
while [ $# -gt 0 ]; do
  case "$1" in
    --out-llvm-before) out_llvm="$2"; shift 2;;
    --out-native) out_nir="$2"; shift 2;;
    *) shift;;
  esac
done
printf '; lang_text_concat_v1\ndefine i32 @main() { ret i32 0 }\n' > "$out_llvm"
printf 'cfg\n' > "$out_nir"
printf '{"status":"passed","phase":"object","has_async":true,"cfg_verified":true}\n'
""",
        encoding="utf-8")
    fake_driver.chmod(0o755)
    accepted("string_with_ir_driver_selected",
             lambda: public_selection(["param-string-await"], fake_driver))
    rejected("ir_driver_without_string_rejected",
             lambda: public_selection(["param-vec-late-start"], fake_driver))
    driver_args = argparse.Namespace(
        ir_driver=fake_driver, ir_driver_sha256=MEMORY.digest(fake_driver),
        cli=base / "fake-cli")
    accepted("real_ir_driver_sha_accepted",
             lambda: resolve_ir_driver(driver_args))
    wrong_sha_args = argparse.Namespace(
        ir_driver=fake_driver, ir_driver_sha256="0" * 64,
        cli=base / "fake-cli")
    rejected("wrong_ir_driver_sha_rejected",
             lambda: resolve_ir_driver(wrong_sha_args))
    helper_driver = base / "lang_native_helper"
    helper_driver.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    helper_driver.chmod(0o755)
    helper_args = argparse.Namespace(
        ir_driver=helper_driver, ir_driver_sha256=None,
        cli=base / "fake-cli")
    rejected("candidate_helper_ir_driver_rejected",
             lambda: resolve_ir_driver(helper_args))

    good_ir = base / "good.before.ll"
    good_ir.write_text("; lang_text_concat_v1\n"
                       "define i32 @main() { ret i32 0 }\n",
                       encoding="utf-8")
    bad_ir = base / "bad.before.ll"
    bad_ir.write_text("define i32 @main() { ret i32 0 }\n",
                      encoding="utf-8")
    empty_ir = base / "empty.before.ll"
    empty_ir.write_bytes(b"")
    accepted("real_before_with_concat_accepted",
             lambda: validate_before_ir(good_ir, "control"))
    rejected("before_missing_concat_rejected",
             lambda: validate_before_ir(bad_ir, "control"))
    rejected("before_empty_rejected",
             lambda: validate_before_ir(empty_ir, "control"))
    rejected("before_missing_path_rejected",
             lambda: validate_before_ir(base / "absent.before.ll", "control"))
    accepted("ir_metadata_passed_accepted",
             lambda: validate_ir_metadata(
                 {"status": "passed", "phase": "object"}, "control"))
    rejected("ir_metadata_failed_rejected",
             lambda: validate_ir_metadata(
                 {"status": "failed", "phase": "object"}, "control"))

    fake_toolchain = base / "toolchain"
    fake_toolchain.mkdir()
    fake_opt = fake_toolchain / "opt"
    fake_opt.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    fake_opt.chmod(0o755)
    ir_out = base / "ir-out"
    ir_out.mkdir()
    ir_args = argparse.Namespace(
        ir_driver=fake_driver, ir_driver_sha256=MEMORY.digest(fake_driver),
        ir_stdlib=stdlib, toolchain=fake_toolchain,
        cli=base / "fake-cli", root=root, out=ir_out, trace_allow=[],
        strace=None, timeout=30, role="candidate", libxml2_lib=None,
        clang=fake_toolchain / "clang")
    ir_ctx = make_public_context(ir_args, corpus)
    ir_case = CASE_BY_ID["param-string-await"]
    ir_dir = ir_out / "param-string-await" / "O0"
    ir_dir.mkdir(parents=True)
    def real_ir_control():
        result = public_ir_emit(ir_case, 0, ir_args, ir_ctx, ir_dir,
                                ir_dir / "commands")
        need(result["before"]["path"].endswith("O0.before.ll"),
             "IR control used the wrong before.ll path")
        need(result["driver_argv"][
                 result["driver_argv"].index("--out-llvm-before") + 1]
             == str(ir_dir / "ir" / "O0.before.ll"),
             "IR control flags do not name the observed before.ll")
        need(result["before"]["sha256"] == MEMORY.digest(
            ir_dir / "ir" / "O0.before.ll"), "IR control hash mismatch")
        need(result["driver_sha256_expected"] == MEMORY.digest(fake_driver),
             "IR control did not record the expected driver SHA")
    accepted("real_before_flags_hash_accepted", real_ir_control)

    rejected("program_wrong_stdout_rejected",
             lambda: validate_program_streams(0, "41\n", "", case, 0))
    rejected("program_stderr_rejected",
             lambda: validate_program_streams(0, case["stdout"], "noise\n",
                                              case, 0))
    rejected("observer_wrong_stdout_rejected",
             lambda: validate_observer_streams(0, "41\n", clean, case, 0))
    rejected("observer_panic_rejected",
             lambda: validate_observer_streams(
                 0, case["stdout"], clean + "LANG_NATIVE_PANIC: boom\n",
                 case, 0))
    rejected("observer_junk_rejected",
             lambda: validate_observer_streams(
                 0, case["stdout"], clean + "JUNK\n", case, 0))
    rejected("observer_missing_free_rejected",
             lambda: validate_observer_streams(
                 0, case["stdout"],
                 clean.replace("ALLOC:0x2:16\nFREE:0x2\n", "ALLOC:0x2:16\n"),
                 case, 0))
    rejected("observer_duplicate_free_rejected",
             lambda: validate_observer_streams(
                 0, case["stdout"], clean + "FREE:0x1\n", case, 0))
    rejected("observer_below_minimum_rejected",
             lambda: validate_observer_streams(
                 0, case["stdout"],
                 "CONFIG:vec_header_bytes:80\nALLOC:0x1:84\n"
                 "VEC:1:0x1:1\nVEC:5:0x1:0\nFREE:0x1\n", case, 0))
    rejected("reject_wrong_diagnostic_rejected",
             lambda: validate_reject_payload(
                 {"status": "error", "diagnostics": [{
                     "code": "E_LINK_FAILED",
                     "message": "native linker exited with status 1",
                     "severity": "error"}]}, reject_case, 0))
    rejected("reject_missing_message_rejected",
             lambda: validate_reject_payload(
                 {"status": "error", "diagnostics": [{
                     "code": "E_CAPABILITY_UNAVAILABLE",
                     "message": "copy failed",
                     "severity": "error"}]}, reject_case, 0))

    residual = base / "residual"
    residual.mkdir()
    (residual / "program").write_text("stale", encoding="utf-8")
    rejected("reject_residual_program_rejected",
             lambda: validate_reject_outputs(residual, reject_case, 0))

    fake_cli = base / "fake-woozyc"
    fake_cli.write_text(
        "#!/bin/sh\n"
        "out=\"\"\n"
        "while [ $# -gt 0 ]; do\n"
        "  if [ \"$1\" = \"--out-dir\" ]; then out=\"$2\"; shift 2; continue; fi\n"
        "  shift\n"
        "done\n"
        "mkdir -p \"$out\"\n"
        "printf program > \"$out/program\"\n"
        "printf '{\"status\":\"ok\",\"diagnostics\":[]}\\n'\n",
        encoding="utf-8")
    fake_cli.chmod(0o755)
    fake_args = argparse.Namespace(**vars(args))
    fake_args.cli = fake_cli
    fake_args.out = base / "fake-out"
    fake_args.out.mkdir()
    fake_args.role = "candidate"
    fake_args.strace = None
    fake_args.root = root
    fake_ctx = make_public_context(fake_args, corpus)
    rejected("missing_object_rejected",
             lambda: public_positive(case, 0, fake_args, fake_ctx))

    pipe_out = base / "pipe-out"
    pipe_out.mkdir()
    pipe_ctx = dict(make_public_context(fake_args, corpus), out=pipe_out)
    pipe = run_command(
        pipe_ctx,
        ["/bin/sh", "-c", "sleep 30 & echo CHILD:$! >&2; wait $!"],
        0.4, "control-timeout", "grandchild pipe control",
        pipe_out / "commands")
    record = pipe["record"]
    import re
    match = re.search(r"CHILD:(\d+)", text_of(pipe["stderr"]))
    gone = MEMORY.wait_pid_gone(int(match.group(1))) if match else False
    checks["grandchild_pipe_bounded"] = {
        "status": "passed" if (record["timed_out"] and gone
                                and record["cleanup_complete"]
                                and not record["abandoned_pipes"])
        else "failed",
        "timed_out": record["timed_out"],
        "cleanup_complete": record["cleanup_complete"],
        "abandoned_pipes": record["abandoned_pipes"],
        "grandchild_gone": gone,
    }

    result = {
        "schema": "woozy/native-async-parameter-copy-public-controls/1",
        "status": "passed" if all(check["status"] == "passed"
                                  for check in checks.values()) else "failed",
        "directory": str(base),
        "checks": checks,
    }
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "passed" else 1


def parse_label_paths(values, option):
    result = []
    seen = set()
    for value in values or []:
        need("=" in value, f"{option} must be LABEL=PATH: {value!r}")
        label, raw = value.split("=", 1)
        need(label and "/" not in label and "\\" not in label,
             f"{option} label is not a simple name: {label!r}")
        need(label not in seen, f"duplicate {option} label: {label}")
        seen.add(label)
        result.append((label, Path(raw).resolve()))
    return result


def write_case_record(out: Path, case_id: str, summary):
    directory = out / case_id
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "case.json"
    path.write_text(json.dumps({
        "id": case_id,
        "status": summary.get("status", "failed"),
        "O0": summary.get("O0", {}),
        "O2": summary.get("O2", {}),
    }, indent=2) + "\n", encoding="utf-8")


def command_counts(commands):
    counts = {}
    for record in commands:
        counts[record["phase"]] = counts.get(record["phase"], 0) + 1
    return counts


def main_public(args, parser):
    if args.helper is not None:
        parser.error("--public-object does not accept --helper; the observer "
                     "must use this role's published program.o")
    if args.role is None:
        parser.error("--public-object requires --role reference|candidate")
    if args.role == "candidate" and args.trace_allow:
        parser.error("--trace-allow is reference-only; a candidate Source "
                     "trace allows only the CLI and fixed LLVM")
    if args.ir_driver is not None:
        try:
            args.ir_driver = resolve_ir_driver(args)
        except Exception as error:  # noqa: BLE001 - command-line boundary
            parser.error(str(error))
    else:
        args.ir_driver = None
    try:
        selected = public_selection(
            args.only if args.only else list(PUBLIC_CASE_IDS),
            ir_driver=args.ir_driver)
    except Exception as error:  # noqa: BLE001 - command-line boundary
        parser.error(str(error))
    needs_ir = bool(set(selected) & set(PUBLIC_IR_REQUIRED_IDS))
    if needs_ir and args.ir_stdlib is None:
        parser.error("param-string-await requires --ir-stdlib with the same "
                     "37-file stdlib used by the test-only IR driver")
    if not needs_ir and args.ir_stdlib is not None:
        parser.error("--ir-stdlib is only used with param-string-await")

    required = ("cli", "runtime", "toolchain", "root", "out")
    missing = [name for name in required if getattr(args, name) is None]
    if missing:
        parser.error("missing required arguments: " + ", ".join(missing))
    try:
        extra_inputs = parse_label_paths(args.extra_input, "--extra-input")
        snapshot_roots = parse_label_paths(args.snapshot_root,
                                          "--snapshot-root")
    except Exception as error:  # noqa: BLE001 - command-line boundary
        parser.error(str(error))
    for label, path in extra_inputs:
        if not path.is_file():
            parser.error(f"--extra-input {label} is not a file: {path}")
    for label, path in snapshot_roots:
        if not path.is_dir():
            parser.error(f"--snapshot-root {label} is not a directory: {path}")

    args.root = args.root.resolve()
    args.out = args.out.resolve()
    args.cli = args.cli.resolve()
    args.runtime = args.runtime.resolve()
    args.toolchain = args.toolchain.resolve()
    args.clang = (args.clang or args.toolchain / "clang").resolve()
    # Keep the llvm-readelf basename: llvm-readobj selects its output mode
    # from argv[0], so resolving the symlink would change the printed shape.
    args.readelf = (args.readelf or args.toolchain / "llvm-readelf").absolute()
    args.observer = (args.root / OBSERVER).resolve()
    args.trace_allow = [path.resolve() for path in args.trace_allow]
    args.extra_input_pairs = extra_inputs
    if args.strace is not None:
        args.strace = args.strace.resolve()
    if args.ir_stdlib is not None:
        args.ir_stdlib = args.ir_stdlib.resolve()
        if not args.ir_stdlib.is_dir():
            parser.error(f"--ir-stdlib is not a directory: {args.ir_stdlib}")
    if args.out.exists() and any(args.out.iterdir()):
        parser.error("output directory is not empty; refusing to overwrite "
                     f"evidence: {args.out}")
    args.out.mkdir(parents=True, exist_ok=True)

    corpus = load_public_corpus(args.root)
    ctx = make_public_context(args, corpus)
    inputs = public_input_paths(args)
    before = snapshot_inputs(inputs)
    snapshot_before = {label: (root, tree_snapshot(root))
                       for label, root in snapshot_roots}
    snapshot_records_before = write_snapshots(args.out, snapshot_before,
                                              "before")
    options = args.opt or [0, 2]
    failures = []
    summary = {
        "schema": PUBLIC_SCHEMA,
        "status": "failed",
        "role": args.role,
        "scope": ("the ten positive async parameter copies plus the "
                  "resource-copy rejection; Source built and observed "
                  "through this role's public program.o"
                  + ("; param-string-await uses an explicit test-only "
                     "Nativeobs driver for the original before.ll marker"
                     if needs_ir else "")),
        "selection": selected,
        "options": options,
        "cases": {},
        "limits": [
            "Each command is limited by the runner timeout; readelf uses "
            "min(timeout, 60).",
            "The observer is the unchanged test-only observe_gc.cpp linked "
            "with this role's program.o and matching runtime archive.",
            ("param-string-await's before.ll comes from the explicit "
             "test-only Nativeobs driver; the driver is not a product CLI "
             "and does not publish program or program.o."
             if needs_ir else
             "param-string-await is not selected; its private before.ll "
             "assertion was not run in this invocation."),
            "IR, observer and static-ELF evidence are runner-only; the "
            "generic corpus schema does not express them.",
        ],
        "resources": {
            "cli": str(args.cli),
            "cli_sha256": MEMORY.digest(args.cli),
            "runtime": str(args.runtime),
            "runtime_sha256": MEMORY.digest(args.runtime),
            "observer": str(args.observer),
            "observer_sha256": MEMORY.digest(args.observer),
            "vec_header": str(args.root / "runtime/native/vec.hpp"),
            "vec_header_sha256": MEMORY.digest(
                args.root / "runtime/native/vec.hpp"),
            "clang": str(args.clang),
            "ld_lld": str(args.toolchain / "ld.lld"),
            "readelf": str(args.readelf),
        },
        "ir": ({
            "driver": str(args.ir_driver),
            "driver_sha256": MEMORY.digest(args.ir_driver),
            "driver_sha256_expected": args.ir_driver_sha256,
            "stdlib": str(args.ir_stdlib),
            "stdlib_tree_sha256": tree_digest(args.ir_stdlib),
            "emits": [],
        } if needs_ir else None),
        "extra_inputs": {
            label: {"path": str(path), "sha256": MEMORY.digest(path)}
            for label, path in extra_inputs
        },
        "snapshots": {
            label: {"root": str(root),
                    "before": snapshot_records_before[label]}
            for label, root in snapshot_roots
        },
        "hashes": {},
    }
    if args.resource_root is not None:
        resource_root = args.resource_root.resolve()
        summary["resources"]["resource_root"] = str(resource_root)
        summary["resources"]["stdlib_tree_sha256"] = tree_digest(
            resource_root / "stdlib")

    error_text = None
    try:
        for case_id in selected:
            case = CASE_BY_ID.get(case_id) or REJECT_BY_ID[case_id]
            summary["cases"][case_id] = {"O0": {}, "O2": {}}
            case_ok = True
            runner = public_positive if case_id in CASE_BY_ID else public_reject
            for opt in options:
                try:
                    result = runner(case, opt, args, ctx)
                    summary["cases"][case_id][f"O{opt}"] = result
                    if result.get("ir"):
                        summary["ir"]["emits"].append({
                            "case": case_id,
                            "opt": opt,
                            "before": result["ir"]["before"],
                            "native": result["ir"]["native"],
                            "driver_sha256": result["ir"]["driver_sha256"],
                            "metadata": result["ir"]["metadata"],
                        })
                except Exception as error:  # noqa: BLE001 - keep evidence
                    summary["cases"][case_id][f"O{opt}"] = {
                        "status": "failed", "error": str(error)}
                    failures.append({"id": case_id, "opt": opt,
                                     "error": str(error)})
                    case_ok = False
                summary["cases"][case_id]["status"] = (
                    "current" if case_ok else "failed")
                write_case_record(args.out, case_id,
                                  summary["cases"][case_id])
            summary["cases"][case_id]["status"] = (
                "passed" if case_ok else "failed")
            write_case_record(args.out, case_id, summary["cases"][case_id])
        after = snapshot_inputs(inputs)
        summary["inputs"] = {
            "before": before,
            "after": after,
            "unchanged": before == after,
            "paths": [key for key, _ in inputs],
        }
        if before != after:
            changed = sorted(key for key in before
                             if before[key] != after.get(key))
            failures.append({"error": "input bytes changed during the run: "
                             + ", ".join(changed)})
        if args.proposal_out is not None:
            write_proposal(args.proposal_out.resolve(), summary, selected,
                           options)
    except Exception as error:  # noqa: BLE001 - always write the summary
        error_text = str(error)
        failures.append({"error": error_text})
    finally:
        if args.resource_root is not None:
            try:
                record_stdlib_after(resource_root, summary["resources"])
            except Exception as error:  # noqa: BLE001 - preserve the summary
                failures.append({"error": str(error)})
        if snapshot_before:
            try:
                snapshot_after = {label: (root, tree_snapshot(root))
                                  for label, root in snapshot_roots}
                snapshot_records_after = write_snapshots(
                    args.out, snapshot_after, "after")
                for label in snapshot_before:
                    before_map = snapshot_before[label][1]
                    after_map = snapshot_after[label][1]
                    unchanged = before_map == after_map
                    summary["snapshots"][label]["after"] = (
                        snapshot_records_after[label])
                    summary["snapshots"][label]["unchanged"] = unchanged
                    if not unchanged:
                        failures.append({
                            "error": f"snapshot root changed during the run: "
                                     f"{label}"})
            except Exception as error:  # noqa: BLE001
                failures.append({"error": f"snapshot failure: {error}"})
        summary["counts"] = command_counts(ctx["commands"])
        summary["status"] = (
            "passed" if not failures and error_text is None else "failed")
        summary["commands"] = ctx["commands"]
        summary["failures"] = failures
        summary["hashes"] = ctx["hashes"]
        (args.out / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        (args.out / "commands.json").write_text(
            json.dumps(ctx["commands"], indent=2) + "\n", encoding="utf-8")
        print(json.dumps({
            "status": summary["status"],
            "role": args.role,
            "passed": sum(1 for case_id in selected
                          for opt in options
                          if summary["cases"].get(case_id, {})
                          .get(f"O{opt}", {}).get("status") == "passed"),
            "expected": len(selected) * len(options),
            "failed": len(failures),
            "failures": failures,
            "counts": summary["counts"],
            "out": str(args.out),
        }))
    return 0 if summary["status"] == "passed" else 1


def build_and_check(case, opt, args, env, commands, hashes):
    case_id = case["id"]
    directory = args.out / case_id / f"O{opt}"
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    manifest = HERE / "fixtures" / case_id / "lang.toml"
    need(manifest.is_file(), f"missing fixture manifest: {manifest}")

    build = invoke([
        args.cli, "build", "--project", manifest, "--backend", "native",
        "--opt", str(opt), "--out-dir", directory / "cli",
        "--format", "json",
    ], args.timeout, env)
    commands.append(build)
    need(not build.get("timeout") and build["exit"] == 0,
         f"{case_id} O{opt} CLI build failed: {build}")
    payload = json.loads(build["stdout"])
    need(payload.get("status") == "ok", f"{case_id} O{opt}: {payload}")
    need(not any(diagnostic.get("code") == "E_CAPABILITY_UNAVAILABLE"
                 for diagnostic in payload.get("diagnostics", [])),
         f"{case_id} O{opt} silently fell back")

    program = directory / "cli" / "program"
    need(program.is_file(), f"{case_id} O{opt} emitted no program")
    executed = invoke([program], args.timeout, env)
    commands.append(executed)
    need(not executed.get("timeout") and executed["exit"] == 0,
         f"{case_id} O{opt} execution failed: {executed}")
    need(executed["stdout"] == case["stdout"],
         f"{case_id} O{opt} stdout mismatch: {executed}")
    need(executed["stderr"] == "",
         f"{case_id} O{opt} unexpected stderr: {executed}")
    elf_check = check_static_elf(program, args.readelf, args.timeout, env,
                                 commands)

    ir_dir = directory / "ir"
    ir_dir.mkdir()
    emit = invoke([
        args.helper, "--project", manifest, "--opt", str(opt),
        "--out-object", ir_dir / f"O{opt}.o",
        "--out-llvm", ir_dir / f"O{opt}.after.ll",
        "--out-llvm-before", ir_dir / f"O{opt}.before.ll",
        "--out-native", ir_dir / f"O{opt}.nir",
        "--format", "json",
    ], args.timeout, env)
    commands.append(emit)
    need(not emit.get("timeout") and emit["exit"] == 0,
         f"{case_id} O{opt} helper emit failed: {emit}")
    emitted = json.loads(emit["stdout"])
    need(emitted.get("status") == "passed",
         f"{case_id} O{opt} helper: {emitted}")
    llvm_text = (ir_dir / f"O{opt}.before.ll").read_text(encoding="utf-8")
    if case_id == "param-string-await":
        need("lang_text_concat_v1" in llvm_text,
             f"{case_id} O{opt} did not create a dynamic string")

    observe_dir = directory / "observe"
    observe_dir.mkdir()
    linked = invoke([
        args.clang, f"-O{opt}", "-std=c++20",
        ir_dir / f"O{opt}.before.ll", args.observer, args.runtime,
        "-o", observe_dir / "observe", "-nostdlib", "-static",
        "-Wl,--wrap=lang_alloc_v1", "-Wl,--wrap=lang_free_v1",
    ], args.timeout, env)
    commands.append(linked)
    need(not linked.get("timeout") and linked["exit"] == 0,
         f"{case_id} O{opt} observer link failed: {linked}")
    observer_elf = check_static_elf(observe_dir / "observe", args.readelf,
                                    args.timeout, env, commands)

    observed = invoke([observe_dir / "observe"], args.timeout, env)
    commands.append(observed)
    need(not observed.get("timeout") and observed["exit"] == 0,
         f"{case_id} O{opt} observer execution failed: {observed}")
    need(observed["stdout"] == case["stdout"],
         f"{case_id} O{opt} observer stdout mismatch: {observed}")
    need("LANG_NATIVE_PANIC" not in observed["stderr"],
         f"{case_id} O{opt} panicked: {observed['stderr']}")
    metrics = parse_observer(observed["stderr"])

    need(metrics["final_live_heap_count"] == 0,
         f"{case_id} O{opt} leaked heap allocations: {metrics}")
    need(metrics["final_live_heap_bytes"] == 0,
         f"{case_id} O{opt} leaked heap bytes: {metrics}")
    need(metrics["heap_allocations"] == metrics["heap_frees"],
         f"{case_id} O{opt} unbalanced heap allocations: {metrics}")
    need(metrics["vec_allocations"] == metrics["vec_frees"],
         f"{case_id} O{opt} unbalanced Vec count: {metrics}")
    need(metrics["live_vecs_at_exit"] == 0,
         f"{case_id} O{opt} leaked Vecs: {metrics}")
    need(metrics["final_live_payload_bytes"] == 0,
         f"{case_id} O{opt} leaked Vec payload: {metrics}")
    need(metrics["heap_allocations"] >= case["min_heap_allocations"],
         f"{case_id} O{opt} did not allocate the object set: {metrics}")
    need(metrics["vec_allocations"] >= case["min_vec_allocations"],
         f"{case_id} O{opt} did not allocate the Vec set: {metrics}")

    for path in (program, directory / "cli" / "program.o",
                 ir_dir / f"O{opt}.nir",
                 ir_dir / f"O{opt}.before.ll",
                 ir_dir / f"O{opt}.after.ll",
                 ir_dir / f"O{opt}.o",
                 observe_dir / "observe"):
        if path.is_file():
            record_hash(hashes, args.out, path)

    return {
        "status": "passed",
        "purpose": case["purpose"],
        "metrics": metrics,
        "elf": elf_check,
        "observer_elf": observer_elf,
        "program_sha256": digest(program),
        "observer_sha256": digest(observe_dir / "observe"),
    }


def reject_check(case, opt, args, env, commands, hashes):
    case_id = case["id"]
    directory = args.out / case_id / f"O{opt}"
    shutil.rmtree(directory, ignore_errors=True)
    directory.mkdir(parents=True)
    manifest = HERE / "fixtures" / case_id / "lang.toml"
    need(manifest.is_file(), f"missing fixture manifest: {manifest}")

    build = invoke([
        args.cli, "build", "--project", manifest, "--backend", "native",
        "--opt", str(opt), "--out-dir", directory / "cli",
        "--format", "json",
    ], args.timeout, env)
    commands.append(build)
    need(not build.get("timeout"), f"{case_id} O{opt} build timed out")
    need(build["exit"] != 0, f"{case_id} O{opt} was not rejected: {build}")
    payload = json.loads(build["stdout"])
    need(payload.get("status") == "error", f"{case_id} O{opt}: {payload}")
    diagnostics = payload.get("diagnostics", [])
    need(any(diagnostic.get("code") == case["code"] and
             any(message in diagnostic.get("message", "")
                 for message in case["messages"])
             for diagnostic in diagnostics),
         f"{case_id} O{opt} rejected with the wrong diagnostic: {diagnostics}")
    for output in ("program", "program.o"):
        need(not (directory / "cli" / output).exists(),
             f"{case_id} O{opt} emitted {output} despite rejection")
    for path in (manifest, HERE / "fixtures" / case_id / "binding.json"):
        record_hash(hashes, args.root, path)
    return {
        "status": "passed",
        "purpose": case["purpose"],
        "code": case["code"],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--helper", type=Path)
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--toolchain", type=Path,
                        help="LLVM tools directory containing clang and llvm-readelf")
    parser.add_argument("--root", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--clang", type=Path, default=None)
    parser.add_argument("--readelf", type=Path, default=None)
    parser.add_argument("--libxml2-lib", type=Path, default=None)
    parser.add_argument("--only", action="append",
                        choices=tuple(CASE_BY_ID) + tuple(REJECT_BY_ID))
    parser.add_argument("--opt", action="append", type=int, choices=(0, 2))
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--public-object", action="store_true",
                        help="build Source only with this role's public CLI "
                             "and relink the observer from its program.o "
                             "(the ten migrated non-string cases)")
    parser.add_argument("--role", choices=("reference", "candidate"),
                        help="required with --public-object for trace policy")
    parser.add_argument("--strace", type=Path, default=None,
                        help="record and whitelist-audit Source builds")
    parser.add_argument("--trace-allow", action="append", type=Path,
                        default=[],
                        help="extra executable permitted by the Source "
                             "trace; reference role only, for its own "
                             "compiled-in stage0 helper")
    parser.add_argument("--resource-root", type=Path, default=None,
                        help="role resource root whose stdlib tree is hashed")
    parser.add_argument("--proposal-out", type=Path, default=None,
                        help="write the eleven existing case ids as a proposal")
    parser.add_argument("--ir-driver", type=Path, default=None,
                        help="explicit test-only Nativeobs driver used only "
                             "for param-string-await before.ll; not a product "
                             "CLI and not the public build")
    parser.add_argument("--ir-driver-sha256", default=None,
                        help="optional exact SHA-256 pin for --ir-driver")
    parser.add_argument("--ir-stdlib", type=Path, default=None,
                        help="stdlib tree passed to --ir-driver")
    parser.add_argument("--snapshot-root", action="append", default=[],
                        metavar="LABEL=PATH",
                        help="record a real per-file SHA map of this tree "
                             "before and after the run")
    parser.add_argument("--extra-input", action="append", default=[],
                        metavar="LABEL=PATH",
                        help="record one additional input file SHA before "
                             "and after the run")
    parser.add_argument("--self-test", action="store_true",
                        help="run short negative controls for --public-object")
    args = parser.parse_args()

    if args.self_test:
        return public_self_test(args)

    if args.public_object:
        return main_public(args, parser)

    public_only = ("ir_driver", "ir_driver_sha256", "ir_stdlib",
                   "snapshot_root", "extra_input")
    leftover = [name for name in public_only if getattr(args, name)]
    if leftover:
        parser.error("these options require --public-object: "
                     + ", ".join("--" + name.replace("_", "-")
                                 for name in leftover))

    required = ("cli", "helper", "runtime", "toolchain", "root", "out")
    missing = [name for name in required if getattr(args, name) is None]
    if missing:
        parser.error("missing required arguments: " + ", ".join(missing))

    args.root = args.root.resolve()
    args.out = args.out.resolve()
    args.cli = args.cli.resolve()
    args.helper = args.helper.resolve()
    args.runtime = args.runtime.resolve()
    args.toolchain = args.toolchain.resolve()
    args.clang = (args.clang or args.toolchain / "clang").resolve()
    args.readelf = (args.readelf or args.toolchain / "llvm-readelf").resolve()
    args.observer = (args.root / OBSERVER).resolve()
    args.out.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    if args.libxml2_lib:
        previous = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = str(args.libxml2_lib) + (
            ":" + previous if previous else "")

    selected = set(args.only) if args.only else (
        set(CASE_BY_ID) | set(REJECT_BY_ID))
    options = args.opt or [0, 2]
    commands = []
    hashes = {}
    summary = {
        "schema": "woozy/native-async-parameter-copy/1",
        "status": "failed",
        "scope": "ordinary async owning parameters copied into the Task frame",
        "cases": {},
        "limits": [
            "Each fixture is limited by the runner timeout.",
            "The observer covers lang_alloc_v1/lang_free_v1 and Vec events; "
            "source output is checked separately.",
        ],
        "hashes": hashes,
    }
    failures = []
    for case in CASES:
        if case["id"] not in selected:
            continue
        summary["cases"][case["id"]] = {"O0": {}, "O2": {}}
        case_ok = True
        for opt in options:
            try:
                summary["cases"][case["id"]][f"O{opt}"] = build_and_check(
                    case, opt, args, env, commands, hashes)
            except Exception as error:
                summary["cases"][case["id"]][f"O{opt}"] = {
                    "status": "failed", "error": str(error)}
                failures.append({"id": case["id"], "opt": opt,
                                 "error": str(error)})
                case_ok = False
        summary["cases"][case["id"]]["status"] = (
            "passed" if case_ok else "failed")

    for case in REJECTS:
        if case["id"] not in selected:
            continue
        summary["cases"][case["id"]] = {"O0": {}, "O2": {}}
        case_ok = True
        for opt in options:
            try:
                summary["cases"][case["id"]][f"O{opt}"] = reject_check(
                    case, opt, args, env, commands, hashes)
            except Exception as error:
                summary["cases"][case["id"]][f"O{opt}"] = {
                    "status": "failed", "error": str(error)}
                failures.append({"id": case["id"], "opt": opt,
                                 "error": str(error)})
                case_ok = False
        summary["cases"][case["id"]]["status"] = (
            "passed" if case_ok else "failed")

    for path in (HERE / "run_parameter_copy.py", args.observer):
        if path.is_file():
            record_hash(hashes, args.root, path)
    for fixture in sorted((HERE / "fixtures").iterdir()):
        for path in sorted(fixture.rglob("*")):
            if path.is_file():
                record_hash(hashes, args.root, path)

    summary["status"] = "passed" if not failures else "failed"
    summary["commands"] = commands
    summary["failures"] = failures
    (args.out / "summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    (args.out / "commands.json").write_text(
        json.dumps(commands, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "status": summary["status"],
        "passed": len(selected) * len(options) - len(failures),
        "failed": len(failures),
        "failures": failures,
    }))
    return 0 if summary["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
