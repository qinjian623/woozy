#!/usr/bin/env python3
"""Exact per-operation IR checks for the GC critical emission reduction.

The runner consumes one real Source fixture, the Root-supplied patched CPP
helper, and the Root-supplied patched Wz Nativeobs entry.  It never builds
either component and never substitutes one emitter's IR for the other.

The per-operation checks tie the real NativeIR slot/input identities to the
LLVM target address, published value, old-value scratch and drop operand.  A
critical pair must wrap the publication itself, not merely an unrelated active
flag.  A fresh Vec prefix is bounded by its own ``lang_vec_new_v1`` result and
that object's data/initialized header; a registration of another object does
not satisfy it.  Unregistered/empty prefixes keep a real following slot or
return publication as their boundary, so an extra constructor pair cannot be
hidden behind the next arbitrary call.

Every real process goes through the existing bounded
``selfhost/corpus.py::_run_process_impl`` transport.  Each command record is
written before its result is asserted; raw stdout/stderr bytes are kept as
hex plus SHA-256 so timeout partial bytes remain JSON-serialisable.  The output
directory must be new or empty: existing evidence is never deleted.

The same Source is then built by the public CLI and executed at O0/O2.  The
unchanged Native observer from ``tests/native/language_gc_source`` is linked
with both emitters' before-optimization LLVM, so both programs are really
executed while the public CLI binary remains the behavior reference.  There is
no skipped or IR-only success mode.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import re
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
FIXTURE = HERE / "fixtures" / "gc_critical"
CASES_FILE = FIXTURE / "cases.json"

FUNC_RE = re.compile(r"^function @(\d+) type=(\d+)$")
SLOT_RE = re.compile(r"^ slot \$(\d+) type=(\d+)(.*)$")
INSTR_RE = re.compile(r"^  (?:%(\d+) = )?([a-z_]+) type=(\d+)(.*)$")
SPAN_RE = re.compile(r"source=(\d+):(\d+)-(\d+)")
LLVM_DEF_RE = re.compile(r"^define\b.*@([A-Za-z0-9_.$]+)\([^)]*\)[^{]*\{")
CALL_RE = re.compile(r"\bcall\b")
STORE_RE = re.compile(r"^\s*store\b")
LOAD_RE = re.compile(r"^\s*(?:%[-A-Za-z0-9_.$]+\s*=\s*)?load\b")
VEC_NEW_LEN_RE = re.compile(r"@lang_vec_new_v1\(i64\s+(\d+)")
ALLOCA_RE = re.compile(
    r"^\s*(%[-A-Za-z0-9_.$]+)\s*=\s*alloca\s+([^,]+)(?:,|$)")
GEP_RE = re.compile(
    r"^\s*(%[-A-Za-z0-9_.$]+)\s*=\s*getelementptr(?:\s+inbounds)?\s+"
    r".+?,\s*ptr\s+(%[-A-Za-z0-9_.$]+)\s*,\s*(.+)$")
LOAD_PTR_RE = re.compile(
    r"^\s*(%[-A-Za-z0-9_.$]+)\s*=\s*load\s+ptr,\s*ptr\s+"
    r"(%[-A-Za-z0-9_.$]+)")
STORE_TARGET_RE = re.compile(
    r"^\s*store\s+(.+?),\s*ptr\s+(%[-A-Za-z0-9_.$]+)\s*(?:,.*)?$")
STORE_I64_RE = re.compile(
    r"^\s*store i64 (\d+), ptr (%[-A-Za-z0-9_.$]+)")
ENTER_RE = re.compile(
    r"^\s*(%[-A-Za-z0-9_.$]+)\s*=\s*call\s+i32\s+"
    r"@lang_gc_program_enter_critical_v1\(\)")
LEAVE_RE = re.compile(
    r"^\s*call\s+void\s+@lang_gc_program_leave_critical_v1\(i32\s+"
    r"(%[-A-Za-z0-9_.$]+)\)")
REGISTER_RE = re.compile(
    r"@lang_gc_program_register_v1\(ptr\s+(%[-A-Za-z0-9_.$]+)")
MEMCPY_DEST_RE = re.compile(
    r"@llvm\.memcpy[^(]*\(\s*ptr\s+(?:align\s+\d+\s+)?(%[-A-Za-z0-9_.$]+)")


def need(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    need(spec is not None and spec.loader is not None, f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_CORPUS = None


def _corpus():
    global _CORPUS
    if _CORPUS is None:
        _CORPUS = load_module(ROOT / "selfhost/corpus.py", "gc_critical_corpus")
    return _CORPUS


def _process_impl(argv, cwd, env, timeout):
    """The one real process transport.  Tests replace this exact seam."""
    return _corpus()._run_process_impl(argv, cwd, env, timeout)


def _json_bytes(value) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode(
        "utf-8")


def write_json_atomic(path: Path, value) -> None:
    """Write a same-directory pending file, then replace the public record."""
    pending = path.with_name(path.name + ".pending")
    pending.write_bytes(_json_bytes(value))
    os.replace(pending, path)


def _stream_record(raw: bytes) -> dict:
    return {
        "bytes": len(raw),
        "hex": raw.hex(),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "text": raw.decode("utf-8", "replace"),
    }


def run(argv, timeout, env, cwd=None, trace="", sink=None):
    """Run one bounded command and save the complete typed record first.

    ``sink`` is a short shared save point used by ``main`` for commands.json;
    it receives the record before any assertion can raise.  Raw bytes are
    always represented as hex/SHA text, so timeout partial bytes can be
    serialised even when they are not UTF-8.
    """
    argv = [str(value) for value in argv]
    actual_cwd = str(cwd if cwd is not None else Path.cwd())
    started_at = datetime.datetime.now(datetime.timezone.utc).isoformat()
    started = time.monotonic()
    row = {
        "trace": trace,
        "argv": argv,
        "cwd": actual_cwd,
        "timeout_seconds": timeout,
        "started_at": started_at,
        "elapsed_s": None,
        "exit": None,
        "timed_out": False,
        "cleanup_complete": False,
        "abandoned_pipes": False,
        "launch_failed": False,
        "launch_error": None,
        "stdout": _stream_record(b""),
        "stderr": _stream_record(b""),
    }
    try:
        result = _process_impl(argv, actual_cwd, env, timeout)
    except BaseException as error:  # launch failure is a record, not a pass
        row["launch_failed"] = True
        row["launch_error"] = f"{type(error).__name__}: {error}"
        result = {"exit": None, "stdout": b"", "stderr": b"",
                  "timed_out": False, "cleanup_complete": False,
                  "abandoned_pipes": False}
    if not isinstance(result, dict):
        row["launch_failed"] = True
        row["launch_error"] = f"process transport returned {type(result).__name__}"
        result = {"exit": None, "stdout": b"", "stderr": b"",
                  "timed_out": False, "cleanup_complete": False,
                  "abandoned_pipes": False}
    def raw(name):
        value = result.get(name) or b""
        if isinstance(value, bytes):
            return value
        return str(value).encode("utf-8", "replace")
    row["exit"] = result.get("exit")
    row["timed_out"] = result.get("timed_out")
    row["cleanup_complete"] = result.get("cleanup_complete")
    row["abandoned_pipes"] = result.get("abandoned_pipes")
    row["stdout"] = _stream_record(raw("stdout"))
    row["stderr"] = _stream_record(raw("stderr"))
    row["elapsed_s"] = round(time.monotonic() - started, 6)
    if sink is not None:
        sink(row)
    return row


def _strict_process(row, what):
    need(not row.get("launch_failed"),
         f"{what}: process launch failed: {row.get('launch_error')}")
    need(type(row.get("exit")) is int,
         f"{what}: exit is not an exact int: {row.get('exit')!r}")
    need(type(row.get("timed_out")) is bool and
         type(row.get("cleanup_complete")) is bool and
         type(row.get("abandoned_pipes")) is bool,
         f"{what}: process flags are not exact bools: {row}")
    need(row["timed_out"] is False, f"{what}: command timed out")
    need(row["cleanup_complete"] is True,
         f"{what}: command cleanup is incomplete")
    need(row["abandoned_pipes"] is False,
         f"{what}: command abandoned pipes")


def ok(row, what):
    _strict_process(row, what)
    need(row["exit"] == 0, f"{what}: exited {row['exit']}")
    return row


def assert_execution_completed(row, what):
    """A real execution must have an exact integer exit; skips are failures."""
    need(not row.get("skipped") and not row.get("not_run"),
         f"{what}: execution was skipped/not run")
    _strict_process(row, what)
    need(row["exit"] == 0, f"{what}: execution exited {row['exit']}")
    return row


# ---------------------------------------------------------------------------
# Real NIR parsing


def parse_nir(text: str) -> dict:
    need(text.strip(), "NIR is empty")
    lines = text.splitlines()
    need(lines[0].startswith("native-scalars "), "NIR header is missing")
    functions = {}
    current = None
    for line in lines[1:]:
        match = FUNC_RE.match(line)
        if match:
            current = {"symbol": int(match.group(1)),
                       "signature": int(match.group(2)),
                       "slots": [], "ops": []}
            need(current["symbol"] not in functions,
                 f"duplicate NIR function symbol {current['symbol']}")
            functions[current["symbol"]] = current
            continue
        if current is None:
            continue
        match = SLOT_RE.match(line)
        if match:
            slot_id, type_id, tail = match.groups()
            parameter = re.search(r"parameter=(\d+)", tail)
            inout = re.search(r"inout=(\d+)", tail)
            current["slots"].append({
                "id": int(slot_id),
                "type": int(type_id),
                "parameter": int(parameter.group(1)) if parameter else None,
                "inout": bool(int(inout.group(1))) if inout else False,
            })
            continue
        match = INSTR_RE.match(line)
        if not match:
            continue
        result, op, type_id, tail = match.groups()
        before_source = tail.split(" source=", 1)[0]
        inputs = [int(value) for value in re.findall(r"%(\d+)", before_source)]
        span_match = SPAN_RE.search(line)
        span = (tuple(int(value) for value in span_match.groups())
                if span_match else None)
        slot = None
        if op in ("load", "store", "address"):
            slot_match = re.search(r"\$(\d+)", before_source)
            if slot_match:
                slot = int(slot_match.group(1))
        current["ops"].append({
            "op": op, "type": int(type_id), "inputs": inputs,
            "span": span, "slot": slot,
            "result": int(result) if result is not None else None,
        })
    need(functions, "NIR contains no functions")
    return {"text": text, "functions": functions}


# ---------------------------------------------------------------------------
# LLVM text parsing


def parse_llvm(text: str) -> dict:
    need(text.strip(), "LLVM IR is empty")
    # Wz quotes simple global names; LLVM's C++ printer leaves them bare.
    text = re.sub(r'@"([A-Za-z0-9_.$]+)"', r'@\1', text)
    functions = {}
    current = None
    body = None
    for line in text.splitlines():
        if current is None:
            match = LLVM_DEF_RE.match(line)
            if match:
                current = match.group(1)
                need(current not in functions,
                     f"duplicate LLVM function {current}")
                body = [line]
            continue
        if line.strip() == "}":
            functions[current] = body
            current = None
            body = None
            continue
        body.append(line)
    need(current is None, f"unterminated LLVM function {current}")
    need(functions, "LLVM contains no function definitions")
    return functions


def call_indices(lines, symbol: str):
    pattern = re.compile(r"\bcall\b[^\n]*@" + re.escape(symbol) + r"\b")
    return [index for index, line in enumerate(lines) if pattern.search(line)]


def drop_indices(lines):
    pattern = re.compile(r"\bcall\b[^\n]*@lang\.drop\.")
    return [index for index, line in enumerate(lines) if pattern.search(line)]


def is_memcpy(line: str) -> bool:
    return "@llvm.memcpy" in line


def is_call(line: str) -> bool:
    return bool(CALL_RE.search(line))


def is_store(line: str) -> bool:
    return bool(STORE_RE.match(line))


def _store_target(line: str):
    match = STORE_TARGET_RE.match(line)
    if not match:
        return None
    return match.group(2)


def _store_value(line: str):
    match = STORE_TARGET_RE.match(line)
    if not match:
        return None
    lhs = match.group(1)
    values = re.findall(r"%[-A-Za-z0-9_.$]+|true|false|null", lhs)
    return values[-1] if values else None


def _drop_argument(line: str):
    match = re.search(r"@lang\.drop\.[0-9]+\(ptr\s+(%[-A-Za-z0-9_.$]+)\)",
                      line)
    return match.group(1) if match else None


def _register_argument(line: str):
    match = REGISTER_RE.search(line)
    return match.group(1) if match else None


def _entry_allocas(lines):
    allocas = []
    for line in lines:
        match = ALLOCA_RE.match(line)
        if match:
            allocas.append((match.group(1), match.group(2).strip()))
        elif allocas and not line.strip().startswith("store i1 "):
            # Slot allocas form the initial block prefix, possibly with
            # active-flag initialisation stores.  Body instructions end it.
            if not line.strip() == "":
                break
    return allocas


def _slot_names(function, lines):
    """Map non-inout NIR slot IDs to their LLVM alloca addresses.

    CPP prepends body scratch before the slots; Wz appends it after them.
    Owning i1 flags are interleaved.  The fixture has no i1-typed slot.
    """
    synthetic = function.get("llvm_slot_names")
    if synthetic:
        return {int(key): value for key, value in synthetic.items()}
    allocas = _entry_allocas(lines)
    non_i1 = [name for name, type_text in allocas if type_text != "i1"]
    slots = [slot for slot in function.get("slots", [])
             if not slot["inout"]]
    need(len(non_i1) >= len(slots),
         f"{function['symbol']}: {len(slots)} non-inout slots but only "
         f"{len(non_i1)} non-i1 entry allocas")
    need(function.get("llvm_emitter") in ("cpp", "wz"),
         f"{function['symbol']}: LLVM emitter layout is absent")
    names = (non_i1[:len(slots)] if function["llvm_emitter"] == "wz"
             else non_i1[-len(slots):]) if slots else []
    return {slot["id"]: name for slot, name in zip(slots, names)}


def _input_slot(function, value_id):
    for op in function["ops"]:
        if op.get("result") == value_id and op["op"] == "address":
            return op.get("slot")
    return None


def _guard_pairs(lines):
    pairs = []
    pending = {}
    for index, line in enumerate(lines):
        enter = ENTER_RE.match(line)
        if enter:
            pending[enter.group(1)] = index
            continue
        leave = LEAVE_RE.match(line)
        if leave:
            token = leave.group(1)
            need(token in pending, f"leave without matching enter: {line}")
            pairs.append((pending.pop(token), index, token))
    need(not pending, f"unmatched critical enter tokens: {sorted(pending)}")
    return sorted(pairs)


def _slot_target(function, slot_id, lines):
    names = _slot_names(function, lines)
    need(slot_id in names,
         f"{function['symbol']}: slot ${slot_id} has no LLVM address")
    return names[slot_id]


def _inout_target(function, slot_id, lines):
    synthetic = function.get("llvm_slot_names") or {}
    for key, value in synthetic.items():
        if int(key) == int(slot_id):
            return value
    slot = next((slot for slot in function.get("slots", [])
                 if slot["id"] == slot_id), None)
    need(slot is not None,
         f"{function['symbol']}: slot ${slot_id} is absent from NIR")
    need(slot["inout"] and slot["parameter"] is not None,
         f"{function['symbol']}: slot ${slot_id} is not an inout parameter")
    header = next((line for line in lines if line.startswith("define ")), "")
    need("(" in header, f"{function['symbol']}: LLVM definition is absent")
    parameters = re.findall(r"%[-A-Za-z0-9_.$]+(?=\s*(?:,|\)))",
                            header[header.index("("):])
    index = slot["parameter"]
    need(type(index) is int and 0 <= index < len(parameters),
         f"{function['symbol']}: LLVM parameter {index} is absent")
    return parameters[index]


def _store_inside(pairs, lines):
    result = []
    for enter, leave, _token in pairs:
        stores = [index for index in range(enter + 1, leave)
                  if is_store(lines[index])]
        result.append((enter, leave, stores))
    return result


# ---------------------------------------------------------------------------
# Source span mapping (only to attach a fixture declaration name to a real
# NIR SymbolId; no compiler behavior is inferred from source text).


def _match_brace(data: bytes, opening: int) -> int:
    depth = 0
    index = opening
    in_string = False
    escaped = False
    line_comment = False
    block_comment = False
    while index < len(data):
        char = data[index]
        if line_comment:
            if char == 10:
                line_comment = False
        elif block_comment:
            if data[index:index + 2] == b"*/":
                block_comment = False
                index += 1
        elif in_string:
            if escaped:
                escaped = False
            elif char == 92:
                escaped = True
            elif char == 34:
                in_string = False
        else:
            if data[index:index + 2] == b"//":
                line_comment = True
                index += 1
            elif data[index:index + 2] == b"/*":
                block_comment = True
                index += 1
            elif char == 34:
                in_string = True
            elif char == 123:
                depth += 1
            elif char == 125:
                depth -= 1
                if depth == 0:
                    return index + 1
        index += 1
    raise RuntimeError("unterminated source function body")


def scan_source_functions(data: bytes) -> dict:
    functions = {}
    prefixes = (b"func ", b"async func ", b"pub func ",
                b"pub async func ")
    index = 0
    while index < len(data):
        at_line = index == 0 or data[index - 1] == 10
        prefix = next((item for item in prefixes
                       if at_line and data.startswith(item, index)), None)
        if prefix is not None:
            cursor = index + len(prefix)
            while cursor < len(data) and (data[cursor:cursor + 1].isalnum()
                                          or data[cursor:cursor + 1] == b"_"):
                cursor += 1
            name = data[index + len(prefix):cursor].decode("ascii")
            opening = data.find(b"{", cursor)
            need(opening >= 0, f"source function {name} has no body")
            end = _match_brace(data, opening)
            need(name not in functions, f"duplicate source function {name}")
            functions[name] = (index, end)
            index = end
            continue
        index += 1
    return functions


def fixture_snapshot(nir: dict, entry: int) -> int:
    function = nir["functions"].get(entry)
    need(function is not None, f"entry SymbolId {entry} is absent from NIR")
    snapshots = {}
    for op in function["ops"]:
        if op["span"]:
            snapshots[op["span"][0]] = snapshots.get(op["span"][0], 0) + 1
    need(snapshots, "entry function has no source spans")
    return max(snapshots, key=snapshots.get)


def map_source_target(nir: dict, entry: int, snapshot: int, ranges: dict,
                      name: str) -> dict:
    need(name in ranges, f"fixture source function {name} is missing")
    start, end = ranges[name]
    candidates = []
    for function in nir["functions"].values():
        if function["symbol"] == entry:
            continue
        spans = [op["span"] for op in function["ops"]
                 if op["span"] and op["span"][0] == snapshot]
        if spans and all(start <= span[1] and span[2] <= end
                         for span in spans):
            candidates.append(function)
    need(len(candidates) == 1,
         f"{name}: expected one NIR SymbolId in source span [{start},{end}), "
         f"found {[f['symbol'] for f in candidates]}")
    return candidates[0]


# ---------------------------------------------------------------------------
# Exact IR assertions


def _prefix_maps(lines, start):
    result_match = re.match(
        r"^\s*(%[-A-Za-z0-9_.$]+)\s*=\s*call\s+ptr\s+@lang_vec_new_v1\(",
        lines[start])
    need(result_match is not None,
         f"vec_new result is missing at line {start}: {lines[start]}")
    vec = result_match.group(1)
    geps = {}
    for index, line in enumerate(lines):
        match = GEP_RE.match(line)
        if match:
            geps[match.group(1)] = (match.group(2), match.group(3))
    header2, header5 = set(), set()
    for name, (base, indices) in geps.items():
        if base != vec:
            continue
        tail = re.findall(r"i32\s+(\d+)", indices)
        if tail and tail[-1] == "2":
            header2.add(name)
        if tail and tail[-1] == "5":
            header5.add(name)
    data = set()
    for index, line in enumerate(lines):
        match = LOAD_PTR_RE.match(line)
        if match and match.group(2) in header5:
            data.add(match.group(1))
    element_targets = {name for name, (base, _indices) in geps.items()
                       if base in data}
    derived = {vec} | header2 | header5 | data | element_targets
    return vec, derived, header2, data, element_targets


def _is_real_slot_target(function, lines, target):
    try:
        names = set(_slot_names(function, lines).values())
    except RuntimeError:
        return False
    return target in names


def _unregistered_prefix_end(function, lines, start, derived):
    index = start + 1
    while index < len(lines):
        line = lines[index]
        enter = ENTER_RE.match(line)
        if enter:
            leave_index = None
            for probe in range(index + 1, len(lines)):
                leave = LEAVE_RE.match(lines[probe])
                if leave and leave.group(1) == enter.group(1):
                    leave_index = probe
                    break
            need(leave_index is not None,
                 f"prefix: unmatched enter at line {index}")
            stores = [probe for probe in range(index + 1, leave_index)
                      if is_store(lines[probe])]
            if any(_is_real_slot_target(function, lines,
                                        _store_target(lines[probe])) and
                   _store_value(lines[probe]) in derived
                   for probe in stores):
                return index
            # A critical pair that does not publish a real slot/return is an
            # extra constructor pair; keep it inside the checked prefix.
            index = leave_index + 1
            continue
        if is_store(line):
            target = _store_target(line)
            if (_is_real_slot_target(function, lines, target) and
                    _store_value(line) in derived):
                return index
        if re.match(r"^\s*(?:ret|return)\b", line):
            return index
        index += 1
    return len(lines)


def assert_prefix_case(name, spec, function, lines):
    vec_ops = [op for op in function["ops"] if op["op"] == "vec_init"]
    need(vec_ops, f"{name}: no vec_init operation found")
    new_indices = call_indices(lines, "lang_vec_new_v1")
    need(len(new_indices) == len(vec_ops),
         f"{name}: {len(vec_ops)} vec_init ops but {len(new_indices)} "
         "lang_vec_new_v1 calls")
    register_indices = call_indices(lines, "lang_gc_program_register_v1")
    facts = []
    for number, (op, start) in enumerate(zip(vec_ops, new_indices)):
        length_match = VEC_NEW_LEN_RE.search(lines[start])
        need(length_match is not None,
             f"{name}: cannot read vec_new length for init {number}")
        need(int(length_match.group(1)) == len(op["inputs"]),
             f"{name}: vec_init {number} has {len(op['inputs'])} inputs but "
             f"vec_new length {length_match.group(1)}")
        vec, derived, header2, data, element_targets = _prefix_maps(
            lines, start)

        own_registers = [index for index in register_indices
                         if _register_argument(lines[index]) == vec]
        wrong_registers = [index for index in register_indices
                           if _register_argument(lines[index]) != vec]
        if spec["registered"]:
            following = [index for index in own_registers if index > start]
            need(following, f"{name}: registration of {vec} is missing after "
                            "vec_new")
            end = following[0]
            need(not any(index < end for index in wrong_registers),
                 f"{name}: another object is registered before {vec}")
        else:
            need(not own_registers,
                 f"{name}: non-strong element unexpectedly registered {vec}")
            end = _unregistered_prefix_end(function, lines, start, derived)

        for index in range(start, end):
            line = lines[index]
            need(not re.search(r"@lang_gc_program_(?:enter|leave)_critical_v1",
                               line),
                 f"{name}: critical call inside vec_init prefix at line "
                 f"{index}: {line}")
            need(index == start or not is_call(line) or is_memcpy(line),
                 f"{name}: non-memcpy call inside vec_init prefix at line "
                 f"{index}: {line}")
            if index > start and is_call(line) and not is_memcpy(line):
                need(_register_argument(line) == vec,
                     f"{name}: unrelated call inside vec_init prefix: {line}")

        stores = []
        memcpy_targets = []
        for index in range(start, end):
            line = lines[index]
            match = STORE_I64_RE.match(line)
            if match and match.group(2) in header2:
                stores.append((index, int(match.group(1))))
            if is_memcpy(line):
                memcpy = MEMCPY_DEST_RE.search(line)
                if memcpy:
                    memcpy_targets.append(memcpy.group(1))
        elements = spec["elements"]
        if elements:
            expected = list(range(1, elements + 1))
            need([value for _, value in stores] == expected,
                 f"{name}: initialized-prefix stores are {stores}, "
                 f"expected {expected}")
            need(stores[-1][0] < end,
                 f"{name}: registration/next call precedes prefix completion")
            need(memcpy_targets,
                 f"{name}: element copy is missing from the fresh prefix")
            if any(target == data_value for target in memcpy_targets
                   for data_value in data):
                pass  # constant table path copies the whole array at once
            else:
                need(all(target in element_targets
                         for target in memcpy_targets),
                     f"{name}: element copy targets another object: "
                     f"{memcpy_targets}")
                need(len(memcpy_targets) == elements,
                     f"{name}: {len(memcpy_targets)} element copies for "
                     f"{elements} elements")
        else:
            need(not stores,
                 f"{name}: empty Vec wrote an initialized-prefix store: {stores}")
            need(not memcpy_targets,
                 f"{name}: empty Vec copied element bytes: {memcpy_targets}")

        if spec["registered"]:
            need(end in register_indices,
                 f"{name}: prefix end is not a registration call")
            need(_register_argument(lines[end]) == vec,
                 f"{name}: prefix end registers another object")
        facts.append({"symbol": function["symbol"], "vec_init_ordinal": number,
                      "vec": vec, "elements": elements,
                      "registered": spec["registered"],
                      "prefix_guard_calls": 0,
                      "prefix_calls": [lines[i].strip()
                                       for i in range(start + 1, end)
                                       if is_call(lines[i])]})
    return facts


def _source_part(name, function, operation):
    data = function["source_bytes"]
    span = operation.get("span")
    need(span is not None and len(span) == 3 and
         all(type(value) is int for value in span) and
         span[0] == function["source_snapshot"] and
         0 <= span[1] < span[2] <= len(data),
         f"{name}: invalid operation Source span: {span}")
    return data[span[1]:span[2]]


def _binding_store(name, function):
    stores = [op for op in function["ops"] if op["op"] == "store"]
    if "source_bytes" not in function:
        # Original small assertion controls have no Source declaration/lifetime.
        need(len(stores) == 1,
             f"{name}: expected one NIR store, found {len(stores)}")
        return stores[0], None
    candidates = []
    for operation, following in zip(function["ops"], function["ops"][1:]):
        if (operation["op"] == "store" and
                following["op"] == "ownership_begin" and
                operation.get("span") == following.get("span")):
            declaration = _source_part(name, function, operation)
            match = re.fullmatch(rb"(?:let|var)\s+value\b[^=]*=\s*(.+)",
                                 declaration, re.DOTALL)
            if match:
                span = operation["span"]
                candidates.append((operation,
                                   (span[1] + match.start(1), span[2])))
    need(len(candidates) == 1,
         f"{name}: expected one value binding store/lifetime, found "
         f"{len(candidates)}")
    return candidates[0]


def _temporary_stores(name, function, operation, rhs):
    if rhs is None:
        return []
    slots = {slot["id"]: slot for slot in function["slots"]}
    producers = {op["result"]: op for op in function["ops"]
                 if op.get("result") is not None}
    temporary = []
    seen = set()
    for index, op in enumerate(function["ops"]):
        if op["op"] != "store":
            continue
        need(len(op["inputs"]) == 1 and op.get("slot") in slots,
             f"{name}: store producer/slot is absent")
        slot = slots[op["slot"]]
        producer = producers.get(op["inputs"][0])
        need(producer is not None and producer["type"] == slot["type"],
             f"{name}: store producer type differs from slot ${slot['id']}")
        _source_part(name, function, producer)
        producer_span = producer["span"]
        need(rhs[0] <= producer_span[1] < producer_span[2] <= rhs[1],
             f"{name}: store producer is outside the declaration/assignment RHS")
        need(slot["id"] not in seen and slot["parameter"] is None,
             f"{name}: duplicate or parameter publication ${slot['id']}")
        seen.add(slot["id"])
        if op is operation:
            continue
        _source_part(name, function, op)
        following = (function["ops"][index + 1]
                     if index + 1 < len(function["ops"]) else {})
        need(following.get("op") == "take" and
             following.get("span") == op["span"] == producer_span,
             f"{name}: extra store is not a same-Source temporary store/take")
        need(slot["type"] in function["type_guards"],
             f"{name}: temporary type {slot['type']} has no case expectation")
        temporary.append(op)
    return temporary


def _publication_pairs(name, function, lines, target, expected, temporary):
    known = {target: expected}
    operations = {}
    for op in temporary:
        address = _slot_target(function, op["slot"], lines)
        need(address not in known,
             f"{name}: duplicate temporary publication address {address}")
        slot = next(slot for slot in function["slots"]
                    if slot["id"] == op["slot"])
        known[address] = function["type_guards"][slot["type"]]
        operations[address] = op
    pairs = _guard_pairs(lines)
    pair_by_enter = {pair[0]: pair for pair in pairs}
    by_target = {address: [] for address in known}
    for enter, leave, stores_inside in _store_inside(pairs, lines):
        need(len(stores_inside) == 1,
             f"{name}: critical pair does not wrap exactly one publication: "
             f"{stores_inside}")
        address = _store_target(lines[stores_inside[0]])
        need(address in known,
             f"{name}: guarded store targets unknown publication {address}")
        need(not any(is_call(line) for line in lines[enter + 1:leave]),
             f"{name}: unexpected call/drop inside publication guard")
        by_target[address].append(pair_by_enter[enter])
    for address, count in known.items():
        need(len(by_target[address]) == count,
             f"{name}: expected {count} guard pairs for {address}, found "
             f"{len(by_target[address])}")
        stores = [i for i, line in enumerate(lines)
                  if is_store(line) and _store_target(line) == address]
        need(len(stores) == 1,
             f"{name}: expected one publication store to {address}, found {stores}")
        if address in operations:
            need(not any(_drop_argument(lines[i]) == address
                         for i in drop_indices(lines)),
                 f"{name}: consumed temporary {address} is dropped")
    # A temporary consumed into another slot must publish the same NIR value.
    values = {}
    for address, op in operations.items():
        line = next(line for line in lines
                    if is_store(line) and _store_target(line) == address)
        value = STORE_TARGET_RE.match(line).group(1).strip()
        value_id = op["inputs"][0]
        need(value_id not in values or values[value_id] == value,
             f"{name}: temporary publications disagree on the same NIR value")
        values[value_id] = value
    function["temporary_publication_values"] = values
    return by_target[target]


def assert_store_case(name, spec, function, lines):
    operation, rhs = _binding_store(name, function)
    need(operation.get("slot") is not None,
         f"{name}: NIR store has no slot")
    slot_id = operation["slot"]
    target = _slot_target(function, slot_id, lines)
    expected = spec["guards"]
    temporary = _temporary_stores(name, function, operation, rhs)
    pairs = _publication_pairs(name, function, lines, target, expected, temporary)
    values = function["temporary_publication_values"]
    if operation["inputs"][0] in values:
        line = next(line for line in lines
                    if is_store(line) and _store_target(line) == target)
        need(STORE_TARGET_RE.match(line).group(1).strip() ==
             values[operation["inputs"][0]],
             f"{name}: binding publication differs from its temporary value")

    publications = []
    for enter, leave, stores_inside in _store_inside(pairs, lines):
        need(enter < leave, f"{name}: leave precedes enter")
        need(len(stores_inside) == 1,
             f"{name}: critical pair does not wrap exactly one publication: "
             f"{stores_inside}")
        store_index = stores_inside[0]
        need(_store_target(lines[store_index]) == target,
             f"{name}: guarded store targets "
             f"{_store_target(lines[store_index])}, expected {target}")
        publications.append(store_index)
    if expected:
        need(len(publications) == 1,
             f"{name}: expected one guarded publication")
        publish_end = pairs[0][1]
    else:
        need(not pairs, f"{name}: non-strong store has a critical guard")
        store_rows = [index for index, line in enumerate(lines)
                      if is_store(line)
                      and _store_target(line) == target]
        need(len(store_rows) == 1,
             f"{name}: expected one publication store to {target}, found "
             f"{store_rows}")
        publications = store_rows
        publish_end = store_rows[0]

    drops = drop_indices(lines)
    own_drops = [index for index in drops
                 if _drop_argument(lines[index]) == target]
    need(len(own_drops) >= spec["min_drops"],
         f"{name}: expected at least {spec['min_drops']} drop of {target}, "
         f"found {own_drops} among {drops}")
    need(min(own_drops) > publish_end,
         f"{name}: owner drop does not follow the published store")
    for enter, leave, _token in _guard_pairs(lines):
        need(not any(enter < drop < leave for drop in drops),
             f"{name}: owner drop occurs inside the critical window")
    return {"symbol": function["symbol"], "slot": slot_id,
            "target": target, "guards": expected, "drops": len(own_drops)}


def assert_replace_case(name, spec, function, lines):
    replaces = [op for op in function["ops"]
                if op["op"] == "replace_indirect"]
    need(len(replaces) == 1,
         f"{name}: expected one NIR replace_indirect, found {len(replaces)}")
    operation = replaces[0]
    slot_id = operation.get("slot")
    if slot_id is None and operation["inputs"]:
        slot_id = _input_slot(function, operation["inputs"][0])
    need(slot_id is not None, f"{name}: replace target slot is unknown")
    target = _inout_target(function, slot_id, lines)
    expected = spec["guards"]
    rhs = None
    if "source_bytes" in function:
        text = _source_part(name, function, operation)
        match = re.fullmatch(rb"value\s*=\s*(.+)", text, re.DOTALL)
        need(match is not None, f"{name}: replace is not the value assignment")
        rhs = (operation["span"][1] + match.start(1), operation["span"][2])
        slot = next(slot for slot in function["slots"] if slot["id"] == slot_id)
        need(function["type_guards"].get(slot["type"]) == expected,
             f"{name}: replace type differs from case expectation")
    temporary = _temporary_stores(name, function, operation, rhs)
    pairs = _publication_pairs(name, function, lines, target, expected, temporary)

    # The old owner must be loaded from the actual target and saved aside.
    old_loads = [index for index, line in enumerate(lines)
                 if LOAD_RE.match(line)
                 and re.search(r"ptr\s+" + re.escape(target) + r"(?:\s*,|\s*$)",
                               line)]
    need(old_loads, f"{name}: old value is not loaded from {target}")
    old_load = old_loads[0]
    old_match = re.match(
        r"^\s*(%[-A-Za-z0-9_.$]+)\s*=\s*load\s+.+?,\s*ptr\s+"
        + re.escape(target), lines[old_load])
    need(old_match is not None,
         f"{name}: cannot read old-value load from {target}")
    old_value = old_match.group(1)
    scratch_stores = [index for index, line in enumerate(lines)
                      if is_store(line)
                      and re.search(r"\s" + re.escape(old_value) + r"\s*,",
                                    line)]
    need(scratch_stores,
         f"{name}: old value {old_value} is not saved to scratch")
    scratch_store = scratch_stores[0]
    scratch = _store_target(lines[scratch_store])
    need(scratch is not None and scratch != target,
         f"{name}: old-value scratch equals the target {target}")

    publications = []
    for enter, leave, stores_inside in _store_inside(pairs, lines):
        need(len(stores_inside) == 1,
             f"{name}: critical pair does not wrap exactly one publication: "
             f"{stores_inside}")
        store_index = stores_inside[0]
        need(_store_target(lines[store_index]) == target,
             f"{name}: guarded store targets "
             f"{_store_target(lines[store_index])}, expected {target}")
        published = _store_value(lines[store_index])
        need(published is not None and published not in
             (old_value, scratch, target),
             f"{name}: publication does not store the new value: "
             f"{lines[store_index]}")
        need(old_load < enter and scratch_store < enter,
             f"{name}: old load/save is not before the critical window")
        publications.append(store_index)
    if expected:
        need(len(publications) == 1,
             f"{name}: expected one guarded publication")
        publish_end = pairs[0][1]
    else:
        need(not pairs, f"{name}: non-strong replace has a critical guard")
        store_rows = [index for index, line in enumerate(lines)
                      if is_store(line)
                      and _store_target(line) == target
                      and index != scratch_store]
        need(len(store_rows) == 1,
             f"{name}: expected one publication store to {target}, found "
             f"{store_rows}")
        publications = store_rows
        publish_end = store_rows[0]
        published = _store_value(lines[store_rows[0]])
        need(published is not None and published not in
             (old_value, scratch, target),
             f"{name}: publication does not store the new value: "
             f"{lines[store_rows[0]]}")
        need(old_load < publications[0] and scratch_store < publications[0],
             f"{name}: old load/save does not precede publication")

    values = function["temporary_publication_values"]
    if len(operation["inputs"]) > 1 and operation["inputs"][1] in values:
        need(STORE_TARGET_RE.match(lines[publications[0]]).group(1).strip() ==
             values[operation["inputs"][1]],
             f"{name}: replacement differs from its temporary value")

    drops = drop_indices(lines)
    need(len(drops) == spec["drops"],
         f"{name}: expected {spec['drops']} old-owner drop, found {len(drops)}")
    for drop in drops:
        need(_drop_argument(lines[drop]) == scratch,
             f"{name}: drop targets {_drop_argument(lines[drop])}, expected "
             f"old scratch {scratch}")
        need(drop > publish_end,
             f"{name}: old-owner drop is not after the publication")
        for enter, leave, _token in _guard_pairs(lines):
            need(not enter < drop < leave,
                 f"{name}: drop occurs inside the critical window")
    return {"symbol": function["symbol"], "slot": slot_id,
            "target": target, "scratch": scratch,
            "guards": expected, "drops": len(drops)}


def assert_store_indirect_case(name, spec, function, lines):
    ops = [op for op in function["ops"] if op["op"] == "store_indirect"]
    need(len(ops) == 1,
         f"{name}: expected one NIR store_indirect, found {len(ops)}")
    operation = ops[0]
    slot_id = operation.get("slot")
    if slot_id is None and operation["inputs"]:
        slot_id = _input_slot(function, operation["inputs"][0])
    need(slot_id is not None, f"{name}: store_indirect target slot is unknown")
    target = _inout_target(function, slot_id, lines)
    pairs = _guard_pairs(lines)
    need(len(pairs) == spec["guards"],
         f"{name}: expected {spec['guards']} guard pairs, found {len(pairs)}")
    stores = [index for index, line in enumerate(lines)
              if is_store(line) and _store_target(line) == target]
    need(len(stores) == 1,
         f"{name}: expected one publication store to {target}, found {stores}")
    return {"symbol": function["symbol"], "slot": slot_id,
            "target": target, "guards": spec["guards"],
            "drops": len(drop_indices(lines))}


# ---------------------------------------------------------------------------
# Engine invocation and full case checks


def emit_cpp(helper, manifest, opt, directory, timeout, env, sink):
    directory.mkdir(parents=True, exist_ok=True)
    return run([helper, "--project", manifest, "--opt", str(opt),
                "--out-object", directory / "program.o",
                "--out-llvm-before", directory / "before.ll",
                "--out-native", directory / "program.nir",
                "--format", "json"], timeout, env,
               trace=f"emit-cpp-O{opt}", sink=sink)


def emit_wz(driver, manifest, stdlib, opt, directory, timeout, env, sink):
    directory.mkdir(parents=True, exist_ok=True)
    return run([driver, "--project", manifest, "--stdlib", stdlib,
                "--opt", str(opt),
                "--out-llvm-before", directory / "before.ll",
                "--out-native", directory / "program.nir",
                "--format", "json"], timeout, env,
               trace=f"emit-wz-O{opt}", sink=sink)


def check_engine(label, emitted, directory, manifest_source, cases):
    payload = json.loads(emitted["stdout"]["text"])
    need(payload.get("status") == "passed",
         f"{label}: emitter did not pass: {emitted}")
    entry = payload.get("entry_symbol")
    need(type(entry) is int, f"{label}: emitter omitted entry_symbol")
    nir = parse_nir((directory / "program.nir").read_text())
    llvm = parse_llvm((directory / "before.ll").read_text())
    need(entry in nir["functions"],
         f"{label}: entry SymbolId {entry} is absent from NIR")
    snapshot = fixture_snapshot(nir, entry)
    source_bytes = manifest_source.read_bytes()
    ranges = scan_source_functions(source_bytes)

    # Type IDs are local to each emitter's NIR.  Derive this module's expected
    # 0/1 guards from the fixture's named user declarations and cases.json,
    # never from the observed LLVM guard count.
    type_guards = {}
    for name, spec in cases["store"].items():
        function = map_source_target(nir, entry, snapshot, ranges, name)
        function = dict(function, source_bytes=source_bytes,
                        source_snapshot=snapshot)
        operation, _rhs = _binding_store(name, function)
        slot = next((slot for slot in function["slots"]
                     if slot["id"] == operation.get("slot")), None)
        need(slot is not None, f"{name}: binding slot is absent")
        previous = type_guards.setdefault(slot["type"], spec["guards"])
        need(previous == spec["guards"],
             f"{name}: conflicting case expectations for type {slot['type']}")

    facts = {}
    seen_symbols = {}
    for group, entries in cases.items():
        for name, spec in entries.items():
            function = map_source_target(nir, entry, snapshot, ranges, name)
            need(function["symbol"] not in seen_symbols,
                 f"{label}: {name} maps to SymbolId {function['symbol']} "
                 f"already used by {seen_symbols.get(function['symbol'])}")
            seen_symbols[function["symbol"]] = name
            llvm_name = f"lang.fn.{function['symbol']}"
            need(llvm_name in llvm,
                 f"{label}: LLVM function {llvm_name} for {name} is missing")
            lines = llvm[llvm_name]
            function = dict(function, llvm_emitter=label.split()[0],
                            source_bytes=source_bytes,
                            source_snapshot=snapshot, type_guards=type_guards)
            if group == "prefix":
                facts[name] = assert_prefix_case(name, spec, function, lines)
            elif group == "store":
                facts[name] = assert_store_case(name, spec, function, lines)
            elif group == "replace":
                facts[name] = assert_replace_case(name, spec, function, lines)
            elif group == "store_indirect":
                facts[name] = assert_store_indirect_case(
                    name, spec, function, lines)
            else:
                raise RuntimeError(f"unknown case group {group}")
    need(facts, f"{label}: no target matched; refusing a zero-target pass")
    return {"payload": payload, "nir": nir, "facts": facts,
            "nir_text": (directory / "program.nir").read_text()}


def build_and_execute(cli, manifest, opt, directory, timeout, env, readelf,
                      sink):
    directory.mkdir(parents=True, exist_ok=True)
    build = run([cli, "build", "--project", manifest, "--backend", "native",
                 "--opt", str(opt), "--out-dir", directory / "cli",
                 "--format", "json"], timeout, env,
                trace=f"build-O{opt}", sink=sink)
    ok(build, f"O{opt} public CLI build")
    payload = json.loads(build["stdout"]["text"])
    need(payload.get("status") == "ok", f"O{opt} public CLI: {payload}")
    program = directory / "cli" / "program"
    need(program.is_file(), f"O{opt} public CLI emitted no program")
    executed = run([program], timeout, env, trace=f"program-O{opt}", sink=sink)
    assert_execution_completed(executed, f"O{opt} program")
    need(not executed["stdout"]["text"] and not executed["stderr"]["text"],
         f"O{opt} program produced output: {executed}")
    elf = run([readelf, "-l", "-d", program], 60, env,
              trace=f"readelf-program-O{opt}", sink=sink)
    ok(elf, f"O{opt} program readelf")
    need("INTERP" not in elf["stdout"]["text"] and
         "NEEDED" not in elf["stdout"]["text"],
         f"O{opt} program is not a static freestanding ELF")
    return build, executed, program


def observe_before_llvm(label, before, directory, opt, timeout, env,
                        gc_support, observer_source, runtime, clang, readelf,
                        sink):
    directory.mkdir(parents=True, exist_ok=True)
    linked = run([clang, f"-O{opt}", "-std=c++20", before,
                  observer_source, runtime, "-o", directory / "observe",
                  "-nostdlib", "-static",
                  "-Wl,--wrap=lang_alloc_v1", "-Wl,--wrap=lang_free_v1"],
                 timeout, env, trace=f"{label}-O{opt}-link", sink=sink)
    ok(linked, f"{label} O{opt} observer link")
    elf = run([readelf, "-l", "-d", directory / "observe"], 60, env,
              trace=f"{label}-O{opt}-readelf", sink=sink)
    ok(elf, f"{label} O{opt} observer readelf")
    need("INTERP" not in elf["stdout"]["text"] and
         "NEEDED" not in elf["stdout"]["text"],
         f"{label} O{opt} observer is not a static freestanding ELF")
    observed = run([directory / "observe"], timeout, env,
                   trace=f"{label}-O{opt}-observe", sink=sink)
    assert_execution_completed(observed, f"{label} O{opt} observer")
    need(not observed["stdout"]["text"],
         f"{label} O{opt} observer wrote stdout")
    metrics = gc_support.parse_events(observed["stderr"]["text"])
    need(metrics["live_vecs_at_exit"] == 0,
         f"{label} O{opt} leaked Vecs: {metrics}")
    need(metrics["final_live_payload_bytes"] == 0,
         f"{label} O{opt} leaked payload: {metrics}")
    need(metrics["vec_allocations"] == metrics["vec_frees"],
         f"{label} O{opt} unbalanced Vec count: {metrics}")
    return {"link": linked, "readelf": elf, "execute": observed,
            "metrics": metrics}


def _fresh_output_directory(path: Path) -> bool:
    if not path.exists():
        return True
    if not path.is_dir():
        return False
    try:
        return not any(path.iterdir())
    except OSError:
        return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--helper", type=Path, required=True,
                        help="Root-supplied patched CPP native helper")
    parser.add_argument("--wz-nativeobs", type=Path, required=True,
                        help="Root-supplied patched Wz Nativeobs entry")
    parser.add_argument("--cli", type=Path, required=True,
                        help="role public CLI for the Source program")
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--readelf", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--stdlib", type=Path)
    parser.add_argument("--observer", type=Path)
    parser.add_argument("--libxml2-lib", type=Path)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--opt", choices=("0", "2", "all"), default="all")
    args = parser.parse_args()
    for name in ("helper", "wz_nativeobs", "cli", "runtime", "clang",
                 "readelf", "root", "out"):
        path = getattr(args, name)
        # LLVM selects its driver/output mode from these invocation names.
        setattr(args, name, path.absolute() if name in ("clang", "readelf")
                else path.resolve())

    # Refuse existing evidence before any write or external command.  This is
    # the only accepted lifecycle: a new or truly empty evidence directory.
    if not _fresh_output_directory(args.out):
        print("GC critical emission test refused: --out is not new or empty; "
              "existing evidence was left unchanged", file=sys.stderr)
        return 2
    args.out.mkdir(parents=True, exist_ok=True)

    stdlib = (args.stdlib or args.root / "stdlib").resolve()
    observer = (args.observer or args.root / "tests/native/language_gc_source/"
                "observe_gc.cpp").resolve()
    manifest = FIXTURE / "lang.toml"
    source = FIXTURE / "src/main.wz"
    cases = json.loads(CASES_FILE.read_text(encoding="utf-8"))
    gc_support = load_module(
        args.root / "tests/native/language_gc_source/run_language_gc_source.py",
        "gc_critical_language_gc_support")

    env = os.environ.copy()
    if args.libxml2_lib:
        previous = env.get("LD_LIBRARY_PATH")
        env["LD_LIBRARY_PATH"] = str(args.libxml2_lib) + (
            ":" + previous if previous else "")

    opt_values = (0, 2) if args.opt == "all" else (int(args.opt),)
    commands = []
    command_path = args.out / "commands.json"

    def save_command(row):
        commands.append(row)
        write_json_atomic(command_path, commands)

    summary = {
        "schema": "woozy/gc-critical-emission/1",
        "status": "failed",
        "scope": "real Source O0/O2 patched-CPP and patched-Wz per-operation IR",
        "fixture": str(FIXTURE.relative_to(args.root)),
        "opt": list(opt_values),
        "engines": {},
        "program_observer": {},
        "commands": commands,
        "hashes": {},
        "limits": [
            "This runner does not build the CPP helper or Wz Nativeobs component.",
            "Broader cycle/live-root/8192/2MiB/concurrency suites stay separate.",
            "No performance claim is made from IR or a non-random profiler sample.",
        ],
    }
    try:
        for opt in opt_values:
            engine_text = {}
            for engine, emitter in (("cpp", args.helper),
                                    ("wz", args.wz_nativeobs)):
                directory = args.out / f"O{opt}" / engine
                if engine == "cpp":
                    emitted = emit_cpp(emitter, manifest, opt, directory,
                                       args.timeout, env, save_command)
                else:
                    emitted = emit_wz(emitter, manifest, stdlib, opt, directory,
                                      args.timeout, env, save_command)
                ok(emitted, f"{engine} O{opt} emit")
                checked = check_engine(f"{engine} O{opt}", emitted, directory,
                                       source, cases)
                engine_text[engine] = checked["nir_text"]
                summary["engines"][f"{engine}-O{opt}"] = {
                    "status": "passed",
                    "entry_symbol": checked["payload"]["entry_symbol"],
                    "facts": checked["facts"],
                    "before_llvm_sha256": digest(directory / "before.ll"),
                    "native_ir_sha256": digest(directory / "program.nir"),
                }
            need(engine_text["cpp"] == engine_text["wz"],
                 f"O{opt}: patched CPP and patched Wz NIR differ")

            program_dir = args.out / f"O{opt}" / "program"
            _build, _executed, program = build_and_execute(
                args.cli, manifest, opt, program_dir, args.timeout, env,
                args.readelf, save_command)
            row = {"status": "passed", "program_sha256": digest(program)}
            for engine in ("cpp", "wz"):
                before = args.out / f"O{opt}" / engine / "before.ll"
                observed = observe_before_llvm(
                    engine, before, args.out / f"O{opt}" / f"{engine}-observe",
                    opt, args.timeout, env, gc_support, observer,
                    args.runtime, args.clang, args.readelf, save_command)
                row[engine] = observed["metrics"]
            summary["program_observer"][f"O{opt}"] = row

        for path in (args.helper, args.wz_nativeobs, args.cli, args.runtime,
                     observer, manifest, source, CASES_FILE,
                     args.root / "tests/native/language_gc_source/"
                     "run_language_gc_source.py"):
            if path.is_file():
                summary["hashes"][str(path)] = digest(path)
        summary["status"] = "passed"
        return 0
    except Exception as error:
        summary["error"] = str(error)
        print(f"GC critical emission test failed: {error}", file=sys.stderr)
        return 1
    finally:
        write_json_atomic(args.out / "summary.json", summary)
        write_json_atomic(command_path, commands)


if __name__ == "__main__":
    sys.exit(main())
