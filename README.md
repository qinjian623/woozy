# Woozy

Woozy 是一门编译型语言，提供 Native 和 Python 两个后端。本仓库包含编译器实现、原生运行库、语言规范和测试。编译器命令为 `woozyc`，源文件后缀为 `.wz`。

`woozyc` 已能检查项目并生成 Native 与 Python 程序。现有实现包括整数、严格标量浮点、函数与非捕获函数值、已有泛型和 trait 组合、struct/enum、Vec、typed throws 与词法块 defer。具体支持的组合和限制见 [GENERICS.md](docs/GENERICS.md)、[值与清理执行说明](docs/T09_EXECUTION.md)、[字符串说明](docs/CONSTANT_STRINGS.md)。

截至 2026-11-10，已完成现有 v0.1 语法和 [SEMANTICS.md](docs/SEMANTICS.md) 已定义的核心要求；文档明确延后的语言扩展和完整标准库另做。Release、Debug、Sanitizer、LLVM OFF 四配置共 290 项检查通过，无失败或跳过。8 种算法各 2 个固定规模的正式测速均在等价 C++ 实现耗时的 2 倍以内，具体结果和测试范围见 [性能记录](docs/PERFORMANCE.md)。

原始 `baseline/` 规范包保留旧 `.lang` 文件名及原始字节，作为语法对照；当前项目、样例和测试使用 `.wz`。项目清单仍名为 `lang.toml`。

## 从哪里开始

学习和使用语言：

- [教程](docs/TUTORIAL.md)：从第一个项目开始，逐步学习函数、Vec、结构体、错误处理、模块和泛型。
- [快速入门](docs/QUICKSTART.md)：面向有编程经验的读者，列出构建、运行命令和与其他语言容易混淆的规则。

参与编译器开发：

1. 阅读 [DEVELOPMENT.md](DEVELOPMENT.md)，了解目标、架构和交付边界。
2. 阅读 [AGENTS.md](AGENTS.md) 和 [语义约定](docs/SEMANTICS.md)，避免不同 agent 自行选择不同语言行为。
3. 从 [任务清单](contracts/tasks.json) 选择依赖已完成的任务，结合 [实施计划](docs/IMPLEMENTATION_PLAN.md) 开始开发。
4. 根据任务阅读前端、IR 或后端文档；使用 [验收规范](docs/ACCEPTANCE.md) 报告结果。

## 文件导航

| 路径 | 内容 |
| --- | --- |
| `docs/TUTORIAL.md` | 逐步编写、检查和运行 Woozy 程序 |
| `docs/QUICKSTART.md` | 面向有编程经验读者的命令与语法速查 |
| `DEVELOPMENT.md` | 总体开发说明、范围、接口责任、实施决策 |
| `AGENTS.md` | agent 工作规则、分工和交接格式 |
| `docs/SEMANTICS.md` | 数值、模块、所有权、错误与清理的实施默认语义 |
| `docs/FRONTEND.md` | lexer、parser、名称解析、类型和诊断 |
| `docs/IR_CONTRACTS.md` | AST、HIR、原生 IR 的数据结构与不变量 |
| `docs/BACKENDS.md` | LLVM、Python 代码生成与链接 |
| `docs/INTEROP.md` | Python 绑定、C++ 包装和系统接口 |
| `docs/IMPLEMENTATION_PLAN.md` | T00 至 T12 的依赖、职责、交付物 |
| `docs/ACCEPTANCE.md` | 分阶段验收和差分测试 |
| `docs/TOOLING.md` | 项目清单、CLI、LSP、缓存、诊断协议 |
| `docs/REFERENCES.md` | 官方资料及适用范围 |
| `contracts/` | 机器可读任务、工具链策略、绑定、诊断、能力和测试契约 |
| `examples/` | 新增最小样例与项目清单格式样例 |
| `prompts/IMPLEMENT_TASK.md` | 可直接交给实现 agent 的任务提示模板 |
| `baseline/` | 原样保留的词法语法 v0.1、7 个正例、9 个负例和检查器 |
| `VALIDATION.md` | 本交付包实际完成的检查与尚未执行的编译器验收 |

## 当前可以运行的检查

```bash
python3 baseline/tools/check_syntax.py --self-test
python3 baseline/tools/check_syntax.py baseline/samples/*.lang
python3 tools/validate_package.py
```

第三条检查文档包、相对链接、JSON 契约、任务依赖和样例的语法分类。它不是编译器测试。

构建与使用见 [BUILD.md](docs/BUILD.md)。当前可执行：

```sh
cmake --preset frontend-debug
cmake --build --preset frontend-debug
ctest --preset frontend-debug
build/frontend-debug/woozyc check --syntax-only --format json examples/core.wz
```

项目语义检查使用 `woozyc check --project MANIFEST`；生成程序使用 `woozyc build --project MANIFEST --backend native|python --out-dir DIR`，构建命令不运行程序。原生生成需要开启 LLVM 22.1.8，Python 运行测试使用独立 3.12.15 解释器。构建方法与实际输出见 [BUILD.md](docs/BUILD.md)。LSP 和 capabilities 命令仍明确报告未支持。
