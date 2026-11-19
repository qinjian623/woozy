# 前端构建与检查

契约版本 compiler-dev/1。需要 C++20 编译器、CMake >= 3.20、Ninja、pkg-config、toml++、json-c 和 GMPXX >= 6.3.0 的开发文件。本机使用 toml++ 3.4.0、json-c 0.19、GMP/GMPXX 6.3.0。前端库与检查命令不链接 LLVM 或 CPython，检查不启动后端进程，包含源码快照、诊断、输入/取消、词法语法、项目和名称解析，以及标量类型检查与 HIR 验证。开发测试若找到 Python 会执行 80 个语法分类和 20 个独立模块项目；可用 LANG_TEST_PYTHON 指定测试解释器，缺少时这些测试明确记为未运行。C++ 单元测试仍可运行。Python 标量代码生成和显式运行测试已实现；原生标量后端已接入正式 CLI，启用方式见下文。

```sh
cmake --preset frontend-debug
cmake --build --preset frontend-debug
ctest --preset frontend-debug
build/frontend-debug/woozyc --help
build/frontend-debug/woozyc --version
```

帮助和版本返回 0；无参数、未知命令、多余帮助/版本参数返回 2。check --syntax-only [--format json] FILE 检查一个文件；check --project MANIFEST [--backend native|python] [--format json] 读取项目并检查已支持的标量类型。检查成功为 0、源码错误或未支持操作为 1、输入文件/配置/参数错误为 2、内部错误为 3。build 已支持 Python/64 和 Linux x86_64 原生标量程序；未开启 LANG_ENABLE_LLVM 时原生 build 明确拒绝。lsp 和 capabilities 仍返回 E_CAPABILITY_UNAVAILABLE。

```sh
build/frontend-debug/woozyc check --syntax-only examples/core.wz
build/frontend-debug/woozyc check --syntax-only --format json examples/core.wz
python3 tests/syntax/run_cases.py build/frontend-debug/woozyc
```

CTest 已包含 80 个分类的真实 C++ CLI 执行及 JSON schema 检查，另有 lexer、AST、恢复、深度、取消、源码和输入错误测试。文件用例先读取实际文件并与固定清单逐字节比较，防止只检查旧副本。

`lang_modules` 库提供 `load_project` 与 `resolve_project`，`lang::sema::check_project` 随后检查类型并调用唯一 HIR 验证器。CLI 的 `--backend` 覆盖清单中的 backend，字宽仍来自清单；检查不运行源程序。`lang_module_check` 只在 BUILD_TESTING 下构建，成功表示项目和名称检查通过：

```sh
python3 tests/modules/run_cases.py build/frontend-debug/lang_module_check
python3 tools/validate_module_fixtures.py --woozyc build/frontend-debug/woozyc
```

第一条真正执行 C++ 项目和名称解析，并检查错误种类与原始文件区间。第二条只检查资料引用及语法，不代替名称检查。实例成员和 trait 选择留给 T07；局部新绑定在初始化完成后才可见，初始化表达式可以引用同名外层绑定。

可选 LLVM 接入：

```sh
cmake -S . -B build/llvm -G Ninja -DLANG_ENABLE_LLVM=ON -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build/llvm --target lang_llvm_probe
build/llvm/lang_llvm_probe
```

仅接受 LLVM_PACKAGE_VERSION 精确等于 22.1.8。lang_llvm_probe 只打印版本；同一选项还构建独立的 native helper、对象发射器和运行库。正式 native build 通过 argv 启动 helper，使用指定的 Clang/LLD 链接，不执行生成程序。探针本身不能代替代码生成和运行检查。不要提交本机 LLVM_DIR 或构建缓存。

系统已安装 LLVM 22.1.8 时，可以用 `-DLLVM_DIR="$(llvm-config --cmakedir)"` 配置。工具和 LLVM 库分别安装时，可加 `-DWOOZY_LLVM_TOOLS_DIR` 指向含 `opt`、`llc`、`clang` 和 `ld.lld` 的目录。C++ 版和 Woozy 自写版都可设置 `WOOZY_LLVM_BIN`，相对值从编译器所在目录解析，默认使用同目录下的 `toolchain/`。搬动编译器时，同时复制标准库、运行库和 C++ 版的 Native helper。目标机器的 LLVM 工具可通过 `WOOZY_LLVM_BIN` 指定。

已验证的独立工具链版本、来源和摘要见 contracts/toolchain.lock.json。本机的官方 Linux 包另外需要 libxml2.so.2 和静态 zstd：兼容库取自验证过发行签名的 Arch libxml2-legacy，zstd 1.5.7 从官方源码构建并核对发布的 SHA-256。系统库没有被替换。

本机复查 LLVM 接入时，使用外层 toolchains/ 中的目录：

```sh
tc="$(cd ../toolchains && pwd)"
export LD_LIBRARY_PATH="$tc/libxml2-legacy-2.13.9/usr/lib"
cmake -S . -B build/llvm-clang22 -G Ninja \
  -DLANG_ENABLE_LLVM=ON \
  -DLLVM_DIR="$tc/llvm-22.1.8/lib/cmake/llvm" \
  -Dzstd_STATIC_LIBRARY="$tc/zstd-1.5.7/lib/libzstd.a" \
  -DCMAKE_CXX_COMPILER="$tc/llvm-22.1.8/bin/clang++"
cmake --build build/llvm-clang22
ctest --test-dir build/llvm-clang22 --output-on-failure
build/llvm-clang22/lang_llvm_probe
```

已分别运行 LLVM 22.1.8 接入探针，以及 Clang 22.1.8 + LLD 22.1.8 的 C++20 编译、链接、运行检查。LLVM 23.1.3 配置会被版本检查拒绝。探针结果只验证工具链；下文的实际源语言程序另做生成与运行检查。

独立验收工程为 tests/fixtures/core 和 tests/fixtures/numeric，每个只有 src/main.wz 的一个 main，源码分别复制 examples/core.wz、examples/numeric.wz，未改历史样例。清单遵循现有项目格式。使用开启 LLVM 的 woozyc，分别执行：

```sh
woozyc build --project tests/fixtures/core/lang.toml --backend native --out-dir build/core/native
./build/core/native/program
woozyc build --project tests/fixtures/core/lang.toml --backend python --out-dir build/core/python
/path/to/python3.12 -s build/core/python/program.py
```

numeric 使用同样命令，将 core 替换为 numeric。native 输出 program 和 program.o；Python 输出 program.py 和 langrt.py。两项目预期无标准输出、程序退出 0。core 验证循环和函数调用；numeric 验证整数回绕、最小整数除 -1、余数和负数截断。这两个项目已经实际通过 native O0/O2 以及 Python 3.12.15 的执行检查。

各阶段记录在外层 checks/。T00 的 C++20、LLVM 22.1.8 探针和 Clang/LLD 运行检查已通过；T02 的 Debug/Release 语法分类为 80/80。后续阶段记录自己的检查结果，不用工具链探针代替语言程序运行。


T04 的类型表与标量 HIR 验证组件位于 lang_hir，包含 74 项损坏检查。lang_sema 从真实
项目/名称结果生成 HIR，支持基本整数、bool、Unit、函数值、无捕获匿名函数、控制流、
调用和数值/bool 整体 inout；sema.scalar 执行 180 个静态项目检查，sema.cli 检查真实 CLI。
范围、未支持操作与实际验证见 [CHECKER.md](../tests/sema/CHECKER.md)。两个标量后端已生成并运行程序；
完整严格浮点算术仍未实现；普通参数重新赋值规则等待用户选择。
T07 的泛型、聚合类型和 trait 静态接口及当前限制见 [GENERICS.md](GENERICS.md)；
静态 HIR 通过不等于新增后端执行已经通过。

## Python 标量程序

```sh
woozyc build --project tests/fixtures/core/lang.toml --backend python --out-dir build/core/python
/path/to/python3.12 -s build/core/python/program.py
```

build 检查源程序并写入 program.py 和 langrt.py，不执行 Python。执行是独立步骤。
生成器与前端不链接 CPython 或 LLVM。Python 首执行 profile 为64位；32位项目在生成阶段
明确拒绝。安装的解释器只供运行测试使用，可用 -DLANG_PYTHON312=/path/to/python3.12
指定；未找到时 python.projects/python.cli 明确记为 not_run，C++ API 测试仍可运行。

正式集成后，GNU Debug/Release、Clang22 ASan/UBSan 各21/21通过；不启用 LLVM 的前端工程18/18通过。17个必须执行项目、15项补充、
5项Panic分类对照均实际通过。检查清单与限制见 [PYTHON_SCALARS.md](PYTHON_SCALARS.md)。


## 原生标量程序

```sh
build/native-cli/woozyc build --project tests/fixtures/core/lang.toml \
  --backend native --opt 2 --out-dir build/core/native --format json
build/core/native/program
```

仅接受 Linux x86_64、64位项目和 O0/O2。woozyc 不链接 LLVM 或 CPython；生成工作由独立
helper 执行。源码/未支持错误返回1，工具缺失和写文件错误返回2，链接失败报告
E_LINK_FAILED。发布前检查目的文件类型；第二个文件替换报错时恢复第一个旧文件，
没有旧文件则移除新文件。恢复失败会保留备份并在诊断中给出位置。构建命令不执行新程序。

正式 CLI 的17个项目分别在O0/O2执行，共34次，另有普通返回7、源码错误保留输出、
无main及写入失败清理测试。独立对象生成测试另有62次ELF执行。
ASan/UBSan覆盖本项目编译器代码；生成程序使用的独立运行库不继承编译器的检查选项。
预编译LLVM和生成程序不计入该检查覆盖范围。

简单结构体、枚举、静态字符串、泛型、ARC和外部绑定仍需后续工作；
T05整项在简单结构体与常量字符串补齐前保留in_progress。细节见[NATIVE_SCALARS.md](NATIVE_SCALARS.md)。
