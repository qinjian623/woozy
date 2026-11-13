# 自举语料执行器

状态日期：2026-11-14。本文件说明 `selfhost/corpus.py`、`selfhost/corpus.json` 和
`tests/selfhost/corpus_test.py` 的实际范围。它是进行中的执行器，不是完整自举或全语料
通过证明。

当前清单共 1250 项，启用 881、未启用 369。Native 161 项、Python 65 项、迭代器等双后端 178 项、闭包等 Native 98 项均已独立复测并合入；原 285 项记录仍保留。线程池另 11 项已按参考记录合入。590 个文件尚无已启用用例，324 项仍需改写运行方式，99 个复杂 runner 尚未接入。额外诊断字段只记录来源，执行器仍以两版完整 JSON 对照判断，尚未单独检查这些字段。

2026-11-14 的 tracked 再冻结已把已接受提交中的已跟踪源码写入清单：文件记录 2672→2837（新增 165、修改 10、删除 0），runner 211→233，case 1234→1250（新增 16 个 disabled component case）。原 881 个 enabled 用例逐字段未变。线程池 runner 的分类已从 adaptation_required 改为 public_cli；Python 批次追加 131 条文件分配，最新双后端批次另追加 178 条。执行器仍不把参考记录当作完整候选编译器通过；完整候选语料与第二、三代自举未运行。以下旧批次的数量和命令记录描述生成时的状态。

最新 178 项来自 89 个项目，每项保留 Native/Python 两个独立 ID，并追加 178 条文件分配。另 98 项中，78 项执行了 Native O0/O2，20 项只做静态检查。11 个浮点项目的 main 没有数值比较，因此这些记录只证明能编译并运行到入口返回，不能证明舍入、NaN/Inf、非正规数或 FMA 规则；原数值观察测试仍未接入。

## 1. 已检查的内容

- 六项独立检查缺陷已修正：超时/信号失败、超时清理有界、失败构建全发布物检查、
  runner/case 分类缺失失败、有效运行断言校验、严格 JSON 类型比较。
- catalog 已按 Root 当前 HEAD `3d7139bc10cab5d62cb4e04c614db8d86e5ee871`
  重新冻结：1198 个 `.wz`、984 个 manifest、211 个 runner 源文件。
- 其中 `012ee28` 新增的 28 个应冻结路径、`3d7139b` 修改的 7 个旧文件新 hash 已写入
  catalog 的 `refreeze` 字段。
- Root 工作区在冻结时仍有 97 个未跟踪的进行中组件文件和 1 个已修改的已跟踪
  文件 (`contracts/selfhost.json`)。catalog 只冻结 `git ls-files` 的已跟踪路径；
  这些未跟踪文件没有盲当已接受源码。因此 `formal_freeze_ready=false`。
- 2026-11-13 本轮把 285 个此前 `public_cli`/`adaptation_required` 的 project case
  补成可执行预期：271 个 generics 静态/运行 case 和 14 个原先缺运行预期的
  accept case。当前 368 enabled（83 旧 + 285 新）、712 disabled、683
  `adaptation_required`、0 个已发布 accept 缺运行预期、100 个复杂 runner 仍待适配。
- 5 个无法从当前 runner 证明的 manifest 明确留在 `not_ready`：2 个 vendored
  dependency 改为 component，3 个孤儿 fixture 保持 required；没有用批量标签
  掩盖缺口。
- 全语料、完整自举、stage1/2/3、完整 Source corpus、Native 候选 O0/O2 正例仍为
  `not_run`。本轮只把 stage0 当参考编译器抓取真实预期，没有把 stage0-as-candidate
  的 build 当通过。

### 1.1 本轮精确启用范围

新增 285 个 enabled case，全部带 `evidence`、真实 argv/exit/stdout/stderr 记录和
源文件 hash。完整精确 ID/预期在
`checks/SH10f-corpus-public-cli-20261113/summary.json` 的 `results[].id`；
逐命令证据在 `checks/SH10f-corpus-public-cli-20261113/evidence.jsonl`。

| 组 | 数量 | 预期 |
|---|---:|---|
| generics 家族 271（255 个 `tests/generics` + 16 个 `tests/native`） | 164 build OK + 70 build reject + 23 check OK + 14 check reject | 运行 exit 0/1/3/5/6/7/8/9/11/21 或 Panic 101；拒绝 exit 1 且无发布物 |
| 原缺预期 14 | 6 build panic + 2 build OK + 6 check OK | 6 个 Panic 101/原因前缀；2 个真实执行 exit 0；6 个只做静态 check |

这 271 个 case 来自 14 个 generics runner 和 `tests/native/run_generics.py` 的显式集合，而不是按目录名猜：
`run_aggregate_inout.py`、`run_builtin_impl.py`、`run_constrained_inout.py`、
`run_generic_traits.py`、`run_indexed_calls.py`、`run_inout.py`、
`run_list_inference.py`、`run_method_inout.py`、`run_method_move.py`、
`run_move.py`、`run_nested_patterns.py`、`run_receiver_modes.py`、
`run_self_modes.py`、`run_unbound_methods.py`，以及 `tests/native/run_generics.py`
的 `tests/native/{generics,pod,pod-reject}`、`positional` 和 `cases.json` 正例。
名字含 `reject` 但 runner 明确当正例的 case（如
`aggregate-inout/reject-alias/field-place`、`method-move/trait-generic-move-reject`、
`self-modes/reject-index-inout`、`generic-traits/reject-own-generic` 的 check-only）
按 runner 实际断言处理，没有用文件名批量改标签。

14 个原缺运行预期的精确 case：

```text
project:tests/backend_common/fixtures/panic-division-by-zero/lang.toml
  build_native, panic exit 101, prefix LANG_NATIVE_PANIC:v1:division-by-zero:
project:tests/backend_common/fixtures/panic-dynamic-conversion/lang.toml
  build_native, panic exit 101, prefix LANG_NATIVE_PANIC:v1:integer-conversion:
project:tests/backend_common/fixtures/panic-invalid-shift/lang.toml
  build_native, panic exit 101, prefix LANG_NATIVE_PANIC:v1:invalid-shift:
project:tests/backend_common/fixtures/panic-negative-shift/lang.toml
  build_native, panic exit 101, prefix LANG_NATIVE_PANIC:v1:invalid-shift:
project:tests/cleanup/fixtures/case-10/lang.toml
  build_native, panic exit 101, prefix LANG_NATIVE_PANIC:v1:division-by-zero:
project:tests/ownership/fixtures/panic-vec-index/lang.toml
  build_native, panic exit 101, prefix LANG_NATIVE_PANIC:v1:vec-index:
project:tests/modules/fixtures/cross-package-public/lang.toml
project:tests/modules/fixtures/nested-shadow-and-for-init/lang.toml
project:tests/modules/fixtures/same-package-mutual-recursion/lang.toml
project:tests/modules/fixtures/self-in-context/lang.toml
project:tests/sema/fixtures/call-labels-and-function-value-accept/lang.toml
project:tests/sema/fixtures/inout-writable-var-accept/lang.toml
project:tests/sema/fixtures/numeric-boundaries-accept/lang.toml
project:tests/sema/fixtures/var-assignment-accept/lang.toml
  cross-package-public / same-package-mutual-recursion 走 tests/native/run_cli.py 的真实
  build+execute（exit 0）；nested-shadow-and-for-init / self-in-context 的原 runner 只
  编译不执行，改为 check；4 个 sema 是静态 accept，也改为 check。均不伪造 run
  expectation。
```

明确未启用、保留在 `not_ready` 的 5 个：

```text
project:tests/generics/fixtures/cross-package-alias/vendor/dep/lang.toml
  component: vendored dependency, 只通过父 case 执行
project:tests/generics/nested-patterns/literal-cross-package/vendor/dep/lang.toml
  component: vendored dependency, 只通过父 case 执行
project:tests/generics/inout/constrained/reject/lang.toml
  required: 当前 runner 不引用；真实 check 已是 accept，不能继续叫 reject
project:tests/generics/move/constrained-reject/lang.toml
  required: 当前 runner 不引用，没有可恢复的断言
project:tests/generics/unbound-method-values/reject-capture/lang.toml
  required: run_unbound_methods.py 不引用；owning_inout/cases.json 的
  reject-capture 是另一个 manifest
```

## 2. 六项修正

### 2.1 编译器/程序超时或信号死亡必须失败

`run_one` 现在无条件把以下情况加入 failure，即使两边输出合法 JSON、退出码相同：

- `timed_out=true`；
- 编译器退出码为负（被信号结束）；
- 清理未完成或管道被放弃。

程序执行同样检查 `timed_out`、负退出码和 `cleanup_complete`。Panic 预期不会接受
harness 的超时或 SIGKILL。回归探针 `compiler-timeout`、`compiler-signal`、
`execution-timeout` 均为 `case_status=failed`。

### 2.2 超时清理有明确期限

`_run_process_impl` 不再在 killpg 后调用无 timeout 的 `communicate()`。它用
selector 持续读取 stdout/stderr，超时后 killpg，再用固定 `CLEANUP_TIMEOUT=0.5s`
清理；仍持有 pipe 的脱离进程组子进程会导致 `cleanup_complete=false`、
`abandoned_pipes=true`，case 失败，但不会无限等待。已收集输出保留。

独立 `timeout_detached_child` 探针配置 0.1 秒，1.2 秒后自然结束的脱离子进程不再拖住
harness；实测 0.601 秒返回，`timed_out=true`，stdout 保留 `ready\n`。

### 2.3 失败构建检查所有新增/改变发布物

Native 发布物检查现在扫描 `out/` 下全部条目，包括 `program.o`、目录、符号链接、
非普通文件。失败构建只要新增或改变任何条目即失败。成功构建要求新发布：

- Native：`program`；
- Python：`program.py`、`langrt.py`。

`preexisting_outputs` 用例会先写入显式已有文件并记录 hash/类型；运行后任何已有文件
缺失、内容或模式改变都会失败。未改变的预置文件不算失败发布物。

回归探针 `partial-object` 现在记录 `published=["program.o"]` 且 case 失败；单测中的
“保留已有输出”和“改变已有输出”正反控制分别通过/失败。

### 2.4 runner/case 分类缺失或非法时不得 complete

- runner 状态只允许 `public_cli`、`component`、`adaptation_required`、`unmapped`；
- 缺失/未知状态、重复 runner、未分类 `runner_source`、非 runner 路径进入 runner 表
  都是完整性错误；
- 所有 runner 分类必须有非空 reason，`component` 尤其不能只删待适配记录；
- case 必须有合法 `enabled`、`support`、`adaptation`；有 runner 时必须有合法
  `runner_status`；启用 case 必须是 `public_cli` + `adaptation=ready`；
- `unmapped`、`unclassified` runner 以及非法 case 分类都会使 coverage 产生 gap，
  不会 exit 0。

### 2.5 run_expectation 必须有实际、可检查的断言

允许字段固定为 `exit`、`status`、`stdout_base64`、`stderr_base64`、
`panic_stderr_prefix`。要求：

- `exit` 必须是真正 int；
- `status` 只允许 `panic`，默认要求 exit 101，不能是 0；
- 字节预期必须是合法 base64；
- `panic_stderr_prefix` 只能配合 panic，且非空；
- 未知键、空对象、`{"todo": ...}`、没有有效断言的字典都无效。

`check_run_expectation` 会实际核对 exit、stdout/stderr 字节、panic 约束，以及超时、
信号和清理失败。独立探针给出的 `exit99 + 错误 stdout/stderr` 不再被空失败列表掩盖；
单测覆盖有效 exit99/字节预期通过、错误输出失败。

### 2.6 JSON 类型严格比较

- `schema_version` 必须是 Python `int` 且等于 1；`true` 和 `1.0` 都被拒绝；
- 参考/候选完整 JSON 始终通过 `json_diff` 逐字段比较，不再先用 Python 字典 `==`
  决定是否比较；
- Bool/Int、Int/Float 类型不同会产生差异；原始 diagnostics 正文、数组顺序、span
  字节仍全部保留比较；
- 非标准 JSON 常量（NaN/Infinity）也拒绝。

独立探针 `json-schema-type` 现在为 `case_status=failed`，候选 `schema_version=true`
被报告为 malformed；单测还直接验证 Bool/Int、Int/Float 的 `json_diff`。

## 3. Root 重新冻结

catalog 来源：`/home/qin/Work/language-dev-20261103/compiler`，只读。

```text
previous_git_head: 191ccbddef99ffe56a86291257f6c2bc52db1a7b
git_head:          3d7139bc10cab5d62cb4e04c614db8d86e5ee871
new_freeze_paths:  28（18 Wz + 3 manifest + 5 runner + 2 binding JSON）
modified_paths:    7
tracked files:     2672（含 1198 Wz、984 manifest、211 runner source）
```

`refreeze.new_freeze_paths` 精确列出 012ee28 新增的 28 项，包括
`selfhost/src/wzcc/{bindings,constant,literal,nativebuild,ownership,project,valueops}`、
`selfhost/tests/{bindings,constant,project}` 的 manifest/runner/Wz 和两个 binding JSON。

```text
selfhost/src/wzcc/bindings/bindings.wz
selfhost/src/wzcc/constant/constant.wz
selfhost/src/wzcc/literal/literal.wz
selfhost/src/wzcc/nativebuild/nativebuild.wz
selfhost/src/wzcc/ownership/analysis.wz
selfhost/src/wzcc/ownership/complete.wz
selfhost/src/wzcc/ownership/equality.wz
selfhost/src/wzcc/ownership/errors.wz
selfhost/src/wzcc/ownership/ownership.wz
selfhost/src/wzcc/project/project.wz
selfhost/src/wzcc/valueops/valueops.wz
selfhost/tests/bindings/fixtures/c_events.json
selfhost/tests/bindings/fixtures/c_other.json
selfhost/tests/bindings/lang.toml
selfhost/tests/bindings/run.py
selfhost/tests/bindings/src/main.wz
selfhost/tests/constant/lang.toml
selfhost/tests/constant/regression/src/main.wz
selfhost/tests/constant/run.py
selfhost/tests/constant/src/main.wz
selfhost/tests/nativebuild/main.wz
selfhost/tests/nativebuild/run.py
selfhost/tests/ownership/independent/main.wz
selfhost/tests/ownership/run.py
selfhost/tests/ownership/src/main.wz
selfhost/tests/project/lang.toml
selfhost/tests/project/run.py
selfhost/tests/project/src/main.wz
```

`refreeze.modified_freeze_paths` 记录 `3d7139b` 修改的 7 个旧文件及 old/new SHA-256：

| 路径 | 新 SHA-256 |
|---|---|
| `selfhost/src/wzcc/input/input.wz` | `e29bd5a4e0f33d903011727519c6ef95bbce2e69262334d1466396da313b6fe8` |
| `selfhost/src/wzcc/paths/paths.wz` | `67b4236be406335340f38b362065634cca1eeaf28279290344e4a0c4c6c6498e` |
| `selfhost/tests/input/project/src/main.wz` | `86f3d5765a1273c899cfd47e6394492d2f40fa18f54e8c926d2e99abc6828dca` |
| `selfhost/tests/input/run.py` | `1a91b21eaa1873e42793a9a9d5491e59bc0ba5b6be6838ddb613f66cbbd5104a` |
| `selfhost/tests/paths/project/src/main.wz` | `dca46e08d90c497d7a22b4bdfde06e567ba7510e6b7087273d680b082bf0d59c` |
| `selfhost/tests/paths/run.py` | `f5063956202563cd73e2b5877213c0a14464a868e686a833554f499af75f2df2` |
| `tests/selfhost/CMakeLists.txt` | `348186bba1c88a743a892d71a65afbcfa0e2aa76a8820ffc12a2f24ed17128e8` |

当前 catalog/执行器/单测/派生脚本 SHA-256：

```text
selfhost/corpus.json      f560725a0cfd9a198c874db4bd96fd3895f336cef57039c5fc1b3781300cb019
selfhost/corpus.py        a154f348ae0e503370c29f848231e78767e4c2cea55a4f4dbbdcb5be2a31a9f8
tests/selfhost/corpus_test.py
                          843e82420c297f4119f9b7e8021b73458b18ad171d9f4c0bbf2634e77d62012c
checks/SH10f-corpus-public-cli-20261113/derive_public_cli.py
                          5951cff30fb82dfcee6d51389fec0e1c5e9993a23fb1662b65c5251f93f08c9d
```

正式冻结前提：所有已接受源码必须已提交，或加入明确的已接受清单并逐字节冻结；
已跟踪工作区必须没有冻结范围内的修改；未跟踪的进行中组件不能算入 catalog。
当前 Root 不满足该前提，所以 `freeze` 只给出 `TRACKED-ONLY PASS`，同时报告：

```text
formal freeze ready: False
untracked corpus files excluded from catalog: 97
dirty tracked corpus files: 1 (contracts/selfhost.json)
```

`.work-tmp/`、`build/`、`reports/` 和 `.build/` 是明确排除的临时/构建目录；其余
未跟踪 corpus 路径全部列入 coverage gap，不作为已接受源码。正式执行前应提交这些
已接受模块，或提供独立、显式的已接受清单，然后重新 refreeze。

## 4. 执行、追踪与比较

公开 CLI 参数不变；参考/候选比较、strace 白名单、诊断保留规则保持上一版：

- 候选只允许自身 + 固定 LLVM 的 `opt/llc/clang/clang++/ld.lld`；
- 参考侧才允许显式 stage0 helper；
- `execve/execveat` 都解析，空路径 `execveat` 失败关闭；
- 程序执行使用独立 `execute.exec.log`，不进入编译白名单；
- 只归一化运行临时目录、已知仓库根和顶层 `compiler_version`；
- 原始 stdout/stderr/argv/input hash/trace/artifacts 全部保留。

输出布局仍为：

```text
<out>/result.json
<out>/inventory.used.json
<out>/runs/<case>/<variant>/reference/{argv,input,exec,stdout,stderr,artifacts,diagnostics,out}
<out>/runs/<case>/<variant>/candidate<N>/{同上}
```

## 5. 本轮实际命令与结果

参考编译器是只读 Root 的 C++ stage0：
`build/woozy-stdlib-native/woozyc`，SHA-256
`6f104ccc6d99540f1888e77eb38ca1b0024fbc6f002fca89f06138c8ce0d908d`。
环境使用真实 LLVM 22.1.8、libxml2 legacy、`TMPDIR=/tmp/sh10-corpus-tmp`；没有
写 Root，没有运行候选自举编译器。

```bash
python3 checks/SH10f-corpus-public-cli-20261113/derive_public_cli.py \
  --evidence checks/SH10f-corpus-public-cli-20261113/evidence.jsonl \
  --write
# exit 0, enabled=285, not_ready=5
# 1113 条真实命令记录：285 check-native, 242 build-O0, 242 build-O2,
# 172 execute-O0, 172 execute-O2
# evidence: ff5cb64c...bc6f（1.4 MiB）
# summary:  72abc535...c441
```

脚本先按 runner 显式集合选定 case，再执行真实 CLI 并核对结果；只有观测字节与
runner 断言一致才写入 inventory。Positive 的 `run_expectation` 保存真实 exit 和
空 stdout/stderr（`base64`），Panic 保存 exit 101、空 stdout 和稳定原因前缀；拒绝
保存 exit 1/status error/no_stderr/完整 diagnostic span 要求。每条证据保留 argv、
cwd、exit、stdout/stderr 全文 base64 与 SHA-256，以及该 case 源文件 hash。

```bash
python3 selfhost/corpus.py coverage
# exit 1, COVERAGE: INCOMPLETE
# cases: total=1080 enabled=368 disabled=712 adaptation_required=683
# files_without_enabled_case=876
# missing_run_expectation=0
# runners_require_adaptation=100
# 无 untracked_corpus_files
# error: frozen hash mismatch: contracts/selfhost.json（见下）
```

与旧快照对比：83→368 enabled，997→712 disabled，970→683
`adaptation_required`，14→0 个已发布 accept 缺运行预期，
1174→876 个文件没有 enabled case。100 个复杂 runner 未减少，仍未把它们的
私有 helper/probe 工作算完成。

小测试不在脏 Root 上放宽 hash；它使用 3d7139b 的临时 clone，以及只把
`contracts/selfhost.json` 一个已知变化文件 hash 更新到该 clone 的临时 inventory；
冻结 inventory 本身没有刷新真实源码 hash：

```bash
git clone --shared /home/qin/Work/language-dev-20261103/compiler /tmp/sh10f-root-3d7139b
git -C /tmp/sh10f-root-3d7139b checkout 3d7139bc10cab5d62cb4e04c614db8d86e5ee871
# test copy: /tmp/sh10f-inventory-test.json（只改 contracts/selfhost.json hash）
python3 tests/selfhost/corpus_test.py \
  --root /tmp/sh10f-root-3d7139b \
  --inventory /tmp/sh10f-inventory-test.json \
  --stage0 /home/qin/Work/language-dev-20261103/compiler/build/woozy-stdlib-native/woozyc \
  --toolchains /home/qin/Work/language-dev-20261103/toolchains
# exit 0
# PASS: strict JSON, timeout/signal, cleanup, artifacts, expectations,
#       coverage, public-CLI harness expectations, stage0 harness, and
#       legacy-helper rejection
```

本轮**没有**运行 `corpus.py run --scope all`：368 个 case 需要真实候选编译器做
reference/candidate 全 JSON 对比；stage0 作为候选会调用旧 native helper，被 trace
白名单拒绝，不能算通过。全量 run 明确记为 `not_run`。Harness 6 项自测在单测中
仍通过（含 Python 3.12 build/execute 与候选 helper 拒绝探针）。

## 6. 保留的缺口

- 712 个 disabled case 未执行；876 个源文件仍没有 enabled case。
- 683 个 case 仍 `adaptation_required`；100 个 runner 仍待适配。
- 285 个新 enabled case 的 Native public-CLI 部分已覆盖；原 runner 的私有
  ownership probe/IR observer、Python 事件观察、helper metadata digest 对比仍是
  单独缺口。`runner_gaps.json` 逐项记录 100 个 runner 的剩余工作。
- Python 3.12 只由现有 harness 自测覆盖；本轮 285 个新 case 是 Native build 或
  Native check，271 个 generics case 的 Python backend 分支尚未拆成独立 corpus case。
- Native 候选 O0/O2 正例仍 `not_run`：stage0 会调用旧 native helper，被候选 trace
  白名单拒绝。
- 完整 stage0→1→2→3 自举、完整 Source corpus、公开完整 compiler 均未执行。
- inventory 冻结在 3d7139b；工作区 `contracts/selfhost.json` 已是另一字节快照，
  coverage 因而继续报告一个 frozen hash mismatch。按任务要求没有刷新真实源码
  hash；正式 freeze 仍待全部接受源码提交后统一重冻。

## 7. 待适配 runner 精确清单

以下 100 项保留线程池合入前的历史清单；其中 `tests/native/threadpool/run_threadpool.py`
已在 2026-11-14 改为 public_cli，当前仍为 adaptation_required 的有 99 项。完整原因和
其余 component/public_cli 分类见 `selfhost/corpus.json`。旧逐项 `required` 字段在
`checks/SH10f-corpus-public-cli-20261113/runner_gaps.json`，当时按真实源码扫描归类：

| 类别 | 数量 | 剩余改动 |
|---|---:|---|
| `private_helper` | 37 | 用 public build 产物替代 `lang_native_helper` 的 object/IR/metadata 生成，并保留失败构建产物检查 |
| `private_probe` | 20 | 用 public CLI JSON + 程序执行替代 compiler probe / Vec observer / ownership observer |
| `static_public_cli_migrated_private_observation` | 16 | 静态 build/check/run case 已启用；只剩私有事件/分配/pairs/Python metadata 观察 |
| `public_cli_split` | 15 | 把 check/build/execute 拆成独立 case，补真实 exit/stdout/stderr |
| `external_observation` | 8 | 在有限超时内观察进程/网络/线程时序，不使用 compiler 私有 probe |
| `python_backend` | 2 | 补 Python 3.12 build/execute 字节预期与 binding metadata |
| `selfhost_candidate` | 2 | 需要完整 selfhost candidate；stage0-as-candidate 仍明确拒绝 |

16 个 `static_public_cli_migrated_private_observation` 是本轮 271 个 generics case
对应 runner 的 14 个，加 `tests/generics/run_aggregate_const_rejections.py` 和
`tests/modules/run_cases.py`；它们仍留在 100 项里，因为原 runner 的私有观察没有
被 public CLI 覆盖。

- `tests/async/run_async_cases.py`
- `tests/async_parameter_copy/run_parameter_copy.py`
- `tests/bootstrap/readiness/run.py`
- `tests/generics/run_aggregate_const_rejections.py`
- `tests/generics/run_aggregate_inout.py`
- `tests/generics/run_aggregate_inout_hypotheses.py`
- `tests/generics/run_builtin_impl.py`
- `tests/generics/run_constrained_inout.py`
- `tests/generics/run_generic_traits.py`
- `tests/generics/run_indexed_calls.py`
- `tests/generics/run_inout.py`
- `tests/generics/run_list_inference.py`
- `tests/generics/run_method_inout.py`
- `tests/generics/run_method_move.py`
- `tests/generics/run_move.py`
- `tests/generics/run_nested_patterns.py`
- `tests/generics/run_receiver_modes.py`
- `tests/generics/run_self_modes.py`
- `tests/generics/run_unbound_methods.py`
- `tests/inout_places/run_inout_places.py`
- `tests/iterator/run_iterator.py`
- `tests/modules/run_cases.py`
- `tests/native/async_fork/run_async_fork.py`
- `tests/native/async_memory/run_async_memory.py`
- `tests/native/automatic_gc/run.sh`
- `tests/native/automatic_gc/run_automatic_gc.py`
- `tests/native/box_source/run_box_source.py`
- `tests/native/c_ffi/run_c_ffi.py`
- `tests/native/c_retained_capturing/run_capturing.py`
- `tests/native/c_retained_static/run_retained_static.py`
- `tests/native/channel-waits/run.py`
- `tests/native/channels/run_channels.py`
- `tests/native/concurrency-extra/run.py`
- `tests/native/concurrency-timeouts/run.py`
- `tests/native/gc_activation/run_gc_activation.py`
- `tests/native/hosted_thread_gc/run_thread_cleanup.py`
- `tests/native/http/run_http.py`
- `tests/native/language_gc_source/run.sh`
- `tests/native/language_gc_source/run_language_gc_source.py`
- `tests/native/process/run_process.py`
- `tests/native/reference_values_backend/run_reference_values_backend.py`
- `tests/native/run.py`
- `tests/native/run_chat.py`
- `tests/native/run_cleanup.py`
- `tests/native/run_cli.py`
- `tests/native/run_collections.py`
- `tests/native/run_concurrency.py`
- `tests/native/run_floating.py`
- `tests/native/run_os.py`
- `tests/native/run_ownership.py`
- `tests/native/run_stdlib.py`
- `tests/native/run_strings.py`
- `tests/native/run_trivial_vec.py`
- `tests/native/run_unreachable_try.py`
- `tests/native/run_value_library.py`
- `tests/native/run_vec_negative.py`
- `tests/native/slice_source/run_slice_source.py`
- `tests/native/stdlib-io-errors/run_io_errors.py`
- `tests/native/task_frame_gc/run_task_frame_gc.py`
- `tests/native/threadpool/run_threadpool.py`
- `tests/native/timezone/run.py`
- `tests/native/tls/run_tls_tests.sh`
- `tests/native/tls_source/run_tls_source.py`
- `tests/native/unicode/run.py`
- `tests/native/unicode/run_source.py`
- `tests/native/weak_source/run_weak_source.py`
- `tests/numeric/run_conversions.py`
- `tests/owning_inout/run_owning_inout.py`
- `tests/performance/binarytrees/large_prepare.py`
- `tests/performance/binarytrees/prepare.py`
- `tests/performance/fannkuch/large_prepare.py`
- `tests/performance/fannkuch/prepare.py`
- `tests/performance/mandelbrot/large_prepare.py`
- `tests/performance/mandelbrot/prepare.py`
- `tests/performance/matmul/large_prepare.py`
- `tests/performance/matmul/prepare.py`
- `tests/performance/run_native.py`
- `tests/performance/run_sort.py`
- `tests/performance/sieve/large_prepare.py`
- `tests/performance/sieve/prepare.py`
- `tests/python/interop_ops/run_interop.py`
- `tests/python/language_gc/run_language_gc.py`
- `tests/python/reference_values_backend/run_reference_values.py`
- `tests/python/run_aggregates.py`
- `tests/python/run_cases.py`
- `tests/python/run_cleanup.py`
- `tests/python/run_cleanup_regressions.py`
- `tests/python/run_floating.py`
- `tests/python/run_ownership.py`
- `tests/python/run_string_binding.py`
- `tests/python/run_strings.py`
- `tests/python/run_t10_bindings.py`
- `tests/python/run_t10_cli.py`
- `tests/python/slice_source/run_slice_source.py`
- `tests/python/weak_source/run_weak_source.py`
- `tests/reference_closure_frontend/run_reference_closure_cases.py`
- `tests/run_isolated.py`
- `tests/selfhost/front_diff.py`
- `tests/selfhost/smoke.py`
- `tests/trait_objects/run_trait_cases.py`

## 测试输入

自测可指定一组相配的语料目录、冻结清单、现成 C++ seed 和工具链：

```bash
python3 tests/selfhost/corpus_test.py \
  --root /absolute/compiler \
  --inventory /absolute/frozen-inventory.json \
  --stage0 /absolute/cpp-seed/woozyc \
  --toolchains /absolute/toolchains
```

默认语料目录是脚本所在仓库；工具默认从 `--toolchains`（或
`WOOZY_TOOLCHAINS`）下的 `llvm-22.1.8`、`strace-7.2`、
`t10-python-cpu/venv`、`libxml2-legacy-2.13.9` 查找。测试不再依赖固定的语料数量、某次提交增加的文件数量，或工作区必须保留脏文件。覆盖不全的失败检查使用测试自己制造的缺项。

清单必须记录该语料目录实际使用的字节。源码哈希改变仍会失败，参数不会放宽冻结检查。本轮使用 3d7139b 的临时 clone 和只更新一个已知变化
`contracts/selfhost.json` hash 的临时清单，完整小测试退出 0；冻结清单本身没有刷新 hash。使用未更新的原清单仍因该哈希失配而非零退出。这次没有运行 368 项已启用子集、完整语料或三代自举。

本轮派生脚本与 checks/SH10f-corpus-public-cli-20261113/ 原始证据保留在开发工作目录 `/home/qin/.local/state/cli-agent-bridge/worktrees/295bf1a9b0de0cddc75560ee`；该相对路径和生成命令指这个工作目录，不是安装后的编译器接口。Root已补五项not_ready分类断言；这批结果只是参考C++兼容性记录，完整候选对照和相对exec修复另行检查。

相对exec路径拒绝已与bootstrap修正合并；保留绝对路径白名单、超时/信号和有限清理检查。完整Source候选、完整语料和三代构建仍未执行。


## 重复执行与线程池用例（2026-11-14）

`execution_repetitions` 指一次成功构建后执行程序的次数，默认 1，必须是正整数，不能用 bool 或浮点数。`execution_timeout_seconds` 指每次执行的独立时限，默认沿用命令行时限；必须是可转换为有限正数的 int/float，bool、NaN/Inf、非正值以及过大的 JSON 整数均报配置错误。这两个字段只允许用于会执行程序的成功 Native/Python 构建。

每次执行都保留独立的 `executions/<index>/` 标准输出、标准错误、trace 和退出记录。各次都检查运行期望，并按相同序号比较参考与候选；次数不足或多出、任一次错误、超时、信号退出或清理未完成均失败。两侧相同的错误输出仍会因不满足运行期望而失败。

已复核的 `SH10n-threadpool-corpus-repeats-20261114` delta 现已应用到 Root：只更改 11 个既有线程池用例与 `tests/native/threadpool/run_threadpool.py` 的分类。其他 1223 个用例、2672 条文件记录和 210 个 runner 保持原值。parallel、multi-submit-wait、backpressure、close-drain、close-submit-race、owned-results、self-join、lifecycle 在每个 O0/O2 程序上各运行 3 次，result-error 各 2 次，invalid-arguments 和 panic-task 各 1 次，共 22 次构建、56 次执行。普通用例每次限时 60 秒、exit 0 且输出为空；panic-task 每次限时 20 秒、exit 101，并检查 division-by-zero Panic 前缀。

这 22 次构建和 56 次执行来自已保留的 C++ 参考编译器记录。新增 Woozy 候选、881 项启用子集、完整语料及三代自举没有因本次合入获得通过结论。原 delta 文件和参考记录保留原字节，其中“尚未应用”描述生成该文件时的状态；本次 Root 应用状态、前后清单与逐项差异记在 `checks/threadpool-root-delta-integration-20261114/`。本次没有刷新源码 hash、文件角色、文件分配或其他冻结字段；已知陈旧 hash 与缺项仍会失败，`formal_freeze_ready` 保持 false。

## 已接受提交的 tracked 源码再冻结（2026-11-14）

本节只记录最新一次冻结；上文旧数字保留为生成时状态。基准清单为 3d7139b 冻结的
`47f7c6d8a2d113645ebeb3bb07739651a7bd173eae148c89f7f24dfea1cdb664`，新清单
SHA-256 为 `7e3cec635fe4b6196568d228b797ac8bd8d381b445000de8ec1d64d9bd958bb7`，
对应 HEAD `b5ac141619627dce9d9658ea8c1e03ca42ac5ca8`。

| 项 | 之前 | 之后 |
|---|---:|---:|
| 文件记录 | 2672 | 2837 |
| case 总数 | 1234 | 1250 |
| enabled | 881 | 881（逐字段不变） |
| disabled | 353 | 369 |
| runner | 211 | 233 |
| adaptation_required runner | 99 | 99（不变） |
| compiler_source | 55 | 116 |

新增 165 个 tracked 冻结路径：61 个 `compiler_source`、58 个 `woozy_source`、
24 个 manifest、22 个 runner 源文件。修改 10 个既有文件记录（只更新已接受提交
产生的 SHA/大小，角色与 assignment 不变），删除 0 个：

- `50a6713`：106 条新增路径，另修改 `cli/driver.wz`、`project/project.wz`。
- `37f5555`：55 条新增 hirverify/sema 路径。
- `8c765d2`：4 条新增 stat_bytes 路径，另修改 `system/system.wz`、`stdlib/os/os.wz`。
- `084a1e5`：修改 ownership analysis、valueops 和 ownership 测试源码。
- `0d517f6`：修改 `tests/selfhost/bootstrap_test.py`；`b5ac141` 最终修改
  `contracts/selfhost.json` 与 `selfhost/bootstrap.py`。

逐路径 old/new SHA 在 `refreeze.modified_freeze_paths` 与
`.work-tmp/selfhost-refreeze-20261114/verification.json` 的 `paths[]`。
10 个修改项的新字节均等于 `b5ac141` 的 blob；旧 catalog 中
`contracts/selfhost.json` 的哈希本来就不是 3d7139b 的 blob，本次按实际 tracked
字节更新并单独记录，没有修改源文件。

### 116 个 compiler source

`selfhost/src` 的 116 个 `.wz` 全部按当前 tracked 字节登记为
`compiler_source`，assignment 仍为 `compiler:source`，manifest 仍为
`selfhost/lang.toml`。stage1 构建早于提交 50a6713，并保留当时的 116 个源文件字节。当前提交只去掉了
其中 3 处空白：`hirverify/run.wz` 尾随空格，以及 `nativelower/construct.wz`、
`nativeverify/copies.wz` 各一个 EOF 空行。`nativeverify/verify_ops.wz` 的新哈希
等于已接受函数类型修正 `a9c375953da70100fbeae8d1220f37cac72788d9a162d0fd3428e24e08da3a48`。
本轮没有重编 116 个源，也没有用参考 stage1 结果代替候选结果。

### 新组件分类

新增 16 个 disabled component case，全部 `enabled=false`、
`adaptation=component`、`runner_status=component`，并写明具体限制：

| case | runner |
|---|---|
| `project:selfhost/tests/hirverify/lang.toml` | `selfhost/tests/hirverify/run.py` |
| `component:selfhost/tests/nativeemit_b` | `selfhost/tests/nativeemit_b/run.py` |
| `component:selfhost/tests/nativeemit_c` | `selfhost/tests/nativeemit_c/run.py` |
| `component:selfhost/tests/nativeemit_d` | `selfhost/tests/nativeemit_d/run.py` |
| `component:selfhost/tests/nativeemit_e` | `selfhost/tests/nativeemit_e/run.py` |
| `project:selfhost/tests/nativelower/lang.toml` | `selfhost/tests/nativelower/run.py` |
| `project:selfhost/tests/nativeverify/callidentity/lang.toml` | `selfhost/tests/nativeverify/callidentity/run.py` |
| `project:selfhost/tests/nativeverify/driver/lang.toml` | `selfhost/tests/nativeverify/driver/run.py` |
| `project:selfhost/tests/nativeverify/layouttest/lang.toml` | `selfhost/tests/nativeverify/layouttest/run.py` |
| `project:selfhost/tests/nativeverify/obligationstest/lang.toml` | `selfhost/tests/nativeverify/obligationstest/run.py` |
| `component:selfhost/tests/nativeverify/packagecheck` | `selfhost/tests/nativeverify/packagecheck/run.py` |
| `project:selfhost/tests/pythonemit/lang.toml` | `selfhost/tests/pythonemit/run.py` |
| `project:selfhost/tests/sema_a/lang.toml` | `selfhost/tests/sema_a/run.py` |
| `component:selfhost/tests/sema_b` | `selfhost/tests/sema_b/run.py` |
| `project:selfhost/tests/stat_bytes/lang.toml` | `selfhost/tests/stat_bytes/run.py` |
| `project:selfhost/tests/stat_bytes_e/lang.toml` | `selfhost/tests/stat_bytes_e/run.py` |

callidentity 的 generic 与 enumclosure 两个 Source fixture 只做组件 API 验证，
不登记为 `public_cli` 通过。nativeemit_c 的 corpus、nativelower 的 fixtures/helpers/
lowercheck、sema_b 的 cases 等嵌套 manifest 和源码分配到所属组件的 disabled
case，没有额外伪造独立项目通过。22 个新 runner 源文件全部登记为 component；
helper 脚本单独写明不编译或执行候选。复杂 99 个 runner 仍为
`adaptation_required`，未改写。

`coverage_expectations` 已按实际文件更新为：`tracked_wz=1317`、
`manifests=1008`、`tests_manifests=960`（只计 `tests/` 下）、
`no_manifest_wz=125`、`auxiliary_dsl_inputs=21`。

### 私有核对与负控制

本轮只在自己的 worktree 内修改 `selfhost/corpus.json` 和本节文档，未写 Root、
index 或远端，未重编、未跑全候选、stage2/3 或 116 源构建。对 tracked 路径运行
`_inventory_errors` 为 0；`coverage_report` 如实保持不完整：

- `files_without_enabled_case=590`、`disabled_cases=369`、
  `adaptation_required=324`、`runners_require_adaptation=99`。
- `complete=false`；没有把 `formal_freeze_ready` 报成 true。

负控制使用明确路径模拟，没有调用默认 `check_inventory`/`coverage` CLI，也没有
扫描整个仓库的 untracked：

- 显式 untracked 列表 1 项、显式 dirty 列表 1 项分别产生对应 reason，二者同时
  出现时两条 reason 都在；这只是私有模拟，不是 Root formal freeze。
- 内存清单负控制覆盖缺文件、错哈希、把 component case 改成 enabled、缺
  component reason、缺 runner 分类、缺 assignment、refreeze 指向未登记路径，
  均被 `_inventory_errors` 拒绝。
- 新建临时小 Git 仓库实际验证 `git ls-files` 跟踪集合：修改、未跟踪新增、commit
  后新增、删除及 commit 后删除均按预期变化。

完整 56 项断言、每路径 SHA、负控制输出、输入记录哈希和覆盖报告在
`.work-tmp/selfhost-refreeze-20261114/verification.json`。该目录是原作者工作区的
私有报告。Root 后续独立核对了 2837 条实际文件与提交字节、原 881 个启用用例
和所有旧记录，并运行负控制后接受并合入；这不表示完整候选语料已通过。
