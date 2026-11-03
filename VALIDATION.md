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
