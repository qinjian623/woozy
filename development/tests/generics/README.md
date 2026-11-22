# T07 源语言案例

operation_id: language-t07-source-fixtures-20261104
契约版本：compiler-dev/1。

包含 27 个独立项目，manifest 路径均相对仓库根目录。由于诊断码尚未发布，用例省略 diagnostic_code，先用 expected_static.reason 说明错误，留待 root 登记。用例预期不因当前实现报 E_CAPABILITY_UNAVAILABLE 而改写。

规范依据包括：docs/SEMANTICS.md §2 的分支作用域、§3 的上下文推断与位宽规则、§5 的命名实参源码求值顺序与枚举穷尽性、§6 的泛型约束、唯一 impl、实例去重与递归预算、§7 的值字段递归复制；以及 docs/FRONTEND.md 的类型检查约定和 baseline/grammar.ebnf 的构造与模式语法。baseline/samples/05_traits.wz 提供显式 impl 示例。无限值布局依据有限值表示和递归值字段规则要求拒绝，具体诊断码待登记。只读接口提案不视为已实施。

每个反例只安排一项语义错误，暂未覆盖普通参数重新赋值、Vec、throws、foreign、复杂拥有值或浮点执行。字段求值顺序用例使用显式声明的 inout 参数。runtime_expected.main_return 记录整数返回值，reason 说明计算过程。跨包别名去重需另外检查实例数量，单靠运行返回 7 不能证明去重。同类型递归应接受，扩张递归应受预算限制，但不绑定具体预算数值。

自检 passed：27 个 ID 互不重复，全部 manifest 均存在、可按 TOML 解析且 package.name=app。正反例数量为 accept 13 个、reject 14 个。

语法验证 passed：使用现成 `build/frontend-debug/woozyc check --syntax-only <file>` 逐文件运行，实际退出码见下表。首轮字段求值顺序用例写错了参数模式位置，修正为 `n inout i32` 后已重新验证全部源文件。

类型检查和执行均为 not_run。当前 checker 预计会返回 E_CAPABILITY_UNAVAILABLE，但尚未用完整 checker 验证这一判断。语法通过不代表类型检查通过、程序可执行或 T07 已完成。

| 文件 | 实际退出码 |
| --- | --- |
| fixtures/branch-scope/src/main.wz | 0 |
| fixtures/cross-package-alias/vendor/dep/src/lib.wz | 0 |
| fixtures/cross-package-alias/src/main.wz | 0 |
| fixtures/enum-empty/src/main.wz | 0 |
| fixtures/enum-nonexhaustive/src/main.wz | 0 |
| fixtures/enum-payload/src/main.wz | 0 |
| fixtures/expanding-recursion/src/main.wz | 0 |
| fixtures/explicit/src/main.wz | 0 |
| fixtures/field-duplicate/src/main.wz | 0 |
| fixtures/field-evaluation-order/src/main.wz | 0 |
| fixtures/field-missing/src/main.wz | 0 |
| fixtures/field-unknown/src/main.wz | 0 |
| fixtures/fields-reordered/src/main.wz | 0 |
| fixtures/function-argument/src/main.wz | 0 |
| fixtures/impl-duplicate/src/main.wz | 0 |
| fixtures/impl-overlap/src/main.wz | 0 |
| fixtures/inference-conflict/src/main.wz | 0 |
| fixtures/inferred/src/main.wz | 0 |
| fixtures/infinite-layout/src/main.wz | 0 |
| fixtures/let-field-write/src/main.wz | 0 |
| fixtures/multi-instance/src/main.wz | 0 |
| fixtures/nested-copy/src/main.wz | 0 |
| fixtures/return-context/src/main.wz | 0 |
| fixtures/same-recursion/src/main.wz | 0 |
| fixtures/trait-missing/src/main.wz | 0 |
| fixtures/trait-unique/src/main.wz | 0 |
| fixtures/unused-template/src/main.wz | 0 |
| fixtures/used-template/src/main.wz | 0 |

主代理补充：跨包用例改为在清单中显式依赖 dep，让两个别名指向同一个 dep 声明；多实例用例补充了局部变量和无捕获函数字面量，后续需核对两个实例的各项身份互不相同。补充后的语法和名称检查记录见外层 checks/t07-independent-fixtures-review.json，仍未执行 T07 的类型检查或运行检查。
