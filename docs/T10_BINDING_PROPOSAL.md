# T10 Python 绑定最小实现方案

操作：`language-t10-binding-proposal-20261104`。依据版本：`compiler-dev/1`，源码 HEAD `51363f5fc40cb720f3ece1b9a5c1d6d788ad5ffb`。本文只提出实现方案；T10 仍为 `planned`，没有发布公共接口，也没有运行绑定、生成程序或训练。

目标是让原版 `baseline/samples/04_numpy_torch.lang` 经公共前端和 Python/64 后端生成程序，并在锁定依赖的 CPU 环境执行。T09 两后端的有限 Vec/拥有聚合已经合入；不退回早期标量限制。普通参数重赋值、计算产生的拥有 switch 临时值等未定行为继续使用 `E_CAPABILITY_UNAVAILABLE`。

## 1. 当前源码缺什么

下表来自本次静态阅读，未用旧文档的“待实现”文字推断源码状态，也未重跑历史测试。

| 文件、实际入口 | 已有内容 | T10 缺项 |
| --- | --- | --- |
| `src/modules/project.cpp`：`bundle/foreign`；`include/lang/modules/project.hpp` | 读取 JSON、检查版本及几个数组；保存原文；建立 type/value/function 导出；同名 constructor 附着 type；method 不进模块普通名字表 | 尚未完整检查 schema、引用和跨字段约束；同名普通函数候选会报重复；未建立可供类型检查使用的签名、效果、拥有信息和绑定项位置 |
| `src/modules/resolve.cpp`：`collect`；`include/lang/modules/resolve.hpp` | 外部导出取得 SymbolId，实例成员留待类型检查 | Symbol 未直接关联 ForeignBindingId/候选组；定义位置为空；构造和成员签名尚不可查 |
| `src/sema/check.cpp`：`declarations/parse_type/expression/scalar_call` | 普通调用、实参次序、类型/效果、T09 拥有处理已存在 | `declarations` 在导入处拒绝 foreign；类型、外部函数和 dtype 值另有拒绝；实例调用只接受 Function 类型；调用要求所有参数提供，未处理位置限定/关键字限定/省略 |
| `include/lang/hir/hir.hpp`；`src/hir/hir.cpp` | 已有 `ForeignBindingId`、`TypeKind::ForeignOpaque`、`Type.binding`；Call 只有 direct/function_value；`verify` 深复制后发布只读包装 | `valid_type` 和后端检查仍拒绝 ForeignOpaque；没有已解析外部描述、外部读取节点及外部调用身份；类型池能驻留此种类不代表验证通过 |
| `src/hir/ownership.cpp` | `classify_value`、`produce_ownership/verify_ownership` 和各类拥有记录已实现 | ForeignOpaque 分类仍是 unsupported；外部接收者、取属性失败和 py.Error 载荷尚无处理 |
| `src/backend/python/emitter.cpp`；`runtime/python/langrt.py` | `emit(VerifiedHirModule, ...)` 先调用公共支持检查；已有结构化生成、`ValueType/Owner/Frame/TypedError`，runtime 为 `compiler-dev/1-python-values/4` | foreign 实际在前端/公共支持检查被拒绝；emitter 没有外部调用分支，不能只删一个拒绝条件。还缺导入/属性读取、关键字/省略参数、外部对象类型描述、Error 载荷和异常桥接；不能把未知值按默认平凡类型放行 |
| `src/driver/main.cpp`：`compile/write_python` | 成功生成完整 Output 后才写 staging，再替换两个输出文件；报告的替换错误尝试恢复旧文件 | `load_project({path, {}, {}})` 没有传 `std_binding_bundle`；`std/py` 因此还需 root 接工具链标准库配置。没有真实 Python 包版本预检；现有双文件替换不保证进程突然中止时成组原子恢复 |

`docs/T09_STATIC.md` 描述的真实 API 在当前头文件中存在：`Ownership` 包含 `ValueUse/Temporary/LocalLifetime/FullExpression/CallLifetime`，出口使用 `ExitPlan.actions`。后续应扩展这些生产与验证步骤。`docs/T09_EXECUTION.md` 和 HEAD 的 T09 合入提交可作为历史交接依据，其测试数量不算本次验证。

## 2. 原 04 样例必需的范围

使用 `contracts/python_bindings.example.json` 现有的 6 个逻辑模块、9 个类型、2 个 dtype 值、15 个 callable；不增加任意 Python 反射。逐项为：

| 输入操作 | 必需行为 |
| --- | --- |
| `std/py`、numpy、torch 及三个 torch 子模块 | 从 `modules` 取得宿主模块名；`std/py` 指向 langrt，不生成 `import std.py`；std 来源由工具链指定，禁止项目声明同名 std 依赖 |
| `np.float32`、`torch.float32` | 分别是 DTypeSpec 与 torch.dtype 的外部值，类型不能混用 |
| `linspace/expand_dims/multiply/add/asarray` | 只接受示例声明的重载；f64/i64 字面量和 ndarray 正常传入；shape/dtype 合法性留给真实库 |
| `manual_seed/tensor` | seed 为 7；Generator 即使被忽略也先正常取得再结束本表达式；源码显式 tensor(ndarray) 才复制数据，dtype float32、device cpu |
| `nn.Linear`、`parameters`、`optim.SGD` | 构造附着已有类型符号；参数迭代对象保持原样，不生成 list；SGD 持有宿主所需引用 |
| `model(x)`、`mse_loss/backward/zero_grad/step` | 保持实例调用及真实 autograd；所有普通 Python 失败按 py.Error 传播；无 closure 的 step、zero_grad、backward 返回 Unit |
| 200 次循环、train 返回和 main | 沿用已实现的整数/循环/throws/清理；train 返回零维真实 Tensor，main 可以忽略它 |

最小范围需要普通外部参数、普通返回、局部持有/赋值和 py.Error catch/传播，以独立小例证明拥有与失败处理。原 04 不使用外部 move/inout、索引、实例数据属性、外部函数值逃逸或外部对象放入 Vec/聚合；这些组合本轮保持 E_CAP，不影响现有非外部 T09 支持。`device` 按绑定声明的 `string` 检查，不新增字面量或设备值限制；设备合法性与 shape、动态 dtype 等由宿主运行时判断，宿主异常按既有 py.Error 处理（`docs/SEMANTICS.md:169`）。CPU 仅是本轮 A08 真实执行、依赖锁定和支持声明的验证范围，不扩大 GPU 验证声明；通用字符串表达式的既有未支持诊断保留，不新增 device 特判。

不增加 GPU、AMP、DDP、torch.compile、自定义 Module、回调语法、通用迭代或 Tensor 运算符。不得隐式添加 `item/detach/cpu/numpy/clone/copy`，也不将 ndarray/Tensor 转成 Vec/list。语法依据包括精确文件 `baseline/LEXICAL_SYNTAX.md` 第 13 节和 `baseline/samples/README.md` 的“绑定解析约定”；后者不是独立绑定 schema。

## 3. schema 字段怎样进入各阶段

格式仍以 `contracts/python-bindings.schema.json` 为准；本方案不改它。读取器既查字段类型/未知字段，也查 schema 尚不能表达的关联约束；均在 C++ 内完成，不启动 Python 或执行绑定文字。

| 字段 | 解析、类型检查 | 不可变 HIR 和 Python 生成 |
| --- | --- | --- |
| `schema_version/contract/allowed_backends` | 检查版本、仅 Python；native 使用这些模块时明确 E_CAP | 固定在已验证配置及绑定描述中，不能换后端复用包装 |
| `review_status/package_requirements/conformance_required` | draft 可供静态检查，不视为真实库已核实；版本策略文字不充当版本锁；保留待验证条目 | 输出记录绑定摘要与依赖要求；执行测试只接受匹配 conformance 记录的精确环境 |
| `modules.logical_path/host_module/provider` | 检查合法标识符、唯一来源和 provider；不按斜线猜宿主文件 | 用已解析模块描述生成固定 import，别名由内部 ID 生成 |
| `types.id/module/name/kind/host_attribute/copy` | 按稳定 id 驻留 ForeignOpaque；host_reference、opaque_protocol、runtime_error 分开；核对归属和引用 | 绑定描述决定类型操作；opaque_protocol 不查找虚构宿主同名类；Error 也不使用诊断恢复类型 `TypeKind::Error` |
| `values.module/name/type/host_attribute` | 建立外部值符号和类型；不读取真实 dtype | 生成有绑定身份的属性读取；按 INTEROP 将会失败的外部读取计入 py.Error，不能折成编译期常量 |
| `callables.id/module/name/call_kind/owner_type/host_attribute` | 按模块名或 owner_type 建有限候选组；constructor 与同名 type 合并；method/callable 必须有合适 owner，callable 保持实例调用 | 固定选中的 ForeignBindingId、调用种类和宿主属性序列；后端不重新选重载 |
| `parameters.name/type/kind/optional/mode` | 检查标签、重复/遗漏、位置限定与关键字限定；按声明类型匹配，歧义拒绝；本轮外部参数仅 ordinary | CallArg 保留 source_index、parameter_index、label、mode；另保存未提供参数集合，不生成占位值或 None |
| `parameters.escapes/receiver_escapes` | `no` 不推导对象不可变；`may_retain` 不改变 ordinary 为 move；检查与 effects 的组合 | 保留逃逸声明，调用期间保活，宿主保存自己的引用；不生成深层 noalias，也不提前释放底层数据 |
| `return_type/error_type/ownership.return` | 正常值与 throws 分开；Unit/unit、外部引用/python_reference、值/value 必须匹配；错误类型精确到 `lang.py.Error` | 输出值只在成功时产生；错误只在失败时产生并进入既有 ExitPlan；禁止自动 Result 或标量化 |
| `ownership.storage/effects` | copy、view_or_alias、host_defined、no_tensor_storage 与句柄拥有分开；保存 RNG、修改、保留、autograd、消耗迭代器等声明 | 不自行执行额外复制；有副作用的调用不得删除/重排；`consumes_iterator` 不表示消费源绑定的拥有权 |

语义检查还需拒绝重复 id、未知类型/owner、非法构造归属、错误的返回拥有组合和不可调用目标。相同外部函数名的不同 id 是候选，不当成多个普通符号；确定选择失败用 `E_CALL_ARGUMENT`，类型不匹配用 `E_TYPE_MISMATCH`，坏绑定用 `E_BINDING_INVALID`，遗漏效果用 `E_EFFECT_UNHANDLED`。保留所有旧 code 和 enum 编号。

生成时先按源码顺序保存被调用者和全部实参，再按已选参数的 kind/name 组装位置及关键字；参数名只能来自已验证描述。省略参数不占值槽，不把后面的值误填进空位；无法用宿主调用规则表达的组合在检查时拒绝，不能填 None。

现有模块测试有刻意精简的旧 JSON：`tests/modules/project_test.cpp`。完整 schema 检查应由 root 指定在正式绑定读取阶段执行，或由 root 更新对应测试格式；不能擅自把旧模块用例解释成完整 T10 绑定，也不能让宽松模块读取替代正式检查。

## 4. 请 root 决定的公共表示增量

以下名字和数据是提议，不是现有 API。定义仍由 root 放进唯一公共头，前端和后端共用；不复制 HIR，不添加另一种 Verified 包装。

1. 将 `Project.bindings` 的原文解析成带绑定项 SourceSpan 的声明表，再在同一个 HirModule 内生成带 TypeId 的外部描述表。保留声明 id、bundle 内容摘要、模块/owner、宿主属性序列、参数限制、返回/错误/拥有/效果。SymbolId 关联导出描述或候选组；类型沿用 ForeignOpaque + binding。会话内 ForeignBindingId 仅用于索引；输出和缓存用契约、模块、稳定 id、内容摘要，不能写裸编号作持久身份。
2. 在现有 `CalleeKind` 尾部增加所需 foreign 种类，Call 增加选中绑定和省略参数信息，沿用 signature/callee/args。constructor 不新增普通函数符号；Linear 本身也不变成 Function 类型。`model(x)` 的 callee 是 Linear 对象，signature 来自已选 callable 描述。
3. 在 NodeData/NodeKind 尾部增加一个外部读取节点（暂称 `ForeignRead`）：固定绑定，可选接收者；dtype 读取产出声明值，函数/构造/方法目标读取产出仅供紧邻外部 Call 使用的内部调用目标。后者可用相应 Function TypeId 描述签名，但不能成为本轮可保存/返回的源函数值。方法读取必须先求 receiver，再取方法，随后才求实参；callable 直接持有实例，不取 forward。每次外部读取与实际调用各有自己的 py.Error 出口。
4. 在 `ValueKind` 尾部追加外部引用及错误载荷所需分类，`needs_drop=true`、`pod_inout=false`；沿用现有 ValueUse、临时、借用、局部和输出记录。内部方法目标需保护其接收者到调用成功/失败，且不得用非捕获函数值的平凡分类跳过保活。具体采用接收者借用还是内部拥有临时，由 root 在同一记录格式内定下。

拆开读取与调用，是为了正确处理 `obj.method(arg())`：若取方法已失败，arg 根本不执行；其错误出口不能假设 arg 的 move 或临时已发生。`ForeignRead` 必须有真实源位置、独立失败前缀及清理；被调用者读取也不能藏进求完全部参数后的调用 helper。无需增加通用求值框架。

供 root 核对的具体数据：

```text
np.linspace(-1.0, 1.0, num: 128, dtype: np.float32)
  binding = "numpy.linspace.scalar"
  signature = (f64, f64, i64, py.numpy.DTypeSpec)
              -> py.numpy.ndarray throws lang.py.Error
  args = [(source=0,param=0), (1,1), (2,2,label="num"), (3,3,label="dtype")]
  omitted = []; return = python_reference; storage = copy

model(x)
  binding = "torch.nn.Linear.call"; kind = callable
  callee = Read(model : py.torch.nn.Linear)
  signature = (py.torch.Tensor) -> py.torch.Tensor throws lang.py.Error
  args = [(source=0,param=0,ordinary)]; receiver_escapes = may_retain
  host_attribute = []; effects 包含 autograd/retains_arguments/retains_receiver

np.asarray(a)  // 独立的省略参数小例
  binding = "numpy.asarray.array_dtype"; omitted = [1]
  生成 asarray(a)，不生成 asarray(a, None)
```

验证器除常规类型/树检查外，要从不可变声明与源码身份重算：绑定候选、owner、构造归属、实参次序和省略、读取目标与 Call 的配对、throws、拥有及失败前缀。篡改 binding/owner/参数索引、漏掉属性读取效果、重算一份自洽但错误的清理记录，都应报 `E_IR_INVALID`。`freeze` 必须深复制新增表，保留 builder/声明别名后修改不能改变已发布值；最终仍只由 `hir::verify` 构造 VerifiedHirModule。支持检查和 emitter 仍只看具体函数及可达节点所需内容，不因未使用模板而拒绝；绑定文件自身格式错误仍应拒绝。

## 5. 对象、拥有值和错误

Python 对象和 Vec 使用不同类型操作。Vec 有语言定义的容器共享、非负下标、切片复制元素与遍历限制；ndarray/Tensor 的对象、存储、下标和析构由宿主约定。外部引用的逻辑 copy 是持有同一真实对象的新 Python 引用，不能调用 Vec.retain/release，也不能复制 Tensor 数据。drop 只撤销语言拥有槽里的宿主引用，不承诺宿主立即析构；确定资源释放仍需显式 defer/受控协议。

| 位置 | 普通成功/失败时的处理 |
| --- | --- |
| 普通实参和方法/callable 接收者 | 调用者保有拥有权并保活到调用结束；临时实参活到所属 full-expression。宿主 may_retain 时由宿主取得其需保留的引用，不释放宿主缓冲区 |
| 保存或返回普通参数 | 产生合法 copy/retain，所得仍是同一个 ndarray/Tensor；返回输出先持有结果，再执行 defer 和局部销毁。不得把借用参数直接 move |
| 外部调用正常返回 | 取得新拥有引用；并不承诺新对象或新存储。被忽略的返回按 full-expression 清理；Unit 只接受已确认的 None，不偷偷丢弃其他返回值 |
| 后续实参/取属性/实际调用失败 | 只清理实际完成的临时前缀，结束已开始的借用，再执行离开词法块的 defer 与局部清理；未成功的返回槽不销毁 |
| py.Error | 新增 runtime 的 Error 载荷，保存原异常类型、消息、cause/traceback、源操作位置；error_payload_copy 复制载荷持有关系，原异常可共享，不复制或重复 take 同一个 TypedError 载体 |

异常桥接只包住已知外部读取/调用，先原样传播内部 TypedError，再捕获普通 Exception，构造稳定类型键 `lang.py.Error` 的 TypedError，并保留原 cause。Panic、KeyboardInterrupt、SystemExit 等 BaseException 不转换，不能用整个生成函数的 except Exception 掩盖生成器错误。Error 作为普通错误载荷也使用 Owner/Frame：传播前保护，catch take 一次，未捕获时 runner 输出可用原因和源位置再 drop；不同生成模块按逻辑类型键匹配。导入缺包/锁不匹配由执行前环境检查报告，不伪装成源码类型错误；源码操作中的宿主失败才按绑定桥接。

Python 生成/runtime 在现有文件里增加这些处理，并同步 runtime 版本及嵌入文件，不另写一套清理。普通参数重赋值仍 E_CAP；本轮不开放外部 move/inout，已有纯语言 move 规则保持不变。

## 6. 属性、索引和还未规定的事项

原 04 只需模块导出值和声明的方法取值。`np.float32` 来自 values，`model.parameters` 来自 method，constructor 来自同名类型的 callable；不能用通用 getattr 推断未知成员。根据 INTEROP/BACKENDS，外部属性失败必须计入 py.Error，现有 values 没有 error_type 字段，本轮可由“外部读取可能失败”这条规范固定推导并保存在节点中，无须让用户重新决定。

schema 没有 property getter/setter、getitem/setitem、切片或迭代 next 的声明种类，也没有它们的返回/错误/拥有规则。原 04 不需要这些操作，遇到外部索引或数据属性先 E_CAP，不走 Vec IndexRead/IndexPlace。不能把普通名为 `__getitem__` 的 method 自动当成所有索引规则，也不能假设负下标、切片复制或别名行为。

| 问题 | 谁回答、是否影响原 04 |
| --- | --- |
| std/py 的确定来源、公共描述表所在头、内部方法目标如何保活、运行依赖记录放在哪个输出文件 | root 的实现选择；需在后续分工前确定，不需用户决定语言行为 |
| draft 与 verified 的用途、可省略参数、constructor、opaque_protocol、异常范围 | 按现有规范直接实现；draft 静态通过不代表真实集成通过，省略交给库默认行为 |
| 任意外部索引/可写属性的声明方式、结果拥有与失败效果 | 要扩出本轮范围时，需用户确认新增支持范围和未规定的语言行为，root 再统一 schema；不影响原 04 |
| 普通参数重赋值、计算产生的拥有 switch 临时值 | 仍待用户回答的既有问题；本轮保持 E_CAP，不凭本方案放开 |

没有必须补答才能实现原 04 的新语言问题；若后来出现实际规范矛盾，提交最小源码及两种行为，不能用 Any、隐式 detach/item 或改原样例解决。

## 7. 文件负责人和先后顺序

以下是一人负责一组文件的后续分工提议，本次没有派 agent。owner 为角色占位，root 领取时指定一个实际实例；总 CMake、公共 schema/锁/driver 始终只有 root 写。

| 唯一 owner | 文件 | 依赖及交付 |
| --- | --- | --- |
| root：共享前端 | `include/lang/modules/{project,resolve}.hpp`、`include/lang/hir/hir.hpp`、`src/modules/{project,resolve}.cpp`、`src/sema/check.cpp`、`src/hir/{hir,ownership}.cpp`、必要的共享验证代码、`src/driver/main.cpp`、contracts 和总构建 | 先定上述增量和 std 配置，再交付可冻结、可验证的 foreign HIR；native 明确拒绝，Python/64 仅开放本轮范围；统一旧模块测试格式变更及锁记录 |
| Python 负责人：生成/runtime | `src/backend/python/emitter.cpp`、`src/backend/python/runtime.hpp.in`、`include/lang/backend/python.hpp`、`runtime/python/langrt.py`、`bindings/python/`（目标目录，目前不存在） | 等 root 确认公共表示；实现已有 schema 的读取调用、对象/错误操作及版本同步。该 backend 头的版本改动由此 owner 提交、root 协调；不自行改 HIR/schema/driver |
| 测试负责人 | `tests/python/` 下新增 T10 文件、`tests/bindings/`（新目录）的 schema/conformance/前端负例及冻结破坏例、独立执行记录 | 根据已定接口写正反例；锁定环境的版本/来源/摘要结果交 root 写 lock，新增 CMake 接线也交 root；不改原 04 或手写参考 |

三个 owner 可先并行做不依赖新接口的声明检查设计、独立预期值和 CPU conformance 测试文件。root 给出实际公共头及 fixture 后，Python 与测试可并行接入；公共节点还未定时不能发布一套临时 HIR 假冒集成。测试负责人独占新增测试文件，root 对旧共享测试的修改另列明确文件，避免同文件并写。T10 不要求重做 T09 或开放其余 ARC 组合。

## 8. 验证顺序与完成条件

以下全部是后续计划，本次均 `not_run`；不安装依赖或执行训练。

1. **无真实库的静态检查。** 用纯声明 bundle 和原 04 建项目；在无 numpy/torch 的隔离环境令 PATH 中 Python 启动入口记录并失败，检查 check/build 不启动 Python、不导入库、不初始化 GPU。正例含原 04、普通参数保存/返回同一对象、省略 dtype、命名实参乱序；device 按声明的 string 检查，不因字符串内容新增 E_CAP，非 string 参数按类型拒绝；若宿主因设备参数失败，在独立错误测试中核对 py.Error。反例含错 dtype、未知标签/成员、重复/缺参数、位置限定/关键字限定、歧义、漏 throws、外部 inout/索引及 native 使用。断言 code、span、状态，不能只做 syntax-only。
2. **公共 HIR 的独立正反对照。** 真正由前端生成正例再 verify；保留 builder 和绑定声明别名后修改，已发布包装应不变。破坏绑定/owner/省略/参数次序/读取错误出口和拥有记录，即使重算提交记录也应拒绝；合法不同接收者/不同参数作对照，不能只验证一批坏数据都被拒绝。两端现有 T09/数值/错误测试应继续通过。
3. **真实依赖 conformance。** 在明确的 CPython 3.12 精确版本和 CPU 环境锁定 NumPy/PyTorch、wheel 来源/sha256、平台与运行时版本。当前 toolchain.lock 只有 Python 3.12.15 等 T00 记录，没有 NumPy/PyTorch 锁；policy 也只要求 T10 实测，不能猜版本。逐项确认 15 个 callable 的参数/省略/返回与错误、2 个 dtype 值、构造和身份；DTypeSpec/ParameterIterator 是 opaque_protocol，不要求宿主存在同名类；不能误用 isinstance(np.float32, np.dtype) 检查 DTypeSpec。确认无 closure 的 step 以及其他 Unit 操作真实返回 None。失败留在 draft，不计 passed。
4. **从原源码生成并执行。** 临时项目原样复制 04 并核对摘要，使用 root 配置的 std 及明确绑定；运行真实 `langc check/build --backend python`，再用锁定解释器执行输出的 `program.py`。CLI 当前不保存 `Output.symbols`；测试辅助程序可经真实前端和 emitter API 读取它来定位生成 train，核对其生成代码与 CLI 输出一致，再调用同一个 CLI 生成模块；不能假设已存在导出名或新 CLI 参数，也不修改原 train。分别记录生成摘要、命令、退出码、stdout/stderr 和环境。手写 `numpy_torch_reference.py` 单独执行作为对照，不能复制为生成结果。
5. **A08 的独立观察。** 数据 `(128,1)/float32`，满足 y=2x+1；seed=7、Linear(1,1)、CPU、lr=0.1，200 次 backward、step 和 zero_grad，201 次模型调用，hook 确实执行，参数确实更新。返回真实零维 CPU float32 Tensor，requires_grad/grad_fn 符合 autograd；loss 有限、非负、较初始下降且 `<1e-3`；与参考最终 loss 按 `rtol=1e-5, atol=1e-6` 比较。数值读取只在测试端显式进行。用只读观察夹具/宿主 hook 取得数据和次数，不改生成函数逻辑；移除 hook 会失败的独立断言证明不会把 forward 误认成实例调用。
6. **独立对象与错误小例。** 由 `.lang` 生成普通参数回传/本地保存，宿主检查 is 和原地修改的可见效果；验证 ndarray view 共享存储、显式 tensor 导入复制、返回 loss 保留图。用测试专用声明/宿主夹具测试 receiver→属性→实参顺序、后续实参失败、构造失败、cause/traceback/span、catch 前清理和跨生成模块错误匹配；另测内部 TypedError、Panic、KeyboardInterrupt、SystemExit 不被二次包装。夹具不冒充真实 NumPy/PyTorch conformance 或训练。
7. **输出失败保留旧文件。** schema/类型/能力/verify/emit 失败或取消时不产生 Output、不触碰已有 program.py/langrt.py。root 在写入阶段用暂存和回滚保护旧内容，测试先放已知旧文件，注入写入及第二次替换失败，核对旧摘要；新目录失败无成功文件。现有代码尚无进程中止后的成组恢复保证，若要求覆盖该故障，root 需另补恢复并验证；不把它写成已实现。运行失败也不删除已有编译输出。

每条记录按 ACCEPTANCE 使用 passed/failed/skipped/not_run/unsupported；没有真实依赖时集成为 not_run，T10 不完成。只有原 04 的真实生成程序、锁定依赖 CPU 执行、独立对象/错误正反对照及现有相关测试均通过后，root 才能标记所声明的 T10 子集完成。

本次交付只有本文和外层 `/tmp/language-t10-binding-proposal-20261104/{report.md,summary.json}`；构建、语法检查、真实库导入、conformance、训练、提交和 push 均未执行。
