# 实现任务提示模板

将 TASK_ID 替换为 `contracts/tasks.json` 中的任务 ID 后交给实现 agent。

```text
任务：实现 TASK_ID。

先读取 README.md、development/AGENTS.md、development/DEVELOPMENT.md、docs/SEMANTICS.md 和 contracts/tasks.json，
再读取该任务所引用的模块文档与验收条目。

确认依赖任务的实际产物和测试记录；不能仅依据“完成”状态跳过接口核对。
只修改本任务负责的目录。涉及公共 AST/HIR、诊断或绑定 schema 时先列出变更，
由集成负责人统一契约，再同步所有消费者。

实现可工作的最小完整功能，按阶段明确拒绝未支持语法/语义。
保留编译器公共前端独立运行，不在检查阶段加载 LLVM 或导入 Python 包。
不得静默改源码语义、抹掉 throws、引入逐调用问号、使用 Any 代替类型检查，
也不得用手写 Python 对照程序冒充生成结果。

按 development/docs/ACCEPTANCE.md 验证，记录命令、版本和实际结果。
不具备依赖/目标机器的测试标为 blocked/not_run，不得计入 passed。

交付：代码变更、必要测试、接口与文档更新、development/AGENTS.md 格式的完成报告。
若遇规范矛盾，给出具体程序、冲突条款和最小修订建议；继续完成不受影响的工作。
```
