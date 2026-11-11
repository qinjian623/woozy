# Woozy

Woozy 是一门静态类型编程语言，支持局部类型推断、结构体、枚举、泛型与 trait 约束。源文件使用 `.wz` 后缀。

```wz
package main;
import "std/os" as os;

func main() throws os.Error {
    os.write_all_text(os.stdout(), "Hello, world!\n");
}
```

## 学习 Woozy

- [语言教程](docs/TUTORIAL.md)：从一段程序开始，逐步学习类型、函数、容器、模式匹配、错误处理、模块和泛型。
- [语言快速入门](docs/QUICKSTART.md)：面向有编程经验的读者，集中介绍写法和容易混淆的规则。
- [语言规则](docs/SEMANTICS.md)：名称、求值顺序、数值、复制、所有权、错误和清理的详细定义。
- [词法与语法](baseline/LEXICAL_SYNTAX.md)：关键字、字面量、表达式和声明形式。
- [系统接口](docs/OS.md)：标准输出、文件读写、目录和进程退出。
- [线程与同步](docs/CONCURRENCY.md)：线程参数、等待、共享数据、锁、条件变量和原子整数。
- [示例](examples/)：函数、数值、错误处理和参数传递。

## 主要规则

类型写在名字后面，如 `count i32`。局部绑定可以推断类型，函数参数必须声明类型。简单语句以分号结束。

`let` 不允许重新绑定或修改值字段，`var` 允许。struct/enum 按字段复制；Vec 赋值共享容器，切片建立新 Vec，引用元素仍可共享。

普通参数不消耗实参，参数名只读。转交拥有值使用 `move`，修改调用者的变量使用 `inout`，调用处都要明确写出。

`throws E` 声明可传播的错误，调用者捕获或继续声明同类错误。`defer` 在当前词法块退出时执行，后注册的先执行。整数除零、越界等 Panic 不能用 typed catch 恢复。

泛型使用 `[T]` 和 `[T Trait]`，trait 必须显式实现。没有模板特化、关联类型或动态 trait 对象。

## 当前范围

已有整数与浮点、字符串值、函数与非捕获函数值、struct/enum、Vec、受约束泛型、模块、typed throws 和块级 defer。程序可以生成 Native 代码或 Python 代码；语言规则不会随所选后端改变。

`std/os` 为 Linux x86_64 Native 程序提供标准输出、文件读写、目录操作和进程退出，使用 C 风格名称与参数顺序，失败时抛出 `os.Error`。不开放原始 syscall 接口。

`std/thread` 支持无捕获函数加明确参数的线程创建与等待，`std/sync` 提供互斥锁、条件变量和原子整数。共享可变数据需要程序员同步，原子 ARC 只保护引用计数。它们同样限于 Linux x86_64 Native 程序。

完整标准库、捕获闭包、默认参数、Map、通用 Slice 和 Ptr 尚未提供。

编译器构建、实现、测试和开发协作资料统一放在 [development/](development/README.md)。
