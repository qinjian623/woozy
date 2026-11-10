# T10 Python 外部生成和运行库

本文件记录 Python 后端当前实现的有限范围；CPU 实测、依赖版本和检查范围见 [T10_CPU_VERIFICATION.md](T10_CPU_VERIFICATION.md)。公共静态表示仍以 [T10_STATIC.md](T10_STATIC.md) 和 `include/lang/hir/hir.hpp` 为准；声明读取见 [T10_DECLARATIONS.md](T10_DECLARATIONS.md)。绑定输入仍是 `review_status=draft_requires_conformance`。

## 1. 已实现的生成范围

只接受 Python/64 的 `VerifiedHirModule`。Python emit 先调用共享 `hir::check_backend_capabilities`，再执行自己的入口和值检查；不复制前端类型、HIR 或拥有规则。

- 现有 15 个 ordinary 外部 callable：模块函数、构造、方法、对象 callable；每个可执行 callable 必须声明 `error_type=lang.py.Error`，参数全部 ordinary。
- dtype/外部值读取：只使用声明中的模块和 `host_attribute` 路径。
- 外部对象的保存、普通参数传递、返回、丢弃，以及方法/对象调用；逻辑 copy 持有同一宿主对象，不复制 ndarray/Tensor 数据。
- 外部宿主引用的整体 move：已有绑定写 `move x`，或移动整个临时值；移动的是本语言持有的句柄并使源绑定失效。它不移动 ndarray/Tensor 数据，也不深复制宿主对象。
- 外部宿主引用保存在 `Vec`、struct 字段、enum payload 和泛型字段/元素中；容器 copy、切片、for-in、字段替换和返回沿用共享容器/聚合所有权规则，普通 copy 仍共享同一宿主对象。
- 可选参数省略、位置/命名实参及其源码顺序；省略参数不补 `None`。
- typed `py.Error` 的传播、跨源模块 catch、`let` copy、再抛出和未捕获报告。
- 生成的 `program.py` 由纯结构化 Python 组成，内嵌 `_LANG_BINDING_INFO`；`langrt.py` 来自 `runtime/python/langrt.py`。不新增第三个输出文件。

不生成：外部 inout、字段/索引部分 move、外部索引/切片/数据属性、外部函数值逃逸、未知成员、动态 shape/device 类型推断、其他 profile。普通借用参数不能直接 move；移动后读取、双 move 和同存储借用/move 冲突沿用既有所有权诊断。无 throws 的外部 callable 即使返回 Unit、参数只有标量，也报 `E_CAPABILITY_UNAVAILABLE`；其源程序和静态 HIR 仍可按静态 API 检查，但不能进入生成。native/32 位仍拒绝。

外部类型直接作为错误类型（`throws`/`throw`/`catch` 的类型本身）时，只支持 `runtime_error` 声明。源语言枚举的泛型实参不是错误根，可含已声明的普通宿主引用（如 `Failure[host.Probe]`、`Failure[Vec[host.Probe]]`），完整有序实参保留在稳定身份中；跨模块导入别名仍指向定义模块的同一身份。普通宿主类型身份按 `@<UTF-8字节长度>:<绑定id>` 编码，id 内的逗号、方括号、函数格式等文字不能冒充类型结构；`runtime_error` 桥仍使用固定 `lang.py.Error`。普通宿主对象或含普通宿主类型的函数值直接作为错误载荷仍在生成时明确报 `E_CAPABILITY_UNAVAILABLE`，不报内部错误。

## 2. API 生成

标准绑定通过 `ProjectOptions::std_binding_bundle` 提供；普通依赖仍使用清单中的 binding 文件。正式 `langc` 的 `check/build --project` 已提供可选 `--std-binding BUNDLE`，把操作者选择的工具链标准声明直接交给同一个 `load_project`。`check_project` 默认后端模式已会检查 Python/64 外部操作。

```cpp
SourceManager sources;
auto loaded = modules::load_project(
    {manifest, {}, std_binding_bundle}, sources);
auto resolved = modules::resolve_project(
    std::make_shared<const modules::Project>(std::move(*loaded.value)));
auto verified = sema::check_project(resolved); // VerifyMode::backend
auto output = backend::python::emit(*verified.value, entry);
// output.value->code 写 program.py，output.value->runtime 写 langrt.py。
for (const auto &symbol : output.value->symbols)
  if (resolved->symbols.at(symbol.symbol.value).name == "train")
    generated_train_name = symbol.name;
```

入口只要求零参数、返回 Unit 或 i32。测试辅助程序通过 `Output.symbols` 定位 `train`，不是按生成名猜函数；原 `04_numpy_torch.lang` 保持不变。

`emit` 只生成字符串，不启动 Python、不 import NumPy/PyTorch，也不访问文件系统或子进程。正式 CLI 使用同一路径：

```bash
langc check --project lang.toml --backend python \
  --std-binding contracts/python_bindings.example.json --format json
langc build --project lang.toml --backend python \
  --std-binding contracts/python_bindings.example.json --out-dir build/python
```

`BUNDLE` 是 argv 给出的现有绑定文件，不从项目依赖或默认机器路径推断，也不执行 Python 包探测。该参数不能用于 `--syntax-only`，项目声明同名 `std` 依赖仍拒绝。未提供时保持旧行为：缺少 `std` 源的导入报 `E_IMPORT_NOT_FOUND`。Python build 只写 `program.py` 和 `langrt.py`；测试用同一项目、同一绑定运行现有 API helper，确认 CLI 与 API 生成的两个文件逐字节相同，并用 API 的 `metadata.symbols` 定位 `train` 后执行 CLI 生成的程序。`build --backend native` 会转发该参数给 native helper，但 Python 外部源仍报 `E_CAPABILITY_UNAVAILABLE`，不会切换后端。

CLI 参数只负责选择绑定文件。该 example bundle 保留 `draft_requires_conformance` 原字节；指定 CPU 环境的实际检查按其内容摘要记录在 [T10_CPU_VERIFICATION.md](T10_CPU_VERIFICATION.md)，依赖版本见正式锁的 `python_cpu`。

## 3. 求值顺序和参数

每个 `ForeignRead` 先求接收者（若有），再按声明的 `host_module`/`host_attribute` 读取；每个读取有独立的 `py.Error` 出口。随后按源码顺序求全部实参，再用源码参数标签和已选参数下标的声明名字组装调用：

- 位置实参保持位置；命名实参按声明参数名生成关键字实参，不按源码标签猜宿主名字。
- `Call::omitted` 中的可选参数完全不出现在实参容器；不补 `None`，也不执行隐式 dtype/device/cpu/item/detach/numpy/clone/copy 转换。
- `model(x)` 直接调用已读取的 model 对象，不改写成 `.forward(x)`；`ParameterIterator` 原样作为普通对象传给构造，不转 list。
- 读取或后续实参失败时，只清理已完成的临时值、结束已开始的借用；对未求值的实参不登记清理，也不执行 Call。

## 4. 运行库和 py.Error

`runtime/python/langrt.py` 版本为 `compiler-dev/1-python-values/7`，与 `include/lang/backend/python.hpp` 和嵌入 runtime 一致。

- `host_module(logical, host)` 只 import 声明中的宿主模块。缓存 key 是 `(logical, host)`，避免两个逻辑模块声明不同宿主时返回错误对象。import 失败抛 `HostEnvironmentError`，不伪装成源码 `py.Error`；该异常发生在外部操作桥之前。
- `foreign_read` 和 `foreign_call` 只包住已声明的属性读取/实际调用。先原样传播 `TypedError`，再捕获普通 `Exception`；`Panic`、`KeyboardInterrupt`、`SystemExit`、`SwitchBreak` 等 `BaseException` 不被包装。生成函数没有整体 `except Exception`。
- `Error` 载荷固定键 `lang.py.Error`，保存原宿主异常、cause、格式化 traceback、源文件及字节范围、操作身份。`error_payload_copy` 共享该载荷，不深复制宿主异常，也不重复 take 同一 TypedError。该载荷可位于 `Vec[py.Error]`、struct 字段、enum payload 和嵌套泛型值中；语言复制/清理沿用同一 Error 对象，宿主异常、cause、traceback 保持同一身份。
- `ValueType('foreign')` 和 `ValueType('error')` 的 copy 保持宿主对象身份；drop 只释放语言 owner 槽，不承诺宿主对象立即析构或释放宿主缓冲区。
- Unit callable 成功返回必须是 `None`；其他返回值触发明确 `Panic`，不静默丢弃。未捕获的 `py.Error` 先输出稳定的 `LANG_TYPED_ERROR: lang.py.Error` 行，再输出可用原因/源位置并 drop。

已声明的宿主对象身份、别名修改、读取/后参/调用失败前缀、typed error catch/copy/跨模块/cause/traceback/span、内部 `TypedError` 和三种 `BaseException` 不重包装，以及正常/typed-error 后无活动 owner/borrow，均由测试用专用 host/binding 实际执行。专用宿主用例与真实库小例分别记录。

## 5. 生成文件中的声明信息

生成的 `program.py` 在版本检查后设置 `_LANG_BINDING_INFO`：

- runtime 版本；
- 每个 bundle 的路径、SHA-256、契约、`review_status`、`allowed_backends`；
- `package_requirements` 的包名和版本策略文本；
- 每个模块的 `logical_path`、`host_module`、`provider`。

该信息是声明快照，不是正式依赖锁、conformance 证明或 `verified` 状态；不修改 draft，不新增锁文件。

## 6. 测试和当前证据

- `tests/python/t10_binding.json`、`t10_host.py`：测试专用 ordinary 声明和宿主，不导入真实 NumPy/PyTorch，不冒充 conformance。
- `tests/python/run_t10_bindings.py`：真实 API 生成，执行顺序/省略/别名/Error/cause/span/cross-module/BaseException/失败清理/无 throws E_CAP/缺宿主环境错误，并检查 Frame 无活动 owner/borrow。`foreign-ownership-*` 用例另执行外部引用整体 move、函数/泛型/trait 参数模式、Vec/嵌套结构/enum/泛型字段身份、普通/return/defer/typed-error/后参失败/部分构造清理；观察器核对每次 foreign copy 返回同一 Probe、move 后源 owner 槽失效、Frame/borrow 清空，并拒绝改坏的 HIR。
- `tests/python/t10_original04.py`：使用专用 CPython 3.12.15、NumPy 2.5.4、PyTorch 2.15.0+cpu，调用真实生成的 `train`；检查 seed 7、Linear(1,1)、lr 0.1、200 次 backward/step/zero_grad、201 次真实 model forward hook、x/y 形状/dtype、y=2x+1、参数更新、loss 下降和返回零维 CPU float32 Tensor 保留 autograd。数值读取仅在测试端进行。
- `tests/python/t10_real_objects.py`：对生成的普通参数/let/返回函数传入真实 ndarray、Tensor、view 和带梯度的派生 Tensor，检查身份、data_ptr、双向别名修改及无隐式转换；真实 NumPy ValueError 经生成代码包装为 py.Error，保留消息、traceback、源位置，函数退出后 owner/borrow 清空。宿主测试中的索引和 no_grad 不扩展源语言。
- `tests/python/t10_reference04.py` 独立执行原手写参考，按 `rtol=1e-5/atol=1e-6` 比较，不修改参考。
- 本地 `tests/python/CMakeLists.txt` 只在配置了 `LANG_T10_PYTHON` 时注册 `python.t10_bindings`、聚焦的 `python.foreign_ownership` 和 `python.t10_cli`；普通 `python3.12` 未被替换成专用依赖环境。`python.t10_cli` 使用正式 `langc` 的 `--std-binding`，检查参数/路径/错误保留/native 转发，并实际执行 CLI 生成的原 04 训练函数。

原 04 的 API 与正式 CLI 生成、真实 CPU 执行、参考比较以及依赖锁的检查结果见 [T10_CPU_VERIFICATION.md](T10_CPU_VERIFICATION.md)。这些结果只覆盖所记录的环境、声明集和样例，不代表 GPU/CUDA、其他训练或任意 Python 库已经支持。
