# 固定八算法的采样代码

`formal_sampling.py` 供各算法的准备程序调用，固定16个算法/规模，不接受缩小规模或减少样本。

- `calibrate(build_cpp, run)` 从R=1倍增，每个R保留3次完整进程记录，选择首个中位数≥500ms且最小值≥250ms的R；另构建、记录2R。`build_cpp(R)`返回该R独立保存的C++程序，`run(path)`返回`run_native.command`格式的实际执行记录。
- `sample_pair(source, cpp, run, calibration)` 核对校准记录和C++文件哈希，再做两端各2次热身和9对交替样本。失败/超时立即保留，禁止丢弃或补跑该样本；二进制变化、校准不完整、实际C++样本低于时长下限均不能判通过。
- `summarize_grid(cases)` 从原始样本重新计算每项比值，检查16项唯一且完整。单项超过2倍就失败，不计算抵消超标项的总平均值。每项格式是`{"algorithm": "heap", "n": 65536, "result": sample_pair返回值}`。

调用方先完成核心要求、核对实际R/2R工作、冻结输入/工具/Source/IR/对象/ELF，再停止本任务的并行构建，并在子进程固定同CPU和32MiB软栈执行。校准和采样的完整返回值应写入新目录，保留此前所有失败。

当前模块尚未接入八算法生成程序；正式校准和采样未启动。13项自检使用合成时间，只检查选R、采样顺序、文件变化和失败处理，不是Source/C++性能结果：

```sh
python3.12 -B -m unittest discover -s tests/performance -p test_formal_sampling.py -v
```
