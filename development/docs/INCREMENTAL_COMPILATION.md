# 源码逻辑包增量编译

## Wz 编译器的源码 Make 入口

```sh
MAKEFLAGS=-j4 woozyc build --project lang.toml --source-make \
  --out-dir build --opt 2
```

此入口适用于 Linux x86_64 和 macOS arm64 的 Native/64，需要 GNU Make 和 LLVM 22.1.8。一个逻辑包可以包含多个 `.wz` 文件。主进程只加载一次源码和项目声明。Make 为每个包安排独立的任务，编译进程从主进程已准备的源码和符号开始。声明检查通过后，未命中或未启用对象缓存的包共享已检查的声明，分别检查本包函数体和用到的外包泛型模板，再校验 IR 并生成该包的 `.o`；启用缓存并命中时，直接取出该包的对象，跳过函数体解析、检查和 LLVM 处理。全部包就绪后合并对象并链接。

需要增量编译时，加上 `--cache-dir .woozy-cache`。三包用例首次构建重编 3 包，重复构建重编 0 包。改私有函数体只重编本包。改公开签名或影响跨包传值的类型布局，会重编本包和调用方。构建失败时保留上次成功的程序。可在输出目录查看 `Makefile`、`make.log` 和 `make-report.json`。

已验证标量跨包调用、struct 和 enum 跨包传值，以及自举编译器实际用到的泛型和 `std/` 导入。用这个入口编译完整工程时，第二、三代各生成 57 个包对象，链接得到的 Woozy 编译器逐字节相同。入口和运行条件见 [自举说明](SELFHOST_BOOTSTRAP.md)。

### macOS arm64 构建

Mac 上的 Wz 编译器默认生成 Mach-O arm64 可执行文件。普通 `build`、`--source-make` 和 `--package-objects` 都会链接 `program`。安装 Xcode Command Line Tools，或将 `SDKROOT` 指向 macOS SDK。把 `liblang_darwin_core.a` 放在编译器旁的 `runtime/` 目录。程序要求 macOS 14.4 或更新版本，核心运行库只链接系统的 `libSystem`。

需要在 Linux 上只生成 Mac 对象时，使用以下命令。

```sh
WOOZY_LLVM_BIN=/path/to/llvm-22.1.8/bin \
  woozyc build --project lang.toml --out-dir build-macos --source-make \
  --objects-only --target arm64-apple-macosx11.0.0 --opt 2
```

GNU Make 按源码包分别检查并编译，LLVM `opt` 校验每个包，`llvm-ar` 把 Mach-O arm64 对象归档到 `program.a`。`--cache-dir` 可用于生成对象或链接程序。包含 async 的项目使用普通 `build` 或 `--package-objects`。C 绑定项目使用普通 `build`。

对象归档、完整程序、带调试信息的程序各用独立的 `--out-dir`。构建只更新本次输出的文件。

将归档放到安装了 macOS SDK 的 Apple Silicon 主机上，即可手动链接使用这些运行库接口的程序。

```sh
cmake -S runtime/darwin -B build-darwin-runtime -DCMAKE_BUILD_TYPE=Release
cmake --build build-darwin-runtime
xcrun --sdk macosx clang -target arm64-apple-macosx14.4 -nostdlib++ \
  -Wl,-dead_strip -Wl,-force_load,build-macos/program.a \
  build-darwin-runtime/liblang_darwin_core.a -Wl,-U,_lang_vec_observe_v1 \
  -Wl,-u,_lang_darwin_startup_v1 \
  -o program
```

链接参数允许可选的 Vec 测试观察函数缺省，并保留读取参数和环境的 Darwin 启动函数。需要在 Linux 上直接链接 Mac 程序时，去掉 `--objects-only`，指定 `SDKROOT`，并在编译器资源目录中放入 Darwin 运行库。

在 macOS 主机启动 O0 自举前，将本次 shell 的栈上限设为至少 16 MiB，例如 `ulimit -S -s 16384`。默认 8176 KiB 时，`nativeverify` 编译进程实测写到了栈保护页；16 MiB 下该包通过 LLVM 校验，第二、三代各编译了 76 个包，两个编译器逐字节相同。

## 原有的分包对象入口（SH31）

当前 C++ 入口：

```sh
woozyc build --project lang.toml --out-dir build \
  --package-objects --cache-dir cache --opt 2 --format json
```

缓存单位为加载器已经识别的规范目录 Module。同包文件仍共享顶层声明，import alias 仍只在本文件有效，包环仍由现有加载器报 `E_IMPORT_CYCLE`。原 build/check/Python 入口保留。

目前已实现 C++ 普通标量函数的持久前端缓存，以及 Linux x86_64 Native 分包对象。Wz 版编译器也通过了实际 cold/warm、私有函数体长度变化、干净构建对照、损坏缓存及同输出目录并发测试。函数体缓存按整包命中：只有整包都符合普通标量等格式条件才可命中；不符合的包会回退到原 checker。SH31 目前仍缺少完整语言前端缓存和 Ninja 前端任务，尚未完成。

## 实际执行步骤

1. 每个当前源码文件由 source.Manager 取得快照，按实际编译器/工具/运行库指纹、规范文件名及原始字节摘要查询 AST。命中时恢复当前快照及 Span，不再对整个文件 lex/parse。字符串 token 的缓存解码值必须与实际源码经现有 lexer 解码的值一致；这项小范围 lex 计入 `string_tokens_checked`。
2. 运行现有全项目 resolver、声明及常量检查。整包都符合普通标量等格式条件时，才恢复该包已检查的函数体及当前会话的 SymbolId、TypeId、HirNodeId、ScopeId、NodeRef 和 Span；不符合的包回退到现有 checker。整包命中后，其中的函数体跳过 `check_body`。当前这一步仍有全项目工作。
3. 恢复记录先检查字段范围、符号唯一性、函数签名、Scope.parent、children 无环、调用参数及受支持节点与当前 AST 的对应关系。重新产生 ownership 后仍需通过原完整 HIR 验证；导入后验证失败会关闭本次前端命中并改用现有 checker 重试。缓存字节不会直接产生 VerifiedHirModule。
4. 原 NativeIR 降低和验证仍执行。使用同一份不可变、已验证 HIR 的身份索引生成各包的独立 LLVM 模块。函数定义只属于 origin 所在包，其余为外部声明。只有 entry 所在包生成 main。泛型实例名包含 origin 的声明键及结构 TypeKey；实例的重命名局部声明由所属实例键派生。
5. 每次先发射每包真实未优化 LLVM，并以其完整字节摘要和工具/编译器指纹查询对象。命中时跳过该包 opt、llc。未命中包由 Ninja 执行 opt → llc，随后 Ninja 执行 `ld.lld -r` 和最终链接。O2 使用完整 `-passes=verify,default<O2>,verify -verify-each` 及工具默认内联。Ninja 来自配置/发现结果；Wz 支持 `WOOZY_NINJA` 或 PATH。
6. 先在 toolio.Stage / C++ StagingDir 中写完所有输出文件。取得输出目录实际 inode 的 `flock(LOCK_EX)` 后，执行备份、整体发布及失败恢复。日志、统计、program.o、program 属于同一次发布；指向同一目录的别名或符号链接共用同一把目录锁，结束后不保留锁文件。

## 身份及失效

声明键使用 UTF-8 字节数的长度前缀，包含包规范身份、声明种类、owner 和名称。普通函数不含全局数字 ID、字节偏移或注释。局部声明含其 AST 子序号；文件内 alias 使用文件 owner。Impl owner 包含头部 token 及其中模块 alias 经 resolver 找到的规范目标身份，允许两个文件把不同 trait 都命名为 Q。TypeKey 结构化包含类型种类、位宽、声明身份、参数 mode、result、throws、实参及外部绑定身份。

三类索引都会一次建立并复用：NodeRef 到 parent/子序号/命名声明的映射、symbol 到实例/owner 的映射，以及已计算好的键。Native 多包发射共用同一不可变快照的索引。前端索引只在声明/实例表不变的单次恢复或写出阶段存在，不会跨 checker 修改保留。

当前依赖接口是从现有 AST 声明生成的规范 token 记录：公开类型及声明、公开常量声明的源码 token（含值表达式）、泛型体、trait/impl，以及文件 import。普通非泛型函数只取签名；私有普通函数体不进入其他包的接口记录。接口记录排序后计算摘要，当前包原始字节改变始终使本包失效。现有 impl 全项目可见，因此 impl 记录变化保守影响所有包。没有新增 orphan 规则。

对象键来自该包真实 LLVM IR，覆盖函数/泛型实例需求、名义类型实际布局、转引的编译期常量值、entry 及嵌入的运行库/快照数字；这些任一改变都会触发重新编译。声明身份和名义 TypeKey 仍不混入布局。只读函数视图按稳定函数名排序，避免函数体缓存命中集合改变声明排列；其余真实 IR 数字发生变化时允许保守未命中。编译器指纹包含实际执行的 driver/helper；工具和运行库的实际内容及选项均参与键计算。

工具文件的内容 SHA 另存于 `cache-dir/file-digests`：键含规范路径、dev/inode、大小、mtime/ctime 的秒和纳秒以及 mode/uid/gid/links/rdev/blocks。命中时核对同一打开文件和当前路径的元数据；未命中则完整读取内容，并在读取和计算后再次核对元数据。文件不稳定时重试或报文件环境错误，不能使用旧值。编译指纹仍由内容 SHA 组成，元数据不直接替代内容摘要。损坏记录会重新计算；删除 cache-dir 会一并清除这层缓存。缓存格式不是跨版本模块分发格式。

每个缓存文件都有有界长度编码和 SHA-256。摘要只用于发现损坏，不提供针对可写缓存的签名认证。AST 解码值及标量 HIR 还要与当前源码/结构核对。C++ 对象用 LLVM ObjectFile 检查；Wz 对象检查 ELF64 ET_REL 的头、节、字符串、符号及重定位范围。不支持或损坏的记录直接重编。

## 统计及已知未完成项

`cache-statistics.json` 提供 parse/frontend/object 命中/未命中、被拒绝的前端记录、实际 body 检查数、LLVM 发射任务数/耗时、opt/llc 任务数和 `tool_digest_hits/misses/bytes_read`；`package-records.json` 列出逻辑包、键和对象命中。`package-build.log` 保存 Ninja 实际命令。`--format json` 的 stdout 只放协议 JSON。

持久函数体格式目前仅覆盖普通标量节点，整包都符合格式条件才命中，不按函数分别命中；包内含泛型模板/实例、nominal、捕获、async、owned/GC 等不支持内容时，该包回退到原 checker。Native 分包可以编译这些既有功能，但不能据此声称这些函数体已经前端命中。C adapter 在新分包入口明确拒绝，原 build 继续支持。

仍需完成：公开类型、已检查泛型及其可达私有依赖的完整持久接口，完整语言 HIR 按包导入，移除跨包 checker/NativeIR 工作，以及 Ninja 前端依赖任务和并行调度。两版编译器已通过本轮针对性检查；SH31 尚无正式 Source O2 性能比较或三代自举结果。

外置工作记录：`<dev-work-dir>/checks/SH31-source-module-incremental-20261117/`。`progress.md` 和 `job.json` 按实际进展更新；v1/v2 报告对应各自冻结的源码与二进制，不能混用。

## 删除缓存与重建

缓存目录可以单独删除；原来发布成功的 program 不依赖缓存继续运行。下次用相同命令重新建立缓存。省略 `--cache-dir` 仍可使用 `--package-objects`，本次全部重新解析、检查并生成包对象。无论对象是否命中，当前入口总是重新组合 program.o 并重新链接 program。

```sh
rm -rf -- .woozy-cache
woozyc build --project lang.toml --out-dir build --package-objects --cache-dir .woozy-cache --opt 2 --format json
```

## 本轮固定版本结果

Wz v1 候选启用缓存时，每次都要重读约 900 MB 的编译器、工具和运行库内容，cold / warm / private 耗时分别约 50.039 / 57.938 / 56.004 秒。独立小包 probe 确认，仅读取四个 LLVM 工具（841705464 B）并计算 SHA 就占了约 48.534 秒；测试没有改动真实 LLVM 工具。

加入摘要缓存并固定 Wz v2 后，每种场景各测一次，实际三包 CLI 耗时如下：cold 48.5914 秒、warm 0.0936943 秒、private 0.126336 秒、禁用缓存 clean 0.117267 秒。warm 构建有 8 个摘要 hit、0 个 miss、读取 0 字节工具内容、0 次 body 检查、3 个 object hit、0 次 opt/llc；修改 A 的私有函数体长度后，只有 A 触发 2 次 body 检查和 1 组 opt/llc，app 和 B 的实际 LLVM 摘要不变。仅截断程序摘要小记录后，出现 7 hit / 1 miss，重读程序并恢复同一编译指纹，耗时约 3.23669 秒，产物结果一致。这些数据是 O0 编译的候选在该小例子上的实测值，不能当作完整 Source 或通用性能结论。

详细记录保存在外置 SH31 目录的 `wz-compiler-v2/build.json`、`wz-cli-incremental-v2/report.json` 和 `file-digest-cross-small/report.json`。保留 mtime 但改内容、替换 inode、修改 symlink 目标，以及截断、坏格式和 digest 形状损坏等情况，均使用小型可控文件测试，两端 SHA 与 Python 一致，未改动真实 LLVM。引用索引保留所有匹配项检查；Source 边界二分保留原有的 BOM/CRLF/Unicode 及非法字节行为，既有 O0/O2 Source 测试继续通过。
