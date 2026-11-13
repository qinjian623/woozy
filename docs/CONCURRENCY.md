# 线程与同步

`std/thread` 创建线程，`std/sync` 提供互斥锁、条件变量和原子整数。当前用于 Linux x86_64 Native 程序。

## 创建与等待

```wz
package main;
import "std/os" as os;
import "std/thread" as thread;

func calculate(value i64) i64 { return value * 2; }

func main() i32 throws os.Error {
    let job = thread.spawn[i64](calculate, 21);
    if !(thread.join(job) == 42) { return 1; }
    return 0;
}
```

`spawn[A]` 接受 `func(A) i64` 和一个 A 参数。函数不捕获局部变量；需要的数据放进参数，多个值可以组成 struct。入口不能声明 `throws`，内部调用可能失败的函数时要自行捕获错误。

参数会复制到线程持有的存储中。struct/enum 仍按字段复制，Vec 仍共享容器。线程函数返回后清理参数，再允许 `join` 返回。结果是完整的 `i64`。

Thread 复制后指向同一个线程，可以多次或从不同线程调用 `join`，得到同一个结果。等待自身会抛出 `os.Error.System(35, 0)`。Linux 创建线程失败也抛出 `os.Error`；内存分配失败沿用语言规则，产生 Panic。

清理 Thread 句柄不会等待或取消线程。即使所有句柄都被清理，线程仍继续执行，结束后释放自己的资源。`main` 返回、`os.exit` 或任一线程 Panic 都会结束整个进程。

## 共享数据与锁

共享 Vec 等可变对象时，读写使用同一把锁。复制 Mutex 共享同一个锁。

```wz
package main;
import "std/os" as os;
import "std/thread" as thread;
import "std/sync" as sync;

struct Work { lock sync.Mutex; values Vec[i64]; }

func increment(work Work) i64 {
    for var i i32 = 0; i < 1000; i += 1 {
        sync.lock(work.lock);
        defer { sync.unlock(work.lock); }
        work.values[0] += 1;
    }
    return 0;
}

func main() i32 throws os.Error {
    let work = Work(lock: sync.mutex(), values: [0]);
    let first = thread.spawn[Work](increment, work);
    let second = thread.spawn[Work](increment, work);
    thread.join(first);
    thread.join(second);
    if !(work.values[0] == 2000) { return 1; }
    return 0;
}
```

这里的 `defer` 在每轮循环体退出时解锁。正常的 `lock`、`unlock` 不抛错。Mutex 不可递归：持有者再次加锁、其他线程解锁，或清理仍被持有的锁会 Panic。

ARC 引用计数使用原子操作，保证引用的计数和最后一次清理不会因并发更新而损坏。它不保护元素读写。没有正确同步的数据竞争属于程序错误，不保证结果。拥有元素的读取也要与替换使用同一把锁，因为读取包含取得该元素的引用。

## 条件变量

`condvar()` 创建条件变量。`wait(condition, mutex)` 要求调用线程已经持有 mutex：它登记等待、解锁、等待通知，再重新加锁后返回。

条件本身保存在用户数据中，始终用循环检查。通知不会替你保存“条件成立”这个事实，也不代表返回时条件仍然成立。

```wz
// work 同时包含 ready Condvar、lock Mutex 和 values Vec[i64]。
sync.lock(work.lock);
for work.values[0] == 0 {
    sync.wait(work.ready, work.lock);
}
let value = work.values[0];
sync.unlock(work.lock);
```

修改条件的线程也持有同一把锁，修改后调用 `notify_one(condition)` 唤醒一个等待者，或 `notify_all(condition)` 唤醒所有等待者。等待允许无通知返回，所以不能把循环改成一次 `if`。

## 原子整数

`AtomicI64` 复制后共享同一个整数。下列操作固定采用顺序一致的原子操作：

| 函数 | 结果 |
| --- | --- |
| `atomic_i64(value)` | 创建共享原子整数 |
| `load(value)` | 读取 |
| `store(value, desired)` | 写入 |
| `fetch_add(value, amount)` | 加法，返回修改前的值；溢出按 64 位补码回绕 |
| `compare_exchange(value, expected, desired)` | 当前值等于 expected 时替换并返回 true，否则不修改并返回 false；不会无故失败 |

原子整数适合计数器、标志和明确设计过的发布操作。多个字段需要一起读写时，使用 Mutex 更直接。

## 线程池与 Future

`std/task` 用固定数量的工作线程执行同一种无捕获函数 `func(A) R`。`A` 是每次提交的参数类型，`R` 是结果类型；多个参数放进 struct。Pool 创建后工作线程数固定，任务进入有界队列，不会为每个任务临时新建线程。

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

`submit` 在队列满时阻塞；容量只算已提交但尚未被工作线程取走的任务。`get` 阻塞到该 Future 完成，可重复调用；`poll` 返回 `Option[R]`，`ready` 返回 bool，两者都不阻塞。结果属于各自的 Future，与完成顺序无关；多个线程可以等待同一个 Future。

`close` 只停止接收新任务，已提交的任务继续执行，随后工作线程退出；新 `submit` 抛 `task.Error.Closed`。`join`/`shutdown` 会先幂等 close，再等待所有工作线程。只想先停止接单、再逐个读取 Future 时，可以只 `close`，最后 `join`。

**没有自动析构**：清理最后一个 Pool 句柄不会停止工作线程、close 队列或 join。必须显式 `shutdown`（或 `close` 后 `join`）；不这样做时工作线程可能继续等待任务并让池状态存活。`main` 返回或 `os.exit` 会结束进程，但这不是资源回收保证。

在池自己的工作线程里 `join`/`shutdown` 返回 `task.Error.SelfJoin`，且不会关闭池。`join` 不检测跨池等待循环，也不检测工作线程等待自己池中需要全部工作线程才能完成的任务。

工作线程入口不能声明 throws。任务需要返回可处理的错误时，把 `R` 写成 `Result[T,E]`：

```wz
package main;
import "std/os" as os;
import "std/result" as result;
import "std/task" as task;

enum JobError { Negative; }

func checked(value i64) result.Result[i64,JobError] {
    if value < 0 { return result.Result[i64,JobError].Err(JobError.Negative); }
    return result.Result[i64,JobError].Ok(value * 2);
}

func main() i32 throws task.Error {
    let pool = task.new[i64,result.Result[i64,JobError]](checked, 2, 4);
    let good = task.submit(pool, 21);
    let bad = task.submit(pool, -1);
    var doubled i64 = 0;
    switch task.get(good) {
        case result.Result.Ok(value): doubled = value;
        case result.Result.Err(error): return 1;
    }
    var rejected = false;
    switch task.get(bad) {
        case result.Result.Ok(value): return 2;
        case result.Result.Err(error): {
            switch error { case JobError.Negative: rejected = true; }
        }
    }
    task.shutdown(pool);
    if doubled == 42 && rejected { return 0; }
    return 3;
}
```

`Ok`/`Err` 是普通 Future 结果，不会被转换成 `task.Error`。任务内发生 Panic 仍按语言规则终止进程。`workers == 0` 抛 `InvalidWorkers`，`capacity == 0` 抛 `InvalidCapacity`，都在创建线程前检查；创建线程失败抛 `SpawnFailed(code)` 并清理已创建的工作线程。当前不提供任务取消、Future 超时和优先级。

## 超时和取消等待

`std/channel` 的 `send_timeout`/`receive_timeout` 在给定时间内等待，`send_cancel`/`receive_cancel` 等待数据、空间、关闭或 token 取消。结果用 `SendWaitResult` / `ReceiveWaitResult[T]` 区分，不通过 `throws` 返回错误。

```wz
package main;
import "std/channel" as channel;

func main() i32 {
    let pipe = channel.bounded[string](1);

    // 50ms 内没有数据：Timeout。
    switch channel.receive_timeout(pipe, 50_000_000) {
        case channel.ReceiveWaitResult.Timeout: {}
        default: { return 1; }
    }

    // 有空位，timeout 0 只检查一次，成功入队。
    switch channel.send_timeout(pipe, "done", 0) {
        case channel.SendWaitResult.Sent: {}
        default: { return 2; }
    }

    // 已有数据：优先返回 Item，而不是 Timeout/Closed。
    switch channel.receive_timeout(pipe, 1_000_000) {
        case channel.ReceiveWaitResult.Item(value): {
            if value != "done" { return 3; }
        }
        default: { return 4; }
    }

    channel.close(pipe);
    // 已关闭优先于超时：返回 Closed，不会接受 "late"。
    switch channel.send_timeout(pipe, "late", 0) {
        case channel.SendWaitResult.Closed: {}
        default: { return 5; }
    }
    return 0;
}
```

`timeout_ns == -1` 无限等待，`0` 只立即检查一次，正数按纳秒。小于 `-1` 与 `sync.wait_for` 一样按同步误用 Panic，不会返回 `Timeout`。每个正数 timeout 调用只计算一次绝对的 `CLOCK_MONOTONIC` 截止时间，被唤醒后继续用剩余时间等待；EINTR、伪唤醒、close 和数据通知都不会重置计时。过大的正数会饱和。

判定顺序固定：

- `send_timeout`：`Closed` > `Sent` > `Timeout`。
- `receive_timeout`：`Item` > `Closed` > `Timeout`。close 不丢弃已排队数据。
- `send_cancel`：`Cancelled` > `Closed` > `Sent`。已经 `Sent` 的值不会被后来的取消撤回。
- `receive_cancel`：`Item` > `Cancelled` > `Closed`。数据优先于取消；同时发生时可能取走已提交的 `Item`，但不会丢失或重复。

下面的程序让一个线程用 `receive_cancel` 等待空 channel，主线程 `request` 后等待返回 `Cancelled`；随后即使 token 已取消，已排队的 7 仍优先返回：

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

`std/cancel.request` 是协作式的：它设置取消标志并唤醒在这个 token 上登记的 channel 等待，但不会强杀线程，也不会打断普通的 `send`、`receive`、条件变量等待或正在运行的任务。任务要在安全位置调用 `is_cancelled` 或 `check` 自己决定退出。Token 副本共享状态；channel 等待的 subscribe/unsubscribe 是库内部接口，用户只传 token。

这些等待接口不能组合成 select；当前不提供多个 channel 的原子选择。

`thread.spawn` 使用无捕获函数加明确参数。暂不提供强制线程取消和 channel select；`std/cancel` 的请求由任务主动检查，或由 channel 等待接口处理。Python 后端会拒绝这些线程和同步接口。


## async 与 await

异步函数调用返回 `Task[R,E]`，创建时不执行函数体。`await` 取得结果并传播该函数声明的错误；异步 main 由执行器运行。下面的程序输出 42：

```wz
package app;
import "std/async" as tasks;
import "std/io" as io;
import "std/os" as os;

async func answer() i32 {
    await tasks.sleep(1);
    return 42;
}

async func main() i32 throws os.Error {
    io.println(await answer());
    return 0;
}
```

`std/async` 的操作如下：

| 函数 | 行为 |
| --- | --- |
| `spawn(task)` | 将任务加入就绪队列，不立即执行函数体；重复提交不重复启动 |
| `run()` | 执行已提交任务并等待已登记事件，不自动提交未启动的普通 Task |
| `run_until(task)` | 等待已提交的目标任务；普通异步调用得到的 Task 需先 spawn，其他任务可继续待执行 |
| `finished(task)` | 检查任务是否成功、失败或已取消 |
| `cancel(task)` | 请求取消；已开始的任务恢复后执行退出清理 |
| `shutdown()` | 取消尚未结束的任务并清理它们；不能从正在执行的任务内调用 |
| `sleep(milliseconds)` | 返回按单调时间等待的任务 |
| `readable(fd)`、`writable(fd)` | 等待描述符可读或可写，不执行读写，也不取得描述符所有权 |
| `join(task)`、`join_all(tasks)` | 在异步函数中等待一个任务或依次等待一组任务，并传播声明的错误 |

执行器属于首次使用它的线程；run、spawn、cancel 等操作只能在该线程调用。等待读写时使用非阻塞描述符，准备就绪后仍要处理实际读写的返回结果。普通拥有参数在创建 Task 时复制，Vec 等引用字段继续共享对象；未启动任务的最后句柄被释放时，不执行函数体。

未启动或已完成 Task 中的纯 Woozy 强引用循环会自动回收。仍由执行器执行或等待事件的任务保持存活，清理句柄不会自动取消它；需要停止时调用 cancel 或 shutdown。异线程拥有执行器时 fork 抛出 os.Error，同线程 fork 的规则见[进程文档](PROCESSES.md)。

目前 async 只支持 Linux x86_64 Native，inout 参数会在编译时拒绝。async 的参数、局部值、结果或错误不能包含外部 C 资源句柄。对函数值和 dyn 的资源检查目前按程序中出现的类型保守处理，因此含同类值、但实际没有保存 C 资源的 async 函数也可能被拒绝。Python 后端暂不支持 async。
