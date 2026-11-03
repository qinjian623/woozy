#pragma once
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
  Error
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
}; // noncapturing function identity
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
}; // child: integer; checked conversion (Panic on overflow)
enum class CalleeKind { direct, function_value };
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
  // children: [callee when function_value], then args in source order.
};
struct Block {};               // children: statements in execution order; Unit
struct ExpressionStatement {}; // child: value expression; Unit
struct If {}; // children: bool condition, then Block, optional else Block; Unit
struct For {}; // children: init (Binding/Assign/ExpressionStatement/Noop), bool condition,
               // step (Assign/ExpressionStatement/Noop), body Block; Unit.
               // For owns a child scope; body owns a further child scope.
struct Noop {};
struct Return {
  SymbolId function;
}; // optional child: result, Unit node type
struct Break {
  HirNodeId loop;
}; // no children, target must be enclosing For
struct Continue {
  HirNodeId loop;
};
struct Unsupported {
  std::string feature;
}; // includes Error/NeutralBracket etc.
using NodeData =
    std::variant<UnitLiteral, BoolLiteral, IntegerLiteral, FloatLiteral,
                 StringLiteral, Read, Place, FunctionValue, Binding, Assign,
                 Unary, Binary, ShortCircuit, IntegerConvert, Call, Block,
                 ExpressionStatement, If, For, Noop, Return, Break, Continue,
                 Unsupported>;
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
  unsupported
};
struct HirNode {
  TypeId type;
  Source source;
  ScopeId scope;
  std::vector<HirNodeId> children;
  NodeData data;
  NodeKind kind() const { return static_cast<NodeKind>(data.index()); }
};
struct Parameter {
  SymbolId symbol;
  std::string
      label; // direct-call label, empty for discard _, not part of Function TypeId
};
struct HirFunction {
  SymbolId symbol;
  TypeId signature;
  std::vector<Parameter> parameters;
  HirNodeId body;
  ScopeId scope;
  Source source;
};
struct HirModule {
  std::shared_ptr<const modules::ResolvedProject> resolved;
  ModuleId identity;
  TypeContext types;
  Config config;
  std::vector<HirFunction> functions;
  std::vector<HirNode> nodes;
  std::vector<Scope> scopes;
};
class VerifiedHirModule;
PhaseResult<VerifiedHirModule> verify(const HirModule &, std::stop_token = {});
class VerifiedHirModule {
  std::shared_ptr<const HirModule> module_;
  explicit VerifiedHirModule(std::shared_ptr<const HirModule> m)
      : module_(std::move(m)) {}
  friend PhaseResult<VerifiedHirModule> verify(const HirModule &,
                                               std::stop_token);

public:
  const HirModule &module() const { return *module_; }
  const Config &config() const { return module_->config; }
  std::string_view version() const { return contract_version; }
};
// verify deep-copies mutable builder data, resolved/project and AST arenas
// before checking. A retained mutable builder/ResolvedProject/AstFile alias
// cannot change the published module. SourceSnapshot is an immutable support
// object. E_IR_INVALID is an internal error; unsupported constructs are
// rejected with that code too (their source diagnostics belong to the checker).
std::string dump(const TypeContext &);
std::string dump(const HirModule &);
std::string dump(const VerifiedHirModule &);
} // namespace lang::hir
