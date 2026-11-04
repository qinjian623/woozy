# T10 CPU host conformance (corrected)

操作：`language-t10-cpu-conformance-20261104-correction`。基点：`058ee4c`。本文件只记录**宿主接口实测**；不是 `.lang` 生成程序、`py.Error` 桥接或 T10 完成证明。

## 修正结论

初版因两处假通过被 rejected：脚本未固定被测试 bundle，声明变更后仍报告 15/15；宿主错误分支捕获任意 `Exception` 即 passed。现已修正：

- 修正脚本只接受原 bundle 的固定字节 SHA-256 `44924c3c33c5acff0e604f126fa1dcb346472fa0a7966de14dd0a5b4a10adb5f`。模块、属性或参数 kind 任一变更，在任何接口调用和手写参考执行前拒绝，退出码 1，结构化结果列出 15 个接口 `not_run`，不再给出 15 passed。
- 宿主错误检查现在断言锁定版本下的 `numpy.exceptions.AxisError`/非法 axis 3 信息和 `RuntimeError`/非法 device 原因；错误类别不符、同类但原因不符均有拒绝对照。替换宿主函数时在 `finally` 恢复。
- 原 bundle 正例重跑：退出码 0，15/15 宿主 callable passed；宿主错误 2 个真实正例 + 4 个无关错误反例全部按预期；手写参考结果与初版一致。
- 初版 `20-conformance.json`、`20b-conformance.json`、`candidate-lock.json` 和报告未被覆盖或改成通过。本轮未安装、下载、升级或改变任何版本。

## 冻结 bundle 拒绝对照

| 临时副本变更 | 实际命令结果 | 结构化结果 |
| --- | --- | --- |
| `modules[numpy].host_module = t10_missing_numpy` | exit 1 | `failed`、`frozen_bundle_sha256`、15 个接口 `not_run`、reference `not_run` |
| `callables[numpy.linspace.scalar].host_attribute = [arange]` | exit 1 | 同上 |
| `callables[numpy.linspace.scalar].parameters[start].kind = keyword_only` | exit 1 | 同上 |

副本及 SHA-256 记录：`bundles/changes.json`。完整拒绝 JSON：`negative-host-module.json`、`negative-host-attribute.json`、`negative-parameter-kind.json`。宿主错误细节：`host-error-controls.json`。脚本没有增加通用反射或动态绑定执行器；它只验证冻结原 bundle。

## 宿主错误对照

- 正例 1：`np.expand_dims(values, axis=3)` 实际抛 `numpy.exceptions.AxisError`，`axis` 为 3，消息含 axis/3/bounds-or-dimension 必要信息。
- 正例 2：非法 device 的 `torch.tensor` 实际抛 `RuntimeError`，消息含 device、测试字符串和 invalid-device 原因。
- 反例 1：替换为 `ValueError`/`TypeError` 的无关类别，验证器拒绝。
- 反例 2：同类但无关原因（`AxisError(1,1)`、`RuntimeError("device initialization failed")`），验证器拒绝。
- 所有替换函数均在 `finally` 恢复；结果记录 `function_restored_in_finally: true`。负例之后仍执行手写参考，实际再次使用 `np.expand_dims`/`torch.tensor` 等恢复后的函数。`py.Error` 桥接仍为 `not_run`，没有写成已通过。

## 正例结果

修正脚本正例：`20-conformance-original.json`，退出码 0，SHA-256 `187b33df871373e5715a70a2896cb82701de37741db2adf72130318d735c61e7`。

- 环境未变：CPython 3.12.15、`numpy 2.5.4`、`torch 2.15.0+cpu`；`torch.version.cuda is None`。
- 15/15 接口 passed；2 个 dtype 值通过；8 个 host 类型声明和 3 个协议检查通过；`lang.py.Error` 为 `not_run`。
- 宿主错误：`{"passed": 6, "failed": 0, "not_run": 0}`，其中 2 个真实正例、4 个正确拒绝的负例。
- 手写参考：`forward=201`、`backward=200`、`zero_grad=200`、`step=200`；最终 loss `9.684752999561397e-13`，有限、非负、`<1e-3`。
- `not_run`：`.lang` 生成/执行、`py.Error` 桥接、源语言清理/控制事件、GPU/CUDA/AMP/DDP。

## 环境与依赖

环境、11 个 wheel 和已安装版本沿用初版，本轮没有重新安装/联网解析。依赖仍引用初版候选锁：

- 初版候选锁：`../initial/candidate-lock.json`，SHA-256 `347acdb51fb6ba1b1e696d7a2e015cff53fa5f40c5439cdef53fb07397db6000`。
- 修正引用：`candidate-lock-reference.json`。
- wheelhouse：`/home/qin/Work/language-dev-20261103/toolchains/t10-python-cpu/wheelhouse`；修正前后 11 个 wheel SHA-256 相同。

## 证据与命令

实际命令、退出码和日志：`commands.log`；结构化汇总：`summary.json`；修正报告：`report.md`。均在：

`/home/qin/Work/language-dev-20261103/checks/t10-cpu-conformance/correction/`

初版证据未被覆盖：

- `../initial/20-conformance.json` SHA-256 `3082ceef0f15b15fdb5899061fe7e54cb0963c40557d8f67e421c14e8941abe8`
- `../initial/20b-conformance.json` 同上
- `../initial/candidate-lock.json` SHA-256 `347acdb51fb6ba1b1e696d7a2e015cff53fa5f40c5439cdef53fb07397db6000`

原 schema、binding example、`04_numpy_torch.lang`、手写参考和正式 `toolchain.lock.json` 哈希仍与 `HEAD` 一致；`review_status` 仍为 `draft_requires_conformance`，正式 lock 未改写。无 commit/push/stash。
