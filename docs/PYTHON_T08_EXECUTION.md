# Python typed throws、catch 与 defer 执行

契约仍为 `compiler-dev/1`。T08引入的运行库版本为`/2`，当前版本为
`compiler-dev/1-python-values/6`；公共头、生成代码的版本检查和嵌入的
`langrt.py` 使用同一版本。旧 runtime 无法运行新生成的程序。

## 生成行为

`src/backend/python/emitter.cpp` 仅接受真实 `VerifiedHirModule`。
每个表达式、提前出口和块尾都会核对 HIR `ExitPlan` 中应执行的 defer：
按 `exited_scopes` 从活跃注册中取出相应清理，并比较执行顺序。
代码生成继续使用公共 ScopeId、SymbolId、TypeId 和 HIR，不另建语言作用域模型。

含 defer 的词法块生成一个私有清理函数、一个 with 清理作用域，以及从零开始的注册计数。
同块的直接注册按语句顺序经过，活跃注册总是这个序列的一段前缀；每经过一个
注册点才把计数更新到该序号。退出作用域时按逆序逐项检查计数，执行实际注册的清理。
平铺增加注册不会增加 Python 的静态控制块深度，空清理也采用相同表示。
嵌套块和循环每轮分别初始化自己的计数。

清理函数通过 Python 闭包读取执行时的原绑定，写入沿用已有 Cell/Field；计数不保存源变量的值。
未经过的注册不会执行。return/throw 和调用参数先求值
到临时变量，随后才会发生清理。不可变 tuple 与 Cell/Field 保持已有行为。

三段式 for 和 for-in 使用 Python 的 continue。三段式 for 首轮跳过 step，之后每轮先执行 step 再检查条件，
因此 continue 先完成本轮清理，再执行下一轮 step；break、return 和 throw 不执行 step。
for-in 同样在下一轮开始时增加下标。switch 继续使用继承 BaseException 的独立 `SwitchBreak`。

生成模块内的私有 `_LangCleanup.__exit__` 检查该作用域实际离开的异常；遇到 `langrt.Panic` 就跳过清理。
清理自身 Panic 会立即中止同块剩余清理，外层清理也跳过。
宿主调用者正在处理另一个 Panic，不影响函数自身的正常清理。
正常返回、typed error、break/continue 均执行实际离开块的清理。

`langrt.TypedError(type_key, value)` 保存语言错误类型和完整载荷。
catch 只捕获 TypedError，再按 type_key 精确匹配；未匹配时原样重新抛出。
handler 的代码位于 except 内，handler 自己抛出的错误进入外层 try。
Panic、SwitchBreak、KeyboardInterrupt、SystemExit 和普通
宿主异常都不会进入 typed handler。

## 类型键与入口

类型键使用现有逻辑模块名和声明名，后接具体泛型实参；不使用会话数字 ID、
绝对源码目录或内存地址。例如 `app.E`、`app/a.E`、`app.Err[i32]`。
泛型实参用 `[...]` 和无空格逗号分隔，递归采用相同表示。
基础类型保留 `i32`、`u64`、`bool`、`Unit`、`isize` 等源类型名。
函数类型表示为 `func(T,inout U)->(R)`，有错误时追加 ` throws(E)`；R/E 递归
使用同一表示。返回类型和错误类型均有分组。例如：

```text
func() (func() throws E)   -> func()->(func()->(Unit) throws(app.E))
func() (func()) throws E  -> func()->(func()->(Unit)) throws(app.E)
```

它们作为 `Err[T]` 的实参或直接错误类型时也保持不同身份。
这只定义当前生成代码的逻辑错误身份，不承诺跨版本私有 ABI。

零参数、返回 i32/Unit 的 main 可以声明 typed throws。错误离开入口时：

```text
退出码 1
stderr: LANG_TYPED_ERROR: <type-key>\n
```

Panic 继续输出 `LANG_PANIC: <reason>` 并退出 101。正常 main 返回 7/101
仍是普通返回，不打印上述 marker。`Options{false}` 保留可导入、无 runner 的 API。

## 测试与集成

`tests/python/run_cleanup.py` 从真实项目执行 load/resolve/check/verify/emit，
逐一运行原 19 项：11 项接受并实际执行，8 项按原诊断拒绝，反例的运行记为
`not_run (source rejection)`。源码和预期直接读取原 cases.json，不改原项目。

`observe_cleanup.py` 在独立 Python 进程中导入未经改写的生成模块，用函数观察器
记录实际 mark/event 调用，用 trace 记录局部绑定及 TypedError 载荷。
源函数仍真实执行。它检查 case-01/02 返回 7/4、case-08 成功返回 4 与错误 -1、
case-09 的 found、原错误样例 cleaned 的实际变化 1/2；case-10 另用无 runner
生成结果调用 probe，捕获 Panic 后读取 Cell=12。

另外 18 项覆盖返回/throw 表达式失败、泛型同型/异型 catch、具体泛型枚举、
函数值、递归、跨模块同名错误、POD 返回值与清理字段写入、step 失败、短路、
主体和清理自身 Panic、不同 BaseException 控制事件、未处理错误 marker、
普通入口返回及 TypeId 改号后的稳定键。编译子进程的 PATH 放置会失败的 Python
启动器；运行阶段则单独调用锁定的 Python 3.12.15。

`python.cleanup_regressions` 保留 root 提供的原碰撞源码字节及数值预期，另检查
反向/直接函数错误、参数与 throws 内的嵌套函数、inout 参数和十组 TypeId 改号。
平铺写入清理检查 0/1/8/9/10/11/20/21/22/64/256 次，以及根代理的两份 21 次
源码；另观察 21 次 LIFO、部分注册、continue、主体 Panic 与清理自身 Panic。
每个接受的程序均先由锁定 CPython 编译完整生成文件，再运行并读取实际值。

## 生成深度限制

平铺注册数量不再转换成静态嵌套深度。源程序本身的多层 loop/try/defer 仍会增加
生成深度。emitter 按锁定 CPython 3.12.15 的静态控制栈检查生成树，上限为
`CO_MAXBLOCKS = 21`。while 和 with 体各占 1；try/except 的 try 体占 1，handler 体占 2；
try/finally 的 try 体占 1，finally 在异常或保留返回值时的执行体也占 1。
组合 try/except/finally 按相邻分支一起计算，不能把各分支的代价重复累加。
每个 def 重新计数，普通 if/elif/else 不增加计数；生成文本缩进上限仍为 80 层。
编译器只检查自己的生成结构，不启动 Python 来判断能否生成。

原生成方式通过时保留原来的全部字节。只有打印生成树明确报告控制块或缩进超限，
才用新的 emitter 重试；重试也只拆分超过限制的 Source 函数。完整的 HIR
block、if、loop、switch、try 节点生成模块级同步私有函数，节点自己的
try/except/finally 保持完整。表达式递归达到 16 层时也生成同步私有函数，
返回原来计算出的结果，不额外复制、取走或清理拥有值。

私有函数只接收 HIR 实际引用且已经绑定的 SymbolId，以及共享 `_frame`；
Cell、Owner 和普通参数原样传入。跨私有函数的 Source return、loop
break/continue 由生成模块内的私有 BaseException 传递，所属 Source 函数或
loop 接住后继续原来的退出顺序。返回值先取走再执行清理；Panic 仍跳过语言清理；
for-in 的 continue 不调用集合 end，其他退出先 end 再释放集合。SourceMap 根据
最终生成行号重新记录原 Source span，公共 HIR 和 langrt API 未增加字段或接口。

每个生成的 Python code object 仍检查上述 21/80 限制。重试另限制最多 200000
个私有函数和 2000000 次 HIR 捕获遍历，并在遍历、生成和打印时检查取消。
超过生成资源预算时，emit/build 在发布文件前返回 `E_CAPABILITY_UNAVAILABLE`，
不创建输出目录，也不改已有输出。公共后端支持检查不复制 Python 私有生成树，
所以 `check` 通过后 `build` 仍可能报告生成资源不足。同步调用仍使用 Python
调用栈；这里没有取消前端、内存、调用栈等总资源限制，也不声称无限嵌套可运行。

此前被保守计数拒绝的四层/五层 try/defer、五层循环和循环内三层 try/defer，
现保留原 Source 并实际编译、运行。原二十二层普通循环、二十一层循环加清理也
保留原 Source，改为完整编译并运行；新增四十八层循环及四种退出、二十四层
未进入分支、递归 Vec 替换、入口绑定捕获、拥有值返回与深表达式、computed owning
switch 错误、Panic 和九十六层短路。测试检查固定事件、迭代 begin/end、Vec
最终释放及正常、类型错误退出的 Frame 槽清空；原 63 个接受项目的
program.py/langrt.py 逐字节保持。
五层循环与 try/defer 混合的 48 个固定事件继续检查。所有接受项先编译整个生成
模块，再运行；未进入的分支也必须是有效 Python。

原 19 项、原泛型输入及清单、baseline、floating fixtures 在运行前后比较 SHA256。
原 Python/native throws/defer 拒绝项改由两端执行测试检查。
其他未支持项继续检查拒绝。重复运行拒绝测试时，每次使用独立的新输出目录，
再向该目录写入旧文件并检查失败 build 保留全部字节。

共享 HIR 后端支持检查现已启用 Python 和 native T08。
`python.cleanup` 和 `python.cleanup_regressions` 已加入测试。执行：

```sh
cmake --build <gnu-debug-build>
ctest --test-dir <gnu-debug-build> --output-on-failure
cmake --build <gnu-release-build>
ctest --test-dir <gnu-release-build> --output-on-failure
```

动态 ARC/Vec、foreign 及泛型直接 inout 的当前范围见 [SEMANTICS.md](SEMANTICS.md)、
[GENERICS.md](GENERICS.md) 与 [T10_PYTHON.md](T10_PYTHON.md)。源语言捕获闭包仍延后；
生成器内部的清理函数不开放该语法。
严格浮点现已实现，见[浮点说明](../tests/python/STRICT_FLOATING.md)。
原生说明见[Native T08](../tests/native/T08_EXECUTION.md)。两端共同使用稳定错误类型键，
原生O0/O2及Python3.12.15的同源执行另有记录。
