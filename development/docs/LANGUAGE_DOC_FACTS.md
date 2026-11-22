# 用户文档事实依据（2026-11-13）

本文件记录本轮用户文档修正所依据的实现与测试范围，不定义语言，也不替代 `SEMANTICS.md`。用户教程只写已确定的规则，具体后端限制和回归记录留在本目录。

## 已确认可用

| 能力 | 依据 |
| --- | --- |
| LANG01 捕获闭包、`move func`、绑定方法值（含 move-Self 重复调用） | `tests/reference_closure_frontend/` 的真实 Source/HIR/NativeIR 检查；`tests/reference_values/fixtures/closure-*`、`bound-*` 的 Native O0/O2 执行；Python 3.12 执行除已记录的线程捕获入口外均通过。 |
| Weak 源接口 | `tests/native/weak_source/`、`tests/python/weak_source/`、`tests/reference_values/fixtures/weak-*`；目标、生命周期、空弱引用和升级失败路径已覆盖。 |
| 字段/元素与拥有值 inout | `tests/inout_places` 原21例；`tests/owning_inout` 新21例Native O0/O2/Python及7种Python存储检查通过。Task和外部资源inout仍拒绝。 |
| `dyn Trait` 普通 object-safe 方法 | `tests/trait_objects/cases.json` 的 22 个 Source case；包含泛型 trait 身份、泛型 impl、闭包/字段/枚举边界、空 trait、dyn 环和拒绝诊断；Native O0/O2、Python 3.12、freestanding ELF 检查。 |
| SourceGC：Vec/Box/闭包环境强引用环 | Native O0/O2 与 Python 的 Source 回归已通过；Python 观察器在禁用宿主 GC 时验证语言收集。 |

## 异步实现与限制

- Task 与协程帧图的 Native 回收已通过 Frame32、永久内存 24 和独立清理检查，并整合到当前源码；完整 171/171 CTest 通过（152.93 秒）。Python 暂不支持 async。
- 普通 async 拥有参数已由 Root 整合：创建 Task 时立即逻辑复制，并复用 active 标志完成清理；22 组配置与独立 10 组合均已通过。
- async 入口、重复 await，以及未启动或已取消 Task 的 move 参数清理已由 Root 合入 `72028b1`，58 项源码检查与完整 149/149 CTest 通过。用户文档仍如实列出单线程执行器及 C 资源的保守拒绝限制。

## 本轮新增示例的核对

以下新示例使用本工作树已有 CLI 实际 `check` Native/Python，并在 Native O0/O2 与 Python 3.12 运行：

- 闭包创建时捕获与作用域逃逸：`make_reader`。
- 闭包只读绑定与共享 Vec 修改。
- `move func` 的拥有值移动与直接值复制。
- 普通 Self 绑定方法值；move-Self 绑定方法值复制并重复调用。
- Weak 生命周期与升级为 `None`。
- `dyn Trait` 标量方法分派。

字段/元素 inout 文档没有新增独立示例，继续沿用 `tests/inout_places` 中 Root 已通过的 21 个 Source fixture 规则，因此本轮未单独为它重跑 CLI。

核对命令使用本工作树 `build/llvm-clang/woozyc`：

```sh
woozyc check --project <fixture>/lang.toml --backend native --format json
woozyc check --project <fixture>/lang.toml --backend python --format json
woozyc build --project <fixture>/lang.toml --backend native --opt 0 --out-dir <out0>
woozyc build --project <fixture>/lang.toml --backend native --opt 2 --out-dir <out2>
woozyc build --project <fixture>/lang.toml --backend python --out-dir <outpy>
```

上述 8 个文档示例全部通过两后端 check，Native O0/O2 与 Python 3.12 实际运行均返回 0。

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

Root 复核时独立提取了这 8 个示例，24 组配置均实际运行通过；修正后的 Weak 内建/模块命名和 dyn 参数模式也用 `72028b1` 跑了两后端 check 确认。

## Task GC 集成前检查（2026-11-13）

验证副本中的 Source Task/frame GC 已通过 32 组配置和永久内存 24 组配置，覆盖 Task→Vec、闭包、promise result/error 循环以及普通参数保活。155 个不同的 CTest 目标分批全部通过（首轮通过 149 项，修正两项测试注册并补齐四项 Python 环境后再通过 6 项），并非单次 155 项全跑。当时 fork 集成仍在实现中，因此该阶段记录不能用来宣称主分支的 Task GC 已完成。

同时补充了 Hosted Thread 的宿主清理：先 drop 其 Woozy 上下文，当最后引用只剩宿主状态时，可在 GC 恢复后 join 并释放。新增 Source 用例 O0/O2 为 2/2 通过，旧库对照均确认在 kernel 侧完成清理；修正后 heap/Vec 归零，相关 3 项 CTest 和原 Hosted Thread O0/O2 均通过，独立复核未发现新问题。该修正已随 973328d 发布，完整 154/154 CTest 通过（135.48 秒）。

## Iterator、Task/frame 与 fork 组合（2026-11-13）

- Iterator 唯一修正版已由 Root 复核并整合，共 22 个源码用例（15 个接受、7 个拒绝）；接受用例在 Native O0/O2 和 Python 下执行，并检查拥有值的分配与释放。Slice 用例因旧 Python 观察器无法编码快照而跳过该观察器，但实际 Python 程序和 Native 观察器仍照常运行。
- Root 重新链接了先前的独立 HIR 探针：用同签名普通函数冒充 next、用源码变量替代隐藏 cursor，均被 verify_types 和 verify 拒绝，合法 HIR 通过；T09 共 371 项检查通过。
- Root 用组合编译器另跑了 3 个独立源码在 Native O0/O2 和 Python 下的 9 次执行，全部通过，覆盖泛型拥有返回、嵌套 throw/defer 和完整 trait 实参选择。
- 组合副本的 5 项定向 CTest 全部通过（34.86 秒），包括 Iterator22、T09、Frame32、永久内存 24 和原 async58。fork 改动已独立复核，三项原死锁、跨线程创建前拒绝以及 FD child-first ack 均通过。
- 永久 fork GC 测试已改为在真实注销前暂停，要求 child 实际 trace 仍注册且 refs=1 的对象，不能靠随机调度绕过该分支；Root 相关 21/21 CTest 通过（33.00 秒）。
- 从教程与并发文档提取的 Counter 迭代器和 async sleep 示例均在 Native O0/O2 下运行通过（共 4 次）；用户文档不包含构建器开发步骤。
- 主目录已完成构建，完整 171/171 CTest 通过（152.93 秒）。随后补充了 async TCP/UDP loopback 测试，Root 实跑 66/66 通过（40.74 秒），覆盖两种协议在 O0/O2 下的普通运行与观察器运行；这次补充只改了测试和状态文档，没有重跑完整 171 项。

TCP/UDP 测试靠接收方的实际 WouldBlock 标志、心跳和 pipe 事件控制先后顺序，不拿固定发送延迟当作“已进入等待”的证明；TCP 同时接受立即 connect 成功、EINTR 和 EINPROGRESS。由于 Future 数量会随合法分支变化，观察器对全部用例统一要求 Future/frame/Vec 的分配与释放平衡；旧用例仍保留精确计数，新增两例保留固定的 frame/Vec 计数和 bad_frame_free 检查。Root 用 1 组平衡计数和 3 组不平衡计数独立验证了这套判定逻辑（4/4 通过）。不过，立即 connect 成功分支在内核实跑中未触发，新增测试也未覆盖 IPv6、公网或 syscall 级追踪。
