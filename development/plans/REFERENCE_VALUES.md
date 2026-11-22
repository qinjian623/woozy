# Reference values / closure / Weak 共享 ABI 提案（修正版）

状态：**Root 已审查的实现约定；共享 HIR 字段已接入，源码闭包与 Weak 尚未实现**。本版只修正 `development/plans/REFERENCE_VALUES.md`；未改 Core、runtime、Root，未构建、未提交。已先读取 `/tmp/closure-abi-independent-20261112/review.json`，并按其中 C1--C7 的实际源码行重新核对；不是照抄审查摘要。源码核对片段保存在 `/tmp/woozy-reference-abi-plan/correction-source-check.txt`。

范围：LANG01（闭包、bound method、函数值）+ 源码 `Weak` 的编译器侧；Native 只承诺 Linux x86_64，Python 只承诺 Python/64。运行库 handle RC 站点和 `std/box` 仍属并行 worker；本文件只发布它们必须遵守的接口。

## 1. Root 已定约束

- Fn 内部 ABI：`Fn{code, LangVec *env}`，16 B；Weak：`Weak{control, code}`，16 B。
- 不新增 `direct_call` opcode。保留现有 `Op::call`、`Exit::try_call` 和 callee-first inputs；按已验证 HIR origin 的 `Call.kind/direct_symbol` 区分直接/间接。直接源码调用使用原 signature；间接 Fn 才拆 `code/env` 并使用带 env 的调用。
- `Weak[T]` 是 builtin；`std/weak` 只提供 `empty`、`downgrade`、`upgrade`，`upgrade` 返回 `std/option.Option[T]`；不增加 `is_alive`/`is_empty`。
- Box、Thread、Mutex 等按 exact canonical 声明识别；Box 只接受 canonical `std/box.Box` 的私有 `cell Vec[T]`，不接受同名/同布局用户 struct，也不递归解包任意单字段 struct。
- `Box` 已由 Root 落在 `stdlib/box/box.wz`：`pub struct Box[T] { cell Vec[T]; }`。worker 直接集成该版本，不另建实现。

## 2. C1--C7 缺陷修正（先列结论，后文给 schema）

| 编号 | 修正要求 | 已核对的源码 |
|---|---|---|
| C1 | move-Self bound 方法每次调用先从 env 取 receiver，按**普通复制**得到独立拥有副本，再把副本交给 move Self；适配器不 take/清空 env 字段。ordinary Self 仍按现有 borrowed ordinary 规则传递；inout Self 禁止绑定为逃逸函数值；不引入 FnOnce，bound Fn 仍可复制、重复调用。receiver 具体类型必须 `ordinary_copyable`；无 retain 的 C resource 及其按值聚合在具体类型确定后拒绝绑定。 | `sema/check.cpp:3239-3251` 要求 Self mode 匹配 receiver marker；`python/emitter.cpp:1840-1851` 把 move owning 参数登记为当前函数拥有值；`langrt.py:844-848` 的 `Vec.get` 不复制。因此不能把 env 字段直接交给 move Self。 |
| C2 | 每个 lambda capture 必须有三个身份：`origin`（原词法声明 SymbolId，跨实例不变）、`local`（该 lambda 专属的只读绑定 SymbolId）、`type`；creator 侧另存实际 `source` SymbolId 和 type。lambda body 的 Read 用 `local`，creator children 用 `source`；比较按有序 `(origin,type)`，不要求两侧 local 相等。泛型 creator 与 lambda 各自用本实例的 renamed 表重命名，origin 不重命名。HIR Read 的源码 occurrence 仍是 origin，验证时必须通过 `HirFunction.captures` 的 origin→local 映射接受这个 local，不能要求 Read.symbol 等于 occurrence。 | `hir.cpp:356-364` 的 Verifier.locals 全局以 SymbolId 为键且一个 Local 只有一个 function/scope；`hir.cpp:6098-6106` 要求 Read 与 Local 同函数；`ownership.cpp:521-524,1543-1580` 的 locals 也是全局 emplace；`sema/check.cpp:5188-5208,5232-5236` 显示 creator/lambda 的 Read/Move 分别 rename。直接复用外层 SymbolId 会被现有 HIR 拒绝。`hir.cpp:4336-4384,4412-4469` 的 template_symbols/instance_records 还要求每个 renamed 源有 definition，capture local 必须走单独受验证分支。 |
| C3 | Weak[Fn] 编码只允许：原 `env==null` 时为 `{null,code}` immortal；原 `env!=null` 且 downgrade 返回 null 时为 `{null,null}` dead；原 `env!=null` 且 control 非 null 时为 `{control,code}`。绝不把“有 env 但 downgrade 失败”编码成 immortal；dead 升级必须返回 None，不能调用捕获 code。 | `runtime/native/weak.hpp:26-29` 与 GC closed-slot 规则：关闭槽后续 downgrade 返回 null。原方案把 null control + 非 null code 解释为 immortal，会复活白色捕获函数。 |
| C4 | 间接 callee 求值并保存为拥有临时值后、任何实参求值前，登记该临时值的 `begin_borrow`；成功和 typed-error 分支都执行现有对应 `end_borrow`。拥有 callee 的释放留到完整表达式结束。Python 同步 `_frame.borrow`。 | `ownership.cpp:795-802` 已为拥有 callee 记录 temporary end_borrow；`ir/lower.cpp:508-539` 只对 args 生成 begin_borrow；`ir/ownership_verify.inc:310-316` 要求 end_borrow 匹配实际 active borrow；`python/emitter.cpp:1000-1017` 同样漏 callee borrow。 |
| C5 | Function 不再是 trivial：`Classifier::support`/`run` 增加 `ValueKind::function`，`needs_drop=true`，`pod_inout=false`；`inout_data_type` 遇到 Function 返回 false，含 Fn 字段的 struct/enum 不再被当 POD inout。不能只改 `needs_drop` helper。 | `ownership.cpp:464-465` 的 needs_drop 与 pod_inout 独立计算；`ownership.cpp:1647-1657` 的 Function 分支目前把作为 member 的 Function 直接判为 trivial inout data。 |
| C6 | 不新增 direct_call。`Op::call` 和 `Exit::try_call` 的 origin 必须指向真实 HIR `Call`；直接调用按 `Call.kind==direct && direct_symbol` 调 `functions.at(symbol)` 并使用原 signature；间接调用从 Fn operand 拆 `code/env` 并使用 env-first signature。直接 try_call 同样不能用裸 callee 指针。 | `include/lang/ir/native.hpp:85-99` 的 throwing call 是 `Exit::try_call`；`ir/lower.cpp:544-552` 不 emit call 指令；`native/emit.cpp:1299-1309` 目前仍把第一个 input 当裸函数指针。 |
| C7 | move func 的 capture Move 是受限合成 Move：只允许出现在 FunctionValue 的 capture child，并独立证明父 Source 是真实 `Unary(move, FunctionLiteral)`、对应 free-variable origin 和 creator binding。普通 Move 保持现有严格源码验证；`move_capture` 必须从真实父 Unary 重算，不信任提交的布尔值。 | `hir.cpp:5680-5724` 要求普通 Move 来自 `Unary(move,...)` 且绑定操作数是 Name；`hir.cpp:5309-5318` 的 `function_value_source` 尚未剥 `move func` 的 Unary。合成 Move 不能伪造普通源码 Move。 |

## 3. 共享 HIR 新增字段（唯一 schema）

```cpp
struct CaptureBinding {          // HirFunction.captures 使用；lambda body 身份
  SymbolId origin;               // 原词法声明，跨实例不变
  SymbolId local;                // 该 lambda 专属只读绑定；由 Checker 新建，不是 Parameter/Binding
  TypeId type;
};
struct CaptureSource {           // FunctionValue.captures 使用；creator 执行来源
  SymbolId origin;               // 必须与目标 CaptureBinding.origin 相同
  SymbolId source;               // creator 中的实际绑定；可为 creator 的 capture.local
  TypeId type;
  bool move;                     // 仅 move func 的真实 owning capture
};
struct BoundReceiver {
  TypeId type;
  ParamMode mode;                // ordinary 或 move；inout 不存入
};
struct FunctionValue {
  SymbolId symbol;
  std::vector<TypeId> type_args;
  std::vector<CaptureSource> captures; // 自由变量，创建顺序
  bool move_capture = false;           // 仅 move func
  std::optional<BoundReceiver> bound;  // bound method 时 captures 为空，唯一 child 是 receiver
};
struct HirFunction {
  // existing fields ...
  std::vector<CaptureBinding> captures;
};
struct LocalLifetime {
  // existing fields ...
  bool read_only_capture = false; // 仅 lambda capture local；函数退出不释放
};
enum class TypeKind { /* existing values unchanged */ Weak };
enum class ValueKind { /* existing values unchanged */ function, weak };
```

规则：

- `FunctionValue` 普通 literal 的 `source` 指向真实 `FunctionLiteral`；`move func` 的节点 `source` 指向真实 `Unary(move, FunctionLiteral)`；bound method 的 source 指向 member/调用来源。`function_value_source` 只对 move func 多剥这一层，并据此重算 `move_capture`。
- 普通 literal 的 children 是每个 `CaptureSource` 在 creator 中执行的来源表达式：普通捕获用 creator 的 `Read{source}`，move 捕获用合成 `Move{binding=source}`；bound method 的唯一 child 是 receiver 表达式。children 数与 captures（或 bound）严格对应。
- `CaptureBinding.local` 由 `Checker::fresh_capture_symbol` 新建：`SymbolKind::value`、名称可沿用 origin、无 definition/declaration，不伪装成源码 Parameter/Binding。每个 sibling lambda 各有自己的 local。
- `ordinary_copyable(const HirModule&, TypeId, stop_token)` 是新增共享 helper（放在 ownership，classify_value 旁）：Vec/Slice/string/handle/Fn/Weak/foreign/pointer/buffer/record/callback 以及普通标量为 true；`CForeignResource` 仅 `CTypeDecl.retain_symbol` 非空为 true；struct/enum 递归，遇 cycle 用有限预算拒绝；Fn 本身为 true（Fn copy 只 retain env，不递归 env 内容）。bound method 和需要复制捕获的普通 `func` 用它拒绝不可复制 receiver/捕获。
- `TypeKind::Weak` 追加在现有枚举末尾，不改变已发布数值；`parse_type` 只接受一个类型参数，Native `type_key`/Python descriptor 的 switch 同步加入 Weak，不能落入 default。Native 只在 64 位下定义 `LangFn`/`LangWeak`；Python 生成器继续要求 `word_bits==64`。

## 4. 捕获、所有权与 lowering

1. **顺序**：source 首次使用、深度优先、包含嵌套 literal body；每个 lambda 都重新按 `origin` 查自己的 `local`，不跨 lambda 复用 local。外层 literal 的遍历把嵌套 literal 的 grandparent 引用也纳入，形成传递捕获；中间 lambda 与内部 lambda 各自有 local，内部 creator source 指向中间 lambda 的某个 capture.local，origin 仍是 grandparent 的原声明。
2. **只读**：lambda body 中 capture local 作为入口已初始化的 ordinary borrowed 值；Sema 在 Name/Place/Move 查 `locals` 前先用该函数的 `capture_by_origin` 把 occurrence origin 换成 capture.local；赋值、`Move`、whole-binding `inout` 拒绝。Vec/Box 等共享对象内容仍按普通别名规则可修改。`HirFunction.captures`、HIR verifier、ownership `gather/run` 和 flow state 都要登记；不在函数退出 drop 环境字段。
3. **普通 func**：creator 的普通 `Read` 产生一次拥有副本（owned Read 的既有 copy/retain）；FunctionValue child 用 `take` 将该结果一次性装入 env，不能再额外 copy/retain。非拥有值直接 take/bitcopy。
4. **move func**：只对真实外层 binding 的 owning capture 生成合成 `Move{binding=source}`，由 FunctionValue child `take`；其他非 owning capture 普通复制。若 move func 要消耗的是外层 lambda 的 capture.local（只读继承值），拒绝，不能把外层只读 capture 当成可 move 的本地 binding。
5. **合成 Move 验证**：普通 Move 仍走 `hir.cpp` 原严格分支。受限分支仅当 Move 是 FunctionValue capture child、父 FunctionValue.move_capture 为真时启用；验证父 source 是真实 Unary(move,FunctionLiteral)、FunctionValue symbol/type_args 对应真实 literal、Move.binding 是 creator source、source 的 origin/type 与该 capture 一致、capture 顺序一致。伪造 move_capture、普通 func 被改成 take、替换 binding、普通 Move 冒充 capture child 均报 `E_IR_INVALID`。
6. **泛型实例**：creator 的 `CaptureSource.source/type` 用 creator 的 renamed/substitution；lambda `CaptureBinding.local/type` 用 lambda 自己的 renamed/substitution；`origin` 保持原声明 SymbolId。不要假定外层与内层 renamed 表相同。`instance_records/template_symbols` 要把 capture local 纳入 `originals/renamed`，但单独验证其为 capture local（有 origin、无 definition/declaration），不能套用普通源码 binding 的 definition 检查；`instance_body`、Sema `transform_node`、`source_lambda_value` 同步处理 local/source 分离。
7. **环境构造**：先完成每个 child 的一次 take/copy，再按 capture 字段顺序初始化单行 Vec；env 只保存值，不保存外层地址。分配/复制失败为 Panic；capture 来源只有 read/move，不引入 typed throw，Panic 路径不作语言清理承诺。env 的 copy/drop/trace 由 capture 字段 TypeId 生成。

## 5. Fn 调用、IR 与 Native/Python

- `LangFn{code,env}`；所有存储/间接 code 统一 C ABI：`R code(void *env, P0..., [value_out,error_out])`。无捕获值用缓存 thunk（忽略 env，调用原函数）；有捕获值 code 就是带 env 的 lambda 入口。Fn 的 copy/drop 只对非 null env retain/release；即使 env 内含不可复制资源，Fn 本身仍可复制（共享 env）。
- `Op::function` 仍产生 Fn 值并保留真实 source SymbolId（C callback 的 `function_value_symbols` 不能记 thunk）。`Op::call` 的 callee operand 仍是 Fn；emitter 读取 origin 对应的真实 HIR Call：direct 调用 `functions.at(direct_symbol)` 并使用原 signature，indirect 从 operand 拆出 code/env 并使用 env-first signature，不得仅凭“类型相同”认定目标。
- `Exit::try_call` 的 `Terminator.origin` 指向真实 HIR Call；emitter 同样按 origin 区分 direct/indirect。直接形式用原 signature + 现有 value/error outputs；间接形式用 env-first code，outputs 仍按现有 throws 顺序。`Instruction.function` 仅作 direct dump 元数据，必须等于 HIR `direct_symbol`；不新增 opcode。
- 间接 callee 是拥有临时值时，lower 在 callee 求值后、args 之前 `begin_borrow`；成功/错误退出用既有 ExitPlan/full-expression end_borrow。Python 在 callee 求值后、args 前 `_frame.borrow`，成功/错误 cleanup 对称 end。callee 的拥有副本留到完整表达式结束。
- `classify_value`：Function 返回 `ValueKind::function`、`needs_drop=true`、`pod_inout=false`；Weak 返回 `ValueKind::weak`、`needs_drop=true`、`pod_inout=false`；`inout_data_type` 对 Function/Weak 返回 false。Vec[Fn]、struct/enum[Fn] 的 copy/drop/trace 递归处理。Python `emit_type` 对 Function/Weak 发 `_rt.ValueType('function')`/`'weak'`；Native `value_op` 对 Function/Weak 处理 env/control retain/release。
- C callback：`check_c_callback`、HIR verifier、IR/native emitter 都显式拒绝 `captures` 或 `bound`；只接受非捕获静态函数源。callback_bridge/trampoline 继续用真实 source C ABI，不把 env thunk 当 callback symbol。
- thread/task：thread context 第一个字段是 `LangFn`，第二个是 `A`；entry 调 `fn.code(fn.env,arg)`。`thread_bridge` 的 copy/drop 无论 A 是否 owning 都要生成并分别处理 Fn env 与 A；当前实现只检查 `owning(argument)`，会漏掉标量 A 对应的捕获型入口。`std/task` 的 entry/payload 随 Fn 值 copy/drop/trace 自动变化，仍需真实源码测试；thread runtime ABI 本身不改。
- 常量：`ConstantValue::FunctionValue` 只允许 captures 为空、无 bound、move_capture=false；Native 发 `{thunk,null}`，Python 发裸 `_fN`。const 中捕获 literal 明确拒绝。

## 6. Bound method 与 Weak/Box

**Bound method**：普通 Self 的 `value.method` 在创建时按普通规则复制 receiver；`(move value).method` 则显式 move 到 env。两种形式都要求 receiver 满足 `ordinary_copyable`，函数值类型为方法 signature 去掉首个 Self。Native 适配器签名为 `R adapter(void *env, remaining args...)`：ordinary Self 直接按 borrowed 参数传入 env 字段；move Self 每次调用先执行 `copy_value(receiver_type, env_field)`，把独立副本交给 move Self，绝不 take 或清空 env 字段；typed error 由方法自身的 move 参数清理逻辑处理。Python 的 `_boundN` 同样先调用 `ops.copy(_env.get(0)[0])` 再转发，不缓存可消耗副本。绑定 inout Self 报 `E_CALL_ARGUMENT`/capability；bound Fn 可复制、可重复调用，不引入 FnOnce。

**Weak 类型/API**：builtin `Weak[T]`；`std/weak.empty[T]() Weak[T]`、`downgrade[T](T) Weak[T]`、`upgrade[T](Weak[T]) Option[T]`。没有 `is_alive`/`is_empty`；空/失效通过 `upgrade` 返回 `None` 观察。copy/drop 为普通值操作。

`LangWeak{control, code}` 规则：

| 原 Fn 状态 | downgrade | 存储 | upgrade |
|---|---|---|---|
| env == null | 不调用 Weak | `{null, code}` immortal | `Some(Fn{code,null})`，永不失效 |
| env != null, downgrade 返回 null | GC closed/失败 | `{null,null}` dead | `None`，不得调用 code |
| env != null, control 有效 | 成功 | `{control, code}` | control 升级成功则 `Some(Fn{code,env})`；否则 `None` |

`LangWeak` copy/drop 仅对非 null control 调 `lang_weak_retain/release`；Weak 不是 GC 强边。Vec/handle/Box 的 control 升级成功后直接移交新强引用给结果，不再配对 retain。`downgrade/upgrade/empty` 走 `std/weak` 的 builtin SystemOperation，不新增 IR opcode；lower/emitter 按 `Weak[T].type_args[0]` 的 canonical target 选择 projection。Native `lower_type(Weak)` 是 16 B；Python 映射现有 `_rt.Weak`/`ValueType('weak')`，`upgrade` 转 `Option`。

**canonical 目标**（按解析后的声明 SymbolId + logical module + exported name，不接受同布局用户类型）：

| 源码类型 | 私有目标 |
|---|---|
| `std/thread.Thread` | `state __woozy_Thread` |
| `std/sync.Mutex` / `Condvar` / `AtomicI64` | `state __woozy_Mutex/Condvar/AtomicI64` |
| `std/ipc.Memory` | `handle __woozy_IpcHandle` |
| `std/box.Box[T]` | `cell Vec[T]`（唯一特许私有字段） |
| Vec / Function | 直接目标；Fn 按上表投影 code/env |

拒绝 Slice、string、fd 包装、foreign Python/C 对象、普通 struct/enum，以及 `std/ipc.Mutex/Condvar/Semaphore` 这类多字段 wrapper。运行库 worker 负责给 `LangVec`/`LangHandle` 追加 `LangWeakControl *weak_control`、构造置空、最终 release detach、GC invalidate；本提案不改这些 RC 站点。环境 Vec 和 Box 共用同一 slot。

## 7. 按文件实现任务

**共享前端/HIR（必须同一原子变更）**

- `include/lang/hir/hir.hpp`：加入上述 `CaptureBinding`、`CaptureSource`、`BoundReceiver`、`FunctionValue`、`HirFunction.captures`、`LocalLifetime.read_only_capture`、`TypeKind::Weak`、`ValueKind::function/weak`；`NodeData` 不新增 node kind。
- `src/modules/resolve.cpp`：prelude 增加 builtin `Weak`；确认 `move func` 仍解析为 `Unary(move, FunctionLiteral)`，不改 AST schema。
- `src/sema/check.cpp`：新增 `fresh_capture_symbol`、`closure_spec`、`capture_symbols`；在 `register_function/check_body/expression(FunctionLiteral)/Unary(move)/Name/Member/Move/place` 中建立并验证 local/source；`method_value` 产生 bound；`typed_call` 检查 move/ordinary/inout receiver；`check_c_callback`、`c_foreign_call` 拒绝 captures/bound；`parse_type` 增加 Weak 并校验 canonical Weak target；`constant` 拒绝捕获；`instantiate/transform_node` 分开 rename origin/local/source 并替换 type。
- `src/hir/hir.cpp`：`function_value_source/source_lambda_value` 支持 move func source 与 capture 列表；`instance_body/instance_records/template_symbols` 用 creator 与 lambda 各自 renamed 表并特判无 definition 的 capture local；`ownership_sources` 允许 Read.symbol 为 occurrence origin 对应的 capture.local，并增加受限合成 Move 分支、独立重算 move_capture；`Verifier::locals` 登记 capture.local；C callback、函数/作用域/类型支持、constant 检查同步。
- `src/hir/ownership.cpp/.hpp`：`Classifier::support/run` 加 Function branch；`needs_drop(Function)=true`；`inout_data_type(Function)=false`；新增 `ordinary_copyable`；`Analysis::run/gather/expr_records` 登记 captures、入口 flow state、FunctionValue children take、owned callee end_borrow、`LocalLifetime.read_only_capture`。
- `src/ir/lower.cpp`、`include/lang/ir/native.hpp`、`src/ir/verify.cpp`、`src/ir/ownership_verify.inc`：不新增 opcode；`Op::function` 保存 Fn 元数据；`Op::call`/`Exit::try_call` 按 origin direct/indirect；capture env 构造、capture slot 入口加载、callee begin_borrow；`Instruction.function` direct dump 校验；dump/negative verifier 同步。

**Native**

- `src/backend/native/emit.cpp`：`lower_type(Function/Weak)`；`signature()` 的 env-first 变体；thunk/cache；capture prologue；direct/indirect 普通与 try_call；C callback 仍用真实 source symbol；constant Function；bound adapter；Weak 降级/升级；Function/Weak value_op。
- `src/backend/native/value_ops.inc`：Function 读 env 并 retain/release；Weak 读 control 并 retain/release；Vec[Fn]/aggregate[Fn] 递归已由现有字段逻辑覆盖。
- `src/backend/native/concurrency.inc`：thread context 首字段 LangFn；entry 调 `code(env,arg)`；copy/drop 无条件生成并处理 Fn env 与 A 两字段。

**Python**

- `src/backend/python/emitter.cpp`：`function_tree` 对 captures 增加 `_env` 参数、`_row=_env.get(0); _sX=_row[i]` 和 `available/named`；`FunctionValue` 创建包装 env；`emit_type` 的 function/weak descriptor；普通 Read owned 一次 copy + FunctionValue child take；direct/throws indirect 调用；callee `_frame.borrow`；bound adapter 每次 move-Self copy；outlining capture 列表包含 capture local。`runtime/python/langrt.py` 现有 FunctionValue/Weak/ValueType 已核对可直接复用，本任务不改 runtime。

**运行库/其他 owner**

- runtime worker：只负责 Vec/Handle weak slot、最后 release 和 Weak fork hooks。Fn/Weak 值操作归 Native emitter；GC 自动登记与线程暂停另做。Box 直接使用现有 `stdlib/box/box.wz`；std/weak 只发布上述三个函数。源码 Weak 和 Native GC 继续保持未完成。

## 8. 测试与原生 IR 负测试

源码/后端测试（全部 planned；本轮未运行）：

- Native O0/O2 + Python/64：普通 func 捕获 copy、move func move、sibling lambda、grandparent 传递、嵌套 creator、泛型两实例（origin 稳定、local 分别 rename）、逃逸/容器/struct/enum field、return/typed throw/defer。
- bound method：ordinary Self 连续调用、move Self 连续调用与复制 bound Fn 后分别调用、env 字段保留、typed error/defer、最终析构；无 retain C resource 或含它的按值 receiver 在具体类型确定后拒绝；inout Self 绑定拒绝。Python `Vec.get` 不复制这一事实要有专门回归。
- C1 负例：move-Self adapter 若 take env 字段，第二次调用/环境析构应失败；实现必须用每调用 copy。
- Weak：Vec、Fn（captured/immortal/dead）、Thread/Mutex/Condvar/Atomic/Ipc Memory、Box；`downgrade` 关闭槽后返回 dead 编码，`upgrade` None；无 `is_empty` 测试；canonical 同名/同布局用户 struct 拒绝。
- C callback：非捕获静态函数通过；captures/bound 拒绝；C fixture 回调与 no-libc 链接回归。
- thread/task：capturing entry + scalar A 必须有 env copy/drop；任务 payload 捕获、并发 join、现有 `native.threadpool`/`native.concurrency`。
- 32/64：只为 Linux x86_64/Python64 定义新布局；32 位 word_bits 遇到 Fn/Weak 应明确 `E_CAPABILITY_UNAVAILABLE`，不定义 16B 32 位 ABI，不新增平台。

原生 IR 负测试（`tests/native/ir` 或新 reference-values IR 测试，必须同时覆盖普通与 throws）：

- direct `Op::call`/`Exit::try_call` origin 指向 `Call.kind=function_value` 却按 direct 调用；direct 的 `Instruction.function` 与 HIR `direct_symbol` 不符；间接 callee operand 与 HIR `Call.callee` 不符。
- 直接 throws 调用仍按裸函数指针/错误 signature；间接 throws 未拆 code/env；try_call 输出参数顺序/env 位置错误。
- FunctionValue capture 数量/顺序/type/origin/local/source 不匹配；move_capture 与真实 Unary 不符；普通 func 的 child 被改成 take；合成 Move 出现在非 FunctionValue child；普通 Move 失去源码 marker。
- capture slot 未在入口初始化；Read 用了别的 lambda local；Read.symbol 等于 occurrence origin 而非该 origin 的 lambda local；creator source 指向未来/不在作用域的 binding；泛型实例 origin 被 rename 或 local/source 用错 renamed 表；capture local 被伪造 definition 冒充普通 Binding；instance renamed map 漏/多 capture local。
- owned callee 缺 begin_borrow、success/error 缺 end_borrow、callee 被提前 drop、双重 retain/双重 take。
- Function 作为 POD inout 仍被接受；Fn field 的 struct/enum 整体 inout 被接受；Vec[Fn]/aggregate[Fn] 的 copy/drop/trace 漏字段。
- Weak[Fn] `{null,code}` 由有 env 的失败 downgrade 伪造；dead `{null,null}` 升级出 Fn；Weak canonical 目标用错声明/解包用户单字段 struct；Weak 升级后再多一次 retain。
- C callback 接受 captures/bound；thread context 在 scalar A 时漏 Fn env copy/drop。

通过标准：上述负例均报 `E_IR_INVALID` 或明确的 capability 诊断；正例在真实源码的 O0/O2 与 Python 下运行通过；不能把单个 function-value 用例通过写成 LANG01/MEM02 完成。

## Root 补充：字段交接与未具体化类型

- 所有非空环境统一为恰好一行 record，bound receiver 也是 record 的第 0 字段。Python 入口先 `_row = _env.get(0)`，第 i 个捕获绑定为 `_row[i]`；这是借用，不额外复制。拥有值的后续 Read 才按该字段类型复制。
- 源码 FunctionValue 生成的 `Op::function` 必须设置 `origin` 为该 HIR FunctionValue，`inputs` 按它的 children 顺序。验证器和 emitter 从 origin 取得 captures/bound；不另加一份可能不一致的 IR capture schema。常量和直接调用生成的无捕获函数值继续分别按原来源验证。
- `ir::Function.symbol` 关联 `HirFunction.captures`，捕获 slot 的 `Slot.binding` 等于 capture.local；其 parameter 为空、inout 为 false，入口已初始化并借用环境字段，不能当拥有局部值在函数退出时 drop。不新增 environment/capture_index 字段。
- TypeParameter 或其他 dependent 类型在模板阶段不作不可复制诊断，具体实例再检查 `ordinary_copyable`。普通 func 不得因 T 尚未知就被拒绝。
- move func 的移动意图保存在父节点 move_capture；T 未具体化时不能永久将 CaptureSource.move 判为 false。实例化后按实际类型重新决定：标量复制不消耗原绑定，拥有值移动并消耗原绑定。若捕获 child 因此由 Read 变为 Move，instance_body 只为经过源码验证的这一捕获构造接受转换；普通节点的 kind/source 验证保持原规则。

本版修正了独立检查 C1–C7，以及复核 R1/I1/I2。它授权实现者按唯一 schema 分别修改前端、Native 和 Python；运行与负测试完成前不得把功能标为完成。

阶段 A 已编译这些共用字段，并保留对非默认捕获/绑定、capture lifetime 及 Weak 类型的明确拒绝。独立检查中 46 项前端、11 项静态和旧函数值执行用例通过，Root 新 schema 及相关 12 项也已通过。后续前端需逐项补齐真实源码与所有权验证后再移除对应拒绝，不能只关闭检查。
