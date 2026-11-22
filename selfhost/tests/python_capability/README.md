# Python capability gate for Native-only system calls

这个组件测试用来锁定 Wz `hirverify` 对 Python 后端的系统操作检查，对应 C++ `src/hir/hir.cpp:check_backend_capabilities` 中的 `value_system_operations` 规则。它只构建一个小驱动，不重建 Wz CLI，也不运行 Python 后端。

## 根因与修复

修复前 `selfhost/src/wzcc/hirverify/backend.wz:system_op_category` 只把 `std/slice` 和 `std/weak` 分成特殊类别，其余所有操作都返回类别 `0`；而 `check_backend_capabilities` 在非 Native/64 时只拒绝类别 1/2。结果 `std/os` 以及线程、网络、进程、FFI、async 等所有 Native-only `CalleeKind::System` 调用都被 Python/64 放行，直到 Python emitter 把它们当成普通 `_rt.value_call(...)` 生成并在运行时失败。

修复后将类别补成与 C++ 检查规则一致：

- `0`：C++ `value_system_operations` 覆盖的 `std/unicode`、`std/text`、`std/vec`、`std/strconv`、`std/math` 操作，仍可生成 Python 代码；
- `1`：`std/slice`，仍由 `slice_values` 控制；
- `2`：`std/weak`，仍由 `weak_values` 控制；
- `3`：其余所有操作（含 `std/os`、运行句柄、网络、进程、FFI、async），非 Native/64 时以 `E_CAPABILITY_UNAVAILABLE` / phase `type` / `system operations require the Native/64 profile` 拒绝。

判定读取 typed HIR 的 `Call.kind == System`、`Call.system` 和生成表 `OperationSpec.owner`，不是按 `os_text_length` 等函数名字符串做黑名单。核对过该快照：C++ `value_system_operations` 的 57 个名称与 Wz 生成表中上述 owner 的 57 个名称完全一致，owner 集合相等。

## Fixture 与断言

- `fixtures/native_only/`：app 导入最小合法 `std/os`，后者调用 `__woozy_os_text_length(value)`。同一份 Source 分别以 Native/64 和 Python/64 跑真实 parse/resolve/sema/HIR。
- `fixtures/value/`：最小 `std/text` 调用 `__woozy_text_len(value)`，Python/64 必须继续接受。
- `fixtures/ordinary/`：无 system call 的普通函数调用，Python/64 必须继续接受。

`src/main.wz` 从 typed HIR 中找出 system call 的精确 `Source.span`，并要求 Python 诊断的 primary 区间与之逐字节相等。当前期望：

```text
CASE native-only-native ... result=accept ... operation=12 call-begin=67 call-end=96
CASE native-only-python ... result=reject code=E_CAPABILITY_UNAVAILABLE phase=type primary=1 begin=67 end=96 ... message=system operations require the Native/64 profile
CASE value-python ... result=accept ... operation=29 call-begin=66 call-end=89
CASE ordinary-python ... result=accept ... call=0
```

`oracle.cpp` 用同一批 fixture 和真实 C++ lex/parse/resolve/sema/HIR API
构造同一个最小 Project，输出同样的 code/phase/message/primary/call span。
`run.py` 要求 C++ oracle 与 Wz 报告逐字段相同。

修复前，在同一 stage-0 seed 上运行该驱动，`native-only-python` 实际为
`result=accept`；10 个检查中 5 个失败。修复后 O0/O2 均 `failures=0`。

## 复现

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
python3 selfhost/tests/python_capability/run.py \
  --woozyc build/woozy-stdlib-native/woozyc \
  --toolchain-root "${WOOZY_TOOLCHAIN_ROOT}" \
  --build-dir build/woozy-stdlib-native \
  --work-dir /tmp/python-capability-final \
  --timeout 1200 --opts 0,2
```

`run.py` 用 `selfhost/corpus._run_process_impl` 为每条命令设置有限
deadline、进程组清理，并保留 stdout/stderr/JSON 和 `results.json`。
它在运行前后对 driver、fixture、`backend.wz` 做 SHA-256 快照并要求不变。

本次实际结果（stage-0 `6e6a436f...`）：

- `check`：exit 0；
- C++ oracle：exit 0，4 行与 Wz 期望逐字段相同；
- Wz driver O0/O2：exit 0，`SUMMARY checks=10 failures=0`；
- `inputs_before == inputs_after`，所有命令 `cleanup_complete=true`、
  `abandoned_pipes=false`。

Source/fixture SHA-256：

```text
backend.wz (修复后)  5e05ff73fb65feda8d7947a1314b0e854f3ea077adb3f21017784b423d2687ab
src/main.wz          739451239af6a1bd489ab2d6fa857142f493dcb7d6020cc7f178bcf5d0d69001
run.py               763e9d7ff98b04eb16d9c35a848cf4c4ef862b6751f0d1d4096a08117c07e833
oracle.cpp           7518afa01406431ab4c4eabf995319e6fd01fe30b2f7a7f4973518cc163a6c00
native_only/main.wz  d18ac3b9b8133229cb95cdcb4720e6271769d6fdff39a816090afa8b518553f9
native_only/os.wz    4c4cd525828c621f1c9d446bd7ef3b70ef9e016281eb68ff11fdb0281a9ebaf3
value/main.wz        041fc33cd066a8c780ed342f825ce8a49a0412e3270f42923ec38300fc903591
value/text.wz        7423e187242abbca59ccf7ad2dfbc9bfb8b62a15a9779c931e5efdf6917b1f10
ordinary/main.wz     db6b1345acfde8be2e2f47199a86fc4d125724adc58fd168412927afe26f620e
```

## 边界

本测试只验证共享 Wz capability gate 对这批最小 Source 的 accept/reject 结果和精确主 span，并确认与 C++ 参考结果一致。它不重建或运行 Wz CLI，因此没有验证 CLI 发布目录最终确实未生成 program.py/langrt.py；但 Python 公共 `verify` 在拒绝时不返回 `VerifiedHirModule`，`pythonemit.emit` 也会先经过同一 gate。实际 `case02` 全 stdlib Source、原 6178 二进制、stage2/3 和完整 corpus 均未在本任务中重跑；6178 的旧行为保持原样，未标记为已修。
