# Weak 控制块隔离验证结果

日期：2026-11-12（Asia/Shanghai）
范围：MEM01 的底层 Weak 控制块；不代表 MEM01 整体或源码级 `Weak[T]` 完成。

## 环境

```text
host:       Linux 7.2.15-1-omarchy x86_64
clang++:    clang version 22.1.8 (https://github.com/llvm/llvm-project ca7933e47d3a3451d81e72ac174dcb5aa28b59d1)
ld.lld:     LLD 22.1.8 (https://github.com/llvm/llvm-project ca7933e47d3a3451d81e72ac174dcb5aa28b59d1)
llvm root:  ${WOOZY_TOOLCHAIN_ROOT}/llvm-22.1.8
legacy:     ${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9
            libxml2-legacy-2.13.9-2-x86_64.pkg.tar.zst
            sha256 aac28a52e81422935e3bfa44044030640c5d525ad8f65a9672a3ab7139974652
            ld.lld 通过 LD_LIBRARY_PATH 使用该包内的 libxml2.so.2
```

`tests/native/weak/run.sh` 会拒绝非 22.1.8 的 clang++/ld.lld，并确认 legacy
libxml2 文件存在。固定 linker 是通过 `-fuse-ld=lld` 和 `PATH` 选中的
`${LLVM_ROOT}/bin/ld.lld`。

## 主验证

命令：

```sh
timeout 700s tests/native/weak/run.sh
```

实际结果：`STATUS=0`，耗时约 3.5 秒（不含本报告记录）。所有脚本内部
编译/链接/运行命令都有独立 `timeout`。

### 生产 freestanding flags + host 并发

被测 `runtime/native/weak.cpp` 按运行时 flags 单独编译：

```text
-std=c++20 -O2 -ffreestanding -fno-builtin -fno-exceptions -fno-rtti
-fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset
-fno-builtin-memmove -fno-stack-protector
```

实际输出：

```text
empty-weak: PASS (0 ms)
basic-lifecycle: PASS (0 ms)
concurrent-downgrade: PASS (2 ms)
upgrade-vs-final-release: PASS (62 ms)
address-reuse: PASS (0 ms)
fork-hooks: PASS (29 ms)
panic-contracts: PASS (0 ms)
weak_test: PASS (control blocks live=0)
```

`weak.o` 未定义符号集被脚本断言为恰好：

```text
U lang_alloc_v1
U lang_free_v1
U lang_native_panic_v1
```

没有 libc、pthread 或额外运行时符号。

### ASAN/UBSAN oracle

`weak.cpp` 仍使用 freestanding 代码模型，为 oracle 只去掉
`-fno-sanitize=all` 并加 `-fsanitize=address,undefined`。allocator/panic
继续使用生产对象；分配观察器在 free 时毒化控制块，在 alloc 时解除毒化。
`alloc.cpp` 的自定义 mmap 分配本身不被 ASAN 跟踪，因此该 oracle 主要检查
控制块释放后访问、harness 内存错误和 UB；目标对象的 UAF 由 `munmap` 保护。

实际输出：

```text
empty-weak: PASS (0 ms)
basic-lifecycle: PASS (0 ms)
concurrent-downgrade: PASS (3 ms)
upgrade-vs-final-release: PASS (93 ms)
address-reuse: PASS (0 ms)
fork-hooks: PASS (44 ms)
panic-contracts: PASS (2 ms)
weak_test: PASS (control blocks live=0)
```

运行环境为 `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`、
`UBSAN_OPTIONS=halt_on_error=1`。

### `-nostdlib` 静态 ELF

链接生产 `weak.cpp`、`alloc.cpp`、`panic.cpp` 和 `nostdlib_test.cpp`：

```sh
clang++ -fuse-ld=lld -nostdlib -static -Wl,-e,_start -Wl,--no-undefined \
  -Wl,-z,noexecstack -Wl,--build-id=none ...
```

实际检查：

```text
ELF type: EXEC
undefined symbols: <none>
dynamic NEEDED entries: none
run exit: 0
```

`nostdlib_test.cpp` 从 `_start` 直接调用测试体，并使用 Linux exit syscall；
没有 `main()`、libc、libstdc++ 或动态加载器。

## 竞争压力与重复性

在主验证之后保留构建目录并重复运行：

```text
20 次未绑核生产测试：                0 失败
2 次 taskset -c 4,5 生产测试：       0 失败
2 次 taskset -c 6,7 ASAN/UBSAN 测试：0 失败
```

每个阶段都实际运行多线程竞争：

- 8 个参与者并发 downgrade 同一 slot，穿插 upgrade、retain/release，共 3000 次/线程。
- 80 轮 upgrade 与最终强释放竞争；每轮先让 racer 进入 upgrade，释放 pin 后等待
  target 实际 detach，再要求 death 后的 upgrade 尝试。
- 24 次与 allocator fork 钩子组合的 fork；后台 upgrade worker 持续运行。
- 地址复用使用 `munmap` 后对同一地址 `MAP_FIXED`，旧 Weak 必须保持 dead。

验证过程中发现并修正了一个测试竞争窗口：最初按“释放后固定尝试数”停止 racer，
在某些调度下最后一个强引用仍由被抢占的 worker 持有，固定尝试数可能先耗尽，导致
没有观察到 dead target。修正为等待 target 实际 detach，再要求 death 后的 upgrade
尝试；实现本身未因此修改。最终压力结果全部通过。

## 覆盖矩阵

| 要求 | 实际覆盖 |
| --- | --- |
| 自建首字段 `u64` 强计数对象 | `Fixture::strong_refs` 偏移 0，`static_assert` |
| 最终 release 调 detach 后销毁/释放 | `fixture_release`：detach → 标记 → destroy → `munmap` |
| 多线程 upgrade 与最后 strong 释放 | 80 轮，racer 与死亡后失败观察 |
| 同一 slot 并发 downgrade | 8 线程 × 3000 次，确认单一控制块 |
| copy/drop | 多线程 retain/release 与 dead Weak 拷贝 |
| 空 Weak | null 全部 no-op，upgrade 返回 null |
| dead Weak | alive=false、upgrade=null、copy/drop 后控制块归零 |
| 对象地址复用 | `munmap` + `MAP_FIXED` 同地址新对象，旧 Weak 不 ABA |
| fork 钩子 | weak prepare/parent/child + allocator 钩子组合，子进程可升级 |
| 最后控制块分配计数归零 | 链接期 wraps 统计 allocation/free/live，所有阶段和最终检查均为 0 |
| Weak 计数 Panic | retain 溢出、release 下溢均 exit 101 且记录 cause 7 |
| 强计数边界 | strong=0 和 strong=UINT64_MAX 时 upgrade 返回 null |
| `-nostdlib` 静态 ELF | 无 libc/动态依赖，直接 `_start` 运行成功 |

## 未执行 / 不声明

以下均未在本任务中实现或验证：

- 源码级 `Weak[T]`、类型检查、编译器 IR lowering、生成代码中的 Weak 拷贝/销毁。
- `LangVec`/`LangHandle` 追加控制块槽位及布局/ABI 更新。
- `process.cpp`、Root CMake、编译器和现有 allocator 的修改。
- GC trace、自动强环收集、整 MEM01 验收、真实 Woozy 语言 Weak 用例。
- 跨平台（当前 allocator 仅 Linux x86_64）。
- 提交、push、远端 CI。

## 交接给 Root

调用顺序、对象槽位前提、最终强释放顺序、fork 钩子组合和限制见
`tests/native/weak/README.md`。Root 接入后应重新运行本目录测试，并补充
Vec/Handle/真实 Weak 语言路径的集成回归；不能把本报告当作 MEM01 整体完成证据。

## 2026-11-12 Weak invalidation / GC integration update

范围：在已接受的 16 字节 Weak 控制块上增加 GC white-candidate invalidation，并验证它与已接受 GC kernel 的组合顺序。这仍不代表源码级 `Weak[T]`、自动 GC 或整个 MEM02 已经完成。

固定工具链：

```text
clang++: clang version 22.1.8 (ca7933e47d3a3451d81e72ac174dcb5aa28b59d1)
ld.lld:  LLD 22.1.8 (same LLVM build)
legacy:  ${WOOZY_TOOLCHAIN_ROOT}/libxml2-legacy-2.13.9
```

新增行为：`lang_weak_invalidate_slot_v1` 在 Weak 锁内将 canonical slot 替换为永久 closed tag（自然对齐下不可能出现的指针值 1），清空旧控制块的 target，并在锁外释放隐含的 Weak 引用。slot 关闭后，后续 downgrade 直接返回 null 且不再重装控制块；指向旧真实控制块的 Weak 仍可正常 copy/drop，但 alive/upgrade 均表现为 dead；final release 遇到 closed tag 时，detach 为无害 no-op。同时，downgrade 读取并 retain 旧控制块的路径已改为在 Weak 锁内重读并 retain，防止 invalidate 释放隐含引用后触发对已释放控制块的 UAF；持锁期间不分配、不释放、不 Panic。

实际运行：

```sh
LANG_WEAK_BUILD_DIR=/tmp/gcweak-all-weak LANG_WEAK_KEEP_BUILD=1 \
  timeout 900s tests/native/weak/run.sh
```

结果：O2 host、O0 host、ASAN/UBSAN、O2 `-nostdlib` 静态 ELF、O0
`-nostdlib` 静态 ELF 全部通过。新增阶段输出：

```text
invalidate-empty-slot: PASS
invalidate-live-control: PASS
invalidate-address-reuse: PASS
invalidate-concurrent: PASS
weak_test: PASS (control blocks live=0)
```

同一更新还运行了 GC kernel 与真实 GC+Weak 集成测试；完整命令、源 hash 与
集成结果见本轮交接报告。weak.o 的未定义符号仍恰好为：

```text
U lang_alloc_v1
U lang_free_v1
U lang_native_panic_v1
```

静态 ELF 均检查为 `EXEC`、无 `INTERP`、无 `NEEDED`、无强未定义符号。
