# 埃氏筛大规模正确性准备

`large_prepare.py` 使用原 Source/C++ 模板，支持含上限 N=1000000/10000000、R1/R2，读取已冻结的完整 prime-flags 文件，不重新生成预期值。当前工具 SHA 固定为性能目录的 d599 版本。

普通入口检查两端 O0/O2 正常运行、R1 的错误 count/sum 退出值、Source CLI/helper 对象字节相同、LLVM verify 和 Source 静态 ELF。辅助观察器读取每轮的全部 composite 字节；不改变正式对象，不参加计时。

已保存的四组准备有8对相同对象、8个观察文件共12帧，全部字节匹配冻结结果。`--recheck-existing` 重新检查保存的 IR，不重编大 AST；它实际调用普通入口的参数解析和验证。

Root 修正了 Source 两轮检查中的常量假累加和无用 select：现在追到实际累加 phi、下标及 flag 贡献，再追到 select 经最终 phi 进入 ret。正常 R2 接受、真实 R1 按 R2 拒绝；额外恒真回边、错误 phi、常量贡献、无用返回等已列反例拒绝。

Root 在保留的同一批二进制上重跑16个正常程序、16个错误预期程序和2个单轮C++控制，退出值符合预期。原失败与修正记录均保留。

这些是固定文件的有限检查，C++ 展开区域仍结合实际 IR 和完整观察帧复核。工具、布局、输入或正式R变化后需要重新检查，不能当成任意LLVM程序的证明。没有正式R校准、性能采样或2倍以内结论。

主目录资料：`reviews/sieve-large-root-review-20261110.md`、`reviews/sieve-large-root-repair-independent-20261110.md`、`checks/sieve-large-root-verification-20261110/`。命令参数见 `large_prepare.py --help`。
