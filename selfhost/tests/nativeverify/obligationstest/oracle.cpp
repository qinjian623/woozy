// C++ oracle for the required exit/call obligation fixture.
//
// Builds the same hand-written HIR control tree as trace.wz and prints
// lang::ir::detail::RequiredExits from src/ir/obligations.hpp.  This is a
// test-only comparison program: it constructs HirModule builder data directly
// (no VerifiedHirModule) and never runs the production compiler.
#include "ir/obligations.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace lang;
using namespace lang::hir;

namespace {
constexpr std::uint64_t TRACE_FUNCTION = 7;
constexpr std::uint64_t N_BODY = 0;
constexpr std::uint64_t N_CALL = 1;
constexpr std::uint64_t N_DEFER_CALL = 2;
constexpr std::uint64_t N_DEFER_CALL_BODY = 3;
constexpr std::uint64_t N_RETURN = 4;
constexpr std::uint64_t N_RETURN_VALUE = 5;
constexpr std::uint64_t N_DEFER_A = 6;
constexpr std::uint64_t N_DEFER_A_BODY = 7;
constexpr std::uint64_t N_NESTED_CALL = 8;
constexpr std::uint64_t N_DEFER_B = 9;
constexpr std::uint64_t N_DEFER_C = 10;
constexpr std::uint64_t N_DEFER_B_BODY = 11;
constexpr std::uint64_t N_DEFER_C_BODY = 12;
constexpr std::uint64_t N_FORIN = 13;
constexpr std::uint64_t N_ITERABLE = 14;
constexpr std::uint64_t N_NEXT = 15;
constexpr std::uint64_t N_FOR_BODY = 16;
constexpr std::uint64_t N_IF = 17;
constexpr std::uint64_t N_CONDITION = 18;
constexpr std::uint64_t N_CONTINUE_BLOCK = 19;
constexpr std::uint64_t N_BREAK_BLOCK = 20;
constexpr std::uint64_t N_CONTINUE = 21;
constexpr std::uint64_t N_BREAK = 22;
constexpr std::uint64_t N_SWITCH = 23;
constexpr std::uint64_t N_SCRUTINEE = 24;
constexpr std::uint64_t N_ARM1_BODY = 25;
constexpr std::uint64_t N_ARM2_BODY = 26;
constexpr std::uint64_t N_ARM1_RETURN = 27;
constexpr std::uint64_t N_ARM2_BREAK = 28;
constexpr std::uint64_t N_ARM1_VALUE = 29;
constexpr std::uint64_t N_TRY = 30;
constexpr std::uint64_t N_TRY_BODY = 31;
constexpr std::uint64_t N_THROW = 32;
constexpr std::uint64_t N_THROW_VALUE = 33;
constexpr std::uint64_t N_CATCH_BODY = 34;
constexpr std::uint64_t N_AWAIT = 35;
constexpr std::uint64_t N_FUTURE = 36;

Source source0() {
  Source value;
  value.node = modules::NodeRef{};
  value.span = SourceSpan{SourceSnapshotId{1}, 0, 0};
  return value;
}

Type simple(TypeKind kind, unsigned bits) {
  Type value;
  value.kind = kind;
  value.bits = bits;
  return value;
}

Type with_declaration(TypeKind kind, std::uint64_t declaration) {
  Type value;
  value.kind = kind;
  value.declaration = SymbolId{declaration};
  return value;
}

ExitPlan plan(ExitKind kind, std::optional<TypeId> error, ExitTarget target,
              std::vector<HirNodeId> defers, bool saves_value) {
  ExitPlan value;
  value.kind = kind;
  value.error_type = error;
  value.target = std::move(target);
  value.defers = std::move(defers);
  value.saves_value = saves_value;
  return value;
}

ExitTarget function_target() {
  return ExitTarget{FunctionExit{SymbolId{TRACE_FUNCTION}}};
}

ExitTarget jump_target(std::uint64_t node) {
  return ExitTarget{JumpExit{HirNodeId{node}}};
}

ExitTarget scope_target() {
  return ExitTarget{ScopeExit{std::nullopt}};
}

ExitTarget catch_target(std::uint64_t node, std::uint32_t index) {
  return ExitTarget{CatchTarget{HirNodeId{node}, index}};
}

HirNode make_node(TypeId type, std::vector<std::uint64_t> children,
                  NodeData data, std::vector<ExitPlan> exits = {}) {
  HirNode node;
  node.type = type;
  node.source = source0();
  node.scope = ScopeId{0};
  for (auto child : children)
    node.children.push_back(HirNodeId{child});
  node.data = std::move(data);
  node.exits = std::move(exits);
  return node;
}

NodeData block_data() { return NodeData{Block{}}; }
NodeData read_data(std::uint64_t symbol) {
  return NodeData{Read{SymbolId{symbol}}};
}
NodeData call_data(TypeId signature) {
  Call call;
  call.kind = CalleeKind::direct;
  call.signature = signature;
  return NodeData{std::move(call)};
}

std::vector<ExitPlan> one(ExitPlan value) {
  std::vector<ExitPlan> result;
  result.push_back(std::move(value));
  return result;
}

std::vector<ExitPlan> two(ExitPlan first, ExitPlan second) {
  std::vector<ExitPlan> result;
  result.push_back(std::move(first));
  result.push_back(std::move(second));
  return result;
}

Pattern variant_pattern(std::uint64_t variant) {
  Pattern pattern;
  pattern.type = TypeId{7};
  pattern.source = source0();
  pattern.variant = SymbolId{variant};
  return pattern;
}

Arm arm(std::uint64_t variant, std::uint64_t body) {
  Arm value;
  value.pattern = variant_pattern(variant);
  value.scope = ScopeId{0};
  value.body = HirNodeId{body};
  value.source = source0();
  return value;
}

std::vector<HirNode> fixture_nodes() {
  std::vector<HirNode> nodes;
  nodes.push_back(make_node(TypeId{1}, {N_CALL, N_RETURN, N_FORIN, N_SWITCH,
                                        N_TRY, N_AWAIT},
                            block_data()));
  nodes.push_back(make_node(
      TypeId{2}, {}, call_data(TypeId{4}),
      one(plan(ExitKind::error, TypeId{3}, function_target(),
               {HirNodeId{N_DEFER_CALL}}, true))));
  nodes.push_back(make_node(
      TypeId{1}, {N_DEFER_CALL_BODY},
      NodeData{Defer{ScopeId{0}, {}}}));
  nodes.push_back(make_node(TypeId{1}, {}, block_data()));
  nodes.push_back(make_node(
      TypeId{1}, {N_RETURN_VALUE}, NodeData{Return{SymbolId{TRACE_FUNCTION}}},
      one(plan(ExitKind::return_, std::nullopt, function_target(),
               {HirNodeId{N_DEFER_A}, HirNodeId{N_DEFER_B}}, true))));
  nodes.push_back(make_node(TypeId{2}, {},
                            NodeData{IntegerLiteral{"1", 1}}));
  nodes.push_back(make_node(
      TypeId{1}, {N_DEFER_A_BODY}, NodeData{Defer{ScopeId{0}, {}}}));
  nodes.push_back(make_node(TypeId{1}, {N_NESTED_CALL}, block_data()));
  nodes.push_back(make_node(
      TypeId{2}, {}, call_data(TypeId{4}),
      one(plan(ExitKind::error, TypeId{3}, function_target(),
               {HirNodeId{N_DEFER_C}}, true))));
  nodes.push_back(make_node(
      TypeId{1}, {N_DEFER_B_BODY}, NodeData{Defer{ScopeId{0}, {}}}));
  nodes.push_back(make_node(
      TypeId{1}, {N_DEFER_C_BODY}, NodeData{Defer{ScopeId{0}, {}}}));
  nodes.push_back(make_node(TypeId{1}, {}, block_data()));
  nodes.push_back(make_node(TypeId{1}, {}, block_data()));
  IteratorFor iterator;
  iterator.cursor = SymbolId{61};
  iterator.option = SymbolId{62};
  iterator.next = HirNodeId{N_NEXT};
  iterator.iterator = TypeId{2};
  iterator.element = TypeId{2};
  iterator.some = SymbolId{63};
  iterator.none = SymbolId{64};
  iterator.some_field = SymbolId{65};
  nodes.push_back(make_node(
      TypeId{1}, {N_ITERABLE, N_FOR_BODY},
      NodeData{ForIn{SymbolId{60}, ScopeId{0}, HirNodeId{N_FOR_BODY},
                     iterator}},
      one(plan(ExitKind::normal, std::nullopt, scope_target(), {}, false))));
  nodes.push_back(make_node(TypeId{2}, {}, read_data(1)));
  nodes.push_back(make_node(TypeId{2}, {}, read_data(2)));
  nodes.push_back(make_node(TypeId{1}, {N_IF}, block_data()));
  nodes.push_back(make_node(
      TypeId{1}, {N_CONDITION, N_CONTINUE_BLOCK, N_BREAK_BLOCK},
      NodeData{If{}}));
  nodes.push_back(make_node(TypeId{5}, {}, NodeData{BoolLiteral{true}}));
  nodes.push_back(make_node(TypeId{1}, {N_CONTINUE}, block_data()));
  nodes.push_back(make_node(TypeId{1}, {N_BREAK}, block_data()));
  nodes.push_back(make_node(
      TypeId{1}, {}, NodeData{Continue{HirNodeId{N_FORIN}}},
      one(plan(ExitKind::continue_, std::nullopt, jump_target(N_FORIN), {},
               false))));
  nodes.push_back(make_node(
      TypeId{1}, {}, NodeData{Break{HirNodeId{N_FORIN}}},
      one(plan(ExitKind::break_, std::nullopt, jump_target(N_FORIN), {},
               false))));
  Switch switch_value;
  switch_value.arms.push_back(arm(23, N_ARM1_BODY));
  switch_value.arms.push_back(arm(24, N_ARM2_BODY));
  switch_value.exhaustive = true;
  nodes.push_back(make_node(TypeId{1}, {N_SCRUTINEE},
                            NodeData{std::move(switch_value)}));
  nodes.push_back(make_node(TypeId{7}, {}, read_data(3)));
  nodes.push_back(make_node(TypeId{1}, {N_ARM1_RETURN}, block_data()));
  nodes.push_back(make_node(TypeId{1}, {N_ARM2_BREAK}, block_data()));
  nodes.push_back(make_node(
      TypeId{1}, {N_ARM1_VALUE}, NodeData{Return{SymbolId{TRACE_FUNCTION}}},
      one(plan(ExitKind::return_, std::nullopt, function_target(), {}, true))));
  nodes.push_back(make_node(
      TypeId{1}, {}, NodeData{Break{HirNodeId{N_SWITCH}}},
      one(plan(ExitKind::break_, std::nullopt, jump_target(N_SWITCH), {},
               false))));
  nodes.push_back(make_node(TypeId{2}, {},
                            NodeData{IntegerLiteral{"2", 2}}));
  Try try_value;
  try_value.catches.push_back(
      Catch{TypeId{3}, SymbolId{50}, ScopeId{0}, HirNodeId{N_CATCH_BODY},
            source0()});
  nodes.push_back(make_node(TypeId{1}, {N_TRY_BODY},
                            NodeData{std::move(try_value)}));
  nodes.push_back(make_node(TypeId{1}, {N_THROW}, block_data()));
  nodes.push_back(make_node(
      TypeId{1}, {N_THROW_VALUE}, NodeData{Throw{}},
      one(plan(ExitKind::error, TypeId{3}, catch_target(N_TRY, 0), {}, true))));
  nodes.push_back(make_node(TypeId{3}, {}, read_data(4)));
  nodes.push_back(make_node(TypeId{1}, {}, block_data()));
  nodes.push_back(make_node(
      TypeId{1}, {N_FUTURE}, NodeData{Await{}},
      two(plan(ExitKind::error, TypeId{3}, function_target(), {}, false),
          plan(ExitKind::cancel, std::nullopt, function_target(), {}, false))));
  nodes.push_back(make_node(TypeId{6}, {}, read_data(5)));
  return nodes;
}

void print_optional(std::optional<TypeId> value) {
  if (value)
    std::cout << value->value;
  else
    std::cout << "-";
}

void print_context(const ir::detail::Obligations::Context &context) {
  for (size_t index = 0; index < context.size(); ++index) {
    if (index)
      std::cout << ",";
    const auto &[origin, kind, error, defer_node] = context[index];
    std::cout << origin.value << ":" << static_cast<unsigned>(kind) << ":";
    print_optional(error);
    std::cout << ":" << defer_node.value;
  }
}

void run() {
  TypeContext context;
  TypeId unit = context.intern(simple(TypeKind::Unit, 0));
  TypeId int_type = context.intern(simple(TypeKind::SignedInt, 32));
  TypeId error_type =
      context.intern(with_declaration(TypeKind::NominalEnum, 20));
  Type function_type;
  function_type.kind = TypeKind::Function;
  function_type.parameters.push_back(
      ParameterType{ParamMode::ordinary, int_type});
  function_type.result = int_type;
  function_type.throws_type = error_type;
  TypeId function_id = context.intern(std::move(function_type));
  TypeId boolean = context.intern(simple(TypeKind::Bool, 0));
  Type task_type;
  task_type.kind = TypeKind::Task;
  task_type.type_args = {unit, error_type};
  TypeId task_id = context.intern(std::move(task_type));
  TypeId enum_type =
      context.intern(with_declaration(TypeKind::NominalEnum, 21));
  if (unit.value != 1 || int_type.value != 2 || error_type.value != 3 ||
      function_id.value != 4 || boolean.value != 5 || task_id.value != 6 ||
      enum_type.value != 7) {
    std::cout << "fixture error\n";
    return;
  }
  HirModule module;
  module.types = context;
  module.config = Config{Backend::native, 64};
  NominalDecl error_nominal;
  error_nominal.symbol = SymbolId{20};
  error_nominal.kind = TypeKind::NominalEnum;
  error_nominal.source = source0();
  error_nominal.variants.push_back(VariantDecl{SymbolId{21}, {}, source0()});
  error_nominal.variants.push_back(VariantDecl{SymbolId{22}, {}, source0()});
  module.nominals.push_back(std::move(error_nominal));
  NominalDecl switch_nominal;
  switch_nominal.symbol = SymbolId{21};
  switch_nominal.kind = TypeKind::NominalEnum;
  switch_nominal.source = source0();
  switch_nominal.variants.push_back(VariantDecl{SymbolId{23}, {}, source0()});
  switch_nominal.variants.push_back(VariantDecl{SymbolId{24}, {}, source0()});
  module.nominals.push_back(std::move(switch_nominal));
  module.nodes = fixture_nodes();
  HirFunction function;
  function.symbol = SymbolId{TRACE_FUNCTION};
  function.signature = TypeId{4};
  function.body = HirNodeId{N_BODY};
  function.scope = ScopeId{0};
  function.source = source0();
  module.functions.push_back(function);
  ir::detail::RequiredExits required(module);
  auto obligations = required.run(function);
  std::cout << "exits=" << obligations.exits.size() << "\n";
  for (const auto &[key, count] : obligations.exits) {
    const auto &[key_context, origin, kind, error] = key;
    std::cout << "exit ctx=";
    print_context(key_context);
    std::cout << " origin=" << origin.value
              << " kind=" << static_cast<unsigned>(kind) << " error=";
    print_optional(error);
    std::cout << " count=" << count << "\n";
  }
  std::cout << "calls=" << obligations.calls.size() << "\n";
  for (const auto &[key, count] : obligations.calls) {
    std::cout << "call ctx=";
    print_context(key.first);
    std::cout << " origin=" << key.second.value << " count=" << count << "\n";
  }
}
} // namespace

int main() {
  run();
  return 0;
}
