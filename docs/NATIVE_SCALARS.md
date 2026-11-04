# T05 原生标量实现与交接

本文保留此前阶段记录。当前 Vec、拥有字段及 move 执行范围见 [T09_EXECUTION.md](T09_EXECUTION.md)。

原生严格f32/f64四则、负号、六种比较和复合赋值已实现，见
[浮点执行说明](../tests/native/STRICT_FLOAT.md)。原76探针及两个baseline副本已实际执行。
当前三种完整构建各34/34、LLVM关闭26/26；后文保留各开发阶段的记录。

原生后端已支持typed throws/catch和词法块defer，见
[错误与清理执行说明](../tests/native/T08_EXECUTION.md)。throws函数采用成功位和
分别保存正常值、错误值的输出参数，入口未处理错误打印稳定类型键并退出1。
运行库原接口`lang-native-scalars/1`保留。以下聚合、字符串和标量小节保留各阶段记录。

常量字符串已支持UTF-8字面量、局部复制/重新绑定及普通参数/返回，见
[字符串实现](CONSTANT_STRINGS.md)。三种完整构建各28/28，关闭LLVM22/22；
该字符串阶段未实现动态字符串、ARC或严格浮点；后续浮点执行结果见上方说明。


## 2026-11-04：平凡值 struct/enum

operation_id：`language-native-pod-aggregates-20261104`；起点为
`1bff58aa6c50b946bc6d7a5761d1bf486e513aa4`，契约仍为 `compiler-dev/1`。
本节更新下方历史记录中的聚合未支持说明，不修改共享规范和原有 fixtures。

已实现平凡 struct/enum 的构造、复制、嵌套字段读写、普通按值参数/返回、
函数字段调用、enum 模式绑定/通配、case 独立作用域，以及 switch/for 内的
break/continue/return。字段仅允许 Unit、bool、整数、f32/f64 位模式、
无捕获函数值和递归仅含这些字段的聚合；浮点运算、拥有字段、字符串、Vec、
ARC、foreign、throws/defer、泛型 inout/move 仍不支持。

Native IR 的公开变化在 `include/lang/ir/native.hpp`：追加
`struct_init`、`enum_init`、`field_read`、`field_address`、`load_indirect`、
`store_indirect`、`enum_test`、`enum_payload`；Instruction 新增已解析的
`field` 与 `variant` SymbolId。原有枚举编号保持不变，store_indirect 无结果，
field_address 的 type 是所指字段类型。构造操作数已经按声明顺序排列，
其定义仍按源码顺序执行。switch 使用 enum_test 和条件边，payload 提取只在
对应变体的真分支中执行。case 绑定各自使用 HIR SymbolId；break/continue
直接使用 HIR 已确定的 switch/for 目标，continue 到 for 步进且只执行一次。

`src/ir/layout.hpp` 在私有 TypeContext 副本上计算替换后的字段类型。
新增的私有类型只用于检查或 LLVM 布局，不能写入 Native IR。布局递归到
函数字段立即停止；实际函数和 Call 的签名另行检查。
`finite-function-field` 中 `S[T].f: func() S[S[T]]` 经正式 CLI 在 O0/O2
生成执行，未调用 bottom；共享类型表始终保持不变，测试要求条目数少于 100。

LLVM 的私有 ABI 使用声明顺序 struct，enum 使用
`{ i32 tag, { variant0 fields }, { variant1 fields }, ... }`。
未活动载荷初始化为零，只读取活动载荷。普通参数与返回使用 LLVM aggregate
值；局部存储使用目标 DataLayout 的 alloca/load/store，字段地址由已验证的
字段索引生成。嵌套写入不会改变此前复制出的值。不使用宿主 sizeof，也没有
增加 noalias、readonly、fast-math、nsw/nuw 或运行库入口；PanicABI 保持不变。

Native IR verifier 在原有操作数、跳转、初始化与支配检查之外，检查构造字段
数量/类型、接收者类型、字段/变体声明身份、地址和值的区别。载荷读取要求
所有进入路径都通过同一 SSA enum 值对应变体的真分支；交换真假目标、绕过
判断、读取其他变体都会失败。21 种新增损坏 CFG 与原 18 种标量损坏 CFG
均由本语言 verifier 拒绝；随后 LLVM verifyFunction/verifyModule 检查生成结果，
优化前后 LLVM 文本还交给同版本 opt 验证。

GNU 16.2.1 Debug/Release 各实际构建运行：

| 检查 | 每套结果 |
| --- | --- |
| 原 13 个 accept 项目，原 cases.json runtime_expected，O0/O2 | 26 passed |
| 原 3 个附加标量泛型项目，O0/O2 | 6 passed |
| 新增 7 个 POD 项目，O0/O2 | 14 passed |
| 原 scalar CLI 执行及普通返回 7 对照 | 64 + 2 passed |
| 拥有字段、浮点运算、浮点字段复合赋值、defer | 8 unsupported；16 次失败构建均无新增输出或已有输出改动 |
| 原 sema.t07 原源码/预期 | failed：仅旧 aggregate 拒绝断言已不适用，前 27 个静态 fixtures passed |
| 临时副本应用独立 sema 预期补丁 | passed |
| 完整 CTest | 24 passed、1 failed（上述原 sema.t07） |
| Clang22 ASan/UBSan | not_run，由 root 随后执行 |

7 个新增运行项目为 copy-return、empty-and-float、enum-control、enum-copy、
enum-eval-order、finite-function-field、function-fields。
原有 Python 标量/泛型测试用外层已安装的 3.12.15 运行，未改 Python 实现。
每个实际生成项目的源摘要、正式 CLI 命令、对象/ELF 摘要、退出值和输出保存在
工作区外的两套逐项 JSON。另对同一对象使用 Clang/LLD 22.1.8 独立链接、执行，
并核对与 CLI 生成程序完全相同的退出值及 stdout/stderr。

主代理已合并共享检查，并把 sema.t07 改为按实际 backend/word_bits 创建聚合
程序。native单独集成时接受native、拒绝Python；Python集成后两端均接受。
native阶段GNU Debug、GNU Release和Clang22
ASan/UBSan完整CTest各25/25通过，无排除项。另加入宽整数嵌套泛型enum复制和
函数字段改写两个项目，各构建O0/O2通过。原27个fixtures和cases.json未改。
agent原始共享补丁和预期补丁保留在外层reviews/，未作为源文件提交。
主代理的实际运行记录保存在外层checks/t07-native-pod-root-verification.json。
两后端合并后完整CTest各26/26，源码比较记录另见外层
checks/t07-pod-backend-comparison.json；运行库、ARC及严格浮点运算的限制仍然保留。

首轮检查记录与修正后的结果分别保留：首轮新增样例误用了浮点一元负号和
缺括号的函数返回类型，capability 也误拒既有裸字符串 HIR 测试；这一次修正
只调整新增样例，并将字符串拒绝限定到拥有字段。裸字符串 HIR 既有验证行为
保持不变，native emitter 仍拒绝字符串生成。首次 CMake 选中 3.12.14 后，
通过 LANG_PYTHON312 指定已有的 3.12.15；未修改 Python 安装或测试预期。

以下保留先前阶段的接口与检查记录。

operation_id：`language-t05-native-scalars-20261103`。契约：`compiler-dev/1`。
基础 HEAD：`b8ccf384a16e85141994995fe1869fd5fc084100`。

2026-11-04：root 已接入正式 `langc build --backend native --out-dir DIR --opt 0|2`，
输出 program.o 和 program，构建不运行程序。GNU Debug、GNU Release、Clang22
ASan/UBSan 各21/21组测试通过，包含正式CLI的34次主项目执行和2次普通返回7对照。
组件的独立62次ELF执行另有记录；两组数字不重复合计为不同源码数。

本文件下方保留组件实现和早期验证记录。T05整项仍为in_progress，
简单结构体和常量字符串需在T07接口发布后补齐。

## 2026-11-04：T07 具体标量实例

operation_id：`language-native-t07-scalar-instances-20261104`。本次起点为
`fb8ba96ebe1a911ca3a036949c15484e58769519`，契约仍为 `compiler-dev/1`。
下方 T05 记录中的“泛型未实施”由本节更新；聚合与字符串生成仍未实施。

`ir::lower` 和两个 `native::emit` 公开入口先调用公共
`hir::check_backend_capabilities`；失败时保留它的 state、failure、diagnostics，
不返回对象。native/64、Linux x86_64、generic CPU、空 features、O0/O2 和
LLVM 22.1.8 限制继续有效。删除了 Emitter 对整个 TypeContext 的拒绝扫描，
仅降低具体 CFG 函数的签名、存储和指令所使用的类型。未使用的 nominal 声明、
模板类型和模板体保留在公共 HIR 中，不生成 LLVM 函数或类型。

已支持具体标量泛型函数、同实例递归、跨包别名、静态 trait 方法选择，以及函数类型
实参与 callback 参数。实际构造或使用 struct/enum、字段及 switch 继续返回
`E_CAPABILITY_UNAVAILABLE`，不进入 `E_IR_INVALID` 或产生部分输出。
没有修改前端、共享接口、CLI/helper、Python 后端、既有 27 个泛型项目或任务清单。

`tests/native/run_generics.py` 从未改的 `tests/generics/cases.json` 读取七项
`runtime_expected.main_return`。另外三个小项目位于 `tests/native/generics`。
下表每格在 GNU Debug、Release 下分别经正式 CLI build、链接和 ELF x86_64 执行；
全部 stdout/stderr 为空，均为普通返回，未被当作 Panic。

| 项目 | O0 exit | O2 exit |
| --- | ---: | ---: |
| explicit | 7 | 7 |
| inferred | 8 | 8 |
| return-context | 0 | 0 |
| multi-instance | 3 | 3 |
| function-argument | 5 | 5 |
| same-recursion | 5 | 5 |
| cross-package-alias | 7 | 7 |
| trait-scalar | 9 | 9 |
| unused-aggregate | 0 | 0 |
| callback | 11 | 11 |

每套构建新增 20 次执行通过。六个既有聚合正例（trait-unique、fields-reordered、
nested-copy、enum-empty、enum-payload、field-evaluation-order）在 O0/O2 各做两次
失败构建：空输出目录不写文件，已有成功输出不改变。每套 24 次均只有 E_CAPABILITY_UNAVAILABLE 诊断，
报告按 12 个“项目/优化等级”记录为 unsupported。

`native.cli` 也覆盖全部原 native 标量项目和新增 callee 求值次序项目，每套 64 次
主测试执行及 2 次普通返回 7 对照通过；其中原 15 shared 与 core/numeric 是 34 次。
覆盖 callee 只求值一次且先于实参、命名实参源码顺序、inout 转传、整数回绕、
除法/移位/转换 Panic 和无 Panic 的普通非零返回。

本次 GNU 16.2.1 Debug 与 Release 各 22/22 CTest passed，包含 native.ir、
native.llvm、native.cli、native.generics、sema.t07；显式排除旧 python.projects
分类错误，由另一任务处理，本次记为 not_run。Clang ASan/UBSan 留给 root 统一运行，
本次未运行。T05/T07 整项状态不变；聚合、字符串、ARC、foreign 和严格浮点执行
不在本次实现范围内。

复现命令（`tc` 由调用者指定为已有外层 toolchains，不安装软件）：

```sh
export LD_LIBRARY_PATH="$tc/libxml2-legacy-2.13.9/usr/lib"
cmake -S . -B /tmp/language-native-t07-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DLANG_ENABLE_LLVM=ON -DLLVM_DIR="$tc/llvm-22.1.8/lib/cmake/llvm" \
  -Dzstd_STATIC_LIBRARY="$tc/zstd-1.5.7/lib/libzstd.a"
cmake --build /tmp/language-native-t07-debug -j4
ctest --test-dir /tmp/language-native-t07-debug --output-on-failure \
  -E '^python.projects$'
```

Release 使用独立目录及 `-DCMAKE_BUILD_TYPE=Release`。实际命令、构建日志、完整
CLI 参数与输出留在 `/tmp/language-native-t07-{debug,release}*`；每套的
`tests/native/{cli-results,generic-results}/results.json` 保存完整执行记录。
原worktree的 `tests/native/generic-results.json` 已保留在外层
checks/t07-native-worker-report.json，含源码SHA-256、ELF检查和执行结果，未加入源代码仓库。

主代理合并两后端后，GNU Debug、Release和Clang22 ASan/UBSan完整测试各24/24通过，
不再排除Python项目。7个公共源码的O0/O2及Python结果一致，另交叉运行两后端的
6个附加源码，均符合预期。记录见外层checks/t07-native-python-comparison.json。

## API 与 CMake

公共入口为 `include/lang/backend/native.hpp`：

```cpp
lang::PhaseResult<lang::backend::native::Output> emit(
    const lang::hir::VerifiedHirModule &,
    lang::SymbolId entry,
    const lang::backend::native::Options & = {});
```

另有接收 `const lang::ir::VerifiedModule &` 的重载。入口 SymbolId 由调用者从
ResolvedProject 选择。后端不解析源码，不查变量名或调用名字，不复制 TypeContext。
`Output` 包含 `object` 字节、`native_ir`、`llvm_before_optimization`、`llvm_ir`、
`triple` 和 `data_layout`。`Options` 目前仅接受 triple
`x86_64-unknown-linux-gnu`、CPU `generic`、空 features 和优化等级 0/2；其它配置
返回 `E_CAPABILITY_UNAVAILABLE`。可执行入口支持无参 `i32` 或 `Unit`。

`include/lang/ir/native.hpp` 发布 `ir::lower`、`ir::verify`、`ir::dump`。
`VerifiedModule` 只能由 verifier 发布，复制并持有不可变 CFG；它引用同一公共
VerifiedHirModule 中的类型。统一使用已有 BlockId/ValueId/LocalSlotId/TypeId/
SymbolId/SourceSpan，没有另建共享 HIR。正式链接驱动新增已登记诊断 E_LINK_FAILED。

root 可在现有前端目标之后添加：

```cmake
add_subdirectory(src/backend/native)
```

该片段定义 `lang_native_ir`、`lang_native`、`lang_native_runtime` 和
`lang_native_helper`。LLVM 只出现在 native 目标中；`langc` 链接前端、Python发射器和进程工具，不链接LLVM或CPython。
不应把 lang_native 链接到 check 或未来的 LSP 目标。

helper 的参数：

```text
lang_native_helper --project MANIFEST
  [--entry NAME | --entry-symbol ID] [--opt 0|2]
  [--target x86_64-unknown-linux-gnu] --out-object FILE
  [--out-llvm FILE] [--out-llvm-before FILE] [--out-native FILE]
  [--format json]
lang_native_helper --project MANIFEST --check-only [--format json]
```

默认入口为入口模块中的 main；名字选择发生在 helper 这个调用者内，库只接收 ID。
helper 明确选择 native 后端，保留清单字宽。每次进程只执行一次真实
load/resolve/check，再执行 lower/verify/LLVM/对象输出；没有 HIR 跨进程序列化。
`--check-only` 只报告静态结果，不声称后端支持该字宽。生成成功的 JSON 标记
`phase=object`，链接和执行由调用者完成。诊断写 stderr，退出码采用共享 PhaseResult；
输出写入失败为 `E_INPUT_IO`/2。

正式 build CLI 通过 helper 的 --out-dir 模式生成并链接；显式对象输出模式仍供测试使用。
build 默认不执行程序；执行步骤属于本测试 runner。

## 实现规则

HIR 首先展开为有类型值、slot、基本块和单个 terminator。循环使用 HIR 的目标 ID，
continue 跳到步进块；短路通过条件边和临时 bool slot 实现。lowering 删除没有入口可达
前驱的生成块，避免在两侧都 return 后继续产生不可达表达式。

原生 verifier 检查函数集合/签名、参数位置/模式、值的唯一性与先定义后使用、
跨块支配关系、分支目标、操作数/调用/返回类型，以及 slot 的所有可达前驱都已初始化。
不能用 LLVM verifier 代替这些检查。LLVM 发射器随后运行 verifyFunction、
verifyModule；O2 使用 PassBuilder 标准 O2 流程，然后再次 verifyModule。

- Unit、bool、全部整数、函数值和无捕获 lambda 使用确定的标量表示。
  私有 ABI 中 Unit 为 i8 零值，bool 为 i1，整数保持位宽，函数值为代码指针；
  inout 为标量存储指针。Unit 入口的 C main 返回 0，i32 入口原样返回。
  这不是已发布的 C/foreign ABI。
- 函数预声明后再生成函数体，支持直接/间接调用、递归及跨模块调用。
  callee 先求值一次，再按源码次序保存实参，最后依据 parameter_index 组装调用。
  空 label 的丢弃参数仍按位置和 SymbolId 接收。
- 普通局部存储只在入口块分配；循环反复进入不会不断增加 alloca。
  整体标量 inout 直接传存储地址，支持继续传给另一个 inout 参数。
  不添加未证明的 noalias/readonly/noescape 属性。
- 加减乘与负号不手工加 nsw/nuw。除零先进入 Panic；MIN/-1 和 MIN%-1
  分别返回 MIN 和 0，普通分支才执行 sdiv/srem。
- 移位量先依自身符号性扩展到 i65，检查负值及大于等于左值位宽，再转成左值
  位宽执行 shift。右值类型不会改变左值类型。动态转换也在 i65 中比较目标上下界，
  合法后才转换，覆盖完整 u64 范围。
- 使用 IRBuilder NoFolder 保留 O0 检查分支，防止字面量危险运算提前折成 poison。
  LLVM 的后续优化可以根据已经证明的条件简化代码。
- f32/f64 常量由 HIR 的确切位模式建立 APFloat，不经过 host double。
  支持字面量、参数、返回、绑定和普通传递。没有 fast-math；严格浮点算术未实施。

目标由 LLVM 22.1.8 TargetMachine 固定为 Linux x86_64/PIC/small/generic；
实际 DataLayout 为：

```text
e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128
```

Native IR dump 保存每个操作的 SourceSpan；Panic 记录保存 snapshot/begin/end。
本阶段没有生成 DWARF 行号，也未实现稳定跨构建符号 ABI。

## Panic 与未支持项

运行库 ABI 为 `lang-native-scalars/1`，符号为 `lang_native_panic_v1`。
运行时向 stderr 写一条记录，然后 `_Exit(101)`：

```text
LANG_NATIVE_PANIC:v1:division-by-zero:2:57:62
LANG_NATIVE_PANIC:v1:invalid-shift:2:97:111
LANG_NATIVE_PANIC:v1:integer-conversion:2:80:90
```

runner 同时检查完整记录、原因、空 stdout 与非零/信号终止。普通非零 main、
无记录的 SIGSEGV、加载/链接失败、超时均不算 Panic 通过。返回 7 的源程序实际执行
了 O0/O2，两次都没有被判为 Panic。四个共享 Panic 案例没有更改预期，错误实现若
没有 Panic 而返回 0 就会失败。

string、聚合、泛型、throws/try/typed catch、defer、ARC/move、foreign、复杂 Place、
捕获 lambda、严格浮点算术/比较/转换均未实施，由 checker 或目标检查报告
`E_CAPABILITY_UNAVAILABLE`。普通参数重新赋值仍遵循 checker 的未支持诊断，
没有自行决定写权限。native32、其它目标/CPU/features/O 等级均未开放。
Panic 直接终止，不进入 typed catch，不执行语言 defer；本阶段不保证 Panic 无泄漏。

## 实际命令

本机 Linux x86_64，GNU 16.2.1；LLVM、Clang、LLD 均精确为 22.1.8。
前端使用已安装的 GMP/GMPXX 6.3.0、toml++ 和 json-c。仅使用外层已有工具链，
没有联网下载或替换系统库。以下 `tc` 指向外层 toolchains，路径由调用者提供：

```sh
tc=/home/qin/Work/language-dev-20261103/toolchains
export LD_LIBRARY_PATH="$tc/libxml2-legacy-2.13.9/usr/lib"
cmake -S tests/native -B /tmp/language-t05-native-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLLVM_DIR="$tc/llvm-22.1.8/lib/cmake/llvm" \
  -Dzstd_STATIC_LIBRARY="$tc/zstd-1.5.7/lib/libzstd.a"
cmake --build /tmp/language-t05-native-build -j4
ctest --test-dir /tmp/language-t05-native-build --output-on-failure
python3 tests/native/run.py --build /tmp/language-t05-native-build \
  --toolchains "$tc" --out /tmp/language-t05-native-runs \
  --report tests/native/verification.json
```

单项目编译、链接和执行的完整示例：

```sh
mkdir -p /tmp/t05-core
/tmp/language-t05-native-build/native/lang_native_helper \
  --project tests/fixtures/core/lang.toml --entry main --opt 2 \
  --out-object /tmp/t05-core/core.o --out-llvm /tmp/t05-core/core.ll \
  --out-native /tmp/t05-core/core.nir --format json
"$tc/llvm-22.1.8/bin/clang" --target=x86_64-unknown-linux-gnu \
  --ld-path="$tc/llvm-22.1.8/bin/ld.lld" /tmp/t05-core/core.o \
  /tmp/language-t05-native-build/native/liblang_native_runtime.a -o /tmp/t05-core/core
/tmp/t05-core/core
```

runner 通过 argv 运行 Clang、LLD、opt 和 ELF 程序，没有 shell 拼接。Python 只负责
启动进程和写测试记录，没有解释或执行源语言/Python 替代程序，也没有 C++ 探针充作
源语言执行。15 个共享 native64 静态前提全部成功后才开始对象生成。每次输出的
优化前后 LLVM 文本还交给同版本 opt 再次验证。产物存放在受控 /tmp 目录。

Clang22 ASan/UBSan 构建使用同一个 standalone CMake 工程：

```sh
cmake -S tests/native -B /tmp/language-t05-native-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER="$tc/llvm-22.1.8/bin/clang" \
  -DCMAKE_CXX_COMPILER="$tc/llvm-22.1.8/bin/clang++" \
  -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined \
  -DLLVM_DIR="$tc/llvm-22.1.8/lib/cmake/llvm" \
  -Dzstd_STATIC_LIBRARY="$tc/zstd-1.5.7/lib/libzstd.a"
cmake --build /tmp/language-t05-native-asan -j4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir /tmp/language-t05-native-asan --output-on-failure
```

该检查覆盖前端、CFG、LLVM 生成与位模式测试。没有声称做过 LeakSanitizer，
也没有把带 sanitizer 的 C++ 生成器测试当作源语言程序运行。

## 结果与证据

`tests/native/verification.json` 保存每个 O0/O2 项目的源码 SHA-256、helper 参数、
CFG/LLVM 验证结果、target/DataLayout、Clang/LLD 参数、链接退出码、ELF x86_64
检查、二进制摘要及执行 exit/stdout/stderr。`tests/native/ctest.json` 保存两套
真实 CTest 的命令与输出。全部结果基于本 worktree，而非旧 rejected 任务代码。

下表每一行的 O0/O2 都实际编译、链接并执行，Panic 行都检查了记录；其它行无输出。

| 项目 | O0 exit | O2 exit | Panic 原因或普通返回 |
| --- | ---: | ---: | --- |
| i8-u8-wrapping | 0 | 0 | 普通返回 |
| signed-division-remainder | 0 | 0 | 普通返回 |
| division-min-over-minus-one | 0 | 0 | 普通返回 |
| bitwise-integers | 0 | 0 | 普通返回 |
| shift-left-right | 0 | 0 | 普通返回 |
| dynamic-integer-conversions | 0 | 0 | 普通返回 |
| short-circuit-panic-guard | 0 | 0 | 普通返回，无 Panic |
| function-values | 0 | 0 | 普通返回，含 lambda |
| control-flow-break-continue | 0 | 0 | 普通返回 |
| lexical-shadowing | 0 | 0 | 普通返回 |
| inout-argument-order | 0 | 0 | 普通返回 |
| panic-division-by-zero | 101 | 101 | division-by-zero |
| panic-invalid-shift | 101 | 101 | invalid-shift |
| panic-dynamic-conversion | 101 | 101 | integer-conversion |
| panic-negative-shift | 101 | 101 | invalid-shift |
| core | 0 | 0 | 普通返回 |
| numeric | 0 | 0 | 普通返回 |
| mutual-recursion | 0 | 0 | 同包跨文件相互递归 |
| cross-package-call | 0 | 0 | 外部源包调用 |
| arithmetic-literals | 0 | 0 | 字面量 MIN/-1、余数、u64 shift |
| float-transfer | 0 | 0 | f32/f64 字面量及传递 |
| nonzero-main | 7 | 7 | 普通非零返回，无 Panic |
| panic-huge-unsigned-shift | 101 | 101 | invalid-shift |
| panic-negative-conversion | 101 | 101 | integer-conversion |
| panic-remainder-zero | 101 | 101 | division-by-zero |
| panic-u64-to-i64-conversion | 101 | 101 | integer-conversion |
| recursive-inout-bool | 0 | 0 | 递归、bool inout 转传 |
| unit-discard-lambda | 0 | 0 | Unit、重复丢弃参数、嵌套/返回 lambda |
| unit-main | 0 | 0 | Unit 入口 |
| unreachable-control | 0 | 0 | 两侧 return 后的不可达调用 |
| wide-shifts-conversions | 0 | 0 | 各位宽回绕、不同 RHS 类型、完整 u64 范围 |

实际结果：62/62 次源语言 ELF 执行通过，其中共享 15 例 30/30、core/numeric 4/4。
GNU Debug 与 Clang22 ASan/UBSan 各 16/16 CTest 通过。native.ir 拒绝 18 种损坏
CFG，并检查不可变发布、O0/O2 对象输出及不支持目标。native.llvm 从真实源码生成
Verified 后检查 f32/f64 ties-to-even 和最小 f32 subnormal 的精确位模式，无 fast-math；
直接用 LLVM C++ API 构造的无 terminator 和错误返回类型模块被 verifyModule 拒绝。
另有合法语法但破坏支配关系的 LLVM 文本被 opt verifier 拒绝。

native32、普通参数写入、严格浮点算术、string、defer 五类实际输入均得到未支持诊断；
不计入功能执行通过数。`langc` 的动态依赖和未解析符号检查未发现 LLVM。
前端 14 个既有 CTest 保持通过；当前没有 LSP 实现可运行。

## 首轮失败及修正

首次完整运行中，30 次执行成功，另外 24 项在链接处失败。原因是 GNU 为 noexcept
Panic 函数产生了 `__gxx_personality_v0` 引用，而 C 链接命令没有 C++ 异常运行库。
记录见 `tests/native/first-run-failures.json`。该运行库只调用 libc 且从不抛出，
现通过 `-fno-exceptions -fno-rtti` 编译；修正后所有含 Panic 的共享案例和额外边界例
均实际链接、运行成功。此规则已经写入 native CMake，无需使用者另补 C++ 异常库。

## 正式 CLI 集成结果

```sh
build/native-cli/langc build --project tests/fixtures/core/lang.toml \
  --backend native --out-dir build/core/native --opt 2 --format json
build/core/native/program
```

默认O0；目标仅支持 x86_64-unknown-linux-gnu。正式模式输出共享诊断JSON，
对象私有模式仍保留先前元信息格式。helper 和链接器通过直接argv调用，
临时文件在同一输出目录创建，程序最后替换；源码错误不修改已有输出。
独立审查发现第二次替换失败时旧object可能被覆盖，已补目的文件类型检查和旧文件备份。
第二次替换报错会恢复旧object；没有旧object时移除新文件。恢复失败保留备份并报告位置。
Python的同类发布代码一并修复。两个后端各3项实际文件替换故障测试通过，含恢复失败；
这不保证进程被中断或掉电时两个文件同时替换。

主目录实际报告位于外层 checks/t05-native-cli-verification.json；
独立对象测试位于 checks/t05-root-runtime-verification.json。
上文 tests/native/verification.json、ctest.json 和 first-run-failures.json 为
原组件worktree的历史记录，没有复制进源代码仓库。

集成的首次ASan构建暴露运行库误继承编译器检查选项，造成 __asan_* 和 __ubsan_*
链接未定义。已对生成程序使用的运行库设置 -fno-sanitize=all，
编译器、helper、HIR/CFG和发射器继续检查；重新运行21/21通过。
预编译LLVM、目标程序和该运行库不包含在ASan覆盖声明内，LeakSanitizer未运行。

待完成：T07发布后接入简单聚合与静态字符串，继续明确拒绝尚未实施的操作。
