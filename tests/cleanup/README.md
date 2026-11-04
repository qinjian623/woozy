# T08 清理与 typed throws fixtures

本目录只准备 T08 的独立源码项目和未来预期，不实现编译器。没有新增 runner、schema 文件或根 CMake 接入；`cases.json` 的 `schema` 字段只是沿用现有 fixtures 的数据格式标签。

语法、JSON、文件引用和复制源码hash检查通过；T08 类型/效果检查、Native/Python 生成与执行全部为 `not_run`。

## 布局

```text
tests/cleanup/
  cases.json
  fixtures/
    case-01/{lang.toml,src/main.lang}       # case-01 ... case-10
    errors-original/{lang.toml,src/main.lang}
    reject-*/{lang.toml,src/main.lang}      # 8 个反例
```

每个项目都是独立 `lang.toml` 项目。manifest 的包名统一为合法标识符 `main`，不使用目录中的短横线；`source_root = "src"`，`entry = "app"`，`source_root` 中唯一的 `main.lang` 声明 `package main;`。

- `case-01` 到 `case-10` 的源码逐字节复制自已审查的 proposal 源码。
- `errors-original/src/main.lang` 是 `examples/errors.lang` 的逐字节副本。
- 8 个 `reject-*` 源码是本任务新增的小反例，全部通过语法检查。

## 正例预期

`cases.json` 中的 `expected_static` 和 `runtime_expected` 是 T08 实现后的目标，不是当前检查结果。事件按 `mark(log,digit)` 的十进制追加顺序或 `cleaned` 计数记录。

| id | 未来静态预期 | 正常返回 | 事件 | log/计数 | 要点 |
| --- | --- | --- | --- | --- | --- |
| case-01 | accept | `probe=7`, `main=0` | 1,2,3,9 | 1239 | 最近块 LIFO、执行时读取、显式快照 |
| case-02 | accept | `probe=4`, `main=0` | 1,2,3 | 123 | 返回值先保存，清理中 `x=8` 不改变结果 |
| case-03 | accept | `main=0` | 4,1,4,2,5,6,7,9 | 41425679 | continue 先清理再 step；break；switch break；不进入 catch 8 |
| case-04 | accept | `main=0` | 1,2,3,4,5,6,7 | 1234567 | throw 后从内到外清理，未经过 defer 8 不激活 |
| case-05 | accept | `main=0` | 1,2,3,4 | 1234 | 命名实参按书写顺序求值，失败后 later/sink 不执行 |
| case-06 | accept | `main=0` | 1,2,3,4 | 1234 | handler 抛错交给外层，同一 try 的 catch 9 不执行 |
| case-07 | accept | `main=0` | 1,2,3,4 | 1234 | defer 内部循环跳转和局部 catch 合法 |
| case-08 | accept | `main=0` | 1,2,3,4 | 1234 | throws 函数值；成功返回 4，失败载荷 -1，`make` 无自动效果 |
| case-09 | accept | `main=0` | 1,2,3 | 123 | 普通 `Result.Err` 不传播，不进入 catch 9 |
| case-10 | accept | 无正常返回 | 1,2 | panic 时 12 | defer 内除零 Panic；原因 `division-by-zero`；typed catch 不捕获 |
| errors-original | accept | `main=0` | cleaned 1,2 | 2 | 成功和失败调用；defer 计数；catch `ReadError.Missing` 后返回 0 |

### case-10 观察限制

native 进程终止后不能直接读取局部 `log=12`。未来 native 检查只验证 Panic 原因、源位置（`src/main.lang` 第 12 行的 `i32(1)/zero`）和 runner 退出 `101`；不能把较早 defer 中的非法移位当成实际 Panic。Python 未来可用 `entrypoint=false` 调用 `probe`，捕获 Panic 后检查 Cell 的 12。

## 反例预期

反例只绑定已公布的诊断码，不新增错误码。

| id | 未来诊断 | 检查内容 |
| --- | --- | --- |
| reject-function-value-missing-throws | `E_TYPE_MISMATCH` | `func(...) ... throws E` 赋给无 throws 函数类型 |
| reject-function-value-wrong-throws | `E_TYPE_MISMATCH` | `throws E` 函数值赋给 `throws F` 函数类型 |
| reject-unhandled-effect | `E_EFFECT_UNHANDLED` | `main` 未声明 throws 且未捕获 `fail()` 的 E |
| reject-defer-return | `E_CLEANUP_ESCAPE` | defer 向外 return |
| reject-defer-break | `E_CLEANUP_ESCAPE` | defer 向外 break 到外层循环 |
| reject-defer-continue | `E_CLEANUP_ESCAPE` | defer 向外 continue 到外层循环 |
| reject-defer-throw | `E_CLEANUP_ESCAPE` | defer 向外抛 E，即使外层函数声明 `throws E` |
| reject-result-as-scalar | `E_TYPE_MISMATCH` | `Result[i32,E]` 直接初始化 `i32` |

## 实际检查（2026-11-04，基线 b734800）

使用现有 C++ 正式 CLI：

```sh
langc check --syntax-only --format json tests/cleanup/fixtures/<id>/src/main.lang
langc check --project tests/cleanup/fixtures/<id>/lang.toml --format json
```

| 检查 | 实际结果 |
| --- | --- |
| `cases.json` JSON 解析、manifest/source 引用 | 19/19 passed |
| 10 个 proposal 源码 hash | 10/10 与独立审查记录一致 |
| `examples/errors.lang` 副本 | 字节一致，hash 一致 |
| 8 个新反例源码 hash | 已记录；无外部原始 hash 可比 |
| `--syntax-only` | 19/19 passed，退出 0，`status=ok`，无诊断 |
| 当前 `check --project`（manifest 默认 native） | 18 个 `E_CAPABILITY_UNAVAILABLE`，1 个 `E_TYPE_MISMATCH` |
| 未来 T08 类型/效果检查 | `not_run` |
| 未来 native/Python 执行 | `not_run` |

当前完整检查中的 `E_CAPABILITY_UNAVAILABLE` 是 T08 未实现导致的能力拒绝，不是未来 `expected_static` 的 accept/reject 结果。唯一当前 `E_TYPE_MISMATCH` 是 `reject-result-as-scalar`：该例不依赖 T08 新语义，现有泛型/标量检查已能拒绝明显类型错误；这不作为任何 T08 用例或运行通过。

语法检查的详细 JSON、逐项源码 hash 和当前项目检查输出保存在外层检查结果中，不写入本目录，避免机器路径进入 Git。
