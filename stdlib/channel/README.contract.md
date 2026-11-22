# `std/channel` 基础契约（channel/1）

状态：本模块实现有类型的 bounded/unbounded FIFO channel、阻塞收发、非阻塞
try 收发和幂等 close。带超时等待与取消 token 不在本模块本次范围内；它们由
Root 的另一 worker 提供底层能力后再单独整合。因此本交付只声明“基础队列与
关闭协议已实现并运行验证”，**不声明高层并发/取消目标全部完成**。

本模块仅使用 `std/sync`、`std/vec` 和 `std/option`，不新增宿主运行库依赖，
不使用捕获闭包、`inout` 字段或裸指针。

## API

```wz
import "std/channel" as channel;

pub enum Error { Closed; }
pub enum SendResult { Sent; Full; Closed; }
pub enum ReceiveResult[T] { Item(value T); Empty; Closed; }

pub func bounded[T](capacity usize) Channel[T];
pub func unbounded[T]() Channel[T];

pub func send[T](pipe Channel[T], value T) throws Error;
pub func receive[T](pipe Channel[T]) option.Option[T];
pub func try_send[T](pipe Channel[T], value T) SendResult;
pub func try_receive[T](pipe Channel[T]) ReceiveResult[T];
pub func close[T](pipe Channel[T]);
```

| 调用 | 结果 |
| --- | --- |
| `bounded[T](capacity)` | 容量固定为 `capacity` 的 channel；`capacity == 0` 产生运行时 Panic |
| `unbounded[T]()` | 无固定容量的 channel；内部按需扩容 |
| `send(pipe, value)` | 成功入队，或在 bounded 满时阻塞；close 后抛 `Error.Closed` |
| `receive(pipe)` | 返回 `Some(T)` 或 `None`；空且未关闭时阻塞 |
| `try_send(pipe, value)` | `Sent`、`Full` 或 `Closed`，从不阻塞 |
| `try_receive(pipe)` | `Item(T)`、`Empty` 或 `Closed`，从不阻塞 |
| `close(pipe)` | 幂等关闭；唤醒所有等待者；不清空已提交数据 |

## 语义

- **FIFO**：值的取出顺序与成功 `send`/`try_send` 的提交顺序一致。多生产者
  之间的全局顺序由锁决定；每个生产者的相对顺序保持。
- **bounded**：`send` 在 `size == capacity` 时等待可写条件。容量只统计已经
  成功提交、尚未被取出的值。`try_send` 在满时返回 `Full`。
- **unbounded**：`send`/`try_send` 不因容量阻塞，也不会返回 `Full`。内部
  ring 满时按逻辑顺序搬到更大的 ring。
- **close**：第一次 `close` 将状态置为 closed 并 `notify_all` 可读、可写两个
  条件变量；后续 `close` 是 no-op。`close` 不丢弃已经成功提交的值。
- **send 与 close**：若 `send` 在 close 前成功提交，值保留；若 close 先发生，
  `send` 抛出 `Error.Closed`，不会提交。等待中的 sender 被 close 唤醒后同样
  抛出 `Error.Closed`。`try_send` 先检查 closed，再检查 Full，所以关闭且满的
  channel 返回 `Closed` 而不是 `Full`。
- **receive 与 close**：关闭后先按 FIFO 排空已提交值，再返回 `None`。
  `try_receive` 在队列还有值时返回 `Item`，排空后返回 `Closed`；未关闭且为空
  时返回 `Empty`。等待中的 receiver 被 close 唤醒后执行同一排空规则。
- **空/满**：`try_receive` 在未关闭的空 channel 返回 `Empty`；`try_send` 在
  未关闭的满 bounded channel 返回 `Full`。两者都不阻塞，也不使用 sleep。

## 拥有值与别名

`Channel[T]` 是值 struct，所有可变状态放在共享 `Vec` 单元中。复制 channel
（赋值、作为普通参数、放入 struct）共享同一个队列、Mutex、Condvar 和 closed
状态；关闭任一别名即关闭该 channel。

`T` 使用普通源语言复制规则：

- 标量按值复制；`Vec` 和 `string` 复制保留 ARC 引用；struct/enum 按字段复制。
- `send` 的普通参数不消费调用方的值。成功入队后，调用方仍持有自己的值。
- 队列中的 `option.Option[T]`、出队返回值和 `ReceiveResult[T].Item` 持有它们
  应有的引用；不会为了“移动”而提前 drop。
- 队列元素按 `Vec[option.Option[T]]` 存储。取出时先复制 `Option`，再把槽位
  重置为 `None`，因此重置旧槽不会使返回值悬空或重复释放。
- unbounded 扩容时，按 `head` 开始的逻辑顺序复制 `size` 个 `Option[T]` 到新
  ring，再把 `head` 置零并替换 ring 句柄。旧 ring 的引用在复制完成后释放；
  这个顺序保证扩容搬移不丢失、不重复 drop 带所有权的元素。扩容也覆盖 ring
  已回绕、`head` 非零后再次填满的情况。

## 实现结构

- 每个 channel 一个 `Mutex`；`readable` 和 `writable` 两个 `Condvar`。
- `ring`、`head`、`size` 和 `closed` 各放在一元素 `Vec` 单元中，使所有别名
  观察到同一状态，不依赖已经过期的 struct 标量副本。
- 队列是 `Vec[option.Option[T]]` 环形缓冲区。bounded 一次分配 `capacity` 个
  `None` 槽；unbounded 从空开始，第一次需要时增长到 8 个槽，之后满时翻倍。
- 所有状态转换在 Mutex 下进行；条件变量只用来等待状态变化，条件本身每轮
  重新检查，允许伪唤醒。
- 不使用固定 sleep、宿主线程库或额外 FFI。线程创建仍由调用方通过
  `std/thread.spawn` 提供。

## 当前限制

- 不提供 timed send/receive、取消 token、select、优先级、容量查询、关闭原因
  或批量操作。等待中的调用只能由数据、空间或 close 推进。
- 不提供 Python 后端实现；当前线程/同步接口本身以 Native Linux x86_64 为准。
- `capacity == 0` 通过语言整数除零 Panic 终止程序（`division-by-zero`），
  不使用新的宿主错误接口。
- 本模块的测试和契约不包含内存泄漏检测或形式化证明；ARC 正确性由带所有权
  payload 的并发运行、校验和以及进程正常退出覆盖。

## 验证

验证使用固定的 Native CLI 与 legacy libxml2 运行库：

```sh
: "${WOOZY_TOOLCHAIN_ROOT:?set WOOZY_TOOLCHAIN_ROOT to the external toolchains directory}"
CLI=build/woozy-stdlib-native/woozyc
export LD_LIBRARY_PATH="${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9/usr/lib"
python3 tests/native/channels/run_channels.py \
  --cli "$CLI" --root "$PWD" --out /tmp/channel-verification \
  --ld-library-path "$LD_LIBRARY_PATH"
```

工作树验证时，已构建 CLI 的 `LANG_STD_SOURCE_ROOT` 指向主仓库 stdlib；为避免
修改其它标准库，使用私有 mount namespace 把一份包含本模块的临时 stdlib 副本
绑定到该路径后运行，未在主 stdlib 写入或留下文件。从包含本模块的仓库正常
配置构建时不需要该绑定。每次 build/run 都有有限超时，stdout/stderr 保存在
`--out` 下，`summary.json` 记录 CLI 哈希、环境、源码哈希、退出状态和失败项。
本次保存的完整日志在 `/tmp/channel-verification/pass{1,2,3}/`。

已运行结果（本次工作树，O0/O2，同一份源码）：

- 3 轮完整 harness：每轮 18 次 build、46 次程序执行，全部通过；合计
  54 次 build、138 次执行、0 失败。CLI SHA-256 为
  `17c7351a7094a304b49d40e12944d9cb16017d06b18844ced4a66fc978a3350a`，
  `stdlib/channel/channel.wz` SHA-256 为
  `613ba7027c04083865f549b1a082e636207065ba06725cba1a0702dde843f357`。
  抽查 O2 `bounded-mpmc` 产物为静态链接 ELF，没有 dynamic section/NEEDED。
- `bounded-mpmc`：4 生产者 × 4 消费者，容量 7，2000 条唯一消息；阻塞 send，
  校验精确条数和校验和。
- `unbounded-mpmc`：4×4，3000 条消息；消费者通过原子门闩延迟，强制所有消息
  在 ring 中驻留并触发扩容，再并发消费；校验精确条数和校验和。
- `bounded-blocking`：容量 1 的真实线程 sender；在 channel 满后阻塞，主线程
  取出前不能完成第二次 send；`try_send` 返回 `Full`，之后顺序和关闭错误均检查。
- `try-ops`：满/空非阻塞、关闭后 `send` 抛错、close 幂等、关闭且满的优先级、
  关闭后排空、ring 回绕、unbounded 不返回 `Full`。
- `close-drain`：4 个生产者先提交，4 个别名并发 close，4 个消费者关闭后并发
  排空；校验条数和校验和，排空后为 `None`/`Closed`。
- `close-wake`：4 个阻塞 sender、4 个阻塞 receiver、多个别名并发 close；所有
  等待者被唤醒，关闭前已提交的值保留，关闭后阻塞 send 不产生新值。
- `owned-payload`：bounded 容量 11，4×4 线程传递含 `string`、`Vec[i64]` 和
  enum payload 的 struct；发送后修改调用方标量字段，验证队列副本仍正确；
  另测 `Channel[string]` 和 `Channel[Vec[i64]]`。
- `unbounded-owned`：600 个带所有权的 payload 在无消费者时全部驻留并多次扩容；
  另构造 `head` 非零后填满再扩容的顺序测试，逐项验证 payload。
- `capacity-zero`：O0/O2 均以退出码 101 和
  `LANG_NATIVE_PANIC:v1:division-by-zero:` 终止。

所有并发程序使用无捕获线程入口和显式 struct 参数；等待使用 join、原子计数或
条件变量，没有用固定 sleep 证明正确性，也没有在未解锁时 join。

## 后续 timed/cancel 接口待接列表

本模块不实现下列能力，需在 Root 汇总底层后统一契约，避免各 worker 各自发明
语义：

1. `send_timeout`：在给定单调时钟截止时间内等待空间；结果建议区分
   `Sent`、`TimedOut`、`Closed`（最终名称由 Root 契约决定）。
2. `receive_timeout`：等待数据或关闭；超时返回可区分的空结果，关闭仍优先
   排空。
3. 取消 token：等待 sender/receiver 可被 token 取消；需要明确 cancel 与
   close、数据提交、部分出队之间的竞争优先级。
4. 底层 timed condvar：不能用 sleep 轮询伪造；需要 Root worker 提供的可控
   单调时钟等待原语。
5. 集成测试：超时精度只做有限边界检查，不承诺硬实时；必须覆盖超时与 close
   同时发生、取消与已提交数据、多个等待者以及 O0/O2。
6. 若新增 select：必须继续遵守普通复制/ARC 和“close 后排空”规则，不改变
   本契约已有的 `send`/`receive`/`try_*` 行为。
