# Woozy

Woozy 是一门静态类型编程语言，支持局部类型推断、结构体、枚举、泛型与 trait 约束。源文件使用 `.wz` 后缀。

```wz
package main;
import "std/io" as io;
import "std/os" as os;

func main() throws os.Error {
    io.println("Hello, world!");
}
```

## 学习 Woozy

- [语言教程](docs/TUTORIAL.md)：从一段程序开始，逐步学习类型、函数、容器、模式匹配、错误处理、模块和泛型。
- [语言快速入门](docs/QUICKSTART.md)：面向有编程经验的读者，集中介绍写法和容易混淆的规则。
- [语言规则](docs/SEMANTICS.md)：名称、求值顺序、数值、复制、所有权、错误和清理的详细定义。
- [词法与语法](baseline/LEXICAL_SYNTAX.md)：关键字、字面量、表达式和声明形式。
- [标准库](docs/STDLIB.md)：各模块的常用接口、完整示例、错误和资源关闭规则。
- [系统接口](docs/OS.md)：标准输出、文件读写、目录和进程退出。
- [线程与同步](docs/CONCURRENCY.md)：线程参数、等待、共享数据、锁、条件变量和原子整数。
- [进程与信号](docs/PROCESSES.md)：启动程序、重定向、等待、参数环境、信号与 fork 规则。
- [共享内存与 IPC](docs/IPC.md)：共享映射、跨进程互斥锁、条件变量和信号量。
- [示例](examples/)：函数、数值、错误处理和参数传递；另有可运行的 [TCP 聊天 server/client](examples/chat/README.md)。

## 主要规则

类型写在名字后面，如 `count i32`。局部绑定可以推断类型，函数参数必须声明类型。简单语句以分号结束。

`let` 不允许重新绑定或修改值字段，`var` 允许。struct/enum 按字段复制；Vec 赋值共享容器，切片建立新 Vec，引用元素仍可共享。

`string` 是不可变 UTF-8 值。`text.len`、索引和切片按 Unicode 码点计算，索引得到 `u32`；`+` 建立新字符串，`==` 和排序按字节内容比较，不做 Unicode 正规化。`var` 整体绑定可以用 `+=`，字段、索引和 `let` 绑定不行。字符串与 `Vec[u8]` 的转换由 `std/text` 显式完成。

普通参数不消耗实参，参数名只读。转交拥有值使用 `move`，修改调用者的变量使用 `inout`，调用处都要明确写出。

`throws E` 声明可传播的错误，调用者捕获或继续声明同类错误。`defer` 在当前词法块退出时执行，后注册的先执行。整数除零、越界等 Panic 不能用 typed catch 恢复。

泛型使用 `[T]` 和 `[T Trait]`，trait 必须显式实现。没有模板特化、关联类型或动态 trait 对象。

## 当前范围

已有整数与浮点、字符串值、函数与非捕获函数值、struct/enum、Vec、受约束泛型、模块、typed throws 和块级 defer。程序可以生成 Native 代码或 Python 代码；语言规则不会随所选后端改变。

标准库提供输入输出、文件系统、字符串与容器、JSON、数学、时间、随机数、网络、进程与并发接口，见 [标准库](docs/STDLIB.md)。系统接口面向 Linux x86_64 Native 程序，使用 C 风格名称与参数顺序，失败时抛出声明的错误，锁误用产生 Panic。不开放原始 syscall 接口。

`std/http` 支持 HTTP/1.1；`std/unicode` 提供 Unicode 18 正规化、casefold 和字素簇；`std/timezone` 支持命名时区、夏令时和本地时间转换。

`std/thread` 支持无捕获函数加明确参数的线程创建与等待，`std/sync` 提供互斥锁、条件变量和原子整数。共享可变数据需要程序员同步，原子 ARC 只保护引用计数。

`std/rwlock` 提供读写锁，`std/cancel` 提供协作取消。`std/channel` 提供有界或无界队列，以及超时和取消等待；`std/task` 提供固定工作线程的线程池与 Future，需要显式调用 shutdown。

Native 程序可以声明并调用 C 库的有类型函数，传递自然布局的 C record、Ptr 和 Buffer，或使用同步的无捕获回调。指针和 Buffer 的索引访问放在 unsafe 块内；C 错误码不会自动转换成 Woozy 错误。需要 libc 的库必须明确选择 hosted 运行方式，普通程序继续独立运行。

`std/process` 支持 spawn、exec、wait、参数和环境，`std/signal` 支持同步信号等待，`std/ipc` 支持共享内存及跨进程同步。多线程 fork 后只保留调用线程，业务锁由程序员处理，子进程通常应尽快 exec。

捕获闭包、默认参数、模板特化和通用 Slice 尚未提供。

编译器编译、实现、测试和开发协作资料统一放在 [development/](development/README.md)。
