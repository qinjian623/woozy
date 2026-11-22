# Woozy 自举实现与检查

2026-11-13 已开始移植完整编译器，源码在 `selfhost/src/`。已移植 Source/SourceManager、完整词法与语法分析、诊断 JSON、语法检查 CLI、大整数、TOML 和 SHA-256。121 个真实输入的源码位置、token、AST、恢复状态及诊断与 C++ 前端逐字段一致，Native O0/O2 均通过。新 CLI 已独立通过现有 80 个语法用例；这部分运行时不调用旧前端。

项目加载、名称与类型检查、HIR/所有权及 Native/Python 代码生成仍在移植。新 CLI 的项目检查和 build 暂时明确报不支持。尚未完成 stage1/2/3，不能称为完整自举。任务和共享接口见 `contracts/selfhost.json`、`SELFHOST_CONTRACT.md`、`SELFHOST_FOUNDATION.md`；检查命令见 `development/tests/selfhost/README.md`。

以下保留开始移植前的就绪评估与探针记录，其“本轮”指当时的评估。

## 早期就绪评估

评估时状态：**小范围探针已通过；完整自举当时尚未开始。**

本文回答的是“现在能不能开始用 Woozy 编写自己的编译器”，不是本轮重写整个编译器，也不把探针结果当成自举证明。支持范围仅限 Linux x86_64；工具链基线为 LLVM/Clang/LLD 22.1.8。

## 1 结论

| 问题 | 当前结论 | 依据或缺口 |
| --- | --- | --- |
| 能否开始用 Wz 写词法、语法、符号表和 IR 文本生成 | **可以开始** | `tests/bootstrap/readiness/` 中的 Wz 探针在 O0/O2 下实际构建并运行，生成并执行了静态 ELF |
| 能否用 Wz 写完整前端 | **尚未证明** | 探针只覆盖受限表达式语言；包/导入、完整声明、类型检查、泛型、trait、错误效果、所有权/ARC 都还没有 Wz 实现 |
| 能否生成 Native 后端 | **只证明了受限子集** | 探针生成文本 LLVM IR，由 Clang/LLD 编译链接；没有覆盖现有 Native 后端的 ABI、运行库、聚合、清理和优化路径 |
| 能否生成 Python 后端 | **未验证** | 本轮没有 Wz 版 Python 发射器或 Python 语义对照 |
| 是否已经自举 | **否** | 没有 stage1/stage2，没有同一 Wz 编译器源码的两级构建，也没有固定点检查 |
| 能否不依赖旧 C++ 前端完成探针运行 | **已验证** | 探针运行时 PATH 只有故障 wrapper，旧前端调用日志为空；只有 stage0 构建探针这一步使用现有 C++ 编译器 |

完整自举的定义必须写死为下面这条链：

```text
stage0: 现有 C++ woozyc
  └─ 构建同一份 Wz 编译器源码 → stage1
stage1: Wz 编译器
  └─ 构建同一份 Wz 编译器源码 → stage2
stage2: Wz 编译器
  └─ 构建同一份 Wz 编译器源码 → stage3
```

固定点要求 stage2 与 stage3 对同一源码产生等价行为：相同的源码接受/拒绝结果、相同的诊断、相同的 Native 与 Python 产物语义、相同的测试结果；可重复构建时还应比较规范化后的 ELF/object/Python 输出摘要。stage1 和 stage2 运行时不得执行 `woozyc`、`lang_native_helper` 或任何 C++ 前端来补足能力；未实现的功能必须报错，不能悄悄转调旧前端。

“完整自举”还必须包含当前已有的 Native/Python 两个后端及其现有功能范围，而不只是能编译一个表达式解释器。仅仅让 Wz 程序调用旧 `woozyc` 并转发源码，不算自举。

## 2 本轮实际完成的探针

新增内容只在以下位置：

```text
development/docs/SELF_HOSTING.md
tests/bootstrap/readiness/
  cases.json
  cases/*.wz
  probe/lang.toml
  probe/src/main.wz
  run.py
```

运行结果保留在本机 `tmp/checks/` 下的本轮运行目录，不提交二进制和日志。

### 2.1 探针做了什么

`tests/bootstrap/readiness/probe/src/main.wz` 是一个 Wz 程序，不是 Woozy 编译器：

1. 以字节读取源文件，验证 UTF-8，并维护行号、Unicode 码点列号和原始字节区间。
2. 跳过空格、LF、CRLF、`//` 行注释和 `/* ... */` 块注释；未闭合块注释、孤立 CR、NUL、非法 UTF-8 都有诊断。
3. 识别 ASCII 标识符、`let`/`return`、十进制/二进制/八进制/十六进制整数、数字下划线、`+ - * / % ( ) = ;`。
4. 用 `Vec[Token]` 保存 token，用 `Vec[Expr]` 保存 AST 节点，节点之间只存 `i64` 节点 ID，不用递归对象图。
5. 用 `std/map` 的 `Map[string, usize]` 做符号表；重复定义和未知变量分别报告。
6. 采用优先级爬升解析表达式；一元 `+/-` 高于 `* / %`，后者高于 `+ -`，左结合，括号覆盖优先级。
7. 生成文本 LLVM IR：每个变量物化为 SSA 值，每个运算生成 `add/sub/mul/sdiv/srem` 或带检查的除法 CFG；最终用 `and i64 ..., 255` 得到进程退出码。
8. 通过 Woozy 的 `std/process` 以绝对路径调用传入的 Clang，把 IR 编译成 object；再调用传入的 LLD，以 `-static` 链接成 ELF；随后启动 ELF 并检查退出码。每次工具/ELF 调用都有探针内部的 `TIMEOUT_MS` 上限，用 `WNOHANG` 轮询并在超时时 `SIGKILL`、回收直接子进程；驱动外层还有自己的 timeout。
9. 探针自己独立求值 AST，作为生成 ELF 退出码的对照；驱动再按 `cases.json` 的独立期望值检查探针输出。因此不是把固定答案写进 IR。

探针 IR 使用 `_start` 和 Linux `exit` 系统调用，生成的是无 libc、无动态解释器的静态 ELF：

```llvm
target triple = "x86_64-unknown-linux-gnu"

define void @_start() {
entry:
  ...
  %exit = and i64 %result, 255
  call void asm sideeffect "syscall", "{rax},{rdi},~{rcx},~{r11},~{memory}"(i64 60, i64 %exit)
  unreachable
}
```

这不是完整 Woozy ABI，也不替代现有 Native 运行库；它只证明 Wz 能完成“前端数据结构 + 文本 IR + 进程调用 + 运行验证”的闭环。

### 2.2 受限输入文法

探针只接受：

```text
program := { "let" NAME "=" expr ";" } "return" expr ";"
expr    := unary { ("+"|"-"|"*"|"/"|"%") unary }
unary   := ("+"|"-") unary | primary
primary := INTEGER | NAME | "(" expr ")"
```

整数支持十进制、`0b`、`0o`、`0x` 和数字之间的 `_`。`-9223372036854775808` 作为 i64 最小值特殊处理；正的 `9223372036854775808` 报溢出。算术按固定宽度 i64 回绕，IR 不添加 `nsw`/`nuw`。生成器为除法发出零除和 `i64 min / -1` 的显式保护分支，避免把 LLVM 的 poison 行为当成语言语义；探针的独立求值器在生成前拒绝可确定的零除，本轮没有把零除列为可接受程序。

明确不支持：package/import、函数、类型、字符串、结构体、枚举、泛型、trait、`throws`、所有权/ARC、被编译程序中的标准库调用、完整 Woozy 语法。它不是“完整 Woozy 编译器”。

### 2.3 实际检查结果

驱动 `tests/bootstrap/readiness/run.py` 做以下事情：

- 用传入的 stage0 `woozyc` 分别以 `--opt 0` 和 `--opt 2` 构建探针；两次构建都成功。
- 10 个正例 × 2 个探针优化级 × 2 个 Clang IR 优化级 = 40 次真实运行。
- 11 个负例 × 2 个探针优化级 = 22 次诊断运行。
- 4 次故障工具运行（Clang 失败、LLD 失败，各覆盖 O0/O2 探针）。
- 每个正例都检查：探针退出 0、Clang/LLD 成功、探针报告的 ELF 退出码与独立期望一致、IR 含预期算术/变量/除法文本、IR 不含 `nsw/nuw`、object 是 x86-64 ET_REL、ELF 是 x86-64 ET_EXEC 且没有 `PT_INTERP`/`PT_DYNAMIC`。
- 驱动另外用空环境独立执行同一个生成的 ELF，核对它的真实退出码；不是只相信探针自己打印的 `elf-exit`。
- 10 个正例产生 10 个不同的 IR 摘要；同一输入在 O0/O2 探针和 O0/O2 Clang 下 IR 文本一致。
- 所有探针运行的旧前端故障 wrapper 日志为空。

本轮实际运行的 stage0 报告 `woozyc 0.1.0-bootstrap (compiler-dev/1)`，Clang 与 LLD 均报告 22.1.8；驱动把版本和摘要写入 `summary.json`。

本地实际输出：

```text
PASS: stage0 builds O0/O2; 40 positive runs, 22 negative runs, 4 tool-failure runs; 10 distinct IR outputs
```

正例与期望退出码：

| 用例 | 覆盖点 | ELF 退出码 |
| --- | --- | --- |
| `value_expr` | 值表达式、乘加优先级 | 14 |
| `variables` | Map 符号解析、变量多次使用 | 51 |
| `precedence` | 括号、`/`、`%`、除法保护 CFG | 8 |
| `unary_min` | i64 最小值字面量 | 0 |
| `wrap` | i64 加法回绕、无 `nsw/nuw` | 0 |
| `utf8_comment` | 中文/emoji 注释后的 UTF-8 位置 | 42 |
| `crlf` | CRLF 行结束 | 30 |
| `div_min` | `min / -1` 与 `min % -1` | 0 |
| `bases` | 二/八/十六进制整数 | 26 |
| `underscores` | 合法数字分隔符（十进制/十六进制） | 232 |

负例与诊断位置：

| 用例 | 诊断 | 位置 | 字节区间 |
| --- | --- | --- | --- |
| `invalid_token` | `WZP001` | 1:11 | `[17,18)` |
| `overflow_literal` | `WZP002` | 1:9 | `[8,27)` |
| `min_magnitude` | `WZP002` | 1:9 | `[8,27)` |
| `redefinition` | `WZP005` | 1:16 | `[15,16)` |
| `unknown_variable` | `WZP004` | 1:19 | `[18,19)` |
| `invalid_utf8` | `WZP009` | 1:15 | `[14,15)` |
| `unterminated_comment` | `WZP013` | 1:12 | `[11,21)` |
| `missing_semicolon` | `WZP003` | 1:11 | `[10,16)` |
| `trailing_token` | `WZP006` | 1:22 | `[21,27)` |
| `empty_input` | `WZP003` | 1:1 | `[0,0)` |
| `bad_integer` | `WZP010` | 1:9 | `[8,12)` |

`invalid_token` 的文件是 `/* 中文🙂 */ @`。`@` 在第 11 个 Unicode 码点列、第 17 个字节；诊断同时保留 `1:11` 与 `bytes [17,18)`，证明 UTF-8 列号和原始字节区间是分开的，而不是把字节偏移直接当列号。

### 2.4 旧前端没有参与探针运行

构建探针时使用现有 stage0 `woozyc`，这是允许且必要的；探针运行时不使用它。证据由驱动自动生成：

1. 探针运行环境 `PATH` 只包含故障 wrapper：`woozyc`、`lang_native_helper`、`woozyc-helper`、`langc`、`clang`、`ld.lld`、`cc`、`gcc`、`as`、`ld`。任何按名称查找这些程序的调用都会写日志并以 97 退出。
2. 探针收到的是驱动传入的 Clang/LLD 绝对路径；它通过 `process.spawn` 使用这些路径，不搜索 PATH。
3. 66 次探针运行的 wrapper 日志全部为空。若探针调用旧前端、旧 helper 或按名称调用编译器，日志不会为空，运行会失败。
4. 构建产物检查：stage0 构建出的探针二进制和探针生成的 ELF 中都不包含 `lang_native_helper` 或 `woozyc` 字符串；探针二进制本身也是静态 ET_EXEC，无 `PT_INTERP`/`PT_DYNAMIC`。
5. 故障工具测试把传入的 Clang 或 LLD 换成一个退出 7 的 wrapper；探针分别返回 3、打印 `clang failed with status 7` 或 `ld.lld failed with status 7`，并且不会伪造 ELF。说明探针确实在执行传入工具，而不是返回预置结果。

因此，本轮可以明确区分：

- **允许**：Root 现有 stage0 编译探针源码。
- **已验证禁止**：探针运行期间调用 `woozyc`、`lang_native_helper` 或旧 C++ 前端来解析/生成用户输入。
- **尚未证明**：未来 stage1/stage2 整个编译器的运行时隔离；那需要同样的 PATH/strace 检查扩展到完整编译器，而不仅是探针。

### 2.5 复现命令

所有实际工具路径由参数传入；驱动不搜索 PATH，也不把机器路径写进提交文件。LLD 若需要发行版兼容 libxml2，用 `--env` 显式传入。

```sh
python3 tests/bootstrap/readiness/run.py \
  --woozyc /path/to/stage0/woozyc \
  --clang /path/to/llvm-22.1.8/bin/clang \
  --lld /path/to/llvm-22.1.8/bin/ld.lld \
  --work-dir tmp/checks/woozy-selfhosting-readiness \
  --timeout 90 \
  --env LD_LIBRARY_PATH=/path/to/libxml2-legacy/usr/lib
```

`run.py` 的每个外部命令都有 `--timeout` 上限；探针还按 `--timeout` 换算出每个工具的上限（至少 1 秒）传给 `run_tool`，对 Clang、LLD 和生成的 ELF 分别计时并在超时时终止、回收直接子进程；外层对整个探针仍有 `--timeout` 兜底。`--probe-opts`、`--ir-opts` 可分别限制为 `0` 或 `2`。结果 `summary.json`、构建日志、IR、object、ELF 都在 `--work-dir`，不进入语言仓库。

## 3 现有 C++ 前端的依赖审计

当前 C++ 前端可以用现有二进制检查/构建当前已支持的程序，但这不等于它用的依赖是语言无法表达的。逐个核对：

| 依赖 | 当前用途 | Wz 迁移路径 | 是否是语言级阻塞 |
| --- | --- | --- | --- |
| GMP/GMPXX 6.3.0 | `src/hir/constant_value.cpp`、`src/hir/literal_value.cpp`、`src/hir/hir.cpp` 的任意精度整数、常量折叠和精确 IEEE 浮点位转换 | 在 Wz 中实现任意精度整数（例如 `Vec[u64]` 限位）或通过未来显式 FFI 绑定 GMP；精确十进制→IEEE 算法可移植 | 否，是移植和性能工作 |
| toml++ 3.4.0 | `src/modules/project.cpp` 读取 `lang.toml` | Wz TOML 解析器，或显式 FFI；先覆盖现有清单语法，再补边界 | 否，是解析器和兼容性工作 |
| json-c 0.19 | `src/modules/project.cpp` 解析 Python 绑定 bundle JSON；`src/driver/main.cpp` 校验 native helper 的 JSON 诊断输出；诊断 JSON 本身由 `src/support/diagnostic.cpp` 手写 | Wz 可直接生成/解析 JSON；也可复用现有 `std/json` 或显式绑定 | 否，是输出/校验/绑定格式工作 |
| LLVM 22.1.8 | Native 后端 `src/backend/native/` 的 LLVM C++ API；Clang/LLD 是外部工具 | 可继续使用 LLVM/Clang/LLD；Wz 后端可走文本 LLVM IR（本探针已验证）或未来显式 FFI 绑定 LLVM C++ API | 否，不要求重写 LLVM |
| Clang/LLD 22.1.8 | Native 目标文件生成和链接 | 继续作为后端工具；探针已经用 Wz 进程接口调用 | 否，不要求重写链接器 |
| 现有 native runtime | 生成程序的分配、ARC、标准库运行时 | 可保留现有 runtime，或按后端需要重写；不是自举前置条件 | 否，不要求本轮重写 |
| f32/f64 位转换 | `src/hir/literal_value.cpp` 用 GMP 做精确舍入到 IEEE 位型 | 用 Wz 大整数实现同样的舍入，或显式 FFI 到 GMP/MPFR；用现有 numeric/floating 向量对照 | 否，是精确数值移植工作 |

结论：**GMP 大整数、TOML 清单解析、JSON 诊断、f32/f64 精确位转换都属于“把已有算法/接口移植到 Wz 或显式绑定”的工程任务，不构成“Wz 不能写编译器”的证据。** 这些移植工作量和风险都不小，尤其是大整数性能和完整 TOML 兼容性，不能跳过实际移植与对照测试。

## 4 自举阶段与可检查条件

以下阶段没有日期承诺。每个阶段的“完成”都必须有实际命令、日志和失败/未运行项，不以文档或生成代码代替运行证据。

### 4.1 阶段 A：开始写 lexer/parser

**当前状态：已达到“可以开始”的最低条件，但只针对核心数据结构。**

可检查条件：

- [x] Wz 能按字节读取源文件并保留原始字节位置。
- [x] Wz 能验证 UTF-8，处理注释、LF/CRLF、标识符和多种整数格式。
- [x] Wz 能用 `Vec`+ID 建 AST，用 `Map` 建符号表，并产生带行列/字节区间的诊断。
- [x] Wz 能生成文本 LLVM IR，并通过 `std/process` 调用 Clang/LLD、运行 ELF。
- [x] O0/O2 构建探针均通过，10 个正例产生 10 个不同 IR，11 个负例位置准确。
- [x] 探针运行期间旧前端/旧 helper 的故障 wrapper 未被调用。
- [ ] 完整 Woozy 词法/语法（包、导入、声明、泛型、模式、错误等）尚未用 Wz 实现。

不确定工作量：受完整语法规模、诊断恢复策略、源码位置/UTF-16 适配和性能影响。探针只证明核心表达可行，不证明完整词法器能一次写完。

### 4.2 阶段 B：移植完整前端

**当前状态：未开始。**

可检查条件：

- 用 Wz 实现包/导入/模块索引、名称解析、类型检查、泛型/trait、结构体/枚举、模式匹配、`throws`/`try/catch`、所有权/ARC 检查和常量求值。
- 对现有正例/负例语料产生与 stage0 等价的接受/拒绝结果；诊断码、主位置和关键关联位置一致或有书面差异记录。
- 覆盖 `tests/syntax`、`tests/modules`、`tests/sema`、`tests/generics`、`tests/ownership`、`tests/cleanup` 等现有前端检查，不跳过环境允许的用例。
- 实现大整数常量、TOML 清单读取、JSON 诊断输出，或通过显式 FFI 绑定；FFI 不能隐藏在“未知类型变 Any”之类的静默行为里。
- Wz 前端进程不执行 `woozyc`/`lang_native_helper`/C++ 前端；用 PATH 故障 wrapper 或 strace 证明。

不确定工作量：这是最大的一块。风险包括泛型实例化预算、trait 静态分派、错误效果、所有权/HIR 验证、常量求值与现有 C++ 实现的逐字节诊断一致性。目前无法给出日期；必须先完成模块/类型/所有权三条线的对照测试。

### 4.3 阶段 C：两个后端

**当前状态：Native 只验证了受限文本 IR；Python 未验证。**

可检查条件（Native）：

- Wz 后端生成的 LLVM IR 通过 LLVM verifier（O0 前和 O2 后都要验证）。
- 支持现有 Native 标量、聚合、错误/清理、ARC、标准库、进程/网络等已声明能力；未支持能力报 `E_CAPABILITY_UNAVAILABLE`，不得静默换后端。
- 同一组程序在 O0/O2 下可观察行为一致；生成的 ELF 在 Linux x86_64 实际运行。
- 链接只使用显式传入的 Clang/LLD/runtime；不调用旧 `lang_native_helper` 生成代码或补功能。
- 现有 `tests/native`、`tests/cleanup`、`tests/ownership`、`tests/generics` 的原生执行用例通过。

可检查条件（Python）：

- Wz 后端生成 Python 3.12 源码，保持现有两后端共同语义子集；NumPy/PyTorch 绑定走声明式接口，不在检查期启动 Python 或猜接口。
- 同一组程序在 Native 与 Python 上的结果、错误和清理行为对照通过；差分测试不能替代独立预期值。
- 现有 `tests/python`、`tests/python_bindings`、CPU 绑定用例通过。
- 不支持的能力在生成阶段明确拒绝，不把执行 Python 说成原生编译。

不确定工作量：Native 的主要风险是 LLVM IR/ABI/所有权/清理覆盖面和 O0/O2 一致性；Python 的主要风险是语义适配、绑定和清理/ARC 对照。若选择 LLVM C++ API 的 FFI，还要处理 C++ ABI、对象生命周期和异常边界；若坚持文本 IR，则要证明完整 Native 能力都能用文本 IR 表达，而不是只覆盖标量。工作量高且不确定。

### 4.4 阶段 D：自举固定点

**当前状态：未开始。**

可检查条件：

- 同一份 Wz 编译器源码由 stage0 构建出 stage1；stage1 再构建 stage2；stage2 再构建 stage3。
- stage2 与 stage3 的源码、构建参数和测试输入相同；二者对完整测试集的接受/拒绝、诊断、Native/Python 产物语义一致。可重复构建时比较规范化 ELF/object 摘要；不能重复的部分要写明原因。
- stage1/stage2/stage3 运行时不调用 `woozyc`、`lang_native_helper` 或 C++ 前端；用 PATH 故障 wrapper、strace 或等价的进程审计证明。
- 完整 Native/Python 功能集与标准库检查通过，不只跑探针用例。
- O0/O2 构建和生成程序都执行；Linux x86_64 之外不声称完成。
- 固定点失败时保留 stage1/stage2 源码、命令、日志和最小复现，不把失败包装成“部分自举通过”。

不确定工作量：取决于阶段 B/C 的完成度、Wz 编译器自身的性能/内存/ARC 行为、诊断一致性和可重复构建。没有日期；在 B/C 没有实际对照通过前，不应开始宣称固定点临近。

## 5 风险与非目标

- **不重写 LLVM/Clang/LLD/Runtime。** 自举替换的是 Woozy 前端和两个后端；LLVM、Clang、LLD 和现有 native runtime 可以继续使用。探针走文本 IR + Clang/LLD 路线，说明不绑定 LLVM C++ API 也能生成真实 ELF；完整后端是否走 FFI 另行决定。
- **不强制 libc。** 探针生成的 ELF 只使用 Linux `exit` 系统调用，`ld.lld -static` 链接，无 libc、无动态解释器。未来程序若选择 libc，应由用户/目标配置显式选择，不能让核心探针偷偷依赖。
- **不接受旧前端冒充。** 任何阶段发现不支持的能力，必须报诊断或明确失败；不能在 stage1/stage2 中 exec 旧 `woozyc`/`lang_native_helper` 后仍报告自举成功。
- **不把探针叫完整编译器。** 它没有类型系统、函数、聚合、泛型、错误效果、所有权/ARC、标准库和 Python 后端；它只覆盖受限表达式和 IR/工具链闭环。
- **不以文档代替运行证据。** 阶段 B/C/D 的完成必须附实际命令、日志、失败/未运行项；缺少环境的测试记 `not_run`，不能标 `passed`。
- **不硬给日期。** 当前只能给出可检查条件和风险，不能从 10 个正例推断完整前端或固定点的完成时间。

## 6 主要不确定点

1. **规模与性能**：完整前端和两个后端的代码量远大于探针；Wz 当前的 ARC/Vec/Map 行为和编译性能需要在大项目上测量，探针没有覆盖。
2. **诊断等价性**：完整自举要求与现有 C++ 前端在错误码、主位置、关联位置和恢复行为上可比；这不是简单重写词法器。
3. **大整数与浮点**：GMP 路径的精确舍入和性能需要 Wz 实现或显式 FFI 对照；边界向量必须复用现有测试，而不是只测几个常量。
4. **TOML/模块系统**：清单解析、依赖图、标准库解析和路径规则都要与现有模块测试对照；只支持一个子集不能算完整前端。
5. **两个后端语义一致**：Native 的错误/清理/ARC/ABI 与 Python 的对象/清理/绑定语义都要逐项对照；不能用一个后端的成功代替另一个。
6. **固定点可重复性**：ELF/object 可能受路径、时间戳、哈希顺序影响；需要规范化比较或明确不可重复项。stage2/stage3 行为等价优先于字节相同，但不能用“等价”掩盖测试缺失。

## 7 结论性回答

- **现在可以开始写编译器前端**：Wz 已经能表达受限 lexer/parser/Map/诊断/文本 IR/进程调用，并在 O0/O2 下生成和运行真实静态 ELF；旧前端没有参与探针运行。
- **现在还不能说“语言可以自举自己的编译器”**：还没有同一份 Wz 编译器源码的 stage0→1→2 链，也没有现有 Native/Python 功能的 Wz 实现。
- **GMP/toml++/json-c/f32-f64 位转换不是语言级阻塞**：它们是可以在 Wz 中实现或通过未来显式 FFI 绑定的移植工作；需要实际移植、对照和性能测量。
- **下一步的硬门槛**是阶段 B 的完整前端对照，而不是继续扩大探针语法；只有阶段 B/C 有实际通过记录后，阶段 D 的固定点检查才有意义。
