# T10 外部类型、读取和调用的静态检查

共同版本仍为 `compiler-dev/1`，本阶段基点为 `e962ed55c19a9ace0431ce988f940e0a7df2e48d`。本文件描述纯 C++ 前端实现。原 `04_numpy_torch.lang` 和 P01–P06 可经真实项目读取、名称解析、类型检查、HIR 验证及静态冻结检查；这不表示已经生成或运行 Python，更不表示已经训练模型。

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

`hir::VerifyMode { backend, static_only }` 是 `hir::verify` 的第三个参数和两个 `sema::check_project` 重载的第四个参数，均默认为 `backend`。旧调用仍执行后端支持检查；`static_only` 深复制 builder、ResolvedProject、Project、AST 后执行 `verify_types`，通过后才发布同一个 `VerifiedHirModule`。没有新增包装或公开构造者。

`static_only` 只跳过尚未实现的生成支持检查。项目 backend/word_bits 必须匹配，声明 `allowed_backends` 仍有效；使用外部模块的 native 源报 `E_CAPABILITY_UNAVAILABLE`。外部读取需要声明中的 `lang.py.Error`；没有该类型的旧测试绑定继续明确报未支持，不伪造 std 声明或恢复类型。

`check_backend_capabilities` 现在只对 Python/64 开放已实现的外部类型、读取和调用：可执行 callable 必须声明 `error_type=lang.py.Error`，所有参数为 ordinary；外部值读取也要求声明存在。外部宿主引用可作为整体 move 参数或从已有绑定/临时值 move，也可放入 Vec、struct 字段、enum payload 和泛型字段/元素；分类与清理继续使用同一 `ValueKind::foreign_reference` 和共享 ownership。声明的 `py.Error` 使用同一规则存入 Vec、struct、enum 和嵌套泛型值，分类为 `ValueKind::foreign_error`，复制共享原异常载荷。native/其他 profile、无 throws 的外部 callable、外部 inout、部分 move 及其他未实现组合仍报 `E_CAPABILITY_UNAVAILABLE`；外部 callable 仍不能成为聚合成员。即便调用只有标量参数、返回 Unit，也按操作和声明本身检查。函数值逃逸、外部索引/数据属性仍拒绝；返回外部对象的函数类型签名本身不被误拒。Python emit 仍独立调用该检查，native lower 仍独立拒绝不支持的输入；未使用模板和类型池中的外部声明不会被当作可执行操作扫描。实际生成范围见 [T10_PYTHON.md](T10_PYTHON.md)。

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

ForeignOpaque、ForeignCallable、py.Error 的 `classify_value` 都返回 `needs_drop=true`、`pod_inout=false`。接收者及内部目标使用既有 Temporary/ValueUse/CallLifetime 保活，目标借用在调用返回或失败时结束，拥有临时值在 full-expression 结束时销毁。方法接收者读取可取得同一对象的逻辑 copy，并保持到该表达式结束。普通参数保存、赋值、返回取得同一对象的新拥有引用；普通调用借用参数，显式 move 才消费整个拥有绑定或临时值。普通外部引用和 `py.Error` 位于 Vec/struct/enum/泛型字段时，copy 递归共享宿主对象，drop 只释放语言槽；move 转移同一句柄，不搬移宿主数据。ForeignCallable 仍只能出现在紧邻外部 Call 的读取结果中，不能作为聚合成员保存。返回或传播错误先保存活动输出，再执行清理。catch 载荷及其复制也有真实拥有记录。

生产者和验证器都从结构重新推导拥有记录，验证器不读取提交的记录作为预期。绑定、owner、读取目标、签名、源码实参、形参索引、省略项、效果、错误出口或拥有记录不符均报 `E_IR_INVALID`。把错误树的拥有记录重新计算成自洽形式，也不能绕过源码身份检查。

## 支持和未实现项

本阶段支持声明中的外部类型、dtype 值、普通函数、构造、方法、对象调用、有限重载、可选参数省略、命名实参，以及普通对象持有/赋值/回传、typed py.Error 传播/catch。shape、设备可用性和动态 dtype 仍由宿主运行时决定；device 只按声明的 string 类型检查，没有 cpu 字面量限制。静态通过不代表设备或库实际存在。

外部 inout、字段/索引部分 move、外部索引/切片/数据属性、外部函数值逃逸仍报 `E_CAPABILITY_UNAVAILABLE`；向 ordinary 外部形参写 move/inout 沿用 `E_CALL_ARGUMENT`。未知成员明确拒绝，不创建 Any、Unsupported 或恢复类型代替新节点。普通参数重赋值、移动后读取/双 move、计算产生的拥有 switch 临时值和原有动态字符串等限制保持。

紧邻调用目标外的透明括号不构成方法值逃逸：`(x.backward)()`、`(np.linspace)(...)`、`(m)(x)` 和 `get(x).backward()` 都按实际紧邻 Call 处理，并保留真实 Call/Member/自变量顺序。真正保存、返回或传递括号方法值，例如 `let v=(x.backward);`，仍报 `E_CAPABILITY_UNAVAILABLE`。

Python/64 的有限外部生成和 runtime 异常桥接已实现，范围见 [T10_PYTHON.md](T10_PYTHON.md)；原 04 的 CPU 生成执行已在独立检查中用 API 和专用解释器实测。正式 `langc check/build --project` 现已提供可选 `--std-binding BUNDLE`，将工具链标准声明交给同一个 `ProjectOptions::std_binding_bundle`；静态检查本身仍不启动 Python、不 import NumPy/PyTorch。该参数不用于 `--syntax-only`，不允许项目 `std` 依赖遮蔽，也没有默认路径或环境变量。已验证的 CPU 声明集、正式依赖锁及主目录整合结果见 [T10_CPU_VERIFICATION.md](T10_CPU_VERIFICATION.md)。schema、原 fixtures 和绑定字节保持不变，绑定仍为 draft；GPU 和任意 Python API 未验证。

## 测试和接入

新增入口是 `tests/bindings/static_test.cpp`，总 CMake 已注册 `sema.t10`。测试使用真实 load/resolve/check_types/verify_types/verify/check_project API；额外绑定变体只写入测试输出目录。函数值调用的返回类型从已验证的 Function 签名取得，支持函数参数取得 Tensor 或 Linear 后的立即方法/对象调用，不依赖可被篡改的调用结果节点。

测试包含原 04、P01–P06、原负例、零号绑定、重载、省略、乱序命名实参、读取与调用各自错误出口、失败前缀、对象和错误载荷 copy/返回/丢弃、冻结别名，以及逐类合法对照和坏 HIR。另有括号方法/函数/ callable、计算方法接收者、未知方法、方法值逃逸负例，以及实例方法/对象 callable/dtype 值读取三类外部操作被删除后清空 errors/exits 并重算 ownership 的坏 HIR；每类都有合法对照，删除后 `verify_types` 和静态 `verify` 都报 `E_IR_INVALID`。还实际检查没有声明 `lang.py.Error` 的静态包装不能通过 Python emit；LLVM 构建另检查 native lower 拒绝 Python 静态包装。所有失败尝试保留，实际构建与测试状态以外层 `checks/t10-static-implementation/correction/` 中报告、完整命令、退出码及源码 SHA-256 为准，不引用声明阶段或初版中断任务的历史通过数量。
