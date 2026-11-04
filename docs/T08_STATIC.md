# T08 静态类型、错误效果与 defer

本轮实现 `compiler-dev/1` 的静态部分。`sema::check_types` 从真实项目生成 HIR，
`hir::verify_types` 独立检查类型、错误与清理记录。两者不授予代码生成权限。
两端T08执行现已接入，见[原生说明](../tests/native/T08_EXECUTION.md)和
[Python说明](PYTHON_T08_EXECUTION.md)。下文保留静态阶段的接口和验证记录；
静态检查本身仍不证明清理事件、错误载荷或退出码。

## 公共接口

| 文件 | 新增或调整的对象 | 使用方式 |
| --- | --- | --- |
| `include/lang/syntax/ast.hpp` | `SignatureSyntax`、`split_signature(file, node)` | 只拆当前函数的直接参数、返回和 throws 类型；不遍历这些类型内部的 throws。声明、函数字面量和 FunctionType 共用。 |
| `include/lang/hir/hir.hpp` | `Catch` | 错误 TypeId、真实 Catch 声明的 SymbolId、ScopeId、body 和 Source；不是模式绑定。 |
| 同上 | `Try`、`Defer`、`Throw` | 追加 NodeKind 30/31/32，旧编号不变。Try 的 children 为 protected body 后接各 handler；Defer/Throw 各一个子节点。 |
| 同上 | `HirNode.errors` | 当前子树可能向外产生的 typed error，按 TypeId 排序去重。Try 只从 protected body 去除已捕获类型，再合并 handler 错误。Defer 向外的集合为空。 |
| 同上 | `HirNode.exits`、`ExitPlan` | 调用自身失败和 throw 总有 error 计划。其他出口仅在有 defer 要执行时保存计划。子表达式的失败另存于子节点。 |
| 同上 | `ExitTarget` | `ScopeExit` 恢复到父作用域；`FunctionExit` 返回/传播到函数；`JumpExit` 指向 For/Switch；`CatchTarget` 指向 try 节点和 handler 下标。 |
| 同上 | `Defer.owner_scope/references` | 最近词法块和注册时已可见的外部局部符号；references 排序去重，执行时读取，不保存注册时的值。 |

`ExitPlan.exited_scopes` 从内向外列出离开的作用域，不含恢复位置。
`defers` 是按执行顺序列出的注册节点：内块先执行，同块后注册先执行。
普通 return/throw 的值或调用失败的错误须先求值并保存，再清理；计划用
`saves_value` 记录这项要求。return 无表达式时为 false。不存在 defer 的普通出口
由原有 Return/Break/Continue、Block/For 的结构直接决定，`exits` 为空；这避免重复
保存可直接推导且后端无需额外表示的信息，也保留原有手工标量 HIR 的有效性。

计划在静态控制结构上说明某个出口会执行哪些注册。块的正常出口可包含完整语句序列
末尾的 defer，但该正常出口若不可达就不会执行。提前 return、throw、调用失败或跳转
的计划只包含各自位置以前的注册；不能拿块尾正常计划代替提前出口的计划。
For 的 continue 保留其外围作用域并退出本轮 body；break 退出 For 的作用域。
Switch 没有独立作用域，break 退出分支块、恢复到 switch 所在作用域。

## 检查与拒绝

函数值和直接调用遵循相同 throws 类型，错误精确匹配。返回一个 throws 函数值
不使构造函数自身产生 typed error。普通 Result 构造/模式匹配和 Panic 都不增加
错误集合。返回/throw 表达式自己的失败与表达式求值成功后的返回/throw 分开记录。

处理 protected body 的 catch 不处理 handler 自身的新错误。defer 内可以使用自己的
循环、switch、局部 try/catch，以及独立非捕获函数；向外 return/break/continue 或
错误传播为 `E_CLEANUP_ESCAPE`。外层函数的 throws 或外层 catch 不允许 defer 错误
逃出。普通参数重赋值维持原有 `E_CAPABILITY_UNAVAILABLE`。同一 try 重复同类型
catch，以及泛型替换后形成重复类型的 catch，均暂报 `E_CAPABILITY_UNAVAILABLE`。
捕获闭包、move、非平凡 inout、ARC、外部绑定等原有限制保持。

Verifier 不调用 sema 的记录生成函数。它先检查 Source、节点、声明和作用域，再用
独立的后序计算重算错误集合；根据父节点及各块的语句顺序重新查找错误目标、注册点、
退出作用域、defer 顺序及可见引用。源 AST 中的 Try/Catch/Defer/Throw 不得被省略、
重复或替换。含 T08 的函数还逐块检查真实 AST 的语句序列、If 分支、For body、
Switch 分支和 Try/Defer body 的控制归属，接受已有 Case Block 和 else-if Block 的
降低形式。交换 defer、将注册移到 return/throw 前后，或移动到另一分支/handler，
即使同时改对 HIR 内部的清理计划也为 `E_IR_INVALID`。本检查不要求任意表达式与
源码逐项等价，保留括号消除、常量折叠等现有降低。旧标量手工 HIR 检查范围保持。
损坏的 HIR 不会进入后端检查后误报 E_CAP。

泛型复制替换错误类型，并改号 catch 绑定、defer 引用、作用域和循环/try 目标。
完整具体结构建立后重新推导 errors/exits，不机械替换模板的这些记录：例如 A/B 在
替换前不同、替换后相同，原本向函数传播的 throw 会转入局部 catch，只清理实际
离开的块。Verifier 保留实例与模板的源码、节点结构、类型、作用域和符号比较；
对派生的 errors/exits 则依据具体实例独立重算，而非要求等于模板记录的逐字段替换。
正常结束路径另做可达性分析，不用存储的 errors 摘要代替。Try 只合并 protected
body 的可达错误可能进入的 handler 的 falls/breaks/continues；handler 自己的新错误
继续走外层。模板类型按现有有限类型替换的可统一性保守判断，A/B、Err[A]/Err[B]
不能仅因当前 TypeId 不同而判为不匹配。具体实例再做正常出口检查。不可达 handler
仍接受完整的类型/效果/清理合法性检查，但不会制造缺返回或循环可退出的路径。
源码检查和独立 HIR flow 分别计算这些结果，不互相采用检查结果。
左侧为 bool 字面量的 `&&/||` 只将实际可能执行的右侧错误用于 handler 可达性分析；
动态左侧仍保留右侧可能出错的情况。类型和错误记录检查继续覆盖完整源码。

FunctionType 内的 Parameter 只是类型描述，不进入声明符号集合。原有限布局、函数
字段、类型/实例/节点预算、取消和资源限制测试继续执行。

`hir::verify` 仍是唯一 VerifiedHirModule 构造入口，先深复制再检查所选后端。
具体函数的 throws 或 Try/Defer/Throw 由共用 `check_backend_capabilities` 拒绝；
未实例化模板维持原有检查范围。没有新增 Native IR 指令，没有改变 runtime。

## 验证入口和交接

根 CMake 已接入 `tests/cleanup`。独立入口也会包含原工程全部测试及 `sema.t08`：

```sh
cmake -S tests/cleanup -B <build-dir> -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLANG_ENABLE_LLVM=OFF
cmake --build <build-dir>
ctest --test-dir <build-dir> --output-on-failure
<build-dir>/lang_cleanup_static_test --types tests/cleanup/fixtures/case-08/lang.toml
```

完整 GNU Debug/Release 使用现有 LLVM 22.1.8、锁定的 Python 3.12.15 和原工程相同
工具链选项。CLI 行为保持原有后端拒绝，contracts 中 T08 仍未完成。
`tests/cleanup/cases.json` 及全部 19 个项目不变；原 27 个泛型输入
不变；`tests/floating` 由另一实现者维护，本轮不修改。

`static_test.cpp` 对全部 19 项分别执行 native/Python 配置的真实 load/resolve/check_types，
正例再独立 verify_types，并确认后端检查拒绝。额外检查函数类型的内外层 throws、
handler 错误、defer 内部/外部跳转、嵌套函数、Result/Panic、泛型错误替换与身份。
一份带真实源码的 HIR 完全由测试手工构造，不调用 sema；损坏该输入及真实 fixture
HIR，检查错误目标、清理次序、退出作用域、值保存、catch、可见引用和实例来源。
未实例化模板的发布测试确认新增记录也不受可变 builder 别名影响。

本轮没有修改旧拒绝断言。旧 T04 的 throws/defer E_CAP 断言通过 `check_project`
检查后端支持，仍有效；它们的通过不代表这些语法在 `check_types` 中仍被拒绝。
机器相关的源码摘要、命令、构建/测试日志、逐项静态结果、CLI check/build 的
unsupported/source-error 和运行 not_run 记录保存在工作区外。ASan/UBSan 由 root
合入前执行；两个后端执行和 A06 运行事件比较由后续任务完成，不能标记 T08 整项 done。

本轮坏 HIR 检查曾复现一处漏检：实例的作用域 Source 被替换成同一函数内的其他
Source，原有“位于函数内部”的检查仍接受。新增 `instance-scope-source` 探针，并让
实例检查同时比较模板作用域的 NodeRef、snapshot 和字节区间，现可拒绝该输入。
记录和探针均留在本任务文件内，不更改其他项目或共享 skill。

本次唯一修正依据根代理与两份独立审查的真实失败，补了泛型匹配变化、handler
可达性、源语句注册顺序三组对照。保留原 116 个检查，追加同型/异型实例、函数值
和泛型调用、嵌套 handler、可能统一的模板返回路径，以及同步伪造树和计划的坏 HIR。
修前与修后的命令、源码摘要和结果保存在工作区外。本次未
改变公共头文件字段、重复 catch 处理、普通参数可写规则、后端执行或浮点行为。

集成时又补充了字面量短路的 handler 可达性检查。GNU Debug/Release 和 Clang22
ASan/UBSan 各通过全部 29 项 CTest；关闭 LLVM 后通过 23 项。静态检查共 210 项，
包含 60 次坏 HIR 拒绝。独立审查还验证了根代理补充的 12 项短路对照。T08 后端
执行仍未实现；这些结果不表示清理事件或 typed error 已运行通过。
