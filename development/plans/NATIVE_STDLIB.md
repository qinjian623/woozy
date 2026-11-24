# Native 标准库扩展

这轮实现和验收面向 Linux x86_64 Native。对比基于 Woozy `d3cf8f6`、C++23 最终工作草案和 Python 3.14 标准库。Woozy 已有的接口见[标准库手册](../../docs/STDLIB.md)。[C++23 草案 N4950](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/n4950.pdf) · [Python 3.14 标准库](https://docs.python.org/3.14/library/)

## C++23 和 Python 3.14

C++23 的标准库包括容器和视图、迭代器、算法和 ranges、数学和随机数、字符串与格式化、正则、文件 I/O、文件系统、时钟、线程与同步。文件和文本操作有多个历史接口。CSV、JSON、socket 和 HTTP 通常由平台接口或其他库提供。 [C++23 草案 N4950](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/n4950.pdf)

Python 的内置容器配合 `collections`、`heapq` 和 `itertools`。`str`、`re` 和 `unicodedata` 处理文本，`pathlib` 和 `shutil` 包装常见文件操作。库里还有 CSV、JSON、压缩、SQLite、socket 和 HTTP，以及线程、进程和 `asyncio.TaskGroup`。Woozy 参考这些常用操作的调用方式。HTTP 服务器另按 Woozy 的超时、缓冲和关闭规则测试，Python 文档只推荐 `http.server` 用于非生产用途。 [Python 3.14 标准库](https://docs.python.org/3.14/library/) · [Python HTTP 服务器](https://docs.python.org/3.14/library/http.server.html) · [Python 任务组](https://docs.python.org/3.14/library/asyncio-task.html#task-groups)

## 七块差距

| 模块 | Woozy 已有 | 这轮要补 |
| --- | --- | --- |
| 容器 | Vec、Slice、哈希 Map/Set、Option/Result | 双端队列、优先队列、常用 Vec 修改操作。与 C++23 相比还缺有序 Map/Set，与 Python 相比还缺按插入顺序遍历的字典保证。 |
| 算法 | 稳定排序、有序性检查、二分搜索 | 通用 map、filter、fold、find、any/all、最值及常用排序辅助操作。C++23 的 ranges 和并行算法范围更大，Python 还有 `itertools`。 |
| 文本 | UTF-8、码点、字素、规范化、case fold、字符串查找拆分和替换 | Unicode 大小写转换、常用格式化及正则捕获、分割、替换。现有 Unicode 处理比 C++23 的字符串容器更接近 Python。 |
| 文件系统 | 读写、单文件复制、列目录、建目录、路径拼接 | 临时文件、递归遍历、整目录复制和删除、安全替换文件。递归修改使用目录描述符和相对名称，不跟随后代符号链接。核对打开的目录身份，并写明并发改名时 Linux 所能保证的行为。 |
| CSV、JSON | JSON 字符串解析、输出、字段查找及保留数字原文 | CSV 逐记录读写，正确处理引号、换行和坏记录。JSON 补格式化输出及文件接口，保持数字精度和已有解析规则。 |
| 网络服务器 | TCP/UDP、DNS、HTTP/1.1 服务器、可选 OpenSSL TLS/HTTPS | handler 可等待 I/O，正文可流式接收和发送，慢连接受超时和缓冲上限约束。HTTP/2、跨线程多执行器和可选 TLS 接入分别评估。 |
| 并发 | Native 线程、锁、原子、Channel、线程池、进程和 async/await | 一组子任务一起等待、失败时取消其他任务、退出前清理；简化并发任务的超时和等待。 |

[Python CSV](https://docs.python.org/3.14/library/csv.html) · [文件和目录操作](https://docs.python.org/3.14/library/shutil.html) · [异步任务组](https://docs.python.org/3.14/library/asyncio-task.html#task-groups) · [HTTP 服务器](https://docs.python.org/3.14/library/http.server.html)

## 接口规则

- 同类操作使用相同名称和参数顺序。常用调用提供短接口，需要限制或其他选项时再用配置版本。容器查不到值或读到文件结尾用 `Option`，读写或格式出错用有类型的 `throws`。
- 容器的复制、共享、迭代顺序和修改限制写进文档。`Vec[bool]` 按普通 Vec 使用。新切片或迭代器说明如何保留源对象以及修改后还能否继续使用，避免把悬空迭代器带到调用者代码里。[WG21 N1185](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/1999/n1185.pdf) · [WG21 P2017R1](https://open-std.org/jtc1/sc22/wg21/docs/papers/2020/p2017r1.html)
- 普通算法使用具体回调和少量泛型。写明就地修改、结果是否分配新容器、是否短路，以及队列和堆的复杂度。库负责关闭自己打开的文件，调用者取得的文件或 socket 用 `defer` 明确关闭。
- 文本以 Unicode 码点为索引，二进制数据用 `Vec[u8]`。CSV 单元格返回字符串，JSON 数字保留十进制原文。网络和格式解析写明缓冲、长度或深度限制。TLS 继续由应用明确选择外部依赖。
- 一条格式化消息由一个模板表达。输出仍用 `std/io`，文件写入沿用同一组错误和缓冲规则。C++ `std::print` 提案列举了逐段输出导致的消息交错、编码差异和临时字符串开销。Woozy 的模板与输出接口应沿用明确的 UTF-8 与资源规则。[WG21 P2093R10](https://open-std.org/JTC1/SC22/WG21/docs/papers/2021/p2093r10.html)
- 每块在 Linux Native O0 和 O2 运行正常、空输入、失败和资源清理用例。macOS 实现和验证留到后续。

第一批并行做 CSV、双端队列与堆、通用算法、临时文件与递归遍历。接口审查同步进行，先处理名称、资源归属和错误规则，再合入。流式 HTTP 和任务组共用取消与清理规则，确定后再接入。

AGY Neon xhigh 已完成首轮接口审查。建议与处理情况见[审查记录](../reports/NATIVE_STDLIB_API_REVIEW.md)。

运行库迁移、标准库 UTF-8 扫描和自举验证见 [Linux Native 运行库的 Wz 迁移](NATIVE_RUNTIME_WZ.md)。
