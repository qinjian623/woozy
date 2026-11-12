# Woozy 语言快速入门

面向已经会编程的读者。先看一段完整的 Woozy 程序：

```wz
package main;

func add(left i32, right i32) i32 {
    return left + right;
}

func main() i32 {
    let values Vec[i32] = [1, 2, 3];
    var total i32 = 0;
    for value in values {
        total += value;
    }
    let answer = add(right: 1, left: total); // answer 是 7
    return 0;
}
```

Woozy 的几个规则值得先记住：

| 项目 | 写法与行为 |
| --- | --- |
| 声明 | 类型在名字后面；简单语句需要分号，换行不代替分号 |
| 可变性 | `let` 不可重新绑定或修改值字段；`var` 可以；共享 Vec 的元素仍可修改 |
| 调用 | 普通参数只读且不消耗实参；`move`、`inout` 要明确写出 |
| 错误 | `throws E` 自动传播未捕获错误，不逐次写 `?` |
| 清理 | `defer` 在当前词法块退出时执行，后注册先执行 |
| 类型约束 | 泛型使用 `[T]`，trait 需要显式 `impl`；可使用静态调用或 `dyn Trait` |

下面分别标注“顶层声明”和“函数体片段”。顶层声明放在 `package` 之后；函数体片段放在 `main` 或其他函数内部，需要用到的声明在相邻代码块中给出。

## 标准输出

Linux x86_64 Native 程序可以这样输出文字；这是一个独立的完整文件：

```wz
package main;
import "std/io" as io;
import "std/os" as os;

func main() throws os.Error {
    io.println("Hello, world!");
}
```

`std/io.println` 写出字符串并补一个换行，失败时抛出 `os.Error`。文件和系统接口使用 C 风格名称与参数顺序；文件读写、短写、中断和 File 生命周期见 [std/os](OS.md)，模块清单见 [标准库](STDLIB.md)。带系统调用的模块用于 Linux x86_64 Native 程序，Python 后端不支持。

## 绑定与类型

函数体片段：

```wz
let count i32 = 3;
var total = count + 1;      // 从 count 得到 i32
const step i32 = 2;
total += step;             // total 是 6
let default_integer = 10;  // 无其他约束时为 i64
let ratio = 0.5;           // 无其他约束时为 f64
let widened = i64(count);  // 显式数值转换
let empty Vec[i32] = [];   // 空列表需要元素类型信息
let title string = "Woozy";
```

参数和返回类型必须写明；省略返回类型表示 `Unit`。局部绑定可以推断类型。普通参数绑定只读，需要重新赋值时建立自己的 `var`。`const` 初始值必须在编译期确定，不执行普通函数调用，也不能创建 Vec 或外部对象。

整数类型有 `i8/i16/i32/i64`、`u8/u16/u32/u64` 和目标字宽的 `isize/usize`；浮点类型为 `f32/f64`。不同具体数值类型不隐式混算，转换用 `T(value)`。字面量可以从上下文取得类型。

定宽整数加减乘会回绕，整数除法向零截断；整数除零、动态转换为整数时越界、非法移位会产生 Panic，编译时已知失败的转换直接报错。浮点除零按 IEEE 规则得到无穷或 NaN。`bool` 不与整数互换。`string` 是不可变 UTF-8 值；`text.len`、索引和切片按 Unicode 码点计算，索引得到 `u32`。字符串支持 `+`、整体 `var` 上的 `+=`、`==` 和大小比较，比较按 UTF-8 字节内容进行，不做 Unicode 正规化。`std/text` 的 `bytes` 和 `from_bytes` 显式转换 `Vec[u8]`，后者严格检查 UTF-8 并抛出 `text.Error`。字符串插值仍未支持；`const` 字符串只接受编译期能确定的值。

注释写 `// ...` 或 `/* ... */`，块注释不嵌套。常用运算符有 `+ - * / %`、`== != < <= > >=`、`! && ||` 和整数位运算；`%` 只用于整数。

## 函数、参数标签与函数值

顶层声明：

```wz
func combine(first i32, second i32) i32 {
    return first * 10 + second;
}

func apply(operation func(i32, i32) i32) i32 {
    return operation(2, 3);
}
```

使用上述声明的函数体片段：

```wz
let direct = combine(second: 3, first: 2); // 23
let operation func(i32, i32) i32 = combine;
let indirect = operation(2, 3);           // 23
let applied = apply(operation);          // 23
let twice = func(value i32) i32 {
    return value * 2;
};
let doubled = twice(4);                  // 8
```

直接调用可以使用参数名，位置实参必须在命名实参之前。实参始终按书写顺序求值，命名参数重排不会重排副作用。函数类型不保存参数标签，通过函数值调用只使用位置实参。

函数字面量可以捕获创建时的外层局部绑定。捕获绑定只读，Vec、Box 等共享对象的内容仍可通过原有接口修改。普通 `func` 复制捕获值，`move func(...) { ... }` 移动实际捕获的拥有值；数值等直接值仍复制。

```wz
func make_reader(seed i32) (func() i32) {
    let values Vec[i32] = [seed, seed + 1];
    return func() i32 { return values[0] + values[1]; };
}

func read_value() i32 {
    let read = make_reader(20);
    return read();  // 41
}
```

方法值 `value.method` 在创建时保存接收者的逻辑副本；之后重新绑定原变量或替换普通按值字段，不会改变保存的函数值，但 Vec 等引用字段仍共享对象内容。move-Self 方法的绑定值可以复制并重复调用，每次调用先复制保存的接收者，再把副本交给方法；inout-Self 方法不能保存为函数值。

```wz
struct Counter {
    value i32;
}

impl Counter {
    func read(self move Counter) i32 { return self.value; }
}

func repeat_bound() i32 {
    let counter = Counter(value: 7);
    let read = (move counter).read;
    let copied = read;
    return read() + copied();  // 14
}
```

## 控制流

函数体片段：

```wz
var total i32 = 0;
for var index i32 = 0; index < 3; index += 1 {
    total += index;
}
let values Vec[i32] = [1, 2, 3];
for value in values {
    total += value;
}
for total < 10 {
    total += 1;
}
if total == 10 {
    total += 1;
} else {
    total = 0;
} // total 是 11
```

条件必须是 `bool`，`&&` 和 `||` 短路；赋值不是表达式。循环统一使用 `for`，支持三段式、条件式、`for-in` 和无条件循环。`break`、`continue` 会执行实际离开词法块中的 `defer`。

`switch` 没有分支穿透，必须覆盖全部可能值；无法列全时写 `default` 或 `_`。每个分支有自己的作用域。裸标识符模式会建立新绑定，即使外层有同名 `const`，也不会当作常量匹配。

## struct、enum 与方法

顶层声明：

```wz
struct Point {
    x i32;
    y i32;
}

impl Point {
    func shifted(self Self, distance i32) Self {
        return Point(x: self.x + distance, y: self.y);
    }
}

enum Reading {
    Missing;
    Value(value i32);
}

func unpack(reading Reading) i32 {
    switch reading {
        case Reading.Missing: return 0;
        case Reading.Value(value): return value;
    }
}
```

使用上述声明的函数体片段：

```wz
let original = Point(x: 1, y: 2);
var copy = original;
copy.x = 9;                       // original.x 仍是 1
let shifted = original.shifted(3); // shifted.x 是 4
let reading = Reading.Value(value: shifted.x);
let value = unpack(reading);       // 4
```

struct 构造使用命名字段，必须提供全部字段；enum 变体可以带载荷。struct/enum 按字段递归复制，字段中的引用值仍遵守其自身的共享规则。

`Self` 指当前 impl 或 trait 的类型。值方法调用会提供声明中的首位 Self 参数；`Point.shifted(original, 3)` 则明确传入全部参数。`Point.shifted` 得到未绑定接收者的函数值，`original.shifted` 得到保存了创建时接收者副本的绑定方法值。

## Vec 共享与参数模式

函数体片段：

```wz
let left Vec[i32] = [1, 2, 3];
let right = left;
right[0] = 9;              // left[0] 也变为 9
let slice = left[0:2];
slice[0] = 7;              // left[0] 仍是 9
```

Vec 赋值共享同一个容器，没有隐式深复制；`let` 不禁止修改元素。切片建立新的 Vec 并复制元素的逻辑值，引用元素仍可共享。下标必须非负，越界产生 Panic，不接受 Python 式负索引。

`std/vec` 提供 `empty`、`make`、`len`、`capacity`、`reserve`、`push`、`pop`、`resize` 和 `clear`。`pop` 返回 `Option[T]`；`resize` 变大时用给定值填充，变小时截断；`clear` 保留容量。Vec 赋值共享容器，这些调用会影响所有别名。遍历期间不要 push、pop、resize、clear 或 reserve，只修改已有元素。

`std/box.Box[T]` 是共享的可变值单元；复制 Box 得到同一个单元，`box.get` 读取，`box.set` 替换。`Weak[T]` 是内建类型，保存不延长目标寿命的弱引用；`std/weak` 只提供 `empty`、`downgrade`、`upgrade` 操作。目标可以是 Vec、函数值、`Box[T]` 或带引用计数的共享句柄；普通 struct/enum 值、string、文件/套接字描述符和外部 Python/C 对象不能直接作为目标。`weak.empty[T]()` 建立空弱引用，`weak.downgrade[T](value)` 从现有强引用建立，`weak.upgrade(w)` 返回 `option.Option[T]`，目标已无强引用时得到 `None`。Weak 可以放在 struct、enum、Vec 和泛型值中。接口见 [标准库](STDLIB.md)。

顶层声明：

```wz
func take(values move Vec[i32]) i32 {
    return values[0];
}

func increment(value inout i32) {
    value += 1;
}
```

使用上述声明的函数体片段：

```wz
let values Vec[i32] = [9];
var answer = take(move values); // values 此后不可读
increment(inout answer);       // answer 是 10
```

普通参数不消耗调用者的值；返回或保存参数时按其类型复制或保留共享引用。`move` 转移整个拥有值，原绑定在重新初始化前不可读；普通参数不能直接移动，要先建立本地副本。

`inout` 接受可写 `var`、已有 `inout` 参数、其字段链和可绑定容器元素，例如 `bump(inout obj.nested.value)` 或 `bump(inout items[0])`。位置类型限数值、bool 和只含直接值的有限 struct/enum；Vec/Slice 本身、string、外部对象、函数值和包含 Vec 等需要清理字段的 struct/enum 仍不能按 `inout` 传递。同一次调用中不同具名字段或可证明不同的字面量索引可以同时借用，重叠路径、整容器访问、动态索引冲突和不属于本地/参数的临时位置会被拒绝。Vec/Slice 句柄复制到其他绑定、struct/enum、函数返回值或闭包后仍可能指向同一对象；元素 `inout` 借用期间，其他可能携带该容器的实参或未知间接 callee 会保守报错。

方法不会自动移动或取得可写访问。首位 Self 参数声明为 `move` 或 `inout` 时，值调用分别写 `(move value).take()`、`(inout value).change()`；类型名调用则继续明确传入全部参数。

## throws 与 defer

顶层声明：

```wz
enum ReadError { Missing; }

func read_value(found bool) i32 throws ReadError {
    if !found { throw ReadError.Missing; }
    return 7;
}

func load(found bool) i32 throws ReadError {
    return read_value(found); // 错误自动向外传播
}
```

使用上述声明的函数体片段：

```wz
var cleaned i32 = 0;
var result i32 = 0;
try {
    defer { cleaned += 1; }
    result = load(false);
} catch error ReadError {
    switch error {
        case ReadError.Missing: result = -1;
    }
} // cleaned 是 1，result 是 -1
```

`throws E` 属于函数类型。未捕获的调用错误必须由当前函数声明兼容的 `throws`；不需要逐次写 `?`。自行定义的 `Result` 枚举只是普通数据，不自动传播。标准库 `std/result.Result[T,E]` 同样是普通 enum，构造时要写完整类型实参，例如 `result.Result[i32,i32].Ok(7)`。

`defer` 在当前词法块退出时执行，后注册先执行；它读取执行时的绑定值。`return value` 先保存返回值，再执行清理。defer 本身不能向外执行 `return/break/continue` 或传播错误。整数除零、越界等 Panic 不能由 typed `catch` 捕获，也不保证执行语言清理。

## 泛型与 trait

顶层声明：

```wz
func identity[T](value T) T { return value; }

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
```

使用上述声明的函数体片段：

```wz
let first = identity[i32](7);       // 显式类型实参
let second = identity(first);       // 推断 T 为 i32
let counter = Counter(value: second);
let measured = measure(counter);    // 7
```

类型参数使用 `[T]`，约束写成 `[T Measure]`；泛型类型、泛型方法和带类型实参的 trait 也使用方括号。函数体按声明约束检查，使用具体类型时生成相应代码。

trait 是名义类型约束，需要显式实现，不按方法“长得像”自动满足。默认使用静态分派；需要运行时多态时写 `dyn Trait` 或 `dyn Trait[Args]`，对象安全的方法在运行时按具体实现分派，具体值在唯一实现下自动转换。不支持关联类型、特化、非类型或变参模板参数。类型参数和约束规则见 [泛型和 trait](SEMANTICS.md#6-泛型和-trait)。

## 线程与同步

Linux x86_64 Native 程序可以使用 `std/thread` 的 `spawn[A](func(A) i64, A)` 与 `join(Thread) i64`，二者都声明 `throws os.Error`。线程函数不捕获局部变量，入口内部自行处理 typed errors。Thread 按引用共享；未 join 就清理句柄不会取消线程，main 返回会结束整个进程。

`std/sync` 提供非递归 Mutex、Condvar 和顺序一致的 AtomicI64。正常锁操作不抛错，错误解锁等误用 Panic。允许在线程间共享 Vec 等对象，由程序员同步读写；原子 ARC 只保证计数安全。示例见 [线程与同步](CONCURRENCY.md)。

## 包与导入

每个文件声明 `package`，同一个包中的顶层声明互相可见。`pub` 控制跨包访问，导入别名只在当前文件有效。下面两个文件组成一个小项目，`app` 表示项目源码根。

`src/geometry/point.wz`（完整文件）：

```wz
package geometry;

pub struct Point { pub x i32; pub y i32; }

pub func horizontal(point Point) i32 {
    return point.x;
}
```

`src/main.wz`（完整文件）：

```wz
package main;

import "app/geometry" as geometry;

func main() i32 {
    let point = geometry.Point(x: 3, y: 4);
    let value = geometry.horizontal(point); // 3
    return 0;
}
```

模块由目录中的 `.wz` 文件组成，`app/geometry` 对应源码根下的 `geometry/`；包依赖不能循环。其他导入首段需要在项目清单声明；导入 Python 库也要有对应绑定声明，源码检查不会执行外部库。

运行示例见 [BUILD.md](../development/docs/BUILD.md)。逐步讲解见 [教程](TUTORIAL.md)，标准库接口和资源规则见 [标准库](STDLIB.md)，数值、共享、错误和清理的完整规则见 [SEMANTICS.md](SEMANTICS.md)。

本文的组织参考官方仓库中的 [Go Tour 基础章节](https://github.com/golang/website/blob/master/_content/tour/basics.article)、[Julia Getting Started](https://github.com/JuliaLang/julia/blob/master/doc/src/manual/getting-started.md)，以及 [Effective Go](https://go.dev/doc/effective_go)，具体语言规则以本仓库为准。
