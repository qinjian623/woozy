# 与 PLB C++ 页的本机比较

来源是 [Programming Language Benchmarks 的 C++ 页](https://programming-language-benchmarks.vercel.app/cpp)。该页正文列出五类测试、十组输入；底部的十八个问题是全站导航。本次补充正文这五类，原有排序及十六组性能记录保留。

| 测试 | 网站输入 | 实际计算 |
| --- | --- | --- |
| Hello World | `QwQ` | 读取参数并输出 `Hello world QwQ!`，主要测启动与输出 |
| Fannkuch-redux | 10、11 | 遍历全部 n! 个排列，计算带符号翻转次数校验和及最大翻转次数 |
| N-body | 500,000、5,000,000 | 五天体、步长 0.01，每步更新十对相互作用，输出初始和末尾能量 |
| Nsieve | 10、12 | 依次筛选小于 `10000 << n`、`10000 << (n-1)`、`10000 << (n-2)` 的素数 |
| Spectral norm | 2,000、4,000、8,000 | 十轮幂迭代，共四十次矩阵向量乘，不保存完整矩阵 |

源代码在 [tests/performance/plb](../../tests/performance/plb)，脚本为 [run_plb.py](../../tests/performance/run_plb.py)。上游源码固定为 [`ad18b203dd1769724f4eea94fc3ac1e99f6593e0`](https://github.com/hanabi1224/Programming-Language-Benchmarks/tree/ad18b203dd1769724f4eea94fc3ac1e99f6593e0)。`source-manifest.json` 保存下载地址与 SHA-256，`vendor` 保留该页七个 C++ 版本、配置和九个小输入预期输出。算法保留作者注与 Benchmarks Game Revised BSD 许可；其余上游文件保留 MIT 许可。

## 两组结果怎么读

主要比较使用相同算法的 Woozy 和手写 C++：同一台机器、同一 CPU、O2、通用 x86-64，关闭 fast-math、FMA 和 LTO。Fannkuch 使用同一标量排列顺序；N-body 使用相同的 35 个浮点数布局和求值顺序；Nsieve 两侧都按位打包、复用存储；谱范数使用相同分母递推和三个工作向量。输入从命令行传入，计算结果实际输出，编译时不填入答案。

网站原版 C++ 另表记录，使用上游配置的 O3、`-march=broadwell`、OpenMP 和 pthread，在本机四个固定 CPU 上运行。原版 N-body `1.cpp` 用模板展开，`0-i.cpp` 用 SIMD 与近似倒平方根；Fannkuch 用 SIMD 和 OpenMP；谱范数两个版本都用 SIMD 和 OpenMP。因此，这组与主表的差值还包含算法表达、指令和线程数量的差别。

网页显示的谱范数 `8-m.cpp` 链接实际指向 `8.cpp`。该源码使用 AVX 和 OpenMP，并调用 `get_nprocs()` 请求主机总线程数。测试设置 `OMP_THREAD_LIMIT=4`、`OMP_NUM_THREADS=4`、`OMP_DYNAMIC=FALSE`，避免本机按 16 线程分块时四元素向量写入跨越分块。LLVM OpenMP 的“改用四线程”提示保留在记录中。

网页的公开成绩来自 2025-08-01、四核 AMD EPYC 7763、GCC 11/Clang 14。这里重新编译并测量，不拿网页毫秒数与本机 Woozy 直接相除。

## 检查与计时

准备阶段分别构建 O0/O2 并运行小输入；O2 运行全部十组网站输入。整数结果完全比较；浮点结果必须有限，绝对误差不超过 `1e-9`。小输入与上游预期输出核对，Nsieve 另用从 p² 开始的 Python 埃氏筛生成预期值；大输入同时与上游 C++ 本机运行结果比较。Fannkuch 检查校验和及最大翻转次数，不只看退出状态。

准备阶段还导出并检查优化前后 LLVM IR，核对 CLI 与 helper 生成的目标文件相同，检查 Woozy ELF 没有 INTERP、NEEDED 或强未定义符号。程序、输入、工具和脚本随后按 SHA-256 固定。现有输出目录与已采样记录禁止覆盖。

计算任务从 C++ 的批次 R=1 倍增，每个 R 实测三次，选首个中位数至少 500ms、最小值至少 250ms 的批次。一个批次启动 R 次完整程序，每次都使用网站的原输入并检查输出；双方保留 2R 对照，再各热身两批、做九对交替计时。表中耗时除以 R，表示一次完整程序的中位耗时，包含启动和输出，不是只测内核函数。Hello World 固定 R=1，单独看启动耗时；上游原版各热身两次、测九次。

按用户要求，CPU 有其他负载时也直接测试，记录负载，不等待空闲、不丢弃或替换已测样本。本次新增比较不改变旧十六组的“两倍”目标，也不计算跨算法平均值。

从仓库根目录运行，`--out` 必须是新的目录：

```bash
python3 tests/performance/run_plb.py selftest
python3 tests/performance/run_plb.py prepare \
  --cli build/woozy-stdlib-native/woozyc \
  --llvm-bin ../toolchains/llvm-22.1.8/bin --out ../checks/plb-new --cpu 1
# 检查准备结果与算法实现后，使用同一目录采样。
python3 tests/performance/run_plb.py sample \
  --cli build/woozy-stdlib-native/woozyc \
  --llvm-bin ../toolchains/llvm-22.1.8/bin --out ../checks/plb-new --cpu 1
```

脚本生成 `results.json`（全部命令、输出、校准、逐次耗时、文件哈希）和 `results.csv`（主表摘要）。脚本需要 Linux x86-64、支持 Broadwell 指令的 CPU、Clang/LLVM/LLD 和 LLVM OpenMP。当前实现没有为其他主机自动改写上游指令集。

## 2026-11-11 本机结果

机器为 AMD Ryzen 7 8745HS、Linux x86-64、Clang/LLVM 22.1.8。编译器源码为 `0c3b332`，本次只增加基准程序、测试脚本和文档。准备阶段 136 条命令通过，包含十组主对照及十五组上游原版输出检查；另有 24 项非法参数检查和输出脚本自测通过。

下表单位为毫秒，数值为九批计时的中位数除以 R。比值为 Woozy / 同算法 C++，越小表示 Woozy 耗时越少。

| 测试 | 输入 | R | C++ O2 ms | Woozy O2 ms | 比值 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Hello World | QwQ | 1 | 3.306 | 1.869 | 0.565 |
| Fannkuch | 10 | 4 | 173.316 | 164.695 | 0.950 |
| Fannkuch | 11 | 1 | 1976.048 | 1920.163 | 0.972 |
| N-body | 500000 | 32 | 30.764 | 80.542 | 2.618 |
| N-body | 5000000 | 2 | 260.298 | 746.683 | 2.869 |
| Nsieve | 10 | 16 | 50.332 | 57.569 | 1.144 |
| Nsieve | 12 | 4 | 224.313 | 256.208 | 1.142 |
| Spectral norm | 2000 | 4 | 224.061 | 224.109 | 1.000 |
| Spectral norm | 4000 | 1 | 880.022 | 874.889 | 0.994 |
| Spectral norm | 8000 | 1 | 3443.598 | 3443.348 | 1.000 |

Hello World 的毫秒数包含 Python 启动子进程的测量开销、系统启动程序和输出，不能用来判断计算速度。其余主表保留全部九对样本，没有移除慢样本。

### 网站原版 C++ 的本机成绩

这组保留上游源码及 O3/Broadwell 选项，固定四个可用 CPU；使用并行版本的程序限制为四线程。Woozy 主表为一个 CPU。此表展示优化过的实际 C++ 实现，不能把主表的“同算法比值”套在这里。

| 测试 | 输入 | 原版源文件 | C++ ms |
| --- | ---: | --- | ---: |
| Hello World | QwQ | `helloworld/1.cpp` | 2.144 |
| Fannkuch | 10 | `fannkuch-redux/1-im.cpp` | 29.099 |
| Fannkuch | 11 | `fannkuch-redux/1-im.cpp` | 292.298 |
| N-body | 500000 | `nbody/1.cpp` | 16.565 |
| N-body | 5000000 | `nbody/1.cpp` | 143.552 |
| N-body | 500000 | `nbody/0-i.cpp` | 13.719 |
| N-body | 5000000 | `nbody/0-i.cpp` | 116.217 |
| Nsieve | 10 | `nsieve/1.cpp` | 53.640 |
| Nsieve | 12 | `nsieve/1.cpp` | 211.154 |
| Spectral norm | 2000 | `spectral-norm/8.cpp` | 19.621 |
| Spectral norm | 4000 | `spectral-norm/8.cpp` | 66.658 |
| Spectral norm | 8000 | `spectral-norm/8.cpp` | 251.956 |
| Spectral norm | 2000 | `spectral-norm/6-im.cpp` | 36.451 |
| Spectral norm | 4000 | `spectral-norm/6-im.cpp` | 127.141 |
| Spectral norm | 8000 | `spectral-norm/6-im.cpp` | 480.087 |

### N-body 的差距

N-body 同算法对照中，Woozy 耗时为 C++ 的 2.62～2.87 倍。冻结 IR 与二进制中已确认：

- Woozy 每对天体调用自己的 `lang_math_sqrt_v1`，运行库静态链接进程序；C++ 使用 LLVM sqrt 内建函数，二进制中为 `sqrtsd`。这个调用不代表依赖 libc 或 libstdc++。
- Woozy 优化后每步仍有 130 个数组检查分支，sqrt 调用后还需重读向量大小和数据指针。C++ 直接数组访问没有这些检查。
- C++ 自动把部分双精度运算向量化，Woozy 这份 IR 没有相应的向量浮点运算。双方的天体配对循环均保留，不能归因于 C++ 已展开所有配对。

这些是已观察到的代码差别，没有逐项开关实验，不能把总差距分配成各项耗时比例。本次保留结果，没有为通过旧“两倍”目标而修改程序或编译器。

### 原始时间与检查记录

可随仓库读取的 [逐批和逐次进程时间 JSON](../performance/plb-20261111.json) 包含全部 220 个主对照批次（1,452 次完整程序执行）、165 次上游采样、校准、2R 对照、源码及工具哈希。主对照中 180 批、上游中 135 次为正式样本，其余为热身。

本地完整记录在 `checks/plb-20261111/`：`preparation-final.json`、`run-final/results.json`、`run-final/results.csv`、两份实现审查、`timing-review.json`、`invalid-inputs.json`、`openmp-thread-preflight.json` 和 `nbody-ir-notes.md`。JSON 摘要保存对应文件哈希，完整命令日志没有上传。首次准备因预期的 OpenMP 四线程提示停止；修正提示检查后重新准备。计时只执行了一轮，没有重测或替换样本。
