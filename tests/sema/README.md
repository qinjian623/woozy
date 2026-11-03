# T04 语义夹具

本目录只准备独立的项目级输入，不实现第二套类型检查器，也不替代 `src/sema/`。每个 fixture 是一个单独的 `lang.toml` 项目，`cases.json` 记录规范依据和预期状态。

## 数据

- `schema`: `compiler-dev/sema-fixtures/1`，合同版本 `compiler-dev/1`。
- 21 个独立项目：4 个 `accept`、14 个 `reject`、3 个 `unavailable`。
- `available_in: T04` 的案例属于当前标量类型/调用/控制流基础。
- `unavailable` 分别是 T07（泛型）、T07/T09（Vec）、T07/T08（typed throws；夹具使用 T07 的枚举作为错误类型），预期 `E_CAPABILITY_UNAVAILABLE`。它们不是 T04 类型错误。
- 局部 `const` 是编译期常量，不允许后续赋值；本组为保持 20 例上限未新增单独 fixture。
- `unresolved_semantics` 只列全文检索后仍然没有规则的例子，未把摘要省略当成缺项。

## 实际检查（HEAD c53298a）

在主仓库根目录使用真实构建：

```sh
build/frontend-debug/langc check --syntax-only --format json <each .lang>
build/frontend-debug/lang_module_check <each lang.toml>
```

- 语法：20/20 `.lang` 通过，退出 0，`status=ok`，无诊断。
- 名称：20/20 项目通过 `lang_module_check`，退出 0，`status=ok`，无诊断。
- 因此 13 个 `reject` 不是因语法、拼写名称或导入失败而拒绝；它们通过语法和名称阶段后才应被类型/效果阶段拒绝。
- 类型检查：`not_run`，不是通过。HEAD 没有 T04 的完整检查器；本目录不执行 fixture 程序，也不声称 T04 或 T08 已完成。

检查由临时 stdlib Python 枚举 `cases.json`、调用上述两个 C++ 程序完成，没有向仓库加入 runner。

## 案例速览

| case | 预期 | 主要依据 |
| --- | --- | --- |
| numeric-boundaries-accept | accept | i8/u8/i32 边界、最小负 i32、默认 i64、i8 回绕 |
| i8-out-of-range-reject | reject | 128/-129 超出 i8 范围 |
| default-i64-mixed-width-reject | reject | 默认 i64 与显式 i32 不隐式混宽 |
| signed-unsigned-mixed-reject | reject | i32/u32 不隐式混用 |
| integer-float-mixed-reject | reject | i32/f64 不隐式混合 |
| bool-condition-int-reject | reject | if 条件必须是 bool |
| let-assignment-reject | reject | let 不可重新赋值 |
| var-assignment-accept | accept | var 可赋值和复合赋值 |
| return-type-mismatch-reject | reject | 返回值不隐式转换 |
| missing-return-reject | reject | i32 函数存在正常漏返回路径 |
| call-missing-argument-reject | reject | 缺少必需实参 |
| call-unknown-label-reject | reject | 未知命名标签且缺参 |
| call-duplicate-parameter-reject | reject | 位置参数与命名参数重复绑定同一形参 |
| call-labels-and-function-value-accept | accept | 直接函数可用命名参数；函数值只用位置参数 |
| function-value-named-label-reject | reject | 函数值调用不得使用命名标签 |
| inout-writable-var-accept | accept | 数值 var 整体绑定与 inout 标注一致 |
| generic-unavailable | unavailable/T07 | 语法和名称有效，泛型属 T07 |
| vec-unavailable | unavailable/T07,T09 | 语法和名称有效，Vec 属聚合/所有权阶段 |
| throws-unavailable | unavailable/T07,T08 | 语法和名称有效，typed throws 属 T08，错误类型用 T07 枚举 |
| inout-let-nonwritable-reject | reject | let 不是可写 var 整体绑定；T04 Place 检查拒绝 |

## 规范未定的最小例子

以下例子没有进入预期案例，因为全文没有给出足够规则，不能由本任务替集成负责人选择语义：

```lang
func bump(x i32) i32 {
    x = x + 1;
    return x;
}
```

`SEMANTICS §2` 只规定参数与函数体同作用域；没有说明普通参数是否可重新赋值，`inout`/`move` 规则不能反推普通参数的写权限。

集成审查另加 constant-integer-conversion-overflow-reject：源码 i8(300) 必须静态拒绝；typed HIR 的 IntegerConvert 仍可表达动态范围检查。原 20 个项目的记录保持为此前检查结果；新增项目单独检查语法和名称，类型检查尚未运行。
