# 本轮扩展

本轮只做 Linux x86_64，范围为网络应用库、并发、语言与容器、内存管理、外部调用和基础库。逐项要求见 [extensions.json](../../contracts/extensions.json)，用户规则见 [SEMANTICS.md](../../docs/SEMANTICS.md)。

## 已实现并检查的部分

- HTTP/1.1、TLS/HTTPS；TLS 明确选择 OpenSSL 及其依赖，默认核心程序保持独立。
- 超时同步、读写锁、取消、Channel、线程池、Native async/await 和执行器；线程池仍需明确 shutdown。
- 按值与 move 捕获闭包、绑定方法、动态 trait、用户 Iterator、Slice，以及文档列出的字段和索引 inout。
- Weak、Box 和纯 Woozy 强引用循环回收，包括 Vec、闭包和 Task/frame；外部 Python/C 对象自身的循环仍由宿主管理。
- 有类型的 C 调用、静态函数保留回调，以及声明式 Python 属性、索引和切片。
- Unicode 18、命名时区，以及此前完成的字符串、容器、文件、进程、IPC 和网络接口。

用户 Iterator 对表达式只求值一次，复制迭代器并推进副本；共享对象字段仍共享。next 不 throws，逐项失败用 Result 元素。

主目录 `228cee9` 完整 171/171 CTest 通过（152.93 秒）。后续 `21b8d5b` 的真实 async TCP/UDP 源码和观察器 66/66 通过，另有 4 次计数正反检查；该测试提交没有重跑完整 171 项。

## 捕获式 C 回调

需求规则已由用户确定：带 userdata、注册期间保留捕获值、注销时等待在途调用、不允许 throws。实现说明见 [C_RETAINED_CAPTURING_CALLBACKS.md](../docs/C_RETAINED_CAPTURING_CALLBACKS.md)，用法见 [调用 C 库](../../docs/FFI.md)。

四个 Pi-Ollama DeepSeek Flash worker 分别编写前端、运行库、Native 代码生成和真实 C/Source 测试，并修正了首版发现的外层 SAFE 清理、并发 close 等待和借用退出顺序问题。私有整合版中相关 11/11 项 CTest 通过，13 个 Source 项目在 O0/O2 下运行通过（含 Weak 清理观察和 move-only 注册句柄的函数捕获），正式 CTest 强制要求所有 Source 用例实际执行。

代码已整合到主目录并重新构建成功，完整 **176/176 CTest 通过（191.60 秒）**，FFI01 已完成。新增的 Source CTest 同样强制要求实际执行，不把跳过项计为通过。

## 保留的限制和后续工作

Python 暂不支持 async；文档明确拒绝的 async 外部资源组合和 inout 类型仍不支持。保存捕获闭包的 C 接口目前要求 hosted、userdata、指针注册句柄和完整注销保证；没有这些条件的接口不会自动接入。

普通开发优先 Pi/Pi-Ollama DeepSeek Flash，Claude-Ollama 暂停。独立文件可并行修改，共享编译器文件每次由一个负责人写入，不设固定三个 bridge worker 上限。

自举条件评估已完成；完整自举尚未实现。后续必须实际完成 stage0、stage1、stage2 和相应测试，不以 tokenizer 或旧编译器包装程序代替。跨平台仍在本轮之外。
