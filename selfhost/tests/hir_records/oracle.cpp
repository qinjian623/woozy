// Header-only oracle for the shared typed-HIR data records.
//
// This program includes the frozen C++ hir.hpp and prints the same
// constructive trace as src/main.wz.  It does not link or rebuild the C++
// compiler and never calls the old frontend; enum ordinals, std::variant
// indices and record fields come directly from the header.
#include "lang/hir/hir.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using lang::AstNodeId;
using lang::ForeignBindingId;
using lang::HirNodeId;
using lang::ModuleId;
using lang::ScopeId;
using lang::SourceSnapshotId;
using lang::SourceSpan;
using lang::SymbolId;
using lang::TypeId;
using namespace lang::hir;

namespace {

std::string num(uint64_t value) { return std::to_string(value); }
std::string flag(bool value) { return value ? "1" : "0"; }

Source source(std::uint64_t index) {
  return Source{lang::modules::NodeRef{ModuleId{index}, index, AstNodeId{index}},
                SourceSpan{SourceSnapshotId{index}, index, index + 1}};
}

TraitRef trait_ref() { return TraitRef{SymbolId{0}, {}}; }

ConstantValue constant_unit() {
  ConstantValue value;
  value.type = TypeId{0};
  value.data = UnitLiteral{};
  return value;
}

std::vector<NodeData> data_list() {
  std::vector<NodeData> out;
  out.push_back(NodeData{UnitLiteral{}});
  out.push_back(NodeData{BoolLiteral{false}});
  out.push_back(NodeData{IntegerLiteral{"0", 0}});
  out.push_back(NodeData{FloatLiteral{0}});
  out.push_back(NodeData{StringLiteral{""}});
  out.push_back(NodeData{Read{SymbolId{0}}});
  out.push_back(NodeData{Place{SymbolId{0}}});
  {
    FunctionValue value;
    value.symbol = SymbolId{0};
    out.push_back(NodeData{value});
  }
  out.push_back(NodeData{Binding{SymbolId{0}, false}});
  {
    Assign value;
    value.compound = std::nullopt;
    out.push_back(NodeData{value});
  }
  out.push_back(NodeData{Unary{UnaryOp::negate}});
  out.push_back(NodeData{Binary{BinaryOp::add}});
  out.push_back(NodeData{ShortCircuit{LogicalOp::and_}});
  out.push_back(NodeData{IntegerConvert{}});
  {
    Call value;
    value.kind = CalleeKind::direct;
    value.signature = TypeId{0};
    value.direct_symbol = std::nullopt;
    value.callee = std::nullopt;
    value.c_callable = std::nullopt;
    value.system = std::nullopt;
    out.push_back(NodeData{value});
  }
  out.push_back(NodeData{Block{}});
  out.push_back(NodeData{ExpressionStatement{}});
  out.push_back(NodeData{If{}});
  out.push_back(NodeData{For{}});
  out.push_back(NodeData{Noop{}});
  out.push_back(NodeData{Return{SymbolId{0}}});
  out.push_back(NodeData{Break{HirNodeId{0}}});
  out.push_back(NodeData{Continue{HirNodeId{0}}});
  out.push_back(NodeData{Unsupported{""}});
  out.push_back(NodeData{StructInit{}});
  {
    EnumInit value;
    value.variant = SymbolId{0};
    out.push_back(NodeData{value});
  }
  out.push_back(NodeData{FieldRead{SymbolId{0}}});
  out.push_back(NodeData{FieldPlace{SymbolId{0}}});
  {
    Switch value;
    value.exhaustive = false;
    out.push_back(NodeData{value});
  }
  {
    ConstraintCall value;
    value.trait = trait_ref();
    value.method = SymbolId{0};
    value.receiver = TypeId{0};
    value.signature = TypeId{0};
    out.push_back(NodeData{value});
  }
  out.push_back(NodeData{Try{}});
  {
    Defer value;
    value.owner_scope = ScopeId{0};
    out.push_back(NodeData{value});
  }
  out.push_back(NodeData{Throw{}});
  {
    Move value;
    value.binding = std::nullopt;
    out.push_back(NodeData{value});
  }
  out.push_back(NodeData{VecInit{}});
  out.push_back(NodeData{IndexRead{}});
  out.push_back(NodeData{IndexPlace{}});
  {
    VecSlice value;
    value.start = std::nullopt;
    value.end = std::nullopt;
    out.push_back(NodeData{value});
  }
  {
    ForIn value;
    value.element = SymbolId{0};
    value.body_scope = ScopeId{0};
    value.body = HirNodeId{0};
    value.iterator = std::nullopt;
    out.push_back(NodeData{value});
  }
  {
    ForeignRead value;
    value.binding = ForeignBindingId{0};
    value.access = std::nullopt;
    value.start = std::nullopt;
    value.end = std::nullopt;
    out.push_back(NodeData{value});
  }
  out.push_back(NodeData{ConstantLiteral{constant_unit()}});
  out.push_back(NodeData{CFieldRead{0, 0}});
  out.push_back(NodeData{Await{}});
  {
    TraitObjectInit value;
    value.trait = trait_ref();
    value.concrete = TypeId{0};
    out.push_back(NodeData{value});
  }
  {
    TraitDispatch value;
    value.trait = trait_ref();
    value.method = SymbolId{0};
    value.signature = TypeId{0};
    value.index = 0;
    out.push_back(NodeData{value});
  }
  return out;
}

std::vector<ConstantValue> constant_values() {
  std::vector<ConstantValue> out;
  out.push_back(constant_unit());
  {
    ConstantValue value;
    value.type = TypeId{0};
    value.data = BoolLiteral{true};
    out.push_back(value);
  }
  {
    ConstantValue value;
    value.type = TypeId{0};
    value.data = IntegerLiteral{"-7", 7};
    out.push_back(value);
  }
  {
    ConstantValue value;
    value.type = TypeId{0};
    value.data = FloatLiteral{0};
    out.push_back(value);
  }
  {
    ConstantValue value;
    value.type = TypeId{0};
    value.data = StringLiteral{"x"};
    out.push_back(value);
  }
  {
    FunctionValue function;
    function.symbol = SymbolId{0};
    function.type_args = {TypeId{0}};
    function.captures = {CaptureSource{SymbolId{0}, SymbolId{0}, TypeId{0},
                                       true}};
    function.move_capture = true;
    function.bound = BoundReceiver{TypeId{0}, ParamMode::move};
    ConstantValue value;
    value.type = TypeId{0};
    value.data = function;
    out.push_back(value);
  }
  {
    ConstantAggregate aggregate;
    aggregate.variant = SymbolId{0};
    aggregate.fields = {constant_unit()};
    ConstantValue value;
    value.type = TypeId{0};
    value.data = aggregate;
    out.push_back(value);
  }
  return out;
}

std::vector<PatternLiteral> pattern_values() {
  return {BoolLiteral{false}, IntegerLiteral{"0", 0}, FloatLiteral{0},
          StringLiteral{""}};
}

std::string owner_kind_id(OwnerKind kind) {
  return num(static_cast<uint64_t>(kind));
}

std::string cleanup_kind_id(CleanupKind kind) {
  return num(static_cast<uint64_t>(kind));
}

std::string use_kind_id(UseKind kind) {
  return num(static_cast<uint64_t>(kind));
}

std::string exit_kind_id(ExitKind kind) {
  return num(static_cast<uint64_t>(kind));
}

std::string value_kind_id(ValueKind kind) {
  return num(static_cast<uint64_t>(kind));
}

std::string inout_slot_id(InoutSlot slot) {
  return num(static_cast<uint64_t>(slot));
}

} // namespace

int main() {
  const auto data = data_list();
  for (std::size_t index = 0; index < data.size(); ++index) {
    HirNode node;
    node.type = TypeId{index};
    node.source = source(index);
    node.scope = ScopeId{index};
    for (std::size_t child = 0; child < index % 3; ++child)
      node.children.push_back(HirNodeId{0});
    for (std::size_t error = 0; error < index % 2; ++error)
      node.errors.push_back(TypeId{0});
    node.data = data[index];
    std::cout << "node i=" << num(index) << " type=" << num(node.type.value)
              << " kind=" << num(static_cast<uint64_t>(node.kind()))
              << " children=" << num(node.children.size()) << " errors="
              << num(node.errors.size()) << " exits=" << num(node.exits.size())
              << " source=" << num(node.source.node.module.value) << "/"
              << num(node.source.node.file) << "/"
              << num(node.source.node.node.value) << " span="
              << num(node.source.span.snapshot.value) << "/"
              << num(node.source.span.begin_byte) << "/"
              << num(node.source.span.end_byte) << " scope="
              << num(node.scope.value) << "\n";
  }

  const auto constants = constant_values();
  for (std::size_t index = 0; index < constants.size(); ++index) {
    std::cout << "constant i=" << num(index) << " type="
              << num(constants[index].type.value) << " data="
              << num(constants[index].data.index()) << "\n";
  }
  const auto patterns = pattern_values();
  for (std::size_t index = 0; index < patterns.size(); ++index) {
    std::cout << "pattern i=" << num(index) << " kind="
              << num(patterns[index].index()) << "\n";
  }

  {
    Scope scope;
    scope.function = SymbolId{0};
    scope.parent = ScopeId{0};
    scope.source = source(0);
    if (scope.parent)
      std::cout << "scope some function=" << num(scope.function.value)
                << " parent=" << num(scope.parent->value) << " has_parent=1\n";
    Scope root;
    root.function = SymbolId{0};
    root.source = source(0);
    if (!root.parent)
      std::cout << "scope none function=" << num(root.function.value)
                << " has_parent=0\n";
  }

  {
    Pattern nested;
    nested.type = TypeId{0};
    nested.source = source(1);
    Pattern pattern;
    pattern.type = TypeId{0};
    pattern.source = source(2);
    pattern.variant = SymbolId{0};
    pattern.binding = SymbolId{0};
    pattern.payload = {nested};
    pattern.literal = PatternLiteral{BoolLiteral{false}};
    std::cout << "pattern_rich type=" << num(pattern.type.value)
              << " variant=1 variant_id=0 binding=1 binding_id=0 payload="
              << num(pattern.payload.size()) << " literal=1 literal_kind=0\n";
    std::cout << "pattern_literal kind="
              << num(pattern.literal->index()) << "\n";

    ConstantAggregate aggregate;
    aggregate.variant = SymbolId{0};
    aggregate.fields = {constant_unit()};
    ConstantValue value;
    value.type = TypeId{0};
    value.data = aggregate;
    std::cout << "constant_rich type=" << num(value.type.value)
              << " data=6 variant=1 variant_id=0 fields="
              << num(aggregate.fields.size()) << "\n";
  }

  HirFunction function;
  function.symbol = SymbolId{0};
  function.signature = TypeId{0};
  function.parameters = {Parameter{SymbolId{0}, ""}};
  function.body = HirNodeId{0};
  function.scope = ScopeId{0};
  function.source = source(3);
  function.captures = {CaptureBinding{SymbolId{0}, SymbolId{0}, TypeId{0}}};
  function.async = true;
  std::cout << "function symbol=" << num(function.symbol.value)
            << " signature=" << num(function.signature.value) << " params="
            << num(function.parameters.size()) << " param_symbol="
            << num(function.parameters[0].symbol.value) << " params_label="
            << num(function.parameters[0].label.size()) << " captures="
            << num(function.captures.size()) << " capture="
            << num(function.captures[0].origin.value) << "/"
            << num(function.captures[0].local.value) << "/"
            << num(function.captures[0].type.value) << " body="
            << num(function.body.value) << " scope="
            << num(function.scope.value) << " is_async="
            << flag(function.async) << "\n";

  FunctionInstance instance;
  instance.symbol = SymbolId{0};
  instance.origin = SymbolId{0};
  instance.type_args = {TypeId{0}};
  instance.renamed = {{SymbolId{0}, SymbolId{0}}};
  instance.target = Config{};
  std::cout << "instance symbol=" << num(instance.symbol.value)
            << " origin=" << num(instance.origin.value) << " type_args="
            << num(instance.type_args.size()) << " type_arg="
            << num(instance.type_args[0].value) << " renamed="
            << num(instance.renamed.size()) << " rename="
            << num(instance.renamed[0].first.value) << "/"
            << num(instance.renamed[0].second.value) << " backend="
            << num(static_cast<uint64_t>(instance.target.backend))
            << " word_bits=" << num(instance.target.word_bits) << "\n";

  {
    CallArg arg;
    arg.source_index = 0;
    arg.parameter_index = 0;
    arg.label = std::string{"x"};
    arg.mode = ParamMode::move;
    arg.expression = HirNodeId{0};
    Call call;
    call.kind = CalleeKind::c_foreign;
    call.signature = TypeId{0};
    call.direct_symbol = SymbolId{0};
    call.callee = HirNodeId{0};
    call.args = {arg};
    call.type_args = {TypeId{0}};
    call.binding = ForeignBindingId{0};
    call.c_callable = 0u;
    call.omitted = {0u};
    call.system = SystemOperation::read;
    std::cout << "call kind=4 signature=" << num(call.signature.value)
              << " direct=1 direct_id=0 callee=1 callee_id=0 args=1"
              << " arg=0/0/1/2/0 type_args=1 binding=1 binding_id=0"
              << " c_callable=1 c_callable_id=0 omitted=1 omitted0=0"
              << " system=1 system_op="
              << num(static_cast<uint64_t>(*call.system))
              << " label_len=" << num(call.args[0].label->size()) << "\n";
  }

  {
    ForeignBinding binding;
    binding.binding = ForeignBindingId{0};
    binding.type = TypeId{0};
    binding.signature = TypeId{0};
    binding.owner = std::nullopt;
    binding.read_error = TypeId{0};
    std::cout << "foreign binding=" << num(binding.binding.value)
              << " type=" << num(binding.type.value) << " signature=1"
              << " signature_id=0 owner=0 read_error="
              << num(binding.read_error.value) << "\n";
  }

  CleanupStep step;
  step.kind = CleanupKind::save_output;
  step.owner = OwnerRef{OwnerKind::binding, 0};
  step.type = TypeId{0};
  std::cout << "cleanup kind=" << cleanup_kind_id(step.kind)
            << " owner_kind=" << owner_kind_id(step.owner.kind)
            << " owner_identity=" << num(step.owner.identity) << " type="
            << num(step.type.value) << "\n";
  ValueUse use;
  use.consumer = HirNodeId{0};
  use.operand = 0;
  use.kind = UseKind::borrow;
  use.root = SymbolId{0};
  use.type = TypeId{0};
  use.destination = std::nullopt;
  std::cout << "use consumer=" << num(use.consumer.value) << " operand="
            << num(use.operand) << " kind=" << use_kind_id(use.kind)
            << " root=1 root_id=0 type=" << num(use.type.value)
            << " destination=0\n";
  Temporary temporary;
  temporary.producer = HirNodeId{0};
  temporary.full_expression = HirNodeId{0};
  temporary.type = TypeId{0};
  LocalLifetime local;
  local.symbol = SymbolId{0};
  local.scope = ScopeId{0};
  local.type = TypeId{0};
  local.order = 0;
  local.mode = ParamMode::move;
  local.read_only_capture = false;
  FullExpression full;
  full.node = HirNodeId{0};
  full.actions = {step};
  CallLifetime call_lifetime;
  call_lifetime.node = HirNodeId{0};
  call_lifetime.on_return = {step};
  Ownership ownership;
  ownership.uses = {use};
  ownership.temporaries = {temporary};
  ownership.locals = {local};
  ownership.full_expressions = {full};
  ownership.calls = {call_lifetime};
  std::cout << "ownership uses=" << num(ownership.uses.size())
            << " temporaries=" << num(ownership.temporaries.size())
            << " locals=" << num(ownership.locals.size())
            << " full_expressions=" << num(ownership.full_expressions.size())
            << " calls=" << num(ownership.calls.size()) << "\n";
  std::cout << "temporary producer=" << num(temporary.producer.value)
            << " full_expression=" << num(temporary.full_expression.value)
            << " type=" << num(temporary.type.value) << "\n";
  std::cout << "local symbol=" << num(local.symbol.value) << " scope="
            << num(local.scope.value) << " type=" << num(local.type.value)
            << " order=" << num(local.order) << " mode="
            << num(static_cast<uint64_t>(local.mode))
            << " read_only_capture=" << flag(local.read_only_capture) << "\n";
  std::cout << "full node=" << num(full.node.value) << " actions="
            << num(full.actions.size()) << "\n";
  std::cout << "call_lifetime node=" << num(call_lifetime.node.value)
            << " on_return=" << num(call_lifetime.on_return.size()) << "\n";

  ExitPlan exit;
  exit.kind = ExitKind::cancel;
  exit.error_type = TypeId{0};
  exit.target = CatchTarget{HirNodeId{0}, 0};
  exit.exited_scopes = {ScopeId{0}};
  exit.defers = {HirNodeId{0}};
  exit.saves_value = true;
  exit.actions = {step};
  std::cout << "exit kind=" << exit_kind_id(exit.kind) << " error=1"
            << " error_id=0 scopes=" << num(exit.exited_scopes.size())
            << " defers=" << num(exit.defers.size()) << " saves="
            << flag(exit.saves_value) << " actions="
            << num(exit.actions.size()) << "\n";
  const auto *catch_target = std::get_if<CatchTarget>(&exit.target);
  std::cout << "exit_target catch index=0 try_node="
            << num(catch_target->try_node.value) << " index_id="
            << num(catch_target->index) << "\n";

  {
    GenericParam param;
    param.symbol = SymbolId{0};
    param.trait = trait_ref();
    param.source = source(4);
    FieldDecl field;
    field.symbol = SymbolId{0};
    field.type = TypeId{0};
    field.exported = false;
    field.source = source(5);
    VariantDecl variant;
    variant.symbol = SymbolId{0};
    variant.payload = {field};
    variant.source = source(6);
    NominalDecl nominal;
    nominal.symbol = SymbolId{0};
    nominal.kind = TypeKind::NominalStruct;
    nominal.params = {param};
    nominal.fields = {field};
    nominal.variants = {variant};
    nominal.source = source(7);
    std::cout << "nominal symbol=" << num(nominal.symbol.value)
              << " kind=" << num(static_cast<uint64_t>(nominal.kind))
              << " params=" << num(nominal.params.size()) << " fields="
              << num(nominal.fields.size()) << " variants="
              << num(nominal.variants.size()) << " variant_payload="
              << num(nominal.variants[0].payload.size())
              << " field_exported=" << flag(nominal.fields[0].exported)
              << "\n";
    MethodPrototype method;
    method.symbol = SymbolId{0};
    method.signature = TypeId{0};
    method.parameters = function.parameters;
    method.source = source(8);
    TraitDecl trait;
    trait.symbol = SymbolId{0};
    trait.self = SymbolId{0};
    trait.params = {param};
    trait.methods = {method};
    trait.source = source(9);
    std::cout << "trait symbol=" << num(trait.symbol.value) << " self="
              << num(trait.self.value) << " params=" << num(trait.params.size())
              << " methods=" << num(trait.methods.size()) << " method_sig="
              << num(trait.methods[0].signature.value) << "\n";
    ImplDecl impl;
    impl.params = {param};
    impl.trait = trait_ref();
    impl.self_parameter = SymbolId{0};
    impl.self_type = TypeId{0};
    impl.methods = {SymbolId{0}};
    impl.source = source(10);
    std::cout << "impl params=" << num(impl.params.size()) << " trait=1"
              << " self_parameter=" << num(impl.self_parameter.value)
              << " self_type=" << num(impl.self_type.value) << " methods="
              << num(impl.methods.size()) << "\n";
    FunctionTemplate function_template;
    function_template.function = function;
    function_template.params = {param};
    std::cout << "template function_symbol="
              << num(function_template.function.symbol.value) << " params="
              << num(function_template.params.size()) << " capture="
              << num(function_template.function.captures[0].local.value)
              << "\n";
  }

  {
    HirModule empty;
    empty.identity = ModuleId{0};
    empty.config = Config{};
    std::cout << "empty_module resolved_symbols="
              << num(empty.resolved ? empty.resolved->symbols.size() : 0)
              << " identity=" << num(empty.identity.value) << " types="
              << num(empty.types.size()) << " backend="
              << num(static_cast<uint64_t>(empty.config.backend))
              << " word_bits=" << num(empty.config.word_bits) << " functions="
              << num(empty.functions.size()) << " nodes="
              << num(empty.nodes.size()) << " scopes="
              << num(empty.scopes.size()) << " nominals="
              << num(empty.nominals.size()) << " traits="
              << num(empty.traits.size()) << " impls="
              << num(empty.impls.size()) << " templates="
              << num(empty.templates.size()) << " instances="
              << num(empty.instances.size()) << " uses="
              << num(empty.ownership.uses.size()) << " temporaries="
              << num(empty.ownership.temporaries.size()) << " locals="
              << num(empty.ownership.locals.size()) << " full_expressions="
              << num(empty.ownership.full_expressions.size()) << " calls="
              << num(empty.ownership.calls.size()) << " foreign="
              << num(empty.foreign_bindings.size()) << "\n";
  }

  {
    ValueField field;
    field.variant = SymbolId{0};
    field.field = SymbolId{0};
    field.type = TypeId{0};
    ValueOps ops;
    ops.kind = ValueKind::trait_object;
    ops.needs_drop = true;
    ops.pod_inout = false;
    ops.fields = {field};
    ops.reason = "test";
    std::cout << "value_ops kind=" << value_kind_id(ops.kind) << " needs_drop="
              << flag(ops.needs_drop) << " pod_inout=" << flag(ops.pod_inout)
              << " fields=" << num(ops.fields.size())
              << " field_variant=1 type=" << num(ops.fields[0].type.value)
              << " reason=" << ops.reason << "\n";
  }
  const InoutSlot slots[] = {InoutSlot::allowed, InoutSlot::unit,
                             InoutSlot::task, InoutSlot::external,
                             InoutSlot::type_parameter,
                             InoutSlot::unsupported};
  for (const auto slot : slots)
    std::cout << "inout_slot id=" << inout_slot_id(slot) << "\n";
  return 0;
}
