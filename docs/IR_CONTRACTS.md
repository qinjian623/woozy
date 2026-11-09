# AST 与中间表示契约

文档版本：0.1，对应本包实施接口版本 `compiler-dev/1`；两者分别表示文档修订和共同接口版本，不是两套接口。本文给出前端、LLVM 后端和 Python 后端共同遵守的待实现数据契约。示例是伪 C++ 和概念 IR，不是已存在的 API 或可执行实现。运行语义见 [SEMANTICS.md](SEMANTICS.md)，语法入口见 [FRONTEND.md](FRONTEND.md)。

## 1 三种表示的职责

| 表示 | 保留的信息 | 不应承担的工作 |
| --- | --- | --- |
| AST | 源码结构、源区间、未解析名字、类型写法、中性后缀、恢复节点 | 目标数据布局、机器 ABI、寄存器和 ARC 物理调用 |
| Typed HIR | 确定类型、符号引用、结构化控制流、调用匹配、效果、所有权合法性事实、外部绑定身份 | LLVM Value 指针、CPython 对象指针、平台栈展开细节 |
| Native IR | 单态化函数、基本块、值和存储位置、正常/错误后继、清理和拥有操作 | 源码名字查找、重新推断类型、从未知动态对象猜 ABI |

Python 后端从经验证的 HIR 生成代码。原生后端从 HIR 生成 Native IR，再降低成 LLVM IR。本文的 Native IR 不是 LLVM 项目里的 Machine IR，代码目录不得把两者混为同一类型。

第一版 HIR 可以是带类型和解析结果的结构化 AST；不要求先实现自定义文本解析器或通用优化框架。必须支持确定性的打印和验证器。

## 2 身份与存储

```cpp
struct AstNodeId;       // 某个 AST arena 内唯一
struct HirNodeId;       // 某个 HIR arena 内唯一
struct SymbolId;        // 当前编译会话的声明身份
struct TypeId;          // 当前 TypeContext 内的规范化类型身份
struct ScopeId;
struct ModuleId;
struct ForeignBindingId;
struct FunctionInstanceId;
struct BlockId;
struct ValueId;
struct LocalSlotId;
```

这些 ID 必须是强类型，不能随意把 `TypeId` 当成 `SymbolId` 使用。数字编号只在所属上下文有效；不得把内存地址或裸编号当作跨进程、跨构建的稳定身份。

缓存或序列化使用带版本的逻辑键：包身份、模块路径、声明路径、规范化类型实参，以及相关源码/绑定内容摘要。实例化键至少为“泛型声明身份 + 具体类型实参 + 影响实例化的目标/配置”。缓存键与对外链接符号的编码分别定义。

arena 和源文件快照由阶段结果持有，发布后不可变。跨阶段关系通过显式映射保存，例如 `HirNodeId -> AstNodeId/SourceSpan`，不能让一个 AST 节点含有可变的 LLVM 后端指针。

## 3 类型表示

```cpp
enum TypeKind {
    Unit, Bool, SignedInt, UnsignedInt, Float, String,
    TargetSizedInt, NominalStruct, NominalEnum,
    TypeParameter, Function, BuiltinContainer, ForeignOpaque,
    ErrorType  // 仅为诊断恢复，不能出现在已验证 HIR
};

struct ParameterType {
    ParamMode mode;     // ordinary / inout / move
    TypeId type;
};

struct FunctionType {
    Vector<ParameterType> params;
    TypeId result;
    Optional<TypeId> throws_type;
};
```

类型规则：

- 类型按结构或声明身份规范化并驻留；结构体/枚举是名义类型，不因为字段相同而相等。
- `FunctionType` 不包含参数名。直接调用的标签来自声明签名或外部绑定。
- `throws_type` 为无或单一错误类型。它不是普通返回值中的 `Result` 参数。
- `Self` 在声明检查时替换为当前实现类型或 trait 的受约束类型变量。
- 泛型体允许 `TypeParameter`；具体 Native IR 函数必须不再有未绑定类型参数。
- 外部类型由 `ForeignBindingId` 和宿主类型身份决定；不能因为都来自 Python 就合并成 `Any`。
- HIR 保留 `isize/usize` 的源类型身份；目标配置决定其位宽。Python 后端使用项目明确的目标数值配置，不能悄悄采用宿主位宽。
- 外部 ndarray/Tensor 的形状和设备若没有静态证明，就保留为运行时属性；不要在 TypeId 中伪造已知形状。

编译期数字保留未定类型常量节点；在经验证 HIR 中，所有可执行数字表达式必须获得确定类型。常量的数学值、目标位模式和源文本分别保存，避免格式化器与代码生成对溢出作不同解释。

## 4 AST 最小节点集合

| 类别 | 最小节点 |
| --- | --- |
| 文件 | PackageDecl、ImportDecl、顶层声明序列 |
| 声明 | BindingDecl、FunctionDecl、StructDecl、EnumDecl、TraitDecl、ImplDecl |
| 类型写法 | NamedTypeSyntax、FunctionTypeSyntax、ParenthesizedTypeSyntax |
| 表达式 | Name、Literal、Unary、Binary、Call、Member、NeutralBracket、Slice、List、FunctionLiteral、ErrorExpr |
| 语句 | Block、Binding、ExprStmt、Assign、If、For、ForIn、Switch、Try、Defer、Unsafe、Return、Throw、Break、Continue |
| 模式 | LiteralPattern、BindPattern、Wildcard、QualifiedVariantPattern |

每个节点含 `SourceSpan`。关键 token（声明名、运算符、实参标签）还需独立源区间，以便精确诊断。括号是否保留为独立节点由格式化需求决定，但不能丢失重建 token 范围所需的信息。

```cpp
struct NeutralBracketItem {
    TokenRange tokens;
    Optional<ExprSyntaxId> expr_candidate;
    Optional<TypeSyntaxId> type_candidate;
};

struct NeutralBracketSuffix {
    AstNodeId base;
    Vector<NeutralBracketItem> items;
    SourceSpan span;
};
```

普通类型上下文中的 `Vec[i32]` 直接使用 TypeSyntax；中性后缀仅处理表达式上下文的歧义。恢复节点必须显式可见，不能使用缺失 SymbolId 的普通 Name 节点冒充错误。

## 5 HIR 通用字段

```cpp
struct HirModule {
    ModuleId identity;
    Vector<ModuleId> imports;
    DeclarationArena declarations;  // 函数、类型、trait、impl、常量
    ExpressionArena expressions;
    StatementArena statements;
    ScopeArena scopes;
    SymbolTable symbols;
    TypeContext types;
    Vector<ForeignBindingId> required_bindings;
    SourceSnapshotSet sources;
};

struct HirExpr {
    HirNodeId id;
    SourceSpan source;
    TypeId type;
    ExprCategory category;  // value / place / callable / type-as-callee
    EffectSet effects;
    HirExprPayload payload;
};

struct HirFunction {
    SymbolId symbol;
    Vector<GenericParameter> type_params;
    Vector<ParameterDecl> parameters;
    TypeId function_type;
    HirBlockId body;
    CaptureSet captures;
    SourceSpan source;
};
```

`ExprCategory` 用于源语言分类；不能把它直接当作机器寄存器/地址选择。`EffectSet` 是局部表达式可能传播的错误集合；函数类型仍只有一个声明错误类型。效果不等于全部副作用：写内存、I/O、Python 调用等另用 `OperationProperties` 描述，不能把“无 throws”当作“纯函数”。

验证后的 HIR 不允许：未解析名称、未决 NeutralBracket、无类型可执行表达式、ErrorType、MissingToken 导出的正常节点、未知外部绑定或未处理的目标不支持能力。

`VerifiedHirModule` 是包含只读 HirModule、规范版本、功能配置、目标配置和绑定摘要的受控结果包装，只能由语义验证阶段构造。已按 Python 配置验证的包装不能直接冒充按 native 配置验证；公共结构可以复用，目标相关检查结果分别保存。

T07 的模板与具体函数使用同一种 HIR 节点和 TypeContext。模板中已有声明和约束的
TypeParameter 是已绑定类型；具体执行函数中不得残留它或模板专用调用。
`sema::check_types` 与 `hir::verify_types` 只返回静态检查结果，不授予后端生成权限。
唯一发布函数 `hir::verify` 深复制数据并检查所选后端已支持的操作，才构造
VerifiedHirModule。后端支持检查只遍历具体函数及其可达节点、实际使用的类型；
未实例化模板及类型池中未使用的条目不作为拒绝生成的理由。

## 5.1 trait 应用身份

泛型 trait 的绑定、实现和约束调用都显式保存完整应用身份，不新增 TypeKind 或 NodeKind：

```cpp
struct TraitRef {
    SymbolId declaration;      // 真实 trait 声明
    Vector<TypeId> args;       // 按声明顺序的类型实参，空表示无参 trait
};

struct GenericParam { SymbolId symbol; Optional<TraitRef> trait; ... };
struct TraitDecl { SymbolId symbol, self; Vector<GenericParam> params; ... };
struct ImplDecl { Vector<GenericParam> params; Optional<TraitRef> trait; ... };
struct ConstraintCall { TraitRef trait; SymbolId method; ... };
```

无参 trait 使用空 `args`，它仍是一项 bound，不是多约束列表。`TraitDecl.params`
保存真实声明参数；本批这些参数无约束。`ImplDecl.trait` 的实参可以引用 impl
自身参数，包括只出现在 trait 应用中、不出现在 Self 或方法签名中的 phantom
参数；这些参数仍进入方法实例键。`ConstraintCall.trait` 随所属模板实例替换。

约束缓存、递归检测和 overlap 判断使用 `(实际类型, trait 声明, 完整有序实参)`，
不能只按 trait `SymbolId`。同一声明的 impl 重叠必须联合求解 Self 与全部实参，
共享同一个替换表。具体实例最终仍是普通 `Call`，不保留 `ConstraintCall` 或未
替换的 `TypeParameter`；后端不接触 trait 分派。验证器从真实 Source 重建声明、
实参顺序、bound、impl 与方法签名，不能只信 producer 记录或可改缓存。

## 6 调用和求值计划

```cpp
enum CalleeKind {
    DirectFunction, Method, FunctionValue, StructConstructor,
    EnumConstructor, ForeignFunction, ForeignMethod, ForeignCallable
};

struct CallArg {
    uint32_t source_index;
    uint32_t parameter_index;
    Optional<InternedText> source_label;
    ParamMode mode;
    HirExprId expression;
};

struct HirCall {
    CalleeKind kind;
    CalleeRef callee;
    Vector<TypeId> type_args;
    Vector<CallArg> args_in_source_order;
    CallSignatureId signature;
    Optional<ForeignBindingId> foreign_binding;
};
```

调用契约：

1. `args_in_source_order` 保持源码顺序；`parameter_index` 只用于放置已经求值的临时结果。
2. callee/接收者先求值且仅求值一次，再依次求值实参。任一步失败，后续步骤不执行，已产生的临时值按语义清理。
3. 接收者可在内部参数表中占第一个位置，但不能因此把方法基对象重新求值。
4. 外部绑定的可省略参数保留 `omitted` 状态；不能自动生成 `None`。显式提供参数和省略参数是两个状态。
5. 外部可调用对象保持其调用种类。`model(x)` 不得在后端偷偷改成 `model.forward(x)`。
6. 单态化改变 callee 身份，但不改变源级求值顺序或参数模式。
7. 类型名调用 `Type.method(...)` 或受约束的 `T.method(...)` 没有运行时接收者表达式，所有声明参数均由源码实参提供，从形参索引 0 开始，包括普通首参 Self；零参数调用允许空实参表。值形式仍保留既有接收者及其求值顺序。约束调用保存用于选择 impl 的真实类型和 trait，不能为沿用接收者索引而虚构一个值；验证器须核对源码的调用形式及所属实例替换，不能仅按实参数量推断。

实现可在后端入口将调用展开为显式 `EvaluationPlan`：先保存 callee，再保存各实参临时值，最后按形参顺序组装调用。若两个后端都使用这一步，应复用纯前端的展开算法，而不是分别手写排序逻辑。

## 7 HIR 控制流、模式与清理

HIR 保留 `If/For/ForIn/Switch/Try/Defer`，并记录解析完成的跳转目标和作用域身份。`break/continue` 不保留“以后找最近循环”的开放含义。

```cpp
struct ExitPlan {
    ExitKind kind;     // normal / return / break / continue / error
    ExitTarget target;
    Vector<ScopeId> exited_scopes;  // 从内向外
};

struct HirDefer {
    ScopeId owner_scope;
    HirBlockId body;
    CaptureUseSet observed_bindings;
    SourceSpan source;
};
```

`ExitPlan` 不直接塞入复制出来的 defer AST。各后端依据同一作用域和清理定义生成路径。返回表达式先求值；返回值按所有权规则保存后，执行清理，再离开函数。

defer 读取执行时的绑定状态，其关联变量不能在注册时被无条件复制成值捕获。若移动或借用使退出时读取非法，语义检查拒绝。隐式销毁与用户 defer 的顺序统一采用 `SEMANTICS.md`，不得由后端各自选择。

defer 注册是运行时可达性的一部分：控制流到达该语句才激活对应清理；每次进入作用域重新开始。case/default 是独立的隐式词法 ScopeId，分支结束清理该作用域。目标可以在证明到达性固定后优化注册记录，但不能预先激活同一函数里的所有 defer。

catch 有明确的错误类型、绑定符号、body 和外层错误目标。当前 try 的 catch 仅处理 try body 的错误，不能把当前 handler 的错误重新送入同一组 handler。

switch 记录：被匹配值临时结果、每个模式的类型化结构、分支绑定、是否穷尽，以及不可达分析结果。被匹配表达式只求值一次；分支正常结束跳到 switch 后，不隐式 fallthrough。

Pattern 的末尾追加字面量值，不新增 NodeKind 或“已匹配”可信标记：

```cpp
using PatternLiteral = std::variant<BoolLiteral, IntegerLiteral,
                                    FloatLiteral, StringLiteral>;
struct Pattern {
    TypeId type;
    Source source;
    Optional<SymbolId> variant;
    Optional<SymbolId> binding;
    Vector<Pattern> payload;
    Optional<PatternLiteral> literal;
};
```

variant、binding、literal 三者互斥；literal 不能带 payload。无三者才是 wildcard。原五字段聚合初始化保持原意。integer 保存目标规范化十进制值和目标位模式，float 保存目标精度位模式（含符号零），string 保存解码后的完整 UTF-8 字节。验证器从真实 LiteralPattern Source 和实际目标类型重新解码并核对类别、值、位模式和字节；不能只信 producer 的值，也不能删除 literal 后把普通 `case` Source 冒充 default。普通 `_` 必须来自 BindPattern，只有起始 token 确为 `default` 的 Case 可作为 wildcard Source。

普通 `case` 的 Pattern Source 必须精确等于本 Case AST 的 `children[0]` 的 NodeRef 与 span；default wildcard 必须精确等于本 Case 的 Source 并保留 `default` token 核对；variant payload 的 Source 必须精确对应当前 AST payload child，而不是只落在父 span 内的任意后代。泛型实例只替换类型/绑定并保留真实 Source 身份。

覆盖分析区分真实空枚举、bool、整数、float、string 与其他总匹配域；内部 Other 类只表示“其余值”，不进入公共 Pattern 也不生成运行时代码。有限整数值按目标类型规范化后判重，float 的覆盖键合并正负零，NaN 只属于 Other。三个调用方（sema、HIR verifier、`RequiredExits`）必须使用同一分析，不能把未知或 `!variant` 一概当成穷尽。Native 模式测试先确认父 tag 再读取活动 payload，全部测试成功后才复制拥有绑定；String 相等的 NativeIR 只允许同类型 `equal/not_equal` 产生 bool，先比较长度，再比较完整字节。

## 8 所有权事实和目标能力

HIR 应描述源语言所需的操作和检查结果，而不是提前确定 retain/release 次数。

| 信息 | 例子 |
| --- | --- |
| 值的使用方式 | ordinary read、copy、move、inout borrow |
| 存储位置 | 整变量、字段、索引；初期复杂可变位置可拒绝 |
| 活跃性 | moved / initialized / maybe-moved 等数据流事实 |
| 逃逸 | 借用是否进入存储、返回、闭包或外部调用 |
| 清理义务 | 拥有的局部值、部分初始化对象、调用临时值 |
| 后端能力 | native-only、Python binding、共用操作 |

普通参数的非消费语义不等于 `readonly` 或 `noescape`。前端不能据此给 LLVM 添加不成立的属性。外部库可能保留对象引用或修改对象内部状态，必须按绑定契约检查。

原生 ARC 操作不进入 Python HIR。Python 对象生命周期由宿主和显式清理实现；本语言 `move/inout/defer` 的可观察约束仍必须由公共检查器或 Python 兼容层遵守。

## 9 Native IR

### 9.1 函数和基本块

```cpp
struct NativeFunction {
    FunctionInstanceId identity;
    LogicalSignature signature;
    Vector<LocalSlot> locals;
    Vector<BasicBlock> blocks;
    BlockId entry;
};

struct BasicBlock {
    BlockId id;
    Vector<BlockParameter> parameters;
    Vector<Instruction> instructions;
    Terminator terminator;
};
```

Native IR 采用块参数形式表达跨块值，局部可变绑定先表示为显式 slot 的读写。第一版允许最终使用 LLVM alloca/load/store 加适当优化实现局部变量；不要求前端先做一套完整的局部 SSA 提升算法。

每个块必须且只能有一个 terminator。块内值定义后才能使用；跨块 SSA 值满足支配关系；分支传给块参数的数量和类型一致。局部 slot 的初始化、移动后使用和销毁义务独立验证，不能靠 LLVM verifier 代劳。

### 9.2 初始操作集合

| 类别 | 操作与约束 |
| --- | --- |
| 常量与数值 | 常量、整数/浮点运算、比较、转换；携带明确语义种类，不使用无规则的 `Add` |
| 存储 | StorageLive、Load、Store、ProjectField、IndexPlace、StorageDead；只在合法初始化/借用状态下使用 |
| 聚合 | 构造结构体/枚举、读标签、提取字段；布局交给目标布局阶段 |
| 所有权 | CopyValue、MoveValue、DestroyValue、BeginInout、EndInout；按类型区分平凡复制和拥有操作 |
| 无错误调用 | Call；callee 的逻辑签名不含 throws |
| 分支 | Branch、CondBranch、SwitchTag；分支目标明确 |
| 可失败调用 | TryCall terminator；正常和错误两个后继 |
| 函数出口 | Return、ErrorReturn、Unreachable；普通错误和不可恢复 Panic 不混为一种出口 |

词法允许但原生目标不支持的外部 Python 操作应在目标能力检查时拒绝，不得在 Native IR 中放一个未定义地址的 Python 调用占位符。

显式数值转换沿用原有 `integer_convert` / `Op::convert` 记录与全部字段：子节点类型
是源类型，结果类型是目标类型。只允许 integer→integer（范围检查）、float→integer
（先按源 IEEE 值向零截断，再检查截断后的数学整数范围；NaN、无穷和越界是 Panic）与
integer→float（直接按目标 f32/f64 精度做一次 RNE）三类。float→float、bool 参与以及
非数值源/目标仍是无效 IR；转换嵌套沿用同一记录。不新增 NodeKind、字段或 Native ABI。

当前平凡值实现追加 `struct_init`、`enum_init`、`field_read`、`field_address`、
`load_indirect`、`store_indirect`、`enum_test`、`enum_payload`，保留原操作编号。
字段与变体使用已解析的 SymbolId；地址结果的 TypeId 表示所指值类型，地址不能
作为普通值使用。载荷读取要求所有进入路径都经过同一 SSA 枚举值对应变体的
真分支。存储布局替换使用私有 TypeContext 副本，其新增 ID 不进入公开 Native IR。
具体结构和检查见 `include/lang/ir/native.hpp` 与 `docs/NATIVE_SCALARS.md`。

### 9.3 可失败调用

```text
entry:
    try_call @checked_square(%input)
        success -> ok(value: i32)
        failure -> fail(error: InputError)

ok(value: i32):
    return value

fail(error: InputError):
    error_return error
```

这是概念 IR，不是源语言语法或 LLVM IR。`TryCall` 正常后继获得正常返回值，错误后继获得相应错误类型；调用失败时不得读取正常返回值。

错误分支可以进入 catch、清理块或当前函数的错误出口。`throws` 的机器编码在 ABI lowering 中确定；当前 Native IR 的逻辑签名不能直接假设它就是源语言 `Result[T,E]`。

`return Result.Err(...)` 使用普通 Return；它不会成为 ErrorReturn。

### 9.4 清理降低

为每条离开作用域的边计算清理序列：正常结束、return、break、continue 和可传播错误都要覆盖。使用共享清理块或复制块均可，但控制流事实、执行次数和顺序必须相同。

记录临时值/局部值是否成功初始化，防止构造中途失败后销毁未初始化内存。移动后的拥有值不能再次销毁。清理体不允许未处理的 typed throws；如果引入可失败析构，必须先修改语义契约，不能由代码生成临时决定吞错。

## 10 降低到 LLVM 的输入契约

LLVM 后端接收经过 Native IR 验证的模块和完整目标配置，包括 triple、CPU/features、数据布局、优化级别、链接模式和运行库 ABI 版本。

- 位宽、符号扩展、截断、溢出、除法和移位行为按 `SEMANTICS.md` 显式降低。
- `nsw/nuw/noalias/nounwind/readonly` 等属性仅在条件已证明时添加；普通参数、`let` 或无 throws 本身不足以证明全部这些属性。
- 目标布局阶段决定对象字段偏移、对齐和参数传递规则。不要把宿主 C++ `sizeof` 当作目标语言类型大小。
- 单态化、清理生成和 ARC 降低完成后，才将语言操作变成 LLVM 指令和运行库调用。
- 每个原生 IR 操作携带源区间；生成调试信息时保持关联。优化后的指令不一定与源码一一对应，不能伪造精确映射。
- 生成后运行 LLVM verifier；它是第二道 IR 检查，不替代 Native IR 和源语言验证器。

## 11 Python 后端输入契约

Python 后端接收 `VerifiedHirModule`、目标语义配置和已锁定的绑定集合；不接收原生对象布局或 ARC 调用序列。

必须保留源码求值顺序、短路、调用身份、错误边界和词法块清理。固定宽度数值语义由兼容函数或等价生成代码实现；原始 Python 运算只有经证明等价时才可直接使用。

第一版通过每个节点的已解析绑定生成 Python 名字，不把原始语言标识符直接拼接成未经处理的宿主名字。保留 SymbolId 到生成名字的映射，避免与 Python 关键字、模块别名或运行库辅助名冲突。

`nn.Linear` 等外部类型的具体调用签名来自绑定。自定义类、动态属性、未知接口和未支持的原生能力产生静态诊断，不生成动态兜底分支。

## 12 阶段验证器

| 验证器 | 必查项 |
| --- | --- |
| AST verifier | node/token 范围有效、子节点归属正确、恢复节点显式、无环节点结构 |
| Resolved verifier | 所有正常引用有符号、符号 kind/可见性正确、作用域不悬空 |
| HIR verifier | 类型完整、调用实参映射唯一、效果已处理、目标能力允许、泛型约束成立 |
| Ownership verifier | 移动/借用状态、inout 独占与不逃逸、捕获与清理可达性 |
| Native IR verifier | CFG/支配/类型一致、slot 状态、TryCall 双后继、正确销毁与出口 |
| LLVM verifier | LLVM 结构、类型和指令规则自洽 |

验证器失败要区分用户程序错误和编译器变换错误。已通过前端的程序在无效 lowering 后产生未处理效果，应报告内部编译器错误并保留最小 IR，而不是指责用户漏写 throws。

## 13 互操作与回归测试

1. 对同一小程序，打印 AST、HIR、Native IR 和 LLVM IR，确认名字、类型、调用身份和源位置逐层可追踪。
2. `f(second: side_a(), first: side_b())` 在两个后端记录相同的副作用顺序；前者抛错时后者不执行。
3. 含短路和循环的程序在优化前后保持一致；不允许提前求值右侧可失败调用。
4. 验证嵌套 defer + return/break/continue/throw 的清理次数和顺序；单独覆盖返回值表达式自身失败。
5. 用基线普通 Result 例子验证 ErrorReturn 不会误用于 Result.Err。
6. 将未处理的 NeutralBracket、ErrorType 或缺失 ForeignBindingId 人为送入 verifier，必须被拒绝。
7. 同一泛型以同一类型多次使用只生成一个逻辑实例；不同目标配置不能错误复用机器布局。
8. 原生和 Python 数值边界测试使用同一预期结果表；与宿主默认运算不同的输入必须覆盖。

这些用例属于实现后的验收计划；本资料包没有声称它们已经通过。

## T08 已实施的静态记录

实际 C++ 字段、编号、稀疏退出记录规则、Verifier 独立重算和泛型复制要求见
[T08_STATIC.md](T08_STATIC.md)。这些字段仍使用本节原有 TypeId/SymbolId/ScopeId/
HirNodeId，不增加身份图。`check_types`/`verify_types` 接受静态 T08，具体函数的
throws、Try、Defer、Throw 仍由共用后端支持检查明确拒绝；`hir::verify` 的唯一发布
入口及不可变快照要求保持。本文概念 Native IR 的 TryCall/ErrorReturn 尚未在本轮实现。
