# 本轮扩展

范围为网络应用库、高层并发、语言与容器扩展、内存管理、外部库调用和基础库细节。只支持 Linux x86_64。完整任务和验收要求保存在 [extensions.json](../../contracts/extensions.json)，没有把第一批可独立实现的模块当成全部完成。

用户已确认：

- 核心生成程序继续独立；明确选择外部库或 TLS 时，可以链接该库及必要依赖。
- 闭包默认捕获创建时的逻辑值。Vec 等引用对象仍共享对象；只有显式 move 才转移所有权，不自动借用外层变量。
- 增加 Weak 和纯 Woozy 强引用循环的自动回收。Python/C 对象循环由对应宿主管理。
- 取消固定的三个 bridge worker 上限。独立任务可以同时分配多个 worker；共享文件每次仍由一人修改，并按服务实际配额执行。

HTTP/1.1、Unicode 和时区已实现并检查。共享类型、所有权规则和 Native/Python 接口由 Root 指定单一写入负责人；运行库、独立检查和文档继续分给不同 worker。

外部库模式必须正确使用宿主的程序启动和线程 TLS。不能仅在现有自有 `_start` 后追加 `-lc`，然后宣称已能安全调用 libc/OpenSSL。独立模式的 ELF 检查继续保留。

自举分开回答两件事：现在是否能开始用 Woozy 写编译器；何时能证明由 Woozy 编译器编译出下一代同一编译器。后者需要实际 stage0、stage1、stage2 和测试结果，不用一个 tokenizer 或调用旧 C++ 编译器的包装程序代替。

## 2026-11-12 已检查的结果

第一批整合后的 CTest 为 122/122，通过日志保存在工作目录 `checks/extensions-20261111/full-ctest.log`。HTTP/1.1、Unicode 18、命名时区、超时同步、基础 Channel，以及声明式 Python 属性/索引/切片已运行检查。

hosted 启动、线程/fork 和 OpenSSL TLS 的底层接口也有独立 O0/O2 测试。源码级同步 C 调用已修正并整合，完整 CTest 通过 126/126（223.75 秒），包括 43 条 C FFI 检查和伪造 HIR/IR 拒绝检查，日志为 `checks/extensions-20261111/full-ctest-126-cabi.log`。保留型回调仍未实现，因此 FFI01 未标为完成。TLS/HTTPS 公共接口仍在开发。

线程池和 Channel 超时取消已经完成独立检查，整合后五项相关 CTest 通过，随后完整 CTest 通过 124/124（228.11 秒），日志为 `checks/extensions-20261111/full-ctest-124-fixedenv.log`。线程池使用无捕获函数与明确参数，不依赖捕获闭包；需要明确调用 shutdown。

GC/Weak 配合独立检查通过；GC 线程暂停的两处错误处理问题已修复，独立复测 34/34 通过。Python 后端自动循环回收正在修正三处独立发现的问题；并发文档已整合。其余 worker 分别实现修改区间、Native 自动回收压力测试、TLS/HTTPS 公共库、C 调用指南和 Native Slice 源接口。共享 Core 目前由 Slice worker 单独修改，Python emitter/runtime 另有负责人。Weak 源接口、Native 自动回收，以及 Slice 的 Python 接入尚未完成。闭包的捕获绑定是否只读、移动捕获写法，以及 Weak 的目标类型等待用户答复；动态 trait、用户 Iterator 和 async/await 仍未实现。完整范围继续以任务表为准。

循环回收接入的修改区间、Weak 失效、外部析构与线程/fork 顺序见 [MEMORY_INTEGRATION.md](MEMORY_INTEGRATION.md)。这些是待实现要求，不能据此将 MEM02 标为完成。
