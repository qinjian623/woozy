# 大规模 Mandelbrot 检查

`large_prepare.py` 检查 N=512、2048 与 R=1、2 的组合，沿用旧模板的 f64 运算顺序和每像素最多 50 次更新。每轮重新生成位图、核对全部字节再释放。本脚本只做测速前检查，不计时，也不校准 R。

预期位图直接读取开发目录 `checks/eight-algorithm-large-oracles-20261110/` 下的冻结 PBM，校验文件头、大小和 SHA，不重新生成预期结果。这两个 N 都能被 8 整除，padding 的错误对照留在原小规模检查里。

每组检查包括：

- Source 和 C++ 在 O0/O2 下的真实执行；篡改 expected 或 bitmap 后必须退出 1。
- 用 Source 辅助观察器获取每轮完整位图，逐字节对照 PBM payload。观察器链接宿主 libc，不用于测速。
- LLVM 验证、Source helper 对象与正式 CLI 对象逐字节一致，以及正式 Source ELF 无 INTERP/NEEDED/强未定义符号。
- 检查 R2 具备真实计算循环或等量展开，并确认把真实 R1 按 R2 预期检查时会拒绝。C++ 减轮对照实际生成并运行 R1，Source 减轮对照实际修改轮次控制；两端还各有无 kernel 对照。

在编译器根目录运行，例如：

```sh
../toolchains/python/cpython-3.12.15-linux-x86_64-gnu/bin/python3.12 \
  tests/performance/mandelbrot/large_prepare.py \
  --n 512 --rounds 2 --out /tmp/new-mandelbrot-check \
  --compiler-root . --woozyc build/native-perf-release/woozyc \
  --helper build/native-perf-release/native/lang_native_helper \
  --runtime build/native-perf-release/native/liblang_native_runtime.a \
  --llvm-bin ../toolchains/llvm-22.1.8/bin \
  --ld-library-path ../toolchains/libxml2-legacy-2.13.9/usr/lib \
  --python-helper build/native-cli-release/tests/python/lang_python_emit
```

输出目录必须事先不存在。`prepare.json` 记录命令、退出码、输入与工具 SHA、观察文件路径和失败原因。辅助 Python 代码生成只用于提取符号信息，不运行大位图的 Python 程序。

C++ 保持默认 O2 优化和向量化，关闭 fast-math、浮点收缩和 LTO，不用 march=native。正式校准出 R 后仍需另行核查实际计算次数；这四组检查通过不代表耗时已在 C++ 的两倍以内。
