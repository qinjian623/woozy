# 前端构建与检查

契约版本 compiler-dev/1。构建前端需要 C++20 编译器、CMake >= 3.20、Ninja、pkg-config、toml++、json-c 和 GMPXX >= 6.3.0 的开发文件；本机环境为 toml++ 3.4.0、json-c 0.19、GMP/GMPXX 6.3.0。前端库和检查命令不链接 LLVM 或 CPython，检查时也不启动后端进程，覆盖源码快照、诊断、输入/取消、词法语法、项目和名称解析，以及标量类型检查与 HIR 验证。

开发测试如果找到 Python，会运行 80 个语法分类和 20 个独立模块项目；测试解释器可用 LANG_TEST_PYTHON 指定。找不到 Python 时，这些测试会明确记为未运行，C++ 单元测试仍照常执行。Python 标量代码生成与显式运行测试已实现；原生标量后端已接入正式 CLI，启用方式见下文。

```sh
cmake --preset frontend-debug
cmake --build --preset frontend-debug
ctest --preset frontend-debug
build/frontend-debug/woozyc --help
build/frontend-debug/woozyc --version
```

帮助和版本选项返回 0；无参数、未知命令或多余的帮助/版本参数返回 2。check --syntax-only [--format json] FILE 检查单个文件；check --project MANIFEST [--backend native|python] [--format json] 读取项目并检查已支持的标量类型。退出码约定：检查成功返回 0，源码错误或未支持操作返回 1，输入文件/配置/参数错误返回 2，内部错误返回 3。build 已支持 Python/64 和 Linux x86_64 原生标量程序；未开启 LANG_ENABLE_LLVM 时，原生 build 会明确拒绝。C++ 版的 `woozyc lsp` 和 `woozyc fmt` 也可用于不带 LLVM 的构建，见 [编辑器配置](EDITOR.md)。`capabilities` 仍返回 `E_CAPABILITY_UNAVAILABLE`。

```sh
build/frontend-debug/woozyc check --syntax-only examples/core.wz
build/frontend-debug/woozyc check --syntax-only --format json examples/core.wz
python3 tests/syntax/run_cases.py build/frontend-debug/woozyc
```

CTest 已用真实 C++ CLI 执行这 80 个语法分类并校验 JSON schema，还包含 lexer、AST、错误恢复、嵌套深度、取消、源码和输入错误测试。文件用例会先读取磁盘上的实际文件，再与固定清单逐字节比对，避免误测旧副本。

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

系统已安装 LLVM 22.1.8 时，可以用 `-DLLVM_DIR="$(llvm-config --cmakedir)"` 配置。工具和 LLVM 库分别安装时，可加 `-DWOOZY_LLVM_TOOLS_DIR` 指向含 `opt`、`llc`、`clang` 和 `ld.lld` 的目录。C++ 版和 Woozy 自写版都支持通过 `WOOZY_LLVM_BIN` 指定 LLVM 工具目录（相对路径按编译器所在目录解析，默认查找同目录下的 `toolchain/`）。搬动编译器时，要一并复制标准库、运行库和 C++ 版的 Native helper，并在目标机器上按需用 `WOOZY_LLVM_BIN` 指定工具位置。

已验证的独立工具链版本、来源和摘要见 contracts/toolchain.lock.json。本机的官方 Linux 包另外需要 libxml2.so.2 和静态 zstd：兼容库取自验证过发行签名的 Arch libxml2-legacy，zstd 1.5.7 从官方源码构建并核对发布的 SHA-256。系统库没有被替换。

### 安装自举版工具链

已有自举版编译器、对应的两份运行库和锁定版本的 LLVM 工具时，在仓库根目录运行以下命令。把示例中的编译器和 LLVM 路径替换为实际位置。默认从编译器旁的 `runtime/` 取得运行库，也可用 `--runtime-dir` 指定。

```sh
python3 tools/install_toolchain.py \
  --prefix "$HOME/.local/opt/woozy-current" \
  --compiler /path/to/selfhost/woozyc \
  --runtime-dir /path/to/runtime-archives \
  --llvm-bin /path/to/llvm-22.1.8/bin
python3.12 tests/tooling/installed_bundle_test.py "$HOME/.local/opt/woozy-current/woozyc"
```

安装器把编译器、标准库、Native/hosted 运行库、Python 运行库、四个 LLVM 工具和 Clang 自带头文件放在同一目录。安装目标须尚不存在。搬动时移动整个目录，可以用指向其中 `woozyc` 的符号链接将命令加入 PATH。

如果 LLVM 提示缺少系统未提供的动态库，可用 `--compat-lib /path/to/libxml2.so.2` 等参数按所需文件名复制兼容库，可重复传入该选项。安装目录中的 LLVM 启动脚本会自行查找这些库，无需在 shell 配置中设置 `LD_LIBRARY_PATH`。目标机器仍需兼容已有二进制所用的 Linux x86_64 系统 ABI 和其他系统库。原生分包构建仍使用系统的 Make 或 Ninja，hosted 构建需要宿主 C 开发环境，执行 Python 输出需 Python 3.12，TLS 附加库单独构建。

在 Apple Silicon 上用同一条安装命令，将 `--compiler` 指向原生 Wz 编译器，将 `--runtime-dir` 指向含 `liblang_darwin_core.a` 的目录，将 `--llvm-bin` 指向 macOS arm64 的 LLVM 22.1.8。链接程序还需要 macOS SDK。先用 [Mac 自举说明](SELFHOST_BOOTSTRAP.md#macos-arm64) 构建原生编译器。打包工具在 Mac 上生成 `woozy-macos-arm64.tar.gz` 和对应的 SHA-256 文件。

### 预编译发行包

要从 GitHub 安装、检查或升级已发布的编译器，使用 README 的[一键安装命令](../../README.cn.md#安装)。

拿到压缩包和 SHA-256 文件后，在下载目录校验并解包。将整个 `woozy/` 目录放到希望安装的位置。

```sh
sha256sum -c woozy-linux-x86_64.tar.gz.sha256
tar -xzf woozy-linux-x86_64.tar.gz
./woozy/woozyc --version
```

制作发行包时，用 Python 3.11 或以上版本运行 `tools/build_release.py`。编译器、运行库和 LLVM 参数与安装器相同。将 `--prefix` 换成 `--out-dir`，两个文件会一并写入新目录。用可重复的 `--notice FILE` 放入 LLVM 及随包复制的兼容库的许可证文件。需要的库取决于 LLVM 的来源，例如上述 Arch libxml2 还依赖 ICU。验证时用无工具链环境变量的 Linux x86_64 环境解包并构建原生和 Python 示例。

GitHub Actions 的 [日常检查](../../.github/workflows/ci.yml) 在 PR 和 `main` 更新时运行。Linux 构建前端和 Python 后端并执行对应回归，Apple Silicon 构建 Darwin 运行库并执行系统、网络、IPC、信号和异步等测试。

[发布工作流](../../.github/workflows/release.yml) 在版本标签推送或手动运行时构建 Linux x86_64 和 macOS arm64 两个包。Ubuntu 24.04 从源码构建 C++ 种子和 Woozy 编译器，并用后者按源码包生成 Mac 编译器的 Mach-O 对象，保留 LLVM 逐步校验。Mac 任务校验对象归档，在 Apple Silicon 上用本机 Xcode SDK 和 Darwin 运行库链接。两个平台都按 [工具链锁文件](../../contracts/toolchain.lock.json) 校验下载的 LLVM。

两个包都要解包校验 SHA-256、版本和源码提交，再用解包后的编译器运行示例、标准库、Python、编辑器和本机网络用例；Linux 在禁网容器中执行，源码调试分别用 GDB 和 LLDB 验证。

在 Actions 中手动运行时只生成附件，可从该次运行的 **Artifacts** 下载产物。推送 `v0.1.0` 这类版本标签时，两个包都通过上述验证后才会一起发布到同一个 GitHub Release；带后缀的标签（例如 `v0.1.0-rc.1`）会发布为预发行版。

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

各阶段记录在外层 checks/。T00 的 C++20、LLVM 22.1.8 探针和 Clang/LLD 运行检查已通过；T02 的 Debug/Release 语法分类为 80/80。后续阶段各自记录检查结果，不拿工具链探针代替语言程序运行。

T04 的类型表与标量 HIR 验证组件位于 lang_hir，包含 74 项损坏检查。lang_sema 基于真实项目和名称解析结果生成 HIR，支持基本整数、bool、Unit、函数值、无捕获匿名函数、控制流、调用和数值/bool 整体 inout；sema.scalar 执行 180 个静态项目检查，sema.cli 检查真实 CLI。具体范围、未支持操作和实际验证见 [CHECKER.md](../tests/sema/CHECKER.md)。两个标量后端已生成并运行程序；完整严格浮点算术仍未实现，普通参数重新赋值规则等待用户选择。T07 的泛型、聚合类型与 trait 静态接口及当前限制见 [GENERICS.md](GENERICS.md)；静态 HIR 通过不代表新增后端执行已经通过。

## Python 标量程序

```sh
woozyc build --project tests/fixtures/core/lang.toml --backend python --out-dir build/core/python
/path/to/python3.12 -s build/core/python/program.py
```

build 会检查源程序并写出 program.py 和 langrt.py，但不执行 Python，运行需要单独执行。生成器与前端不链接 CPython 或 LLVM。Python 首个执行 profile 为 64 位，32 位项目会在生成阶段明确拒绝。本机安装的解释器只用于运行测试，可通过 -DLANG_PYTHON312=/path/to/python3.12 指定；找不到解释器时，python.projects/python.cli 会明确记为 not_run，C++ API 测试仍照常运行。

正式集成后，GNU Debug/Release 和 Clang22 ASan/UBSan 均为 21/21 通过，不启用 LLVM 的前端构建为 18/18 通过；17 个必须执行的项目、15 项补充检查和 5 项 Panic 分类对照均已实际通过。完整检查清单和限制见 [PYTHON_SCALARS.md](PYTHON_SCALARS.md)。

## 原生标量程序

```sh
build/native-cli/woozyc build --project tests/fixtures/core/lang.toml \
  --backend native --opt 2 --out-dir build/core/native --format json
build/core/native/program
```

原生构建仅接受 Linux x86_64、64 位项目和 O0/O2。woozyc 不链接 LLVM 或 CPython，代码生成由独立 helper 完成。源码或未支持错误返回 1，工具缺失和写文件错误返回 2，链接失败报 E_LINK_FAILED。写出产物前会检查目标文件类型；替换第二个文件出错时，会恢复第一个旧文件（没有旧文件则删除新文件），恢复失败则保留备份并在诊断中标出位置。构建命令本身不执行生成的程序。

正式 CLI 的 17 个项目分别在 O0 和 O2 下执行（共 34 次），另外覆盖普通返回 7、源码错误时保留原输出、无 main 以及写入失败清理等测试；独立对象生成测试还包含 62 次 ELF 执行。ASan/UBSan 只覆盖本项目编译器代码，生成程序使用的独立运行库不继承编译器的检查选项，预编译 LLVM 和生成程序也不计入该检查范围。

## 用 GDB 调试 Woozy 程序

Linux x86_64 原生构建加 `-g`，将 `.wz` 的源码位置和变量信息以 DWARF 格式写入可执行文件。

```sh
woozyc build --project tests/fixtures/core/lang.toml \
  --backend native --opt 0 -g --out-dir build/core/debug
gdb -ex 'directory tests/fixtures/core' build/core/debug/program
```

在 GDB 中用 `break main.wz:行号` 或 `break 函数名` 设置断点。`run` 启动，`step` 进入调用，`next` 执行到下一行，`bt` 查看调用栈，`info args` 和 `info locals` 查看参数和局部变量。`info threads` 查看原生线程。

DWARF 中的项目源码路径相对于 `lang.toml` 所在目录。移动项目后，在 GDB 中用 `directory 项目目录` 指向新的位置。项目之外的标准库源码记作 `stdlib/…`，外部源码依赖记作 `dependencies/模块编号/逻辑模块/…`。可用 GDB 的 `set substitute-path` 指向这些源码的实际目录。

结构体按源码字段显示，枚举显示当前变体及其字段。局部变量从初始化完成后可见，内层变量可以遮蔽外层同名变量。进入闭包后可以查看捕获值。异步函数可以在 `await` 前后的源码行设置断点，并查看该位置仍有可用存储的变量。

多个退出分支可以各自执行同一个 `defer`。GDB 按行设断点时可能只选中一处，可以用 `disassemble /s` 查看其余分支的地址，再用 `break *地址` 设置断点。

在仓库根目录启动的 GDB 中执行 `source tools/gdb/woozy.py`，就可以用 `print` 直接查看 UTF-8 字符串及 Vec、Slice 的元素。该脚本只读被调试进程的内存。

`-g` 可以与 `--package-objects` 一起用，Woozy 版也可以和 `--source-make` 一起用。含有异步函数的项目可以用整包或 `--package-objects` 构建。源码单步和变量检查使用 `--opt 0` 较方便。`--opt 2` 下，内联和代码消除可能让行号跳跃，GDB 也可能显示 `<optimized out>`。C 外部类型和运行库内部对象的字段暂未完整描述。

## 用 LLDB 调试 Woozy 程序

Linux 上，LLDB 和 GDB 可打开同一个用 `-g` 生成的 ELF 文件。进入 `lang.toml` 所在目录，运行 `lldb /path/to/build/core/debug/program`。

macOS 上加 `-g` 会在 `program` 旁写出 `program.dwarf`。用 `--symfile` 载入符号。需要单步或查看局部变量时用 `--opt 0`。

```sh
woozyc build --project examples/hello/lang.toml --opt 0 -g --out-dir build/hello
lldb -o 'target create --symfile build/hello/program.dwarf build/hello/program'
```

在 LLDB 中用 `breakpoint set -f main.wz -l 行号` 设置断点，`run` 启动，`next` 单步，`frame variable` 查看当前帧的参数和局部变量。源码移动后用 `settings set target.source-map 原目录 新目录` 指定位置。

用 `command script import /path/to/woozy/tools/lldb/woozy.py` 加载字符串、Vec 和 Slice 的显示脚本。Vec 在调试信息中是指针，`frame variable *numbers` 可查看名为 `numbers` 的 Vec 的元素。
