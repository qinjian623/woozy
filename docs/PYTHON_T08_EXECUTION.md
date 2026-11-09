# Python typed throws、catch 与 defer 执行

契约仍为 `compiler-dev/1`。T08引入的运行库版本为`/2`，当前严格浮点版本为
`compiler-dev/1-python-values/3`；公共头、生成代码的版本检查和嵌入的
`langrt.py` 使用同一版本。旧 runtime 无法运行新生成的程序。

## 生成行为

`src/backend/python/emitter.cpp` 仅接受真实 `VerifiedHirModule`。
每个表达式、提前出口和块尾都会核对 HIR `ExitPlan` 中应执行的 defer：
按 `exited_scopes` 从活跃注册中取出相应清理，并比较执行顺序。
代码生成继续使用公共 ScopeId、SymbolId、TypeId 和 HIR，不另建语言作用域模型。

含 defer 的词法块只生成一组 try/except/finally，以及一个从零开始的注册计数。
同块的直接注册按语句顺序经过，活跃注册总是这个序列的一段前缀；每经过一个
注册点才把计数更新到该序号。finally 按逆序逐项检查计数，执行实际注册的清理。
平铺增加注册不会增加 Python 的静态控制块深度，空清理也采用相同表示。
嵌套块和循环每轮分别初始化自己的计数。

清理体直接生成在 finally 中，读取执行时的原绑定；计数不保存源变量的值，
也不创建 Python 闭包。未经过的注册不会执行。return/throw 和调用参数先求值
到临时变量，随后才会发生清理。不可变 tuple 与 Cell/Field 保持已有行为。

三段式 for 的 continue 抛出内部 `LoopContinue(target)`，只由对应循环接收。
循环体的 finally 先执行，再执行 step。break 仍退出实际对应的循环；switch
继续使用独立 `SwitchBreak`。两种控制事件都继承 BaseException。

每个含 defer 的块单独捕获 `langrt.Panic`，禁止该块执行清理后重新抛出。
清理自身 Panic 会立即中止同块剩余清理；外层块也会捕获它并跳过外层清理。
正常返回、typed error、break/continue 均执行实际离开块的清理。

`langrt.TypedError(type_key, value)` 保存语言错误类型和完整载荷。
catch 只捕获 TypedError，再按 type_key 精确匹配；未匹配时原样重新抛出。
handler 的代码位于 except 内，handler 自己抛出的错误进入外层 try。
Panic、SwitchBreak、LoopContinue、KeyboardInterrupt、SystemExit 和普通
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
生成深度。当前 emitter 按锁定 CPython 3.12.15 的静态控制栈检查生成树，上限为
`CO_MAXBLOCKS = 21`。while 体占 1；try/except 的 try 体占 1，handler 体占 2；
try/finally 的 try 体占 1，finally 在异常或保留返回值时的执行体也占 1。
组合 try/except/finally 按相邻分支一起计算，不能把各分支的代价重复累加。
每个 def 重新计数，普通 if/elif/else 不增加计数；生成文本缩进上限仍为 80 层。
编译器只检查自己的生成结构，不启动 Python 来判断能否生成。

超过该生成范围时 emit/build 在发布文件前返回 `E_CAPABILITY_UNAVAILABLE`，
不创建输出目录，也不改已有输出。该检查依赖 Python 私有生成树，公共 HIR 的
后端支持检查只确认类型和操作，因此当前 `check` 可通过、`build` 再明确拒绝。
不在公共前端复制 Python 的生成结构。若扩展 CLI check 的生成前检查，应使用同一 emitter，避免重复计算生成深度。

此前被保守计数拒绝的四层/五层 try/defer、五层循环和循环内三层 try/defer，
现保留原 Source 并实际编译、运行。测试还包括目标上限附近的十层循环接受项与
十一层循环拒绝项；拒绝项记录为 `unsupported`、运行 `not_run`，同时检查新目录
和已有输出保留。该目标限制不改变源语言语法或清理规则，也不代表任意嵌套已支持。

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

动态 ARC/Vec、foreign、捕获闭包及泛型直接 inout 调用仍未支持。
严格浮点现已实现，见[浮点说明](../tests/python/STRICT_FLOATING.md)。
原生说明见[Native T08](../tests/native/T08_EXECUTION.md)。两端共同使用稳定错误类型键，
原生O0/O2及Python3.12.15的同源执行另有记录。
