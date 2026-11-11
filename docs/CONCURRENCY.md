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

暂不提供捕获闭包、线程取消、带超时的等待、递归锁和读写锁。Python 后端会拒绝这些线程和同步接口。
