# 工程工具和诊断接口

版本：`compiler-dev/1`。本文件定义待实现接口，当前可运行工具只有包内语法原型和文档包校验器。

## 1 工具链策略

正式编译器采用 C++20，LLVM/Clang/LLD 22.1.8，CMake 和 Ninja 构建。LLVM 22.1.8 的存在已通过 [官方发行页](https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.8) 核实；该选择是固定开发基线，不是追踪最新版。

[toolchain.policy.json](../contracts/toolchain.policy.json) 是选择策略，不是已安装环境的证明。T00 生成真实 lock，记录工具版本、来源、物料摘要、构建选项和目标 SDK；不提交开发机专用的绝对路径。消费已有 LLVM 二进制库时，核对编译器、C++ runtime、RTTI/exception 配置和链接模式。前端可采用不依赖 LLVM 的独立构建目标。

Python profile 以 Python 3.12.x 为首个支持基线；精确补丁版本由实际环境锁定。NumPy/PyTorch CPU 版本在 T10 的真实集成环境锁定，当前不伪造已验证的依赖组合。

LLVM API 示例优先查 22.1.x 发布文档和 22.1.8 头文件，禁止从主分支复制接口后自行放宽版本检查。版本迁移需要同时更新 IR 发射、构建、测试与缓存 key。

## 2 项目清单

采用 `lang.toml` 作为暂定文件名，示例见 [project.example.toml](../examples/project.example.toml)。这是工具链数据格式，不增加源语言语法。

解析规则：

1. 清单中的相对文件路径相对于清单目录；import 字符串始终是逻辑模块名。
2. `std` 由选定工具链的标准库清单唯一解析，不允许项目依赖遮蔽。
3. `app` 对应 `package.source_root`；`app/foo` 对应其下明确声明或可定位的 foo 模块。
4. 其他首段必须出现在 dependencies。source 依赖指定来源，python 依赖指定绑定 bundle 与宿主包要求，native 依赖指定 ABI 声明及目标库。
5. 安装在系统位置的库也先登记为逻辑依赖。第一版不实现通过任意 PATH 搜索隐式选择“第一个同名库”。
6. 锁文件记录模块身份、内容摘要、绑定版本、来源和目标支持。两个位置提供同一逻辑模块时必须报冲突，不能靠扫描顺序决定。
7. 包循环在构建图阶段拒绝；函数递归正常允许。当前不运行顶层动态初始化。

标准库位置可以由工具链安装目录、显式 `--sysroot` 或配置指定。后两者进入有效配置与缓存 key；源码不随机器路径变化。导入补全使用同一个模块索引，支持标准库、项目库和已声明依赖。

T03 采用以下目录约定：每个模块读取对应目录直接包含的 `.lang` 文件，按文件名排序；`app/foo` 读取 `package.source_root/foo/`，不把 `foo.lang` 同时当成候选。source 依赖的 path 指项目目录，存在 `lang.toml` 时读取其 source_root 和 dependencies，否则显式 path 本身作为源码根。库的 entry 不影响导入模块的选择。源码目录先取 canonical 身份，同一目录经不同别名导入只加载一次；不同来源提供同一逻辑模块时报告冲突。

## 3 CLI 目标接口

以下列出目标命令。当前 `langc` 支持单文件 syntax-only；项目和名称检查已由前端库实现，完整 check 与 build 等待后续阶段：

```bash
langc check --syntax-only examples/core.lang
langc check --project lang.toml --backend native --format json
langc check --project lang.toml --backend python --format json
langc build --project lang.toml --backend native --emit llvm
langc build --project lang.toml --backend native --target x86_64-unknown-linux-gnu
langc build --project lang.toml --backend python --out-dir build/python
langc capabilities --format json
langc lsp
```

`check --syntax-only` 不要求依赖安装或链接器；全量 check 读取项目、绑定、目标能力并检查语义，但不运行代码、不启动 Python、不加载 LLVM 后端。build 才进行代码生成和链接；build 默认不执行产物。生成 Python 之后的运行是显式独立步骤。

### 3.1 退出码

| 命令状态 | langc 退出码 |
| --- | --- |
| 检查或构建成功 | 0 |
| 用户源码、绑定或目标能力不合法 | 1 |
| 参数、输入文件、配置或工具链环境错误 | 2 |
| 编译器内部验证失败 | 3 |
| 被取消 | 130 |

链接器缺失、SDK 未配置属于环境错误；已有工具链上的明确未定义符号等构建失败附链接诊断和原始工具状态，不将其报告为 parser 错误。被编译程序自身的返回值与上述编译器退出码不同。

## 4 诊断协议

机器输出使用 [diagnostic.schema.json](../contracts/diagnostic.schema.json)，样例见 [diagnostic.example.json](../contracts/diagnostic.example.json)。稳定字段包括 code、phase、severity、主源区间、关联位置和可选修改建议；自然语言 message 可改进，消费者不能解析 message 来判断错误类型。

SourceSpan 的字节区间对应原始文件 UTF-8，使用左闭右开 `[start_byte,end_byte)`。内部标准化后的偏移不能直接对外暴露；CRLF/BOM 映射由 SourceManager 提供。字节区间、行表、LSP UTF-16 坐标是不同表示。

JSON 顶层包含 schema_version、compiler_version、source_revision、status 和 diagnostics。文件路径在项目内优先用逻辑相对路径，便于 agent 复现；外部文件可给规范路径。字段不使用裸 TypeId 作为跨进程类型身份，期望/实际类型采用规范化可读表示。

诊断建议能关联“调用位置”和“函数声明位置”，例如未处理 throws 的消息应指明调用的错误类型以及当前函数可传播的类型。fix-it 必须带源码 revision，应用时检测版本冲突；编译器不主动修改文件。

稳定错误码以 `E_` 开头，提示以 `W_` 开头。未实现能力统一使用 `E_CAPABILITY_UNAVAILABLE`；错误节点只用于恢复，不得进入已验证 HIR。初版只需实现验收范围内的代码，新增代码更新注册表与样例。

## 5 LSP 与快速检查

LSP 使用不可变文档快照和请求 revision。新编辑到达后取消过期分析，发布诊断时核对 revision，不能把旧错误覆盖新文件。第一阶段允许整文件重新解析并缓存未变模块，不要求字符级增量 parser。

提供的首批功能：诊断、导入路径补全、名称补全、定义跳转、类型悬停。语义重命名、格式化和完整引用索引后续实施。Tree-sitter 作为可选编辑器语法工具，不成为另一套互相矛盾的语义判断来源。

## 6 缓存和确定性

缓存 key 至少包括：编译器/契约版本、源码摘要、导出接口摘要、依赖锁、绑定内容、有效目标和 profile、影响结果的编译选项。后端缓存还包含 LLVM 版本及运行库 ABI 版本。

AST/HIR 调试打印采用稳定排序和逻辑符号名，避免随机内存地址。缓存中的序号 ID 必须在反序列化时重新绑定，不把本进程指针持久化。

初期可只实现进程内模块缓存；跨进程缓存没有实现时明确报告，不伪造命中率。优化目标通过项目大小、冷/热检查时间和峰值内存测量，不预设未经测量的延迟承诺。

## 7 agent 对接

`contracts/tasks.json` 提供依赖、角色、备份和交付物；`prompts/IMPLEMENT_TASK.md` 是可替换任务 ID 的启动模板。仓库采用哪个 bridge、任务调度或 PR 系统由使用者环境决定，本包不假定不存在的服务。

agent 应将诊断 JSON、实际测试命令和版本记录作为交接输入。无需把全部 LLVM IR 放入每次对话；只附能复现问题的源码、相关 IR 和失败日志。
