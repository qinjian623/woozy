# T00 工程构建

契约版本 compiler-dev/1。需要 C++20 编译器、CMake >= 3.20、Ninja。默认不查找 LLVM 或 Python，不执行 Python。lang_frontend 是仅含版本信息的真实静态库，后续前端模块可加入此目标；尚无 lexer、parser、类型检查或代码生成，T01 未完成。

```sh
cmake --preset frontend-debug
cmake --build --preset frontend-debug --target lang_frontend langc
ctest --preset frontend-debug
build/frontend-debug/langc --help
build/frontend-debug/langc --version
```

帮助和版本返回 0；无参数、未知命令、多余帮助/版本参数返回 2；check/build/lsp/capabilities 返回 1 并输出 E_CAPABILITY_UNAVAILABLE。尚未解析这些命令的子参数，不能用其结果判断输入文件是否合法。输入文件错误的退出码 2 在后续实现命令时单独测试。

可选接入探针：

```sh
cmake -S . -B build/llvm -G Ninja -DLANG_ENABLE_LLVM=ON -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build/llvm --target lang_llvm_probe
build/llvm/lang_llvm_probe
```

仅接受 LLVM_PACKAGE_VERSION 精确等于 22.1.8。探针链接 LLVM Support，打印版本；没有代码生成。此选项不验证完整 Clang/LLD、SDK 或 ABI 配置，不能代替主任务的工具链 lock。不要提交本机 LLVM_DIR 或构建缓存。

独立验收工程为 tests/fixtures/core 和 tests/fixtures/numeric，每个只有 src/main.lang 的一个 main，源码分别复制 examples/core.lang、examples/numeric.lang，未改历史样例。清单遵循现有项目格式。未来分别执行：

```sh
langc build --project tests/fixtures/core/lang.toml --backend native --out-dir build/core/native
./build/core/native/core
langc build --project tests/fixtures/core/lang.toml --backend python --out-dir build/core/python
python3 build/core/python/core.py
```

numeric 使用同样命令，将 core 替换为 numeric。上述输出命名是后续实现应确认的约定，当前驱动不会生成文件。两项目预期无标准输出、程序退出 0。core 验证循环和函数调用；numeric 验证整数回绕、最小整数除 -1、余数和负数截断。native O0/O2 及 Python 执行均未运行，需 T05/T06 后端实现后验证。

本次 Debug 配置、编译及 bootstrap CLI 测试通过；日志位于 ignored reports/t00-{configure,build,test}.log。LLVM 22.1.8 探针未运行，工具链由主任务另行安装。测试检查帮助/版本文本、退出码及失败时 stdout 为空，不声称语言检查通过。
