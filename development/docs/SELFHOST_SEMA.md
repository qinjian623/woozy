# Selfhost sema 共享状态与分工

这份记录固定 `src/sema/check.cpp` 的移植分工与包内接口。`selfhost/src/wzcc/sema/{state,errors,clone,common}.wz` 保存共享记录、错误、完整试探复制和已实测的公共助手；`run.wz`、`check.wz` 已实现构造、运行次序和公开阶段函数。A/B 算法及 hirverify 等依赖尚待联合检查，不能把 common 或独立助手测试通过称作完整 Checker 可编译或自举完成。clone 依赖真实 hirclone，尚未由 sema_common 测试覆盖。

保持一个 `package sema`，A/B 按函数分到不同文件，同包直接互调。不要拆出循环导入的类型/表达式包，也不创建让半个 Checker 冒充运行成功的算法 stub。Root 维护共享记录、common/errors/clone 和后续 facade/run；A 负责声明、类型、泛型及实例化；B 负责表达式、调用、语句、函数正文和效果记录。parameter_type 归 A，不在 common 调用未实现的 parse_type。

CPP 方法的完整归属如下，冒号后是原 check.cpp 行号；同名重载各列一次。file/type 在 Wz 改名 parsed_file/lookup_type。Root 的 Checker 构造/run/facade 代码已写入 run/check；其余 Source/公共助手已在 common。A/B 必须覆盖各自函数，不能因为已有独立算法包便遗漏 Checker 的衔接工作。

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

建议 A 写 types/generics/declarations，B 写 expressions/calls/statements/effects；只是同包多文件。B 比 A 重，不再为了平均行数切开 typed_call 与 expression/place/borrow。A→B 主要是 instantiate 调用 annotate/flow；B→A 则是 parse_type、inference、方法来源/约束、Weak、foreign 类型包装。可独立编辑，完整运行验证必须等两组算法同时到齐。

现有算法包和 Checker 责任不同：constant 缓存/递归保护/临时 m.nodes 截断、pattern 将绑定写 locals、foreign_call 的候选试探、annotate 的 defer/effect/exit 生成仍在 Checker；分别调用 constant/pattern/foreign/valueops/ownership，不表示那些包已经实现这些 Checker 函数。

CPP 字段对应：stop 没有当前 Wz facade 对应物，草稿不加 bool 或假 token；取消 API 尚未移植，最终 CLI SIGINT/输出清理由 Root 后续验证。owned 与 m.resolved 合成 state.m.resolved 唯一可变副本，所有 synthetic/capture/instance symbol 都从它追加。i64 类型缓存字段改名 i64_type；Value.string 改名 string_value；ClosureInfo.move 改名 is_move，属于 Wz 拼写迁移。

CPP foreign_call:3873 的 trial 不得变成 `var trial=state` 后直接检查；必须 `var trial=clone_checker(state)`。CPP 默认 Checker 拷贝本来就深复制 vector/map/TypeContext，但 shared_ptr owned 和 m.resolved 仍共享，所以接着专门复制 owned 并重新指向。Wz Vec 传值会共享底层存储，故除了 m，还必须复制所有 locals/constants/tables/caches。hirclone 复制整份 resolved/project 比 CPP 不可变 project 的 shared_ptr 更彻底，但保留 ID 和数据值，不重新解析、不重排、不生成新 Symbol/Type/HirNode ID。成功试探不能把 trial 的 TypeId/node/symbol 缓存直接发布给原 state；CPP 做法是选出 binding 后在原 state 重新 build。失败试探必须完全不影响原 state。

具名 Vec entry 的更新约定必须一起冻结：

1. 模拟 std::map：按真实 key 查找，upsert 替换唯一 entry，不追加重复项；涉及遍历时按 CPP key 升序（NodeKey 为 module,file,node；SymbolId 为数值；TraitKey 为 actual,declaration,args 的词法序；InstanceKey 为 origin,args）。不能拿插入顺序代替 bodies/generic_params/trait_signature_context 等 map 的迭代顺序。
2. 模拟 std::set 的 Vec 保持无重复；需要迭代输出时数值/键升序；loops/break_targets/current_captures/参数/类型实参是原本有序 vector，禁止当 set 排序。
3. 所有 usize Symbol/Module ID 的 0 都可能有效；TypeId 0 才无效。空 optional 必须 Option.None。Ref.file 是模块内序号。
4. 对 entries 的读取拿到含 Vec 的值仍会共享句柄，不能像 C++ 值副本一样修改。凡是将读取结果用于替换类型实参、inference sub、renamed captures 或 trial 后再修改的地方，先复制对应 Vec。尤其 select_method 的候选局部 sub、instantiate 的临时 HirNode/Pattern/Constant 和 m.functions 的 concrete 快照，不能把原记录改坏。try_infer 本身与 CPP 一样，false 不承诺回滚传入的 sub；应在 CPP 本来创建/复制局部 sub 的位置隔离，不额外发明统一回滚。
5. 初始 scalar intern 顺序仍是 Unit、bool、i64；definitions 按 symbol 遍历 emplace，occurrences 按输入顺序覆盖相同 key。declarations 的多轮处理、nominal_fields_ready 切换、完整类型检查后的 weak_validated 缓存策略和 run 顺序不能因分文件改动。constants 的临时 m.nodes 截断仍按原 begin；不会重排其他表或回收已分配 TypeId。


Checker 有45个实际持久字段、30个Vec字段。state.wz 保留当前 async_function/current_result/current_async_error、闭包来源与绑定、trait上下文/约束缓存、常量/精确整数、模板/实例缓存、Weak完整性标志。C++ stop_token 没有当前 Wz facade 等价物，不加 bool 或假 token；取消 API 和最终 CLI SIGINT/临时输出清理由后续阶段验证。

必须保留声明顺序：nominal形状 → trait arity → nominal bounds → trait Self/prototype → fields/payload → nominal_fields_ready → impl匹配 → trait/inout完整复检 → 普通函数与const内Fn注册。run 将来仍按常量、pub常量接口、按SymbolId的bodies、concrete快照transform、nominal约束、Weak/无限布局、ownership、verify顺序执行；typed_call保留template symbol/type_args，不能提前改变实例化时序。signature_parameters 的临时环境和 weak_validated 的完成条件也不能改。

以下是包内函数声明清单，不是可编译的空函数实现；省掉 C++ 默认参数，调用方必须传全。全部算法在 `package sema`，共用 state.wz / errors.wz。有状态检查统一 `state inout Checker`；Error 是同包 errors.wz 的错误。返回 AST/Type/声明记录是只读快照，不允许调用方修改其嵌套 Vec。

A 负责类型、声明与泛型；B 负责表达式、正文和效果。A→B 仅需 `flow` / `annotate`；下面同时列出 Root facade 要调用的入口。`trait_index`/`impl_index`/`method_index` 是 m 中数组下标，不是 SymbolId。泛型实例化仍在类型/泛型一侧，不能随意移到独立包。

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

错误构造函数返回 Error，算法调用点必须写 `throw reject(...)` / `throw unavailable(...)`，不能只调用 Unit-throws 辅助函数来假设 never-return；Wz 没有 C++ `[[noreturn]]` 的跨函数流程承诺。`reject` 等声明中的 throws Error 只为构造诊断时读取不合法 Ref 的 Support 故障；正常路径返回原 Rejected 值。

其中 C++ `file()` 改名 parsed_file，返回 ParseResult；必须经过 `project.files[project.modules[r.module].files[r.file]]`，不能直接用 r.file 索引全项目文件。`type()` 改名 lookup_type，避免关键字；类型表 intern/substitute 的 types.Error 先转 SupportError，再包 Error.Support。`node()` 保留按调用次序 append，不能按 AST 编号重新排序。

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

`try_infer` 的 false 只吞 CPP Rejected，不吞 Error.Support。上面 infer 的第三、四参是 TypeId；不要误用 hir.Pattern。A 内部 `signature`、`generics`、`check_trait_application`、`check_trait_application_data`、弱类型递归助手、register_function 等不再列成跨文件 API，仍可在 A 自己的几个文件之间直接互调。

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

B 内的 expression/call/typed_call/place/pattern/switch_statement/block/statement/foreign_call 不必另做模块接口；同包直接互调。Root run 按 CPP:6703-6796 的顺序：declarations → 完整 bounds 重检 → 顶层常量 → pub 常量接口 → bodies 按 SymbolId → concrete transform（先取 snapshot）→ nominal 参数约束 → Weak/无限布局 → produce_ownership → facade verify_types。公开阶段函数及未移植的 cancellation API 见文末。

common.wz 已实现上面 Root 公共助手（clone_checker 仍在 clone.wz）；parameter_type 按新分工归 A，common 不调用它。所有同时使用同一 inout state 的嵌套实参必须先拆为局部值，例如先 `let value=child(inout state,r,0);`，再 `expression_mode(inout state,value)`。这一写法已由真实 stage0 类型检查确认。


回调错误与可变 Checker 必须同时保留。共享 helper 只抛 SupportError；回调把原 Error 存入本次独立 error_slot，helper退出后优先抛回原 Error，不能丢失 code/span/related/FailureKind。回调通过捕获 Vec[Checker] 单元修改 Checker；不要捕获 inout 形参或仅捕获一份 state 值。取回时先 state=cell[0]，再 state.m.types=context，正常/错误退出都执行。

下面是已经通过真实类型检查/O0/O2的同包可编译模式，源码位于 selfhost/tests/sema_common/helpers.wz。invoke_cell_context 是真实调用 types API 的普通测试 helper，不冒充 source_constant_value；换成实际 helper 时保留同样的取回/错误次序。

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


必须保留以下次序：每次调用从空 error_slot 开始；先以 cell[0] 取回整个 Checker，再以 context 取回 helper 的 TypeContext；正常和错误退出都做这两次赋值；若 error_slot 非空，优先抛其中原 Error，随后才包装 helper 自身 SupportError。正常返回也检查槽，不能让一个吞掉回调错误的 helper 伪造成功。

测试中回调新建 TypeId，修改 function 标量，并整体替换 parameters 表（不是只 append 一个原本共享的 Vec）。helper 在正常回调后把 Context clone 成新存储再 intern Vec，故最终 state.m.types=context 的必要性可实际检查。三条路径均检查 Checker 标量/表/TypeId、原诊断 code/span/related 或 helper 原 Limit；具体结果记录在 cell-review.md。

此模式依赖当前 types.Context 的公开 intern 在共享 entries Vec 上追加；初始 context=state.m.types 因而与 cell 的上下文保持同一批已知 ID。helper 如果在回调前把 Context 替换为另一份独立表，则需在实际 helper 处另核对同步，不能仅凭调用后覆盖就保证回调期间 ID 一致。本轮没有调用尚未合入的 source_constant_value；只确认真实 Checker cell/普通 Fn/当前 types API 的模式可用。

has_async_marker 保留 Root 实际 CPP:148-153 的遍历所有 children 写法；上方首 child 注释是 parser 不变量，按 Root 纠正未改成仅首 child。没有伪造取消 token。

`common.node` 复制 children；NodeData 是已组装结束的交付 payload。A/B 若还会修改留在局部的 payload/Call.args/FunctionValue.captures/Pattern/Constant 等 Vec，先做对应深复制，不依赖 Wz 赋值模拟 C++ 值复制。common不是全套NodeData clone。当前 node 测试仅验证叶payload与children隔离，没有把它说成全payload复制验证。

`selfhost/tests/sema_common/run.py` 临时组装 sema：只复制当前生产 state/errors/common 三份源码和同包 helpers，其他包链接当前 Root；明确不包括 clone 或未完成 A/B。它执行 check 和 O0/O2，各运行必须精确46项、0失败；测试构造器已填全。runner默认路径来自 __file__，--woozyc可覆盖，单命令超时会杀整个进程组。此处不注册CMake，统一注册由Root处理。

```sh
python3 selfhost/tests/sema_common/run.py --woozyc /absolute/path/to/woozyc
```

该测试只证明 common/普通Fn cell/context/错误槽可用；完整Checker、clone隔离、真实source_constant_value调用及取消均不能据此声称通过。

HIR 克隆接口现可抛 model.SupportError；clone_checker 统一包成 Error.Support。递归超限/循环必须返回内部错误，不允许故意越界触发 Panic。Sema 变换局部HIR时使用 clone_node/clone_function/clone_pattern/clone_constant，四者同样 throws SupportError。

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

`make_checker` 将 ResolvedProject 放入空 HirModule，再调用真实 `hirclone.clone_module`，使 Checker 对符号的追加和更新不改写 ResolveResult。Wz 没有 C++ 的 `shared_ptr<const ResolvedProject>`；Project/Source 的数组也由现有 clone 实现复制，不另维护一份复制代码。构造时先按 Unit、bool、i64 顺序 intern，随后建 definitions/occurrences：相同 Ref 的 definitions 保留第一次 emplace，occurrences 保留最后一次赋值；两个表按 `(module,file,node)` 排序。45个实际状态字段均沿用 C++ 初值。

完整 bounds 重检依次遍历 traits、nominals、impls、按 owner 排序的 generic_params。每次检查暂存 signature_parameters，重新建立泛型参数、Self 和 trait 环境的集合，参数 bounds 在前，impl trait bound 在后。顶层常量仍按模块/文件/AST 顺序求值，pub 常量按符号表顺序检查。正文按 SymbolId 排序；函数转换先保存当前函数的 body ID 数组，只转换这份快照。随后固定 types.size 的原值检查 nominal 参数约束，不能将检查期间新 intern 的类型重复算入本轮根集合。

`run_checker` 的 UnitResult 来自真实 `ownership.produce_ownership(inout state.m)`。C++ 在 ownership 失败且有诊断时只保留首条诊断与原 failure，未附诊断时抛 Cancelled；check_types 保持这两个分支，不把缺诊断改成 Support 内部错误，也不扩充共享 Error。ownership 成功后调用 `hirverify.verify_types`；该 UnitResult 的 state/failure/全部 diagnostics 原样传回，只有成功时 HirResult.has_value 才为 true。check_project 仅在 check_types 成功时调用 hirverify.verify，否则返回 value=None，并转发原阶段信息。

ResolveResult 的 success 按 C++ 定义：Complete、有 value、没有 severity="error" 的诊断。失败输入携带 value 也不会进入 Checker；Recovered/Cancelled 保留原 state，错误的 Complete 改成 Failed；无诊断且非 Cancelled 时生成 `E_INTERNAL_ERROR: resolve did not successfully complete`。转发诊断复制 related/fixits/notes，保留原 code/span/failure。成功 resolve 才检查 Project.backend、可选 config 是否与 Project 一致、word_bits 是否为32/64，顺序和 C++ 相同。`Error.Rejected` 保留源诊断及失败类别，`Error.Support` 产生 type 阶段无 primary 的 E_INTERNAL_ERROR，并保留消息。

C++ 空 owning 指针在当前按值 Wz API 中不可构造，不添加 has_owner 一类标志。当前 Wz facade 没有 stop_token；可转发已收到的 Cancelled，尚未提供运行中的共享取消。最终 CLI SIGINT 与临时输出清理仍需另测。

本轮检查覆盖两个文件的真实 C++ lexer/parser 语法检查，以及从当前生产源码提取的构造/排序/阶段助手独立程序；后者 stage0 check 通过，O0/O2 各33项通过，结果在仓库外 `checks/sema-run-facade-20261113/`。该程序不包含 make_checker 的 hirclone 调用、run_checker、公开 check_types/check_project；没有任何 A/B 或 verifier stub。完整包类型检查、constructor 的实际 clone 隔离、完整 run 的诊断先后和 CPP/Wz 行为对照均为 **not_run**，须等 A/B、hirclone、valueops、ownership、hirverify 联合后检查。没有重复运行原46项 common 测试。
