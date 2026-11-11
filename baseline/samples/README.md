# v0.1 示例说明

这些文件使用 Woozy 正式的 `.wz` 后缀，保留 v0.1 的词法、语法和行为预期。具体执行支持见 [Woozy 说明](../../README.md)，语法通过不代表程序已经运行通过。

每个 `.wz` 文件是独立示例，并以 `package main;` 开始。不要将多个示例拼成同一模块：不同文件会有意复用 `main`、`Point` 等名称。`import` 声明位于 package 之后、其他声明之前。

## 正例

| 文件 | 重点 | 预期行为 |
| --- | --- | --- |
| `01_basics.wz` | 数字与字符串字面量、初始化、结构体、C 式和条件式 `for` | `main()` 返回 `0`；平方距离为 `25.0`，最终计数为 `10` |
| `02_generics.wz` | 本地 `Ordered` trait、约束泛型、显式泛型调用、`Vec[T]`、`for-in` | 从 `[4, 9, 2]` 中求得 `9`，`main()` 返回 `0` |
| `03_errors.wz` | 类型化 `throws`、自然传播、`try/catch`、普通 `Result` 数据 | 捕获负数错误；收集一个成功和一个失败结果；`main()` 返回 `0` |
| `04_numpy_torch.wz` | Python 绑定、命名参数、NumPy 数据处理与 CPU 训练 | 200 次 SGD 更新后返回真实的标量 loss Tensor；不自动标量化或断开梯度 |
| `05_traits.wz` | `trait`、`impl`、显式 `self` 参数、方法调用 | `(3, 4)` 的平方范数为 `25.0`，`main()` 返回 `0` |
| `06_control_flow.wz` | `inout`、`for-in`、`continue`、提前返回、`defer` | 求和为 `5`，清理计数为 `1`，`main()` 返回 `0` |
| `07_functions.wz` | 函数字面量、函数值、返回函数的括号、回调的 `throws` | 函数调用结果为 `6`；返回的回调错误被捕获，`main()` 返回 `0` |

样例中的 `Ordered` 与 `Result` 都有本地完整声明，不依赖尚未定义的同名 prelude 类型。`Vec[T]` 是本文采用的标准集合类型名。列表字面量的元素可由上下文确定，例如 `Vec[i32]`。

本稿允许显式枚举作为 `throws` 错误类型，不要求示例另外实现 `Error` trait。`Result` 是普通类型名；本地定义的 `Result[T, E]` 不会触发特殊解包。

## Python 参考实现

`numpy_torch_reference.py` 是手工编写的 Python 行为参考，**不是 `.wz` 编译器生成的文件**。它与 `04_numpy_torch.wz` 的训练流程对应，并在 Python 命令行入口显式调用 `item()` 输出数值；训练函数本身仍返回 `torch.Tensor`。

绑定解析约定：

- `std/py` 提供语言的 Python 错误桥接类型；不表示运行时存在名为 `std.py` 的第三方 Python 包。
- `torch/nn`、`torch/optim`、`torch/nn/functional` 分别映射到真实 Python 子模块。
- `model(x)` 保留模块实例调用，不替换成 `model.forward(x)`。
- 数据边界的 `torch.tensor(...)` 是源码明确要求的复制。编译器不得把它变成任意现有 Tensor 的隐式适配。
- Python 失败必须按绑定的 `throws py.Error` 契约传播，并保留原始异常信息；类型桩不证明外部实现必然符合声明。

当前验证状态：Python 参考文件通过 Python 语法编译；实际运行了 NumPy 数据准备部分，两组数据均为 `(128, 1)`、`float32`。当前环境未安装 PyTorch，因此未执行训练，也没有验证最终 loss 或性能。

## 负例与检查阶段

| 文件 | 预期拒绝阶段 | 语法检查器应如何处理 |
| --- | --- | --- |
| `negative/01_missing_semicolon.wz` | 语法：声明缺少分号 | 拒绝 |
| `negative/02_invalid_string_escape.wz` | 词法：未知转义 `\q` | 拒绝 |
| `negative/03_positional_after_named.wz` | 调用语法：命名参数后出现位置参数 | 拒绝 |
| `negative/04_unhandled_throws.wz` | 静态效果：非 `throws` 函数未捕获错误 | 接受语法；后续语义检查应拒绝 |
| `negative/05_type_mismatch.wz` | 类型：字符串赋给 `i32` | 接受语法；后续语义检查应拒绝 |
| `negative/06_uninitialized_binding.wz` | 语法：`let` 缺少初始化表达式 | 拒绝 |
| `negative/07_result_is_not_unwrapped.wz` | 类型：把 `Result` 当成 `i32` | 接受语法；后续语义检查应拒绝 |
| `negative/08_throwing_cleanup.wz` | 静态清理规则：`defer` 内留下未处理错误 | 接受语法；后续语义检查应拒绝 |
| `negative/09_ambiguous_function_return_type.wz` | 语法：函数返回类型没有括号化 | 拒绝 |

语法通过不能证明类型正确、所有权安全、外部绑定正确或程序能够执行。语义负例被语法检查器接受，正是阶段划分的预期结果。

若从压缩包根目录检查全部正例，可运行：

```sh
python tools/check_syntax.py samples/*.wz
```

不要直接要求整个 `samples/negative/` 目录全部产生语法错误；其中有意包含语法合法、语义非法的程序。机器可读分类位于包根目录的 `examples_notes.json`。
