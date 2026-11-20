# 编译器开发资料

这里保存 Woozy 编译器的构建说明、实现记录、测试说明和 agent 工作要求。语言教程、快速入门与语言规则见 [语言文档](../docs/)。本目录中的阶段记录保留当时的状态；当前项目介绍见 [仓库首页](../README.md)。

| 要做的事 | 入口 |
| --- | --- |
| 安装可整体移动的自举版 | [安装工具链](docs/BUILD.md#安装自举版工具链) |
| 构建编译器、运行示例 | [构建与运行](docs/BUILD.md)、[自举检查](docs/SELFHOST_BOOTSTRAP.md) |
| 配置编辑器、格式化和检查源码 | [编辑器配置](docs/EDITOR.md) |
| 了解源码结构与开发约定 | [开发说明](DEVELOPMENT.md)、[前端](docs/FRONTEND.md)、[IR](docs/IR_CONTRACTS.md)、[后端](docs/BACKENDS.md) |
| 查询实现范围 | [泛型与方法](docs/GENERICS.md)、[动态 trait 对象](docs/TRAIT_OBJECTS.md)、[值与清理](docs/T09_EXECUTION.md)、[互操作](docs/INTEROP.md) |
| 了解原生线程实现 | [线程、锁与原子计数](docs/CONCURRENCY_NATIVE.md) |
| 查看检查和性能记录 | [检查记录](VALIDATION.md)、[验收要求](docs/ACCEPTANCE.md)、[性能记录](docs/PERFORMANCE.md) |
| 查测试说明 | [语法测试](tests/syntax/README.md)、[模块测试](tests/modules/README.md)、[类型检查](tests/sema/README.md)、[跨后端测试](tests/backend_common/README.md)、[性能测试](tests/performance/README.md) |
| 交给 agent 开发 | [工作要求](AGENTS.md)、[任务提示模板](prompts/IMPLEMENT_TASK.md)、[任务清单](../contracts/tasks.json) |

测试说明在 `development/tests/` 保留原测试目录结构，测试源码与执行脚本仍在仓库根的 `tests/`。语言规范原包位于 [baseline/](../baseline/)，供语言设计与历史样例对照。

## 构建

C++ 版和自举版都用仓库旁的 `../toolchains/llvm-22.1.8`，LLVM/Clang/LLD 版本必须正好是 22.1.8，系统自带的 23.x 会被版本检查拒绝。C++ 版另外需要 CMake >= 3.20、Ninja、pkg-config、toml++、json-c 和 GMPXX >= 6.3.0。工具链的来源和摘要在 `contracts/toolchain.lock.json`。

### C++ 版

在仓库根目录执行：

```sh
tc="$(cd ../toolchains && pwd)"
cmake -S . -B build/woozyc-cpp -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLANG_ENABLE_LLVM=ON \
  -DLLVM_DIR="$tc/llvm-22.1.8/lib/cmake/llvm" \
  -Dzstd_STATIC_LIBRARY="$tc/zstd-1.5.7/lib/libzstd.a" \
  -DCMAKE_CXX_COMPILER="$tc/llvm-22.1.8/bin/clang++"
cmake --build build/woozyc-cpp --target woozyc -j"$(nproc)"
```

产物是 `build/woozyc-cpp/woozyc` 和 `build/woozyc-cpp/native/lang_native_helper`，构建目录里还有指向源码 `stdlib` 和 LLVM 22.1.8 的 `toolchain/` 链接。省略 `--target` 会连测试一起构建。

运行 `woozyc` 编译程序前，要把 LLVM 22.1.8 工具的兼容库目录加进 `LD_LIBRARY_PATH`。它的 `ld.lld` 依赖 `libxml2.so.2`，系统现在的 libxml2 2.15 没有这个 soname，不设会报 `E_TOOLCHAIN_MISSING`：

```sh
export LD_LIBRARY_PATH="$PWD/../toolchains/libxml2-legacy-2.13.9/usr/lib"
build/woozyc-cpp/woozyc build --project tests/fixtures/core/lang.toml \
  --backend native --opt 2 --out-dir build/core/native
./build/core/native/program
```

只改前端、不需要 LLVM 时用 preset，这个配置不链接 LLVM，适合快速改语法和类型检查；此时 `build --backend native` 会明确拒绝：

```sh
cmake --preset frontend-debug
cmake --build --preset frontend-debug
ctest --preset frontend-debug
```

### 自举版

`selfhost/src/` 是同一编译器的 Woozy 源码。第一代用上面的 C++ 版构建，第二、三代只使用上一代 Woozy 编译器，最后要求 stage2 和 stage3 的二进制逐字节相同。用 Make 按源码包构建：

```sh
make -C selfhost bootstrap \
  OUT="$PWD/build/selfhost-bootstrap" \
  STAGE0="$PWD/build/woozyc-cpp/woozyc" \
  RUNTIME_DIR="$PWD/build/woozyc-cpp/native" \
  JOBS=6
```

`STAGE0` 和 `RUNTIME_DIR` 默认指向 `build/woozy-stdlib-native/`，换构建目录时两个都要改。`OUT` 必须为空。默认构建三代、`OPT=0`；`STAGES=2` 只到第二代，`OPT=2` 改用 O2。第二、三代需要可执行的 `strace`，默认从 `PATH` 查找，也可用 `STRACE=` 指定，本机在 `../toolchains/strace-7.2/usr/bin/strace`。脚本会冻结源码、标准库、运行库和工具输入并逐代核对哈希，结果写在 `OUT/result.json`；那里的 `status=ok` 只表示分代构建完成，不代表语料对照通过。

自举使用的 `--source-make` 和 `--package-objects` 入口见 [增量编译](docs/INCREMENTAL_COMPILATION.md)。自举模块的移植范围和当前状态见 [自举实现与检查](docs/SELF_HOSTING.md)。
