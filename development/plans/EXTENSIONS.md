# 本轮扩展

范围为网络应用库、高层并发、语言与容器、内存管理、外部调用和基础库，只做 Linux x86_64。完整要求见 [extensions.json](../../contracts/extensions.json)，源码的用户规则见 [SEMANTICS.md](../../docs/SEMANTICS.md)。

## 已实现

HTTP/1.1、超时同步、读写锁、取消、Channel、线程池、Unicode 18、命名时区、声明式 Python 属性/索引/切片已经检查通过。线程池须明确调用 shutdown。

有类型的同步 C 调用支持标量、指针/缓冲、自然 C record、资源以及无捕获同步回调。只有明确声明的 C 接口按 C ABI 调用；普通 Woozy 值和内部函数 ABI 不属于稳定 C/C++ ABI。保留型回调尚未实现，FFI01 保持未完成。

Python 已自动回收纯 Woozy 强引用循环，包括经 Slice 与 Box 字段持有的边。运行时测试与真实源码测试禁用宿主 GC；不声称回收外部 Python/C 对象自身的循环。

## 2026-11-12 整合

- Native Source Slice 已提交。Python Source Slice 修正已通过独立 84 条源码命令、13 条运行库测试和拥有值/迭代检查，现已合入主目录并重建。视图持有 owner 并保护存储；Vec 切片仍复制，Slice 切片产生子视图。更广的 inout 尚未完成。
- Box 已合入 `stdlib/box`，只有 new/get/set 和私有 Vec cell。修正测试目录后，主目录 Native O0/O2、Python 和四线程检查共 59 条通过。Native 强循环暂不会自动回收。
- TLS/HTTPS 公共库已合入主目录，55 项实际测试通过，含证书、主机名、过期、IP/SNI、非阻塞进度、部分写入和独立对端。Root 修正的三处错误为重复关闭 fd、失败清理等待对端、无效 transport token 被当作地址。默认核心仍独立，选择 TLS 时明确链接 OpenSSL 及必要依赖。
- 整合后的完整 CTest 已通过 134/134（296.35 秒），包括新增 Python Slice、Box 和 TLS/HTTPS 检查；日志为 `checks/extensions-20261111/full-ctest-134-slice-box-tls.log`。

## 正在做和未实现的部分

Weak 的真实 Vec/共享句柄 slot、最后释放和 fork 已合入，完整 CTest 136/136（259.77 秒）通过；加入共用 HIR 字段后，完整 CTest 137/137（257.95 秒）也通过。Root 修正了测试观察器的原子计数和释放完成记账；独立 4×20000 精确计数及七种配置均通过。Source Weak、捕获闭包与绑定方法、动态 trait、自定义 Iterator、更广的 inout、async/await 和保留型 C 回调尚未完成。

Native GC、线程暂停、修改区间、压力控制器及外部回调恢复已在独立测试中通过，尚未接入真实源码自动回收。源码压力目标继续如实报告未实现。接入顺序见 [MEMORY_INTEGRATION.md](MEMORY_INTEGRATION.md)，真实对象的 Weak 修改点见 [WEAK_HANDLE_SITES.md](WEAK_HANDLE_SITES.md)。

闭包实现遵守用户确定的按值捕获、只读捕获绑定、move func 和每次复制 move-Self 接收者规则。函数表示和捕获身份方案已经修正并审查，共用 HIR 字段已接入；新字段尚未有真实源码/所有权验证时仍明确拒绝。前端/Sema/所有权/IR 已分给独立 Flash worker；Native、Python 在前端验证后分别接入。另一 worker 接入 GC 生命周期、线程/阻塞与 fork；24 个独立源码用例已返回待审，未宣称运行通过。

## 分工与自举

独立目录同时安排多个 worker，不设固定三人上限；共享 AST/HIR、所有权和后端文件每次只有一个写入负责人。普通开发使用 Pi/Pi-Ollama DeepSeek Flash，独立审查与整合另分工。Claude-Ollama 暂停使用。

现在可以开始用 Woozy 写编译器。完整自举需要实际 stage0、stage1、stage2 编译与测试，不以 tokenizer 或调用旧 C++ 编译器的包装程序代替。跨平台不在本轮。
