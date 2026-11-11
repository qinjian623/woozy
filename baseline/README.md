# 词法与语法 v0.1

本目录保留 Woozy 第一版的词法与语法资料。语言发布名称为 Woozy，正式源文件后缀为 `.wz`。

## 内容

- `LEXICAL_SYNTAX.md`：完整设计草案。
- `grammar.ebnf`：词法与表层文法。
- `samples/01_basics.wz`：字面量、变量、结构体与循环。
- `samples/02_generics.wz`：具名 trait 约束与泛型调用。
- `samples/03_errors.wz`：throws、捕获和普通 Result 值。
- `samples/04_numpy_torch.wz`：NumPy 与 PyTorch 训练流程。
- `samples/05_traits.wz`：trait、impl 与显式 receiver。
- `samples/06_control_flow.wz`：inout、提前返回与 defer。
- `samples/07_functions.wz`：函数字面量、函数值与回调错误类型。
- `samples/negative/`：9 个按阶段分类的负例。
- `samples/numpy_torch_reference.py`：手写 Python 行为参考。
- `tools/check_syntax.py`：只依赖 Python 标准库的词法与语法检查器。
- `examples_notes.json`：机器可读的样例分类与行为预期。
- `VALIDATION.md`、`validation.json`：实际验证结果和范围。

## 先看什么

先读规范第 1 节的决策表，再看 `01_basics.wz` 与 `03_errors.wz`。关心 AI 计算代码时，直接看 `04_numpy_torch.wz`。需要实现 parser 时，结合 `grammar.ebnf` 和检查器阅读。

所有正例各自独立，不能把多个 main 文件当作一个包同时构建。

## 使用语法检查器

在解压后的本目录执行：

```sh
python3 tools/check_syntax.py --self-test
python3 tools/check_syntax.py samples/*.wz
python3 tools/check_syntax.py --json samples/04_numpy_torch.wz
```

目录参数会递归查找所有 `.wz`，包括负例。不要把“整个 negative 目录必须全部语法失败”作为验收条件：其中 4 个文件的语法合法，但后续语义检查应拒绝。

返回码：0 表示语法接受，1 表示拒绝，2 表示命令、文件或检查器资源问题。

## 能力状态

检查器只验证词法和语法，不能执行 `.wz`。名称解析、类型、泛型约束、throws、所有权、ARC 和后端代码生成尚未实现。

Python 参考文件可以在已安装 NumPy 与 PyTorch 的环境中运行。它不是转译器输出。当前交付验证了 Python 文件语法与 NumPy 数据准备，没有执行 PyTorch 训练。

## 设计重点

错误模型采用“函数声明 throws、调用自动传播、边界捕获”。普通计算语句不需要逐行问号；Result 仍可作为普通枚举值存储和匹配。

显式分号、参数名字在类型前、具名实参、方括号泛型、明确返回与简单块结构共同形成当前语法。完整原生系统接口和所有权模型将独立设计。

