# 自举检查

## Linux x86_64

`selfhost/bootstrap.py` 把同一份编译器和标准库源码复制到固定目录，再依次构建 stage1、stage2、stage3。stage1 使用现有 C++ 编译器；后两代只能使用上一代 Woozy 编译器，以及指定目录里的 opt、llc、Clang 和 LLD。运行库使用构建目录中已有的两个静态库。

按源码包自举时，第一代用 C++ 种子的 `--package-objects` 入口，仍需 Ninja。第二、三代用上一代 Woozy 编译器执行 `--source-make`，编译器直接安排每个源码包的编译，汇总对象后链接。第二、三代的执行记录只允许上一代编译器和冻结的 LLVM 工具。

```sh
python3 -B selfhost/bootstrap.py --source-make \
  --out-dir "$PWD/build/selfhost-bootstrap" \
  --stage0 "$PWD/build/woozy-stdlib-native/woozyc" \
  --runtime-dir "$PWD/build/woozy-stdlib-native/native" \
  --opt 0 --jobs 6
```

从仓库根目录运行这条命令。自举默认在仓库旁的 `toolchains/` 查找 LLVM 和兼容库，也可用 `WOOZY_TOOLCHAIN_ROOT` 指向目标机器上的同一目录布局。第二代起需要 Linux 的 `strace`，默认从 `PATH` 查找，可通过 `--strace` 指定。

习惯使用 Make 的开发者仍可运行 `make -C selfhost bootstrap`，也可从其他目录用 `make -f '/absolute/path to/repo/selfhost/Makefile' bootstrap` 独立调用；两种调用都允许通过 `MAKEFILES` 预载其他 Makefile，但不支持通过 `include` 或多份 `-f` 混用该入口。`OUT`、`STAGE0`、`RUNTIME_DIR` 和 `STRACE` 按字面值传给脚本；例如在 shell 中写 `OUT='/path with spaces/$name'`，不要为了 Make 额外转义 `$`。`PYTHON` 仍可指定命令及参数，例如 `PYTHON='python3 -I'`。

Python 脚本默认构建三代，`--stages 2` 只构建到第二代。用 `--opt 0` 或 `--opt 2` 选择优化级别，每个包都执行 LLVM 校验。第二、三代用 `--jobs` 设置同时编译的包数，默认 1，省略对象缓存。可选的 Make 入口默认使用 `OPT=0`，并把 `STAGES`、`OPT` 和 `JOBS` 直接传给 Python 脚本。

`--opt 0` 构建第二代及后续版本时，脚本会把本次构建进程的栈上限提高到至少 16 MiB。系统硬上限低于 16 MiB 时，脚本会报错。

自举源码按目录分包。`hirverify` 保留公共验证入口，声明、调用、节点、控制流、泛型实例和后端检查由六个子包实现。`semaeffects` 处理控制流和错误效果，`nativeadapter` 生成 C 适配器。各源码包生成自己的 LLVM IR 和对象文件，`OPT=2` 时每包完整执行一次 O2 和逐遍校验，再按固定顺序链接。

## 冻结输入与 stage0 实际来源

脚本读取 stage0 同级的 `build.ninja` 和 `CMakeCache.txt`，要求 `LANG_ENABLE_LLVM=ON`。新版 C++ 编译器会主动报告实际使用的标准库、helper、运行库和 LLVM 工具路径；旧版仍从 CMake 编译宏确定资源。生成的报告会记下路径、解析后的目标和文件哈希，每一代构建前后都会重新核对，符号链接改向其他文件也会记为 `input_changes`。另存在 `resources/stage0` 的 seed 和 helper 只用来核对文件字节，实际构建仍从原路径启动。

脚本会把 stage0 实际使用的 stdlib `.wz` 和两个运行库归档与冻结副本逐字节比较；无法解析宏、宏值冲突、路径缺失或字节不同，都会在 stage0 阶段直接失败。每一代运行前后还会重新核对全部冻结输入：compiler `.wz`、compiler manifest、stdlib `.wz` 集合、native/hosted 归档、两个 Python runtime 文件、`resources/runtime` 的全部文件、实际解析后的工具路径和字节，以及 seed/helper 及其 build 配置。任何新增、丢失或字节变化都会导致失败；缺失文件同样记录为输入变化（例如 before 为原哈希、after 为 missing），并在 `result.json` 的 `failed_stage` 和该代 `input_changes` 中保留阶段、路径和哈希。当前阶段发生任何异常都会把该 stage 标为 failed，避免未完成记录停留在 running。

每个阶段启动前先写入 `stages` 记录，再执行构建；stdout、stderr、退出码、`timed_out`、`cleanup_complete` 和 `abandoned_pipes` 都会落盘。超时清理复用 `corpus.py` 的进程组和管道清理 helper，会一并终止孙进程；若有子进程脱离进程组且仍持有管道，则在限定时间内返回并标记失败。超时或清理不完整一律不算 passed。

exec 追踪中的相对 pathname 取决于被跟踪进程的 cwd，仅凭 strace 记录无法恢复，bootstrap 和 corpus 都会直接拒绝；绝对路径的 `execve`/`execveat` 仍按允许集合核对，空 pathname 和无法解析的记录继续判失败。

完整移植完成后运行：

```sh
python3 selfhost/bootstrap.py \
  --out-dir /absolute/path/to/empty-bootstrap-directory \
  --strace /absolute/path/to/strace
```

两代及以上需要可执行的 strace。默认在调用者的 `PATH` 中查找 `strace`；`--strace name` 也按 `PATH` 查找，`--strace ./name` 则相对于调用目录解析。单代不查找 strace。`--timeout`（Make 入口的 `TIMEOUT`）必须是有限正数。

默认工具链是仓库旁的 LLVM 22.1.8，并为 LLVM 工具设置相应 libxml2 目录。`--stage0`、`--toolchain`、`--runtime-dir`、`--libxml-dir` 可以指定已有安装；脚本不下载或安装工具。输出目录必须为空，且不能位于编译器源码目录内。

目录中保存冻结的源码、完整输入快照（含 stage0 实际路径/哈希）、各代标准输出/错误输出、构建命令、程序哈希和后两代 exec 记录。出现未冻结输入变化、未允许的子进程或尚不支持的编译步骤时立即失败。三代模式还要求 stage2 与 stage3 的二进制完全相同。

`result.json` 的 `status=ok` 只说明分代构建及上述检查完成。`corpus_parity_checked=false` 明确表示它没有运行现有语言语料；有效/无效程序、诊断及执行结果对照仍须单独完成。`--stages 1` 只检查复制资源和首代构建，不能证明自举。

2026-11-13 已实际检查：当时的部分 CLI 可由 stage0 构建，但 stage1 尝试生成 stage2 时按预期报 `E_CAPABILITY_UNAVAILABLE`，exec 记录只有该 stage1，没有旧 C++ helper。此检查证明脚本会拒绝未完成的编译器，完整自举尚未通过。

2026-11-18 已实跑完整 Make 自举。第二、三代均实际编译 57 个源码包并链接，两个可执行文件及各自合并后的 `program.o` 均逐字节相同。此次完整构建使用 `OPT=0` 和 `JOBS=6`。

## macOS arm64

使用能在 Mac 上运行并支持 `--source-make --jobs N` 的 Wz 版 `woozyc` 作为第一代。准备好 LLVM 22.1.8、macOS SDK，以及 [Darwin 运行库](INCREMENTAL_COMPILATION.md#macos-arm64-构建)。以下命令从仓库根目录运行。

```sh
python3 selfhost/bootstrap_macos.py \
  --seed /path/to/seed/woozyc \
  --llvm-bin /path/to/llvm-22.1.8/bin \
  --runtime-archive build-darwin-runtime/liblang_darwin_core.a \
  --out-dir build/macos-bootstrap-o0 --opt 0 --jobs 4
```

输出目录必须是新目录。标准库默认取自种子旁的 `stdlib/`，也可用 `--stdlib` 指定。脚本只在本次自举的进程内将栈上限提高到至少 16 MiB。即使选了 `--opt 2`，输入的种子也可能是 O0 编译出来的。

第二、三代各由上一代通过 `--source-make` 按源码包构建。每个包保留 LLVM 校验，生成的 Mach-O 归档用 SDK 链接。脚本运行两个新编译器检查项目，最后逐字节比较两代可执行文件。每代日志及 `result.json` 在输出目录内。

Apple Silicon 实机用移植期间的固定源码快照完成了 O0 和 O2 两轮自举。每轮的第二、三代各编译 76 个源码包，两个可执行文件逐字节相同。
