# 开发文档包验证记录

本文件记录本轮交付包检查，不替代未来编译器的验收报告。具体机器结果由 `tools/validate_package.py --write-report` 写入 `package_validation.json`。

## 已检查的范围

- Markdown 相对文件链接及代码围栏完整性。
- 所有 JSON 文件可解析，任务 ID 唯一、依赖无环、任务输入路径存在。
- 诊断、Python 绑定和测试案例的实例满足本包 schema 使用的字段约束。
- Python 绑定的模块、类型、方法 owner、返回值和参数引用一致。
- 旧规范包的 25 个文件与原始快照 SHA-256 一致。
- 基线 59 项词法/语法自测，以及基线 16 个文件和新增 5 个样例的语法分类。

包内校验器使用 Python 标准库，实现本包所用 JSON Schema 关键字的受限检查；未知关键字会报错。它不是通用 JSON Schema 引擎，也不运行绑定的宿主库。

## 复核后统一的规则

- 错误码统一为 `E_CAPABILITY_UNAVAILABLE`。
- Python finally 区分 Panic，避免意外执行语言 defer；清理本身 Panic 也跳过外层后续清理。
- 首版 inout 限定平凡值整体绑定，避免与共享 Vec 的深层别名规则冲突。
- 明确 for、for-in、case、catch 和函数参数作用域。
- 明确完整表达式临时值、部分构造、聚合成员的销毁顺序。
- C++ owned 句柄须有 ARC 共享包装或 retain/release，不能隐含引入 move-only 类型。
- Python 错误类型使用稳定身份，不能持久化会话内 TypeId 编号。
- Vec 活跃迭代期间的长度修改在修改前拒绝，不仅在下一轮检查。
- NumPy linspace 的有限绑定把 dtype 限为命名参数；可能返回视图或保留 autograd 输入的调用采用保守 escape 声明。

## 尚未执行

C++ 编译器构建、语义检查器、LLVM 代码生成、Python 转译、ARC 运行库、真实绑定 conformance、PyTorch 训练和多平台执行均未在本轮实现或运行。对应测试是开发任务的验收要求，不能把本包检查通过记为这些功能完成。

基线验证文件中的 NumPy 数据准备记录属于上轮历史结果，保留其原始时间和范围；本轮没有重新执行训练或声称已经生成 Python 程序。

## 2026-11-13 捕获式 C 回调

在主目录完成 Release 构建后，使用 LLVM/Clang 22.1.8 和现有 legacy libxml2 运行完整 CTest，**176/176 全部通过，耗时 191.60 秒**。命令：

```sh
ctest --test-dir build/woozy-stdlib-native --output-on-failure -j6
```

新增的 13 个真实 Source 项目在 O0 和 O2 下全部运行，覆盖逃逸闭包、函数变量、绑定方法、不同位置的 userdata、bool/i64/Unit、注册期间同步调用、C pthread、失败与 typed-error 清理、显式/自动注销、双句柄借用、Weak 清理和 move-only 句柄捕获。CTest 强制要求源码实际执行，本轮没有 failed 或 not_run。核心 ELF 仍无 INTERP 和 NEEDED，旧 C 接口、静态回调和双后端检查继续通过。

独立复核发现并修正了以下问题：外层 SAFE 下清理捕获环境、并发注销等待阻碍 GC、多句柄借用退出顺序，以及注册未完成时 fork 后缺少 PID 检查；运行库测试已补上对应断言。纯 Woozy 对象仍按普通 ARC/GC 清理；C 回调注册属于外部根，形成循环时需要显式 close。

测试说明：[Source/C fixture](tests/native/c_retained_capturing/README.md)、[运行库](tests/native/c_callback_runtime/README.md)；用法：[调用C库](../docs/FFI.md)。完整自举仍未实现，本次完成的是既定扩展与自举条件评估。

最后的 PID 修正先在新增的进程测试上复现了旧实现失败：无论是注册成功还是 finish 为 null，子进程继承句柄后继续使用都会在加锁和清理前触发 Panic，父进程正常清理一次。完成该修正和测试目录隔离后，完整 176 项 CTest 重新执行通过。
