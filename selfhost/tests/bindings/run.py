#!/usr/bin/env python3
"""Differential tests for app/wzcc/bindings (SH05j).

The candidate is the real Wz module built by the frozen stage0 compiler at O0
and O2. The oracle is a test-only C++ program linked against the already-built
Root liblang_modules.a/liblang_frontend.a: it loads a generated project with
src/modules/project.cpp, which is the reference implementation of both binding
schema readers. Neither side is a Python reimplementation, and the Wz module
never invokes the old compiler.

The accepted dependencies (jsondoc, model, source, input, paths, digest) are
symlinked from the accepted Root tree into an external test project; only
`bindings` points at this worktree. Every published bundle/declaration/C record,
every path/identity cache entry and every diagnostic (code, phase, severity,
message, primary snapshot path/bytes, related spans) is compared byte for byte
through a length-prefixed token tree.

The standalone API has no import-site span for a missing binding file. That
one filesystem failure compares classification separately; Root project tests
check the complete import diagnostic. All JSON failures, including trailing
content, compare the full real C++ message and span.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
DRIVER = HERE / "src" / "main.wz"
MANIFEST = HERE / "lang.toml"
ORACLE = HERE / "oracle.cpp"
DEFAULT_STAGE0 = REPO / "build/woozy-stdlib-native/woozyc"
DEFAULT_ACCEPTED = REPO
TOOLCHAIN = REPO.parent / "toolchains"
DEFAULT_CLANG = TOOLCHAIN / "llvm-22.1.8/bin/clang++"
DEFAULT_WORK = REPO / "build/bindings-tests"
ACCEPTED_MODULES = ("model", "source", "input", "paths", "digest", "jsondoc")
TIMEOUT = 600.0
TOOLCHAIN_LIBRARY_PATH = ":".join((
    str(TOOLCHAIN / "llvm-22.1.8/lib"),
    str(TOOLCHAIN / "libxml2-legacy-2.13.9/usr/lib")))

TEMP_DIR: Path | None = None


def environment() -> dict[str, str]:
    env = dict(os.environ)
    previous = env.get("LD_LIBRARY_PATH")
    env["LD_LIBRARY_PATH"] = (
        TOOLCHAIN_LIBRARY_PATH + (":" + previous if previous else ""))
    # /tmp can hit the user's tmpfs quota; clang and the stage0 linker need a
    # writable intermediate directory under the work tree instead.
    if TEMP_DIR is not None:
        env["TMPDIR"] = str(TEMP_DIR)
    return env


class Fail(Exception):
    pass


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    raise SystemExit(1)


def run(argv: list[str], *, stdin: bytes | None = None,
        cwd: Path | None = None) -> subprocess.CompletedProcess[bytes]:
    print("+", " ".join(argv), flush=True)
    try:
        return subprocess.run(argv, input=stdin, cwd=cwd, env=environment(),
                              capture_output=True, check=False,
                              timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        fail(f"timed out: {' '.join(argv)}")
    except OSError as error:
        fail(f"{error}: {' '.join(argv)}")
    raise AssertionError("unreachable")


# ---------------------------------------------------------------------------
# Token tree (see src/main.wz and oracle.cpp).

def parse_tree(data: bytes):
    """Parse the length-prefixed tree. Lengths count UTF-8 bytes."""

    def parse(index: int):
        lead = data[index:index + 1]
        if lead == b"s":
            colon = data.index(b":", index)
            length = int(data[index + 1:colon])
            start = colon + 1
            return (data[start:start + length].decode(
                "utf-8", "surrogateescape"), start + length)
        if lead == b"n":
            end = index + 1
            while end < len(data) and 48 <= data[end] <= 57:
                end += 1
            return int(data[index + 1:end]), end
        if lead == b"[":
            index += 1
            items = []
            while data[index:index + 1] != b"]":
                item, index = parse(index)
                items.append(item)
            return items, index + 1
        if lead == b"{":
            index += 1
            items = []
            while data[index:index + 1] != b"}":
                item, index = parse(index)
                items.append(item)
            return items, index + 1
        if lead == b"t":
            return True, index + 1
        if lead == b"f":
            return False, index + 1
        if lead == b"z":
            return None, index + 1
        raise Fail(f"bad token at {index}: {data[index:index + 20]!r}")

    value, end = parse(0)
    if end != len(data):
        raise Fail(f"trailing tokens: {data[end:end + 40]!r}")
    return value


def decode(data: bytes) -> str:
    return data.decode("utf-8", "surrogateescape")


def short(value, limit: int = 600) -> str:
    text = repr(value)
    return text if len(text) <= limit else text[:limit] + "..."


def same(label: str, actual, want) -> None:
    if actual != want:
        raise Fail(f"{label}\n  want {short(want)}\n  got  {short(actual)}")


# ---------------------------------------------------------------------------
# Fixtures and mutations.

PY_FULL = (REPO / "contracts" / "python_bindings.example.json").read_text(
    encoding="utf-8")
C_FIXTURE = (REPO / "tests" / "native" / "c_ffi" / "bindings" /
             "fixture.json").read_text(encoding="utf-8")
C_EVENTS = (HERE / "fixtures" / "c_events.json").read_text(encoding="utf-8")
C_OTHER = (HERE / "fixtures" / "c_other.json").read_text(encoding="utf-8")

PY_SMALL = (
    '{"schema_version":1,"contract":"compiler-dev/1",'
    '"review_status":"draft_requires_conformance",'
    '"allowed_backends":["python"],'
    '"package_requirements":[{"package":"host","version_policy":"test"}],'
    '"modules":[{"logical_path":"py","host_module":"host",'
    '"provider":"python_package"}],'
    '"types":[{"id":"host.Thing","module":"py","name":"Thing",'
    '"kind":"host_reference","host_attribute":["Thing"],'
    '"copy":"shared_reference"}],'
    '"values":[{"module":"py","name":"v","type":"i64",'
    '"host_attribute":["v"]}],'
    '"callables":[{"id":"host.run","module":"py","name":"run",'
    '"call_kind":"function","owner_type":null,"host_attribute":["run"],'
    '"parameters":[{"name":"x","type":"i64",'
    '"kind":"positional_or_keyword","optional":false,"mode":"ordinary",'
    '"escapes":"no"}],"return_type":"Unit","error_type":null,'
    '"ownership":{"return":"unit","storage":"no_tensor_storage"},'
    '"effects":[],"receiver_escapes":"not_applicable"}],'
    '"conformance_required":["check"]}')

PY_EXTRA = (
    '{"schema_version":1,"contract":"compiler-dev/1",'
    '"review_status":"draft_requires_conformance",'
    '"allowed_backends":["python"],'
    '"package_requirements":[{"package":"extra","version_policy":"test"}],'
    '"modules":[{"logical_path":"extra","host_module":"extra",'
    '"provider":"python_package"}],'
    '"types":[{"id":"extra.Thing","module":"extra","name":"Thing",'
    '"kind":"host_reference","host_attribute":["Thing"],'
    '"copy":"shared_reference"}],"values":[],"callables":[],'
    '"conformance_required":[]}')


C_NULL_BASE = (
    '{"schema_version":2,"contract":"compiler-dev/1","abi":"c",'
    '"target":"x86_64-linux-gnu","requires_hosted":false,'
    '"modules":[{"logical_path":"nulls/lib","provider":"native_library",'
    '"library":"nulls"}],'
    '"types":[{"id":"nulls.Handle","module":"nulls/lib","name":"Handle",'
    '"kind":"resource","release_symbol":"nulls_release"}],'
    '"callables":[{"id":"nulls.make","module":"nulls/lib","name":"make",'
    '"symbol":"nulls_make","parameters":[],'
    '"result":{"type":"nulls.Handle","ownership":"owned"}}]}')

C_NULL_MANAGED = (
    '{"schema_version":2,"contract":"compiler-dev/1","abi":"c",'
    '"target":"x86_64-linux-gnu","requires_hosted":true,'
    '"modules":[{"logical_path":"nulls/lib","provider":"native_library",'
    '"library":"nulls"}],'
    '"types":['
    '{"id":"nulls.Callback","module":"nulls/lib","name":"Callback",'
    '"kind":"callback","result":"i32","parameters":['
    '{"name":"context","type":"PtrMut[Unit]"},{"name":"x","type":"i32"}]},'
    '{"id":"nulls.Handle","module":"nulls/lib","name":"Handle",'
    '"kind":"resource","registration":{"callback":"nulls.Callback",'
    '"unregister_symbol":"nulls_close"}}],'
    '"callables":[{"id":"nulls.make","module":"nulls/lib","name":"make",'
    '"symbol":"nulls_make","parameters":['
    '{"name":"cb","type":"nulls.Callback"},'
    '{"name":"ctx","type":"PtrMut[Unit]"}],'
    '"result":{"type":"nulls.Handle","ownership":"owned"},'
    '"registers":{"callback_parameter":"cb","userdata_parameter":"ctx",'
    '"callback_userdata_parameter":"context"}}]}')

PY_UTF8_IDENTITY = (
    '{"schema_version":1,"contract":"compiler-dev/1",'
    '"review_status":"draft_requires_conformance",'
    '"allowed_backends":["python"],'
    '"package_requirements":[{"package":"host","version_policy":"汉😀\\u0000v"}],'
    '"modules":[{"logical_path":"py","host_module":"host",'
    '"provider":"python_package"},{"logical_path":"std/py",'
    '"host_module":"langrt","provider":"language_runtime"}],'
    '"types":[{"id":"Host/汉😀","module":"py","name":"Thing",'
    '"kind":"host_reference","host_attribute":["Thing"],'
    '"copy":"shared_reference"},{"id":"lang.py.Error","module":"std/py",'
    '"name":"Error","kind":"runtime_error","host_attribute":["Error"],'
    '"copy":"error_payload_copy"}],"values":[],'
    '"callables":[{"id":"方法/😀","module":"py","name":"act",'
    '"call_kind":"method","owner_type":"Host/汉😀",'
    '"host_attribute":["act"],"parameters":[{"name":"x","type":"i32",'
    '"kind":"positional_or_keyword","optional":false,"mode":"ordinary",'
    '"escapes":"no"}],"return_type":"Unit","error_type":null,'
    '"ownership":{"return":"unit","storage":"no_tensor_storage"},'
    '"effects":[],"receiver_escapes":"no"}],'
    '"properties":[{"id":"property.item","module":"py",'
    '"owner_type":"Host/汉😀","name":"item","type":"i32",'
    '"getter":["item"],"setter":null,"error_type":"lang.py.Error"}],'
    '"indexers":[{"id":"index.item","module":"py",'
    '"owner_type":"Host/汉😀","index_type":"i32",'
    '"result_type":"Host/汉😀","readable":true,"writable":false,'
    '"error_type":"lang.py.Error"}],'
    '"slices":[{"id":"slice.item","module":"py",'
    '"owner_type":"Host/汉😀","start_type":null,"end_type":"i64",'
    '"result_type":"Host/汉😀","readable":true,"writable":false,'
    '"error_type":"lang.py.Error"}],'
    '"conformance_required":["check"]}')


def json_bundle(text: str, mutate) -> str:
    value = json.loads(text)
    mutate(value)
    return json.dumps(value, ensure_ascii=False, indent=2)


def c_null(mutate) -> str:
    return json_bundle(C_NULL_BASE, mutate)


def c_managed_null(mutate) -> str:
    return json_bundle(C_NULL_MANAGED, mutate)


def replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) < 1:
        raise Fail(f"mutation target missing: {old!r}")
    return text.replace(old, new, 1)


def py_json(**changes) -> str:
    value = json.loads(PY_FULL)
    value.update(changes)
    return json.dumps(value, ensure_ascii=False, indent=2)


def duplicate_callable(identity: str, new_identity: str) -> str:
    value = json.loads(PY_FULL)
    for callable_value in value["callables"]:
        if callable_value["id"] == identity:
            copy = json.loads(json.dumps(callable_value))
            copy["id"] = new_identity
            value["callables"].append(copy)
            return json.dumps(value, ensure_ascii=False, indent=2)
    raise Fail(f"callable missing: {identity}")


def mutate_callable(identity: str, new_identity: str, path: list[str],
                    value) -> str:
    payload = json.loads(PY_FULL)
    for callable_value in payload["callables"]:
        if callable_value["id"] == identity:
            copy = json.loads(json.dumps(callable_value))
            copy["id"] = new_identity
            target = copy
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = value
            payload["callables"].append(copy)
            return json.dumps(payload, ensure_ascii=False, indent=2)
    raise Fail(f"callable missing: {identity}")


def access_bundle(properties=(), indexers=(), slices=()) -> str:
    value = json.loads(PY_FULL)
    value["properties"] = list(properties)
    value["indexers"] = list(indexers)
    value["slices"] = list(slices)
    return json.dumps(value, ensure_ascii=False, indent=2)


def c_json(text: str, mutate) -> str:
    value = json.loads(text)
    mutate(value)
    return json.dumps(value, ensure_ascii=False, indent=2)


PROPERTY = {
    "id": "py.torch.Tensor.device.projection", "module": "torch",
    "owner_type": "py.torch.Tensor", "name": "device", "type": "string",
    "getter": ["device"], "setter": None, "error_type": "lang.py.Error"}
INDEXER = {
    "id": "py.torch.Tensor.index.projection", "module": "torch",
    "owner_type": "py.torch.Tensor", "index_type": "i64",
    "result_type": "py.torch.Tensor", "readable": True, "writable": False,
    "error_type": "lang.py.Error"}
SLICE = {
    "id": "py.torch.Tensor.slice.projection", "module": "torch",
    "owner_type": "py.torch.Tensor", "start_type": "i64", "end_type": "i64",
    "result_type": "py.torch.Tensor", "readable": True, "writable": False,
    "error_type": "lang.py.Error"}


@dataclass
class Dep:
    name: str
    kind: str          # python | native
    module: str
    host: str
    file: str
    data: str | bytes | None


@dataclass
class Case:
    name: str
    deps: list[Dep]
    hosted: bool = False
    relaxed: tuple[str, ...] = ()  # accepted-dependency text differences
    notes: str = ""
    imports: list[str] | None = None
    ops: list[int] | None = None
    expect: str = "accept"  # accept | reject

    def __post_init__(self):
        if self.imports is None:
            self.imports = [dep.module for dep in self.deps]
        if self.ops is None:
            self.ops = list(range(len(self.deps)))


def python_case(name: str, text: str, *, module: str = "numpy",
                host: str = "numpy", **kwargs) -> Case:
    return Case(name, [Dep(module.split("/")[0], "python", module, host,
                           "bundle.json", text)], **kwargs)


def c_case(name: str, text: str, *, module: str = "events/lib",
           hosted: bool = True, **kwargs) -> Case:
    return Case(name, [Dep(module.split("/")[0], "native", module, "",
                           "bundle.json", text)], hosted=hosted, **kwargs)


def mutate_effects() -> str:
    payload = json.loads(PY_FULL)
    for callable_value in payload["callables"]:
        if callable_value["id"] == "numpy.linspace.scalar":
            callable_value["effects"] = 0
            return json.dumps(payload, ensure_ascii=False, indent=2)
    raise Fail("callable missing: numpy.linspace.scalar")


def python_cases() -> list[Case]:
    cases: list[Case] = [
        python_case("python-contract-example", PY_FULL),
        python_case("python-contract-example-torch", PY_FULL, module="torch",
                    host="torch"),
        python_case("python-utf8-crlf", json.dumps(
            dict(json.loads(PY_FULL),
                 conformance_required=["汉字校验", "emoji \U0001f600"]),
            ensure_ascii=False, indent=2).replace("\n", "\r\n")),
        python_case("python-access-valid",
                    access_bundle([PROPERTY], [INDEXER], [SLICE]),
                    module="torch", host="torch"),
        Case("python-multi-bundle",
             [Dep("extra", "python", "extra", "extra", "extra.json", PY_EXTRA),
              Dep("numpy", "python", "numpy", "numpy", "full.json", PY_FULL)]),
        Case("python-cache-repeat",
             [Dep("numpy", "python", "numpy", "numpy", "full.json", PY_FULL)],
             imports=["numpy", "numpy"], ops=[0, 0]),
        Case("python-identity-conflict",
             [Dep("numpy", "python", "numpy", "numpy", "full.json", PY_FULL),
              Dep("py", "python", "py", "host", "conflict.json",
                  PY_SMALL.replace("host.Thing", "numpy.linspace.scalar"))]),
        Case("python-rollback-after-success",
             [Dep("numpy", "python", "numpy", "numpy", "full.json", PY_FULL),
              Dep("torch", "python", "torch", "torch", "bad.json",
                  PY_FULL.replace('"schema_version": 1',
                                  '"schema_version": 9', 1))]),
        python_case("python-missing-id-field",
                    replace_once(PY_FULL, '"id": "numpy.linspace.scalar",',
                                 '"missing_id": "numpy.linspace.scalar",')),
        python_case("python-unknown-declaration-field",
                    replace_once(PY_FULL,
                                 '"id": "numpy.linspace.scalar",',
                                 '"unknown": 0, "id": "numpy.linspace.scalar",')),
        python_case("python-version-float",
                    replace_once(PY_FULL, '"schema_version": 1',
                                 '"schema_version": 1.0')),
        python_case("python-version-two",
                    replace_once(PY_FULL, '"schema_version": 1',
                                 '"schema_version": 2')),
        python_case("python-contract-value",
                    replace_once(PY_FULL, '"contract": "compiler-dev/1"',
                                 '"contract": "compiler-dev/2"')),
        python_case("python-review-status",
                    replace_once(PY_FULL,
                                 '"draft_requires_conformance"', '"unchecked"')),
        python_case("python-allowed-backends",
                    replace_once(PY_FULL,
                                 '\"allowed_backends\": [\n    \"python\"\n  ]',
                                 '\"allowed_backends\": [\n    \"native\"\n  ]')),
        python_case("python-duplicate-package",
                    replace_once(PY_FULL,
                                 '"package": "numpy",\n      "version_policy": "T10按实际通过conformance的CPU环境写入lock"\n    },\n    {\n      "package": "torch"',
                                 '"package": "numpy",\n      "version_policy": "T10按实际通过conformance的CPU环境写入lock"\n    },\n    {\n      "package": "numpy"')),
        python_case("python-app-reserved-module",
                    replace_once(PY_FULL, '"logical_path": "numpy"',
                                 '"logical_path": "app/numpy"')),
        python_case("python-std-provider",
                    replace_once(PY_FULL, '"provider": "language_runtime"',
                                 '"provider": "python_package"')),
        python_case("python-bad-host-module",
                    replace_once(PY_FULL, '"host_module": "torch.nn"',
                                 '"host_module": "torch/nn"')),
        python_case("python-duplicate-module",
                    replace_once(PY_FULL, '"logical_path": "torch"',
                                 '"logical_path": "numpy"')),
        python_case("python-type-kind-copy-mismatch",
                    replace_once(PY_FULL, '"copy": "error_payload_copy"',
                                 '"copy": "shared_reference"')),
        python_case("python-runtime-error-identity",
                    replace_once(PY_FULL, '"id": "lang.py.Error"',
                                 '"id": "lang.py.Other"')),
        python_case("python-reserved-declaration-id",
                    replace_once(PY_FULL, '"id": "numpy.linspace.scalar"',
                                 '"id": "i64"')),
        python_case("python-unknown-value-type",
                    replace_once(PY_FULL, '"type": "py.numpy.DTypeSpec"',
                                 '"type": "Any"')),
        python_case("python-empty-value-attribute",
                    replace_once(PY_FULL,
                                 '"host_attribute": [\n        "float32"\n      ]',
                                 '"host_attribute": []')),
        python_case("python-discard-export",
                    replace_once(PY_FULL, '"name": "float32"',
                                 '"name": "_"')),
        python_case("python-function-owner",
                    replace_once(PY_FULL, '"owner_type": null',
                                 '"owner_type": "py.numpy.ndarray"')),
        python_case("python-method-missing-owner",
                    replace_once(PY_FULL,
                                 '"owner_type": "py.torch.nn.Linear"',
                                 '"owner_type": null')),
        python_case("python-receiver-escape",
                    replace_once(PY_FULL,
                                 '"receiver_escapes": "may_retain"',
                                 '"receiver_escapes": "not_applicable"')),
        python_case("python-callable-attribute",
                    replace_once(PY_FULL,
                                 '"owner_type": "py.torch.nn.Linear",\n      "host_attribute": [],',
                                 '"owner_type": "py.torch.nn.Linear",\n      "host_attribute": ["forward"],')),
        python_case("python-constructor-target",
                    replace_once(PY_FULL,
                                 '"return_type": "py.torch.nn.Linear"',
                                 '"return_type": "py.torch.Tensor"')),
        python_case("python-return-ownership",
                    replace_once(PY_FULL,
                                 '"return": "python_reference"',
                                 '"return": "value"')),
        python_case("python-unknown-error-type",
                    replace_once(PY_FULL,
                                 '"error_type": "lang.py.Error"',
                                 '"error_type": "unknown.Type"')),
        python_case("python-error-not-runtime",
                    replace_once(PY_FULL,
                                 '"error_type": "lang.py.Error"',
                                 '"error_type": "py.numpy.ndarray"')),
        python_case("python-duplicate-parameter",
                    replace_once(PY_FULL, '"name": "stop"',
                                 '"name": "start"')),
        python_case("python-discard-parameter",
                    replace_once(PY_FULL, '"name": "start"',
                                 '"name": "_"')),
        python_case("python-parameter-kind-order",
                    replace_once(PY_FULL, '"kind": "positional_or_keyword"',
                                 '"kind": "keyword_only"')),
        python_case("python-optional-positional",
                    replace_once(PY_FULL, '"name": "start",\n          "type": "f64",\n          "kind": "positional_or_keyword",\n          "optional": false',
                                 '"name": "start",\n          "type": "f64",\n          "kind": "positional_or_keyword",\n          "optional": true')),
        python_case("python-inout-escape",
                    replace_once(PY_FULL,
                                 '"mode": "ordinary",\n          "escapes": "may_retain"',
                                 '"mode": "inout",\n          "escapes": "may_retain"')),
        python_case("python-duplicate-signature", duplicate_callable(
            "numpy.linspace.scalar", "numpy.linspace.duplicate")),
        python_case("python-duplicate-effect",
                    replace_once(PY_FULL,
                                 '"effects": [\n        "reads_arguments"\n      ]',
                                 '"effects": [\n        "reads_arguments",\n        "reads_arguments"\n      ]')),
        python_case("python-retains-mismatch",
                    replace_once(PY_FULL,
                                 '"escapes": "may_retain"',
                                 '"escapes": "no"')),
        python_case("python-receiver-escape-effect",
                    replace_once(PY_FULL,
                                 '"effects": [\n        "reads_arguments",\n        "retains_receiver"\n      ]',
                                 '"effects": [\n        "reads_arguments"\n      ]')),
        python_case("python-mutates-receiver-function",
                    replace_once(PY_FULL,
                                 '"effects": [\n        "reads_arguments"\n      ]',
                                 '"effects": [\n        "mutates_receiver",\n        "reads_arguments"\n      ]')),
        python_case("python-unknown-effect",
                    replace_once(PY_FULL, '"reads_arguments"',
                                 '"unknown_effect"')),
        python_case("python-duplicate-json-key",
                    replace_once(PY_FULL, '"contract": "compiler-dev/1",',
                                 '"contract": "compiler-dev/1", "contract": "compiler-dev/1",')),
        python_case("python-nul-json-key",
                    replace_once(PY_FULL, '"schema_version": 1,',
                                 '"schema_version": 1, "schema_version\\u0000hidden": 1,')),
        python_case("python-trailing-content", PY_FULL + "x"),
        python_case("python-missing-field",
                    replace_once(PY_FULL, '"review_status"', '"missing_status"')),
        python_case("python-unknown-root-field",
                    replace_once(PY_FULL, '{\n  "schema_version"',
                                 '{\n  "unknown": 0,\n  "schema_version"')),
        python_case("python-host-attribute-identifier",
                    replace_once(PY_FULL, '"ndarray"\n      ]',
                                 '"ndarray()"\n      ]')),
        python_case("python-indexer-writable",
                    access_bundle([PROPERTY], [dict(INDEXER, readable=False,
                                                    writable=False)], [SLICE]),
                    module="torch", host="torch"),
        python_case("python-property-getter-empty",
                    access_bundle([dict(PROPERTY, getter=[])], [INDEXER],
                                  [SLICE]),
                    module="torch", host="torch"),
        python_case("python-property-conflict",
                    access_bundle([dict(PROPERTY,
                                        owner_type="py.torch.nn.Linear",
                                        name="parameters")],
                                  [INDEXER], [SLICE]),
                    module="torch", host="torch"),
        python_case("python-duplicate-property",
                    access_bundle([PROPERTY, dict(PROPERTY, id="x.y.z")],
                                  [INDEXER], [SLICE]),
                    module="torch", host="torch"),
        python_case("python-duplicate-indexer",
                    access_bundle([PROPERTY],
                                  [INDEXER, dict(INDEXER, id="x.y.z")],
                                  [SLICE]),
                    module="torch", host="torch"),
        python_case("python-duplicate-slice",
                    access_bundle([PROPERTY], [INDEXER],
                                  [SLICE, dict(SLICE, id="x.y.z")]),
                    module="torch", host="torch"),
        python_case("python-access-owner",
                    access_bundle([dict(PROPERTY, owner_type="missing.Type")],
                                  [INDEXER], [SLICE]),
                    module="torch", host="torch"),
        python_case("python-access-owner-kind",
                    access_bundle([dict(PROPERTY,
                                        owner_type="lang.py.Error")],
                                  [INDEXER], [SLICE]),
                    module="torch", host="torch"),
        python_case("python-index-type",
                    access_bundle([PROPERTY],
                                  [dict(INDEXER, index_type="f64")], [SLICE]),
                    module="torch", host="torch"),
        python_case("python-slice-start-type",
                    access_bundle([PROPERTY], [INDEXER],
                                  [dict(SLICE, start_type="f64")]),
                    module="torch", host="torch"),
        python_case("python-access-unit",
                    access_bundle([dict(PROPERTY, type="Unit")], [INDEXER],
                                  [SLICE]),
                    module="torch", host="torch"),
        python_case("python-access-error",
                    access_bundle([dict(PROPERTY,
                                        error_type="py.torch.Tensor")],
                                  [INDEXER], [SLICE]),
                    module="torch", host="torch"),
        python_case("python-setter-empty",
                    access_bundle([dict(PROPERTY, setter=[])], [INDEXER],
                                  [SLICE]),
                    module="torch", host="torch"),
        python_case("python-overload-function", mutate_callable(
            "numpy.linspace.scalar", "numpy.linspace.integer",
            ["parameters", 0, "type"], "i64")),
        python_case("python-overload-constructor",
                    mutate_callable("torch.nn.Linear.constructor",
                                    "torch.nn.Linear.other",
                                    ["parameters", 0, "type"], "i32"),
                    module="torch/nn", host="torch"),
        python_case("python-access-nullable-slice",
                    access_bundle([PROPERTY], [INDEXER],
                                  [dict(SLICE, start_type=None,
                                        end_type=None)]),
                    module="torch", host="torch"),
        python_case("python-access-setter",
                    access_bundle([dict(PROPERTY, setter=["device"])],
                                  [INDEXER], [SLICE]),
                    module="torch", host="torch"),
        python_case("python-export-name-collision",
                    replace_once(PY_FULL, '"name": "float32"',
                                 '"name": "ndarray"')),
        python_case("python-constructor-name",
                    replace_once(PY_FULL,
                                 '"id": "torch.nn.Linear.constructor",\n      "module": "torch/nn",\n      "name": "Linear"',
                                 '"id": "torch.nn.Linear.constructor",\n      "module": "torch/nn",\n      "name": "linear_missing"')),
        python_case("python-function-conflict",
                    replace_once(PY_FULL,
                                 '"name": "linspace",\n      "call_kind": "function"',
                                 '"name": "ndarray",\n      "call_kind": "function"')),
        python_case("python-owner-module",
                    replace_once(PY_FULL,
                                 '"owner_type": "py.torch.nn.Linear"',
                                 '"owner_type": "py.torch.Tensor"')),
        python_case("python-storage-missing",
                    replace_once(PY_FULL, '"storage": "copy"',
                                 '"missing_storage": "copy"')),
        python_case("python-protocol-attribute",
                    replace_once(PY_FULL,
                                 '"kind": "opaque_protocol",\n      "host_attribute": []',
                                 '"kind": "opaque_protocol",\n      "host_attribute": ["x"]')),
        python_case("python-runtime-error-provider",
                    replace_once(PY_FULL,
                                 '"id": "lang.py.Error",\n      "module": "std/py"',
                                 '"id": "lang.py.Error",\n      "module": "numpy"')),
        python_case("python-slice-end-type",
                    access_bundle([PROPERTY], [INDEXER],
                                  [dict(SLICE, end_type="f64")]),
                    module="torch", host="torch"),
        python_case("python-slice-writable",
                    access_bundle([PROPERTY], [INDEXER],
                                  [dict(SLICE, readable=False,
                                        writable=False)]),
                    module="torch", host="torch"),
        python_case("python-property-getter-identifier",
                    access_bundle([dict(PROPERTY, getter=["device()"])],
                                  [INDEXER], [SLICE]),
                    module="torch", host="torch"),
        python_case("python-owner-field-missing",
                    replace_once(PY_FULL, '"owner_type": null,',
                                 '"missing_owner": null,')),
        python_case("python-parameter-optional-missing",
                    replace_once(PY_FULL,
                                 '"optional": false,\n          "mode": "ordinary",\n          "escapes": "no"',
                                 '"missing_optional": false,\n          "mode": "ordinary",\n          "escapes": "no"')),
        python_case("python-conformance-not-array",
                    py_json(conformance_required=1)),
        python_case("python-allowed-backends-extra",
                    py_json(allowed_backends=["python", "native"])),
        python_case("python-missing-schema-version",
                    py_json(**{}) if False else
                    json.dumps({k: v for k, v in json.loads(PY_FULL).items()
                                if k != "schema_version"},
                               ensure_ascii=False, indent=2)),
        python_case("python-version-policy-missing",
                    replace_once(PY_FULL, '"version_policy"',
                                 '"missing_policy"')),
        python_case("python-effects-not-array", mutate_effects()),
        python_case("python-version-exponent",
                    replace_once(PY_FULL, '"schema_version": 1',
                                 '"schema_version": 1e0')),
        python_case("python-version-nan",
                    replace_once(PY_FULL, '"schema_version": 1',
                                 '"schema_version": NaN')),
        python_case("python-duplicate-decoded-key",
                    replace_once(PY_FULL, '"contract": "compiler-dev/1",',
                                 '"contract": "compiler-dev/1", '
                                 '"contr\\u0061ct": "compiler-dev/1",')),
        python_case("python-nul-value", json.dumps(dict(
            json.loads(PY_FULL),
            package_requirements=[{"package": "numpy",
                                   "version_policy": "safe\u0000policy"}])[
                0] if False else None) if False else
            json.dumps({**json.loads(PY_FULL),
                        "package_requirements": [
                            {"package": "numpy",
                             "version_policy": "safe\u0000policy"}]},
                       ensure_ascii=False, indent=2)),
        Case("python-bom",
             [Dep("numpy", "python", "numpy", "numpy", "bundle.json",
                  b"\xef\xbb\xbf" + PY_FULL.encode("utf-8"))]),
        python_case("python-utf8-identity", PY_UTF8_IDENTITY, module="py",
                    host="host"),
        python_case("python-properties-null", py_json(properties=None)),
        python_case("python-optional-null", json_bundle(
            PY_FULL, lambda value: value["callables"][0]["parameters"][0]
            .update(optional=None))),
        python_case("python-readable-null", access_bundle(
            [PROPERTY], [dict(INDEXER, readable=None)], [SLICE]),
            module="torch", host="torch"),
        python_case("python-schema-bool",
                    replace_once(PY_FULL, '"schema_version": 1',
                                 '"schema_version": false')),
        python_case("python-version-policy-bool", json_bundle(
            PY_FULL, lambda value: value["package_requirements"][0]
            .update(version_policy=False))),
        python_case("python-empty-version-policy", json_bundle(
            PY_FULL, lambda value: value["package_requirements"][0]
            .update(version_policy=""))),
        python_case("python-schema-u64-upper",
                    replace_once(PY_FULL, '"schema_version": 1',
                                 '"schema_version": 9223372036854775808')),
        python_case("python-unused-u64-overflow", replace_once(
            PY_FULL, '"schema_version": 1,',
            '"schema_version": 1, "unused": 18446744073709551616,')),
        python_case("python-unused-i64-underflow", replace_once(
            PY_FULL, '"schema_version": 1,',
            '"schema_version": 1, "unused": -9223372036854775809,')),
        python_case("python-schema-null",
                    replace_once(PY_FULL, '"schema_version": 1',
                                 '"schema_version": null')),
        python_case("python-schema-zero",
                    replace_once(PY_FULL, '"schema_version": 1',
                                 '"schema_version": 0')),
        python_case("python-schema-negative",
                    replace_once(PY_FULL, '"schema_version": 1',
                                 '"schema_version": -1')),
        python_case("python-schema-leading-zero",
                    replace_once(PY_FULL, '"schema_version": 1',
                                 '"schema_version": 00')),
        python_case("python-bad-json", "{\n"),
    ]
    return cases


def events_json(mutate) -> str:
    return c_json(C_EVENTS, mutate)


def c_fixture_json(mutate) -> str:
    return c_json(C_FIXTURE, mutate)


def find_by_id(values, identity):
    for value in values:
        if value.get("id") == identity:
            return value
    raise Fail(f"fixture type missing: {identity}")


def c_cases() -> list[Case]:
    def events_type(identity):
        def apply(value):
            return find_by_id(value["types"], identity)
        return apply

    def mutation(target, **fields):
        def apply(value):
            target(value).update(fields)
        return apply

    def callable_fields(identity, **fields):
        def apply(value):
            find_by_id(value["callables"], identity).update(fields)
        return apply

    cases: list[Case] = [
        c_case("c-real-fixture", C_FIXTURE, module="fixture/lib"),
        c_case("c-events", C_EVENTS),
        c_case("c-other", C_OTHER, module="other/lib"),
        Case("c-multi-bundle",
             [Dep("events", "native", "events/lib", "", "events.json",
                  C_EVENTS),
              Dep("other", "native", "other/lib", "", "other.json", C_OTHER)],
             hosted=True),
        Case("c-cache-repeat",
             [Dep("events", "native", "events/lib", "", "events.json",
                  C_EVENTS)], hosted=True,
             imports=["events/lib", "events/lib"], ops=[0, 0]),
        Case("c-rollback-after-success",
             [Dep("events", "native", "events/lib", "", "events.json",
                  C_EVENTS),
              Dep("other", "native", "other/lib", "", "bad.json",
                  C_OTHER.replace('"schema_version": 2',
                                  '"schema_version": 3', 1))], hosted=True),
        c_case("c-freestanding-registration", C_EVENTS, hosted=False),
        c_case("c-version-float",
               replace_once(C_EVENTS, '"schema_version": 2',
                            '"schema_version": 2.0')),
        c_case("c-version-one", replace_once(C_EVENTS, '"schema_version": 2',
                                             '"schema_version": 1')),
        c_case("c-abi", replace_once(C_EVENTS, '"abi": "c"',
                                     '"abi": "cxx"')),
        c_case("c-target", replace_once(C_EVENTS,
                                        '"target": "x86_64-linux-gnu"',
                                        '"target": "aarch64-linux-gnu"')),
        c_case("c-unknown-root",
               replace_once(C_EVENTS, '{\n  "schema_version"',
                            '{\n  "unknown": 0,\n  "schema_version"')),
        c_case("c-missing-root",
               replace_once(C_EVENTS, '"requires_hosted": true,', '')),
        c_case("c-app-module", replace_once(C_EVENTS,
                                            '"logical_path": "events/lib"',
                                            '"logical_path": "app/events"')),
        c_case("c-library", replace_once(C_EVENTS,
                                         '"library": "events_fixture"',
                                         '"library": "bad/lib"')),
        c_case("c-duplicate-module",
               replace_once(C_EVENTS, '"logical_path": "events/lib"',
                            '"logical_path": "events/lib",\n'
                            '      "provider": "native_library",\n'
                            '      "library": "dup"},\n'
                            '    {"logical_path": "events/lib"')),
        c_case("c-empty-modules",
               events_json(lambda value: value.update(modules=[]))),
        c_case("c-type-id-dotted",
               replace_once(C_EVENTS, '"id": "events.Callback"',
                            '"id": "events/Callback"')),
        c_case("c-type-duplicate-id",
               replace_once(C_EVENTS, '"id": "events.FirstCallback"',
                            '"id": "events.Callback"')),
        c_case("c-type-unknown-module",
               replace_once(C_EVENTS,
                            '"id": "events.First", "module": "events/lib"',
                            '"id": "events.First", "module": "missing/lib"')),
        c_case("c-type-discard-name",
               replace_once(C_EVENTS, '"name": "First"', '"name": "_"')),
        c_case("c-type-bad-kind",
               replace_once(C_EVENTS, '"kind": "callback"',
                            '"kind": "struct"')),
        c_case("c-type-unknown-field",
               replace_once(C_EVENTS, '"id": "events.Callback"',
                            '"extra": 1, "id": "events.Callback"')),
        c_case("c-registration-on-record",
               events_json(lambda value: (
                   find_by_id(value["types"], "events.Subscription").update(
                       kind="record"),
                   find_by_id(value["types"], "events.Subscription").update(
                       size=8, align=8, fields=[]))),
               module="events/lib"),
        c_case("c-managed-release-symbol", events_json(
            lambda value: find_by_id(value["types"],
                                     "events.Subscription").update(
                release_symbol="events_unsubscribe"))),
        c_case("c-registration-callback-missing",
               replace_once(C_EVENTS, '"callback": "events.Callback"',
                            '"callback": "events.Missing"')),
        c_case("c-registration-callback-kind",
               replace_once(C_EVENTS, '"callback": "events.Callback"',
                            '"callback": "events.Subscription"')),
        c_case("c-registration-duplicate-unregister",
               replace_once(C_EVENTS, '"unregister_symbol": "events_first_close"',
                            '"unregister_symbol": "events_unsubscribe"')),
        c_case("c-callback-managed-result",
               events_json(lambda value: find_by_id(
                   value["types"], "events.Callback").update(result="events.Subscription"))),
        c_case("c-callback-managed-parameter",
               events_json(lambda value: find_by_id(
                   value["types"], "events.Callback")["parameters"][2].update(
                       type="events.Subscription"))),
        c_case("c-register-slot-missing",
               replace_once(C_EVENTS,
                            '"callback_parameter": "callback"',
                            '"callback_parameter": "missing"')),
        c_case("c-register-userdata-slot",
               replace_once(C_EVENTS,
                            '"userdata_parameter": "userdata"',
                            '"userdata_parameter": "priority"')),
        c_case("c-register-callback-type",
               replace_once(C_EVENTS,
                            '"name": "callback", "type": "events.Callback"',
                            '"name": "callback", "type": "events.FirstCallback"')),
        c_case("c-register-userdata-type",
               replace_once(C_EVENTS,
                            '"name": "userdata", "type": "PtrMut[Unit]"',
                            '"name": "userdata", "type": "Ptr[Unit]"')),
        c_case("c-register-callback-userdata-name",
               replace_once(C_EVENTS,
                            '"callback_userdata_parameter": "context"',
                            '"callback_userdata_parameter": "missing"')),
        c_case("c-duplicate-callable-id",
               replace_once(C_EVENTS, '"id": "events.subscribe_first"',
                            '"id": "events.subscribe"')),
        c_case("c-duplicate-export-name",
               replace_once(C_EVENTS, '"name": "subscribe_first"',
                            '"name": "subscribe"')),
        c_case("c-callable-symbol",
               replace_once(C_EVENTS, '"symbol": "events_subscribe"',
                            '"symbol": "int"')),
        c_case("c-managed-without-registers", events_json(
            lambda value: find_by_id(value["callables"],
                                     "events.subscribe").pop("registers"))),
        c_case("c-close-parameter", events_json(
            lambda value: find_by_id(value["callables"],
                                     "events.unsubscribe")["parameters"][0].update(
                type="events.First"))),
        c_case("c-close-duplicate", events_json(
            lambda value: find_by_id(value["callables"],
                                     "events.close_first").update(
                symbol="events_unsubscribe"))),
        c_case("c-empty-exports", events_json(
            lambda value: value.update(types=[], callables=[]))),
        c_case("c-record-size", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").update(size=24)),
            module="fixture/lib"),
        c_case("c-record-align", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").update(align=4)),
            module="fixture/lib"),
        c_case("c-record-offset", c_fixture_json(
            lambda value: find_by_id(value["types"], "fixture.Point")
            ["fields"][1].update(offset=8)), module="fixture/lib"),
        c_case("c-record-field-type", c_fixture_json(
            lambda value: find_by_id(value["types"], "fixture.Point")
            ["fields"][0].update(type="i64")), module="fixture/lib"),
        c_case("c-record-recursive", c_fixture_json(
            lambda value: find_by_id(value["types"], "fixture.Tiny")
            ["fields"][0].update(type="fixture.Tiny")), module="fixture/lib"),
        c_case("c-record-zero-field", c_fixture_json(
            lambda value: find_by_id(value["types"], "fixture.Tiny")
            ["fields"][0].update(type="Unit")), module="fixture/lib"),
        c_case("c-record-field-resource", c_fixture_json(
            lambda value: find_by_id(value["types"], "fixture.Tiny")
            ["fields"][0].update(type="fixture.OwnedResource")),
            module="fixture/lib"),
        c_case("c-buffer-element", c_fixture_json(
            lambda value: find_by_id(value["types"], "fixture.BufferView")
            ["fields"][1].update(type="Buffer[Ptr[i32]]")),
            module="fixture/lib"),
        c_case("c-pointer-resource", c_fixture_json(
            lambda value: find_by_id(
                value["callables"], "fixture.cf_i32")["parameters"][0].update(
                type="Ptr[fixture.OwnedResource]")), module="fixture/lib"),
        c_case("c-unknown-type", c_fixture_json(
            lambda value: find_by_id(
                value["callables"], "fixture.cf_i32")["parameters"][0].update(
                type="fixture.Nope")), module="fixture/lib"),
        c_case("c-malformed-type", c_fixture_json(
            lambda value: find_by_id(
                value["callables"], "fixture.cf_i32")["parameters"][0].update(
                type="Ptr[i32")), module="fixture/lib"),
        c_case("c-two-type-arguments", c_fixture_json(
            lambda value: find_by_id(
                value["callables"], "fixture.cf_i32")["parameters"][0].update(
                type="Ptr[i32,i64]")), module="fixture/lib"),
        c_case("c-type-depth", c_fixture_json(
            lambda value: find_by_id(
                value["callables"], "fixture.cf_i32")["parameters"][0].update(
                type="Ptr[" * 33 + "i32" + "]" * 33)), module="fixture/lib"),
        c_case("c-opaque-bare", c_fixture_json(
            lambda value: (value["types"].append(
                {"id": "fixture.Handle", "module": "fixture/lib",
                 "name": "Handle", "kind": "opaque"}),
                find_by_id(value["callables"], "fixture.cf_i32")
                ["parameters"][0].update(type="fixture.Handle"))),
            module="fixture/lib"),
        c_case("c-owned-scalar", c_fixture_json(
            lambda value: find_by_id(
                value["callables"], "fixture.cf_i32").update(
                result={"type": "i32", "ownership": "owned"})),
            module="fixture/lib"),
        c_case("c-release-not-owned", c_fixture_json(
            lambda value: find_by_id(
                value["callables"], "fixture.cf_i32").update(
                result={"type": "i32", "ownership": "value",
                        "release_symbol": "x"})), module="fixture/lib"),
        c_case("c-duplicate-json-key",
               replace_once(C_EVENTS, '"abi": "c",',
                            '"abi": "c", "abi": "c",')),
        c_case("c-nul-json-key",
               replace_once(C_EVENTS, '"abi": "c",',
                            '"abi": "c", "abi\\u0000x": "c",')),
        c_case("c-trailing-content", C_EVENTS + "x"),
        c_case("c-missing-field",
               replace_once(C_EVENTS, '"abi": "c"', '"missing_abi": "c"')),
        c_case("c-unknown-nested-field",
               replace_once(C_EVENTS, '"provider": "native_library"',
                            '"provider": "native_library", "extra": 1')),
        c_case("c-module-provider",
               replace_once(C_EVENTS, '"provider": "native_library"',
                            '"provider": "python_package"')),
        c_case("c-library-dash",
               replace_once(C_EVENTS, '"library": "events_fixture"',
                            '"library": "-bad"')),
        c_case("c-registers-same-slot",
               replace_once(C_EVENTS,
                            '"userdata_parameter": "userdata", "callback_userdata_parameter": "context"',
                            '"userdata_parameter": "callback", "callback_userdata_parameter": "context"')),
        c_case("c-registers-same-slot-second",
               replace_once(C_EVENTS,
                            '"callback_parameter": "callback", "userdata_parameter": "userdata"',
                            '"callback_parameter": "callback", "userdata_parameter": "callback"')),
        c_case("c-registers-non-managed", events_json(
            lambda value: find_by_id(value["callables"],
                                     "events.unsubscribe").update(
                registers={"callback_parameter": "callback",
                           "userdata_parameter": "userdata",
                           "callback_userdata_parameter": "context"}))),
        c_case("c-registers-missing-field", events_json(
            lambda value: find_by_id(value["callables"],
                                     "events.subscribe")["registers"].pop(
                "callback_parameter"))),
        c_case("c-callback-userdata-type", events_json(
            lambda value: find_by_id(value["types"], "events.Callback")
            ["parameters"][1].update(type="Ptr[Unit]"))),
        c_case("c-callback-duplicate-param", events_json(
            lambda value: find_by_id(value["types"],
                                     "events.FirstCallback")["parameters"][1].update(
                name="context"))),
        c_case("c-callback-unit-param", events_json(
            lambda value: find_by_id(value["types"],
                                     "events.FirstCallback")["parameters"][1].update(
                type="Unit"))),
        c_case("c-record-fields-missing", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").pop("fields")),
            module="fixture/lib"),
        c_case("c-record-size-missing", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").pop("size")),
            module="fixture/lib"),
        c_case("c-field-duplicate", c_fixture_json(
            lambda value: find_by_id(value["types"], "fixture.Point")
            ["fields"][1].update(name="x")), module="fixture/lib"),
        c_case("c-field-keyword", c_fixture_json(
            lambda value: find_by_id(value["types"], "fixture.Point")
            ["fields"][0].update(name="int")), module="fixture/lib"),
        c_case("c-registration-missing-symbol", events_json(
            lambda value: find_by_id(value["types"], "events.Subscription")
            ["registration"].pop("unregister_symbol"))),
        c_case("c-callable-unknown-module",
               replace_once(C_EVENTS,
                            '"id": "events.poll", "module": "events/lib"',
                            '"id": "events.poll", "module": "missing/lib"')),
        c_case("c-callable-name-discard",
               replace_once(C_EVENTS,
                            '"id": "events.poll", "module": "events/lib", "name": "poll"',
                            '"id": "events.poll", "module": "events/lib", "name": "_"')),
        c_case("c-close-result", events_json(
            lambda value: find_by_id(value["callables"],
                                     "events.unsubscribe").update(
                result={"type": "i32", "ownership": "value"}))),
        c_case("c-nested-record", c_fixture_json(
            lambda value: value["types"].append(
                {"id": "fixture.Pair", "module": "fixture/lib",
                 "name": "Pair", "kind": "record", "size": 16,
                 "align": 8, "fields": [
                     {"name": "point", "type": "fixture.Point",
                      "offset": 0}]})), module="fixture/lib"),
        c_case("c-nested-record-offset", c_fixture_json(
            lambda value: value["types"].append(
                {"id": "fixture.Pair", "module": "fixture/lib",
                 "name": "Pair", "kind": "record", "size": 24,
                 "align": 8, "fields": [
                     {"name": "point", "type": "fixture.Point",
                      "offset": 8}]})), module="fixture/lib"),
        c_case("c-indirect-record-cycle", c_fixture_json(
            lambda value: (value["types"].append(
                {"id": "fixture.CycleA", "module": "fixture/lib",
                 "name": "CycleA", "kind": "record", "size": 8,
                 "align": 8, "fields": [
                     {"name": "b", "type": "fixture.CycleB", "offset": 0}]}),
                value["types"].append(
                    {"id": "fixture.CycleB", "module": "fixture/lib",
                     "name": "CycleB", "kind": "record", "size": 8,
                     "align": 8, "fields": [
                         {"name": "a", "type": "fixture.CycleA",
                          "offset": 0}]}))), module="fixture/lib"),
        c_case("c-buffer-mut", c_fixture_json(
            lambda value: find_by_id(value["types"], "fixture.BufferView")
            ["fields"][1].update(type="BufferMut[u8]")), module="fixture/lib"),
        c_case("c-opaque-pointer", c_fixture_json(
            lambda value: (value["types"].append(
                {"id": "fixture.Handle", "module": "fixture/lib",
                 "name": "Handle", "kind": "opaque"}),
                find_by_id(value["callables"], "fixture.cf_i32")
                ["parameters"][0].update(type="Ptr[fixture.Handle]"))),
            module="fixture/lib"),
        c_case("c-c-name-null", events_json(
            lambda value: find_by_id(value["types"],
                                     "events.Callback").update(c_name=None))),
        c_case("c-c-name-false", events_json(
            lambda value: find_by_id(value["types"],
                                     "events.Callback").update(c_name=False))),
        c_case("c-registration-null-opaque", events_json(
            lambda value: value["types"].append(
                {"id": "events.Handle", "module": "events/lib",
                 "name": "Handle", "kind": "opaque",
                 "registration": None})), module="events/lib"),
        c_case("c-registration-null-resource", c_null(
            lambda value: value["types"][0].update(registration=None)),
            module="nulls/lib"),
        c_case("c-resource-retain-null", c_null(
            lambda value: value["types"][0].update(retain_symbol=None)),
            module="nulls/lib"),
        c_case("c-result-release-null", c_null(
            lambda value: value["callables"][0]["result"].update(
                release_symbol=None)), module="nulls/lib"),
        c_case("c-result-retain-null", c_null(
            lambda value: value["callables"][0]["result"].update(
                retain_symbol=None)), module="nulls/lib"),
        c_case("c-registers-null", c_null(
            lambda value: value["callables"][0].update(registers=None)),
            module="nulls/lib"),
        c_case("c-managed-release-null", c_managed_null(
            lambda value: value["types"][1].update(release_symbol=None)),
            module="nulls/lib"),
        c_case("c-managed-retain-null", c_managed_null(
            lambda value: value["types"][1].update(retain_symbol=None)),
            module="nulls/lib"),
        c_case("c-record-size-null", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").update(size=None)),
            module="fixture/lib"),
        c_case("c-record-align-null", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").update(align=None)),
            module="fixture/lib"),
        c_case("c-record-fields-null", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").update(fields=None)),
            module="fixture/lib"),
        c_case("c-callback-result-null", events_json(
            lambda value: find_by_id(value["types"],
                                     "events.Callback").update(result=None))),
        c_case("c-callback-parameters-null", events_json(
            lambda value: find_by_id(value["types"],
                                     "events.Callback").update(
                parameters=None))),
        c_case("c-requires-hosted-null", events_json(
            lambda value: value.update(requires_hosted=None))),
        c_case("c-record-size-u64", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").update(
                size=9223372036854775808)), module="fixture/lib"),
        c_case("c-record-align-u64", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").update(
                align=18446744073709551615)), module="fixture/lib"),
        c_case("c-record-offset-u64", c_fixture_json(
            lambda value: find_by_id(value["types"], "fixture.Point")
            ["fields"][1].update(offset=9223372036854775808)),
            module="fixture/lib"),
        c_case("c-unused-size-u64-overflow", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").update(
                size=18446744073709551616)), module="fixture/lib"),
        c_case("c-unused-size-i64-underflow", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").update(
                size=-9223372036854775809)), module="fixture/lib"),
        c_case("c-c-name-empty", events_json(
            lambda value: find_by_id(value["types"],
                                     "events.Callback").update(c_name=""))),
        c_case("c-modules-null", events_json(
            lambda value: value.update(modules=None))),
        c_case("c-record-size-negative", c_fixture_json(
            lambda value: find_by_id(value["types"],
                                     "fixture.Point").update(size=-1)),
            module="fixture/lib"),
        c_case("c-record-offset-negative", c_fixture_json(
            lambda value: find_by_id(value["types"], "fixture.Point")
            ["fields"][1].update(offset=-1)), module="fixture/lib"),
        c_case("c-record-size-leading-zero",
               replace_once(C_FIXTURE, '"size": 16', '"size": 00'),
               module="fixture/lib"),
        c_case("c-record-offset-negative-zero",
               replace_once(C_FIXTURE, '"offset": 4', '"offset": -0'),
               module="fixture/lib"),
        c_case("c-bad-json", "{\n"),
        Case("c-cross-bundle-type-identity",
             [Dep("events", "native", "events/lib", "", "events.json",
                  C_EVENTS),
              Dep("other", "native", "other/lib", "", "other.json",
                  C_OTHER.replace('"id": "other.Handler"',
                                  '"id": "events.Callback"'))],
             hosted=True),
        Case("python-invalid-source-nul",
             [Dep("numpy", "python", "numpy", "numpy", "bundle.json",
                  b'{"schema_version": 1}\x00')]),
        Case("python-invalid-source-cr",
             [Dep("numpy", "python", "numpy", "numpy", "bundle.json",
                  b'{"schema_version": 1}\r')]),
        Case("python-invalid-source-utf8",
             [Dep("numpy", "python", "numpy", "numpy", "bundle.json",
                  b'{"x": "\xff"}')]),
    ]
    cases.append(Case("c-missing-file",
                      [Dep("events", "native", "events/lib", "",
                           "absent.json", "")], hosted=True,
                      relaxed=("missing-file",)))
    # The missing-file dep must not create the file.
    cases[-1].deps[0].data = None  # type: ignore[assignment]
    return cases


# ---------------------------------------------------------------------------
# Project staging.

def stage_project(work: Path, accepted: Path) -> Path:
    project = work / "project"
    if project.exists():
        shutil.rmtree(project)
    source = project / "src"
    source.mkdir(parents=True)
    shutil.copy2(DRIVER, source / "main.wz")
    wzcc = source / "wzcc"
    wzcc.mkdir()
    for module in ACCEPTED_MODULES:
        (wzcc / module).symlink_to(accepted / "selfhost" / "src" / "wzcc" /
                                   module)
    (wzcc / "bindings").symlink_to(REPO / "selfhost" / "src" / "wzcc" /
                                   "bindings")
    shutil.copy2(MANIFEST, project / "lang.toml")
    return project


def prepare_case(work: Path, case: Case) -> Path:
    root = work / "cases" / case.name
    if root.exists():
        shutil.rmtree(root)
    (root / "src").mkdir(parents=True)
    imports = []
    manifest = [
        'schema_version=1', '[package]', 'name="main"', 'source_root="src"',
        'entry="app"', '[build]', 'backend="native"', 'word_bits=64',
        '[build.native]',
        'runtime="' + ("hosted" if case.hosted else "freestanding") + '"',
    ]
    for dep in case.deps:
        if dep.data is not None:
            if isinstance(dep.data, bytes):
                (root / dep.file).write_bytes(dep.data)
            else:
                (root / dep.file).write_bytes(dep.data.encode("utf-8"))
        if dep.kind == "python":
            manifest.append(f'[dependencies.{dep.name}]')
            manifest.append('kind="python"')
            manifest.append(f'binding="{dep.file}"')
            manifest.append(f'host_package="{dep.host}"')
        else:
            manifest.append(f'[dependencies.{dep.name}]')
            manifest.append('kind="native"')
            manifest.append(f'binding="{dep.file}"')
            manifest.append('libraries=[]')
            manifest.append('library_dirs=[]')
    source = "package main;\n"
    for module in case.imports:
        source += f'import "{module}" as m{len(source)};\n'
    source += "func main() {}\n"
    (root / "src" / "main.wz").write_text(source, encoding="utf-8")
    (root / "lang.toml").write_text("\n".join(manifest) + "\n",
                                    encoding="utf-8")
    return root


# ---------------------------------------------------------------------------
# Comparison.

def compare(case: Case, oracle, candidate, opt: str) -> None:
    if not isinstance(oracle, list) or len(oracle) != 5:
        raise Fail(f"{case.name} O{opt}: malformed oracle tree")
    oracle_state, oracle_failure, oracle_diags, marker, oracle_store = oracle
    if marker != "value":
        raise Fail(f"{case.name} O{opt}: oracle marker {marker!r}")
    if not isinstance(candidate, list) or len(candidate) != 2:
        raise Fail(f"{case.name} O{opt}: malformed candidate tree")
    ops, store = candidate
    if not isinstance(ops, list) or len(ops) != len(case.ops):
        raise Fail(f"{case.name} O{opt}: op count {len(ops)} != "
                   f"{len(case.ops)}")

    same(f"{case.name} O{opt}: published store", store, oracle_store)

    previous_sizes = None
    previous_by_path: dict[str, tuple[int, int]] = {}
    python_success = 0
    c_success = 0
    for position, dep_index in enumerate(case.ops):
        dep = case.deps[dep_index]
        op = ops[position]
        state, failure, has_value, value, diags, sizes = op
        path = str(Path(case_path(case, dep_index)).resolve())
        if has_value:
            if state != "complete" or failure != "source":
                raise Fail(f"{case.name} O{opt}: success op state {state}/"
                           f"{failure}")
            if diags:
                raise Fail(f"{case.name} O{opt}: success op diagnostics "
                           f"{short(diags)}")
            if path in previous_by_path:
                old_value, old_snapshots = previous_by_path[path]
                if value != old_value:
                    raise Fail(f"{case.name} O{opt}: cached path changed index")
                if sizes[0] != old_snapshots:
                    raise Fail(f"{case.name} O{opt}: cache read extra snapshot")
            else:
                if dep.kind == "python":
                    want = python_success
                    python_success += 1
                else:
                    want = c_success
                    c_success += 1
                if value != want:
                    raise Fail(f"{case.name} O{opt}: bundle index {value} != "
                               f"{want}")
                previous_by_path[path] = (value, sizes[0])
        else:
            if previous_sizes is not None:
                if sizes[2:] != previous_sizes[2:]:
                    raise Fail(f"{case.name} O{opt}: failed load published "
                               f"store records {sizes[2:]} != "
                               f"{previous_sizes[2:]}")
        previous_sizes = sizes

    if case.expect == "accept":
        if oracle_state != "complete":
            raise Fail(f"{case.name} O{opt}: oracle rejected a case that "
                       f"must be accepted: {short(oracle_diags)}")
        for index, op in enumerate(ops):
            if op[0] != "complete" or not op[2]:
                raise Fail(f"{case.name} O{opt}: oracle complete but op "
                           f"{index} failed")
        return
    if oracle_state == "complete":
        raise Fail(f"{case.name} O{opt}: oracle accepted a case that must "
                   f"be rejected")

    if oracle_state not in ("failed", "recovered"):
        raise Fail(f"{case.name} O{opt}: unexpected oracle state "
                   f"{oracle_state}")
    last = ops[-1]
    if last[0] != "failed" or last[2]:
        raise Fail(f"{case.name} O{opt}: last op did not fail: {short(last)}")
    if last[1] != oracle_failure:
        raise Fail(f"{case.name} O{opt}: failure kind {last[1]} != "
                   f"{oracle_failure}")
    for index, op in enumerate(ops[:-1]):
        if op[0] != "complete" or not op[2]:
            raise Fail(f"{case.name} O{opt}: earlier op {index} failed")
    if case.relaxed:
        same(f"{case.name} O{opt}: diagnostic count", len(last[4]),
             len(oracle_diags))
        for got, want in zip(last[4], oracle_diags):
            same(f"{case.name} O{opt}: diagnostic code", got[0], want[0])
            same(f"{case.name} O{opt}: diagnostic phase", got[1], want[1])
            same(f"{case.name} O{opt}: diagnostic severity", got[2], want[2])
            if "missing-file" in case.relaxed:
                # The standalone API has no import span; the accepted input
                # module reports an environment failure with no primary span.
                same(f"{case.name} O{opt}: missing-file primary",
                     got[4], None)
            same(f"{case.name} O{opt}: diagnostic related", got[5], want[5])
        return
    same(f"{case.name} O{opt}: diagnostics", last[4], oracle_diags)


def case_path(case: Case, index: int) -> Path:
    dep = case.deps[index]
    return Path(case.root) / dep.file


def build_oracle(work: Path, clang: Path, accepted: Path, lib_dir: Path) -> Path:
    binary = work / "bindings-oracle"
    result = run([str(clang), "-std=c++20", "-O2", str(ORACLE),
                  f"-I{accepted / 'include'}",
                  str(lib_dir / "liblang_modules.a"),
                  str(lib_dir / "liblang_frontend.a"),
                  # liblang_modules.a resolves ParseCache::store's
                  # StagingDir/publish symbols from this library.
                  str(lib_dir / "liblang_driver_process.a"),
                  "-ljson-c", "-ltomlplusplus", "-lm", "-o", str(binary)])
    if result.returncode != 0:
        fail("oracle build failed:\n" +
             decode(result.stdout) + decode(result.stderr))
    return binary


def build_candidate(work: Path, stage0: Path, project: Path) -> dict[str, Path]:
    manifest = str(project / "lang.toml")
    checked = run([str(stage0), "check", "--project", manifest,
                   "--backend", "native"], cwd=work)
    if checked.returncode != 0:
        fail("stage0 check failed:\n" + decode(checked.stdout) +
             decode(checked.stderr))
    binaries = {}
    for opt in ("0", "2"):
        out = work / f"opt{opt}"
        if out.exists():
            shutil.rmtree(out)
        built = run([str(stage0), "build", "--project", manifest, "--backend",
                     "native", "--out-dir", str(out), "--opt", opt],
                    cwd=work)
        if built.returncode != 0:
            fail(f"build O{opt} failed:\n" + decode(built.stdout) +
                 decode(built.stderr))
        binary = out / "program"
        if not binary.is_file():
            fail(f"O{opt}: missing {binary}")
        binaries[opt] = binary
    return binaries


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage0", default=str(DEFAULT_STAGE0))
    parser.add_argument("--accepted-root", default=str(DEFAULT_ACCEPTED))
    parser.add_argument("--clang", default=str(DEFAULT_CLANG))
    parser.add_argument("--lib-dir", default="")
    parser.add_argument("--work-dir", default=str(DEFAULT_WORK))
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    stage0 = Path(args.stage0).expanduser().resolve()
    accepted = Path(args.accepted_root).expanduser().resolve()
    clang = Path(args.clang).expanduser()
    work = Path(args.work_dir).expanduser().resolve()
    lib_dir = Path(args.lib_dir).expanduser() if args.lib_dir else (
        accepted / "build" / "woozy-stdlib-native")
    for path, label in ((stage0, "stage0 compiler"), (clang, "clang++"),
                        (lib_dir / "liblang_modules.a", "liblang_modules.a"),
                        (lib_dir / "liblang_frontend.a",
                         "liblang_frontend.a"),
                        (lib_dir / "liblang_driver_process.a",
                         "liblang_driver_process.a")):
        if not path.is_file():
            fail(f"{label} missing: {path}")
    if not (accepted / "selfhost" / "src" / "wzcc" / "jsondoc").is_dir():
        fail(f"accepted jsondoc missing under {accepted}")
    work.mkdir(parents=True, exist_ok=True)
    global TEMP_DIR
    TEMP_DIR = work / "tmp"
    TEMP_DIR.mkdir(parents=True, exist_ok=True)
    print(f"stage0: {stage0}")
    print(f"accepted-root: {accepted}")
    print(f"clang++: {clang}")
    print(f"lib-dir: {lib_dir}")
    print(f"work-dir: {work}")

    oracle_binary = build_oracle(work, clang, accepted, lib_dir)
    print("PASS: C++ oracle build")
    project = stage_project(work, accepted)
    binaries = build_candidate(work, stage0, project)
    print("PASS: stage0 check and O0/O2 builds")

    cases = python_cases() + c_cases()
    accepted = {
        "python-contract-example", "python-contract-example-torch",
        "python-utf8-crlf", "python-access-valid", "python-multi-bundle",
        "python-cache-repeat", "python-overload-function",
        "python-overload-constructor", "python-access-setter",
        "python-nul-value", "python-access-nullable-slice",
        "python-utf8-identity", "python-empty-version-policy",
        "c-nested-record", "c-opaque-pointer", "c-buffer-mut",
        "c-c-name-null", "c-registration-null-opaque",
        "c-registration-null-resource", "c-resource-retain-null",
        "c-result-release-null", "c-result-retain-null", "c-registers-null",
        "c-managed-release-null", "c-managed-retain-null", "c-c-name-empty",
        "c-real-fixture", "c-events", "c-other", "c-multi-bundle",
        "c-cache-repeat",
    }
    for case in cases:
        case.expect = "accept" if case.name in accepted else "reject"
    for case in cases:
        root = prepare_case(work, case)
        case.root = str(root)
        oracle_result = run([str(oracle_binary), str(root / "lang.toml")],
                            cwd=work)
        if oracle_result.returncode != 0:
            fail(f"{case.name}: oracle exit {oracle_result.returncode}\n" +
                 decode(oracle_result.stderr))
        oracle = parse_tree(oracle_result.stdout)
        request = {"hosted": case.hosted, "ops": [
            {"kind": "python" if case.deps[dep_index].kind == "python" else "c",
             "path": str((root / case.deps[dep_index].file).resolve())}
            for dep_index in case.ops]}
        payload = json.dumps(request, ensure_ascii=False).encode("utf-8")
        for opt in ("0", "2"):
            result = run([str(binaries[opt])], stdin=payload, cwd=work)
            if result.returncode != 0:
                fail(f"{case.name} O{opt}: candidate exit "
                     f"{result.returncode}\n" + decode(result.stderr))
            candidate = parse_tree(result.stdout)
            compare(case, oracle, candidate, opt)
            print(f"PASS: {case.name} O{opt}")
    relaxed = [case.name for case in cases if case.relaxed]
    print(f"PASS: {len(cases)} binding cases at O0 and O2 "
          f"({len(cases) * 2} comparisons); accepted-dependency differences: "
          f"{', '.join(relaxed) if relaxed else 'none'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
