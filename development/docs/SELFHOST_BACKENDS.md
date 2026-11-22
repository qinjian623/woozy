# 自举后端接口

本接口覆盖现有完整的 Native 和 Python 后端。新编译器不得调用旧 C++ frontend、lower、verify、emitter 或 helper；LLVM、Clang、LLD 和既有运行库继续沿用。测试对照程序允许链接旧实现。

`app/wzcc/nativeir` 是已接受的可变 CFG 记录包。Module.hir 保存原始 HirModule，直接构造该记录不会获得已验证状态。TypeId 为 usize；可选值仍用 Option，0 的含义沿用原记录。

```text
nativelayout.AggregateTypes { module hir.HirModule, context types.Context }
nativelayout.aggregate_types(module hir.HirModule) AggregateTypes
nativelayout.declaration(state AggregateTypes,id usize) hir.NominalDecl throws model.SupportError
nativelayout.fields(state inout AggregateTypes,id usize,variant Option[usize]) Vec[hir.FieldDecl] throws model.SupportError
nativelayout.field_index(state inout AggregateTypes,id usize,field usize,variant Option[usize]) usize throws model.SupportError
nativelayout.variant_index(state AggregateTypes,id usize,variant usize) usize throws model.SupportError

nativeverify.VerifiedModule // 所有字段私有；只有 verify 能构造
nativeverify.VerifyResult { state, failure, diagnostics, value Option[VerifiedModule] }
nativeverify.verify(input nativeir.Module) VerifyResult
nativeverify.snapshot(input VerifiedModule) nativeir.Module throws model.SupportError

nativelower.lower(input hirverify.VerifiedHirModule) nativeverify.VerifyResult
nativeirdump.dump(input nativeir.Module) string throws model.SupportError
```

layout 直接移植自 `src/ir/layout.hpp`，其中的 context 是独立的类型表。Native verify 会先深复制全部 CFG 和 HIR，再重新验证 HIR 以及完整的 CFG 和所有权规则，通过后保存副本；snapshot 每次同样返回新副本，不共享可写 Vec。lower 负责检查 Native/64、执行全部转换并调用 verify，诊断顺序和失败分类沿用 C++；lower 不直接创建 VerifiedModule，也不跳过验证。

layout 由 Native verifier worker 单独编写，lower 与后续 emitter 共同使用，其他 worker 不复制或改动它。lower/dump 由 lower worker 编写。各包不得导入 sema，不能循环依赖。

```text
pythonemit.Options { entrypoint bool }
pythonemit.LineSource { line u64, source model.Span }
pythonemit.SymbolName { symbol usize, name string }
pythonemit.Output { code string, runtime string, source_map Vec[LineSource], symbols Vec[SymbolName] }
pythonemit.EmitResult { state, failure, diagnostics, value Option[Output] }
pythonemit.emit(input hirverify.VerifiedHirModule,entry usize,options Options,runtime string) EmitResult
```

Python emitter 只做纯代码生成。CLI 读取已有 `runtime/python/unicode18.py` 和 `langrt.py`，中间拼一个换行后作为 runtime 参数传入；Output.runtime 原样返回它，与 CMake 嵌入旧 emitter 的内容一致。旧运行库文件可以继续分发，不把整份 Unicode 表重复写入 Woozy 字符串。emitter 不读文件、不运行 Python、不启动子进程。目标必须是 Python/64，能力限制与现有实现相同；并不为 Python 新增 async。

Native 文本 emitter 的状态和跨文件函数签名另由主实现者固定。它直接输出 LLVM 文本，不增加另一套通用 IR；所有语言相关的 copy/drop、thunk、vtable、GC、async 和 C adapter 生成逻辑都需迁移，不能只覆盖编译器目前使用的节点。

```text
nativeemit.Options { triple string, cpu string, features string, optimization u32 }
nativeemit.Output { llvm_sections Vec[string], c_adapter_source string, triple string, data_layout string, has_async bool }
nativeemit.EmitResult { state, failure, diagnostics, value Option[Output] }
nativeemit.emit(input nativeverify.VerifiedModule,entry usize,options Options) EmitResult
```

上述入口只做纯文本生成，外部工具调用由 CLI 负责。普通 O0 调用 opt 验证；async O0 额外需要 default<O0> 拆分协程；O2 使用 default<O2>，优化前后都要验证。llc 生成目标文件后再执行相应的链接步骤，全部成功后才发布输出文件。共享记录和各组函数的精确声明见 SELFHOST_NATIVEEMIT.md。
