# T07 源语言案例

operation_id: language-t07-source-fixtures-20261104
契约版本：compiler-dev/1。

27 个独立项目；manifest 相对仓库根目录。诊断码未发布，省略 diagnostic_code，由 expected_static.reason 说明错误，留给 root 登记。预期不随当前实现的 E_CAPABILITY_UNAVAILABLE 调整。

规范依据：docs/SEMANTICS.md §2 的分支作用域、§3 的上下文推断与位宽规则、§5 的命名实参源码求值顺序和枚举穷尽性、§6 的泛型约束、唯一 impl、实例去重及递归预算、§7 的值字段递归复制；docs/FRONTEND.md 类型检查约定及 baseline/grammar.ebnf 的构造和模式语法。baseline/samples/05_traits.lang 提供显式 impl 示例。无限值布局依据有限值表示及递归值字段要求拒绝，具体诊断码待登记。只读接口提案未视为已实施。

每个反例安排一项语义错误。未覆盖普通参数重新赋值、Vec、throws、foreign、复杂拥有值或浮点执行。字段顺序项目使用明确声明的 inout 参数。runtime_expected.main_return 表示整数返回值，reason 解释计算。跨包别名去重须另检查实例数量，运行返回 7 不能证明去重。同类型递归应接受，扩张递归应受预算限制，不绑定预算数值。

自检 passed：27 个 ID 唯一，manifest 全部存在、可按 TOML 读取且 package.name=app。正反例数量：{'accept': 13, 'reject': 14}。

语法验证 passed：现成 `/home/qin/Work/language-dev-20261103/compiler/build/frontend-debug/woozyc check --syntax-only <file>` 的逐文件实际退出码如下。首轮字段顺序例的参数模式位置写错，修正为 `n inout i32` 后重新验证全部源文件。

类型检查 not_run，执行 not_run。当前 checker 预计 E_CAPABILITY_UNAVAILABLE；未用完整 checker 验证此预测。语法通过未证明类型检查、执行通过或 T07 功能完成。

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

主代理补充：跨包例改为清单显式依赖dep，两个别名指向同一dep声明；多实例例加入局部变量及无捕获函数字面量，后续需检查两实例的所有身份互异。补充后的语法和名称检查另见外层checks/t07-independent-fixtures-review.json，仍未执行T07类型或运行检查。
