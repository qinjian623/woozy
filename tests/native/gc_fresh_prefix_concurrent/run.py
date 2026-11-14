#!/usr/bin/env python3
"""Fresh strong-edge Vec prefix versus a pending world collection request.

The fixture builds one dynamic ``Vec[Vec[Node]]`` of length three from three
just-returned self-cycle ``Vec[Node]`` rings, plus one empty ``Vec[Vec[Node]]``
control.  A test-only C++ shim (``observe_fresh_prefix.cpp``) wraps exactly two
existing entry points:

    lang_vec_new_v1
    lang_gc_program_register_v1

The shim identifies its probe target by allocation generation: the
``lang_vec_new_v1`` return that produced the requested shape selects one
generation of one raw address, and every later constructor return at the same
address retires that generation, whatever shape the new Vec has.  A repeated
registration with no intervening constructor is still the same generation, so
it is counted and rejected instead of being ignored.

At the selected constructor return a real host pthread registers with
lang-gc-world/1 and calls the ordinary ``lang_gc_runtime_collect_v1(5 s)``.
The wrapper waits for the actual world request bit and then returns the
original pointer, so the generated code writes its own prefix while the stop
request is pending.  The shim asserts at the registration call that the same
object now has ``initialized == length``, that the request is still pending and
that the runtime phase is STOPPING (not KERNEL).  Only after the real
registration returns true does it call ``lang_gc_world_poll_v1``; that poll is
the only parking point, and a still-pending request afterwards is a failure
(SAFE/critical/fork-stable contamination or an emitter that already parked).
The collector completion flag wait is bounded by 6 s; ``pthread_join`` has no
separate deadline and is bounded only by the outer program budget, but its
return value is checked.

This runner does not build any component.  The CPP helper, the Wz Nativeobs
entry, the C++ CLI, the hosted runtime archive, strace and the LLVM toolchain
are Root-supplied.  Neither emitter's IR is substituted for the other: each
engine supplies its own pre-optimization LLVM, which is both IR-checked and
linked for the real observation run.

Every real process goes through the existing bounded
``selfhost/corpus.py::_run_process_impl`` transport by way of the existing
``tests/native/gc_critical_emission`` runner helpers.  The fixed
``strace -e trace=execve,execveat`` prefix is added at that one common entry,
and every exec log is audited against the role's own executable allowlist:
Wz emission may execute the Wz driver and nothing else, so a Cpp CLI/helper or
an LLVM tool standing in for the Source frontend is a recorded failure.

Each record is written before its result is asserted: trace label, the logical
argv, the traced argv, the exec log path/SHA and audit, cwd, actual timeout,
the three exact booleans, raw stdout/stderr as hex plus SHA-256, exit, an env
delta, and the SHA-256 of every configured input before and after the command.
The typed/raw process record is durable before the after-input snapshot is
read, so a snapshot error can never discard a command that already ran.  A
changed or unreadable input is a failure.  The output directory must be new or
truly empty; existing evidence is never deleted or overwritten.

Nothing here claims performance, and this is not exhaustive multi-thread
correctness.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
import platform
import re
import struct
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
FIXTURE = HERE / "fixtures" / "fresh-prefix"
MANIFEST = FIXTURE / "lang.toml"
SOURCE = FIXTURE / "src" / "main.wz"
SHIM = HERE / "observe_fresh_prefix.cpp"
GENERATION_CONTROL = HERE / "generation_control.cpp"
PURE_CONTROLS = HERE / "pure_controls.py"
CRITICAL_RUNNER = (ROOT / "tests/native/gc_critical_emission"
                   / "run_gc_critical_emission.py")
# hosted_thread_gc already reuses this observer/parser pair: the allocation
# observer emits CONFIG/ALLOC/FREE/VEC and parse_observer owns that schema.
OBSERVER_RUNNER = ROOT / "tests/native/async_memory/run_async_memory.py"
ALLOCATION_OBSERVER = ROOT / "tests/native/language_gc_source/observe_gc.cpp"
CORPUS_FILE = ROOT / "selfhost/corpus.py"

MODES = ("nonempty", "empty")
ENGINES = ("cpp", "wz")
PROGRAM_TIMEOUT = 60
HANDSHAKE_SECONDS = 2.0
COLLECT_TIMEOUT_NS = 5000000000
COLLECTOR_DONE_SECONDS = 6.0
READELF_TIMEOUT = 60

# The fixed LLVM tools the supplied clang driver may legitimately execute.  They
# are resolved from the supplied --clang directory, frozen as inputs and named
# in the allowlist; nothing is probed for a version.
LLVM_TOOL_NAMES = ("clang", "clang++", "opt", "llc", "ld.lld")

# The one fixed exec-trace prefix.  It is added at the common process entry, so
# every role is traced by the same code path.
TRACE_OPTIONS = ("--seccomp-bpf", "-f", "-qq", "-s", "16384",
                 "-e", "trace=execve,execveat")

# mode -> (expected length, element stride, minimum live registered objects)
MODE_SPEC = {
    "nonempty": {"length": 3, "stride": 8, "min_registry_live": 4,
                 "purpose": "fresh direct prefix of three owned self-cycle Vecs"},
    "empty": {"length": 0, "stride": 8, "min_registry_live": 5,
              "purpose": "empty strong-edge Vec control"},
}

# The prefix interval must contain no safepoint, blocking or abort seam.  The
# reused structural assertion already rejects any non-memcpy call there; this
# explicit list makes the observation legible and is checked separately.
FORBIDDEN_SEAM = (
    "lang_gc_runtime_poll_v1",
    "lang_gc_world_poll_v1",
    "lang_gc_world_poll_required_v1",
    "lang_gc_world_request_pending_v1",
    "lang_gc_world_enter_safe_v1",
    "lang_gc_world_leave_safe_v1",
    "lang_gc_program_enter_safe_v1",
    "lang_gc_program_leave_safe_v1",
    "lang_gc_program_enter_critical_v1",
    "lang_gc_program_leave_critical_v1",
    "lang_gc_world_enter_critical_v1",
    "lang_gc_world_leave_critical_v1",
    "lang_gc_program_callback_enter_v1",
    "lang_gc_program_callback_leave_v1",
    "lang_gc_world_callback_enter_v1",
    "lang_gc_world_callback_leave_v1",
    "lang_os_exit_v1",
    "lang_native_panic_v1",
)

# One constructor call with a length and either an integer stride literal (the
# Cpp emitter) or the canonical Native64 pointer allocation-size expression
# emitted by the Wz text backend:
#   i64 ptrtoint (ptr getelementptr (ptr, ptr null, i32 1) to i64)
VEC_NEW_CALL = re.compile(
    r"@lang_vec_new_v1\(i64\s+(\d+),\s*i64\s+"
    r"(?:(?P<literal>\d+)|"
    r"(?P<pointer>ptrtoint \(ptr getelementptr \(ptr, ptr null, i32 1\) "
    r"to i64\)))"
    r",\s*ptr\s+@")
VEC_NEW_PREFIX = re.compile(r"@lang_vec_new_v1\(i64\s+(\d+),\s*i64\s+(.*)$")
POINTER_STRIDE_TEXT = ("ptrtoint (ptr getelementptr (ptr, ptr null, i32 1) "
                       "to i64)")

# Configured once per run: key -> real file path.  Nothing is filtered by
# ``Path.is_file`` during a snapshot, so a missing input is an error.
INPUT_PATHS = None
STDLIB_ROOT = None
# Command evidence root and index.  Controls may point this at a temp dir.
_COMMANDS = {"root": None, "index": 0}


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    need(spec is not None and spec.loader is not None, f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The sibling runner already owns the reviewed NIR/LLVM parsing, the fresh
# prefix assertion and the one bounded process transport.  Reuse it instead of
# adding another framework.
SUPPORT = load_module(CRITICAL_RUNNER, "gc_fresh_prefix_critical_support")
# The real allocation observer's parser/schema, as used by hosted_thread_gc.
OBSERVER_SUPPORT = load_module(OBSERVER_RUNNER, "gc_fresh_prefix_observer_support")
CORPUS = load_module(CORPUS_FILE, "gc_fresh_prefix_corpus")


class _DuplicateKey(RuntimeError):
    pass


def _pairs(pairs):
    seen = {}
    for key, value in pairs:
        if key in seen:
            raise _DuplicateKey(f"duplicate JSON key {key!r}")
        seen[key] = value
    return seen


def _reject_constant(token):
    raise RuntimeError(f"non-finite JSON constant {token!r} is not allowed")


def _finite_float(token):
    value = float(token)
    need(math.isfinite(value), f"non-finite JSON number {token!r}")
    return value


def strict_json(text, what):
    """Parse protocol JSON, rejecting non-finite constants and duplicate keys."""
    try:
        return json.loads(text, parse_constant=_reject_constant,
                          parse_float=_finite_float,
                          object_pairs_hook=_pairs)
    except (json.JSONDecodeError, _DuplicateKey, RuntimeError, ValueError) as error:
        raise RuntimeError(f"{what}: malformed JSON: {error}") from error


def exact_int(value, what):
    need(type(value) is int,
         f"{what}: expected an exact integer, got {type(value).__name__} "
         f"{value!r}")
    return value


def exact_bool(value, what):
    need(type(value) is bool,
         f"{what}: expected an exact boolean, got {type(value).__name__} "
         f"{value!r}")
    return value


def pointer_stride():
    """The one platform width for which the pointer allocation-size form is 8."""
    system = platform.system()
    machine = platform.machine().lower()
    size = struct.calcsize("P")
    need(system == "Linux" and machine in ("x86_64", "amd64") and size == 8,
         f"canonical pointer stride is only confirmed for Linux x86_64/8 "
         f"bytes, got {system}/{machine}/{size}")
    return size


def digest(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json_atomic(path: Path, value) -> None:
    """Refuse non-finite evidence, then reuse the shared pending+replace."""
    json.dumps(value, allow_nan=False)
    SUPPORT.write_json_atomic(path, value)


def configure_inputs(args, stdlib: Path):
    """Freeze every actual executable/helper/runtime/std/support input once."""
    global INPUT_PATHS, STDLIB_ROOT
    paths = {
        "runner": Path(__file__).resolve(),
        "pure_controls": PURE_CONTROLS,
        "corpus": CORPUS_FILE,
        "critical_runner": CRITICAL_RUNNER,
        "observer_runner": OBSERVER_RUNNER,
        "allocation_observer": ALLOCATION_OBSERVER,
        "shim": SHIM,
        "generation_control": GENERATION_CONTROL,
        "fixture_manifest": MANIFEST,
        "fixture_source": SOURCE,
    }
    for role in ("helper", "wz_nativeobs", "cli", "runtime", "clang",
                 "readelf", "strace"):
        paths[role] = Path(getattr(args, role))
    if getattr(args, "ld", None) is not None:
        paths["ld"] = Path(args.ld)
    for name in LLVM_TOOL_NAMES:
        paths[f"llvm:{name}"] = Path(args.clang).parent / name
    need(stdlib.is_dir(), f"stdlib directory does not exist: {stdlib}")
    stdlib_files = sorted(item for item in stdlib.rglob("*") if item.is_file())
    need(stdlib_files, f"stdlib directory has no files: {stdlib}")
    for item in stdlib_files:
        paths[f"stdlib:{item.relative_to(stdlib)}"] = item
    missing = sorted(key for key, path in paths.items()
                     if not Path(path).is_file())
    need(not missing,
         f"configured inputs are missing or not regular files: {missing}")
    INPUT_PATHS = paths
    STDLIB_ROOT = stdlib
    manifest_text = MANIFEST.read_text()
    need('runtime = "hosted"' in manifest_text,
         "the fixture manifest does not request the hosted runtime")
    return {key: digest(path) for key, path in paths.items()}


def tracked_inputs() -> dict:
    need(INPUT_PATHS is not None,
         "the tracked input set was not configured before use")
    paths = dict(INPUT_PATHS)
    if STDLIB_ROOT is not None:
        paths = {key: path for key, path in paths.items()
                 if not key.startswith("stdlib:")}
        for item in sorted(STDLIB_ROOT.rglob("*")):
            if item.is_file():
                paths[f"stdlib:{item.relative_to(STDLIB_ROOT)}"] = item
    return {key: digest(path) for key, path in paths.items()}


def configure_output(root: Path) -> None:
    _COMMANDS["root"] = Path(root)
    _COMMANDS["index"] = 0


def command_root() -> Path:
    if _COMMANDS["root"] is None:
        _COMMANDS["root"] = Path(
            tempfile.mkdtemp(prefix="gc-fresh-prefix-commands-"))
    return _COMMANDS["root"]


def allowed_executables(role, program=None, observed=None):
    """The exact executable set one role may really execute."""
    inputs = INPUT_PATHS
    if role == "emit-wz":
        # The Wz driver and nothing else: no Cpp CLI/helper and no LLVM tool may
        # stand in for the Wz Source frontend.
        return {str(inputs["wz_nativeobs"].resolve())}
    if role == "program-run":
        need(program is not None, "program-run role needs the built program")
        return {str(Path(program).resolve())}
    if role == "readelf":
        return {str(inputs["readelf"].resolve())}
    if role == "observed":
        need(observed is not None, "observed role needs the linked binary")
        return {str(Path(observed).resolve())}
    llvm = {str(inputs[f"llvm:{name}"].resolve()) for name in LLVM_TOOL_NAMES}
    ld = inputs.get("ld")
    ld_set = {str(ld.resolve())} if ld is not None else set()
    if role == "emit-cpp":
        return {str(inputs["helper"].resolve())} | llvm
    if role == "program-build":
        return ({str(inputs["cli"].resolve()),
                 str(inputs["helper"].resolve())} | llvm | ld_set)
    if role == "link":
        return {str(inputs["clang"].resolve()),
                str(inputs["llvm:ld.lld"].resolve())} | ld_set
    raise RuntimeError(f"unknown process role: {role!r}")


def run(argv, timeout, env, cwd=None, trace="", sink=None, env_delta=None,
        role=None, program=None, observed=None):
    """Record one traced bounded command completely.

    The typed/raw process record is handed to ``sink`` before the after-input
    snapshot is read, so a later snapshot error keeps the executed command.
    The exec log is audited and its result is recorded before any assertion.
    """
    need(role is not None, f"{trace}: a process role is required")
    argv = [str(part) for part in argv]
    root = command_root()
    index = _COMMANDS["index"]
    _COMMANDS["index"] = index + 1
    safe = re.sub(r"[^A-Za-z0-9._-]+", "_", trace) or "command"
    directory = root / "commands" / f"{index:02d}-{safe}"
    directory.mkdir(parents=True, exist_ok=False)
    log = directory / "exec.log"
    allowed = allowed_executables(role, program=program, observed=observed)
    before = tracked_inputs()
    traced = [str(INPUT_PATHS["strace"]), *TRACE_OPTIONS, "-o", str(log),
              *argv]
    row = SUPPORT.run(traced, timeout, env, cwd=cwd, trace=trace, sink=None)
    row["argv"] = argv
    row["traced_argv"] = traced
    row["command_index"] = index
    row["role"] = role
    row["exec_log"] = str(log)
    row["allowed"] = sorted(allowed)
    row["env_delta"] = dict(env_delta or {})
    row["inputs_before"] = before
    row["inputs_after"] = None
    row["inputs_after_error"] = None
    row["inputs_unchanged"] = False
    row["exec_audit"] = None
    row["exec_log_sha256"] = None
    if sink is not None:
        # Typed/raw result of the real process is durable from here on.
        sink(row)
    try:
        after = tracked_inputs()
    except OSError as error:
        row["inputs_after_error"] = f"{type(error).__name__}: {error}"
        if sink is not None:
            sink(row)
        raise RuntimeError(
            f"{trace}: after-input snapshot failed after the command "
            f"completed: {row['inputs_after_error']}") from error
    row["inputs_after"] = after
    row["inputs_unchanged"] = before == after
    if sink is not None:
        sink(row)

    if row.get("launch_failed"):
        row["exec_audit"] = {"status": "not_traced",
                             "error": f"launch failed: {row.get('launch_error')}",
                             "allowed": sorted(allowed)}
    elif not log.is_file():
        row["exec_audit"] = {"status": "failed",
                             "error": f"strace log is missing: {log}",
                             "allowed": sorted(allowed)}
    else:
        row["exec_log_sha256"] = digest(log)
        try:
            audit = CORPUS.audit_trace(log, sorted(allowed))
        except Exception as error:  # recorded, then rejected by strict()
            row["exec_audit"] = {"status": "failed",
                                 "error": f"{type(error).__name__}: {error}",
                                 "allowed": sorted(allowed)}
        else:
            row["exec_audit"] = {"status": "ok",
                                 "observed": audit["observed"],
                                 "allowed": sorted(allowed)}
    if sink is not None:
        sink(row)
    need(row["exec_audit"]["status"] == "ok",
         f"{trace}: OS exec audit rejected the command: {row['exec_audit']}")
    return row


def strict(row, what):
    SUPPORT._strict_process(row, what)
    audit = row.get("exec_audit") or {}
    need(audit.get("status") == "ok",
         f"{what}: OS exec audit is not ok: {audit}")
    need(row.get("inputs_unchanged") is True,
         f"{what}: tracked inputs changed during the command: "
         f"{row.get('inputs_before')} -> {row.get('inputs_after')} "
         f"({row.get('inputs_after_error')})")
    return row


def ok(row, what):
    strict(row, what)
    need(row["exit"] == 0, f"{what}: exited {row['exit']}")
    return row


def assert_execution_completed(row, what):
    SUPPORT.assert_execution_completed(row, what)
    strict(row, what)
    return row


# ---------------------------------------------------------------------------
# Fresh prefix IR assertions


def assert_target_stride(label, lines, length):
    matches = []
    unrecognized = []
    for index, line in enumerate(lines):
        match = VEC_NEW_CALL.search(line)
        if match:
            literal = match.group("literal")
            pointer = match.group("pointer")
            if literal is not None:
                matches.append((index, int(match.group(1)), int(literal),
                                "literal"))
            else:
                need(pointer == POINTER_STRIDE_TEXT,
                     f"{label}: unexpected pointer stride text {pointer!r}")
                matches.append((index, int(match.group(1)), pointer_stride(),
                                "pointer-allocation-size"))
            continue
        prefix = VEC_NEW_PREFIX.search(line)
        if prefix:
            unrecognized.append({"line": index, "length": int(prefix.group(1)),
                                 "stride_text": prefix.group(2)[:160]})
    need(not unrecognized,
         f"{label}: unrecognized lang_vec_new_v1 element stride expression: "
         f"{unrecognized}")
    need(len(matches) == 1,
         f"{label}: expected one lang_vec_new_v1 call, found {matches}")
    _index, got_length, got_stride, form = matches[0]
    need(got_length == length,
         f"{label}: lang_vec_new_v1 length is {got_length}, expected {length}")
    # Every element of Vec[Vec[Node]] is one owned pointer.  The unique
    # (length, pointer-stride) pair is the identity the shim selects.  The Wz
    # text backend prints the canonical pointer allocation-size expression,
    # whose value on the confirmed Linux x86_64 target is 8.
    need(got_stride == pointer_stride(),
         f"{label}: element stride is {got_stride}, expected one pointer "
         f"({pointer_stride()}) from a {form} expression")
    return got_stride


def assert_no_forbidden_seam(label, lines):
    new_calls = SUPPORT.call_indices(lines, "lang_vec_new_v1")
    register_calls = SUPPORT.call_indices(lines, "lang_gc_program_register_v1")
    need(len(new_calls) == 1,
         f"{label}: expected one constructor call, found {new_calls}")
    need(len(register_calls) == 1,
         f"{label}: expected one registration call, found {register_calls}")
    start, end = new_calls[0], register_calls[0]
    need(start < end,
         f"{label}: registration precedes the constructor at {start}>{end}")
    found = []
    for index in range(start, end + 1):
        for symbol in FORBIDDEN_SEAM:
            if f"@{symbol}(" in lines[index]:
                found.append({"line": index, "symbol": symbol})
    need(not found,
         f"{label}: forbidden seam call inside the fresh prefix: {found}")
    return {"start_line": start, "end_line": end,
            "checked_symbols": list(FORBIDDEN_SEAM)}


def check_engine(label, emitted, directory, source):
    payload = strict_json(emitted["stdout"]["text"],
                          f"{label}: emitter payload")
    need(payload.get("status") == "passed",
         f"{label}: emitter did not pass: {emitted}")
    entry = payload.get("entry_symbol")
    need(type(entry) is int, f"{label}: emitter omitted entry_symbol")
    nir = SUPPORT.parse_nir((directory / "program.nir").read_text())
    llvm = SUPPORT.parse_llvm((directory / "before.ll").read_text())
    need(entry in nir["functions"],
         f"{label}: entry SymbolId {entry} is absent from NIR")
    snapshot = SUPPORT.fixture_snapshot(nir, entry)
    ranges = SUPPORT.scan_source_functions(source.read_bytes())
    emitter = label.split("-")[0]
    facts = {}
    seen = set()
    for name, length in (("build_rings", 3), ("build_empty", 0)):
        function = SUPPORT.map_source_target(nir, entry, snapshot, ranges, name)
        need(function["symbol"] not in seen,
             f"{label}: {name} reuses SymbolId {function['symbol']}")
        seen.add(function["symbol"])
        llvm_name = f"lang.fn.{function['symbol']}"
        need(llvm_name in llvm,
             f"{label}: LLVM function {llvm_name} for {name} is missing")
        lines = llvm[llvm_name]
        function = dict(function, llvm_emitter=emitter)
        facts[name] = {
            "symbol": function["symbol"],
            "prefix": SUPPORT.assert_prefix_case(
                f"{label}:{name}", {"registered": True, "elements": length},
                function, lines),
            "stride": assert_target_stride(f"{label}:{name}", lines, length),
            "seam": assert_no_forbidden_seam(f"{label}:{name}", lines),
        }
    need(facts, f"{label}: no target matched; refusing a zero-target pass")
    return {"payload": payload, "facts": facts,
            "before_llvm_sha256": digest(directory / "before.ll"),
            "native_ir_sha256": digest(directory / "program.nir")}


# ---------------------------------------------------------------------------
# Shim report parsing and assertions


def parse_shim(text):
    parsed = {"handshake": [], "counts": [], "failures": []}
    for line in text.splitlines():
        if line.startswith("FRESH_PREFIX_FAIL:"):
            parsed["failures"].append(
                strict_json(line[len("FRESH_PREFIX_FAIL:"):],
                            "FRESH_PREFIX_FAIL record"))
        elif line.startswith("FRESH_PREFIX_COUNTS:"):
            parsed["counts"].append(
                strict_json(line[len("FRESH_PREFIX_COUNTS:"):],
                            "FRESH_PREFIX_COUNTS record"))
        elif line.startswith("FRESH_PREFIX:"):
            parsed["handshake"].append(
                strict_json(line[len("FRESH_PREFIX:"):],
                            "FRESH_PREFIX record"))
    return parsed


def split_shim_lines(text):
    """Return (shim text, remaining allocation-observer text)."""
    shim, rest = [], []
    for line in text.splitlines():
        if line.startswith("FRESH_PREFIX"):
            shim.append(line)
        else:
            rest.append(line)
    return "\n".join(shim), "\n".join(rest)


HANDSHAKE_INTS = (
    "target_length", "target_stride", "init_at_new", "init_at_register",
    "refs_at_new", "refs_at_register", "can_collect_at_new",
    "phase_at_new_return", "request_at_new_return",
    "collector_done_at_new_return", "phase_at_register",
    "collector_busy_at_register", "request_at_register",
    "collector_done_at_register", "request_after_poll", "phase_after_poll",
    "collector_status", "target_refs_after_collection", "target_new_hits",
    "target_register_hits",
)
HANDSHAKE_BOOLS = (
    "markers_ok_at_register", "self_cycles_at_register",
    "real_register_true", "alive_after_collection",
    "markers_after_collection", "self_cycles_after_collection",
    "collector_joined",
)
STATS_FIELDS = ("registered", "traced_edges", "reachable", "candidates",
                "pinned", "cleared", "collected", "registry_live_after")
COUNTS_INTS = ("nonempty_new_hits", "nonempty_register_hits", "empty_new_hits",
               "empty_register_hits", "target_new_hits", "target_register_hits")
COUNTS_BOOLS = ("handshake_completed", "collector_joined")


def assert_handshake(label, mode, parsed):
    """Assert one observed fresh-prefix/collection handshake record.

    ``parsed`` is the already-parsed structure, so controls can feed records
    without any text or source scanning.  Every numeric protocol field must be
    an exact integer (a boolean is not a number) and every flag an exact bool.
    """
    spec = MODE_SPEC[mode]
    if parsed["failures"]:
        raise RuntimeError(
            f"{label}: shim-failure: {parsed['failures']}")
    need(len(parsed["handshake"]) == 1,
         f"{label}: report-count: expected one handshake record, found "
         f"{len(parsed['handshake'])}")
    need(len(parsed["counts"]) == 1,
         f"{label}: report-count: expected one counts record, found "
         f"{len(parsed['counts'])}")
    record = parsed["handshake"][0]
    need(isinstance(record, dict), f"{label}: handshake record is not an object")
    for name in HANDSHAKE_INTS:
        exact_int(record.get(name), f"{label}: {name}")
    for name in HANDSHAKE_BOOLS:
        exact_bool(record.get(name), f"{label}: {name}")
    need(record.get("schema") == "woozy/gc-fresh-prefix/handshake/1",
         f"{label}: schema: {record.get('schema')!r}")
    need(record.get("mode") == mode,
         f"{label}: mode: {record.get('mode')!r} != {mode!r}")

    need(record["target_length"] == spec["length"] and
         record["target_stride"] == spec["stride"],
         f"{label}: target-identity: length/stride "
         f"{record['target_length']}/{record['target_stride']} != "
         f"{spec['length']}/{spec['stride']}")

    # init must go 0 -> length across the generated prefix writes.
    need(record["init_at_new"] == 0,
         f"{label}: init-prefix: init_at_new is "
         f"{record['init_at_new']}, expected 0")
    need(record["init_at_register"] == spec["length"],
         f"{label}: init-prefix: init_at_register is "
         f"{record['init_at_register']}, expected {spec['length']}")

    # The probe must not retain or modify the fresh object.
    need(record["refs_at_new"] == 1 and record["refs_at_register"] == 1,
         f"{label}: target-refs: refs {record['refs_at_new']} -> "
         f"{record['refs_at_register']}, expected 1 -> 1")

    # Before any request the mutator is registered, ACTIVE and outside both
    # SAFE and critical (can_collect is false in all of those).
    need(record["can_collect_at_new"] == 1,
         f"{label}: preexisting-request: can_collect_at_new is "
         f"{record['can_collect_at_new']} (SAFE/critical/pending stop)")

    # The request must already be visible when the constructor returns and
    # still pending at the generated registration, with the collector running.
    need(record["request_at_new_return"] == 1,
         f"{label}: request-span: request_at_new_return is "
         f"{record['request_at_new_return']}")
    need(record["collector_done_at_new_return"] == 0,
         f"{label}: request-span: collector finished before the constructor "
         f"returned")
    need(record["request_at_register"] == 1,
         f"{label}: request-span: request_at_register is "
         f"{record['request_at_register']}")
    need(record["collector_done_at_register"] == 0,
         f"{label}: request-span: collector finished before registration")

    # The request must not have reached the collection kernel yet.
    need(record["phase_at_new_return"] == 1,
         f"{label}: phase-order: phase_at_new_return is "
         f"{record['phase_at_new_return']}, expected STOPPING (1)")
    need(record["phase_at_register"] == 1,
         f"{label}: phase-order: phase_at_register is "
         f"{record['phase_at_register']}, expected STOPPING (1)")
    need(record["collector_busy_at_register"] == 1,
         f"{label}: phase-order: collector_busy_at_register is "
         f"{record['collector_busy_at_register']}")

    need(record["markers_ok_at_register"] is True and
         record["self_cycles_at_register"] is True,
         f"{label}: prefix-payload: markers "
         f"{record['markers_ok_at_register']} / self-cycles "
         f"{record['self_cycles_at_register']}")

    need(record["real_register_true"] is True,
         f"{label}: real-register: the real registration returned false")

    # Only a poll that actually parked clears the request.
    need(record["request_after_poll"] == 0,
         f"{label}: poll-park: request still pending after the poll "
         f"(SAFE/critical/fork-stable or an earlier park)")
    need(record["phase_after_poll"] == 0,
         f"{label}: post-phase: phase_after_poll is "
         f"{record['phase_after_poll']}, expected IDLE (0)")
    need(record["collector_status"] == 0,
         f"{label}: collector-status: {record['collector_status']}")

    stats = record.get("stats")
    need(isinstance(stats, dict), f"{label}: stats is not an object: {stats!r}")
    for name in STATS_FIELDS:
        exact_int(stats.get(name), f"{label}: stats.{name}")
    need(stats["collected"] >= 1 and stats["candidates"] >= 1,
         f"{label}: kernel-stats: the collection did no reclaim work: {stats}")
    need(stats["registered"] >= 4,
         f"{label}: kernel-stats: snapshot too small: {stats}")
    need(stats["registry_live_after"] >= spec["min_registry_live"],
         f"{label}: kernel-stats: registry_live_after "
         f"{stats['registry_live_after']} < {spec['min_registry_live']}")

    need(record["alive_after_collection"] is True and
         record["markers_after_collection"] is True and
         record["self_cycles_after_collection"] is True,
         f"{label}: alive-after: the target was not readable after the "
         f"collection")
    need(record["target_refs_after_collection"] == 1,
         f"{label}: alive-after: refs after collection is "
         f"{record['target_refs_after_collection']}, expected 1")
    need(record["collector_joined"] is True,
         f"{label}: join: the collector thread was not joined")
    need(record["target_new_hits"] == 1 and
         record["target_register_hits"] == 1,
         f"{label}: selection: target hits new/register "
         f"{record['target_new_hits']}/{record['target_register_hits']}")

    counts = parsed["counts"][0]
    need(isinstance(counts, dict), f"{label}: counts record is not an object")
    for name in COUNTS_INTS:
        exact_int(counts.get(name), f"{label}: counts.{name}")
    for name in COUNTS_BOOLS:
        exact_bool(counts.get(name), f"{label}: counts.{name}")
    need(counts.get("schema") == "woozy/gc-fresh-prefix/counts/1",
         f"{label}: schema: counts {counts.get('schema')!r}")
    need(counts.get("mode") == mode,
         f"{label}: mode: counts {counts.get('mode')!r} != {mode!r}")
    for name in ("nonempty_new_hits", "nonempty_register_hits",
                 "empty_new_hits", "empty_register_hits"):
        need(counts[name] == 1,
             f"{label}: selection: {name} is {counts[name]}, expected 1")
    need(counts["target_new_hits"] == 1 and
         counts["target_register_hits"] == 1,
         f"{label}: selection: final target hits "
         f"{counts['target_new_hits']}/{counts['target_register_hits']}")
    need(counts["handshake_completed"] is True and
         counts["collector_joined"] is True,
         f"{label}: join: handshake/join flags are false")
    return record


OBSERVER_INTS = ("header_bytes", "heap_allocations", "heap_frees",
                 "final_live_heap_count", "final_live_heap_bytes",
                 "vec_allocations", "vec_frees", "live_vecs_at_exit",
                 "peak_live_payload_bytes", "final_live_payload_bytes")


def assert_observer_balance(label, metrics):
    need(isinstance(metrics, dict),
         f"{label}: allocation observer result is not an object")
    for name in OBSERVER_INTS:
        exact_int(metrics.get(name), f"{label}: observer.{name}")
    need(metrics["final_live_heap_count"] == 0,
         f"{label}: leaked heap allocations: {metrics}")
    need(metrics["final_live_heap_bytes"] == 0,
         f"{label}: leaked heap bytes: {metrics}")
    need(metrics["heap_allocations"] == metrics["heap_frees"],
         f"{label}: unbalanced heap allocation/free: {metrics}")
    need(metrics["vec_allocations"] == metrics["vec_frees"],
         f"{label}: unbalanced Vec allocation/free: {metrics}")
    need(metrics["live_vecs_at_exit"] == 0 and
         metrics["final_live_payload_bytes"] == 0,
         f"{label}: leaked Vec payload: {metrics}")
    # Not vacuous: the target, the empty control, three rings and the garbage
    # control really were allocated.
    need(metrics["heap_allocations"] >= 4 and metrics["vec_allocations"] >= 4,
         f"{label}: too few observed allocations: {metrics}")
    return metrics


def parse_link_classes(text, paths):
    """Split one real readelf run into a hosted-link class per input file."""
    sections = {}
    current = None
    for line in text.splitlines():
        if line.startswith("File:"):
            current = line.split(":", 1)[1].strip()
            sections[current] = []
        elif current is not None:
            sections[current].append(line)
    classes = {}
    for path in paths:
        need(str(path) in sections,
             f"readelf did not report {path}; reported {sorted(sections)}")
        body = "\n".join(sections[str(path)])
        classes[str(Path(path).resolve())] = {
            "hosted": "INTERP" in body or "NEEDED" in body,
            "interpreted": "INTERP" in body,
            "dynamic_needed": "NEEDED" in body,
            "has_elf_header": "ELF" in body or "Type:" in body,
        }
    return classes


def _fresh_output_directory(path: Path) -> bool:
    """Same refusal rule as the reused sibling runner: new or truly empty."""
    if not path.exists():
        return True
    if not path.is_dir():
        return False
    try:
        return not any(path.iterdir())
    except OSError:
        return False


def matrix_groups(opt_values):
    """The fixed 14-group / 32-command matrix for --opt all."""
    groups = []
    for opt in opt_values:
        for engine in ENGINES:
            groups.append({"id": f"emit-{engine}-O{opt}",
                           "commands": [f"emit-{engine}-O{opt}"]})
        groups.append({"id": f"program-O{opt}",
                       "commands": [f"build-O{opt}", f"program-O{opt}"]})
        for engine in ENGINES:
            for mode in MODES:
                label = f"{engine}-O{opt}-{mode}"
                groups.append({"id": label,
                               "commands": [f"link-{label}",
                                            f"readelf-{label}",
                                            f"observed-{label}"]})
    return groups


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", type=Path, required=True,
                        help="Root-supplied CPP native helper")
    parser.add_argument("--wz-nativeobs", type=Path, required=True,
                        help="Root-supplied Wz Nativeobs entry")
    parser.add_argument("--cli", type=Path, required=True,
                        help="Root-supplied CPP CLI for the hosted program")
    parser.add_argument("--runtime", type=Path, required=True,
                        help="Root-supplied hosted runtime archive (pthread "
                             "observer link)")
    parser.add_argument("--clang", type=Path, required=True,
                        help="Root-supplied clang driver")
    parser.add_argument("--readelf", type=Path, required=True)
    parser.add_argument("--strace", type=Path, required=True,
                        help="Root-supplied strace for the fixed exec audit")
    parser.add_argument("--ld", type=Path,
                        help="linker driver the supplied clang may execute")
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--stdlib", type=Path)
    parser.add_argument("--timeout", type=int, default=PROGRAM_TIMEOUT,
                        help="per test-program budget (seconds)")
    parser.add_argument("--tool-timeout", type=int, default=300,
                        help="per emit/build/link budget (seconds)")
    parser.add_argument("--opt", choices=("0", "2", "all"), default="all")
    args = parser.parse_args()
    for name in ("helper", "wz_nativeobs", "cli", "runtime", "clang",
                 "readelf", "strace", "root", "out"):
        path = getattr(args, name)
        # LLVM selects its driver/output mode from these invocation names.
        setattr(args, name, path.absolute() if name in ("clang", "readelf")
                else path.resolve())
    if args.ld is not None:
        args.ld = args.ld.resolve()

    if not _fresh_output_directory(args.out):
        print("GC fresh-prefix test refused: --out is not new or empty; "
              "existing evidence was left unchanged", file=sys.stderr)
        return 2
    args.out.mkdir(parents=True, exist_ok=True)

    stdlib = (args.stdlib or args.root / "stdlib").resolve()
    try:
        configured = configure_inputs(args, stdlib)
    except Exception as error:
        print(f"GC fresh-prefix test failed before the first command: "
              f"{error}", file=sys.stderr)
        return 1
    configure_output(args.out)
    write_json_atomic(args.out / "inputs.before.json", configured)

    env = os.environ.copy()
    opt_values = (0, 2) if args.opt == "all" else (int(args.opt),)
    groups = matrix_groups(opt_values)
    for group in groups:
        group["status"] = "not_run"
    expected_commands = sum(len(group["commands"]) for group in groups)
    commands = []
    command_path = args.out / "commands.json"

    def save_command(row):
        if not any(existing is row for existing in commands):
            commands.append(row)
        write_json_atomic(command_path, commands)

    summary = {
        "schema": "woozy/gc-fresh-prefix-concurrent/1",
        "status": "failed",
        "scope": "fresh direct Vec prefix versus a real pending collection "
                 "request at O0/O2 for both emitters",
        "fixture": str(FIXTURE.relative_to(args.root)),
        "modes": list(MODES),
        "opt": list(opt_values),
        "budgets": {
            "handshake_seconds": HANDSHAKE_SECONDS,
            "collect_timeout_ns": COLLECT_TIMEOUT_NS,
            "collector_done_wait_seconds": COLLECTOR_DONE_SECONDS,
            "join_note": "pthread_join has no separate deadline; the wait for "
                         "the collector completion flag is bounded by 6 s and "
                         "the join itself only by the outer program budget, "
                         "and its return value is checked",
            "program_timeout_seconds": args.timeout,
            "tool_timeout_seconds": args.tool_timeout,
            "readelf_timeout_seconds": READELF_TIMEOUT,
        },
        # Both artifacts come from the same hosted manifest, so both are hosted
        # libc/pthread links.  Neither is a freestanding core program.
        "link_class": {},
        "runtime_class": "hosted (libc/pthreads) for the public CLI program and "
                         "for the observed binary; the same manifest requests "
                         "runtime = hosted for both, and neither artifact is "
                         "described as a freestanding core program",
        "matrix": {
            "logical_groups": len(groups),
            "expected_commands": expected_commands,
            "completed_commands": 0,
            "groups": groups,
        },
        "inputs": {"configured": len(configured), "before": configured,
                   "after": None, "unchanged": False},
        "engines": {},
        "observed": {},
        "limits": [
            "This runner builds no component and probes no version.",
            "The public CLI program and the observed binary are both hosted "
            "libc/pthread links from the same hosted manifest; both are "
            "recorded with a real readelf link-class check and neither is "
            "claimed to be a freestanding core program.",
            "The shim mirrors the fixture's Node layout (int32 marker plus one "
            "pointer) and fails on a stride mismatch instead of guessing.",
            "The Weak liveness/final-reclamation assertions live in the real "
            "source program, not in the shim.",
            "This proves a pending-request overlap and observed liveness for "
            "the selected constructor paths; it is not exhaustive "
            "multi-thread correctness and makes no performance claim.",
        ],
    }
    try:
        for opt in opt_values:
            for engine in ENGINES:
                directory = args.out / f"O{opt}" / engine
                directory.mkdir(parents=True, exist_ok=True)
                if engine == "cpp":
                    emit_argv = [
                        args.helper, "--project", MANIFEST, "--opt", str(opt),
                        "--out-object", directory / "program.o",
                        "--out-llvm-before", directory / "before.ll",
                        "--out-native", directory / "program.nir",
                        "--format", "json",
                    ]
                    emit_role = "emit-cpp"
                else:
                    emit_argv = [
                        args.wz_nativeobs, "--project", MANIFEST,
                        "--stdlib", stdlib, "--opt", str(opt),
                        "--out-llvm-before", directory / "before.ll",
                        "--out-native", directory / "program.nir",
                        "--format", "json",
                    ]
                    emit_role = "emit-wz"
                emitted = run(emit_argv, args.tool_timeout, env,
                              trace=f"emit-{engine}-O{opt}", sink=save_command,
                              role=emit_role)
                ok(emitted, f"{engine} O{opt} emit")
                checked = check_engine(f"{engine}-O{opt}", emitted, directory,
                                       SOURCE)
                summary["engines"][f"{engine}-O{opt}"] = {
                    "status": "passed",
                    "entry_symbol": checked["payload"]["entry_symbol"],
                    "facts": checked["facts"],
                    "before_llvm_sha256": checked["before_llvm_sha256"],
                    "native_ir_sha256": checked["native_ir_sha256"],
                }

            program_dir = args.out / f"O{opt}" / "program"
            program_dir.mkdir(parents=True, exist_ok=True)
            build = run([args.cli, "build", "--project", MANIFEST,
                         "--backend", "native", "--opt", str(opt),
                         "--out-dir", program_dir / "cli",
                         "--format", "json"],
                        args.tool_timeout, env, trace=f"build-O{opt}",
                        sink=save_command, role="program-build")
            ok(build, f"O{opt} public CLI build")
            payload = strict_json(build["stdout"]["text"],
                                  f"O{opt} public CLI payload")
            need(payload.get("status") == "ok",
                 f"O{opt} public CLI: {payload}")
            need(not any(diagnostic.get("code") == "E_CAPABILITY_UNAVAILABLE"
                         for diagnostic in payload.get("diagnostics", [])),
                 f"O{opt} public CLI silently fell back")
            program = program_dir / "cli" / "program"
            need(program.is_file(), f"O{opt} public CLI emitted no program")
            executed = run([program], args.timeout, env,
                           trace=f"program-O{opt}", sink=save_command,
                           role="program-run", program=program)
            assert_execution_completed(executed, f"O{opt} program")
            need(executed["stdout"]["text"] == "" and
                 executed["stderr"]["text"] == "",
                 f"O{opt} program produced unexpected output: {executed}")

            for engine in ENGINES:
                before = args.out / f"O{opt}" / engine / "before.ll"
                for mode in MODES:
                    label = f"{engine}-O{opt}-{mode}"
                    directory = (args.out / f"O{opt}" / f"{engine}-observe"
                                 / mode)
                    directory.mkdir(parents=True, exist_ok=True)
                    link = run([
                        args.clang, "-std=c++20", f"-O{opt}", before, SHIM,
                        ALLOCATION_OBSERVER, args.runtime, "-pthread",
                        "-Wl,--wrap=lang_alloc_v1",
                        "-Wl,--wrap=lang_free_v1",
                        "-Wl,--wrap=lang_vec_new_v1",
                        "-Wl,--wrap=lang_gc_program_register_v1",
                        "-o", directory / "observed",
                    ], args.tool_timeout, env, trace=f"link-{label}",
                        sink=save_command, role="link")
                    ok(link, f"{label} observer link")
                    observed_binary = directory / "observed"
                    # One real readelf invocation records both hosted artifacts,
                    # so the 14-group/32-command matrix count is exact.
                    elf = run([args.readelf, "-l", "-d", observed_binary,
                               program], READELF_TIMEOUT, env,
                              trace=f"readelf-{label}", sink=save_command,
                              role="readelf")
                    ok(elf, f"{label} readelf")
                    classes = parse_link_classes(elf["stdout"]["text"],
                                                 (observed_binary, program))
                    for path, evidence in classes.items():
                        need(evidence["hosted"],
                             f"{label}: {path} is not a hosted libc link; the "
                             "link class would be misreported")
                    summary["link_class"][label] = {
                        "readelf_argv": elf["argv"],
                        "exec_log_sha256": elf["exec_log_sha256"],
                        "observed": classes[str(observed_binary.resolve())],
                        "public_program": classes[str(program.resolve())],
                    }
                    observed_env = dict(env)
                    observed_env["GC_FRESH_PREFIX_MODE"] = mode
                    observed = run([observed_binary], args.timeout,
                                   observed_env, trace=f"observed-{label}",
                                   sink=save_command,
                                   role="observed", observed=observed_binary,
                                   env_delta={"GC_FRESH_PREFIX_MODE": mode})
                    shim_text, observer_text = split_shim_lines(
                        observed["stderr"]["text"])
                    parsed = parse_shim(shim_text)
                    if parsed["failures"]:
                        raise RuntimeError(
                            f"{label}: shim-failure: {parsed['failures']}")
                    need(observed["stdout"]["text"] == "",
                         f"{label}: observed program wrote stdout")
                    assert_execution_completed(observed, f"{label} observed")
                    record = assert_handshake(label, mode, parsed)
                    metrics = OBSERVER_SUPPORT.parse_observer(observer_text)
                    assert_observer_balance(label, metrics)
                    summary["observed"][label] = {
                        "status": "passed",
                        "purpose": MODE_SPEC[mode]["purpose"],
                        "handshake": record,
                        "allocation_observer": metrics,
                        "observed_sha256": digest(observed_binary),
                        "hosted_link": True,
                        "collect_timeout_ns": COLLECT_TIMEOUT_NS,
                    }

        need(len(commands) == expected_commands,
             f"expected {expected_commands} real commands for the matrix, "
             f"recorded {len(commands)}")
        traces = [row["trace"] for row in commands]
        for group in groups:
            for trace in group["commands"]:
                need(traces.count(trace) == 1,
                     f"matrix group {group['id']}: command {trace} ran "
                     f"{traces.count(trace)} times")
            group["status"] = "passed"
        after = tracked_inputs()
        summary["inputs"]["after"] = after
        summary["inputs"]["unchanged"] = configured == after
        need(summary["inputs"]["unchanged"],
             "configured inputs changed between the first and last command")
        summary["matrix"]["completed_commands"] = len(commands)
        summary["status"] = "passed"
        return 0
    except Exception as error:
        summary["error"] = str(error)
        print(f"GC fresh-prefix test failed: {error}", file=sys.stderr)
        return 1
    finally:
        try:
            after = tracked_inputs()
        except Exception as error:  # recorded, never silently dropped
            summary["inputs"]["after_error"] = f"{type(error).__name__}: {error}"
        else:
            summary["inputs"]["after"] = after
            summary["inputs"]["unchanged"] = configured == after
        summary["matrix"]["completed_commands"] = len(commands)
        for group in groups:
            ran = sum(1 for row in commands if row["trace"] in group["commands"])
            group["status"] = ("passed" if group["status"] == "passed"
                               else "not_run" if ran == 0 else "failed")
        summary["hashes"] = {}
        hash_error = None
        for key, path in sorted((INPUT_PATHS or {}).items()):
            try:
                summary["hashes"][key] = digest(path)
            except OSError as error:
                summary["hashes"][key] = f"error: {type(error).__name__}: {error}"
                hash_error = f"{key}: {error}"
        if hash_error is not None:
            summary["hash_error"] = hash_error
            summary["status"] = "failed"
        write_json_atomic(args.out / "inputs.after.json", {
            "after": summary["inputs"]["after"],
            "unchanged": summary["inputs"]["unchanged"],
            "error": summary["inputs"].get("after_error"),
        })
        write_json_atomic(args.out / "summary.json", summary)
        write_json_atomic(command_path, commands)


if __name__ == "__main__":
    sys.exit(main())
