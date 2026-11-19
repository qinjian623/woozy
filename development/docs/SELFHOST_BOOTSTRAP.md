# 自举检查

`selfhost/bootstrap.py` 把同一份编译器和标准库源码复制到固定目录，再依次构建 stage1、stage2、stage3。stage1 使用现有 C++ 编译器；后两代只能使用上一代 Woozy 编译器，以及指定目录里的 opt、llc、Clang 和 LLD。运行库使用构建目录中已有的两个静态库。

用 Make 按源码包自举时，执行 `make -C selfhost bootstrap`。第一代用 C++ 种子生成分包对象，第二、三代用上一代 Woozy 编译器执行 `--source-make`。GNU Make 为每个源码包编译对象，再合并对象和链接。执行检查也会核对 Make、shell 和 mkdir 的路径。

```sh
make -C selfhost bootstrap \
  OUT=/absolute/path/to/empty-bootstrap-directory \
  STAGE0=/absolute/path/to/seed/woozyc \
  RUNTIME_DIR=/absolute/path/to/seed/native \
  STRACE=/absolute/path/to/strace JOBS=6
```

也可从其他目录用 `make -f '/absolute/path to/repo/selfhost/Makefile' bootstrap` 独立调用；两种调用都允许通过 `MAKEFILES` 预载其他 Makefile，但不支持通过 `include` 或多份 `-f` 混用该入口。`OUT`、`STAGE0`、`RUNTIME_DIR` 和 `STRACE` 按字面值传给脚本；例如在 shell 中写 `OUT='/path with spaces/$name'`，不要为了 Make 额外转义 `$`。`PYTHON` 仍可指定命令及参数，例如 `PYTHON='python3 -I'`。

Make 入口默认构建三代，使用 `OPT=0`，每个包仍执行 LLVM 验证。可指定 `OPT=2`，或用 `STAGES=2` 只构建到第二代。下游 GNU Make 使用 `-jJOBS`，不继承外层 Make 的选项、jobserver、额外 Makefile 或命令行变量覆盖。该入口省略对象缓存。

多代 `OPT=2` 自举会一并冻结 LLVM 22.1.8 的 `llvm-split` 和 `llvm-objcopy`。第二、三代的源码 Make 先验证大包的完整 LLVM IR，再把至少 512 个函数的包拆成 16 份。每份使用相同的 O2 流程、默认内联和逐遍验证，最后按固定顺序合并成该包的对象文件。不同分区之间的调用留给链接器，内联在各自分区内进行。

## 冻结输入与 stage0 实际来源

stage0 的 C++ 可执行文件不会读取目标目录里的 `resources`。脚本读取 stage0 同级的 `build.ninja` 和 `CMakeCache.txt`，要求 `LANG_ENABLE_LLVM=ON`，并解析 seed 编译规则及 native helper 编译规则中的 `LANG_STD_SOURCE_ROOT`、`LANG_NATIVE_HELPER`、`LANG_NATIVE_RUNTIME`、`LANG_HOSTED_RUNTIME`、`LANG_NATIVE_CLANG`、`LANG_NATIVE_LLD`。报告中的 `stage0` 同时保留宏的原始 pathname 和首次解析目标、字节哈希及 CMake 配置哈希，并明确 `uses_copied_resources=false`。每一代前后都重新跟随宏原始 pathname，检查其指向目标、stdlib 目录集合和文件字节；只改向 symlink 而保留旧目标也会记录为 `input_changes` 失败。seed 命令行入口、helper 和 resources 链接按同样方式核对。seed 和 helper 另存到 `resources/stage0` 只作为核对副本，不参与执行，不能据此声称 stage0 使用了复制资源。

脚本把 stage0 实际 stdlib `.wz`、两个运行库归档与冻结副本逐字比较；无法解析宏、宏值冲突、路径缺失或字节不同都在 stage0 明确失败。每一代运行前后还会重新核对完整冻结输入：compiler `.wz`、compiler manifest、stdlib `.wz` 集合、native/hosted 归档、两个 Python runtime 文件、`resources/runtime` 的完整文件集合、实际解析后的工具路径和字节、seed/helper 及其 build 配置。新增、丢失或字节变化都会失败；缺失文件也作为可报告的输入变化记录（例如 before 为原哈希、after 为 missing），并在 `result.json` 的 `failed_stage` 和该代 `input_changes` 中保留阶段、路径和哈希。当前阶段发生任何异常都会把该 stage 标为 failed，不让未完成记录停留在 running。

每次阶段启动前先写入 `stages` 记录，再执行构建；stdout、stderr、退出码、`timed_out`、`cleanup_complete` 和 `abandoned_pipes` 都会落盘。超时使用 `corpus.py` 的进程组和管道清理 helper，孙进程一并终止；脱离进程组并持有管道时有限返回并标记失败。超时或清理不完整绝不算 passed。

exec 追踪中的相对 pathname 取决于被跟踪进程 cwd，strace 记录不足以恢复，bootstrap 和 corpus 都直接拒绝；绝对 `execve`/`execveat` 仍按允许集合核对，空 pathname 和无法解析的记录继续失败。

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
