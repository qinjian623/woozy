# 本轮扩展

范围为网络应用库、高层并发、语言与容器扩展、内存管理、外部库调用和基础库细节。只支持 Linux x86_64。完整任务和验收要求保存在 [extensions.json](../../contracts/extensions.json)，没有把第一批可独立实现的模块当成全部完成。

用户已确认：

- 核心生成程序继续独立；明确选择外部库或 TLS 时，可以链接该库及必要依赖。
- 闭包默认捕获创建时的逻辑值。Vec 等引用对象仍共享对象；只有显式 move 才转移所有权，不自动借用外层变量。
- 增加 Weak 和纯 Woozy 强引用循环的自动回收。Python/C 对象循环由对应宿主管理。
- 取消固定的三个 bridge worker 上限。独立任务可以同时分配多个 worker；共享文件每次仍由一人修改，并按服务实际配额执行。

先实施 HTTP/1.1、Unicode 和时区。这三项可在当前编译器上独立检查。AGY 同时审查闭包、Weak/循环回收、可变借用、C ABI/启动方式和 async 的相互影响；共享类型、所有权规则和 Native/Python 接口由 Root 统一修改，不让多个实现者各自定义。

外部库模式必须正确使用宿主的程序启动和线程 TLS。不能仅在现有自有 `_start` 后追加 `-lc`，然后宣称已能安全调用 libc/OpenSSL。独立模式的 ELF 检查继续保留。

自举分开回答两件事：现在是否能开始用 Woozy 写编译器；何时能证明由 Woozy 编译器编译出下一代同一编译器。后者需要实际 stage0、stage1、stage2 和测试结果，不用一个 tokenizer 或调用旧 C++ 编译器的包装程序代替。

## 2026-11-12 已检查的结果

整合后的 CTest 为 122/122，通过日志保存在工作目录 `checks/extensions-20261111/full-ctest.log`。HTTP/1.1、Unicode 18、命名时区、超时同步、基础 Channel，以及声明式 Python 属性/索引/切片已运行检查。

hosted 启动、线程/fork 和 OpenSSL TLS 的底层接口也有独立 O0/O2 测试。源码级 C ABI、TLS/HTTPS 公共接口仍在开发。Weak、Slice 和循环回收的低层部分不能代替源语言接入或自动回收完成。

C ABI、Channel 超时取消、线程池、Weak、Slice 和循环回收按文件分给独立 worker。闭包的捕获绑定是否只读、移动捕获写法等待用户答复；动态 trait、用户 Iterator 和 async/await 仍未实现。完整范围继续以任务表为准。
