# LLVM 与 Python 后端开发契约

本文规定后端开发要求；当前实现范围见[NATIVE_SCALARS](NATIVE_SCALARS.md)、
[PYTHON_SCALARS](PYTHON_SCALARS.md)及两端T08执行说明。ARC、外部绑定和严格浮点
执行仍未完成，不能按本文的目标要求宣称已经支持。本文以 [SEMANTICS.md](SEMANTICS.md) 的语言语义、[IR_CONTRACTS.md](IR_CONTRACTS.md) 的 IR 定义为准。后端不得自行选择另一套数值、所有权或清理规则。

## 1 目标与模块边界

采用 C++20。公共前端负责名称解析、类型检查、泛型约束和错误效果检查，输出带类型 HIR。Python 从 HIR 分流；原生后端经控制流 IR 再生成 LLVM IR。

| 模块职责 | 输入 | 输出 | 禁止依赖或行为 |
| --- | --- | --- | --- |
| Python lowering | 已检查 HIR、已解析绑定、Python 能力配置 | 结构化 Python AST、源码映射 | 不读取 LLVM IR；不为检查而导入 Python 包 |
| Native lowering | 已检查 HIR、所有权信息、目标配置 | 原生控制流 IR | 不重新解析源码；不悄悄改变函数效果 |
| LLVM emission | 已验证原生 IR、目标 ABI、运行库接口版本 | LLVM Module 或文本 IR | 不根据 AST 猜类型；不调用 Python 推断绑定 |
| Toolchain driver | 目标文件、显式链接配置、运行库 | 可执行文件或库 | 不从宿主架构推断交叉目标；不自动安装 SDK |

后端对其输入实施显式验证。未完成的语言特性应报告 capability 诊断，不能以跳过语句、返回零、调用动态 eval 或退回其他后端的方式伪造成功。

第一版不包含 MLIR、JIT、任意 Python 动态执行、直接导入完整 C++ 模板库、自定义 GPU kernel 编译。模块边界允许以后增加这些能力，但没有对应实现前不可宣称支持。

## 2 原生目标配置

以下是字段契约，不是已经存在的 C++ 头文件。

~~~cpp
struct NativeTargetConfig {
    TargetTriple triple;
    std::string cpu;
    std::vector<std::string> features;
    RelocationModel relocation;
    CodeModel code_model;
    OptimizationLevel optimization;
    RuntimeAbiVersion runtime_abi;
    ToolchainPaths toolchain;
};

struct ToolchainPaths {
    Path linker_driver;
    std::optional<Path> sysroot;
    std::vector<Path> library_search_paths;
    std::vector<Path> runtime_objects;
    std::vector<std::string> link_libraries;
};
~~~

规则：

1. target triple 是显式配置或编译器记录下来的宿主默认值。用户选择交叉目标后，禁止补用宿主 DataLayout、宿主 size_t 或宿主系统库。
2. CPU/features 影响机器码，必须进入构建缓存键。可移植发行构建不得默认启用宿主全部指令集。
3. 从所选 TargetMachine/目标描述取得 DataLayout。结构体字段偏移、对齐、指针宽度和 isize/usize 都以目标信息为准。
4. DataLayout 只提供布局基础；聚合参数如何传寄存器、是否使用 sret/byval、外部符号名与调用约定仍由 ABI lowering 处理。
5. 实施基线固定 LLVM/Clang/LLD 22.1.8，使用对应发行版文档和实际头文件；这不是“最新版”声明。禁止照抄开发分支 24.0.0git 的 API。构建记录同时保存工具链和运行库 ABI 版本；不能把不同版本的头文件和库混用。
6. sysroot、SDK、系统库、运行库或目标后端缺失时报告 toolchain 错误。生成过目标文件不等于已能链接、加载或运行。

## 3 LLVM 接入步骤

### 3.1 生命周期与数据所有权

每个独立代码生成任务拥有自己的 LLVMContext 和 Module。Module 与其中 Value/Type 的寿命不得超过 Context。前端公共类型使用 TypeId，不能把 llvm::Type* 写回公共 HIR。并行编译模块之间通过本语言的接口摘要和链接符号交换信息，不共享可变 Module。

设置 target triple 和 DataLayout 后，先声明函数与全局符号，再生成函数体。递归和跨模块调用必须引用已确定的签名。泛型实例化有稳定键：泛型声明 ID、具体类型列表、语义版本与目标相关布局条件；符号改名要避免不同实例碰撞。

### 3.2 指令生成

1. 把本语言基本类型、记录类型和函数签名降低为 LLVM 类型与 ABI 参数列表。
2. 给每个原生 IR 基本块建立对应 LLVM BasicBlock。
3. 为指令和块参数建立 ValueId 到 llvm::Value* 映射。
4. 生成算术、比较、内存、调用及终结指令，补齐 phi 或采用可验证的局部存储方案。
5. 在需要的源操作附带调试位置；生成代码的辅助指令必须可追溯到源 Span 或标明编译器生成。
6. 函数生成后验证函数，模块完成后验证模块。优化后再次验证。
7. 用 PassBuilder 建立所选优化流水线，再通过 TargetMachine 输出目标文件。

初期允许把 LLVM Module 输出成 .ll 后调用匹配版本的 opt/llc/clang 工具，验证模块边界。最终 API 集成必须复用同一个 lowering 逻辑，不能另写一条行为不同的字符串拼接后端。

可变局部变量初版允许通过入口块 alloca/load/store 实现，再由适当优化提升。禁止在循环中为同一个普通局部变量每轮增加无法回收的栈分配。对象地址逃逸、inout、销毁时点等必须根据语义决定，不能为了提升到 SSA 而改变可观察行为。

### 3.3 IR 不变量

| 不变量 | 检查位置 |
| --- | --- |
| 每个基本块恰好一个终结指令，终结后没有普通指令 | 原生 IR verifier、LLVM verifier |
| 值在使用前定义，SSA 定义支配使用；phi 前驱完整 | 原生 IR verifier、LLVM verifier |
| 调用参数、返回类型和调用约定与声明一致 | ABI lowering、LLVM verifier |
| 跳转离开的作用域正确执行 cleanup | 原生 IR verifier与路径测试；LLVM 不认识本语言 defer |
| 拥有值的每条路径都有正确的转移或销毁 | 所有权验证器；LLVM 不替代它 |
| 错误结果只在失败路径读取，正常结果只在成功路径读取 | 原生 IR verifier、路径测试 |
| 语言层类型、trait、错误效果已经成立 | 前端；不能依靠 LLVM verifier 补做 |

### 3.4 优化约束

默认先使用固定的 O0/O2 流水线。后续仅在基准和 IR 证据支持时修改 pass 顺序。优化等级不改变数值溢出、错误传播、边界检查或可观察析构的语言规则。

- nsw/nuw、inbounds、noalias、nonnull、nounwind 等都表示需要证明的事实，不能作为通用加速开关添加。
- 普通参数不自动意味着 readonly/noescape；inout 的语言限制也不能未经证明直接变成任意 noalias 属性。
- 浮点 fast-math 必须来自明确、可追溯的语言/构建选项。默认不得因 O2 自动启用。
- 不从宿主 C++ 的整数溢出、强制转换或求值顺序推导本语言行为。
- LLVM 验证通过只能证明 IR 结构满足相应约束，不能证明编译器保存了源语言语义。

### 3.5 数值 lowering 的必要分支

按 SEMANTICS 的本轮选择，整数加减乘和 neg 为固定宽度回绕，可生成不带 nsw/nuw 的对应运算。类型化常量计算也必须使用目标位宽规则，不能先在宿主有符号 C++ 整数上溢出后再截断。

有符号除法向零截断。对 MIN / -1，语言结果为 MIN，余数为 0；LLVM lowering 应先判断并单独处理该边界，不能直接对该输入执行 sdiv/srem。除数为 0 进入 Panic 路径。移位数为负或不小于位宽先进入 Panic 路径，再在合法分支执行移位，不能指望目标机器的掩码行为替代检查。显式数值转换的范围检查同样遵守 SEMANTICS。

Python 辅助函数复用同一组语言级数值测试。仅把源整数运算打印成 Python 运算符，会在溢出、负数整除和移位方面产生另一门语言。

## 4 原生错误通道与清理

### 4.1 throws 的内部调用约定

源码中的普通返回值 T 与错误 E 分别建模；Result[T, E] 仍是普通语言枚举。第一版原生后端采用显式成功/失败分支，不使用平台异常栈展开。

采用下列编译器内部 ABI 概念，与运行库和 IR 文档共同版本化；它不承诺跨编译器版本稳定：

~~~text
source: func f(args...) T throws E
native: f(args..., out_value: ptr<T>, out_error: ptr<E>) -> success:i1
~~~

success=true 时只保证 out_value 完成初始化；success=false 时只保证 out_error 完成初始化。Unit 返回值省略 out_value。两侧存储的布局、对齐及析构需求由类型布局计算，不能统一假定为机器字。此 ABI 不作为公开 C/C++ ABI。

HIR 只记录调用效果；原生 IR 使用 TryCall 双后继终结指令，成功块接收 T，失败块接收 E，Return 与 ErrorReturn 分开。状态参数只在 ABI lowering 时加入，不泄漏到公共类型系统。活动载荷只构造、转移或销毁一次；非活动侧不执行析构。

调用者的失败边首先转移错误拥有权，再执行所离开作用域的 cleanup，随后进入最近的匹配 catch 或继续向外传播。成功边取得正常结果。禁止先加载两侧输出再用 select 选择，避免读取未初始化对象。

handler 的错误流连接到外层错误目标，不回到同一 try 的其他 catch。不同原生错误类型默认不自动转换。调用图中不能产生源码签名未允许的错误。

### 4.2 清理路径

原生 IR 应沿用公共 ScopeId，记录正常离开目标、循环跳转目标和错误目标；ExitPlan.exited_scopes 按从内向外排列。lowering 应覆盖正常块尾、return、break、continue、throw、可抛错调用失败这六类出口。

对于 return expr，先求出并保护返回值，再执行清理，最后转移返回值。defer 注册点之前发生的错误不会执行该 defer。执行过的 defer 在退出最近词法块时按 LIFO 运行；清理读取执行时的绑定状态。

defer 本身不得向外传播错误。移动、借用或异步行为是否允许由 SEMANTICS 决定，后端不能通过复制捕获来绕过错误。内部生成的 cleanup 不允许被优化成正常路径专属。

同一作用域的显式 defer 与隐式销毁的相对顺序以 SEMANTICS 为准；代码生成必须引用一个统一 cleanup 计划，而非由各 AST 节点分别插入 release。

## 5 ARC 与原生运行库接口

下表仅给出后端需要的接口职责，具体符号、对象头和共享规则由 SEMANTICS 与运行库任务冻结。不要在源码语法之外新增隐式共享规则。

| 操作 | 职责 |
| --- | --- |
| allocate_object | 按目标布局和对齐分配对象，绑定销毁所需类型信息 |
| retain | 为已确定的共享拥有关系增加引用，不复制对象存储 |
| release | 释放一个拥有关系，必要时执行对象销毁与存储回收 |
| destroy_value | 根据类型递归销毁记录、枚举活动成员或拥有句柄 |
| panic/report | 处理语言规定的不可恢复失败，保留诊断位置 |

在原生 IR 中先使用类型化的 ownership 操作，再降低为运行库调用。简单标量和可平凡复制值不得无条件装箱或计数。优化可以消除被证明多余的 retain/release，但不能跨越可能保存引用、观察对象身份或触发销毁副作用的调用。

没有弱引用、循环引用策略、线程间所有权规则或适当限制之前，不得宣称 ARC 完整。暂不支持的组合应由能力检查阻止。native ARC 与 Python 对象引用管理是两套后端机制，不能把同一对象既交给 CPython 又交给自有对象头回收。

## 6 Python 后端

### 6.1 输出与运行环境

输入为已检查 HIR 和显式绑定。输出为可读 .py、必要的运行时辅助模块和源位置映射。Python 运行基线为 3.12.x，精确补丁版本和依赖组合由工具链锁定任务记录。C++ 实现可使用自有 PyAst 节点和确定性打印器；不要求在编译器内嵌 CPython。内部必须使用结构化 AST/代码生成节点，禁止把任意用户文本交给 eval/exec。

编译和 check 阶段不导入 numpy/torch，不启动 Python 子进程推断签名，不初始化 GPU。执行前单独检查锁定的 Python 环境和依赖版本；缺少库属于运行环境错误，不能伪装成源码语法错误。

普通用户函数、枚举和结构体名称需要按 SymbolId 稳定改名，避免 Python 关键字、模块别名、内部临时名和块级遮蔽冲突。本语言块级作用域不得直接继承 Python 函数级局部变量作用域。

### 6.2 语义映射

| 本语言结构 | Python lowering 要求 |
| --- | --- |
| 固定宽度整数和显式转换 | 遵守 SEMANTICS；必要时使用辅助函数，不能直接继承任意精度整数行为 |
| 浮点类型 | 保留精度、舍入和允许的转换；禁止默认忽略 f32 与 f64 差异 |
| 参数与调用 | 按源码从左到右求值；命名参数重排前先保存各实参值 |
| inout | 使用显式可写单元/位置对象或等价机制；不能用只在正常返回时执行的复制回写 |
| move | 共享的静态检查保证移动后的合法性；不要隐式复制 Tensor 存储 |
| Vec | 使用 langrt.Vec 或已证明等价的实现，保持共享容器、切片复制元素、活跃遍历期间禁止结构修改；不能直接套用 Python list/slice/for 的全部行为 |
| enum/switch | 使用稳定类型/variant 标识和数据载荷，保留穷尽性与绑定规则 |
| struct/enum 值复制 | 按类型递归复制值字段；嵌套 Vec/外部引用成员保持共享，不能用普通 Python 对象赋值替代全部逻辑 copy |
| typed throws | 本语言错误载荷由内部异常载体保存；catch 根据语言类型匹配 |
| 外部 Python 错误 | 在绑定调用边界包装为 py.Error，保留原异常信息和 cause |
| unsafe/原生能力 | 没有对应实现时报后端能力错误；unsafe 不授予自动兼容能力 |

函数对象调用和外部对象调用必须在 HIR 中区分。对于 model(x)，绑定记录的是对象调用能力，输出保持实例调用，不改为 model.forward(x)。外部调用的结果保持真实 ndarray/Tensor，不在返回时隐式 item、detach、cpu、numpy 或转成 list。

### 6.3 defer 不能只包函数尾部

defer 属于最近词法块，并且只有执行经过注册点后才生效。可用嵌套 try/finally 表达注册顺序：

~~~text
源代码的一个块：
    before;
    defer A;
    middle;
    defer B;
    after;

Python 结构：
    before
    allow_A = True
    try:
        middle
        allow_B = True
        try:
            after
        except langrt.Panic:
            allow_B = False
            raise
        finally:
            if allow_B:
                B
    except langrt.Panic:
        allow_A = False
        raise
    finally:
        if allow_A:
            A
~~~

这不是把所有 defer 提前注册。before 失败不执行 A；middle 发生 typed error 只执行 A；正常完成或 after 发生 typed error 执行 B 后执行 A。Panic 则跳过语言 defer；若 B 自身 Panic，外层也跳过 A。每层标志须为生成器唯一的作用域局部名。return 的表达式在 finally 之前求值。清理代码必须读取正确的语言绑定，不能被 Python 闭包晚绑定或循环变量复用改变。

三段式 for 降为 while 时，源码 continue 仍然必须执行步进语句。不能直接输出 Python continue 跳过步进。实现可通过携带 ControlTargetId 的结构化改写处理；嵌套循环、switch 的 break 和块清理要分别测试。

### 6.4 Python 异常边界

外部调用适配器只捕获 Exception 范围内的普通 Python 异常；不捕获 KeyboardInterrupt、SystemExit 等 BaseException 控制事件。langrt.Panic 继承 BaseException，不能被 typed catch 或 py.Error 适配器捕获；顶层入口单独转成失败诊断。内部语言错误载体应先识别并重新传播，避免被二次转换成 py.Error。包装保存原类型、消息、traceback/cause 与源调用位置。

不能用一个覆盖整个生成函数的 except Exception 把编译器生成错误与真实库错误混为一谈。适配器应包围已知外部调用及绑定规定的外部属性/迭代操作。检查器也必须把这些可能失败的操作计入效果。

## 7 目标文件与链接

编译器输出目标文件后，由 toolchain driver 链接语言运行库、目标系统库和用户显式依赖。初期可以调用已配置的 Clang 驱动完成平台链接；这不表示本语言先翻译为 C。后续统一驱动接口时保持相同输入记录。

将参数作为 argv 数组传给进程 API，不能拼成 shell 命令执行。记录工具路径、版本、退出码和保留的诊断。临时文件使用受控目录；失败时不得把残缺文件登记为成功产物。

原生可执行文件入口由驱动/运行库适配：源 main、操作系统入口和 C main 不是同一个符号约定。参数、退出码、顶层未处理错误和启动/退出清理由入口适配层规定。没有实现时先支持测试用无参数 main i32 子集，并明确诊断其他形式。

Linux、Windows、macOS 使用各自的目标布局、链接输入和系统接口实现。std/os 是逻辑模块接口；直接系统调用和操作系统 SDK 调用不能由后端把名字替换一下就完成。Python-only 绑定在原生构建中必须被拒绝，除非显式启用了独立设计的嵌入 Python 能力。

## 8 验收测试

以下是后续实现任务的测试要求，不是本次已运行结果。

| 编号 | 测试 | 通过条件 |
| --- | --- | --- |
| BE-01 | 标量、函数、条件、循环、简单记录 | O0/O2 执行结果一致且符合规范；不能只比较生成文本 |
| BE-02 | 递归调用和跨模块签名 | 正确链接与运行，错误签名在代码生成前拒绝 |
| BE-03 | throws 成功/失败两个路径 | 只读取对应初始化载荷，错误类型和 cleanup 顺序正确 |
| BE-04 | return/break/continue/throw 离开嵌套块 | 事件日志证明 defer 恰好执行一次且次序正确 |
| BE-05 | defer 注册前后发生错误 | 仅执行已经注册的 cleanup |
| BE-06 | 三段式 for 中 continue | 步进执行一次，循环行为与原生后端一致 |
| BE-07 | 命名实参带副作用 | 两个后端均按源码顺序求值，且每个实参只求值一次 |
| BE-08 | 回绕整数、除法、显式转换与边界值 | 逐条符合 SEMANTICS；O2 不改变错误或结果 |
| BE-09 | ARC 移动、共享、提前返回、错误清理 | 在可观察的测试运行库中无泄漏/重复释放/释放后使用 |
| BE-10 | 32/64 位布局或两种目标布局 | 使用目标宽度，不依赖宿主 sizeof；不能在无法运行的目标上声称运行通过 |
| BE-11 | 缺失目标 SDK/运行库 | 明确 toolchain 诊断，无成功产物 |
| BE-12 | Python-only 模块进入 native 构建 | 能力检查阶段拒绝并定位导入或调用 |
| BE-13 | Python 外部异常与控制事件 | py.Error 保留原因；KeyboardInterrupt/SystemExit 不被吞掉 |
| BE-14 | NumPy/PyTorch CPU sample | 见 INTEROP 的训练与对象语义验收 |
| BE-15 | 普通代码或 cleanup 发生 Panic | typed catch 不捕获；语言 defer 事件日志不出现随后被跳过的清理；Python finally 不意外扩大保证 |

若开启 sanitizer，优先用它检查编译器自身内存错误和运行库错误。sanitizer 测试通过不能替代跨后端语义测试。

## 9 参考资料

- [LLVM Parser and AST](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl02.html)：前端解析与 AST 分层。
- [LLVM 22.1.8 release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.8)：本轮冻结的 LLVM/Clang/LLD 版本。
- [LLVM IR generation](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl03.html)：IRBuilder 与验证器。
- [LLVM object generation](https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/LangImpl08.html)：TargetMachine、目标配置与目标文件。
- [LLVM PassBuilder](https://llvm.org/docs/NewPassManager.html)：优化流水线。
- [LLVM Language Reference](https://llvm.org/docs/LangRef.html)：指令、属性、DataLayout 与数值语义。
- [LLVM exception handling](https://llvm.org/docs/ExceptionHandling.html)：平台异常机制；本版采用显式错误通道。
- [Clang cross compilation](https://clang.llvm.org/docs/CrossCompilation.html)：目标平台、SDK 与链接配置。
- [Swift SIL](https://github.com/swiftlang/swift/blob/main/docs/SIL/SIL.md)：保留语言语义和所有权信息的中间层参考。
- [Python exception rules](https://docs.python.org/3/tutorial/errors.html)：异常传播与 finally 行为。

以上为参考项目资料；本语言的规范以本开发包中的规则为准，不从参考项目自动继承隐含语义。
