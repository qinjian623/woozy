# 固定八算法的采样代码

`formal_sampling.py` 供各算法的准备脚本调用，固定覆盖 16 个算法/规模组合，不允许缩小规模或削减样本数。

- `calibrate(build_cpp, run)` 从 R=1 起倍增，每个 R 保留 3 次完整进程记录，选取首个中位数 ≥ 500ms 且最小值 ≥ 250ms 的 R，同时构建并记录 2R。其中 `build_cpp(R)` 返回该 R 下独立保存的 C++ 程序，`run(path)` 返回 `run_native.command` 格式的实际执行记录。
- `sample_pair(source, cpp, run, calibration)` 先核对校准记录和 C++ 文件哈希，再对两端各做 2 次热身和 9 对交替采样。遇到失败或超时会立即保留记录，禁止丢弃或补跑该样本；二进制发生变化、校准不完整或实际 C++ 样本低于时长下限时，均不得判通过。
- `summarize_grid(cases)` 根据原始样本重新计算每项耗时比，确认 16 项唯一且齐全。只要单项超过 2 倍即判失败，不计算跨项总平均值来抵消超标项。每项输入格式为 `{"algorithm": "heap", "n": 65536, "result": sample_pair返回值}`。

调用方必须先完成核心要求、核对实际 R/2R 工作量，并冻结输入、工具、Source、IR、对象与 ELF，再停掉本任务的并行构建，在子进程中绑定同一 CPU 和 32MiB 软栈后执行。校准和采样的完整返回值须写入新目录，并保留此前所有失败记录。

`formal_sort_programs.SortPrograms` 提供三种排序算法的构建回调：按固定二进制输入生成 Source 和 C++，由 `build_cpp(R)` 保存独立的 O2 对象、链接出的程序、同参数 LLVM IR 及 verify 记录，可直接传给 `calibrate`。它不会运行生成的测速程序。

`formal_algorithm_programs.AlgorithmPrograms` 为其余五种算法提供同类构建回调，只接受 `formal_sampling.GRID` 固定的两组规模：sieve 1000000/10000000、fannkuch 9/10、mandelbrot 512/2048、binarytrees 14/18、matmul 128/512；R 只能取允许范围内的 2 的幂。它直接调用各目录 `large_prepare.py` 现有的生成函数，不重复维护算法、模板、工作量、尺寸或预期值。`render(R)` 会保留 Source、C++、manifest 以及冻结的输入/模板/工具哈希；`build_cpp(R)` 采用与 `SortPrograms` 相同的四条 C++ 命令，保存独立对象、链接出的 ELF、同 flags 无调试 LLVM IR、verify 结果以及全部命令和失败记录。R1 与 R2 各自独立生成，不把 R1 缓存复制冒充 R2；输出固定标记 `actual_work_review=pending` 和 `formal_timing=false`。

本任务的新增检查覆盖十个固定单元的 R1/R2 渲染，并与对应旧 prepare 产出的真实文件逐字节比对（记录哈希、是否相等及差异位置）；同时对每个算法的小规模各构建一次 R4 C++ O2 对象、ELF 与 IR 并执行 verify，不运行算法程序、不编译 Source，也不启动校准采样。检查记录位于 `checks/formal-other-five-program-builders-20261110/results.json`，失败运行保存在同目录的 `failed-run-1/`。R/2R 实际工作量审读仍需后续接入。

`formal_native_programs.build_source(programs, rounds, langc, helper, runtime, llvm_bin, env)` 是上述两种程序构建对象共用的 Source O2 构建入口。它只调用现有的 `programs.render(R)`，并在 `programs.out/r{R}/source-O2` 下新建输出目录；目录已存在时直接拒绝，避免拿 R1 缓存冒充 R2。构建前会通过 `run_native.build_tree_info` 校验 Release CLI 实际嵌入的 helper、runtime、clang 和 ld 与传入路径一致。

构建命令沿用准备脚本的顺序：依次执行 `woozyc check`、`woozyc build --backend native --opt 2`，以及 helper 的 `--out-object/--out-llvm/--out-llvm-before/--out-native`；helper 生成的对象必须与 CLI 产出的 `program.o` 逐字节一致，优化前后 IR 均经 `opt -passes=verify` 校验。Source ELF 经 `common.checked_elf`、`llvm-readelf -l/-d` 和 `llvm-nm -u` 检查，要求无 INTERP、无 NEEDED、无未解析强符号；Mandelbrot 和 matmul 则直接调用各自生成器已有的 `float_policy`/`float_flags` 检查实际 Source IR。`build.json` 记录源码、manifest、对象、ELF、Native IR、LLVM、全部命令与失败记录，以及 langc、helper、runtime、LLVM 工具和构建元数据的前后 SHA，并固定写入 `actual_work_review=pending` 与 `formal_timing=false`。该入口不执行生成的程序，也不能替代核心完成、最终冻结、R/2R 工作量审读或闲置机器等前提。

API 检查仅使用旧冻结的 `build/native-perf-7917`（7917f70，即派单所称 391/7917 旧版本）对 heap 65536/R1 构建了一次，未运行程序，也未编译其余 15 格或百万/千万规模 AST，不构成最终产品或性能结论。检查记录在 `checks/formal-native-program-artifacts-20261110/results.json`。

正式校准和采样尚未启动。目前有 13 项使用合成时间的采样自检、4 项排序固定输入生成检查、4 项五算法固定输入生成检查和 6 项 Source 构建自检，这些都不是 Source/C++ 的性能结果：

```sh
python3.12 -B -m unittest discover -s tests/performance -p 'test_formal*.py' -v
```
