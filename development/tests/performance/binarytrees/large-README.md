# 二叉树大规模准备检查

`large_prepare.py` 检查深度 14 和 18 下的 R1/R2。每轮沿用原算法：先建 stretch 树，再建跨所有桶存活的 long-lived 树，随后对各深度批量建树、完整遍历并释放。本脚本不做计时，也不校准 R。

Source 实现中，每个逻辑节点都持有一个真实的 Vec（叶子节点持有空 Vec）；C++ 实现采用手写 C++20、24 字节 Node 和两个 unique_ptr。两端都保留常规 O2 优化，不换算法，也不共享叶子节点。

在编译器根目录运行：

```sh
../toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12 \
  -I -B tests/performance/binarytrees/large_prepare.py \
  --out /tmp/new-binarytrees-check
```

输出目录必须不存在。默认使用本阶段冻结的工具和独立 oracle；替换工具后要更新冻结值并重新检查，不能套用旧 IR 结论。

检查包括：

- 四组 Source 与 C++ 的 O0/O2 正常程序，以及两端在 O2 下的四种错误 expected 对照，分别要求以退出码 1/2/3/4 退出。
- LLVM verify 检查、Source 正式 CLI 与 helper 生成的目标文件逐字节一致、正式 Native ELF 不含 INTERP/NEEDED 及强未定义符号；C++ IR 按常规编译选项生成。
- 实际外层 phi、回边、退出条件、区域内的创建/核值/清理以及 ScalarEvolution 上限；实际的 R1 以及减轮、提前退出 IR 必须被拒绝。对于 Source 的带条件回边，还会结合同一正式目标文件的实际两轮观察。
- 观察器记录实际的节点创建、叶子/内部节点数、释放、活动/峰值计数和 long-lived 存活情况；地址表允许释放后复用地址，并拒绝重复的 live 地址和未知 free。每轮及各阶段只输出固定大小的汇总，不保存逐节点日志。

`results.json`、`commands.jsonl`、`logs/`、`round-controls/`、`observers/` 保存退出码、SHA、错误对照及实际汇总。任何必检项失败时，入口返回非零。

观察程序链接宿主库，仅用于检查，不用于正式测速。R1/R2 的验证结论只覆盖本次实际深度和生成文件；校准后的其他 R 值必须另行检查。本检查不产生正式性能比值或达标结论。
