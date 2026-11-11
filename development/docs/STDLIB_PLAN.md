# 基础库实现与检查（2026-11-11）

功能、完整检查和文档示例已完成。本轮之前的提交为 711fa3c。

| 任务 | 已实现内容 | 实际检查 |
| --- | --- | --- |
| ST01 | 不可变 UTF-8 String、码点长度/索引/切片/遍历、拼接、比较、字节转换 | Native O0/O2、Python 3.12；非法 UTF-8 与所有权检查 |
| ST02 | String/Vec 的 HIR、NativeIR、LLVM/Python，Option/Result | 真实源码到二进制、修改 IR 后拒绝、分配释放计数 |
| ST03 | Vec 容量、共享身份、reserve/push/pop/resize/clear | 增长、自别名、元素清理、迭代期间修改拒绝 |
| ST04 | 有类型的文件、目录、链接、权限、管道、时钟和随机系统接口 | 运行库检查及真实 Woozy 程序 |
| ST05 | TCP/UDP、IPv4/IPv6、Unix socket、poll、选项、DNS | 本机真实服务、零长度报文、EPIPE、UDP/TCP DNS 测试服务器 |
| ST06 | print/println、读行、缓冲 I/O、文件/路径、数字转换、argv/env | EINTR/短写、EAGAIN 后恢复、目录复制拒绝、非 UTF-8 名称和参数 |
| ST07 | Hash/Map/Set、JSON、数学、随机、UTC 日期时间 | 5000 元素 Map、强制碰撞、JSON 独立参考检查、IEEE 特例 |
| ST08 | fork/exec/spawn/wait、重定向、进程组、信号、FIFO、共享内存和进程同步 | Native O0/O2、并发 fork/spawn、锁误用和错误返回 |
| ST09 | 综合程序、旧测试、独立 ELF、文档 | 完整 113+44 项通过；快排/归并排序 65536 元素 O0/O2 正确性通过 |

## 确定的规则

字符串长度、索引和切片按 Unicode 码点，组合符独立计数，不做规范化。Vec、Map、Set 复制后共享内容；跨线程写入由程序员加锁。JSON Number 保存原始十进制文本，按需转为 i64/u64/f64，读取和写回大整数不经过浮点数。

系统接口有类型，名称和参数顺序接近 C，系统错误通过 throws os.Error 返回；没有公开原始 syscall 入口。生成程序不依赖 libc/libstdc++/libm。fork 按 POSIX 规则只保留调用线程，运行库恢复自己的内部锁；业务锁由程序员处理，通常随后 exec。File/Socket 不自动关闭，Writer 不自动 flush；Memory 通过 ARC 管理。

## 实现中修复的问题

- Map 桶逐个创建，避免 vec.make(capacity, vec.empty()) 让各桶共享同一 Vec，重新散列时指数增长；5000 元素和快照数量已测。
- 抛错调用直接作为 switch/if/for 条件时，保留调用失败的借用清理，避免完整表达式清理覆盖它。
- ownership 标记表示清理，不按常量值重新展开；仍检查真实 HIR 来源、函数和类型，保留损坏 IR 的拒绝测试。
- Python 递归类型先建立描述符再填字段，聚合 copy/drop 使用工作栈；256 层 JSON 数组和对象清理完成，1538 个 Vec 均无剩余引用。
- 泛型实例上限从全项目 128 提高至 4096，保留 48 层深度和 200000 个复制节点限制。6 种 Map 组合、重复调用及实例/深度超限均已测。
- 深层 JSON 曾超出 1 MiB 线程栈；现保留 8 MiB 虚拟地址，按使用提交物理页。线程内 JSON O0/O2 和栈保护检查通过。
- Clang O3 曾把 DNS 扫描变为外部 strlen。运行库增加 -ffreestanding -fno-builtin；归档符号检查只允许 main 与弱 observer，另外确认拒绝强 observer 和 strlen。
- 非阻塞读行保留未完成的行，缓冲写入错误后只移除已写字节。复制目录在打开目标前拒绝，避免截断已有文件。
- DNS 完整校验响应后才接收地址，继续查询跨响应 CNAME，拒绝单个响应内及跨响应循环。
- 最后独立审读补上空左侧 path.join 与 time.difference 纳秒借位极值的反例，修复后重新执行完整检查；数学接口文档分别写明 bool、Fraction 与指数参数类型。

## 检查记录

构建目录和详细日志留在本机，不提交到语言仓库。以下位置相对 /home/qin/Work/language-dev-20261103：

- compiler/build/woozy-stdlib-native/final-ctest.log：Clang 22.1.8、LLVM 开启、Release，113/113 通过。
- compiler/build/woozy-stdlib-frontend/final-ctest.log：GCC、LLVM 关闭、Release，44/44 通过。
- compiler/build/woozy-stdlib-native/tests/native/stdlib-results/：11 个程序/后端组合，含真实文件、网络与线程内 JSON，检查 ELF 无 INTERP/NEEDED。
- compiler/build/woozy-stdlib-native/tests/native/collections-results/：5 个程序分别在 Native O0/O2/Python 3.12 运行，另有 38 个合法与 34 个非法 JSON 输入。
- compiler/build/woozy-stdlib-native/tests/native/map-budget-results/ 与 recursive-json-results/：泛型限制和深层 JSON 检查。
- checks/woozy-stdlib-20261111/：最终汇总、74 项第三方源码/许可证核对、9 处独立程序所需修改、19951 个随机浮点位型比较和文档程序检查。
- checks/woozy-stdlib-process-ipc-20261111-v1/：进程/IPC 的运行库、独立 ELF 和源码程序检查。
- checks/woozy-stdlib-sort-20261111/{quick,merge}/prepare.json：65536 元素、1 轮，Woozy/C++ O0/O2、LLVM IR、目标文件一致和错误参考值拒绝。此次没有正式测速。

## 当前限制

系统接口用于 Linux x86_64 Native；纯源代码的 text/vec/option/result/strconv/math/path/hash/map/set/json 同时支持 Python。JSON 深度限制为 256，引用循环不会由 ARC 自动回收。

没有异步语言信号回调、持锁进程死亡恢复或共享内存中的 ARC 指针。fork 后其他线程遗留的引用可能保留至 exec/进程退出，业务锁状态保持原样。日期时间为 UTC，没有时区数据库；网络没有 TLS 或 HTTP。详见 [标准库](../../docs/STDLIB.md)、[进程](../../docs/PROCESSES.md)、[IPC](../../docs/IPC.md)。

## 分工

Pi/Ollama DeepSeek Flash 完成 Vec、基础系统调用、网络、数字转换、数学、DNS 与集合/JSON 的分项工作；原生子代理完成核心编译器、进程/IPC、独立检查和文档。Root 检查实际文件和运行结果，修正遗留问题后合入。每个文件每次只有一个写入负责人，未使用 Claude/Ollama。已完成的 bridge 子会话均关闭，并检查其所属进程已停止。
