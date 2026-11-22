# 埃氏筛大规模正确性准备

`large_prepare.py` 沿用原 Source/C++ 模板，支持含上限的 N=1000000/10000000 和 R1/R2，直接读取冻结的完整 prime-flags 文件，不重新生成预期值。当前工具 SHA 固定为性能目录的 d599 版本。

普通入口会检查两端在 O0/O2 下的正常运行、R1 的错误 count/sum 退出码、Source CLI 与 helper 对象逐字节一致、LLVM verify 和 Source 静态 ELF。辅助观察器读取每轮全部 composite 字节，不改动正式对象，也不参与计时。

已保存的四组准备结果包含 8 对一致对象和 8 个观察文件（共 12 帧），全部字节与冻结结果匹配。`--recheck-existing` 只复查已保存的 IR，不重新编译大 AST，且实际复用了普通入口的参数解析与验证逻辑。

Root 修正了 Source 两轮检查里常量假累加和无用 select 的漏洞：现在会一直追到实际累加 phi、下标与 flag 贡献，再追到 select 经最终 phi 进入 ret。正常 R2 通过，把真实 R1 按 R2 检查时会拒绝；额外恒真回边、错误 phi、常量贡献、无用返回等所列反例均会拒绝。

Root 在保留的同一批二进制上重跑了 16 个正常程序、16 个错误预期程序和 2 个单轮 C++ 对照，退出值均符合预期。原有失败与修正记录保持保留。

这些都只是针对固定文件的有限检查，C++ 展开区域仍需结合实际 IR 和完整观察帧复核。工具、布局、输入或正式 R 变动后必须重查，不能当作对任意 LLVM 程序的通用证明。当前没有正式 R 校准、性能采样或 2 倍以内结论。

主目录资料：`reviews/sieve-large-root-review-20261110.md`、`reviews/sieve-large-root-repair-independent-20261110.md`、`checks/sieve-large-root-verification-20261110/`。命令参数见 `large_prepare.py --help`。
