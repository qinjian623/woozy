# 大规模矩阵乘法检查

`large_prepare.py` 检查 N=128、512 与 R=1、2 的组合。Source 和 C++ 使用同一份输入、i-j-k 顺序和 f64 运算，每轮完成分配、计算、全量核值和释放。本脚本只做正确性与重复计算检查，不计时，也不校准 R。

输入取自开发目录的 `checks/eight-algorithm-large-oracles-20261110/`。脚本先校验文件 SHA，再按固定整数公式逐项核对 A、B 和预期结果。旧模板与小规模脚本保持不动。

每组检查包括：

- Source 和 C++ 在 O0/O2 下的构建与真实运行；错误结果必须退出 1。
- 对两端每轮完整的 C 矩阵做逐字节比对，对照冻结的 `.f64le` 文件。C++ 辅助程序只替换空 visit；辅助观察程序链接宿主 libc，不用于测速。
- LLVM 验证、Source helper 与正式 CLI 对象逐字节一致，以及正式 Source ELF 无 INTERP/NEEDED/强未定义符号。
- 检查 R2 存在真实的乘加循环或两份展开计算，把真实 R1 按 R2 预期检查时必须拒绝；破坏乘法或恒假回边的 LLVM 反例也必须拒绝。

在编译器根目录运行，例如：

```sh
../toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12 \
  tests/performance/matmul/large_prepare.py \
  --n 128 --rounds 2 --out /tmp/new-matmul-check \
  --compiler-root . --woozyc build/native-perf-release/woozyc \
  --helper build/native-perf-release/native/lang_native_helper \
  --runtime build/native-perf-release/native/liblang_native_runtime.a \
  --llvm-bin ../toolchains/llvm-22.1.8/bin \
  --ld-library-path ../toolchains/libxml2-legacy-2.13.9/usr/lib \
  --python-helper build/native-cli-release/tests/python/lang_python_emit
```

输出目录必须事先不存在。`prepare.json` 记录命令、退出码、SHA、完整观察文件路径及失败原因；运行前后输入与工具的校验值必须一致。

轮次判断只针对已核对的这四个实例。布尔 phi 必须真正控制回边，单纯出现 phi 不算重复计算。函数未内联时，负对照直接修改真实被调函数里的乘法，不靠改名冒充移除计算。校准出正式 R 后仍需另行核查实际计算量。

C++ 保持默认 O2 优化和向量化开启，不使用 fast-math、FMA、LTO 或 march=native。合法优化可能会在两端消除不同的分配，初始化循环数量一致不代表机器指令相同。
