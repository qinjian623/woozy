# T10 声明读取接口

本批实现位于 `include/lang/modules/{project,resolve}.hpp` 和 `src/modules/{project,resolve}.cpp`，负责读取绑定声明和解析导出名称。

`load_project(const ProjectOptions &, SourceManager &, std::stop_token)` 返回 `PhaseResult<Project>`；调用者必须先检查结果状态。
普通 Python 依赖从项目清单取得绑定文件；标准绑定由 `ProjectOptions.std_binding_bundle` 指定，不能同时指定 `std_source_root`，项目也不能声明同名 std 依赖。

`Project.bindings` 保存每个 bundle 的原始 JSON、绝对文件路径、SHA-256、模块声明及验证要求。
`Project.foreign_bindings` 是所有 bundle 共用的声明表；每项为 `ForeignDeclaration`，其 `detail` 是 `BindingType`、`BindingValue` 或 `BindingCallable`。
类型引用、owner、返回类型、错误类型和参数限制仍保存为声明字符串，尚未转换为 `TypeId` 或 HIR 签名；拥有和效果字段也不表示相应执行行为已经实现。
类型引用目前只检查基础类型及当前 bundle 的类型声明；不同 bundle 的重复显式 id 会拒绝，不执行重复声明合并。

`ForeignBindingId` 是当前 Project 声明表从 0 开始的下标，跨 bundle 连续分配；0 是合法首项，不能用非零判断是否有绑定。
读取声明用 `Project::foreign_binding(id)`；这类编号仅在对应 Project 内有效，不能直接写入缓存或跨项目传递。
声明的 `identity` 保留原 id；values 没有原 id，使用 module/name。`stable_identity` 另包含契约、原 JSON 摘要、模块、类别及该身份，不能与项目内编号混用。

`resolve_project(std::shared_ptr<const Project>, std::stop_token)` 返回 `PhaseResult<ResolvedProject>`。
`import "numpy" as np` 的 np 只是当前源文件的模块别名；`np.linspace` 依次解析到目标模块和导出 SymbolId，不改变绑定身份。
外部符号的 `Symbol.foreign` 指向 `ForeignExportRef{module,index}`；`Symbol.declaration` 保存绑定源位置，`definition` 不指向 `.wz` 声明。
下一阶段应从已解析的 SymbolId 调用 `ResolvedProject::foreign_export(symbol)`；非外部符号返回 nullptr。
再按导出的 `declaration` 或 `candidates` 取 `ForeignDeclaration`，不要通过别名或显示名称重新猜绑定。

导出与候选的归属如下：

- 类型和值由 `ForeignExport.declaration` 指向具体声明。
- 同名普通函数只有一个导出符号，`candidates` 保存各个重载；此阶段尚未根据调用实参选择重载。
- constructor 附着同模块、同名类型的 `candidates`，不再建立普通函数符号；类型导出的 `declaration` 仍指向类型本身。
- method/callable 进入 owner 的 `BindingType.members`，不加入模块普通名称表；实例成员解析留给后续类型检查。

`Module.binding` 引用 bundle 内的模块描述，`Project::binding_module(module)` 可读取它；源模块返回 nullptr。
声明保留 `SourceSpan` 和 JSON pointer，模块、依赖项及参数也保存来源；格式化这些位置的诊断时需保留对应的 SourceManager。

读取器按现有 `python-bindings.schema.json` 严格检查必填字段、未知字段、字段类型和枚举，再检查类型/owner 引用、构造归属、参数顺序及拥有/效果组合。
JSON 键先按完整解码字符串检查 NUL 和重复项，拒绝转义后重复的键；NUL 键错误精确指向原始编码键及引号。
合法 Unicode 转义键仍可使用。允许自由文本的字符串值保留 NUL 及后缀；名字、id 等字段仍受各自规则限制。
坏绑定使用 `E_BINDING_INVALID`；文件中的原始 NUL 字节会更早由源码读取阶段拒绝。

以上读取和名称解析不启动 Python，不执行 import，也不读取真实 NumPy/PyTorch 属性。
`review_status=verified_against_lock` 只是输入中的声明，不是读取器核实依赖后产生的证明。
声明读取本身不代表源码能力；实际支持范围见 [T10_STATIC.md](T10_STATIC.md) 和 [T10_PYTHON.md](T10_PYTHON.md)。当前 Python/64 已实现声明的外部类型、读取、调用、普通 copy、整体 move 及宿主引用进入 Vec/struct/enum/泛型字段；外部 inout、部分 move、宿主索引和函数值逃逸仍拒绝，native/32 位仍拒绝。
CPU conformance 只检查宿主库是否符合声明；它不能替代编译器静态检查、原 04 的真实生成执行或训练验证。

后续类型检查及生成计划见 [T10_BINDING_PROPOSAL.md](T10_BINDING_PROPOSAL.md)。其中提出的 HIR 扩展尚未实现。
