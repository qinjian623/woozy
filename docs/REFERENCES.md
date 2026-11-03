# 官方资料和适用范围

资料核对日期：2026-11-03。下列资料用于理解已有工具的能力；本语言规则以本包规范为准。表中主分支页面可能随时间变化，实际 API 以 LLVM 22.1.8 源码/头文件为准。

| 资料 | 用途 |
| --- | --- |
| [LLVM 22.1.8 发行页](https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.8) | 固定工具链版本、平台物料与校验信息 |
| [LLVM 发行文档](https://releases.llvm.org/22.1.0/docs/) | 22.1 系列参考入口 |
| [LLVM Parser 教程](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl02.html) | 递归下降与表达式优先级解析的最小示例 |
| [LLVM 22.1 IR 生成教程](https://releases.llvm.org/22.1.0/docs/tutorial/MyFirstLanguageFrontend/LangImpl03.html) | Context、Module、IRBuilder、函数验证 |
| [LLVM 目标文件教程](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl08.html) | target、TargetMachine、DataLayout、目标文件 |
| [LLVM Language Reference](https://llvm.org/docs/LangRef.html) | 指令、poison、整数标记、属性和调用约定 |
| [LLVM New Pass Manager](https://llvm.org/docs/NewPassManager.html) | PassBuilder 与优化流水线 |
| [LLVM 前端性能建议](https://llvm.org/docs/Frontend/PerformanceTips.html) | 正确表达语言属性、内存效果和优化信息 |
| [LLVM Exception Handling](https://llvm.org/docs/ExceptionHandling.html) | 平台异常机制；第一版不采用平台 unwind 实现 typed throws |
| [LLVM Developer Policy](https://llvm.org/docs/DeveloperPolicy.html) | C API 稳定性与版本演进边界 |
| [Swift SIL](https://github.com/swiftlang/swift/blob/main/docs/SIL/SIL.md) | 自有高层 IR 与 LLVM 的分工参考 |
| [Swift Ownership SSA](https://github.com/swiftlang/swift/blob/main/docs/SIL/Ownership.md) | 拥有、借用及数据流不变量参考，不直接照搬全部复杂度 |
| [Clang ARC](https://clang.llvm.org/docs/AutomaticReferenceCounting.html) | ARC 依赖具体对象和所有权约定，不能作为任意语言开关 |
| [Swift C++ 互操作](https://www.swift.org/documentation/cxx-interop/) | Clang 导入和跨语言所有权边界参考 |
| [Clang 交叉编译](https://clang.llvm.org/docs/CrossCompilation.html) | 目标、SDK、库和工具链配置 |
| [MLIR Toy](https://mlir.llvm.org/docs/Tutorials/Toy/) | 后续需要高层张量优化时评估的基础设施 |
| [MLIR 降低到 LLVM](https://mlir.llvm.org/docs/Tutorials/Toy/Ch-6/) | dialect 转换与 LLVM 输出 |
| [Tree-sitter](https://github.com/tree-sitter/tree-sitter) | 可选编辑器增量 CST |
| [ANTLR](https://www.antlr.org/) | 文法生成解析器的备选路线 |
| [NumPy typing](https://numpy.org/doc/stable/reference/typing.html) | NumPy 类型约束与运行时边界 |
| [NumPy linspace](https://numpy.org/doc/stable/reference/generated/numpy.linspace.html) | 示例数据构造接口 |
| [PyTorch Module](https://docs.pytorch.org/docs/stable/generated/torch.nn.Module.html) | 保持实例调用和 hook 行为 |
| [PyTorch tensor](https://docs.pytorch.org/docs/stable/generated/torch.tensor.html) | 显式创建 Tensor 的数据边界 |
| [PyTorch from_numpy](https://docs.pytorch.org/docs/stable/generated/torch.from_numpy.html) | 后续共享内存绑定，不等于 tensor 的复制语义 |
| [PyTorch detach](https://docs.pytorch.org/docs/stable/generated/torch.Tensor.detach.html) | 梯度与共享存储行为，不作为隐式复制 |
| [PyTorch SGD](https://docs.pytorch.org/docs/stable/generated/torch.optim.SGD.html) | 第一批优化器接口 |

本包的 Python 绑定是供实施的有限签名集合，必须针对实际锁定的依赖做 conformance 测试。引用文档不等于已经在交付环境安装或执行这些库。
