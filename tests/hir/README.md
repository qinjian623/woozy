# T04 标量 HIR 组件

接口版本 `compiler-dev/1`，功能范围 `scalar-v1`。唯一公共头为
`include/lang/hir/hir.hpp`；实现为 `src/hir/hir.cpp`。本目录只交付类型表、
结构化 HIR、验证器和测试，不表示整个 T04、源语言检查器或后端已经完成。

## 给 checker 的接口

所有接口位于 `lang::hir`：

- `TypeContext::intern(Type) -> TypeId`、`find(TypeId) -> const Type*`、`size()`。
  TypeId 从 1 开始，0 永远无效。find 返回的指针可能被后续 intern 重分配失效，跨驻留调用应保存 ID。`intern` 对错误形状或不存在的类型引用抛
  `std::invalid_argument`；语义诊断由 checker 产生。相同类型驻留一次，编号随
  确定的插入顺序确定。Function 的参数 mode/result/throws 参与身份，标签不参与。
  名义类型以声明 SymbolId 区分，ForeignOpaque 另保存 binding ID；这些表示目前不能进入 Verified。
- `Config { Backend backend; unsigned word_bits; }`；backend 为 `native/python`，
  word_bits 为 32/64，必须与拥有的 Project 一致。
  `bit_width(Type, Config)` 取得整数/浮点的位宽；Isize/Usize 使用配置，不使用宿主宽度。
- `HirModule { resolved, identity, types, config, functions, nodes, scopes }`。
  `resolved` 为 `shared_ptr<const modules::ResolvedProject>`，拥有符号和源码。
  不另建符号表。functions 可以包含所需项目模块的函数定义，直接调用必须找到其中的定义。
- `HirFunction { symbol, signature, parameters, body, scope, source }`。
  `Parameter { symbol, label }` 中丢弃参数 `_` 的 label 为空；其它 label 与声明名字一致。
  顺序对应 Function Type 的 parameters；实际类型及 mode
  来自唯一签名。根 body 为 Block，使用函数根作用域；参数与根 body 共用作用域。
- `HirNode { type, source, scope, children, data }`，`kind()` 返回由 NodeData variant
  确定的 NodeKind，避免 kind 与 payload 不一致。每个节点包括语句均有有效 TypeId；
  语句为 Unit。Source 同时保存 `modules::NodeRef` 与原始 SourceSpan。
  HirNodeId/ScopeId 为容器的零起始下标；SymbolId 沿用 ResolvedProject 的零起始下标。
- 节点字段与 children 顺序直接写在头文件中。`Read/Place/Binding` 保存 SymbolId；
  Binding 的类型来自已定类型的初始化表达式，Read/Place 必须精确相等。
  Place 只允许整变量赋值目标或 inout 实参。Assign 保存可选 compound 运算符，目标只求值一次。
  `IntegerLiteral` 同时保存规范十进制数学值和零扩展目标位模式，Source 回指原文。
  `FloatLiteral` 保存精确 IEEE 位模式，不经过宿主 double 重新舍入。
- `Call { kind, signature, direct_symbol, callee, args }`。
  args 每项保存 `source_index/parameter_index/label/mode/expression`，不排序。
  children 严格为函数值 callee（如有），然后按源码顺序的所有实参。
  直接调用可使用声明标签；函数值调用只使用位置参数。当前接受位置参数、命名参数、
  以及位置参数前缀后接命名参数；位置参数的 parameter_index 等于 source_index。
  其它混合形式暂不接受，不改变源语言规范。
- `For` children 为 init、condition、step、body；init 接受 Binding、Assign、
  ExpressionStatement 或 Noop，缺省 init/step 使用 Noop，
  缺省 condition 使用 BoolLiteral(true)。For 建立子作用域，body Block 再建立子作用域。
  每个普通 Block 也建立子作用域。`Return.function`、`Break.loop`、`Continue.loop`
  必须已经确定；loop 指向同函数最近包围的 For 节点。
- `verify(const HirModule&, stop_token = {}) -> PhaseResult<VerifiedHirModule>`。
  成功才有 value；结构损坏或当前不支持的数据返回 `E_IR_INVALID`，FailureKind::internal，
  exit_code 3，phase 使用诊断 schema 已有的 lowering。源程序的 `E_CAPABILITY_UNAVAILABLE` 等应由 checker 提前报告。
  取消返回 PhaseState::cancelled、无 value、exit_code 130。
- `VerifiedHirModule` 无公开/default 构造，只有 verify 可以创建；提供只读
  `module()/config()/version()`。内部为 `shared_ptr<const HirModule>`。
  发布时复制 builder、ResolvedProject、Project、各 AstFile，避免保留的可写别名改动结果。
  SourceSnapshot 使用 support 的不可变对象。调用者不得在 verify 复制期间并发修改 builder。
- `dump(TypeContext/HirModule/VerifiedHirModule)` 返回确定文本，包含配置、类型、函数、
  作用域、节点、调用顺序、源编号和区间，不输出地址或机器目录。不定义反序列化格式。

## 验证内容和限制

验证器检查所有节点和作用域，包括不可达节点；不允许游离节点、游离作用域、有环或共享的
可执行子节点。多次执行相同表达式要建立多个节点，不能用共享节点暗示只计算一次。

检查类型存在性、操作数/结果类型、整数数学值范围及位模式、符号身份与声明种类/原文、
源码节点及区间、函数签名和参数、完整且唯一的调用参数匹配、求值顺序、标签和模式、
作用域归属、初始化先于使用、写权限、跳转种类及同函数包围关系。
对正常返回做保守分析：非 Unit 函数不能正常结束；只有字面量 true 且没有可达的、指向
该循环的 break 的 For 才认定为不正常结束。Block 在已终结的语句之后不再累计 break；
内层循环的 break 不作为外层循环的退出。If 条件为 BoolLiteral 时只累计实际分支的
正常出口和 break；其它条件仍保守检查各分支，不在 verifier 内做数值常量折叠。
纯 const 项目可以没有函数、节点和作用域；游离节点或作用域仍被拒绝。checker 必须
在调用 verify 之前实际检查顶层 const，不能借此跳过源程序检查。

已支持 Unit/bool/定宽整数/目标整数/f32/f64/string/非捕获函数值，局部绑定、读取、赋值、
复合赋值、一元/二元/短路运算、整数转换、直接及函数值调用、Block/If/For/Return/Break/Continue。
这里的支持指 HIR 表示与结构验证，不表示已经实现运行时数值计算或字符串管理。

支持数值/bool 的 var 整体 inout，支持在被调函数中读取、赋值和继续传入 inout。
参数 mode 必须与真实 Parameter AST 的标记一致。调用必须以 Place 表示 inout 实参；
Place 仍须满足同函数、可写、已初始化、作用域可见及类型相等条件。调用实参和 children
保持源码顺序；两个 inout 实参不能指向同一 SymbolId，其它实参的整个表达式树也不能
读取或借用该绑定。可接受的存储只有无引用位置的平凡局部绑定和已独占的 inout 参数；
不据此推断未知对象图或生成 noalias 属性。

未实现的泛型/聚合、错误效果与 defer、ARC/Vec、move、捕获闭包、外部绑定均拒绝进入
Verified。Function Type 可驻留 mode/throws 以固定身份，但标量验证阶段只接受 ordinary
和数值/bool inout 模式，不接受 throws。普通参数重赋值的源语言规则仍待用户决定，
当前 verifier 保持它只读；checker 应为这个未定操作报告未支持。string 仅支持值、绑定、普通参数/返回，尚无字符串
运算。未知成员、Error、NeutralBracket 等必须由 checker 拒绝或表示为 Unsupported；
验证器同时拒绝来自恢复 AST/Error/NeutralBracket 的正常节点。

本验证器不做源语言类型推断、名称查找、泛型检查、所有权分析或 AST 到 HIR 的转换。
例如 checker 必须先检查负数常量和字面量上下文，再生成确定类型与位模式。后端只读取
节点与签名，不需要重新从 AST 判断类型、标签或跳转目标。

## CMake 接入（已由主代理接入）

```cmake
add_library(lang_hir STATIC src/hir/hir.cpp)
target_link_libraries(lang_hir PUBLIC lang_modules)
target_compile_features(lang_hir PUBLIC cxx_std_20)
add_executable(lang_hir_type_test tests/hir/type_test.cpp)
target_link_libraries(lang_hir_type_test PRIVATE lang_hir)
add_test(NAME hir.types COMMAND lang_hir_type_test)
add_executable(lang_hir_test tests/hir/hir_test.cpp)
target_link_libraries(lang_hir_test PRIVATE lang_hir)
add_test(NAME hir.scalar COMMAND lang_hir_test)
```

生产 HIR 对象只有 `validate_utf8` 这项 support 实现依赖；不调用模块加载器，不启动 Python，
不包含 LLVM/CPython 接口。独立测试脚本直接编译已有 support/lexer/parser/resolve 源文件，
用真实 parse/resolve 结果建立手写 HIR，不安装库，也不运行 Python。

## 本机验证

运行入口（任意工作目录可用）：

```sh
CXX=g++ tests/hir/run.sh -O0 -g
CXX=g++ tests/hir/run.sh -O2 -DNDEBUG
CXX=clang++ tests/hir/run.sh -O2 -DNDEBUG
ASAN_OPTIONS=detect_leaks=0 CXX=clang++ tests/hir/run.sh -O1 -g -DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer
```

测试检查有效函数、直接/函数值调用、命名参数反序匹配、循环初始化变量、跳转、类型驻留、
74 项损坏、取消、保留可写 builder/项目/AST 引用后结果不变、确定 dump、四种 backend/字宽
组合和 Isize/Usize 32/64 位范围差异。测试使用运行时 require，不依赖 assert。

2026-11-03 实测结果：

| 检查 | 编译器/参数 | 结果 |
| --- | --- | --- |
| 类型表和 HIR 两个测试程序 | GNU 16.2.1，`-O0 -g` | passed，退出 0 |
| 类型表和 HIR 两个测试程序 | GNU 16.2.1，`-O2 -DNDEBUG` | passed，退出 0 |
| 类型表和 HIR 两个测试程序 | Clang 23.1.3，`-O2 -DNDEBUG` | passed，退出 0 |
| 类型表和 HIR 两个测试程序 | Clang 23.1.3，ASan/UBSan，`-DNDEBUG`，`detect_leaks=0` | passed，退出 0 |
| 默认 LeakSanitizer 检查 | 环境不允许其 ptrace 操作，第一次 sanitizer 执行因此失败 | not_run，未声称无内存泄漏 |
| HIR 对象未定义符号检查 | `nm -uC /tmp/language-t04-hir-final.o` | passed，除标准库/C++运行库外仅依赖 `lang::validate_utf8`，无 LLVM/Python 符号 |
| 接入主工程 CMake/CTest | 主代理接入，GNU Release 与 Clang22 ASan/UBSan | passed，各 14/14 组测试通过 |

两测试的标准输出分别为：

```text
type tests passed: canonical scalars/signatures/modes/throws/nominal identity, invalid type shapes
HIR passed: valid function/direct+value calls/loop, 74 corruptions, cancellation, immutable snapshot, deterministic dump
```

## inout 扩展实测

2026-11-03，T04 checker 分工只修改本实现和指定测试，没有修改公共头。
新增正例包括可写 var、被调函数内参数赋值、直接调用、函数值调用、两个不同绑定的
inout 调用，以及把 inout 参数继续传入 inout；同一有效 inout HIR 分别按 native/python
和 32/64 位四种配置验证。新增 14 个 inout 损坏案例检查声明 mode 不符、字面量/Read 冒充
Place、只读绑定、错误类型/模式、children 顺序、错误作用域/所属函数、同绑定普通读取
（含嵌套表达式）、非平凡参数及重复 inout。另有真实 parse/resolve 的无限循环正例：
`for var i i32 = 0; true; i += 1 { continue; break; }`，其不可达 break 不应造成缺失
return 诊断；把 continue 改为 Noop 后会正常退出循环，第 15 个新增损坏案例必须拒绝。
补充测试从真实 parse/resolve 结果建立 HIR，覆盖 `for g(); false; g() {}` 的表达式初始化、
纯 const 项目、`if true { return 1; }`、`if false ... else ...`，以及常量 false 分支中的
break 不导致外层无限循环正常退出；改变成 true 分支则必须要求 return。空函数列表中
的游离节点/作用域和 false 条件无 else 的缺失 return 也分别拒绝。

独立编译命令：

```sh
g++ -std=c++20 -O2 -DNDEBUG -Iinclude tests/hir/hir_test.cpp src/hir/hir.cpp src/support/source.cpp src/syntax/lexer.cpp src/syntax/parser.cpp src/modules/resolve.cpp -o /tmp/language-t04-hir-inout-release
/tmp/language-t04-hir-inout-release
```

结果：passed，退出 0。另用 `-O2 -DNDEBUG` 编译运行本测试和主目录发布的
`tests/hir/type_test.cpp`，均 passed、退出 0；Clang 的
`-O1 -g -DNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer` 与
`ASAN_OPTIONS=detect_leaks=0` 检查本测试也 passed、退出 0。
测试检查使用 require，NDEBUG 下仍执行。

公共头的数据结构/API 保持不变。主代理已更新两处注释：Place 可用于赋值
或 inout 实参；For 的 init 列表包含 ExpressionStatement。

## 与 checker 的交接

本次 checker 已从真实名称检查结果生成 ordinary scalar 和数值/bool 整体 inout，
并调用本验证器；具体范围、CMake 接入与运行记录见 `tests/sema/CHECKER.md`。
同类型形参使用声明顺序，实参使用源码顺序，Read/Place/direct callee 使用名称阶段选定的
SymbolId。验证器检查内部类型、结构和来源区间，不证明每项 AST 到 HIR 的转换都等价。
常量整数转换越界由 checker 静态拒绝，动态 IntegerConvert 保留运行时 Panic 检查。

无捕获 FunctionLiteral 已使用 resolver 发布的独立函数 SymbolId；HirFunction 来源
只能是真实 Function 或 FunctionLiteral，不伪造外层函数声明。普通参数重新赋值的规则
仍未确定，checker 对该操作报告未支持。
拥有对象、复杂借用、聚合/泛型、throws/defer 和外部调用均在进入 verifier 前明确拒绝。
标成 writable 的 let 和 recovered AstFile 的拒绝测试已经通过，不再列作待修复项。
