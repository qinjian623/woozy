# LANG03 字段与索引 inout 位置

日期：2026-11-12。契约：`compiler-dev/1`。目标平台：Linux x86_64。本文件记录 richer mutable places 的实际范围；较早阶段文档中的“字段/索引 inout 未支持”是该批次的当时状态。

## 已支持

- `inout` 实参可以是整体可写绑定、已有 `inout` 参数、字段链 `obj.nested.field`、容器索引 `items[i]`，以及组合 `items[i].field`、`matrix[i][j]`。
- 位置类型继续使用 `inout_data_type`：数值、bool，或递归只含平凡值的有限 struct/enum。Vec/Slice 容器本身、string、外部对象、函数值和含拥有字段的聚合仍拒绝。
- 字段位置的基础绑定必须可写；索引接收者可以是 `let` Vec/Slice，因为元素写入修改共享容器。位置必须能追溯到本地或参数存储。
- Native 将字段位置降低为 `field_address`/GEP，将 Vec/Slice 元素位置降低为带边界检查的 `vec_element_address`，然后把元素地址或元素字段地址作为指向被调用方存储的指针。O0 与 O2 使用同一 ABI。
- Python 使用现有 `_rt.Field`/`_rt.Index` 位置对象传递 Cell、字段路径和元素索引；被调用方读写同一真实存储，索引写回仍调用 Vec/Slice 的 `set`。
- 同一调用中，不同具名字段可同时借用；同一索引字面量下的不同字段也可同时借用；不同字面量索引可证明不重叠时可同时借用。比较容器内位置时只使用索引序列和最后一个索引之后的字段后缀；索引之前的字段只表示“从哪个句柄进入”，不能证明 Vec 对象身份。相同/前缀路径、整容器访问、动态索引和容器移动继续拒绝。
- Vec/Slice 句柄复制到其他绑定、struct 字段、enum 载荷、函数返回值或闭包环境后，不同 SymbolId 不能证明对象不同。元素地址 `inout` 借用期间，其他可能携带共享容器的实参、其索引表达式中的别名、以及环境未知的间接 callee 均在编译期保守拒绝。未给任何未知别名添加 `noalias`。
- 调用成功与 typed error 出口都按既有 full-expression/call 计划结束借用。借用位置本身不注册 destroy；拥有接收者临时值在借用结束后才释放，避免双重释放。

## 仍拒绝

- owning 非 POD `inout` 尚未实现：Vec/Slice 整体、string、函数值、外部对象、含拥有字段的聚合。
- 临时容器、非可写字段基础、无法追溯到绑定存储的位置。
- 部分字段/元素 `move`，从 `inout` 参数移出，以及借用期间再次读取/移动/替换同一存储或整个容器。
- 动态索引无法静态证明不重叠，因此同一容器的两个动态索引 `inout` 保守拒绝。
- 元素地址借用期间，任何可能持有同一 Vec/Slice 的其他实参、别名或未知间接 callee；不会为这些未知别名生成 `noalias`。

## 验证

`tests/inout_places/cases.json` 与 `tests/inout_places/run_inout_places.py` 覆盖：

- 深层嵌套 struct 字段写回；Vec 标量与 struct 元素写回；循环内字段/索引写回；非重叠字段和不同字面量索引同时借用。
- typed throw 后的字段/索引写回、借用结束和容器继续可用。
- 重叠字段、整容器别名、借用期间整体 move、从 `inout` 参数移出。
- Root 独立反例：`let alias = items` 后把 alias 交给会 reserve 的 callee；隐藏在其他实参索引表达式中的 `prepare(items)` reserve。
- 永久回归：别名藏 struct/enum、返回同一 Vec 的函数、闭包捕获别名、未知间接 callee、不同绑定同一索引的共享 Vec 假设。

运行器对每个正例执行 Native O0/O2 和 Python 后端，检查 Native 程序为无 `INTERP`/`NEEDED` 的 ELF64 x86_64，并核对 Python 退出码；拒绝项同时检查两个后端的诊断和失败构建不留下产物。CTest 名称：`native.inout_places`。
