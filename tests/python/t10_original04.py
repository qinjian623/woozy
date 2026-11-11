"""Execute a generated original-04 train function with real CPU PyTorch.

The generated train function is not modified.  This observer only wraps host
functions and records test-side observations; all explicit detach/clone/item
calls stay outside the generated function.
"""
import json
import math
import platform
import sys
import traceback
from pathlib import Path

sys.dont_write_bytecode = True
directory = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(directory))

import numpy as np
import torch
import torch.nn.functional as F
import langrt as rt
import program

row = {'status': 'failed'}
try:
    metadata = json.loads((directory / 'metadata.json').read_text())
    names = {s['source_name']: s['generated'] for s in metadata['symbols']}
    if 'train' not in names:
        raise AssertionError('Output.symbols has no train')
    train = getattr(program, names['train'])
    info = program._LANG_BINDING_INFO
    bundle = info['bundles'][0]
    binding_info = {
        'runtime_version': info['runtime_version'],
        'contract': bundle['contract'],
        'sha256': bundle['sha256'],
        'review_status': bundle['review_status'],
        'modules': [m['logical_path'] + ':' + m['host_module']
                    for m in bundle['modules']],
        'package_requirements': [r['package'] + ':' + r['version_policy']
                                 for r in bundle['package_requirements']],
    }
    environment = {
        'python': platform.python_version(),
        'numpy': np.__version__,
        'torch': torch.__version__,
        'cuda_build': torch.version.cuda,
        'cuda_available': torch.cuda.is_available(),
        'platform': platform.platform(),
    }
    counts = {'forward': 0, 'mse': 0, 'backward': 0, 'zero_grad': 0, 'step': 0}
    first_x = []
    first_target = []
    losses = []
    step_before = []
    step_after = []
    seeds = []
    linear_specs = []
    sgd_specs = []

    original_manual_seed = torch.manual_seed
    original_linear_init = torch.nn.Linear.__init__
    original_sgd_init = torch.optim.SGD.__init__
    original_mse = F.mse_loss
    original_backward = torch.Tensor.backward
    original_zero_grad = torch.optim.SGD.zero_grad
    original_step = torch.optim.SGD.step

    def manual_seed(seed):
        seeds.append(seed)
        return original_manual_seed(seed)

    def linear_init(self, in_features, out_features, *args, **kwargs):
        linear_specs.append({
            'in_features': in_features,
            'out_features': out_features,
            'device': kwargs.get('device'),
            'dtype': kwargs.get('dtype'),
        })
        return original_linear_init(self, in_features, out_features, *args, **kwargs)

    def sgd_init(self, params, *args, **kwargs):
        sgd_specs.append({'lr': kwargs.get('lr', args[0] if args else None)})
        return original_sgd_init(self, params, *args, **kwargs)

    def mse(input, target):
        counts['mse'] += 1
        losses.append(float(original_mse(input, target).detach()))
        if not first_target:
            first_target.append(target.detach().clone())
        return original_mse(input, target)

    def backward(self, *args, **kwargs):
        counts['backward'] += 1
        return original_backward(self, *args, **kwargs)

    def zero_grad(self, *args, **kwargs):
        counts['zero_grad'] += 1
        return original_zero_grad(self, *args, **kwargs)

    def step(self, *args, **kwargs):
        parameters = self.param_groups[0]['params']
        before = [p.detach().clone() for p in parameters]
        result = original_step(self, *args, **kwargs)
        after = [p.detach().clone() for p in parameters]
        step_before.append(before)
        step_after.append(after)
        counts['step'] += 1
        return result

    def hook(module, inputs, output):
        counts['forward'] += 1
        if not first_x and inputs:
            first_x.append(inputs[0].detach().clone())

    handle = torch.nn.modules.module.register_module_forward_hook(hook)
    torch.manual_seed = manual_seed
    torch.nn.Linear.__init__ = linear_init
    torch.optim.SGD.__init__ = sgd_init
    F.mse_loss = mse
    torch.Tensor.backward = backward
    torch.optim.SGD.zero_grad = zero_grad
    torch.optim.SGD.step = step
    try:
        result = train()
    finally:
        handle.remove()
        torch.manual_seed = original_manual_seed
        torch.nn.Linear.__init__ = original_linear_init
        torch.optim.SGD.__init__ = original_sgd_init
        F.mse_loss = original_mse
        torch.Tensor.backward = original_backward
        torch.optim.SGD.zero_grad = original_zero_grad
        torch.optim.SGD.step = original_step

    value = float(result.detach())
    changed = sum(int(not torch.equal(before, after))
                  for before, after in zip(step_before, step_after)
                  for before, after in zip(before, after))
    first_parameter = step_before[0][0] if step_before and step_before[0] else None
    last_parameter = step_after[-1][0] if step_after and step_after[-1] else None
    parameter_changed = first_parameter is not None and last_parameter is not None and \
        not torch.equal(first_parameter, last_parameter)
    x = first_x[0] if first_x else None
    target = first_target[0] if first_target else None
    data = {
        'input_shape': [] if x is None else list(x.shape),
        'input_dtype': None if x is None else str(x.dtype),
        'input_device': None if x is None else str(x.device),
        'target_shape': [] if target is None else list(target.shape),
        'target_dtype': None if target is None else str(target.dtype),
        'target_device': None if target is None else str(target.device),
        'target_matches_linear': bool(x is not None and target is not None and
                                      torch.equal(target, x * 2.0 + 1.0)),
    }
    initial_loss = losses[0] if losses else None
    final_loss = losses[-1] if losses else None
    result_shape = list(result.shape)
    acceptance = {
        'finite': math.isfinite(value),
        'nonnegative': value >= 0.0,
        'less_than_1e-3': value < 1e-3,
        'forward_201': counts['forward'] == 201,
        'mse_201': counts['mse'] == 201,
        'backward_200': counts['backward'] == 200,
        'zero_grad_200': counts['zero_grad'] == 200,
        'step_200': counts['step'] == 200,
        'hook_executed': counts['forward'] > 0,
        'parameters_changed': changed > 0 and parameter_changed,
        'result_zero_dim': result_shape == [],
        'result_cpu_float32': str(result.dtype) == 'torch.float32' and
                              str(result.device) == 'cpu',
        'result_requires_grad': bool(result.requires_grad),
        'result_grad_fn': result.grad_fn is not None,
        'input_128x1_float32_cpu': data['input_shape'] == [128, 1] and
                                   data['input_dtype'] == 'torch.float32' and
                                   data['input_device'] == 'cpu',
        'target_128x1_float32_cpu': data['target_shape'] == [128, 1] and
                                    data['target_dtype'] == 'torch.float32' and
                                    data['target_device'] == 'cpu',
        'target_is_2x_plus_1': data['target_matches_linear'],
        'seed_7': seeds == [7],
        'linear_1x1_cpu_float32': len(linear_specs) == 1 and
            linear_specs[0]['in_features'] == 1 and
            linear_specs[0]['out_features'] == 1 and
            linear_specs[0]['device'] == 'cpu' and
            linear_specs[0]['dtype'] == torch.float32,
        'sgd_lr_0_1': len(sgd_specs) == 1 and sgd_specs[0]['lr'] == 0.1,
        'initial_loss_finite': initial_loss is not None and math.isfinite(initial_loss),
        'final_loss_decreased': initial_loss is not None and final_loss is not None and
                                math.isfinite(final_loss) and final_loss < initial_loss,
        'environment_python_3_12_15': environment['python'] == '3.12.15',
        'environment_numpy_2_5_4': environment['numpy'] == '2.5.4',
        'environment_torch_2_15_0_cpu': environment['torch'] == '2.15.0+cpu',
        'environment_no_cuda_build': environment['cuda_build'] is None,
        'environment_cuda_unavailable': environment['cuda_available'] is False,
        'binding_info_contract': binding_info['contract'] == 'compiler-dev/1',
        'binding_info_sha': binding_info['sha256'] ==
            'c118c3e4912234dc752355fd8f41b6274433c86a3448fdc53dcdb63d7aa2cd71',
        'binding_info_draft': binding_info['review_status'] ==
            'draft_requires_conformance',
    }
    row.update({
        'status': 'passed' if all(acceptance.values()) else 'failed',
        'counts': counts,
        'losses': {'initial': initial_loss, 'final': final_loss,
                   'history_len': len(losses)},
        'result': {
            'type': type(result).__module__ + '.' + type(result).__qualname__,
            'shape': result_shape,
            'dtype': str(result.dtype),
            'device': str(result.device),
            'requires_grad': bool(result.requires_grad),
            'grad_fn': None if result.grad_fn is None else type(result.grad_fn).__name__,
            'value': value,
        },
        'data': data,
        'changed_parameter_snapshots': changed,
        'environment': environment,
        'binding_info': binding_info,
        'acceptance': acceptance,
    })
except BaseException as error:
    row['error'] = {'type': type(error).__module__ + '.' + type(error).__qualname__,
                    'message': str(error)}
    row['traceback'] = traceback.format_exc(limit=20)
print(json.dumps(row, ensure_ascii=False, default=str))
raise SystemExit(0 if row['status'] == 'passed' else 1)
