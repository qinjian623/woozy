#!/usr/bin/env python3
"""Host-side conformance checks for the T10 NumPy/PyTorch binding bundle.

This program deliberately exercises the real NumPy and PyTorch host APIs on
CPU.  It does not generate or execute a .lang program, and it does not
implement or imitate the future lang.py.Error runtime type.  The JSON result
therefore separates host-level results from the generator/runtime items that
remain not_run.

Run with the dedicated CPython 3.12.15 venv described in CONFORMANCE.md.
"""

from __future__ import annotations

import argparse
import collections.abc
import hashlib
import importlib.metadata as importlib_metadata
import importlib.util
import json
import math
import os
import platform
import sys
import traceback
from pathlib import Path
from typing import Any, Callable

# Do not create __pycache__ in the source tree when loading the reference file.
sys.dont_write_bytecode = True

# Limit BLAS/OpenMP before importing NumPy or PyTorch.  This changes only this
# test process; the caller's environment and GPU/training jobs are untouched.
THREAD_ENV = (
    "OMP_NUM_THREADS",
    "MKL_NUM_THREADS",
    "OPENBLAS_NUM_THREADS",
    "NUMEXPR_NUM_THREADS",
    "VECLIB_MAXIMUM_THREADS",
)
for _name in THREAD_ENV:
    os.environ[_name] = "1"

import numpy as np
import torch
from torch import nn, optim
from torch.nn import functional as F

try:
    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
    _THREAD_SETUP_ERROR = None
except Exception as _exc:  # keep the evidence instead of hiding the host error
    _THREAD_SETUP_ERROR = f"{type(_exc).__module__}.{type(_exc).__qualname__}: {_exc}"


FROZEN_BUNDLE_SHA256 = "44924c3c33c5acff0e604f126fa1dcb346472fa0a7966de14dd0a5b4a10adb5f"
OPERATION_ID = "language-t10-cpu-conformance-20261104-correction"

EXPECTED_CALLABLES = {
    "numpy.linspace.scalar",
    "numpy.expand_dims.array_axis",
    "numpy.multiply.array_scalar",
    "numpy.add.array_scalar",
    "numpy.asarray.array_dtype",
    "torch.manual_seed",
    "torch.tensor.ndarray",
    "torch.nn.Linear.constructor",
    "torch.nn.Linear.parameters",
    "torch.nn.Linear.call",
    "torch.optim.SGD.constructor",
    "torch.optim.SGD.zero_grad",
    "torch.nn.functional.mse_loss",
    "torch.Tensor.backward",
    "torch.optim.SGD.step.no_closure",
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def type_name(value: Any) -> str:
    cls = type(value)
    return f"{cls.__module__}.{cls.__qualname__}"


def require(condition: Any, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def host_modules() -> dict[str, Any]:
    return {
        "numpy": np,
        "torch": torch,
        "torch/nn": nn,
        "torch/optim": optim,
        "torch/nn/functional": F,
    }


def resolve_attribute(module: Any, path: list[str]) -> Any:
    value = module
    for part in path:
        value = getattr(value, part)
    return value


class Recorder:
    def __init__(self) -> None:
        self.records: list[dict[str, Any]] = []

    def run(self, ident: str, fn: Callable[[], dict[str, Any]]) -> dict[str, Any]:
        try:
            evidence = fn()
            record = {"id": ident, "status": "passed", "evidence": evidence}
        except Exception as exc:
            record = {
                "id": ident,
                "status": "failed",
                "error": {
                    "type": type_name(exc),
                    "message": str(exc),
                },
                "traceback": traceback.format_exc(limit=10),
            }
        self.records.append(record)
        return record


def require_decl(
    by_id: dict[str, dict[str, Any]],
    ident: str,
    name: str,
    call_kind: str,
    parameters: list[str],
    return_type: str,
    optional: list[str],
) -> dict[str, Any]:
    spec = by_id.get(ident)
    require(spec is not None, f"bundle is missing callable {ident}")
    require(spec["name"] == name, f"{ident}: name {spec['name']!r} != {name!r}")
    require(spec["call_kind"] == call_kind, f"{ident}: unexpected call_kind")
    require(
        [p["name"] for p in spec["parameters"]] == parameters,
        f"{ident}: declared parameter names differ: {[p['name'] for p in spec['parameters']]}",
    )
    require(
        [p["name"] for p in spec["parameters"] if p["optional"]] == optional,
        f"{ident}: declared optional parameters differ",
    )
    require(spec["return_type"] == return_type, f"{ident}: unexpected return_type")
    require(spec["error_type"] == "lang.py.Error", f"{ident}: unexpected error_type")
    return spec


def check_bundle(bundle: dict[str, Any]) -> dict[str, Any]:
    checks: list[str] = []
    require(bundle.get("schema_version") == 1, "schema_version is not 1")
    checks.append("schema_version=1")
    require(bundle.get("contract") == "compiler-dev/1", "contract is not compiler-dev/1")
    checks.append("contract=compiler-dev/1")
    require(bundle.get("allowed_backends") == ["python"], "allowed_backends changed")
    checks.append("allowed_backends=[python]")
    require(bundle.get("review_status") == "draft_requires_conformance", "bundle is not draft")
    checks.append("review_status=draft_requires_conformance (not modified)")
    require(len(bundle.get("modules", [])) == 6, "expected 6 modules")
    checks.append("modules=6")
    require(len(bundle.get("types", [])) == 9, "expected 9 types")
    checks.append("types=9")
    require(len(bundle.get("values", [])) == 2, "expected 2 values")
    checks.append("values=2")
    callables = bundle.get("callables", [])
    require(len(callables) == 15, f"expected 15 callables, got {len(callables)}")
    ids = [c["id"] for c in callables]
    require(len(ids) == len(set(ids)), "duplicate callable id in bundle")
    require(set(ids) == EXPECTED_CALLABLES, f"callable id set differs: {sorted(set(ids) ^ EXPECTED_CALLABLES)}")
    checks.append("callables=15, unique, expected id set")
    return {"status": "passed", "checks": checks}


def test_linspace(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "numpy.linspace.scalar"
    require_decl(
        ctx["by_id"], ident, "linspace", "function",
        ["start", "stop", "num", "dtype"], "py.numpy.ndarray", ["num", "dtype"],
    )
    source_form = np.linspace(-1.0, 1.0, num=128, dtype=np.float32)
    keyword_form = np.linspace(start=-1.0, stop=1.0, num=128, dtype=np.float32)
    defaulted = np.linspace(-1.0, 1.0)
    values = source_form
    require(np.array_equal(source_form, keyword_form), "linspace positional/keyword forms differ")
    require(isinstance(values, np.ndarray), "linspace did not return ndarray")
    require(values.shape == (128,), f"linspace shape {values.shape}")
    require(values.dtype == np.float32, f"linspace dtype {values.dtype}")
    require(math.isclose(float(values[0]), -1.0, abs_tol=1e-7), "linspace start value")
    require(math.isclose(float(values[-1]), 1.0, abs_tol=1e-7), "linspace stop value")
    require(defaulted.shape == (50,), f"omitted num default shape {defaulted.shape}")
    require(defaulted.dtype == np.float64, f"omitted dtype default {defaulted.dtype}")
    ctx["values"] = values
    return {
        "call": "np.linspace(-1.0, 1.0, num=128, dtype=np.float32)",
        "keyword_form_call": "np.linspace(start=-1.0, stop=1.0, num=128, dtype=np.float32)",
        "forms_equal": True,
        "omitted_call": "np.linspace(-1.0, 1.0)",
        "return_type": type_name(values),
        "shape": list(values.shape),
        "dtype": str(values.dtype),
        "omitted_defaults": {"shape": list(defaulted.shape), "dtype": str(defaulted.dtype)},
    }


def test_expand_dims(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "numpy.expand_dims.array_axis"
    require_decl(
        ctx["by_id"], ident, "expand_dims", "function",
        ["a", "axis"], "py.numpy.ndarray", [],
    )
    values = ctx.get("values")
    require(values is not None, "linspace prerequisite did not provide values")
    source_form = np.expand_dims(values, axis=1)
    keyword_form = np.expand_dims(a=values, axis=1)
    result = source_form
    require(np.array_equal(source_form, keyword_form), "expand_dims positional/keyword forms differ")
    require(isinstance(result, np.ndarray), "expand_dims did not return ndarray")
    require(result.shape == (128, 1), f"expand_dims shape {result.shape}")
    require(result.dtype == np.float32, f"expand_dims dtype {result.dtype}")
    require(np.shares_memory(result, values), "expand_dims result does not share memory")
    ctx["x_np"] = result
    return {
        "call": "np.expand_dims(values, axis=1)",
        "keyword_form_call": "np.expand_dims(a=values, axis=1)",
        "forms_equal": True,
        "shape": list(result.shape),
        "dtype": str(result.dtype),
        "shares_memory_with_input": bool(np.shares_memory(result, values)),
        "storage_conclusion": "view_or_alias observed",
    }


def test_multiply(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "numpy.multiply.array_scalar"
    require_decl(
        ctx["by_id"], ident, "multiply", "function",
        ["x1", "x2"], "py.numpy.ndarray", [],
    )
    x_np = ctx.get("x_np")
    require(x_np is not None, "expand_dims prerequisite did not provide x_np")
    result = np.multiply(x_np, 2.0)
    require(isinstance(result, np.ndarray), "multiply did not return ndarray")
    require(result.shape == (128, 1), f"multiply shape {result.shape}")
    require(result.dtype == np.float32, f"multiply dtype {result.dtype}")
    max_diff = float(np.max(np.abs(result - x_np * 2.0)))
    require(max_diff <= 1e-6, f"multiply numeric difference {max_diff}")
    ctx["scaled"] = result
    return {
        "call": "np.multiply(x_np, 2.0)  # declared positional_only",
        "shape": list(result.shape),
        "dtype": str(result.dtype),
        "max_abs_diff_vs_numpy_expected": max_diff,
    }


def test_add(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "numpy.add.array_scalar"
    require_decl(
        ctx["by_id"], ident, "add", "function",
        ["x1", "x2"], "py.numpy.ndarray", [],
    )
    scaled = ctx.get("scaled")
    require(scaled is not None, "multiply prerequisite did not provide scaled")
    result = np.add(scaled, 1.0)
    require(isinstance(result, np.ndarray), "add did not return ndarray")
    require(result.shape == (128, 1), f"add shape {result.shape}")
    require(result.dtype == np.float32, f"add dtype {result.dtype}")
    max_diff = float(np.max(np.abs(result - (scaled + 1.0))))
    require(max_diff <= 1e-6, f"add numeric difference {max_diff}")
    ctx["shifted"] = result
    return {
        "call": "np.add(scaled, 1.0)  # declared positional_only",
        "shape": list(result.shape),
        "dtype": str(result.dtype),
        "max_abs_diff_vs_numpy_expected": max_diff,
    }


def test_asarray(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "numpy.asarray.array_dtype"
    require_decl(
        ctx["by_id"], ident, "asarray", "function",
        ["a", "dtype"], "py.numpy.ndarray", ["dtype"],
    )
    shifted = ctx.get("shifted")
    require(shifted is not None, "add prerequisite did not provide shifted")
    source_form = np.asarray(shifted, dtype=np.float32)
    keyword_form = np.asarray(a=shifted, dtype=np.float32)
    with_dtype = source_form
    without_dtype = np.asarray(shifted)
    require(np.array_equal(source_form, keyword_form), "asarray positional/keyword forms differ")
    require(isinstance(with_dtype, np.ndarray), "asarray did not return ndarray")
    require(with_dtype.shape == (128, 1), f"asarray shape {with_dtype.shape}")
    require(with_dtype.dtype == np.float32, f"asarray dtype {with_dtype.dtype}")
    require(np.shares_memory(with_dtype, shifted), "asarray result loses input storage")
    require(np.shares_memory(without_dtype, shifted), "omitted-dtype asarray loses storage")
    x_np = ctx.get("x_np")
    require(x_np is not None, "x_np missing")
    max_diff = float(np.max(np.abs(with_dtype - (2.0 * x_np + 1.0))))
    require(max_diff <= 1e-6, f"y=2x+1 numeric difference {max_diff}")
    ctx["y_np"] = with_dtype
    return {
        "call": "np.asarray(shifted, dtype=np.float32)",
        "keyword_form_call": "np.asarray(a=shifted, dtype=np.float32)",
        "forms_equal": True,
        "omitted_call": "np.asarray(shifted)",
        "shape": list(with_dtype.shape),
        "dtype": str(with_dtype.dtype),
        "shares_memory_with_input": bool(np.shares_memory(with_dtype, shifted)),
        "storage_conclusion": "view_or_alias observed; no implicit copy asserted",
        "max_abs_diff_y_vs_2x_plus_1": max_diff,
    }


def test_manual_seed(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "torch.manual_seed"
    require_decl(
        ctx["by_id"], ident, "manual_seed", "function",
        ["seed"], "py.torch.Generator", [],
    )
    source_form = torch.manual_seed(7)
    keyword_form = torch.manual_seed(seed=7)
    generator = source_form
    require(isinstance(source_form, torch.Generator), "manual_seed(7) did not return Generator")
    require(isinstance(keyword_form, torch.Generator), "manual_seed(seed=7) did not return Generator")
    require(source_form.initial_seed() == 7, f"generator seed {source_form.initial_seed()}")
    require(keyword_form.initial_seed() == 7, f"keyword generator seed {keyword_form.initial_seed()}")
    require(generator.device.type == "cpu", f"generator device {generator.device}")
    require(torch.initial_seed() == 7, f"torch.initial_seed {torch.initial_seed()}")
    ctx["generator"] = generator
    return {
        "call": "torch.manual_seed(7)",
        "keyword_form_call": "torch.manual_seed(seed=7)",
        "return_type": type_name(generator),
        "is_torch_Generator": True,
        "initial_seed": generator.initial_seed(),
        "torch_initial_seed": torch.initial_seed(),
        "device": str(generator.device),
    }


def test_tensor(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "torch.tensor.ndarray"
    require_decl(
        ctx["by_id"], ident, "tensor", "function",
        ["data", "dtype", "device"], "py.torch.Tensor", ["dtype", "device"],
    )
    x_np = ctx.get("x_np")
    y_np = ctx.get("y_np")
    require(x_np is not None and y_np is not None, "NumPy pipeline prerequisite missing")
    source_x = torch.tensor(x_np, dtype=torch.float32, device="cpu")
    keyword_x = torch.tensor(data=x_np, dtype=torch.float32, device="cpu")
    source_y = torch.tensor(y_np, dtype=torch.float32, device="cpu")
    keyword_y = torch.tensor(data=y_np, dtype=torch.float32, device="cpu")
    require(torch.equal(source_x, keyword_x), "tensor x positional/keyword forms differ")
    require(torch.equal(source_y, keyword_y), "tensor y positional/keyword forms differ")
    x, y = source_x, source_y
    for label, tensor, source in (("x", x, x_np), ("y", y, y_np)):
        require(isinstance(tensor, torch.Tensor), f"torch.tensor {label} did not return Tensor")
        require(tensor.shape == (128, 1), f"{label} shape {tensor.shape}")
        require(tensor.dtype == torch.float32, f"{label} dtype {tensor.dtype}")
        require(tensor.device.type == "cpu", f"{label} device {tensor.device}")
        require(tensor.data_ptr() != source.ctypes.data, f"{label} unexpectedly shares numpy storage")
    probe_np = np.array([[1.0], [2.0]], dtype=np.float32)
    probe_tensor = torch.tensor(probe_np)
    require(probe_tensor.dtype == torch.float32, "omitted dtype did not default to float32")
    require(probe_tensor.device.type == "cpu", "omitted device did not default to cpu")
    probe_np[0, 0] = 9.0
    require(float(probe_tensor[0, 0]) == 1.0, "torch.tensor did not copy input data")
    ctx["x"] = x
    ctx["y"] = y
    return {
        "call": "torch.tensor(x_np, dtype=torch.float32, device=\"cpu\")",
        "keyword_form_call": "torch.tensor(data=x_np, dtype=torch.float32, device=\"cpu\")",
        "forms_equal": True,
        "omitted_call": "torch.tensor(probe_np)",
        "x": {"shape": list(x.shape), "dtype": str(x.dtype), "device": str(x.device)},
        "y": {"shape": list(y.shape), "dtype": str(y.dtype), "device": str(y.device)},
        "explicit_copy_observed": True,
        "input_mutation_after_construction_visible_in_tensor": False,
    }


def test_linear_constructor(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "torch.nn.Linear.constructor"
    require_decl(
        ctx["by_id"], ident, "Linear", "constructor",
        ["in_features", "out_features", "device", "dtype"],
        "py.torch.nn.Linear", ["device", "dtype"],
    )
    torch.manual_seed(7)
    source_model = nn.Linear(1, 1, device="cpu", dtype=torch.float32)
    torch.manual_seed(7)
    keyword_model = nn.Linear(
        in_features=1,
        out_features=1,
        device="cpu",
        dtype=torch.float32,
    )
    require(torch.equal(source_model.weight, keyword_model.weight), "Linear weight positional/keyword forms differ")
    require(torch.equal(source_model.bias, keyword_model.bias), "Linear bias positional/keyword forms differ")
    model = source_model
    minimal = nn.Linear(1, 1)
    require(isinstance(model, nn.Linear), "Linear constructor did not return nn.Linear")
    require(model.weight.shape == (1, 1), f"weight shape {model.weight.shape}")
    require(model.bias.shape == (1,), f"bias shape {model.bias.shape}")
    require(model.weight.dtype == torch.float32, f"weight dtype {model.weight.dtype}")
    require(model.weight.device.type == "cpu", f"weight device {model.weight.device}")
    require(minimal.weight.dtype == torch.float32, "omitted dtype did not default to float32")
    require(minimal.weight.device.type == "cpu", "omitted device did not default to cpu")
    ctx["model"] = model
    return {
        "call": "nn.Linear(1, 1, device=\"cpu\", dtype=torch.float32)",
        "keyword_form_call": "nn.Linear(in_features=1, out_features=1, device=\"cpu\", dtype=torch.float32)",
        "forms_equal": True,
        "omitted_call": "nn.Linear(1, 1)",
        "return_type": type_name(model),
        "weight_shape": list(model.weight.shape),
        "bias_shape": list(model.bias.shape),
        "dtype": str(model.weight.dtype),
        "device": str(model.weight.device),
    }


def test_parameters(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "torch.nn.Linear.parameters"
    require_decl(
        ctx["by_id"], ident, "parameters", "method",
        [], "py.torch.ParameterIterator", [],
    )
    model = ctx.get("model")
    require(model is not None, "Linear constructor prerequisite missing")
    first_iterator = model.parameters()
    require(not isinstance(first_iterator, (list, tuple)), "parameters() returned a list/tuple")
    require(isinstance(first_iterator, collections.abc.Iterator), "parameters() is not an iterator")
    require(iter(first_iterator) is first_iterator, "parameters() iterator is not its own iterator")
    first_parameter = next(first_iterator)
    require(isinstance(first_parameter, nn.Parameter), "parameters() did not yield Parameter")
    count = sum(1 for _ in model.parameters())
    require(count == 2, f"expected 2 parameters, got {count}")
    require(getattr(torch, "ParameterIterator", None) is None, "unexpected host torch.ParameterIterator")
    return {
        "call": "model.parameters()",
        "return_type": type_name(first_iterator),
        "is_list_or_tuple": False,
        "is_iterator": True,
        "iter_is_self": True,
        "yielded_type": type_name(first_parameter),
        "parameter_count": count,
        "host_torch_ParameterIterator": "<absent>",
        "materialization_conclusion": "host returns a lazy generator; no list conversion used",
    }


def test_linear_call(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "torch.nn.Linear.call"
    require_decl(
        ctx["by_id"], ident, "__call__", "callable",
        ["input"], "py.torch.Tensor", [],
    )
    model = ctx.get("model")
    x = ctx.get("x")
    require(model is not None and x is not None, "model/x prerequisite missing")
    hook_calls: list[dict[str, Any]] = []

    def hook(module: Any, args: tuple[Any, ...], output: Any) -> None:
        hook_calls.append(
            {
                "module_type": type_name(module),
                "arg_count": len(args),
                "output_shape": list(output.shape),
                "output_requires_grad": bool(output.requires_grad),
            }
        )

    handle = model.register_forward_hook(hook)
    try:
        prediction = model(x)
    finally:
        handle.remove()
    require(len(hook_calls) == 1, f"forward hook fired {len(hook_calls)} times")
    require(isinstance(prediction, torch.Tensor), "model(x) did not return Tensor")
    require(prediction.shape == (128, 1), f"prediction shape {prediction.shape}")
    require(prediction.dtype == torch.float32, f"prediction dtype {prediction.dtype}")
    require(prediction.device.type == "cpu", f"prediction device {prediction.device}")
    require(prediction.requires_grad, "prediction lost requires_grad")
    require(prediction.grad_fn is not None, "prediction lost grad_fn")
    ctx["prediction"] = prediction
    return {
        "call": "model(x)  # instance call, not model.forward(x)",
        "hook_fired": True,
        "hook_count": len(hook_calls),
        "hook_observation": hook_calls[0],
        "return_shape": list(prediction.shape),
        "dtype": str(prediction.dtype),
        "device": str(prediction.device),
        "requires_grad": bool(prediction.requires_grad),
        "grad_fn": type_name(prediction.grad_fn) if prediction.grad_fn is not None else None,
    }


def test_sgd_constructor(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "torch.optim.SGD.constructor"
    require_decl(
        ctx["by_id"], ident, "SGD", "constructor",
        ["params", "lr"], "py.torch.optim.SGD", ["lr"],
    )
    model = ctx.get("model")
    require(model is not None, "model prerequisite missing")
    source_optimizer = optim.SGD(model.parameters(), lr=0.1)
    keyword_optimizer = optim.SGD(params=model.parameters(), lr=0.1)
    optimizer = source_optimizer
    require(isinstance(optimizer, optim.SGD), "SGD constructor did not return SGD")
    require(len(optimizer.param_groups) == 1, "unexpected param group count")
    group = optimizer.param_groups[0]
    keyword_group = keyword_optimizer.param_groups[0]
    require(float(group["lr"]) == 0.1, f"SGD lr {group['lr']}")
    require(float(keyword_group["lr"]) == 0.1, f"SGD keyword lr {keyword_group['lr']}")
    require(len(group["params"]) == 2, f"SGD param count {len(group['params'])}")
    require(len(keyword_group["params"]) == 2, "SGD keyword param count")
    require(group["params"][0] is model.weight, "SGD did not retain weight identity")
    require(group["params"][1] is model.bias, "SGD did not retain bias identity")
    require(keyword_group["params"][0] is model.weight, "keyword SGD did not retain weight identity")
    require(keyword_group["params"][1] is model.bias, "keyword SGD did not retain bias identity")
    default_optimizer = optim.SGD(model.parameters())
    require(float(default_optimizer.param_groups[0]["lr"]) == 0.001, "omitted lr default")
    ctx["optimizer"] = optimizer
    return {
        "call": "optim.SGD(model.parameters(), lr=0.1)",
        "keyword_form_call": "optim.SGD(params=model.parameters(), lr=0.1)",
        "forms_equal": True,
        "omitted_call": "optim.SGD(model.parameters())",
        "return_type": type_name(optimizer),
        "lr": float(group["lr"]),
        "param_count": len(group["params"]),
        "parameter_identity_preserved": True,
        "omitted_lr_default": float(default_optimizer.param_groups[0]["lr"]),
        "params_was_list": False,
    }


def test_mse_loss(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "torch.nn.functional.mse_loss"
    require_decl(
        ctx["by_id"], ident, "mse_loss", "function",
        ["input", "target"], "py.torch.Tensor", [],
    )
    prediction = ctx.get("prediction")
    y = ctx.get("y")
    require(prediction is not None and y is not None, "prediction/y prerequisite missing")
    source_loss = F.mse_loss(prediction, y)
    keyword_loss = F.mse_loss(input=prediction, target=y)
    loss = source_loss
    require(torch.equal(source_loss, keyword_loss), "mse_loss positional/keyword forms differ")
    require(isinstance(loss, torch.Tensor), "mse_loss did not return Tensor")
    require(loss.dim() == 0, f"mse_loss is not zero-dimensional: {loss.shape}")
    require(loss.dtype == torch.float32, f"loss dtype {loss.dtype}")
    require(loss.device.type == "cpu", f"loss device {loss.device}")
    require(loss.requires_grad, "loss lost requires_grad")
    require(loss.grad_fn is not None, "loss lost grad_fn")
    value = float(loss.item())
    require(math.isfinite(value), f"loss is not finite: {value}")
    require(value >= 0.0, f"loss is negative: {value}")
    ctx["loss"] = loss
    return {
        "call": "F.mse_loss(prediction, y)",
        "keyword_form_call": "F.mse_loss(input=prediction, target=y)",
        "forms_equal": True,
        "shape": list(loss.shape),
        "dim": loss.dim(),
        "dtype": str(loss.dtype),
        "device": str(loss.device),
        "requires_grad": bool(loss.requires_grad),
        "grad_fn": type_name(loss.grad_fn) if loss.grad_fn is not None else None,
        "value": value,
        "finite": True,
        "nonnegative": True,
    }


def test_backward(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "torch.Tensor.backward"
    require_decl(
        ctx["by_id"], ident, "backward", "method",
        [], "Unit", [],
    )
    loss = ctx.get("loss")
    model = ctx.get("model")
    require(loss is not None and model is not None, "loss/model prerequisite missing")
    result = loss.backward()
    require(result is None, f"backward returned {result!r}, expected None")
    require(model.weight.grad is not None, "weight.grad is None after backward")
    require(model.bias.grad is not None, "bias.grad is None after backward")
    require(bool(torch.isfinite(model.weight.grad).all()), "weight grad is not finite")
    require(bool(torch.isfinite(model.bias.grad).all()), "bias grad is not finite")
    return {
        "call": "loss.backward()",
        "return_is_None": True,
        "weight_grad_shape": list(model.weight.grad.shape),
        "weight_grad_dtype": str(model.weight.grad.dtype),
        "bias_grad_shape": list(model.bias.grad.shape),
        "bias_grad_dtype": str(model.bias.grad.dtype),
        "grads_finite": True,
    }


def test_step(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "torch.optim.SGD.step.no_closure"
    require_decl(
        ctx["by_id"], ident, "step", "method",
        [], "Unit", [],
    )
    optimizer = ctx.get("optimizer")
    model = ctx.get("model")
    require(optimizer is not None and model is not None, "optimizer/model prerequisite missing")
    before_weight = model.weight.detach().clone()
    before_bias = model.bias.detach().clone()
    result = optimizer.step()
    require(result is None, f"step returned {result!r}, expected None")
    weight_changed = not torch.equal(before_weight, model.weight.detach())
    bias_changed = not torch.equal(before_bias, model.bias.detach())
    require(weight_changed, "optimizer.step did not update weight")
    require(bias_changed, "optimizer.step did not update bias")
    return {
        "call": "optimizer.step()  # no closure",
        "return_is_None": True,
        "weight_changed": weight_changed,
        "bias_changed": bias_changed,
        "weight_grad_still_present": model.weight.grad is not None,
    }


def test_zero_grad(ctx: dict[str, Any]) -> dict[str, Any]:
    ident = "torch.optim.SGD.zero_grad"
    require_decl(
        ctx["by_id"], ident, "zero_grad", "method",
        ["set_to_none"], "Unit", ["set_to_none"],
    )
    optimizer = ctx.get("optimizer")
    model = ctx.get("model")
    x = ctx.get("x")
    y = ctx.get("y")
    require(
        optimizer is not None and model is not None and x is not None and y is not None,
        "optimizer/model/x/y prerequisite missing",
    )
    # Produce real gradients after the step so set_to_none=True has something
    # to clear.  This is host-level observation, not generated source.
    second_prediction = model(x)
    second_loss = F.mse_loss(second_prediction, y)
    second_loss.backward()
    require(model.weight.grad is not None, "weight.grad missing before zero_grad")
    result = optimizer.zero_grad(set_to_none=True)
    require(result is None, f"zero_grad returned {result!r}, expected None")
    require(model.weight.grad is None, "weight.grad was not set to None")
    require(model.bias.grad is None, "bias.grad was not set to None")
    omitted_result = optimizer.zero_grad()
    require(omitted_result is None, f"omitted zero_grad returned {omitted_result!r}")
    return {
        "call": "optimizer.zero_grad(set_to_none=True)",
        "omitted_call": "optimizer.zero_grad()",
        "return_is_None": True,
        "grads_cleared_to_none": True,
        "second_cycle_grads_created": True,
        "omitted_return_is_None": True,
    }


INTERFACE_TESTS: list[tuple[str, Callable[[dict[str, Any]], dict[str, Any]]]] = [
    ("numpy.linspace.scalar", test_linspace),
    ("numpy.expand_dims.array_axis", test_expand_dims),
    ("numpy.multiply.array_scalar", test_multiply),
    ("numpy.add.array_scalar", test_add),
    ("numpy.asarray.array_dtype", test_asarray),
    ("torch.manual_seed", test_manual_seed),
    ("torch.tensor.ndarray", test_tensor),
    ("torch.nn.Linear.constructor", test_linear_constructor),
    ("torch.nn.Linear.parameters", test_parameters),
    ("torch.nn.Linear.call", test_linear_call),
    ("torch.optim.SGD.constructor", test_sgd_constructor),
    ("torch.nn.functional.mse_loss", test_mse_loss),
    ("torch.Tensor.backward", test_backward),
    ("torch.optim.SGD.step.no_closure", test_step),
    ("torch.optim.SGD.zero_grad", test_zero_grad),
]


def run_interface_checks(
    ctx: dict[str, Any],
) -> list[dict[str, Any]]:
    recorder = Recorder()
    for ident, test in INTERFACE_TESTS:
        record = recorder.run(ident, lambda test=test: test(ctx))
        spec = ctx["by_id"].get(ident)
        if spec is not None:
            record["declared"] = {
                "module": spec["module"],
                "name": spec["name"],
                "call_kind": spec["call_kind"],
                "parameters": [
                    {
                        "name": p["name"],
                        "type": p["type"],
                        "kind": p["kind"],
                        "optional": p["optional"],
                    }
                    for p in spec["parameters"]
                ],
                "return_type": spec["return_type"],
                "error_type": spec["error_type"],
                "ownership": spec["ownership"],
            }
    # Keep output in the original bundle order even though execution order
    # puts backward/step before zero_grad.
    order = {c["id"]: index for index, c in enumerate(ctx["bundle"]["callables"])}
    recorder.records.sort(key=lambda record: order.get(record["id"], 10_000))
    return recorder.records


def run_value_checks(ctx: dict[str, Any]) -> list[dict[str, Any]]:
    recorder = Recorder()

    def numpy_float32() -> dict[str, Any]:
        value = np.float32
        arr = np.asarray([1.0, 2.0], dtype=value)
        require(arr.dtype == np.dtype(value), "np.float32 was not accepted as dtype")
        require(getattr(np, "DTypeSpec", None) is None, "unexpected host numpy.DTypeSpec")
        return {
            "declared_type": "py.numpy.DTypeSpec",
            "host_value_type": type_name(value),
            "host_DTypeSpec": "<absent>",
            "accepted_by": ["np.asarray(..., dtype=np.float32)", "np.linspace(..., dtype=np.float32)"],
            "protocol_note": "opaque protocol checked by actual dtype arguments; not by isinstance(np.float32, np.dtype)",
        }

    def torch_float32() -> dict[str, Any]:
        value = torch.float32
        require(isinstance(value, torch.dtype), "torch.float32 is not a torch.dtype")
        tensor = torch.tensor([1.0], dtype=value)
        require(tensor.dtype == value, "torch.float32 was not accepted by torch.tensor")
        model = nn.Linear(1, 1, dtype=value)
        require(model.weight.dtype == value, "torch.float32 was not accepted by nn.Linear")
        return {
            "declared_type": "py.torch.dtype",
            "host_value_type": type_name(value),
            "is_torch_dtype": True,
            "accepted_by": ["torch.tensor(..., dtype=torch.float32)", "nn.Linear(..., dtype=torch.float32)"],
        }

    recorder.run("numpy.float32", numpy_float32)
    recorder.run("torch.float32", torch_float32)
    return recorder.records


def run_type_checks(ctx: dict[str, Any]) -> list[dict[str, Any]]:
    recorder = Recorder()
    modules = host_modules()

    for spec in ctx["bundle"]["types"]:
        ident = spec["id"]
        if spec["kind"] == "runtime_error":
            recorder.records.append(
                {
                    "id": f"type:{ident}",
                    "status": "not_run",
                    "reason": "lang.py.Error is a future language-runtime payload; host has no langrt.Error and no bridge was faked",
                    "declared": spec,
                }
            )
            continue

        def check(spec: dict[str, Any] = spec) -> dict[str, Any]:
            require(spec["module"] in modules, f"{spec['id']}: unknown logical module")
            module = modules[spec["module"]]
            if spec["kind"] == "opaque_protocol":
                require(spec["host_attribute"] == [], f"{spec['id']}: opaque type has host attributes")
                host_same_name = getattr(module, spec["name"], None)
                require(host_same_name is None, f"{spec['id']}: host unexpectedly defines {spec['name']}")
                return {
                    "kind": spec["kind"],
                    "host_same_name_class": "<absent>",
                    "protocol_note": "declared opaque protocol; no host class lookup used as the check",
                }
            require(spec["kind"] == "host_reference", f"{spec['id']}: unknown kind")
            target = resolve_attribute(module, spec["host_attribute"])
            require(isinstance(target, type), f"{spec['id']}: host attribute is not a class")
            return {
                "kind": spec["kind"],
                "host_attribute": spec["host_attribute"],
                "host_type": type_name(target),
                "resolved": True,
            }

        recorder.run(f"type:{ident}", check)
    return recorder.records


def run_protocol_checks(ctx: dict[str, Any]) -> list[dict[str, Any]]:
    recorder = Recorder()

    def dtype_spec() -> dict[str, Any]:
        require(getattr(np, "DTypeSpec", None) is None, "unexpected numpy.DTypeSpec")
        require(np.asarray([0.0], dtype=np.float32).dtype == np.float32, "np.float32 dtype use")
        return {
            "protocol": "py.numpy.DTypeSpec",
            "host_same_name_class": "<absent>",
            "actual_use": "np.asarray(np.float32) and np.linspace(dtype=np.float32) executed",
            "wrong_check_avoided": "isinstance(np.float32, np.dtype) was not used",
        }

    def parameter_iterator() -> dict[str, Any]:
        model = ctx.get("model")
        require(model is not None, "model prerequisite missing")
        iterator = model.parameters()
        require(getattr(torch, "ParameterIterator", None) is None, "unexpected torch.ParameterIterator")
        require(not isinstance(iterator, (list, tuple)), "parameters() materialized to list/tuple")
        require(isinstance(iterator, collections.abc.Iterator), "parameters() is not an iterator")
        optimizer = optim.SGD(params=model.parameters(), lr=0.1)
        require(isinstance(optimizer, optim.SGD), "SGD did not accept parameter iterator")
        return {
            "protocol": "py.torch.ParameterIterator",
            "host_same_name_class": "<absent>",
            "actual_use": "model.parameters() passed directly to optim.SGD",
            "returned_type": type_name(iterator),
            "is_list_or_tuple": False,
            "is_iterator": True,
        }

    def generator() -> dict[str, Any]:
        generator = ctx.get("generator")
        require(generator is not None, "manual_seed prerequisite missing")
        require(isinstance(generator, torch.Generator), "manual_seed result is not Generator")
        return {
            "protocol": "py.torch.Generator",
            "host_type": type_name(generator),
            "actual_use": "torch.manual_seed(seed=7) returned and inspected",
        }

    recorder.run("opaque:py.numpy.DTypeSpec", dtype_spec)
    recorder.run("opaque:py.torch.ParameterIterator", parameter_iterator)
    recorder.run("host:py.torch.Generator", generator)
    return recorder.records


def run_data_checks(ctx: dict[str, Any]) -> list[dict[str, Any]]:
    recorder = Recorder()

    def data() -> dict[str, Any]:
        x_np = ctx.get("x_np")
        y_np = ctx.get("y_np")
        x = ctx.get("x")
        y = ctx.get("y")
        require(all(value is not None for value in (x_np, y_np, x, y)), "data prerequisite missing")
        require(x_np.shape == (128, 1) and y_np.shape == (128, 1), "NumPy data shape")
        require(x_np.dtype == np.float32 and y_np.dtype == np.float32, "NumPy data dtype")
        max_diff = float(np.max(np.abs(y_np - (2.0 * x_np + 1.0))))
        require(max_diff <= 1e-6, f"y=2x+1 difference {max_diff}")
        require(x.shape == (128, 1) and y.shape == (128, 1), "Tensor data shape")
        require(x.dtype == torch.float32 and y.dtype == torch.float32, "Tensor data dtype")
        require(x.device.type == "cpu" and y.device.type == "cpu", "Tensor data device")
        return {
            "x_np": {"shape": list(x_np.shape), "dtype": str(x_np.dtype)},
            "y_np": {"shape": list(y_np.shape), "dtype": str(y_np.dtype)},
            "y_equals_2x_plus_1": True,
            "max_abs_diff": max_diff,
            "x_tensor": {"shape": list(x.shape), "dtype": str(x.dtype), "device": str(x.device)},
            "y_tensor": {"shape": list(y.shape), "dtype": str(y.dtype), "device": str(y.device)},
        }

    recorder.run("data:shape_dtype_y_equals_2x_plus_1", data)
    return recorder.records


def _capture_exception(fn: Callable[[], Any]) -> Exception | None:
    try:
        fn()
    except Exception as exc:
        return exc
    return None


def _numpy_axis_error_assessment(exc: Exception | None) -> tuple[bool, dict[str, Any]]:
    if exc is None:
        return False, {"reason": "no exception raised"}
    if not isinstance(exc, np.exceptions.AxisError):
        return False, {"reason": f"unexpected exception class {type_name(exc)}"}
    message = str(exc)
    lower = message.lower()
    axis_attribute = getattr(exc, "axis", None)
    checks = {
        "is_axis_error": True,
        "axis_attribute": axis_attribute,
        "message_mentions_axis": "axis" in lower,
        "message_mentions_invalid_axis_3": "3" in message,
        "message_mentions_bounds_or_dimension": ("out of bounds" in lower or "dimension" in lower),
    }
    if axis_attribute is not None and axis_attribute != 3:
        return False, {**checks, "reason": f"AxisError axis is {axis_attribute}, not 3"}
    if not checks["message_mentions_axis"]:
        return False, {**checks, "reason": "message does not identify an axis"}
    if not checks["message_mentions_invalid_axis_3"]:
        return False, {**checks, "reason": "message does not name the tested invalid axis 3"}
    if not checks["message_mentions_bounds_or_dimension"]:
        return False, {**checks, "reason": "message lacks an out-of-bounds/dimension reason"}
    return True, checks


def _torch_device_error_assessment(exc: Exception | None) -> tuple[bool, dict[str, Any]]:
    if exc is None:
        return False, {"reason": "no exception raised"}
    if not isinstance(exc, RuntimeError):
        return False, {"reason": f"unexpected exception class {type_name(exc)}"}
    message = str(exc)
    lower = message.lower()
    checks = {
        "is_runtime_error": True,
        "message_mentions_device": "device" in lower,
        "message_mentions_tested_token": "not-a-real-device" in message,
        "message_mentions_invalid_reason": any(
            token in lower for token in ("invalid", "unrecognized", "expected", "unknown", "not a valid")
        ),
    }
    if not checks["message_mentions_device"]:
        return False, {**checks, "reason": "message does not mention device"}
    if not checks["message_mentions_tested_token"]:
        return False, {**checks, "reason": "message does not name the tested device string"}
    if not checks["message_mentions_invalid_reason"]:
        return False, {**checks, "reason": "message lacks an invalid-device reason"}
    return True, checks


def run_host_error_checks(ctx: dict[str, Any]) -> list[dict[str, Any]]:
    recorder = Recorder()

    def positive_numpy_axis() -> dict[str, Any]:
        values = ctx.get("values")
        require(values is not None, "values prerequisite missing")
        exc = _capture_exception(lambda: np.expand_dims(values, axis=3))
        accepted, detail = _numpy_axis_error_assessment(exc)
        require(accepted, f"real np.expand_dims axis error did not match: {detail}")
        return {
            "kind": "positive",
            "call": "np.expand_dims(values, axis=3)",
            "raised": True,
            "exception_type": type_name(exc),
            "message": str(exc),
            "assertions": detail,
            "is_ordinary_exception": True,
            "py_error_bridge": "not_run",
        }

    def positive_torch_device() -> dict[str, Any]:
        x_np = ctx.get("x_np")
        require(x_np is not None, "x_np prerequisite missing")
        exc = _capture_exception(
            lambda: torch.tensor(x_np, dtype=torch.float32, device="not-a-real-device")
        )
        accepted, detail = _torch_device_error_assessment(exc)
        require(accepted, f"real torch.tensor device error did not match: {detail}")
        return {
            "kind": "positive",
            "call": "torch.tensor(x_np, dtype=torch.float32, device=\"not-a-real-device\")",
            "raised": True,
            "exception_type": type_name(exc),
            "message": str(exc),
            "assertions": detail,
            "is_ordinary_exception": True,
            "py_error_bridge": "not_run",
        }

    def negative_control(
        ident: str,
        owner: Any,
        attribute: str,
        replacement: Callable[..., Any],
        call: Callable[[], Any],
        assessor: Callable[[Exception | None], tuple[bool, dict[str, Any]]],
    ) -> dict[str, Any]:
        original = getattr(owner, attribute)
        setattr(owner, attribute, replacement)
        try:
            exc = _capture_exception(call)
        finally:
            setattr(owner, attribute, original)
        require(exc is not None, f"{ident}: replacement did not raise")
        accepted, detail = assessor(exc)
        require(not accepted, f"{ident}: negative control was incorrectly accepted: {detail}")
        return {
            "kind": "negative_control",
            "expected": "rejected",
            "validator_decision": "rejected",
            "exception_type": type_name(exc) if exc is not None else None,
            "message": str(exc) if exc is not None else None,
            "assertions": detail,
            "function_restored_in_finally": True,
            "py_error_bridge": "not_run",
        }

    def numpy_wrong_class() -> dict[str, Any]:
        values = ctx.get("values")
        require(values is not None, "values prerequisite missing")

        def wrong(*_args: Any, **_kwargs: Any) -> Any:
            raise ValueError("unrelated numpy exception")

        return negative_control(
            "host_error_negative:numpy.wrong_class",
            np, "expand_dims", wrong,
            lambda: np.expand_dims(values, axis=3),
            _numpy_axis_error_assessment,
        )

    def numpy_same_class_unrelated_reason() -> dict[str, Any]:
        values = ctx.get("values")
        require(values is not None, "values prerequisite missing")

        def wrong(*_args: Any, **_kwargs: Any) -> Any:
            raise np.exceptions.AxisError(1, 1)

        return negative_control(
            "host_error_negative:numpy.same_class_unrelated",
            np, "expand_dims", wrong,
            lambda: np.expand_dims(values, axis=3),
            _numpy_axis_error_assessment,
        )

    def torch_wrong_class() -> dict[str, Any]:
        x_np = ctx.get("x_np")
        require(x_np is not None, "x_np prerequisite missing")

        def wrong(*_args: Any, **_kwargs: Any) -> Any:
            raise TypeError("unrelated torch exception")

        return negative_control(
            "host_error_negative:torch.wrong_class",
            torch, "tensor", wrong,
            lambda: torch.tensor(x_np, dtype=torch.float32, device="not-a-real-device"),
            _torch_device_error_assessment,
        )

    def torch_same_class_unrelated_reason() -> dict[str, Any]:
        x_np = ctx.get("x_np")
        require(x_np is not None, "x_np prerequisite missing")

        def wrong(*_args: Any, **_kwargs: Any) -> Any:
            raise RuntimeError("device initialization failed")

        return negative_control(
            "host_error_negative:torch.same_class_unrelated",
            torch, "tensor", wrong,
            lambda: torch.tensor(x_np, dtype=torch.float32, device="not-a-real-device"),
            _torch_device_error_assessment,
        )

    recorder.run("host_error:numpy.axis", positive_numpy_axis)
    recorder.run("host_error:torch.device", positive_torch_device)
    recorder.run("host_error_negative:numpy.wrong_class", numpy_wrong_class)
    recorder.run("host_error_negative:numpy.same_class_unrelated", numpy_same_class_unrelated_reason)
    recorder.run("host_error_negative:torch.wrong_class", torch_wrong_class)
    recorder.run("host_error_negative:torch.same_class_unrelated", torch_same_class_unrelated_reason)
    return recorder.records


def run_reference(ctx: dict[str, Any]) -> dict[str, Any]:
    reference_path = ctx["repo_root"] / "baseline/samples/numpy_torch_reference.py"
    spec = importlib.util.spec_from_file_location("t10_numpy_torch_reference", reference_path)
    require(spec is not None and spec.loader is not None, "cannot load reference module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    counts = {"forward": 0, "backward": 0, "zero_grad": 0, "step": 0}
    original_step = torch.optim.SGD.step
    original_backward = torch.Tensor.backward
    original_zero_grad = torch.optim.Optimizer.zero_grad
    original_forward = nn.Linear.forward

    def counted_step(self: Any, *args: Any, **kwargs: Any) -> Any:
        counts["step"] += 1
        return original_step(self, *args, **kwargs)

    def counted_backward(self: Any, *args: Any, **kwargs: Any) -> Any:
        counts["backward"] += 1
        return original_backward(self, *args, **kwargs)

    def counted_zero_grad(self: Any, *args: Any, **kwargs: Any) -> Any:
        counts["zero_grad"] += 1
        return original_zero_grad(self, *args, **kwargs)

    def counted_forward(self: Any, input: Any) -> Any:
        counts["forward"] += 1
        return original_forward(self, input)

    torch.optim.SGD.step = counted_step
    torch.Tensor.backward = counted_backward
    torch.optim.Optimizer.zero_grad = counted_zero_grad
    nn.Linear.forward = counted_forward
    try:
        loss = module.train()
    finally:
        torch.optim.SGD.step = original_step
        torch.Tensor.backward = original_backward
        torch.optim.Optimizer.zero_grad = original_zero_grad
        nn.Linear.forward = original_forward

    require(isinstance(loss, torch.Tensor), "reference train() did not return Tensor")
    require(loss.dim() == 0, f"reference loss is not zero-dimensional: {loss.shape}")
    require(loss.dtype == torch.float32, f"reference loss dtype {loss.dtype}")
    require(loss.device.type == "cpu", f"reference loss device {loss.device}")
    require(loss.requires_grad, "reference loss lost requires_grad")
    require(loss.grad_fn is not None, "reference loss lost grad_fn")
    value = float(loss.item())
    finite = math.isfinite(value)
    nonnegative = value >= 0.0
    below_threshold = value < 1e-3
    require(finite, f"reference loss is not finite: {value}")
    require(nonnegative, f"reference loss is negative: {value}")
    require(below_threshold, f"reference loss is not < 1e-3: {value}")
    require(counts == {"forward": 201, "backward": 200, "zero_grad": 200, "step": 200},
            f"reference operation counts differ: {counts}")
    return {
        "label": "handwritten_reference_not_generated",
        "path": str(reference_path),
        "sha256": sha256_file(reference_path),
        "execution": "unmodified source loaded with importlib and train() called once",
        "counts": counts,
        "expected_counts": {"forward": 201, "backward": 200, "zero_grad": 200, "step": 200},
        "result": {
            "type": type_name(loss),
            "shape": list(loss.shape),
            "dtype": str(loss.dtype),
            "device": str(loss.device),
            "requires_grad": bool(loss.requires_grad),
            "grad_fn": type_name(loss.grad_fn) if loss.grad_fn is not None else None,
            "value": value,
        },
        "acceptance": {
            "finite": finite,
            "nonnegative": nonnegative,
            "less_than_1e-3": below_threshold,
        },
    }


def collect_environment(ctx: dict[str, Any]) -> dict[str, Any]:
    distributions = {}
    for dist in importlib_metadata.distributions():
        name = dist.metadata.get("Name")
        if name:
            distributions[name] = dist.version
    gpu_like = sorted(
        name
        for name in distributions
        if any(token in name.lower() for token in ("nvidia", "cuda", "triton", "torchvision", "torchaudio"))
    )
    return {
        "python": {
            "version": platform.python_version(),
            "full_version": sys.version.replace("\n", " "),
            "executable": sys.executable,
            "prefix": sys.prefix,
            "base_prefix": sys.base_prefix,
            "implementation": platform.python_implementation(),
        },
        "host": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "libc": list(platform.libc_ver()),
        },
        "numpy": {
            "version": np.__version__,
            "module_file": str(Path(np.__file__).resolve()),
        },
        "torch": {
            "version": torch.__version__,
            "module_file": str(Path(torch.__file__).resolve()),
            "version_cuda": torch.version.cuda,
            "cuda_is_available": bool(torch.cuda.is_available()),
            "cuda_is_built": bool(torch.backends.cuda.is_built()),
            "num_threads": torch.get_num_threads(),
            "num_interop_threads": torch.get_num_interop_threads(),
            "thread_setup_error": _THREAD_SETUP_ERROR,
        },
        "thread_env": {name: os.environ.get(name) for name in THREAD_ENV},
        "distributions": dict(sorted(distributions.items())),
        "gpu_like_distributions": gpu_like,
        "pip_version": importlib_metadata.version("pip"),
    }


def input_hashes(repo_root: Path) -> dict[str, str]:
    paths = [
        "contracts/python-bindings.schema.json",
        "contracts/python_bindings.example.json",
        "baseline/samples/04_numpy_torch.lang",
        "baseline/samples/numpy_torch_reference.py",
        "docs/T10_BINDING_PROPOSAL.md",
        "docs/INTEROP.md",
        "docs/ACCEPTANCE.md",
        "docs/SEMANTICS.md",
    ]
    return {path: sha256_file(repo_root / path) for path in paths}


def not_run_items() -> list[dict[str, str]]:
    return [
        {
            "id": "generated_language_program",
            "status": "not_run",
            "reason": "No T10 Python emitter/runtime/CLI integration is present in this worktree; no .lang program was generated or executed.",
        },
        {
            "id": "lang.py.Error_bridge",
            "status": "not_run",
            "reason": "lang.py.Error is a future language-runtime payload. No langrt.Error exists and no handwritten substitute was used.",
        },
        {
            "id": "source_language_cleanup_and_control_events",
            "status": "not_run",
            "reason": "defer/drop/TypedError/Panic/KeyboardInterrupt/SystemExit behavior requires the generated runtime; only raw host exceptions were observed.",
        },
        {
            "id": "gpu_cuda_amp_ddp_training",
            "status": "not_run",
            "reason": "CPU-only task scope; no GPU packages were installed and no GPU/AMP/DDP workload was started.",
        },
    ]


def status_counts(records: list[dict[str, Any]]) -> dict[str, int]:
    counts = {"passed": 0, "failed": 0, "not_run": 0}
    for record in records:
        counts[record["status"]] = counts.get(record["status"], 0) + 1
    return counts


def emit_result(result: dict[str, Any], json_out: Path | None) -> int:
    text = json.dumps(result, indent=2, ensure_ascii=False)
    if json_out is not None:
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(text + "\n")
        print(f"wrote {json_out}")
    else:
        print(text)
    return 0 if result.get("status") == "passed" else 1


def frozen_bundle_rejection(
    bundle_path: Path,
    expected_sha256: str,
    actual_sha256: str | None,
    error: str | None = None,
) -> dict[str, Any]:
    reason = (
        "The bundle bytes do not match the frozen, reviewed original SHA-256. "
        "No host interface call and no handwritten-reference execution was performed."
    )
    interfaces = [
        {
            "id": ident,
            "status": "not_run",
            "reason": "frozen bundle SHA-256 mismatch; interface was not called",
        }
        for ident, _test in INTERFACE_TESTS
    ]
    return {
        "schema_version": 1,
        "kind": "t10_cpu_host_conformance",
        "contract": "compiler-dev/1",
        "operation_id": OPERATION_ID,
        "status": "failed",
        "decision": "rejected",
        "failure_stage": "frozen_bundle_sha256",
        "scope": (
            "This corrected script accepts only the frozen original binding bundle; "
            "declaration changes are rejected before host calls or reference execution. "
            "It is not .lang generation and not py.Error bridge verification."
        ),
        "bundle": {
            "path": str(bundle_path),
            "expected_sha256": expected_sha256,
            "actual_sha256": actual_sha256,
            "frozen_match": False,
            "error": error,
        },
        "interfaces": interfaces,
        "interfaces_summary": {"passed": 0, "failed": 0, "not_run": len(interfaces)},
        "reference": {"status": "not_run", "reason": reason},
        "host_errors": [],
        "host_errors_summary": {"passed": 0, "failed": 0, "not_run": 0},
        "not_run": [
            {
                "id": "host_interfaces_and_reference_due_to_frozen_bundle_mismatch",
                "status": "not_run",
                "reason": reason,
            },
            *not_run_items(),
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root (default: inferred from this file)",
    )
    parser.add_argument(
        "--json-out",
        type=Path,
        default=None,
        help="write the full JSON result here; otherwise print it to stdout",
    )
    args = parser.parse_args(argv)
    repo_root = args.repo_root.resolve()
    bundle_path = repo_root / "contracts/python_bindings.example.json"
    try:
        bundle_bytes = bundle_path.read_bytes()
    except OSError as exc:
        return emit_result(
            frozen_bundle_rejection(
                bundle_path,
                FROZEN_BUNDLE_SHA256,
                None,
                f"{type_name(exc)}: {exc}",
            ),
            args.json_out,
        )
    actual_bundle_sha256 = hashlib.sha256(bundle_bytes).hexdigest()
    if actual_bundle_sha256 != FROZEN_BUNDLE_SHA256:
        return emit_result(
            frozen_bundle_rejection(
                bundle_path,
                FROZEN_BUNDLE_SHA256,
                actual_bundle_sha256,
            ),
            args.json_out,
        )
    bundle = json.loads(bundle_bytes.decode("utf-8"))
    by_id = {callable_spec["id"]: callable_spec for callable_spec in bundle["callables"]}

    bundle_result: dict[str, Any]
    try:
        bundle_result = check_bundle(bundle)
    except Exception as exc:
        bundle_result = {
            "status": "failed",
            "error": {"type": type_name(exc), "message": str(exc)},
            "traceback": traceback.format_exc(limit=10),
        }

    ctx: dict[str, Any] = {
        "repo_root": repo_root,
        "bundle": bundle,
        "by_id": by_id,
        "bundle_path": bundle_path,
    }
    interfaces = run_interface_checks(ctx)
    values = run_value_checks(ctx)
    types = run_type_checks(ctx)
    protocols = run_protocol_checks(ctx)
    data = run_data_checks(ctx)
    host_errors = run_host_error_checks(ctx)

    reference_recorder = Recorder()
    reference = reference_recorder.run("handwritten_reference", lambda: run_reference(ctx))
    reference_result = reference
    if reference["status"] == "passed":
        reference_result = {"status": "passed", **reference["evidence"]}
    else:
        reference_result = reference

    executed_sections = [interfaces, values, types, protocols, data, host_errors, [reference_result]]
    failed = any(
        record["status"] == "failed"
        for section in executed_sections
        for record in section
    )
    overall = "passed" if not failed and bundle_result["status"] == "passed" else "failed"
    result = {
        "schema_version": 1,
        "kind": "t10_cpu_host_conformance",
        "contract": "compiler-dev/1",
        "operation_id": OPERATION_ID,
        "status": overall,
        "scope": (
            "Host-level execution of the 15 declared NumPy/PyTorch callables and 2 dtype values. "
            "This is not .lang generation/execution and not py.Error bridge verification."
        ),
        "bundle": {
            "path": str(bundle_path),
            "sha256": actual_bundle_sha256,
            "frozen_sha256": FROZEN_BUNDLE_SHA256,
            "frozen_match": True,
            "verification_scope": "Only the frozen original bundle bytes are accepted; declaration changes are rejected before host calls.",
            "review_status": bundle.get("review_status"),
            **bundle_result,
        },
        "inputs_sha256": input_hashes(repo_root),
        "environment": collect_environment(ctx),
        "interfaces": interfaces,
        "interfaces_summary": status_counts(interfaces),
        "values": values,
        "values_summary": status_counts(values),
        "types": types,
        "types_summary": status_counts(types),
        "protocols": protocols,
        "protocols_summary": status_counts(protocols),
        "data": data,
        "data_summary": status_counts(data),
        "host_errors": host_errors,
        "host_errors_summary": status_counts(host_errors),
        "reference": reference_result,
        "not_run": not_run_items(),
    }

    return emit_result(result, args.json_out)


if __name__ == "__main__":
    raise SystemExit(main())
