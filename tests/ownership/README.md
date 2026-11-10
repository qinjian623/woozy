# T09 所有权与 Vec fixtures

operation_id: `language-t09-ownership-fixtures-20261104`
correction_operation_id: `language-t09-ownership-fixtures-20261104-correction`
root_evidence_correction_count: `1`
契约版本：`compiler-dev/1`
根版本：`5827dce`（初版目录准备于 `e808169`）

本目录保存 T09 的独立源码项目、静态/运行预期和当前执行的入口。`cases.json` 的 `schema` 字段只是数据格式标签。初版 18 个项目的历史检查记录保留在 `current_check`；2026-11-09 追加 13 个经过 Vec 的递归值项目，原 18 份源码和 manifest 未改。

初版 task 已 rejected，不作为 accepted dependency。初版 root 证据审查后只做一次修正：`defer-before-destroy` 使用显式 `i64(values[0])`；`for-in-element-copy` 增加写后 `value` 断言并让首轮修改后续槽。其余 16 份源码和 18 份 manifest 与初版快照逐字一致。

当前 32 个项目的 `expected_static` 由 `sema.t09` 实际执行；Native 和 Python ownership runner 读取同一清单，构建/运行、真实 alloc/free 与语言引用计数观察由 `native.ownership`、`python.ownership` 执行。新增 `recursive-for-in-early-exits` 检查递归元素遍历的 continue、break、return 和 typed throw。`recursive-generic-growth` 以固定预算 `E_CAPABILITY_UNAVAILABLE` 拒绝；三个无限按值布局项目保持 `E_TYPE_MISMATCH`；`recursive-cycle-retained` 不要求零存活，必须单独观察语言 refs 保持非零。

## 布局与计数

```text
tests/ownership/
  README.md
  cases.json
  fixtures/
    <case-id>/lang.toml
    <case-id>/src/main.wz
```

`case_count = 32`；静态应接受 `accepted_count = 21`（包含运行时 Panic 的 `panic-vec-index` 与有意保持存活的 `recursive-cycle-retained`），静态应拒绝 `reject_count = 11`。初版项目使用包名 `main`；递归值批次沿用项目清单的 `package app;`。每个项目都是独立 `lang.toml` 项目。

初版修正只改了两份源码：

- `defer-before-destroy/src/main.wz`：`log * 10 + values[0]` 的 i64/i32 混宽改为 `log * 10 + i64(values[0])`；未来目标仍是 defer 先于局部 Vec 销毁，`log=17`。
- `for-in-element-copy/src/main.wz`：写当前槽后断言本轮 `value` 未变；首轮写 `values[1]=20`，后续迭代必须读取新值。正确迭代值为 1/20/3，`sum=24`，最终 `values=[11,30,13]`。

其余 16 份源码和 18 份 manifest 保持初版逐字不变；逐项 hash 见 `cases.json`。

`cases.json` 为每个案例保存：

- `source_sha256` 和 `source_bytes`；
- 规范出处 `specification`；
- 未来静态 `expected_static`；
- 未来运行 `runtime_expected`（全部 `status = not_run`）；
- 当前实际检查摘要 `current_check`（语法与完整 check 的退出码/诊断）。

## 规范依据

- `docs/SEMANTICS.md` §5：实参从左到右、move 在求值时转移、后续实参失败不恢复源绑定、返回结果先保存；
- `docs/SEMANTICS.md` §7.1：Vec 共享容器、索引写入别名可见、切片新容器与元素逻辑 copy、越界 Panic、for-in 入口长度/每轮元素拥有值/活跃遍历期间元素写入可观察；
- `docs/SEMANTICS.md` §7.2：普通参数不消费、返回/存储取得 copy/retain、显式 move、移动后未重建不可读、普通参数借用不可 move、inout 首版限制；
- `docs/SEMANTICS.md` §7.3：copy/move/borrow/destroy 与 retain/release 的内部边界，计数次数不是源语言可观察值；
- `docs/SEMANTICS.md` §8：Panic 不可由 typed catch 恢复，不保证 Panic 后清理；
- `docs/SEMANTICS.md` §9：块退出先 LIFO defer，再逆声明销毁仍拥有值的局部；被移动值不销毁；defer 读取已移动且未重建绑定应报所有权错误；
- `docs/ACCEPTANCE.md` A07 所有权和 ARC；
- `docs/BACKENDS.md` §5 ARC 与原生运行库接口、§6 Python 降级要求；
- `DEVELOPMENT.md` D10（Vec 共享引用）与阶段未覆盖使用 `E_CAPABILITY_UNAVAILABLE`。

没有使用未冻结的标准库方法名。Vec 的长度修改 API 名称没有在冻结规范中给出，本目录不发明 `push`、`append`、`truncate` 等源 API；活跃遍历长度修改的正例和“修改前 Panic”反例留作 TODO，见 `observation_policy.length_modification`。

## 正例的未来预期

以下 `main_return`、返回值、log 和观测值都是 T09 实现后的目标，当前全部为 `not_run`。源码只能直接观察整数返回、log 和分支结果；不能从 `main` 返回 0 推出析构/retain/release 计数。

| id | 未来静态 | 未来运行目标 | 要点 |
| --- | --- | --- | --- |
| `vec-shared-index` | accept | `main=0`；`left[0]=9`, `right[0]=9` | 两个 Vec 绑定共享容器，索引写入对所有别名可见 |
| `slice-copy-scalars` | accept | `main=0`；`base[1]=2`, `part=[9,3]` | 切片是新 Vec；i32 元素逻辑复制，不改 base |
| `slice-shares-elements` | accept | `main=0`；`inner[0]=7`, `outer[0][0]=7` | 切片复制 Vec 元素句柄，内层容器仍共享 |
| `struct-copy-shares-vec` | accept | `main=0`；`original.marker=3`, `original.items[0]=8`, `inner[0]=8` | struct 复制标量字段；Vec 字段共享容器 |
| `ordinary-param-return` | accept | `main=0`；`answer=5`, `values[0]=6` | 普通参数不消费；返回 Vec 参数取得共享 copy/retain |
| `move-reinitialize` | accept | `main=0`；`first=1`, `values[0]=7` | move 后重新初始化再读合法；旧容器由被调用方负责销毁 |
| `branch-reinit-merge` | accept | `main=0`；`choose(true)=2`, `choose(false)=1` | 一侧 move 后重建、另一侧保留，合流后两条路径都有效 |
| `defer-before-destroy` | accept | `main=0`；`result=0`, `log=17` | defer 在局部 Vec 仍存活时用显式 `i64(...)` 读取；defer 后才销毁 |
| `error-arg-interrupt` | accept | `main=0`；事件 `[2,3]`, `log=23`；禁止 `9,8` | 第一个实参 move 后，第二个实参抛 E；第三个不执行，源绑定不恢复 |
| `for-in-element-copy` | accept | `main=0`；`iteration_values=[1,20,3]`, `sum=24`, `values=[11,30,13]` | 写后 `value` 不变；首轮改 `values[1]=20`，后续迭代必须读到 20，不能用整表快照 |
| `panic-vec-index` | accept | 无正常返回；Vec 越界 Panic | 长度 1、动态索引 1；Panic 不可 typed catch，清理无保证 |

### `panic-vec-index` 的观测限制

`probe` 注册了一个 defer 后执行 `values[index]`，其中 `index=1`、`len=1`。未来只能断言发生 Vec 越界 Panic、没有正常返回；`SEMANTICS.md` 只规定“越界 Panic”，没有冻结 Vec 越界的原因码、诊断文本或进程退出状态，因此 `reason_code` 为 `null`。按 §8，Panic 后不保证执行语言 defer 或析构，不能断言 `log=1` 或任何 drop 计数。

## 反例的未来预期

`contracts/diagnostic_codes.json` 已登记 `E_OWNERSHIP_USE_AFTER_MOVE`，`contracts/test-cases.json` 的 use_after_move 也固定此码；5个已移动绑定反例沿用该码。普通参数借用不可直接 move 没有固定专用诊断，`code` 保留 `null`；inout Vec 使用已公布的 `E_CAPABILITY_UNAVAILABLE`。本目录不新增诊断码。

| id | 未来静态 | 规范依据 | 目标 |
| --- | --- | --- | --- |
| `reject-read-after-move` | reject，`E_OWNERSHIP_USE_AFTER_MOVE` | §7.2；A07 | `take(move values)` 后未重建就读 `values[0]` |
| `reject-double-move` | reject，`E_OWNERSHIP_USE_AFTER_MOVE` | §7.2；A07 | 同一绑定在重建前第二次 `move` |
| `reject-move-ordinary-param` | reject，code `null` | §7.2；A07 | 普通参数 `values` 只是借用，`take(move values)` 不允许 |
| `reject-defer-moved` | reject，`E_OWNERSHIP_USE_AFTER_MOVE` | §9；§7.2 | defer 读取已被 move 且未重建的 `values` |
| `reject-error-does-not-restore` | reject，`E_OWNERSHIP_USE_AFTER_MOVE` | §5；§7.2；A07 | 后续实参抛错后，catch 读取先前 move 的源绑定 |
| `reject-inout-vec` | reject，`E_CAPABILITY_UNAVAILABLE` | §7.2；A07 | 首版 inout 拒绝共享 Vec，不把槽独占推导成深层 noalias |
| `reject-branch-maybe-moved` | reject，`E_OWNERSHIP_USE_AFTER_MOVE` | §7.2；A07 | 一侧 move 后未重建、另一侧保留，合流读取可能已移动绑定 |

## 递归值批次（2026-11-09，根版本 5827dce）

新增项目保存于 `tests/ownership/fixtures/recursive-*`：原始审查 Source 的逐字副本、copy/move/slice/for-in、struct/enum 互相回指、Python Vec 描述顺序、两组 `Node[T]` 实例与周期交换 `Flip[A,B]`、未使用和实际使用的增长泛型、typed-error 清理顺序、三个无限布局反例，以及整元素替换形成的强引用自环。实际命令、工具/源码 SHA、变异名称和 alloc/free 指针记录在项目外层 `checks/recursive-values-flash-20261109/`。

定向运行示例：

```sh
python3 tests/native/run_ownership.py ... --case recursive-finite-tree --case recursive-cycle-retained
python3 tests/python/run_ownership.py ... --case recursive-finite-tree --case recursive-cycle-retained
```

Native observer 链接额外使用 `--wrap=lang_alloc_v1` / `--wrap=lang_free_v1`，记录真实 pointer 并调用真实 allocator；`VEC:5` 只表示释放路径开始，不再单独作为物理 free 已完成的证明。普通与 typed-error 项目要求最终 heap live 为空；自环项目要求两个真实分配仍存活、Vec 语言 refs 非零且没有最后释放。

## 当前实际检查（2026-11-04，根版本 e808169；历史记录）

使用根仓库现有 C++ CLI：

```sh
woozyc check --syntax-only --format json tests/ownership/fixtures/<id>/src/main.wz
woozyc check --project tests/ownership/fixtures/<id>/lang.toml --backend native --format json
woozyc check --project tests/ownership/fixtures/<id>/lang.toml --backend python --format json
```

实际二进制为 `/home/qin/Work/language-dev-20261103/compiler/build/native-cli/langc`，版本 `0.1.0-bootstrap (compiler-dev/1)`，源码版本 `e808169`。逐项 argv、绝对路径、退出码和原始 native/python JSON 保存在 `/tmp/language-t09-ownership-fixtures-correction-20261104/checks.json`；`/tmp` 中的机器记录不属于本目录。

| 检查 | 实际结果 |
| --- | --- |
| `cases.json` JSON 解析、manifest/source 引用 | 18/18 可读取，计数 18/11/7 与案例数一致 |
| 18 个源码的 `source_sha256` | 已逐项记录在 `cases.json`；两份修正源码已重算，其余 16 份与 `checks/t09-fixtures-root/initial/candidate` 逐字一致；18 个 manifest 未变 |
| `check --syntax-only --format json` | **18/18 passed**：退出 0、`status=ok`、`diagnostics=[]` |
| 当前完整 `check --project --backend native` | **18/18 rejected**：退出 1，`E_CAPABILITY_UNAVAILABLE`，phase `type`，message `container, owned string and Result support requires T08/T09` |
| 当前完整 `check --project --backend python` | **18/18 rejected**：退出 1，`E_CAPABILITY_UNAVAILABLE`，phase `type`，同一 message |
| 未来 T09 类型/所有权检查 | `not_run` |
| 未来 native/Python 生成与执行 | `not_run` |
| 未来 drop/retain/release 计数与顺序 | `not_run` |

当前完整 check 的 native/python 各 18 个 `E_CAPABILITY_UNAVAILABLE` 都发生在文件中的第一个 `Vec` 类型，不是未来反例的预期诊断，也不是正例的所有权通过。不得据此把任何案例标成 T09 已实现或运行通过。`cases.json` 的 `current_check` 记录 native；逐项 Python 原始输出在 `/tmp/language-t09-ownership-fixtures-correction-20261104/checks.json`。

两处修正只解决普通 i64/i32 显式转换和加强 for-in 的可观察断言；它们通过语法检查且未来预期自洽，**不代表 Vec、所有权、后端或运行已经实现**。

## 析构、retain/release 与未观测项

源码没有用户定义析构函数、显式 `drop`、`retain/release` 或引用计数 API，因此：

- `main_return = 0` 只表示未来运行路径上的整数结果，不是析构计数或生命周期证明；
- `cases.json` 中 `future_observations.drop_order`、`retain_release`、`cleanup` 全部为 `not_run`；
- 精确 retain/release 次数没有冻结，优化可以消除冗余操作，不能要求 O0 与 O2 的内部计数相同；
- 需要未来 runtime 观察、失败注入或地址检查器验证对象生命周期；仅凭静态 IR 快照或返回值不能证明无泄漏、无双重释放；
- `panic-vec-index` 的 Panic 路径按 §8 不保证清理，不能把未执行的 defer/drop 记成通过。

## 未覆盖与资料缺口

1. Vec 长度修改方法的名称未冻结：没有写 `push`、`append`、`truncate`、`remove` 等调用；活跃遍历中“修改长度必须在修改前 Panic”的正例/反例留作 TODO。
2. 冻结文本只说 Vec 索引是“非负整数”，没有单独冻结索引表达式的具体整数类型；`for-in-element-copy` 和 `panic-vec-index` 使用 `i32` 动态索引。如果 T09 接口决定只接受某一具体整数类型，应先更新规范并复核这两个案例，不静默改预期。
3. 普通参数借用不可直接 move 尚无固定专用诊断，其 code 为 null；5个已移动绑定反例已使用既有 E_OWNERSHIP_USE_AFTER_MOVE，不把已公布的码误列为缺项。
4. 没有单独的 enum 活动 payload 销毁或“部分构造失败只逆序销毁已完成成员”项目；这需要能在构造实参中抛错并观察已完成成员 drop 的运行期支持，留作后续测试。
5. 没有单独覆盖 A07 的借用逃逸、同一存储同时作为 inout 与另一访问、受限借用期间销毁或移动拥有者；本批 18 例优先用户指定的 Vec/move/defer/error/Panic 主路径。
6. 初版 18 例没有 runtime 记录；2026-11-09 的递归值批次已由 Native/Python runner 实际执行并记录 alloc/free、drop 元素逆序和语言 refs。初版案例也随之由同一 runner 覆盖，但历史 `current_check` 仍只表示 `e808169` 的拒绝结果。
7. 有意保留的语言强引用自环已由专用进程/observer 覆盖；没有实现循环回收、弱引用、线程、捕获闭包或外部 Python/C++ 对象所有权。自环结果不能写成“已回收”或“GC 已实现”。
8. 没有把当前 `E_CAPABILITY_UNAVAILABLE` 当作未来语义通过，也没有为适应当前 checker 修改未来预期或规范。

root 集成补充：诊断码采用既有公开定义，18份源码与修正交付逐字一致；独立复核及实际检查保存在外层 checks/t09-fixtures-root/correction/ 和 checks/t09-fixtures-independent/。源码数值、事件及生命周期目标仍未执行。
