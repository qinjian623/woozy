# 动态 trait 对象（LANG02）

状态：修正候选，LANG02 保持 in_progress，等待 Root 验收；测试位于 `tests/trait_objects/`，由 `trait_objects.cases` 执行。

## 源码

`dyn Trait` 与 `dyn Trait[Args]` 是显式类型。`dyn` 不是新关键字，只在类型位置识别。一个具体值可以出现在期望 dyn 类型的位置（绑定、参数、返回值、struct/enum 字段、Vec 元素和赋值），转换要求该具体类型对完整 trait 应用恰有一个 impl。

```wz
trait Convert[T] { func value(self Self, fallback T) T; }

let number dyn Convert[i32] = tag;
let values Vec[dyn Convert[i32]] = [tag, other];
```

`Convert[i32]` 和 `Convert[bool]` 的 `TypeId` 不同：`TypeKind::TraitObject` 保存真实 trait `declaration` 和有序 `type_args`。转换要求完整 trait 应用恰有一个 impl；空 trait 也必须存在 impl，否则在语义检查阶段报告源错误，不进入 HIR。dyn 值按普通值规则复制/移动；复制共享同一个装箱值，最后一次销毁释放拥有字段。

## 对象安全

trait 在用作 dyn 时检查全部方法：

- 每个方法必须有普通（ordinary）`self Self` 接收者；
- 方法自身不能有泛型参数（trait 自身的泛型参数可以用）；
- 不能 throws；
- 不能返回 `Self`；
- 非接收者参数不能出现 `Self`；
- 参数模式必须是 ordinary。

不满足时报告 `E_OBJECT_UNSAFE`；测试覆盖 generic method、Self 返回、throws、move Self、move 参数、无 Self 接收者和非接收者 Self 参数。空 trait 没有 impl 时仍在语义阶段报告 `E_TYPE_MISMATCH`。重叠/歧义 impl 由原有 impl 检查报告 `E_TYPE_MISMATCH`。

## Native 表示

- 源值布局：`{data_ptr: ptr, vtable_ptr: ptr}`。
- 装箱值是一个单元素内部 Vec；`data_ptr` 指向其中的具体值，因此 GC 仍能追踪拥有字段。
- vtable 槽固定为：`drop(data)`、`clone(data)`，然后按 trait 方法声明顺序排列 `ret(data, args...)` 函数指针。
- `drop` 释放最后一次装箱引用；`clone` retain 同一个装箱值并返回 data。
- 方法 thunk 从 `data` 读取具体值，按普通 self 规则复制接收者，调用已选择的具体 impl 方法，随后销毁该临时副本。返回复合类型按正常函数 ABI 返回。
- NativeIR 新增 `trait_object` 与 `trait_call` 操作；两者都经过 HIR/NativeIR 验证。生成程序仍是无解释器、无 libc 依赖的 freestanding ELF。

## Python 表示

`langrt.DynObject` 保存具体 `ValueType`、装箱值和按声明顺序排列的方法函数表。复制使用具体类型的 `ValueType.copy`；调用时复制接收者并转发到具体函数；销毁使用 `ValueType.drop`。`ValueType('dyn')` 参与 Vec/struct 的 copy/drop/trace。语言 GC 在计数阶段已经减过 dyn 内部强边，因此退役阶段必须穿过 DynObject 的具体值和嵌套结构，跳过已计数的 Vec/function/Slice 边，禁止对旧别名二次 drop 或重新激活；DynObject 本身在退役/销毁后标记为 dead。

## 验证

`tests/trait_objects/cases.json` 包含正例（标量/复合返回、异构 `Vec[dyn]`、泛型 trait 身份、泛型 impl、泛型函数边界、复制/移动/销毁、字段/枚举/函数边界、闭包捕获、空 trait 有 impl、纯 Woozy dyn 环、dyn 内拥有函数/Slice 的退役环）与拒绝例（对象不安全方法、Self 返回、歧义 impl、空 trait 无 impl）。`run_trait_cases.py` 对两个前端后端执行 check，正例执行 Python 与 Native O0/O2，检查 ELF64/x86_64、无 `PT_INTERP`、无强未定义符号；`observe_dyn_cycle.py` 在禁用 CPython GC 的进程中运行 dyn 环，检查语言 GC 实际收集、无 live Vec、DynObject 全部退役；`dyn-retire` 另覆盖 dyn 具体值内的闭包环境与 Slice 退役分支。
