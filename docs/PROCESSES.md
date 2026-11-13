# 进程与信号

`std/process` 创建、替换和等待进程，`std/signal` 屏蔽及同步等待信号。两者用于 Linux x86_64 Native 程序，系统错误通过 `throws os.Error` 返回。

## 启动程序并读取输出

下面是完整的 `main.wz`。它调用 Linux 上的 `/usr/bin/printf`，把子进程的标准输出接到管道，再由父进程读取。`WOOZY_NAME` 设置默认名字，第一个命令行参数可以覆盖它。

```wz
package main;
import "std/io" as io;
import "std/os" as os;
import "std/process" as process;
import "std/option" as option;
import "std/vec" as vec;

func main() i32 throws os.Error {
    var name = "Woozy";
    switch process.getenv("WOOZY_NAME") {
        case option.Option.Some(value): name = value;
        case option.Option.None: {}
    }
    let arguments = process.args();
    if vec.len(arguments) > 1 { name = arguments[1]; }

    let output = os.pipe(u32(os.O_CLOEXEC));
    var read_open = true;
    var write_open = true;
    var child i32 = -1;
    var waited = false;
    defer {
        if write_open { try { os.close(output.write_end); } catch error os.Error {} }
        if read_open { try { os.close(output.read_end); } catch error os.Error {} }
        if child > 0 && !waited { try { process.wait(child); } catch error os.Error {} }
    }
    child = process.spawn_with(
        "/usr/bin/printf", ["printf", "hello, %s\n", name], ["LC_ALL=C"],
        -1, os.fd(output.write_end), -1);
    write_open = false;
    os.close(output.write_end);
    let message = io.read_text(output.read_end);
    read_open = false;
    os.close(output.read_end);
    let status = process.wait(child);
    waited = true;
    if !process.exited(status) || process.exit_status(status) != 0 { return 1; }
    io.print(message);
    return 0;
}
```

没有额外参数或 `WOOZY_NAME` 时输出 `hello, Woozy`。参数为 `程序员` 时输出 `hello, 程序员`。

`spawn_with` 的最后三个参数分别指定子进程的标准输入、标准输出和标准错误；`-1` 表示继承。传入 `os.fd(file)` 使用已有 File 的描述符。重定向不会关闭父进程的 File，所以示例关闭父进程的写端后才读取到管道结束。

示例先读完管道，再等待子进程。若先等待一个持续输出的子进程，它可能因管道写满而等待父进程读取。defer 出错清理时也先关闭管道，再等待。

## spawn、exec 与 wait

| 接口 | 行为 |
| --- | --- |
| `spawn(path string, arguments Vec[string]) i32 throws os.Error` | 使用当前环境，继承标准流，返回子进程 PID |
| `spawn_with(path, arguments, env, input, output, error)` | 明确指定环境 `Vec[string]` 和三个 `i32` 描述符，返回 PID |
| `spawnp(command string, arguments Vec[string]) i32 throws os.Error` | 按 PATH 查找；command 含 `/` 时直接使用它 |
| `execve(path string, arguments Vec[string], env Vec[string]) throws os.Error` | 用新程序替换当前进程，成功后不返回 |
| `execv(path string, arguments Vec[string]) throws os.Error` | 使用当前环境执行 execve |
| `wait(pid i32) WaitStatus throws os.Error` | 等待指定子进程，遇到 Interrupted 自动重试 |
| `waitpid(pid i32, options i32) WaitStatus throws os.Error` | 一次等待，可组合 `WNOHANG`、`WUNTRACED`、`WCONTINUED` |

`arguments[0]` 也由调用者提供，通常写程序名。`spawn` 和 `execve` 不解释 shell 命令字符串；需要 shell 时，明确调用 `/bin/sh` 并传 `-c` 及命令。程序路径、参数和环境条目都不能含 NUL。

`spawn_with` 在返回前确认 exec 是否成功。路径不存在等 exec 错误会直接抛出 `os.Error`，失败的子进程会被回收。成功启动后仍要调用 `wait` 或 `waitpid`；子进程自行退出码非零不会自动变成 Woozy 的 typed error。

`WaitStatus` 有 `pid i32` 和原始 `status i32` 两个字段。`WNOHANG` 暂时没有可报告状态时 `pid == 0`。先用 `exited(status)` 判断正常退出，再取 `exit_status(status)`；信号退出用 `signaled` 和 `term_signal`。还可以用 `stopped`、`stop_signal`、`continued`、`core_dumped` 检查其他状态。

`os.File`、`net.Socket` 复制后仍指向同一个描述符，每个描述符只关闭一次。一次 close 出错也不要重试。fork 会让父子各自持有描述符副本，双方要分别关闭自己不再需要的端点；exec 时 `O_CLOEXEC` 描述符不会保留。文件和 socket 的规则见 [系统接口](OS.md) 与 [标准库](STDLIB.md)。

## 参数与环境：字符串和字节

| 字符串接口 | 保留 Linux 原始字节的接口 |
| --- | --- |
| `args() Vec[string]` | `args_bytes() Vec[Vec[u8]]` |
| `environment() Vec[string]` | `environment_bytes() Vec[Vec[u8]]` |
| `getenv(name string) option.Option[string]` | `getenv_bytes(name Vec[u8]) option.Option[Vec[u8]]` |
| `setenv(name string, value string, overwrite bool)` | `setenv_bytes(name Vec[u8], value Vec[u8], overwrite bool)` |
| `unsetenv(name string)` | `unsetenv_bytes(name Vec[u8])` |

这些函数均声明 `throws os.Error`。`args` 包括第 0 项程序名；环境是 `NAME=value` 条目组成的 Vec。返回的是副本，修改它不会改变进程保存的参数或环境。`getenv` 用 `None` 表示没有该名称，空字符串值是 `Some("")`。

字符串接口严格检查 UTF-8；Linux 传入非 UTF-8 参数或环境值时，抛出 `os.Error.System(84,0)`。需要保留它们时选择 bytes 接口。环境名不能为空，不能含 `=` 或 NUL，环境值不能含 NUL。

`setenv` 和 `unsetenv` 在运行库内同步修改环境；它们不会改变已经运行的子进程。`spawn`、`spawnp` 和 `execv` 读取当前环境，`spawn_with` 和 `execve` 只使用明确传入的环境。不要假定两个分别调用的环境操作会一起完成。

`execve_bytes(path Vec[u8], arguments Vec[Vec[u8]], env Vec[Vec[u8]])` 与 `spawn_bytes(path, arguments, env, input, output, error)` 接受原始字节；其余返回和错误规则与字符串版本相同。bytes 版本也不能传入 NUL。

## 同步等待信号

先用 `signal.bit(number)` 取得 `u64` 位掩码，用 `|` 合并，再屏蔽要等待的信号。下面是函数体片段；除上例导入外，还需 `import "std/signal" as signal;`，所在函数声明 `throws os.Error`。

```wz
let mask = signal.bit(signal.SIGUSR1);
let old_mask = signal.sigprocmask(signal.SIG_BLOCK, mask);
defer { try { signal.sigprocmask(signal.SIG_SETMASK, old_mask); } catch error os.Error {} }
os.kill(os.getpid(), signal.SIGUSR1);
let received = signal.sigwaitinfo(mask);
io.println(received.number); // 输出 10
```

| 接口 | 行为 |
| --- | --- |
| `sigprocmask(how i32, mask u64) u64 throws os.Error` | 修改调用线程的屏蔽集合，返回原集合 |
| `sigpending() u64 throws os.Error` | 当前待处理的信号集合 |
| `sigwaitinfo(mask u64) Info throws os.Error` | 等待并取走集合中的一个信号 |
| `sigtimedwait(mask u64, timeout_ns i64) Info throws os.Error` | 纳秒超时；0 只检查一次，-1 无限等待 |
| `signal(number i32, action Action) throws os.Error` | 设置 `signal.Action.Default` 或 `signal.Action.Ignore` |

`sigtimedwait` 超时抛出 `os.Error.WouldBlock(11,0)`。`Info` 包含 `number`、`code`、`pid`、`uid`、`status` 和 `value`；后两个字段的含义依信号种类而定，例如 SIGCHLD 的退出状态和排队信号携带的值。

屏蔽集合属于调用线程，Default/Ignore 的动作属于整个进程。多线程程序通常在创建线程前屏蔽相关信号，让新线程继承屏蔽集合，再由一个线程同步等待。Linux 的信号规则见 [signal(7)](https://man7.org/linux/man-pages/man7/signal.7.html)。

可用常量包括 `SIGINT`、`SIGTERM`、`SIGUSR1`、`SIGUSR2`、`SIGCHLD`、`SIGPIPE` 等；`SIG_BLOCK` 添加屏蔽，`SIG_UNBLOCK` 解除指定屏蔽，`SIG_SETMASK` 替换集合。SIGKILL 和 SIGSTOP 不能改成忽略。

当前没有异步调用 Woozy 函数的信号处理器。`signal.signal` 只接受 Default/Ignore，不能传函数值。

## fork 后继续执行

`process.fork() i32 throws os.Error` 在父进程返回子进程 PID，在子进程返回 0。普通变量和 Vec 随进程内存复制，父子之后的修改互不影响；需要共享数据时使用 [std/ipc](IPC.md) 或管道。

若 async 执行器已属于另一线程，`process.fork` 在创建子进程前抛出 `os.Error`。执行器自身线程仍可 fork；父子分别继续自己的任务副本，取消或等待不会修改对方的事件监听。`spawn` 的子进程直接 exec，因此另一线程使用执行器不妨碍 spawn。

多线程 fork 后，子进程只保留调用 fork 的线程。其他线程持有的业务锁状态仍被复制，不会自动解锁；这些线程在子进程也不会回来解锁。调用方要在 fork 前安排好业务锁和数据访问。这个线程保留规则与 [Linux fork](https://man7.org/linux/man-pages/man2/fork.2.html) 相同。

Woozy 会修复自己的分配器等内部锁，使子进程可以使用运行库；它不会重置用户 Mutex 或 Condvar。继承的 ARC 引用计数不会减去已消失线程持有的引用，部分引用可能一直保留到 exec 或进程退出。子进程通常应尽快调用 exec，优先用 `spawn` 完成“启动另一个程序”的需求。

子进程中，对已消失线程的未完成 Thread 调用 `thread.join` 会抛出 `os.Error.System(3,0)`；等待当前调用线程仍抛出代码 35。fork 前已经完成的 Thread 可以返回其缓存结果。

`main` 返回只结束当前进程；`os.exit` 也结束当前进程，但不执行语言 defer。父进程负责等待自己的子进程。需要跨进程同步时，使用 `std/ipc` 的锁，不要把线程用的 `std/sync` 锁当成进程共享锁。

构建和运行说明见 [BUILD.md](../development/docs/BUILD.md)。
