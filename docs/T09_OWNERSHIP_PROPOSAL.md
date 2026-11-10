# T09 受限所有权、ARC 与 Vec 接口提案

操作：`language-t09-ownership-proposal-20261104`；角色：`developer/shared-interface-analysis`。
依据提交：`e808169dfbdd5711952545d980372a642f2a2653`；共同接口版本：`compiler-dev/1`。
这是接口负责人的初版，供 root 确认和分派；没有修改公共头、实现、测试、清单或旧文档，T09 仍为 `planned`。下文拟议的 C++ 记录和 runtime 操作均未实现。

已完整读取外层 `reviews/t09-preparation-plan.md`。本次准备支持的范围是：已有平凡值，单线程 Vec，整个拥有绑定及临时值的 move，普通参数借用后 copy，包含 Vec 的有限 struct/enum，现有普通泛型实例，现有非嵌套枚举模式，以及正常/typed error 出口的销毁。复杂组合按后文分阶段开放。继续拒绝字段/元素的部分 move、非平凡 inout、捕获闭包、foreign、动态字符串、递归拥有类型图及 T07 尚未实施的组合；不增加关键字、用户 drop、线程、弱引用、GC 或 FFI。

## 1. 实际检查及可直接定位的代码

在本工作树源码上新建 `/tmp/language-t09-ownership-proposal-20261104/build`，以 GCC 16.2.1、CMake 4.4.4、Debug、`LANG_ENABLE_LLVM=OFF`、`BUILD_TESTING=OFF` 实际配置并编译 `langc`（现发布名 `woozyc`）。没有借用其他 worktree 的二进制，没有安装包。native/64、python/64 都使用这个 CLI 做完整 check；本次没有运行生成程序、CTest、LLVM 或 CPython，因此不重报历史三构建测试数量为本次结果。

18 个独立临时项目、54 次 CLI 调用的源码、清单、SHA256、完整 argv、退出码及诊断保存在外层 `probes.json` 和 `probes/`。全部 syntax-only 退出 0。以下完整 check 的两种后端结果相同：

| 项目 ID | 当前完整 check | 能说明什么 |
| --- | --- | --- |
| scalar-control、cleanup-control、scalar-inout-control | 各退出 0，共 6 次 | 当前标量、typed throws/defer、标量 inout 能通过检查；没有执行结论 |
| vec-sharing、vec-slice-elements、vec-for-in、generic-vec-return、owned-enum-pattern、baseline-02 | 各退出 1，`E_CAPABILITY_UNAVAILABLE`，停在 Vec 类型 | 后续共享、切片、遍历、泛型拥有返回及模式行为均未检查到 |
| list-inference | 退出 1，`E_CAPABILITY_UNAVAILABLE`，停在 List 表达式 | 列表推断尚未实施 |
| move-signature | 退出 1，`E_CAPABILITY_UNAVAILABLE`，move parameters require T09 | 未进入移动后的状态检查 |
| move-argument | 退出 1，`E_CALL_ARGUMENT`，参数模式不匹配 | 此项目刻意把 move 传给普通参数，不能当作合法 move 调用的拒绝证据 |
| move-outside-call | 退出 1，`E_CALL_ARGUMENT`，mode marker outside its matching call | 当前实现限制；不能据此断言所有此类语法永久非法 |
| pod-inout | 退出 1，`E_CAPABILITY_UNAVAILABLE` | 冻结规则允许的平凡 POD 整体 inout 当前仍缺实现 |
| ordinary-rebind | 退出 1，`E_CAPABILITY_UNAVAILABLE`，no specified source-language rule | 普通参数重新赋值仍须用户决定 |
| index-current-site、slice-current-site、for-in-current-site | 退出 1，`E_CAPABILITY_UNAVAILABLE` | 用标量接收者定位三个独立的当前拒绝入口；它们不是未来合法 Vec 正例 |

合计：语法检查 `passed` 18；完整检查接受 `passed` 6；能力拒绝 `unsupported` 26；参数模式反例按预期拒绝 `passed` 4。功能运行、ARC/drop 计数、失败注入、O0/O2/Python 同源比较全部 `not_run`。没有把当前拒绝计作所有权功能通过。

当前入口的具体位置如下，行号以起点提交为准：

| 对象 | 当前位置 | T09 要补的内容 |
| --- | --- | --- |
| 源 AST、参数模式、列表/索引/切片、for-in | `include/lang/syntax/ast.hpp:6-55`；`src/syntax/parser.cpp:178-255,343-365` | 原语法已经足够；不新增 AST 关键字。Slice 省略端点需根据冒号及 token 范围区分，不能只凭 children 数量猜 |
| for-in 的真实元素声明 | `src/modules/resolve.cpp:287-294` | 已以 ForIn AST 为声明分配 SymbolId；先检查集合，再建立每轮绑定。沿用它，不能在 sema 凭名字新造声明 |
| 唯一类型表 | `include/lang/hir/hir.hpp:7-55`；`src/hir/hir.cpp:37-93,95-120` | 当前没有 Vec TypeKind；追加类型及类型实参形状检查，保留旧类型编号 |
| Vec、move、inout 拒绝 | `src/sema/check.cpp:265-267,292-307,703-708,740-774,1270-1285` | 分别补类型、签名、值/位置分类及实参拥有处理；不能只删除 unavailable |
| 泛型/方法实参模式 | `src/sema/check.cpp:1145-1188`，尤其 `1171-1173` | 现有泛型/方法非 ordinary 模式明确拒绝；先保留，不能随 Vec 类型一起误放开 |
| 绑定、赋值和退出 | `src/sema/check.cpp:1637-1735,1779-1848` | 初始化完成后才可见；赋值目标先于右值；ForIn 目前落入默认拒绝；增加拥有状态和 full-expression 处理 |
| 当前局部记录 | `src/sema/check.cpp:28-32,2242-2267` | Local 仅有类型、可写及普通参数标志，没有 initialized/moved 状态；函数参数与最外层块同作用域 |
| 模式与 catch | `include/lang/hir/hir.hpp:182-214`；`src/sema/check.cpp:1504-1627,1684-1713` | 模式绑定和 catch 都有真实 SymbolId/ScopeId；拥有绑定需要独立 copy 或错误载荷转交，不能裸复制引用值 |
| 退出及 defer 引用 | `include/lang/hir/hir.hpp:215-247,291-302`；`src/sema/check.cpp:2043-2239` | 目前只有 defer，非错误出口无 defer 时不保存计划；需补临时值、局部销毁和迭代登记的清理 |
| 独立 HIR 检查、发布 | `src/hir/hir.cpp:429-460,566-656,2091-2254,2576-2735` | 独立重算拥有事实；维持 freeze → 检查 → 唯一 VerifiedHirModule 发布 |
| 泛型复制 | `src/sema/check.cpp:2312-2466`；`include/lang/hir/hir.hpp:360-386` | 新记录中的符号、节点、作用域要改号；派生事实在具体树完成后重算 |
| Native 当前临时值 | `src/ir/lower.cpp:160-282,343-375` | 当前只有 SSA 计算结果，没有拥有临时值的结束/失败销毁；discard 也只是忽略结果 |
| Native 退出、模式与参数 | `src/ir/lower.cpp:36-75,137-157,294-339,378-461,484-500` | 原有 defer 清理、载荷提取、参数 slot、自然 Unit 返回都需接上拥有义务 |
| Native 独立检查 | `src/ir/verify.cpp:200-226,515-734`；`src/ir/obligations.hpp:10-175` | 目前 slot 初始化只会增加；已有待完成清理栈和真实 call/exit 次数比较，尚无 move/drop 数据流 |
| 私有布局替换 | `src/ir/layout.hpp:9-60` | AggregateTypes 在私有 TypeContext 驻留存储类型；这些新增 ID 不得写回公共 HIR/NIR |
| LLVM 类型、参数及双输出 | `src/backend/native/emit.cpp:66-114,173-187,459-564` | 当前普通值直接传值、inout 传地址；只在对应 TryCall 后继读取活动输出。追加 Vec/drop，保持现有 ABI 行为 |
| Python 临时值及块清理 | `src/backend/python/emitter.cpp:223-227,240-391,401-423,466-518` | save 只有 Python 局部名；tuple/Cell/Field 无语言拥有计数；无 defer 的块目前没有 finally |
| 运行库 | `runtime/native/panic.cpp:7-56`；`runtime/python/langrt.py:6-71,227-236` | native 当前只有 Panic/typed-error 报告；Python 当前没有 Vec，Panic 为 BaseException，TypedError 保存逻辑类型键和载荷 |

`contracts/tasks.json` 记录 T05/T06/T08 当前 profile 为 done，T07 为 in_progress，T09 为 planned。`docs/GENERICS.md:46` 和 `docs/IR_CONTRACTS.md` 末尾仍保留早期 T08/浮点拒绝描述；实际当前代码和 cleanup-control 已接受 T08 检查。提案不更改这些旧记录，也不由这些文档推断所有功能已完成。

## 2. 已冻结规则与本次实施选择

只引用与接口有关的条目，不重抄规范：

| 已冻结规则 | 依据 | 对实施的直接要求 |
| --- | --- | --- |
| 普通参数不消费，返回/存储参数时 copy；泛型也相同 | `SEMANTICS:95,121,125` | 不能把 borrow 原样放入返回槽或对象；先取得新拥有值 |
| move 消费整个拥有绑定/临时值，普通参数借用不可被 move | `SEMANTICS:87,122,125` | 在表达式完成时清空源拥有状态；后续实参失败也不回滚 |
| inout 限数值/bool 或平凡 POD 的整体 var | `SEMANTICS:123-124` | 共享容器、拥有聚合、字段/索引、外部引用继续拒绝；递归 POD 分类须在两端共同使用 |
| Vec copy 共享容器；索引写入所有别名可见；切片新容器、元素逻辑 copy | `SEMANTICS:101-111` | 无 COW，无 Python 负索引、端点截断；let Vec 仍可索引写入 |
| 遍历固定入口长度，每轮取得元素拥有值；长度改变前检查活跃遍历 | `SEMANTICS:113` | 登记状态放容器对象，别名共用；取值不能保存扩容后失效的元素地址 |
| full-expression 逆序销毁；输出先保存；后续实参错误清理先前临时值 | `SEMANTICS:76-87` | 需要成功点和每个失败点的拥有状态，不能只在块尾插 release |
| 每块 defer LIFO，然后局部逆声明销毁，再离开外块 | `SEMANTICS:153-159` | 不能先跑所有内外 defer 再统一销毁局部；defer 可读取本块仍存活绑定 |
| struct/活动 enum/Vec 分别逆字段/逆活动字段/逆索引 drop；失败逆实际初始化顺序 | `SEMANTICS:115,133` | 成功销毁顺序与命名构造实参的求值顺序要分开记录 |
| Panic 不进 typed catch，没有语言清理保证；Python finally 须隔离 | `SEMANTICS:143-147,161`；`BACKENDS:125,174-176` | 不把 Panic 当 typed error，也不把 CPython 的宿主析构当语言释放记录 |

本提案的内部选择：保持普通参数的短期 borrow；所有会被保存的读都执行类型化 copy；用已有 ID 标识拥有者和临时值；构造器先按源码顺序把成员放在独立拥有暂存槽，全部成功才提交完整值；Python 对自有 Vec 维护明确的语言拥有计数及 drop，不依赖 `__del__`。这些选择不增加源语言 API。

初期元素类型限数值、bool、Unit、非捕获函数值，以及这些类型、Vec、有限 struct/enum 的非递归组合。分两步先 Vec[平凡值]，再嵌套 Vec/拥有聚合。普通泛型函数和泛型 struct/enum 在得到具体类型后适用同一规则。保持既有字面量 string profile；第一轮不放开动态 string、Vec[string] 或原来被拒的 string 成员。递归到 Vec 的布局虽可以有限，第一轮仍单独报告未支持，不能误报为无限值布局或宣称已处理强引用环。

## 3. 最小共享记录与 API

以下是供 root 选择具体拼写的拟议记录；由一个公共头定义，sema/Native/Python 同时使用。本次不发布头文件，不另起契约版本，也不把 runtime 私有对象头纳入源 ABI。

### 3.1 继续沿用的身份与新节点

保留 TypeKind `0..13`、HIR NodeKind `0..32`、现有 ParamMode、CalleeKind、ExitKind 和所有强类型 ID 的旧值。TypeId 仍从 1 开始，其余现有 arena ID 仍从 0 开始。仅在 enum 和 NodeData 尾部追加，二者顺序一致；不重排旧节点，不为 T09 另造 AST/HIR。

建议只追加一种类型 `Vec`，复用 `Type.type_args`，恰好一个元素 TypeId，无名义 declaration/binding。同步修改 TypeContext::intern、substitute、类型打印/逻辑键、valid_type、有限布局和支持检查。Vec 的存储是句柄，有限值布局检查不沿元素展开；元素是否可 copy/drop 是另一个检查，不能混同。普通用户声明若遮蔽 Vec，仍先按已解析 SymbolId 判断，不按字符串强行识别内建。

建议追加以下 HIR payload；Source 继续指向原 AST：

| 拟议 payload | 最小字段及 children | 检查 |
| --- | --- | --- |
| Move | 源整个绑定 SymbolId，或一个临时表达式 child，二者择一 | 剥去括号后分类；拒绝字段/索引、普通参数和 inout 借用；已有绑定须有源码 move 标记 |
| VecInit | children 为源码顺序的元素 | 上下文/推断得到同一元素类型；空列表必须有上下文 |
| IndexRead / IndexPlace | children 为 receiver、index | Vec 单项整数下标；IndexPlace 仅用于索引赋值，不得充当 inout 或普通一等值 |
| VecSlice | receiver 及可选 start/end 的节点引用；children 按源码顺序，只列实际存在者 | 从 AST token 明确识别省略端点；后端不再解析冒号 |
| ForIn | 元素 SymbolId、每轮 body ScopeId、body 引用；children 为集合和 body | 使用 resolver 已存在的 ForIn 声明，拥有隐藏集合句柄和迭代登记；Break/Continue 可指向它 |

`Read/Place/FieldRead/FieldPlace/Binding/Assign/Call/Pattern/Try/Catch/Defer/Return/Throw` 保持原身份。Move 不加入数值 UnaryOp。常量折叠不能抹掉具有拥有或销毁效果的表达式。

### 3.2 共享的五类信息

不增加新的全局 ID。下面 OwnerRef 是带 kind 的现有身份引用：`binding(SymbolId)`、`temporary(HirNodeId)`、`return_output(出口节点)`、`error_output(出口/调用节点)`、`iteration(ForIn 节点)`。同一个节点的一次动态执行只产生一个该类拥有结果；循环和 defer 展开在后端各有执行实例，不能共享运行时状态。

| 拟议记录/API | 必需字段/输入输出 | 谁使用 |
| --- | --- | --- |
| ValueOps / classify_value(module, TypeId) | copy/drop 种类：平凡、Vec 句柄、struct 字段、enum 活动载荷；递归字段的具体类型与声明顺序；是否可作 POD inout；模板参数用待替换规则，未支持类型给原因 | sema、HIR verifier、两端代码生成；不含 LLVM/PyObject 指针、物理 retain 次数或对象偏移 |
| ValueUse | consumer HirNodeId、operand 在源码 children 中的位置、`borrow/copy/take/inout`、可选根 SymbolId | 规定每条值使用怎样取得所有权。CallArg.parameter_index 只排列已经求值的结果；不能拿它代替求值顺序 |
| Temporary | producer HirNodeId、所属 full-expression 节点、TypeId | 标记会产生拥有结果的表达式；其当前 live/已转移状态由控制流决定，不能无条件销毁所有候选临时值 |
| LocalLifetime | SymbolId、ScopeId、类型、声明次序；参数模式从函数签名取得 | 普通参数是 borrowed，move 参数是 owned，inout 是独占存储。模式/catch/每轮元素也在这里登记；弃置 `_` 仍需销毁值 |
| CleanupStep / FullExpression | step 为 `end_borrow/destroy_if_live/defer/end_iteration` 加 OwnerRef 或注册节点；FullExpression 记所属节点及成功结束所需步骤 | 两端按相同顺序清理，HIR verifier 独立重算；部分构造未完成的成员由其独立临时拥有者表示 |

TypeContext 仍是唯一公共类型表。实例字段替换可能生成新 TypeId：sema 在发布前将实际需要的 copy/drop 类型驻留；若分类器内部用私有 TypeContext，则返回的私有 ID 必须随其上下文留在后端内，不得写进公共记录。两种策略由 root 选一个共同接口；不要把 `src/ir/layout.hpp` 现有私有 ID 当成公共 TypeId。

最小公开调用仍是现有 `check_types → verify_types`、`check_project → hir::verify`、`ir::lower → ir::verify`、两端 emit。可以增加纯前端 `check_ownership/verify_ownership` 内部步骤，但无需第三种“半 Verified”包装。只有独立验证、目标支持检查成功后才能由原 `hir::verify` 发布快照；`ir::verify` 同样仍为 VerifiedModule 唯一构造者。

新记录建议附在 HirModule 的一个 ownership 记录集合中，类型/节点/作用域使用现有引用；采用 vector/value 成员，freeze 时自然复制。旧平凡手工 HIR 允许这些集合为空：verifier 由树证明没有新增拥有义务；遇到 move、Vec 或非平凡成员时缺记录就是 `E_IR_INVALID`，不能把“空”解释成跳过检查。合法但当前 profile 未开放的组合用 `E_CAPABILITY_UNAVAILABLE`；所有权源码错误使用共同诊断结构。`contracts/diagnostic_codes.json` 已登记 `E_OWNERSHIP_USE_AFTER_MOVE`，`contracts/test-cases.json` 的 use_after_move 也固定此码，移动后使用及重复移动应沿用；借用冲突等未指定诊断若需新增，由 root 发布，本提案不先占号码。

### 3.3 扩展 ExitPlan，保留 T08 的含义

保留 `kind/error_type/target/exited_scopes/defers/saves_value`，尾部追加有序 `actions: vector<CleanupStep>`。`defers` 是 actions 中 defer 项的投影，旧消费者仍可核对它，但新消费者必须执行完整 actions。二者由同一组作用域事实产生，验证器独立重算并比较，不能容许两份不同计划。

退出顺序固定为：先保护正常/错误输出；结束相关短借用，按逆创建顺序销毁仍拥有的 full-expression 临时值；然后对每个 exited_scope 从内到外执行该块 defer LIFO、该块局部逆声明销毁。iteration 登记在真正离开整个 ForIn 时结束；continue 保留登记，只清理本轮绑定和 body。普通 For 的条件为假出口也要销毁循环初始化绑定。若清理体自己 Panic，剩余动作不保证执行。

错误出口仍总有计划。非错误出口的稀疏条件从“有 defer”扩展为“有任何非平凡清理动作”；旧平凡且无 defer 的节点仍保持空 exits。无词法块退出的 full-expression 正常结束放在 FullExpression 记录，不伪造额外 ExitKind；嵌套调用失败的临时前缀附在该调用自己的 error ExitPlan。`saves_value` 保留布尔含义，具体被保护者由输出 OwnerRef 表示，不能销毁后再加载返回值。

## 4. 拥有检查的实际数据流

每个拥有槽至少有 `uninitialized/live/moved/maybe_live`；borrowed 普通参数和 inout 单独分类，不能拿 live 当作拥有。源码 let/var 均须初始化，uninitialized 只是初始化前、构造中或内部输出槽的状态，不开放无初始化声明。

| 操作 | 要求与状态变化 |
| --- | --- |
| 普通读、字段读、模式绑定、存入字段/元素、返回普通参数 | 源可读；取得独立逻辑 copy。Vec retain，聚合递归 copy，标量直接复制；参数本身不变 |
| 普通调用实参 | 绑定/字段可短借用，活到调用返回的成功或失败边；临时实参的拥有者由调用者保留到 full-expression 结束；被调用方存储/返回时 copy |
| 整个 move | 源须为 live 的本地拥有绑定或 move 参数；将值交给新临时拥有者，源立即 moved。普通参数/inout、字段/索引位置拒绝；非捕获函数值仍是平凡值，不创造捕获环境 |
| move 参数接收临时值 | 可直接转交新建拥有临时值；已有绑定必须显式 move。签名模式确定临时值的 take，不能要求给所有临时调用再添加新语法 |
| 调用前失败 | 先保护实际错误载荷；结束已开始的 borrow/inout；逆序销毁已求出的拥有临时值；未求值实参不执行，已 moved 源不恢复 |
| 真正进入调用 | move 参数的临时拥有义务交给被调用方；无论被调用方成功还是 typed error，调用者都不能再销毁这些实参。ordinary 临时仍属调用者 |
| 重新初始化 var | 先计算目标一次，再求出并保护 RHS；仅成功时安装新拥有值，原 live 值销毁一次，原 moved 值不销毁；RHS typed error 时不提交赋值，保留失败点的真实状态及已发生副作用（包括 RHS 中已完成的 move 或重建），不恢复求值前状态。禁止 RHS 读取已 moved 的旧值 |
| let、const、普通参数重新赋值 | 保持已有不可写/未规定处理；不因 move 后为空而自行允许 let 第二次赋值。若要开放另需用户确认 |
| 条件/短路 | 只让实际求值的分支产生临时值；条件 full-expression 在进入分支/body 前结束。动态分支在合流处按所有可达状态合并，不能只检查源码扫描顺序 |

实现时 ordinary 参数借用不是深层 readonly：通过 Vec 借用照样可以索引写入同一容器。借用存活期间不能 move/destroy 其拥有槽；别名 Vec 句柄指向同一对象不代表两个绑定槽重叠。whole-POD inout 延用根存储身份检查，扫描 callee、receiver、全部嵌套实参读/写，拒绝与其他访问重叠；不可由两个名字不同推断外部存储不重叠。

同一调用中，较早实参借用 x 后，较晚实参试图 move x，必须拒绝，不能为消除冲突偷偷把前一个实参改成 copy。重建整个 var 的 `=` 只需要取得槽地址，不读取旧值，moved 槽可以作该目标；复合赋值、字段位置及索引接收者仍要求相应基础值可读。Native 当前 `verify.cpp:296-310` 把 load/address 都要求为已初始化，接入重建时须区分“仅定位待写槽”与“读取值”。字段赋值求 RHS 期间还需保护基础拥有值，不能取得字段位置后又通过嵌套调用 move 整个基础值。

分支只有一侧 move，合流得到 maybe_live，后续读或再次 move 拒绝；清理按实际 live 标志销毁。无条件完整 var 赋值可将所有进入状态变为 live。返回/throw 分支不参与其后正常合流。循环对入口、回边、continue（经过 step）求不动点：外层绑定本轮 move 且没有在每条回边重建，下一轮使用应拒绝；循环零次执行与 break 状态也参与循环后合流。不要每轮把状态重置成声明时的 live。

try 的 catch 输入来自 protected body 每个真实 typed-error 点的状态及该点完成的清理。后续实参错误发生在 move 后，catch 看到 moved；发生在 move 前，catch 看到原 live，合并时可能 maybe_live。handler 自己的错误走外层，不能拿 try 入口状态“恢复”所有绑定。正常 body 与可达 handler 的状态再合并。沿用当前类型/效果对完整源码的检查及实际可达流分析，不靠 errors 摘要代替状态。

defer 注册时不复制值、不立即冻结变量；在每个可能执行它的出口，检查其 body 实际访问的绑定状态。`Defer.references` 是定位外部引用的起点，仍需分析读、赋值目标及执行过程，不能把所有 Place 都误算成读取旧值。中间 move 后又在每条到达出口的路径重建，可以执行 defer；重建前若有可能抛错并离开该块，那个出口必须报错。未经过注册的分支没有该 defer，不能因此误拒 move。清理体也使用同一状态转移，LIFO 中先执行的 defer 对后执行者的影响要保留。

模板在声明约束下检查一般的 copy/move/借用规则；不要求 TypeParameter 已知为平凡类型，也不把它擦成 Any。具体实例替换完成后重新分类字段、实例局部、Temporary、ValueUse 和退出清理；沿用实例预算及来源检查。新增 ForIn/Move 等节点的引用、defer 引用、模式/catch 绑定必须随实例改号。泛型普通返回 `return x` 对 Vec 实例为 retain，对拥有 struct 为递归 copy；非捕获函数实例的函数值本身无 ARC。

## 5. 临时值、部分构造和销毁顺序

不让两个后端各自决定临时值何时结束。Temporary 属于一个源 full-expression，成功时被转移的值从活跃列表移除；其余在结束时逆创建销毁。借用视图不新增拥有义务，但必须保护其底层拥有者。

| 求值区域 | 必须记录的拥有者及结束点 |
| --- | --- |
| 表达式语句、弃置结果、绑定初始化 | 调用/列表/切片/逻辑 copy 的结果均可能拥有；绑定成功才 take 到新槽；弃置结果也必须 destroy |
| 简单/复合赋值 | 目标 receiver、index 各算一次并保持可用，之后 RHS 临时值；复杂目标不能只保留裸元素指针跨越 RHS 调用。成功写入后释放目标保护和未转交的临时值 |
| if、for 条件；For 的 init/step | 每次条件求值各有结束点；条件内部调用失败走其自己的前缀清理；step 在 continue 的 body 清理之后执行 |
| return/throw | 普通借用先 copy，拥有结果 take 到专用输出槽；先清理其他临时值，再逐块 defer/drop。对同一个类型的 value/error 也分别跟踪活动侧 |
| Call/函数值/方法 | callee（含接收者）在前，args 按 CallArg.source_index；临时值表含嵌套调用、命名参数和已求值 move 参数。按 parameter_index 排列不能重排创建或失败顺序 |
| 构造器 | 每个完成的字段/载荷/元素表达式先取得拥有暂存值，记录其真实初始化次序；最后一次性转交完整聚合/Vec。最后结果被弃置时也销毁 |
| ForIn 集合 | 集合表达式只算一次，将需要的拥有 copy/临时转入隐藏循环拥有者，再结束其余临时值。每轮取元素 copy 到本轮槽，之后才能执行 body |
| switch/模式/catch | 简单拥有绑定的 switch 可借用被匹配值，命中后 copy 所需模式绑定；通配不 copy。catch 从错误拥有槽 take 到 handler 槽，包括弃置错误的销毁；计算产生的拥有 switch 临时值见第 9 节的未规定项 |

构造的最小做法沿用 `StructInit.fields/EnumInit.payload` 的源码顺序与 parameter_index：成员先存独立暂存槽，完整聚合值尚未初始化；全部成功后按声明位置提交，并清空这些暂存槽的拥有标志。因而不用一开始就增加可任意访问的“半个 struct”或部分 move。暂存槽代表已经完成的逻辑成员，失败时仅逆序销毁它们。

具体次序举例（这里是未来要求，未执行）：声明字段为 a、b、c，构造源码提供顺序 b、a、c；若 c 的生产函数 typed throw，先保护错误，销毁已经完成的 a，再销毁 b，不能销毁 c 或调用完整 struct 的 drop。全部成功后则完整 struct 按 c、b、a 销毁，与实参顺序无关。enum 只处理选中的 variant，错误侧/其他 variant 不读不 drop。字段值若是拥有聚合，自身按该类型的规则递归 drop。

Vec 字面量也可先用元素暂存拥有槽：第 k 个元素生产失败时，销毁 k-1 到 0 个已完成元素和各自尚未转移的内部临时值；目标 Vec 尚未对源码可见。成功时创建容器并接管全部元素；runtime 初始化容器存储另用 completed_count，只能释放已经装入的前缀，完成后最后 release 才按 n-1 到 0 销毁。后端应明确标记“暂存拥有 → 容器拥有”的转交，不能两边同时 drop。

部分构造的源 typed-error 测试使用本语言已声明 throws 的成员生产函数，分别在第 0/中间/最后一个生产点失败；不能在 copy/drop 中凭空引入 typed error。资源分配失败需与这些测试分开：当前规范没有固定 OOM 如何报告，不能私自将 bad_alloc/MemoryError 转成某个源错误。runtime 单元失败注入可以验证自身 completed_count 的回收，但源程序的 OOM 行为待第 9 节决定，Panic 路径也不能承诺上述语言清理。

字段和元素替换先取得 RHS 的独立拥有值，防止 `x = x`、同源元素替换先释放后 copy。旧值只销毁一次，新值只接管一次。当前没有用户 drop/外部回调时，后端可选择安全的安装和旧值释放细节；未来若引入可重入析构，须另定其对替换操作的观察规则。索引赋值只替换目标元素，不释放容器其他元素。

## 6. NativeIR 与运行库的最小交接

保留 `include/lang/ir/native.hpp` 中 Op `0..20`、Exit `0..6`、ValueId/LocalSlotId、TryCall 两侧块参数与 Handler 的实际实例身份。追加类型化 copy/move/destroy、borrow/end_borrow、槽初始化/移出、Vec 构造/取值/写入/切片/迭代登记操作；具体枚举拼写由 root 一次发布。不把这些操作伪装成源 `Call`，不把 runtime retain 当成额外源码调用来满足 T08 的 call 数量。

Slot 需补拥有种类（local owned / ordinary borrowed / move owned / inout）及必要的初始化标志；SSA 定义需区分 owned、borrowed、地址。现有 `store/load` 对平凡值保持原意义，非平凡操作必须表明 copy、take 或 replace；不能靠 emitter 看到某类型就随手多加一次 retain。分支/循环合流的 conditional live 用标志或分块表达，读取仍须在所有路径可用，destroy_if_live 必须以相应标志为条件。

必须保留并扩展两种独立检查：

1. CFG 数据流继续维护 T08 尚未完成的退出清理栈；每帧的 next 从 defer 下标扩展为 action 下标。叠加活跃拥有者、借用、输出侧和部分构造暂存状态；合流、正常/错误返回不得留下未完成动作。已证明不可能执行的穷尽 enum default 仍按现有规则处理，不得为了让 drop 通过放松证明。
2. `RequiredExits` 继续只读不可变 HIR，独立计算每份出口展开中的真实源码 call/exit 义务。新增 full-expression、copy/take/destroy/end_iteration 义务也从已验证 HIR 推导，按同一展开上下文逐份核对，不能读取 lowering 输出自证。只加标记、删掉实际 drop、将释放挪到另一份 defer 展开或不可达块，都必须拒绝。对于类型化 destroy，verifier 验证实际 opcode、对象/slot 和状态；LLVM 再将它生成真正 release/drop，运行测试核对最终效果。

当前 call 检查核对 origin、签名、类型和展开次数（`verify.cpp:355-375,455-480,618-619,651-652,729-734`），不等于证明任意表达式都与源码等价。新增拥有检查不能把这些已有保证删掉，也不能声称计数本身证明了 release。需要强化 direct call 时应核对实际 callee 的函数来源；indirect call 保留已求值函数值及签名，不凭同签名猜目标。该项是阅读所得的检查范围说明，本次没有运行坏 NIR 注入测试。

运行库只需要下列私有职责。`lang_rt_retain/lang_rt_release` 是 `SEMANTICS:131` 允许的实现符号；表中其余名字是内部概念名，不是源方法或已发布 ABI：

| 内部操作 | 输入、输出、拥有责任 |
| --- | --- |
| allocate_vec | 元素布局/类型操作、长度/容量、SourceSpan → 新拥有句柄；保存已初始化元素数，计算 size/alignment 使用目标 DataLayout，检查乘法/对齐溢出 |
| initialize_element_take / finish | 构造中的句柄、连续索引、元素拥有槽 → 接管元素并更新 completed_count；finish 才发布可用 Vec。失败中对象不允许普通 get/set/遍历 |
| retain / release | 已有句柄；retain 检查计数溢出；release 在 0 时调用 Vec 专用 drop，逆序 destroy 已初始化元素，再回收存储 |
| copy_value / destroy_value | 类型专用生成函数或静态描述加值地址；struct 递归，enum 查活动 tag，Vec 句柄 retain/release，平凡值不装箱。这是类型专用代码，不要求建立通用运行时反射 |
| vec_length / get_copy | 容器借用、已检查索引 → 长度或新的元素拥有值；copy 完成前不释放容器，不能返回会逃逸的裸元素地址 |
| set_take | 容器保护句柄、稳定整数索引、新元素拥有值 → 接管新元素、销毁旧元素；边界检查在访问前完成，失败为 Panic，无 typed error |
| slice_copy | 容器借用、合法 start/end → 新拥有容器，元素逐个逻辑 copy；结果容器不同，引用元素仍共享；失败构造用 completed_count |
| iteration_begin / iteration_end | begin 取得循环保护拥有关系，增加同一容器的 active_iterators，返回入口长度；end 恰好一次减少计数并释放保护。每轮 get_copy，不缓存元素指针 |
| check_length_change | 容器、拟议长度变化 → 活跃遍历时在修改、挪动、释放元素前 Panic；每个改变长度的实际内部入口都必须调用，包括以后从别名调用的入口 |

索引/切片保留整数的数学值，先检查负数及范围，再转换为目标索引宽度。省略 start 为 0，省略 end 为实际 len；不继承 Python 截断。赋值目标保存容器句柄及索引，跨 RHS 调用后重新定位元素；不保存可能失效的 data 指针。对 RHS 期间可能改变长度的未来组合，具体重新检查时点见第 9 节。

第一轮允许 struct 的 Vec 字段索引写入、嵌套 Vec 索引写入和整体元素替换；`vec[i].value_field = rhs` 涉及对容器中值聚合的投影回写，先明确拒绝，不假装已有 FieldPlace 自动支持。inout 索引元素及 move 索引元素同样拒绝。此限制不影响把 struct/enum 作为 Vec 元素保存、取出逻辑 copy 和最终递归销毁。

拥有错误离开 native main 时，入口包装先读取逻辑类型键并报告，再销毁活动错误载荷一次；不能只沿用当前平凡载荷的 report 后 return。正常 main 的现有 i32/Unit 返回约束保持，普通 Result.Err 仍不进入错误通道。

对象头只需引用计数、Vec 长度/容量、已初始化前缀、活跃迭代计数及元素 drop 所需信息；布局由 native lowering/runtime 共同选择，不写入源 TypeId，不与外部 PyObject 共用释放器。计数非原子，仅单语言线程；copy/drop/retain/release 不抛 typed error。不要给未知别名加 noalias，不改变已有整数回绕与严格浮点选项。

## 7. Python 的最小交接与 Panic 隔离

Python 使用相同 ValueUse、Temporary、LocalLifetime、FullExpression、ExitPlan.actions，接收 VerifiedHirModule；不接收 native 对象头或 ARC 调用次数。

自有 `langrt.Vec` 可以包含共享存储对象和语言拥有计数。逻辑 copy 增加语言拥有关系，最后一次逻辑 release 执行逆序元素 drop 并清空存储；Python 引用只保障实现对象存活，不负责决定语言 drop 时刻。不使用 `__del__`、weakref 或宿主 GC 回调作为正常/typed-error 释放证明。struct/enum 的 tuple 可以继续作为值表示，但 copy/drop 必须按类型递归，不能把 tuple 赋值等同于取得所有字段拥有权。

生成器为每个需要清理的块建立注册/初始化标志；即使没有 defer，只要有拥有局部，也必须生成对应清理。成功 return/typed throw 的结果先转入输出拥有者；TypedError.value 也是一个拥有者，匹配 handler 后交给 catch 槽并清空载体责任，未匹配则继续向外传，顶层报告后按普通错误出口处理载荷。普通借用参数函数退出不 drop 调用者句柄。

现有 `except _rt.Panic → allow=False → raise` 与带条件 finally 的规则扩展到临时值、部分构造、迭代及局部销毁；Panic 在主体或某个清理中发生时，不自动执行剩余语言动作。不要新加无条件 finally 去释放 Vec 或 iteration token，造成与 native 不同的清理承诺。LoopContinue/SwitchBreak 属于结构化跳转，仍执行实际离开块的动作；typed catch 只匹配 TypedError。KeyboardInterrupt/SystemExit 不包装成 typed error，外部 Exception 转换留在 T10 的声明绑定处。

Python 运行库的 logical type key 为 Vec 递归扩展，不能写入会话 TypeId；非活动 enum、TryCall/异常的未初始化正常侧不 drop。改动 runtime 后由 root 同步 `include/lang/backend/python.hpp` 的 runtime_version、嵌入文件和生成产物检查；不能让旧 `/3` runtime 加载新拥有代码。新增 finally 也计入现有生成深度限制，超限仍在发布文件前明确拒绝。

## 8. 同源共享与释放怎样实际观察

后续验证至少使用同一份源码、独立预期及记录，分别经真实 check/build/native O0/O2/Python 执行；不能只运行手写 runtime 示例。分配数与 retain 次数可随优化变化，最终存活对象、drop 次数、受规范约束的顺序必须成立。

| 验证内容 | 必须观察的结果 |
| --- | --- |
| 共享写入 | 本次 vec-sharing 源码：copy a 为 b，b[0]=7 后 a[0] 为 7；同时观察两句柄的内部逻辑对象 ID 相同，防止两个后端共同深复制还碰巧返回同值 |
| 切片隔离与元素共享 | 新容器对象 ID 不同；Vec[Vec[i32]] 切片中元素句柄与原元素相同；修改内层元素在原容器可见，替换切片外层槽不改原槽 |
| 返回借用、泛型返回及 move | 调用者仍能读取 ordinary 实参；返回值在调用者结束后仍有效；move 后读/二次 move 被前端拒绝，var 完整重建后可读；函数值调用同样处理 |
| 最后释放与聚合顺序 | 对象只 drop 一次，最后拥有者离开后 live_objects 为 0；局部逆声明、struct/enum 逆字段、Vec 逆索引；defer 事件发生在本块局部 drop 前 |
| 失败前缀 | 后续实参、字段/元素生产函数真实 typed throw；后续生产函数未执行，之前的临时值逆序 drop；move 源在 catch 中仍 moved；错误载荷只由活动错误通道持有 |
| 循环与未注册 defer | 每轮元素 copy 在本轮出口销毁；continue 保留迭代登记，break/return/throw 结束登记；未经过的 defer 没事件 |
| 索引/切片 Panic | 负数、越界、反向区间、端点越界均访问前失败，检查 Panic marker 与位置；普通 main 返回 101 无 marker，不误算 Panic |
| 活跃遍历改长度 | 源方法名未冻结，源码用例 TODO；先测真实内部修改入口：别名修改后立即 break/return、删后恢复长度都应在第一次修改前 Panic，容器未被改写。此内部测试不能冒称源码调用已实现 |

计数观察器属于测试配置，使用不抛错、无额外拥有引用的事件输出，记录稳定于一次运行的 object_id、逻辑类型键、源码位置、allocate/final_drop/free 及 live_objects。Native 观察器须链接进同一份 CLI 生成程序；Python 观察器读取实际生成代码调用的 Vec/copy/drop，不能让观察器额外 retain 而改变最后释放。不要比较两个进程的地址或裸 TypeId。

drop 事件只能在真实执行类型 drop/release 的位置产生；同样记录 free 和最终存活集合，配合运行库内存检查，不能由“计划含 destroy”生成成功事件。Native 编译器的 ASan 不覆盖生成程序与 runtime，后者需要单独启用合适检查。Panic 用例只核对已发生事件和禁止误执行的后续 defer，不要求进程终止前 live_objects 归零。本次所有这些观察均为 `not_run`。

## 9. 需要用户决定的未规定行为

这些不由本提案批准。root 可先分派不受影响的步骤；涉及下列组合时保持明确未支持，问题必须连同最小源码形式询问，不能由 sema/两个后端分别选答案。

| 未规定项 | 现有依据、具体影响 | 本次处理 |
| --- | --- | --- |
| 普通参数重新赋值 | `check.cpp:759-766`、`GENERICS:46` 明确等待用户选择。是局部重绑定、禁止，还是另有含义，影响参数槽拥有责任 | 继续 E_CAP，不把新 LocalLifetime 当授权 |
| Vec 长度查询及长度修改的源方法/内置名、签名与返回值 | 现有输入只冻结类型、列表、索引、切片和 for-in；没有冻结 len/push/append/pop 等公开名字 | 不发明源 API；内部 length/结构修改检查可先实现；源码修改长度测试 TODO |
| share 的签名、适用类型和与普通 copy 的关系 | `LEXICAL_SYNTAX:98,372` 只保留 share(value) 名称，不能推出完整接口 | 不实现 share；本地 `let copy = parameter` 已足以取得合法 copy |
| 计算产生的拥有 switch 被匹配临时值何时销毁 | `IR_CONTRACTS` 要求只求值一次和分支绑定，但 `SEMANTICS:85` 未明确 switch 临时的 full-expression 结束点。先取得模式 copy 后立即销毁，或保留到分支结束，会改变可观察 drop 顺序 | 第一轮支持借用简单绑定并 copy 模式，计算产生的拥有 scrutinee 暂拒绝；root 请用户补充销毁时点后再开放，不能由两个后端各选 |
| OOM/分配大小溢出的源级行为 | 已规定 retain 溢出 Panic，但未规定分配失败是 Panic、某种 throws 或其他不可恢复报告；影响 allocate_vec 和失败注入预期 | 不造错误类型/方法，不把宿主异常直接公开；相关源运行验收 not_run，内部资源回收测试单列 |
| 索引赋值目标检查时点，且 RHS 会改变同一 Vec 长度 | `SEMANTICS:78` 固定目标先求值一次，但未固定越界检查在 RHS 前还是写入前、以及 RHS 改长度后的结果 | 第一轮无公开长度修改方法，不需猜此组合；保留句柄+索引，不保存裸地址。开放长度修改前请用户定案；负索引和每次实际访问前的安全检查不省略 |

let 不可重新赋值是已有规则，暂不把“move 后重建 let”宣称可用；若用户希望特例，再一起确认。for-in 元素绑定重新赋值也不在第一轮开放，先按只读拥有绑定实施该受限子集，不宣称这是永久语言规则。copy/drop 的私有函数名、非原子计数、内部暂存表示、对象头字段位置是内部实现选择，无需用户设计；它们不能改变表中已冻结行为。

## 10. 实施先后及交接

这是后续分工建议，不是本次派工或 contracts 修改。公共头/共享检查始终只有 root 指定的一个编辑者，避免 sema 与后端同时改 `hir.hpp/hir.cpp/native.hpp`。

| 顺序 | 负责人及修改范围建议 | 可交给下一方的结果与必须验证的事项 |
| --- | --- | --- |
| 0 | root：确认本提案的记录、初期类型组合、待问项；集中发布 include/lang/hir、include/lang/ir 追加项 | 明确同一 compiler-dev/1 变更集的生产者/消费者；旧 ID、旧平凡手工 HIR、Verified 快照不变；未完成项保持 E_CAP |
| 1 | sema：`src/sema/check.cpp`；共享验证修改由指定编辑者执行 | Vec 类型与最小节点、ValueOps、拥有状态、临时前缀、分支/循环/try/defer、普通借用返回 copy；真实 CLI 正反例和坏 HIR 拒绝。此时未接两端的能力不能发布为可生成 |
| 2N | Native：`src/ir/{lower,verify,obligations,layout}`、`src/backend/native`、`runtime/native` | 先 Vec[平凡值]，真实 retain/release 和输出通道；独立义务检查拒绝漏调用、漏释放及伪标记；实际编译/链接/O0/O2 运行 |
| 2P | Python：`src/backend/python`、`runtime/python` | 相同 HIR 记录、Vec 共享和明确语言 drop、Panic 隔离、runtime 版本匹配；实际生成文件执行，不靠 runtime 手写对照代替 |
| 3 | sema + 两端，逐项启用拥有 struct/enum、嵌套 Vec、普通泛型与模式/catch | 按实际接入的类型组合开放 capability；源字段、元素、临时值部分失败及逆序销毁、错误活动侧均实际观察。POD 整体 inout 另补递归分类和两端实现，不顺带开放泛型/方法 inout |
| 4 | validation/root：使用测试负责人的 ownership 项目及同源比较 | 检查原 19 cleanup、27 generic、76 float 输入与预期未改；旧标量/浮点/T08 检查通过；release 计数和异常次序有独立预期；分别记录 passed/failed/skipped/not_run/unsupported |

2N/2P 只有在步骤 0/1 的共享记录可编译、真实 HIR 已验证后才适合并行。步骤 3 可先实现局部逻辑，不能以 mock 或“依赖已完成”的消息代替实际构建测试。T07 泛型 trait、trait 泛型方法、嵌套变体模式、泛型/方法 move/inout 仍独立未支持；完成本提案或 Vec[平凡值] 不等于完整 02_generics 已运行，更不等于 T09 done。

交接外层目录：`/tmp/language-t09-ownership-proposal-20261104/`，其中 `report.md`、`summary.json` 是本次实际结果，`probes.json` 是逐次 CLI 记录，`environment.json` 和构建日志说明二进制来源。T09 正式实现、计数观察和跨后端执行仍待上述后续任务完成。
