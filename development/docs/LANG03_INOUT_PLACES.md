# LANG03 字段与索引 inout 位置

日期：2026-11-12。契约：`compiler-dev/1`。目标平台：Linux x86_64。本文件记录 richer mutable places 的实际范围；较早阶段文档中的“字段/索引 inout 未支持”是该批次的当时状态。

## 已支持

- `inout` 实参可以是整体可写绑定、已有 `inout` 参数、字段链 `obj.nested.field`、容器索引 `items[i]`，以及组合 `items[i].field`、`matrix[i][j]`。
- 位置类型由 `hir::inout_slot_type` 决定：数值、bool、递归只含平凡值的有限 struct/enum，以及纯 Woozy 拥有值（Vec、Slice、string、函数值、Weak、dyn trait object、线程/同步运行时句柄、含这些字段的 struct/enum）。旧 `hir::inout_data_type`/`ValueOps.pod_inout` 保留为“平凡 POD”分类元数据，不再作为 inout 的类型支持规则。
- 拥有值使用已有 copy/drop，struct/enum 按成员递归处理，所以允许整体、字段和元素替换。被调用方通过调用者真实存储写入：先发布完整新值，再在修改区间外释放旧值，旧值恰好 drop 一次；借用的存储本身从不 destroy。若调用者还持有同一对象的普通副本，ARC 按普通复制规则让它继续存活（共享身份不变）。
- `Task` 句柄的完成生命周期归执行器所有，外部 C resource 没有可核对的 retain/release 配对，两者报 `E_CAPABILITY_UNAVAILABLE`；`Unit` 不是可写值位置。
- 字段位置的基础绑定必须可写；索引接收者可以是 `let` Vec/Slice，因为元素写入修改共享容器。位置必须能追溯到本地或参数存储。
- Native 将字段位置降低为 `field_address`/GEP，将 Vec/Slice 元素位置降低为带边界检查的 `vec_element_address`，然后把元素地址或元素字段地址作为指向被调用方存储的指针。O0 与 O2 使用同一 ABI。
- Python 使用现有 `_rt.Field`/`_rt.Index` 位置对象传递 Cell、字段路径和元素索引；被调用方读写同一真实存储，索引写回仍调用 Vec/Slice 的 `set`。
- 同一调用中，不同具名字段可同时借用；同一索引字面量下的不同字段也可同时借用；不同字面量索引可证明不重叠时可同时借用。比较容器内位置时只使用索引序列和最后一个索引之后的字段后缀；索引之前的字段只表示“从哪个句柄进入”，不能证明 Vec 对象身份。相同/前缀路径、整容器访问、动态索引和容器移动继续拒绝。
- Vec/Slice 句柄复制到其他绑定、struct 字段、enum 载荷、函数返回值或闭包环境后，不同 SymbolId 不能证明对象不同。元素地址 `inout` 借用期间，其他可能携带共享容器的实参、其索引表达式中的别名、以及环境未知的间接 callee 均在编译期保守拒绝。未给任何未知别名添加 `noalias`。
- 调用成功与 typed error 出口都按既有 full-expression/call 计划结束借用。借用位置本身不注册 destroy；拥有接收者临时值在借用结束后才释放，避免双重释放。

## 仍拒绝

- `Task` 与外部 C resource/foreign 值的 `inout`；`Unit` 位置。
- 临时容器、非可写字段基础、无法追溯到绑定存储的位置。
- 部分字段/元素 `move`，从 `inout` 参数移出，以及借用期间再次读取/移动/替换同一存储或整个容器。
- 动态索引无法静态证明不重叠，因此同一容器的两个动态索引 `inout` 保守拒绝。
- 元素地址借用期间，任何可能持有同一 Vec/Slice 的其他实参、别名或未知间接 callee；不会为这些未知别名生成 `noalias`。

## 验证

`tests/inout_places/cases.json` 与 `tests/inout_places/run_inout_places.py` 覆盖：

- 深层嵌套 struct 字段写回；Vec 标量与 struct 元素写回；循环内字段/索引写回；非重叠字段和不同字面量索引同时借用。
- typed throw 后的字段/索引写回、借用结束和容器继续可用。
- 重叠字段、整容器别名、借用期间整体 move、从 `inout` 参数移出。
- Root 独立反例：先用 `let alias = items` 建立别名，再把 alias 交给会触发 reserve 的 callee；以及把会触发 reserve 的 `prepare(items)` 藏在其他实参的索引表达式里。
- 永久回归：藏在 struct/enum 中的别名、返回同一 Vec 的函数、闭包捕获的别名、未知间接 callee，以及不同绑定使用同一索引时按共享 Vec 处理的场景。

运行器对每个正例执行 Native O0/O2 和 Python 后端，检查 Native 程序为无 `INTERP`/`NEEDED` 的 ELF64 x86_64，并核对 Python 退出码；拒绝项同时检查两个后端的诊断和失败构建不留下产物。CTest 名称：`native.inout_places`。

## 拥有值 inout（本批）

`tests/owning_inout/cases.json` 与 `tests/owning_inout/run_owning_inout.py` 覆盖 14 个正例与 7 个拒绝项，CTest 名称 `native.owning_inout`：

- Vec 整体/字段/元素替换，字段与元素中的 disjoint 拥有字段，嵌套 `Vec[Wrap]` 元素字段。
- string 整体与字段替换（含嵌入 NUL 和存活副本）。
- 函数值替换：旧环境恰好释放一次，保存的副本仍可调用。
- Weak 整体与字段替换，切换所观察的对象。
- 拥有 struct/enum 整体替换、嵌套 struct 字段、enum owning 载荷。
- `std/box.Box` ARC 共享句柄替换：新身份保留，旧身份恰好释放一次。
- dyn trait object 替换：旧箱内具体值（及其 Vec）恰好释放一次，新对象生效。
- Slice 整体替换：旧描述符释放，新视图可读。
- 显式 `(inout it).` 方法改写自身 Vec 字段与 POD 字段（用户 Iterator 目标场景）。
- 正常与 typed throw 出口：写回可见、defer 执行、每个被替换的旧值恰好释放一次。
- `inout` 参数转交、自由泛型与受约束泛型的拥有值 swap。
- 拒绝项：从 `inout` 参数 `move` 出、闭包捕获、同存储重叠、字段与整体重叠、元素地址借用与共享别名并存、`Task`、`Unit`。

实现要点：

- sema/HIR：`owning_inout_type`（sema）与 `hir::inout_slot_type` 取代原 POD 类型限制；调用实参、函数类型、trait 应用与 constraint/trait-static 路径共用同一规则，并按同一 TypeId 图递归检查成员。
- Native IR：整体 `inout` 参数赋值的 `Place` 走 `replace_indirect`（借用槽不 emit destroy、不把值写进指针变量），字段路径继续走 `field_address` + `replace_indirect`，Vec 元素走 `vec_set`（运行时释放旧元素）。
- Python：新增 `_rt.replace_place`，按真实位置对象（Owner/Cell/Field/Index）发布新值后释放旧值；只有 Field 有 `root`，所以仅对 Field 判断“根为 Index”并交给 `Vec.set` 释放旧元素，避免重复 drop；直接 Cell 与 Field-over-Owner/Cell 显式 drop，Owner 的 `_DEAD` 行为不变。运行库契约版本升为 `compiler-dev/1-python-values/10`，六种 place 形状由 `tests/owning_inout/python_places_test.py` 永久断言。
- 借用/别名规则不变：元素地址借用期间，其他可能持有同一共享容器的实参或未知间接 callee 仍保守拒绝；未给未知别名添加 `noalias`。

## Root 整合检查

独立 21 例在三种配置下及额外 15 次 Source 检查均通过。Cell helper 漏 drop 的问题已修复；Root 将 FieldOwner 单测改为使用真实 struct ops 并释放整根 Owner，7 种位置检查全部通过。主目录 152 项首跑通过 142 项，其余 10 个共享字符串测试仍沿用旧的 inout 拒绝预期；Root 补充了三类成功执行用例并保留 Task 拒绝检查后，这 10 项复跑全部通过。Task、外部资源的 inout 以及用户 Iterator 仍未因此完成。
