# Selfhost nativebuild

`app/wzcc/nativebuild` 将 `nativeemit.Output` 的 LLVM 文本交给固定 LLVM 工具，发布 `program.o` 和 `program`。只用当前 Wz 的 model、resources、toolio 和标准库；生产代码不调用旧 C++ 前端或 helper。

```wz
pub func build(output nativeemit.Output, project model.Project,
               out_directory string, optimization u32) model.UnitResult
```

只接受 `x86_64-unknown-linux-gnu` 和 O0/O2。调用方负责 load、resolve、sema、lower、verify、emit；这里没有新的 Verified 标志。成功返回 Complete、has_value=true、空 diagnostics，CLI 自己打印成功信息。失败返回 Failed、has_value=false；工具诊断没有源位置。

## 固定资源与命令

资源取自 `resources.directory()`，即 `/proc/self/exe` 所指可执行文件的目录：

```
toolchain/opt
toolchain/llc
toolchain/clang
toolchain/clang++
toolchain/ld.lld
runtime/liblang_native_runtime.a
runtime/liblang_hosted_runtime.a
```

这些工具可以是指向现有 LLVM22.1.8 的符号链接。每次 build 都运行实际使用的 opt、llc、clang、ld.lld 的 `--version`，检查完整版本 token `22.1.8`，不接受 `22.1.80`。clang++ 目前不执行。两个 runtime 采用现有归档，不重新编译运行库；与旧 helper 一样，hosted 也先检查 native runtime 是否存在。

所有命令使用绝对资源路径，经 `toolio.run` 执行，不经过 shell。其 stdout/stderr 原始字节合并捕获。LLVM 各段用 `os.write_all_text` 依次写入私有目录的 input.ll，不拼整模块字符串。

| 步骤 | 参数 |
|---|---|
| 普通 O0 | `opt -S -passes=verify -verify-each -mtriple=<triple> -mcpu=generic input.ll -o verified.ll` |
| 有 async 的 O0 | 将 passes 改为 `verify,default<O0>,verify`，执行 coroutine split |
| O2 | 将 passes 改为 `verify,default<O2>,verify` |
| 对象 | `llc -filetype=obj -O=0/2 -mtriple=<triple> -mcpu=generic -relocation-model=pic -code-model=small verified.ll -o program.o` |
| 非空 C adapter | `clang --target=<triple> -std=c11 -O2 -fPIC -c c_adapter.c -o c_adapter.o` |

链接顺序为 `clang --target=<triple>`、runtime 模式参数、`--ld-path=<资源目录>/toolchain/ld.lld`、program.o、可选 c_adapter.o、选定 runtime、Project.c_library_dirs 中的各 `-L`、Project.c_libraries 中的各 `-l`、`-o program`。hosted 参数为 `-Wl,-u,lang_hosted_startup_v1 -pthread`；freestanding 为 `-nostdlib -static`。本次要求两种模式都固定 LLD，所以 hosted 从旧 helper 默认的 GNU ld 改为固定 LLD；其余对象、库和参数顺序保留。

不添加 `--fp-contract`。x86 llc 会忽略该选项；无 FMA contraction 由发射的 LLVM 指令没有 fast/contract 标志保证，nativebuild 不改浮点指令。

## 失败与输出

| 失败 | code / phase / FailureKind |
|---|---|
| 非固定 target | E_TARGET_UNSUPPORTED / codegen / Unsupported |
| 非 O0/O2 | E_CAPABILITY_UNAVAILABLE / codegen / Unsupported |
| 版本不符、缺工具、工具执行错误 | E_TOOLCHAIN_MISSING / codegen 或 link / Environment |
| opt/llc 已执行但退出非零或被信号结束 | E_IR_INVALID / codegen / Internal |
| C adapter 或链接普通非零 | E_LINK_FAILED / link / Source |
| C adapter 或链接被信号结束、退出126/127 | E_TOOLCHAIN_MISSING / link / Environment |
| 创建、写入、发布输出失败 | E_INPUT_IO / codegen / Environment |

OS 错误信息保留实际 errno 和 completed，工具非零退出附原始输出 note。有效 UTF-8 原样保留，包括 NUL；非法 UTF-8 时用 `raw tool output bytes:` 后的逐字节十六进制保留全部内容。C adapter 与 linker 的消息保留旧 helper 文本。版本不符也附实际 `--version` 输出。

`toolio.stage(out_directory,"native")` 创建私有目录，defer cleanup。全部工具成功后调用 `publish_pair(inout stage,"program.o","program")`；LLVM 验证或链接失败时不发布。普通失败清理临时文件，不改旧输出。toolio 报 Rollback 时保留其设置的 stage.preserve，并在诊断中报告实际备份/残留路径。这里只提供 program.o 和 program，LLVM 文本和 adapter 不作为用户输出。进程中断不保证两个 rename 同时完成，这与 toolio 和旧 helper 的限制一致。

## 实际测试

```
python3 selfhost/tests/nativebuild/run.py --woozyc <现成stage0> --work-dir <独立目录>
```

runner 快照实际生产 nativebuild、toolio 及依赖包，只取 nativeemit/state.wz 和 text.wz，避免拉入尚未完成的四组 emitter 主体。main.wz 直接调用上述 build，手写 Output 仅测试工具驱动，没有伪造 VerifiedModule。每个测试可执行文件复制到独立资源目录，工具和运行库在其旁用符号链接提供。现成 C++ stage0/helper 只在编译这个 Wz 测试程序时使用，不在被测试的 build 内执行；没有重建 compiler 或运行库。

O0/O2 各测试 freestanding、hosted、C adapter、C adapter+现有 `tests/native/c_ffi/fixture/ffi_fixture.c` 静态外库，以及实际 suspend/resume/destroy 的 LLVM coroutine，生成程序均须返回42。每组成功测试运行 strace，仅允许测试 driver 和固定 LLVM 程序的 execve；检查 hosted/freestanding 都执行资源目录的 ld.lld、没有 `/usr/bin/ld`，并检查实际 opt passes、llc 和链接参数。

另外检查坏 LLVM、坏 C、未定义符号、工具 ENOENT/EACCES、已有输出保留、非法输出文件/目录、非法 target/优化级别。小 C 诊断工具单独检查版本前缀拒绝、非法 UTF-8/NUL 输出保留，以及 signal/126/127 的分类；这些记录标为 diagnostic fixture，不算真实 LLVM 执行成功。每条命令保留 stdout/stderr/退出码，inputs.json 记录本次实际输入哈希，results.json 记录检查结果与未运行项。

未测试 toolio 恢复操作本身再次失败的 Rollback 分支；该分支只检查诊断与 preserve 传递，不重复 toolio 的40项测试。完整语言 emitter 与新 CLI 联合执行由 Root 另测。

2026-11-13 实跑 check、O0/O2 全通过：每级19个 driver 用例，另各5个固定工具 strace 检查，共48项检查、54条命令；10个生成程序均执行返回42。结果位于仓库外 `checks/nativebuild-20261113/woozy-nativebuild-wv27vzxk/results.json`，对应 inputs.json SHA-256 为 `7cb9a4af6232df3e11124de454b2343142322053265e4c795e494bd3ca67505c`。测试快照 nativebuild.wz SHA-256 为 `e4996399a8b89ca493e2aeb5f5d031ff0e78da3bec0b2c1b82d3445da409f648`，测试后与生产文件一致。
