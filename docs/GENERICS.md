# T07 泛型与聚合类型

契约 compiler-dev/1。当前实现检查值struct/enum、泛型函数/类型、单一名义trait、显式impl和静态方法选择。新增后端执行单独检查，不由静态通过推定。

`sema::check_types`返回有类型的HirModule；`hir::verify_types`检查类型、声明、作用域、实例身份和控制流。这个结果不能直接交给后端。`sema::check_project`继续通过唯一的`hir::verify`发布只读VerifiedHirModule；发布前还检查当前后端已支持的操作。合法但后端尚未实现的操作返回E_CAPABILITY_UNAVAILABLE，损坏HIR返回E_IR_INVALID。

HIR沿用唯一TypeContext、SymbolId和节点arena。名义类型按声明身份和类型实参驻留；声明字段、enum变体、trait/impl、模板和具体实例均有独立记录。原NodeKind 0至23不变，新节点追加。模板内的已绑定TypeParameter有真实声明；具体函数中不允许残留它、未消解调用或ConstraintCall。Source可以指向原NeutralBracket AST，具体HIR必须已经确定含义。

模板体在声明约束下检查，包括未调用模板。同一声明与规范类型实参在一次检查中只产生一个实例；每个实例的函数、参数、局部、模式、嵌套函数字面量、作用域和节点分别分配身份，保留原AST来源。实例映射的左侧必须是原模板实际拥有的符号；复制品不能冒充来源。输入ResolvedProject不变，发布时再次深复制。

支持构造实参按源码顺序求值、字段读写及let值字段拒写、函数字段调用、enum载荷绑定/通配与穷尽性。跨包私有字段构造和私有固有方法拒绝，公开trait接口可以调用其实现。完整限定枚举模式和括号/函数类型实参保留名称、kind和可见性检查。

函数实例预算为128个实例、48层展开、200000个复制节点，超限E_INSTANTIATION_LIMIT。有限值布局只沿真正占据值存储的字段检查；函数签名和未参与值字段的类型参数不展开。原类型表持续增长反例现可有限完成，普通80层结构链接受，真正无限值布局拒绝。

验证器另检查所有泛型实参TypeId及所属模板、名义声明字段/变体/载荷完整性与声明顺序、泛型参数的真实来源及顺序。它不重新证明每条经过转换的表达式与Source文本等价。

## 验证

27个独立源项目（13个正例、14个反例）、完整未改的05_traits样例、7组初版问题修复、实例来源损坏和新增结构损坏测试均从真实源码进入HIR。主代理另跑14个固定源码探针、8个损坏HIR探针；与初版相比均得到预期结果。泛型函数含函数类型参数的误拒已由主代理补充未实例化和实例化两种检查。

标量泛型已接入两后端，7个公共项目分别经正式build、native O0/O2和Python3.12.15运行。
非零main返回按普通返回处理。两后端各有3个附加项目，主代理交叉重编译这6份源码，
结果也一致，共13份相同源码完成两后端比较。

平凡值struct/enum已在两个后端通过原13个正例及值复制、字段赋值、函数值字段和
switch/for补充程序。主代理GNU Debug、GNU Release、Clang22 ASan/UBSan
完整CTest各26/26通过，无排除项；关闭LLVM的前端构建21/21通过。
原13个项目在三种构建中完成native O0/O2与Python比较，共78项；另将两个后端
各自测试中的34份补充源码交叉编译，新增56次执行，结果一致。
常量字符串随后合入时，完整测试增至三种构建各28/28，关闭LLVM22/22；
原13个项目继续逐项比较通过。位置式枚举载荷构造现已支持：按声明顺序绑定，
允许位置参数在前、命名参数在后的组合；struct仍要求命名字段。变体后的类型实参
如`E.A[i64](42)`明确报E_CALL_ARGUMENT，泛型参数应写在`E[T]`上。
新共享源码在三种构建下的native O0/O2与Python共9次执行通过，输入文件哈希一致。
根代理另检查6种非法变体后缀，check/新目录build/已有目录build共108次均报告
正确源码位置且保留已有输出；新增15个损坏HIR继续由验证器拒绝。
旧Python分类失败日志仅作为修正前记录保留。

资源限制测试在普通构建使用384MiB虚拟地址限制、3秒CPU和10秒wall超时。ASan自身保留大块虚拟地址，因此其测试只保留CPU和wall限制；未运行LeakSanitizer。ASan覆盖本项目代码，不涵盖预编译LLVM与生成程序。这里是上述历史测试的范围；本次新增检查的泄漏检测结果见下节。

## 普通泛型函数的 move 参数

无 trait 约束的自由泛型函数直接调用现已支持 `move` 形参，例如 `func relay[T](value move T) T { return move value; }`。已有绑定必须显式写 `relay(move values)`；临时值可按现有规则转交，具体类型来自显式实参或既有推断。已验证 Vec 整体移动、含 Vec 的拥有 struct、返回后别名共享，以及移动后的 var 重新初始化。模板不直接进入执行，具体实例和调用保留真实参数模式与 Move 来源。

实参仍按源码顺序求值，命名实参只改变形参对应关系。后续实参抛错时，已经移动的临时值在 catch 前清理，原绑定不恢复；尚未求值的 move 不执行。普通借用参数直接 move、漏写 move、重复 move、移动后读取、defer 读取已移动值仍拒绝。

这里新增的是直接调用；具体泛型函数值仍使用既有的标量调用处理。独立对比旧、新编译器确认，带 trait 约束的 move 泛型函数值有一条原先就接受的入口，本次保留其原行为，不把它记为新增或完整受约束调用支持。

这一批只增加自由泛型函数的直接 move 调用；后续固有方法和数字/bool 泛型 inout 的支持见下文。trait 约束泛型的直接 move 调用仍未开放。foreign 和动态 string 的 move 实例给出 `E_CAPABILITY_UNAVAILABLE`，损坏的参数模式、源码 Move、模板/实例关系仍由既有 HIR 检查拒绝。

合入 move 调用这一批时，主目录 GNU Debug/Release 和 Clang22 ASan+UBSan+LSan 的 LLVM 构建各44/44，关闭 LLVM 为33/33，共165项，无跳过。三个 LLVM 构建每套实际运行三份同源项目的 Native O0/O2 与 Python3.12.15，共9项；关闭 LLVM 不执行这9项。独立新增的嵌套泛型聚合抛错例在两端验证两个 Vec 按逆序各释放一次，Python Frame 无残留。泄漏检测覆盖本项目检查程序，不声称预编译 LLVM 或生成程序也全部经过 sanitizer 插桩。

## 普通泛型函数的 inout 参数

无 trait 约束的自由泛型函数现已支持数字/bool 的整体可写绑定，例如 `func swap[T](a inout T,b inout T){let tmp=a;a=b;b=tmp;}`。已验证 i32/bool/f32/f64、显式和推断实参、泛型之间转交、已有 inout 参数转交及具体函数值调用。返回值先保存，再执行 defer；随后修改调用者槽不会改变已返回的标量值。

let/const、普通参数、临时值、字段和索引不能经此入口作为可写整体绑定。重复借用同一槽、另一实参读取/移动该槽、嵌套调用再次访问同一槽均拒绝，不因命名参数重排而改变。不同绑定即使值相等仍接受。

有 trait 约束的泛型函数自身的 inout 参数仍未开放；固有方法数字/bool inout 见下文。普通参数只是接收或返回带 inout 的 callback 时，不按目标函数自身的 inout 处理，保留原有合法调用。嵌套 FunctionType、struct/enum callback 字段具体化后也检查 inout 数据类型：合法模板中的真实 T 可以保留，具体非 scalar 数据报源级 `E_CAPABILITY_UNAVAILABLE`，不能落成内部 HIR 错误。具体函数与实例不能残留 T，Place、模板来源和重复存储检查仍保留。

主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 的 LLVM 构建各45/45，关闭 LLVM 为33/33，共168项，无跳过。每个 LLVM 构建的新测试有5条记录，其中2条为check，3条为Native O0/O2和Python3.12.15实际执行。独立组合程序另验证先取得inout后的后参抛错、callee自身抛错与defer恢复、return先复制再清理、循环continue/break，以及不同事件值区分的命名实参源码顺序；Python23个Frame无残留。scalar inout没有动态begin/end-borrow调用，空borrow列表不被记成观察到借用登记次数。

## 固有方法的 move 参数

非泛型、无 trait 的固有 impl 现已支持非 receiver 的 move 参数。实例方法的 self 必须是 ordinary 参数，只借用调用者的值；无 self 的静态方法通过类型名调用。结构体、枚举和已支持的标量 impl 使用相同的选择和参数检查。参数数据沿用现有64位标量、Vec、有限拥有 struct/enum；已有绑定需显式 move，临时值可以直接转交。

枚举类型上的静态方法现可调用。成员名对应真实变体时仍先按变体构造，构造参数不合要求时不会改选同名方法；其余名称进入已有方法选择。私有方法继续拒绝。此修复没有新增重载规则或 HIR 字段。

receiver 先求值且只求值一次，命名实参仍按书写顺序求值。ordinary self 与后面的 move 实参引用同一绑定时拒绝；另一个拥有副本即使与 self 共享内部 Vec，也可移动。receiver 或后续实参抛错时，尚未执行的 move 不发生，已转交的临时值按现有顺序清理；callee 抛错不重复释放参数，返回值先保存再执行 defer。

move/inout self、trait/约束方法 inout、带 trait 约束的泛型方法 move、trait 方法 move、绑定方法值逃逸、部分字段 move、foreign 和动态 string move 继续拒绝。本次未改公共 HIR、ABI 或运行库。

主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 的 LLVM 构建各46/46，关闭 LLVM 为33/33，共171项，无跳过。每个 LLVM 构建新增方法测试包含6个同源项目的 Native O0/O2/Python3.12.15 共18次实际执行；另12条为check、2条为拒绝构建。独立程序补查了求值顺序、错误清理、实际地址复用，以及枚举本身作为静态方法目标。生成 Native 程序和预编译 LLVM 未声称全部经过 sanitizer 插桩。

## 固有 impl 内的泛型方法 move

非泛型、无 trait 的固有 impl 中，方法自身现可带一个或多个无 trait 约束的类型参数，例如 `func take[T](self Self,value move T)T{return move value;}`；无 self 的静态方法也支持。显式和推断类型实参、多类型参数、方法之间及与自由泛型函数之间转交，均使用已有实例化过程。枚举本身可作为静态泛型方法目标，真实变体仍优先构造。

实例按原方法 SymbolId 和具体类型实参缓存；同名但属于不同 impl 的方法分别生成实例，相同组合只生成一次。ordinary self 类型保留为所属 impl 的固定类型，move 参数和 Move.binding 指向具体实例重命名后的符号。具体调用不保留类型参数或 ConstraintCall。静态声明中的普通 `self T` 参数不会因 T 后来取 impl 类型就改认成隐藏 receiver。普通 callback 内的合法 inout 继续接受，固有方法自身的数字/bool inout 见下文。

trait 方法、带 trait 约束的泛型方法及前节列出的其他限制仍未开放；没有新增 HIR 字段或后端接口。原方法运行例扩充为泛型形式，仍保留非泛型拥有副本、标量实例和静态 move 的执行检查。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各46/46，关闭 LLVM 为33/33，共171项，无跳过。每个 LLVM 构建的7个方法项目共21次正式 Native O0/O2/Python 执行，另14条为check、2条为拒绝构建。独立新程序与49d4428的完整非泛型positive另有12次执行及12次释放观察，参数顺序、enum静态目标、throws/defer及共享返回值符合预期。

## 泛型固有 impl 的 move

无 trait 且 impl 类型参数均无约束时，方法自身没有额外类型参数的 move 调用现已支持，例如 `impl[T] Box[T]{func take(self Self,value move T)T{return move value;}}`。ordinary self 实例调用和无 self 的 `Box[Vec[i32]].take_static(...)` 使用相同的 impl 参数替换；泛型枚举也可作为静态方法目标。

本节方法登记的泛型参数 SymbolId 及顺序等于所属 impl 的参数；方法另带类型参数时见下节。`Self` 完整替换为具体的 `Box[Vec[i32]]` 等类型，普通 Self 参数和返回值也经过相同替换。实例仍按原方法和完整 impl 类型实参缓存，相同组合复用，不同类型组合和方法分别记录；Move.binding 指向该实例重命名后的参数。没有新增公开的 impl 实例表示，也没有放宽 Source/HIR 检查。

impl 参数由 receiver 或类型名上的实参确定，本批方法没有另行提供的方法类型实参。trait 或有 trait 约束的 impl、move/inout self 继续拒绝；无约束固有方法的数字/bool inout 见下文。原非泛型 impl 中的无约束泛型方法、普通 callback、enum 变体优先及原有 move 数据限制保持。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各46/46，关闭 LLVM 为33/33，共171项，无跳过。每个 LLVM 配置的9个方法项目共27次正式生成执行，另18条为check、2条为拒绝构建。独立3项目另有9次执行及9次释放观察，覆盖两个Box具体类型、泛型enum静态目标、Self返回copy、共享及错误清理。

## impl 与方法各自带类型参数的 move

无 trait、两层类型参数均无约束时，现已支持 `impl[T] Box[T]{func take[U](self Self,value move U)U{return move value;}}`，以及无 self 的静态方法。impl 参数来自 receiver 或目标类型；方法上显式提供的类型实参只填写尚未确定的方法自身参数，也可从实参推断。两层均可有多个参数，T 与 U 可以分别传递不同的拥有值。

完整参数列表保留真实 impl SymbolId 的有序前缀，随后是登记时记录的方法参数。即使方法参数与 impl 参数同名，它们仍是不同声明。实例按原方法和完整 `[impl 类型实参, 方法类型实参]` 缓存；签名相同而完整组合不同的静态方法实例仍分开保存。`Self` 只用 impl 实参替换，具体 Move 使用重命名后的参数绑定，不保留 T、U 或模板调用。

ordinary self 借用、参数求值顺序、共享副本、throws/defer 和枚举变体优先规则保持。trait 或任一层带 trait 约束、move/inout self、绑定方法值及原有 move 数据限制仍拒绝；未改公共 HIR、后端接口或运行库。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各46/46，关闭 LLVM 为33/33，共171项，无跳过。每个 LLVM 配置的11个方法项目有33次正式 Native O0/O2/Python 执行，另22条check及2条拒绝构建。独立3个新项目另有9次执行、9次释放观察和4次同绑定反例检查，覆盖完整两层组合、Self共享副本、泛型enum静态方法及错误清理。生成 Native 程序和预编译 LLVM 未声称全部经过 sanitizer 插桩。

## 固有方法的数字/bool inout

无 trait、impl 和方法自身类型参数均无约束的固有方法，现在支持非 self 的数字/bool inout。非泛型、仅方法泛型、仅 impl 泛型及两层泛型均可直接调用；实例方法要求 ordinary self，静态方法没有真实 self。例如 `func replace[U](self Self,slot inout U,value U)U{let old=slot;slot=value;return old;}`。显式和推断类型参数、方法间及自由泛型转交沿用现有实例化；Self、完整实例键和 Place 使用原有检查。

inout 实参必须是整体可写绑定，数据类型具体化后只能是已支持的数字/bool。let、普通借用参数、字段、索引、临时值及同存储冲突继续拒绝。receiver 先求值一次，命名实参按书写顺序求值；callee 已写入的数值在 typed throw 后仍可见，后参抛错时 callee 不执行。返回值先保存再执行 defer，拥有 move 参数仍按原有顺序清理。

来源合格也必须检查完整嵌套类型图。ordinary callback 内的合法 inout 不算外围函数自身的 inout；具体非数字/bool 的 callback、返回类型及名义字段内 inout 仍报源级 `E_CAPABILITY_UNAVAILABLE`。trait 或任一层约束、实际 move/inout self、绑定或静态未绑定方法值继续拒绝；`check_function_value_inout` 保持原样，没有新增 HIR 字段、后端接口或运行库。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各47/47，关闭 LLVM 为33/33，共174项，无跳过，包含T10绑定和CLI测试。每个 LLVM 配置的新增2个方法 inout 项目有6次正式 Native O0/O2/Python 执行、6次私有观察、4次check及2次拒绝构建；旧方法 move 的11个项目仍有33次正式执行。独立2个新项目另有6次执行、6次观察和8次别名反例检查。标量 inout 没有动态 borrow 登记；生成 Native 程序和预编译 LLVM 未声称全部经过 sanitizer 插桩。

## 尚未实施

标量泛型及两后端平凡值聚合生成执行已通过。两个后端入口均调用共用检查，
只扫描具体函数及其使用类型；未实例化模板不进入生成过程。

泛型trait及trait泛型方法、嵌套变体模式、带trait约束的泛型方法及trait方法move、trait/约束方法inout，以及非scalar数据或有trait约束的泛型函数自身inout仍未实施。Vec与有限拥有聚合见[T09_EXECUTION.md](T09_EXECUTION.md)，完整ARC仍未完成；throws/defer见[T08_STATIC.md](T08_STATIC.md)，Python外部声明集见[T10_PYTHON.md](T10_PYTHON.md)。严格浮点执行已在两后端验证。普通参数重新赋值继续等待用户选择；初始化表达式读取同名外层绑定的规则已定案并保留。
