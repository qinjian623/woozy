# Woozy 快速入门

本文面向已经会一门编程语言的读者，先运行一个完整项目，再看容易写错的规则。编译器叫 `woozyc`，源码使用 `.wz`，项目清单仍叫 `lang.toml`。逐步讲解见 [教程](TUTORIAL.md)，完整规则见 [SEMANTICS.md](SEMANTICS.md)。

## 构建编译器

以下命令在本仓库根目录执行，使用 Linux shell。需要 C++20 编译器、CMake 3.20 及以上、Ninja、pkg-config，以及 toml++、json-c、GMPXX 6.3.0 及以上的开发文件；运行生成的 Python 程序需要 Python 3.12.x。依赖安装和更多构建配置见 [BUILD.md](BUILD.md)。

先构建不需要 LLVM 的编译器；它能检查项目并生成 Python：

```sh
cmake -S . -B build/quickstart-frontend -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLANG_ENABLE_LLVM=OFF -DBUILD_TESTING=OFF
cmake --build build/quickstart-frontend --target woozyc
woozyc="$(pwd)/build/quickstart-frontend/woozyc"
"$woozyc" --version
"$woozyc" --help
```

保留这个终端，后面的命令使用变量 `woozyc`。编译器构建完成后才能执行 `check` 和 `build`；Python 解释器只在运行生成程序时使用。

## 创建、检查、构建、运行

没有 `init` 子命令，直接建目录并保存下面两个文件：

```sh
mkdir -p quickstart/src
```

`quickstart/lang.toml`：

```toml
schema_version = 1

[package]
name = "quickstart"
source_root = "src"
entry = "app"

[build]
backend = "python"
word_bits = 64
```

`source_root` 相对于清单所在目录。`app` 表示这个源码根；同一包中的 `.wz` 文件需要声明相同的 `package` 名。

`quickstart/src/main.wz`：

```wz
package main;

func add(left i32, right i32) i32 {
    return left + right;
}

func main() i32 {
    var total i32 = 0;
    for var i i32 = 0; i < 4; i += 1 {
        total += i;
    }
    if add(right: 1, left: total) != 7 {
        return 1;
    }
    return 0;
}
```

在仓库根目录继续执行：

```sh
"$woozyc" check --project quickstart/lang.toml --backend python
"$woozyc" build --project quickstart/lang.toml --backend python \
  --out-dir build/quickstart-python
python3.12 -s build/quickstart-python/program.py
echo $?
```

程序没有标准输出，最后一行应显示 `0`。`build` 写入 `program.py` 和同目录的 `langrt.py`，不会执行程序；两个文件需要放在一起。返回 `1` 表示程序里的预期值检查失败。编译器自身返回非零时，应先处理它报告的错误，再运行程序。

没有 `run` 子命令。`check --syntax-only FILE` 只检查词法和语法，不证明名字、类型、错误传播和所有权正确；日常使用项目检查。

## 常用规则速查

| 项目 | Woozy 的写法与行为 |
| --- | --- |
| 分号 | `package main;`、变量声明、赋值、调用和 `return` 等简单语句以 `;` 结束；换行不代替分号 |
| 类型位置 | 写 `name Type`，例如 `count i32`；局部绑定可省略类型 |
| 函数 | `func add(a i32, b i32) i32`；接口参数必须写类型，省略返回类型表示 `Unit` |
| 绑定 | `let` 不能重新绑定或修改本语言值字段，`var` 可以；普通函数参数的绑定只读，需要修改时新建 `var` |
| 数字 | 没有上下文时整数默认 `i64`，浮点默认 `f64`；不同具体数值类型不隐式混算，转换写 `i32(x)` 等 |
| 整数运算 | 定宽加减乘回绕；除零和非法移位产生 Panic；除法向零截断 |
| 条件与循环 | 条件必须是 `bool`；使用 `if` 和 `for`，赋值不是表达式 |
| 参数标签 | 直接调用可以写 `add(right: 1, left: total)`；位置实参必须在命名实参前，所有实参按书写顺序求值 |
| 函数值 | 非捕获函数值可保存和调用；函数类型不保留参数标签，间接调用只用位置实参 |
| 值复制 | struct/enum 按字段复制；Vec 赋值共享同一容器，切片建立新 Vec，引用元素仍可共享 |
| `move` | 声明 `x move T`，已有绑定调用时写 `move x`；移出后重新初始化前不能读取 |
| `inout` | 声明 `x inout T`，调用写 `inout x`；接受整体可写绑定，限数值、bool 和只含平凡值的有限 struct/enum |
| 接收者 | 普通 `x.method()` 不自动移动或取得可写访问；需要时写 `(move x).take()` 或 `(inout x).change()` |
| 错误 | 函数声明 `throws E`；未捕获错误自动向外传播，不逐次写 `?`；调用者必须捕获或声明兼容的 `throws` |
| `Result` | 自行定义的 `Result` 枚举是普通数据，不会自动传播；当前未提供可直接使用的内建 `Result` |
| `defer` | 注册在当前词法块，退出时后注册先执行；读取执行时的变量值，`return` 的值先保存 |
| `switch` | 不穿透；必须覆盖所有可能的值，或提供 `default`、`_` 等总匹配分支 |
| 泛型与 trait | 用 `[T]`、`[T Trait]` 和显式 `impl`；静态选择实现，不支持动态 trait 对象、特化或关联类型 |

`let` 的只读规则不禁止修改共享 Vec 的元素。Vec 不接受 Python 式负索引，越界会产生 Panic。字符串是不可变 UTF-8 值，不能据此假设已有完整字符串标准库。

## 共享容器、移动与可写参数

下面是另一个完整的 `quickstart/src/main.wz`，替换原文件后重新执行同样的项目检查、Python 构建和运行命令，预期仍返回 `0`：

```wz
package main;

func take(values move Vec[i32]) i32 {
    return values[0];
}

func increment(value inout i32) {
    value += 1;
}

func main() i32 {
    let left Vec[i32] = [1, 2];
    let right = left;
    right[0] = 9;
    var answer = take(move right);
    increment(inout answer);
    if answer != 10 || left[0] != 9 {
        return 1;
    }
    return 0;
}
```

`right` 被移动后不能再读，`left` 仍持有共享容器。`inout` 不能接收 `let`、字段、索引或 Vec 整体，也不能在同一次调用里重复访问同一存储。普通参数不能直接被移动，先建立本地副本才能移动它。

## 错误传播和块级清理

将 `quickstart/src/main.wz` 换成下面的完整程序，同样预期返回 `0`：

```wz
package main;

enum ReadError { Missing; }

func read_value(found bool) i32 throws ReadError {
    if !found {
        throw ReadError.Missing;
    }
    return 7;
}

func load() i32 throws ReadError {
    return read_value(false);
}

func main() i32 {
    var cleaned i32 = 0;
    try {
        defer { cleaned += 1; }
        let value = load();
        return value;
    } catch err ReadError {
        switch err {
            case ReadError.Missing:
                if cleaned != 1 { return 1; }
        }
    }
    return 0;
}
```

`load` 直接传播 `ReadError`，离开 `try` 块时先执行 `defer`，然后进入 `catch`。这个枚举只有一个变体，`switch` 已覆盖全部值。除零、越界等 Panic 不能用 typed `catch` 恢复，也不保证执行语言的清理代码。错误离开 `main` 时输出诊断并以非零状态结束。

## 泛型与显式 trait 实现

以下也是完整的 `quickstart/src/main.wz`，两后端都应返回 `0`：

```wz
package main;

struct Counter { value i32; }

trait Measure {
    func measure(self Self) i32;
}

impl Measure for Counter {
    func measure(self Self) i32 { return self.value; }
}

func measure[T Measure](value T) i32 {
    return value.measure();
}

func main() i32 {
    let counter = Counter(value: 7);
    if measure(counter) != 7 { return 1; }
    return 0;
}
```

trait 必须显式实现；结构体构造要提供全部字段，使用命名字段。泛型函数体按声明约束检查，实际使用的具体类型生成相应代码。完整组合与限制见 [GENERICS.md](GENERICS.md)。

## 改用 Native 后端

Native 构建需要 LLVM、Clang 和 LLD **精确版本 22.1.8**，目前只支持 Linux x86_64、64 位项目，以及 `--opt 0` 或 `--opt 2`。准备好工具链后，在仓库根目录配置另一份编译器；将示例中的 LLVM 安装位置换成实际目录：

```sh
cmake -S . -B build/quickstart-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DLANG_ENABLE_LLVM=ON \
  -DLLVM_DIR=/path/to/llvm-22.1.8/lib/cmake/llvm
cmake --build build/quickstart-native --target woozyc
build/quickstart-native/woozyc check --project quickstart/lang.toml --backend native
build/quickstart-native/woozyc build --project quickstart/lang.toml \
  --backend native --opt 2 --out-dir build/quickstart-native-output
./build/quickstart-native-output/program
echo $?
```

`--backend native` 覆盖清单中的后端选择；这里预期退出 `0`。构建输出 `program` 和 `program.o`，构建本身仍不执行程序。LLVM 的依赖库、编译器兼容配置和排错步骤见 [BUILD.md](BUILD.md)。

| 后端 | 生成内容 | 运行要求与区别 |
| --- | --- | --- |
| Python | `program.py`、`langrt.py` | Python 3.12.x；当前使用 64 位项目；声明式绑定可接入已支持的 Python 库接口 |
| Native | `program`、`program.o` | 开启 LLVM 的编译器与对应工具链；当前只支持 Linux x86_64/64；Python 外部库不能直接在此后端使用 |

两后端遵守相同的源语言数值、作用域、Vec、错误和清理规则；选择 Python 后端不会获得 Python 的隐式真假转换、任意动态属性或自动导入库。后端不支持的操作报告 `E_CAPABILITY_UNAVAILABLE`，不会自动切换后端。

## 工具限制和后续阅读

`lsp`、`capabilities` 尚未实现；也没有 `init`、`run` 或 `fmt` 命令。捕获闭包、默认参数、动态 trait 对象、完整标准库和任意 C++ 直接导入不在当前支持范围。本文通过返回值检查程序，不假设存在 `print`、字符串插值或通用 I/O API。

需要 JSON 诊断时，在项目 `check` 或 `build` 命令末尾加 `--format json`。编译器退出码：成功 `0`，源码或目标不支持 `1`，参数、文件或环境错误 `2`，内部错误 `3`，取消 `130`；这些与生成程序的 `main` 返回值分开判断。项目与导入规则见 [TOOLING.md](TOOLING.md)，详细讲解见 [教程](TUTORIAL.md)。

本文参考了官方仓库中的 [Go Tour 基础章节](https://github.com/golang/website/blob/master/_content/tour/basics.article)、[Julia Getting Started](https://github.com/JuliaLang/julia/blob/master/doc/src/manual/getting-started.md)，以及 [Effective Go](https://go.dev/doc/effective_go)：先给可运行项目，再集中说明已有经验容易带错的规则。Woozy 的具体语法与支持范围以本仓库为准。
