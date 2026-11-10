# 固定八算法的采样代码

`formal_sampling.py` 供各算法的准备程序调用，固定16个算法/规模，不接受缩小规模或减少样本。

- `calibrate(build_cpp, run)` 从R=1倍增，每个R保留3次完整进程记录，选择首个中位数≥500ms且最小值≥250ms的R；另构建、记录2R。`build_cpp(R)`返回该R独立保存的C++程序，`run(path)`返回`run_native.command`格式的实际执行记录。
- `sample_pair(source, cpp, run, calibration)` 核对校准记录和C++文件哈希，再做两端各2次热身和9对交替样本。失败/超时立即保留，禁止丢弃或补跑该样本；二进制变化、校准不完整、实际C++样本低于时长下限均不能判通过。
- `summarize_grid(cases)` 从原始样本重新计算每项比值，检查16项唯一且完整。单项超过2倍就失败，不计算抵消超标项的总平均值。每项格式是`{"algorithm": "heap", "n": 65536, "result": sample_pair返回值}`。

调用方先完成核心要求、核对实际R/2R工作、冻结输入/工具/Source/IR/对象/ELF，再停止本任务的并行构建，并在子进程固定同CPU和32MiB软栈执行。校准和采样的完整返回值应写入新目录，保留此前所有失败。

`formal_sort_programs.SortPrograms` 已提供三种排序的构建回调：用固定二进制输入生成Source/C++，`build_cpp(R)`保存独立的O2对象、由该对象链接的程序、同参数LLVM IR和verify记录，可传给`calibrate`。它不会运行生成的测速程序。

`formal_algorithm_programs.AlgorithmPrograms` 为其余五种算法提供同样的构建回调，只接受 `formal_sampling.GRID` 中固定的两个规模：sieve 1000000/10000000、fannkuch 9/10、mandelbrot 512/2048、binarytrees 14/18、matmul 128/512；R 只接受允许范围内的2的幂。它直接调用各目录 `large_prepare.py` 的现有生成函数，不复制算法、模板、工作量、尺寸或预期值。`render(R)` 保留Source、C++、manifest、冻结输入/模板/工具哈希；`build_cpp(R)` 使用与 `SortPrograms` 相同的四条C++命令，保存独立对象、由该对象链接的ELF、同flags无调试LLVM IR、verify以及全部命令和失败记录。R1/R2分别生成，不把R1缓存复制成R2。输出记录 `actual_work_review=pending` 和 `formal_timing=false`。

本任务的新检查覆盖十个固定单元的R1/R2渲染，并与对应旧prepare真实文件逐字节比较（记录哈希、是否相等及差异位置）；对每个算法的小规模实际构建一次R4 C++ O2对象、ELF、IR并verify，不运行算法程序、不编译Source、不启动校准采样。检查记录在 `checks/formal-other-five-program-builders-20261110/results.json`，失败运行保存在同目录 `failed-run-1/`。R/2R实际工作审读仍需接入。

`formal_native_programs.build_source(programs, rounds, langc, helper, runtime, llvm_bin, env)` 是两种程序构建对象共用的Source O2构建入口。它只调用现有 `programs.render(R)`，在 `programs.out/r{R}/source-O2` 创建新目录；目录已存在即拒绝，不以R1缓存冒充R2。构建前用 `run_native.build_tree_info` 要求Release CLI实际嵌入的helper/runtime/clang/ld与传入路径一致。命令顺序沿用准备脚本：`langc check`、`langc build --backend native --opt 2`、helper的`--out-object/--out-llvm/--out-llvm-before/--out-native`；helper对象必须与CLI `program.o`逐字节一致，前后IR均由`opt -passes=verify`验证。Source ELF用`common.checked_elf`、`llvm-readelf -l/-d`和`llvm-nm -u`检查无INTERP、无NEEDED、无未解析强符号；Mandelbrot/matmul只调用各自生成器已有的`float_policy`/`float_flags`检查实际Source IR。`build.json`保存源码/manifest/对象/ELF/Native IR/LLVM/全部命令/失败及langc、helper、runtime、LLVM工具和构建元数据的前后SHA，并固定`actual_work_review=pending`、`formal_timing=false`。它不执行程序，不替代核心完成、最终冻结、R/2R工作审读或闲置机器要求。

API检查只用旧冻结`build/native-perf-7917`（7917f70；派单所称391/7917旧版本）对heap 65536/R1构建一次，不运行程序，不编译其他15格或百万/千万AST，结果不是最终产品或性能结论。检查记录在 `checks/formal-native-program-artifacts-20261110/results.json`。

正式校准和采样未启动。13项采样自检使用合成时间，4项排序固定输入生成检查，4项五算法固定输入生成检查，另有6项Source构建自检；这些都不是Source/C++性能结果：

```sh
python3.12 -B -m unittest discover -s tests/performance -p 'test_formal*.py' -v
```
