# 二叉树大规模准备检查

`large_prepare.py` 检查深度 14、18 的 R1/R2。每轮沿用原算法：stretch 树、跨所有桶存活的 long-lived 树，以及各深度的批量建树、完整遍历和释放。这里不计时或校准 R。

Source 每个逻辑节点持有真实 Vec，包括叶子的空 Vec；C++ 使用手写 C++20、24 字节 Node 和两个 unique_ptr。保留正常 O2 优化，不换算法、不共享叶子。

在编译器根目录运行：

```sh
../toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12 \
  -I -B tests/performance/binarytrees/large_prepare.py \
  --out /tmp/new-binarytrees-check
```

输出目录必须不存在。默认使用本阶段冻结的工具和独立 oracle；替换工具后要更新冻结值并重新检查，不能套用旧 IR 结论。

检查包括：

- 四组 Source/C++ O0/O2 正常程序，以及两端 O2 的四种错误 expected，分别要求退出 1/2/3/4。
- LLVM verify、Source 正式 CLI/helper 对象相同、正式 Native ELF 无 INTERP/NEEDED/强未定义符号；C++ IR 使用正常编译选项。
- 实际外层 phi、回边、退出条件、区域内创建/核值/清理及 ScalarEvolution 上限；实际 R1 和减轮/提前退出 IR 必须拒绝。Source 的带条件回边还结合相同正式对象的实际两轮观察。
- 观察器记录真实创建、叶子/内部节点、释放、活动/峰值及 long-lived 存活；地址表允许释放后复用，拒绝重复 live 地址和未知 free。每轮/阶段只输出固定大小汇总，不保存逐节点日志。

`results.json`、`commands.jsonl`、`logs/`、`round-controls/`、`observers/` 保存退出码、SHA、错误对照及实际汇总。任何必检项失败时，入口返回非零。

观察程序使用宿主库，仅用于检查，不用于正式测速。R1/R2 判断只覆盖本次实际深度和生成文件；校准后的其他 R 必须另查。没有正式性能比值或达标结论。
