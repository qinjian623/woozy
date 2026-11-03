# 词法与语法设计草案 v0.1

日期：2026-11-03  
状态：供评审的语法快照。语言名称与正式文件扩展名未定；示例暂用 `.lang`。

目标是用接近 Go/C 的表层语法表达静态类型程序，同时支持 LLVM 原生后端和 Python 后端。Python 后端优先接入 NumPy 与 PyTorch。常见代码使用类型推断；公开接口、泛型约束和错误传播保持可检查。

本稿规定词法、声明、表达式和语句，并补充避免歧义所需的少量静态语义。ARC、完整借用规则、对象布局、ABI、数值运行语义和 Python 绑定格式需要各自的规范。本稿中的语法检查通过，不代表这些能力已经实现。

## 1 设计范围与决策

### 1.1 已确认的需求

| 需求 | 本稿对应方案 |
| --- | --- |
| 接近 Go/C，语法简单 | 名字在前、类型在后；花括号代码块；统一使用 `func` 与 `for` |
| 静态类型，支持 auto 推断 | `let/var name = expr;`；省略局部类型时推断 |
| 支持泛型 | `Vec[i32]`、`func less[T Ordered](a T, b T) bool` |
| 快速语法检查 | 词法与语法阶段不执行依赖、不启动 Python、不加载 GPU |
| 多平台模块系统 | `import "std/os";` 表示逻辑模块，不表示文件系统绝对路径 |
| Python 端优先 NumPy/PyTorch | 普通模块导入、命名实参、外部类型、可调用对象 |
| 避免每次调用都写问号 | `throws` 声明错误；函数内自动传播；`try/catch` 集中处理 |

### 1.2 本次草案选择

以下是为了形成可实现的文法而选定的规则，仍可在评审后调整。

- 使用显式分号，不自动插入分号。
- 标识符第一版使用 ASCII；字符串和注释使用 UTF-8。
- 使用 `let` 和 `var`；不增加 `auto` 关键字。
- 结构体构造采用 `Point(x: 1.0, y: 2.0)`。
- 泛型使用方括号；不采用尖括号泛型。
- 普通函数没有默认参数或可变参数；外部绑定可以声明哪些参数可省略。
- 一个泛型参数最多写一个具名约束；更复杂约束后续扩展。
- `Result[T, E]` 是普通数据类型；`Err` 值不会自动传播。
- 使用 `defer { ... }` 表示退出当前词法块时执行的清理。
- 函数类型作为返回类型时必须加括号，明确 `throws` 的归属。
- 原生固定大小数组、常量泛型、异步、宏、类继承和完整 FFI 声明暂不纳入本次语法。

### 1.3 文件与验证边界

| 文件 | 用途 |
| --- | --- |
| `LEXICAL_SYNTAX.md` | 本规范，包含规则、理由和示例说明 |
| `grammar.ebnf` | 对应的表层文法 |
| `samples/*.lang` | 完整的正例源文件，各自独立 |
| `samples/negative/*.lang` | 预期拒绝的词法、语法或静态语义示例 |
| `samples/numpy_torch_reference.py` | 手写的 Python 对照程序，不是编译器产物 |
| `tools/check_syntax.py` | 词法与语法检查原型 |
| `VALIDATION.md` | 本次实际完成的验证与尚未验证的能力 |

## 2 源文件与词法

### 2.1 编码与换行

- 源文件必须为有效 UTF-8。
- 接受 LF 与 CRLF，词法分析前统一为 LF；不接受单独的 CR。
- 文件开头可以有一个 UTF-8 BOM；其他位置的 BOM 不作空白处理。
- 原始源文件中不允许 NUL 字符；字符串中的 `\0` 转义另行解码。
- ASCII 空格、制表符和换行在字符串、注释之外作为分隔符；缩进不改变语法。
- 位置诊断记录原始文件的字节区间和行号；LSP 使用的字符位置由适配层转换，不把字节偏移当 UTF-16 偏移。

源码换行标准化也作用于原始多行字符串的物理换行，因此相同源码在 LF/CRLF 文件中产生相同的换行字符。

### 2.2 标识符

```text
Identifier = LetterOrUnderscore, { LetterOrUnderscore | Digit };
LetterOrUnderscore = "A" … "Z" | "a" … "z" | "_";
Digit = "0" … "9";
```

合法示例：`count`、`train_step`、`Tensor2D`、`_temporary`。

标识符区分大小写。`Model` 与 `model` 是不同名称。中文可以出现在字符串和注释中，第一版不用于标识符。

单独的 `_` 是弃置名称或通配模式，不产生可读取的变量。名字以 `_` 开头不会自动改变可见性。`pub` 明确控制导出。

### 2.3 关键字

当前语法使用以下关键字：

```text
as       break    case     catch    const    continue
default  defer    else     enum     false    for
func     if       impl     import   in       inout
let      move     package  pub      return   struct
switch   throw    throws   trait    true     try
unsafe   var
```

为后续功能保留、但本版不接受相应声明的词：

```text
async    await    class    extends  extern   operator
```

`fn`、`while`、`auto` 不是本语言的函数、循环或推断语法。`null` 也不是字面量或关键字；未声明的同名标识符由名称解析报错。`i32`、`f32`、`Self`、`Vec`、`Result` 等属于类型或预定义名称，不属于词法关键字；它们的作用域由名称解析处理。`share` 是内建操作的名称，不是关键字。

### 2.4 注释

```text
// 单行注释，到换行或文件结束。

/* 块注释，
   可以跨行，但不嵌套。 */
```

`///` 与 `/** ... */` 在词法上仍是普通注释；文档工具可以识别其用途。注释相当于分隔符，不能拼接标识符。未结束的块注释是词法错误。

### 2.5 整数字面量

| 形式 | 例子 | 规则 |
| --- | --- | --- |
| 十进制 | `0`、`42`、`1_000` | 多位数字不以 `0` 开头 |
| 二进制 | `0b1010`、`0b1010_0011` | 前缀必须是小写 `0b` |
| 八进制 | `0o755` | 前缀必须是小写 `0o` |
| 十六进制 | `0xFF`、`0xdead_BEEF` | 前缀小写，十六进制数字可大小写 |

下划线只能放在同一数字序列的两个合法数字之间。`0x_FF`、`1__0`、`10_`、`007` 都非法。

负号是独立的一元运算符，`-42` 由 `-` 与整数 token 组成。词法阶段保留整数文本，不先强制存入宿主的有符号整数，避免提前拒绝 `-9223372036854775808` 这类值。

不支持类型后缀：`123i32` 非法。需要指定类型时使用上下文或显式转换：

```text
let small i32 = 123;
let converted = i32(123);
```

### 2.6 浮点字面量与推断

合法形式：

```text
1.0
0.25
1e3
1.0e-3
1_000.25
```

若包含小数点，点的两侧都必须有数字。`.5`、`1.`、十六进制浮点和数字类型后缀不支持。指数中的符号属于指数 token 内容；整个浮点数之前的负号仍是一元运算符。

数字字面量先保留为未定类型常量：

- 有明确上下文时，检查值是否能表示为目标类型，如 `let n u8 = 10;`。
- 没有约束时，整数字面量默认 `i64`，浮点字面量默认 `f64`。
- `let x = 1;` 得到 `i64`，`let x f32 = 1.0;` 得到 `f32`。
- 变量之间的数值转换不因字面量规则而自动成立。
- 溢出、舍入、除法等运行时规则必须另行统一两个后端；不能直接继承宿主 Python 的所有数值行为。

### 2.7 字符串

解释型字符串使用双引号，内部不能直接包含控制字符；换行、制表等使用转义：

```text
"hello"
"你好"
"line 1\nline 2"
"\u{4F60}\u{597D}"
```

仅支持下列转义：

| 转义 | 含义 |
| --- | --- |
| `\\` | 反斜线 |
| `\"` | 双引号 |
| `\n` | 换行 |
| `\r` | 回车 |
| `\t` | 制表符 |
| `\0` | NUL |
| `\u{H…}` | 1 至 6 个十六进制数字表示的 Unicode 标量值 |

Unicode 值必须不超过 U+10FFFF，且不能属于代理区 U+D800–U+DFFF。未知转义、`\xHH` 和不完整转义均为词法错误。

原始字符串以反引号包围，可以跨行，不处理反斜线转义；其中不能出现反引号本身：

~~~text
let pattern = `\d+\.\d+`;
let message = `第一行
第二行`;
~~~

字符串的内容使用 UTF-8 表达。本稿不定义字符串索引究竟返回字节还是 Unicode 标量；相应标准库接口需要单独说明。第一版不提供单引号字符、字节串或字符串插值语法。

### 2.8 运算符与分隔符

按最长匹配识别运算符，例如 `>>=` 是一个 token，而不是 `>>` 加 `=`。

```text
+  -  *  /  %  !  ~  &  |  ^  <  >
== != <= >= && || << >>
=  += -= *= /= %= &= |= ^= <<= >>=
(  )  [  ]  {  }  .  ,  :  ;
```

第一版没有 `++`、`--`、`?`、`??`、`?.`、三元条件运算符、`@` 矩阵乘运算符或 `->` 返回类型箭头。自增写作 `i += 1;`，矩阵乘写作 `torch.matmul(a, b)`。

## 3 文件组织与分号

每个完整源文件必须以一个 package 声明开头。导入位于 package 后、普通声明前。

```text
package demo;

import "std/io" as io;
import "torch/nn" as nn;

const DefaultBatch i64 = 32;

func batch_size() i64 {
    return DefaultBatch;
}
```

package 后使用一个标识符。多文件包、依赖别名和文件系统位置由模块系统规定，不从单个源文件的路径猜测包身份。

简单语句、字段、枚举变体和 trait 方法原型以分号结束；函数、类型、impl 和控制语句的块后不加分号。

```text
let x = 1;
return x;

if enabled {
    run();
}
```

换行不产生隐式分号。`return` 后换行再写表达式，仍是同一条 return 语句，直到遇到分号。格式化器应把这种写法归并到一行。

函数字面量是表达式，因此它所在的变量声明仍需要分号：

```text
let twice = func(x i32) i32 {
    return x * 2;
};
```

## 4 模块导入与可见性

```text
import "std/os";
import "numpy" as np;
import "torch/nn" as nn;
import "app/model" as model;
```

导入路径必须是编译期固定的双引号字符串。路径使用正斜线分隔逻辑模块段，不使用反斜线、绝对路径、`..`、字符串拼接或插值。每段使用 ASCII 标识符形式；依赖包的外部名称可以通过清单映射到合法别名。

省略 `as` 时，使用最后一段作为本地模块名。别名冲突、模块是否存在、目标后端是否支持该模块，属于名称解析和目标检查。

`std` 保留给工具链标准库；`app` 指向当前项目；其他首段由项目声明的依赖别名解析。安装在系统、项目目录或包缓存中的库可以映射到同一逻辑导入名。具体搜索路径不进入源代码。

默认声明仅在包内可见，`pub` 明确导出：

```text
pub func square(x f32) f32 {
    return x * x;
}
```

导入字符串仍可补全：编辑器查询编译器共享的模块索引，不需要把字符串改成另一套语法。

## 5 变量、常量与类型

### 5.1 let、var、const

```text
let count = 10;
let ratio f32 = 0.5;

var step i32 = 0;
step += 1;

const MaxBatch i64 = 128;
```

- `let`：绑定初始化后不能重新赋值。
- `var`：绑定可以重新赋值。
- `const`：初始化式必须可在编译期求值。
- `let` 和 `var` 都必须初始化；不提供隐式未初始化状态。
- 省略类型时推断；显式类型写在名字之后。
- 字段、函数参数与函数返回值必须声明类型；省略函数返回类型表示 `Unit`。
- `let` 不代表外部对象深度不可变。`let model` 仍可通过有相应副作用的库方法更新参数。
- 同一个作用域内不允许重复绑定；嵌套作用域是否遮蔽已有名称由名称检查与诊断策略控制。

推断能力由省略类型提供，不引入重复的 `auto` 写法。

### 5.2 类型形式

第一版提供以下表层类型形式：

```text
i32
f64
torch.Tensor
Vec[i32]
Map[string, Vec[f32]]
(func(i32) i32)
func(inout i32) Unit
func(torch.Tensor) torch.Tensor throws py.Error
```

基本类型包括 `bool`、`i8/i16/i32/i64`、`u8/u16/u32/u64`、`isize/usize`、`f32/f64`、`string` 与 `Unit`。`isize/usize` 的宽度取决于目标，不能假定跨目标固定。`Vec`、`Map`、`Slice`、`Ptr`、`Result` 等是预定义或导入的类型名称，其存在与能力由对应环境定义。

本稿不提供 `T?`、隐式 null、联合类型简写或 C 风格声明符。可空结果可以由普通枚举 `Option[T]` 表达。

### 5.3 列表与切片

```text
let values = [1, 2, 3];
let typed Vec[f32] = [1.0, 2.0];
let empty Vec[i32] = [];
let first = values[0];
let middle = values[1:3];
```

列表字面量推断为 `Vec[T]`，元素必须形成可检查的公共类型。空列表需要上下文确定 `T`。

这是可能分配存储的容器字面量，不承诺是栈上固定数组。`Vec` 的复制、移动和共享规则由所有权规范决定，本稿不把容器赋值定义为隐式深复制。

切片仅支持 `[start:end]`，端点可省略，区间为左闭右开。`[start:end:step]` 不在本版。切片视图、复制、负索引及越界处理是类型相关操作；绑定必须声明其行为。普通下标语法不能保证不同库的索引语义相同。

## 6 函数与调用

### 6.1 声明

```text
func add(a i32, b i32) i32 {
    return a + b;
}

func reset(counter inout i32) {
    counter = 0;
}
```

- 使用 `func`，不使用 `fn` 或 `function`。
- 返回类型位于参数列表之后，不加箭头。
- 非 `Unit` 函数必须在所有正常结束路径上返回值。
- 不支持函数体末尾表达式隐式返回。
- 普通参数、泛型参数、实参和列表字面量允许尾随逗号。
- 第一版不支持源函数的默认参数、参数展开、可变参数与同名重载；外部绑定可为同一个宿主符号提供多个静态签名。

### 6.2 参数模式

```text
func inspect(value Point) f32 {
    return value.x;
}

func increment(value inout i32) {
    value += 1;
}

increment(inout counter);
consume(move buffer);
```

普通参数、`inout` 和 `move` 使用不同的调用契约：

| 形式 | 本稿承诺的含义 |
| --- | --- |
| `value T` | 普通传参，不显式消耗调用者的拥有句柄；具体值传递与借用策略另定 |
| `value inout T` | 可写访问调用者的变量存储；实参需要显式 `inout` |
| `value move T` | 消耗实参的拥有值；已有变量需要显式 `move` |

`inout` 实参必须可寻址且可写；`move` 后能否继续使用变量由所有权检查决定。两者的独占性、别名和借用生命周期不是语法检查器能证明的性质。

`share(value)` 作为显式共享操作的接口名保留给所有权设计；本稿不使用它假装复制 Tensor 存储。外部库可以保留自己的宿主引用，普通参数不隐含 `readonly` 或 `noescape` 保证。

### 6.3 命名实参

```text
let model = nn.Linear(
    128,
    64,
    bias: true,
    dtype: torch.float32,
    device: "cpu",
);
```

位置实参在前，命名实参在后。实参按源码从左到右求值，即使命名参数顺序不同于声明顺序。

- 参数名不存在、漏传必需参数或同一形参被重复提供，属于静态错误。
- 明显重复的命名标签与命名参数后再写位置参数，可在语法检查时诊断。
- 外部绑定必须区分位置限定参数、关键字限定参数、可省略参数与显式 None。
- `func(T) R` 函数值类型不保留参数标签，所以通过函数值调用时仅使用位置实参。
- 直接解析到声明的方法、函数、构造器和外部可调用对象，可以按已知签名使用命名实参。

### 6.4 函数值与 throws 归属

```text
func apply(f func(i32) i32, value i32) i32 {
    return f(value);
}

let twice = func(value i32) i32 {
    return value * 2;
};
```

函数类型记录参数模式、返回类型和错误类型，不记录参数名字。

函数返回另一个函数时，必须把返回的函数类型放进括号：

```text
func make() (func(i32) i32) throws BuildError
func make() (func(i32) i32 throws ComputeError)
func make() (func(i32) i32 throws ComputeError) throws BuildError
```

上面是签名片段，分别表示外层函数可失败、返回的函数可失败、两层都可失败。作为完整声明时仍须函数体。

以下写法拒绝，避免 `throws` 归属含糊：

```text
func make() func(i32) i32 throws ErrorType
```

闭包捕获、引用逃逸和跨语言回调生命周期需要所有权规范补充；支持其语法不等于已经支持所有捕获方式。

## 7 泛型与约束

```text
trait Ordered {
    func less_than(self Self, other Self) bool;
}

impl Ordered for i32 {
    func less_than(self i32, other i32) bool {
        return self < other;
    }
}

func less[T Ordered](a T, b T) bool {
    return a.less_than(b);
}

let left i32 = 3;
let right i32 = 9;
let inferred = less(left, right);
let explicit = less[i32](left, right);
```

`T` 是类型参数，`Ordered` 是具名约束。第一版一个参数最多写一个约束，不加入 `where`、约束交集、关联类型和常量泛型。

泛型类型示例：

```text
struct Pair[T] {
    first T;
    second T;
}

let pair = Pair[i32](first: 3, second: 9);
```

显式类型实参与索引共享方括号外形：

```text
less[i32](3, 9)
callbacks[index](value)
```

语法分析保留中性方括号后缀，名称解析后再判断它表示类型实参、索引或外部多维索引。第一遍 parser 不查询类型表，也不执行库。

因此，某段代码通过 parser，只能说明它可形成候选结构，不能证明方括号里的内容一定是合法类型。涉及函数类型的显式类型实参也必须保留类型候选，而不是一律按普通表达式处理。

对于 `tensor[i, j]` 这类多项下标，是否允许由类型/绑定决定；`Vec[T]` 只接受本稿规定的普通单项下标。本版不提供 NumPy 的全部高级索引语法，如 `...` 与多轴混合切片。

## 8 结构体、枚举与方法

### 8.1 结构体

```text
struct Point {
    x f32;
    y f32;
}

let point = Point(x: 1.0, y: 2.0);
```

结构体构造采用普通调用外形，字段用命名实参提供。第一版字段必须全部初始化，没有字段默认值。是否为结构体构造由名称解析决定。

使用这种形式可以避免 `if` 条件中的结构体字面量与后续代码块争用花括号。字段访问使用 `point.x`；其可写性由变量与类型规则检查。

### 8.2 枚举

```text
enum Color {
    Red;
    Green;
    Blue;
}

enum Option[T] {
    None;
    Some(value T);
}
```

无 payload 的变体按限定名使用，有 payload 的变体按构造调用使用：

```text
let color = Color.Red;
let value = Option[i32].Some(42);
```

`Option`、`Result` 可以由普通枚举定义，不是特殊词法形式。裸标识符模式表示变量绑定；枚举变体必须使用限定名。模式中能从被匹配值唯一确定的泛型实参可以省略，如 `case Result.Ok(value):`；这由类型检查确认。

### 8.3 trait 与 impl

```text
trait SquaredNorm {
    func squared_norm(self Self) f32;
}

impl SquaredNorm for Point {
    func squared_norm(self Self) f32 {
        return self.x * self.x + self.y * self.y;
    }
}
```

- trait 的方法使用原型并以分号结束。
- impl 的方法有函数体。
- `Self` 表示当前实现类型。
- 接收者显式写在第一个参数位置，方法调用时不再显式传入。
- `impl Point { ... }` 定义类型自身的方法。
- `impl[T] Box[T] { ... }` 可以表达泛型实现。
- 第一版 trait 用作静态约束；动态 trait object、继承和对象布局不从这个语法自动获得。
- impl 的方法签名、接收者模式和 `throws` 必须与 trait 要求匹配。

## 9 表达式与运算符

### 9.1 优先级

从高到低：

| 层级 | 运算 | 结合方式 |
| --- | --- | --- |
| 最高 | 调用、成员访问、方括号后缀 | 从左到右 |
| 一元 | `+ - ! ~ move inout` | 从右到左 |
| 乘法 | `* / %` | 从左到右 |
| 加法 | `+ -` | 从左到右 |
| 移位 | `<< >>` | 从左到右 |
| 大小比较 | `< <= > >=` | 从左到右解析 |
| 相等比较 | `== !=` | 从左到右 |
| 位与 | `&` | 从左到右 |
| 位异或 | `^` | 从左到右 |
| 位或 | `\|` | 从左到右 |
| 逻辑与 | `&&` | 从左到右，短路 |
| 逻辑或 | `\|\|` | 从左到右，短路 |

位或与逻辑或在文法文件中按实际 token 表示。本表中的竖线是运算符，不是表格层级标记。

赋值只作为语句，不是表达式，因此 `if x = 1 { ... }` 非法。复合赋值的目标只求值一次。

条件必须为 `bool`。`a < b < c` 不表示数学链式比较；应写 `a < b && b < c`。普通 `if tensor` 不因 Tensor 内容而隐式成立。

### 9.2 求值顺序

- 普通操作数和实参按源码从左到右求值。
- `&&` 和 `||` 保持短路。
- 每个函数调用按照声明的效果传播失败。
- 算术运算符对应的具体类型行为由类型规则或外部绑定决定。
- 原生数值运算与 Tensor 运算不共用一套隐式数据转换。
- `&` 与 `*` 第一版仅列入二元运算；裸地址获取和指针解引用另行设计。

Tensor 的运算符绑定可以声明错误效果：

```text
let delta = prediction - target;
let loss = (delta * delta).mean();
```

这类表达式必须处于可传播对应错误的函数或捕获块中。它不需要逐个 `?`，也不隐式转换为 NumPy。

## 10 控制语句与模式

### 10.1 if 与 for

```text
if ready {
    run();
} else {
    wait_for_input();
}

for {
    process_one();
}

for remaining > 0 {
    remaining -= 1;
}

for var index i32 = 0; index < count; index += 1 {
    process(index);
}

for value in values {
    process(value);
}
```

花括号必须存在。条件外侧的圆括号可作为普通分组使用，但语法不要求它。

统一使用 `for`，不增加 `while`。三段式循环头部的两个分号是分隔符，初始化声明不再额外加一个分号。第一版不支持 `if` 初始化子句、循环标签或多变量解构循环。

`for value in values` 的绑定在每次迭代内有效。迭代器的元素类型、借用关系和可能错误由接口决定。

### 10.2 switch

```text
switch value {
    case Option[i32].Some(number):
        consume_number(number);
    case Option[i32].None:
        handle_empty();
}
```

第一版模式包括：

- 字面量，包括负的数值字面量；
- `_` 通配模式；
- 裸标识符绑定；
- 限定的枚举变体及其 payload 模式。

case 之后使用冒号。分支结束不会落入下一分支。`default:` 表示兜底分支；没有 guard、范围、or-pattern 或 switch 表达式。

裸标识符是绑定，不是常量匹配。例如 `case limit:` 会引入一个绑定，不能把它解释为“与此前名为 limit 的变量比较”。

模式穷尽性、不可达分支、变体是否存在与 payload 类型匹配属于静态语义。检查器可以解析 switch，但不声称已证明这些性质。

### 10.3 跳转

- `return;` 结束 Unit 函数；`return expr;` 返回显式结果。
- `break;` 退出最近的循环或 switch。
- `continue;` 进入最近循环的下一轮。
- 跳转目标是否合法由上下文检查决定。
- 局部普通块不构成新的函数返回边界。

## 11 错误处理

### 11.1 throws 与自动传播

```text
enum InputError {
    Negative(value i32);
}

func require_nonnegative(value i32) i32 throws InputError {
    if value < 0 {
        throw InputError.Negative(value);
    }
    return value;
}
```

`throws E` 是函数类型的一部分。普通返回值仍为声明的 `T`；失败走独立的错误传播路径。

在允许传播 `E` 的函数中，调用 `throws E` 函数可以直接书写。未捕获错误会离开当前函数并执行相应作用域清理。第一版错误枚举不强制额外实现一个 Error trait。

不同本语言错误类型默认不自动转换；可以通过捕获并构造另一错误显式适配。外部错误的分类和子类型匹配由绑定规范声明，不能推导为所有类型通配。

### 11.2 try 与 catch

```text
try {
    let checked = require_nonnegative(input);
    use_value(checked);
} catch error InputError {
    handle_input_error(error);
}
```

一个 try 后必须有至少一个 catch。catch 的错误变量只在该分支中有效。handler 自身产生的错误不由同一 try 的其他 catch 再次捕获，而是交给外层规则处理。

不声明 `throws` 的函数，必须局部处理自己可能产生的可传播错误。这个条件由效果检查验证，parser 本身不能证明。

第一版不另加 finally 语法，作用域清理由 defer 表达。捕获错误不回滚已经发生的 I/O、数组修改或优化器更新。

### 11.3 Result 是普通数据

```text
enum Result[T, E] {
    Ok(value T);
    Err(error E);
}
```

构造 `Result[i32, InputError].Err(...)` 只产生一个值，不触发 throw。返回 Result 的函数也不会自动获得 throws 效果。

本版不定义 `?` 操作符。错误收集与传播之间的转换需要显式接口或匹配，不能根据“周围有没有 throws”改变一个普通 Result 值的含义。

panic、进程中断和普通外部库错误使用不同的处理类别；Python 后端不能用一次捕获全部 BaseException 的操作吞掉所有控制事件。

## 12 defer、unsafe 与所有权边界

### 12.1 defer

```text
defer {
    resource.release();
}
```

defer 在退出最近的词法块时执行，同一块中按后注册先执行。正常块结束，以及 return、break、continue 或可传播错误离开该块时，均执行对应清理。

- return 的结果表达式先求值，再执行离开的作用域中的 defer。
- defer 体在退出时执行；引用到的变量按其绑定在执行时的状态读取。
- 需要保存注册时的值，应先显式创建满足所有权规则的快照。
- 不允许 defer 体跳出该清理体，或向外传播未处理错误。
- defer 体内的嵌套循环可以使用只退出该嵌套循环的 break/continue。
- 进程直接终止或不可恢复的崩溃不提供清理执行保证。

语法检查只能识别 defer 结构。它是否调用了会抛错的函数、是否延长了借用、是否与 move 冲突，需要效果与所有权检查。

### 12.2 unsafe

```text
unsafe {
    perform_declared_unsafe_operation();
}
```

unsafe 标记一个允许受限操作的词法范围，不改变语法解析，也不关闭名称、类型和错误检查。哪些操作需要 unsafe，及其在 Python 目标下是否可用，由对应能力规范决定。

本稿没有定义可执行的 C++ 导入、裸系统调用或指针解引用接口。高性能原生目标仍保留这些方向，但不能仅因代码出现 unsafe 就声称支持了它们。

### 12.3 ARC 与 Python 外部对象

本稿保持以下边界：

- 值、拥有句柄、借用和共享存储是不同概念。
- `let` 的绑定不变性不能代替对象不可变性。
- 外部库返回拥有型句柄，不代表新建了独立数据存储。
- Python 对象遵守宿主管理的生命周期；原生 ARC 析构时机不能直接套用。
- `self Self`、`inout`、`move` 的完整生命周期证明属于后续所有权规范。

## 13 NumPy 与 PyTorch 示例的约定

导入使用普通模块名：

```text
import "std/py" as py;
import "numpy" as np;
import "torch";
import "torch/nn" as nn;
import "torch/nn/functional" as F;
import "torch/optim" as optim;
```

绑定保留真实的 ndarray、Tensor、模型和优化器对象。绑定函数正常返回业务值，并声明 `throws py.Error`；不默认包装每个调用为 Result。

```text
func train_step(
    model nn.Linear,
    optimizer optim.SGD,
    x torch.Tensor,
    target torch.Tensor
) torch.Tensor throws py.Error {
    optimizer.zero_grad(set_to_none: true);

    let prediction = model(x);
    let loss = F.mse_loss(prediction, target);

    loss.backward();
    optimizer.step();

    return loss;
}
```

这是声明片段；完整导入、数据准备和训练样例见 `samples/04_numpy_torch.lang`。`nn.Linear` 的调用签名明确为 Tensor 到 Tensor；不把任意 `nn.Module` 都假定为这个签名。

绑定必须明确下列行为：

| 行为 | 要求 |
| --- | --- |
| `model(x)` | 保留模块实例调用，不能自动替换为 `model.forward(x)` |
| `torch.tensor(ndarray)` | 显式复制导入，样例固定 CPU 与 float32 |
| `torch.from_numpy(ndarray)` | 显式共享存储，受 dtype/布局等条件约束 |
| `clone()` | 复制数据与梯度关系分别描述 |
| `detach()` | 明确的梯度边界，不当作独立存储快照 |
| `np.float32` 与 `torch.float32` | 分属各自库的已声明值，不混成同一种参数类型 |
| 形状与设备 | 先保留运行时信息，不宣称任意 shape 可以静态证明 |
| Python 异常 | 保留有用的原始异常信息与源代码映射 |

样例默认不需要自定义 Python 类。自定义 nn.Module、参数注册、梯度上下文、GPU、混合精度、分布式与 torch.compile 兼容性另设实现阶段。

Python 后端仍需要相应 Python 环境。支持这些绑定不代表同一模块已经可以通过 LLVM 后端独立运行，也不代表将普通 Python 循环自动变成原生循环。

## 14 快速检查与诊断

检查分层如下：

| 阶段 | 可以检查 | 不应声称已证明 |
| --- | --- | --- |
| 词法 | 编码、token、数字形式、转义、注释结束 | 名称存在、类型正确 |
| 语法 | 声明、语句、表达式结构、分号、括号 | 泛型成立、调用签名匹配 |
| 名称与类型 | 导入、成员、实参、返回值、泛型约束 | 外部实际环境与全部运行时行为 |
| 效果与所有权 | throws、move、inout、清理与借用 | 任意外部 Python 别名都被追踪 |
| 目标检查 | 目标是否支持模块和能力 | 设备一定存在、驱动一定兼容 |
| 运行验证 | 对照结果、绑定行为、实际环境 | 所有输入和所有平台都已正确 |

诊断应包含阶段、稳定错误代码、源区间、简明解释和可用时的修复建议。编辑器与命令行使用同一前端。暂缺绑定应明确报告“未覆盖的接口”，不能把未知符号静默视作动态成功。

样例中的语义负例故意可能通过语法检查。例如，调用了未声明 throws 的函数，只能在效果分析中被拒绝。这不是 parser 漏检。

## 15 暂缓的语法

以下能力继续作为语言目标或后续提案，不在本次 EBNF 的可用范围内：

- 固定大小数组的类型语法、常量泛型。
- 异步、生成器与协程。
- 自定义类继承与 Python 外部类定义。
- 宏、预处理器和用户定义的运算符优先级。
- 完整 C/C++/Python 绑定声明语言。
- 裸指针地址获取和解引用。
- 多个泛型约束、where、关联类型和动态 trait object。
- 默认参数、可变参数、参数展开。
- 字符、字节串、字符串插值。
- 用户定义运算符重载、源函数重载。
- switch 表达式、模式 guard、范围和 or-pattern。
- 完整 NumPy 高级索引语法。

这些暂缓项不应在正例中悄悄出现。新增语法必须同步修改文法、正反例与诊断规则。

## 16 参考依据

以下资料用于检查已有语言的表述方式与宿主库行为。本稿的语法选择属于本项目提案，不声称与参考语言兼容。

- [Go Language Specification](https://go.dev/ref/spec)：词法、EBNF、声明与语句的组织方式。本稿采用显式分号与更窄的标识符规则。
- [Python Lexical Analysis](https://docs.python.org/3/reference/lexical_analysis.html)：源代码、token 与换行处理的分层。本语言并不采用 Python 缩进语法。
- [Python Errors and Exceptions](https://docs.python.org/3/tutorial/errors.html)：Python 后端的异常传播行为。
- [NumPy typing](https://numpy.org/doc/stable/reference/typing.html)：外部类型信息及其覆盖边界。
- [PyTorch nn.Module](https://docs.pytorch.org/docs/stable/generated/torch.nn.Module.html)：模型实例调用与 hooks。
- [PyTorch torch.tensor](https://docs.pytorch.org/docs/stable/generated/torch.tensor.html)：显式复制与自动求导历史。
- [PyTorch torch.from_numpy](https://docs.pytorch.org/docs/stable/generated/torch.from_numpy.html)：共享存储。
- [PyTorch Tensor.detach](https://docs.pytorch.org/docs/stable/generated/torch.Tensor.detach.html)：梯度关系与共享存储。

