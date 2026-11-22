# Selfhost sema 共享状态与分工

本文件固定 `src/sema/check.cpp` 的移植分工与包内接口。`selfhost/src/wzcc/sema/{state,errors,clone,common}.wz` 存放共享状态、错误类型、完整试探复制和已实测的公共助手；`run.wz` 与 `check.wz` 已实现构造、执行次序和公开阶段函数。A/B 算法和 hirverify 等依赖尚未联合验证，因此不能把 common 或独立助手测试通过说成完整 Checker 可编译或自举完成。clone 依赖真实的 hirclone，目前也尚未被 sema_common 测试覆盖。

所有代码保持在同一个 `package sema` 下，A/B 按函数拆到不同文件、同包直接互调；不要拆成会循环导入的类型/表达式包，也不要写算法 stub 让半成品 Checker 冒充运行成功。分工上，Root 维护共享状态、common/errors/clone 以及 facade/run；A 负责声明、类型、泛型与实例化（含 parameter_type，避免 common 去调尚未实现的 parse_type）；B 负责表达式、调用、语句、函数正文和效果记录。

下面是 C++ 方法的完整归属，冒号后为原 check.cpp 行号，同名重载各自列出。file 和 type 在 Wz 中改名为 parsed_file 与 lookup_type。Root 负责的 Checker 构造、run 和 facade 代码已写入 run/check，其余 Source 与公共助手在 common。A/B 必须补齐各自列表里的函数，不能因为已有独立算法包就省掉 Checker 层的衔接逻辑。

Root：

```text
file:129, ast:132, child:133, at:137, token:141
first:144, has_async_marker:148, source:154, reject:155, reject_source:168
unavailable:181, private_type_leak:184, definition:197, occurrence:198, symbol:204
type:207, integer:414, signed_:419, width:423, modulus:424
wrap:425, range:432, bits:438, node:447, int_literal:455
scalar:462, mode:468, expression_mode:487, argument_mode:499, Checker:6687
run:6703
```

A：

```text
trait_key:208, trait_prototype:214, trait_decl:221, symbol_from_module:227, std_iterator_trait:235
std_option:241, iterator_candidate:254, mentions_self:280, require_object_safe:307, check_trait_application:361
check_trait_parameter_bounds:376, parameter_type:502, template_inout_type:508, owning_inout_type:522, owning_inout_reason:531
trait_self_type:543, impl_of:548, impl_method:555, declared_self:556, inherent_self_source:568
inherent_method_source:590, inherent_method_route:606, method_own_parameters:623, trait_impl_signature_matches:637, trait_impl_inout_source:693
trait_impl_move_source:744, constraint_method_source:783, constraint_method_move_source:825, constraint_static_method_move_source:868, constraint_static_method_source:914
free_generic_template:960, check_inout_graph:965, check_trait_application_data:1092, check_generic_trait_method_modes:1137, check_function_value_inout:1155
check_method_value_modes:1167, generics:1189, nominal:1219, nominal_subs:1222, field_type:1229
parse_type:1232, signature:1426, contains_parameter:2865, may_contain_weak:2891, validate_weak_type:2924
validate_weak_type:2927, validate_weak_types_before_publish:2994, infer:3270, try_infer:3301, finish_arguments:3309
satisfies:3324, check_constraints:3389, select_method:3409, declared_foreign_type:3623, c_type:3626
c_decl:3636, c_callable:3639, check_c_callback:3646, self_parameter:5497, register_function:5503
overlap:5599, impls_overlap:5606, declarations:5624, duplicate_symbol:6169, explicit_method_type_refs:6190
constraint_method_arguments:6217, transform_node:6287, instantiate:6412
```

B：

```text
literal_integer:1465, round_ratio:1484, round_fraction:1490, decode_ieee:1505, float_bits:1528
binary_token:1532, binary_op:1535, match:1547, convert_to:1556, contextual:1600
list_hint:1620, fold:1650, float_fold:1710, expression:1849, place_root:2454
check_inout_place_type:2465, place_path:2477, paths_overlap:2498, container_path:2524, container_overlap:2536
places_overlap:2554, may_hold_shared:2565, collect_alias_risks:2616, collect_place_index_risks:2645, collect_access_paths:2660
collect_place_index_paths:2688, place:2702, capture_uses:3032, fresh_capture_symbol:3056, make_function_value:3073
bound_method_value:3118, trait_object_call:3156, type_expression:3245, find_field:3258, method_value:3463
constructor:3536, foreign_owner:3592, foreign_members:3596, foreign_property:3607, foreign_indexer:3613
foreign_slice:3618, foreign_place_kind:3663, peek_type:3670, system_call:3771, foreign_call:3826
c_assignable:3888, c_foreign_call:3897, call:3967, check_call_borrows:4218, typed_call:4278
scalar_call:4511, evaluated_constant:4695, constant:4710, check_public_constant:4746, new_scope:4757
flow:4769, block:4903, pattern:4921, switch_statement:4988, statement:5045
annotate:5896, check_body:6116, hidden_local:6177
```

建议 A 写 types/generics/declarations，B 写 expressions/calls/statements/effects，都放在同包的不同文件里。B 的代码量比 A 大，但不要为了凑平均行数把 typed_call 与 expression/place/borrow 拆开。跨边调用主要是 A→B 的 instantiate 调 annotate/flow，以及 B→A 的 parse_type、inference、方法来源/约束、Weak 和 foreign 类型包装。两边可以独立编辑，但完整运行验证必须等两组算法都到齐。

现有算法包和 Checker 的职责边界不同：constant 的缓存、递归保护和临时 m.nodes 截断，pattern 把绑定写入 locals，foreign_call 的候选试探，以及 annotate 生成 defer/effect/exit，这些仍留在 Checker 中。Checker 会调用 constant/pattern/foreign/valueops/ownership，但不代表那些算法包已经代替 Checker 完成了上述工作。

C++ 字段映射：stop（C++ stop_token）在当前 Wz facade 中没有对应物，不要加 bool 或假 token；取消 API 尚未移植，最终的 CLI SIGINT 与临时输出清理由 Root 后续验证。owned 与 m.resolved 合并为 state.m.resolved 这一份唯一可变副本，所有 synthetic/capture/instance symbol 都往这里追加。i64 类型缓存字段改名 i64_type，Value.string 改名 string_value，ClosureInfo.move 改名 is_move，都属于适应 Wz 语法的拼写迁移。

C++ foreign_call:3873 的 trial 不能写成 `var trial=state` 后直接检查，必须调用 `var trial=clone_checker(state)`。C++ 默认 Checker 拷贝本身就会深复制 vector/map/TypeContext，只有 shared_ptr 管理的 owned 和 m.resolved 共享，因此紧接着专门复制 owned 并重设指针。Wz 中 Vec 传值会共享底层存储，所以除了 m，还要复制所有 locals/constants/tables/caches。hirclone 会复制整份 resolved/project，比 C++ 共享不可变 project 的 shared_ptr 更彻底，但会原样保留 ID 和数据值，不重新解析、不重排、也不新生成 Symbol/Type/HirNode ID。试探成功后，不能把 trial 的 TypeId/node/symbol 缓存直接并回原 state，仍按 C++ 做法选定 binding 后在原 state 上重新 build；试探失败时则不能对原 state 产生任何副作用。

具名 Vec entry 的更新约定一并冻结：

1. 模拟 std::map：按真实 key 查找，upsert 替换唯一 entry，不追加重复项；遍历时按 C++ key 升序（NodeKey 按 module,file,node；SymbolId 按数值；TraitKey 按 actual,declaration,args 的词法序；InstanceKey 按 origin,args）。不能拿插入顺序代替 bodies、generic_params、trait_signature_context 等 map 的遍历顺序。
2. 模拟 std::set 的 Vec 保持无重复，需要迭代输出时按数值或键升序；而 loops、break_targets、current_captures、参数和类型实参原本就是有序 vector，禁止当作 set 排序。
3. usize 类型的 Symbol/Module ID 中 0 都可能有效，只有 TypeId 0 是无效值。空 optional 必须用 Option.None。Ref.file 表示模块内文件序号。
4. 从 entries 读出含 Vec 的记录后，内部 Vec 仍共享句柄，不能像 C++ 值副本那样原地修改。凡是要拿读取结果去替换类型实参、inference sub、renamed captures 或在 trial 后修改的地方，都要先复制对应的 Vec，尤其是 select_method 的候选局部 sub、instantiate 的临时 HirNode/Pattern/Constant，以及 m.functions 的 concrete 快照，避免改坏原记录。try_infer 与 C++ 一致，返回 false 时不保证回滚传入的 sub，应在 C++ 原先创建或复制局部 sub 的位置做隔离，不要额外发明统一回滚。
5. 初始 scalar intern 顺序保持为 Unit、bool、i64；definitions 按 symbol 遍历 emplace，occurrences 按输入顺序覆盖相同 key。declarations 的多轮处理、nominal_fields_ready 切换、完整类型检查后的 weak_validated 缓存策略和 run 顺序不能因分文件而改动。constants 的临时 m.nodes 截断仍按原 begin 执行，不重排其他表，也不回收已分配的 TypeId。

Checker 共有 45 个实际持久字段，其中 30 个是 Vec 字段。state.wz 保留当前的 async_function/current_result/current_async_error、闭包来源与绑定、trait 上下文与约束缓存、常量与精确整数、模板/实例缓存以及 Weak 完整性标志。

声明处理必须保持原有顺序：nominal 形状 → trait arity → nominal bounds → trait Self/prototype → fields/payload → nominal_fields_ready → impl 匹配 → trait/inout 完整复检 → 普通函数与 const 内 Fn 注册。run 按常量、pub 常量接口、按 SymbolId 排序的 bodies、concrete 快照 transform、nominal 约束、Weak/无限布局、ownership、verify 的顺序执行；typed_call 保留 template symbol/type_args，不能提前改变实例化时序。signature_parameters 的临时环境和 weak_validated 的完成条件也保持不变。

下面是包内函数声明清单，不是可直接编译的空实现；接口省去了 C++ 默认参数，调用方须传全参数。全部算法都在 `package sema` 下，共用 state.wz 和 errors.wz。有状态检查统一传 `state inout Checker`，Error 为同包 errors.wz 定义的错误类型。返回的 AST、Type 和声明记录视为只读快照，调用方不得修改其中的嵌套 Vec。

跨文件接口中，A→B 只需要 `flow` 和 `annotate`，下面也一并列出 Root facade 调用的入口。`trait_index`、`impl_index` 和 `method_index` 都是 m 里的数组下标，不是 SymbolId。泛型实例化留在类型/泛型一侧，不要随意迁到独立包。

Root 公共访问/构造/错误（CPP:129-207、414-500；只列有跨文件调用的部分）：

```wz
func parsed_file(state inout Checker, r model.NodeRef) model.ParseResult throws Error
func ast(state inout Checker, r model.NodeRef) model.AstNode throws Error
func child(state inout Checker, r model.NodeRef, index usize) model.NodeRef throws Error
func at(r model.NodeRef, node usize) model.NodeRef
func token(state inout Checker, r model.NodeRef, offset usize) string throws Error
func first(state inout Checker, r model.NodeRef) string throws Error
func has_async_marker(state inout Checker, r model.NodeRef) bool throws Error
func source(state inout Checker, r model.NodeRef) hir.HirSource throws Error
func definition(state inout Checker, r model.NodeRef) usize throws Error
func occurrence(state inout Checker, r model.NodeRef) option.Option[usize] throws Error
func symbol(state inout Checker, id usize) model.Symbol throws Error
func lookup_type(state inout Checker, id usize) model.Type throws Error
func intern_type(state inout Checker, value model.Type) usize throws Error
func substitute_type(state inout Checker, id usize, substitutions Vec[model.Substitution]) usize throws Error
func scalar(state inout Checker, kind model.TypeKind, bits u32) usize throws Error
func integer(state inout Checker, id usize) bool throws Error
func signed_(state inout Checker, id usize) bool throws Error
func width(state inout Checker, id usize) u32 throws Error
func modulus(state inout Checker, id usize) bigint.BigInt throws Error
func wrap(state inout Checker, value bigint.BigInt, id usize) bigint.BigInt throws Error
func range(state inout Checker, r model.NodeRef, value bigint.BigInt, id usize) throws Error
func bits(state inout Checker, value bigint.BigInt, id usize) u64 throws Error
func int_literal(state inout Checker, r model.NodeRef, value bigint.BigInt, type_id usize) Expr throws Error
func mode(state inout Checker, r model.NodeRef) model.ParamMode throws Error
func expression_mode(state inout Checker, input model.NodeRef) ArgumentMode throws Error
func argument_mode(state inout Checker, r model.NodeRef) ArgumentMode throws Error
func node(state inout Checker, r model.NodeRef, type_id usize, data hir.NodeData, children Vec[usize]) usize throws Error
func reject(state inout Checker, r model.NodeRef, message string, code string) Error throws Error
func reject_source(location hir.HirSource, message string, code string) Error
func unavailable(state inout Checker, r model.NodeRef, feature string) Error throws Error
func private_type_leak(state inout Checker, r model.NodeRef, private_type usize) Error throws Error
func clone_checker(state Checker) Checker throws Error
```

错误构造函数直接返回 Error，调用处必须写 `throw reject(...)` 或 `throw unavailable(...)`，不能靠调用一个返回 Unit 且 throws 的辅助函数来假定 never-return，因为 Wz 没有 C++ `[[noreturn]]` 那样的跨函数控制流承诺。`reject` 等函数签名里的 throws Error 只用于处理构造诊断时遇到非法 Ref 的 Support 故障，正常路径返回的是构造好的 Rejected 值。

其中，C++ 的 `file()` 改名为 parsed_file，返回 ParseResult，且必须通过 `project.files[project.modules[r.module].files[r.file]]` 取值，不能拿 r.file 直接索引全项目文件列表。`type()` 改名为 lookup_type 以避开关键字；类型表 intern/substitute 产生的 types.Error 先转成 SupportError，再包装为 Error.Support。`node()` 保持按调用次序 append，不要按 AST 编号重新排序。

B / Root → A 的类型与约束助手（CPP:208-413、507-1464、2865-3020、3269-3457）：

```wz
func parameter_type(state inout Checker, symbol usize) usize throws Error
func trait_prototype(state inout Checker, method usize) option.Option[MethodPrototypeRef] throws Error
func trait_decl(state inout Checker, symbol usize) option.Option[usize] throws Error
func std_iterator_trait(state inout Checker) option.Option[usize] throws Error
func std_option(state inout Checker, type_id usize) bool throws Error
func iterator_candidate(state inout Checker, r model.NodeRef, actual usize, trait_index usize) option.Option[IteratorCandidate] throws Error
func require_object_safe(state inout Checker, r model.NodeRef, trait_symbol usize) throws Error
func check_trait_parameter_bounds(state inout Checker, r model.NodeRef, trait_ref hir.TraitRef, application usize, depth usize) throws Error
func impl_of(state inout Checker, target usize) option.Option[usize] throws Error
func declared_self(state inout Checker, target usize) bool throws Error
func inherent_self_source(state inout Checker, target usize) bool throws Error
func inherent_method_source(state inout Checker, target usize) bool throws Error
func inherent_method_route(state inout Checker, target usize, receiver bool, signature usize) bool throws Error
func method_own_parameters(state inout Checker, method usize) Vec[hir.GenericParam] throws Error
func trait_impl_inout_source(state inout Checker, target usize, receiver bool) option.Option[TraitMethodSource] throws Error
func trait_impl_move_source(state inout Checker, target usize, receiver bool) option.Option[TraitMethodSource] throws Error
func constraint_method_source(state inout Checker, target usize, trait_ref hir.TraitRef, receiver usize) option.Option[MethodPrototypeRef] throws Error
func constraint_method_move_source(state inout Checker, target usize, trait_ref hir.TraitRef, receiver usize) option.Option[MethodPrototypeRef] throws Error
func constraint_static_method_source(state inout Checker, target usize, trait_ref hir.TraitRef, receiver usize) option.Option[MethodPrototypeRef] throws Error
func constraint_static_method_move_source(state inout Checker, target usize, trait_ref hir.TraitRef, receiver usize) option.Option[MethodPrototypeRef] throws Error
func free_generic_template(state inout Checker, target usize) bool throws Error
func check_inout_graph(state inout Checker, r model.NodeRef, root usize, allow_template bool, trait_self option.Option[usize], trait_self_callback bool) bool throws Error
func check_function_value_inout(state inout Checker, r model.NodeRef, target usize, signature usize) throws Error
func check_method_value_modes(state inout Checker, r model.NodeRef, target usize, signature usize, required option.Option[hir.TraitRef], qualifier option.Option[usize]) throws Error
func parse_type(state inout Checker, r model.NodeRef, trait_self option.Option[usize], trait_self_callback bool) usize throws Error
func nominal(state inout Checker, type_id usize) hir.NominalDecl throws Error
func nominal_subs(state inout Checker, type_id usize) Vec[model.Substitution] throws Error
func field_type(state inout Checker, type_id usize, field hir.FieldDecl) usize throws Error
func contains_parameter(state inout Checker, type_id usize) bool throws Error
func validate_weak_type(state inout Checker, r model.NodeRef, type_id usize) throws Error
func validate_weak_at(state inout Checker, location hir.HirSource, type_id usize) throws Error
func infer(state inout Checker, r model.NodeRef, pattern usize, actual usize, substitutions inout Vec[model.Substitution]) throws Error
func try_infer(state inout Checker, r model.NodeRef, pattern usize, actual usize, substitutions inout Vec[model.Substitution]) bool throws Error
func finish_arguments(state inout Checker, r model.NodeRef, parameters Vec[hir.GenericParam], substitutions inout Vec[model.Substitution]) Vec[usize] throws Error
func satisfies(state inout Checker, r model.NodeRef, actual usize, trait_ref hir.TraitRef) bool throws Error
func check_constraints(state inout Checker, r model.NodeRef, parameters Vec[hir.GenericParam], arguments Vec[usize], substitutions option.Option[Vec[model.Substitution]]) throws Error
func select_method(state inout Checker, r model.NodeRef, receiver usize, name string, required option.Option[hir.TraitRef]) Selection throws Error
```

`try_infer` 返回 false 时只吞掉 C++ 对应的 Rejected 错误，不会吞掉 Error.Support。上面 infer 的第三、四个参数是 TypeId，不要误传 hir.Pattern。A 内部的 `signature`、`generics`、`check_trait_application`、`check_trait_application_data`、弱类型递归助手和 register_function 等不再列为跨文件 API，仍可在 A 自己的文件之间直接互调。

B → A 的 foreign 类型薄包装；它们只把既有 foreign 算法包接到 Checker（CPP:3623-3662），foreign call 构造仍归 B：

```wz
func declared_foreign_type(state inout Checker, name string) usize throws Error
func c_type(state inout Checker, ref model.CTypeRef) usize throws Error
func c_decl(state inout Checker, id u32) model.CTypeDecl throws Error
func c_callable(state inout Checker, id u32) model.CCallableDecl throws Error
func check_c_callback(state inout Checker, r model.NodeRef, function_type usize, callback_type u32) throws Error
```

A ↔ B 的正文后处理以及 Root 顺序驱动入口（CPP:4695-4756、4769-4897、5496-5893、5896-6168、6287-6684）：

```wz
// B owns these.
func constant(state inout Checker, symbol usize) Constant throws Error
func check_public_constant(state inout Checker, symbol usize) throws Error
func flow(state inout Checker, id usize, depth usize) Flow throws Error
func annotate(state inout Checker, function hir.HirFunction) throws Error
func check_body(state inout Checker, id usize) throws Error
// A owns these; expression.convert_to can call instantiate.
func declarations(state inout Checker) throws Error
func transform_node(state inout Checker, id usize, depth usize, instance_sub option.Option[Vec[model.Substitution]]) throws Error
func instantiate(state inout Checker, origin usize, arguments Vec[usize], use model.NodeRef) usize throws Error
func validate_weak_types_before_publish(state inout Checker) throws Error
```

B 内部的 expression、call、typed_call、place、pattern、switch_statement、block、statement 和 foreign_call 不必另设模块接口，同包直接互调即可。Root run 按 CPP:6703-6796 的顺序依次执行：declarations → 完整 bounds 重检 → 顶层常量 → pub 常量接口 → 按 SymbolId 排序的 bodies → concrete transform（先取 snapshot）→ nominal 参数约束 → Weak/无限布局 → produce_ownership → facade verify_types。公开阶段函数与尚未移植的 cancellation API 见文末。

common.wz 已实现上面列出的 Root 公共助手（clone_checker 仍在 clone.wz）。嵌套实参如果同时用到同一个 inout state，必须先拆成局部值，例如先写 `let value=child(inout state,r,0);`，再调用 `expression_mode(inout state,value)`；这一写法已通过真实 stage0 类型检查确认。

通过共享 helper 回调时，必须同时保留回调错误与 Checker 的可变修改。共享 helper 只抛 SupportError，因此回调需要把原始 Error 存入本次调用独立的 error_slot，helper 返回后优先重抛该原始 Error，不丢失 code、span、related 和 FailureKind。修改 Checker 时通过捕获 Vec[Checker] 单元完成，不要捕获 inout 形参或单纯捕获一份 state 值。退出 helper 后，无论是正常路径还是错误路径，都要先执行 state=cell[0]，再执行 state.m.types=context。

下面是已通过真实类型检查及 O0/O2 运行的同包可编译写法，源码在 selfhost/tests/sema_common/helpers.wz。其中的 invoke_cell_context 是真实调用 types API 的普通测试 helper，不冒充 source_constant_value；替换成真实 helper 时保持相同的取回和错误处理顺序即可。

```wz
// An ordinary helper that really interns through the separate inout Context.
// It is not a stub for source_constant_value and never claims to evaluate it.
func invoke_cell_context(context inout types.Context,
    callback func()usize throws model.SupportError, helper_failure bool,
    observed Vec[usize])usize throws model.SupportError {
    let callback_type=callback();
    // Deliberately detach AFTER callback, then append through the helper ctx.
    // This makes the final state.m.types=context assignment observable.
    context=types.clone_context(context);
    var vector=model.empty_type(model.TypeKind.Vec);vector.type_args=[callback_type];
    var result usize=0;
    try {result=types.intern(inout context,vector);}
    catch error types.Error {throw support_from_type_error(error);}
    observed[1]=result;
    if helper_failure {throw model.SupportError.Limit("cell helper error");}
    return result;
}
func checker_cell_bridge(state inout Checker,kind model.TypeKind,bits u32,
    owner usize,parameters Vec[hir.Parameter],callback_failure bool,
    helper_failure bool,original Error,observed Vec[usize])usize throws Error {
    let cell=vec.make[Checker](1,state);
    var context=state.m.types;
    let error_slot=vec.empty[Error]();
    let callback=func()usize throws model.SupportError {
        try {
            let result=scalar(inout cell[0],kind,bits);
            cell[0].function=owner;
            let table=vec.empty[ParametersEntry]();
            vec.push(table,ParametersEntry(owner:owner,values:parameters));
            cell[0].parameters=table;
            observed[0]=result;
            if callback_failure {throw original;}
            return result;
        }catch error Error {
            vec.push(error_slot,error);
            throw model.SupportError.Invalid("sema callback failed");
        }
    };
    var value usize=0;
    var helper_error=option.Option[model.SupportError].None;
    try {value=invoke_cell_context(inout context,callback,helper_failure,observed);}
    catch error model.SupportError {helper_error=option.Option[model.SupportError].Some(error);}
    // Restore BOTH paths before rethrow. Reversing these loses helper ctx edits.
    state=cell[0];
    state.m.types=context;
    if vec.len(error_slot)>0 {throw error_slot[0];}
    switch helper_error {
        case option.Option.Some(error):throw Error.Support(error);
        case option.Option.None:{}
    }
    return value;
}
```

上述顺序不能调换：每次调用都从空的 error_slot 开始；先用 cell[0] 取回整个 Checker，再用 context 回写 helper 的 TypeContext；正常和错误路径都必须做这两步赋值；只要 error_slot 非空就优先抛回里面的原始 Error，之后才包装 helper 自身的 SupportError。即使 helper 正常返回也要检查 error_slot，防止某个吞掉回调错误的 helper 伪造成功。

测试里回调会新建 TypeId、修改 function 标量，并整体替换 parameters 表（而不只是往原本共享的 Vec 里 append）。helper 在正常回调后会先 clone 一份独立 Context 再 intern Vec，以此实际检验最后一步 state.m.types=context 是否到位。三条路径都会核对 Checker 的标量、表、TypeId，以及原始诊断的 code/span/related 或 helper 的原始 Limit 错误，具体结果记录在 cell-review.md。

这套写法依赖当前 types.Context 的公开 intern 直接往共享的 entries Vec 追加，因此初始的 context=state.m.types 与 cell 内的上下文看到的是同一批已知 ID。如果某个 helper 在回调前就把 Context 换成了另一份独立表，就必须在那个 helper 处单独处理同步，不能指望调用结束后覆盖写回就能保证回调期间 ID 一致。本轮未调用尚未合入的 source_constant_value，只确认了真实 Checker cell、普通 Fn 与当前 types API 组合可行。

has_async_marker 沿用 Root 实际 CPP:148-153 遍历全部 children 的写法；上方关于首 child 的注释属于 parser 不变量，已按 Root 意见保留遍历逻辑，没有缩成只看首 child，也未伪造取消 token。

`common.node` 会复制 children，NodeData 则视为已组装完成的交付 payload。如果 A/B 后续还要修改留在局部的 payload、Call.args、FunctionValue.captures、Pattern、Constant 等 Vec，必须先自行深复制，不要指望 Wz 赋值表现得像 C++ 值拷贝。common 不负责全套 NodeData clone；当前 node 测试也只验证了叶 payload 与 children 的隔离，不能当作全 payload 复制验证。

`selfhost/tests/sema_common/run.py` 会临时组装 sema：只复制当前生产中的 state、errors、common 三份源码和同包 helpers，其他包直接链接当前 Root，明确不包含 clone 以及未完成的 A/B。脚本会执行 check 和 O0/O2 运行，每次运行都要求精确 46 项通过、0 失败，测试构造器字段均已填全。runner 默认路径基于 __file__ 解析，可用 --woozyc 覆盖，单条命令超时会终止整个进程组。此处暂不注册 CMake，统一由 Root 接入。

```sh
python3 selfhost/tests/sema_common/run.py --woozyc /absolute/path/to/woozyc
```

该测试只能证明 common、普通 Fn cell、context 和错误槽机制可用，不能据此宣称完整 Checker、clone 隔离、真实 source_constant_value 调用或取消功能已经通过。

HIR 克隆接口现已改为可抛 model.SupportError，clone_checker 会统一将其包装为 Error.Support。递归超限或成环时必须返回内部错误，不允许靠故意越界触发 Panic。Sema 变换局部 HIR 时使用 clone_node、clone_function、clone_pattern 和 clone_constant，这四个函数同样 throws SupportError。

## Checker 构造、run 与公开阶段函数

```wz
// 包内：调用者不需要构造 Checker。
func make_checker(resolved model.ResolvedProject,
    config model.CheckConfig) Checker throws Error
func run_checker(state inout Checker) model.UnitResult throws Error

// 对应 C++ 接收 ResolveResult 的重载；没有可伪造的 verified 标志。
pub func check_types(input model.ResolveResult,
    config option.Option[model.CheckConfig]) hir.HirResult
pub func check_project(input model.ResolveResult,
    config option.Option[model.CheckConfig],
    mode hirverify.VerifyMode) hirverify.VerifyResult
```

`make_checker` 先把 ResolvedProject 放进空的 HirModule，再调用真实的 `hirclone.clone_module`，确保 Checker 追加和更新符号时不会改写传入的 ResolveResult。Wz 没有 C++ 的 `shared_ptr<const ResolvedProject>`，Project 和 Source 的数组也直接走现有 clone 实现复制，不另维护一套复制代码。构造时先按 Unit、bool、i64 的顺序 intern，再建立 definitions 和 occurrences：相同 Ref 的 definitions 保留第一次 emplace，occurrences 保留最后一次赋值，两张表都按 `(module,file,node)` 排序。45 个实际状态字段全部沿用 C++ 初值。

完整 bounds 重检依次遍历 traits、nominals、impls，以及按 owner 排序的 generic_params。每次检查都会暂存 signature_parameters，重新建立泛型参数、Self 和 trait 环境集合，其中参数 bounds 在前、impl trait bound 在后。顶层常量仍按模块/文件/AST 顺序求值，pub 常量按符号表顺序检查。函数正文按 SymbolId 排序；函数转换会先拍一份当前 body ID 数组的快照，只转换快照内的函数。之后固定检查前记录的 types.size 来校验 nominal 参数约束，避免把检查期间新 intern 的类型算进本轮根集合。

`run_checker` 返回的 UnitResult 来自真实的 `ownership.produce_ownership(inout state.m)`。C++ 在 ownership 失败且带有诊断时只保留首条诊断和原 failure，不带诊断时抛 Cancelled；check_types 沿用这两个分支，不把缺诊断改报成 Support 内部错误，也不扩充共享 Error。ownership 成功后调用 `hirverify.verify_types`，原样透传其 UnitResult 的 state、failure 和全部 diagnostics，只有成功时 HirResult.has_value 才为 true。check_project 只在 check_types 成功时才调用 hirverify.verify，否则返回 value=None 并转发原阶段信息。

ResolveResult 是否成功沿用 C++ 定义：状态为 Complete、带有 value、且没有 severity="error" 的诊断。失败输入即使带有 value 也不会进入 Checker；Recovered 和 Cancelled 保留原 state，出错的 Complete 改为 Failed；无诊断且非 Cancelled 时生成 `E_INTERNAL_ERROR: resolve did not successfully complete`。转发诊断时会复制 related、fixits 和 notes，保留原 code、span 和 failure。只有在 resolve 成功后，才按与 C++ 相同的顺序检查 Project.backend、可选 config 与 Project 是否一致，以及 word_bits 是否为 32/64。`Error.Rejected` 保留源诊断及失败类别，`Error.Support` 则生成 type 阶段无 primary 的 E_INTERNAL_ERROR 并保留原消息。

C++ 中的空 owning 指针在当前按值传递的 Wz API 下无法构造，因此不额外增加 has_owner 这类标志。当前 Wz facade 尚未接入 stop_token，只能转发已收到的 Cancelled，还不支持运行中的共享取消；最终的 CLI SIGINT 和临时输出清理仍需另行测试。

本轮检查覆盖了这两个文件的真实 C++ lexer/parser 语法检查，以及从当前生产源码提取的构造/排序/阶段助手独立程序：后者通过了 stage0 check，O0 和 O2 各 33 项测试通过，结果保存在仓库外的 `checks/sema-run-facade-20261113/`。该独立程序不包含 make_checker 中的 hirclone 调用、run_checker，以及公开的 check_types/check_project，也没有任何 A/B 或 verifier stub。完整包类型检查、构造函数的实际 clone 隔离、完整 run 的诊断先后顺序，以及 C++/Wz 行为对照目前均为 **not_run**，必须等 A/B、hirclone、valueops、ownership 和 hirverify 联合后再测。本轮没有重跑原有的 46 项 common 测试。
