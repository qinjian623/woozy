# Woozy

[English](README.md)

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

- [语言教程](docs/TUTORIAL.cn.md)：从一段程序开始，逐步学习类型、函数、容器、模式匹配、错误处理、模块和泛型。
- [语言快速入门](docs/QUICKSTART.cn.md)：面向有编程经验的读者，集中介绍写法和容易混淆的规则。
- [语言规则](docs/SEMANTICS.md)：名称、求值顺序、数值、复制、所有权、错误和清理的详细定义。
- [词法与语法](baseline/LEXICAL_SYNTAX.md)：关键字、字面量、表达式和声明形式。
- [标准库](docs/STDLIB.md)：各模块的常用接口、完整示例、错误和资源关闭规则。
- [系统接口](docs/OS.md)：标准输出、文件读写、目录和进程退出。
- [线程与同步](docs/CONCURRENCY.md)：线程参数、等待、共享数据、锁、条件变量和原子整数。
- [进程与信号](docs/PROCESSES.md)：启动程序、重定向、等待、参数环境、信号与 fork 规则。
- [共享内存与 IPC](docs/IPC.md)：共享映射、跨进程互斥锁、条件变量和信号量。
- [调用 C 库](docs/FFI.md)：有类型的 C 绑定、指针、缓冲区、资源和回调。
- [示例](examples/)：函数、数值、错误处理和参数传递；另有可运行的 [TCP 聊天 server/client](examples/chat/README.md) 和 [异步 HTTP server](examples/http_server/README.md)。

## 安装

Woozy 安装包面向 Linux x86_64 和 Apple Silicon，均包含 `woozyc`、标准库、运行库和锁定版本的 LLVM 工具。Mac 原生程序要求 macOS 14.4 或更新版本，编译时需要 Xcode Command Line Tools 或已安装的 macOS SDK。

装有 curl 和 Python 3.12 及以上版本时，可以用下面这条命令检查 [GitHub 发布页](https://github.com/qinjian623/woozy/releases)；找到可用版本后，脚本会询问是否安装或升级。

```sh
curl -fsSL https://raw.githubusercontent.com/qinjian623/woozy/main/install.py | python3 -
```

只查可用版本。

```sh
curl -fsSL https://raw.githubusercontent.com/qinjian623/woozy/main/install.py | python3 - --check
```

跳过询问，直接安装或升级。

```sh
curl -fsSL https://raw.githubusercontent.com/qinjian623/woozy/main/install.py | python3 - --yes
```

脚本优先选择正式版。已安装预发布版的用户可以升到更新的预发布版或正式版。校验安装包后，脚本会切换 `~/.local/bin/woozyc`，旧版本仍保留。如果终端找不到 `woozyc`，将 `~/.local/bin` 加到 `PATH`。可以用 `--prefix DIR` 指定其他用户安装目录。

如果 `~/.local/bin/woozyc` 已指向源码构建版或没有发行信息的旧安装包，请改用另一个安装目录，以免覆盖原有安装。

### 手动安装

下面的命令安装 [发布页](https://github.com/qinjian623/woozy/releases) 上的 `v0.1.0-rc.2`。

```sh
archive=woozy-linux-x86_64.tar.gz   # Apple Silicon 改用 woozy-macos-arm64.tar.gz。
base=https://github.com/qinjian623/woozy/releases/download/v0.1.0-rc.2
curl -fLO "$base/$archive"
curl -fLO "$base/$archive.sha256"
sha256sum -c "$archive.sha256"     # macOS 改用 shasum -a 256 -c "$archive.sha256"。
tar -xzf "$archive"
mkdir -p "$HOME/.local/bin"
ln -s "$PWD/woozy/woozyc" "$HOME/.local/bin/woozyc"
export PATH="$HOME/.local/bin:$PATH"   # 将这一行加入 shell 配置。
woozyc --version
```

在仓库检出目录构建并运行 Hello World 示例。发行包不包含 `examples/`。

```sh
woozyc build --project examples/hello/lang.toml --backend native --out-dir build/hello
./build/hello/program
```

输出 `Hello, world!`。运行生成的 Python 代码需要 Python 3.12。`--package-objects` 使用 Ninja，`--source-make` 由 Woozy 编译器直接安排分包编译。TLS 需要单独构建附加库。[构建说明](development/docs/BUILD.md#安装自举版工具链) · [标准库说明](docs/STDLIB.md)

## 编辑器配置

Linux 和 macOS 上的 Emacs、Vim 和 Neovim 打开 `.wz` 文件时可自动启用语法高亮和 Woozy LSP。自举版编译器提供补全、悬停、定义跳转和实时项目诊断。把 `woozyc` 加到 `PATH` 后，在仓库根目录运行下面这条命令即可配置 Vim 和 Emacs。

```sh
python3 development/editor/setup.py --editor vim-emacs
```

省略 `--editor` 时配置 Neovim 和 Emacs。环境要求、单个编辑器的安装方式和编译器选择见[编辑器配置](development/docs/EDITOR.md)。

## 主要规则

类型写在名字后面，如 `count i32`。局部绑定可以推断类型，函数参数必须声明类型。简单语句以分号结束。

`let` 不允许重新绑定或修改值字段，`var` 允许。struct/enum 按字段复制；Vec 赋值共享同一容器，切片会建新 Vec，但其中的引用元素仍会共享。

`string` 是不可变 UTF-8 值。`text.len`（来自 `std/text`）、索引和切片都按 Unicode 码点计算，索引得到 `u32`；`+` 生成新字符串，`==` 和排序按字节内容比较，不做 Unicode 正规化。只有整体 `var` 绑定能用 `+=`，字段、索引和 `let` 绑定不行。字符串与 `Vec[u8]` 之间通过 `std/text` 显式转换。

普通参数不消耗实参，参数名只读。转交拥有值使用 `move`，修改调用者的变量使用 `inout`，调用处都要明确写出。

函数值可以捕获创建时的局部绑定。捕获绑定只读，Vec、Box 等共享对象的内容仍可通过接口修改。普通 `func` 复制捕获值，`move func(...) { ... }` 转移实际捕获的拥有值。把 `value.method` 存为函数值时会保留接收者的逻辑副本：重新绑定原变量或替换普通按值字段不会影响它，Vec 等引用字段仍共享内容。

`throws E` 声明可传播的错误，调用者要么捕获，要么继续声明同类错误。`defer` 在当前词法块退出时执行，后注册的先执行。整数除零、越界等 Panic 不能用 typed catch 恢复。

泛型使用 `[T]` 和 `[T Trait]`，trait 必须显式实现。`dyn Trait` 与 `dyn Trait[Args]` 是显式的动态 trait 类型，支持对象安全方法的动态分派、`Vec[dyn Trait]`、复制/移动/销毁，也能按类型实参区分泛型 trait 身份（如 `Convert[i32]` 与 `Convert[bool]` 不同）。不支持模板特化和关联类型。

## 当前范围

已有整数与浮点、字符串值、函数值与捕获闭包、struct/enum、Vec、Slice、受约束泛型、trait 与 `dyn Trait`、模块、typed throws 和块级 defer。程序可以生成 Native 代码或 Python 代码；语言规则不会随所选后端改变。

标准库提供输入输出、文件系统、字符串与容器、JSON、数学、时间、随机数、网络、进程与并发接口，见 [标准库](docs/STDLIB.md)。系统接口用于 Linux x86_64 和 macOS arm64 的原生程序，使用 C 风格名称与参数顺序，失败时抛出声明的错误，锁误用产生 Panic。不开放原始 syscall 接口。

`std/http` 支持 HTTP/1.1；`std/http_server` 是异步 HTTP/1.1 服务器，支持 keep-alive、流水线、非阻塞背压、请求/正文/空闲/写超时、`Expect: 100-continue` 以及优雅 stop/drain/abort；`std/unicode` 提供 Unicode 18 正规化、casefold 和字素簇；`std/timezone` 支持命名时区、夏令时和本地时间转换。

`std/thread` 用无捕获函数和显式参数来创建并等待线程，`std/sync` 提供互斥锁、条件变量和原子整数。共享可变数据需要程序员自行同步，原子 ARC 只保护引用计数。

Native 与 Python 都会自动回收由 Vec、Box 和闭包环境形成的纯 Woozy 强引用环。Native 还支持 async/await、计时器与描述符等待，以及未启动或已完成 Task 的纯 Woozy 引用环回收；异步函数的普通拥有参数在创建 Task 时复制并持有。执行器为单线程，Python 后端暂不支持 async，用法见[线程与异步](docs/CONCURRENCY.md)。外部 Python/C 对象自身的环由宿主管理。

`std/rwlock` 提供读写锁，`std/cancel` 提供协作取消。`std/channel` 提供有界或无界队列，以及超时和取消等待；`std/task` 提供固定工作线程的线程池与 Future，需要显式调用 shutdown。

`std/box` 提供共享可变单元 `Box[T]`；`Weak[T]` 是内建弱引用类型，`std/weak` 提供 `empty`、`downgrade`、`upgrade` 操作。目标可以是 Vec、函数值、`Box[T]` 或带引用计数的共享句柄，弱引用不延长目标寿命，升级结果用 `Option[T]` 表示。

Native 程序可以声明并调用 C 库的有类型函数，传递自然布局的 C record、Ptr 和 Buffer。无捕获、不抛错的函数可以交给 C 同步调用，或由 C 保存后稍后调用。指针和 Buffer 的索引访问放在 unsafe 块内；C 错误码不会自动转换成 Woozy 错误。需要 libc 的库必须明确选择 hosted 运行方式，普通程序继续独立运行。

`std/process` 支持 spawn、exec、wait、参数和环境，`std/signal` 支持同步信号等待，`std/ipc` 支持共享内存及跨进程同步。多线程 fork 后只保留调用线程，业务锁由程序员处理，子进程通常应尽快 exec。

默认参数尚未提供。

编译器编译、实现、测试和开发协作资料统一放在 [development/](development/README.md)。

## 许可证

Woozy 的自有代码和文档采用 [Apache License 2.0](LICENSE)。第三方材料沿用各自的许可证。Woozy 名称和项目形象不在此授权范围内。
