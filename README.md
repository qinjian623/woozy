# Woozy

<img src="assets/woozy-eye-sparks.gif" alt="Woozy 项目形象" width="256">

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
- [调用 C 库](docs/FFI.md)：有类型的 C 绑定、指针、缓冲区、资源和回调。
- [示例](examples/)：函数、数值、错误处理和参数传递；另有可运行的 [TCP 聊天 server/client](examples/chat/README.md)。

## 主要规则

类型写在名字后面，如 `count i32`。局部绑定可以推断类型，函数参数必须声明类型。简单语句以分号结束。

`let` 不允许重新绑定或修改值字段，`var` 允许。struct/enum 按字段复制；Vec 赋值共享容器，切片建立新 Vec，引用元素仍可共享。

`string` 是不可变 UTF-8 值。`text.len`、索引和切片按 Unicode 码点计算，索引得到 `u32`；`+` 建立新字符串，`==` 和排序按字节内容比较，不做 Unicode 正规化。`var` 整体绑定可以用 `+=`，字段、索引和 `let` 绑定不行。字符串与 `Vec[u8]` 的转换由 `std/text` 显式完成。

普通参数不消耗实参，参数名只读。转交拥有值使用 `move`，修改调用者的变量使用 `inout`，调用处都要明确写出。

函数值可以捕获创建时的局部绑定。捕获绑定只读；Vec、Box 等共享对象的内容仍可通过接口修改。普通 `func` 复制捕获值，`move func(...) { ... }` 转移实际捕获的拥有值。保存 `value.method` 会保存接收者的逻辑副本；重新绑定或替换普通按值字段不会改变它，Vec 等引用字段仍共享内容。

`throws E` 声明可传播的错误，调用者捕获或继续声明同类错误。`defer` 在当前词法块退出时执行，后注册的先执行。整数除零、越界等 Panic 不能用 typed catch 恢复。

泛型使用 `[T]` 和 `[T Trait]`，trait 必须显式实现。`dyn Trait` 与 `dyn Trait[Args]` 是显式的动态 trait 类型，支持对象安全方法的动态分派、`Vec[dyn Trait]`、复制/移动/销毁以及泛型 trait 应用身份（如 `Convert[i32]` 与 `Convert[bool]` 不同）。没有模板特化或关联类型。

## 当前范围

已有整数与浮点、字符串值、函数值与捕获闭包、struct/enum、Vec、Slice、受约束泛型、trait 与 `dyn Trait`、模块、typed throws 和块级 defer。程序可以生成 Native 代码或 Python 代码；语言规则不会随所选后端改变。

标准库提供输入输出、文件系统、字符串与容器、JSON、数学、时间、随机数、网络、进程与并发接口，见 [标准库](docs/STDLIB.md)。系统接口面向 Linux x86_64 Native 程序，使用 C 风格名称与参数顺序，失败时抛出声明的错误，锁误用产生 Panic。不开放原始 syscall 接口。

`std/http` 支持 HTTP/1.1；`std/unicode` 提供 Unicode 18 正规化、casefold 和字素簇；`std/timezone` 支持命名时区、夏令时和本地时间转换。

`std/thread` 支持无捕获函数加明确参数的线程创建与等待，`std/sync` 提供互斥锁、条件变量和原子整数。共享可变数据需要程序员同步，原子 ARC 只保护引用计数。

Native 与 Python 都会自动回收由 Vec、Box 和闭包环境形成的纯 Woozy 强引用环。Native 还支持 async/await、计时器与描述符等待，以及未启动或已完成 Task 的纯 Woozy 引用环回收；异步普通拥有参数在创建 Task 时复制并持有。执行器保持单线程，Python 暂不支持 async；用法见[线程与异步](docs/CONCURRENCY.md)。外部 Python/C 对象自身的环由宿主管理。

`std/rwlock` 提供读写锁，`std/cancel` 提供协作取消。`std/channel` 提供有界或无界队列，以及超时和取消等待；`std/task` 提供固定工作线程的线程池与 Future，需要显式调用 shutdown。

`std/box` 提供共享可变单元 `Box[T]`；`Weak[T]` 是内建弱引用类型，`std/weak` 提供 `empty`、`downgrade`、`upgrade` 操作。目标可以是 Vec、函数值、`Box[T]` 或带引用计数的共享句柄，弱引用不延长目标寿命，升级结果用 `Option[T]` 表示。

Native 程序可以声明并调用 C 库的有类型函数，传递自然布局的 C record、Ptr 和 Buffer。无捕获、不抛错的函数可以交给 C 同步调用，或由 C 保存后稍后调用。指针和 Buffer 的索引访问放在 unsafe 块内；C 错误码不会自动转换成 Woozy 错误。需要 libc 的库必须明确选择 hosted 运行方式，普通程序继续独立运行。

`std/process` 支持 spawn、exec、wait、参数和环境，`std/signal` 支持同步信号等待，`std/ipc` 支持共享内存及跨进程同步。多线程 fork 后只保留调用线程，业务锁由程序员处理，子进程通常应尽快 exec。

默认参数和模板特化尚未提供。

编译器编译、实现、测试和开发协作资料统一放在 [development/](development/README.md)。
