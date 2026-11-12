# Native source `Slice[T]` contract

任务：LANG03 的明确 SourceSlice 接入步骤（Native only）
契约：`compiler-dev/1`，Native runtime ABI `lang-native-scalars/1`
日期：2026-11-12

## 1 公共源接口

`Slice[T]` 是 builtin generic 类型名，不新增关键字。模块写 `Slice[T]` 即可；
`std/slice` 提供以下函数：

```woozy
import "std/slice" as slice;

pub func view[T](values Vec[T], start usize, end usize) Slice[T]
pub func len[T](value Slice[T]) usize
pub func sub[T](value Slice[T], start usize, end usize) Slice[T]
pub func to_vec[T](value Slice[T]) Vec[T]
```

- `view` 创建 `[start, end)` 半开区间视图，端点为 `usize`，相对当前 Vec。
- `len` 返回当前视图长度。
- `sub` 创建相对当前视图的子视图；`start == end` 是合法空视图。
- `to_vec` 返回新的 Vec 容器；元素按普通逻辑 copy 复制。内层 Vec/结构体/枚举
  仍共享其引用对象，不是深复制。
- 泛型实参可以省略并由 `Vec`/`Slice` 实参推断；实跑覆盖 `slice.len(s)`、
  `slice.to_vec(outlives)`，以及用户泛型 `measure[T](Slice[T])` 的单态化。

`Slice[T]` 上的 `v[start:end]` 产生子 view；原 `Vec[T]` 上的 `v[start:end]`
继续产生复制后的新 Vec。string 的码点切片规则不变。

## 2 值、生命周期和别名

- 运行时值是 inline 24 字节 `{owner Vec pointer, start u64, length u64}`；
  不创建新的 heap 节点，不复制元素存储。
- 强边是 `ownerVec`：每个拥有 Slice copy 会 retain owner 并 begin 一个
  iteration pin；drop 先清空 descriptor，再 end pin 和 release owner。
- 普通借用参数不额外 retain/drop；return/move/赋值、struct/enum/Vec 嵌套和
  临时值按 HIR ownership 记录正常 copy/drop。
- owner 绑定清理后 view 仍有效：view 持有 owner 的强引用；最后一个 view 释放
  才允许 owner 结构变化。
- Slice 存活时，共享 Vec 别名的 push/pop/clear、改变长度的 resize、需要扩容的
  reserve 在修改前 Panic（`active-iteration`，退出码 101）。不改变长度/存储的
  reserve/resize 可以是无操作。
- 空 view 也 retain + pin，因此同样阻止结构修改。
- `view[i]` 读取元素的正常逻辑副本（拥有元素会 copy/retain）；`view[i] = value`
  替换元素，与 Vec 一样遵守 `let` 只禁止重绑定、不冻结对象内容。
- 元素替换和别名读取共享 owner 元素；本步骤不添加深层 `noalias` 证明，用户仍需
  自行同步并发别名访问。
- 索引先做 signed 动态值检查，再做相对/绝对 bounds 检查；负的 signed 索引不能
  绕过 bounds。越界、`start > end`、端点超出当前 view 都是 Panic。
- `to_vec` 的结果容器独立；修改结果容器槽不会改变原 view，但其中的内层引用元素
  仍是普通逻辑 copy。

## 3 Type/HIR/IR 与后端

- 公共定义只有一份：`TypeKind::Slice`、`TypeId` 参数、HIR `ValueKind::slice`、
  `VecSlice`/`IndexRead`/`IndexPlace`/`ForIn` 的既有节点和 NativeIR 的
  `vec_*`/`iteration_*`/`system_call` 操作统一随 Slice 类型检查。
- Slice 不替代 `Any`，不用裸 `usize` 表示 descriptor，也不在局部复制一套 types。
- Native 真实调用以下低层 ABI，不做源级 mock：
  `lang_slice_new_v1`、`lang_slice_copy_v1`、`lang_slice_drop_v1`、
  `lang_slice_sub_v1`、`lang_slice_at_v1`、`lang_slice_length_v1`，以及
  `lang_vec_slice_v1`（`to_vec`）、`lang_vec_set_v1`（拥有元素替换）、
  `lang_vec_begin/end_v1`（Vec 遍历/结构保护）。
- Slice 遍历不额外 pin；Slice 自身已有 lifetime pin，`iteration_end` 对 Slice
  是无操作，退出计划的 Slice drop 负责 end pin + release。
- 索引/切片 IR 必须绑定真实 HIR：`IndexRead`/`IndexPlace`/`VecSlice` 的
  receiver、index/endpoint 顺序和 presence 都按 HIR children 的精确 Source
  identity 校验；`vec_check`/`vec_set` 带 `IndexPlace` origin，`vec_set` 和
  `vec_element_address` 必须有同 origin、同 receiver/index 且支配写入的 bounds
  check。不能用同类型或相邻指令冒充。伪造元数据在 helper/emit 之前由
  NativeIR verifier 拒绝。
- Python 后端遇到 `Slice[T]`/`std/slice` 明确返回
  `E_CAPABILITY_UNAVAILABLE`，不悄悄退化为 Vec 复制。既有 Vec/string/inout 和
  Python 互操作路径不改。

## 4 明确限制（本步骤未完成）

- 仅 Native/64 提供 source Slice；Python source Slice 不提供。
- C 绑定已有 `Ptr`/`PtrMut` 和 Buffer 接口；field/index `inout`、owning mutable
  place 的重叠访问检查仍属于 LANG03 后续步骤，本步骤没有把它们标成完成。
- Slice 的 owner 强边已经可供后续 GC 遍历，但本步骤不伪装自动 GC 已完成；不改变
  GC/Weak/alloc/thread/process 的实现。
- 不提供 Slice 常量、`inout Slice`、Slice 深复制、跨线程同步或自动 noalias。
- 低层 `runtime/native/slice.cpp/.hpp` 本步骤未修改；只把它加入 Native runtime
  构建，并补充源码级接入。

## 5 可复跑测试

新增目录：`tests/native/slice_source/`。

- `run_slice_source.py`：每条正例 O0/O2 构建、运行、helper CFG/LLVM 验证，检查
  `readelf -d` 无 `(NEEDED)`、IR 无 `interp` 标记，并记录源码/程序/对象/IR 的
  SHA-256。每条命令有 timeout（默认 180s，panic 运行 60s）。
- 分配/Vec observer：用 `tests/native/vec_observer.cpp` 和
  `alloc_observer.cpp` 以 `--wrap=lang_alloc_v1/free_v1` 单独链接，验证
  ALLOC/FREE 平衡、Vec create/free、retain/release、pin begin/end 平衡。
- panic 用例断言退出码 101 和 `LANG_NATIVE_PANIC:v1:<cause>:` 记录。
- 负例断言 `E_CAPABILITY_UNAVAILABLE` / `E_TYPE_MISMATCH`，且不生成 program。
- `forged_test.cpp`：真实 load/check/lower 后，篡改 Slice system op、origin、
  slice result type、endpoint presence、read inputs、set result type、check index，
  以及用户独立复核发现的 6 个 source-identity 变体（vec_slice 端点交换、
  vec_read 错 index/receiver、vec_check 错 index、vec_set 错 index/receiver）、
  同错 check+set 和删除 check；全部由 NativeIR verifier 以 `E_IR_INVALID` 拒绝。
  变体使用已有、支配且类型正确的 SSA operands，HIR 不变。

推荐命令（本工作树私有构建，LLVM 22.1.8）：

```sh
export LD_LIBRARY_PATH=/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9/usr/lib:$LD_LIBRARY_PATH
cmake --build build-slice --target lang_slice_forged_test lang_native_helper lang_native_runtime woozyc
./build-slice/tests/native/lang_slice_forged_test tests/native/slice_source/fixtures/core/lang.toml
python3 tests/native/slice_source/run_slice_source.py \
  --cli build-slice/woozyc \
  --helper build-slice/native/lang_native_helper \
  --clang /home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/clang \
  --lld /home/qin/Work/language-dev-20261103/toolchains/llvm-22.1.8/bin/ld.lld \
  --runtime build-slice/native/liblang_native_runtime.a \
  --root . --out /tmp/slice-source-results \
  --ld-library-path /home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9/usr/lib
```

CTest 注册 patch 是 `tests/native/CMakeLists.txt` 中
`cmake-registration.patch` 所示的最小段落；没有整体覆盖该共享文件。

## 6 状态

本步骤完成 Native source Slice 接入和上述验收，但**不把 LANG03 整体标为
done**：owning/index/field inout、Ptr 以及 Python Slice 仍待后续步骤。
