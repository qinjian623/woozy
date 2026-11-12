# MEM01 底层 Weak 控制块（独立子范围）

本目录和 `runtime/native/weak.*` 只实现 Weak 的运行时控制块，不是 MEM01
整体完成。源码级 `Weak[T]`、编译器类型检查/降级、`LangVec`/`LangHandle`
追加槽位、GC trace、组集成和真实语言用例均不在本任务内。

## 文件

- `runtime/native/weak.hpp`：私有运行时 ABI、调用前提和 fork 钩子说明。
- `runtime/native/weak.cpp`：控制块、专用全局锁、原子升级、detach、fork 重置。
- `tests/native/weak/weak_test.cpp`：host 并发/生命周期/地址复用/fork 测试。
- `tests/native/weak/weak_alloc_observer.cpp`：链接期分配观察与 ASAN 毒化。
- `tests/native/weak/nostdlib_test.cpp`：无 libc 的静态 ELF 冒烟测试。
- `tests/native/weak/run.sh`：固定 LLVM 22.1.8 + legacy libxml2 的三套验证。
- `tests/native/weak/RESULTS.md`：本轮实际运行结果。

## 控制块语义

对象强计数保持原位：目标对象地址偏移 0 的 `u64`，仍由现有 ARC 原子更新。
`LangWeakControl` 是独立的小分配（16 字节），不改变 Vec/Handle 对象大小。

```text
weak_refs = 真实 Weak 拥有者数量 + 目标活着时的一个隐含引用
target    = 目标对象地址；detach 在 Weak 全局锁内清空
```

- `lang_weak_downgrade_v1(target, owner_slot)`：调用者必须先持有强引用，
  `*owner_slot` 初始为 null。并发调用使用同一槽位的原子 CAS 汇聚到一个控制块；
  每次成功调用返回一份真实 Weak 引用。竞争失败的候选控制块立即 `lang_free_v1`。
- `lang_weak_retain_v1` / `lang_weak_release_v1`：Weak 拷贝/销毁；null 为空 Weak。
  `u64` 计数溢出或下溢使用现有 Panic cause 7（`reference-count-overflow`）。
- `lang_weak_upgrade_v1`：在 Weak 全局锁内读取 target，并 CAS target 首字段
  `u64`。只有目标强计数非 0 且不是 `UINT64_MAX` 时才 `+1` 并返回 target；
  否则返回 null。成功返回值已经带一份强引用，调用者按目标真实类型 release。
  强计数溢出在这里不是 Panic，而是升级失败。
- `lang_weak_alive_v1`：仅提示，不保证随后升级成功。
- `lang_weak_detach_v1`：最终强释放调用一次。先在 Weak 全局锁内清空 target，
  再释放隐含 Weak 引用；目标内存只能在 detach 返回后 free。重复调用无害。
  null 和 closed tag 都是无害 no-op，因此 final release 可以直接传入已被 GC
  invalidate 的槽值，而不会读取已释放 control。
- `lang_weak_invalidate_slot_v1`：GC 白色候选专用。调用者必须持有强引用（GC
  已先 pin 全部候选）。在 Weak 全局锁内原子地把 canonical slot 换成永久 closed
  tag（指针值 1），清空旧 control 的 target，锁外释放隐含 Weak 引用。之后同槽
  downgrade 返回 null，不会安装新 control；旧真实 Weak 仍可 copy/drop，但
  upgrade/alive 为 dead。重复 invalidate 和无 control 的空槽都幂等。
- 目标槽位属于目标。detach 不负责清槽；目标释放后不得再读取该槽位。存储复用时
  必须先写入 null。closed tag 只在存储复用并重新构造后才被 null 替换。

Weak 全局锁只保护 target 指针读取/CAS 和 detach。分配、free、Panic、payload
析构都不在锁内；因此不存在 Weak 锁 -> allocator 锁的反向持有。锁是 ticket
自旋锁，不使用 pthread/libc/futex。

## Root 接入顺序（本任务未改现有文件）

### 1. 构建

Root 后续把 `runtime/native/weak.cpp` 加入 `lang_native_runtime`，并继续使用
现有 freestanding flags：

```text
-ffreestanding -fno-builtin -fno-exceptions -fno-rtti
-fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memmove
```

`weak.cpp` 的未定义符号只有 `lang_alloc_v1`、`lang_free_v1` 和
`lang_native_panic_v1`。

### 2. 对象槽位

在 `LangVec`、`LangHandle` 或生成对象末尾追加一个
`LangWeakControl *weak_control` 槽，保持 `refs` 仍在偏移 0。不要把它塞进现有
字段或改变旧字段偏移。新对象初始化为 null；对象内存地址复用/重新构造时再次
置 null，然后才能发布对象。

`owner_slot` 由调用方传入，Weak 实现不假设槽位在目标中的具体偏移，也不读取目标
大小。目标首字段必须是原子 `u64` 强计数。

### 3. Weak 值的生成代码

- 源码级 Weak→控制块的降级路径持有强引用，调用一次
  `lang_weak_downgrade_v1(target, &object.weak_control)`。
- Weak 拷贝调用 `lang_weak_retain_v1`；Weak 销毁调用
  `lang_weak_release_v1`。
- 升级结果按目标的强引用语义释放；不能对升级结果再次调用 Weak release。

### 4. 最终强释放

在最终 ARC 释放路径中，强计数从 1 CAS 到 0、获取 acquire fence 之后：

```text
按现有顺序 drop payload
if (weak_control) lang_weak_detach_v1(weak_control);
按现有路径释放目标内存
```

本任务不要求 detach 在 payload drop 之前；要求是在目标内存 free 之前、且
detach 返回后才能 free。目标活着时槽位指向的控制块一定活着（隐含引用）；
detach 之后目标即将释放，不得再读槽位。

### 4.1 GC 白色候选顺序

GC 必须先把全部候选 retain pin 住，再对全部候选调用对象的
`invalidate_weak`（内部只执行
`lang_weak_invalidate_slot_v1(&object.weak_control)`），最后才执行第一个
`clear`。不能边 clear 边 invalidate：否则 `clear(A)` 可能通过 Weak upgrade
复活尚未 clear 的白色 B，并逃逸出可观察的强引用。closed tag 的最终 detach
是无害 no-op。

### 5. Fork 钩子

Root 把 Weak 钩子接到现有 `process.cpp` 的集中 fork 路径。推荐顺序：

```text
lang_alloc_fork_prepare_v1();
lang_weak_fork_prepare_v1();
pid = sys(57);
if (pid == 0) {
  lang_alloc_fork_child_v1();
  lang_weak_fork_child_v1();
  ... reset other inherited locks ...
} else {
  if (pid < 0) { /* 同样释放父侧钩子 */ }
  lang_weak_fork_parent_v1();
  lang_alloc_fork_parent_v1();
  ... unlock other parent locks ...
}
```

Weak 代码不会在持锁时 fork，也不会在持锁时分配。父侧释放、子侧重置必须成对；
`fork()` 失败也要调用父侧释放。测试中已经按上述组合顺序运行。

### 6. Root 侧验收

本目录测试只验证控制块。Root 接入后还需要补充：

- Vec/Handle 追加槽的布局/ABI 检查和构造初始化。
- 真实 `Weak[T]` 的源码语义、拷贝/销毁、throws/defer/panic 清理路径。
- 纯 Woozy 强环的编译器/标准库方案；本任务没有 GC trace 或自动环收集器。
- 现有强 ARC retain/release 与新 detach 顺序的集成回归。

## 运行

```sh
tests/native/weak/run.sh
```

脚本检查并使用固定 `LLVM 22.1.8` 的 `clang++`、`ld.lld`，以及
`libxml2-legacy-2.13.9`（ld.lld 运行时的 `libxml2.so.2` 来源）。可通过以下环境
变量覆盖路径：

```text
LANG_LLVM_ROOT LANG_LIBXML2_ROOT CXX NM READELF LLD LANG_WEAK_BUILD_DIR
```

每次编译、链接和运行都有独立 timeout。三套验证：

1. 被测 `weak.cpp` 按生产 freestanding flags 单独编译，host harness 分别在
   `-O0`/`-O2` 运行；使用 std::thread/std::barrier，最终 release 调 detach 后
   `munmap` 目标。
2. ASAN/UBSAN oracle：`weak.cpp` 仍按 freestanding 代码模型编译，只去掉
   `-fno-sanitize=all`；分配观察器对已释放控制块做 ASAN 毒化。这是额外的
   oracle，不是生产编译标志。
3. `-nostdlib -static` ELF：`-O0`/`-O2` 均链接生产 `weak.cpp`、`alloc.cpp`、
   `panic.cpp`、`freestanding.cpp`，检查无强未定义符号、无 `NEEDED`、ELF 为
   静态 `EXEC`，并从 `_start` 直接运行。

当前覆盖：

- 空 Weak；null retain/release/upgrade/detach。
- 基本生命周期、控制块唯一安装、真实 Weak 计数和隐含引用。
- 同一 `owner_slot` 的多线程 downgrade/upgrade/copy/drop 竞争。
- 多线程 upgrade 与最终强释放/detach 的竞争。
- empty slot/live control 的 invalidate、closed slot 重复 invalidate、closed
  slot 后续 downgrade 不重装、final release 遇 tag 无害、地址复用后重新构造。
- invalidate 与并发 downgrade/upgrade 的边界，包括旧 control 只剩隐含
  引用时被 invalidate 立即释放的路径。
- dead Weak 的 copy/drop 和升级失败。
- `munmap` 后 `MAP_FIXED` 地址复用，验证旧控制块不会 ABA 到新对象。
- 与 allocator 钩子组合的 fork prepare/parent/child。
- 最后控制块分配观察计数归零。
- Weak 计数溢出/下溢 Panic cause 7；强计数最大/零时 upgrade 返回 null。

## 明确限制

- 不实现源码级 `Weak[T]`、类型检查、编译器 lowering 或标准库 API。
- 不修改 `LangVec`/`LangHandle` 布局、allocator、process.cpp、CMake 或编译器。
- 不实现编译器 GC trace/自动强环触发；跨模块的 GC+Weak 顺序由
  `tests/native/cycle_gc/` 的集成测试验证，Root 仍负责 safepoint/自动触发。
- 底层运行时目标当前仍是 Linux x86_64（allocator 限定）；Weak 的 pause 指令有
  aarch64 分支，但本任务未声称跨平台运行。
