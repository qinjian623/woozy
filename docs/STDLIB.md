# Woozy 标准库

标准库模块用 `import "std/模块名" as 别名;` 导入。`std/io`、`std/os`、`std/fs`、`std/net`、`std/http`、`std/http_server`、`std/async`、`std/thread`、`std/sync`、`std/rwlock`、`std/cancel`、`std/channel`、`std/task`、`std/process`、`std/signal`、`std/ipc`、`std/time`、`std/timezone` 和 `std/random` 用于 Linux x86_64 和 macOS arm64 的 Native 程序。`std/text`、`std/unicode`、`std/vec`、`std/sort`、`std/slice`、`std/box`、`std/weak`、`std/iter`、`std/option`、`std/result`、`std/strconv`、`std/math`、`std/path`、`std/hash`、`std/map`、`std/set` 和 `std/json` 在 Native 和 Python 后端都可以使用。`std/tls` 和 `std/https` 用于明确选择 hosted 运行库和 OpenSSL 附加库的 Native 程序，配置见下文。

`std/algo`、`std/deque`、`std/heap`、`std/fmt`、`std/regex` 和 `std/csv` 有 Linux Native 和 Python 用例。`std/csv_io` 和 `std/json_io` 提供 Linux Native 文件读写接口。

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

各模块用 `throws` 标明错误。`os`、`fs`、`io`、`net`、`thread`、`process`、`signal`、`ipc`（创建和数据访问）、`time`（系统调用部分）和 `random` 主要抛出 `os.Error`；`text`、`strconv`、`time`、`timezone`、`option`、`cancel`、`channel`、`task` 和 `http` 另有自己的错误枚举；`http_server` 沿用 `os.Error`。`math` 和 `unicode` 不抛错。`sync`、`rwlock` 和 `ipc` 的锁误用产生 Panic，不通过 typed catch 处理。

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
| `write_byte(output Writer, value u8) throws os.Error` | 追加一个字节，缓冲已满时先 flush |
| `write_text(output Writer, value string) throws os.Error` | 追加字符串的 UTF-8 字节 |

`println` 等函数要求类型实现 `Printable`。标准库已经为 `string`、`bool`、各种整数和浮点类型提供实现；自定义类型可以写 `impl Printable for 类型名` 并实现 `to_text`。

`read_line` 去掉行尾的 LF，并去掉 LF 前紧挨着的一个 CR。文件在最后一行后没有 LF 时，剩余数据仍返回 `Some`；没有剩余数据才返回 `None`。Reader 复制后共享缓冲区和游标。非阻塞读行遇到 WouldBlock 时，尚未返回的行前缀留在 Reader 中，下一次 read_line 或 read_byte 可以继续读取。

Writer 必须显式 `flush`。丢弃 Writer 不会自动写出 pending；只用 `write` 写满 4096 字节也不代表最后一段已经写到文件。`flush` 失败时，已经完成的字节会从 pending 移除，剩余字节保留，错误继续抛出。

`write_byte` 需要先 `flush` 且刷新失败时，本次传入的字节还没进缓冲。恢复写入后可重试该字节。

## std/os

有类型的系统接口。函数名和参数顺序接近 C。常用接口：

| 接口 | 说明 |
| --- | --- |
| `stdin() File`、`stdout() File`、`stderr() File` | 标准输入、输出、错误 |
| `open(path string, flags i32, mode u32) File throws Error` | 打开文件 |
| `openat(directory, name, flags, mode)` | 从已打开的目录描述符打开一个目录项 |
| `mkdirat(directory, name, mode)` | 在已打开的目录中创建子目录 |
| `unlinkat(directory, name, kind)` | 删除目录项，`kind` 指定普通项或目录 |
| `fstatat(directory, name, follow)` | 读取目录项信息，`follow` 控制是否跟随末尾符号链接 |
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

`openat`、`mkdirat`、`unlinkat` 和 `fstatat` 的 `name` 只接受一个名称，拒绝空串、`.`、`..`、斜杠和 NUL。`_bytes` 版本接收 `Vec[u8]`，用于保留 Linux 的非 UTF-8 名称。调用不会关闭传入的目录描述符。

`File` 离开作用域不会关闭，复制只复制描述符。`os.pipe` 的两个端点都要关闭；关闭写端后，读端读到文件结束。短写、中断和描述符生命周期的更多说明见 [系统接口](OS.md)。

## std/fs

文件、目录和路径项的便捷操作。

| 接口 | 说明 |
| --- | --- |
| `read_file(name string) Vec[u8] throws os.Error` | 读完整文件 |
| `read_text(name string) string throws os.Error` | 读完整文件并按 UTF-8 解码 |
| `write_file(name string, data Vec[u8], mode u32) throws os.Error` | 创建或截断后写入 |
| `write_text(name string, value string, mode u32) throws os.Error` | 写入字符串 |
| `write_file_atomic(name, data, mode)`、`write_text_atomic(name, value, mode)` | 先写入同一目录的临时文件，再用重命名替换目标 |
| `append_file(name string, data Vec[u8], mode u32) throws os.Error` | 追加字节 |
| `copy_file(source string, destination string, mode u32) throws os.Error` | 复制文件 |
| `copy_dir(source string, destination string) throws os.Error` | 在 Linux Native 复制目录树，目标必须尚不存在 |
| `exists(name string) bool throws os.Error` | 路径存在时返回 `true` |
| `create_dir_all(name string, mode u32) throws os.Error` | 逐级创建目录 |
| `remove_dir_all(name string) throws os.Error` | 删除实际目录及内容。后代符号链接只删除链接本身 |
| `read_dir(name string) Vec[DirEntry] throws os.Error` | 读目录项，不含 `.` 和 `..` |
| `name_text(entry DirEntry) string throws os.Error` | 把目录项名称按 UTF-8 解码 |
| `is_dir(info os.FileInfo)`、`is_file(info)`、`is_symlink(info)` | 判断 `stat` 结果的类型 |
| `create_temp(directory, prefix) TempFile throws os.Error` | 在指定目录创建临时文件，返回已打开的 `file` 和 `path` |
| `create_temp_dir(directory, prefix) string throws os.Error` | 创建临时目录并返回路径 |
| `temp_dir() string throws os.Error` | 返回非空的 `TMPDIR` 或 `/tmp` |
| `walk(root) Vec[WalkItem] throws os.Error` | 列出根路径及其后代，超过默认条目上限时抛出 `EOVERFLOW` |
| `walk_with(root, options) WalkResult throws os.Error` | 指定深度、条目数和子项错误的处理方式 |

`DirEntry.name` 是 `Vec[u8]`，用于保留系统返回的文件名字节。需要字符串时调用 `name_text`，非 UTF-8 名称会抛出 `os.Error`。目录项的 `kind` 是系统给出的类型字节，跨文件系统时不要只依赖它。

`exists` 只在系统返回“不存在”或“路径中间不是目录”时返回 `false`，其他错误继续抛出。`copy_file` 在截断目标前比较源和目标，源和目标指向同一 inode 时抛出 `os.Error.System(22,0)`。

临时文件和目录用随机后缀，分别以 `0600` 和 `0700` 请求权限。调用者负责关闭临时文件并删除文件或目录。`walk` 解析调用者给出的根符号链接。后代符号链接按一项返回，悬空根链接报 `ENOENT`。`WalkEntry.name` 是字符串，无法按 UTF-8 解码的名字通过 `WalkItem.NameError` 返回原始字节。遍历期间若其他进程修改了目录，结果可能变化或抛出系统错误。

`walk_with` 返回 `items` 和 `truncated`。默认上限为 100,000 项，根项和错误项都计入上限。达到上限且仍有条目时 `truncated` 为 `true`。正数 `max_depth` 用于过滤更深项，设为 0 时不限制深度。`nonfatal_child_errors` 为 `true` 时，子项的 I/O 错误用 `WalkItem.IoError` 返回。

原子写入在替换前设置最终权限、同步并关闭临时文件。目标是符号链接时，替换链接本身。已打开旧文件的描述符仍可读取旧内容。出错时尝试删除临时文件，并保留引发操作失败的错误。函数只同步文件，断电后重命名可能丢失。

`remove_dir_all` 在 Linux Native 用目录描述符和原始文件名字节遍历。根路径必须指向实际目录，根符号链接会报错。操作失败时已经删除的内容不会恢复。每层目录占用一个描述符，超出进程限额会返回 `EMFILE`。调用期间应保持目录树稳定，最后一次身份检查和删除之间仍有一次按名称操作。

`copy_dir` 复制常规文件、子目录和符号链接。文件名和链接目标保留原始字节，源目录的普通权限位会复制。复制期间应保持源目录和目标父目录稳定。目标父目录须已存在，且不能位于源目录内。每层打开一对目录描述符，超出进程限额时返回 `EMFILE`。写入失败会留下已复制的部分，由调用者决定如何处理。

`read_file`、`write_file` 这类直接读写接口会关闭自己打开的文件。下面的程序写入三行并输出。

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
| `find_last(value string, needle string) option.Option[usize]` | 最后一次出现的码点位置，允许重叠。空子串返回字符串长度 |
| `count(value string, needle string) usize` | 统计不重叠的匹配。空子串匹配所有码点边界，返回长度加一 |
| `split(value string, separator string) Vec[string]` | 拆分；空分隔符按每个码点拆 |
| `split_n(value string, separator string, limit usize) Vec[string]` | 最多返回 `limit` 段，最后一段保留剩余内容。限制为 0 时返回空 Vec，为 1 时返回包含原文的 Vec。空分隔符按码点拆 |
| `split_lines(value string) Vec[string]` | 识别 LF、CRLF 和 CR，保留空行，结尾的换行符不额外产生一行。空串返回空 Vec |
| `join(values Vec[string], separator string) string` | 用分隔符连接 |
| `replace(value string, old string, replacement string) string` | 替换 |
| `repeat(value string, count usize) string` | 重复 |
| `trim`、`trim_start`、`trim_end` | 分别去掉两端、开头、结尾的空白 |
| `strip_prefix`、`strip_suffix` | 匹配时去掉一次前缀或后缀，返回 `Option[string]` |
| `is_space(rune u32) bool` | 判断空白码点 |
| `ascii_lower(value string) string`、`ascii_upper` | 只转换 ASCII 字母 |

`Error` 有两个变体：`InvalidUtf8` 和 `InvalidScalar(value u32)`。`from_bytes` 遇到不完整或非法的 UTF-8 时抛出 `InvalidUtf8`；`from_rune` 遇到代理区或超出 Unicode 范围的码点时抛出 `InvalidScalar`。

字符串本身可以用 `len`、索引和切片；这些操作都按码点计算，索引结果是 `u32`。字符串不自动做 Unicode 正规化，例如组合字符和预组合字符是不同的字节序列。需要按字节处理时先用 `bytes`，再显式转换回来。

`split_whitespace(value)` 按 Unicode 空白拆分，略过空字段。`pad_left`、`pad_right`、`pad_center` 按码点数填充空格，`*_with` 版本可选填充码点。无效填充值或宽度超过 `MAX_PADDING_WIDTH` 时抛出 `PaddingError`。

## std/fmt

`fmt.format(template, values)` 把 `Vec[string]` 中的值填入模板。`{}` 依次取值，`{0}` 取第一个值。一个模板中使用一种编号方式。`{{` 和 `}}` 输出字面花括号。`{:8}` 和 `{:<8}` 按 8 个码点左对齐，`{:>8}` 右对齐，`{:^8}` 居中。例如 `fmt.format("{:>5}", ["42"])` 返回 `"   42"`。宽度 `0` 合法，`08` 这类零填充写法会报错。

单次结果至多 100 万码点和 400 万 UTF-8 字节。模板错误、缺少字段或输出超限时抛出 `fmt.Error`，位置按模板码点计数。

## std/regex

正则表达式在 Native 和 Python 后端使用同一实现。可先编译一个模式，再搜索多段文本。

| 接口 | 说明 |
| --- | --- |
| `compile(pattern string) Pattern throws Error` | 编译模式。有余量时同时记录捕获组，语法错误报告码点位置 |
| `compile_captures(pattern string) Pattern throws Error` | 要求记录捕获组，超出限制时直接报 `PatternTooLarge` |
| `find(compiled Pattern, value string) option.Option[Match]` | 返回开始位置最早的匹配，同一起点取最长结果。字段 `start`、`end` 是左闭右开的码点下标 |
| `find_from(compiled, value, from)`、`find_all(compiled, value)` | 从指定码点查找，或收集所有不重叠的匹配 |
| `is_match(compiled Pattern, value string) bool` | 是否存在匹配 |
| `split`、`split_n`、`replace_all`、`replace_n` | 按匹配拆分，或用给定字符串替换匹配。带 `_n` 的写法接受数量上限 |
| `find_captures`、`find_captures_from` | 取得匹配和捕获组。用 `group` 读取区间，用 `group_text` 读取原文 |

支持字面字符、`.`、`^`、`$`、分组 `(...)`、分支 `|`、重复 `* + ?`、字符类 `[a-z]` 和反向字符类 `[^x]`。可转义元字符，也可用 `\n`、`\r`、`\t`。锚点匹配整个字符串的首尾，点号跳过 LF。`regex.find(regex.compile("中+"), "a中中b")` 的匹配区间是 `[1, 3)`。

匹配器逐码点推进状态集合，模式最多 4096 个码点，分组最多嵌套 128 层。捕获限 32 组。组号 0 是整段匹配，后续组从 1 开始。分组数或状态数超过捕获限额时，`compile` 仍可编译符合普通匹配限额的模式，此时捕获接口会抛出 `CaptureError.Unavailable`。反向引用、环视、`{m,n}` 和模式标志尚未提供。 [正则匹配规则](../stdlib/regex/README.md)

## std/unicode

使用固定的 Unicode 18.0.0 数据进行正规化、完整默认大小写折叠和字素簇分段。`VERSION` 是 `"18.0.0"`。这些操作不改变普通字符串长度、索引和切片的定义：它们仍按 Unicode 码点计算；字素簇是另外提供的分段方式，也不等同于终端显示宽度。

| 接口 | 说明 |
| --- | --- |
| `normalize(value string, form Form) string` | 按 `Form.NFC`、`NFD`、`NFKC` 或 `NFKD` 正规化 |
| `nfc`、`nfd`、`nfkc`、`nfkd` | 对应正规化形式的简写，参数和结果都是 string |
| `casefold(value string) string` | 完整默认大小写折叠，可把一个码点展开成多个；不按用户地区选择规则 |
| `upper(value string)`、`lower(value string)` | 按 Unicode 默认规则转换大小写，支持多码点展开和希腊词尾 σ 的转换 |
| `boundaries(value string) Vec[usize]` | 字素簇边界，按码点位置计，包含 0 和字符串末尾；空串返回 `[0]` |
| `grapheme_count(value string) usize` | 字素簇数量；空串为 0 |
| `graphemes(value string) Vec[string]` | 逐个字素簇拆分；空串返回空 Vec |

以下片段位于函数内，需导入 `std/unicode` 为 `unicode`、`std/text` 为 `text`：

```wz
let value = "e\u{301}";
let points = text.len(value);                 // 2 个码点
let clusters = unicode.grapheme_count(value); // 1 个字素簇
let composed = unicode.nfc(value);            // "é"
let folded = unicode.casefold("Straße");      // "strasse"
let bounds = unicode.boundaries("🇨🇳e\u{301}"); // [0, 2, 4]
```

正规化和 casefold 都需要主动调用；字符串比较不会自动执行它们。它们保留嵌入的 NUL。Native 和 Python 均支持这些接口，使用相同版本的数据。

`upper` 和 `lower` 保留嵌入的 NUL，使用统一的默认规则，不读取系统语言设置。 [大小写数据来源](../stdlib/unicode/CASE_PROVENANCE.md)

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
| `insert(values, index, value)` | 在下标处插入，后续元素右移。下标等于长度时追加 |
| `remove_at(values, index)` | 删除并返回指定元素，后续元素左移 |
| `swap_remove(values, index)` | 删除并返回指定元素，用末尾元素填空位 |
| `extend(values, extra)` | 追加 `extra` 中的元素。两个参数共享容器时，追加调用前的原有元素 |

Vec 赋值共享同一个容器；`push`、`pop`、`resize` 和 `clear` 会改变所有别名。切片建立新的 Vec，复制选中的元素。`pop` 在空 Vec 上返回 `option.Option.None`。

遍历 Vec 时只能修改已有元素，不能改变长度或存储。循环里不要调用 `push`、`pop`、`resize`、`clear` 或 `reserve`。

## std/sort

给 `Vec[T]` 传入比较函数 `less func(T,T) bool`。比较函数须满足严格弱序，例如整数用 `left < right`，并且不能修改正在排序或查找的 Vec。

| 接口 | 说明 |
| --- | --- |
| `sort_by(values, less)` | 原地稳定归并排序。相等元素保持原顺序，其他 Vec 别名也会看到变更。最坏耗时 O(n log n)，额外空间 O(n) |
| `is_sorted_by(values, less) bool` | 检查是否已按同一比较规则排序 |
| `lower_bound_by(values, value, less) usize` | 在已排序的 Vec 中找第一个不小于目标的位置，没有则返回长度 |
| `upper_bound_by(values, value, less) usize` | 找第一个大于目标的位置，没有则返回长度 |
| `equal_range_by(values, value, less) Range` | 返回 `start`、`end`，表示与目标等价的半开区间 |
| `binary_search_by(values, value, less) option.Option[usize]` | 找到第一个与目标等价的位置，没有则返回 `None` |
| `sort_by_key(values, key, less)` | 按键原地稳定排序。按输入顺序取键，每个元素只调用一次 `key` |

例如，导入 `std/sort` 后，`sort.sort_by(values, func(a i32, b i32) bool { return a < b; });` 会将 `Vec[i32]` 升序排列。

排序回调执行期间，试图通过别名改变原 Vec 的长度或存储会触发 Panic。排序返回后可以继续追加或删除元素。

## std/algo

按输入顺序处理 `Vec[T]`。Native 和 Python 后端都支持。

| 接口 | 说明 |
| --- | --- |
| `map(values, transform)`、`filter(values, keep)` | 返回新 Vec，分别转换或保留元素 |
| `fold(values, initial, combine)` | 逐元素调用 `combine(累积值, 元素)`，空 Vec 返回 `initial` |
| `find(values, predicate)`、`find_index(values, predicate)` | 返回第一个匹配的元素或下标，找不到返回 `None` |
| `any(values, predicate)`、`all(values, predicate)` | 在结果可确定时结束。空 Vec 分别返回 `false`、`true` |
| `min_by(values, less)`、`max_by(values, less)`、`min_max_by(values, less)` | 按比较函数取最值，相等时保留最先出现的值。空 Vec 返回 `None` |

回调可以替换输入中的现有元素，遍历期间不能改变 Vec 长度或存储。返回的元素遵循普通复制规则，Vec 等共享对象仍指向同一份数据。

## std/deque 和 std/heap

`deque.new[T]()` 创建双端队列。用 `push_front` 或 `push_back` 放入元素，用 `pop_front` 或 `pop_back` 取出元素，空队列返回 `None`。两端添加的摊还成本为 O(1)。`deque.to_vec` 按队列顺序复制元素。 [Deque 接口](../stdlib/deque/README.md)

`heap.new[T](before)` 创建优先队列。比较函数 `before(left, right)` 返回 `true` 时，左侧元素优先弹出。用 `push` 添加，用 `peek` 查看堆顶，用 `pop` 弹出堆顶。比较和元素复制的成本固定时，弹出耗时 O(log n)，`heap.from_vec(values, before)` 批量建堆耗时 O(n)。元素排队期间应保持比较所用的值。两种容器复制后共享原容器。 [Heap 接口](../stdlib/heap/README.md)

## std/slice

`Slice[T]` 是已有 Vec 的视图，Native 和 Python 后端都支持。视图保留底层 Vec，使它在原变量离开作用域后仍然存活；复制 Slice 也共享这些元素。

| 接口 | 说明 |
| --- | --- |
| `view[T](values Vec[T], start usize, end usize) Slice[T]` | 建立半开区间 `[start, end)` 的视图 |
| `sub[T](value Slice[T], start usize, end usize) Slice[T]` | 按当前视图的相对位置建立子视图 |
| `len[T](value Slice[T]) usize` | 视图的元素个数 |
| `to_vec[T](value Slice[T]) Vec[T]` | 建立独立 Vec，按普通规则复制视图中的元素 |

Slice 可索引、修改已有元素和遍历；修改会被原 Vec 及其他视图看到。`s[start:end]` 在 Slice 上产生子视图，在 Vec 上则复制元素到新的 Vec。`to_vec` 也只按普通规则复制元素，元素中的 Vec 等引用对象仍共享底层对象。

范围须满足 `start <= end <= len`，索引和范围越界产生 Panic。视图存活期间，任何 Vec 别名改变长度或扩容都会产生 Panic；空视图也保留这个限制。最后一份相关视图释放后，Vec 才能再次改变长度或扩容。

## std/box

`Box[T]` 是保存一个可修改值的共享单元，Native 和 Python 后端都支持。字段不公开，公开操作只有：

| 接口 | 说明 |
| --- | --- |
| `new[T](value T) Box[T]` | 建立单元并按普通规则复制 value |
| `get[T](value Box[T]) T` | 返回当前值的普通副本 |
| `set[T](value Box[T], next T)` | 保存 next 的普通副本，清理原先保存的值 |

复制 Box 后，两个变量共享同一单元，通过任何副本调用 `set` 都能被其他副本看到。`get` 不执行深复制，返回值里的 Vec 等引用对象仍共享底层对象。给一个 Box 变量重新赋值，只改变该变量指向的单元，不改变其他别名。

跨线程访问同一可修改单元时，程序需要自行加锁。Box 不增加自动同步。可以通过 `std/weak` 建立 Box 的弱引用；纯 Woozy 强引用循环会自动回收。

## std/iter

`Iterator[T]` 是 for-in 使用的标准 trait，声明 `func next(cursor inout Self) option.Option[T]`。for 只对迭代目标求值一次，按普通规则复制迭代器并推进副本；引用字段仍共享对象。next 不抛错，可用 Result 作为元素类型。完整示例见[自定义迭代器](TUTORIAL.cn.md#自定义迭代器)。

## std/async

异步函数返回 `Task[R,E]`，调用时不执行函数体。`spawn` 把任务加入就绪队列，`run`/`run_until` 推进执行器；`await` 只能写在 async 函数里，等待并驱动目标任务，未 spawn 的任务会直接启动，同一个 Task 可重复等待。`sleep`、`readable` 和 `writable` 返回可等待任务，`cancel`、`shutdown`、`finished`、`join` 和 `join_all` 控制等待与取消。完整规则见[async 与 await](CONCURRENCY.md#async-与-await)。

在 Linux Native，`gather(tasks)` 按输入顺序收集结果，成员失败时取消余下任务并等待清理。`or_throw(await group)` 取得成功结果，或把组返回的有类型错误抛给调用者。 [任务组示例](CONCURRENCY.md#等待一组任务)

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

## std/timezone

从 TZif 时区文件读取转换规则，在 POSIX 时间戳和当地日期之间换算。`load(name)` 使用系统的 `/usr/share/zoneinfo`，不内嵌一份时区数据库；程序运行的机器需要安装所需数据。

| 接口 | 说明 |
| --- | --- |
| `load(name string) Zone throws Error` | 例如 `load("Asia/Shanghai")` |
| `load_from(directory string, name string) Zone throws Error` | 从指定目录读取时区文件 |
| `parse(name string, data Vec[u8]) Zone throws Error` | 从 TZif 字节建立 Zone；name 是保存的名称 |
| `name(zone Zone) string` | 返回建立 Zone 时使用的名称 |
| `to_local(zone Zone, instant os.Timespec) Local throws Error` | 时间戳转当地日期、UTC 偏移、夏令时标志和缩写 |
| `from_local(zone Zone, date time.DateTime, resolution Resolution) os.Timespec throws Error` | 当地日期转时间戳，明确指定重复时刻如何选择 |

`Local` 的公开字段是 `date time.DateTime`、`offset_seconds i32`、`daylight bool` 和 `abbreviation string`。偏移为当地时间减 UTC 的秒数。下面只有导入和函数声明；调用方须处理 `timezone.Error`：

```wz
import "std/os" as os;
import "std/timezone" as timezone;

func epoch_in_shanghai() timezone.Local throws timezone.Error {
    let zone = timezone.load("Asia/Shanghai");
    return timezone.to_local(zone, os.Timespec(seconds: 0, nanoseconds: 0));
    // 当地日期为 1970-01-01 08:00:00，offset_seconds 为 28800。
}
```

夏令时开始时，某些当地时刻不存在，转换抛出 `Error.Nonexistent`，不会把时间向前挪。结束时可能出现重复时刻：`Resolution.Reject` 抛出 `Error.Ambiguous(earliest, latest)`，两个载荷是候选 POSIX 秒数；`Earlier` 和 `Later` 分别选择较早和较晚的时间戳。没有歧义时三种选择结果相同。

其他错误：`InvalidName` 表示 load/load_from 的名称无效；`InvalidData(offset)` 表示 TZif 数据不合法，位置按输入字节计；`Io(error os.Error)` 保留打开或读取失败；`OutOfRange` 表示日期、纳秒、计算范围或该时区的有效覆盖范围不满足要求。TZif 的 `-00` 缩写表示当地时间未定义，转换同样抛出 `OutOfRange`，不能当作 UTC。没有延续规则时，最后一个转换时刻及之后也不作猜测。

解析支持 TZif v1/v2/v3/v4 和有效的 POSIX 尾部延续规则，文件大小最多 1 MiB。带闰秒记录的数据抛出 `UnsupportedLeapSeconds`；本模块使用 POSIX 秒，不提供闰秒时间轴。

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

macOS 上用 `unix_address` 创建文件路径形式的 Unix socket。抽象地址需要 Linux。把 `SOCK_CLOEXEC` 传给 `accept` 时，macOS 要求监听 socket 设置 `SOCK_NONBLOCK`。

`getaddrinfo` 在两端都直接处理数字 IP 和 `localhost`。解析其他主机名时，Linux 读取 `/etc/hosts` 和 `/etc/resolv.conf`，macOS 交给系统解析器，按系统配置选择 DNS，包括 VPN 的按域名分流规则。返回的地址和顺序取决于主机的解析器配置。

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

## std/http

HTTP/1.0、HTTP/1.1 消息的解析和编码，以及已连接 Socket 上的读写。建立连接、监听和关闭使用 `std/net`。

| 接口 | 说明 |
| --- | --- |
| `reader(socket net.Socket, limits Limits) Reader` | 建立共享读取状态，保留未处理的字节；复制 Reader 共享状态 |
| `read_request(reader Reader) Request throws Error` | 读取一个请求，保留后续消息的字节 |
| `read_response(reader Reader, request_method string) Response throws Error` | 读取一个响应；须传对应请求方法，例如 `"GET"` 或 `"HEAD"` |
| `read_request_head(reader)`、`read_request_body(reader)` | 只读请求头，再完成同一请求正文；顺序错误报 `ReadOrder` |
| `read_request_body_part(reader, max_bytes) BodyPart throws Error` | 读已解码的正文片段，最多返回 `max_bytes` 字节 |
| `read_response_head(reader, request_method)`、`read_response_body(reader)` | 只读响应头，再完成同一响应正文 |
| `encode_response_head(response, request_method)` | 只编码状态行、头和 Content-Length；调用方随后发送正好 `response.body` 字节 |
| `parse_request(data Vec[u8]) Request throws Error`、`parse_response(data, request_method)` | 解析完整消息，多余字节报 `TrailingBytes`；另有带 Limits 的 `_limited` 版本 |
| `encode_request(request Request) Vec[u8] throws Error`、`encode_response(response, request_method)` | 编码为字节，正文使用 Content-Length 定长发送 |
| `write_request(socket, request) throws Error`、`write_response(socket, response, request_method)` | 编码后完整写出，系统错误包装为 `Error.Io(os.Error)` |

`default_limits()` 限制初始头部为 8192 字节、解码后正文为 1 MiB、头字段总数为 100；用 `limits(head_bytes, body_bytes, header_count)` 修改。trailers 另受头部字节限制，并计入头字段总数。超过限制分别报 `HeadTooLarge`、`BodyTooLarge`、`TooManyHeaders`。

`Header` 包含 `name string`、`value Vec[u8]`；`Response.reason`、请求和响应的 `body` 也都是字节。用 `text_header(name, value)` 从文本创建头字段；`header`/`header_values` 按不区分 ASCII 大小写的名称取字节值，`header_text`/`header_text_values` 和 `value_text` 才进行 UTF-8 解码，失败报 `InvalidUtf8`。

解码支持定长和 chunked 正文，响应也支持读到连接关闭。`read_*_head` 返回空的正文和 trailers，Reader 停留在正文读取阶段。`Request.trailers` 和 `Response.trailers` 独立保存尾部字段。`encode_request`、`encode_response` 和 `encode_response_head` 使用定长编码，收到非空 trailers 时抛出 `UnsupportedTrailers`。`encode_response_head` 只返回头部，不复制正文。HTTP/1.1 请求要求唯一且合法的 Host，缺少、重复或无效分别报 `MissingHost`、`DuplicateHost`、`InvalidHost`。同时包含 Transfer-Encoding 与 Content-Length 报 `TransferEncodingWithContentLength`。

以下只有导入和普通函数声明，接收已连接的阻塞 Socket，处理一个请求后关闭。函数负责关闭，调用方不要再次关闭同一描述符：

```wz
import "std/http" as http;
import "std/net" as net;
import "std/os" as os;
import "std/text" as text;
import "std/vec" as vec;

func serve_once(socket net.Socket) throws http.Error {
    defer { try { net.close(socket); } catch error os.Error {} }
    let input = http.reader(socket, http.default_limits());
    let request = http.read_request(input);
    var body = text.bytes("hello\n");
    if request.method == "HEAD" { body = vec.empty[u8](); }
    let response = http.Response(
        version: "HTTP/1.1", status: 200, reason: text.bytes("OK"),
        headers: [http.text_header("Connection", "close")],
        body: body, trailers: vec.empty[http.Header]());
    http.write_response(socket, response, request.method);
}
```

非阻塞读取遇到 `Error.Io(os.Error.WouldBlock(...))` 时，保留同一个 Reader，等 `net.poll` 报告可读后重试同一种 read 操作；read_response 还须保留相同的 request_method。不要重新建立 Reader，否则会丢掉已读取的消息部分。消息进行中切换请求/响应读取会报 `ReaderBusy`。协议错误后应关闭连接，不继续使用该 Reader；消息尚未完成即结束报 `UnexpectedEof`。

非阻塞写出遇到 WouldBlock 后，不能直接从头重调 write_request/write_response；若需恢复，应先 encode 并保留字节与已写偏移，结合错误中的 completed、`net.poll` 可写事件继续发送。TLS 连接使用下面的 `std/https`；它通过 `reader_external`、`write_request_on` 和 `write_response_on` 使用同一个 HTTP 解析器和读取状态。

读完 `read_request_head` 后，可以用 `read_request_body_part(reader, max_bytes)` 逐段读取定长或 chunked 正文。已解码的字节可以少于上限，函数不会为了填满而再读一次。`max_bytes` 为 0 时抛出 `ReadOrder`。`BodyPart.done` 表示消息结束，此时可取 `trailers`。遇到 `WouldBlock` 时保留 Reader 并重试同一读法。读取同一消息时只能选择逐段或整段接口之一。

发送 chunked 响应时，用 `encode_response_chunked_head(response, request_method)`、`encode_response_chunk_data(data)` 和 `encode_response_chunk_end(trailers)` 依次编码。头部函数要求 `response.body` 和 `response.trailers` 为空，正文和尾部字段分别由后两个函数编码。 [HTTP 帧格式和重试规则](../stdlib/http/README.contract.md)

## std/http_server

基于 `std/http`、`std/net` 和 Native 异步执行器的 HTTP/1.1 服务器。Linux Native 提供普通 handler、异步 handler 和流式 handler。

| 接口 | 说明 |
| --- | --- |
| `default_config() Config` | 默认限制、backlog、连接数、keep-alive、TCP_NODELAY、超时和大正文阈值 |
| `listen(address, handler) Server throws os.Error` | 用默认配置监听 |
| `listen_with(address, handler, config) Server throws os.Error` | 指定限制、backlog、连接数、每连接请求数、超时和时间片 |
| `listen_async(address, handler)`、`listen_async_with(address, handler, config)` | handler 返回 `Task[http.Response,http.Error]` |
| `listen_async_with_settings(address, handler, config, settings)` | 用 `default_async_settings()` 取得默认配置，再按字段调整 handler 超时 |
| `listen_stream(address, handler)`、`listen_stream_with(address, handler, config)` | handler 先收到请求头和 `Stream`，可以逐段读写 |
| `listen_stream_with_settings(address, handler, config, settings)` | 用 `default_stream_settings()` 取得默认配置，再按字段调整流式超时 |
| `read(stream, max_bytes)` | 等待读到一段解码后的正文。最后一段的 `done` 为 true，并带尾部字段 |
| `start(stream, response)` | 发送一份完整的定长响应 |
| `start_chunked(stream, response)`、`write_chunk(stream, data)`、`finish(stream, trailers)` | 发送 chunked 响应头、正文字节和结束块 |
| `local_address(server) net.Address throws os.Error` | 查询监听地址 |
| `active_connections(server) usize`、`accepted_connections(server) u64`、`stats(server) Stats` | 当前/累计连接、成功响应请求数、成功响应字节数（含 100 Continue）、错误、超时和 503 拒绝计数 |
| `config(server) Config` | 读取配置 |
| `serve(server) Unit throws os.Error` | async 接受循环；每个连接一个任务 |
| `stop(server)` | 优雅停止接受；空闲连接立即关闭，在途请求继续到 drain 截止时间 |
| `abort(server)`、`close(server)` | 立即关闭监听并取消活动连接，连接任务负责关闭各自的描述符 |
| `drain(server, timeout_ms) bool` | 停止并等待所有连接任务退出；正超时到期返回 false |
| `text_response(status, body)`、`bytes_response(status, body)` | 构造带默认 reason 的响应 |

复制 `Server` 会共享监听 Socket 和计数；任一副本调用 `stop`/`abort`/`close` 都会影响所有副本。服务器状态和连接表不做同步，只能在拥有它的执行器线程里使用。每个 Server 只应调用一次 `serve`。同步 handler 类型是 `func(http.Request) http.Response throws http.Error`。异步 handler 用普通函数返回请求任务，任务等待 I/O 时其他连接可以继续。同一连接仍逐个处理请求。handler 抛出 `http.Error` 时返回 500 并关闭连接。应用自有错误应在 handler 内捕获并返回响应。

监听 Socket 和接受的连接都是非阻塞、close-on-exec；读写遇到 `WouldBlock` 时，连接任务挂起等待执行器的可读/可写事件，不阻塞线程。单个 watchdog 任务按 `timer_tick_ms` 扫描连接表，执行 header/body/idle/write 超时和 graceful drain 截止时间；超时用 `CLOCK_MONOTONIC`，只对连接调用 `shutdown`，由连接任务负责关闭描述符。超时按绝对截止时间计算，不会在每次活动时重置。`Expect: 100-continue` 在读取正文前返回 100；不支持的 expectation 返回 417。

普通响应使用 `Content-Length`。服务器会移除 handler 设置的 `Connection` 头，按请求版本和连接令牌决定 keep-alive 或 close；handler 明确要求 close 时关闭连接。HEAD、1xx、204 和 304 不发送正文；HEAD/304 缺少 `Content-Length` 时用 handler 正文长度补齐。普通响应中 handler 设置的 `Content-Length` 会被实际正文长度替换。handler 设置 `Transfer-Encoding`、非空 trailers 或非法状态码时，编码失败并返回 500。协议错误返回 400、408、413、417、431、501 或 505 并关闭；EOF 和 I/O 失败直接关闭。连接数超过 `max_connections` 时，新连接收到 503 后关闭；`max_requests` 到达上限后发送 close。正文超过 `large_body_threshold` 时先用 `encode_response_head` 编码头部再单独写出正文，避免完整复制一次正文。

执行器是单线程的，handler 中的长时间 CPU 计算、阻塞文件 I/O 或阻塞 C 调用会阻塞所有连接。接受 32 个连接或处理 1024 个 keep-alive 响应后，任务让出 1 毫秒，避免繁忙的监听循环或流水线饿死其他描述符。普通 handler 返回完整的 Response。请求正文按 `Config.limits` 全量缓冲。完整规则见 [服务器文档](../stdlib/http_server/README.md)。

```wz
import "std/http" as http;
import "std/http_server" as server;
import "std/net" as net;
import "std/os" as os;

func route(request http.Request) http.Response throws http.Error {
    return server.text_response(200,"hello");
}

async func main() i32 throws os.Error {
    let listener = server.listen(net.any4(8080), route);
    await server.serve(listener);
    return 0;
}
```

### 流式读写

流式 handler 收到请求头后开始工作。使用 `read` 时每次指定正数上限，已解码的数据可以少于这个值。定长和 chunked 请求都可逐段读取。`start_chunked` 接收正文和尾部字段为空的响应。客户端若尚有请求体未读完，或任一方发送 `Connection: close`，服务器发完响应后关闭连接。

同一个 `Stream` 同时只允许一个读写操作。重叠操作和 handler 结束后再使用该流都会抛出 `http.Error.ReadOrder`。

`StreamSettings` 的三个时限以毫秒计。读按一次 `read` 调用计时，处理按 handler 开始后或两次 I/O 之间的间隔计时，写按一次公开写调用计时。局部读写进度不会延长当前调用的时限。非正值关闭对应时限。启用优雅停止后，阶段截止时间还受 `drain_timeout_ms` 限制。正文大小上限仍取自 `Config.limits`。

下面的 handler 逐段读完 POST 正文，然后发送两个响应块。

```wz
package app;

import "std/http" as http;
import "std/http_server" as server;
import "std/io" as io;
import "std/net" as net;
import "std/os" as os;
import "std/text" as text;

async func reply(stream server.Stream) Unit throws http.Error {
    for true {
        let part = await server.read(stream, 4096);
        if part.done { break; }
    }

    await server.start_chunked(stream, server.text_response(200, ""));
    await server.write_chunk(stream, text.bytes("hello "));
    await server.write_chunk(stream, text.bytes("from Woozy\n"));
    await server.finish(stream, []);
}

func route(request http.Request, stream server.Stream) Task[Unit, http.Error] {
    return reply(stream);
}

async func main() i32 throws os.Error {
    let listener = server.listen_stream(net.ip("127.0.0.1", 0), route);
    defer { server.close(listener); }
    io.println(server.local_address(listener).port);
    await server.serve(listener);
    return 0;
}
```

## std/tls 和 std/https

这两个模块在 Native 后端使用 OpenSSL 3。项目须选择 hosted 运行库，并明确声明 TLS 附加库；普通 freestanding 程序不会因此依赖 OpenSSL 或 C++ 运行库：

```toml
[build.native]
runtime = "hosted"

[dependencies.woozy_tls]
kind = "native"
binding = "<包含libwoozy_tls.a的目录>/tls.json"
libraries = ["ssl", "crypto", "stdc++"]
library_dirs = ["<包含libwoozy_tls.a的目录>"]
```

macOS arm64 上用生成的 Darwin 绑定，将 `stdc++` 换成 `c++`，并把 OpenSSL 的 `lib` 目录加入 `library_dirs`。TLS 附加库的编译方法见 [stdlib/tls/addon/README.md](../stdlib/tls/addon/README.md)。它单独编译，不加入默认运行库。

`std/tls` 包装调用方已有的 Socket。`Context` 和 `Stream` 的普通复制共享同一个对象，最后一份释放 TLS 对象；Socket 始终由调用方关闭，包括调用 `tls.close` 之后。

| 接口 | 说明 |
| --- | --- |
| `client_default() Context throws Error`、`client_with_ca(ca_file string)` | 使用系统默认信任库或指定 PEM CA 文件 |
| `server(certificate_chain_file string, private_key_file string) Context throws Error` | 加载服务端证书链和私钥 |
| `client(context, socket, name, kind) Stream throws Error`、`server_stream(context, socket)` | 创建 TLS 流，尚未握手 |
| `connect(context, socket, name, kind) Stream throws Error`、`handshake_all(stream)` | 使用 `net.poll` 等待并完成握手 |
| `handshake(stream)`、`shutdown(stream)` | 返回 `Status.Ok`、`WantRead`、`WantWrite` 或 `Eof`；错误抛出 `tls.Error` |
| `read(stream, data, capacity) Transfer throws Error`、`write(stream, data, offset, length)` | 读写字节，返回状态和本次完成的字节数；缓冲区长度须足够 |
| `wait(stream, status)`、`write_all(stream, data)`、`close(stream)` | 等待重试、完整写出、双向正常关闭；均可能抛出 `tls.Error` |

客户端始终检查证书链、有效期和服务端身份，没有跳过验证的接口。`NameKind.Auto` 区分 IP 字面量与 DNS 名；DNS 使用 SNI，IP 验证不发送 SNI。只接受 TLS 1.2 及以上版本。主机名和文件名拒绝空值、内嵌 NUL；主机名还限制为 255 字节。

`WantRead`/`WantWrite` 后保留同一流和未完成的数据，调用 `wait` 后重试；只按成功写入的字节数推进偏移。`Eof` 表示收到正常的 `close_notify`；未收到它就结束 TCP 连接会抛出 `UnexpectedEof`。正常 `close` 等待对端完成关闭。

`tls.token(stream)` 返回经过表查询验证的借用编号，供 HTTP 传输回调使用；它不持有 Stream，使用期间必须保留至少一份 Stream。`transport_read_token` 和 `transport_write_token` 收到未知或已释放编号时抛出 typed `os.Error`，不会把编号当作内存地址。复制 Stream 和临时取得引用只保护对象存活，不保证同一 TLS 流可以并发读写；程序仍需同步这些操作。

`std/https` 负责它创建或接收的 Socket，并复用 `std/http` 的头字段、正文、trailers 和大小限制。

| 接口 | 说明 |
| --- | --- |
| `connect(context tls.Context, host string, port u16) Connection throws Error` | 解析地址、连接并完成证书和身份验证 |
| `send(connection, request)`、`receive(connection, request_method)` | 在同一连接上收发 HTTP 消息，保留后续消息的读取状态 |
| `request(context, host, port, request) http.Response throws Error` | 完成一次请求后关闭连接 |
| `listen(context, address, backlog) Server throws Error`、`accept(server)` | 监听并完成新连接的 TLS 握手 |
| `receive_request(connection)`、`respond(connection, response, request_method)` | 服务端收发请求与响应 |
| `local_address(server)`、`close(connection)`、`close_server(server)` | 查询监听地址，或关闭模块拥有的连接、监听 Socket |

这些操作抛出 `https.Error`，分别包装 TLS、网络和 HTTP 错误。复制 Connection 会共享同一连接，整个连接只关闭一次，不要通过两个别名重复关闭。`request` 收发失败时只关闭模块拥有的 Socket 并继续抛出原错误，不等待 TLS 正常关闭；成功后执行正常的双向关闭。

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

两个模块还提供限时等待，时间单位为纳秒，正数使用单调时钟计算等待时限：

| 接口 | 说明 |
| --- | --- |
| `thread.wait_for(worker Thread, timeout_ns i64) bool throws os.Error` | 线程完成返回 true，超时返回 false；不取出线程结果，也不取消线程 |
| `sync.wait_for(c Condvar, m Mutex, timeout_ns i64) bool` | 收到通知返回 true，超时返回 false；返回前重新取得 m |

`timeout_ns == -1` 无限等待，`0` 不等待通知或线程完成，正数按给定纳秒数限时等待。线程已经完成时，即使参数为 0 也返回 true。`thread.wait_for` 的参数小于 -1 时抛出 `os.Error.System(22,0)`；等待自身抛出 `System(35,0)`。fork 子进程等待仍未完成、但已不属于当前进程的线程句柄时抛出 `System(3,0)`。其他系统错误也通过 `os.Error.System` 返回。wait_for 超时后仍可再次等待或 join；join 取得缓存结果，不会用掉句柄。

调用 `sync.wait` 或 `sync.wait_for` 前必须持有 m。等待会释放它，包括 timeout 为 0 的情况，返回前再取得它；重新取得锁所需的时间可能超过等待时限。小于 -1、未持锁或其他锁误用产生 Panic。收到通知不代表业务条件已经成立，超时也必须在重新持锁后检查条件。以下函数声明需导入 `std/sync` 为 `sync`：

```wz
func await_ready(ready Vec[bool], m sync.Mutex, changed sync.Condvar) bool {
    sync.lock(m);
    defer { sync.unlock(m); }
    for !ready[0] {
        let notified = sync.wait_for(changed, m, 100000000);
        if !notified && !ready[0] { return false; }
    }
    return true;
}
```

修改 `ready[0]` 的线程也须持有同一把锁，并通知 changed。上例每次等待分别计时；若要限制整个操作的总时长，调用方须记录截止时间并传入剩余时长。

线程参数、等待和共享数据的完整示例见 [线程与同步](CONCURRENCY.md)。

## std/rwlock 和 std/cancel

`std/rwlock` 提供进程内、写者优先的读写锁。`new() ReadWriteLock` 创建锁；`read_lock`/`read_unlock` 和 `write_lock`/`write_unlock` 分别取得和释放读锁、写锁，参数都是 ReadWriteLock，不抛 typed error。锁的副本共享同一状态。

多个线程可同时持读锁，写锁独占访问；已有写者等待时，新读者也要等待。它不可递归，不支持持锁升级或降级。同一线程重复取得读锁或写锁、持读锁再取写锁、持写锁再取读锁，以及非持有线程解锁，都产生 Panic。取得锁后可以用块级 defer 保证释放。以下只有函数声明，需导入 `std/rwlock` 为 `rwlock`：

```wz
func first(lock rwlock.ReadWriteLock, values Vec[i64]) i64 {
    rwlock.read_lock(lock);
    defer { rwlock.read_unlock(lock); }
    return values[0];
}
```

`std/cancel` 提供合作式取消。

| 接口 | 说明 |
| --- | --- |
| `new() Token` | 建立尚未取消的 token |
| `request(token Token)` | 标记取消，重复调用没有额外效果 |
| `is_cancelled(token Token) bool` | 查询是否已请求取消 |
| `check(token Token) throws Error` | 已取消时抛出 `Error.Cancelled`，否则正常返回 |

Token 的副本共享状态。`request` 标记取消并唤醒在这个 token 上等待的 `send_cancel`/`receive_cancel`；它不会强制停止线程，也不会打断普通的 `send`、`receive`、`sync.wait` 或正在运行的任务。工作函数需要在安全位置主动查询 `is_cancelled` 或调用 `check`，再决定何时返回。传给 `thread.spawn` 的入口仍不能声明 throws，因此其中调用 `check` 时须处理 `cancel.Error`，不能把取消异常直接带出线程入口。

`subscribe`/`unsubscribe`/`subscription_count` 是 `std/channel` 使用的内部登记接口。普通代码只创建 token 并把它传给 `*_cancel` 等待，不要自己登记或注销订阅。

## std/channel

进程内的泛型 FIFO 队列，支持多个发送和接收线程。Channel 的副本共享队列和关闭状态；发送值按普通复制规则保存，struct 的值字段独立，Vec 字段仍共享。发送后继续修改共享 Vec 时仍须自行同步。

| 接口 | 说明 |
| --- | --- |
| `bounded[T](capacity usize) Channel[T]` | 建立固定容量队列；capacity 必须大于 0，零容量产生 Panic，不表示同步交接通道 |
| `unbounded[T]() Channel[T]` | 没有固定容量上限，按需分配内存 |
| `send[T](pipe Channel[T], value T) throws Error` | 有界队列满时等待；关闭后抛出 `Error.Closed` |
| `receive[T](pipe Channel[T]) option.Option[T]` | 空且未关闭时等待；关闭并排空后返回 None |
| `try_send[T](pipe Channel[T], value T) SendResult` | 不等待，返回 `Sent`、`Full` 或 `Closed` |
| `try_receive[T](pipe Channel[T]) ReceiveResult[T]` | 不等待，返回 `Item(value)`、`Empty` 或 `Closed` |
| `close[T](pipe Channel[T])` | 幂等关闭，唤醒等待中的发送者和接收者 |

关闭不丢弃已经排队的值：receive 继续返回 Some，排空后才返回 None；try_receive 同样先返回 Item，排空后才返回 Closed。关闭后的 send 不再接受新值，try_send 优先返回 Closed。最后一个句柄清理不会代替显式 close；有其他线程等待时，应先关闭再 join。

下面是完整程序，两个线程传递字符串；队列关闭后主线程结束接收。退出码为 0：

```wz
package main;
import "std/channel" as channel;
import "std/option" as option;
import "std/os" as os;
import "std/text" as text;
import "std/thread" as thread;

func produce(pipe channel.Channel[string]) i64 {
    defer { channel.close(pipe); }
    try {
        channel.send(pipe, "hello");
        channel.send(pipe, "world");
    } catch error channel.Error {
        return 1;
    }
    return 0;
}

func main() i32 throws os.Error {
    let pipe = channel.bounded[string](2);
    let worker = thread.spawn[channel.Channel[string]](produce, pipe);
    var joined = "";
    var done = false;
    for !done {
        switch channel.receive(pipe) {
            case option.Option.Some(value): { joined += value; }
            case option.Option.None: { done = true; }
        }
    }
    if thread.join(worker) != 0 { return 1; }
    return i32(text.len(joined)) - 10;
}
```

### 限时和取消等待

`send_timeout`、`receive_timeout` 在有限时间内等待，`send_cancel`、`receive_cancel` 等待数据、空间、关闭或 token 取消。它们不通过 `throws` 返回错误，结束原因放在结果枚举里：

```wz
pub enum SendWaitResult { Sent; Closed; Timeout; Cancelled; }
pub enum ReceiveWaitResult[T] { Item(value T); Closed; Timeout; Cancelled; }
```

| 接口 | 说明 |
| --- | --- |
| `send_timeout[T](pipe Channel[T], value T, timeout_ns i64) SendWaitResult` | 等待可写空间；成功入队返回 `Sent`，关闭返回 `Closed`，截止时间到返回 `Timeout` |
| `receive_timeout[T](pipe Channel[T], timeout_ns i64) ReceiveWaitResult[T]` | 有数据返回 `Item`；关闭且空返回 `Closed`；截止时间到返回 `Timeout` |
| `send_cancel[T](pipe Channel[T], value T, token cancel.Token) SendWaitResult` | token 已取消返回 `Cancelled`，关闭返回 `Closed`，成功入队返回 `Sent` |
| `receive_cancel[T](pipe Channel[T], token cancel.Token) ReceiveWaitResult[T]` | 有数据先返回 `Item`；无数据时已取消返回 `Cancelled`，关闭返回 `Closed` |

`timeout_ns == -1` 无限等待，`0` 只立即检查一次、不睡眠，正数按纳秒等待。小于 `-1` 与 `sync.wait_for` 一样按同步误用 Panic，不会返回 `Timeout`。每个正数 timeout 调用只计算一次绝对的 `CLOCK_MONOTONIC` 截止时间；被唤醒后只用剩余时间重试，EINTR、伪唤醒、close 或其他通知都不会重新开始计时。正数过大时饱和而不是回绕；单调时钟读取失败产生 Panic。

结果判定顺序是固定的，并且一次成功提交或取出不会被后来的超时、取消回滚：

- `send_timeout`：`Closed` > 可写空间 `Sent` > `Timeout`。已关闭时即使 timeout 为 0 也返回 `Closed`。
- `receive_timeout`：已排队 `Item` > `Closed`（关闭且空） > `Timeout`。close 不丢弃已排队数据。
- `send_cancel`：`Cancelled` > `Closed` > `Sent`。取消优先于关闭和新出现的空间；已经返回 `Sent` 的值不会被之后的取消撤回。
- `receive_cancel`：已排队 `Item` > `Cancelled`（无数据） > `Closed`。数据优先于取消；取消与数据同时发生时可能取走已提交的 `Item`，但不会丢失或重复。

下面的程序先等待 50ms 超时，再发送和取出一个值，最后验证关闭优先于超时：

```wz
package main;
import "std/channel" as channel;

func main() i32 {
    let pipe = channel.bounded[string](1);

    switch channel.receive_timeout(pipe, 50_000_000) {
        case channel.ReceiveWaitResult.Timeout: {}
        default: { return 1; }
    }

    switch channel.send_timeout(pipe, "done", 0) {
        case channel.SendWaitResult.Sent: {}
        default: { return 2; }
    }

    switch channel.receive_timeout(pipe, 1_000_000) {
        case channel.ReceiveWaitResult.Item(value): {
            if value != "done" { return 3; }
        }
        default: { return 4; }
    }

    channel.close(pipe);
    switch channel.send_timeout(pipe, "late", 0) {
        case channel.SendWaitResult.Closed: {}
        default: { return 5; }
    }
    return 0;
}
```

`send_cancel`/`receive_cancel` 的 token 来自 `std/cancel`。取消是协作式的：`request` 只唤醒在这些调用上登记的等待，不终止线程，也不打断普通 `send`/`receive`。已经取消的 token 上，`send_cancel` 不再睡眠；`receive_cancel` 仍先取走队列中已提交的数据。等待的订阅登记由 `std/channel` 完成，用户只传 token，不要调用 `std/cancel` 的 `subscribe`/`unsubscribe`。

下面的程序让一个线程等待一个空 channel，主线程取消后取得 `Cancelled`；随后即使 token 已取消，已提交的值 7 仍先从 `receive_cancel` 返回：

```wz
package main;
import "std/cancel" as cancel;
import "std/channel" as channel;
import "std/os" as os;
import "std/thread" as thread;

struct WaitJob { pipe channel.Channel[i64]; token cancel.Token; }

func wait(job WaitJob) i64 {
    switch channel.receive_cancel(job.pipe, job.token) {
        case channel.ReceiveWaitResult.Item(value): return value;
        case channel.ReceiveWaitResult.Cancelled: return -1;
        case channel.ReceiveWaitResult.Closed: return -2;
        case channel.ReceiveWaitResult.Timeout: return -3;
    }
}

func main() i32 throws os.Error {
    let pipe = channel.bounded[i64](1);
    let token = cancel.new();
    let job = WaitJob(pipe: pipe, token: token);
    let worker = thread.spawn[WaitJob](wait, job);

    cancel.request(token);
    if thread.join(worker) != -1 { return 1; }

    switch channel.try_send(pipe, 7) {
        case channel.SendResult.Sent: {}
        default: { return 2; }
    }
    switch channel.receive_cancel(pipe, token) {
        case channel.ReceiveWaitResult.Item(value): {
            if value != 7 { return 3; }
        }
        default: { return 4; }
    }
    channel.close(pipe);
    return 0;
}
```

这些接口不能组合成 `select`；当前不提供 select 或异步运行时。

## std/task

固定工作线程数的有类型线程池和 Future，建立在 `std/thread`、`std/sync` 与 `std/channel` 上。工作线程入口必须是无捕获的 `func(A) R`；`A` 和 `R` 在创建池时确定，所有任务共用同一个入口。多个参数放进 struct，捕获闭包仍不受支持。

| 接口 | 说明 |
| --- | --- |
| `new[A,R](entry func(A) R, workers usize, capacity usize) Pool[A,R] throws Error` | 创建固定入口、固定工作线程数和有界任务队列的池 |
| `submit[A,R](pool Pool[A,R], argument A) Future[R] throws Error` | 提交一个任务并返回对应 Future；队列满时阻塞，关闭后抛 `Error.Closed` |
| `get[R](future Future[R]) R` | 阻塞到该任务完成并返回结果；可重复调用 |
| `poll[R](future Future[R]) option.Option[R]` | 不阻塞；已完成返回 `Some(R)`，否则 `None` |
| `ready[R](future Future[R]) bool` | 不阻塞；已完成返回 true |
| `close[A,R](pool Pool[A,R])` | 停止接收新任务，立即返回；已提交任务继续排空；幂等 |
| `join[A,R](pool Pool[A,R]) throws Error` | 先幂等 close，再等待所有工作线程退出；可重复调用 |
| `shutdown[A,R](pool Pool[A,R]) throws Error` | `join` 的显式别名 |

`Error` 包含 `Closed`、`InvalidWorkers`、`InvalidCapacity`、`SpawnFailed(code i32)`、`SelfJoin` 和 `JoinFailed(code i32)`。`Pool` 和 `Future` 的副本共享同一状态，可以由多个线程提交或等待。

下面的程序用两个工作线程计算两个平方，读取各自 Future 后显式 `shutdown`；退出码为 0：

```wz
package main;
import "std/os" as os;
import "std/task" as task;

func square(value i64) i64 { return value * value; }

func main() i32 throws task.Error {
    let pool = task.new[i64,i64](square, 2, 4);
    let first = task.submit(pool, 7);
    let second = task.submit(pool, 9);
    let a = task.get(first);
    let b = task.get(second);
    task.shutdown(pool);
    if a + b == 130 { return 0; }
    return 1;
}
```

`submit` 的队列容量只统计已提交但尚未被工作线程取走的任务。close 与新 submit 竞争时只有两种结果：submit 在关闭前成功提交并返回 Future，或抛 `Error.Closed` 且不返回 Future；不会留下永远没有结果的 Future。Future 与任务的完成顺序无关，`get` 只等待自己的那次提交。`get` 可被多个线程、多个 Future 副本重复调用，每次按 `R` 的普通复制规则返回：标量/struct 按字段复制，`string`/`Vec` 保留 ARC 共享引用。结果是共享的，`get` 不会清空它。

工作线程入口不能声明 throws。任务需要返回可处理的错误时，让 `R` 为 `Result[T,E]`，由调用方检查 `Ok`/`Err`；池不捕获错误，也不会把错误擦除成 `task.Error` 或 `Any`。任务内发生 Panic 按语言规则终止进程，不会转成 Future 结果。

`new` 先检查参数：`workers == 0` 抛 `Error.InvalidWorkers`，`capacity == 0` 抛 `Error.InvalidCapacity`，两者都在创建线程和队列之前检查；两者都为 0 时先报 `InvalidWorkers`。创建工作线程失败抛 `Error.SpawnFailed(code)`，已创建的工作线程会先关闭队列并 join，不会留下后台线程。

生命周期必须显式管理，没有自动析构：

- `close` 只停止接收，不等待；已提交任务仍会被工作线程排空，排空后工作线程退出。适合先 close、继续读取已提交的 Future，最后再 join。
- `join`/`shutdown` 先做一次幂等 close，再等待所有工作线程退出。
- 在池自己的工作线程内调用 `join`/`shutdown` 返回 `Error.SelfJoin`；该检查发生在 close 之前，所以不会顺带关闭池。
- `join` 只检测当前池的工作线程自等待。跨池等待循环，以及工作线程等待自己池中需要全部工作线程才能推进的 Future，不会被检测，仍可能死等。
- 清理最后一个 Pool 句柄**不会**停止工作线程、close 队列或 join。不显式 `shutdown`（或 close 后 join）时，工作线程可能继续运行并让状态存活。进程退出会结束线程，但这不是资源回收保证。

当前不提供任务取消、Future 超时、优先级或抢占；需要取消时在任务内部用 `std/cancel` 协作检查。完整用法和任务返回错误的示例见 [线程与同步](CONCURRENCY.md#线程池与-future)。

## std/process

启动、替换和等待进程。下列接口均声明 `throws os.Error`。

| 接口 | 说明 |
| --- | --- |
| `spawn(path string, arguments Vec[string]) i32` | 使用当前环境和标准流，返回子进程 PID |
| `spawn_with(path, arguments, env, input, output, error) i32` | 明确指定环境 Vec[string] 与三个 i32 描述符；-1 继承 |
| `spawnp(command string, arguments Vec[string]) i32` | 按 PATH 查找程序 |
| `execve(path string, arguments Vec[string], env Vec[string])`、`execv(path, arguments)` | 替换当前进程，成功不返回；execv 使用当前环境 |
| `fork() i32` | 父进程取得子 PID，子进程取得 0；async 执行器属于另一线程时抛出 os.Error |
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

`set.union(left, right)` 返回并集，`set.intersection(left, right)` 返回交集，`set.difference(left, right)` 返回只在 `left` 中出现的键。三者返回独立的 Set，键按普通规则复制。修改结果不会改变输入的成员。

## std/csv

`csv.parse(source)` 把字符串解析成 `Vec[Vec[string]]`，`csv.serialize(rows)` 输出以 LF 分隔的记录。`*_with` 版本可指定 ASCII 分隔符。引号内可包含分隔符和换行，两个相邻的双引号表示一个双引号。字段始终为字符串。空行返回空记录，单个空字段输出为 `""`。

默认限制为每字段 1 MiB、每记录 16 MiB、每记录 16,384 字段。用 `csv.Limits` 和 `parse_limited` 或 `parse_with_limited` 调整。语法和超限错误包含从 0 开始的原始输入字节偏移，以及从 1 开始的记录号和字段号。 [CSV 语法和上限](../stdlib/csv/README.md)

逐字节输入时，用 `csv.parser()` 建立解析器，调用 `parser_feed`，结束时调用 `parser_finish`。成功结束后重复调用 `parser_finish` 返回 `None`，再送入字节会抛出 `FeedAfterEnd`。解析器副本共享进度和错误。 [增量 CSV 接口](../stdlib/csv/README.md)

## std/csv_io

Native 文件和流接口。`read_file(path)` 和 `write_file(path, rows)` 负责打开和关闭文件。也可以把 `io.Reader` 交给 `csv_io.reader`，再用 `csv_io.read_row` 逐条读取。`None` 表示读完。Reader 及其副本在语法错或超限后都会返回同一个错误。

`write_file` 和 `write_file_with` 截断已有文件，以 `0o666` 和进程 umask 创建新文件。写入失败返回 `Error.Io`，文件可能只写了一部分。

用 `csv_io.writer`、`write_row` 或 `write_all` 写入已有的 `io.Writer`，最后调用 `flush`，底层文件由调用者关闭。`write_row` 返回 `Io` 时，当前行已接收，调用 `flush` 继续。`PendingOutput` 表示本次输入尚未接收。`BatchIo` 给出已接收行数，`flush` 成功后从剩余行继续。 [CSV 文件和流](../stdlib/csv_io/README.md)

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

输出完整整数 `9007199254740993`，写回的 JSON 也保留原数字文本。`field` 和 `index` 返回 `Option[Value]`。重复键按最后一个值读取。解析器保留键第一次出现的位置，手工构造的 Member 列表则按原顺序全部写出。

解析检查数字、控制字符和 Unicode 转义，Syntax(offset) 的位置按码点计数。数组与对象最多嵌套 256 层，超过时抛 DepthLimit；循环容器序列化也会报错。纯 Woozy 容器形成的强引用循环会自动回收；仍可访问的对象保持存活。Error 还有 InvalidNumber 和 NumberRange。

`as_i64` 和 `as_u64` 接受整数形式的 JSON 数字。小数点、指数或类型不匹配时抛出 `InvalidNumber`。整数超出目标类型范围时抛出 `NumberRange`。`as_u64` 把带负号的整数文本视为越界。

`json.serialize_pretty(value)` 每层缩进两个空格，保留成员顺序和数字原文。空数组和空对象仍输出为 `[]` 和 `{}`。结果末尾没有换行。

## std/json_io

Native 文件接口。`read_file(path)` 读取 UTF-8 JSON，`write_file(path, value, mode)` 写紧凑 JSON，`write_file_pretty(path, value, mode)` 写缩进格式。三者负责关闭自己打开的文件。错误为 `Error.Io(os.Error)` 或 `Error.Json(json.Error)`。写入前先完成序列化，编码错误时原文件保持原样。文件写入沿用 `fs.write_text` 的截断方式。

## 尚未提供

默认参数和模板特化尚未提供。带 userdata 的 C 注册接口可以保存捕获闭包，用法和注销要求见 [调用 C 库](FFI.md)。C 互操作的有类型 Ptr 访问放在 unsafe 块内。
