# 用户文档事实依据（2026-11-12）

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

## 尚未完成

- Task/协程帧图的自动回收未完成；不能把 Vec/Box/闭包以外的纯 Woozy 环都写成已回收。
- 普通async拥有参数已由Root整合：创建Task时立即逻辑复制，复用active标志清理；22配置与独立10组合通过。
- async 入口、重复 await、未启动/取消 Task 的 move 参数清理已由 Root 合入 `72028b1`；58 项源码检查与完整 149/149 CTest 通过。Task GC 仍未完成，用户文档不把 async/await 写成完整可用范围。

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
