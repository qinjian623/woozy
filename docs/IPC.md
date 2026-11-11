# 共享内存与跨进程同步

`std/ipc` 提供共享内存，以及放在其中的 Mutex、Condvar 和 Semaphore，用于 Linux x86_64 Native 程序。映射只保存字节和数值，不保存 string、Vec、函数值或其他 ARC 对象的指针。

## 一个单元素通道

下面是完整的 `main.wz`。父进程先建立共享区域并初始化同步对象，再 fork。子进程依次写入 1、2、3，父进程读取并输出。

Semaphore 记录空闲槽位，Mutex 保护数值和 ready 标志，Condvar 等待 ready 发生变化。

```wz
package main;
import "std/io" as io;
import "std/ipc" as ipc;
import "std/os" as os;
import "std/process" as process;

func main() i32 throws os.Error {
    let memory = ipc.map_anon(4096, ipc.PROT_READ | ipc.PROT_WRITE);
    let mutex = ipc.mutex_init(memory, 0);
    let changed = ipc.cond_init(memory, 16);
    let space = ipc.sem_init(memory, 32, 1);
    // 0..47 是三个同步对象；64 是 ready，72 是传送的 i64。
    let child = process.fork();
    if child == 0 {
        for var value i64 = 1; value <= 3; value += 1 {
            ipc.sem_wait(space);
            ipc.mutex_lock(mutex);
            ipc.write_i64(memory, 72, value);
            ipc.write_i64(memory, 64, 1);
            ipc.cond_signal(changed);
            ipc.mutex_unlock(mutex);
        }
        os.exit(0);
    }
    for var index i32 = 0; index < 3; index += 1 {
        ipc.mutex_lock(mutex);
        for ipc.read_i64(memory, 64) == 0 {
            ipc.cond_wait(changed, mutex);
        }
        let value = ipc.read_i64(memory, 72);
        ipc.write_i64(memory, 64, 0);
        ipc.mutex_unlock(mutex);
        ipc.sem_post(space);
        io.println(value);
    }
    let status = process.wait(child);
    ipc.sem_destroy(space);
    ipc.cond_destroy(changed);
    ipc.mutex_destroy(mutex);
    if !process.exited(status) || process.exit_status(status) != 0 { return 1; }
    return 0;
}
```

输出三行 `1`、`2`、`3`。匿名共享区域最初全为 0，所以 ready 无需另写初始值。同步对象由父进程初始化一次，子进程使用继承的映射。等待子进程结束后，父进程才销毁同步对象。

示例中的三个对象各占 16 字节。编写自己的布局时可用 `ipc.MUTEX_BYTES`、`ipc.COND_BYTES`、`ipc.SEM_BYTES` 和 `ipc.SYNC_ALIGNMENT`，不要让对象相互重叠。数值字段也要留出各自的空间。

## 建立和读取映射

| 接口 | 行为 |
| --- | --- |
| `map_anon(bytes usize, protection i32) Memory throws os.Error` | 建立匿名共享映射，适合在 fork 前创建 |
| `map_file(file os.File, bytes usize, offset u64, protection i32) Memory throws os.Error` | 共享映射文件的一段，offset 必须是 4096 的倍数 |
| `len(memory Memory) usize` | 映射长度，单位为字节 |
| `read_bytes(memory, offset, count) Vec[u8] throws os.Error` | 复制指定字节到新的 Vec |
| `write_bytes(memory, offset, data Vec[u8], start, count) throws os.Error` | 从 data 的 start 开始复制 count 字节 |
| `read_i32(memory, offset) i32`、`read_i64(memory, offset) i64` | 读取整数，均 throws os.Error |
| `write_i32(memory, offset, value)`、`write_i64(memory, offset, value)` | 写入对应整数，均 throws os.Error |

表中的 offset、start、count 都是 `usize`。`read_bytes` 返回的 Vec 不与共享区域相连；修改它后，需要再调用 `write_bytes` 才会写回。跨进程传字符串时可以写入 `text.bytes(value)`，在接收端用 `text.from_bytes` 严格解码。

`protection` 可以组合 `PROT_READ`、`PROT_WRITE`。长度必须大于 0；访问超出映射、数值偏移没有按 i32 的 4 字节或 i64 的 8 字节对齐时抛出 `os.Error`。读写权限不符也抛错。

复制 Memory 共享同一映射；Mutex、Condvar、Semaphore 也持有 Memory，让映射在使用期间保持有效。当前进程最后一个 Memory 引用被清理时解除该进程的映射。映射建立后可以关闭原 File，不会使映射失效；文件若被并发截短，仍可能发生 Linux SIGBUS，因此参与者须约定文件大小。[mmap(2)](https://man7.org/linux/man-pages/man2/mmap.2.html)

普通 Vec 的复制共享规则只在同一进程内适用。fork 后不能靠 Vec 的相同地址在父子之间传数据，也不能把 ARC 引用写入共享区域；使用上表的数值和字节操作。

## 命名共享内存

`shm_open(name string, flags i32, mode u32) os.File throws os.Error` 创建或打开 Linux 的命名共享对象；`shm_unlink(name string) throws os.Error` 删除其名称。名称形如 `/woozy-job`，斜杠后只有一个非空名称；名称部分不能为 `.` 或 `..`，不能再含 `/` 或 NUL。flags 使用 `os.O_RDONLY` 或 `os.O_RDWR`，可组合 `O_CREAT`、`O_EXCL`、`O_TRUNC`、`O_CLOEXEC`。

下面是辅助函数；与上例一样导入 ipc 和 os 后即可使用。它只供创建方调用，返回的区域还需要由创建方初始化数据和同步对象。

```wz
func create_shared(name string) ipc.Memory throws os.Error {
    let file = ipc.shm_open(name, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600);
    var closed = false;
    var ready = false;
    defer {
        if !closed { try { os.close(file); } catch error os.Error {} }
        if !ready { try { ipc.shm_unlink(name); } catch error os.Error {} }
    }
    os.ftruncate(file, 4096);
    let memory = ipc.map_file(file, 4096, 0, ipc.PROT_READ | ipc.PROT_WRITE);
    closed = true;
    os.close(file);
    ready = true;
    return memory;
}
```

另一个进程在创建方初始化完成后，用同一名称 `shm_open(name, os.O_RDWR, 0)`，再以相同长度 `map_file`，关闭自己的 File，并使用双方约定的偏移。名称已经存在不表示初始化已经完成；创建方可以通过管道或其他已准备好的通信方式通知对方。

创建方使用 `O_EXCL` 避免覆盖已有对象。打开方不要传 `O_TRUNC`，也不要重新执行 `mutex_init`、`cond_init` 或 `sem_init`；已有同步对象用 `mutex_at(memory, offset)`、`cond_at`、`sem_at` 取得包装值。

所有参与者都打开对象后，可以调用 `shm_unlink(name)` 删除名称；已有映射仍然可用，后来的进程无法再按这个名称打开它。只关闭 File 或清理 Memory 不会删除名称。名称的删除与已有映射的生命周期见 [shm_open(3)](https://man7.org/linux/man-pages/man3/shm_open.3.html)。

## Mutex 和 Condvar

| 接口 | 行为 |
| --- | --- |
| `mutex_init(memory Memory, offset usize) Mutex throws os.Error` | 在未初始化的共享区域创建锁 |
| `mutex_at(memory Memory, offset usize) Mutex` | 使用已有锁，不重新初始化 |
| `mutex_lock(mutex Mutex)`、`mutex_unlock(mutex Mutex)` | 加锁、解锁，不声明 throws |
| `mutex_destroy(mutex Mutex)` | 销毁已无人使用的锁 |
| `cond_init(memory, offset) Condvar throws os.Error`、`cond_at(memory, offset) Condvar` | 创建或使用条件变量 |
| `cond_wait(condition Condvar, mutex Mutex)` | 解锁并等待，返回前重新加锁 |
| `cond_timedwait(condition, mutex, timeout_ns i64) bool` | 纳秒超时；收到通知或无通知返回时为 true，超时为 false |
| `cond_signal(condition)`、`cond_broadcast(condition)` | 通知一个或全部等待者 |
| `cond_destroy(condition)` | 销毁已无人等待的条件变量 |

同步对象需要读写权限，起始偏移按 `SYNC_ALIGNMENT`（4 字节）对齐。首次初始化的区域必须全为 0；重复初始化抛出 `os.Error`。`*_at` 不验证或创建对象，后续实际操作会检查其类型和状态。

Mutex 不可递归。持锁者再次加锁、错误线程或进程解锁、销毁仍被持有或等待的对象等误用会 Panic。锁操作不通过 typed catch 返回这些错误。

条件变量只负责等待通知，不保存“条件成立”。调用 wait 前必须已经持有传入的同一把锁；条件数据也用这把锁保护。等待可以在没有通知时返回，所以要像示例那样用 `for` 重新检查条件，不能只检查一次。

`cond_timedwait` 返回 true 也不保证条件仍成立，返回 false 时仍重新持有 mutex。超时可以为 0，-1 表示无限等待。

## Semaphore 与原子整数

| 接口 | 行为 |
| --- | --- |
| `sem_init(memory, offset, value u32) Semaphore throws os.Error` | 创建计数信号量 |
| `sem_at(memory, offset) Semaphore` | 使用已有信号量 |
| `sem_wait(semaphore)` | 等待计数大于 0，成功后减 1 |
| `sem_timedwait(semaphore, timeout_ns i64) bool` | 成功取得一个计数时 true，超时 false |
| `sem_trywait(semaphore) bool` | 立即尝试，没有计数时 false |
| `sem_post(semaphore)` | 加 1，并唤醒等待者 |
| `sem_value(semaphore) u32` | 读取当前计数；不要靠它代替 wait/trywait |
| `sem_destroy(semaphore)` | 销毁已无人等待的信号量 |

初始值范围为 0 到 2147483647。计数达到上限后再 post 会 Panic；sem_wait、post 等正常操作不声明 throws，误用仍 Panic。超时参数与条件变量相同。

共享区域中的 i64 还可以使用以下顺序一致原子操作，所有操作均声明 `throws os.Error`，偏移须为 8 的倍数，映射须有读写权限：

| 接口 | 返回 |
| --- | --- |
| `atomic_load(memory, offset) i64` | 当前值 |
| `atomic_store(memory, offset, value i64)` | 写入值 |
| `atomic_add(memory, offset, delta i64) i64` | 加法前的值，溢出按 64 位补码回绕 |
| `atomic_compare_exchange(memory, offset, expected i64, desired i64) bool` | 相等才替换，成功 true，失败 false，不会无故失败 |

不要把同一字段的普通 read/write 与并发原子操作混用。多个字段要一起修改时使用 Mutex。

这些锁不支持持有者退出后的自动恢复，也不会在多线程 fork 后重置业务锁。调用方负责初始化顺序、正常销毁和进程异常退出后的处理。多线程 fork 继承的 ARC 引用可能保留到 exec 或进程退出；详细的线程、引用和子进程执行规则见 [进程与信号](PROCESSES.md)。

构建和运行说明见 [BUILD.md](../development/docs/BUILD.md)。
