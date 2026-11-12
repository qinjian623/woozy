# 编译器开发资料

这里保存 Woozy 编译器的构建说明、实现记录、测试说明和 agent 工作要求。语言教程、快速入门与语言规则见 [语言文档](../docs/)。本目录中的阶段记录保留当时的状态；当前项目介绍见 [仓库首页](../README.md)。

| 要做的事 | 入口 |
| --- | --- |
| 构建编译器、运行示例 | [构建与运行](docs/BUILD.md) |
| 了解源码结构与开发约定 | [开发说明](DEVELOPMENT.md)、[前端](docs/FRONTEND.md)、[IR](docs/IR_CONTRACTS.md)、[后端](docs/BACKENDS.md) |
| 查询实现范围 | [泛型与方法](docs/GENERICS.md)、[动态 trait 对象](docs/TRAIT_OBJECTS.md)、[值与清理](docs/T09_EXECUTION.md)、[互操作](docs/INTEROP.md) |
| 了解原生线程实现 | [线程、锁与原子计数](docs/CONCURRENCY_NATIVE.md) |
| 查看检查和性能记录 | [检查记录](VALIDATION.md)、[验收要求](docs/ACCEPTANCE.md)、[性能记录](docs/PERFORMANCE.md) |
| 查测试说明 | [语法测试](tests/syntax/README.md)、[模块测试](tests/modules/README.md)、[类型检查](tests/sema/README.md)、[跨后端测试](tests/backend_common/README.md)、[性能测试](tests/performance/README.md) |
| 交给 agent 开发 | [工作要求](AGENTS.md)、[任务提示模板](prompts/IMPLEMENT_TASK.md)、[任务清单](../contracts/tasks.json) |

测试说明在 `development/tests/` 保留原测试目录结构，测试源码与执行脚本仍在仓库根的 `tests/`。语言规范原包位于 [baseline/](../baseline/)，供语言设计与历史样例对照。
