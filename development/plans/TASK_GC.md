# Task / coroutine frame 的 GC 接入

设计对象是 Native 的 Future、LLVM coroutine 和 Slot.owner/active。
普通async参数私有复制已合入2c9cd64，Hosted Thread清理修正已合入973328d。Source frame metadata、Task图跟踪与fork恢复已在Root验证副本组合，通过Frame32、内存24及相关21项CTest；主目录完整171/171 CTest通过（152.93秒），待提交推送。下文记录实现要求和保留限制。

## 1. 保留 executor 强引用，不给 collector 增加协程 finalizer

继续规定：`spawn` / 隐式启动 await 给任务增加 **一个** `kActive` 强引用；只有 body 已执行 normal/error/cancel 清理并返回后，`complete` 才释放它。GC 不追踪 executor 的这个外部根。

因此，已启动且尚未结束的 Task，无论 queued、running、timer/fd suspended，都满足 `refs > traced_incoming`，不能成为 GC 候选。GC 候选只可能是：

- 从未启动，只有创建时保存的参数，尚未登记任何源码 defer；
- 已成功、typed error 或取消完成，源码 defer 已执行。

`Future.gc_clear` 断言 `!kActive && (!kStarted || status != Pending)`；不调用 resume、cancel exit 或用户 defer。不要自动取消“用户没有 Task handle、但 executor 仍在执行”的任务：它由 executor 持有，按现有规则继续运行，cancel/shutdown 才结束它。

这条规则覆盖未启动/已完成 Task 的强循环；不会回收无限等待且未取消的 spawned Task。后者是有 executor 根的活动任务，不应标为不可达垃圾。

## 2. 私有字段和 C ABI

新增 `runtime/native/async.hpp`，只声明共享 ABI；`Future` 仍留在 async.cpp。保留 `refs` 为首字段。

```cpp
#include "gc.hpp"

struct LangAsyncFrameGcOps {
  // 为空表示该函数的 frame / promise 不可能持有 Wz 图强边，不注册 GC。
  void (*trace)(void *state, LangGcVisitor visitor, void *context) noexcept;
  // 丢弃仍拥有的值；幂等；不执行源码 defer，不调用 coro.resume。
  void (*clear_owners)(void *state) noexcept;
};

extern "C" void lang_async_publish_v2(
    void *future, void *handle,
    void (*resume)(void *) noexcept,
    void (*destroy)(void *) noexcept,
    void *promise, void *gc_state,
    const LangAsyncFrameGcOps *frame_ops) noexcept;
```

Future 的现有 `reserved` 改为 `uint32_t gc_registered`；现有 `release_promise` 字段改为 `void *gc_state`，另加 `const LangAsyncFrameGcOps *frame_ops`。promise 单独 release 的工作合入生成的 clear_owners；不用再保存一份 runtime release_promise 指针。编译器和两种 runtime 同时改用 publish_v2，避免旧六参数调用误接新函数。

async.cpp 提供一份静态 `LangGcOps`：

```cpp
static void task_gc_trace(void *, LangGcVisitor, void *) noexcept;
static void task_gc_clear(void *) noexcept;
// retain/release 直接使用 lang_async_task_retain_v1 / release_v1。
// invalidate_weak = nullptr；本次不新增 Weak[Task]。
```

`task_gc_trace` 调用 frame_ops->trace(gc_state, ...)。`task_gc_clear` 检查第 1 节条件，调用 clear_owners；未启动的 Pending 随后通过共同的完成/摘列表代码变为 Cancelled。不能只改 status 而留下 g_all、ready、waiter、epoll 记录。

## 3. typed metadata 放在 coroutine 中，不计算 frame 偏移

每个 async **函数 SymbolId** 生成一个 LLVM metadata 结构、一份常量 FrameGcOps、两个回调。不能只按 Task[R,E] 缓存，因为不同函数的参数和局部槽不同。

metadata 结构是编译器内部类型：

```text
{ promise_address,
  owner_slot_0_address, active_flag_0_address,
  owner_slot_1_address, active_flag_1_address, ... }
```

枚举 `f.slots` 中的所有 `slot.owner`，包括 binding、temporary、iteration 和参数。metadata 保存 slot/flag 的 **地址**，不保存值的快照；`Op::store/move/take/destroy` 继续操作现有存储和 active。借用槽、inout 槽不作为拥有者登记。

metadata、slot、flag 都通过现有 coroutine 内的 LLVM alloca 创建。把 metadata 地址发布到 Future 后，CoroSplit 必须把这批逃逸地址及其存储放进 heap frame。生成的 typed 回调根据自己已知的 metadata 结构 GEP，再用对应 TypeId 的 LLVM 类型读取 slot；不从 handle 猜 CoroSplit 的字段编号，不在 C++ 中解释 i1 flag。

- `trace`：对每个 active=true 且 `has_strong_edge(type)` 的槽调用 `gc_trace_value`；再追踪有效的 promise 拥有值。每个实际 ARC owner 恰好计一次，重复指向同一对象也分别计边。
- `clear_owners`：按逆序处理 active owner，**先清 active，再调用该类型的 drop**；随后按 promise 状态/拥有位丢弃有效结果或错误。重复调用没有动作。
- await 的 Task 临时值必须在 owner slot 中跨挂起保存。当前运行库的 `waiting_on` 和 `waiters` 双向链各持一份真实强引用，必须分别计边；不能重复计算源码 await 槽，也不能漏掉运行库新增的引用。ready、g_all 仍是借用指针；kActive 是执行器的外部根。
- timer/fd Future 没有 frame / Wz 强边，不注册 GC；Vec[Task] 指向它们的边可按现有 collector 规则指向未注册叶对象。

## 4. 创建、发布和退出顺序

publish 位于初次 suspend 之前，参数槽、active 标志和 promise 初始化后才发布：

1. task_new：refs=1，所有 runtime 字段置零；尚未向 GC 注册，也不加入 executor 的 g_all。timer/fd 的完整对象仍可在各自构造完成后直接加入。
2. coro.begin；创建 promise、全部 owner slot/active 和 metadata。
3. active 初值均 false，promise 为 Pending、无有效拥有槽；填好 metadata 的全部地址。
4. 对 async 普通拥有型参数做普通 copy，写入参数槽，再设 active=true；move 参数直接接收所有权并设 true。至此参数由 frame 独立持有。
5. publish_v2 填好 handle/resume/destroy/promise/state/ops。若 trace 非空且 program enabled，调用 `lang_gc_program_register_v1(future, &task_gc_ops)`；成功后置 gc_registered。enabled 时注册失败应 Panic，不能静默漏掉图节点。全部准备好后再加入 executor 的 g_all；不要让 shutdown 看见尚未 publish 的 Future。
6. 初次 suspend；start 返回 Future。此后才允许调用者 spawn、保存、复制或释放 Task。

普通参数的**源语言模式仍为 ordinary**，不应把它改成 move 来获得清理。最小修改可用现有参数 owner slot：async prologue 给它私有副本，common async.final 在源码 exit plan / defer 结束后，再 drop 仍 active 的普通参数副本。move 参数继续沿用 HIR 清理；never-started 的参数由 clear_owners 处理。需要把 async inout 暂时明确拒绝，不能把调用者栈地址交给可逃逸 Task。

normal/error/cancel 的源码清理结束后进入 async.final，再完成普通参数副本清理，最后 final suspend；run_one 读到终态后才 complete / 释放 executor 根。未启动 Task 的最后一次 release 则直接 clear_owners + coro.destroy，不进入用户 body。初次 suspend 的 destroy 分支应直接到 frame-free cleanup，不能复用 body 分支。

最后一次 `release` 在 async 短锁中复核 refs=1 并摘除借用表项，释放短锁后保持 refs=1 执行真实 GC unregister；完成注销才清 gc_registered、将 refs 置零并进入 `destroy_future`。随后释放运行库拥有边、执行幂等 clear_owners、coro.destroy 和 free。不得持 async 锁获取 GC 锁，也不得在 frame 已释放后调用 typed 回调。最后 release 不取得跨用户 atfork 回调的首次绑定预留，因此 prepare/parent 回调等待最后 release 的线程不会因此死锁。

## 5. promise 的四种状态

继续复用 promise 第二个 u32，但固定其含义为“该槽不归 promise 拥有”的位图：result=1，error=2。重复 await **复制**结果/错误，不改变这些位。Unit 没有对应字段，始终视为不拥有。

| 状态 | promise 持有内容 | trace / clear |
| --- | --- | --- |
| Pending=0，包括 never-started | 没有 result/error；两位=3 | 只看 active owner slots |
| Done=1 | 非 Unit result 已初始化；error 位=2；result 位=0 | 仅追踪/丢弃仍归 promise 的 result |
| Error=2 | 非 Unit typed error 已初始化；result 位=1；error 位=0 | 仅追踪/丢弃仍归 promise 的 error |
| Cancelled=3 | 无 result/error；两位=3 | 清理完成后无 promise 强边；未启动取消可能仍有参数 owner |

发布终态前先写 payload 和拥有位；drop 前先置对应“不拥有”位。只有状态有效且该位为 0、且该类型实际 needs_drop 时才 drop，防止读未初始化槽或第二次释放。这部分与 Root 正在接入的结果修正采用同一规则，不能两边分别维护一套状态。

## 6. 修改位置与明确限制

- `value_ops.inc::value_op`：Task 专用 retain/release；普通容器/聚合/closure 递归沿用现有实现。
- `gc_ops.inc::has_strong_edge / gc_trace_value`：Task 是可能成环的强边，访问 Future；复用递归 aggregate trace。
- `emit.cpp::function`：参数私有副本、metadata、发布位置、typed trace/clear、common final 参数清理、initial destroy 分支；`run` 中原 async_release 生成部分改成 FrameGcOps 回调生成。
- `async.hpp / async.cpp`：new_future/task_new 的延后发布、publish_v2、Future 字段、静态 GC ops、注册/最终销毁；继续使用 Root 修复后的列表处理函数。epoll_wait 周围加 GC SAFE，但离开 SAFE 后才能触碰 Future/队列/metadata。
- `sema/check.cpp` 的 async 参数检查：暂拒绝 inout。无需改 Slot.owner 的含义、HIR 参数模式或 collector 算法。

现有 `lang_gc_runtime_defer_release_v1(payload, release)` **只接收外部 C/reference payload，明确禁止纯 Wz 强边**。不能把整个 frame、Task、Vec 或其参数记录交给这个队列。纯 Woozy owner 在 clear 中立即按 ARC 丢弃；如槽内含外部 C 资源，其共享 drop wrapper 可调用该函数，返回 true 即转移该外部引用，false 才同步 release。队列已有 world_resume、busy=0 后 drain 的顺序。当前生产代码尚未把所有 C resource release 接到该队列，不能直接宣称 foreign leaf 清理已经支持；本次不支持的混合字段应明确拒绝，或把该小项一起实现。

本次不新增 Weak[Task]、多线程并发 run/spawn/cancel、自动取消不可见 spawned Task，也不宣称回收 foreign Python/C 循环。任何访问 metadata 的线程都必须是已注册 mutator，发布/更新不能发生在 SAFE 内；executor 全局列表的跨线程 final-release 同步仍由 runtime 修正负责，不能用 GC trace 替代。

## 7. 必须实际通过的检查

1. O0/O2：未启动 Task↔Vec、已完成 result↔Vec、typed error↔Vec 三类循环；observer 确认 Future、frame、Vec 最终各 free 一次，而非只看 exit0。保留当前 gc-task-cycle 小例。
2. source Task 复制/聚合/Vec/closure 持有；删掉一个 alias 不 free，删掉最后一个 alias 后正常 free；重复 await 拥有型 result/error 后值仍有效。
3. 普通 Vec/string/closure/聚合参数在调用方出块后仍可用；move 参数只 drop 一次；never-started、success、typed error、cancel 四种退出分别检查。
4. 悬挂任务只剩 executor 根时，其他线程施加 GC 压力仍不回收；defer 读取 frame 内值；cancel/shutdown 后 defer 恰执行一次且 frame 最终 free。GC clear callback 中不得 resume 或执行该 defer。
5. 使用实例私有输出目录；构建前后 LLVM 检查 metadata 与所有逃逸 slot/flag 地址确实属于 heap frame，O2 不得留下发布后的栈地址；检查未初始化/已 move slot 不被 trace。
6. 原 ASan registry/waiter UAF 小例、hosted async 与纯 Woozy GC 原7例保留；带外部资源时确认其 release 发生在 world_resume 后，或明确诊断拒绝未实现字段。

以上是待实现设计，未运行新测试；不需要改 gc.cpp 的候选、pin、clear-check 或统计定义。

## 分步接入约束

1. 先接运行库 publish_v2、Future 的 GC 登记和实际强边遍历，并保留 publish_v1，现有编译器仍可链接运行。运行库测试不能冒充真实源码已经接入。
2. 然后接编译器 typed slot metadata、普通参数保活和 v2 调用，使用真实源码及分配观察器验证。
3. no-graph 启动优化正在合入：编译器生成有 trace 的 Task FrameGcOps 时，必须设置 `gc_program_required=true`，即使登记动作在 publish_v2 的运行库内部执行。
4. 单线程执行器不允许同时 run/spawn/cancel；Task 句柄跨线程复制和最后释放仍须保护运行库内部登记表，不能把内部表竞争留给程序员。任意析构回调不得在持执行器锁或世界暂停时执行。实际接口限制必须明确诊断或 Panic，不能静默产生数据竞争。
5. 暂不增加 Weak[Task]。捕获型 C 回调与其外部所有权规则仍等用户选择；无捕获静态回调另做实际测试。
