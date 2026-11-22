# T09 当前执行范围

native/64 Linux x86_64 与 python/64 CPython 3.12.15 已支持 Vec 和含 Vec 的 struct/enum：共享复制、整体 move、普通参数借用和返回复制、索引读写、切片、for-in，以及 return/typed-error/defer 的拥有值清理。经 Vec 回指的有限递归值类型图（包括结构体/枚举互相引用、具体泛型实例和周期交换的具体 TypeId）也按同一套 copy/drop 规则处理；直接按值包含自身的无限布局仍拒绝。Python runtime 为 `compiler-dev/1-python-values/7`；native 保留标量 ABI，新增私有 `lang-native-values/1`。

字符串值已支持复制和整体 move，保留完整 UTF-8 字节和嵌入 NUL。`Vec[string]` 支持元素替换、切片和 for-in。Vec 元素的字段写回也已支持，包括嵌套字段、拥有字段替换，以及右值求值失败时保留原值。相应源码见 `tests/native/ownership/string-vec-aggregates` 和 `indexed-field-*`。

仍不支持部分 move、拥有值或 foreign 的 inout、字段/索引 inout、捕获闭包和循环回收。强引用环保持存活，不自动回收。字符串动态构造和公开 len/push/share API 尚未定义。Python 已声明宿主引用的整体 move、Vec/struct/enum 保存见 [T10_PYTHON.md](T10_PYTHON.md)；`py.Error` 也可作为这些容器的递归成员保存和复制，共享原异常载荷。Native 仍不支持 foreign，包括 `py.Error` 容器。

运行时支持数值/bool 和有限平凡 struct/enum 的整体 inout。普通参数绑定只读，重赋值报 `E_TYPE_MISMATCH`；普通 Vec 参数仍可修改共享容器内容。计算得到的拥有 switch 主体持有到分支结束：先执行分支 defer 和局部清理，再释放主体。typed-error handler 不提前销毁由 finally 持有的主体；Panic 不执行语言清理。

Vec 别名共享写入；切片创建新容器并逻辑复制元素，引用元素继续共享。for-in 持有一次求值的旧集合，使用入口长度，每轮取得当前元素的逻辑副本。元素替换可见；活动迭代中的长度改变必须在修改前 Panic，此项目前只由私有 runtime 检查。

分配失败、分配大小或引用计数溢出、非法索引和切片均 Panic，不能被 typed catch 捕获。Panic 不保证语言清理。普通和 typed-error 结束才检查零存活、最后释放及次序；有意构造的语言强引用环是例外，必须单独观察其语言引用计数保持非零，不能套用零存活要求。Python 不依赖 `__del__` 或宿主 GC 完成语言释放。

默认 Linux x86_64 原生程序使用自有入口和直接系统调用；无 libc 运行支持与索引优化见 [NATIVE_RUNTIME.md](NATIVE_RUNTIME.md)。下表保留 T09 首次开放时的检查记录，后续检查以项目外层最新报告为准。

## 经过 Vec 的递归值

具体成员补全以真实函数/模板签名、调用签名、可达节点、catch 类型和模式类型为根，按精确 TypeId 去重；只有在完整检查后仍被实际使用的具体类型族才会触发预算。每个根最多展开 128 层、访问 4096 个不同具体 TypeId，超限则在触发根的源码位置返回 `E_CAPABILITY_UNAVAILABLE` / unsupported，且不发布输出目录。未使用的增长型声明仍合法；直接按值回指继续由 `infinite_value_layout` 报源码类型错误。泛型模板中仍含真实 TypeParameter 的 Vec 元素不会继续展开具体替换，具体实例会在实例化时重新补全和分类。独立的 ownership verifier 不信任 producer 的完成标记或记录，遇到损坏、不完整或增长型 HIR 时会在分类预算内返回 `E_IR_INVALID`。

支持检查与按值 `needs_drop` 分开计算：Vec 恒为拥有值并在此停止；struct/enum 字段按各声明字段或活动载荷的结果做逻辑或；遇到精确 TypeId 回指时只结束当前遍历，不会把临时的 false 结果缓存为最终结论。名称、类型实参或公开 member TypeId 缺失仍是内部错误。Python 的 Vec `ValueType('vec')` 描述先于元素描述输出，名义值仍先输出字段描述；Native 复用既有 ptr Vec 布局、copy/drop 私有函数和 runtime ABI，没有新增公共 HIR/NativeIR 字段或运行库入口。

持久 Source 在 `tests/ownership/fixtures/recursive-*`，`tests/native/run_ownership.py` 与 `tests/python/run_ownership.py` 支持 `--case` 定向运行。实际 alloc/free 观察使用测试链接的 `--wrap=lang_alloc_v1` / `--wrap=lang_free_v1`，调用真实 allocator 并记录 pointer；事件 5 只表示 Vec 释放路径开始，不单独作为物理内存已释放的证明。自环用例在独立进程/专用 observer 中只记录语言 refs、items 与 frame 清理，不宣称 CPython 物理释放或 GC。

本轮实际运行记录、工具/源码 SHA 与变异清单见项目外层 `checks/recursive-values-flash-20261109/`。下表是 T09 首次开放时的历史记录，不能代替本轮定向证据。

## 平凡聚合整体 inout 的执行范围

修正版允许同一 nominal 声明下具体实参严格缩小的有限嵌套（如 `Box[Box[i32]]`），并把函数签名回指与值布局环区分开；固定 POD callback 数据不再因为与 impl Self 具体类型相同而被误判为 Self 依赖；当真实 Self 为 POD 时，`Wrap[Self]` 按实际替换后的字段检查，含拥有字段的情况仍在 Source 检查阶段拒绝。真实 Self callback 仅限数字/bool 的阶段限制，以及对 actual-self/generic 依赖 callback 的既有拒绝均未放宽。

有限平凡 struct/enum 的整体 inout 已在 native/64 和 Python/64 上实际运行。这类聚合只能包含数字/bool、Unit、非捕获函数值、空 struct、无载荷 enum，或由它们递归组成的字段与载荷；Vec、string、foreign、拥有字段和无限布局继续拒绝。Native 的 inout ABI 传递调用者存储指针，整体赋值、字段修改和 enum 变体替换都直接写回该地址；Python 用 Cell 包装 writable 绑定，普通读取和返回仍得到不可变 tuple 的逻辑副本，不会把 Cell 当作值复制。

调用方仍必须是整体可写绑定；let、普通参数、临时值、字段和索引均拒绝。重复或嵌套借用，以及同一绑定在其他实参中被读取、move 或访问子字段的情况也继续拒绝。callee 可以整体替换、切换活动 enum 变体、修改值字段或转交给其他 inout 参数；后续实参抛错时不会进入 callee，callee 抛出 typed throw 时保留已写回的值，return 结果在 defer 执行前保存。`tests/generics/aggregate-inout/` 和 `tests/generics/run_aggregate_inout.py` 覆盖这些行为、拒绝构建以及 Native O0/O2 和 Python 执行，完整 CTest 结果见本次交付日志。

## NativeIR 的两处修正

独立复核发现，如果两个拥有读取共用同一份实际复制结果，原验证器仍可能放行；正常代码生成不会这样写，但手改 IR 的程序在 ASan 下确认触发了释放后使用。主代理随后补上了值创建、store、take、构造、调用和返回之间的拥有状态检查：一个逻辑拥有值只能实际转交一次，遗留未转交的复制也会被拒绝。该检查直接依据实际操作和 CFG，不从可修改的元数据推断复制是否发生。

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

原版 `02_generics.wz`、`03_errors.wz`、`06_control_flow.wz` 未改，三种编译器构建的 native O0/O2 与 Python 共 27 次实际执行通过。

生成的 Native 程序另有 70 次 ASan/UBSan 执行及 6 项插桩私有 runtime 检查通过。独立运行库复核使用不同回调，另完成了 8 种组合在 O0/O2/ASan 下共 24 次检查并全部通过。前端 sanitizer 不能代替生成程序 sanitizer；预编译 LLVM 和 CPython 未被本项目插桩。主要检查关闭 LeakSanitizer，不能据此声称完整泄漏检测；普通结束的对象存活和释放观察另行检查。

实际命令、退出码、源码/生成文件 hash、原失败记录与主代理报告在项目外层 `checks/t09-combined-root/` 和 `checks/t09-native-root/initial/`。两份独立交付记录见 `T09_NATIVE.md`、`T09_PYTHON.md`；其中临时阶段的“尚未开放”是历史结果。

## 使用示例

使用已启用 LLVM 的主目录构建，执行共享写入例子：

```sh
build/native-cli/woozyc build --project tests/ownership/fixtures/vec-shared-index/lang.toml --backend native --opt 2 --out-dir build/vec-native --format json
build/vec-native/program
build/native-cli/woozyc build --project tests/ownership/fixtures/vec-shared-index/lang.toml --backend python --out-dir build/vec-python --format json
python3.12 -s build/vec-python/program.py
```

两端应返回 0。Python 构建不接受 `--opt`；检查时不执行 Python，运行生成程序需 CPython 3.12。
