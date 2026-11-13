# 用户文档事实依据（2026-11-13）

本文件记录本轮用户文档修正所依据的实现与测试范围，不定义语言，也不替代
`SEMANTICS.md`。用户教程只描述已确定规则；具体后端限制和回归记录留在本目录。

## 已确认可用

| 能力 | 依据 |
| --- | --- |
| LANG01 捕获闭包、`move func`、绑定方法值（含 move-Self 重复调用） | `tests/reference_closure_frontend/` 的真实 Source/HIR/NativeIR 检查；`tests/reference_values/fixtures/closure-*`、`bound-*` 的 Native O0/O2 执行；Python 3.12 执行除已记录的线程捕获入口外均通过。 |
| Weak 源接口 | `tests/native/weak_source/`、`tests/python/weak_source/`、`tests/reference_values/fixtures/weak-*`；目标、生命周期、空弱引用和升级失败路径已覆盖。 |
| 字段/元素与拥有值 inout | `tests/inout_places` 原21例；`tests/owning_inout` 新21例Native O0/O2/Python及7种Python存储检查通过。Task和外部资源inout仍拒绝。 |
| `dyn Trait` 普通 object-safe 方法 | `tests/trait_objects/cases.json` 的 22 个 Source case；包含泛型 trait 身份、泛型 impl、闭包/字段/枚举边界、空 trait、dyn 环和拒绝诊断；Native O0/O2、Python 3.12、freestanding ELF 检查。 |
| SourceGC：Vec/Box/闭包环境强引用环 | Native O0/O2 与 Python 的 Source 回归已通过；Python 观察器在禁用宿主 GC 时验证语言收集。 |

## 异步实现与限制

- Task/协程帧图的Native回收已通过Frame32、永久内存24与独立清理检查；已整合到当前源码，完整171/171 CTest通过（152.93秒）。Python暂不支持async。
- 普通async拥有参数已由Root整合：创建Task时立即逻辑复制，复用active标志清理；22配置与独立10组合通过。
- async 入口、重复 await、未启动/取消 Task 的 move 参数清理已由 Root 合入 `72028b1`；58 项源码检查与完整 149/149 CTest 通过。Native Task/frame GC已整合，用户文档列出单线程执行器及C资源的保守拒绝限制。

## 本轮新增示例的核对

以下新示例使用本工作树已有 CLI 实际 `check` Native/Python，并在 Native O0/O2 与 Python 3.12 运行：

- 闭包创建时捕获与作用域逃逸：`make_reader`。
- 闭包只读绑定与共享 Vec 修改。
- `move func` 的拥有值移动与直接值复制。
- 普通 Self 绑定方法值；move-Self 绑定方法值复制并重复调用。
- Weak 生命周期与升级为 `None`。
- `dyn Trait` 标量方法分派。

字段/元素 inout 的文档没有新增独立示例；继续使用 `tests/inout_places` 已通过的 Source 规则。

核对命令使用本工作树 `build/llvm-clang/woozyc`：

```sh
woozyc check --project <fixture>/lang.toml --backend native --format json
woozyc check --project <fixture>/lang.toml --backend python --format json
woozyc build --project <fixture>/lang.toml --backend native --opt 0 --out-dir <out0>
woozyc build --project <fixture>/lang.toml --backend native --opt 2 --out-dir <out2>
woozyc build --project <fixture>/lang.toml --backend python --out-dir <outpy>
```

8 个文档示例全部通过两后端 check；Native O0/O2 与 Python 3.12 实际运行返回 0。没有新增字段/元素 inout 代码示例，因此没有为它重跑 CLI；字段/元素 inout 的依据仍是 Root 已通过的 21 个 Source fixture。

## 文档审查修正核对

本轮只修正用户文档事实，不修改语言或实现。最小源码检查使用本工作树 CLI：

| 检查 | 源码 | Native/Python 结果 |
| --- | --- | --- |
| 内建 `Weak[T]` 成功 | `/tmp/doc_correction/weak_builtin` | 两后端 `check` exit 0 |
| `weak.Weak[T]` 失败 | `/tmp/doc_correction/weak_module_type` | 两后端 exit 1，`E_NAME_NOT_FOUND: module has no declaration: Weak` |
| dyn 非接收者 `move i32` 拒绝 | `/tmp/doc_correction/dyn_move_param` | 两后端 exit 1，`E_OBJECT_UNSAFE` |
| 绑定方法接收者逻辑副本与 Vec 共享 | `/tmp/doc_correction/bound_receiver` | 两后端 `check` exit 0；Native O0/O2、Python 3.12 运行 exit 0 |

`bound_receiver` 同时验证：修改原 Vec 元素会反映到保存的绑定方法；替换普通按值字段或重新绑定原变量不会改变保存副本，保存的 Vec 句柄仍指向旧容器。

`std/weak` 模块只提供 `empty`、`downgrade`、`upgrade`；`Weak[T]` 是内建类型。未运行 full CTest；LANG02 生产实现和 trait 测试不在本轮修改范围。

Root 复核：独立提取8例、24配置实际运行通过；修正后的 Weak 内建/模块命名、dyn 参数模式又用 `72028b1` 的两后端 check 确认。

## Task GC 集成前检查（2026-11-13）

验证副本的Source Task/frame GC已通过32配置、永久内存24配置；包含Task→Vec、闭包、promise result/error环，以及普通参数保活。155个不同CTest目标分别通过（149项首跑、修正两项测试注册并补齐四项Python环境后6项通过），不是一次155项全检。fork集成尚在实现，不能据此把主分支的Task GC写成已完成。

另补Hosted Thread宿主清理：其Woozy上下文先drop，最后引用只剩宿主状态时可在GC恢复后join并释放。新Source O0/O2为2/2，旧库对照均确认在kernel清理；修正后heap/Vec归零，相关3项CTest及原Hosted Thread O0/O2通过，独立复核无新增问题。该修正已由973328d发布，完整154/154 CTest通过（135.48秒）。


## Iterator、Task/frame 与 fork 组合（2026-11-13）

- Iterator 唯一修正版已由Root复核并整合：22源码用例（15接受、7拒绝），接受用例执行Native O0/O2、Python，并检查拥有值分配释放。Slice的旧Python观察器因无法编码快照而跳过；其实际Python程序和Native观察器仍运行。
- Root重新链接先前的独立HIR探针：同签名普通函数冒充next、使用源码变量替代隐藏cursor均被verify_types和verify拒绝，合法HIR通过。T09共有371项检查通过。
- Root用组合编译器另跑3个独立源码×Native O0/O2/Python，共9次运行通过；覆盖泛型拥有返回、嵌套throw/defer及完整trait实参选择。
- 组合副本的5项定向CTest全部通过（34.86秒），含Iterator22、T09、Frame32、永久内存24和原async58。fork已独立复核：三项原死锁、异线程创建前拒绝及FD child-first ack均通过。
- 永久fork GC测试已改为真实注销前暂停，child必须实际trace仍注册且refs=1的对象；不能靠随机排队绕过该分支。Root相关21/21 CTest通过（33.00秒）。
- 从教程与并发文档提取Counter迭代器、async sleep示例，各在Native O0/O2运行通过，共4次。用户文档不含构建器开发步骤。
- 主目录已完成构建；完整171/171 CTest通过（152.93秒）。随后补充async TCP/UDP loopback测试，Root实际66/66通过（40.74秒），包含两种协议的O0/O2普通及观察器运行。第二次修改只涉及测试和状态文档，没有重新跑完整171项。


TCP/UDP测试用接收方的实际WouldBlock标志、心跳与pipe事件控制先后；没有以固定发送延迟当作已等待的证明。TCP接受立即connect成功、EINTR和EINPROGRESS。Future数量可随合法分支变化，因此观察器对全部用例要求Future/frame/Vec分配与释放平衡；旧用例的精确计数完整保留，新两例保留固定frame/Vec和bad_frame_free检查。Root用一次平衡计数和三种不平衡计数独立检查该判断，4/4通过。立即connect成功分支未在内核实跑中出现，IPv6、公网和syscall级追踪未在新增测试中执行。
