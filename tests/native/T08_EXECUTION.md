# T08 native 执行

契约 `compiler-dev/1`，起点 `805ed72d9cd2fa9b7f2602ed8729a3d5b930b715`。
本目录实现 native 部分。共享支持判断及最终主分支验证由 root 完成，T08 整项仍为
`in_progress`；没有修改主分支新增的 f32 sema 或测试。

`ir::lower` 只接收真实 `VerifiedHirModule`，使用具体函数实例和各出口的共享
`ExitPlan`。Return/Throw/失败调用先保存 SSA 值，再按计划执行实际经过注册的 defer。
正常块尾、return、break、continue、throw、调用失败都生成清理；handler 自身的错误
按自身计划进入外层。defer 读取执行时的外部 slot，POD 返回值和错误载荷保持值复制。

同一 defer 可以从多个出口展开。每份展开保存并恢复 slots、loops 和 handlers 的
降低上下文，局部绑定、局部 catch 与循环得到各自的新 slot/CFG；外部绑定继续使用
原 slot。嵌套 defer 恢复其父展开的上下文。返回值先保存，不会因清理修改绑定而变化。

旧 Op 0..17、Exit 0..4 编号保持；新增 TryCall=5、ErrorReturn=6 和三个清理标记。
TryCall 的成功/失败块各有一个参数，记录原调用 HirNodeId 与侧别，Unit 也有逻辑参数。
两个后继必须不同，分别只有当前调用这一条前驱。参数只在自身块及被其支配的块中
可用；正常值与错误值类型相同时也不能交换两侧。普通 Call 必须无 throws，并保存
可核对的 HIR 调用身份。

一个 HIR Try 可以有多份 native Handler。每份都记录 protected_entry、handler block
和 catch slot；三者是实际 CFG/存储身份，不按源 Try 合并。verifier 检查源 Catch 的
类型、唯一 slot/handler block、单一入口前驱上的原 Try 身份和 SourceSpan，以及错误
出口是否被对应 protected_entry 支配。出口只能写入该份 handler 的 slot 并跳入其块。
NIR dump 同时打印这些记录。

NativeIR verifier 独立检查签名、错误类型、参数模式、CFG 目标、SSA 支配、slot 初始化、
catch 绑定与清理顺序。CFG 数据流传递待完成的 ExitPlan 栈，检查 begin/end、保存值、
嵌套清理和合流状态；清理体内指令的 SourceSpan 必须属于对应 HIR defer body。

这些检查还必须能发现整组标记缺失。`src/ir/obligations.hpp` 从不可变 HIR 的结构化
控制流独立计算应出现的出口和调用，不读取 native 的 origin 或清理标记，也不调用
Lower。按未优化 CFG 保留条件和调用的可能后继，排除 return/throw 后的不可达语句及
没有错误入口的 handler。每个必需事件带着“哪个外层出口展开哪个 defer”的完整上下文，
与 CFG 数据流观察到的事件逐份比较；不能删掉一个展开的调用，再挪到另一份凑总数。
因此同时删除 Unit 自然块尾的实际清理、标记和 origin 仍返回 `E_IR_INVALID`。检查只
约束这些控制/调用义务，不要求所有算术指令与 HIR 逐项等价。清理标记不生成机器指令，
也不承担 ARC 或对象销毁。

LLVM 对 throws 函数使用 `f(args..., out_value, out_error) -> i1`，Unit 省略 out_value。
输出存储在入口分配；成功块只加载 value，失败块只加载 error，没有先读两侧再 select。
普通调用、函数值、递归、跨模块和具体泛型共用此函数类型降低。Result.Err 保持普通
数据。原整数 Panic 检查与非 throws ABI 不变；runtime 原 `/1` 接口保留，另有
`lang_native_typed_error_v1`。

入口未处理的错误输出单行 `LANG_TYPED_ERROR: <key>\n`，退出 1，不打印 Panic 标记。
名义类型 key 为 `<logical_module>.<declaration>[<args>]`，如 `app.E`、
`app.Err[i32]`、`app/errors.Err[i32]`；内置类型使用 `i32`、`string` 等源名字。
函数类型为 `func(T,inout U)->(R)`，有错误时追加 ` throws(E)`，R/E 递归使用同一格式。
例如 `func()->(func()->(Unit) throws(app.E))` 与
`func()->(func()->(Unit)) throws(app.E)` 分别表示内层和外层 throws，不能相同。
没有 session 数字。正常 throws main 返回 7/101 仍按普通退出处理。

`native.cleanup` 不修改原 19 个项目或 cases.json：11 个正例经正式 CLI 构建并实际
执行 O0/O2，8 个反例检查 check/build 诊断及旧输出保留。另有 33 个 native 项目，包含
初版的 18 个和本次 15 个对照。两种完整构建各运行 88 个 CLI ELF 及 88 个观察 ELF，
实际读取 264 条记录，进行 1038 次坏 IR 拒绝检查；其中同一坏 IR 对照在 O0/O2 重复执行。
原 58 次执行的源码 hash、观察记录和退出/输出与初版逐项相同。

`cleanup_probe.cpp` 走真实 load/resolve/check_project/lower/emit，确认其目标文件与 CLI
program.o 逐字节相同。观察器解析实际优化前 LLVM，在生成的 mark/probe/f/read_value/main
返回点添加无缓冲 write，再分别用 Clang O0/O2 编译运行。旧 mark 记录实际 digit/log；
单参数 mark 记录一次增量事件及实际 log；多出口 f 直接记录返回值或成功位及实际 log。
它没有改写源程序，也没有 C++ 参考实现。case-01/02 的 probe 返回、errors-original 的
cleaned/成功位和成功侧值均实际读取；捕获的错误载荷也由原 source 检查。

case-10 原程序检查 division-by-zero、原第 12 行及 exit101；受控观察器读取实际事件
1、2 和 log12，确认之后没有 9 或正常返回。panic-body、panic-defer 是独立对照。
没有声称终止后读到原程序的局部内存。

本次新增反例同时删除 Unit 自然块尾/显式 return/嵌套块的整组清理及 origin，另检查
保留标记而删除实际调用、调用移到另一份展开、错误的 Handler 实例和成功/失败侧。
合法无 defer Unit 程序仍通过。多出口局部绑定、catch、循环和返回表达式失败有直接
返回/log 观察；高阶函数 key 的两个源程序在 O0/O2 输出不同 marker，新增具体函数
使错误 TypeId 从 6 变为 8 后 marker 仍相同。

主代理正式合入两端支持后，GNU Debug、GNU Release 和 Clang22 ASan/UBSan 完整
测试各32/32，关闭LLVM25/25。每套native.cleanup运行92个CLI程序和92个受控观察
程序，读取274条真实记录，进行1122次坏IR拒绝检查（含O0/O2重复）。新增两个defer
内穷尽枚举switch项目，保留原19输入和所有旧预期。

主代理另修正穷尽enum switch默认不可达块的清理误拒绝。既有enum_test事实同时记录
同一SSA值各变体的真、假分支；事实经所有前驱交集，完整类型检查后确认全部真实变体
都已排除，才允许该不可达出口保留外层清理状态。该块中的Call/Exit不计入必须执行
次数；源HIR的独立统计也跳过穷尽变体后的不可达default。重复标签不能代替完整覆盖。
新增损坏测试包括活分支伪造unreachable、遗漏变体、把调用连同依赖移到不可能的默认块，
均须拒绝；不允许通过添加不可达调用抵算实际清理。

共享后端检查已启用两端T08；静态检查正例现要求真实VerifiedHIR。sema.scalar仅更新
两个过时throws/defer拒绝预期，保留141项f32检查。旧字符串拒绝表中的这两条也删除，
执行由两端cleanup测试覆盖。没有改根CMake、原fixture清单或数值预期。

动态字符串、ARC/Vec、foreign、严格浮点运算和generic inout/move仍未支持。
ASan/UBSan检查编译器；受控观察程序没有启用sanitizer，不含LeakSanitizer结论。
