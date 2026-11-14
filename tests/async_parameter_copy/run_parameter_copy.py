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
# The tenth case stays out of this mode: its own assertion reads the
# pre-optimization ``lang_text_concat_v1`` text from before.ll, which a
# published object cannot prove.  ``param-string-await`` is pending, not passed.
PUBLIC_POSITIVE_IDS = tuple(
    case["id"] for case in CASES if case["id"] != "param-string-await")
PUBLIC_REJECT_IDS = tuple(case["id"] for case in REJECTS)
PUBLIC_CASE_IDS = PUBLIC_POSITIVE_IDS + PUBLIC_REJECT_IDS
PUBLIC_PENDING_IDS = ("param-string-await",)
LLVM_TRACE_TOOLS = ("clang", "clang++", "ld.lld", "opt", "llc")
PUBLIC_SCHEMA = "woozy/native-async-parameter-copy-public-object/1"


def load_public_corpus(root: Path):
    """Load the existing bounded process runner from the given Root."""
    path = root / "selfhost/corpus.py"
    need(path.is_file(), f"missing corpus driver: {path}")
    spec = importlib.util.spec_from_file_location("woozy_public_corpus", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def public_selection(only):
    selected = list(dict.fromkeys(only or ()))
    need(bool(selected),
         "public-object selection is empty; refusing a vacuous pass")
    pending = sorted(set(selected) & set(PUBLIC_PENDING_IDS))
    need(not pending,
         "param-string-await keeps its private before.ll lang_text_concat_v1 "
         "assertion and is not migrated: " + ", ".join(pending))
    unknown = sorted(set(selected) - set(PUBLIC_CASE_IDS))
    need(not unknown, "not migrated to public objects: " + ", ".join(unknown))
    return selected


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
    return {
        "corpus": corpus,
        "env": env,
        "cwd": args.root,
        "strace": args.strace,
        "role": args.role,
        "allowed_executables": allowed,
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
        "timed_out": result["timed_out"],
        "cleanup_complete": result["cleanup_complete"],
        "abandoned_pipes": result["abandoned_pipes"],
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
    record = result["record"]
    need(not record["timed_out"], f"{label} timed out")
    need(record["exit"] is not None and record["exit"] >= 0,
         f"{label} terminated by signal: exit {record['exit']}")
    need(record["cleanup_complete"], f"{label} cleanup incomplete")
    need(not record["abandoned_pipes"], f"{label} left abandoned pipes")
    return record


def audit_public_trace(ctx, trace_path: Path, label):
    """Fail closed unless the Source compile ran only the allowed programs."""
    corpus = ctx["corpus"]
    try:
        observed = corpus.parse_trace(trace_path)
    except corpus.TraceError as error:
        raise RuntimeError(f"{label} Source trace unparseable: {error}")
    unexpected = sorted(set(observed) - set(ctx["allowed_executables"]))
    need(not unexpected,
         f"{label} Source trace ran outside the allowed set: {unexpected}")
    if ctx["role"] == "candidate":
        forbidden = [path for path in observed
                     if "helper" in Path(path).name.lower()
                     or "frontend" in Path(path).name.lower()]
        need(not forbidden,
             f"{label} candidate Source used a C++ helper/frontend: {forbidden}")
        policy = "candidate Source compile: this role's CLI + fixed LLVM only"
    else:
        policy = ("reference Source compile: reference CLI + its compiled-in "
                  "stage0 helper + fixed LLVM")
    return {"status": "ok", "observed": sorted(observed),
            "allowed": sorted(ctx["allowed_executables"]),
            "trace_path": evidence_path(trace_path, ctx["out"]),
            "policy": policy}


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
    trace_path = None
    if ctx["strace"]:
        trace_path = directory / "trace" / f"source-build.O{opt}.exec.log"
        trace_path.parent.mkdir()
        build_argv = [ctx["strace"], "--seccomp-bpf", "-f", "-qq",
                      "-s", "16384", "-e", "trace=execve,execveat",
                      "-o", trace_path, *build_argv]
    built = run_command(ctx, build_argv, args.timeout, "source-build",
                        f"{label} public Source build", command_root)
    record = require_clean_command(built, f"{label} public Source build")
    trace = None
    if trace_path is not None:
        trace = audit_public_trace(ctx, trace_path, f"{label} public Source build")
        MEMORY.record_hash(ctx["hashes"], ctx["out"], trace_path)
    return built, record, trace


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

    executed = run_command(ctx, [program], args.timeout, "program-execute",
                           f"{label} program execution", command_root)
    execute_record = require_clean_command(executed, f"{label} program execution")
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
    linked = run_command(ctx, link_argv, args.timeout, "observer-link",
                         f"{label} observer link", command_root)
    link_record = require_clean_command(linked, f"{label} observer link")
    need(link_record["exit"] == 0,
         f"{label} observer link failed: exit {link_record['exit']}")
    observer_elf = public_static_elf(observer, args, ctx, command_root,
                                     f"{label} observer")

    observed = run_command(ctx, [observer], args.timeout, "observer-execute",
                           f"{label} observer execution", command_root)
    observe_record = require_clean_command(observed, f"{label} observer execution")
    metrics = validate_observer_streams(
        observe_record["exit"], text_of(observed["stdout"]),
        text_of(observed["stderr"]), case, opt)

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
                    "exit": execute_record["exit"]},
        "observer_link": {
            "command": link_record["index"],
            "trace_scope": ("test-only clang observer shim link; not part "
                            "of the candidate Source trace whitelist"),
            "program_object": evidence_path(program_object, ctx["out"]),
            "program_object_sha256": MEMORY.digest(program_object),
            "observer_source_sha256": MEMORY.digest(args.observer),
            "runtime_sha256": MEMORY.digest(args.runtime),
        },
        "observer": {"command": observe_record["index"],
                     "exit": observe_record["exit"]},
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
    for name in args.trace_allow:
        paths.append((f"trace-allow/{name.name}", name))
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
    document = {
        "schema": "woozy/native-async-parameter-copy-proposal/1",
        "source": "SH12g async parameter copy public-object run",
        "enabled": False,
        "notes": [
            "Proposal only: update the ten existing case ids; do not add "
            "duplicate cases and do not enable them from this file.",
            "param-string-await stays disabled; its private before.ll "
            "lang_text_concat_v1 assertion is still pending.",
            "The generic corpus schema does not express the observer or "
            "static-ELF evidence recorded here.",
        ],
        "cases": rows,
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


def main_public(args, parser):
    if args.helper is not None:
        parser.error("--public-object does not accept --helper; the observer "
                     "must use this role's published program.o")
    if args.role is None:
        parser.error("--public-object requires --role reference|candidate")
    if args.role == "candidate" and args.trace_allow:
        parser.error("--trace-allow is reference-only; a candidate Source "
                     "trace allows only the CLI and fixed LLVM")
    selected = public_selection(
        args.only if args.only else list(PUBLIC_CASE_IDS))

    required = ("cli", "runtime", "toolchain", "root", "out")
    missing = [name for name in required if getattr(args, name) is None]
    if missing:
        parser.error("missing required arguments: " + ", ".join(missing))

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
    if args.strace is not None:
        args.strace = args.strace.resolve()
    if args.out.exists() and any(args.out.iterdir()):
        parser.error("output directory is not empty; refusing to overwrite "
                     f"evidence: {args.out}")
    args.out.mkdir(parents=True, exist_ok=True)

    corpus = load_public_corpus(args.root)
    ctx = make_public_context(args, corpus)
    inputs = public_input_paths(args)
    before = snapshot_inputs(inputs)
    options = args.opt or [0, 2]
    failures = []
    summary = {
        "schema": PUBLIC_SCHEMA,
        "status": "failed",
        "role": args.role,
        "scope": ("the nine non-string positive async parameter copies plus "
                  "the resource-copy rejection; Source built by this role's "
                  "public CLI and observed through the published program.o"),
        "selection": selected,
        "options": options,
        "cases": {},
        "limits": [
            "Each command is limited by the runner timeout; readelf uses "
            "min(timeout, 60).",
            "The observer is the unchanged test-only observe_gc.cpp linked "
            "with this role's program.o and matching runtime archive.",
            "param-string-await is not run here: its private before.ll "
            "lang_text_concat_v1 assertion is still pending.",
            "This range does not complete the parameter-copy runner or the "
            "three-stage bootstrap.",
            "The generic corpus schema does not express observer or static "
            "ELF assertions.",
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
                    summary["cases"][case_id][f"O{opt}"] = runner(
                        case, opt, args, ctx)
                except Exception as error:  # noqa: BLE001 - keep evidence
                    summary["cases"][case_id][f"O{opt}"] = {
                        "status": "failed", "error": str(error)}
                    failures.append({"id": case_id, "opt": opt,
                                     "error": str(error)})
                    case_ok = False
            summary["cases"][case_id]["status"] = (
                "passed" if case_ok else "failed")
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
                        help="write the ten existing case ids as a proposal")
    parser.add_argument("--self-test", action="store_true",
                        help="run short negative controls for --public-object")
    args = parser.parse_args()

    if args.self_test:
        return public_self_test(args)

    if args.public_object:
        return main_public(args, parser)

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
