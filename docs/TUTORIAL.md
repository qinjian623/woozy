# Woozy 教程

这份教程带你写一个计算分数总和的小程序，再加入函数、容器、结构体、枚举、错误处理和模块。你需要知道变量、函数和循环是什么；已经熟悉多门语言的读者可以直接看 [快速入门](QUICKSTART.md)。

本文使用当前仓库的 `woozyc` 和 `.wz` 文件。命令在 Linux shell 中执行。示例通过返回码检查结果：`0` 表示符合预期，其他值表示检查失败。这里不依赖输入输出标准库。

## 1. 准备编译器和项目

先取得本仓库并进入仓库根目录。需要 C++20 编译器、CMake 3.20 及以上、Ninja、pkg-config、toml++、json-c 和 GMPXX 6.3.0 及以上的开发文件；运行本教程生成的程序需要 Python 3.12.x。依赖和 Native 构建说明见 [BUILD.md](BUILD.md)。

构建能生成 Python 程序的编译器，不需要安装 LLVM：

```sh
cmake -S . -B build/tutorial-frontend -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLANG_ENABLE_LLVM=OFF -DBUILD_TESTING=OFF
cmake --build build/tutorial-frontend --target woozyc
woozyc="$(pwd)/build/tutorial-frontend/woozyc"
"$woozyc" --version
mkdir -p tutorial/src
```

后续命令沿用这个终端中的 `woozyc` 变量。创建 `tutorial/lang.toml`：

```toml
schema_version = 1

[package]
name = "tutorial"
source_root = "src"
entry = "app"

[build]
backend = "python"
word_bits = 64
```

清单仍叫 `lang.toml`。`source_root` 相对于清单所在目录，`app` 指向这个源码根。创建 `tutorial/src/main.wz`：

```wz
package main;

func main() i32 {
    let first i32 = 12;
    let second i32 = 18;
    let total = first + second;
    if total != 30 {
        return 1;
    }
    return 0;
}
```

检查项目、生成程序，再运行它：

```sh
"$woozyc" check --project tutorial/lang.toml --backend python
"$woozyc" build --project tutorial/lang.toml --backend python \
  --out-dir build/tutorial-python
python3.12 -s build/tutorial-python/program.py
echo $?
```

最后一行应显示 `0`，程序本身没有标准输出。编译器报告错误时，先修正错误再运行程序。`build` 只生成 `program.py` 和 `langrt.py`，不会自动运行；运行时这两个文件需要放在一起。

`package main;` 声明包名。`func main() i32` 声明入口函数，返回一个 `i32` 状态码。类型写在名称后面；函数返回类型写在参数列表后面，不使用箭头。简单语句末尾要写分号，换行不能代替分号。返回非 `Unit` 的函数必须用 `return` 返回值；省略返回类型的 `Unit` 函数可以直接执行到末尾。末尾表达式不会自动成为返回值。

后面的完整示例都替换 `tutorial/src/main.wz`，再执行上面三条检查、构建和运行命令；除标明“错误示例”的代码外，都应返回 `0`。

## 2. 绑定与数值类型

`let` 不允许重新赋值，`var` 允许。类型明确时，可以省略局部绑定的类型标注：

```wz
package main;

func main() i32 {
    let first i32 = 12;
    var total = first;
    total += 18;
    let ratio f64 = f64(total) / 4.0;
    if total != 30 || ratio != 7.5 {
        return 1;
    }
    return 0;
}
```

`total` 从 `first` 推断为 `i32`，`18` 由上下文确定类型。没有上下文的整数字面量默认是 `i64`，浮点字面量默认是 `f64`。已经确定类型的变量不能随意混算；例如 `i32` 与 `i64` 相加需要先转换。

转换写成 `目标类型(表达式)`。整数窄化会检查范围，浮点转整数先向零截断再检查范围；编译时已知转换失败就报错，动态失败产生 Panic。定宽整数加减乘会回绕，不能把它当作带溢出报错的运算。详细数值规则见 [SEMANTICS.md 第 3、4 节](SEMANTICS.md#3-基础类型与推断)。

下面是**完整的错误示例**，项目检查应失败。把 `let` 改成 `var` 后才能重新赋值：

```wz
package main;

func main() i32 {
    let total i32 = 12;
    total = 30;
    return 0;
}
```

## 3. 函数、循环和参数标签

现在把分数保存为列表，并把求和写成函数：

```wz
package main;

func sum(scores Vec[i32]) i32 {
    var total i32 = 0;
    for score in scores {
        total += score;
    }
    return total;
}

func reaches(value i32, minimum i32) bool {
    return value >= minimum;
}

func main() i32 {
    let scores Vec[i32] = [12, 18, 20];
    let total = sum(scores);
    if !reaches(minimum: 50, value: total) {
        return 1;
    }
    return 0;
}
```

函数参数必须写类型。普通参数的名称只读；需要修改累计值时，像 `sum` 一样新建 `var` 局部变量。

直接调用可以按参数名传值。`reaches(minimum: 50, value: total)` 与按位置传入 `reaches(total, 50)` 绑定到相同参数。实参按源码书写顺序求值；命名实参不会改变副作用的先后顺序。混用时，位置实参必须写在命名实参前。

`if` 条件必须是 `bool`，不能用整数代替真假。所有循环都使用 `for`。下面是三段式循环的完整例子：

```wz
package main;

func main() i32 {
    var total i32 = 0;
    for var i i32 = 0; i < 4; i += 1 {
        total += i;
    }
    if total != 6 {
        return 1;
    }
    return 0;
}
```

还可以写 `for condition { ... }` 或 `for { ... }`，分别表示条件循环和无限循环；这里的省略号仅表示循环体，不是 Woozy 语法。

## 4. Vec、共享与切片

`Vec[T]` 是元素类型为 `T` 的容器。复制 Vec 绑定会共享同一个容器，切片会建立新 Vec：

```wz
package main;

func main() i32 {
    let scores Vec[i32] = [12, 18, 20];
    let alias = scores;
    alias[0] = 15;
    let first_two = scores[0:2];
    first_two[0] = 99;
    if scores[0] != 15 || first_two[0] != 99 {
        return 1;
    }
    return 0;
}
```

`alias[0] = 15` 修改共享容器，因此 `scores[0]` 也变成 `15`。`let` 禁止给名称重新赋值，也禁止修改本语言值的字段；它允许通过 Vec 的索引赋值修改容器内容。

`scores[0:2]` 复制第 0、1 个元素，不包含第 2 个元素。这里元素为整数，所以修改新 Vec 不影响原 Vec；元素若也是 Vec，复制后的元素仍共享内部容器。

Vec 没有负索引。下标越界或切片不满足 `0 <= start <= end <= len` 会产生 Panic。遍历中可以修改已有元素，不能改变正在遍历的 Vec 的长度。

## 5. struct、enum 与 switch

用结构体记录总分，用枚举区分是否达标：

```wz
package main;

struct Summary {
    total i32;
    count i32;
}

enum Verdict {
    Pass(total i32);
    Fail;
}

func evaluate(summary Summary) Verdict {
    if summary.count > 0 && summary.total >= 50 {
        return Verdict.Pass(summary.total);
    }
    return Verdict.Fail;
}

func main() i32 {
    let summary = Summary(total: 50, count: 3);
    switch evaluate(summary) {
        case Verdict.Pass(total):
            if total != 50 {
                return 1;
            }
        case Verdict.Fail:
            return 2;
    }
    return 0;
}
```

结构体构造使用调用形式，字段必须全部初始化。字段通过 `summary.total` 读取。结构体与枚举按字段复制；字段为 Vec 时，复制的仍是共享容器的句柄。

`Verdict.Pass` 携带一个整数，`Verdict.Fail` 不带数据。`switch` 分支使用冒号，不会自动进入下一分支；这里列出了所有变体，因此不需要 `default`。计算出来的主体 `evaluate(summary)` 只执行一次。

`case Verdict.Pass(total):` 中的 `total` 是分支内的新绑定。裸标识符模式总是新绑定，不能用 `case limit:` 表示“匹配外面的 limit 常量”。对于不能列全的值，可以用 `default:` 或 `case _:` 覆盖剩余情况。没有总匹配分支时，编译器必须能证明所有值都已覆盖，例如 bool 的 `true` 和 `false` 两项。

## 6. 可恢复错误与块退出清理

把负分视为可恢复错误，用 `throws` 声明它，用 `defer` 记录一次函数退出：

```wz
package main;

enum ScoreError {
    Negative(value i32);
}

func require_score(value i32) i32 throws ScoreError {
    if value < 0 {
        throw ScoreError.Negative(value);
    }
    return value;
}

func checked_sum(scores Vec[i32], cleaned inout i32) i32 throws ScoreError {
    defer {
        cleaned += 1;
    }
    var total i32 = 0;
    for score in scores {
        total += require_score(score);
    }
    return total;
}

func main() i32 {
    var cleaned i32 = 0;
    try {
        let total = checked_sum([12, -1, 20], inout cleaned);
        return 1;
    } catch error ScoreError {
        switch error {
            case ScoreError.Negative(value):
                if value != -1 || cleaned != 1 {
                    return 2;
                }
        }
    }
    return 0;
}
```

`require_score` 的错误会自动从 `checked_sum` 向外传播，调用处不加 `?`。不声明兼容 `throws` 的函数必须用 `try/catch` 处理错误。`Result` 如果由你定义成枚举，只是普通数据，不会自动触发传播。

`cleaned inout i32` 让函数修改调用者的变量，调用处也要写 `inout cleaned`。现有 `inout` 接受整体 `var` 绑定，也可以继续转交已有 `inout` 参数，限数值、bool 和递归只含平凡值的有限 struct/enum；不能给字段、索引或 Vec 本身取得这种可写访问。同一次调用不能对同一存储同时做 `inout` 和另一种访问。

`defer` 在**当前词法块**退出时执行，后注册的先执行。正常退出、`return`、`break`、`continue` 和 typed throw 都执行离开代码块时的清理；上例进入 `catch` 前，`cleaned` 已变成 `1`。`defer` 读取执行时的绑定值，返回表达式的值则先于清理保存。

Panic 用于越界、除零等运行失败，不能用上面的 typed catch 恢复，也不保证执行 `defer`。不要用它处理预期内的输入错误。

## 7. 传递拥有值

普通参数不会消耗调用者的值。需要转交拥有值时，声明 `move` 参数并在调用处写明移动：

```wz
package main;

func take_first(scores move Vec[i32]) i32 {
    return scores[0];
}

func main() i32 {
    let scores Vec[i32] = [12, 18, 20];
    let kept = scores;
    let first = take_first(move scores);
    if first != 12 || kept[0] != 12 {
        return 1;
    }
    return 0;
}
```

移动后不能继续读取 `scores`；`kept` 是先前取得的独立拥有句柄，仍可使用同一容器。移动不会自动深复制或清空其他别名。

普通方法调用也不会自动移动或取得 `inout`。需要这些接收者模式时，写作 `(move value).take()` 或 `(inout value).change()`，并要求方法签名声明相应模式。具体限制见 [SEMANTICS.md 第 7 节](SEMANTICS.md#7-值容器与-arc)。

## 8. 拆成多个文件

同一目录里的 `.wz` 文件属于同一个包，顶层声明彼此可见，不需要互相 import。将求和函数保存到 `tutorial/src/scores.wz`：

```wz
package main;

func sum(scores Vec[i32]) i32 {
    var total i32 = 0;
    for score in scores {
        total += score;
    }
    return total;
}
```

将 `tutorial/src/main.wz` 替换为：

```wz
package main;

func main() i32 {
    if sum([12, 18, 20]) != 50 {
        return 1;
    }
    return 0;
}
```

这两个文件一起检查、构建，仍应返回 `0`。不要同时保留前面版本里的第二个 `sum` 或 `main` 声明。

子模块使用独立目录。例如将下一份代码保存到 `tutorial/src/stats/stats.wz`，需要先创建 `tutorial/src/stats/`：

```wz
package stats;

pub func passed(total i32) bool {
    return total >= 50;
}
```

再将 `tutorial/src/main.wz` 替换为：

```wz
package main;
import "app/stats" as stats;

func main() i32 {
    if !stats.passed(sum([12, 18, 20])) {
        return 1;
    }
    return 0;
}
```

保留 `scores.wz`，结果仍应为 `0`。`app/stats` 是清单源码根下的逻辑模块名，读取 `src/stats/` 中的文件；不要把它写成源码文件路径。跨包使用的声明要加 `pub`。import 别名只在写有该 import 的文件内有效；包之间不能循环导入。

## 9. 泛型与 trait

下面换一个独立例子：从两个值中取较小值。先删除本教程创建的 `scores.wz` 和 `stats/`，只保留下方的 `main.wz` 与原清单。

```wz
package main;

trait Ordered {
    func less(self Self, other Self) bool;
}

impl Ordered for i32 {
    func less(self Self, other Self) bool {
        return self < other;
    }
}

func smaller[T Ordered](left T, right T) T {
    if left.less(right) {
        return left;
    }
    return right;
}

func main() i32 {
    let result = smaller[i32](12, 18);
    if result != 12 {
        return 1;
    }
    return 0;
}
```

`T` 是类型参数，`Ordered` 约束它能执行的操作。`impl` 为 `i32` 提供实现；`Self` 表示该实现对应的类型，方法的接收者写在第一个参数位置。

这是按类型参数和约束检查的泛型。不要把 C++ 模板特化、常量泛型、关联类型或动态 trait 对象写法带进来。每个类型参数最多一个具名约束；更完整的支持范围见 [GENERICS.md](GENERICS.md)。

## 10. 继续使用

同一公共子集项目也可以生成 Native 程序；需要用 LLVM 22.1.8 构建带 Native 后端的编译器。在 Linux x86_64 上，生成的程序不需要 Python、libc 或 libstdc++。构建编译器本身仍需要 C++ 和相关依赖。具体命令见 [快速入门的 Native 部分](QUICKSTART.md#改用-native-后端)。

- 想快速查写法：读 [QUICKSTART.md](QUICKSTART.md)。
- 想确认值、错误、清理的精确定义：读 [SEMANTICS.md](SEMANTICS.md)。
- 想使用 Python 外部库：读 [INTEROP.md](INTEROP.md)，需要声明绑定，不能直接套用 Python import。
- 想查看已运行过的样例：读 [examples/](../examples/) 和 [tests/fixtures/](../tests/fixtures/)。

本文的组织参考了 [Rust Book 的章节目录](https://github.com/rust-lang/book/blob/main/src/SUMMARY.md) 和 [Go Tour 的基础篇](https://github.com/golang/website/blob/master/_content/tour/basics.article)：从完整可运行的文件开始，每次增加少量规则。示例与语言规则均以 Woozy 当前实现为准。
