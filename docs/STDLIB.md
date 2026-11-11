# Woozy 标准库

标准库模块用 `import "std/模块名" as 别名;` 导入。`std/io`、`std/os`、`std/fs`、`std/net`、`std/thread`、`std/sync`、`std/process`、`std/signal`、`std/ipc`、`std/time` 和 `std/random` 使用 Linux 系统接口，目前用于 Linux x86_64 Native 程序。`std/text`、`std/vec`、`std/option`、`std/result`、`std/strconv`、`std/math`、`std/path`、`std/hash`、`std/map`、`std/set` 和 `std/json` 不依赖这些接口，Native 和 Python 后端都可以使用。

下面先给出一个完整程序，后面的模块说明不再重复它的导入和 `main`。

```wz
package main;
import "std/io" as io;
import "std/os" as os;
import "std/strconv" as strconv;
import "std/text" as text;
import "std/vec" as vec;

func main() throws os.Error {
    let pieces = text.split("12,18,20", ",");
    let scores = vec.empty[i64]();
    for piece in pieces {
        try {
            vec.push(scores, strconv.parse_i64(piece));
        } catch error strconv.Error {
            io.eprintln("invalid number: " + piece);
            return;
        }
    }
    var total i64 = 0;
    for score in scores {
        total += score;
    }
    io.println(total);
}
```

运行后输出 `50`。`io.eprintln` 把错误写到标准错误。

## 错误和资源

各模块用 `throws` 标明错误。`os`、`fs`、`io`、`net`、`thread`、`process`、`signal`、`ipc`（创建和数据访问）、`time`（系统调用部分）和 `random` 主要抛出 `os.Error`；`text`、`strconv`、`time`、`option` 另有自己的错误枚举。`math` 不抛错。`sync` 和 `ipc` 的锁误用产生 Panic，不通过 typed catch 处理。

`os.File` 和 `net.Socket` 只是描述符包装。复制 `File` 或 `Socket` 不会调用 `dup`，副本指向同一个描述符；离开作用域不会自动关闭。显式 `close` 一次后，其他副本也不能继续使用。需要独立描述符时用 `os.dup`。`net.file(socket)` 取得同一个描述符的 `os.File`，不要把它和 `Socket` 分别当成两个资源关闭。

`io.Reader` 和 `io.Writer` 也不关闭底层文件。复制 Reader 或 Writer 会共享其中的缓冲区；Writer 的 `pending` 和 Reader 的游标也会被副本看到。需要独立状态时重新调用 `io.reader` 或 `io.writer`。

`std/fs` 的便捷函数会在内部打开并关闭文件。直接使用 `os.open`、`os.pipe` 或 `io.reader`/`io.writer` 时，关闭和 `flush` 由调用者负责。出错路径可以用 `defer` 关闭，并在 defer 内处理关闭错误。`os.exit` 立即结束进程，不执行 defer。

## std/io

输出、整段读取、按行读取和带缓冲的写入。

| 接口 | 说明 |
| --- | --- |
| `print[T Printable](value T) throws os.Error` | 写出值的文本，不补换行 |
| `println[T Printable](value T) throws os.Error` | 写出值的文本并补换行 |
| `eprint` / `eprintln` | 与上两项相同，写到标准错误 |
| `decode(data Vec[u8]) string throws os.Error` | 按 UTF-8 解码；失败抛出 `os.Error.System(84,0)` |
| `read_all(file os.File) Vec[u8] throws os.Error` | 读到文件结束，遇到中断会继续读 |
| `read_text(file os.File) string throws os.Error` | `read_all` 后按 UTF-8 解码 |
| `reader(file os.File) Reader` | 建立按字节读取的 Reader |
| `read_byte(input Reader) option.Option[u8] throws os.Error` | 读一个字节，结束时返回 `None` |
| `read_line_bytes(input Reader) option.Option[Vec[u8]] throws os.Error` | 读一行字节 |
| `read_line(input Reader) option.Option[string] throws os.Error` | 读一行并解码 |
| `writer(file os.File) Writer` | 建立带缓冲的 Writer |
| `flush(output Writer) throws os.Error` | 写出 pending 中的字节 |
| `write(output Writer, data Vec[u8]) throws os.Error` | 追加字节，pending 到 4096 字节时先 flush |
| `write_text(output Writer, value string) throws os.Error` | 追加字符串的 UTF-8 字节 |

`println` 等函数要求类型实现 `Printable`。标准库已经为 `string`、`bool`、各种整数和浮点类型提供实现；自定义类型可以写 `impl Printable for 类型名` 并实现 `to_text`。

`read_line` 去掉行尾的 LF，并去掉 LF 前紧挨着的一个 CR。文件在最后一行后没有 LF 时，剩余数据仍返回 `Some`；没有剩余数据才返回 `None`。Reader 复制后共享缓冲区和游标。非阻塞读行遇到 WouldBlock 时，尚未返回的行前缀留在 Reader 中，下一次 read_line 或 read_byte 可以继续读取。

Writer 必须显式 `flush`。丢弃 Writer 不会自动写出 pending；只用 `write` 写满 4096 字节也不代表最后一段已经写到文件。`flush` 失败时，已经完成的字节会从 pending 移除，剩余字节保留，错误继续抛出。

## std/os

Linux 系统接口。函数名和参数顺序接近 C，不提供原始 syscall 编号。常用接口：

| 接口 | 说明 |
| --- | --- |
| `stdin() File`、`stdout() File`、`stderr() File` | 标准输入、输出、错误 |
| `open(path string, flags i32, mode u32) File throws Error` | 打开文件 |
| `close(file File) throws Error` | 关闭描述符，不自动重试 |
| `read(file File, data Vec[u8], count usize) usize throws Error` | 读一次，返回实际字节数；结束返回 0 |
| `write(file File, data Vec[u8], count usize) usize throws Error` | 写一次，返回实际字节数 |
| `write_all(file File, data Vec[u8]) throws Error` | 写完整个字节容器，遇到中断在原位置重试 |
| `write_text(file File, text string) usize throws Error` | 写一次字符串，返回实际字节数 |
| `write_all_text(file File, text string) throws Error` | 写完字符串 |
| `lseek(file File, offset i64, whence i32) i64 throws Error` | 改变位置并返回新位置 |
| `stat` / `lstat` / `fstat` | 取得 `FileInfo` |
| `unlink`、`mkdir`、`rmdir`、`rename`、`chmod`、`fchmod`、`chdir` | 文件和目录操作 |
| `symlink`、`link`、`truncate`、`ftruncate` | 链接和截断 |
| `fsync`、`fdatasync` | 同步文件数据 |
| `dup(file File) File throws Error`、`dup2(file File, descriptor i32) File throws Error` | 复制描述符 |
| `pipe(flags u32) Pipe throws Error` | 建立管道，得到 `read_end` 和 `write_end` |
| `getpid() i32`、`exit(status i32)` | 进程号和退出 |
| `clock_gettime(clock i32) Timespec throws Error` | 取得时间，`CLOCK_REALTIME` 或 `CLOCK_MONOTONIC` |
| `nanosleep(request Timespec, remaining inout Timespec) throws Error` | 睡眠；被中断时返回剩余时间 |
| `getrandom(data Vec[u8], count usize, flags u32) usize throws Error` | 取得随机字节 |
| `getcwd()` / `getcwd_bytes()` | 当前目录，字节版本保留非 UTF-8 内容 |
| `readlink()` / `readlink_bytes()` | 读符号链接 |
| `getdents(directory File, data Vec[u8], count usize) usize throws Error` | 读目录项 |

打开标志有 `O_RDONLY`、`O_WRONLY`、`O_RDWR`、`O_CREAT`、`O_EXCL`、`O_TRUNC`、`O_APPEND`、`O_NONBLOCK`、`O_CLOEXEC`、`O_DIRECTORY`、`O_NOFOLLOW`，用 `|` 组合。定位使用 `SEEK_SET`、`SEEK_CUR`、`SEEK_END`。权限常量包括 `S_IFMT`、`S_IFREG`、`S_IFDIR`、`S_IFLNK`、`S_IFIFO`。

错误类型：

```wz
pub enum Error {
    Interrupted(code i32, completed usize);
    WouldBlock(code i32, completed usize);
    System(code i32, completed usize);
    NoProgress(completed usize);
}
```

`read` 和 `write` 只执行一次。遇到中断且没有完成字节时抛出 `Interrupted`，不会自动重试。`write_all` 和 `write_all_text` 从已经写出的位置继续，遇到中断时重试；零进展抛出 `NoProgress`，`WouldBlock` 和其他错误立即返回。`completed` 表示失败前已经完成的字节数。`close` 失败不重试，因为描述符可能已经释放。

`read` 和 `write` 使用缓冲区已有长度，`count` 不能超过它；读取不会改变 Vec 的长度，只会改已有元素。空 Vec 只能读写 0 字节。

`File` 离开作用域不会关闭，复制只复制描述符。`os.pipe` 的两个端点都要关闭；关闭写端后，读端读到文件结束。短写、中断和描述符生命周期的更多说明见 [系统接口](OS.md)。

## std/fs

文件、目录和路径项的便捷操作。

| 接口 | 说明 |
| --- | --- |
| `read_file(name string) Vec[u8] throws os.Error` | 读完整文件 |
| `read_text(name string) string throws os.Error` | 读完整文件并按 UTF-8 解码 |
| `write_file(name string, data Vec[u8], mode u32) throws os.Error` | 创建或截断后写入 |
| `write_text(name string, value string, mode u32) throws os.Error` | 写入字符串 |
| `append_file(name string, data Vec[u8], mode u32) throws os.Error` | 追加字节 |
| `copy_file(source string, destination string, mode u32) throws os.Error` | 复制文件 |
| `exists(name string) bool throws os.Error` | 路径存在时返回 `true` |
| `create_dir_all(name string, mode u32) throws os.Error` | 逐级创建目录 |
| `read_dir(name string) Vec[DirEntry] throws os.Error` | 读目录项，不含 `.` 和 `..` |
| `name_text(entry DirEntry) string throws os.Error` | 把目录项名称按 UTF-8 解码 |
| `is_dir(info os.FileInfo)`、`is_file(info)`、`is_symlink(info)` | 判断 `stat` 结果的类型 |

`DirEntry.name` 是 `Vec[u8]`，因为 Linux 文件名不保证是 UTF-8。需要字符串时调用 `name_text`，非 UTF-8 名称会抛出 `os.Error`。目录项的 `kind` 是系统给出的类型字节，跨文件系统时不要只依赖它。

`exists` 只在系统返回“不存在”或“路径中间不是目录”时返回 `false`，其他错误继续抛出。`copy_file` 在截断目标前比较源和目标，源和目标指向同一 inode 时抛出 `os.Error.System(22,0)`。

这些便捷函数会自行打开和关闭文件。完整程序：

```wz
package main;
import "std/fs" as fs;
import "std/io" as io;
import "std/os" as os;
import "std/text" as text;

func main() throws os.Error {
    let name = "/tmp/woozy-stdlib-example.txt";
    fs.write_text(name, "alpha\nbeta\n", 0o644);
    fs.append_file(name, text.bytes("gamma\n"), 0o644);
    io.print(fs.read_text(name));
    os.unlink(name);
}
```

运行后输出三行：`alpha`、`beta`、`gamma`。

## std/text

字符串按 UTF-8 保存，函数按 Unicode 码点处理。

| 接口 | 说明 |
| --- | --- |
| `len(value string) usize` | Unicode 码点数 |
| `byte_len(value string) usize` | UTF-8 字节数 |
| `bytes(value string) Vec[u8]` | 取得字节容器；修改它不改变字符串 |
| `from_bytes(value Vec[u8]) string throws Error` | 严格按 UTF-8 建立字符串 |
| `from_rune(value u32) string throws Error` | 从码点建立字符串 |
| `runes(value string) Vec[u32]` | 拆成码点容器 |
| `starts_with`、`ends_with`、`contains` | 前缀、后缀、包含判断 |
| `find(value string, needle string) option.Option[usize]` | 第一次出现的位置，按码点计 |
| `find_from(value string, needle string, start usize)` | 从指定码点位置开始查找 |
| `split(value string, separator string) Vec[string]` | 拆分；空分隔符按每个码点拆 |
| `join(values Vec[string], separator string) string` | 用分隔符连接 |
| `replace(value string, old string, replacement string) string` | 替换 |
| `repeat(value string, count usize) string` | 重复 |
| `trim(value string) string` | 去掉两端空白 |
| `is_space(rune u32) bool` | 判断空白码点 |
| `ascii_lower(value string) string`、`ascii_upper` | 只转换 ASCII 字母 |

`Error` 有两个变体：`InvalidUtf8` 和 `InvalidScalar(value u32)`。`from_bytes` 遇到不完整或非法的 UTF-8 时抛出 `InvalidUtf8`；`from_rune` 遇到代理区或超出 Unicode 范围的码点时抛出 `InvalidScalar`。

字符串本身可以用 `len`、索引和切片；这些操作都按码点计算，索引结果是 `u32`。字符串不自动做 Unicode 正规化，例如组合字符和预组合字符是不同的字节序列。需要按字节处理时先用 `bytes`，再显式转换回来。

## std/vec

Vec 的长度、容量和可变操作。

| 接口 | 说明 |
| --- | --- |
| `empty[T]() Vec[T]` | 空 Vec |
| `make[T](length usize, fill T) Vec[T]` | 建立指定长度的 Vec，元素都填 `fill` |
| `len[T](values Vec[T]) usize` | 当前长度 |
| `capacity[T](values Vec[T]) usize` | 当前容量 |
| `reserve[T](values Vec[T], minimum usize)` | 保证至少能放下 `minimum` 个元素 |
| `push[T](values Vec[T], value T)` | 末尾追加 |
| `pop[T](values Vec[T]) option.Option[T]` | 去掉并返回末尾元素 |
| `resize[T](values Vec[T], length usize, fill T)` | 改变长度，新增元素填 `fill` |
| `clear[T](values Vec[T])` | 长度变为 0，容量保留 |

Vec 赋值共享同一个容器；`push`、`pop`、`resize` 和 `clear` 会改变所有别名。切片建立新的 Vec，复制选中的元素。`pop` 在空 Vec 上返回 `option.Option.None`。

遍历 Vec 时只能修改已有元素，不能改变长度或存储。循环里不要调用 `push`、`pop`、`resize`、`clear` 或 `reserve`。

## std/option 和 std/result

两者都是普通泛型 enum，不是内建的特殊控制流。文件顶部导入模块：

```wz
import "std/option" as option;
import "std/result" as result;
```

在函数体内构造时要写完整类型实参：

```wz
let some = option.Option[i32].Some(3);
let none = option.Option[i32].None;
let ok = result.Result[i32,i32].Ok(7);
let err = result.Result[i32,i32].Err(2);
```

匹配时可以省略类型实参：

```wz
switch some {
    case option.Option.Some(value): // 使用 value
    case option.Option.None:        // 没有值
}
```

从其他包导入时要写 `option.Option.Some` 和 `option.Option.None`。在 `std/option` 包内部才可以省略包名，写成 `Option.Some`、`Option.None`；`Result` 同理。

`std/option` 的常用接口：

| 接口 | 说明 |
| --- | --- |
| `is_some[T](value Option[T]) bool`、`is_none` | 判断是否有值 |
| `get[T](value Option[T]) T throws Error` | 取值；`None` 抛出 `Error.Empty` |
| `unwrap_or[T](value Option[T], fallback T) T` | 有值取值，否则返回 `fallback` |
| `map[A,B](value Option[A], transform func(A) B) Option[B]` | 转换内部值 |

`std/result` 的常用接口：

| 接口 | 说明 |
| --- | --- |
| `is_ok[T,E](value Result[T,E]) bool`、`is_err` | 判断成功或失败 |
| `unwrap_or[T,E](value Result[T,E], fallback T) T` | 成功取值，否则返回 `fallback` |
| `map[A,B,E](value Result[A,E], transform func(A) B) Result[B,E]` | 转换成功值 |
| `map_err[T,A,B](value Result[T,A], transform func(A) B) Result[T,B]` | 转换错误值 |

`Result` 携带的数据不会自动传播。需要传播时，调用者要显式 `throw`，或者把函数声明成 `throws`。

## std/strconv

数字和字符串之间的转换。

| 接口 | 说明 |
| --- | --- |
| `parse_i64(value string) i64 throws Error` | 解析有符号整数 |
| `parse_u64(value string) u64 throws Error` | 解析无符号整数 |
| `parse_f32(value string) f32 throws Error` | 解析 f32 |
| `parse_f64(value string) f64 throws Error` | 解析 f64 |
| `format_i64(value i64) string` | 格式化有符号整数 |
| `format_u64(value u64) string` | 格式化无符号整数 |
| `format_f32(value f32) string` | 格式化 f32 |
| `format_f64(value f64) string` | 格式化 f64 |

`Error` 有 `InvalidSyntax` 和 `OutOfRange`。语法不符合目标类型时抛出前者，数值超出目标类型范围时抛出后者。`format_*` 不抛错，输出十进制文本。

## std/math

`f64` 数学函数。常量有 `PI` 和 `E`。`Fraction` 有 `fraction f64` 和 `exponent i32` 两个字段，用于 `frexp`。

常用接口按用途分组：

| 用途 | 接口 |
| --- | --- |
| 取整和绝对值 | `fabs`、`floor`、`ceil`、`trunc`、`round` |
| 幂、根、指数、对数 | `sqrt`、`pow`、`exp`、`exp2`、`expm1`、`log`、`log2`、`log10`、`log1p` |
| 三角函数 | `sin`、`cos`、`tan`、`asin`、`acos`、`atan`、`atan2` |
| 组合和分解 | `copysign`、`hypot`、`fmod`、`remainder`、`fma`、`ldexp`、`frexp` |
| 分类和比较 | `isnan`、`isinf`、`isfinite`、`signbit`、`min`、`max` |

浮点参数使用 `f64`，`f32` 值要先用 `f64(value)` 转换。`isnan`、`isinf`、`isfinite`、`signbit` 返回 `bool`；`frexp(value f64)` 返回 `Fraction { fraction f64, exponent i32 }`，`ldexp(value f64, exponent i32)` 接受整数指数；其余函数返回 `f64`。结果遵循 IEEE 754：定义域外的结果可以是 NaN，溢出可以得到无穷，负零和 NaN 参与比较时要按 IEEE 规则处理。`min` 和 `max` 遇到 NaN 时返回 NaN。

## std/time

时间值和 UTC 日期换算。`DateTime` 的字段都是 `i64`：

```wz
pub struct DateTime {
    pub year i64; pub month i64; pub day i64;
    pub hour i64; pub minute i64; pub second i64; pub nanoseconds i64;
}
```

| 接口 | 说明 |
| --- | --- |
| `now() os.Timespec throws os.Error` | 当前实时时间 |
| `monotonic() os.Timespec throws os.Error` | 单调时钟 |
| `sleep(duration os.Timespec) throws os.Error` | 睡眠，被中断时按剩余时间继续 |
| `sleep_ms(milliseconds i64) throws os.Error` | 按毫秒睡眠；负数抛出 `os.Error.System(22,0)` |
| `difference(end os.Timespec, start os.Timespec) os.Timespec throws Error` | 两个时间点的差 |
| `is_leap(year i64) bool` | 是否闰年 |
| `days_in_month(year i64, month i64) i64` | 一个月天数；月份无效返回 0 |
| `to_timestamp(value DateTime) os.Timespec throws Error` | UTC 日期转时间戳 |
| `from_timestamp(value os.Timespec) DateTime throws Error` | 时间戳转 UTC 日期 |
| `weekday(value DateTime) i64 throws Error` | 星期，周日为 0 |
| `format_rfc3339(value DateTime) string throws Error` | 输出 RFC 3339 |
| `parse_rfc3339(value string) os.Timespec throws Error` | 解析 RFC 3339 |

`Error` 有 `InvalidDate` 和 `OutOfRange`。日期按 UTC 和前推格里高利历处理。RFC 3339 输出只在纳秒非零时带小数，小数位为 1 到 9 位；解析接受 1 到 9 位小数，以及 `Z`、`z` 或 `±HH:MM` 偏移，不接受闰秒。

完整程序：

```wz
package main;
import "std/io" as io;
import "std/math" as math;
import "std/os" as os;
import "std/random" as random;
import "std/time" as time;

func main() throws os.Error {
    let point = time.DateTime(year:2024,month:2,day:29,
        hour:12,minute:0,second:0,nanoseconds:0);
    try {
        let stamp = time.to_timestamp(point);
        let utc = time.from_timestamp(stamp);
        io.println(time.format_rfc3339(utc));
    } catch error time.Error {
        io.eprintln("invalid date");
        return;
    }
    let generator = random.seeded(7);
    io.println(random.next_u64(generator));
    io.println(math.sqrt(9.0));
}
```

运行后依次输出 `2024-02-29T12:00:00Z`、一个固定的随机数（种子为 7）和 `3`。

## std/random

随机字节和伪随机数。

| 接口 | 说明 |
| --- | --- |
| `fill(data Vec[u8]) throws os.Error` | 用系统随机字节填满已有 Vec |
| `bytes(count usize) Vec[u8] throws os.Error` | 建立指定长度的随机字节 |
| `secure_u64() u64 throws os.Error` | 系统随机数 |
| `secure_below(bound u64) u64 throws os.Error` | 小于 `bound` 的系统随机数；`bound` 为 0 时抛出错误 |
| `seeded(seed u64) Random` | 用种子建立 xoshiro256++ 生成器 |
| `new() Random throws os.Error` | 用系统随机种子建立生成器 |
| `next_u64(random Random) u64` | 下一个 64 位值 |
| `below(random Random, bound u64) u64 throws os.Error` | 小于 `bound` 的值 |
| `next_f64(random Random) f64` | 区间 `[0,1)` 的浮点数 |
| `shuffle[T](random Random, values Vec[T]) throws os.Error` | 原地打乱 Vec |

`seeded` 和 `below` 等生成器不是密码学安全的随机源；需要密钥、令牌或盐时用 `fill`、`bytes`、`secure_u64` 或 `secure_below`。`Random` 内部状态是一个共享的 Vec，复制 `Random` 会共享同一份生成器状态。

## std/net

套接字接口。地址、端口和常量使用有类型的 C 风格名称，不提供裸地址或原始 syscall。

| 接口 | 说明 |
| --- | --- |
| `Socket`、`Address`、`Accepted`、`Received`、`Pair`、`PollFd` | 套接字、地址和结果结构 |
| `socket(family i32, kind i32, protocol i32) Socket throws os.Error` | 建立套接字 |
| `close(socket Socket) throws os.Error` | 关闭描述符 |
| `fd(socket Socket) i32`、`from_fd(descriptor i32) Socket` | 描述符转换 |
| `file(socket Socket) os.File` | 取得同一个描述符的 File |
| `ip(value string, port u16) Address throws os.Error` | 解析 IPv4 或 IPv6 地址 |
| `inet_pton` / `inet_ntop` | 文本和地址互转 |
| `any4(port u16)` / `any6(port u16)` | 通配地址 |
| `unix_address(name string)` / `unix_abstract(name Vec[u8])` | Unix 地址 |
| `bind`、`connect`、`listen`、`accept`、`getsockname`、`getpeername` | 连接和地址操作 |
| `send`、`recv`、`sendto`、`recvfrom`、`send_all`、`shutdown` | 收发数据 |
| `setsockopt_int`、`getsockopt_int`、`setsockopt`、`getsockopt` | 套接字选项 |
| `socketpair(family i32, kind i32, protocol i32) Pair throws os.Error` | 建立成对套接字 |
| `poll(rows Vec[PollFd], timeout_ms i32) i32 throws os.Error` | 等待事件 |
| `getaddrinfo(hostname string, port u16, family i32) Vec[Address] throws os.Error` | 查询地址列表 |

`Address` 的公开字段是 `family`、`port`、`flags`、`scope` 和 `data`；`Received` 有 `count` 和 `address`，`PollFd` 有 `fd`、`events` 和 `revents`。

常用常量有 `AF_UNIX`、`AF_INET`、`AF_INET6`、`AF_UNSPEC`、`SOCK_STREAM`、`SOCK_DGRAM`、`SOCK_NONBLOCK`、`SOCK_CLOEXEC`、`SOL_SOCKET`、`SO_REUSEADDR`、`SO_ERROR`、`SO_KEEPALIVE`、`SO_RCVTIMEO`、`SO_SNDTIMEO`、`IPPROTO_TCP`、`IPPROTO_IPV6`、`TCP_NODELAY`、`IPV6_V6ONLY`、`SHUT_RD`、`SHUT_WR`、`SHUT_RDWR`、`MSG_PEEK`、`MSG_DONTWAIT`、`MSG_WAITALL`、`MSG_TRUNC`、`POLLIN`、`POLLOUT`、`POLLERR`、`POLLHUP`、`POLLNVAL`。

`send` 和 `recv` 只执行一次；`send_all` 会继续发送剩余字节，遇到中断时重试。`recvfrom` 返回 `Received`，其中 `count` 是实际收到的字节数，`address` 是发送方地址。`Socket` 复制只复制描述符，不会自动关闭；`net.file` 和 `Socket` 指向同一个描述符，不能分别关闭。

下面的完整程序用 `socketpair` 在两个端点之间传四个字节：

```wz
package main;
import "std/io" as io;
import "std/net" as net;
import "std/os" as os;
import "std/vec" as vec;

func main() throws os.Error {
    let pair = net.socketpair(net.AF_UNIX, net.SOCK_STREAM, 0);
    defer { try { net.close(pair.first); } catch error os.Error {} }
    defer { try { net.close(pair.second); } catch error os.Error {} }
    net.send_all(pair.first, [112,105,110,103], 0);
    let buffer = vec.make[u8](4, 0);
    let count = net.recv(pair.second, buffer, 4, 0);
    io.println(count);
    io.println(buffer[0]);
}
```

运行后输出 `4` 和 `112`。

## std/path

不访问文件系统的路径处理。

| 接口 | 说明 |
| --- | --- |
| `is_absolute(value string) bool` | 是否以 `/` 开头 |
| `clean(value string) string` | 合并 `/`，处理 `.` 和 `..` |
| `join(left string, right string) string` | 连接并清理；left 为空或 right 为绝对路径时返回 clean(right) |
| `base(value string) string` | 最后一段 |
| `dir(value string) string` | 去掉最后一段 |
| `extension(value string) string` | 扩展名，包含点；没有时返回空字符串 |

这些函数只处理字符串，不检查路径是否存在。

## std/thread 和 std/sync

`std/thread.spawn[A](entry func(A) i64, argument A) Thread throws os.Error` 创建线程，`join(worker Thread) i64 throws os.Error` 等待并取得结果。入口必须是无捕获函数，局部变量不能通过闭包带入；需要的数据用参数传入。Thread 不 join 也能继续运行，main 返回会结束整个进程。

`std/sync` 提供 `mutex`、`lock`、`unlock`、`condvar`、`wait`、`notify_one`、`notify_all`，以及 `atomic_i64`、`load`、`store`、`fetch_add`、`compare_exchange`。Mutex 不可递归；错误解锁等误用产生 Panic。共享 Vec 等可变数据仍由程序员用这些同步工具保护，原子引用计数只保护对象存活。

线程参数、等待和共享数据的完整示例见 [线程与同步](CONCURRENCY.md)。

## std/process

启动、替换和等待进程。下列接口均声明 `throws os.Error`。

| 接口 | 说明 |
| --- | --- |
| `spawn(path string, arguments Vec[string]) i32` | 使用当前环境和标准流，返回子进程 PID |
| `spawn_with(path, arguments, env, input, output, error) i32` | 明确指定环境 Vec[string] 与三个 i32 描述符；-1 继承 |
| `spawnp(command string, arguments Vec[string]) i32` | 按 PATH 查找程序 |
| `execve(path string, arguments Vec[string], env Vec[string])`、`execv(path, arguments)` | 替换当前进程，成功不返回；execv 使用当前环境 |
| `fork() i32` | 父进程取得子 PID，子进程取得 0 |
| `wait(pid i32) WaitStatus`、`waitpid(pid i32, options i32) WaitStatus` | 等待子进程；wait 自动重试 Interrupted |
| `args()`、`args_bytes()` | `Vec[string]` 或原始 `Vec[Vec[u8]]` 参数，含第 0 项程序名 |
| `environment()`、`environment_bytes()` | `NAME=value` 条目组成的 Vec，返回副本 |
| `getenv(name)`、`getenv_bytes(name)` | `option.Option[string]` 或 `option.Option[Vec[u8]]` |
| `setenv(name, value, overwrite bool)`、`unsetenv(name)` | 修改环境；另有 setenv_bytes、unsetenv_bytes |

arguments 必须自己包含第 0 项，调用不会解释 shell 命令字符串。spawn 在 exec 失败时报告真实系统错误并回收失败子进程；成功启动后仍须等待。WaitStatus 包含 pid 和原始 status，使用 `exited`/`exit_status`、`signaled`/`term_signal` 等函数读取。

字符串接口严格检查 UTF-8，非 UTF-8 参数或环境抛出 `os.Error.System(84,0)`。`execve_bytes`、`spawn_bytes`、参数及环境的 bytes 版本保留原字节；程序路径、参数、环境条目都不能含 NUL。环境名不能为空，也不能含 `=`。

多线程 fork 后只保留调用线程，用户业务锁不会自动解锁。继承的 ARC 引用计数不会减去已消失线程持有的引用，部分引用可能保留到 exec 或进程退出；子进程通常应尽快 exec。完整程序、重定向和 fork 规则见 [进程与信号](PROCESSES.md)。

## std/signal

提供同步信号等待，不提供异步调用 Woozy 函数的信号处理器。下列函数均声明 `throws os.Error`。

| 接口 | 说明 |
| --- | --- |
| `bit(number i32) u64` | 信号对应的位；用 `|` 合并 |
| `sigprocmask(how i32, mask u64) u64` | 修改调用线程的屏蔽集合，返回原集合 |
| `sigpending() u64` | 待处理的信号集合 |
| `sigwaitinfo(mask u64) Info` | 同步等待并取走一个信号 |
| `sigtimedwait(mask u64, timeout_ns i64) Info` | 纳秒超时；0 只检查，-1 无限等，超时抛 WouldBlock |
| `signal(number i32, action Action)` | 只接受 `signal.Action.Default` 或 `signal.Action.Ignore` |

先屏蔽要等待的信号，再调用 wait。Info 提供 number、code、pid、uid、status、value；字段含义依信号种类而定。信号常量、线程屏蔽集合和完整片段见 [进程与信号](PROCESSES.md#同步等待信号)。发送信号使用 `os.kill(pid, signal)`。

## std/ipc

Memory 是共享映射；复制它共享同一映射，当前进程最后一个引用清理时解除映射。共享区只能保存字节和数值，不保存 string、Vec 或其他 ARC 对象的指针。

| 接口 | 说明 |
| --- | --- |
| `map_anon(bytes usize, protection i32)`、`map_file(file, bytes, offset, protection)` | 建立 Memory，throws os.Error；PROT_READ/PROT_WRITE 可组合 |
| `shm_open(name, flags, mode)`、`shm_unlink(name)` | 打开或删除命名共享对象，throws os.Error |
| `len(memory)` | 映射的字节长度 |
| `read_bytes`、`write_bytes`、`read_i32`、`write_i32`、`read_i64`、`write_i64` | 检查范围、权限和对齐的数据访问，throws os.Error |
| `atomic_load`、`atomic_store`、`atomic_add`、`atomic_compare_exchange` | 共享 i64 的顺序一致原子操作，throws os.Error |
| `mutex_init` / `cond_init` / `sem_init` | 在共享区域初始化一次，throws os.Error |
| `mutex_at` / `cond_at` / `sem_at` | 使用已初始化的对象，不重新初始化 |
| `mutex_lock` / `mutex_unlock` | 非递归互斥锁 |
| `cond_wait` / `cond_timedwait` / `cond_signal` / `cond_broadcast` | 条件等待和通知；timedwait 超时 false，返回时仍持锁 |
| `sem_wait` / `sem_timedwait` / `sem_trywait` / `sem_post` / `sem_value` | 计数信号量；timedwait 超时或 trywait 无计数时 false |
| `mutex_destroy` / `cond_destroy` / `sem_destroy` | 全部使用者结束后显式销毁同步对象 |

正常锁、等待和通知不声明 throws；错误解锁、销毁仍使用中的对象等误用产生 Panic。这些锁不会在持有者退出后自动恢复。共享映射的范围和数值对齐错误通过 os.Error 返回。

Memory 的自动清理不代替同步对象的显式 destroy，也不会删除共享对象名称。File 和 Socket 仍须分别按描述符只关闭一次。匿名、命名映射及完整跨进程程序见 [共享内存与 IPC](IPC.md)。

## std/hash、std/map 和 std/set

Map 的键需要实现 `hash.Hash`，包含 `hash(value Self) u64` 和 `equals(left Self, right Self) bool`。标准库为 string、bool 和各种整数提供实现。相等的键必须有相同哈希值；键留在 Map 内时，不要修改影响 hash/equals 的内容。默认哈希不用于密码或签名。

```wz
package main;
import "std/io" as io;
import "std/os" as os;
import "std/map" as map;
import "std/option" as option;
import "std/set" as set;
func main() throws os.Error {
    let counts = map.new[string,i32]();
    map.insert(counts,"cat",3);
    io.println(option.unwrap_or(map.get(counts,"cat"),0));
    let names = set.new[string]();
    set.add(names,"cat");
    set.add(names,"cat");
    io.println(set.len(names));
}
```

输出 `3` 和 `1`。Map 支持 new、len、insert、get、contains、remove、clear、keys、values、entries。insert 返回旧值 Option[V]，remove 返回被删除的值；Set 的 add/remove 返回是否真的增加或删除了键。

复制 Map/Set 共享同一个容器；keys/values/entries 返回快照，元素仍按原类型的复制规则处理。操作耗时依赖哈希分布，碰撞时会比较键；快照顺序不作保证。多线程读写仍需加锁。

## std/json

`parse(source string)` 和 `serialize(value Value)` 都 `throws json.Error`。Value 有 Null、Bool、String、Number、Array 和 Object；Array 存 Vec[Value]，Object 存 Vec[Member]，Member 字段为 name 和 value。

Number 保存十进制原文，不先转换成 f64。number_text 返回 Option[string]；as_i64/as_u64/as_f64 显式转换，失败抛错。整数转换要求整数字面量文本。手工构造的 Number 也会检查语法，拒绝 NaN、Inf、前导加号等无效 JSON 数字。

```wz
package main;
import "std/io" as io;
import "std/os" as os;
import "std/json" as json;
import "std/option" as option;
func main() throws os.Error {
    try {
        let value = json.parse("{\"count\":9007199254740993}");
        switch json.field(value,"count") {
            case option.Option.Some(number): io.println(json.as_i64(number));
            case option.Option.None: io.println("missing count");
        }
        io.println(json.serialize(value));
    } catch error json.Error { io.eprintln("invalid JSON"); }
}
```

输出完整整数 `9007199254740993`，写回的 JSON 也保留原数字文本。field/index 返回 Option[Value]。解析重复对象键时保留最后一个值，位置保持第一次出现的位置；序列化手工构造的 Member 列表时保留所有成员。

解析检查数字、控制字符和 Unicode 转义，Syntax(offset) 的位置按码点计数。数组与对象最多嵌套 256 层，超过时抛 DepthLimit；循环容器序列化也会报错。ARC 不回收强引用循环，程序需要自行拆开它们。Error 还有 InvalidNumber 和 NumberRange。

## 尚未提供

捕获闭包、默认参数、模板特化和通用 Ptr 仍未支持。
