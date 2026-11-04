# 严格浮点源码 fixtures 与独立预期

operation_id: language-strict-float-fixtures-20261104  
契约版本：compiler-dev/1。  
correction: 1/1（唯一一次根代理证据修正）。  
起点：`aacd4486f04efea9186605d9562e028be14edf95`。

本目录只准备严格 f32/f64 的源语言项目、未来静态/运行预期和独立 oracle 记录，不实现编译器、前端、HIR 或任何后端。严格浮点运行时实现属于 T05 后续；本目录不承担错误清理实现。没有新增 runner、schema 文件或根 CMake；`cases.json` 的 `schema` 只是本目录的数据格式标签。所有修改仅位于 `tests/floating/**`。

## 范围与布局

```text
tests/floating/
  cases.json                         # 18 个项目、76 条探针；未来静态/探针分类和位模式
  fixtures/
    f32-* / f64-*                    # 10 个新增浮点正例
    baseline-01-basics/              # 01_basics.lang 逐字节副本
    baseline-05-traits/              # 05_traits.lang 逐字节副本
    reject-*                         # 6 个语法合法的类型反例
  tools/apfloat_oracle.cpp           # 测试工具；只链接外部 LLVM APFloat
  oracle/vectors.tsv                 # 该工具的实际输出
```

18 个项目 = 12 个正例（10 个新增浮点项目 + 2 个既有样例副本）+ 6 个反例；共 76 条探针（原 62 条 + 本修正追加 14 条）。每个项目有独立 `lang.toml`，源码位于 `src/main.lang`。

- 正例覆盖：两种精度的 RNE ties-to-even（加法和十进制字面量）、两个只能直接按 f32 解析才正确的 double-rounding 陷阱、溢出到带符号 Inf、subnormal/下溢到带符号零、subnormal tie、`+0/-0` 的运算与符号、除零、`0/0` 和 `Inf-Inf` 的 NaN 分类、NaN 的有序比较、有限有序数与 `+0/-0` 的 true/false 对照、以及禁止隐式融合的 `a*b+c`。
- 反例覆盖：浮点 `%`、已定 f32/f64 混合、浮点 bitwise、浮点 shift、浮点条件、动态 f64→i32 转换。
- 不使用 NaN 字面量、bitcast、十六进制浮点或类型后缀。NaN 和 Inf 均由合法的 IEEE 运算产生。
- 每个 probe 都有显式参数/返回类型。`cases.json` 记录未来静态结果、探针签名、参数位模式、返回分类/固定宽度 hex 位模式和零符号。NaN 的 `bits` 和 `sign` 写为 `null`，明确表示不比较 payload 或符号；只比较 `class=nan`。
- `baseline-01-basics` 和 `baseline-05-traits` 的源码与 `baseline/samples/01_basics.lang`、`05_traits.lang` 逐字节相同，仅作为未来严格浮点/trait 执行的既有依赖；原样例没有被修改。

## 预期摘要

完整 76 条探针见 `cases.json` 和 `oracle/vectors.tsv`。位模式为按 canonical 整数位输出的固定宽度小写 hex，f32 8 位、f64 16 位；不是宿主内存字节序。

| 项目 | 探针数 | 关键未来结果 |
| --- | ---: | --- |
| `f32-round-ties-even` | 6 | 原 4 条中点探针不变；新增两条略偏离中点的字面量，直接 f32 均为 `3f800001`，f64-first 分别错误得到 `3f800000`/`3f800002` |
| `f64-round-ties-even` | 4 | `1+2^-53 → 3ff0000000000000`；`1+3*2^-53 → 3ff0000000000002`；两个字面量相同 |
| `f32-overflow` | 2 | `MAX*2 → 7f800000`；`-MAX*2 → ff800000` |
| `f64-overflow` | 2 | `MAX*2 → 7ff0000000000000`；`-MAX*2 → fff0000000000000` |
| `f32-subnormal` | 4 | 最小正规数/2 → `00400000`；最小 subnormal/2 → `+0 00000000`/`-0 80000000`；subnormal tie → `00000002` |
| `f64-subnormal` | 4 | 最小正规数/2 → `0008000000000000`；最小 subnormal/2 → `+0`/`-0`；subnormal tie → `0000000000000002` |
| `f32-zero-div-nan` | 26 | 原 20 条不变；新增有限 `< <= > >=` 各一个 true、`+0 == -0` true、`+0 != -0` false |
| `f64-zero-div-nan` | 26 | 原 20 条不变；新增有限 `< <= > >=` 各一个 true、`+0 == -0` true、`+0 != -0` false |
| `f32-no-fma` | 1 | 分别乘加得到 `00000000`；若隐式 FMA 会得到非预期 `28800000`（2^-46） |
| `f64-no-fma` | 1 | 分别乘加得到 `0000000000000000`；若隐式 FMA 会得到非预期 `3970000000000000`（2^-104） |
| `baseline-01-basics` | 0 | 字节副本；未来执行 `main` 目标返回 0，实际未执行 |
| `baseline-05-traits` | 0 | 字节副本；未来执行 `main` 目标返回 0，实际未执行 |

`f32/f64-no-fma` 的 `contraction_check.fused_if_contracted_bits` 是反例说明，不是期望值。未来实现必须观察到分别舍入后的 `+0`；不能把融合结果当成通过。

两个 double-rounding 陷阱的 `double_rounding_check.f64_first_wrong_bits` 同样只是错误策略说明，不是期望值。旧的中点字面量都能被 f64 精确表示，因此不能检出 f64-first 解析；新字面量距中点极近，只有直接按 f32 解析才得到正确 expected。具体地，第一个字面量略高于 `1+2^-24`，f64-first 先舍入到精确的 `1+2^-24`（`3ff0000010000000`），再转 f32 时 ties-to-even 向下得到 `3f800000`；第二个字面量略低于 `1+3*2^-24`，f64-first 先舍入到精确的 `1+3*2^-24`（`3ff0000030000000`），再转 f32 时 ties-to-even 向上得到 `3f800002`。

反例不新增诊断码；`expected_static.reason` 说明目标错误类别，具体 code 由 root/集成登记。当前实现的实际拒绝见下表。

## APFloat oracle

预期由外部 LLVM 22.1.8 的 `APFloat` 独立计算：

- 十进制字面量直接以目标精度 `IEEEsingle` 或 `IEEEdouble` 解析，不先转宿主 `double` 或 Python float。
- 每一步算术都显式使用 `APFloat::rmNearestTiesToEven`。
- 边界输入用 `require_bits` 与固定 hex 直接核对（最大有限值、最小正规数、最小 subnormal 等）；oracle 不依赖 `getLargest/getSmallestNormalized/getSmallest` 生成预期。
- 两个 f32 double-rounding 陷阱同时计算 direct f32 与 f64-first 两种策略；只有 direct f32 的结果写入 expected，f64-first 仅作为错误对照。
- NaN 只检查 `isNaN()`；零检查 `isZero()`/`isNegative()`；其他值输出 `bitcastToAPInt()` 的 canonical 位模式。
- `tools/apfloat_oracle.cpp` 只是测试工具，不进入 `langc`，不给前端增加 LLVM 依赖。

实际使用的工具链位于工作区外的 `toolchains/llvm-22.1.8` 和 `toolchains/libxml2-legacy-2.13.9/usr/lib`；逐字机器路径与构建日志保存在外层检查目录，不写入本目录。下面命令用占位变量表示这些路径。

构建/运行命令（实际日志保存在工作区外）：

```sh
LLVM_PREFIX=/path/to/llvm-22.1.8
OUT=/path/outside/repo/apfloat-oracle
"$LLVM_PREFIX/bin/clang++" -std=c++17 -Wall -Wextra -Wpedantic \
  $("$LLVM_PREFIX/bin/llvm-config" --cxxflags) \
  tests/floating/tools/apfloat_oracle.cpp -o "$OUT" \
  -L"$LLVM_PREFIX/lib" -lLLVMSupport -lLLVMDemangle \
  -lrt -ldl -lm -lz -lxml2
LD_LIBRARY_PATH=/path/to/libxml2-legacy-2.13.9/usr/lib \
  "$OUT" > tests/floating/oracle/vectors.tsv
```

说明：该 LLVM 安装的 `llvm-config --system-libs` 指向不存在的静态 `/usr/lib/x86_64-linux-gnu/libzstd.a`，因此使用上面的显式链接列表；没有安装或修改任何包。编译成功，`-Wextra` 只在 LLVM 自带 `Error.h` 报告一个未使用参数警告，不是本工具代码的错误。

实际结果：

| 项 | 结果 |
| --- | --- |
| oracle 编译 | passed；LLVM 22.1.8 `APFloat` |
| oracle 运行 | passed，退出 0 |
| `vectors.tsv` 行数 | 78 行 = 2 行表头 + 76 条探针 |
| `vectors.tsv` sha256 | `aeb1ed5801e16be6cff88422351d9197e6777c374146c0797ccf9a8eb9837d70` |
| `apfloat_oracle.cpp` sha256 | `594684cb1f912584cdf396c3bf53ca6d6fccf20c3ab1d2766da10ef958498c84` |
| `cases.json` sha256 | `996282c9e9076382e62700a690ec7d04037427f0828f153942ffc6ac6b2b9fae` |
| 独立 cases/oracle 对照 | passed：18 个项目、76 条探针、0 差异 |
| 当前 CLI `check --project` | 36/36 退出非 0（native 18 + python 18；12 个正例均 `E_CAPABILITY_UNAVAILABLE`） |
| 未来严格浮点静态/动态实现 | not_run（本任务不实现） |
| native/Python 执行 | not_run |

## 源文件、JSON 与语法检查

使用现有 C++ 正式 CLI：

```sh
langc check --syntax-only --format json tests/floating/fixtures/<id>/src/main.lang
langc check --project tests/floating/fixtures/<id>/lang.toml --format json
```

`--syntax-only` 实际结果：18/18 退出 0，`status=ok`，无诊断。native/python 各 18 个 `check --project` 实际为 36/36 退出非 0；正例均为当前 `E_CAPABILITY_UNAVAILABLE`。语法通过不代表类型/效果检查或执行通过。

`cases.json` 自查通过：

- 18 个 ID 唯一；`case_count=18`、`positive_count=12`、`reject_count=6`、`probe_count=76`、`original_probe_count=62`、`added_probe_count=14`。
- 18 个 manifest/source 引用均存在；18 个 source sha256 和 18 个 manifest sha256 与文件一致。
- 两个 baseline 副本与原件字节一致，原文件 sha256 也一致：
  - `01_basics.lang`: `b8b035cd0fc09798abb310d23e416f714ad449806c1e0fb726306c82c2e70051`
  - `05_traits.lang`: `d435ed5a30cfe75600bd982e12e27ec59306181c5fe998aba05751370bd3c961`
- 76 条探针逐条与 `oracle/vectors.tsv` 的 class/bits/sign（或 bool 值）一致；NaN 条目的 bits/sign 为 `null` 表示不比较；固定宽度 hex 全部通过。
- 原 62 条探针对象与本轮修正前的 `cases.json` 逐字段相同；`vectors.tsv` 中原 62 行逐行相同，只新增 14 行。
- 本修正新增的 14 条为：2 条 f32 double-rounding 陷阱、f32/f64 各 6 条 bool 对照。
- 每个未来探针带显式签名；no-fma 项目带参数位模式。

## 当前完整检查的真实拒绝

以下为基线 `aacd448` 的现有 `langc check --project` 实际结果（默认 native；另用 `--backend python` 复跑得到相同的 18 组 code）。正例的 `E_CAPABILITY_UNAVAILABLE` 是当前后端/严格浮点尚未实现导致的真实拒绝，**不是**未来 `expected_static` 的结果。反例的当前 code 仅记录实际行为，未来仍以 `expected_static` 的目标类别为准。

| 项目 | 当前退出码 | 当前 code |
| --- | ---: | --- |
| `baseline-01-basics` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `baseline-05-traits` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `f32-round-ties-even` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `f64-round-ties-even` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `f32-overflow` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `f64-overflow` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `f32-subnormal` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `f64-subnormal` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `f32-zero-div-nan` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `f64-zero-div-nan` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `f32-no-fma` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `f64-no-fma` | 1 | `E_CAPABILITY_UNAVAILABLE` |
| `reject-float-remainder` | 1 | `E_TYPE_MISMATCH`（bitwise/remainder 要求整数） |
| `reject-mixed-float-width` | 1 | `E_TYPE_MISMATCH` |
| `reject-float-bitwise` | 1 | `E_TYPE_MISMATCH`（bitwise/remainder 要求整数） |
| `reject-float-shift` | 1 | `E_TYPE_MISMATCH`（shift 要求整数） |
| `reject-float-condition` | 1 | `E_TYPE_MISMATCH` |
| `reject-dynamic-float-int-conversion` | 1 | `E_CAPABILITY_UNAVAILABLE`（float/integer 转换未实现） |

未来严格浮点（T05 后续）静态检查、native/Python 生成和执行均为 `not_run`。即使未来 `main` 返回 0，也不能证明观察了 probe 的浮点结果；必须按 `cases.json` 中的实际生成函数类型调用探针并读取返回值和位模式。

## 修正记录

本轮 correction 1/1：

- 保留原有 18 个项目、原 62 条 probe 的函数体、签名、参数和数值预期；只追加 14 条 probe。
- 新增两个 f32 double-rounding 陷阱：`1.0000000596046447753906250000000000000000001` 直接 f32 为 `3f800001`，f64-first 错误为 `3f800000`；`1.0000001788139343261718749999999999999999999` 直接 f32 为 `3f800001`，f64-first 错误为 `3f800002`。只有 direct f32 是 expected。
- 新增 f32/f64 各 6 条 bool 对照：有限 `< <= > >=` 各一个 true，`+0 == -0` true，`+0 != -0` false。旧的 NaN false 探针保留，防止固定返回 false 的实现通过全部旧探针。
- 文档改正：oracle 实际用 `require_bits` 固定 hex 核对边界，不使用 `getLargest/getSmallestNormalized/getSmallest` 生成预期；NaN 的 `bits/sign=null` 明确表示不比较；严格浮点属于 T05 后续，文档不再把本目录称为错误清理 fixture。

此前修正记录：

- 首轮 `one_less_than_nan` 把浮点字面量放在比较左侧，当前检查器在合法源码上报 `E_TYPE_MISMATCH`；改为两个显式 f32/f64 变量后再检查，正例当前拒绝统一为 `E_CAPABILITY_UNAVAILABLE`。APFloat 的预期结果没有变化。
- 首轮外层校验脚本误把 bool 探针按浮点 bits 比较；只修正校验脚本，`cases.json` 和 oracle 输出没有因此改动。
- oracle 和 `cases.json` 的原始 62 条数值在初始 APFloat 运行后没有手改。
