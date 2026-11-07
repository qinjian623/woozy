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

这一批增加无约束自由泛型函数的直接 move 调用；满足 trait 约束的直接调用、固有方法和数字/bool 泛型 inout 的后续支持见下文。foreign 和动态 string 的 move 实例给出 `E_CAPABILITY_UNAVAILABLE`，损坏的参数模式、源码 Move、模板/实例关系仍由既有 HIR 检查拒绝。

合入 move 调用这一批时，主目录 GNU Debug/Release 和 Clang22 ASan+UBSan+LSan 的 LLVM 构建各44/44，关闭 LLVM 为33/33，共165项，无跳过。三个 LLVM 构建每套实际运行三份同源项目的 Native O0/O2 与 Python3.12.15，共9项；关闭 LLVM 不执行这9项。独立新增的嵌套泛型聚合抛错例在两端验证两个 Vec 按逆序各释放一次，Python Frame 无残留。泄漏检测覆盖本项目检查程序，不声称预编译 LLVM 或生成程序也全部经过 sanitizer 插桩。

## 普通泛型函数的 inout 参数

无 trait 约束的自由泛型函数现已支持数字/bool 的整体可写绑定，例如 `func swap[T](a inout T,b inout T){let tmp=a;a=b;b=tmp;}`。已验证 i32/bool/f32/f64、显式和推断实参、泛型之间转交、已有 inout 参数转交及具体函数值调用。返回值先保存，再执行 defer；随后修改调用者槽不会改变已返回的标量值。

let/const、普通参数、临时值、字段和索引不能经此入口作为可写整体绑定。重复借用同一槽、另一实参读取/移动该槽、嵌套调用再次访问同一槽均拒绝，不因命名参数重排而改变。不同绑定即使值相等仍接受。

自由泛型函数现在也支持满足 trait 约束的数字/bool inout 及既有具体函数值，见下文；固有方法数字/bool inout 同样单列说明。普通参数只是接收或返回带 inout 的 callback 时，不按目标函数自身的 inout 处理，保留原有合法调用。嵌套 FunctionType、struct/enum callback 字段具体化后也检查 inout 数据类型：合法模板中的真实 T 可以保留，具体非 scalar 数据报源级 `E_CAPABILITY_UNAVAILABLE`，不能落成内部 HIR 错误。具体函数与实例不能残留 T，Place、模板来源和重复存储检查仍保留。

主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 的 LLVM 构建各45/45，关闭 LLVM 为33/33，共168项，无跳过。每个 LLVM 构建的新测试有5条记录，其中2条为check，3条为Native O0/O2和Python3.12.15实际执行。独立组合程序另验证先取得inout后的后参抛错、callee自身抛错与defer恢复、return先复制再清理、循环continue/break，以及不同事件值区分的命名实参源码顺序；Python23个Frame无残留。scalar inout没有动态begin/end-borrow调用，空borrow列表不被记成观察到借用登记次数。

## 固有方法的 move 参数

非泛型、无 trait 的固有 impl 现已支持非 receiver 的 move 参数。实例方法的 self 必须是 ordinary 参数，只借用调用者的值；无 self 的静态方法通过类型名调用。结构体、枚举和已支持的标量 impl 使用相同的选择和参数检查。参数数据沿用现有64位标量、Vec、有限拥有 struct/enum；已有绑定需显式 move，临时值可以直接转交。

枚举类型上的静态方法现可调用。成员名对应真实变体时仍先按变体构造，构造参数不合要求时不会改选同名方法；其余名称进入已有方法选择。私有方法继续拒绝。此修复没有新增重载规则或 HIR 字段。

receiver 先求值且只求值一次，命名实参仍按书写顺序求值。ordinary self 与后面的 move 实参引用同一绑定时拒绝；另一个拥有副本即使与 self 共享内部 Vec，也可移动。receiver 或后续实参抛错时，尚未执行的 move 不发生，已转交的临时值按现有顺序清理；callee 抛错不重复释放参数，返回值先保存再执行 defer。

move/inout self、绑定方法值逃逸、部分字段 move、foreign 和动态 string move 继续拒绝。受约束固有方法和 trait 非 self 参数的后续支持见下文。本次未改公共 HIR、ABI 或运行库。

主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 的 LLVM 构建各46/46，关闭 LLVM 为33/33，共171项，无跳过。每个 LLVM 构建新增方法测试包含6个同源项目的 Native O0/O2/Python3.12.15 共18次实际执行；另12条为check、2条为拒绝构建。独立程序补查了求值顺序、错误清理、实际地址复用，以及枚举本身作为静态方法目标。生成 Native 程序和预编译 LLVM 未声称全部经过 sanitizer 插桩。

## 固有 impl 内的泛型方法 move

非泛型、无 trait 的固有 impl 中，方法自身现可带一个或多个无 trait 约束的类型参数，例如 `func take[T](self Self,value move T)T{return move value;}`；无 self 的静态方法也支持。显式和推断类型实参、多类型参数、方法之间及与自由泛型函数之间转交，均使用已有实例化过程。枚举本身可作为静态泛型方法目标，真实变体仍优先构造。

实例按原方法 SymbolId 和具体类型实参缓存；同名但属于不同 impl 的方法分别生成实例，相同组合只生成一次。ordinary self 类型保留为所属 impl 的固定类型，move 参数和 Move.binding 指向具体实例重命名后的符号。具体调用不保留类型参数或 ConstraintCall。静态声明中的普通 `self T` 参数不会因 T 后来取 impl 类型就改认成隐藏 receiver。普通 callback 内的合法 inout 继续接受，固有方法自身的数字/bool inout 见下文。

前节列出的其他限制仍保留；受约束固有方法和 trait 非 self 参数的后续支持见下文；没有新增 HIR 字段或后端接口。原方法运行例扩充为泛型形式，仍保留非泛型拥有副本、标量实例和静态 move 的执行检查。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各46/46，关闭 LLVM 为33/33，共171项，无跳过。每个 LLVM 构建的7个方法项目共21次正式 Native O0/O2/Python 执行，另14条为check、2条为拒绝构建。独立新程序与49d4428的完整非泛型positive另有12次执行及12次释放观察，参数顺序、enum静态目标、throws/defer及共享返回值符合预期。

## 泛型固有 impl 的 move

无 trait 且 impl 类型参数均无约束时，方法自身没有额外类型参数的 move 调用现已支持，例如 `impl[T] Box[T]{func take(self Self,value move T)T{return move value;}}`。ordinary self 实例调用和无 self 的 `Box[Vec[i32]].take_static(...)` 使用相同的 impl 参数替换；泛型枚举也可作为静态方法目标。

本节方法登记的泛型参数 SymbolId 及顺序等于所属 impl 的参数；方法另带类型参数时见下节。`Self` 完整替换为具体的 `Box[Vec[i32]]` 等类型，普通 Self 参数和返回值也经过相同替换。实例仍按原方法和完整 impl 类型实参缓存，相同组合复用，不同类型组合和方法分别记录；Move.binding 指向该实例重命名后的参数。没有新增公开的 impl 实例表示，也没有放宽 Source/HIR 检查。

impl 参数由 receiver 或类型名上的实参确定，本批方法没有另行提供的方法类型实参。move/inout self 继续拒绝；受约束固有 impl 和 trait 非 self 参数的后续支持见下文；无约束固有方法的数字/bool inout 见下文。原非泛型 impl 中的无约束泛型方法、普通 callback、enum 变体优先及原有 move 数据限制保持。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各46/46，关闭 LLVM 为33/33，共171项，无跳过。每个 LLVM 配置的9个方法项目共27次正式生成执行，另18条为check、2条为拒绝构建。独立3项目另有9次执行及9次释放观察，覆盖两个Box具体类型、泛型enum静态目标、Self返回copy、共享及错误清理。

## impl 与方法各自带类型参数的 move

无 trait、两层类型参数均无约束时，现已支持 `impl[T] Box[T]{func take[U](self Self,value move U)U{return move value;}}`，以及无 self 的静态方法。impl 参数来自 receiver 或目标类型；方法上显式提供的类型实参只填写尚未确定的方法自身参数，也可从实参推断。两层均可有多个参数，T 与 U 可以分别传递不同的拥有值。

完整参数列表保留真实 impl SymbolId 的有序前缀，随后是登记时记录的方法参数。即使方法参数与 impl 参数同名，它们仍是不同声明。实例按原方法和完整 `[impl 类型实参, 方法类型实参]` 缓存；签名相同而完整组合不同的静态方法实例仍分开保存。`Self` 只用 impl 实参替换，具体 Move 使用重命名后的参数绑定，不保留 T、U 或模板调用。

ordinary self 借用、参数求值顺序、共享副本、throws/defer 和枚举变体优先规则保持。move/inout self、绑定方法值及原有 move 数据限制仍拒绝；trait 非 self 参数的后续支持见下文；任一层带 trait 约束的后续支持见下文；未改公共 HIR、后端接口或运行库。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各46/46，关闭 LLVM 为33/33，共171项，无跳过。每个 LLVM 配置的11个方法项目有33次正式 Native O0/O2/Python 执行，另22条check及2条拒绝构建。独立3个新项目另有9次执行、9次释放观察和4次同绑定反例检查，覆盖完整两层组合、Self共享副本、泛型enum静态方法及错误清理。生成 Native 程序和预编译 LLVM 未声称全部经过 sanitizer 插桩。

## 固有方法的数字/bool inout

无 trait、impl 和方法自身类型参数均无约束的固有方法，现在支持非 self 的数字/bool inout。非泛型、仅方法泛型、仅 impl 泛型及两层泛型均可直接调用；实例方法要求 ordinary self，静态方法没有真实 self。例如 `func replace[U](self Self,slot inout U,value U)U{let old=slot;slot=value;return old;}`。显式和推断类型参数、方法间及自由泛型转交沿用现有实例化；Self、完整实例键和 Place 使用原有检查。

inout 实参必须是整体可写绑定，数据类型具体化后只能是已支持的数字/bool。let、普通借用参数、字段、索引、临时值及同存储冲突继续拒绝。receiver 先求值一次，命名实参按书写顺序求值；callee 已写入的数值在 typed throw 后仍可见，后参抛错时 callee 不执行。返回值先保存再执行 defer，拥有 move 参数仍按原有顺序清理。

来源合格也必须检查完整嵌套类型图。ordinary callback 内的合法 inout 不算外围函数自身的 inout；具体非数字/bool 的 callback、返回类型及名义字段内 inout 仍报源级 `E_CAPABILITY_UNAVAILABLE`。实际 move/inout self、绑定或静态未绑定方法值继续拒绝；受约束固有方法和 trait 非 self 参数见下文；`check_function_value_inout` 保持原样，没有新增 HIR 字段、后端接口或运行库。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各47/47，关闭 LLVM 为33/33，共174项，无跳过，包含T10绑定和CLI测试。每个 LLVM 配置的新增2个方法 inout 项目有6次正式 Native O0/O2/Python 执行、6次私有观察、4次check及2次拒绝构建；旧方法 move 的11个项目仍有33次正式执行。独立2个新项目另有6次执行、6次观察和8次别名反例检查。标量 inout 没有动态 borrow 登记；生成 Native 程序和预编译 LLVM 未声称全部经过 sanitizer 插桩。

## 带 trait 约束的自由泛型直接 move

自由泛型函数的实际类型参数满足既有 trait 约束时，现在可以直接传递 move 参数，例如 `func read[T Value](value move T)i32{return value.get();}`。显式与推断类型参数、多个参数各自带单个 trait 约束、外层同约束泛型转交均沿用现有约束检查。缺少或错误的 impl 继续报类型错误；不会因声明带约束就认定实际类型满足。

模板内的 ordinary trait receiver 调用保留 ConstraintCall，具体化时按所需 trait 和真实 impl 选择方法，再转成具体直接调用。同名方法属于不同 trait 时分别选择；实例仍按 origin 和完整类型实参缓存，Move、参数与局部使用重命名后的符号，具体签名、节点及调用不保留类型参数或 ConstraintCall。

此次只增加自由泛型直接 move 调用，保留旧受约束 move 函数值。实际 self move/inout 仍拒绝；受约束固有方法和 trait 非 self 参数见下文；自由泛型 inout 的后续支持见下文。ordinary trait 方法借用拥有参数是原有合法操作。后参抛错、callee throws/defer、返回先保存和共享副本继续使用原有清理。没有改 HIR、后端、运行库或数据类型限制。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各47/47，关闭 LLVM 为33/33，共174项，无跳过，包含两项T10。每个 LLVM 配置的自由泛型 move 测试执行5个源码项目的 Native O0/O2/Python，共15次正式执行及15次私有观察；另有4次check和2次拒绝构建。结果文件的17条runs不含单列的4条checks。旧方法move与数字/bool inout测试保持；生成 Native 程序和预编译 LLVM 未声称全部有sanitizer插桩。

## 带 trait 约束的自由泛型数字/bool inout

自由泛型函数的实际类型满足既有 trait 约束时，数字/bool 整体绑定 inout 现在支持直接调用和具体函数值。显式后缀如 `swap[i32]`、已有 Name 期望函数类型、函数值参数/返回值及间接调用沿用原有入口；没有新增推断规则。参数仍必须是可写整体绑定，具体类型受原有数字/bool 检查限制。

三处来源判断使用同一 free generic 身份：直接调用、具体函数值检查及实例化。实际 trait 约束和完整嵌套类型图均继续检查，ordinary callback 内的 inout 不改变外围函数自身模式。具体 FunctionValue 指向已实例化函数；Place、参数与局部使用重命名后的符号，同类型组合复用实例，ConstraintCall 按真实 trait/impl 转成具体调用。满足 trait 的 struct/enum、Vec、string、foreign 自身 inout 仍拒绝，真实 self 的 move/inout 仍未开放。trait 非 self 参数的后续支持见下文。受约束固有方法的后续支持见下文。

写回、typed throw、返回先保存再 defer、后参失败不进入 callee、命名实参源码顺序及同存储冲突沿用现有规则。旧 free move 的拒绝构建例原先使用受约束标量 inout，现已合法，因此换成满足 trait 的非 scalar inout；原始标量拒绝记录保留在前一阶段资料中。

Native 曾在 try 成功 return 后的不可达拥有调用处报内部错误；该旧错误已修复，见下节。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各48/48，关闭 LLVM 为33/33，共177项，无跳过，包含两项T10。新runtime每个 LLVM 配置有4次check、6次正式 Native O0/O2/Python 执行、6次私有观察和2次拒绝构建；另在主目录原2份before源码上完成4次check、6次正式执行，源码SHA保持。独立14输入/4HIR检查及2个新程序的6次执行、6次观察、8次拒绝检查通过。未将空borrow列表算作动态登记，生成Native和预编译LLVM未声称全部有sanitizer插桩。

## Native 不可达块与局部槽

`Lower::take` 删除无可达前驱的块和 handler 后，现在一并清理只属于这些被删控制流、没有存活引用的非参数局部槽，再重排保留的 Native LocalSlotId。参数、存活 handler 和真实指令引用全部保留；constant、call、end_borrow 等默认 slot0 不当作实际引用。HIR 的 OwnerRef、SymbolId、ValueId 和参数位置不重排。

原 before 源码未改，Native O0/O2 和 Python 均实际返回规定的7，不可达的 mix/[55] 没有执行或分配Vec。死catch、双return join、非尾部删槽、零活槽和真实throws后继续尾部调用均有检查。没有删掉或放宽验证器：可达拥有槽的initializer、清理或槽身份被破坏时仍报E_IR_INVALID。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各49/49，关闭LLVM为33/33，共180项，无跳过，包含两项T10。每LLVM配置新增5个源码项目的15次正式执行、10次Native私有观察及3个坏IR拒绝控制；原样before另以正式CLI构建并执行Native O0/O2/Python，均返回7。独立6组Source/API检查和23次坏IR修改通过，另4个自有案例的12次正式执行/12次观察通过。47个原保护文件及2个验证器保持原字节；生成Native与预编译LLVM未声称全部有sanitizer插桩。

## 受约束固有方法的 move 与数字/bool inout

非 trait 的固有方法，现在允许 impl 类型参数、方法自身类型参数或两者带既有的单个 trait 约束。实例方法保持 ordinary self，无 self 的静态方法通过类型名调用。实际类型必须满足所声明的 trait；模板中的 ordinary trait receiver 调用按真实 trait impl 具体化，同名方法不会互相替代。

非 self 的 move 沿用已有拥有 struct/enum 和 Vec 数据范围；inout 仍只支持数字/bool 整体可写绑定。显式方法类型实参填写方法自身参数，也可沿用已有推断。实例按原方法与完整 `[impl 实参, 方法实参]` 保存；仅在签名中未使用的类型参数也参与区分。Self、Move、Place、参数与局部的来源检查保持。

receiver 求值一次，命名实参按源码顺序执行；throws 后的写回、return 先保存再 defer、拥有副本共享和释放使用原有规则。缺少 trait impl、同存储冲突、移动后读取及非法 inout 继续拒绝。实际 self move/inout 和方法值仍未支持；trait 非 self 参数及 ConstraintCall 的后续支持见下节；未改公共 HIR、后端或运行库。

主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各49/49，关闭 LLVM 为33/33，共180项，无跳过，包含两项T10。每个 LLVM 配置新增两例合计4次check、6次正式 Native O0/O2/Python 执行和6次私有观察；方法 move 全部12例、方法 inout 全部3例保持通过。原两份 before 源码另完成4次check和6次正式执行，源码SHA未改。49个原保护文件保持原字节。生成Native和预编译LLVM未声称全部经过sanitizer插桩。

## trait 方法的非 self move 与数字/bool inout

非泛型 trait、方法自身不带类型参数且首参为 `self Self` 时，现已支持非 self 的 move 和形参声明为具体数字/bool 的 inout。具体 receiver 及 `[T Trait]` 模板内调用都支持，trait impl 可以带既有类型参数。move 继续使用已有 Vec、有限拥有 struct/enum 和标量范围，包括另一个 `value move Self`；inout 仍要求整体可写绑定。

模板中的 ConstraintCall 保留真实 trait 和 prototype，具体化后只允许转成该 trait 对应的唯一 impl 方法。同名同签名的另一 trait 方法不能替代。receiver 先求值且一次，命名实参按书写顺序；throws 后写回、return 先保存再 defer、共享副本和释放顺序沿用现有规则。

HIR 对 Call 和 ConstraintCall 使用相同的嵌套存储冲突检查，并检查 receiver0、标签、参数模式和索引、inout Place 及实例目标。初次实现误把新模式的首参名称限制用于所有 ConstraintCall，导致旧 `ordinary(owner Self)` 调用报内部错误；修正后，已调用和未实例化的旧普通模板都保留合法行为，新模式的窄来源检查仍保留。

trait 静态调用、泛型 trait、方法自身泛型、方法值、实际 self move/inout 仍未支持；顶层 `slot inout Self` 的后续支持见下文。缺 move、重复或移动后读取、同绑定 receiver 与 move/inout、重复或嵌套 inout、非数字/bool 或非整体绑定 inout 继续拒绝。未修改公共 HIR 表示、后端或运行库。

修正后主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各49/49，关闭 LLVM 为33/33，共180项，无跳过，包含两项T10。两份新增项目每个配置合计4次check、6次正式执行和6次观察；原两份before与F1原样源码另完成6次check、9次正式执行，源码SHA未改。49个原保护文件保持原字节。旧ordinary调用和未调用模板已补回归测试；初次失败及修正记录保留。生成Native和预编译LLVM未声称全部经过sanitizer插桩。

## trait 顶层参数 inout Self

trait 方法现在可声明非 self 的顶层参数 `slot inout Self`，具体 Self 仍限已支持的数字/bool。实例方法、`[T Trait]` 内的 ConstraintCall、显式或推断类型及同约束转交都使用原有调用和实例化过程。actual self 仍为普通借用；receiver 与 slot 引用同一存储时拒绝，不同绑定即使数值相等也可传入。

声明检查以参数显式传入所属 trait 的真实 Self SymbolId，只在该 prototype 的非 self 顶层 inout 位置接受它。Self 不冒充方法泛型，不进入实例键或 active_params；原有 signature_parameters 保存和恢复。嵌套 Self callback 的后续支持见下节。具体 impl 即使未调用，也必须通过完整类型图检查；POD struct/enum、Vec、string、foreign、函数、Unit 和 Box[T] 等非数字/bool Self 继续报 `E_CAPABILITY_UNAVAILABLE`。

HIR 只增加一种依赖 Place：原 prototype 形参确为该 trait Self，替换后正好等于当前真实约束 receiver T。receiver0、标签、模式和索引、重复/嵌套存储冲突、真实 trait/impl 目标、Move/Place/局部重命名继续检查，具体 Call 仍只收数字/bool inout。旧普通 owner 首参和固定数字/bool trait 方法保留原行为，未改公共 HIR 表示、后端或运行库。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各49/49，关闭 LLVM 为33/33，共180项，无跳过，包含两项T10。新trait-self例每套LLVM配置为2次check、3次正式执行、3次观察；完整方法inout runner共5份源码均通过。原两份before及旧F1另有6次check、9次正式执行，原源码SHA保持，49个保护文件未改。生成Native和预编译LLVM未声称全部经过sanitizer插桩。

## trait 参数和返回类型中的 Self callback

trait 普通参数和返回函数类型现在可以包含 `func(inout Self, Self) Self`。callback 没有 receiver，第0位参数也可为 inout Self；外围方法的实际 self 仍是 ordinary。具体 Self 限已有数字/bool，支持传入已有 free 函数值、实际调用，以及 factory 返回函数值后间接调用。函数返回函数类型继续按语法加括号，内部 callback throws 与外围方法 throws 分开声明。

真实 trait Self 上下文经 parse_type 递归传入括号和函数类型，只在有普通 Self receiver 的 prototype 中生效，不使用全局开关，不把 Self 加进方法泛型。普通 owner 首参保留合法行为；callback 内的 inout 不改变外围方法自身模式。已有固定数字/bool callback 即使使用拥有 struct receiver，也保持可用；只有实际依赖 Self 的 inout 数据要求具体化为数字/bool，未调用的非数字/bool impl 也报源码 E_CAP。

函数值的类型与签名相同，不代表实现函数可以互换。HIR 对解析为函数声明的名称引用和函数字面量增加源码身份检查，要求目标是名称解析时确定的函数或其实例；原有 trait/impl 选择、参数顺序、Place、存储冲突和实例检查保持。 同一 origin 下，类型/签名相同且记录合法的实例互换仍可能被接受；验证器不逐处证明 HIR 与源码类型实参等价。未增加公共 HIR 字段、后端或运行库接口。方法值、捕获闭包、actual self move/inout、trait-static 的 Self callback、泛型 trait 和非数字/bool inout 仍未开放；trait 方法自身泛型见下文。

本批主目录 GNU Debug/Release、Clang22 ASan+UBSan+LSan 各49/49，关闭 LLVM 为33/33，共180项，无跳过，包含两项T10。新callback项目每套LLVM配置为2次check、3次正式执行、3次观察；方法inout runner全部6份源码通过。原两before与旧F1另6次check、9次正式执行，源码SHA未改，49个保护文件保持原字节。生成Native和预编译LLVM未声称全部经过sanitizer插桩。

## 泛型 trait 的完整应用身份

泛型 trait 现支持多个无约束类型参数、显式 impl 和显式 bound，例如
`trait Carry[A,B]{func choose(owner Self,left A,right B)B;}` 与
`impl[T] Carry[i32,T] for Box[T]`。bound、impl 头与 ConstraintCall 都保存真实
trait 声明加有序类型实参；同一个 Self 可以有 `Convert[i32,bool]` 与
`Convert[bool,i32]` 两个不同完整应用，方法签名相同也不会合并。

值调用、具体 `Type.method(...)` 和受约束 `T.method(...)` 均按完整应用选择
prototype/impl；无足够限定的具体直接调用（同 Self 多个应用）仍报源级歧义，
不按预期返回类型或实参猜另一项 trait。impl 参数即使只由 trait 应用确定
（phantom，不在 Self 或方法签名中）也进入 `[impl 实参..., 方法实参...]` 实例键，
具体调用仍是普通直接 `Call`。

重叠判断联合求解 Self 与全部有序实参，共享同一替换表：`Match[X,bool] for
Box[X]` 与 `Match[i32,Y] for Box[bool]` 不重叠，改第二项为 `Match[bool,Y]`
后以 X=Y=bool 的实际见证拒绝。trait 实参次序是身份的一部分，错序 bound 不能
满足 impl；impl 实参数量与 trait 声明不等在声明处拒绝。

## 泛型 trait 的 ordinary 方法自身泛型

泛型 trait 的 ordinary 方法现在可以声明一个或多个自身类型参数，每个参数可无约束或带一个已有 trait bound。值调用、具体 `Type.method(...)`、受约束的 `T.method(...)`、显式与既有推断、泛型 impl、跨包公开 trait、泛型转交及非捕获 lambda 沿用同一实例化过程。impl 可按位置改名方法自身参数，声明与 impl 的参数个数、标签、返回/throws 以及完整 bound 必须对应；未调用的原型也检查。

方法自身 bound 可以引用所属 trait 的真实 Self 和有序 trait 实参，例如 `trait Use[A]{func read[U Tag[A]](owner Self,x U)i32;}` 与 `func own[U Tag[Self]]`。这些名称属于原型的环境，不进入方法的 own 参数列表或实例键。原型与 impl 先建立 `Self`、trait 实参和全部 own 参数的位置替换，再比较 bound 与签名；纯 alpha 改名保持身份，真正不同的 bound 仍报源级类型错误。

实例仍按真实 impl 方法 SymbolId 与完整 `[impl 实参..., 方法实参...]` 缓存。仅由 trait 应用确定的 impl 参数和签名中未出现的 phantom own 参数都进入键；同一组合重复调用去重，不同组合即使签名相同也不合并。具体 `ConstraintCall` 从真实 Source 后缀和所属实例恢复方法实参，替换 Self、trait 实参及全部 own 参数后再验证 bound，最终仍是普通直接 `Call`。

本批只开放 ordinary 组合：方法自身泛型与顶层 move/inout、依赖 Self/trait 参数/own 参数的 inout callback、实际 self move/inout、方法值及捕获闭包仍报源级 `E_CAPABILITY_UNAVAILABLE`。泛型 trait 的参数自身仍不加 bound；非泛型 trait 与固有方法已有的泛型、move/inout、Self callback 和 POD 整体 inout 行为不变。验证器从真实 Source 重建 trait 参数、own 参数、bound、调用后缀与实例键；伪造或换序 trait 应用、改写完整实例键、换用同签名 phantom 实例、破坏 clone 重命名，以及成功重算 ownership 后的坏图都报 `E_IR_INVALID`。

本批新 fixture 与 `run_generic_traits.py` 在两后端执行原三个 before 组合、参数 bound、Self bound、四组完整 phantom 实例和去重、显式/推断、跨包、转交、非捕获 lambda、Vec copy/返回、命名参数顺序及 typed-error/defer 事件；拒绝组合验证 fresh 构建不产物且 stale 旧字节保持。未新增公共 HIR/NodeKind、ABI、schema 或后端接口。

修正版在 nominal 字段、enum 载荷、返回函数类型和 `throws` 类型完成后，用同一 helper 再次以真实 Self、全部 trait 参数和全部 own 参数检查完整 callback 图（含递归 nominal 与嵌套函数）。因此 `Holder[U]`、`Holder[A]`、`Holder[Self]` 及无 own 的旧 `Holder[Self]` 隐藏回调均在 Source 报 `E_CAPABILITY_UNAVAILABLE`，不再落到验证器的内部 `E_IR_INVALID`；固定 `Holder[i32]` 与非泛型 trait 的既有固定 callback/ordinary 模式保持接受。

验证器从真实 Source 重建 trait 参数列表、bound/impl 的有序应用和方法签名；
同包内后声明的 nominal 在 bound 中与前置声明等价，不按未登记名称猜测 foreign。
trait 原型（含泛型 trait）的 own bound 可引用该 trait 的真实 Self 和有序 trait 参数；原型与 impl 方法
own bound 先建立 Self、trait 实参和全部方法 own 参数的位置替换，再比较有序
trait 实参，纯改名不影响身份，真正不同的 bound 仍报 Source 错误。
伪造、删除、换序或互换 TraitRef（含两个同签名 phantom 应用）、伪造 phantom
实例键，以及重算 ownership 后再验证的坏图都报 `E_IR_INVALID`。外层冻结的
原样 Source 覆盖同 Self 双 phantom、联合重叠/非重叠、跨包 ordinary Vec 与
仅由 trait 应用推断的 impl 参数；新 fixture 与 runner 在两后端实际执行。

## 尚未实施

标量泛型及两后端平凡值聚合生成执行已通过。两个后端入口均调用共用检查，
只扫描具体函数及其使用类型；未实例化模板不进入生成过程。

泛型 trait 的 move/inout 方法组合、实际 self move/inout，以及
Vec/string/foreign 及含拥有字段聚合的 inout 仍未实施；有限平凡聚合整体 inout 已支持。Vec与有限拥有聚合见[T09_EXECUTION.md](T09_EXECUTION.md)，完整ARC仍未完成；throws/defer见[T08_STATIC.md](T08_STATIC.md)，Python外部声明集见[T10_PYTHON.md](T10_PYTHON.md)。严格浮点执行已在两后端验证。普通参数的只读规则已在 SEMANTICS §7.2 定案；对普通参数名重新赋值按源码类型错误 `E_TYPE_MISMATCH` 拒绝，需改值时建立 var 局部变量。初始化表达式读取同名外层绑定的规则已定案并保留。

## 递归枚举模式

枚举载荷可以继续使用限定变体模式，也可绑定或写通配符；支持多层、多载荷、泛型及跨包公开枚举。分支按源码顺序匹配，所有内部标签匹配成功后才建立该分支绑定，没有 fallthrough。两个后端均支持已有 POD 值和有限拥有聚合；计算产生的 switch 值只求值一次，POD 结果按普通 full-expression 清理，拥有的结果登记为真实 subject temporary，保留到所选分支的 defer、模式绑定和局部值清理完成之后，再按正常退出、return、throw、break、continue 的退出计划释放；早于外层作用域清理。内部 catch 不离开分支时继续持有；subject 求值失败不清理未产生的结果；嵌套 switch 按内到外释放；再次循环迭代每轮新建一次 subject。named 拥有 switch 保持原有借用与绑定 copy 行为不变。

覆盖检查同时考虑所有载荷列。同一外层变体可由多个不同内部组合覆盖；部分重叠但仍匹配新值的后续分支合法，被前面多个分支联合覆盖的分支拒绝。缺少任何内部组合均报不穷尽。普通值或 Vec 位于前面的载荷列时，继续检查后面的枚举列；原空枚举规则保持。共享分析有工作量与深度限制，无法完成时拒绝编译，不将未知结果当成穷尽。

HIR 验证每层的源码、真实枚举/变体、类型、载荷顺序和绑定作用域，再重算覆盖。显式模式类型实参按所属函数实例替换，包括泛型非捕获 lambda；不直接将模板 T 与具体 i32 比较。Native 只在父标签匹配后读取活动载荷，所有测试成功后才复制拥有绑定；Python 使用递归短路判断。退出计划包括同一外层变体的后续内部组合，以及其 return、throw、break、continue 和 defer。

这项支持不包含部分字段 move、literal/guard/or/range 模式或 switch 表达式。失败分支没有绑定 copy/store 的检查来自生成 CFG 和源码，零存活观察不能单独证明平衡 copy/release 从未发生。T07/T09 仍为 in_progress。

## trait 类型名调用

非泛型 trait 的 `Type.method(...)` 和 `[T Trait]` 下的 `T.method(...)` 显式提供全部声明参数，包括普通首参 Self；也支持零参数工厂。类型限定符只用于选择方法，不产生运行时接收者。旧 `value.method(...)` 保留既有接收者规则，具体类型和值调用中已有的 move/inout 支持保持。

新 T 入口支持顶层 ordinary 参数、Self 参数和返回、现有泛型 impl、跨包公开 trait、泛型间调用和非捕获泛型 lambda，以及已有普通 callback、typed throws 和有限拥有值。命名实参按书写顺序求值，首个实参抛错后不执行后续实参；复制、返回共享和清理仍采用原规则。泛型调用依据 T 实际声明的 trait 选择 prototype 和 impl，不按同名或相同签名替换目标。具体枚举的真实变体仍优先构造；从 T 的 trait 方法具体化时保持原 trait 选择。

HIR 由真实源码的调用形式检查有无值接收者，核对显式参数 0、标签、类型、参数顺序、实例来源与完整类型实参；没有参数时仍验证真实约束、目标和返回/错误类型。impl 内的 Self 按实现类型解析，保留 `Self.helper(...)` 和括号形式；它不会因此变成有 trait bound 的模板参数。后端只接收具体普通 Call，不保留 ConstraintCall 或未替换的 T。

Type/T 入口现支持普通参数、非 self move 和非 self 顶层数字/bool inout；具体限制见末尾一节。已有固定数字/bool callback 内的 inout 不改变外围参数模式。泛型 trait、trait 名直接调用、方法值、捕获闭包和实际 self move/inout 仍未支持；trait 方法自身泛型见下节。T07/T09 仍为 in_progress。

## trait 方法自身的泛型参数

非泛型 trait 的方法现在可以声明多个类型参数，每个参数可无约束或带一个已有 trait 约束。例如 `trait Echo { func echo[U](owner Self, value U) U; }`，impl 可把 U 改名为 V；参数位置、数量、约束、普通参数标签、返回和 throws 类型必须对应。impl 不能增加、删除或改变约束，未调用的声明也检查。

支持 `value.echo[i32](7)`、`Type.echo[i32](value, 7)` 和 `[T Echo]` 下的 `T.echo[i32](value, 7)`，也沿用已有的参数和返回类型推断。Type/T 调用仍显式提供全部普通参数，包括首参 Self；值调用仍使用原有接收者规则。跨包公开声明、泛型转交、非捕获泛型 lambda、普通 callback、typed throws、Vec 和有限拥有聚合沿用已有执行与清理规则。

方法实例按真实 impl 方法 SymbolId 及完整 `[impl 类型实参..., 方法类型实参...]` 保存。签名中没有出现的类型参数必须显式填写；HIR 从该调用的真实源码及所属实例还原它，不能用相同签名猜测。One/Two 等签名相同、完整实参不同的实例分别保存，调换显式调用或具体化 ConstraintCall 的目标会被拒绝。trait 原型只有声明，没有虚构的可执行模板体；公共 HIR 和后端接口保持原样。

泛型 trait 方法的顶层参数支持 ordinary、非 self move 和非 self 数字/bool inout。依赖方法类型参数或 Self 的 inout callback 仍报 `E_CAPABILITY_UNAVAILABLE`。旧非泛型 trait 的数字/bool Self callback、固定 callback 和 `Self.helper` 保持原规则。泛型 trait 本身、方法值、捕获闭包、实际 self move/inout 和完整 ARC 尚未完成；T07/T09 仍为 in_progress。

## trait 非 self move 与类型名调用

非泛型 trait 的非 self move 现在支持无方法类型参数、多方法类型参数、已有单个 trait 约束和泛型 impl，覆盖值调用、具体 Type 调用和受约束的 T 调用。例如 `func take[U](owner Self, value move U) U` 可通过 `x.take[Vec[i32]](move v)` 或 `T.take[Vec[i32]](x, move v)` 调用。普通首参 Self 可命名为 self 或 owner，仍按原规则借用；Type/T 显式提供全部声明参数，不生成运行时接收者。

无接收者的静态方法也可声明 move 参数。若原型是 `func echo[U](self move U) U`，首参仍是方法类型参数 U；即使调用为 `Host.echo[Host](move h)`，也不因 U 的具体类型等于 Self 而增加接收者。原型首参直接为 trait Self 且采用 move/inout 的情况，包括 owner 标签，本批继续报 `E_CAPABILITY_UNAVAILABLE`。固有静态方法保持已有声明判断，合法的 `Host.consume(value move Host)` 和 `i32.adjust(slot inout i32)` 不会因首参类型与实现类型相同而被改判。

已有绑定仍须显式 move；临时值、按书写顺序求值的命名实参、普通共享副本、typed error、return 和 defer 沿用原规则。后续实参失败时，已经 move 的临时按逆序销毁，原绑定不会恢复可读；若更早的实参失败，尚未求值的 move 不发生。catch 同时接收 move 前的实参错误和 callee 接管后的同型错误时，所有权状态会合并，不能据此假定源绑定仍可读。实例仍按真实方法及完整 impl/方法实参保存，phantom 参数、真实 Source、Move.binding 与清理记录继续验证。

非 self 顶层数字/bool inout 见下节。依赖方法类型参数或 Self 的 inout callback 继续检查整个函数签名、替换后的 struct 字段及 enum 载荷；旧 ordinary-only Type/T 调用保留原行为。公共 HIR、NodeKind、后端接口和运行库未变。泛型 trait 本身、方法值、捕获闭包、实际 self move/inout、部分字段 move、foreign/动态 string move 和完整 ARC 仍未完成。T07/T09 保持 in_progress。

## trait 方法的非 self 顶层数字/bool inout

非泛型 trait 的方法支持非 self 的顶层数字/bool inout，包括固定标量、方法自身的类型参数 U/V，以及非首位的原型 Self。方法可以属于泛型 impl；实际调用时，inout 的数据必须替换为数字/bool。值调用、具体 `Type.method(...)` 和受约束的 `T.method(...)` 均支持这些形式，方法没有自身类型参数时也一样。

普通首参的真实 Self 可以命名为 self 或 owner，按声明类型判断。Type/T 调用须显式提供全部声明参数，包括普通首参 Self，不生成值接收者。原型首位直接为 trait Self 的 move/inout 仍不支持，即使标签为 owner；首位为方法 U 且名称为 self 时仍是静态参数，即使实例中的 U 恰好等于 Self。

inout 必须传入可写的整体绑定；let、普通参数、临时值、字段、索引，以及同存储冲突、重复或嵌套借用继续拒绝。值接收者先求值一次，命名实参按书写顺序求值。后续实参抛错时不进入 callee；callee 抛错前的写回仍可见；返回值先保存，再执行 defer。move 实参及局部拥有值沿用原释放顺序。

方法自身泛型中的 inout callback 如果依赖方法类型参数或 trait Self，继续报 `E_CAPABILITY_UNAVAILABLE`。新开放的 Type/T 顶层 inout 调用还检查完整嵌套签名，包括参数、返回、throws 和替换后的 struct/enum 字段及载荷。已有值调用，以及旧 ordinary-only Type/T 调用中的 Self callback 返回和 Holder 参数保持原行为；原先拒绝的直接 callback 参数仍拒绝。固定数字/bool callback 不受此限制。

HIR 沿用 Call/ConstraintCall、Place、Move、实例和 Source 记录。验证器核对真实 trait/impl、调用形式、参数顺序/标签/模式、整体 Place 及完整实例类型实参；方法类型参数保留为真实 TypeParameter，不替换成 Any。递归函数字段即使与外围签名使用同一 Function TypeId，也须检查其 callback 参数。

测试包含 Native O0/O2 与 Python3.12.15 实际执行、写回和求值顺序、throw/defer、move 清理、同签名 phantom 实例、坏 HIR 及重算所有权后的拒绝。`tests/generics/run_method_inout.py` 还检查拒绝构建不会产生新程序或覆盖旧文件。旧合法控制与两个原始漏检源码分别保存在 `trait-ordinary-*-control`、`trait-function-cycle-reject` 和 `trait-type-inout-return-reject` 项目中。

方法值、捕获闭包、实际 self move/inout、字段/索引 inout、foreign 和动态 string 仍未开放。平凡聚合整体 inout 见下节。T07/T09 保持 in_progress。

## 平凡 struct/enum 的整体 inout

数字/bool 之外的有限平凡聚合现在可以作为整体可写绑定传入 `inout`。顶层数据仍只接受数字/bool 或 `NominalStruct`/`NominalEnum`；struct 字段和所有 enum 变体载荷递归允许数字/bool、`Unit`、非捕获函数值、空 struct 和无载荷 enum。顶层 `Unit`/函数值、`Vec`、`string`、foreign/未知类型、含拥有字段的聚合和无限值布局继续拒绝。该判断读取实际实例、声明字段及替换后的字段类型，不按名称或仅按 `needs_drop` 猜测；`ValueOps.pod_inout` 同步表示同一顶层规则，但 HIR 独立验证不依赖它。

自由/泛型函数、固有方法、trait 非 self 参数、值/具体 Type/T 调用、函数值和既有泛型转交共用该数据规则。`slot inout Self` 在具体或泛型 impl 中按替换后的聚合检查；trait 原型首位真实 Self 的 move/inout 仍不支持。依赖方法自身类型参数或 trait Self 的 callback inout 数据继续限制为数字/bool，普通 callback 中与 Self 无关的平凡聚合参数合法。捕获闭包、方法值、非平凡拥有值 inout 和字段/索引 inout 未开放。

被调用方可以整体替换 struct、改变 enum 活动变体、修改值字段或把同一整体绑定继续转交给另一个 inout 参数。调用仍要求整体可写 `var` 绑定；let、普通参数、临时值、字段和索引不能作为 inout。重复或嵌套借用、另一实参读取/移动同一绑定或其子字段继续拒绝；不同平坦 POD 绑定各自复制。receiver 只求值一次，命名实参按源码顺序，后续实参抛错不进入 callee，callee 的 typed throw 写回可见，return 先保存返回值再执行 defer。

Native 以 LLVM 聚合值和调用者存储指针降低；Python 的 writable `Cell` 只承载可写绑定，普通读取和返回仍复制逻辑值。零尺寸/空 struct、Unit 字段和固定函数值字段都进入真实执行检查，函数字段被替换后实际调用；测试不比较 padding，只检查有效字段和活动变体。

新增 `tests/generics/aggregate-inout/` 覆盖嵌套 POD、padding、enum 活动变体、显式/推断泛型、函数值、固有和 trait 方法、ConcreteType/T 调用、整体替换、字段修改、copy/return、throw/defer/求值顺序，以及 owning 聚合和别名拒绝。`tests/generics/run_aggregate_inout.py` 在两后端检查和执行，并对拒绝项目验证 fresh 构建不产物、已有目录不被覆盖。C++ 静态检查另覆盖 `ValueOps.pod_inout`、顶层 Unit/Function 拒绝、坏 HIR 字段和把 Place 改成 Read 后重算 ownership 仍拒绝。旧 constrained move/inout 拒绝项目因聚合 inout 变为合法，保留原输入未改，测试改用新增的含 Vec 字段版本作为拒绝控制。四配置的最终数量以本次交付日志为准。

修正版进一步区分了三个来源问题：同一个泛型 nominal 声明的有限嵌套（如 `Box[Box[i32]]`）按具体类型实参的严格缩小判断值存储循环，不再仅因声明相同就当作无限布局；函数签名中的 nominal 回指不参与值布局环判断。trait callback 的 Self 依赖只从真实原型中的 trait Self SymbolId 判断，固定 `Pair` callback 即使 impl Self 也是 `Pair` 或 `i32` 也不被当作 Self 依赖；真正的 Self callback 仍保留既有数字/bool 限制。`slot inout Wrap[Self]` 在原型检查时把真实 Self 放入类型环境，具体 impl 仍按实际替换后的聚合检查，拥有字段的 Self 继续报 Source `E_CAPABILITY_UNAVAILABLE`。这些限制同时由独立 HIR 验证器按同一 raw prototype/Self 关系重查。
