# Native 线程实现与检查

Linux x86_64 的 `std/thread`、`std/sync` 已接入源码、HIR、NativeIR 和 LLVM。语言用法见 [线程与同步](../../docs/CONCURRENCY.md)。

`ThreadHandle`、`MutexHandle`、`CondvarHandle`、`AtomicI64Handle` 是四种不同的拥有类型，LLVM 中用指针表示。公开结构体保存私有状态；用户不能把整数当成地址。只有所属标准模块能调用对应私有操作，spawn 的具体 A、入口函数签名和源类型实参由 HIR 检查器重新检查。Python 和 32 位目标明确拒绝这些类型与操作。

线程参数存储包含函数指针和 A。LLVM 为具体 A 生成入口及 copy/drop 函数，运行库复制并保留参数后调用 clone；入口通过真正的 Woozy 函数签名调用，不猜测 C++ 聚合参数的约定。分配失败 Panic，clone 失败由标准模块转成 `os.Error`。

每个线程有独立的 1 MiB 栈、保护页和 TCB；TCB 的 FS 自指针和栈保护 canary 在 clone 前设置。函数返回并清理参数后，线程发布结果、唤醒 join、释放运行者持有的控制块引用。退出前先屏蔽信号，最后的汇编解除自身栈和 TCB 的映射，再调用 SYS_exit。join 等待函数与语言清理完成，线程尚可能执行这几条运行库退出指令。main、os.exit 和 Panic 仍结束整个进程。

Vec 与运行库句柄的 retain 使用 relaxed CAS；release 使用 release CAS，最后一次释放执行 acquire fence 再清理。LLVM 内联 Vec 计数也使用原子 CAS。迭代计数是原子的；元素访问仍由程序同步。小对象池的共享列表和页计数由内部锁保护。

Mutex 使用 futex 和线程 ID 检查持有者。Condvar 在解锁之前登记栈上的等待记录；通知者持队列锁直到 wake 完成，等待者取得同一队列锁后才离开，避免访问已返回函数的栈。AtomicI64 固定 seq_cst。gettid/futex 意外失败会 Panic，不能静默重试或忽略。

2026-11-11 实际检查：

| 检查 | 结果 |
| --- | --- |
| GCC Release，LLVM OFF，完整 CTest | 43/43 |
| Clang 22.1.8 Release，LLVM ON，完整 CTest | 93/93 |
| Woozy 并发项目 | 9 个项目 × O0/O2，18 次构建、30 次运行 |
| 生成程序依赖 | 无 INTERP、无 NEEDED；使用项目自己的入口和运行库 |
| 线程运行库 | 独立 TLS、canary、保护页、并发 join、自等待、参数清理、提前 drop、实际 mmap 释放、创建失败与 Panic 记录 |
| 锁与计数 | 竞争、发布、条件等待/通知、强 CAS、补码回绕、单次销毁、系统调用失败注入 |

CTest 的 `native.concurrency` 同时运行文档中的两个完整例子，并拒绝 Python 目标。`native.sync_runtime` 使用真实句柄 ARC，以链接包装观察实际 free；没有测试替代实现。`native.arc_concurrency` 保留 release 观察函数可以 retain 的检查。

本次未重测排序性能，未新增捕获闭包、取消、超时等待、递归锁或读写锁。生成程序不计入编译器 ASan/UBSan 的覆盖范围。
