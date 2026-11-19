#!/usr/bin/env python3
"""Python supplemental floating observations: 14 projects, 9 observations.

This is a focused entry point.  It imports the accepted Root
``run.py`` and reuses its provenance, process, trace and comparison helpers;
it does not copy the full runner or build the driver.  Every project is a
real candidate Python check/build/run.  For the nine requested functions the
observation is executed from the real CLI ``program.py`` with metadata made
by the retained pure-Wz driver.  The driver's normal ``program.py`` and
``langrt.py`` must be byte-identical to the CLI files.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
import re
import struct
import sys
import time
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUN_PATH = HERE / "run.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("float_observer_run", RUN_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {RUN_PATH}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["float_observer_run"] = module
    spec.loader.exec_module(module)
    return module


run = load_runner()
sha = run.sha
TOOLS = run.TOOLS
EXPECTED_PROJECTS = 14
EXPECTED_OBSERVATIONS = 9
# Independent case table from the original supplemental fixtures.  It is not
# read from driver metadata or from an observed row.  ``evaluation`` returns
# i32; each f32-/f64- original case has that exact function result type.
PROBE_RESULT_TYPES = {
    "evaluation": "i32",
    "f32-return-defer": "f32",
    "f32-typed-defer": "f32",
    "f32-panic-skip": "f32",
    "f32-float-zero-in-defer": "f32",
    "f64-return-defer": "f64",
    "f64-typed-defer": "f64",
    "f64-panic-skip": "f64",
    "f64-float-zero-in-defer": "f64",
}
DEFAULT_PYTHON = run.DEFAULT_PYTHON


def manifest_text():
    return (
        'schema_version=1\n[package]\nname="app"\nsource_root="src"\n'
        'entry="app"\n[build]\nbackend="python"\nword_bits=64\n')


def make_project(work: Path, ident: str, body: str) -> dict:
    root = work / "projects" / ident
    (root / "src").mkdir(parents=True)
    source = root / "src/main.wz"
    source.write_text("package app;\n" + body)
    manifest = root / "lang.toml"
    manifest.write_text(manifest_text())
    return {
        "id": ident,
        "root": root,
        "source": source,
        "manifest": manifest,
        "source_sha256": sha(source),
        "manifest_sha256": sha(manifest),
    }


def first_difference(left: bytes, right: bytes):
    limit = min(len(left), len(right))
    for index in range(limit):
        if left[index] != right[index]:
            return index
    if len(left) != len(right):
        return limit
    return None


def require_identical(left: bytes, right: bytes, label: str):
    offset = first_difference(left, right)
    if offset is not None:
        left_byte = left[offset:offset + 1].hex() or "EOF"
        right_byte = right[offset:offset + 1].hex() or "EOF"
        raise RuntimeError(
            f"{label}: bytes differ at offset {offset}: "
            f"{left_byte} != {right_byte}")


def json_status_ok(result: dict, label: str) -> dict:
    if result["stderr"]:
        raise RuntimeError(
            f"{label}: unexpected stderr: "
            f"{result['stderr'][:1000].decode('utf-8', 'replace')}")
    try:
        payload = json.loads(result["stdout"])
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"{label}: malformed JSON stdout: {error}") from error
    if payload.get("status") != "ok":
        raise RuntimeError(f"{label}: status is not ok: {payload}")
    return payload


def allowed_for_bundle(bundle: Path, toolchain: Path, name: str) -> set[str]:
    allowed = {str((bundle / name).resolve())}
    for tool in TOOLS:
        allowed.add(str((bundle / "toolchain" / tool).resolve()))
        allowed.add(str((toolchain / tool).resolve()))
    return allowed


def traced(recorder, label: str, argv, cwd: Path, env: dict, timeout: float,
           expected_exit: int, trace: Path | None, allowed: set[str] | None,
           binary: bool = False):
    command = [str(value) for value in argv]
    if trace is not None:
        command = [
            str(recorder.strace), "--seccomp-bpf", "-f", "-qq", "-s", "16384",
            "-e", "trace=execve,execveat", "-o", str(trace), *command,
        ]
    record, result = recorder.run(
        label, command, cwd, env, timeout, expected_exit=expected_exit,
        binary=binary)
    info = None
    if trace is not None:
        info = run.trace_check(trace, allowed or set(), label, recorder.parse_trace)
        info["trace_sha256"] = sha(trace)
        record["trace"] = info
    return record, result, info


def _float_class(bits: str, typ: str) -> tuple[str, str]:
    width = 32 if typ == "f32" else 64
    if not isinstance(bits, str) or len(bits) != width // 4:
        raise RuntimeError(f"invalid {typ} bits: {bits!r}")
    raw = bytes.fromhex(bits)
    fmt = ">f" if typ == "f32" else ">d"
    value = struct.unpack(fmt, raw)[0]
    negative = struct.unpack(">I" if typ == "f32" else ">Q", raw)[0] >> (width - 1)
    if math.isnan(value):
        return "nan", "negative" if negative else "positive"
    if math.isinf(value):
        return "infinity", "negative" if negative else "positive"
    if value == 0.0:
        return "zero", "negative" if negative else "positive"
    return "finite", "negative" if negative else "positive"


def strict_value(actual, expected, where: str) -> None:
    if not isinstance(actual, dict) or not isinstance(expected, dict):
        raise RuntimeError(f"{where}: value is not an object")
    typ = expected.get("type")
    if not isinstance(typ, str) or actual.get("type") != typ:
        raise RuntimeError(
            f"{where}: type {actual.get('type')!r} != {typ!r}")
    if typ in ("f32", "f64"):
        category = actual.get("class")
        if category not in ("nan", "infinity", "zero", "finite"):
            raise RuntimeError(f"{where}: invalid float class {category!r}")
        if "class" in expected and actual["class"] != expected["class"]:
            raise RuntimeError(
                f"{where}: class {actual['class']!r} != {expected['class']!r}")
        actual_bits = actual.get("bits")
        actual_sign = actual.get("sign")
        if category == "nan":
            if actual_bits is not None or actual_sign is not None:
                raise RuntimeError(f"{where}: NaN carried bits/sign")
            if "bits" in expected and expected["bits"] is not None:
                raise RuntimeError(f"{where}: expected NaN bits")
            if "sign" in expected and expected["sign"] is not None:
                raise RuntimeError(f"{where}: expected NaN sign")
        else:
            if not isinstance(actual_bits, str):
                raise RuntimeError(f"{where}: missing float bits")
            derived, derived_sign = _float_class(actual_bits, typ)
            if derived != category:
                raise RuntimeError(
                    f"{where}: bits class {derived!r} != {category!r}")
            if actual_sign != derived_sign:
                raise RuntimeError(
                    f"{where}: sign {actual_sign!r} != {derived_sign!r}")
            if "bits" in expected and actual_bits != expected["bits"]:
                raise RuntimeError(
                    f"{where}: bits {actual_bits!r} != {expected['bits']!r}")
            if "sign" in expected and actual_sign != expected["sign"]:
                raise RuntimeError(
                    f"{where}: sign {actual_sign!r} != {expected['sign']!r}")
        if "value" in expected:
            want = expected["value"]
            got = actual.get("value")
            if type(got) is not type(want) or got != want:
                raise RuntimeError(f"{where}: value {got!r} != {want!r}")
        return
    if typ == "bool":
        if actual.get("class") != "bool":
            raise RuntimeError(f"{where}: bool class is {actual.get('class')!r}")
        if type(actual.get("value")) is not bool:
            raise RuntimeError(
                f"{where}: bool value has type {type(actual.get('value'))}")
        if "value" in expected:
            if type(expected["value"]) is not bool:
                raise RuntimeError(f"{where}: expected bool is not bool")
            if actual["value"] != expected["value"]:
                raise RuntimeError(
                    f"{where}: bool {actual['value']!r} != {expected['value']!r}")
        return
    if typ.startswith(("i", "u")):
        got = actual.get("value")
        if type(got) is not int:
            raise RuntimeError(f"{where}: integer value has type {type(got)}")
        if "value" in expected:
            want = expected["value"]
            if type(want) is not int:
                raise RuntimeError(f"{where}: expected integer is not int")
            if got != want:
                raise RuntimeError(f"{where}: integer {got!r} != {want!r}")
        return
    if typ == "Unit":
        if actual.get("value") is not None:
            raise RuntimeError(f"{where}: Unit value is not None")
        if "value" in expected and expected["value"] is not None:
            raise RuntimeError(f"{where}: expected Unit is not None")
        return
    for key, want in expected.items():
        if key == "type":
            continue
        got = actual.get(key)
        if type(got) is not type(want) or got != want:
            raise RuntimeError(f"{where}: {key} {got!r} != {want!r}")


def exact_expected_results(probes: list[dict], expected_result,
                           label: str) -> list[str]:
    """Normalize an independent exact-result expectation.

    ``expected_result`` is a string for one-function cases, a parallel list
    for mixed control cases, or None.  A non-empty probe list without an
    exact expectation is always rejected; no actual metadata/row value is
    used to fill it in.
    """
    if expected_result is None:
        if probes:
            raise RuntimeError(f"{label}: missing exact expected result")
        return []
    if isinstance(expected_result, str):
        values = [expected_result] * len(probes)
    elif isinstance(expected_result, list):
        values = expected_result
    else:
        raise RuntimeError(f"{label}: invalid expected result specification")
    if len(values) != len(probes):
        raise RuntimeError(
            f"{label}: expected-result count {len(values)} != {len(probes)}")
    for value in values:
        if not isinstance(value, str) or not value:
            raise RuntimeError(f"{label}: invalid exact expected result")
    return values


def strict_validate(probes: list[dict], rows, label: str,
                    expected_result) -> list[dict]:
    if not isinstance(rows, list):
        raise RuntimeError(f"{label}: observations are not a list")
    expected_results = exact_expected_results(probes, expected_result, label)
    if len(rows) != len(probes):
        raise RuntimeError(
            f"{label}: observation count {len(rows)} != {len(probes)}")
    values = []
    for index, (probe, row) in enumerate(zip(probes, rows)):
        want_result = expected_results[index]
        where = f"{label}[{index}] {probe.get('function')}"
        if not isinstance(row, dict):
            raise RuntimeError(f"{where}: row is not an object")
        if row.get("function") != probe["function"]:
            raise RuntimeError(
                f"{where}: function {row.get('function')!r} != "
                f"{probe['function']!r}")
        if not run.same_json(row.get("arguments"), probe.get("arguments", [])):
            raise RuntimeError(f"{where}: arguments differ")
        signature = row.get("signature")
        if not isinstance(signature, dict):
            raise RuntimeError(f"{where}: missing signature")
        parameters = [argument["type"] for argument in probe.get("arguments", [])]
        if signature.get("source_name") != probe["function"]:
            raise RuntimeError(f"{where}: signature source_name differs")
        if "result" in probe and probe["result"]["type"] != want_result:
            raise RuntimeError(
                f"{where}: expected table/probe result disagree: "
                f"{want_result!r} != {probe['result']['type']!r}")
        if signature.get("result") != want_result:
            raise RuntimeError(
                f"{where}: signature result {signature.get('result')!r} != "
                f"exact expected {want_result!r}")
        if signature.get("parameters") != parameters:
            raise RuntimeError(
                f"{where}: signature parameters "
                f"{signature.get('parameters')!r} != {parameters!r}")
        if not isinstance(signature.get("generated"), str) or \
                not signature["generated"]:
            raise RuntimeError(f"{where}: missing generated symbol")
        outcome = row.get("outcome")
        if "result" in probe:
            if outcome != "return":
                raise RuntimeError(f"{where}: outcome is {outcome!r}")
            strict_value(row.get("result"), probe["result"], where + ".result")
        elif "error_key" in probe:
            if outcome != "typed":
                raise RuntimeError(f"{where}: outcome is {outcome!r}")
            if row.get("error_key") != probe["error_key"]:
                raise RuntimeError(
                    f"{where}: error key {row.get('error_key')!r} != "
                    f"{probe['error_key']!r}")
            strict_value(
                row.get("error_value"), probe["error_value"],
                where + ".error_value")
        else:
            if outcome != "panic":
                raise RuntimeError(f"{where}: outcome is {outcome!r}")
            if row.get("panic") != probe["panic"]:
                raise RuntimeError(
                    f"{where}: panic {row.get('panic')!r} != "
                    f"{probe['panic']!r}")
        expected_cells = probe.get("cells", [])
        actual_cells = row.get("cells")
        if not isinstance(actual_cells, list):
            raise RuntimeError(f"{where}: cells are not a list")
        if len(actual_cells) != len(expected_cells):
            raise RuntimeError(
                f"{where}: cell count {len(actual_cells)} != "
                f"{len(expected_cells)}")
        for cell_index, (actual, expected) in enumerate(
                zip(actual_cells, expected_cells)):
            strict_value(actual, expected, where + f".cells[{cell_index}]")
        values.append({
            "function": probe["function"],
            "outcome": outcome,
            "expected": probe,
            "expected_result": want_result,
            "actual": row,
            "status": "passed",
        })
    return values


def metadata_check(metadata: dict, probes: list[dict], cli_program: bytes,
                   cli_langrt: bytes, label: str, expected_result) -> dict:
    expected_results = exact_expected_results(probes, expected_result, label)
    if metadata.get("schema") != "selfhost-float-observer/1":
        raise RuntimeError(f"{label}: unexpected metadata schema")
    if metadata.get("backend") != "python" or metadata.get("word_bits") != 64:
        raise RuntimeError(f"{label}: unexpected backend/word_bits")
    if metadata.get("normal_code_sha256") != hashlib.sha256(cli_program).hexdigest():
        raise RuntimeError(f"{label}: metadata code hash differs from CLI")
    if metadata.get("normal_runtime_sha256") != hashlib.sha256(cli_langrt).hexdigest():
        raise RuntimeError(f"{label}: metadata runtime hash differs from CLI")
    functions = metadata.get("functions")
    symbols = metadata.get("symbols")
    if not isinstance(functions, list) or not isinstance(symbols, list):
        raise RuntimeError(f"{label}: metadata lacks functions/symbols")
    text = cli_program.decode("utf-8")
    notes = {"cell_probes": [], "scalar_probes": [],
             "expected_results": expected_results}
    for index, probe in enumerate(probes):
        want_result = expected_results[index]
        name = probe["function"]
        matches = [item for item in functions
                   if item.get("source_name") == name]
        if len(matches) != 1:
            raise RuntimeError(
                f"{label}: {name} has {len(matches)} metadata functions")
        item = matches[0]
        parameters = [argument["type"] for argument in probe.get("arguments", [])]
        if item.get("parameters") != parameters:
            raise RuntimeError(
                f"{label}: {name} metadata parameters "
                f"{item.get('parameters')!r} != {parameters!r}")
        if "result" in probe and probe["result"]["type"] != want_result:
            raise RuntimeError(
                f"{label}: {name} expected table/probe result disagree")
        if item.get("result") != want_result:
            raise RuntimeError(
                f"{label}: {name} metadata result {item.get('result')!r} != "
                f"exact expected {want_result!r}")
        generated = item.get("generated")
        if not isinstance(generated, str) or not generated:
            raise RuntimeError(f"{label}: {name} lacks generated symbol")
        symbol_matches = [entry for entry in symbols
                          if entry.get("generated") == generated]
        if len(symbol_matches) != 1:
            raise RuntimeError(
                f"{label}: {name} generated symbol is not unique")
        symbol = symbol_matches[0]
        if symbol.get("source_name") != name or \
                symbol.get("symbol") != item.get("symbol"):
            raise RuntimeError(f"{label}: {name} symbol metadata differs")
        definitions = re.findall(
            rf"^def {re.escape(generated)}\(", text, re.MULTILINE)
        if len(definitions) != 1:
            raise RuntimeError(
                f"{label}: CLI program.py has {len(definitions)} "
                f"definitions for {generated}")
        if any(argument.get("cell") for argument in probe.get("arguments", [])):
            if item.get("status") != "not_covered":
                raise RuntimeError(
                    f"{label}: cell function {name} metadata is "
                    f"{item.get('status')!r}")
            if item.get("reason") not in (None, "unsupported_signature"):
                raise RuntimeError(
                    f"{label}: cell function {name} reason is "
                    f"{item.get('reason')!r}")
            notes["cell_probes"].append({
                "function": name,
                "status": item.get("status"),
                "reason": item.get("reason"),
            })
        elif item.get("status") != "covered":
            raise RuntimeError(
                f"{label}: scalar function {name} is not covered")
        else:
            notes["scalar_probes"].append(name)
    return notes


def python_observe_cli(recorder, root: Path, python: Path, observation_dir: Path,
                       probes_path: Path, env: dict, timeout: float,
                       strace: Path, trace_path: Path, label: str):
    script = root / "tests/python/observe_floating.py"
    argv = [python, "-B", "-s", script, observation_dir, probes_path]
    record, result, trace = traced(
        recorder, label, argv, observation_dir, env, timeout, 0,
        trace_path, {str(python.resolve())})
    if result["stderr"]:
        raise RuntimeError(
            f"{label}: unexpected stderr: "
            f"{result['stderr'][:1000].decode('utf-8', 'replace')}")
    try:
        rows = json.loads(result["stdout"])
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError(f"{label}: malformed observations: {error}") from error
    return record, rows, trace


def run_small_controls(work: Path, recorder, harness, candidate_bundle: Path,
                       python: Path, env: dict,
                       timeout: float) -> list[dict]:
    controls = []
    control_dir = work / "controls"
    control_dir.mkdir()

    def add(name: str, detected: bool, **extra):
        controls.append({"name": name, "detected": detected, **extra})

    i32_probe = {
        "function": "probe",
        "arguments": [],
        "result": {"type": "i32", "value": 0},
    }
    i32_row = {
        "function": "probe",
        "signature": {
            "source_name": "probe", "symbol": 1, "generated": "_f1",
            "result": "i32", "parameters": [], "status": "covered",
        },
        "arguments": [],
        "outcome": "return",
        "result": {"type": "i32", "value": 0},
        "cells": [],
    }
    cell_probe = {
        "function": "probe",
        "arguments": [
            {"type": "i32", "value": 0, "cell": True},
            {"type": "f32", "bits": "3f800000", "cell": True},
        ],
        "result": {"type": "f32", "bits": "3f000000"},
        "cells": [
            {"type": "i32", "value": 12},
            {"type": "f32", "bits": "40000000"},
        ],
    }
    cell_row = {
        "function": "probe",
        "signature": {
            "source_name": "probe", "symbol": 2, "generated": "_f2",
            "result": "f32", "parameters": ["i32", "f32"],
            "status": "not_covered", "reason": "unsupported_signature",
        },
        "arguments": [
            {"type": "i32", "value": 0, "cell": True},
            {"type": "f32", "bits": "3f800000", "cell": True},
        ],
        "outcome": "return",
        "result": {
            "type": "f32", "class": "finite", "bits": "3f000000",
            "sign": "positive",
        },
        "cells": [
            {"type": "i32", "value": 12},
            {
                "type": "f32", "class": "finite", "bits": "40000000",
                "sign": "positive",
            },
        ],
    }
    bool_probe = {
        "function": "probe",
        "arguments": [],
        "result": {"type": "bool", "value": True},
    }
    bool_row = {
        "function": "probe",
        "signature": {
            "source_name": "probe", "symbol": 3, "generated": "_f3",
            "result": "bool", "parameters": [], "status": "covered",
        },
        "arguments": [],
        "outcome": "return",
        "result": {"type": "bool", "class": "bool", "value": True},
        "cells": [],
    }
    zero_probe = {
        "function": "probe",
        "arguments": [],
        "result": {
            "type": "f32", "class": "zero", "bits": "80000000",
            "sign": "negative",
        },
    }
    zero_row = {
        "function": "probe",
        "signature": {
            "source_name": "probe", "symbol": 4, "generated": "_f4",
            "result": "f32", "parameters": [], "status": "covered",
        },
        "arguments": [],
        "outcome": "return",
        "result": {
            "type": "f32", "class": "zero", "bits": "80000000",
            "sign": "negative",
        },
        "cells": [],
    }
    nan_probe = {
        "function": "probe",
        "arguments": [],
        "result": {
            "type": "f32", "class": "nan", "bits": None, "sign": None,
        },
    }
    nan_row = {
        "function": "probe",
        "signature": {
            "source_name": "probe", "symbol": 5, "generated": "_f5",
            "result": "f32", "parameters": [], "status": "covered",
        },
        "arguments": [],
        "outcome": "return",
        "result": {
            "type": "f32", "class": "nan", "bits": None, "sign": None,
        },
        "cells": [],
    }
    probes = [i32_probe, cell_probe, bool_probe, zero_probe, nan_probe]
    rows = [i32_row, cell_row, bool_row, zero_row, nan_row]
    script = control_dir / "emit_rows.py"
    script.write_text(
        "import json\nprint(json.dumps(" + repr(rows) + "))\n")
    record, result = recorder.run(
        "control-observation-json", [python, "-B", "-s", script],
        control_dir, env, timeout, expected_exit=0)
    emitted = json.loads(result["stdout"])
    control_results = [probe["result"]["type"] for probe in probes]
    try:
        strict_validate(probes, emitted, "control-valid", control_results)
        valid = True
    except RuntimeError:
        valid = False
    add("valid-observation-accepted", valid)

    def mutation_detected(name: str, changed_rows):
        try:
            strict_validate(probes, changed_rows, name, control_results)
            add(name, False)
        except RuntimeError:
            add(name, True)

    wrong_value = json.loads(json.dumps(rows))
    wrong_value[0]["result"]["value"] = 1
    mutation_detected("wrong-i32-value-rejected", wrong_value)

    wrong_cell = json.loads(json.dumps(rows))
    wrong_cell[1]["cells"][0]["value"] = 13
    mutation_detected("wrong-cell-value-rejected", wrong_cell)

    wrong_signature = json.loads(json.dumps(rows))
    wrong_signature[1]["signature"]["parameters"] = ["i32", "f64"]
    mutation_detected("wrong-signature-rejected", wrong_signature)

    wrong_result_type = json.loads(json.dumps(rows))
    wrong_result_type[1]["result"] = {
        "type": "bool", "class": "bool", "value": False,
    }
    mutation_detected("wrong-result-type-rejected", wrong_result_type)

    bool_as_int = json.loads(json.dumps(rows))
    bool_as_int[2]["result"]["value"] = 1
    mutation_detected("bool-as-int-rejected", bool_as_int)

    int_as_bool = json.loads(json.dumps(rows))
    int_as_bool[0]["result"]["value"] = True
    mutation_detected("int-as-bool-rejected", int_as_bool)

    wrong_zero_sign = json.loads(json.dumps(rows))
    wrong_zero_sign[3]["result"]["bits"] = "00000000"
    wrong_zero_sign[3]["result"]["sign"] = "positive"
    mutation_detected("negative-zero-sign-rejected", wrong_zero_sign)

    nan_payload = json.loads(json.dumps(rows))
    nan_payload[4]["result"]["bits"] = "7fc00000"
    mutation_detected("nan-payload-bits-rejected", nan_payload)

    mutation_detected("missing-observation-rejected", rows[:2])
    mutation_detected("extra-observation-rejected", rows + [rows[0]])

    control_project = make_project(
        work, "control-cli-bytes", "func main() i32 {return 0;}")
    cli_dir = control_dir / "cli-bytes"
    check_argv = [
        candidate_bundle / "woozyc", "check", "--project",
        control_project["manifest"], "--backend", "python", "--format", "json",
    ]
    check_record, check_result = recorder.run(
        "control-cli-bytes-check", check_argv, control_dir, env, timeout, 0)
    json_status_ok(check_result, "control-cli-bytes-check")
    build_argv = [
        candidate_bundle / "woozyc", "build", "--project",
        control_project["manifest"], "--backend", "python", "--out-dir",
        cli_dir, "--format", "json",
    ]
    build_record, build_result = recorder.run(
        "control-cli-bytes-build", build_argv, control_dir, env, timeout, 0)
    json_status_ok(build_result, "control-cli-bytes-build")
    original = (cli_dir / "program.py").read_bytes()
    copied = cli_dir / "program-copy.py"
    copied.write_bytes(original)
    mutated = cli_dir / "program-mutated.py"
    mutated.write_bytes(original + b"# control\n")
    same = original == copied.read_bytes()
    different = original == mutated.read_bytes()
    add("cli-bytes-unequal-rejected", same and not different,
        original_sha256=hashlib.sha256(original).hexdigest(),
        mutated_sha256=sha(mutated),
        equal_when_same=same, equal_when_different=different,
        check=check_record, build=build_record)
    return controls


def expected_exit(expected: dict) -> int:
    if "panic" in expected:
        return 101
    if "typed" in expected:
        return 1
    return expected.get("main_return", 0)


def check_program_result(result: dict, expected: dict, label: str) -> None:
    if result["stdout"]:
        raise RuntimeError(f"{label}: unexpected stdout")
    if "panic" in expected:
        want = ("LANG_PANIC: " + expected["panic"] + "\n").encode()
        if result["stderr"] != want:
            raise RuntimeError(
                f"{label}: panic stderr {result['stderr']!r} != {want!r}")
    elif "typed" in expected:
        want = ("LANG_TYPED_ERROR: " + expected["typed"] + "\n").encode()
        if result["stderr"] != want:
            raise RuntimeError(
                f"{label}: typed stderr {result['stderr']!r} != {want!r}")
    elif result["stderr"]:
        raise RuntimeError(
            f"{label}: unexpected stderr: "
            f"{result['stderr'][:1000].decode('utf-8', 'replace')}")


def run_project(work: Path, root: Path, recorder, candidate_bundle: Path,
                driver_bundle: Path, python: Path, strace: Path,
                args, harness, ident: str, body: str, expected: dict,
                probes: list[dict]) -> dict:
    expected_result = PROBE_RESULT_TYPES.get(ident)
    if probes and expected_result is None:
        raise RuntimeError(f"{ident}: no independent exact result type")
    project = make_project(work, ident, body)
    run_dir = work / "runs" / ident
    run_dir.mkdir(parents=True)
    result = {
        "id": ident,
        "status": "failed",
        "source_sha256": project["source_sha256"],
        "manifest_sha256": project["manifest_sha256"],
        "expected": expected,
        "expected_result": expected_result,
        "signature_checked_fields": [
            "source_name", "generated", "parameters", "result"
        ] if probes else [],
        "signature_unchecked_fields": [
            "parameter_modes", "throws"
        ] if probes else [],
        "probe_count": len(probes),
        "observations": 0,
        "values": [],
    }
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONNOUSERSITE"] = "1"
    try:
        check_argv = [
            candidate_bundle / "woozyc", "check", "--project",
            project["manifest"], "--backend", "python", "--format", "json",
        ]
        check_record, check_result, check_trace = traced(
            recorder, f"{ident}-cli-check", check_argv, run_dir, env,
            args.command_timeout, 0, run_dir / "cli-check.strace",
            allowed_for_bundle(candidate_bundle, args.toolchain.resolve(),
                               "woozyc"))
        json_status_ok(check_result, f"{ident}-cli-check")

        cli_dir = run_dir / "cli"
        build_argv = [
            candidate_bundle / "woozyc", "build", "--project",
            project["manifest"], "--backend", "python", "--out-dir", cli_dir,
            "--format", "json",
        ]
        build_record, build_result, build_trace = traced(
            recorder, f"{ident}-cli-build", build_argv, run_dir, env,
            args.command_timeout, 0, run_dir / "cli-build.strace",
            allowed_for_bundle(candidate_bundle, args.toolchain.resolve(),
                               "woozyc"))
        json_status_ok(build_result, f"{ident}-cli-build")
        cli_program = cli_dir / "program.py"
        cli_langrt = cli_dir / "langrt.py"
        if not cli_program.is_file() or not cli_langrt.is_file():
            raise RuntimeError(f"{ident}: CLI did not publish Python files")

        driver_out = run_dir / "driver-out"
        driver_argv = [
            driver_bundle / "driver", "--project", project["manifest"],
            "--stdlib", driver_bundle / "stdlib", "--resources",
            driver_bundle, "--out", driver_out, "--backend", "python",
        ]
        driver_record, driver_result, driver_trace = traced(
            recorder, f"{ident}-driver", driver_argv, run_dir, env,
            args.command_timeout, 0, run_dir / "driver.strace",
            allowed_for_bundle(driver_bundle, args.toolchain.resolve(),
                               "driver"))
        json_status_ok(driver_result, f"{ident}-driver")
        normal = driver_out / "normal"
        driver_program = normal / "program.py"
        driver_langrt = normal / "langrt.py"
        driver_metadata = normal / "metadata.json"
        metadata_path = driver_out / "float-metadata.json"
        for path in (driver_program, driver_langrt, driver_metadata,
                     metadata_path):
            if not path.is_file():
                raise RuntimeError(f"{ident}: driver omitted {path.name}")
        if driver_metadata.read_bytes() != metadata_path.read_bytes():
            raise RuntimeError(f"{ident}: driver metadata copies differ")

        cli_program_bytes = cli_program.read_bytes()
        cli_langrt_bytes = cli_langrt.read_bytes()
        driver_program_bytes = driver_program.read_bytes()
        driver_langrt_bytes = driver_langrt.read_bytes()
        require_identical(
            driver_program_bytes, cli_program_bytes, f"{ident}: program.py")
        require_identical(
            driver_langrt_bytes, cli_langrt_bytes, f"{ident}: langrt.py")

        metadata = json.loads(metadata_path.read_text())
        metadata_notes = metadata_check(
            metadata, probes, cli_program_bytes, cli_langrt_bytes, ident,
            expected_result)

        program_argv = [python, "-B", "-s", cli_program]
        program_record, program_result, program_trace = traced(
            recorder, f"{ident}-program", program_argv, cli_dir, env,
            args.command_timeout, expected_exit(expected),
            run_dir / "program.strace", {str(python.resolve())})
        check_program_result(program_result, expected, f"{ident}-program")

        observed_rows = None
        observe_record = None
        observe_trace = None
        observe_dir = None
        probes_path = None
        if probes:
            observe_dir = run_dir / "observe-cli"
            observe_dir.mkdir()
            (observe_dir / "program.py").write_bytes(cli_program_bytes)
            (observe_dir / "langrt.py").write_bytes(cli_langrt_bytes)
            (observe_dir / "metadata.json").write_bytes(
                metadata_path.read_bytes())
            probes_path = run_dir / "probes.json"
            probes_path.write_text(json.dumps(probes, indent=2) + "\n")
            observe_record, observed_rows, observe_trace = python_observe_cli(
                recorder, root, python, observe_dir, probes_path, env,
                args.command_timeout, strace, run_dir / "observe.strace",
                f"{ident}-observe")
            result["values"] = strict_validate(
                probes, observed_rows, f"{ident}-observe", expected_result)
            result["observations"] = len(observed_rows)
            if result["observations"] != len(probes):
                raise RuntimeError(
                    f"{ident}: observation count "
                    f"{result['observations']} != {len(probes)}")

        result.update({
            "status": "passed",
            "project": {
                "source": str(project["source"]),
                "manifest": str(project["manifest"]),
                "source_sha256": project["source_sha256"],
                "manifest_sha256": project["manifest_sha256"],
            },
            "check": check_record,
            "build": build_record,
            "driver": driver_record,
            "program": program_record,
            "observe": observe_record,
            "check_trace": check_trace,
            "build_trace": build_trace,
            "driver_trace": driver_trace,
            "program_trace": program_trace,
            "observe_trace": observe_trace,
            "cli_tree": run.tree_hashes(cli_dir),
            "driver_normal_tree": run.tree_hashes(normal),
            "metadata_sha256": sha(metadata_path),
            "metadata_copy_sha256": sha(driver_metadata),
            "metadata_notes": metadata_notes,
            "probes_sha256": sha(probes_path) if probes_path else None,
            "observe_inputs": {
                "program_sha256": sha(observe_dir / "program.py"),
                "langrt_sha256": sha(observe_dir / "langrt.py"),
                "metadata_sha256": sha(observe_dir / "metadata.json"),
            } if observe_dir else None,
            "driver_program_sha256": sha(driver_program),
            "driver_langrt_sha256": sha(driver_langrt),
            "cli_program_sha256": sha(cli_program),
            "cli_langrt_sha256": sha(cli_langrt),
            "program_identical": True,
            "langrt_identical": True,
        })
    except run.CommandError as error:
        result["failed_record"] = error.record
        result["error"] = str(error)
        result["traceback"] = traceback.format_exc()
    except Exception as error:
        result["error"] = str(error)
        result["traceback"] = traceback.format_exc()
    return result


def source_comparison(root: Path, candidate: dict) -> dict:
    root_tree = run.wz_tree(root / "selfhost/src/wzcc")
    proof_source = Path(candidate.get("proof_source", ""))
    candidate_tree = run.wz_tree(proof_source / "src/wzcc")
    differences = []
    for name in sorted(set(root_tree) | set(candidate_tree)):
        if root_tree.get(name) == candidate_tree.get(name):
            continue
        item = {
            "path": name,
            "root": root_tree.get(name),
            "candidate": candidate_tree.get(name),
        }
        left = root / "selfhost/src/wzcc" / name
        right = proof_source / "src/wzcc" / name
        if left.is_file() and right.is_file():
            item["whitespace_only"] = (
                left.read_bytes().split() == right.read_bytes().split())
        differences.append(item)
    return {
        "root_tree_sha256": run.aggregate_hashes(root_tree),
        "candidate_tree_sha256": run.aggregate_hashes(candidate_tree),
        "differences": differences,
        "all_differences_whitespace_only": all(
            item.get("whitespace_only") for item in differences),
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--driver-binary", type=Path, required=True)
    parser.add_argument("--driver-build-evidence", type=Path, required=True)
    parser.add_argument("--candidate-cli", type=Path, required=True)
    parser.add_argument("--accepted-proof", type=Path, required=True)
    parser.add_argument("--woozyc", type=Path, default=run.DEFAULT_BOOTSTRAP)
    parser.add_argument("--python", type=Path, default=DEFAULT_PYTHON)
    parser.add_argument("--toolchain", type=Path, default=run.DEFAULT_TOOLCHAIN)
    parser.add_argument("--strace", type=Path, default=run.DEFAULT_STRACE)
    parser.add_argument("--runtime-dir", type=Path)
    parser.add_argument("--command-timeout", type=float, default=180)
    parser.add_argument("--controls-only", action="store_true")
    args = parser.parse_args(argv)
    if not math.isfinite(args.command_timeout) or args.command_timeout <= 0:
        parser.error("--command-timeout must be finite and positive")
    root = args.root.resolve()
    required = [
        root / "selfhost/src/wzcc/nativeverify/verify_ops.wz",
        root / "tests/python/floating_cases.py",
        root / "tests/python/observe_floating.py",
        root / "stdlib", root / "runtime/python/unicode18.py",
        root / "runtime/python/langrt.py",
        args.candidate_cli.resolve(), args.driver_binary.resolve(),
        args.woozyc.resolve(), args.python.resolve(),
        args.strace.resolve(), args.accepted_proof.resolve(),
        args.driver_build_evidence.resolve(),
    ]
    required.extend(args.toolchain.resolve() / tool for tool in TOOLS)
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        parser.error("missing required input: " + ", ".join(missing))
    work = run.prepare_work_dir(args.work_dir.resolve())
    recorder = run.Recorder(work)
    harness = run.Harness(root)
    recorder.run_process = harness.run_process
    recorder.parse_trace = harness.parse_trace
    recorder.strace = args.strace.resolve()
    run.HarnessActive.float_expected = harness.float_expected
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONNOUSERSITE"] = "1"
    candidate = args.candidate_cli.resolve()
    driver = args.driver_binary.resolve()
    bootstrap = args.woozyc.resolve()
    python = args.python.absolute()
    python_resolved = python.resolve()
    toolchain = args.toolchain.resolve()
    proof = args.accepted_proof.resolve()
    started = time.monotonic()
    report = {
        "operation_id": "SH10v-python-floating-supplement14-20261114",
        "contract": "compiler-dev/1",
        "status": "failed",
        "work_dir": str(work),
        "harness_source_sha256": sha(Path(__file__).resolve()),
        "run_py_sha256": sha(RUN_PATH),
        "root": str(root),
        "root_identity": run.root_identity(root),
        "python": {"path": str(python), "resolved": str(python_resolved),
                   "version": None},
        "signature_scope": {
            "checked_fields": ["source_name", "generated",
                               "parameters", "result"],
            "unchecked_fields": ["parameter_modes", "throws"],
            "unchecked_reason": (
                "ScalarSignature/python_functions do not serialize parameter "
                "modes or throws; no independent metadata check is possible."),
            "behavior_checks": (
                "Real CLI program.py outcomes, Cells, TypedError and Panic "
                "are checked separately."),
            "expected_result_source": (
                "Explicit case table from original "
                "tests/python/floating_cases.py supplemental(): evaluation=i32, "
                "f32-*=f32, f64-*=f64."),
        },
        "candidate": None,
        "driver": None,
        "resources": {},
        "inventory": {},
        "controls": [],
        "runs": [],
        "not_run": [
            "existing 9878 numeric observations were not rerun",
            "full 116-source CLI build was not run",
            "Native supplemental observations were not requested",
            "stage2/stage3 were not started",
        ],
        "failures": [],
    }
    try:
        python_record, python_result = recorder.run(
            "python-version", [python, "--version"], work, env, 30,
            expected_exit=0)
        version = python_result["stdout"].decode().strip()
        report["python"]["version"] = version
        if version != "Python 3.12.15":
            raise RuntimeError(f"unexpected Python: {version}")
        report["python"]["record"] = python_record
        report["python"]["records"] = {
            "executable_sha256": sha(python),
            "resolved_sha256": sha(python_resolved),
        }

        candidate_identity = run.canonical_identity(candidate, proof, root)
        report["candidate"] = candidate_identity
        if candidate_identity.get("role") != "canonical":
            raise RuntimeError(
                f"candidate provenance is not canonical: "
                f"{candidate_identity.get('reasons')}")
        report["candidate_source_comparison"] = source_comparison(
            root, candidate_identity)
        report["candidate_sha256"] = sha(candidate)
        report["candidate_source_sha256"] = candidate_identity.get(
            "proof_source_tree_sha256")

        evidence = run.validate_driver_evidence(
            driver, args.driver_build_evidence.resolve(), root, bootstrap)
        report["driver"] = {
            "binary": str(driver),
            "sha256": sha(driver),
            "evidence": evidence,
            "evidence_sha256": evidence.get("file_sha256"),
        }
        if report["driver"]["sha256"] != \
                "2a90a73945b95e4e1b3026d12df96dd9c2067ffc0987deb2cff9a083a4386315":
            raise RuntimeError("retained driver SHA is not 2a90a739")
        for path in (root / "stdlib", root / "runtime/python",
                     candidate.parent / "stdlib",
                     candidate.parent / "runtime/python"):
            if not path.is_dir():
                raise RuntimeError(f"missing resource directory: {path}")
        runtime_dir = run.find_runtime_dir(bootstrap, args.runtime_dir)
        report["resources"]["runtime_dir"] = str(runtime_dir)
        report["resources"]["runtime_tree"] = run.tree_hashes(runtime_dir)
        report["resources"]["driver_stdlib"] = run.tree_hashes(
            root / "stdlib")
        report["resources"]["driver_python_runtime"] = run.tree_hashes(
            root / "runtime/python")
        report["resources"]["candidate_stdlib"] = run.tree_hashes(
            candidate.parent / "stdlib")
        report["resources"]["candidate_python_runtime"] = run.tree_hashes(
            candidate.parent / "runtime/python")

        supplemental = list(harness.floating_cases.supplemental())
        report["inventory"] = {
            "projects": len(supplemental),
            "observations": sum(len(item[3]) for item in supplemental),
            "expected_projects": EXPECTED_PROJECTS,
            "expected_observations": EXPECTED_OBSERVATIONS,
            "ids": [item[0] for item in supplemental],
            "probe_counts": {item[0]: len(item[3]) for item in supplemental},
            "expected_results": PROBE_RESULT_TYPES,
            "source": str(root / "tests/python/floating_cases.py"),
            "source_sha256": sha(root / "tests/python/floating_cases.py"),
            "observer_source_sha256": sha(
                root / "tests/python/observe_floating.py"),
        }
        if len(supplemental) != EXPECTED_PROJECTS:
            raise RuntimeError(
                f"supplemental project count {len(supplemental)} != "
                f"{EXPECTED_PROJECTS}")
        if report["inventory"]["observations"] != EXPECTED_OBSERVATIONS:
            raise RuntimeError(
                f"supplemental observation count "
                f"{report['inventory']['observations']} != "
                f"{EXPECTED_OBSERVATIONS}")
        expected_ids = [
            "old-string-float", "evaluation",
            "f32-return-defer", "f32-typed-defer", "f32-panic-skip",
            "f32-float-zero-in-defer", "f32-ordinary-101",
            "f32-unhandled-typed", "f64-return-defer", "f64-typed-defer",
            "f64-panic-skip", "f64-float-zero-in-defer",
            "f64-ordinary-101", "f64-unhandled-typed",
        ]
        if [item[0] for item in supplemental] != expected_ids:
            raise RuntimeError("supplemental project order/ids changed")
        probe_ids = {item[0] for item in supplemental if item[3]}
        if set(PROBE_RESULT_TYPES) != probe_ids:
            raise RuntimeError(
                f"exact-result table {sorted(PROBE_RESULT_TYPES)} != "
                f"probe cases {sorted(probe_ids)}")
        if any(value not in ("i32", "f32", "f64")
               for value in PROBE_RESULT_TYPES.values()):
            raise RuntimeError("exact-result table has an unexpected type")

        driver_bundle = run.prepare_bundle(
            work, "driver-bundle", driver, "driver", root / "stdlib",
            runtime_dir, root / "runtime/python", toolchain)
        candidate_bundle = run.prepare_bundle(
            work, "candidate-bundle", candidate, "woozyc",
            candidate.parent / "stdlib", runtime_dir,
            candidate.parent / "runtime/python", toolchain)
        report["resources"]["driver_bundle"] = run.tree_hashes(driver_bundle)
        report["resources"]["candidate_bundle"] = run.tree_hashes(
            candidate_bundle)
        report["resources"]["driver_binary_sha256"] = sha(
            driver_bundle / "driver")
        report["resources"]["candidate_binary_sha256"] = sha(
            candidate_bundle / "woozyc")

        report["controls"] = run_small_controls(
            work, recorder, harness, candidate_bundle, python, env,
            args.command_timeout)
        if not all(control["detected"] for control in report["controls"]):
            raise RuntimeError(
                f"small control was not detected: {report['controls']}")
        if args.controls_only:
            report["status"] = "passed" if all(
                control["detected"] for control in report["controls"]) else "failed"
            return finish(report, recorder, started)

        for ident, body, expected, probes in supplemental:
            row = run_project(
                work, root, recorder, candidate_bundle, driver_bundle, python,
                args.strace.resolve(), args, harness, ident, body, expected,
                probes)
            report["runs"].append(row)
            if row["status"] != "passed":
                report["failures"].append(
                    f"{ident}: {row.get('error', 'failed')}")

        report["inventory"]["executed_projects"] = len(report["runs"])
        report["inventory"]["executed_observations"] = sum(
            row.get("observations", 0) for row in report["runs"])
        if len(report["runs"]) != EXPECTED_PROJECTS:
            raise RuntimeError(
                f"executed {len(report['runs'])} projects != "
                f"{EXPECTED_PROJECTS}")
        if report["inventory"]["executed_observations"] != \
                EXPECTED_OBSERVATIONS:
            raise RuntimeError(
                f"executed {report['inventory']['executed_observations']} "
                f"observations != {EXPECTED_OBSERVATIONS}")
        if not all(row["status"] == "passed" for row in report["runs"]):
            raise RuntimeError(
                f"{len(report['failures'])} supplemental projects failed")
        if not all(record["process_ok"] for record in recorder.records):
            raise RuntimeError("a command did not clean up")
        if any(record["timed_out"] for record in recorder.records):
            raise RuntimeError("a command timed out")

        after = {
            "candidate_binary": sha(candidate),
            "driver_binary": sha(driver),
            "driver_bundle": run.tree_hashes(driver_bundle),
            "candidate_bundle": run.tree_hashes(candidate_bundle),
            "root_verify_ops": sha(
                root / "selfhost/src/wzcc/nativeverify/verify_ops.wz"),
            "supplemental_source": sha(
                root / "tests/python/floating_cases.py"),
            "observer_source": sha(
                root / "tests/python/observe_floating.py"),
        }
        expected_after = {
            "candidate_binary": report["candidate_sha256"],
            "driver_binary": report["driver"]["sha256"],
            "driver_bundle": report["resources"]["driver_bundle"],
            "candidate_bundle": report["resources"]["candidate_bundle"],
            "root_verify_ops": report["root_identity"]["verify_ops_sha256"],
            "supplemental_source": report["inventory"]["source_sha256"],
            "observer_source": report["inventory"]["observer_source_sha256"],
        }
        report["hashes_after"] = after
        report["hash_stability"] = {
            name: after[name] == expected_after[name]
            for name in after
        }
        if not all(report["hash_stability"].values()):
            raise RuntimeError(
                f"input/output hash changed: {report['hash_stability']}")
        report["status"] = "passed"
    except run.CommandError as error:
        report["status"] = "failed"
        report["error"] = str(error)
        report["failed_record"] = error.record
        report["traceback"] = traceback.format_exc()
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
        report["traceback"] = traceback.format_exc()
    return finish(report, recorder, started)


def finish(report: dict, recorder, started: float) -> int:
    report["elapsed_s"] = round(time.monotonic() - started, 3)
    report["records"] = recorder.records
    report["process_cleanup_all"] = all(
        record["process_ok"] for record in recorder.records)
    report["timed_out_all"] = [
        record["label"] for record in recorder.records if record["timed_out"]]
    report["abandoned_pipes_all"] = [
        record["label"] for record in recorder.records
        if record["abandoned_pipes"]]
    path = Path(report["work_dir"]) / "results.json"
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({
        "status": report["status"],
        "error": report.get("error"),
        "work_dir": report["work_dir"],
        "inventory": report.get("inventory"),
        "controls": len(report.get("controls", [])),
        "runs": len(report.get("runs", [])),
        "failures": report.get("failures"),
        "elapsed_s": report["elapsed_s"],
        "results": str(path),
    }, indent=2))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
