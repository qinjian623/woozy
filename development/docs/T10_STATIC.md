# T10 外部类型、读取和调用的静态检查

共同版本仍为 `compiler-dev/1`，本阶段基点为 `e962ed55c19a9ace0431ce988f940e0a7df2e48d`。本文件描述纯 C++ 前端实现。原 `04_numpy_torch.wz` 和 P01–P06 可经真实项目读取、名称解析、类型检查、HIR 验证及静态冻结检查；这不表示已经生成或运行 Python，更不表示已经训练模型。

## 调用 API

标准绑定由已有 `modules::ProjectOptions::std_binding_bundle` 提供。普通依赖仍使用清单中的 binding 文件，不能用项目 std 依赖代替工具链输入。检查器不启动 Python，也不导入 NumPy/PyTorch。

```cpp
SourceManager sources;
auto loaded = modules::load_project({manifest, {}, std_bundle}, sources);
// 每一阶段都先判断 success()；失败结果不能当作下一阶段的有效输入。
auto resolved = modules::resolve_project(
    std::make_shared<const modules::Project>(*loaded.value));
auto typed = sema::check_types(resolved);
auto verified = hir::verify(*typed.value, {}, hir::VerifyMode::static_only);
// 也可以直接调用完整静态 API：
auto checked = sema::check_project(resolved, {}, {}, hir::VerifyMode::static_only);
```

`hir::VerifyMode { backend, static_only }` 是 `hir::verify` 的第三个参数，也是两个 `sema::check_project` 重载的第四个参数，均默认为 `backend`。旧调用仍执行后端支持检查；`static_only` 会深复制 builder、ResolvedProject、Project 和 AST 后再执行 `verify_types`，通过后才发布同一个 `VerifiedHirModule`，没有新增包装类型或公开构造函数。

`static_only` 只跳过尚未实现的生成支持检查。项目 backend/word_bits 必须匹配，声明中的 `allowed_backends` 仍生效；使用外部模块的 native 源码仍报 `E_CAPABILITY_UNAVAILABLE`。外部读取要求声明中存在 `lang.py.Error`；缺少该类型的旧测试绑定继续报未支持，不会伪造 std 声明或恢复类型。

`check_backend_capabilities` 目前只对 Python/64 开放已实现的外部类型、读取和调用：可执行 callable 必须声明 `error_type=lang.py.Error` 且所有参数为 ordinary，外部值读取也要求对应声明存在。外部宿主引用可作为整体 move 参数或从已有绑定/临时值 move，也能放入 Vec、struct 字段、enum payload 和泛型字段/元素；分类与清理沿用同一 `ValueKind::foreign_reference` 和共享 ownership。声明的 `py.Error` 按同一规则存入 Vec、struct、enum 和嵌套泛型值，分类为 `ValueKind::foreign_error`，复制时共享原异常载荷。

native 及其他 profile、不带 throws 的外部 callable、外部 inout、部分 move 和其他未实现组合仍报 `E_CAPABILITY_UNAVAILABLE`；外部 callable 仍不能作为聚合成员。即使调用只有标量参数且返回 Unit，也按外部操作和声明本身检查。函数值逃逸以及外部索引、数据属性继续拒绝，但返回外部对象的函数类型签名本身不会被误拒。Python emit 仍独立调用该检查，native lower 仍独立拒绝不支持的输入；未使用的模板和类型池中的外部声明不会被当作可执行操作扫描。实际生成范围见 [T10_PYTHON.md](T10_PYTHON.md)。

## 公共表示

唯一公共头仍为 `include/lang/hir/hir.hpp`。旧编号不变，新增 `TypeKind::ForeignCallable = 15`、`NodeKind::foreign_read = 39`、`CalleeKind::foreign = 2`；ValueKind 在尾部追加 `foreign_reference/foreign_callable/foreign_error`。

`HirModule::foreign_bindings` 是 Project 声明表的带类型描述，保持相同的零起始下标。`ForeignBindingId{0}` 合法，不能据其数值判断有无绑定。

| 字段 | 含义 |
| --- | --- |
| `ForeignBinding::binding` | 当前 Project 内的声明编号 |
| `type` | 类型声明对应的 ForeignOpaque、导出值的类型，或内部 ForeignCallable |
| `signature` | callable 的普通 Function TypeId；其他声明为空 |
| `owner` | method/callable 接收者的 ForeignOpaque TypeId；模块函数和构造为空 |
| `read_error` | 声明中 `lang.py.Error` 对应的 ForeignOpaque TypeId |
| `ForeignRead::binding` | 固定的值或调用目标声明；method/callable 有一个接收者 child，模块导出没有 child |
| `Call::binding` | 外部调用选中的声明；普通调用必须为空 |
| `Call::omitted` | 没提供的可选形参下标，升序；没有表达式或占位值 |

身份、模块、宿主属性、参数名字/kind/optional/escapes、拥有和 effects 保存在同一个已冻结 Project 的 `foreign_binding(binding)` 中。带类型描述不复制另一套字符串声明。验证器从声明重新计算这些 TypeId、签名和 owner，再与 HIR 核对；不会按导入别名重新寻找宿主名字。

`ForeignOpaque` 继续使用 `Type::binding`；`py.Error` 是 runtime_error 声明对应的拥有载荷，绝不使用诊断恢复用的 `TypeKind::Error`。`ForeignCallable` 也按 binding 区分具体候选，只允许出现在紧邻外部 Call 的 ForeignRead 结果中，不能作为源语言函数值保存、返回或传递。`Call::signature` 始终是普通 Function 类型。

## 求值、错误和拥有记录

调用顺序由真实 children 表达：先计算接收者，再执行 ForeignRead，再按源码次序计算 args，最后调用。`model(x)` 选择声明中的 callable 并持有 model 对象，不改成 `.forward(x)`，也不增加宿主属性。参数按类型选择重载，零个或多个匹配都拒绝。

验证器不只检查提交 HIR 中仍然存在的 ForeignRead/Call。它从函数 AST、resolved occurrences、已核对的参数/局部类型以及声明和函数签名重建该函数必需的源码外部操作集合，再与 HIR 节点的实际 source 逐项比较。实例方法、对象 callable、模块函数/构造和独立 dtype/值读取都包括在内；缺少、替换、搬移或伪造这些操作均报 `E_IR_INVALID`。该检查只识别源码要求的读取/调用，不执行表达式，也不把验证器变成通用 AST 解释器。

`CallArg::source_index` 是源码次序，`parameter_index` 是所选签名中的位置；label 和 mode 原样保留。位置限定参数不接受标签，关键字限定参数不接受位置实参，重复、遗漏或未知标签拒绝。省略项只记录在 omitted 中，不补 None，不把关键字限定参数写成位置实参。位置实参跟在命名实参后由已有 parser 拒绝。

每个 ForeignRead 有独立的 py.Error 错误出口；Call 的独立出口来自声明的 error_type。两者都沿用原 errors、ExitPlan、try/catch 和 typed throws 检查。读取失败时不会登记尚未求值的实参或未成功的调用结果；后续实参失败只清理已完成的临时值，并结束已开始的借用。清空 errors/exits/ownership 后重新计算，也不能绕过上面的源码外部操作检查。

ForeignOpaque、ForeignCallable 和 py.Error 的 `classify_value` 都返回 `needs_drop=true`、`pod_inout=false`。接收者及内部目标沿用既有 Temporary/ValueUse/CallLifetime 保活规则。目标借用在调用返回或失败时结束，拥有临时值在 full-expression 结束时销毁。读取方法接收者时，可以取得同一对象的逻辑 copy，并保持到该表达式结束。

保存、赋值或返回普通参数时，会取得同一对象的新拥有引用；普通调用借用参数，只有显式 move 才会消费整个拥有绑定或临时值。普通外部引用和 `py.Error` 位于 Vec、struct、enum 或泛型字段中时，copy 递归共享宿主对象，drop 只释放语言槽，move 只转移同一句柄而不搬移宿主数据。ForeignCallable 仍只能出现在紧邻外部 Call 的读取结果中，不能作为聚合成员保存。返回或传播错误时先保存活动输出再执行清理；catch 载荷及其复制也带有真实拥有记录。

生产者和验证器都从结构重新推导拥有记录，验证器不读取提交的记录作为预期。绑定、owner、读取目标、签名、源码实参、形参索引、省略项、效果、错误出口或拥有记录不符均报 `E_IR_INVALID`。把错误树的拥有记录重新计算成自洽形式，也不能绕过源码身份检查。

## 支持和未实现项

本阶段支持声明中的外部类型、dtype 值、普通函数、构造、方法、对象调用、有限重载、可选参数省略、命名实参，以及普通对象持有/赋值/回传、typed py.Error 传播/catch。shape、设备可用性和动态 dtype 仍由宿主运行时决定；device 只按声明的 string 类型检查，没有 cpu 字面量限制。静态通过不代表设备或库实际存在。

外部 inout、字段/索引部分 move、外部索引/切片/数据属性、外部函数值逃逸仍报 `E_CAPABILITY_UNAVAILABLE`；向 ordinary 外部形参写 move/inout 沿用 `E_CALL_ARGUMENT`。未知成员明确拒绝，不创建 Any、Unsupported 或恢复类型代替新节点。普通参数重赋值、移动后读取/双 move、计算产生的拥有 switch 临时值和原有动态字符串等限制保持。

紧邻调用目标外的透明括号不构成方法值逃逸：`(x.backward)()`、`(np.linspace)(...)`、`(m)(x)` 和 `get(x).backward()` 都按实际紧邻 Call 处理，并保留真实 Call/Member/自变量顺序。真正保存、返回或传递括号方法值，例如 `let v=(x.backward);`，仍报 `E_CAPABILITY_UNAVAILABLE`。

Python/64 的有限外部生成和 runtime 异常桥接已实现，范围见 [T10_PYTHON.md](T10_PYTHON.md)；原 04 的 CPU 生成执行已在独立检查中用 API 和专用解释器实测。正式 `woozyc check/build --project` 现已提供可选 `--std-binding BUNDLE`，将工具链标准声明交给同一个 `ProjectOptions::std_binding_bundle`；静态检查本身仍不启动 Python、不 import NumPy/PyTorch。该参数不用于 `--syntax-only`，不允许项目 `std` 依赖遮蔽，也没有默认路径或环境变量。已验证的 CPU 声明集、正式依赖锁及主目录整合结果见 [T10_CPU_VERIFICATION.md](T10_CPU_VERIFICATION.md)。schema、原 fixtures 和绑定字节保持不变，绑定仍为 draft；GPU 和任意 Python API 未验证。

## 测试和接入

新增测试入口为 `tests/bindings/static_test.cpp`，总 CMake 已注册 `sema.t10`。测试直接调用真实的 load/resolve/check_types/verify_types/verify/check_project API，额外的绑定变体只写入测试输出目录。函数值调用的返回类型取自已验证的 Function 签名，支持通过函数值参数取得 Tensor 或 Linear 后立即执行方法或对象调用，不依赖可被篡改的调用结果节点。

测试覆盖原 04、P01–P06、原负例、零号绑定、重载、省略、乱序命名实参、读取与调用各自错误出口、失败前缀、对象和错误载荷的 copy/返回/丢弃、冻结别名，以及逐类合法对照和坏 HIR。同时覆盖带括号的方法/函数/callable、计算所得的方法接收者、未知方法、方法值逃逸负例，以及删除实例方法、对象 callable 或 dtype 值读取后清空 errors/exits 并重算 ownership 的三类坏 HIR；每类都配有合法对照，删除操作后 `verify_types` 和静态 `verify` 均报 `E_IR_INVALID`。测试还验证了未声明 `lang.py.Error` 的静态包装不能通过 Python emit；启用 LLVM 的构建另验证 native lower 会拒绝 Python 静态包装。所有失败尝试均保留，实际构建与测试状态以外层 `checks/t10-static-implementation/correction/` 中的报告、完整命令、退出码及源码 SHA-256 为准，不引用声明阶段或初版中断任务的历史通过数量。
