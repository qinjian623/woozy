# Python 标量实现与验证

## T07 具体标量泛型实例接入（2026-11-04）

operation_id：`language-python-t07-scalar-instances-20261104`；契约 `compiler-dev/1`。
本次 HEAD 为 `fb8ba96ebe1a911ca3a036949c15484e58769519`，开始时工作树干净。
读取了该提交的 HIR、共用检查函数和静态泛型测试；没有修改共享 HIR、sema、CLI、
Native、运行库、contracts 或既有 27 个泛型项目。

`python::emit` 入口调用 `hir::check_backend_capabilities`，失败或取消时保留它的
state、failure 和诊断，无部分输出。Python/64 和入口签名限制保留。
生成器只从 `m.functions` 的签名、参数和每个 body 的子节点收集类型、局部名字与 Cell；
不再遍历整个节点 arena。模板中的 TypeParameter、聚合类型、构造、字段操作和 switch
仍由静态检查器检查，但未实例化的模板不生成 Python。具体实例已含确定的 SymbolId 和类型，
emitter 直接使用这些 ID，没有重新按名字查找或自行实例化。

callee 一次求值、实参源码顺序、inout Cell、continue step、函数值和非捕获 lambda 的
生成规则未改。真实聚合构造、字段读写、enum switch 和严格浮点操作继续报告
`E_CAPABILITY_UNAVAILABLE`；Verified 输入仍只由共用 `hir::verify` 发布。

`tests/python/run_generics.py` 读取原 `tests/generics/cases.json` 中的预期返回值。
每个项目分别执行正式 `langc check`、`langc build --backend python`，再以独立的
Python **3.12.15** 进程运行生成的 `program.py`。check/build 子进程的 PATH 只含会失败的
Python 命令，确认它们无需通过 PATH 启动 Python；代码检查也确认 Python build 分支只写文件。
执行阶段使用指定解释器绝对路径。生成器没有新增解释器调用或 Python 导入。

| 原公共项目 | Debug / Release 的实际 main 返回值 | 状态 |
| --- | --- | --- |
| explicit | 7 / 7 | passed |
| inferred | 8 / 8 | passed |
| return-context | 0 / 0 | passed |
| multi-instance | 3 / 3 | passed |
| function-argument | 5 / 5 | passed |
| same-recursion | 5 / 5 | passed |
| cross-package-alias | 7 / 7 | passed |

七项均无 stdout/stderr，普通非零返回按 `ordinary_nonzero` 记录，不当作 Panic。
同一 runner 另生成并实际运行三个小源码：i32 的 trait 约束（显式与推断实例）、
泛型 callback 的函数类型参数及模板内非捕获 lambda、未实例化但含泛型 struct/enum
构造与字段读写和 switch 的模板。两种构建中三项均返回 0。

旧 `UNSUPPORTED_SOURCES` 的 aggregate/generic 只有未使用声明，已移入正例并实际运行。
新增真正执行 struct 构造、字段读取、字段写入、enum switch 的四个拒绝例，检查诊断 code、
原文件和有效字节区间，且没有生成输出目录。原 shared15 源码与预期未改。
API 测试另检查具体多实例输出不含模板原始符号，并比较取消时共用检查结果。

本工作树 GNU 16.2.1、Linux x86_64，Debug 与 Release 完整 CTest 各 **20/20 passed**
（`LANG_ENABLE_LLVM=OFF`），包含四项 Python 测试。每种构建的 `python.projects` 都保留
17 个必测项目、原 15 个补充执行和 5 个分类对照；加上两个未使用声明正例后补充为 17 项。
14 项拒绝检查记为 `unsupported`，不计为功能运行通过。普通 return 101 与语言 Panic、
宿主 Python 异常的区分保持通过。

复现命令（`PY312` 指向外层 `toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12`）：

```sh
cmake -S . -B build/t07-python-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLANG_ENABLE_LLVM=OFF -DLANG_PYTHON312="$PY312" -DLANG_TEST_PYTHON="$PY312"
cmake --build build/t07-python-debug -j4
ctest --test-dir build/t07-python-debug --output-on-failure -j4
cmake -S . -B build/t07-python-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLANG_ENABLE_LLVM=OFF -DLANG_PYTHON312="$PY312" -DLANG_TEST_PYTHON="$PY312"
cmake --build build/t07-python-release -j4
ctest --test-dir build/t07-python-release --output-on-failure -j4
```

逐项 check/build 命令、源码及清单 SHA-256、program.py/langrt.py SHA-256、解释器版本和
实际退出码/输出保存在两种构建的 `tests/python/generics/generic-results.json`；
生成程序在相邻 `generics/generated/<项目>/`，新增源程序在 `generics/sources/<项目>/`。
既有 Python 逐项执行与拒绝记录在两种构建的 `tests/python/results/results.json`。
公共七项的两种构建记录另汇总至临时 `tests/python/generic-results.json`，含本机路径，不应提交。

接口变化：无。Clang ASan/UBSan 留给主代理统一运行，本次记为 not_run。
Native 执行与比较由另一 agent 负责，本次未测；聚合执行、ARC、第三方库和严格浮点执行
不在本次范围。没有 commit、push、stash 或派子代理。

## T06 初版记录

任务：T06；operation_id：`language-t06-python-scalars-20261103`；契约：`compiler-dev/1`。
本次在 T04 提交 `b8ccf384a16e85141994995fe1869fd5fc084100` 的独立工作目录实现并运行验证。
公共 HIR、`lang_sema` 与既有 `runtime/python/langrt.py` 均已实际读取；T04 的依赖结果另见
`/home/qin/Work/language-dev-20261103/checks/t04-verification.json`。

## 文件与 API

| 文件 | 内容 |
| --- | --- |
| `include/lang/backend/python.hpp` | `emit`、选项、生成文本、符号表和逐行 SourceSpan 表 |
| `src/backend/python/emitter.cpp` | 公共 HIR 到私有结构化 Python 语句树，再打印 Python |
| `src/backend/python/CMakeLists.txt`、`runtime.hpp.in` | `lang_backend_python`，编译时嵌入运行库 |
| `runtime/python/langrt.py` | 定宽整数、检查转换、IEEE 位模式、Cell 和 Panic runner |
| `tests/python/emit_project.cpp` | 真实项目前端与 emitter helper，不执行 Python |
| `tests/python/api_test.cpp` | 真实 Verified 输入、确定性、配置、入口、取消等 C++ 检查 |
| `tests/python/run_cases.py` | 显式执行生成文件、检查预期结果、保存逐项 JSON |
| `tests/python/CMakeLists.txt` | 独立构建与集成用测试片段 |

```cpp
lang::PhaseResult<lang::backend::python::Output>
lang::backend::python::emit(
    const lang::hir::VerifiedHirModule& module,
    lang::SymbolId entry,
    const lang::backend::python::Options& options = {},
    std::stop_token stop = {});
```

输入只能由公共 `hir::verify` 发布，不能传入未验证的 `HirModule`。配置必须为 Python/64，
入口为无参数、返回 Unit 或 i32 的函数。`Options.entrypoint=false` 省略顶层 runner，
方便显式导入测试；入口签名仍检查。不存在的入口 SymbolId 返回 internal / `E_IR_INVALID`。
取消返回 cancelled，无部分输出。

`Output.code` 写为 `program.py`，`Output.runtime` 写为同目录的 `langrt.py`。
运行库版本为 `compiler-dev/1-python-scalars/1`，只使用 Python 标准库 struct/sys。
`Output.symbols` 给出 SymbolId 到生成名字的对应；`Output.source_map` 给出生成文件
一基行号到原 SourceSpan 的对应。辅助 import、顶层 runner 和自动生成的空块 pass 不伪造源码位置。
这些 ID 仍属于本次编译，不能当成跨构建缓存身份。

emitter 不读 `.lang`，不按源名字查找变量，不调用解释器、eval、exec 或外部包。
函数为 `_f<SymbolId>`，参数和绑定为 `_s<SymbolId>`，临时量为
`_t<函数SymbolId>_<序号>`。重复 `_` 参数使用各自独立的 SymbolId，得到不同合法形参名。
helper 仅在选取入口时根据已解析符号表寻找入口模块的 main；代码生成不重新解析名字。

## 支持的行为

- Unit、bool、i8/i16/i32/i64、u8/u16/u32/u64、isize/usize。目标字宽固定 64，不读取宿主指针宽度。
- f32/f64 按 HIR 的 IEEE bits 重建；支持绑定、读取、赋值、普通传参/返回和整体 inout。
  传递过程中不做算术。测试逐位检查零、1、ties-to-even、最小 subnormal、下溢、上溢及 inout 更新。
- 声明函数、函数值、无捕获 lambda、立即/间接调用、返回函数值、递归和相互递归、跨文件/模块调用。
- let/var/const、赋值和整数复合赋值；局部 const 的计算由前端完成。
- Block、If、三段式/条件式/无限 For、Return、Break、Continue、bool 短路。
- 数值/bool 的整体 inout，包括再次转交 inout。所有可写绑定使用 Cell，inout 参数接收原 Cell；
  普通参数接收读取出的值，不产生意外写回。Panic 前的写入立即可见，没有“正常返回后再复制”的步骤。

callee 先求值并保存一次；`Call.args` 按已发布的源码顺序逐个求值到临时量，再按
`parameter_index` 放置。Binary 操作数也分别先保存，避免后一个调用修改前一个读值。
赋值先保存 Place，复合赋值先保存旧值，再求 RHS。变量和循环跳转都用公共 ID；
嵌套块的同名绑定不会变成同一个 Python 局部变量。

For 输出为 while，条件每轮重新求值。正常完成循环体和 Continue 都执行 step；
Break 和 Return 不执行 step。Continue/Break 根据 HIR 的循环节点 ID 检查目标，
嵌套循环分别处理，不根据缩进或源名字猜测目标。

整数运算按操作数的位宽、有符号性回绕。除法用整数绝对值求商再确定符号，不经过 float；
余数由同一向零截断的商计算。MIN/-1 回绕为 MIN，MIN%-1 为 0。
移位 RHS 可以是另一种整数类型，先检查其数学值是否在 `[0,LHS位宽)`；
结果始终保持 LHS 位宽。unsigned 右移为逻辑右移，signed 为算术右移。
IntegerConvert 检查目标范围，动态越界产生 Panic。

调试时发现 bool 的逻辑非不能请求 `bit_width`；现在 bool not/equality 传入无整数位宽含义的
`0,False`，运行库在整数处理前返回 bool 结果。共享短路程序、带副作用短路和 bool inout
均实际执行验证了这个处理。

## 拒绝的输入

T06 初版拒绝严格浮点算术/比较/转换、捕获闭包、string、Vec、聚合、泛型、typed throws、defer、ARC、move、
复杂 Place/借用及 foreign 均不在本标量版本内；已有前端或 emitter 报
`E_CAPABILITY_UNAVAILABLE`，不会转成 Any 或切换后端。
本次 T07 已接入的具体标量泛型与未使用声明见文首，不能继续按初版分类为未支持。
普通参数重新赋值仍等待用户选择，checker 对它报告同一未支持诊断。
Python/32 虽可通过前端验证，本 emitter 仍在 codegen 阶段明确拒绝。
没有新增共享类型、SourceSpan、PhaseResult 或诊断 code。

Panic 继承 BaseException，不属于普通 Exception。顶层仅捕获 Panic，输出
`LANG_PANIC: <原因>` 到 stderr 并返回 101；普通 main 返回值原样交给 SystemExit，
其它 Python 异常继续给出 traceback。这是本运行库的终止方式，没有修改共享用例对
语言 Panic 的要求。此阶段拒绝 defer，没有执行语言 defer，也不声称检查了 ARC 或泄漏。

## 构建与接入

由 integrator 在总 CMake 已定义 `lang_hir` 后加入：

```cmake
add_subdirectory(src/backend/python)
# 正式 driver 按需要链接 lang_backend_python，组合 load/resolve/check/emit。
if(BUILD_TESTING)
  add_subdirectory(tests/python)
endif()
```

生产库只链接 `lang_hir`，不需要 Python 开发包、json-c 或 LLVM。
tests/helper 通过 `lang_sema` 使用 GMPXX、toml++ 和 json-c；测试期间不下载任何依赖。
总 CMake、正式 driver、公共 HIR/sema/modules/contracts 和共享用例均未修改。

本次独立验证命令，在本工作目录执行：

```sh
py=/home/qin/Work/language-dev-20261103/toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12
"$py" --version
cmake -S tests/python -B build/t06-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLANG_ENABLE_LLVM=OFF -DLANG_PYTHON312="$py"
cmake --build build/t06-debug -j4
ctest --test-dir build/t06-debug --output-on-failure -j4

cmake -S tests/python -B build/t06-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLANG_ENABLE_LLVM=OFF -DLANG_PYTHON312="$py"
cmake --build build/t06-release -j4
ctest --test-dir build/t06-release --output-on-failure -j4

cmake -S tests/python -B build/t06-sanitized -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DLANG_ENABLE_LLVM=OFF \
  -DCMAKE_CXX_COMPILER=/home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/clang++ \
  -DCMAKE_CXX_FLAGS='-O1 -g -DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' -DLANG_PYTHON312="$py"
cmake --build build/t06-sanitized -j4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/t06-sanitized --output-on-failure -j4

g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
  -Iinclude -Ibuild/t06-debug/backend-python -fsyntax-only \
  src/backend/python/emitter.cpp tests/python/emit_project.cpp tests/python/api_test.cpp
```

单项目生成与执行是两个独立命令：

```sh
build/t06-debug/lang_python_emit --project tests/fixtures/core/lang.toml \
  --out-dir build/t06-core
"$py" -s build/t06-core/program.py
```

helper 真实调用 `load_project → resolve_project → check_project → hir::verify → emit`。
它在 resolve/check 前选择 Python 后端，保留清单字宽，没有修改已验证配置。
只在所有阶段成功后写入 program.py、langrt.py、verified.hir、metadata.json。
失败时输出共享诊断 JSON，源码/未支持返回 1、输入/参数返回 2、内部错误返回 3。

## 本次实际结果

Linux x86_64；GNU 16.2.1、Clang 22.1.8、GMPXX 6.3.0；运行解释器为上述绝对路径的
Python **3.12.15**。

| 构建/检查 | 实际结果 |
| --- | --- |
| GNU Debug CTest | passed，16/16（既有 14 项 + Python API/项目执行 2 项） |
| GNU Release/NDEBUG CTest | passed，16/16，C++ require 保留 |
| Clang22 ASan/UBSan + NDEBUG CTest | passed，16/16；detect_leaks=0，不计泄漏检查 |
| 三个新增 C++ 文件的 Wall/Wextra/Wpedantic/Werror | passed，GNU O2/NDEBUG |
| 必须执行的项目 | passed，17/17，每个真实生成并执行 |

以下逐项结果在三个构建中都相同。正常程序 stdout/stderr 均为空；所有 Panic 程序 stdout
为空，stderr 恰好一行标识与对应原因。

| 项目 | 执行结果 | 状态 |
| --- | --- | --- |
| i8-u8-wrapping | return 0 | passed |
| signed-division-remainder | return 0 | passed |
| division-min-over-minus-one | return 0 | passed |
| bitwise-integers | return 0 | passed |
| shift-left-right | return 0 | passed |
| dynamic-integer-conversions | return 0 | passed |
| short-circuit-panic-guard | return 0 | passed |
| function-values | return 0 | passed |
| control-flow-break-continue | return 0 | passed |
| lexical-shadowing | return 0 | passed |
| inout-argument-order | return 0，命名实参保留源码副作用顺序 | passed |
| panic-division-by-zero | Panic，integer division by zero，101 | passed |
| panic-invalid-shift | Panic，invalid shift count，101 | passed |
| panic-dynamic-conversion | Panic，integer conversion overflow，101 | passed |
| panic-negative-shift | Panic，invalid shift count，101 | passed |
| core | return 0 | passed |
| numeric | return 0 | passed |

runner 同时检查 101、Panic 标识和原因，不能以任意非零返回判为通过。
四个共享 Panic 源码的危险操作后仍为原有 return 0，没有修改源码或预期。

| 对照 | 实际观察 | 状态 |
| --- | --- | --- |
| 普通 main return 101 | 101、无输出，识别为 ordinary_nonzero | passed |
| 普通 main 算术后 return 7 | 7、无输出，识别为 ordinary_nonzero | passed |
| 在真实生成函数使用的 runtime 运算处加入受控 Python 异常 | RuntimeError traceback，1，无 Panic 标识 | passed |
| Python 语法错误 | SyntaxError，1，无 Panic 标识 | passed |
| Panic 继承关系 | 属于 BaseException，不属于 Exception | passed |

另有 15 项补充运行检查通过：callee 的次数与顺序、lambda/递归/丢弃参数、inout 与值复制、
短路副作用、嵌套循环 step/提前 return、Unit main/函数变量赋值、余数除零、u64 移位量、
负数转 unsigned、Python 关键字改名、全部整数位宽与大整数除法、浮点传递、跨模块调用、
Panic 前的 inout 写入、14 个浮点位模式观察。
12 项未支持诊断检查按 `unsupported` 记录，不计为功能运行通过。

逐项命令、输入 SHA-256、生成代码/运行库/HIR/metadata 的 SHA-256、Python 版本和
实际返回码/输出保存于：

- `build/t06-debug/results/results.json`
- `build/t06-release/results/results.json`
- `build/t06-sanitized/results/results.json`

每项生成文件保存在相应 `results/generated/<项目>/`；补充源程序在
`results/sources/<项目>/`。CTest 测试日志在各构建目录的 `Testing/Temporary/LastTest.log`。
共享 cases.json、15 组源码/清单、core/numeric 源码/清单的前后 SHA-256 相同。

接口变化：新增 Python 后端 API，无共享接口修改、新诊断 code 或正式 CLI 接入。
依赖交接：integrator 可直接使用上述头文件、库目标和 CMake 片段；两份生成文本写入同目录后即可显式运行。
本次 T06 所要求的实现与独立验证均已完成。正式 build CLI 由 root 接入，任务清单未改；
native 差分、T08 错误清理、T09 ARC/泄漏、T10 foreign 集成未执行，不作为本次完成声明。
按本次指令没有 commit、push、stash、派单或服务操作。

## 主目录集成结果

主代理已加入正式 `langc build --project MANIFEST --backend python --out-dir DIR [--format json]`。
该命令先检查再生成 program.py/langrt.py，不运行解释器。主目录 Debug、Release、Clang22
ASan/UBSan 均17/17通过（包括新增 python.cli），实际17工程、15补充、5分类对照通过。
独立只读审查310f6335未发现可确认错误。主目录新输出不依赖上述旧独立构建目录。

构建时缺少 Python3.12 不影响 C++生成器；仅两项执行测试记为not_run。执行测试须设置
LANG_PYTHON312且由runner核实真实版本。正式CLI源码错误或未支持不会覆盖已有输出；
文件写入先在私有临时目录完成，program.py最后替换，写入失败返回2并清除临时目录。

```sh
cmake --preset frontend-debug -DLANG_PYTHON312=/path/to/python3.12
cmake --build --preset frontend-debug
ctest --preset frontend-debug
build/frontend-debug/langc build --project tests/fixtures/core/lang.toml --backend python --out-dir build/core/python
/path/to/python3.12 -s build/core/python/program.py
```
