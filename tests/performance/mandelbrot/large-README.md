# 大规模 Mandelbrot 检查

`large_prepare.py` 检查 N=512、2048，R=1、2，保留旧模板中的 f64 运算顺序和每像素最多 50 次更新。每轮重新生成位图、核对全部字节并释放。这里只做测速前检查，不计时或校准 R。

预期位图读取开发目录 `checks/eight-algorithm-large-oracles-20261110/` 的冻结 PBM，检查文件头、大小和 SHA，不重新生成预期结果。两个 N 都能被 8 整除；padding 的错误对照保留在原小规模检查中。

每组检查包括：

- Source、C++ O0/O2 的真实执行；坏 expected、坏 bitmap 必须退出 1。
- Source 辅助观察器取得每轮完整位图，逐字节对照 PBM payload。观察器使用宿主 libc，不用于测速。
- LLVM 验证、Source helper 对象与正式 CLI 对象相同、正式 Source ELF 无 INTERP/NEEDED/强未定义符号。
- R2 真实计算的循环或等量展开；真实 R1 按预期 R2 检查时拒绝。C++ 减轮对照实际生成并运行 R1；Source 减轮对照实际改变轮次控制。两端另有无 kernel 对照。

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

输出目录必须不存在。`prepare.json` 保存命令、退出码、输入与工具 SHA、观察文件和失败原因。辅助 Python 生成仅用于取得符号信息，不执行大位图 Python 程序。

保留默认 C++ O2 优化和向量化，关闭 fast-math、浮点收缩和 LTO，不用 march=native。正式校准后的 R 仍需另查真实计算次数；这四组通过不表示达到 C++ 耗时的两倍以内。
