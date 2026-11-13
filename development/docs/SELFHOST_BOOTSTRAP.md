# 自举检查

`selfhost/bootstrap.py` 把同一份编译器和标准库源码复制到固定目录，再依次构建 stage1、stage2、stage3。stage1 使用现有 C++ 编译器；后两代只能使用上一代 Woozy 编译器，以及指定目录里的 opt、llc、Clang 和 LLD。运行库使用构建目录中已有的两个静态库。

完整移植完成后运行：

```sh
python3 selfhost/bootstrap.py \
  --out-dir /absolute/path/to/empty-bootstrap-directory \
  --strace /absolute/path/to/strace
```

默认工具链是仓库旁的 LLVM 22.1.8，并为 LLVM 工具设置相应 libxml2 目录。`--stage0`、`--toolchain`、`--runtime-dir`、`--libxml-dir` 可以指定已有安装；脚本不下载或安装工具。输出目录必须为空，且不能位于编译器源码目录内。

目录中保存冻结的源码、各代标准输出/错误输出、构建命令、程序哈希和后两代 exec 记录。出现未允许的子进程或尚不支持的编译步骤时立即失败。三代模式还要求 stage2 与 stage3 的二进制完全相同。

`result.json` 的 `status=ok` 只说明分代构建及上述检查完成。`corpus_parity_checked=false` 明确表示它没有运行现有语言语料；有效/无效程序、诊断及执行结果对照仍须单独完成。`--stages 1` 只检查复制资源和首代构建，不能证明自举。

2026-11-13 已实际检查：当时的部分 CLI 可由 stage0 构建，但 stage1 尝试生成 stage2 时按预期报 `E_CAPABILITY_UNAVAILABLE`，exec 记录只有该 stage1，没有旧 C++ helper。此检查证明脚本会拒绝未完成的编译器，完整自举尚未通过。
