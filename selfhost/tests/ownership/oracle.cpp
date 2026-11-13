// Test-only C++ oracle for the SH07a fixture.
//
// This file constructs the same published HIR fixture as src/main.wz and
// publishes it with the real C++ ownership producer.  It is never linked into
// or called by the selfhost compiler; run.py uses it only to compare actual
// ownership fields, evaluator order and diagnostic-free results.
#include "hir/ownership.hpp"
#include <iostream>
#include <memory>

using namespace lang;
using namespace lang::hir;

static Type base(TypeKind kind, unsigned bits = 0) {
  Type value;
  value.kind = kind;
  value.bits = bits;
  return value;
}
static Type named(TypeKind kind, SymbolId declaration,
                  std::vector<TypeId> args = {}) {
  Type value = base(kind);
  value.declaration = declaration;
  value.type_args = std::move(args);
  return value;
}
static Type function_type(std::vector<ParameterType> parameters, TypeId result,
                          std::optional<TypeId> throws = std::nullopt) {
  Type value = base(TypeKind::Function);
  value.parameters = std::move(parameters);
  value.result = result;
  value.throws_type = throws;
  return value;
}
static ParameterType parameter(ParamMode mode, TypeId type) {
  return ParameterType{mode, type};
}
static Source source() { return Source{}; }
static HirNode node(TypeId type, size_t scope, std::vector<HirNodeId> children,
                    NodeData data) {
  HirNode value;
  value.type = type;
  value.source = source();
  value.scope = ScopeId{scope};
  value.children = std::move(children);
  value.data = std::move(data);
  return value;
}
static Scope scope(SymbolId function, std::optional<ScopeId> parent) {
  return Scope{function, parent, source()};
}
static FieldDecl field(SymbolId symbol, TypeId type) {
  return FieldDecl{symbol, type, false, source()};
}
static std::string number(uint64_t value) { return std::to_string(value); }
static std::string option_text(const std::optional<SymbolId> &value) {
  return value ? number(value->value) : "-";
}
static std::string option_text(const std::optional<TypeId> &value) {
  return value ? number(value->value) : "-";
}
static std::string option_text(const std::optional<ScopeId> &value) {
  return value ? number(value->value) : "-";
}
static std::string flag(bool value) { return value ? "1" : "0"; }
static std::string action_text(const CleanupStep &action) {
  return number(uint64_t(action.kind)) + ":" +
         number(uint64_t(action.owner.kind)) + ":" +
         number(action.owner.identity) + ":" + number(action.type.value);
}
static std::string actions_text(const std::vector<CleanupStep> &actions) {
  std::string text;
  for (size_t i = 0; i < actions.size(); ++i) {
    if (i) text += ",";
    text += action_text(actions[i]);
  }
  return text;
}
static std::string ids_text(const std::vector<ScopeId> &values) {
  std::string text;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) text += ",";
    text += number(values[i].value);
  }
  return text;
}
static std::string ids_text(const std::vector<HirNodeId> &values) {
  std::string text;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) text += ",";
    text += number(values[i].value);
  }
  return text;
}
static std::string target_text(const ExitTarget &target) {
  if (auto value = std::get_if<ScopeExit>(&target))
    return "S:" + option_text(value->scope);
  if (auto value = std::get_if<FunctionExit>(&target))
    return "F:" + number(value->function.value);
  if (auto value = std::get_if<JumpExit>(&target))
    return "J:" + number(value->node.value);
  const auto &value = std::get<CatchTarget>(target);
  return "C:" + number(value.try_node.value) + ":" + number(value.index);
}

int main() {
  auto project = std::make_shared<modules::Project>();
  modules::CTypeDecl retained;
  retained.id = 1;
  retained.kind = modules::CDeclKind::resource;
  retained.release_symbol = "release_resource";
  retained.retain_symbol = "retain_resource";
  modules::CTypeDecl plain = retained;
  plain.id = 2;
  plain.release_symbol = "release_plain";
  plain.retain_symbol = "";
  project->c_types = {retained, plain};
  auto resolved = std::make_shared<modules::ResolvedProject>();
  resolved->project = project;

  TypeContext context;
  int type_counter = 0;
  auto add = [&](Type value) {
    ++type_counter;
    try {
      return context.intern(std::move(value));
    } catch (const std::exception &) {
      std::cerr << "invalid type " << type_counter << " kind "
                << int(value.kind) << "\n";
      throw;
    }
  };
  const TypeId unit = add(base(TypeKind::Unit));
  const TypeId boolean = add(base(TypeKind::Bool));
  const TypeId ints = add(base(TypeKind::SignedInt, 32));
  const TypeId uints = add(base(TypeKind::UnsignedInt, 32));
  const TypeId text = add(base(TypeKind::String));
  const TypeId handle = add(base(TypeKind::ThreadHandle));
  const TypeId error = add(base(TypeKind::Error));
  const TypeId parameter_id =
      add(named(TypeKind::TypeParameter, SymbolId{90}));
  const TypeId enumeration =
      add(named(TypeKind::NominalEnum, SymbolId{5}));
  const TypeId vec_i32 = add(named(TypeKind::Vec, SymbolId{0}, {ints}));
  const TypeId vec_vec = add(named(TypeKind::Vec, SymbolId{0}, {vec_i32}));
  const TypeId vec_param =
      add(named(TypeKind::Vec, SymbolId{0}, {parameter_id}));
  const TypeId slice_id =
      add(named(TypeKind::Slice, SymbolId{0}, {ints}));
  const TypeId weak_id = add(named(TypeKind::Weak, SymbolId{0}, {vec_i32}));
  const TypeId task_id =
      add(named(TypeKind::Task, SymbolId{0}, {ints, enumeration}));
  const TypeId trait_object =
      add(named(TypeKind::TraitObject, SymbolId{10}));
  Type resource = base(TypeKind::CForeignResource);
  resource.c_type = 1;
  Type plain_resource = base(TypeKind::CForeignResource);
  plain_resource.c_type = 2;
  Type pointer = base(TypeKind::CForeignPointer);
  pointer.type_args = {ints};
  const TypeId resource_id = add(resource);
  const TypeId plain_id = add(plain_resource);
  const TypeId pointer_id = add(pointer);
  const TypeId s_id = add(named(TypeKind::NominalStruct, SymbolId{2}));
  const TypeId p_id = add(named(TypeKind::NominalStruct, SymbolId{17}));
  const TypeId node_id = add(named(TypeKind::NominalStruct, SymbolId{19}));
  const TypeId node_vec_id =
      add(named(TypeKind::NominalStruct, SymbolId{20}));
  const TypeId node_vec_vec =
      add(named(TypeKind::Vec, SymbolId{0}, {node_vec_id}));
  const TypeId box_id =
      add(named(TypeKind::NominalStruct, SymbolId{8}, {vec_i32}));
  const TypeId wrong_box =
      add(named(TypeKind::NominalStruct, SymbolId{18}, {vec_i32}));
  const TypeId function_f = add(function_type(
      {parameter(ParamMode::ordinary, vec_i32)}, vec_i32));
  const TypeId function_g = add(function_type(
      {parameter(ParamMode::ordinary, vec_vec)}, vec_i32));
  const TypeId function_h = add(function_type(
      {parameter(ParamMode::ordinary, ints)}, ints, enumeration));
  const TypeId function_a = add(function_type(
      {parameter(ParamMode::move, task_id)}, ints, enumeration));
  const TypeId function_c = add(function_type({}, unit));
  const TypeId function_sw = add(function_type({}, ints));
  const TypeId function_or = add(function_type(
      {parameter(ParamMode::ordinary, boolean),
       parameter(ParamMode::ordinary, boolean),
       parameter(ParamMode::move, text)}, unit));

  NominalDecl s_nominal;
  s_nominal.symbol = SymbolId{2};
  s_nominal.kind = TypeKind::NominalStruct;
  s_nominal.fields = {field(SymbolId{3}, vec_i32), field(SymbolId{4}, ints)};
  NominalDecl e_nominal;
  e_nominal.symbol = SymbolId{5};
  e_nominal.kind = TypeKind::NominalEnum;
  VariantDecl variant;
  variant.symbol = SymbolId{6};
  variant.payload = {field(SymbolId{7}, vec_i32)};
  e_nominal.variants = {variant};
  NominalDecl box_nominal;
  box_nominal.symbol = SymbolId{8};
  box_nominal.kind = TypeKind::NominalStruct;
  box_nominal.params = {GenericParam{SymbolId{90}, std::nullopt, source()}};
  box_nominal.fields = {field(SymbolId{9}, vec_param)};
  NominalDecl p_nominal;
  p_nominal.symbol = SymbolId{17};
  p_nominal.kind = TypeKind::NominalStruct;
  p_nominal.fields = {field(SymbolId{21}, ints)};
  NominalDecl node_nominal;
  node_nominal.symbol = SymbolId{19};
  node_nominal.kind = TypeKind::NominalStruct;
  node_nominal.fields = {field(SymbolId{22}, node_id)};
  NominalDecl node_vec_nominal;
  node_vec_nominal.symbol = SymbolId{20};
  node_vec_nominal.kind = TypeKind::NominalStruct;
  node_vec_nominal.fields = {field(SymbolId{23}, node_vec_vec)};

  std::vector<Scope> scopes = {
      scope(SymbolId{11}, {}), scope(SymbolId{11}, ScopeId{0}),
      scope(SymbolId{12}, {}), scope(SymbolId{12}, ScopeId{2}),
      scope(SymbolId{13}, {}), scope(SymbolId{13}, ScopeId{4}),
      scope(SymbolId{13}, ScopeId{4}), scope(SymbolId{14}, {}),
      scope(SymbolId{15}, {}), scope(SymbolId{24}, {}),
      scope(SymbolId{24}, ScopeId{9}), scope(SymbolId{25}, {})};

  std::vector<HirNode> nodes;
  nodes.push_back(node(unit, 0, {HirNodeId{1}, HirNodeId{2}, HirNodeId{3}},
                       Block{}));
  nodes.push_back(node(unit, 0, {HirNodeId{4}},
                       Binding{SymbolId{101}, false}));
  nodes.push_back(node(unit, 0, {HirNodeId{5}},
                       Defer{ScopeId{0}, {}}));
  nodes.push_back(node(unit, 0, {HirNodeId{6}}, Return{SymbolId{11}}));
  nodes.push_back(node(vec_i32, 0, {}, VecInit{}));
  nodes.push_back(node(unit, 1, {HirNodeId{7}}, Block{}));
  nodes.push_back(node(vec_i32, 0, {}, Read{SymbolId{100}}));
  nodes.push_back(node(unit, 1, {HirNodeId{8}}, ExpressionStatement{}));
  nodes.push_back(node(vec_i32, 1, {}, Read{SymbolId{101}}));
  nodes.push_back(node(unit, 2, {HirNodeId{10}}, Block{}));
  nodes.push_back(node(unit, 2, {HirNodeId{11}, HirNodeId{12}},
                       ForIn{SymbolId{201}, ScopeId{3}, HirNodeId{12},
                             std::nullopt}));
  nodes.push_back(node(vec_vec, 2, {}, Read{SymbolId{200}}));
  nodes.push_back(node(unit, 3, {HirNodeId{13}}, Block{}));
  nodes.push_back(node(unit, 3, {HirNodeId{14}}, Return{SymbolId{12}}));
  nodes.push_back(node(vec_i32, 3, {}, Read{SymbolId{201}}));
  for (int i = 15; i < 20; ++i) nodes.push_back(node(unit, 0, {}, Noop{}));
  nodes.push_back(node(unit, 4, {HirNodeId{21}}, Block{}));
  Catch catcher{enumeration, SymbolId{301}, ScopeId{6}, HirNodeId{24},
                source()};
  nodes.push_back(node(unit, 4, {HirNodeId{22}, HirNodeId{24}},
                       Try{{catcher}}));
  nodes.push_back(node(unit, 5, {HirNodeId{23}}, Block{}));
  nodes.push_back(node(ints, 5, {},
                       Call{CalleeKind::direct, function_h, SymbolId{16},
                            std::nullopt, {}, {}, std::nullopt, std::nullopt,
                            {}, std::nullopt}));
  nodes.push_back(node(unit, 6, {HirNodeId{25}}, Block{}));
  nodes.push_back(node(unit, 6, {HirNodeId{26}},
                       ExpressionStatement{}));
  nodes.push_back(node(enumeration, 6, {}, Read{SymbolId{301}}));
  for (int i = 27; i < 30; ++i) nodes.push_back(node(unit, 0, {}, Noop{}));
  nodes.push_back(node(unit, 7, {HirNodeId{31}}, Block{}));
  nodes.push_back(node(ints, 7, {HirNodeId{32}}, Await{}));
  nodes.push_back(node(task_id, 7, {}, Read{SymbolId{400}}));
  for (int i = 33; i < 40; ++i) nodes.push_back(node(unit, 0, {}, Noop{}));
  nodes.push_back(node(unit, 8, {HirNodeId{41}}, Block{}));
  nodes.push_back(node(unit, 8, {HirNodeId{42}},
                       ExpressionStatement{}));
  nodes.push_back(node(vec_i32, 8, {}, Read{SymbolId{501}}));
  for (int i = 43; i < 50; ++i) nodes.push_back(node(unit, 0, {}, Noop{}));
  nodes.push_back(node(unit, 9, {HirNodeId{51}}, Block{}));
  Pattern payload;
  payload.type = vec_i32;
  payload.binding = SymbolId{600};
  Pattern arm_pattern;
  arm_pattern.type = enumeration;
  arm_pattern.variant = SymbolId{6};
  arm_pattern.payload = {payload};
  Arm arm{arm_pattern, ScopeId{10}, HirNodeId{54}, source()};
  nodes.push_back(node(unit, 9, {HirNodeId{52}, HirNodeId{54}},
                       Switch{{arm}, false}));
  nodes.push_back(node(enumeration, 9, {HirNodeId{53}},
                       EnumInit{SymbolId{6},
                                {CallArg{0, 0, std::nullopt,
                                         ParamMode::ordinary,
                                         HirNodeId{53}}}}));
  nodes.push_back(node(vec_i32, 9, {}, VecInit{}));
  nodes.push_back(node(unit, 10, {HirNodeId{55}}, Block{}));
  nodes.push_back(node(unit, 10, {HirNodeId{56}}, Return{SymbolId{24}}));
  nodes.push_back(node(ints, 10, {}, IntegerLiteral{"0", 0}));
  nodes.push_back(node(unit, 11, {HirNodeId{58}}, Block{}));
  nodes.push_back(node(unit, 11, {HirNodeId{59}},
                       ExpressionStatement{}));
  nodes.push_back(node(boolean, 11, {HirNodeId{60}, HirNodeId{61}},
                       ShortCircuit{LogicalOp::or_}));
  nodes.push_back(node(boolean, 11, {}, Read{SymbolId{700}}));
  nodes.push_back(node(boolean, 11, {}, Read{SymbolId{701}}));

  std::vector<HirFunction> functions;
  functions.push_back(HirFunction{SymbolId{11}, function_f,
                                  {{SymbolId{100}, "v"}}, HirNodeId{0},
                                  ScopeId{0}, source(), {}, false});
  functions.push_back(HirFunction{SymbolId{12}, function_g,
                                  {{SymbolId{200}, "v"}}, HirNodeId{9},
                                  ScopeId{2}, source(), {}, false});
  functions.push_back(HirFunction{SymbolId{13}, function_h,
                                  {{SymbolId{300}, "n"}}, HirNodeId{20},
                                  ScopeId{4}, source(), {}, false});
  functions.push_back(HirFunction{SymbolId{14}, function_a,
                                  {{SymbolId{400}, "task"}}, HirNodeId{30},
                                  ScopeId{7}, source(), {}, true});
  CaptureBinding capture{SymbolId{500}, SymbolId{501}, vec_i32};
  functions.push_back(HirFunction{SymbolId{15}, function_c, {},
                                  HirNodeId{40}, ScopeId{8}, source(),
                                  {capture}, false});
  functions.push_back(HirFunction{SymbolId{24}, function_sw, {},
                                  HirNodeId{50}, ScopeId{9}, source(), {},
                                  false});
  functions.push_back(HirFunction{SymbolId{25}, function_or,
                                  {{SymbolId{700}, "a"},
                                   {SymbolId{701}, "b"},
                                   {SymbolId{702}, "text"}},
                                  HirNodeId{57}, ScopeId{11}, source(), {},
                                  false});

  HirModule module;
  module.resolved = resolved;
  module.identity = ModuleId{0};
  module.types = std::move(context);
  module.config = Config{Backend::native, 64};
  module.functions = std::move(functions);
  module.nodes = std::move(nodes);
  module.scopes = std::move(scopes);
  module.nominals = {s_nominal, e_nominal, box_nominal, p_nominal,
                     node_nominal, node_vec_nominal};
  for (size_t index = 0; index < module.nodes.size(); ++index)
    module.nodes[index].source.span =
        SourceSpan{SourceSnapshotId{1}, index, index + 1};
  auto produced = detail::produce_ownership(module, {});
  if (!produced.success()) {
    for (const auto &d : produced.diagnostics)
      std::cerr << d.code << ": " << d.message << "\n";
    return 1;
  }

  auto print_error = [](const char *label, const PhaseResult<std::monostate> &r) {
    std::cout << "ORACLE error " << label << " " << int(r.failure) << " ";
    if (r.diagnostics.empty()) {
      std::cout << "- -\n";
      return;
    }
    const auto &d = r.diagnostics.front();
    std::cout << d.code << " ";
    if (d.primary)
      std::cout << d.primary->begin_byte << "\n";
    else
      std::cout << "-\n";
  };
  auto unsupported = module;
  unsupported.nodes[4].type = error;
  print_error("produce_unsupported", detail::produce_ownership(unsupported, {}));

  auto source_issue = module;
  source_issue.nodes[6].data = Read{SymbolId{999}};
  print_error("verify_source", detail::verify_ownership(source_issue, {}));

  auto tampered = module;
  tampered.ownership.locals[0].type = ints;
  print_error("verify_tamper", detail::verify_ownership(tampered, {}));

  auto broken = module;
  broken.functions[0].body = HirNodeId{9999};
  print_error("verify_bounds", detail::verify_ownership(broken, {}));

  for (uint64_t id = 1; id <= module.types.size(); ++id) {
    ValueOps ops;
    try {
      ops = classify_value(module, TypeId{id});
    } catch (const std::exception &) {
      ops.kind = ValueKind::unsupported;
      ops.needs_drop = false;
      ops.pod_inout = false;
      ops.fields.clear();
    }
    std::cout << "ORACLE classify " << id << " " << int(ops.kind) << " "
              << flag(ops.needs_drop) << " " << flag(ops.pod_inout) << " "
              << ops.fields.size() << "\n";
    for (size_t index = 0; index < ops.fields.size(); ++index) {
      std::cout << "ORACLE field " << id << " " << index << " "
                << option_text(ops.fields[index].variant) << " "
                << ops.fields[index].field.value << " "
                << ops.fields[index].type.value << "\n";
    }
  }
  ValueKind cycle_kind;
  try {
    cycle_kind = classify_value(module, TypeId{22}).kind;
  } catch (const std::exception &) {
    cycle_kind = ValueKind::unsupported;
  }
  std::cout << "ORACLE cycle " << flag(cycle_kind == ValueKind::unsupported)
            << "\n";
  auto layout = infinite_value_layout(module, {});
  std::cout << "ORACLE layout "
            << (layout ? number(layout->value) : "-") << "\n";
  for (uint64_t id = 1; id <= module.types.size(); ++id) {
    bool data = false, copy = false, own = false;
    InoutSlot slot = InoutSlot::unsupported;
    try {
      data = inout_data_type(module, TypeId{id});
      slot = inout_slot(module, TypeId{id});
      copy = ordinary_copyable(module, TypeId{id});
      own = owning_value(module, TypeId{id});
    } catch (const std::exception &) {
      data = false;
      slot = InoutSlot::unsupported;
      copy = false;
      own = false;
    }
    std::cout << "ORACLE helpers " << id << " " << flag(data) << " "
              << int(slot) << " " << flag(copy) << " " << flag(own) << "\n";
  }
  for (const auto &use : module.ownership.uses)
    std::cout << "ORACLE use " << use.consumer.value << " " << use.operand
              << " " << int(use.kind) << " " << option_text(use.root) << " "
              << use.type.value << " " << option_text(use.destination)
              << "\n";
  for (const auto &temporary : module.ownership.temporaries)
    std::cout << "ORACLE temp " << temporary.producer.value << " "
              << temporary.full_expression.value << " "
              << temporary.type.value << "\n";
  for (const auto &local : module.ownership.locals)
    std::cout << "ORACLE local " << local.symbol.value << " "
              << local.scope.value << " " << local.type.value << " "
              << local.order << " " << int(local.mode) << " "
              << flag(local.read_only_capture) << "\n";
  for (const auto &full : module.ownership.full_expressions)
    std::cout << "ORACLE full " << full.node.value << " "
              << actions_text(full.actions) << "\n";
  for (const auto &call : module.ownership.calls)
    std::cout << "ORACLE call " << call.node.value << " "
              << actions_text(call.on_return) << "\n";
  for (size_t index = 0; index < module.nodes.size(); ++index)
    for (size_t plan_index = 0;
         plan_index < module.nodes[index].exits.size(); ++plan_index) {
      const auto &plan = module.nodes[index].exits[plan_index];
      std::cout << "ORACLE exit " << index << " " << plan_index << " "
                << int(plan.kind) << " " << option_text(plan.error_type) << " "
                << target_text(plan.target) << " "
                << ids_text(plan.exited_scopes) << " "
                << ids_text(plan.defers) << " " << flag(plan.saves_value)
                << " " << actions_text(plan.actions) << "\n";
    }
  return 0;
}
