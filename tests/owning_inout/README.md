# LANG03 拥有值 inout（owning inout）

契约：`compiler-dev/1`。目标平台 Linux x86_64。本目录只覆盖纯 Woozy 拥有值的
`inout`：整体绑定、字段和容器元素的替换，以及对应的借用/清理规则。POD 字段与
索引位置的既有覆盖仍在 `tests/inout_places/`。

## 范围

已支持（每个类至少一个真实 Source，Native O0/O2 与 Python/64 都执行）：

- Vec：整体绑定替换、struct 拥有字段、`Vec[Vec[i32]]` 元素、`Vec[Struct]` 元素字段。
- string：整体与字段替换（含嵌入 NUL 字节与存活副本）。
- 函数值：替换释放旧环境一次，保存的副本仍可调用。
- Weak：整体与字段替换切换被观察对象。
- 拥有 struct/enum：整体替换、嵌套 struct 字段、enum owning 载荷。
- ARC 共享句柄：`std/box.Box[T]` 替换保留新身份并按 ARC 释放旧身份。
- dyn trait object：替换释放旧箱内具体值一次。
- Slice：整体替换释放旧描述符。
- 显式 `(inout it).method()`：含 Vec 字段的用户 Iterator 修改自身。
- typed throw：正常/错误出口都发布完整新值、执行 defer、每个被替换者恰好释放一次。

仍拒绝并给出明确诊断：

- `Task[A,E]` 的 `inout`（完成生命周期归执行器，另有 worker 处理）。
- 外部 C resource / foreign 值的 `inout`（无可核对的 retain/release 配对）。
- `Unit` 位置。
- 从 `inout` 参数 `move` 出、闭包捕获 `inout` 参数、同存储重叠、字段与整体重叠、
  元素地址借用期间可能持有同一共享容器的其他实参。

## 语义要点

- 拥有值都是引用计数句柄，允许替换但不能 `move` 出借用槽。
- 替换顺序是先发布完整新值，再在修改区间外释放旧值；C/Python 析构重入看不到旧成员。
- 借用的调用者存储本身从不 `destroy`；另有普通副本时旧对象按 ARC 继续存活。
- 元素地址借用期间的未知共享别名保守拒绝；没有为未知别名生成 `noalias`。

## 运行

```bash
python3 tests/owning_inout/run_owning_inout.py \
  --cli build/release/woozyc --root . --python $(command -v python3) \
  --readelf <llvm-bin>/llvm-readelf --out /tmp/owning_inout
```

CTest 名称：`native.owning_inout`。运行器对每个正例执行 Native O0/O2 与 Python，
检查 Native 程序为无 `INTERP`/`NEEDED` 的 ELF64 x86_64；拒绝项同时检查两个后端的
诊断以及失败构建不留下产物。

运行器还会执行 `python_places_test.py`，直接对真实运行库 `_rt.replace_place` 覆盖
六种真实 place 形状（Owner、Cell、Field-over-Owner、Field-over-Cell、Index、
Field-over-Index）以及 Owner 的 `_DEAD` 行为：每种都必须先发布完整新值，再恰好
释放一次旧值，新所有者保持 `refs == 1`；Index/Field-over-Index 不得重复 drop，
`Cell` 不得报错或漏 drop。
