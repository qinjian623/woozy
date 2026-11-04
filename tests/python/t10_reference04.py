"""Execute the untouched handwritten reference independently."""
import hashlib
import importlib.util
import json
import math
import sys
from pathlib import Path

sys.dont_write_bytecode = True
path = Path(sys.argv[1]).resolve()
row = {'status': 'failed', 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
try:
    import torch
    spec = importlib.util.spec_from_file_location('t10_reference04_module', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    result = module.train()
    value = float(result.detach())
    row.update({
        'status': 'passed',
        'result': {
            'type': type(result).__module__ + '.' + type(result).__qualname__,
            'shape': list(result.shape),
            'dtype': str(result.dtype),
            'device': str(result.device),
            'requires_grad': bool(result.requires_grad),
            'grad_fn': None if result.grad_fn is None else type(result.grad_fn).__name__,
            'value': value,
        },
        'acceptance': {'finite': math.isfinite(value), 'nonnegative': value >= 0.0,
                       'less_than_1e-3': value < 1e-3,
                       'tensor': isinstance(result, torch.Tensor),
                       'scalar': result.ndim == 0,
                       'cpu_float32': result.device.type == 'cpu' and result.dtype == torch.float32,
                       'autograd': bool(result.requires_grad) and result.grad_fn is not None},
    })
    if not all(row['acceptance'].values()):
        row['status'] = 'failed'
except BaseException as error:
    row['error'] = type(error).__module__ + '.' + type(error).__qualname__ + ': ' + str(error)
print(json.dumps(row, ensure_ascii=False))
raise SystemExit(0 if row['status'] == 'passed' else 1)
