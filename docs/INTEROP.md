# Python 与 C++ 互操作开发契约

状态：待实现。绑定只描述编译器允许使用的接口，不能替代真实库，也不表示动态库所有行为都能静态证明。语法基线见 [词法语法规范](../baseline/LEXICAL_SYNTAX.md)，运行语义见 [SEMANTICS.md](SEMANTICS.md)，代码生成见 [BACKENDS.md](BACKENDS.md)。

## 1 共同原则

1. 编译期间仅读取经过版本管理的声明数据，不运行绑定脚本、不导入外部 Python 库、不执行外部构建钩子以发现接口。
2. 逻辑模块解析、名称与签名解析、目标能力检查、运行环境装载是不同阶段。错误应指明失败阶段。
3. 普通参数不消费源绑定，不意味着外部库只读、不会保存引用或不会修改对象。绑定中的 ownership 和 escape 约定必须与实际 API 对应。
4. 只支持已声明的重载、可省略参数和调用能力。找不到绑定时明确报错，不退回动态 Any。
5. 绑定版本、目标后端、包版本约束和生成器版本进入依赖锁及编译缓存键。
6. 不为使绑定“更安全”而隐式复制 Tensor、迁移设备、截断梯度或强制求标量。

## 2 绑定数据模型

开发包中的机器可读 schema 是格式权威。本节描述字段语义，不能据此创建第二套不兼容格式。

| 概念 | 必需内容 |
| --- | --- |
| 模块身份 | 逻辑 import 路径、提供者/依赖身份、版本、允许后端 |
| 宿主映射 | Python 模块名，或原生库与符号；禁止可执行表达式 |
| 导出类型 | 稳定类型 ID、表示类别、可复制/移动/共享约定、可调用能力 |
| 导出值 | name、类型、宿主属性或符号，不在编译时读取实际值 |
| 函数/方法 | name、parameters、return_type、error_type、call_kind、ownership |
| 参数 | 顺序、名字、位置/命名限制、是否可省略、类型、参数模式 |
| 重载 | 有限候选集和确定的选择规则；歧义时报错 |
| 生命周期 | 返回是否拥有、参数是否逃逸、对象是否可保存宿主引用、句柄销毁职责 |

禁止在绑定中放任意 Python/C++ 代码字符串作为签名。结构化的宿主模块和属性路径经过标识符校验后，由后端生成访问操作。默认值应优先表示“省略这个实参，由库使用其默认行为”，不要在编译时实例化 Python 默认对象。

每个导出使用 ForeignBindingId。HIR Call 记录所选候选与参数映射；EvaluationPlan 保存源码求值顺序。后端不得重新选择重载，不得因为参数名称相同而丢失位置实参顺序。

返回值可标记与输入共享存储，但这不代表源绑定被消费。对象身份、句柄拥有、底层数据拥有是不同维度。例如一个 ndarray view 可以是新 Python 对象，同时共享原数组的数据。

## 3 Python 逻辑模块映射

| 源 import | 运行时目标 | 要求 |
| --- | --- | --- |
| std/py | 本语言的 Python 桥接运行时 | 提供 py.Error 等桥接类型，不生成 import std.py |
| numpy | numpy | 别名如 np 仅是源作用域名 |
| torch | torch | 保留真实模块对象 |
| torch/nn | torch.nn | 由绑定给出映射，不把斜线当作 Python 文件路径猜测 |
| torch/optim | torch.optim | 同上 |
| torch/nn/functional | torch.nn.functional | 支持源别名 F |

普通包的子模块映射也必须来自解析结果。不得把任意 import 字符串直接拼成 Python import。绑定的模块存在与否由执行环境检查；语法和静态检查不要求安装这些库。

Python 不支持源文件声明的 native-only 能力时报告能力错误，并指出需要原生构建或已有显式 Python 实现。原生后端也不能把 Python 绑定当作可直接链接的 C 符号。

## 4 NumPy 与 PyTorch 第一阶段接口

第一阶段以 [04_numpy_torch.lang](../baseline/samples/04_numpy_torch.lang) 为完整目标，Python 3.12.x，CPU，float32，固定随机种子。NumPy、PyTorch 的精确版本与 wheel 来源由依赖锁任务实际验证后写入，不虚构尚未测试的兼容矩阵。

最少绑定集合如下。这里的类型名是语义说明；机器表示使用统一 schema 的稳定类型引用。

| 导出 | 第一阶段约束 | 返回/效果要求 |
| --- | --- | --- |
| np.float32 | 已声明的 NumPy dtype 值 | 不与 torch.float32 混用 |
| np.linspace | start、stop、num、dtype 的已选重载 | ndarray；错误为 py.Error |
| np.expand_dims | ndarray 与 axis | ndarray view/结果；不承诺新存储 |
| np.multiply、np.add | ndarray 与标量的所需重载 | ndarray；不静态承诺任意输入 shape 合法 |
| np.asarray | ndarray/所需输入与 dtype | ndarray；是否复制取决于 API，不假称总复制 |
| torch.float32 | 已声明的 torch dtype 值 | 与 NumPy dtype 分开建模 |
| torch.manual_seed | 整数 seed | 真实 Generator 返回对象，即使源代码忽略它 |
| torch.tensor | 本样例的 ndarray + dtype + device 重载 | Tensor；该显式 API 为拷贝导入 |
| nn.Linear | 输入输出特征数、CPU、dtype | 具体 Linear 实例 |
| Linear.parameters | 本样例无参形式 | 宿主参数迭代对象，不能隐式物化成 list |
| Linear 实例调用 | 单个 Tensor 输入 | Tensor；输出保持 model(x) |
| optim.SGD | 参数迭代对象、lr | SGD 实例 |
| SGD.zero_grad | set_to_none 的命名参数 | Unit；修改优化器/参数状态 |
| F.mse_loss | prediction、target 的本样例形式 | Tensor；保留梯度关系 |
| Tensor.backward | 本样例的无参形式 | Unit；执行宿主反向传播 |
| SGD.step | 无 closure 的本样例形式 | 对应无 closure 的已确认返回/效果契约 |

所有库调用按绑定计入 py.Error 效果，包括构造、对象调用、会失败的属性/迭代操作。绑定导出的 dtype 常量属于静态可解析属性；编译器不加载它们来做类型推断。

第一阶段不需要任意 nn.Module 子类、继承语法、用户回调、grad context、GPU、AMP、DDP 或 torch.compile。任何其中一项都要有独立能力声明和验收用例。

### 4.1 不允许的隐式改写

| 源操作 | 禁止的改写 | 原因 |
| --- | --- | --- |
| model(x) | model.forward(x) | 丢失模块实例调用路径的行为 |
| 返回 loss | loss.item() | 返回类型改变，且截断张量接口 |
| 传递 Tensor | Tensor.detach() | 改变自动求导关系 |
| 使用设备上的 Tensor | Tensor.cpu().numpy() | 改变设备、同步和存储语义 |
| 存储 Tensor/ndarray 引用 | 隐式 clone/copy | 对象共享与性能改变 |
| 参数生成器 | list(parameters()) | 物化与求值时机改变 |

torch.from_numpy、clone、detach 可以后续增加，但必须分别描述共享存储与梯度行为。detach 不等于独立数据快照，不能用它实现语言层面的“复制”。

### 4.2 错误与生命周期

py.Error 在外部操作边界包装普通 Python Exception，保存原始异常类型、消息和 cause/traceback；调用处无需显式问号。语言内部 typed throws 与 Panic 不被重新包装。控制事件不属于 py.Error，规则见 BACKENDS。

Python 对象的底层生命周期交给宿主。生成程序不得依赖 CPython 的某次引用计数立即归零来实现语言承诺的作用域资源清理；有确定时机要求的资源通过显式 defer/受控运行时协议处理。

对象可能保留对输入的引用或共享底层存储。只要真实对象保持可达，不能额外释放其 ndarray/Tensor 数据缓冲区。语言的静态 move/inout 约束不声称掌握所有外部 Python 别名。

## 5 Python 验收

1. 没有安装 numpy/torch 的环境中，使用已存在绑定运行 check 仍能完成静态检查；测试进程不能因检查而 import 这些库。
2. 正确生成上述模块路径，std/py 映射到语言运行时。生成 Python 通过语法编译。
3. 在依赖锁定的 CPU 环境实际运行完整训练 sample，而非仅执行 NumPy 前五条语句。
4. 数据 x_np 与 y_np 形状均为 (128, 1)，dtype 为 float32；满足 y = 2x + 1 的数值比较。
5. train 返回真实 Tensor，标量形状、float32、CPU；训练期间参数发生更新，损失有限且较初始化下降。
6. 对同一锁定环境、相同种子执行手写参考和生成程序；使用任务记录的明确容差比较最终损失与关键结果，不跨库版本承诺逐位一致。
7. 用宿主测试夹具注册 Linear forward hook，证明生成的 model(x) 路径执行 hook。测试夹具不扩大本语言支持的回调语法。
8. 证明返回值仍保留预期的 autograd 信息；没有隐式 item/detach/cpu/numpy。
9. 错误输入产生 py.Error，保留可用的原始原因和源位置；控制事件和 Panic 不被 typed catch 吞掉。

手写参考程序是 [numpy_torch_reference.py](../baseline/samples/numpy_torch_reference.py)。它只能作为行为参考，不能复制它然后把文件标记为转译器输出。训练数值测试通过也不能替代前端效果与类型负例测试。

## 6 C++ 互操作第一阶段

第一阶段支持由 Clang 22.1.8 编译的小型 C++ 包装层，通过显式绑定与本语言互操作。包装层负责 C++ 类型、构造/析构、重载和异常；本语言原生后端调用受控 ABI。

不要求本语言源码先翻译成 C。采用 C ABI 是为降低第一阶段接口复杂度；C++ 仍按 C++ 编译。不得以“双方都用 LLVM”为依据直接传递 std::string、std::vector 或 C++ 类对象。

### 6.1 ABI 范围

首版公开包装接口只使用明确宽度的整数、已声明浮点、指针/长度对和 opaque handle。状态码使用 u32/uint32_t，不能把 LLVM i1 或宿主 bool 当作跨语言约定。字符串、切片和记录的具体可用形式由绑定任务逐项实现；未实现类型拒绝导入。

以下是包装 ABI 的说明片段，不是可直接编译的完整头文件：

~~~cpp
extern "C" uint32_t image_create(
    int32_t width,
    int32_t height,
    void** out_handle,
    void** out_error
) noexcept;

extern "C" void image_destroy(void* handle) noexcept;
extern "C" void bridge_error_destroy(void* error) noexcept;
~~~

约定：状态 0 为成功，非零为失败；正常返回时 out_handle/out_error 都有确定指针值，但只有与状态对应的非空句柄可使用。创建失败不把部分初始化对象转移给调用者。错误句柄由包装库创建并由同一库销毁，错误码可表达无法分配错误对象等失败；错误句柄不保证每次失败都有值。读取输出指针前须先确认调用返回及状态。

本语言绑定把状态协议转换成语言的普通返回值或 typed throws，源码不手动检查每次 C 返回码。内部 LLVM throws ABI 与这套对外包装 ABI 是两个层次。

### 6.2 所有权与异常

| 边界对象 | 必须明确的约定 |
| --- | --- |
| 新建 opaque handle | 谁拥有、何时销毁、是否可共享、销毁是否幂等；不能默认幂等 |
| 借用 handle | 有效期、是否允许保存、返回后是否仍有效 |
| 输入缓冲区 | 长度单位、对齐、只读/可写、能否保存指针、失败时是否部分修改 |
| 返回缓冲区 | 分配者、释放者、长度、有效期；禁止跨不同分配器直接 free |
| 错误对象 | 谁保存消息、谁销毁、是否可嵌套原因 |

所有可能抛出的 C++ 异常在包装层捕获并转成状态/错误。未知异常也要在包装边界捕获；不让它跨过尚未支持 unwind 的本语言栈。包装销毁函数不向外抛异常；析构失败的处理必须形成独立的失败策略，不能继续使用已损坏句柄。

本语言 ARC 只管理绑定声明拥有的包装句柄。如果句柄内部是 shared_ptr，保留/释放动作必须调用包装层提供的协议，不能操作 C++ 对象内存或猜引用计数位置。禁止复制拥有裸句柄导致双重销毁。

第一版所有已实施源类型都可逻辑 copy。owned 外部句柄必须装入本语言 ARC 共享包装（最后一个拥有者 drop 时销毁外部句柄一次），或绑定提供可靠的 retain/release 协议；两者皆无则拒绝导入。不能在某个绑定里偷偷引入规范尚未支持的 move-only 类型。

### 6.3 后续直接导入 C++

直接导入头文件、C++ 类、模板或虚函数需要 Clang AST/Sema、目标 ABI、实例化、名称改编和所有权约定的共同支持。它是后续任务，不通过修改本语言 parser 来完成，也不是第一阶段包装方案的隐含能力。

## 7 std/os 最小接口

T11 首批只承诺标准输出和同步字节写入，后续再增加文件打开、路径、异步 I/O。接口签名如下，具体由标准库源码和声明式平台绑定实现：

| 接口 | 行为 |
| --- | --- |
| `os.stdout() os.File` | 返回标准输出的共享包装；包装不拥有底层标准句柄，释放包装不会关闭 stdout |
| `os.write(file os.File, data Vec[u8]) usize throws os.Error` | 执行一次可移植同步写；返回本次成功写出的字节数，允许部分写入 |
| `os.write_all(file os.File, data Vec[u8]) throws os.Error` | 循环写完全部字节；零进展时返回 NoProgress 错误，不能无限循环 |

`os.File` 是不透明平台句柄包装；不能将 Windows HANDLE 简化为 Linux fd。`os.Error` 至少提供稳定类别、平台原始错误码以及本次高级操作已完成字节数，避免 write_all 部分成功后报错时丢失进度。源级错误通过 typed throws 传播。

空缓冲区成功返回 0；write_all 的空缓冲区直接成功。非空 write 返回 0 不代表写入完毕，由 write_all 转成 NoProgress。WouldBlock 立即返回，交给调用者处理。

write 遇到 EINTR 且本次尚未写出字节时，返回 Interrupted，不自动重试。write_all 遇到这种 Interrupted 时，在同一位置重试，不设固定重试次数；此前已经写出的字节不得重写。平台调用已经成功写出字节时，按本次成功写出的数量推进，不能因遗留错误码再次报告中断。其他错误立即返回，并保留本次高级操作累计完成的字节数。

Linux 平台实现可以选择直接 syscall；Windows 第一版调用稳定的系统 API；两者均可不经 C 源码转换。原始 syscall 数字、寄存器约定、Windows 内核内部服务编号不暴露为可移植 os.write 合约。外部调用遵守目标 ABI，标准库不假定 LLVM 会替它决定系统接口。

Python profile 在出现明确的 Python std/os 实现前拒绝此模块。跨操作系统兼容和跨后端兼容是两个独立维度。

## 8 C++ 与平台验收

| 编号 | 场景 | 通过条件 |
| --- | --- | --- |
| FFI-01 | 创建、使用、销毁 opaque handle | 析构计数一次，正常返回与提前返回都正确 |
| FFI-02 | C++ 构造或方法抛异常 | 包装转为 typed error，无异常跨语言栈 |
| FFI-03 | 缓冲区传入/返回 | 边界、长度与所有权符合绑定，零长度规则明确 |
| FFI-04 | 不同分配器 | 始终由规定的包装释放函数回收 |
| FFI-05 | 错误对象分配失败 | 返回仍可识别的状态，不继续抛过 noexcept 边界 |
| FFI-06 | 不支持的 C++ 类/模板直接入参 | 导入或能力阶段明确拒绝 |
| FFI-07 | Linux 与 Windows 的分别构建 | 分别使用目标 ABI 和 SDK；实际执行后才记录平台支持 |
| FFI-08 | 复制/移动/共享句柄 | 满足 SEMANTICS 和绑定协议，无双重销毁 |

## 9 参考资料

- [NumPy typing](https://numpy.org/doc/stable/reference/typing.html)：静态类型描述的边界。
- [PyTorch Module](https://docs.pytorch.org/docs/stable/generated/torch.nn.Module.html)：模块调用与 hook 行为。
- [torch.tensor](https://docs.pytorch.org/docs/stable/generated/torch.tensor.html)：构造、复制与自动求导说明。
- [torch.from_numpy](https://docs.pytorch.org/docs/stable/generated/torch.from_numpy.html)：共享存储及限制。
- [Tensor.detach](https://docs.pytorch.org/docs/stable/generated/torch.Tensor.detach.html)：梯度边界与共享存储。
- [Swift C++ interoperability](https://www.swift.org/documentation/cxx-interop/)：Clang 集成及跨语言所有权约定的工程参考。
- [LLVM 22.1.8](https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.8)：包装编译器与原生后端使用的固定版本。

外部文档解释宿主 API；绑定必须在实际锁定版本上验证，而不是把官网滚动更新的签名当作已测试结果。
