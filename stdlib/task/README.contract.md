# `std/task` 线程池与 Future 契约（task/1）

状态：本模块提供纯 Wz 实现的有类型线程池和 Future，使用 `std/channel` 的
bounded 任务队列、`std/thread` 的线程句柄和 `std/sync` 的同步原语。当前要求
worker 函数是**无捕获**的 `func(A) R`；显式参数放进 `A`。捕获闭包、timed
等待、取消 token 和自动环境清理由 Root 后续统一扩展，本模块不自行实现。

生命周期是显式的：`Pool`/`Future` 是普通值 struct，没有用户自定义析构器。
销毁最后一个 Pool 句柄**不会**停止 worker，也不会自动 close/join。调用方必须
调用 `close` 后 `join`/`shutdown`；进程退出会结束仍运行的线程，但这不是资源
回收保证。此限制在后续 Root 环境/清理机制接入前保持。

## API

```wz
import "std/task" as task;

pub enum Error {
    Closed;
    InvalidWorkers;
    InvalidCapacity;
    SpawnFailed(code i32);
    SelfJoin;
    JoinFailed(code i32);
}

pub struct Pool[A,R];
pub struct Future[R];

pub func new[A,R](entry func(A) R, workers usize, capacity usize) Pool[A,R] throws Error;
pub func submit[A,R](pool Pool[A,R], argument A) Future[R] throws Error;
pub func close[A,R](pool Pool[A,R]);
pub func join[A,R](pool Pool[A,R]) throws Error;
pub func shutdown[A,R](pool Pool[A,R]) throws Error;

pub func get[R](future Future[R]) R;
pub func poll[R](future Future[R]) option.Option[R];
pub func ready[R](future Future[R]) bool;
```

| 调用 | 语义 |
| --- | --- |
| `new(entry, workers, capacity)` | 创建固定 worker 函数、固定 worker 数和 bounded 任务队列的池 |
| `submit(pool, argument)` | 入队一个任务并返回对应 Future；队列满时阻塞；关闭后抛 `Closed` |
| `get(future)` | 阻塞到结果可用，返回 R；可重复调用，每次按普通复制规则返回 |
| `poll(future)` | 非阻塞；完成返回 `Some(R)`，否则 `None` |
| `ready(future)` | 非阻塞；完成返回 true |
| `close(pool)` | 停止接收新任务；已入队任务继续被 workers 排空；幂等 |
| `join(pool)` | 幂等关闭接收，然后等待所有 worker 退出；可重复调用 |
| `shutdown(pool)` | `join` 的显式别名 |

### 参数错误

- `workers == 0` 抛 `Error.InvalidWorkers`。
- `capacity == 0` 抛 `Error.InvalidCapacity`。
- 两者都在任何线程创建或队列分配之前检查，不依赖 Panic。
- 当前实现不进行无检查的计数乘法/转换；worker 数和容量直接使用 `usize`。
- `thread.spawn` 失败抛 `Error.SpawnFailed(code)`；已创建的 worker 会先 close
  队列并 join，不会留下后台线程。`thread.join` 的其他失败抛
  `Error.JoinFailed(code)`。

## 排队、背压和关闭

任务队列复用 `std/channel` 的 bounded channel：

- 容量是已提交但尚未被 worker 取走的任务上限。
- `submit` 在队列满时阻塞直到 worker 取走任务或池关闭。
- `close` 后队列中已经提交的任务仍会被 worker 排空；排空后 workers 退出。
- close 与 submit 竞争时只有两种结果：submit 在关闭前成功提交并返回 Future，
  或 submit 抛 `Error.Closed` 且不返回 Future。不会留下“已入队但永远没有
  结果”的 Future。
- `join` 本身也会先执行幂等 close；因此单独调用 `join` 不会等待一个仍在接收
  任务的池。`close` 只停止接收，不等待；适合先 close 再继续使用已提交的
  Future，最后 join。

## Future 和结果

- **结果绑定 Future，而不是完成顺序**：每次 `submit` 返回的 Future 对应该次
  入队的 argument；worker 完成顺序可以不同，`get` 只关心自己的 Future。
- `get` 在结果未完成时阻塞；结果完成后可被任意多个线程、任意多个 Future
  副本重复读取。每次读取按 R 的普通复制规则复制：标量/struct 按字段复制，
  `string`/`Vec` 保留 ARC 共享引用。
- 结果保存在共享状态中直到最后一个 Future 句柄销毁；`get` 不清空结果。
- `poll`/`ready` 不阻塞，可用于等待前检查；它们不替代同步，调用方仍需遵守
  自己的共享数据规则。
- `R` 可以是 `string`、`Vec[T]`、struct、enum 或 `Result[T,E]`。用户任务若
  有 typed throws，应把入口写成返回 `Result[T,E]`；本池不捕获错误，也不把
  未知错误擦除成 `Any`。
- worker 入口返回后 Future 只完成一次。任务内部的 Panic 不会转换成 Future
  结果或 `Error`：它按语言规则终止进程，因此不能通过 `try/catch` 捕获成普通
  返回值。

## 并发与生命周期

- 固定 `workers` 个线程在 `new` 内创建，之后只消费任务队列；`submit` 不会
  为每个任务临时创建线程。
- Pool 和 Future 的值复制共享同一状态。多个 submitter 可同时入队，多个
  waiter 可同时等待同一个 Future。
- `join` 从本池 worker 内调用时，返回 `Error.SelfJoin`，不会等待自己；该检查
  先于 close，因此不会顺带关闭池。实现以当前线程 id 匹配已登记 worker；若
  底层 `thread.join` 仍返回 EDEADLK（35），也转换为 `Error.SelfJoin`。
- 跨池等待造成的循环不在检测范围内；`join` 只检测当前池的 worker 自等待。
  同一个 worker 若 `get` 自己池中尚未完成、且需要全部 worker 才能推进的 Future，
  仍可能等待自己；本模块不检测这种 Future 自等待。
- worker 线程参数持有池状态直到线程退出；没有用户析构器或循环回收，因此只
  drop Pool 句柄而不 close/join 时 worker 可能继续运行并保持状态存活。
- 不提供 worker 级取消、任务取消、超时 Future、优先级或抢占。任务函数必须是
  无捕获函数值；捕获闭包升级后本 API 保持兼容。
- 未实现自动 drop 清理。调用方不 close/join 时，worker 会继续等待任务；这是
  明确的未完成生命周期，而不是被隐藏的后台清理。
- 泄漏/分配跟踪：当前没有公开的 Wz 分配观察接口，本模块验证记录会明确标注
  `not_run`，不能把正常运行当作物种级泄漏证明。

## 验证记录

环境：Linux x86_64，Native 后端，固定 CLI
`build/woozy-stdlib-native/woozyc`，
固定 `LD_LIBRARY_PATH=${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9/usr/lib`。

验收程序使用真实 Native O0/O2、原子 started 门和最大并发计数证明 worker 并行，
覆盖多 submitter/多 waiter、背压、close 排空、close/submit 竞争、拥有结果复制、
`R=Result` typed 失败、自 join、零参数错误和无固定 sleep。harness 为
`tests/native/threadpool/run_threadpool.py`，每个 build/run 都有有限 timeout，
build/run 输出和失败项保存在运行目录的 `summary.json`。

实际结果（本次工作树，O0/O2）：

- 3 轮完整 harness：每轮 22 次 build、56 次程序执行，全部通过；合计
  66 次 build、168 次执行、0 失败。
- 验证用 CLI 快照 SHA-256：`2b2c2ad3b90740a6b8218981caffbbce4c09d61ea3375c0ef17f4eec006f4070`。
  当时 Root 正在后台重建共享 build，故在私有 mount namespace 中把同一次构建的
  CLI/helper/runtime 快照绑定到原绝对路径后运行，避免只读到正在替换的 helper；
  Root 文件未被修改。`stdlib/task/task.wz` SHA-256：
  `9178c5f3d43c18d4a2721c5f544d50073f6b91b809618691603d85b3a281eb3d`。
- `parallel`：2 workers，两个任务同时进入并阻塞，最大并发计数达到 2；一个
  Future 在另一个任务仍阻塞时先完成并可读取，证明结果早于 join。
- `multi-submit-wait`：4 个 submitter 提交 200 个任务，4 个 waiter 共享同一个
  Future；按 Future 映射逐项验证 0..199 的结果，不依赖完成顺序。
- `backpressure`：1 worker、容量 1；worker 阻塞时第二个任务占满队列，第三个
  submit 在线程中等待，释放后三个 Future 均完成且任务执行计数为 3。
- `close-drain`：close 时仍有排队任务；排空前 submit 抛 `Closed`；释放后所有
  Future 在 join 前完成。另测 `shutdown` 的 close+join 和关闭后结果保留。
- `close-submit-race`：4 个 submitter 与 closer 竞争；成功提交数与拒绝数之和
  精确等于尝试数，所有返回的 Future 最终完成且结果唯一；没有永久等待的 Future。
- `owned-results`：`string`、`Vec`、含 `string`/`Vec`/enum 的 struct 结果；
  重复 get、局部 rebind、Vec ARC 共享和 poll `Some` 均验证。
- `result-error`：`R=Result[i64,JobError]` 的 Ok/Err 作为普通结果返回，未被
  转成 `task.Error` 或抛异常。
- `self-join`：worker 内 `join` 与 `shutdown` 都返回 `Error.SelfJoin`，不关闭
  池；之后仍可 submit，主线程最终正常 join。
- `lifecycle`：close 幂等、多个 join 副本并发回收、直接 join 自动 close+join、
  `shutdown` 及 join 后 submit 拒绝均通过。
- `invalid-arguments`：0 workers、0 capacity 及两者都为 0 返回明确的 typed
  `InvalidWorkers`/`InvalidCapacity`，不创建线程或队列。
- `panic-task`：worker 内除零使进程以 101 和
  `LANG_NATIVE_PANIC:v1:division-by-zero:` 终止，没有被捕获成 Future 结果。
- 抽查 O2 产物为静态链接 ELF，无 dynamic section/NEEDED。

### 明确 not_run / 未覆盖

- **分配/泄漏精确跟踪：not_run。** 当前没有公开的 Wz 分配观察接口；现有 native
  ownership observer 属于共享编译器/测试范围，本 task 未修改也未运行。正常退出
  和结果校验不等同于物理内存泄漏证明。
- **worker spawn 失败清理：not_run。** 要稳定触发 `SpawnFailed` 需要耗尽线程/
  资源，未在共享机器上故意制造；已实现 close+join 已创建 worker 的清理路径，
  但没有运行证据。
- 极端 `workers`/`capacity` 的资源上限没有压测；实现不做未检查的计数乘法，
  0 值已在任何分配前拒绝。
- 跨池等待循环不检测；只检测本池 worker 自 join。
- 捕获闭包、timed Future、取消、自动 drop 清理和 Python 后端不在本次范围。
