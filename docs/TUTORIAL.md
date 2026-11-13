# Woozy 语言教程

Woozy 使用静态类型，支持局部类型推断、泛型、结构体、枚举和可声明的错误。函数、值、作用域与容器的规则在不同后端保持一致。

这份教程讲如何用 Woozy 表达程序。下面从一段完整代码开始，再逐项介绍写法。已有编程经验、想快速查差异的读者可以看 [语言快速入门](QUICKSTART.md)。示例使用 `.wz` 文件；准备运行环境见 [BUILD.md](../development/docs/BUILD.md)，标准库接口见 [标准库](STDLIB.md)。

## 1. 先读一个程序

从输出一行文字开始。这是一个完整程序：

```wz
package main;
import "std/io" as io;
import "std/os" as os;

func main() throws os.Error {
    io.println("Hello, world!");
}
```

`import "std/io" as io;` 导入输出函数。`io.println` 写出字符串并补一个换行。`throws os.Error` 声明输出可能失败，错误处理在后面的章节介绍。这个程序用于 Linux x86_64 Native 后端，运行时输出 `Hello, world!`。

下面是另一个完整程序，用来计算一组分数的总和；与上例分别阅读和保存：

```wz
package main;

func sum(scores Vec[i32]) i32 {
    var total i32 = 0;
    for score in scores {
        total += score;
    }
    return total;
}

func main() i32 {
    let scores Vec[i32] = [12, 18, 20];
    let total = sum(scores);  // 50
    return 0;
}
```

`package main;` 声明这个文件所属的包。`sum` 接收一组 `i32` 整数，遍历后返回总和；`main` 是程序入口，这里的 `0` 表示正常退出。

几个基本写法：

- 类型放在名称后面：`scores Vec[i32]`、`total i32`。
- 函数用 `func` 声明，返回类型写在参数列表后面。
- 简单语句以分号结束，代码块用花括号包围。
- `let` 声明不能重新赋值的绑定，`var` 声明可以重新赋值的绑定。
- `return` 明确返回一个值，函数体最后的表达式不会自动返回。

以下各节的示例独立阅读，按说明组合相邻的声明与调用，不要把所有片段拼到同一个文件。短片段会说明写在函数体内还是作为顶层声明；它们不需要各自再重复一个 `main`。代码注释标出预期值；需要输出字符串或字节时，使用 [std/os](OS.md)。

## 2. 注释、绑定与作用域

单行注释以 `//` 开始。`/* ... */` 可以跨行，但块注释不能嵌套：

```wz
// 在函数体内
let initial i32 = 10;
var count = initial;
count += 2;  // count 为 12
/* initial 和 count 都是 i32。
   count 的类型由初始化式推断。 */
```

给 `initial` 重新赋值会报错。`let` 和 `var` 都必须初始化；后续赋值不能改变已经确定的类型。局部绑定可以省略类型，函数参数、结构体字段必须写类型。

`const` 要求初始值能在编译期确定，可以写在顶层或局部作用域中：

```wz
const PassingScore i32 = 60;
const Greeting string = "你好，Woozy";
```

普通函数调用、Vec 和外部对象不能作为 `const` 的初始值。纯 struct/enum 的递归常量构造及常量字段读取可以使用。

花括号引入局部作用域。同一作用域不能重复声明同名绑定，内层作用域可以遮蔽外层名称：

```wz
// 在函数体内
let x i32 = 1;
{
    let x = x + 1;  // 右边读取外层 x，内层 x 为 2
}
let original = x;   // 仍为 1
```

新绑定在初始化完成后才可见。分支、循环体、switch 分支和 catch 也有各自的局部作用域。

## 3. 基础类型与字面量

| 类型 | 用途 |
| --- | --- |
| `bool` | `true` 或 `false` |
| `i8`、`i16`、`i32`、`i64` | 指定位宽的有符号整数 |
| `u8`、`u16`、`u32`、`u64` | 指定位宽的无符号整数 |
| `isize`、`usize` | 位宽由目标决定的整数 |
| `f32`、`f64` | IEEE 754 浮点数 |
| `string` | 不可变 UTF-8 字符串 |
| `Unit` | 不携带数据的值类型，常用于无返回数据的函数 |

数字可以写成下面这些形式：

```wz
// 在函数体内
let decimal = 1_000;  // 默认 i64
let binary = 0b1010;
let octal = 0o755;
let hex = 0xFF;
let ratio = 0.25;     // 默认 f64
let scale f32 = 1e3;  // 上下文指定 f32
let small u8 = 10;
```

没有上下文时，整数字面量默认是 `i64`，浮点字面量默认是 `f64`。字面量可以由上下文确定类型，已经有具体类型的变量不会因此自动转换。数字不支持 `123i32` 一类类型后缀；`.5`、`1.` 和 `007` 也不是合法写法。

字符串用双引号，支持换行、制表符和 Unicode 转义；反引号包围的原始字符串不处理转义，可以跨行：

```wz
// 在函数体内
let greeting = "你好";
let lines = "第一行\n第二行";
let same_greeting = "\u{4F60}\u{597D}";
let pattern = `\d+\.\d+`;
let message = `第一行
第二行`;
```

没有单引号字符字面量、字符串插值或 `\xHH` 转义。`string` 是不可变 UTF-8 值，可以保存、复制、传参和放入聚合类型。拼接、相等和排序已经可用；比较按 UTF-8 字节内容进行，不做 Unicode 正规化。

`std/text` 的 `len` 按 Unicode 码点计数，字符串索引也返回 `u32` 码点，切片端点同样是码点位置。需要字节时用 `text.bytes`，从字节建立字符串用 `text.from_bytes`；后者严格检查 UTF-8，失败抛出 `text.Error`。先导入 `std/text`：

```wz
import "std/text" as text;
```

以下写在函数体内：

```wz
let sample = "a你😀";
let count = text.len(sample);            // 3 个码点
let byte_count = text.byte_len(sample);  // 8 个字节
let second = sample[1];                  // 20320，u32
let tail = sample[1:3];                  // "你😀"
let raw = text.bytes(sample);            // Vec[u8]
raw[0] = 98;                             // 修改字节容器不影响 sample
try {
    let restored = text.from_bytes(text.bytes(sample));
} catch error text.Error {
    // 输入不是合法 UTF-8
}
```

`string` 可以用 `+` 拼接，也可以用 `==`、`!=` 和大小比较。`var` 整体绑定支持 `+=`：

```wz
var greeting = "你好";
greeting += "，Woozy";  // 建立新字符串
```

字段、索引和 `let` 绑定的字符串不能直接 `+=`，需要写成 `target = target + suffix`。字符串 `const` 的初始值仍须在编译期确定，`const C = "a" + "b";` 会报错；const 只接受能在编译期确定的字符串值。

## 4. 运算与转换

算术使用 `+ - * / %`，比较使用 `== != < <= > >=`。`%` 只接受整数；具体类型是否支持某种运算由类型检查决定，例如不能因为整数支持 `==` 就推断 Vec 也支持。

逻辑运算使用 `!`、`&&`、`||`，后两者短路求值。位运算使用 `~`、`&`、`|`、`^`、`<<`、`>>`。赋值和复合赋值是语句，不是表达式。

不同具体数值类型不会自动混算。显式转换写作 `目标类型(值)`：

```wz
// 在函数体内
let count i32 = 7;
let wide i64 = i64(count);
let average f64 = f64(count) / 2.0;  // 3.5
let truncated = i32(3.9);           // 3
let negative = i32(-3.9);           // -3
```

整数到整数的转换检查范围；浮点转整数先向零截断，再检查整数范围。能在编译期确定失败就报错，动态越界、NaN 或无穷转整数产生 Panic。

定宽整数的加减乘会回绕。例如：

```wz
// 在函数体内
let highest i8 = 127;
let wrapped = highest + 1;  // -128
let quotient i32 = -7 / 3;  // -2，向零截断
let remainder i32 = -7 % 3; // -1，符号随被除数
```

整数除零和非法移位产生 Panic。浮点除零按 IEEE 754 得到 Inf 或 NaN。浮点运算按 `f32` 或 `f64` 精度执行，不默认启用放宽精度的运算规则。完整数值细节见 [SEMANTICS.md](SEMANTICS.md#4-数值运行语义)。

## 5. 函数与调用

顶层函数声明：

```wz
func add(left i32, right i32) i32 {
    return left + right;
}
```

省略返回类型表示返回 `Unit`，可以直接执行到函数体末尾。非 `Unit` 函数的所有正常结束分支必须返回值。

已声明 `add` 后，在函数体内可以这样调用：

```wz
let positional = add(2, 3);              // 5
let named = add(right: 3, left: 2);       // 5
let mixed = add(2, right: 3);            // 5
```

位置实参必须在命名实参前。被调用者和实参按源码从左到右求值，参数名不会改变副作用的先后顺序。函数不支持默认参数、可变参数或同名重载。

普通参数的绑定只读。需要改一个数值时，先建局部 `var`；修改调用者变量的 `inout` 会在后面介绍。

函数也能作为值传递。下面的声明和调用展示了函数类型及函数字面量：

```wz
func apply(operation func(i32) i32, value i32) i32 {
    return operation(value);
}

func calculate() i32 {
    let twice = func(value i32) i32 {
        return value * 2;
    };
    return apply(twice, 21);  // 42
}
```

函数类型记录参数类型、参数模式、返回类型和错误类型，不保留参数标签；通过函数值调用时只能使用位置实参。

函数字面量可以捕获创建它的函数中的局部变量。捕获发生在创建时：数值等直接值保存当时的副本，Vec、Box 等共享对象仍指向同一个对象。捕获绑定在函数值内只读，但可以通过已有接口修改共享对象的内容。下面的函数返回的读取器离开创建它的作用域后仍然可用：

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

普通 `func` 复制捕获值；`move func(...) { ... }` 会移动所有实际捕获的拥有值，例如 Vec，而数值等直接值仍按普通规则复制。被移动的原绑定在重新初始化前不能读取：

```wz
func moved_capture() i32 {
    var values Vec[i32] = [41];
    var scalar i32 = 1;
    let read = move func() i32 { return values[0] + scalar; };
    scalar = 2;
    return read();  // 42
}
```

方法也可以保存为函数值。创建 `value.method` 时会保存接收者的逻辑副本。之后重新绑定原变量，或替换普通按值字段，不会改变已保存的函数值；但 Vec 等引用字段仍共享同一对象，修改其内容会被看到。move-Self 方法同样可以绑定，绑定值可复制并重复调用，每次调用先复制保存的接收者，再把副本交给方法：

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

inout-Self 方法不能作为函数值保存。捕获的值会随函数值一起复制和释放。

## 6. 分支与循环

条件必须是 `bool`，不会把 `0`、空容器等当成假：

```wz
// 在函数体内
let score i32 = 72;
var passed = false;
if score >= 60 {
    passed = true;
} else {
    passed = false;
}
```

循环统一使用 `for`。三段式循环、条件循环和无限循环分别这样写：

```wz
// 在函数体内
var total i32 = 0;
for var i i32 = 0; i < 4; i += 1 {
    total += i;
}
// total 为 6，i 在循环外不可见

var remaining i32 = 3;
for remaining > 0 {
    remaining -= 1;
}

var attempts i32 = 0;
for {
    attempts += 1;
    if attempts == 2 {
        break;
    }
}
```

`break` 退出最近的循环或 switch，`continue` 跳到下一轮循环。遍历 Vec 使用 `for element in values`，见下一节。

## 7. Vec：列表、索引与切片

`Vec[T]` 是元素类型为 `T` 的容器，列表字面量会建立 Vec：

```wz
// 在函数体内
let values Vec[i32] = [12, 18, 20];
let first = values[0];       // 12
let first_two = values[0:2]; // [12, 18]
let empty Vec[i32] = [];
var total i32 = 0;
for value in values {
    total += value;
}
// total 为 50
```

列表元素必须能确定为同一种元素类型，推断不依赖元素顺序。空列表需要上下文提供类型。切片左闭右开，端点可以省略；新 Vec 复制选中的元素。

Vec 赋值共享同一个容器：

```wz
// 在函数体内
let scores Vec[i32] = [12, 18, 20];
let alias = scores;
alias[0] = 15;               // scores[0] 也变成 15
let copy = scores[0:2];
copy[0] = 99;                // scores[0] 仍为 15
```

`let` 禁止给名称重新赋值，也禁止修改本语言值的字段，但允许修改共享 Vec 的元素。切片复制元素的值；若元素也是 Vec，里面的容器仍会共享，不是递归深复制。

`std/vec` 提供长度和可变操作，使用前写 `import "std/vec" as vec;`。以下写在函数体内：

```wz
let items = vec.empty[i32]();
vec.reserve(items, 8);
vec.push(items, 12);
vec.push(items, 18);
let last = vec.pop(items); // option.Option[i32]
vec.resize(items, 4, 0);   // 长度变为 4，新增元素填 0
let capacity = vec.capacity(items);
vec.clear(items);          // 长度为 0，容量保留
```

`vec.pop` 在空 Vec 上返回 `None`。`resize` 变大时用给定值填充，变小时截断。`clear` 不释放容量。Vec 赋值共享同一个容器，这些操作会改变所有别名看到的内容。

Vec 没有负索引。越界下标、非法切片会产生 Panic。遍历期间可以修改已有元素，不能改变正在遍历的 Vec 的长度或存储；不要在循环里 push、pop、resize、clear 或 reserve。

### 共享单元与弱引用

`std/box.Box[T]` 保存一个可修改的值。复制 Box 会得到指向同一个单元的句柄，`box.get` 读取当前值，`box.set` 替换它：

```wz
import "std/box" as box;

func shared_cell() i32 {
    let cell = box.new[i32](5);
    let alias = cell;
    box.set(alias, 8);
    return box.get(cell);  // 8
}
```

`Weak[T]` 是内建类型，保存不延长目标寿命的弱引用；`std/weak` 只提供 `empty`、`downgrade`、`upgrade` 操作。目标可以是 Vec、函数值、`Box[T]` 或带引用计数的共享句柄；普通 struct/enum 值、string、文件/套接字描述符和外部 Python/C 对象不能直接作为目标。`weak.empty[T]()` 建立空弱引用，`weak.downgrade[T](value)` 从现有强引用建立，`weak.upgrade(w)` 返回 `option.Option[T]`。目标已经没有强引用时升级得到 `None`：

```wz
import "std/option" as option;
import "std/weak" as weak;

func observe_values() i32 {
    var saved Weak[Vec[i32]] = weak.empty[Vec[i32]]();
    {
        let values Vec[i32] = [7];
        saved = weak.downgrade[Vec[i32]](values);
        switch weak.upgrade(saved) {
        case option.Option.Some(strong):
            if strong[0] != 7 { return 1; }
        case option.Option.None:
            return 2;
        }
    }
    switch weak.upgrade(saved) {
    case option.Option.Some(strong):
        return 3;
    case option.Option.None:
        return 0;
    }
}
```

Weak 可以保存到 struct、enum、Vec 和泛型值中，但不能把普通按值聚合当成可弱引用的对象。接口见 [标准库](STDLIB.md)。

由 Vec、Box 和闭包形成的纯 Woozy 强引用环会自动回收；涉及任务或协程帧的环尚未完成，具体范围见 [开发记录](../development/docs/LANGUAGE_DOC_FACTS.md)。

## 8. 结构体与方法

顶层声明一个结构体和它的方法：

```wz
struct Point {
    x i32;
    y i32;
}

impl Point {
    func squared_length(self Self) i32 {
        return self.x * self.x + self.y * self.y;
    }
}
```

结构体字段必须写类型。构造时要初始化全部字段，没有字段默认值。`Self` 表示当前实现的类型，接收者写在方法第一个参数的位置。

声明上面的类型后，在函数体内：

```wz
var point = Point(x: 3, y: 4);
let length = point.squared_length();  // 25
let saved = point;
point.x = 6;
// saved.x 仍为 3，point.x 为 6
```

结构体按字段复制；整数等值字段各有自己的值，Vec 字段复制后仍共享容器。通过 `let` 绑定的结构体不能直接修改字段。

## 9. 枚举与模式匹配

枚举描述几个互斥的情况，每个变体可以携带不同的数据。顶层声明：

```wz
enum Verdict {
    Pass(total i32);
    Fail;
}

func evaluate(total i32) Verdict {
    if total >= 60 {
        return Verdict.Pass(total);
    }
    return Verdict.Fail;
}

func accepted(verdict Verdict) bool {
    switch verdict {
        case Verdict.Pass(total):
            return total >= 60;
        case Verdict.Fail:
            return false;
    }
}
```

无数据变体用 `Verdict.Fail`，带数据变体用构造调用。`case Verdict.Pass(total):` 取出携带的分数，`total` 只在这个分支内可见。

`switch` 不穿透到下一分支，而且必须覆盖全部可能值。上面列全了两个变体；bool 可以列出 `true`、`false`。不能列全的值需要 `default:`、`case _:` 或其他总匹配分支：

```wz
func category(code i32) i32 {
    switch code {
        case 0:
            return 10;
        case 1:
            return 20;
        default:
            return -1;
    }
}
```

主体表达式只求值一次。裸标识符模式总是新建绑定；`case limit:` 不表示“和外面的 limit 比较”。`_` 匹配任何值而不创建可读名称。当前不支持范围模式、guard 或 switch 表达式。

## 10. 普通参数、move 与 inout

普通参数不消耗调用者的值。数值按值复制，Vec 仍指向共享容器，参数名本身只读。下面的函数允许修改 Vec 内容：

```wz
func replace_first(values Vec[i32], value i32) {
    values[0] = value;
}
```

需要修改调用者的变量存储时，双方都写 `inout`：

```wz
func increment(value inout i32) {
    value += 1;
}

func changed() i32 {
    var count i32 = 10;
    increment(inout count);
    return count;  // 11
}
```

`inout` 接受可写 `var` 绑定、已有 `inout` 参数、其字段链和容器元素，例如 `swap(inout state.left, inout state.right)`、`bump(inout values[0])` 或 `grow(inout items[1].count)`。位置可以是数值、bool、递归只含平凡值的有限 struct/enum，也可以是 Vec/Slice 容器本身、string、函数值、Weak、dyn 和含拥有字段的聚合；被调用方可以整体替换它们，旧值恰好释放一次。同一次调用可以对不同具名字段或可证明不同的字面量索引分别借用；重叠路径、整容器访问和动态索引冲突仍会拒绝，`Task` 与外部 C resource 的 `inout` 报 `E_CAPABILITY_UNAVAILABLE`。Vec/Slice 句柄可能通过绑定复制、struct/enum、返回函数或闭包共享同一对象，元素 `inout` 借用期间这些未知别名会被保守拒绝。

转交一个拥有值时使用 `move`：

```wz
func take_first(values move Vec[i32]) i32 {
    return values[0];
}

func transferred() i32 {
    let values Vec[i32] = [12, 18];
    let kept = values;
    let first = take_first(move values);
    // 这里不能再读 values；kept 仍可使用共享容器
    return first + kept[1];  // 30
}
```

移动后，绑定重新初始化之前不能读取。移动不会清空其他别名，也不会深复制容器。普通参数不能直接被移动，必须先取得本地副本。

方法的接收者遵守同样的规则。普通 `value.method()` 不自动移动或取得可写访问；方法声明需要 `move` 或 `inout` 接收者时，调用写作 `(move value).take()` 或 `(inout value).change()`。

## 11. 可声明的错误

`throws E` 表示函数可能抛出 `E` 类型的错误，正常返回值仍是声明的类型：

```wz
enum ScoreError {
    Negative(value i32);
}

func require_score(value i32) i32 throws ScoreError {
    if value < 0 {
        throw ScoreError.Negative(value);
    }
    return value;
}

func checked_sum(scores Vec[i32]) i32 throws ScoreError {
    var total i32 = 0;
    for score in scores {
        total += require_score(score);
    }
    return total;
}
```

`require_score` 抛出的错误会自动从 `checked_sum` 继续传播，调用处不逐次加 `?`。调用者必须声明兼容的 `throws`，或者用 `try/catch` 处理。已声明上面两个函数后，可以写：

```wz
func handled() i32 {
    try {
        return checked_sum([12, -1, 20]);
    } catch error ScoreError {
        switch error {
            case ScoreError.Negative(value):
                return value;  // 本例返回 -1
        }
    }
}
```

错误类型可以是普通枚举，不必实现特殊 Error trait。自行定义的 `Result[T, E]` 也是普通数据，不会自动触发错误传播。标准库的 `std/result.Result[T,E]` 同样是普通 enum，变体是 `result.Result.Ok` 和 `result.Result.Err`；构造泛型 enum 时写完整类型实参，例如 `result.Result[i32,i32].Ok(7)`，跨包匹配写 `result.Result.Ok(value)`。捕获错误不会撤销已经发生的容器修改或外部操作。

越界、整数除零等运行失败称为 Panic，不能由 typed catch 恢复。可预期、需要调用者处理的错误应通过 `throws` 表达。

## 12. defer 与代码块退出

`defer` 在当前代码块退出时执行，后注册的先执行：

```wz
// 在函数体内
var order i32 = 0;
{
    defer { order = order * 10 + 1; }
    defer { order = order * 10 + 2; }
}
// 离开这个块后，order 为 21
```

它属于声明所在的词法块，不会一律等到函数结束。正常退出、`return`、`break`、`continue`、typed throw 都执行离开代码块时的清理；Panic 不保证执行。

`defer` 读取执行时的变量值。返回表达式则在清理前求值并保存：

```wz
func before_cleanup(value inout i32) i32 {
    defer { value += 1; }
    return value;
}

func demonstrate() i32 {
    var value i32 = 10;
    let returned = before_cleanup(inout value);
    // returned 为 10，value 为 11
    return returned + value;  // 21
}
```

需要给 defer 保存快照时，先建一个新绑定。defer 内不能向外 `return`、`break`、`continue` 或传播 typed error；它可以在内部自行处理错误。

## 13. 包、文件与可见性

同一目录里的 `.wz` 文件属于同一个包，必须声明相同的包名。顶层声明在同包文件之间可见，不需要相互 import。

跨包访问使用 import 别名，声明需要加 `pub`。假设 `app` 是项目源码根，`app/stats` 表示根目录下的 `stats/` 包，其中 `stats.wz` 可以写：

```wz
package stats;

pub func passed(total i32) bool {
    return total >= 60;
}
```

另一个包里的文件通过逻辑模块名导入：

```wz
package main;
import "app/stats" as stats;

func main() i32 {
    let passed = stats.passed(72);  // true
    return 0;
}
```

import 别名仅在当前文件有效。导入名不带 `.wz` 后缀；包不能循环导入，但函数之间可以递归调用。顶层可以声明 const、类型、函数、trait 和 impl，当前不允许顶层 `let/var` 动态初始化。

## 14. 泛型与 trait

类型参数放在方括号里。下面是一个泛型结构体和泛型函数：

```wz
struct Pair[T] {
    first T;
    second T;
}

func identity[T](value T) T {
    return value;
}
```

声明后，在函数体内：

```wz
let pair = Pair[i32](first: 12, second: 18);
let explicit = identity[i32](pair.first);  // 12
let inferred = identity(pair.second);     // 从参数推断 T 为 i32，值为 18
```

泛型函数只能执行类型参数已声明支持的操作。需要调用类型上的方法时，用 trait 声明约束，再提供 impl：

```wz
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
```

`smaller[i32](12, 18)` 得到 `12`。泛型 enum 构造也要写完整类型实参，例如 `option.Option[i32].Some(1)`；跨包匹配写 `option.Option.Some(value)`。`std/option.Option` 和 `std/result.Result` 都是普通泛型 enum；在 `std/option` 包内部才可以省略成 `Option.Some(value)`。类型必须显式实现对应 trait，仅有同名方法不够。每个类型参数最多一个具名约束；泛型 trait 可以带类型参数。同一类型可实现不同实参的同一个 trait，调用存在歧义时会报错，不根据预期返回类型猜测。

当前不支持 C++ 模板特化、常量泛型或关联类型。需要运行时多态时，可以让具体值转换成一个 `dyn Trait` 值，再通过 trait 方法调用：

```wz
trait Area {
    func area(self Self) i32;
}

struct Square {
    side i32;
}

impl Area for Square {
    func area(self Square) i32 { return self.side * self.side; }
}

func area_of_shape() i32 {
    let square = Square(side: 3);
    let shape dyn Area = square;
    return shape.area();  // 9
}
```

`dyn Trait` 的方法必须是普通 `self Self` 接收者，所有非接收者参数必须是普通按值参数；`move` 或 `inout` 参数（例如 `move i32`）同样不能用于 dyn。方法不能有自身的泛型参数，也不能返回 `Self`、抛出错误或在非接收者参数中使用 `Self`。每个具体值必须有恰好一个完整 trait 实现。`Vec[dyn Trait]` 可以保存不同具体类型的值。完整规则见 [泛型和 trait](SEMANTICS.md#6-泛型和-trait)。

### 自定义迭代器

实现 `std/iter.Iterator[T]` 后，类型就能用于 for-in。next 通过 inout 修改迭代器，返回 Some 产生一个元素，返回 None 结束：

```wz
package app;
import "std/iter" as iter;
import "std/option" as option;

struct Counter { value i32; limit i32; }

impl iter.Iterator[i32] for Counter {
    func next(cursor inout Self) option.Option[i32] {
        if cursor.value >= cursor.limit {
            return option.Option[i32].None;
        }
        let item = cursor.value;
        cursor.value += 1;
        return option.Option[i32].Some(item);
    }
}

func main() i32 {
    let source = Counter(value: 0, limit: 4);
    var total i32 = 0;
    for item in source { total += item; }
    if source.value != 0 { return 1; }
    return total - 6;
}
```

for 只求值一次，并推进它保存的迭代器副本，所以这里 source.value 仍是 0。副本里的 Vec 等引用字段仍共享对象。要直接推进自己的可写变量，可调用 `(inout cursor).next()`。

next 不能 throws；需要逐项报告错误时，让元素类型 T 为 Result。一个类型若有多个不同 Iterator 实参实现，直接 for-in 会报歧义；带明确 Iterator 约束的泛型函数可选定相应实现。只有同名 next 方法不够，必须实现标准 trait。

## 15. 线程与共享数据

线程入口使用无捕获函数，工作数据通过参数传入。`thread.spawn[A](function, argument)` 创建线程，`thread.join(job)` 等待并取得 i64 结果；创建和等待可能抛出 `os.Error`。

Vec 传给线程后仍共享容器。多个线程读写同一份可变数据时，用 `std/sync` 的 Mutex 保护访问；Condvar 用于等待共享条件，AtomicI64 用于原子计数。引用计数的原子操作不会代替这些同步。完整示例和接口见 [线程与同步](CONCURRENCY.md)。

## 16. 常用标准库

标准库按 `std/...` 导入。下面完整文件用 `std/text` 拆分输入，用 `std/strconv` 解析整数，再用 `std/io` 输出总和：

```wz
package main;
import "std/io" as io;
import "std/os" as os;
import "std/strconv" as strconv;
import "std/text" as text;

func main() throws os.Error {
    let pieces = text.split("12,18,20", ",");
    var total i64 = 0;
    for piece in pieces {
        try {
            total += strconv.parse_i64(piece);
        } catch error strconv.Error {
            io.eprintln("invalid number: " + piece);
            return;
        }
    }
    io.println(total);
}
```

`std/io.println` 补换行，`std/io.eprintln` 写到标准错误。文件读写用 `std/fs`，文件和缓冲 I/O 用 `std/os`、`std/io`，时间、随机、数学、网络、线程和同步见 [标准库](STDLIB.md)、[系统接口](OS.md) 和 [线程与同步](CONCURRENCY.md)。

`std/fs` 的 `read_text`、`write_text` 等函数会打开并关闭文件；直接使用 `os.File` 或 `io.Reader`/`io.Writer` 时，关闭和 `flush` 由调用者负责。详细规则见 [标准库](STDLIB.md)。

## 继续阅读

[快速入门](QUICKSTART.md) 按有经验读者常遇到的差异组织内容。[SEMANTICS.md](SEMANTICS.md) 规定数值、复制、错误和清理的精确行为；[词法与语法](../baseline/LEXICAL_SYNTAX.md) 可查字面量与声明写法，[系统接口](OS.md) 介绍输出、文件读写与进程操作，[标准库](STDLIB.md) 按模块列出接口和资源规则。

章节组织参考了 [Rust Book](https://github.com/rust-lang/book/blob/main/src/SUMMARY.md) 与 [Go Tour 基础篇](https://github.com/golang/website/blob/master/_content/tour/basics.article)。示例和规则以 Woozy 为准。
