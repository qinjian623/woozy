# T09 当前执行范围

native/64 Linux x86_64 与 python/64 CPython 3.12.15 已支持有限、无环的 Vec 和含 Vec 的 struct/enum：共享复制、整体 move、普通参数借用和返回复制、索引读写、切片、for-in，以及 return/typed-error/defer 的拥有值清理。Python runtime 为 `compiler-dev/1-python-values/4`；native 保留标量 ABI，新增私有 `lang-native-values/1`。

仍不支持动态 string/Vec[string]、部分 move、拥有值或 foreign 的 inout、索引 inout、索引字段写回、递归拥有图、捕获闭包、foreign 及原 T07 限制。运行时对顶层数字/bool 和有限平凡 struct/enum 的整体 inout 分别用标量槽和聚合存储处理，详见下文。普通参数绑定只读、计算产生的拥有 switch 主体保留到分支结束，这两项规则已在 SEMANTICS 中确定，代码仍待补齐。没有新增公开 len/push/share API。完整 ARC 和外部对象支持尚未完成。

Vec 别名共享写入；切片创建新容器并逻辑复制元素，引用元素继续共享。for-in 持有一次求值的旧集合，使用入口长度，每轮取得当前元素的逻辑副本。元素替换可见；活动迭代中的长度改变必须在修改前 Panic，此项目前只由私有 runtime 检查。

分配失败、分配大小或引用计数溢出、非法索引和切片均 Panic，不能被 typed catch 捕获。Panic 不保证语言清理。普通和 typed-error 结束才检查零存活、最后释放及次序；Python 不依赖 `__del__` 或宿主 GC 完成语言释放。

默认 Linux x86_64 原生程序使用自有入口和直接系统调用；无 libc 运行支持与索引优化见 [NATIVE_RUNTIME.md](NATIVE_RUNTIME.md)。下表保留 T09 首次开放时的检查记录，后续检查以项目外层最新报告为准。

## 平凡聚合整体 inout 的执行范围

修正版保留同一 nominal 声明下具体实参严格缩小的有限嵌套（如 `Box[Box[i32]]`），并把函数签名回指与值布局环分开；固定 POD callback 数据不再因与 impl Self 具体类型相同而被误判为 Self 依赖；`Wrap[Self]` 在真实 Self 为 POD 时按实际替换字段检查，拥有字段仍在 Source 拒绝。真实 Self callback 的数字/bool 阶段限制和 actual-self/generic 依赖 callback 的既有拒绝未放宽。

有限平凡 struct/enum 的整体 inout 已在 native/64 和 Python/64 实际执行。聚合只含数字/bool、Unit、非捕获函数值、空 struct、无载荷 enum，或递归由这些组成的字段/载荷；Vec、string、foreign、拥有字段和无限布局继续拒绝。Native 的 inout ABI 是调用者存储指针，整体赋值、字段修改和 enum 变体替换都写回该地址；Python 用 Cell 包装 writable 绑定，普通读和返回仍取得不可变 tuple 的逻辑副本，不把 Cell 当值复制。

调用方仍必须是整体可写绑定；let、普通参数、临时值、字段和索引被拒绝。重复/嵌套借用及同绑定的其他实参读取、move 或子字段访问继续拒绝。callee 可以整体替换、改活动 enum 变体、修改值字段、转交给已有 inout 参数；后参抛错不进入 callee，callee 的 typed throw 保留写回，return 在 defer 前保存。`tests/generics/aggregate-inout/` 和 `tests/generics/run_aggregate_inout.py` 覆盖这些行为、拒绝构建、Native O0/O2 和 Python 执行，完整 CTest 结果见本次交付日志。

## NativeIR 的两处修正

独立检查发现：两个拥有读取共用一份实际复制结果，原验证器仍可能放行；正常生成不这样做，但手改 IR 的程序在 ASan 下确认发生释放后使用。主代理补充值创建、store、take、构造、调用和返回间的拥有状态检查。一个逻辑拥有值只能实际转交一次；遗留的未转交复制也会被拒绝。检查读取实际操作和 CFG，不从可改元数据推断复制已发生。

另有三种合法源码会误报内部错误：`if false`、`if true … else` 和 `false && …` 的不执行分支含 move，之后读取原绑定。拥有状态现在只沿已验证 bool 常量实际选择的后继传播；其他结构、类型、调用和清理检查仍保留。新增源码和两个损坏 IR 测试覆盖这些情况。

## 最终检查

| 构建 | 完整 CTest | 跳过 |
| --- | --- | --- |
| GNU Debug、LLVM ON | 40/40 | 0 |
| GNU Release、LLVM ON | 40/40 | 0 |
| Clang22 ASan/UBSan、LLVM ON | 40/40 | 0 |
| GNU Debug、LLVM OFF | 30/30 | 0 |

每个 LLVM 构建含 Native 86 项源码检查（70 次实际执行、14 次静态拒绝、2 次无限清理仅构建），6 项私有 runtime 检查和 714 次坏 IR 拒绝调用，后者包含 O0/O2 重复。每个构建含 Python 43 项源码检查、12 项 runtime 检查及 8 类坏 HIR 检查。旧的 Vec/move 拒绝断言已改为实际支持检查；原源码和数值预期未变，旧失败日志保留。

80 份同源输入在 GNU Debug/Release 和 Clang sanitizer 编译器下共 240 项比较通过。每项核对 native O0/O2 与 Python 的普通结束、Panic、typed-error、静态拒绝或仅构建分类；两个无限清理输入未执行。比较程序自身曾给 Python 错传 `--opt`，修正调用后复用已成功的 native 记录并完成 Python 检查，没有把该参数错误当产品错误。

原版 `02_generics.lang`、`03_errors.lang`、`06_control_flow.lang` 未改，三种编译器构建的 native O0/O2 与 Python 共 27 次实际执行通过。

生成的 Native 程序另有 70 次 ASan/UBSan 执行及 6 项插桩私有 runtime 检查通过。独立运行库检查者以不同回调另做 8 种组合、O0/O2/ASan 共 24 次检查通过。前端 sanitizer 不能代替生成程序 sanitizer；预编译 LLVM 和 CPython 未被本项目插桩。主要检查关闭 LeakSanitizer，不能据此声称完整泄漏检测；普通结束的对象存活和释放观察另行检查。

实际命令、退出码、源码/生成文件 hash、原失败记录与主代理报告在项目外层 `checks/t09-combined-root/` 和 `checks/t09-native-root/initial/`。两份独立交付记录见 `T09_NATIVE.md`、`T09_PYTHON.md`；其中临时阶段的“尚未开放”是历史结果。

## 使用示例

使用已启用 LLVM 的主目录构建，执行共享写入例子：

```sh
build/native-cli/langc build --project tests/ownership/fixtures/vec-shared-index/lang.toml --backend native --opt 2 --out-dir build/vec-native --format json
build/vec-native/program
build/native-cli/langc build --project tests/ownership/fixtures/vec-shared-index/lang.toml --backend python --out-dir build/vec-python --format json
python3.12 -s build/vec-python/program.py
```

两端应返回 0。Python 构建不接受 `--opt`；检查时不执行 Python，运行生成程序需 CPython 3.12。
