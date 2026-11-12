# FFI02 / Python interop operations contract

本文件固定本次 `interop_ops` 的声明字段和类型规则。实现只接受这里列出的
声明；未声明成员、错误类型、重复声明和未知字段继续是诊断，不做运行时
`hasattr`/`getattr` 探测，也不把结果降级成 `Any` 或 `.item()/.detach()`。

## 绑定 bundle 增量

保持 `schema_version = 1`、`contract = compiler-dev/1` 和原有
`modules/types/values/callables` 不变。新增三个**可选**顶层数组：

- `properties`：宿主属性读取/写入。
- `indexers`：宿主对象单下标读取/写入。
- `slices`：宿主对象切片读取/写入。

旧 bundle 不写这三个字段时行为与 T10 完全一致。字段存在但值为 JSON `null`
不是“缺省”，而是 `E_BINDING_INVALID`（`properties:null` / `indexers:null` /
`slices:null` 同理）。三个数组中的每个条目都有唯一 `id`（与
`types`/`callables` 共用重复检查），`module` 必须是已声明模块，
`owner_type` 必须是同模块内已声明的 `host_reference` 或 `opaque_protocol`
类型。所有条目都要求 `"error_type": "lang.py.Error"`；该类型必须在同一
bundle 中声明为 `runtime_error`。

### properties

```json
{
  "id": "interophost.Probe.value",
  "module": "interophost",
  "owner_type": "interophost.Probe",
  "name": "value",
  "type": "f64",
  "getter": ["value"],
  "setter": ["value"],
  "error_type": "lang.py.Error"
}
```

- `name` 是源码成员名；`getter`/`setter` 是相对 receiver 的非空宿主属性路径。
- `getter` 必填；`setter` 为数组或 `null`。`null` 表示只读，源码写入会在
  类型检查阶段报错。
- 读取执行 `getattr(receiver, *getter)`；写入先遍历 `setter[:-1]`，再对最后
  一段执行 `setattr`。路径中的宿主异常包装为 `py.Error`。
- 同一 owner 上属性名唯一，且不能与同名 method/callable 同时声明。

### indexers

```json
{
  "id": "interophost.Probe.index",
  "module": "interophost",
  "owner_type": "interophost.Probe",
  "index_type": "interophost.Key",
  "result_type": "i64",
  "readable": true,
  "writable": true,
  "error_type": "lang.py.Error"
}
```

- 每个 owner 最多一个 indexer；源码下标必须恰有一个表达式。
- `index_type` 是整数内建类型（`i8..u64/isize/usize`）或声明的宿主类型。
  宿主类型允许实现 `__index__` 的 key 对象；编译器不会自动转换它。
- `result_type` 是该下标表达式的静态结果类型，也是写入时的值类型。
- `readable`/`writable` 至少一个为 `true`；未授予的读写方向在类型检查阶段
  报错。读取执行 `receiver[index]`，写入执行 `receiver[index] = value`。

### slices

```json
{
  "id": "numpy.ndarray.slice",
  "module": "numpy",
  "owner_type": "numpy.ndarray",
  "start_type": "i64",
  "end_type": "i64",
  "result_type": "numpy.ndarray",
  "readable": true,
  "writable": true,
  "error_type": "lang.py.Error"
}
```

- 每个 owner 最多一个 slice；源码最多提供 start/end 两个端点。
- `start_type`/`end_type` 为整数内建类型或 `null`。`null` 表示该端点不允许
  在源码中提供；省略的端点按 Python 的 `None` 处理。
- `result_type` 同时是切片读取结果和切片赋值值类型。
- `readable`/`writable` 规则与 indexer 相同。切片读取使用 Python
  `receiver[start:end]`，写入使用同一 slice 对象赋值。

## 类型限制

允许的属性/下标/切片结果类型和属性值类型：

- 内建 `bool`、所有整数、`f32/f64`、`string`（不接受 `Unit`）；
- bundle 中 `kind` 为 `host_reference` 或 `opaque_protocol` 的类型。

不接受 `runtime_error`、`Vec`/struct/enum 等需要语言侧构造的聚合类型。这样
不会把宿主返回的任意 Python 对象解释成语言聚合，也不引入动态 dtype/shape
推断。索引/切片声明中的固定结果类型就是唯一的 dtype 承诺；例如 ndarray 的
`index_type = i64, result_type = f32` 只适用于按该声明使用的 f32 数组。

## 求值与错误规则

1. 源码左到右、各一次求值：属性读取先 receiver；下标先 receiver 再 key；
   切片先 receiver 再 start 再 end；写入再求 value。
2. 宿主调用只执行已声明的路径/下标操作。宿主抛出的 `Exception` 包装成
   `lang.py.Error`，保留原异常、`cause` 和格式化 traceback（通过运行时已有的
   `Error` 桥）；内部 `TypedError`/`Panic` 不二次包装。
3. 结果值按声明类型进入现有拥有/借用规则；不做 `.item()`、`.detach()`、
   `.cpu()`、`.numpy()` 或其他宿主转换。写入值按普通赋值规则转移/借用。
4. 未声明属性/下标/切片、只读写入、错误参数类型、缺少端点声明、复合赋值
   都在检查阶段诊断。Native 后端仍明确拒绝这些 Python 操作。

## 明确不支持

- 任意反射、动态成员名、宿主方法值逃逸、按运行时 dtype 选类型；
- 多下标/step 切片、宿主属性描述符的自动展开、`__getattr__` 探测；
- 闭包、Weak、任意 Any、Native FFI 实现。

## 验收测试

`tests/python/interop_ops/run_interop.py` 覆盖：

- host `property` 的 getter/setter、调用计数、嵌套路径和抛错 cause/traceback；
- 有副作用的 key 对象和单次求值顺序；
- NumPy ndarray / PyTorch Tensor 的合法索引与切片读取/赋值；
- 未声明、只读、错误参数、错误赋值、复合赋值等负例；
- 宿主引用在 Vec/struct 中的嵌套保存，以及抛错后的后续操作与 T10 控制组。
