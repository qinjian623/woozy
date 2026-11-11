"""手工编写的 Python 行为参考，不是尚不存在的 .wz 编译器输出。

依赖真实的 NumPy 与 PyTorch。训练函数返回原始 torch.Tensor。
只在本文件的命令行入口显式调用 item() 以输出标量。
"""

import numpy as np
import torch
from torch import nn, optim
from torch.nn import functional as F


def train() -> torch.Tensor:
    torch.manual_seed(7)

    values = np.linspace(-1.0, 1.0, num=128, dtype=np.float32)
    x_np = np.expand_dims(values, axis=1)
    scaled = np.multiply(x_np, 2.0)
    shifted = np.add(scaled, 1.0)
    y_np = np.asarray(shifted, dtype=np.float32)

    x = torch.tensor(x_np, dtype=torch.float32, device="cpu")
    y = torch.tensor(y_np, dtype=torch.float32, device="cpu")

    model = nn.Linear(1, 1, device="cpu", dtype=torch.float32)
    optimizer = optim.SGD(model.parameters(), lr=0.1)

    for _ in range(200):
        optimizer.zero_grad(set_to_none=True)
        prediction = model(x)
        loss = F.mse_loss(prediction, y)
        loss.backward()
        optimizer.step()

    final_prediction = model(x)
    return F.mse_loss(final_prediction, y)


if __name__ == "__main__":
    print(train().item())
