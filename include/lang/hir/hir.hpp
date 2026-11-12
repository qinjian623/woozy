#pragma once
#include "lang/hir/system_operation.hpp"
#include "lang/modules/resolve.hpp"
#include <variant>

namespace lang::hir {
inline constexpr std::string_view contract_version = "compiler-dev/1";
enum class TypeKind {
  Unit,
  Bool,
  SignedInt,
  UnsignedInt,
  Float,
  String,
  Isize,
  Usize,
  Function,
  NominalStruct,
  NominalEnum,
  TypeParameter,
  ForeignOpaque,
  Error,
  Vec,
  ForeignCallable,
  // C ABI values. Pointer/buffer carry their pointee/element in type_args[0]
  // and only their write permission in bits. Record/resource/callback carry
  // the 1-based Project::c_types index in c_type and never a declaration or
  // binding ID. These are distinct from Python's ForeignOpaque/Callable.
  CForeignPointer,
  CForeignBuffer,
  CForeignRecord,
  CForeignResource,
  CForeignCallback,
  CForeignOpaque,
  // Compiler-private owning runtime handles. These are canonical leaves: no
  // parameters, declaration, binding or type arguments. Public std modules
  // wrap them in ordinary nominal structs.
  ThreadHandle,
  MutexHandle,
  CondvarHandle,
  AtomicI64Handle,
  IpcHandle,
  // Borrowed view over a Vec. type_args[0] is the element type. The inline
  // 24-byte {owner,start,length} value retains+pins its owner while live.
  // Appended to preserve the published numeric TypeKind values.
  Slice
};
enum class ParamMode { ordinary, inout, move };
struct ParameterType {
  ParamMode mode;
  TypeId type;
  bool operator==(const ParameterType &) const = default;
};
struct Type {
  TypeKind kind = TypeKind::Error;
  unsigned bits = 0;
  SymbolId declaration = SymbolId{0};
  ForeignBindingId binding = ForeignBindingId{0};
  std::vector<ParameterType> parameters;
  TypeId result = TypeId{0};
  std::optional<TypeId> throws_type;
  std::vector<TypeId>
      type_args; // nominal declaration arguments, in declaration order
  uint32_t c_type = 0; // C ABI declaration index, 1-based; zero for others
  bool operator==(const Type &) const = default;
};
// TypeId 0 is invalid. All other IDs index their owning context, starting at 1.
// Canonical IDs are deterministic for a deterministic sequence of intern calls.
// Nominal/type-parameter identity is declaration; foreign identity also uses
// binding.
class TypeContext {
  std::vector<Type> entries_;

public:
  TypeId intern(Type); // invalid type shape/reference throws invalid_argument
  const Type *find(TypeId) const;
  size_t size() const { return entries_.size(); }
};
TypeId system_operation_signature(TypeContext &, SystemOperation,
                                 const std::vector<TypeId> &args = {});
struct HirModule;
TypeId system_operation_signature(TypeContext &, const HirModule &, SystemOperation,
                                 const std::vector<TypeId> &args = {});
enum class Backend { native, python };
struct Config {
  Backend backend = Backend::native;
  unsigned word_bits = 64;
  bool operator==(const Config &) const = default;
};
unsigned bit_width(const Type &,
                   const Config &); // target width, never host sizeof
struct Source {
  modules::NodeRef node{ModuleId{0}, 0, AstNodeId{0}};
  SourceSpan span{SourceSnapshotId{0}, 0, 0};
};
// ScopeId, HirNodeId and SymbolId are zero-based, unlike TypeId.
struct Scope {
  SymbolId function;
  std::optional<ScopeId> parent;
  Source source;
};
enum class UnaryOp { negate, logical_not, bit_not };
enum class BinaryOp {
  add,
  subtract,
  multiply,
  divide,
  remainder,
  shift_left,
  shift_right,
  bit_and,
  bit_or,
  bit_xor,
  equal,
  not_equal,
  less,
  less_equal,
  greater,
  greater_equal
};
enum class LogicalOp { and_, or_ };
struct UnitLiteral {};
struct BoolLiteral {
  bool value = false;
};
struct IntegerLiteral {
  std::string decimal; // canonical mathematical value: 0 or -?[1-9][0-9]*
  uint64_t bits = 0;   // target two's-complement bit pattern, zero extended
};
struct FloatLiteral {
  uint64_t bits = 0;
}; // exact IEEE f32/f64 bits
struct StringLiteral {
  std::string utf8;
};
struct Read {
  SymbolId symbol;
};
struct Place {
  SymbolId symbol;
}; // whole writable binding, for assignment or an inout argument
struct FunctionValue {
  SymbolId symbol;
  std::vector<TypeId> type_args;
}; // noncapturing function identity
struct ConstantValue;
struct ConstantAggregate {
  std::optional<SymbolId> variant;   // absent for struct; active enum variant
  std::vector<ConstantValue> fields; // declaration/active-payload order
};
struct ConstantValue {
  TypeId type;
  std::variant<UnitLiteral, BoolLiteral, IntegerLiteral, FloatLiteral,
               StringLiteral, FunctionValue, ConstantAggregate>
      data;
};
struct ConstantLiteral {
  ConstantValue value;
}; // pure immutable value, no executable children; verified against source
struct Binding {
  SymbolId symbol;
  bool writable = false;
}; // children: initializer
struct Assign {
  std::optional<BinaryOp> compound;
}; // children: place, rhs; Unit
struct Unary {
  UnaryOp op;
}; // child: operand
struct Binary {
  BinaryOp op;
}; // children: lhs, rhs
struct ShortCircuit {
  LogicalOp op;
}; // children: lhs, rhs; rhs is conditional
struct IntegerConvert {
}; // child: source; node.type is the numeric destination. Allowed pairs are
   // integer->integer (checked range), float->integer (truncate towards zero,
   // then range; NaN/infinity/out-of-range is a Panic), integer->float (one
   // direct round to the destination precision, RNE), and float->float for
   // every f32/f64 pair. f32->f64 widening is exact; f64->f32 narrows once
   // with RNE, overflowing finite values to signed infinity and never using
   // the float->integer Panic rule.
enum class CalleeKind {
  direct,
  function_value,
  foreign,   // Python schema-v1 host call
  system,
  c_foreign  // C schema-v2 native-library call
};
struct CallArg {
  uint32_t source_index = 0, parameter_index = 0;
  std::optional<std::string> label;
  ParamMode mode = ParamMode::ordinary;
  HirNodeId expression;
};
struct Call {
  CalleeKind kind = CalleeKind::direct;
  TypeId signature;
  std::optional<SymbolId> direct_symbol;
  std::optional<HirNodeId> callee;
  std::vector<CallArg> args; // source order, never sorted by parameter_index
  std::vector<TypeId> type_args;
  std::optional<ForeignBindingId> binding;
  std::optional<uint32_t> c_callable; // C ABI callable index, 1-based
  std::vector<uint32_t> omitted; // declaration indices, sorted; no value slots
  std::optional<SystemOperation> system;
  // children: [callee when function_value], then args in source order.
};
struct Block {};               // children: statements in execution order; Unit
struct ExpressionStatement {}; // child: value expression; Unit
struct If {}; // children: bool condition, then Block, optional else Block; Unit
struct For {}; // children: init (Binding/Assign/ExpressionStatement/Noop), bool
               // condition, step (Assign/ExpressionStatement/Noop), body Block;
               // Unit. For owns a child scope; body owns a further child scope.
struct Noop {};
struct Return {
  SymbolId function;
}; // optional child: result, Unit node type
struct Break {
  HirNodeId loop;
}; // no children, target must be enclosing For/ForIn
struct Continue {
  HirNodeId loop;
};
struct Unsupported {
  std::string feature;
}; // includes Error/NeutralBracket etc.
// Appended payloads preserve NodeKind 0..23 and use the existing arena.
struct StructInit {
  std::vector<CallArg> fields;
};
struct EnumInit {
  SymbolId variant;
  std::vector<CallArg> payload;
};
struct FieldRead {
  SymbolId field;
}; // child: receiver value
struct FieldPlace {
  SymbolId field;
}; // child: writable base place
// A read of a field in a C ABI record. The C declaration and field are
// resolved from frozen Project metadata, not from a source symbol.
struct CFieldRead {
  uint32_t c_type = 0; // 1-based Project::c_types index
  uint32_t field = 0;  // declaration-order field index
};
using PatternLiteral =
    std::variant<BoolLiteral, IntegerLiteral, FloatLiteral, StringLiteral>;
struct Pattern {
  TypeId type;
  Source source;
  std::optional<SymbolId> variant;
  std::optional<SymbolId> binding;
  std::vector<Pattern> payload;
  // Appended to preserve the original five-field aggregate initialization.
  // Exactly one of variant, binding, or literal may identify a pattern; a
  // wildcard has none. Literal patterns never carry payload or a binding.
  std::optional<PatternLiteral> literal;
}; // no variant/binding/literal: wildcard
struct Arm {
  Pattern pattern;
  ScopeId scope;
  HirNodeId body;
  Source source;
};
struct Switch {
  std::vector<Arm> arms;
  bool exhaustive = false;
};
// Complete static trait application identity: the real declaration plus the
// ordered type arguments. An unparameterized trait uses an empty argument
// list; it is still one bound/application, not a multi-constraint list.
struct TraitRef {
  SymbolId declaration;
  std::vector<TypeId> args; // declaration order
  bool operator==(const TraitRef &) const = default;
};

struct ConstraintCall {
  TraitRef trait;
  SymbolId method;
  TypeId receiver, signature;
  std::vector<CallArg>
      args; // receiver first, then explicit args in source order
}; // template only; becomes a direct Call in concrete instances
struct Catch {
  TypeId error_type;
  SymbolId binding; // real AstKind::Catch declaration, including discard _
  ScopeId scope;
  HirNodeId body;
  Source source;
};
struct Try {
  std::vector<Catch> catches;
}; // children: protected Block, then handler Blocks in source order
struct Defer {
  ScopeId owner_scope; // nearest lexical Block; body is the only child
  // Sorted unique external locals; read when executed, never value snapshots.
  std::vector<SymbolId> references;
};
struct Throw {}; // child: error value, saved before executing any cleanup
struct Move {
  std::optional<SymbolId> binding; // otherwise one newly computed child
};
struct VecInit {};
struct IndexRead {};
struct IndexPlace {
}; // protected receiver/index, never a borrowed element address
struct VecSlice {
  std::optional<HirNodeId> start, end;
};
struct ForIn {
  SymbolId element;
  ScopeId body_scope;
  HirNodeId body;
};
// Declared host access. Read kinds are value expressions; place kinds are
// writable assignment targets. No access is inferred from host reflection.
// A null access keeps the original module-value/method/callable read.
enum class ForeignAccessKind {
  property_read,
  property_place,
  index_read,
  index_place,
  slice_read,
  slice_place
};
struct ForeignRead {
  ForeignBindingId binding;
  std::optional<ForeignAccessKind> access;
  std::optional<HirNodeId> start, end; // slice children, source order
  // A method/callable has one receiver child. Module exports have none, and
  // an access read/place has exactly the declared receiver/key/endpoint
  // children.
};
// Typed projection of Project.foreign_bindings, at the same zero-based index.
// Identity, host attributes, parameter restrictions, ownership and effects are
// read from the corresponding frozen Project declaration, not import aliases.
struct ForeignBinding {
  ForeignBindingId binding;
  TypeId type; // opaque type, exported value, or internal ForeignCallable
  std::optional<TypeId> signature, owner;
  TypeId read_error; // lang.py.Error, distinct from diagnostic TypeKind::Error
  bool operator==(const ForeignBinding &) const = default;
};
enum class OwnerKind {
  binding,
  temporary,
  return_output,
  error_output,
  iteration
};
struct OwnerRef {
  OwnerKind kind;
  uint64_t identity; // SymbolId for binding; HirNodeId otherwise
  bool operator==(const OwnerRef &) const = default;
};
enum class CleanupKind {
  save_output,
  end_borrow,
  destroy_if_live,
  defer_,
  end_iteration
};
struct CleanupStep {
  CleanupKind kind;
  OwnerRef owner;
  TypeId type{0}; // zero only for defer/iteration registration
  bool operator==(const CleanupStep &) const = default;
};
enum class UseKind { borrow, copy, take, inout };
struct ValueUse {
  HirNodeId consumer;
  uint32_t operand;
  UseKind kind;
  std::optional<SymbolId> root;
  TypeId type;
  std::optional<SymbolId>
      destination; // pattern/catch/per-iteration acquisition
  bool operator==(const ValueUse &) const = default;
};
struct Temporary {
  HirNodeId producer, full_expression;
  TypeId type;
  bool operator==(const Temporary &) const = default;
};
struct LocalLifetime {
  SymbolId symbol;
  ScopeId scope;
  TypeId type;
  uint32_t order;
  ParamMode mode; // ordinary parameters borrowed; local bindings use move
  bool operator==(const LocalLifetime &) const = default;
};
struct FullExpression {
  HirNodeId node;
  std::vector<CleanupStep> actions;
  bool operator==(const FullExpression &) const = default;
};
struct CallLifetime {
  HirNodeId node;
  // Runs at the call's successful return, before the parent expression.
  // Error returns use the matching ExitPlan.actions instead.
  std::vector<CleanupStep> on_return;
  bool operator==(const CallLifetime &) const = default;
};
struct Ownership {
  std::vector<ValueUse> uses;
  std::vector<Temporary> temporaries;
  std::vector<LocalLifetime> locals;
  std::vector<FullExpression> full_expressions;
  std::vector<CallLifetime> calls;
  bool operator==(const Ownership &) const = default;
};
enum class ExitKind { normal, return_, break_, continue_, error };
struct ScopeExit {
  std::optional<ScopeId> scope; // resume in parent, or normal function end
  bool operator==(const ScopeExit &) const = default;
};
struct FunctionExit {
  SymbolId function;
  bool operator==(const FunctionExit &) const = default;
};
struct JumpExit {
  HirNodeId node; // For/ForIn or Switch; kind distinguishes break from continue
  bool operator==(const JumpExit &) const = default;
};
struct CatchTarget {
  HirNodeId try_node;
  uint32_t index;
  bool operator==(const CatchTarget &) const = default;
};
using ExitTarget = std::variant<ScopeExit, FunctionExit, JumpExit, CatchTarget>;
struct ExitPlan {
  ExitKind kind;
  std::optional<TypeId> error_type; // only the operation's own typed failure
  ExitTarget target;
  std::vector<ScopeId> exited_scopes; // inner to outer, excludes resumed scope
  std::vector<HirNodeId> defers;      // active registrations in execution order
  bool saves_value = false; // value/error evaluated and saved before defers
  std::vector<CleanupStep> actions;
  bool operator==(const ExitPlan &) const = default;
};
using NodeData = std::variant<
    UnitLiteral, BoolLiteral, IntegerLiteral, FloatLiteral, StringLiteral, Read,
    Place, FunctionValue, Binding, Assign, Unary, Binary, ShortCircuit,
    IntegerConvert, Call, Block, ExpressionStatement, If, For, Noop, Return,
    Break, Continue, Unsupported, StructInit, EnumInit, FieldRead, FieldPlace,
    Switch, ConstraintCall, Try, Defer, Throw, Move, VecInit, IndexRead,
    IndexPlace, VecSlice, ForIn, ForeignRead, ConstantLiteral, CFieldRead>;
enum class NodeKind {
  unit_literal,
  bool_literal,
  integer_literal,
  float_literal,
  string_literal,
  read,
  place,
  function_value,
  binding,
  assign,
  unary,
  binary,
  short_circuit,
  integer_convert,
  call,
  block,
  expression_statement,
  if_,
  for_,
  noop,
  return_,
  break_,
  continue_,
  unsupported,
  struct_init,
  enum_init,
  field_read,
  field_place,
  switch_,
  constraint_call,
  try_,
  defer_,
  throw_,
  move,
  vec_init,
  index_read,
  index_place,
  vec_slice,
  for_in,
  foreign_read,
  constant_literal,
  c_field_read
};
struct HirNode {
  TypeId type;
  Source source;
  ScopeId scope;
  std::vector<HirNodeId> children;
  NodeData data;
  std::vector<TypeId> errors; // sorted unique errors escaping this subtree;
                              // Panic and ordinary Result data are excluded
  // Error exits always have a plan. Other exits have a plan when defer or
  // owning cleanup is required; trivial exits are determined by the tree.
  std::vector<ExitPlan> exits;
  NodeKind kind() const { return static_cast<NodeKind>(data.index()); }
};
struct Parameter {
  SymbolId symbol;
  std::string label; // direct-call label, empty for discard _, not part of
                     // Function TypeId
};
struct HirFunction {
  SymbolId symbol;
  TypeId signature;
  std::vector<Parameter> parameters;
  HirNodeId body;
  ScopeId scope;
  Source source;
};
struct GenericParam {
  SymbolId symbol;
  std::optional<TraitRef> trait;
  Source source;
};
struct FieldDecl {
  SymbolId symbol;
  TypeId type;
  bool exported;
  Source source;
};
struct VariantDecl {
  SymbolId symbol;
  std::vector<FieldDecl> payload;
  Source source;
};
struct NominalDecl {
  SymbolId symbol;
  TypeKind kind;
  std::vector<GenericParam> params;
  std::vector<FieldDecl> fields;
  std::vector<VariantDecl> variants;
  Source source;
};
struct MethodPrototype {
  SymbolId symbol;
  TypeId signature;
  std::vector<Parameter> parameters;
  Source source;
};
struct TraitDecl {
  SymbolId symbol, self;
  std::vector<GenericParam> params; // real declared parameters, declaration
                                   // order; unconstrained in this batch
  std::vector<MethodPrototype> methods;
  Source source;
};
struct ImplDecl {
  std::vector<GenericParam> params;
  std::optional<TraitRef> trait;
  SymbolId self_parameter;
  TypeId self_type;
  std::vector<SymbolId> methods;
  Source source;
};
struct FunctionTemplate {
  HirFunction function;
  std::vector<GenericParam> params;
};
struct FunctionInstance {
  SymbolId symbol, origin;
  std::vector<TypeId> type_args;
  std::vector<std::pair<SymbolId, SymbolId>> renamed;
  Config target;
};
using Substitutions = std::vector<std::pair<SymbolId, TypeId>>;
TypeId substitute(TypeId, const Substitutions &, TypeContext &);
// True when the two type patterns admit a common finite substitution.
bool type_patterns_overlap(TypeId, TypeId, const TypeContext &);
struct HirModule {
  std::shared_ptr<const modules::ResolvedProject> resolved;
  ModuleId identity;
  TypeContext types;
  Config config;
  std::vector<HirFunction> functions;
  std::vector<HirNode> nodes;
  std::vector<Scope> scopes;
  std::vector<NominalDecl> nominals;
  std::vector<TraitDecl> traits;
  std::vector<ImplDecl> impls;
  std::vector<FunctionTemplate> templates;
  std::vector<FunctionInstance> instances;
  Ownership ownership;
  std::vector<ForeignBinding> foreign_bindings;
};
enum class ValueKind {
  trivial,
  vec,
  structure,
  enumeration,
  dependent,
  unsupported,
  foreign_reference,
  foreign_callable,
  foreign_error,
  // C ABI values. Pointers/buffers/records/callbacks are non-owning; a
  // resource is an owning reference released through its binding.
  c_pointer,
  c_buffer,
  c_record,
  c_resource,
  c_callback,
  // An owning compiler-private thread/mutex/condvar/atomic handle. The source
  // std modules expose it only through ordinary nominal wrappers.
  runtime_handle,
  string,
  // A borrowed Slice inline value. It owns one strong Vec reference and one
  // iteration pin; copy and drop use the private 24-byte runtime ABI.
  slice
};
struct ValueField {
  std::optional<SymbolId> variant;
  SymbolId field;
  TypeId type;
};
struct ValueOps {
  ValueKind kind = ValueKind::unsupported;
  bool needs_drop = false, pod_inout = false;
  std::vector<ValueField> fields; // declaration order, enum grouped by variant
  std::string reason;
};
// All returned TypeIds belong to module.types. No private layout IDs escape.
ValueOps classify_value(const HirModule &, TypeId);
// Returns a declaration with infinite value layout, including unused templates.
// Function signatures and unused type arguments do not contribute value fields.
std::optional<SymbolId> infinite_value_layout(const HirModule &,
                                              std::stop_token = {});
PhaseResult<std::monostate> verify_types(const HirModule &,
                                         std::stop_token = {});
// Checks only executable functions, reachable nodes and recursively used types.
// Target-width restrictions of code generation remain at backend entry points.
PhaseResult<std::monostate> check_backend_capabilities(const HirModule &,
                                                       std::stop_token = {});
class VerifiedHirModule;
enum class VerifyMode { backend, static_only };
PhaseResult<VerifiedHirModule> verify(const HirModule &, std::stop_token = {},
                                     VerifyMode = VerifyMode::backend);
class VerifiedHirModule {
  std::shared_ptr<const HirModule> module_;
  explicit VerifiedHirModule(std::shared_ptr<const HirModule> m)
      : module_(std::move(m)) {}
  friend PhaseResult<VerifiedHirModule> verify(const HirModule &,
                                               std::stop_token, VerifyMode);

public:
  const HirModule &module() const { return *module_; }
  const Config &config() const { return module_->config; }
  std::string_view version() const { return contract_version; }
};
// verify deep-copies mutable builder data, resolved/project and AST arenas
// before checking. A retained mutable builder/ResolvedProject/AstFile alias
// cannot change the published module. SourceSnapshot is an immutable support
// object. Malformed HIR (including Unsupported payloads) is E_IR_INVALID;
// well-typed operations unavailable in the selected backend are
// E_CAPABILITY_UNAVAILABLE.
std::string dump(const TypeContext &);
std::string dump(const HirModule &);
std::string dump(const VerifiedHirModule &);
} // namespace lang::hir
