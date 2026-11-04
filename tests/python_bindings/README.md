# T10 Python 绑定测试资料

本目录只包含测试资料，不包含编译器、绑定或运行库实现。它用于给 T10 准备独立源码正反例、未来静态期望和未来 CPU 执行观察；**不表示 T10 已经完成**。`cases.json` 是测试清单，不是新的公共 schema；没有修改 `contracts/` 下的绑定格式。

## 输入与边界

- 原训练样例：`baseline/samples/04_numpy_torch.lang`，SHA-256 `beb672124e6ccc697c8c129d2f7aa3bf21640de0b4af7a058eefa907fc6e6797`，保持原字节。
- 绑定声明：`contracts/python-bindings.schema.json` 和 `contracts/python_bindings.example.json`；本目录没有新增模块、公开函数、类型或 Vec 长度 API。
- 规范依据：`docs/SEMANTICS.md`、`baseline/LEXICAL_SYNTAX.md`、`docs/ACCEPTANCE.md` A08、`docs/IMPLEMENTATION_PLAN.md` T10、`docs/TOOLING.md`。
- 未导入真实 NumPy/PyTorch，未运行训练，未伪造 `torch`/`numpy` 模块。所有未来执行在 `cases.json` 中均为 `not_run`。

## 用例

| 文件 | 未来静态期望 | 覆盖点 |
| --- | --- | --- |
| `fixtures/01_imports_and_types.lang` | accept | 六个原导入、全部外部类型、`py.Error` |
| `fixtures/02_numpy_pipeline.lang` | accept | NumPy 管线、显式 `torch.tensor` 复制、dtype 分离、关键字 |
| `fixtures/03_module_call_optimizer.lang` | accept | 构造、`model(x)` 实例调用、`parameters`、zero_grad/backward/step |
| `fixtures/04_autograd_loss.lang` | accept | Tensor 运算与返回保留 autograd，不 detach/item |
| `fixtures/05_error_cause.lang` | accept | 外部异常桥接为 `py.Error`，保留原始 cause/traceback |
| `fixtures/06_named_argument_order.lang` | accept | 命名实参逆形参顺序书写，按源码顺序求值，平凡 `inout` 计数 |
| `fixtures/07_bad_call_signature.lang` | reject | 未知参数名；keyword-only 参数按位置传递 |
| `fixtures/08_bad_member.lang` | reject | 未声明的模型成员（拼写错误）；正确接口是 `model(x)` |
| `fixtures/09_bad_types.lang` | reject | `torch.float32` 当 `np.DTypeSpec`；ndarray 当 Tensor |
| `fixtures/10_bad_move.lang` | reject | 对普通借用参数使用 `move`，且目标是 ordinary 形参 |
| `fixtures/11_bad_inout.lang` | reject | 外部对象作为首版 `inout`，规范明确未支持 |
| `fixtures/12_external_index_attributes.lang` | unresolved | 外部属性 `x.device` 和多下标 `x[-1, 0]`；不得套用 Vec 非负索引规则 |

`cases.json` 为每个文件记录源码 SHA-256、规范出处、未来静态 accept/reject、未来执行观察和当前 syntax-only 结果。反例的诊断码是当前注册表中的预期提示；规范只固定“必须静态拒绝”，最终稳定码仍由 T10 诊断契约确认。

## 未来执行要观察什么

- NumPy 数据：`x_np`、`y_np` 为 `(128, 1)`、`float32`，`y_np = 2*x_np + 1`；精确值在锁定环境中与参考计算比较。
- PyTorch：`x`、`y` 是 CPU float32 Tensor；`torch.tensor` 只出现在源码明确的显式复制边界。
- Module：通过调用 hook 观察到 `model(x)`，生成产物不得改成 `model.forward(x)`；backward 和 optimizer.step 各自计数。
- autograd：返回的 loss 是零维 Tensor，`requires_grad`/`grad_fn` 保留，参数梯度非空；不比较手写训练的 loss 数值。
- 异常：NumPy 的 axis 运行时错误在绑定边界变为 `py.Error`，原始异常类、消息、cause/traceback 与同环境直接调用一致；不包装 `BaseException`。
- 参数顺序：`06` 先写 `out_features` 再写 `in_features`，副作用计数应为 `order == 21`；错误地先按形参顺序重排求值会得到 `12`。模型应为 `in_features=1`、`out_features=2`。
- 以上全部为 `not_run`；数值阈值只引用 `docs/ACCEPTANCE.md` A08，不写死训练结果。

## 当前实测（2026-11-04，基点 51363f5）

临时构建位于 `/tmp/language-t10-binding-fixtures-20261104/build/langc`，未改编译器和仓库构建目录。先读了 `docs/TOOLING.md` 第 3 节和 `langc --help`。

```sh
langc check --syntax-only --format json <fixture>
```

- 12 个新 fixture + 原 `04_numpy_torch.lang`：13/13 `status=ok`、0 diagnostics。语法通过不代表类型、绑定或执行通过。
- 用临时项目清单和原绑定示例跑完整 check 时，12 个 fixture 都先导入 `std/py`；当前 CLI 没有 std binding bundle 配置，因此在 **resolve** 阶段得到 `E_IMPORT_NOT_FOUND: std source is not configured`。这不是语法拒绝，也不是 T10 静态通过。
- 为区分 std 配置问题，仅在 `/tmp` 去掉 `std/py` 做了临时探针；项目载入成功后，在 **type** 阶段得到 `E_CAPABILITY_UNAVAILABLE: foreign declarations require T10/T11`。因此当前完整 check 没有进入 T10 绑定语义检查。
- 初版命令和失败检查保存在 `/tmp/language-t10-binding-fixtures-20261104/initial/`；唯一修正的 13 份语法结果、13 次完整检查及报告在同目录的 `correction/`。root 的独立记录保存在外层工作目录 `checks/t10-fixtures-root/{initial,correction}/`。

本目录没有修改 CMake，因此当前不会自动进入 CTest；未来 T10 测试接入时由 owner 读取 `cases.json` 并建立项目清单/绑定配置。

## 未决问题

1. **外部属性/下标绑定**：原 schema 只有 modules/types/values/callables，没有字段属性、Tensor/ndarray 下标、单/多下标或结果类型的声明位置。T10 是扩展绑定契约，还是把这些统一报为未支持能力？在决定前，`12_...lang` 不写成 accept 或 reject；更不能用 Vec 非负索引规则代替外部语义。

## 已确定的测试处理

- 按 INTEROP 和 A08 的要求，由宿主测试检查生成的 Python/runtime 是否保留 cause/traceback；不新增 `py.Error` 的源语言成员。
- 借用参数上的 `move` 及向 ordinary 形参传 `move` 沿用现有 `E_CALL_ARGUMENT`，不另设 T10 诊断码。测试同时检查拒绝位置，不只匹配自然语言 message。

这些处理不表示绑定检查或运行已经通过；相应未来执行仍为 `not_run`。
