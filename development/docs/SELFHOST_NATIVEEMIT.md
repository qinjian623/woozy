# nativeemit 同包接口（移植中）

这里只实现共享记录和 LLVM 文本函数。没有实现 B/C/D/E 的语言发射主体，没有 `emit`、`VerifiedModule`、`verified=true` 或替代检查器。`Emitter.input` 是 Root 已接受的 `nativeir.Module` 原始组装数据；公开编译入口仍须等独立 lower/verify 完成。

把 `nativeemit/state.wz`、`nativeemit/text.wz` 放入同一个 `wzcc/nativeemit` 目录。后续四人分别新增 `scalar.wz`、`values.wz`、`async_gc.wz`、`system.wz/c_ffi.wz`，均写 `package nativeemit`。Root 独占 state/text。辅助函数自己的短期局部记录可以放所属文件，不能在各文件再定义 Emitter、Value、Signature 或公共缓存。

**同包的非 pub 函数也共享名字空间。** B/C/D/E 局部 helper 必须分别加 `b_`/`c_`/`d_`/`e_` 前缀；下文明确列出的跨组函数才用合同名字，且只有所列 owner 定义。Root 已有 `invalid/number/copy_signature/check_function/check_block/current_function/current_block/global_name_available`，其他组不得重复定义；`invalid(message string) Error` 与 `number(value usize) string` 可在同包直接调用。四组完成后合并执行 check，不各自放同名 stub。

## 共用约定

* `Value` 仅有 LLVM `type_name/reference`。`ValueId/TypeId/SymbolId` 保留为已接受记录中的 usize；C schema 索引仍为 u32。缓存使用具名记录和真实 ID；环境与 GC 的匿名字段键是有序 `Vec[usize]`，不编码成字符串。
* `Emitter.storage_types` 用 `types.clone_context(input.hir.types)` 单独复制。聚合算法**只使用 Native verifier worker 编写的 nativelayout 包**。B 的包装在调用时创建 `AggregateTypes(module:state.input.hir,context:state.storage_types)`，fields/field_index 返回后把 context 写回 state.storage_types；不能再次调用 aggregate_types(input.hir) 丢失已追加类型。所有 type lookup 用这份私有 Context；存储专用 ID 不写回 input/HIR/NativeIR。
* 四组的失败统一 `throws Error`，捕获为 `Error.Failure(code, kind, message)`。无效内部数据为 `E_IR_INVALID/Internal`，原来不支持的组合为 `E_CAPABILITY_UNAVAILABLE/Unsupported`；环境错误由 Root 驱动用同一 payload 表达 `Environment`。不把 typed source throws、编译器 Error 和源语言 Result 混为一类。
* 可写 Emitter 参数在最后，仅为统一写法；**参数顺序不能取消 Woozy 的别名检查**。同一个调用不要既传 `inout state` 又在别的实参里读取整个 state。先把需要的 Value/Signature/文本读到局部变量。`new_block` 直接给 `BlockRef`，分支拼接用 `block.reference`，避免每条分支都查 Emitter。
* 文本 cursor 与 `FunctionFrame` 分开。进入 helper：`saved=save_cursor(state)`；new_function/new_block 不改 cursor；position 后写 helper；restore_cursor。不清空源函数 frame。SSA 和 label 计数属于每个 FunctionText，helper 不会占用调用者的 SSA 编号。
* 所有 helper 缓存必须先登记函数/全局引用，再递归生成内容，保持原实现对递归 copy/drop/trace 的处理。 缓存中的 Vec 键必须复制后保存，不能共享调用方随后可能修改的数组。只有组装一条新的源函数时，B 重置 FunctionFrame。
* 固定类型：Unit 普通值 i8、语言 bool i1、C bool 存储 i8；String `{ptr,i64,i64,ptr}`，Fn/Weak/dyn `{ptr,ptr}`，Slice `{ptr,i64,i64}`。`declare_runtime_types` 只写这些固定运行库布局，不做语言类型分析。
* `Signature` 是确切 LLVM 参数/结果拼写。普通函数 Unit 返回 i8；throws 为 `args..., [out_value], out_error -> i1`；函数值 env-first；async coroutine 为 `args..., future -> void`。FFI adapter 的实参仍是各存储槽指针，真实 C 分类由 Clang 编译 Wz 生成的 C。
* 不发 `fast/contract/nsw/nuw`，不为未知别名发 noalias。`allocation_size(T)` 只生成 DataLayout 可求值的 GEP/ptrtoint 常量；空元素 stride/scratch 处理由 B/C 保留。默认对齐交给 LLVM，额外对齐只写原实现已知的值。
* `sections` 返回有序片段，供驱动缓冲或逐块写；不拼接整个模块，也不运行工具。它只检查文本容器是否写完，LLVM 类型/CFG/ownership 的正确性仍由各自检查器和 opt 判断。

## Root 维护的文本函数（已经有实现）

精确声明见 text.wz；常用写法：

```text
new_emitter(input nativeir.Module, options Options) Emitter throws Error
define_type(name string, body string, state inout Emitter) string throws Error
define_global(name string, body string, state inout Emitter) Value throws Error
declare_function(name string, signature Signature, state inout Emitter) Value throws Error
define_metadata(key string, body string, state inout Emitter) string throws Error
new_function(name string, signature Signature, linkage string, attributes string,
             state inout Emitter) usize throws Error
new_block(function_index usize, prefix string, state inout Emitter) BlockRef throws Error
position(block BlockRef, state inout Emitter) throws Error
save_cursor(state Emitter) Cursor
restore_cursor(cursor Cursor, state inout Emitter) throws Error
here(state Emitter) BlockRef throws Error
parameter(state Emitter, function_index usize, index usize) Value throws Error
function_reference(state Emitter, function_index usize) Value throws Error
fresh_value(type_name string, state inout Emitter) Value throws Error
append(line string, state inout Emitter) throws Error
result(type_name string, expression string, state inout Emitter) Value throws Error
terminate(line string, state inout Emitter) throws Error
entry_result(type_name string, expression string, state inout Emitter) Value throws Error
reserve_prefix(state inout Emitter) PrefixRef throws Error
set_prefix(slot PrefixRef, line string, state inout Emitter) throws Error
allocation_size(type_name string) Value
sections(state Emitter) Vec[string] throws Error
typed(value Value) string
```

`result` 只在表达式前加 `%vN =`；append/terminate 只写调用者给的 LLVM 文本，未实现运算对象图。循环 PHI 用 fresh_value + reserve_prefix；所有实际后继值已知后 set_prefix。entry_result 将 alloca 等文本放当前函数首块的 prefix。引用使用 `%vN/%pN`，定义名和块名统一做 LLVM quoted-name 转义。escape_bytes/llvm_bytes/llvm_string 按 UTF-8 **字节**转义为 `\HH`，可输入任意 Vec[u8]（包括 FF）；不调用 std::quoted 的规则。

声明重复且签名相同只保留一份；后续同名定义取代声明。类型、全局、metadata 的同名不同内容报错，不静默覆盖。declare_function 的统一声明不带函数属性；源函数/helper 属性写在 new_function 的 attributes，panic 的 noreturn/nounwind 可放在 call-site。这样 generic runtime 声明不会与 guard 的额外属性产生文本冲突。

Signature.parameters 是可写 Vec，存入声明/定义时必须复制；调用者之后修改原 Vec 不得改变已记录的签名。其他组保存 fields/type_args 等可写向量作为缓存键时也必须复制向量，不能让后续修改改变旧键；字符串本身不可变，可正常共享。

## 公开的纯发射入口（冻结签名，尚未实现）

```text
pub func emit(input nativeverify.VerifiedModule, entry usize, options Options) EmitResult
Output { llvm_sections Vec[string], c_adapter_source string,
         triple string, data_layout string, has_async bool }
EmitResult { state model.PhaseState, failure model.FailureKind,
             diagnostics Vec[model.Diagnostic], value Option[Output] }
```

Output/EmitResult 记录已在 state.wz。emit 入口只接 nativeverify 的私有 VerifiedModule；先 snapshot 取得单独副本，纯组装器在内部使用 raw nativeir.Module。失败返回 value=None。成功仅表示文本组装完成，`llvm_sections` 是**优化前** IR；不声称 LLVM 或对象生成已通过。has_async 为输入任意一个可执行 NativeIR 函数的 is_async，供 Root 选择 O0 的 coroutine pipeline。组装器在发射任何源函数前必须设置 `state.gc_entry_symbol = Some(entry)`，与 C++ 相同；不能等 `emit_main` 才设置，否则会漏掉主函数循环出口的回收调用。入口不读文件、不调用子进程、不跑 opt/llc、不链接，也不发布最终文件。Root 驱动验证 LLVM 并完成全部后续步骤后才发布输出。

## 缓存唯一写入者

| 所有者 | 写入位置 |
| --- | --- |
| Root | 顶层 type_definitions/declarations/globals/metadata/functions/cursor/data_layout；只能经 text 函数改。storage_types 的创建/克隆也由 Root 做 |
| B | storage_types 后续 intern；b.layouts/source_functions/TBAA；frame.native_index/values/constants/slots/blocks/try_outputs/incoming。Root 先选择 gc_entry_symbol，B 的 emit_main 可确认同一入口。SourceFunction.text_index 是 LLVM 函数容器下标，native_index 是输入 NativeIR 函数下标，两者不能混用 |
| C | c 的 value_ops/thunks/environment_types/environment_ops/bound_adapters/trait_vtables/trait_drop/trait_clone/thread_bridges/vec RC helpers；frame.function_values |
| D | d 的 async_symbols/promises/trace_vecs/trace_fields/ops_vecs/ops_fields/gc_ops_type/gc_invalidate 和三个 flags；frame.async_state |
| E | e 的 C record/callback/dispatcher 缓存、used ID 集合、adapter_source。C 需要 C resource retain/release 时调用 E 的 resource_operation，不直接改 e.used_resources |

每个函数的本地 BlockRef、PrefixRef 和变量由生成它的组负责。这里只固定字段，不为缓存另做一套泛型 map/registry。

## B：scalar.wz

负责 C++ `lower_type/type_key/signature/guard/binary/convert/vec_memory/vec_field/call_origin/indirect_call_operand`，普通指令/终结符、聚合、可写位置、普通函数与 main。`ir/layout.hpp` 算法归 nativelayout，B 只做薄包装、转换 SupportError 和同步私有 context，不能再移植一份。源函数首块/slots/active/try/await incoming 地址和 RPO 遍历由 B 做，异步专有部分调用 D。

所有跨组函数的精确签名如下；只读 state 在首位，可写 state 在末尾：

```text
type_of(state Emitter, type_id usize) model.Type throws Error
aggregate_declaration(state Emitter, type_id usize) hir.NominalDecl throws Error
aggregate_fields(type_id usize, variant option.Option[usize], state inout Emitter) Vec[hir.FieldDecl] throws Error
field_index(type_id usize, field usize, variant option.Option[usize], state inout Emitter) usize throws Error
variant_index(state Emitter, type_id usize, variant usize) usize throws Error
lower_type(type_id usize, state inout Emitter) string throws Error
type_key(state Emitter, type_id usize) string throws Error
signature(type_id usize, env_first bool, skip_first_parameter bool, state inout Emitter) Signature throws Error
find_hir_function(state Emitter, symbol usize) option.Option[hir.HirFunction]
source_function(state Emitter, symbol usize) SourceFunction throws Error
call_target(state Emitter, symbol usize) Value throws Error
value(state Emitter, value_id usize) Value throws Error
value_type(state Emitter, value_id usize) usize throws Error
constant_value(state Emitter, value_id usize) option.Option[Value]
storage(type_name string, state inout Emitter) Value throws Error
stride(type_id usize, state inout Emitter) Value throws Error
element_storage(type_id usize, value Value, state inout Emitter) Value throws Error
guard(bad Value, cause u32, source model.Span, state inout Emitter) throws Error
checked_index(index Value, type_id usize, source model.Span, cause u32, state inout Emitter) Value throws Error
vec_memory(header bool, state inout Emitter) string throws Error
vec_field(vec Value, field u32, type_name string, state inout Emitter) Value throws Error
call(signature Signature, callee Value, args Vec[Value], state inout Emitter) Value throws Error
runtime(name string, result_type string, args Vec[Value], state inout Emitter) Value throws Error
```

`runtime/call` 的 void 分支只 append，不给 void 分配 SSA；返回 `{type_name:"void",reference:""}` 供丢弃。语言 Unit 值仍由调用者返回 i8 0。vec_memory 返回 `, !tbaa !N` 后缀。constant_value 仅保存原 ConstantInt/FP 和纯 fneg 常量拼写，供 C 的批量 Vec 初始化使用；不引入其他常量折叠。

同组主体建议固定为 `declare_source_functions(state inout Emitter)`、`emit_function(function_index usize,state inout Emitter)`、`emit_instruction(instruction nativeir.Instruction,state inout Emitter)`、`emit_terminator(terminator nativeir.Terminator,block_id usize,state inout Emitter)`、`emit_main(entry usize,state inout Emitter)`，均 throws Error。这些是后续 B 要实现的函数，草稿没有假实现。

B 的其他局部函数用 b_ 前缀，包括 b_binary、b_convert、b_signed、b_call_origin、b_indirect_call_operand 和指令分支短 helper，不能与 C/D/E 的局部函数重名。

## C：values.wz

负责 value_ops.inc、weak_ops.inc、concurrency.inc 和 emit.cpp 的环境、普通 thunk、绑定接收者 adapter、函数值、Vec/迭代器指令、dyn 装箱/调用。storage/stride/checked_index 归 B；text_binary/value 系统调用归 E。旧 id_list 字符串键删除，按 fields 向量比较。

```text
owning(type_id usize, state inout Emitter) bool throws Error
value_op(type_id usize, copy bool, state inout Emitter) Value throws Error
copy_value(type_id usize, value Value, state inout Emitter) Value throws Error
destroy_slot(slot usize, type_id usize, state inout Emitter) throws Error
env_record_type(fields Vec[usize], state inout Emitter) string throws Error
env_record_op(fields Vec[usize], copy bool, state inout Emitter) Value throws Error
make_environment(fields Vec[usize], children Vec[Value], state inout Emitter) Value throws Error
function_thunk(symbol usize, state inout Emitter) Value throws Error
bound_adapter(schema hir.FunctionValue, state inout Emitter) Value throws Error
function_value(instruction nativeir.Instruction, args Vec[Value], state inout Emitter) Value throws Error
function_value_info(state Emitter, value_id usize) option.Option[FunctionValueInfo]
trait_box_op(concrete usize, clone bool, state inout Emitter) Value throws Error
trait_method_thunk(dyn_type usize, concrete usize, target usize, state inout Emitter) Value throws Error
trait_vtable(dyn_type usize, concrete usize, methods Vec[usize], state inout Emitter) Value throws Error
thread_bridge(entry_type usize, state inout Emitter) ThreadBridge throws Error
thread_spawn(instruction nativeir.Instruction, args Vec[Value], state inout Emitter) Value throws Error
weak_system_call(instruction nativeir.Instruction, args Vec[Value], state inout Emitter) option.Option[Value] throws Error
emit_value_instruction(instruction nativeir.Instruction, args Vec[Value], state inout Emitter) option.Option[Value] throws Error
```

`emit_value_instruction` 处理 Copy/Move/Take/Destroy/Borrow/Function、VecInit/Read/Set/Slice/Length/Check/ElementAddress、IterationBegin/End/Test/Read/Next、TraitObject/TraitCall；不属于它的 Op 返回 None，已处理的无结果操作返回 Some(i8 0)。B 最后统一登记 instruction.result 的 ValueBinding。function_value 同时写 frame.function_values，E 据此拒绝捕获/绑定函数作为普通 C callback。

C 自有局部函数：c_contains_function、c_vec_rc、c_constant_vec_init、c_function_value_schema、c_emit_vec_check；局部记录 CWeakProjection 及 c_weak_projection/c_weak_vec_slot/c_weak_handle_slot/c_weak_slot_address/c_weak_value/c_function_value_of/c_weak_downgrade_function/object/c_check_weak_option/c_weak_payload/c_weak_upgrade/c_weak_operand。它们不改别组缓存。C 创建环境/Vec/dyn 后调用 D.gc_register_fields/vec；C 的 C resource 分支调用 E.resource_operation。

## D：async_gc.wz

负责所有 promise、coroutine/start/resume/destroy、await/取消/async return、async main 分支、FrameGcOps、gc_ops.inc 和 C resource 可达性扫描。保留普通拥有参数的私有复制、未开始任务清理、重复 await、claimed 位、循环稳定点及 SAFE/critical 区间。

```text
scan_c_resource_flags(state inout Emitter) throws Error
type_owns_c_resource(type_id usize, state inout Emitter) bool throws Error
promise_type(task_type usize, state inout Emitter) PromiseLayout throws Error
coroutine_signature(type_id usize, state inout Emitter) Signature throws Error
declare_async(function_index usize, state inout Emitter) SourceFunction throws Error
async_begin(function_index usize, state inout Emitter) AsyncState throws Error
copy_async_ordinary_parameters(function_index usize, state inout Emitter) throws Error
build_async_frame_gc(function_index usize, owner_slots Vec[usize], state inout Emitter) AsyncFrameGcPlan throws Error
async_publish_and_suspend(function_index usize, state inout Emitter) throws Error
emit_async_terminator(terminator nativeir.Terminator, block_id usize, state inout Emitter) bool throws Error
async_finish(function_index usize, state inout Emitter) throws Error
emit_async_main(entry usize, state inout Emitter) bool throws Error
has_strong_edge(type_id usize, state inout Emitter) bool throws Error
gc_trace_value(type_id usize, address Value, visitor Value, visit_context Value, state inout Emitter) throws Error
gc_register_vec(object Value, element_type usize, state inout Emitter) throws Error
gc_register_fields(object Value, fields Vec[usize], state inout Emitter) throws Error
gc_poll(state inout Emitter) throws Error
gc_loop_safepoints(function nativeir.Function) LoopSafepoints throws Error
gc_collect_loop_exit(state inout Emitter) throws Error
gc_enter_critical(state inout Emitter) Value throws Error
gc_leave_critical(token Value, state inout Emitter) throws Error
gc_enter_safe(state inout Emitter) Value throws Error
gc_leave_safe(token Value, state inout Emitter) throws Error
```

declare_async 登记 d.async_symbols，生成并完成 start/resume/destroy 三个 helper，返回含 coroutine text_index/code 的 SourceFunction 给 B 登记。async_begin 在 coroutine 新建首 entry 块及 final/suspend-return 块，返回 AsyncState；B 保存到 frame.async_state，然后创建 NativeIR 块、slots、参数/incoming 地址。D.copy_async_ordinary_parameters 在 B 已保存参数后调用。D.async_publish_and_suspend 在 slots/active/promise 均完整后生成 FrameGcOps、publish、initial suspend 和首次取消检查，并跳 frame.blocks[0]。B 随后遍历源 CFG；D 处理 async 专有终结符，末尾 async_finish 完成 final/free。只有 B 重置整个 frame，D 只写 frame.async_state。

D 的局部函数：d_promise_slot、d_gc_trace_type、d_gc_visitor_call、d_gc_trace_for_vec/fields、d_gc_invalidate、d_gc_ops_for_vec/fields、d_gc_register_object、d_drop_async_parameters。gc_trace_fields 用 C.env_record_type；FrameGcOps/clear 用 C.value_op；两组因此允许正常相互调用，必须先缓存 helper，再生成函数体。`coro.end` 固定 LLVM22 的 `void(ptr,i1,token)`，使用 token none；不猜 CoroSplit frame 字段。

## E：system.wz / c_ffi.wz

负责完整 system_call（包括 process/network/text/Unicode/math/Vec/Slice/async intrinsic 路由）、C 存储类型/读写/调用、C11 adapter 生成与静态/托管回调。内部 system routes 调 C/D；不改变任何标准库支持范围。

```text
c_ref_type(reference model.CTypeRef, state inout Emitter) string throws Error
c_lower_type(type_id usize, state inout Emitter) string throws Error
to_c_storage(type_id usize, value Value, state inout Emitter) Value throws Error
from_c_storage(type_id usize, value Value, state inout Emitter) Value throws Error
resource_operation(c_type u32, copy bool, state inout Emitter) Value throws Error
callback_bridge(symbol usize, c_type u32, state inout Emitter) Value throws Error
callback_trampoline(symbol usize, c_type u32, state inout Emitter) Value throws Error
registration_dispatcher(c_type u32, userdata usize, state inout Emitter) Value throws Error
borrow_registration(owner Value, state inout Emitter) RegistrationBorrow throws Error
end_registration_borrow(borrow RegistrationBorrow, state inout Emitter) throws Error
c_foreign_call(instruction nativeir.Instruction, args Vec[Value], state inout Emitter) Value throws Error
emit_c_instruction(instruction nativeir.Instruction, args Vec[Value], state inout Emitter) option.Option[Value] throws Error
system_call(instruction nativeir.Instruction, args Vec[Value], state inout Emitter) Value throws Error
value_address(type_id usize, value Value, state inout Emitter) Value throws Error
text_binary(instruction nativeir.Instruction, left Value, right Value, state inout Emitter) Value throws Error
stdlib_variant(type_id usize, tag u32, payload option.Option[Value], state inout Emitter) Value throws Error
finalize_c_adapter(state inout Emitter) throws Error
registration_callback(project model.Project, callable model.CCallableDecl) option.Option[RegistrationCallback] throws Error
registration_close(project model.Project, callable model.CCallableDecl) bool throws Error
```

emit_c_instruction 处理 ForeignCall/CFieldRead/CPtrRead/CPtrWrite/CBufferRead/CBufferWrite，非本组 Op 返回 None；C bool 扩展/截断、out_slot、SAFE、borrow 逆序结束、registration context 和 GC callback token 继续由 E 负责。finalize_c_adapter 先完成所需 dispatcher，再输出 c_ffi.cpp 的 record/callback/buffer/resource/adapter 全部内容到 e.adapter_source。不能继续调用 C++ cffi Generator。

E 局部函数：e_process_system_call、e_network_system_call、e_extra_system_call、e_value_system_call、e_c_callback_source_type；完整 C spelling/collect/record_definition/emit_callable/emit_callback_trampoline/emit_registration_trampoline，以及 adapter/release/retain/callback/registration 的全部符号命名函数，都加 e_ 前缀。

**std::function 成功回调不能照抄为捕获 inout state 的 Wz 闭包。** E 用本文件私有的 VariantJoin 和三个函数替代，避免失败侧提前读未初始化成功 out：

```text
struct EVariantJoin { type_id usize; success_tag u32; done BlockRef;
                    fallback Value; fallback_block BlockRef; phi Value; prefix PrefixRef; }
e_stdlib_result_begin(type_id usize, status Value, state inout Emitter) EVariantJoin throws Error
e_stdlib_option_begin(type_id usize, present Value, state inout Emitter) EVariantJoin throws Error
e_stdlib_finish(join EVariantJoin, payload Value, state inout Emitter) Value throws Error
```

begin 建成功/失败/done 三块，只在失败块构造 Err/None，在 done 保留 phi prefix，最后把 cursor 留在成功块。调用者此时才 load 成功 out 或做其他成功专属生成。finish 构造 Ok/Some，记录 here(state) 这个真正前驱（成功生成可能再分块）、branch done、补 PHI，position done。这仅代替原两个标准库 helper 的回调，不是新的 IR 或 visitor。

## 组装次序与尚未实现的部分

Root 后续内部组装器按顺序调用 declare_runtime_types、D.scan_c_resource_flags、B.declare_source_functions、B.emit_function 全部输入函数、B.emit_main、E.finalize_c_adapter，最后 sections。普通 O0 仅 LLVM verify；async O0 必须 default<O0> 做 coroutine 拆分；O2 default<O2>。固定 opt/llc/Clang/LLD 的执行、退出码、临时目录和最终文件发布由 Root 驱动负责，不放在 state/text。

共享代码不提供上述 B/C/D/E 主体的 stub 或 E_CAPABILITY_UNAVAILABLE 假完成。C++ frontend/helper 只能 bootstrap 这个 Wz 小程序；运行小程序后由 Wz 文本函数产生的 IR 才交 LLVM 工具，旧 helper 不代做这一步。可重跑的测试是 selfhost/tests/nativeemit_text/run.py；本机记录在仓库外 checks/nativeemit-text-root-20261113/woozy-nativeemit-text-k77r0mnd/results.json。O0/O2 生成同一份1497字节IR，经固定LLVM工具验证、生成对象并链接执行，均返回42；此结果只检查共享文本函数。
