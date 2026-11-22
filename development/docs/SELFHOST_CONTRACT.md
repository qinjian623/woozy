# Woozy 编译器移植接口 selfhost/1

目标是完整替换现有语言前端和两个代码生成器。新编译器的源码位于 `selfhost/src/`；旧 C++ 编译器作为 stage0 和测试对照保留。语言规则、诊断和既有能力限制不变。

Root 单独修改 `selfhost/src/wzcc/model/`、项目清单和 CLI。其他模块导入 `app/wzcc/model`，不各自重新定义 Span、Token、AstNode 或诊断。所有序号沿用现有 C++ 枚举的顺序；数组下标从 0 开始，Source id 从 1 开始。span 是原始文件字节区间；Position 为从 0 开始的 UTF-16 行列。

第一批接口：

- `wzcc/source.snapshot(file_id u64, revision_id u64, id u64, path string, bytes Vec[u8]) model.Source`
- `wzcc/source.raw_span(source model.Source, begin u64, end u64) model.Span`
- `wzcc/source.span(source model.Source, begin usize, end usize) model.Span`：输入正规化字节位置，输出原始字节位置。
- `wzcc/source.position(source model.Source, byte u64) model.Position`
- `wzcc/source.byte_at(source model.Source, position model.Position) u64`
- `wzcc/lexer.lex(source model.Source) model.LexResult`
- `wzcc/parser.parse(input model.LexResult) model.ParseResult`

Source 保留原始字节、正规化字节与字节对应表；只移除文件开头 BOM，并将 CRLF 转成 LF。UTF-8、原始 NUL 和孤立 CR 的失败位置与现有 SourceSnapshot 相同。boundaries 只保存 Unicode 字符边界及其 UTF-16 位置；不得把字节数当列号。span、position、byte_at 的无效内部调用是实现错误，可 Panic；不得把无效位置静默取整。

Token 与 trivia 的 spelling 是正规化文本对应的 UTF-8 内容，span 保持原始字节区间。字符串解码值使用 has_string_value 区分空字符串与不存在。AST 使用连续 nodes 与 children 下标；不持有递归对象引用。

LexResult/ParseResult 的 has_value 对应现有 PhaseResult.value。诊断顺序、恢复节点、token 区间、子节点顺序与现有实现一致。parser.parse 负责保留词法诊断；不在内部重跑词法器或重复追加诊断。

首批阶段只发布显式词法/语法操作；未移植的名称/类型检查及 build 必须明确报 E_CAPABILITY_UNAVAILABLE。语法成功不得被报告为完整编译成功。

LLVM/Clang/LLD 与现有运行库可以保留。新编译器不得启动 woozyc/lang_native_helper 或通过 FFI 调用旧前端。测试对照程序可使用 stage0 库，但不能链接进或被新编译器调用。

后续按模块移植清单/绑定/解析、常量、类型与泛型、所有权/HIR、Native 控制流与文本 LLVM IR、Python 发射器。完整完成条件为同份源码 stage0→1→2→3，接受/拒绝、诊断与程序执行对照通过；阶段2/3行为一致。不得以只支持表达式、忽略复杂语法、套壳旧编译器或只比较程序退出0来代替。

模块逻辑导入名以 `app/wzcc/` 开头（现有加载器以 app 表示当前 source_root）。枚举 ID 通过 model.token_kind_id/trivia_kind_id/ast_kind_id 输出，顺序与 C++ 一致。LexResult/ParseResult 同时保留 state 和 failure。前端阶段另安排取消测试，不得把 cancelled 报成成功。

## SourceManager（SH01b）

Source 包新增公开结构 `Manager { pub files Vec[string]; pub snapshots Vec[model.Source]; pub revision u64; }` 及四个函数：`manager() Manager`、`add_file(state inout Manager, path string) u64`、`update(state inout Manager, file_id u64, bytes Vec[u8]) model.Source`、`get(state Manager, snapshot_id u64) model.Source`。file/snapshot ID 从 1 开始、revision 全局递增等规则严格沿用 C++ SourceManager；update 会保留旧快照，非法 source 同样记入快照。传入空 path 或非法 ID 属于内部错误，不能静默创建新的 file。已有的 snapshot 六函数与共享 model 保持不变。

## 项目与名称解析（SH05）

记录定义在 `selfhost/src/wzcc/model/project.wz`，Root 单独维护。Python v1/C ABI v2 的全部结构、注册回调槽位和所有权字段均从现有 `project.hpp` 移植；不以无类型 JSON 代替。可选字段使用 std/option.Option，ID 0 仍是有效 Module/Symbol/ForeignBinding。C type/callable ID 从 1 开始。C++ 的 `type` 字段在 Wz 中为 `type_name` 或 `type_ref`，PackageRequirement.package 为 package_name；枚举成员用 PascalCase，序号沿用 C++。

Project.files 是全项目 ParseResult 数组；Module.files 保存其下标。NodeRef.file 是所属模块内序号，访问文件必须经过 project.files[project.modules[ref.module].files[ref.file]]。词法诊断已经包含在 ParseResult，不重复追加。

```text
manifest.load(path string, is_main bool, manager inout source.Manager) model.ManifestResult
bindings.load_python(path string, manager inout source.Manager, store inout model.BindingStore) model.BundleResult
bindings.load_c(path string, manager inout source.Manager, store inout model.BindingStore) model.BundleResult
project.load(options model.ProjectOptions, manager inout source.Manager) model.ProjectResult
resolve.resolve_project(project model.Project) model.ResolveResult
```

BindingStore.hosted 由 project 在读取 C bundle 前设置；C 注册回调在非 hosted 模式仍按现有规则拒绝。bundle 缓存和 identity 冲突由 bindings 负责，成功后才发布声明。图加载完成时将 store 中的声明数组放入 Project。

ManifestConfig 保存 canonical source_root、manifest、package_name、entry、backend、word_bits、hosted、按名字排序的 dependencies 和清单 span。对非主清单中的 backend/word_bits 维持现状，不予采用。TOML 原始整数必须递归检查当前 toml++ 的 i64 范围，含未使用字段中的整数；不能因通用 TOML 模块保留任意大文本而扩大清单接受范围。E_MANIFEST_INVALID 属于 Environment，span 为整个原始清单。TOML parser 的错误文本由项目模块按实际对照结果处理，不得省略诊断。

Module 从入口开始 DFS 首次发现分配 ID，先登记 canonical identity，再按路径排序解析目录内全部 .wz，随后按文件/AST 顺序解析 import。符号链接来源去重；相同逻辑名的不同来源报冲突；全部 import 环报错。library/dir 按首次使用顺序去重。foreign exports 按名字排序，重载候选按声明次序。

resolve 先收集全部顶层声明，再按文件解析正文，保留文件作用域的 import alias、初始化后才加入的新绑定、参数/最外层函数体共同作用域、匿名函数的独立符号及 const 环检查。const 中的函数体不计入常量求值依赖。实例成员保留到 sema，不猜方法。prelude 先取 system.builtin_types()，再加 assert/panic/share，再取 system.operations() 的名字，顺序不能改。

`app/wzcc/system` 的 `OperationSpec {operation u32,name string,result string,parameters Vec[string],owner string,type_arity u32}` 和 operations()/builtin_types() 由现有头文件数据对照；operation 是 C++ 枚举值，不是表内序号。

## 类型表（SH06b）

`model/types.wz` 定义 TypeKind/ParamMode/ParameterType/Type/Backend/CheckConfig/Substitution；TypeId 使用 usize，0 无效，其余从 1 开始。C++ ParameterType.type 和 Substitution 的 TypeId 字段写作 type_id。Type 的 throws_type 使用 Option[usize]，declaration/binding 的 0 仍沿用 C++ 的普通 ID 值，不自行加强验证。

`app/wzcc/types` 公开 Context（entries 私有）、Error.Invalid(message string) 以及：

```text
context() Context
clone_context(state Context) Context
intern(state inout Context, value model.Type) usize throws Error
find(state Context, id usize) option.Option[model.Type]
size(state Context) usize
equal(left model.Type,right model.Type) bool
bit_width(value model.Type,config model.CheckConfig) u32 throws Error
substitute(id usize,substitutions Vec[model.Substitution],state inout Context) usize throws Error
patterns_overlap(left usize,right usize,state Context) bool throws Error
```

完整沿用 TypeContext::intern 的合法形状/引用检查及确定 ID，不把类型声明存在性检查提前到表内。intern 复制 parameters/type_args；find 返回的数组也不能让调用方改写表内记录。clone_context 用于承接 C++ 里按值复制类型表做试探性检查的写法，向新表追加条目不能影响旧表。substitute 与 patterns_overlap 对照现有 hir.cpp，含 occurs check 与无递归环处理。system signature 中需要 std/Option/Result/系统记录声明的部分，等 HIR 记录发布后另做，不能用形状相似的假记录替代。

## HIR 数据定义（SH06c）

`app/wzcc/hir` 是同一个数据包，nodes/ownership/module 三个文件分别保存节点、清理与退出记录、声明与 HirModule。SH06c worker 是该批三个文件的唯一写入负责人；model、CLI 和契约仍由 Root 修改。hir 导入 model/types，types 只导入 model，不形成 import 环。所有需要完整 HirModule 的签名生成、验证与分类算法放到后续独立包。

字段来源为完整 `include/lang/hir/hir.hpp`；Source/Scope 分别改名 HirSource/HirScope，type 字段改 type_id，CaptureSource.move 改 is_move，HirFunction.async 改 is_async，pair 重命名表改 SymbolRename。NodeData/NodeKind 必须保留全部45项顺序；kind 从 data 推导。不得省略捕获、bound receiver、异步取消、ownership/exits、迭代器、dyn trait、C callback 引用或 Python access 元数据。数据定义阶段不提供可随意构造的 VerifiedHirModule 或 verified bool，发布隔离和验证留待后续阶段实现。

## HIR 共用算法

`model.SupportError.Invalid(message)` 对应现有内部 invalid_argument/LiteralError；`Limit(message)` 对应 PatternAnalysisLimit。算法包统一抛这个类型；内部 types/number 错误应转换并保留原文，不能 Panic 或伪造成功。`model.UnitResult` 保存 state/failure/diagnostics/has_value，对应 PhaseResult<monostate>。检查器和验证器仍负责将错误与源位置组合成完整诊断。

```text
literal.decode_pattern_literal(project model.Project, context types.Context,
    node model.NodeRef, expected usize, config model.CheckConfig)
    hir.PatternLiteral throws model.SupportError

foreign.binding_type(project model.Project, context inout types.Context, name string)
    usize throws model.SupportError
foreign.binding_records(project model.Project, context inout types.Context)
    Vec[hir.ForeignBinding] throws model.SupportError
foreign.bind_arguments(callable model.BindingCallable, labels Vec[Option[string]])
    Option[foreign.BoundArguments]
foreign.c_source_parameter_indices(callable model.CCallableDecl) Vec[usize]
foreign.c_type_id(project model.Project, context inout types.Context, ref model.CTypeRef)
    usize throws model.SupportError
foreign.c_callback_closure_signature(project model.Project, context inout types.Context,
    callback_type u32, userdata_index usize) usize throws model.SupportError
foreign.c_callable_signature(project model.Project, context inout types.Context,
    callable model.CCallableDecl) usize throws model.SupportError
foreign.private_interface_type(context types.Context, resolved model.ResolvedProject,
    roots Vec[usize]) Option[usize]

signature.system_operation_signature(context inout types.Context,
    module Option[hir.HirModule], operation u32, arguments Vec[usize])
    usize throws model.SupportError

valueops.infinite_value_layout(module hir.HirModule) Option[usize] throws model.SupportError
valueops.inout_data_type(module hir.HirModule, type_id usize, parameters Vec[usize]) bool
    throws model.SupportError
valueops.inout_slot(module hir.HirModule, type_id usize, parameters Vec[usize]) hir.InoutSlot
    throws model.SupportError
valueops.inout_slot_reason(slot hir.InoutSlot) string
valueops.weak_target(module hir.HirModule, type_id usize) bool
valueops.ordinary_copyable(module hir.HirModule, type_id usize) bool throws model.SupportError
valueops.owning_value(module hir.HirModule, type_id usize) bool throws model.SupportError
valueops.classify_value(module hir.HirModule, type_id usize) hir.ValueOps
    throws model.SupportError

ownership.produce_ownership(module inout hir.HirModule) model.UnitResult
ownership.verify_ownership(module hir.HirModule) model.UnitResult
```

foreign.BoundArguments 的 parameters/omitted 字段均为 Vec[u32]，顺序沿用现有头文件。`valueops` 只读输入；临时替换必须走 clone_context，返回字段的 TypeId 必须属于公开的 module.types。共享标准类型必须按真实声明身份查找，不能只按名字匹配或自造记录。

`hirclone.clone_module(module hir.HirModule) hir.HirModule throws model.SupportError` 必须深复制全部可变 Vec，包括 resolved/project、Source 字节与位置表、词法/AST arena、绑定声明、HIR、嵌套模式/常量/清理步骤以及 types.Context，不能只复制顶层 Vec 就当成完成了隔离。源 string 值可共享其不可变存储。新编译器不得调用旧前端执行复制或转储。

Sema 的具体实例变换还直接需要 `hirclone.clone_node(value hir.HirNode) hir.HirNode throws model.SupportError`、`clone_function(value hir.HirFunction) hir.HirFunction throws model.SupportError`、`clone_pattern(value hir.Pattern) hir.Pattern throws model.SupportError`、`clone_constant(value hir.ConstantValue) hir.ConstantValue throws model.SupportError`。这些入口复用 clone_module 的相同复制函数，不能再维护另一份45节点复制器。超过内部递归限制或循环时抛 SupportError.Invalid；不得用故意越界或 Panic 代替错误返回。

```text
constant.same_constant_value(left hir.ConstantValue,right hir.ConstantValue) bool
constant.transform_constant(value inout hir.ConstantValue, substitutions Vec[model.Substitution],
    context inout types.Context,
    transform func(value hir.FunctionValue) hir.FunctionValue throws model.SupportError)
    throws model.SupportError
constant.source_constant_value(module hir.HirModule, context inout types.Context,
    node model.NodeRef, expected usize,
    parse func(node model.NodeRef) usize throws model.SupportError,
    function func(node model.NodeRef,expected usize) hir.FunctionValue throws model.SupportError)
    hir.ConstantValue throws model.SupportError
constant.evaluate_hir_constant(module hir.HirModule,node usize) Option[hir.ConstantValue]
constant.aggregate_constant_expression(module hir.HirModule,node usize) bool

pattern.DomainKind/Constructor/Domain 对应 pattern_analysis.hpp 的三个记录。
pattern.coverage(scrutinee usize,
    lookup func(type_id usize) pattern.Domain throws model.SupportError,
    budget usize) pattern.Coverage
pattern.add(state inout pattern.Coverage,value hir.Pattern) bool throws model.SupportError
pattern.exhaustive(state inout pattern.Coverage) bool throws model.SupportError
pattern.pattern_constructors(module hir.HirModule,type_id usize,context inout types.Context)
    Vec[pattern.Constructor] throws model.SupportError
pattern.pattern_domain(module hir.HirModule,type_id usize,context inout types.Context)
    pattern.Domain throws model.SupportError
```

constant 保留现有 128 深度/100000 次工作限制、源 AST 重新求值与 HIR 求值各自的校验，动态函数调用不在编译期执行。失败的 evaluate_hir_constant 返回 None，不能把非法值填为零。pattern 默认调用方传 budget=100000，递归深度 256，+0/-0 重叠，NaN 不被有限浮点字面量穷尽；保留现有执行次序与错误原文。

`hirdump.dump_types(context types.Context) string` / `hirdump.dump_module(module hir.HirModule) string throws model.SupportError` 对应现有两个 C++ dump 的完整稳定文本，用于移植对照；不得只打印节点种类或依赖旧前端生成内容。

## Native CFG 数据

`app/wzcc/nativeir` 保存 `include/lang/ir/native.hpp` 的完整 Op、ExitSite、Instruction、Exit、Terminator、BlockParameter、BasicBlock、Slot、Handler、Function 记录。ID 为 usize，C type/callable 仍为 u32；type 字段为 type_id，async 为 is_async，Slot.inout 为 is_inout，system 为 Option[u32]。枚举顺序和可选的零 ID 沿用 C++。

`nativeir.Module { pub hir hir.HirModule; pub functions Vec[nativeir.Function]; }` 是可变构造数据，不表示已验证。数据阶段不发布 VerifiedModule。后续 nativeverify 必须先验证并隔离其中的完整 HIR，再验证和复制 CFG，才能创建仅该包可写的 VerifiedModule；后端只接收该对象。不得增加可任意填成 true 的 verified 标志。

## 检查与经过验证的 HIR

`sema.check_types(input model.ResolveResult, config Option[model.CheckConfig]) hir.HirResult` 对应现有 check_types：Resolve 处于失败、恢复或取消状态时不得继续进入检查器；未显式传入配置时，沿用 Project 的 backend/word_bits。HirResult 与现有普通阶段记录一样保存 state/failure/diagnostics/has_value/value。本函数只产生可变 HIR，不能直接授予代码生成权限。

`app/wzcc/hirverify` 公开 VerifyMode { Backend; StaticOnly; } 和 VerifiedHirModule（所有实例字段私有，只能由本包 verify 创建）。VerifyResult 的公开字段为 state/failure/diagnostics/value，value 是 Option[VerifiedHirModule]，不另存可被填错的 has_value。

```text
hirverify.verify_types(input hir.HirModule) model.UnitResult
hirverify.check_backend_capabilities(input hir.HirModule) model.UnitResult
hirverify.verify(input hir.HirModule, mode hirverify.VerifyMode) hirverify.VerifyResult
hirverify.snapshot(value hirverify.VerifiedHirModule) hir.HirModule throws model.SupportError
hirverify.config(value hirverify.VerifiedHirModule) model.CheckConfig
```

验证器与现有完整 Verifier 一致，逐项重读 Source、AST 和声明，不信任上游产出的常量、类型、函数实例、effect、ownership 或出口信息。verify 必须先调用 hirclone.clone_module，验证通过后再保存副本；snapshot 每次也返回独立副本，防止调用方通过共享 Vec 改写已验证数据。backend 与 static_only 模式的差异、仅遍历可执行节点的能力检查以及现有诊断顺序都必须保留。

本次 Wz 阶段函数尚未公开 C++ 的共享 stop_token。取消分支不能以普通 bool 冒充；完整 CLI 的 SIGINT 退出和临时输出处理还需在分代测试中检查。

Sema 共用的 Checker/Value/Expr/Constant/Error 和 clone_checker 只由 Root 维护，详见 SELFHOST_SEMA.md。A/B 在同一个 sema 包写不同文件，原 C++ 方法的归属、全部跨文件签名、回调状态单元及错误槽以该文档为准；不各自改名或复制状态。包内的 reject 系列辅助函数只构造 Error，调用处必须显式 throw。若嵌套实参共用同一个 inout state，必须先求值到局部变量。
