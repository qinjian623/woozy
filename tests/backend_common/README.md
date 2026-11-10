# T05/T06 共用整数与控制流夹具

本目录只准备后端共用的源码、规范预期和阶段记录，不实现类型检查器、解释器或任一后端。15 个独立 `lang.toml` 项目的 `src/main.wz` 都声明 `main() i32`；成功程序在语义正确时返回 0，内部检查失败时返回不同的小非零码。`cases.json` 给出每个预期值和依据。

## 数据

- `schema`: `compiler-dev/backend-common-fixtures/1`，合同 `compiler-dev/1`。
- 15 个项目：11 个正常返回 `0`，4 个运行时 `panic`。
- 成功案例的预期直接按 `docs/SEMANTICS.md` 计算，不读取未来后端输出。
- `inout-argument-order` 单独标 `requires_inout: true`；其余案例不需要 inout。
- Panic 案例只规定失败原因、“不被 typed catch 处理”和“非 Panic 路径 `main` 返回 0”。runner 必须区分 Panic 与普通 0 或其它返回值，不能把任意非零返回当作 Panic 证据；精确进程退出码和诊断文字不作规定。
- 清单中的 `backend = "native"` 只是项目清单必填目标；源码属于公共子集，T06 应以 Python 目标运行同一份源码。本目录不声称任一目标已构建或执行。

## 覆盖

| fixture | 预期 | 覆盖点 |
| --- | --- | --- |
| i8-u8-wrapping | return 0 | i8/u8 加减乘、一元负号、复合赋值回绕 |
| signed-division-remainder | return 0 | 负数除法向零截断，余数随被除数；i32/i8 |
| division-min-over-minus-one | return 0 | i32/i8 的 `MIN / -1` 与 `MIN % -1` |
| bitwise-integers | return 0 | i32/u8 的按位与、或、异或、取反 |
| shift-left-right | return 0 | 算术/逻辑右移、左移丢位、任意整数型移位量 |
| dynamic-integer-conversions | return 0 | 有符号/无符号、窄化/扩宽的动态范围内的 `T(x)` |
| short-circuit-panic-guard | return 0 | 逻辑与/逻辑或短路阻止除零和非法移位 RHS |
| function-values | return 0 | 非捕获函数字面量、函数指针、函数值复制和位置调用 |
| control-flow-break-continue | return 0 | 三段式/条件 for、break、continue、嵌套 break |
| lexical-shadowing | return 0 | 块遮蔽、初始化式看外层绑定、for 初始化绑定作用域 |
| inout-argument-order | return 0 | 实参从左到右；位置参数及命名参数逆序；同一 var 的两次顺序 inout 借用 |
| panic-division-by-zero | panic | `10 / 0` |
| panic-invalid-shift | panic | i32 移位数 32 不在 `[0, 32)` |
| panic-dynamic-conversion | panic | 动态 `i8(i32 128)` 越界 |
| panic-negative-shift | panic | i32 值左移动态 i8 移位量 -1 |

`cases.json` 的每个 `check` 都通过 `rule` 引用完整规范来源，并给出具体计算。Panic 案例用 `trigger` 给出触发值和原因，记录非 Panic 路径 `main` 返回 0，但不规定 Panic 的精确退出码或诊断文字。

## 已执行检查（HEAD 7c49989）

使用主仓库已有构建，不依赖 LLVM 后端、Python 转译器或 GPU：

```sh
build/frontend-debug/woozyc check --syntax-only --format json <fixture>/src/main.wz
build/frontend-debug/lang_module_check <fixture>/lang.toml
```

2026-11-03 实测：

- 语法：15/15 退出 0，`status=ok`，无诊断。
- 名称/模块：15/15 退出 0，`status=ok`，无诊断。
- 类型检查：`not_run`。T04 checker 仍在开发，inout 子集尚未完成。
- T05 native 生成/链接/运行：`not_run`。
- T06 Python 生成/运行：`not_run`。

所以本记录只证明源码可解析、名称和模块可解析，不能证明 T04 完成，更不能证明 T05/T06 通过。

## 未使用或未规定的项

- 普通非 `inout` 参数是否可重新赋值在 `SEMANTICS.md` 中没有结论；本组不修改普通参数，局部修改一律用 `var`，需要跨调用修改时只用平凡 scalar `var` 整体 inout。
- `inout-argument-order` 的每次外层调用中，两个 helper 调用是先后发生的独立借用，不是同一调用同时接收同一存储的两个 inout 实参。位置参数预期为 1；命名参数按源码先求值 `second`、再求值 `first`，预期为 10。T04 正在补齐该子集；类型检查结果仍记为 `not_run`。
- 规范已规定 Panic 不进入 typed catch，Panic 路径不执行语言 defer。本组没有 defer，因此未测试清理，也不把“Panic 路径无泄漏”列为条件。只将精确退出状态和错误文字标为未由规范固定。
- 不使用 `try/catch`、`defer`、泛型、Vec 或捕获闭包；这些属于后续任务。

后续开发者真实执行类型检查和两个后端后，应补充按后端的 `passed`/`failed` 记录；若规范证据变化，只修正本目录并保留命令与结果，不修改历史基线。

集成检查将短路移位用例的左操作数改为显式 i32 的 one；裸字面量 1 在无其他上下文时默认 i64，移位 32 并不越界。预期值保持不变。
