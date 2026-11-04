# T09 第一段静态实现

本阶段基点为 `db563f46ee49ab9528a248c549bb423c0f6d2f66`，共同版本仍为 `compiler-dev/1`。本文件描述已写入生产前端的 API；Python 拥有值执行记录见 `T09_PYTHON.md`。两后端 Vec 正式支持尚未同时开放，T09 整体仍未完成。

`sema::check_types` 产生实际类型及拥有记录，随后调用 `hir::verify_types`。`check_project` 和 `hir::verify` 继续执行后端支持检查；Vec、move、拥有聚合及含 Vec/move 的函数签名不能取得 `VerifiedHirModule`。freeze 和唯一 Verified 构造者未变。没有另建 AST、HIR 或中间 Verified 包装。

## 类型和节点

唯一公共定义是 `include/lang/hir/hir.hpp`。旧 `TypeKind 0..13`、`NodeKind 0..32` 不变，尾部增加 `Vec` 及 `Move/VecInit/IndexRead/IndexPlace/VecSlice/ForIn`，后者编号为 33..38。

- Vec 恰有一个 `type_args` 元素。内建名仍以 resolver 的身份识别，同名用户声明不变成内建 Vec。
- 列表有同一种元素类型，空列表需要 Vec 上下文。整数/bool 不自动转换成容器。
- 索引和切片端点保留自己的整数类型、数学十进制值及位模式，不提前转换成目标 `usize`。负值和越界值可通过静态检查，后续 runtime 应在实际访问前产生 Panic。
- VecSlice 明确保存 start/end 是否存在，前端从 AST token 与 BracketItem 区分省略端点，后端不必重新解析冒号。
- IndexPlace 保存接收者和索引计算，不代表可逃逸的元素地址，也不产生一个元素拥有值。let Vec 可索引写；`vec[i].field = rhs` 继续拒绝。
- ForIn 使用 resolver 已建立的元素 SymbolId，独占一个循环 ScopeId 和每轮 body ScopeId。元素暂限只读拥有局部。集合求值一次，成功后交给隐藏集合拥有者并开始迭代登记；集合求值失败不结束未开始的登记。

`classify_value(module, TypeId)` 返回 `ValueOps`：`trivial/vec/structure/enumeration/dependent/unsupported`、`needs_drop`、`pod_inout`、声明顺序的具体成员类型、enum 所属 variant 和拒绝原因。enum 的 fields 是各活动变体的备选描述，销毁时只能处理活动 variant。Vec 最后释放应逆索引销毁元素，聚合应逆声明顺序销毁；本阶段提供类型事实，没有生成这些 runtime 操作。

生产者在写记录前将具体替换后的成员类型驻留在公共 TypeContext。分类器不返回私有布局表的 TypeId。有限嵌套泛型聚合可重复同一个声明；循环或不断扩张的拥有图仍拒绝。非捕获函数值本身为平凡值，不沿其签名当作拥有字段递归。

## 拥有记录

`HirModule::ownership` 中的记录使用现有 SymbolId、HirNodeId、ScopeId、TypeId，没有新增全局 ID。

| 记录 | 实际含义 |
| --- | --- |
| ValueUse | consumer、operand、borrow/copy/take/inout、源根绑定、类型；destination 非空时是模式/catch/每轮元素取得值，operand 为该取得操作的 0 号输入。普通节点的 operand 是源码 children 下标；Read/Move 本身的绑定输入也记为 0。 |
| Temporary | producer、full_expression、类型；只登记真实拥有值，Place/FieldPlace/IndexPlace 不产生拥有临时值。每次动态执行需各自维护 live 状态。 |
| LocalLifetime | symbol、scope、type、声明 order、模式；ordinary 参数借用，move 参数与局部/模式/catch/元素拥有，inout 是借用存储。 |
| FullExpression | 表达式结束点和成功清理步骤。绑定/赋值/表达式语句/return/throw 使用语句节点；普通条件使用条件节点；ForIn 和拥有 switch 使用所属节点。 |
| CallLifetime | 调用成功返回时执行的 end_borrow；发生错误使用该调用自己的 error ExitPlan。borrow 不跨入父表达式。 |
| ExitPlan.actions | save_output、end_borrow、destroy_if_live、defer、end_iteration，均带真实对象身份和类型。defers 必须恰为 actions 中 defer 的投影。 |

OwnerRef 的 binding identity 为 SymbolId，其余为 HirNodeId。temporary 是表达式结果，return_output/error_output 是该出口的活动输出，iteration 是 ForIn 隐藏集合拥有者或迭代登记。只有 defer/end_iteration 的 TypeId 为 0。

保存或返回的 Read 取得逻辑 copy；ordinary 调用直接借用绑定或字段，不为回避借用冲突偷偷 copy。较早实参的借用持续到调用返回或失败，期间较晚实参及其嵌套调用不能 move 同一个根槽。临时实参由调用者持有，到 full-expression 结束销毁。返回普通参数或保存到局部真实登记 copy。move 绑定先使源不可读，再产生拥有临时值；move 实参只在真正进入 callee 后转交。后续实参失败不会恢复源。

赋值先计算目标一次并保护接收者，RHS 成功后取得新值再提交，按 Assign 的目标类型替换旧值，旧槽 live 时销毁一次、moved 时不销毁。ValueUse.take 表示成功提交；失败出口只清理已完成的临时前缀，不安装 RHS，不恢复已经发生的 move。没有保存可能因容器变化而失效的元素裸地址。

构造实参按源码顺序产生暂存拥有值，成功后按 parameter_index 交给声明槽位。中途调用失败的计划只逆序销毁实际已完成的成员；失败调用的正常结果、未完成的完整聚合和不活动 enum 载荷均不出现在该前缀中。

return/throw 先保存拥有输出，再销毁当前 full-expression 的剩余临时值。随后逐块执行本块 defer LIFO、本块局部逆声明销毁，再离开外块。destroy_if_live 必须读取对应执行实例的初始化/移动标志，不能无条件释放所有候选对象。非错误出口只在需要清理时登记；错误出口仍总有计划。

简单已绑定拥有 enum 的 switch 借用原绑定，选中分支后 copy 模式绑定，结束这次借用再进入分支体，不制造提前销毁的 scrutinee 拥有临时值。计算产生的拥有 switch 值仍拒绝。ForIn 每轮 copy 元素，离开本轮销毁本轮局部；continue 保留登记，break/return/throw 离开整个循环时结束登记并释放隐藏集合。Panic 无语言清理保证。

## 状态检查和独立验证

实现位于 `src/hir/ownership.cpp`，没有前端对 LLVM 或 Python 的依赖。

拥有状态用 live/moved 的可能集合表示，普通参数和 inout 的借用类别另存。分支和短路只合并实际可达结果；循环计算入口、零次执行、回边、continue 和 break 的固定点。try 汇集每个真实调用失败点的状态，赋值 RHS 失败不提交，handler 中再次失败传到外层。defer 经过注册点才执行，退出时分析实际状态，先执行的 defer 对后执行者的写入保留。

生产者和验证器使用相同的确定性推导函数，但验证器不读取提交的 ownership 或 actions 来生成期望：它从已检查的结构、类型、解析身份和源码次序重新计算，然后逐项核对。原 T08 检查继续独立重算错误、目标、退出作用域和 defer 投影。新增源码检查核对列表/切片/索引/for-in/move 构造是否遗漏，检查拥有读取、move、具体泛型身份、类型标注、调用来源和实参次序。这里没有宣称验证任意优化后 HIR 与源码完全等价。

普通参数非法 move 用 `E_CALL_ARGUMENT`。已移动读取、重复 move、可能已移动合流和 defer 读取用 `E_OWNERSHIP_USE_AFTER_MOVE`。不支持的合法组合用 `E_CAPABILITY_UNAVAILABLE`，坏 HIR 用 `E_IR_INVALID`。没有新增诊断编号。

旧平凡手工 HIR 的新增集合可以为空，验证器仍需独立证明没有新增拥有义务；已有 scalar/HIR/T08 手工构造测试保留。新功能缺记录、改类型/身份、清理顺序错误、重复销毁、遗漏输出、错误活动侧、把清理移到其他出口、伪造 defer 投影均应被拒绝。

泛型先沿现有 T07 实例化过程改号；新 Move、ForIn、VecSlice 也改号。拥有事实在全部具体树完成后重新计算，具体普通泛型实例与普通函数共用同一分类和状态检查。

## 保持拒绝和未运行

继续拒绝普通参数重赋值、动态 string/Vec[string]/原受限 string 成员、部分字段/元素 move、非平凡 inout、索引 inout、索引聚合投影写回、递归拥有图、捕获闭包、foreign 和原 T07 不支持的泛型/方法 move/inout/trait 组合。POD 整体 inout 仍沿用原阶段限制，分类结果没有自动开放后端。

没有新增 len/push/share 等源 API，RHS 改变长度时的检查时点仍未规定。Vec 分配失败或分配大小溢出按 `SEMANTICS.md` 第 7.1 节产生 Panic。静态检查不能证明 ARC/drop 次数、Vec 共享或 Panic 运行结果。Python 的实际运行记录见 `T09_PYTHON.md`；Native 执行、runtime sanitizer 和两端比较仍待独立审查。

测试入口为 `tests/ownership/static_test.cpp`，原 18 份项目及 cases.json 原样保留。`static-extra/independent` 是新增独立源码项目。最终实际命令、环境、失败日志、逐项 CLI JSON、hash 和未执行项记录在 `/tmp/language-t09-static-20261104/{report.md,summary.json,checks/}`；构建结果以这些文件为准，不引用旧任务的通过数量。


## root 证据修正（2026-11-04，唯一一次）

初版任务 `e092e7c2` 被退回，不是已接受依赖。本次 `language-t09-static-correction-20261104` 的 `root_evidence_correction_count=1`，共同版本和旧编号不变。

- Move 由不可变源码操作数决定类别。剥去括号后若为 Name，独立查询 resolver 并按所属具体函数的泛型改号表取得实际 SymbolId，必须对应 Move.binding，不能改为 Read-copy 子节点。计算临时的 Move 不能伪造绑定。
- VecSlice 除省略标记外，逐一检查 receiver、start、end 与各自真实 AST 输入的身份和次序，只允许略去括号、一元加号等透明包装。一起交换 children/start/end、再重算全部拥有记录，也不能通过验证。
- cleanup 返回可选的正常状态。内部合法无限循环没有正常状态时，不再执行更早 defer、外层清理或原定 return/error/jump；分支仍可正常结束的部分继续分析。真正外逃的 return/break/continue/typed error 仍拒绝。
- Call 与 ConstraintCall 共用实参借用、ValueUse、CallLifetime 和失败前缀推导。未实例化模板也按已知 trait 签名检查先借用后 move；不自动 copy，也不关闭已支持的普通 trait 调用。

修正测试包括用真实前端 HIR 同时改树和重算记录的反例，以及括号、具体泛型身份、省略/嵌套切片、不终止清理的可达性、普通 scalar trait、不同拥有绑定及具体实例对照。实际构建、root helper 对照、128 次 CLI 和输入 hash 结果保存在 `/tmp/language-t09-static-correction-20261104/`；初版目录保留不动。本次静态修正没有运行 ARC/drop/Vec；后续 Python 执行见 `T09_PYTHON.md`，T09 仍为 in_progress。
