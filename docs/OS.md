# 系统接口：std/os

`std/os` 提供有类型的系统接口，函数名称和参数顺序接近 C，失败时抛出 `os.Error`。用于 Linux x86_64 和 macOS arm64 的 Native 程序。Python 后端不支持这个模块。`getrandom` 在 macOS 上只接受 `flags = 0`，`std/random` 使用这一设置。

## 输出 Hello World

这是一个完整的 `.wz` 文件：

```wz
package main;
import "std/os" as os;

func main() throws os.Error {
    os.write_all_text(os.stdout(), "Hello, world!\n");
}
```

`stdout()` 取得标准输出，`write_all_text` 写完字符串的全部 UTF-8 字节。`throws os.Error` 允许输出失败时把错误交给程序入口处理。程序正常执行时输出一行 `Hello, world!`。

## 文件与函数

`os.File` 表示一个文件描述符。复制 File 不会执行 `dup`，也不会建立独立文件位置；这些副本指向同一个描述符。File 离开作用域时不会自动关闭，调用者负责执行 `close`。显式关闭一个副本会影响其他副本；描述符号码还可能被系统重新使用。

| 接口 | 返回和行为 |
| --- | --- |
| `stdin() File` | 标准输入 |
| `stdout() File` | 标准输出 |
| `stderr() File` | 标准错误 |
| `open(path string, flags i32, mode u32) File throws Error` | 打开文件；创建文件时使用 mode |
| `close(file File) throws Error` | 关闭描述符，不自动重试 |
| `read(file File, data Vec[u8], count usize) usize throws Error` | 向缓冲区开头读取最多 count 字节，返回实际字节数；文件结束返回 0 |
| `write(file File, data Vec[u8], count usize) usize throws Error` | 从缓冲区开头写出最多 count 字节，返回实际字节数 |
| `write_all(file File, data Vec[u8]) throws Error` | 写完整个字节容器 |
| `write_text(file File, text string) usize throws Error` | 写一次字符串的 UTF-8 字节，返回实际字节数 |
| `write_all_text(file File, text string) throws Error` | 写完字符串的全部 UTF-8 字节 |
| `lseek(file File, offset i64, whence i32) i64 throws Error` | 改变文件位置，返回新的位置 |
| `unlink(path string) throws Error` | 删除文件的目录项 |
| `mkdir(path string, mode u32) throws Error` | 创建目录 |
| `rmdir(path string) throws Error` | 删除空目录 |
| `getpid() i32` | 当前进程 ID |
| `exit(status i32)` | 立即终止进程，退出状态取低 8 位，不执行语言 defer |

`open` 总是传三个参数，不使用 C 的可变参数。未创建文件时，仍可以传 `mode: 0`。

可用的打开标志有 `O_RDONLY`、`O_WRONLY`、`O_RDWR`、`O_CREAT`、`O_EXCL`、`O_TRUNC`、`O_APPEND`、`O_NONBLOCK`、`O_CLOEXEC`，用 `|` 组合。文件定位使用 `SEEK_SET`、`SEEK_CUR`、`SEEK_END`。例如 `os.O_CREAT | os.O_RDWR`、`os.SEEK_SET`。

路径使用 `string` 的完整 UTF-8 字节，不要求调用者补结尾 NUL。路径中包含 NUL 时抛出代码为 22（EINVAL）的错误；长度达到 4096 字节时抛出代码为 36（ENAMETOOLONG）的错误，不会静默截断。

### 描述符相对操作

`openat`、`mkdirat`、`unlinkat` 和 `fstatat` 以已打开的目录 `File` 为基准解析名称，适用于逐项复制、删除和遍历。它们只接受一个目录项名称：空名称、`.`、`..`、含 `/` 或 NUL 的名称都抛出代码为 22（EINVAL）的错误。这样遍历器每次只处理一个 `getdents` 返回的项，不会在一次调用中跟随名称内部的符号链接。`_bytes` 版本直接接受 `Vec[u8]`，可处理非 UTF-8 文件名。

| 接口 | 返回和行为 |
| --- | --- |
| `openat(directory File, name string, flags i32, mode u32) File throws Error` | 相对目录打开一个名称；标志和 mode 与 `open` 相同 |
| `openat_bytes(directory File, name Vec[u8], flags i32, mode u32) File throws Error` | 字节名称版本 |
| `mkdirat(directory File, name string, mode u32) throws Error` | 在目录中创建一个子目录 |
| `mkdirat_bytes(directory File, name Vec[u8], mode u32) throws Error` | 字节名称版本 |
| `unlinkat(directory File, name string, kind UnlinkKind) throws Error` | 删除一个目录项；`UnlinkKind.File` 删除文件或符号链接本身，`UnlinkKind.Directory` 删除空目录 |
| `unlinkat_bytes(directory File, name Vec[u8], kind UnlinkKind) throws Error` | 字节名称版本 |
| `fstatat(directory File, name string, follow bool) FileInfo throws Error` | 取得目录项信息；`follow` 为 `false` 时报告符号链接本身 |
| `fstatat_bytes(directory File, name Vec[u8], follow bool) FileInfo throws Error` | 字节名称版本 |

```wz
pub enum UnlinkKind {
    File;
    Directory;
}
```

目录描述符在创建后绑定到该目录对象，不随之后的 `rename` 改变；用旧描述符创建或删除的子项仍落在原来的目录对象中。`unlinkat` 不跟随最后一个符号链接，`fstatat` 的 `follow = false` 只读取链接本身。

## 读写字节

下面是另一个完整文件，写入三个字节，再读取并检查它们：

```wz
package main;
import "std/os" as os;

func main() i32 throws os.Error {
    let file = os.open("example.bin", os.O_CREAT | os.O_TRUNC | os.O_RDWR, 0o600);
    let bytes Vec[u8] = [65, 0, 66];
    os.write_all(file, bytes);
    let position = os.lseek(file, 0, os.SEEK_SET);
    let buffer Vec[u8] = [0, 0, 0];
    let count = os.read(file, buffer, 3);
    os.close(file);
    if count != 3 || buffer[0] != 65 || buffer[1] != 0 || buffer[2] != 66 {
        return 1;
    }
    return 0;
}
```

这会创建或截断当前目录的 `example.bin`，内容是 `A`、NUL、`B` 三个字节。`read` 修改共享 Vec 的已有元素，不改变容器长度；未读到的尾部元素保持原值。

`count` 不能超过缓冲区的已有长度，否则产生 EINVAL。调用者要事先准备好缓冲区；空 Vec 只能读取或写入 0 字节。空读写直接成功返回 0，不访问缓冲区，也不检查描述符；`write_all` 和 `write_all_text` 遇到空数据同样直接成功。

上例只展示正常流程。若中间操作抛错，尚未执行的 `close` 不会自动发生；需要覆盖错误退出时，可在 defer 中调用 close 并在 defer 内处理它可能产生的错误。

## 短写、中断与错误

`read`、`write` 和 `write_text` 只执行一次读写，可能成功但只完成部分字节。发生中断且没有完成字节时，抛出 `Interrupted`；不会自动重试。

`write_all` 和 `write_all_text` 会继续写剩余部分。遇到 `Interrupted` 时，在相同位置重试；已经成功写出的字节不再重写。零进展产生 `NoProgress`，`WouldBlock` 和其他错误立即返回。

错误类型是普通枚举：

```wz
// std/os 中的声明
pub enum Error {
    Interrupted(code i32, completed usize);
    WouldBlock(code i32, completed usize);
    System(code i32, completed usize);
    NoProgress(completed usize);
}
```

`code` 使用 Linux 错误码编号。macOS 运行库将对应的 Darwin 错误转成相同编号。`completed` 表示本次高级操作在失败前已完成的字节数。一次读写在失败时为 0；`write_all` 可能已经完成一部分。NoProgress 只携带已完成字节数。

用 `try/catch` 捕获 `os.Error`，再用 `switch` 区分错误类别并取出 `code` 和 `completed`；写法见 [教程的错误处理一节](TUTORIAL.cn.md#11-可声明的错误)。需要继续发送时，从已完成字节之后开始，不能把整段数据重新写一遍。

`close` 的中断不能套用写入的重试方式：Linux 可能已经释放描述符，重试可能关闭随后复用该号码的文件。因此 `close` 报错后不自动重试。
