# 大规模矩阵乘法检查

`large_prepare.py` 检查 N=128、512，R=1、2。Source 和 C++ 使用同一输入、i-j-k 顺序和 f64 运算；每轮分配、计算、完整核值、释放。这里只准备正确性和重复工作检查，不计时或校准 R。

输入取自开发目录的 `checks/eight-algorithm-large-oracles-20261110/`。脚本检查文件 SHA，再按固定整数公式逐项检查 A、B 和预期结果。保留旧模板及小规模脚本。

每组检查包括：

- Source、C++ 的 O0/O2 构建和真实运行；错误结果必须退出 1。
- 两端每轮完整 C 矩阵的位检查，逐字节对照冻结的 `.f64le`。C++ 辅助程序只替换空 visit；辅助观察程序使用宿主 libc，不用于测速。
- LLVM 验证、Source helper 与正式 CLI 对象相同、正式 Source ELF 无 INTERP/NEEDED/强未定义符号。
- R2 的真实乘加循环或两份展开计算；真实 R1 按预期 R2 检查时必须拒绝。破坏乘法和恒假回边的 LLVM 对照也必须拒绝。

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

输出目录必须不存在。`prepare.json` 保存命令、退出码、SHA、完整观察文件位置及失败原因；输入和工具前后必须相同。

轮次判断支持已核对的四个实例。布尔 phi 必须真正控制回边；只出现 phi 不算重复计算。函数未内联时，负对照修改真实被调函数的乘法，不通过改名冒充移除计算。校准后的正式 R 仍需另查实际工作。

C++ 默认 O2 优化、向量化保持开启；不使用 fast-math、FMA、LTO 或 march=native。合法优化可消除两端不同的分配，初始化循环数不表示机器指令相同。
