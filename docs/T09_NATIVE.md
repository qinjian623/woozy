# T09 Native 实现与验证记录

本文保留独立交付时的检查记录；正式启用、主代理修正和最终共同检查见 [T09_EXECUTION.md](T09_EXECUTION.md)。

共同版本为 `compiler-dev/1`，实现基点为 `ff3ace254080036373cd08b1f785163a8804a80c`。本次只修改 native 所属文件。T09 整体仍为 `in_progress`，正式公共支持检查仍拒绝 Vec/move；root 审查两后端后再决定开放。本文记录已运行的实现，不替代独立审查。

## 接口与实现

继续使用 `VerifiedHirModule → ir::lower → ir::verify → native::emit`，没有绕过 HIR 验证或 freeze。NativeIR 原有操作编号 0..20、Exit 编号和标量调用约定不变；拥有操作和 Vec 操作追加在枚举尾部。`Slot` 增加拥有者身份、绑定身份和 move 参数标志，LLVM 为拥有槽分配 live 标志。值复制、移动、借用结束、条件销毁及迭代登记均使用公共拥有记录，退出清理按 `ExitPlan.actions` 执行。返回或错误值先保存在 SSA 值中，再执行清理。

`src/ir/ownership_verify.inc` 独立检查实际存储、copy/move/take、借用及迭代状态、live 标志和清理顺序；删除实际操作、保留或重造元数据的测试会收到 `E_IR_INVALID`。原 T08 从不可变 HIR 重算调用和出口的检查仍保留。该验证器不宣称证明任意优化后的程序与源码完全等价。

旧 `lang-native-scalars/1` ABI 保留，新增私有拥有接口名称为 `lang-native-values/1`。`runtime/native/vec.hpp` 定义非原子计数的 `LangVec`，保存长度、已初始化前缀、元素步长、活动迭代数及类型专用 copy/drop 回调。回调接收存储地址；不使用猜测的 C++ 聚合调用约定。LLVM 对 struct 按逆字段顺序 drop，对 enum 只处理活动载荷；Vec 最后释放时逆索引 drop 元素并释放存储。零大小元素使用一个字节的物理步长。

Vec 复制共享容器；切片建立新容器并逻辑复制元素。索引先按原整数的数学值检查，再转为 64 位索引。索引赋值保存接收者和索引，RHS 成功后替换；不把元素裸地址保留到 RHS 调用之后。for-in 保存一次求值的集合与入口长度，每轮复制当前元素，退出时结束登记并释放隐藏集合。

分配失败、分配大小溢出、计数溢出、非法索引/切片和活动迭代中的长度修改检查均产生终止性 Panic，与 typed error 分开。没有新增公开长度修改 API；`lang_vec_check_resize_v1` 仅供私有 runtime 检查。Panic 不保证执行语言清理。

## 已运行范围与保留的拒绝

临时开放 native/64 支持时，已运行共享写入、有限嵌套 Vec、含 Vec 的 struct/enum、整个绑定或临时值 move、移动后重建、普通参数借用/保存/返回、具体泛型实例、部分构造失败、较晚实参失败、成功/错误两输出、字段替换、别名自赋值、defer/模式绑定，以及 for-in 的 continue/break/return/throw。目标为 Linux x86_64、generic CPU、O0/O2。

仍拒绝普通参数重赋值、计算产生的拥有 switch 临时值、动态 string/Vec[string]、部分 move、非平凡 inout、索引 inout、索引字段写回、递归拥有图、捕获闭包、foreign 及原 T07 未支持组合。没有新增 len/push/share 等源语言接口。

## 实际验证与限制

- GNU Debug/Release 的临时支持结果各为 80/80：64 次源码构建并运行及释放观察、14 次静态拒绝、2 次无限循环清理的仅构建检查；每套另执行 6 个私有 runtime 项目。每套有 570 次损坏 NativeIR 拒绝记录。原 18 份 ownership 源码及全部原预期未改。
- 观察程序记录对象身份、分配、最后释放、元素顺序和剩余存活对象；普通及 typed-error 结束检查零存活，Panic 不作此断言。另检查 struct/enum 字段释放顺序和用户事件。观察代码直接检查生成的 LLVM 函数类型。
- 已保存的沙箱外 ASan/UBSan 为 66/66：60 个生成程序及 6 个私有 runtime 检查。沙箱内 LeakSanitizer 因 ptrace 限制失败的记录保留。最后新增 `owned-two-channels`、`assign-move-error` 的 4 次 O0/O2 sanitizer 执行被中断，记为 `not_run`；其普通源码执行已包含于 80/80。
- 临时支持下，两套完整 CTest 均为 34 passed / 3 failed，失败均为旧阶段要求 Vec/move 继续被拒绝的断言；断言及其后未运行内容保存在交付说明中。还原临时补丁后，两套重建成功，完整 CTest 各为 36 passed / 0 failed / 1 skipped。`native.ownership` 因公共支持未开放而跳过，记为 `not_run`；私有 runtime 测试通过。
- LLVM/Clang/LLD 为 22.1.8；测试 Python 固定为工具目录内的 3.12.15。公共 Clang sanitizer、LLVMoff 和跨后端比较由 root 运行，本次 `not_run`。

完整命令、退出码、旧失败日志、逐项观察及 hash 位于 `/tmp/language-t09-native-20261104/`。临时公共补丁已还原，`src/hir/hir.cpp` SHA256 为 `bd8e0fab69833d3a556ce502706cc689551c49701447ea7b78c3fd46c099af7f`。产品补丁含新增文件，不含公共支持修改。
