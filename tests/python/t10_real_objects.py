#!/usr/bin/env python3
"""Observe real NumPy/PyTorch object identity and host errors in generated code.

The generated program is not modified.  This observer only supplies real CPU
objects, calls the generated functions found through metadata.symbols, and
checks the existing runtime Frame/TypedError conventions.
"""
import json
import platform
import sys
import traceback
from pathlib import Path

sys.dont_write_bytecode = True
directory = Path(sys.argv[1]).resolve()
mode = sys.argv[2] if len(sys.argv) > 2 else 'identity'
sys.path.insert(0, str(directory))

import numpy as np
import torch
import langrt as rt
import program

metadata = json.loads((directory / 'metadata.json').read_text())
names = {entry['source_name']: entry['generated']
         for entry in metadata['symbols']}
row = {
    'status': 'failed',
    'mode': mode,
    'generated': {},
    'environment': {
        'python': platform.python_version(),
        'numpy': np.__version__,
        'torch': torch.__version__,
        'cuda_build': torch.version.cuda,
        'cuda_available': torch.cuda.is_available(),
        'platform': platform.platform(),
    },
}


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def generated(source_name):
    require(source_name in names, 'metadata.symbols has no ' + source_name)
    generated_name = names[source_name]
    require(isinstance(generated_name, str) and generated_name.startswith('_'),
            source_name + ' has an invalid generated name')
    function = getattr(program, generated_name, None)
    require(callable(function), source_name + ' is not callable')
    row['generated'][source_name] = generated_name
    return function


def cleanup(frames):
    owners = []
    for frame in frames:
        for key, owner in frame.owners.items():
            owners.append([key[0], key[1], owner.value is rt._DEAD])
    return {
        'frames': len(frames),
        'owners': owners,
        'owners_dead': all(dead for _, _, dead in owners),
        'borrows_empty': all(not frame.borrows for frame in frames),
    }


def call_traced(function):
    frames = []

    def trace(frame, event, arg):
        if frame.f_code.co_filename == str(directory / 'program.py'):
            language_frame = frame.f_locals.get('_frame')
            if language_frame is not None and \
                    all(language_frame is not old for old in frames):
                frames.append(language_frame)
        return trace

    sys.settrace(trace)
    try:
        return function(), frames
    finally:
        sys.settrace(None)


def guarded_calls(function):
    calls = []
    tensor_names = ('clone', 'detach', 'item', 'cpu', 'numpy')
    tensor_original = {name: getattr(torch.Tensor, name)
                       for name in tensor_names}

    def replacement(name, original):
        def wrapped(*args, **kwargs):
            calls.append(name)
            return original(*args, **kwargs)
        return wrapped

    try:
        for name, original in tensor_original.items():
            setattr(torch.Tensor, name, replacement(name, original))
        return function(), calls
    finally:
        for name, original in tensor_original.items():
            setattr(torch.Tensor, name, original)


def identity_mode():
    pass_array = generated('pass_array')
    pass_tensor = generated('pass_tensor')
    generated('main')

    array = np.arange(6, dtype=np.float32).reshape(2, 3)
    array_alias = array[:, 1:]
    tensor = torch.arange(6, dtype=torch.float32).reshape(2, 3)
    tensor.requires_grad_(True)
    tensor_view = tensor.view(3, 2)
    tensor_derived = tensor * 2.0

    def invoke():
        return (pass_array(array_alias), pass_tensor(tensor),
                pass_tensor(tensor_view), pass_tensor(tensor_derived))

    guarded, frames = call_traced(lambda: guarded_calls(invoke))
    results, calls = guarded
    returned_array, returned_tensor, returned_view, returned_derived = results
    frame_state = cleanup(frames)

    acceptance = {
        'array_identity': returned_array is array_alias,
        'array_shares_memory': bool(np.shares_memory(returned_array, array)),
        'tensor_identity': returned_tensor is tensor,
        'tensor_view_identity': returned_view is tensor_view,
        'tensor_derived_identity': returned_derived is tensor_derived,
        'tensor_data_ptr': returned_tensor.data_ptr() == tensor.data_ptr(),
        'tensor_view_data_ptr': returned_view.data_ptr() == tensor.data_ptr(),
        'tensor_derived_data_ptr':
            returned_derived.data_ptr() == tensor_derived.data_ptr(),
        'tensor_requires_grad': bool(returned_tensor.requires_grad),
        'tensor_leaf_grad_fn': returned_tensor.grad_fn is None,
        'tensor_derived_autograd':
            bool(returned_derived.requires_grad) and
            returned_derived.grad_fn is tensor_derived.grad_fn and
            returned_derived.grad_fn is not None,
        'no_tensor_clone_detach_item_cpu_numpy': calls == [],
        'frames_present': frame_state['frames'] >= 1,
        'owners_dead': frame_state['owners_dead'],
        'borrows_empty': frame_state['borrows_empty'],
    }

    # Host writes through the base object and through a returned alias must both
    # be visible.  These calls are test-side only and do not extend the source
    # language with ndarray/Tensor indexing or storage operations.
    array[0, 1] = 99.0
    acceptance['array_base_write_visible'] = (
        float(array_alias[0, 0]) == 99.0 and
        float(returned_array[0, 0]) == 99.0)
    returned_array[1, 1] = 77.0
    acceptance['array_alias_write_visible'] = float(array[1, 2]) == 77.0

    with torch.no_grad():
        tensor[0, 1] = 42.0
    acceptance['tensor_base_write_visible'] = (
        float(returned_tensor[0, 1].item()) == 42.0 and
        float(returned_view[0, 1].item()) == 42.0)
    with torch.no_grad():
        returned_view[2, 1] = -5.0
    acceptance['tensor_alias_write_visible'] = (
        float(tensor[1, 2].item()) == -5.0)

    row.update({
        'acceptance': acceptance,
        'frames': frame_state,
        'guard_calls': calls,
        'array': {
            'shape': list(returned_array.shape),
            'dtype': str(returned_array.dtype),
            'base_is_array': bool(np.shares_memory(returned_array, array)),
        },
        'tensor': {
            'shape': list(returned_tensor.shape),
            'dtype': str(returned_tensor.dtype),
            'device': str(returned_tensor.device),
            'requires_grad': bool(returned_tensor.requires_grad),
            'grad_fn': None if returned_tensor.grad_fn is None else
                type(returned_tensor.grad_fn).__name__,
            'derived_grad_fn': None if returned_derived.grad_fn is None else
                type(returned_derived.grad_fn).__name__,
        },
        'host_note': 'view/base and no_grad writes are observer-side only',
    })


def error_mode():
    fail_negative_num = generated('fail_negative_num')
    generated('main')
    frames = []

    def trace(frame, event, arg):
        if frame.f_code.co_filename == str(directory / 'program.py'):
            language_frame = frame.f_locals.get('_frame')
            if language_frame is not None and \
                    all(language_frame is not old for old in frames):
                frames.append(language_frame)
        return trace

    caught = None
    sys.settrace(trace)
    try:
        try:
            fail_negative_num()
        except rt.TypedError as error:
            caught = error
        else:
            raise AssertionError('generated call did not produce typed error')
    finally:
        sys.settrace(None)

    error = caught
    payload = error.value
    require(isinstance(payload, rt.Error), 'error payload is not langrt.Error')
    frame_state = cleanup(frames)
    source_path = Path(payload.file)
    expected_source = directory.parent / 'src' / 'main.lang'
    source = source_path.read_bytes()
    require(0 <= payload.begin_byte < payload.end_byte <= len(source),
            'invalid source span')
    span = source[payload.begin_byte:payload.end_byte].decode('utf-8')
    expected_span = 'np.linspace(0.0, 1.0, num: -1, dtype: np.float32)'
    expected_message = 'Number of samples, -1, must be non-negative.'
    traceback_text = payload.traceback_text

    payload_row = {
        'type_key': error.type_key,
        'payload_class': type(payload).__module__ + '.' +
            type(payload).__qualname__,
        'type_name': payload.type_name,
        'message': payload.message,
        'cause': None if payload.cause is None else
            type(payload.cause).__module__ + '.' +
            type(payload.cause).__qualname__ + ':' + str(payload.cause),
        'exception_class': type(payload.exception).__module__ + '.' +
            type(payload.exception).__qualname__,
        'traceback_has_error': 'ValueError: ' + expected_message in
            traceback_text,
        'operation': payload.operation,
        'file': str(source_path),
        'begin_byte': payload.begin_byte,
        'end_byte': payload.end_byte,
        'span': span,
    }

    acceptance = {
        'typed_error_key': error.type_key == 'lang.py.Error',
        'payload_is_lang_py_error': isinstance(payload, rt.Error),
        'original_exception_type': isinstance(payload.exception, ValueError),
        'original_type_name': payload.type_name == 'builtins.ValueError',
        'original_message': payload.message == expected_message,
        'original_exception_message': str(payload.exception) == payload.message,
        'original_cause_none': payload.cause is None,
        'traceback_present': 'Traceback (most recent call last):' in
            traceback_text,
        'traceback_has_original_error':
            'ValueError: ' + expected_message in traceback_text,
        'source_file': source_path.resolve() == expected_source.resolve(),
        'source_span': span == expected_span,
        'source_span_operation': payload.operation == 'numpy.linspace.scalar',
        'frames_present': frame_state['frames'] >= 1,
        'owners_dead': frame_state['owners_dead'],
        'borrows_empty': frame_state['borrows_empty'],
    }

    row.update({
        'acceptance': acceptance,
        'payload': payload_row,
        'frames': frame_state,
    })

    # Drop through the same typed-error convention used by generated code.
    error.drop()
    row['drop'] = {
        'payload_dead': error.value is rt._DEAD,
        'ops_cleared': error.ops is None,
    }
    acceptance['drop_clears_payload'] = (
        row['drop']['payload_dead'] and row['drop']['ops_cleared'])


try:
    if mode == 'identity':
        identity_mode()
    elif mode == 'error':
        error_mode()
    else:
        raise AssertionError('unknown observer mode: ' + mode)
    row['status'] = 'passed' if all(row['acceptance'].values()) else 'failed'
except BaseException as error:
    row['error'] = {
        'type': type(error).__module__ + '.' + type(error).__qualname__,
        'message': str(error),
    }
    row['traceback'] = traceback.format_exc(limit=20)
print(json.dumps(row, ensure_ascii=False, default=str))
raise SystemExit(0 if row['status'] == 'passed' else 1)
