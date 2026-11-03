# 前端构建与检查

契约版本 compiler-dev/1。需要 C++20 编译器、CMake >= 3.20、Ninja、toml++ 与 json-c 的开发文件。本机使用已安装的 toml++ 3.4.0、json-c 0.19。前端不链接或运行 LLVM/Python，包含源码快照、诊断、输入/取消、词法语法，以及项目、模块、名称和作用域解析。开发测试若找到 Python 会执行 80 个语法分类和 20 个独立模块项目；可用 LANG_TEST_PYTHON 指定测试解释器，缺少时这些测试明确记为未运行。C++ 单元测试仍可运行。类型检查和代码生成尚未实现。

```sh
cmake --preset frontend-debug
cmake --build --preset frontend-debug
ctest --preset frontend-debug
build/frontend-debug/langc --help
build/frontend-debug/langc --version
```

帮助和版本返回 0；无参数、未知命令、多余帮助/版本参数返回 2。check --syntax-only [--format json] FILE 检查一个文件，语法成功为 0、源码错误为 1、输入文件或参数错误为 2、内部错误为 3。全量语义 check、build、lsp 和 capabilities 返回 1 及 E_CAPABILITY_UNAVAILABLE。

```sh
build/frontend-debug/langc check --syntax-only examples/core.lang
build/frontend-debug/langc check --syntax-only --format json examples/core.lang
python3 tests/syntax/run_cases.py build/frontend-debug/langc
```

CTest 已包含 80 个分类的真实 C++ CLI 执行及 JSON schema 检查，另有 lexer、AST、恢复、深度、取消、源码和输入错误测试。文件用例先读取实际文件并与固定清单逐字节比较，防止只检查旧副本。

T03 的 `lang_modules` 库提供 `load_project` 与 `resolve_project`。当前通过测试程序运行这两阶段，CLI 全量 check 仍等待 T04 类型检查。`lang_module_check` 只在 BUILD_TESTING 下构建，成功表示项目和名称检查通过：

```sh
python3 tests/modules/run_cases.py build/frontend-debug/lang_module_check
python3 tools/validate_module_fixtures.py --langc build/frontend-debug/langc
```

第一条真正执行 C++ 项目和名称解析，并检查错误种类与原始文件区间。第二条只检查资料引用及语法，不代替名称检查。实例成员和 trait 选择留给 T04；局部新绑定在初始化完成后才可见，初始化表达式可以引用同名外层绑定。

可选接入探针：

```sh
cmake -S . -B build/llvm -G Ninja -DLANG_ENABLE_LLVM=ON -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build/llvm --target lang_llvm_probe
build/llvm/lang_llvm_probe
```

仅接受 LLVM_PACKAGE_VERSION 精确等于 22.1.8。探针链接 LLVM Support，打印版本；没有代码生成。此选项不验证完整 Clang/LLD、SDK 或 ABI 配置，不能代替主任务的工具链 lock。不要提交本机 LLVM_DIR 或构建缓存。

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

已分别运行 LLVM 22.1.8 接入探针，以及 Clang 22.1.8 + LLD 22.1.8 的 C++20 编译、链接、运行检查。LLVM 23.1.3 配置会被版本检查拒绝。这些结果不表示本语言程序已经可以编译。

独立验收工程为 tests/fixtures/core 和 tests/fixtures/numeric，每个只有 src/main.lang 的一个 main，源码分别复制 examples/core.lang、examples/numeric.lang，未改历史样例。清单遵循现有项目格式。未来分别执行：

```sh
langc build --project tests/fixtures/core/lang.toml --backend native --out-dir build/core/native
./build/core/native/core
langc build --project tests/fixtures/core/lang.toml --backend python --out-dir build/core/python
python3 build/core/python/core.py
```

numeric 使用同样命令，将 core 替换为 numeric。上述输出命名是后续实现应确认的约定，当前驱动不会生成文件。两项目预期无标准输出、程序退出 0。core 验证循环和函数调用；numeric 验证整数回绕、最小整数除 -1、余数和负数截断。native O0/O2 及 Python 执行均未运行，需 T05/T06 后端实现后验证。

各阶段记录在外层 checks/。T00 的 C++20、LLVM 22.1.8 探针和 Clang/LLD 运行检查已通过；T02 的 Debug/Release 语法分类为 80/80。后续阶段记录自己的检查结果，不用工具链探针代替语言程序运行。
