// Test-only C++ oracle for the pure-Wz HIR dumps.
//
// This translation unit links against the already-built stage0 libraries.  It
// parses the same real fixture source as the Wz harness and builds the same
// TypeContext/HirModule records, so the two dump strings can be compared byte
// for byte.  It never participates in the Wz compiler.
#include "lang/hir/hir.hpp"
#include "lang/support/source.hpp"
#include "lang/syntax/parser.hpp"
#include "lang/syntax/token.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lang;
using namespace lang::hir;
using ConstantData = std::variant<UnitLiteral, BoolLiteral, IntegerLiteral,
                                  FloatLiteral, StringLiteral, FunctionValue,
                                  ConstantAggregate>;

namespace {

Source src(const AstFile &file, size_t index) {
  const auto &node = file.nodes.at(index);
  return Source{modules::NodeRef{ModuleId{2}, 1, AstNodeId{index}},
                node.span};
}

Type blank(TypeKind kind) {
  Type t;
  t.kind = kind;
  return t;
}

TypeId intern(TypeContext &context, Type value) {
  return context.intern(std::move(value));
}

Parameter param(SymbolId symbol, std::string label) {
  return Parameter{symbol, std::move(label)};
}

TraitRef trait_ref(SymbolId declaration, std::vector<TypeId> args) {
  return TraitRef{declaration, std::move(args)};
}

GenericParam generic(SymbolId symbol, std::optional<TraitRef> bound,
                     Source source) {
  return GenericParam{symbol, std::move(bound), source};
}

FieldDecl field_decl(SymbolId symbol, TypeId type, bool exported,
                     Source source) {
  return FieldDecl{symbol, type, exported, source};
}

CallArg call_arg(uint32_t source_index, uint32_t parameter_index,
                 std::optional<std::string> label, ParamMode mode,
                 HirNodeId expression) {
  return CallArg{source_index, parameter_index, std::move(label), mode,
                 expression};
}

CleanupStep cleanup(CleanupKind kind, OwnerKind owner_kind, uint64_t identity,
                    TypeId type) {
  return CleanupStep{kind, OwnerRef{owner_kind, identity}, type};
}

ExitPlan exit_plan(ExitKind kind, std::optional<TypeId> error_type,
                   ExitTarget target, std::vector<ScopeId> exited_scopes,
                   std::vector<HirNodeId> defers, bool saves_value,
                   std::vector<CleanupStep> actions) {
  return ExitPlan{kind, error_type, std::move(target), std::move(exited_scopes),
                  std::move(defers), saves_value, std::move(actions)};
}

Pattern pattern(TypeId type, Source source, std::optional<SymbolId> variant,
                std::optional<SymbolId> binding, std::vector<Pattern> payload,
                std::optional<PatternLiteral> literal) {
  return Pattern{type, source, variant, binding, std::move(payload),
                 std::move(literal)};
}

ConstantValue constant(TypeId type, ConstantData data) {
  return ConstantValue{type, std::move(data)};
}

HirFunction function(SymbolId symbol, TypeId signature,
                     std::vector<Parameter> parameters, HirNodeId body,
                     ScopeId scope, Source source,
                     std::vector<CaptureBinding> captures, bool is_async) {
  return HirFunction{symbol, signature, std::move(parameters), body, scope,
                     source, std::move(captures), is_async};
}

TypeId build_types(TypeContext &context) {
  const auto unit = intern(context, blank(TypeKind::Unit));
  const auto boolean = intern(context, blank(TypeKind::Bool));
  Type signed_int = blank(TypeKind::SignedInt);
  signed_int.bits = 8;
  const auto i8 = intern(context, signed_int);
  Type unsigned_int = blank(TypeKind::UnsignedInt);
  unsigned_int.bits = 16;
  const auto u16 = intern(context, unsigned_int);
  Type f32 = blank(TypeKind::Float);
  f32.bits = 32;
  const auto float32 = intern(context, f32);
  const auto string_type = intern(context, blank(TypeKind::String));
  const auto isize = intern(context, blank(TypeKind::Isize));
  const auto usize = intern(context, blank(TypeKind::Usize));

  Type function_type = blank(TypeKind::Function);
  function_type.parameters = {ParameterType{ParamMode::ordinary, boolean},
                              ParameterType{ParamMode::inout, i8},
                              ParameterType{ParamMode::move, usize}};
  function_type.result = unit;
  function_type.throws_type = i8;
  const auto signature = intern(context, function_type);

  Type struct_type = blank(TypeKind::NominalStruct);
  struct_type.declaration = SymbolId{100};
  struct_type.type_args = {boolean, i8};
  intern(context, struct_type);

  Type enum_type = blank(TypeKind::NominalEnum);
  enum_type.declaration = SymbolId{101};
  enum_type.type_args = {u16};
  intern(context, enum_type);

  Type parameter = blank(TypeKind::TypeParameter);
  parameter.declaration = SymbolId{102};
  intern(context, parameter);

  Type foreign_opaque = blank(TypeKind::ForeignOpaque);
  foreign_opaque.declaration = SymbolId{103};
  foreign_opaque.binding = ForeignBindingId{200};
  intern(context, foreign_opaque);

  const auto error = intern(context, blank(TypeKind::Error));

  Type vec = blank(TypeKind::Vec);
  vec.type_args = {boolean};
  intern(context, vec);

  Type foreign_callable = blank(TypeKind::ForeignCallable);
  foreign_callable.declaration = SymbolId{104};
  foreign_callable.binding = ForeignBindingId{201};
  intern(context, foreign_callable);

  Type c_pointer = blank(TypeKind::CForeignPointer);
  c_pointer.type_args = {boolean};
  c_pointer.bits = 1;
  intern(context, c_pointer);

  Type c_buffer = blank(TypeKind::CForeignBuffer);
  c_buffer.type_args = {string_type};
  intern(context, c_buffer);

  for (unsigned c_type = 1; c_type <= 4; ++c_type) {
    Type declared = blank(static_cast<TypeKind>(static_cast<int>(
                              TypeKind::CForeignRecord) + c_type - 1));
    declared.c_type = c_type;
    intern(context, declared);
  }

  intern(context, blank(TypeKind::ThreadHandle));
  intern(context, blank(TypeKind::MutexHandle));
  intern(context, blank(TypeKind::CondvarHandle));
  intern(context, blank(TypeKind::AtomicI64Handle));
  intern(context, blank(TypeKind::IpcHandle));

  Type slice = blank(TypeKind::Slice);
  slice.type_args = {usize};
  intern(context, slice);

  Type weak = blank(TypeKind::Weak);
  weak.type_args = {boolean};
  intern(context, weak);

  Type task = blank(TypeKind::Task);
  task.type_args = {unit, error};
  intern(context, task);

  Type trait_object = blank(TypeKind::TraitObject);
  trait_object.declaration = SymbolId{105};
  trait_object.type_args = {boolean};
  intern(context, trait_object);
  return signature;
}

void build_module(const AstFile &file, TypeContext context, TypeId signature,
                  HirModule &module) {
  const size_t ast_count = file.nodes.size();
  auto at = [&](size_t index) { return src(file, (index * 13 + 5) % ast_count); };
  auto type_at = [](size_t index) { return TypeId{1 + (index % 31)}; };
  auto scope_at = [](size_t index) { return ScopeId{index % 4}; };

  module.identity = ModuleId{7};
  module.config = Config{Backend::python, 32};

  // Generic/declaration records.
  const auto trait_ref_value =
      trait_ref(SymbolId{501}, {TypeId{2}, TypeId{3}, TypeId{9}});
  const auto parameter_with_bound =
      generic(SymbolId{500}, trait_ref_value, at(40));
  const auto parameter_without_bound =
      generic(SymbolId{502}, std::nullopt, at(41));

  NominalDecl nominal;
  nominal.symbol = SymbolId{503};
  nominal.kind = TypeKind::NominalStruct;
  nominal.params = {parameter_with_bound, parameter_without_bound};
  nominal.fields = {field_decl(SymbolId{504}, TypeId{10}, true, at(42)),
                    field_decl(SymbolId{505}, TypeId{11}, false, at(43))};
  nominal.variants = {
      VariantDecl{SymbolId{506},
                  {field_decl(SymbolId{507}, TypeId{12}, true, at(44))},
                  at(45)},
      VariantDecl{SymbolId{508},
                  {field_decl(SymbolId{509}, TypeId{13}, false, at(46)),
                   field_decl(SymbolId{510}, TypeId{14}, true, at(47))},
                  at(48)}};
  nominal.source = at(39);
  module.nominals.push_back(nominal);
  NominalDecl empty_nominal;
  empty_nominal.symbol = SymbolId{511};
  empty_nominal.kind = TypeKind::NominalEnum;
  empty_nominal.source = at(49);
  module.nominals.push_back(empty_nominal);

  TraitDecl trait;
  trait.symbol = SymbolId{520};
  trait.self = SymbolId{521};
  trait.params = {parameter_with_bound};
  trait.methods = {
      MethodPrototype{SymbolId{522}, signature,
                      {param(SymbolId{523}, "first"),
                       param(SymbolId{524}, "quote\"slash\\tab\t")},
                      at(50)},
      MethodPrototype{SymbolId{525}, signature,
                      {param(SymbolId{526}, "")}, at(51)}};
  trait.source = at(52);
  module.traits.push_back(trait);

  ImplDecl impl;
  impl.params = {parameter_with_bound};
  impl.trait = trait_ref_value;
  impl.self_parameter = SymbolId{530};
  impl.self_type = TypeId{10};
  impl.methods = {SymbolId{531}, SymbolId{532}};
  impl.source = at(53);
  module.impls.push_back(impl);
  ImplDecl plain_impl;
  plain_impl.self_parameter = SymbolId{533};
  plain_impl.self_type = TypeId{11};
  plain_impl.source = at(54);
  module.impls.push_back(plain_impl);

  FunctionTemplate templ;
  templ.function = function(
      SymbolId{540}, signature,
      {param(SymbolId{541}, "left"), param(SymbolId{542}, "")}, HirNodeId{5},
      ScopeId{2}, at(55),
      {CaptureBinding{SymbolId{543}, SymbolId{544}, TypeId{3}}}, true);
  templ.params = {parameter_with_bound, parameter_without_bound};
  module.templates.push_back(templ);

  FunctionInstance instance;
  instance.symbol = SymbolId{550};
  instance.origin = SymbolId{551};
  instance.type_args = {TypeId{2}, TypeId{10}, TypeId{28}};
  instance.renamed = {{SymbolId{552}, SymbolId{553}},
                      {SymbolId{554}, SymbolId{555}}};
  instance.target = Config{Backend::native, 64};
  module.instances.push_back(instance);

  HirFunction first =
      function(SymbolId{560}, signature,
               {param(SymbolId{561}, "alpha"),
                param(SymbolId{562}, "beta\"gamma\\delta\n")}, HirNodeId{8},
               ScopeId{1}, at(56),
               {CaptureBinding{SymbolId{563}, SymbolId{564}, TypeId{5}},
                CaptureBinding{SymbolId{565}, SymbolId{566}, TypeId{6}}},
               true);
  module.functions.push_back(first);
  module.functions.push_back(function(SymbolId{567}, signature, {}, HirNodeId{9},
                                     ScopeId{3}, at(57), {}, false));

  Scope scope_one;
  scope_one.function = SymbolId{560};
  scope_one.parent = ScopeId{0};
  scope_one.source = at(58);
  module.scopes.push_back(scope_one);
  Scope scope_two;
  scope_two.function = SymbolId{560};
  scope_two.source = at(59);
  module.scopes.push_back(scope_two);
  Scope scope_three;
  scope_three.function = SymbolId{567};
  scope_three.parent = ScopeId{1};
  scope_three.source = at(60);
  module.scopes.push_back(scope_three);

  // All 31 TypeKind and 45 NodeData alternatives.  Extra nodes cover the
  // absent-optional and empty-vector branches.
  auto add = [&](NodeData data, size_t source_index, std::vector<HirNodeId> children,
                 std::vector<TypeId> errors, std::vector<ExitPlan> exits) {
    HirNode node;
    node.type = type_at(module.nodes.size());
    node.source = at(source_index);
    node.scope = scope_at(module.nodes.size());
    node.children = std::move(children);
    node.data = std::move(data);
    node.errors = std::move(errors);
    node.exits = std::move(exits);
    module.nodes.push_back(std::move(node));
  };
  auto add_full = [&](NodeData data, size_t source_index) {
    add(std::move(data), source_index, {HirNodeId{1}, HirNodeId{3}},
        {TypeId{3}, TypeId{14}}, {});
  };

  const std::vector<CleanupStep> all_actions = {
      cleanup(CleanupKind::save_output, OwnerKind::binding, 700, TypeId{1}),
      cleanup(CleanupKind::end_borrow, OwnerKind::temporary, 701, TypeId{2}),
      cleanup(CleanupKind::destroy_if_live, OwnerKind::return_output, 702,
              TypeId{3}),
      cleanup(CleanupKind::defer_, OwnerKind::error_output, 703, TypeId{4}),
      cleanup(CleanupKind::end_iteration, OwnerKind::iteration, 704,
              TypeId{5})};
  std::vector<ExitPlan> all_exits = {
      exit_plan(ExitKind::normal, std::nullopt,
                ExitTarget{ScopeExit{ScopeId{3}}}, {ScopeId{4}, ScopeId{5}},
                {HirNodeId{6}}, true, all_actions),
      exit_plan(ExitKind::return_, TypeId{3},
                ExitTarget{FunctionExit{SymbolId{710}}}, {}, {}, false, {}),
      exit_plan(ExitKind::break_, std::nullopt,
                ExitTarget{JumpExit{HirNodeId{11}}}, {ScopeId{6}},
                {HirNodeId{12}}, true, {}),
      exit_plan(ExitKind::continue_, std::nullopt,
                ExitTarget{JumpExit{HirNodeId{13}}}, {}, {}, false, {}),
      exit_plan(ExitKind::error, TypeId{14},
                ExitTarget{CatchTarget{HirNodeId{14}, 7}}, {ScopeId{7}},
                {HirNodeId{15}}, true, {}),
      exit_plan(ExitKind::cancel, std::nullopt,
                ExitTarget{ScopeExit{std::nullopt}}, {ScopeId{8}}, {}, false,
                {})};

  add_full(UnitLiteral{}, 0);
  add_full(BoolLiteral{true}, 1);
  add_full(IntegerLiteral{"-12345678901234567890", 18446744073709551615ULL}, 2);
  add_full(FloatLiteral{0x400921fb54442d18ULL}, 3);
  add_full(StringLiteral{"quote\" slash\\ newline\n tab\t unicode-\xe4\xb8\xad"}, 4);
  add_full(Read{SymbolId{600}}, 5);
  add_full(Place{SymbolId{601}}, 6);
  add_full(FunctionValue{SymbolId{602},
                         {TypeId{2}, TypeId{3}, TypeId{9}},
                         {CaptureSource{SymbolId{603}, SymbolId{604},
                                        TypeId{4}, true},
                          CaptureSource{SymbolId{605}, SymbolId{606},
                                        TypeId{5}, false}},
                         true, BoundReceiver{TypeId{6}, ParamMode::move}},
           7);
  add_full(Binding{SymbolId{607}, true}, 8);
  add_full(Assign{BinaryOp::greater_equal}, 9);
  add_full(Unary{UnaryOp::bit_not}, 10);
  add_full(Binary{BinaryOp::shift_right}, 11);
  add_full(ShortCircuit{LogicalOp::or_}, 12);
  add_full(IntegerConvert{}, 13);
  add_full(
      Call{CalleeKind::c_foreign,
           signature,
           SymbolId{610},
           HirNodeId{16},
           {call_arg(0, 1, std::string{"label\"one"}, ParamMode::ordinary,
                     HirNodeId{17}),
            call_arg(1, 2, std::nullopt, ParamMode::inout, HirNodeId{18})},
           {TypeId{2}, TypeId{10}},
           ForeignBindingId{611},
           uint32_t{9},
           {1, 3, 5},
           static_cast<SystemOperation>(42)},
      14);
  add_full(Block{}, 15);
  add_full(ExpressionStatement{}, 16);
  add_full(If{}, 17);
  add_full(For{}, 18);
  add_full(Noop{}, 19);
  add_full(Return{SymbolId{612}}, 20);
  add_full(Break{HirNodeId{19}}, 21);
  add_full(Continue{HirNodeId{20}}, 22);
  add_full(Unsupported{"neutral[ bracket ] quote\" slash\\ newline\n"}, 23);
  add_full(StructInit{{call_arg(0, 1, std::string{"first"}, ParamMode::ordinary,
                               HirNodeId{21}),
                       call_arg(2, 3, std::nullopt, ParamMode::move,
                               HirNodeId{22})}},
           24);
  add_full(EnumInit{SymbolId{613},
                    {call_arg(1, 2, std::string{"payload"}, ParamMode::inout,
                              HirNodeId{23})}},
           25);
  add_full(FieldRead{SymbolId{614}}, 26);
  add_full(FieldPlace{SymbolId{615}}, 27);

  std::vector<Pattern> nested_payload = {
      pattern(TypeId{3}, at(70), std::nullopt, SymbolId{616},
              {pattern(TypeId{2}, at(71), std::nullopt, std::nullopt, {},
                       PatternLiteral{BoolLiteral{false}})},
              PatternLiteral{IntegerLiteral{"-42", 12345678901234567890ULL}}),
      pattern(TypeId{1}, at(72), std::nullopt, std::nullopt, {},
              std::nullopt)};
  Switch switch_data;
  switch_data.exhaustive = true;
  switch_data.arms = {
      Arm{pattern(TypeId{5}, at(69), SymbolId{617}, std::nullopt,
                  std::move(nested_payload), std::nullopt),
          ScopeId{9}, HirNodeId{24}, at(68)},
      Arm{pattern(TypeId{4}, at(73), std::nullopt, std::nullopt, {},
                  PatternLiteral{FloatLiteral{0x3ff0000000000000ULL}}),
          ScopeId{10}, HirNodeId{25}, at(74)},
      Arm{pattern(TypeId{7}, at(75), std::nullopt, std::nullopt, {},
                  PatternLiteral{IntegerLiteral{"7", 7}}),
          ScopeId{11}, HirNodeId{26}, at(76)},
      Arm{pattern(TypeId{8}, at(77), std::nullopt, SymbolId{618}, {},
                  PatternLiteral{IntegerLiteral{"8", 8}}),
          ScopeId{12}, HirNodeId{27}, at(78)},
      Arm{pattern(TypeId{6}, at(79), std::nullopt, std::nullopt, {},
                  PatternLiteral{StringLiteral{"pattern\"\\\n"}}),
          ScopeId{13}, HirNodeId{28}, at(80)},
      Arm{pattern(TypeId{2}, at(81), std::nullopt, std::nullopt, {},
                  PatternLiteral{BoolLiteral{true}}),
          ScopeId{14}, HirNodeId{29}, at(82)},
      Arm{pattern(TypeId{1}, at(83), std::nullopt, std::nullopt, {}, std::nullopt),
          ScopeId{15}, HirNodeId{30}, at(84)}};
  add_full(switch_data, 28);

  add_full(ConstraintCall{trait_ref_value, SymbolId{620}, TypeId{10},
                          signature,
                          {call_arg(0, 1, std::string{"receiver"},
                                    ParamMode::ordinary, HirNodeId{31})}},
           29);
  add_full(Try{{Catch{TypeId{14}, SymbolId{621}, ScopeId{16}, HirNodeId{32},
                      at(85)},
                Catch{TypeId{3}, SymbolId{622}, ScopeId{17}, HirNodeId{33},
                      at(86)}}},
           30);
  add_full(Defer{ScopeId{18}, {SymbolId{623}, SymbolId{624}, SymbolId{625}}}, 31);
  add_full(Throw{}, 32);
  add_full(Move{SymbolId{626}}, 33);
  add_full(VecInit{}, 34);
  add_full(IndexRead{}, 35);
  add_full(IndexPlace{}, 36);
  add_full(VecSlice{HirNodeId{34}, HirNodeId{35}}, 37);
  add_full(ForIn{SymbolId{627}, ScopeId{19}, HirNodeId{36},
                 IteratorFor{SymbolId{628}, SymbolId{629}, HirNodeId{37},
                             TypeId{15}, TypeId{2}, SymbolId{630},
                             SymbolId{631}, SymbolId{632}}},
           38);
  add_full(ForeignRead{ForeignBindingId{633},
                       ForeignAccessKind::slice_place, HirNodeId{38},
                       HirNodeId{39}},
           39);

  const ConstantValue aggregate = constant(
      TypeId{10},
      ConstantAggregate{
          SymbolId{640},
          {constant(TypeId{1}, UnitLiteral{}),
           constant(TypeId{2}, BoolLiteral{true}),
           constant(TypeId{3},
                    IntegerLiteral{"-9", 18446744073709551615ULL}),
           constant(TypeId{4}, FloatLiteral{0x400921fb54442d18ULL}),
           constant(TypeId{6},
                    StringLiteral{"constant\" slash\\ newline\n tab\t"}),
           constant(TypeId{9},
                    FunctionValue{SymbolId{641},
                                  {TypeId{2}, TypeId{3}},
                                  {CaptureSource{SymbolId{642}, SymbolId{643},
                                                 TypeId{4}, true}},
                                  true,
                                  BoundReceiver{TypeId{5}, ParamMode::inout}}),
           constant(TypeId{15},
                    ConstantAggregate{
                        std::nullopt,
                        {constant(TypeId{1}, UnitLiteral{})}})}});
  add_full(ConstantLiteral{aggregate}, 40);
  add_full(CFieldRead{9, 7}, 41);
  add_full(Await{}, 42);
  add_full(TraitObjectInit{trait_ref_value, TypeId{10},
                           {SymbolId{644}, SymbolId{645}, SymbolId{646}},
                           {{TypeId{2}, TypeId{3}}, {TypeId{4}}}},
           43);
  add_full(TraitDispatch{trait_ref_value, SymbolId{647}, signature, 11,
                         {call_arg(0, 1, std::string{"self"}, ParamMode::inout,
                                   HirNodeId{40}),
                          call_arg(1, 2, std::nullopt, ParamMode::move,
                                   HirNodeId{41})}},
           44);

  // Extras: absent options, empty children/errors/exits and empty vectors.
  add(Assign{std::nullopt}, 0, {}, {}, {});
  add(FunctionValue{SymbolId{650}, {}, {}, false, std::nullopt}, 1, {}, {}, {});
  add(ConstantLiteral{constant(TypeId{1}, UnitLiteral{})}, 2, {}, {}, {});
  add(VecSlice{std::nullopt, std::nullopt}, 3, {}, {}, {});
  add(ForIn{SymbolId{651}, ScopeId{20}, HirNodeId{42}, std::nullopt}, 4, {},
      {}, {});
  add(Call{CalleeKind::direct,
           signature,
           std::nullopt,
           std::nullopt,
           {},
           {},
           std::nullopt,
           std::nullopt,
           {},
           std::nullopt},
      5, {}, {}, {});
  add(Move{std::nullopt}, 6, {}, {}, {});
  add(ForeignRead{ForeignBindingId{652}, std::nullopt, std::nullopt,
                  std::nullopt},
      7, {}, {}, {});
  add(TraitObjectInit{trait_ref_value, TypeId{10}, {SymbolId{653}, SymbolId{654}},
                      {}},
      8, {}, {}, {});
  add(Switch{{Arm{pattern(TypeId{1}, at(90), std::nullopt, std::nullopt, {},
                           std::nullopt),
                  ScopeId{21}, HirNodeId{43}, at(91)},
              Arm{pattern(TypeId{999}, at(92), std::nullopt, std::nullopt, {},
                          PatternLiteral{IntegerLiteral{"5", 5}}),
                  ScopeId{22}, HirNodeId{44}, at(93)}},
             false},
      9, {}, {}, {});
  add(UnitLiteral{}, 10, {}, {}, {});
  // Every remaining enum ordinal used by the dump, so a mistaken mapping
  // cannot hide behind a single representative value.
  add(Unary{UnaryOp::negate}, 11, {}, {}, {});
  add(Unary{UnaryOp::logical_not}, 12, {}, {}, {});
  add(Binary{BinaryOp::add}, 13, {}, {}, {});
  add(Binary{BinaryOp::subtract}, 14, {}, {}, {});
  add(Binary{BinaryOp::multiply}, 15, {}, {}, {});
  add(Binary{BinaryOp::divide}, 16, {}, {}, {});
  add(Binary{BinaryOp::remainder}, 17, {}, {}, {});
  add(Binary{BinaryOp::shift_left}, 18, {}, {}, {});
  add(Binary{BinaryOp::shift_right}, 19, {}, {}, {});
  add(Binary{BinaryOp::bit_and}, 20, {}, {}, {});
  add(Binary{BinaryOp::bit_or}, 21, {}, {}, {});
  add(Binary{BinaryOp::bit_xor}, 22, {}, {}, {});
  add(Binary{BinaryOp::equal}, 23, {}, {}, {});
  add(Binary{BinaryOp::not_equal}, 24, {}, {}, {});
  add(Binary{BinaryOp::less}, 25, {}, {}, {});
  add(Binary{BinaryOp::less_equal}, 26, {}, {}, {});
  add(Binary{BinaryOp::greater}, 27, {}, {}, {});
  add(Binary{BinaryOp::greater_equal}, 28, {}, {}, {});
  add(ShortCircuit{LogicalOp::and_}, 29, {}, {}, {});
  add(Call{CalleeKind::function_value, signature, SymbolId{670},
           HirNodeId{70}, {}, {}, std::nullopt, std::nullopt, {},
           std::nullopt},
      30, {}, {}, {});
  add(Call{CalleeKind::foreign, signature, SymbolId{671},
           std::nullopt, {}, {}, std::nullopt, std::nullopt, {},
           std::nullopt},
      31, {}, {}, {});
  add(Call{CalleeKind::system, signature, std::nullopt, std::nullopt, {}, {},
           std::nullopt, std::nullopt, {}, static_cast<SystemOperation>(1)},
      32, {}, {}, {});
  module.nodes[0].exits = all_exits;
  module.nodes.back().type = TypeId{999};
  module.nodes.back().scope = ScopeId{99};

  module.foreign_bindings = {
      ForeignBinding{ForeignBindingId{5}, TypeId{1}, signature, TypeId{10},
                     TypeId{14}},
      ForeignBinding{ForeignBindingId{6}, TypeId{2}, std::nullopt, std::nullopt,
                     TypeId{13}}};

  ValueUse first_use;
  first_use.consumer = HirNodeId{60};
  first_use.operand = 3;
  first_use.kind = UseKind::inout;
  first_use.root = SymbolId{660};
  first_use.type = TypeId{10};
  first_use.destination = SymbolId{661};
  module.ownership.uses.push_back(first_use);
  ValueUse second_use;
  second_use.consumer = HirNodeId{61};
  second_use.operand = 4;
  second_use.kind = UseKind::take;
  second_use.type = TypeId{11};
  module.ownership.uses.push_back(second_use);
  ValueUse borrow_use;
  borrow_use.consumer = HirNodeId{66};
  borrow_use.operand = 5;
  borrow_use.kind = UseKind::borrow;
  borrow_use.type = TypeId{12};
  module.ownership.uses.push_back(borrow_use);
  ValueUse copy_use;
  copy_use.consumer = HirNodeId{67};
  copy_use.operand = 6;
  copy_use.kind = UseKind::copy;
  copy_use.type = TypeId{13};
  module.ownership.uses.push_back(copy_use);

  module.ownership.temporaries.push_back(
      Temporary{HirNodeId{62}, HirNodeId{63}, TypeId{12}});

  LocalLifetime borrowed;
  borrowed.symbol = SymbolId{662};
  borrowed.scope = ScopeId{22};
  borrowed.type = TypeId{13};
  borrowed.order = 9;
  borrowed.mode = ParamMode::inout;
  borrowed.read_only_capture = true;
  module.ownership.locals.push_back(borrowed);
  LocalLifetime ordinary;
  ordinary.symbol = SymbolId{663};
  ordinary.scope = ScopeId{23};
  ordinary.type = TypeId{14};
  ordinary.order = 10;
  ordinary.mode = ParamMode::ordinary;
  module.ownership.locals.push_back(ordinary);

  module.ownership.full_expressions.push_back(
      FullExpression{HirNodeId{64},
                     {cleanup(CleanupKind::defer_, OwnerKind::iteration, 705,
                              TypeId{15}),
                      cleanup(CleanupKind::end_iteration, OwnerKind::binding,
                              706, TypeId{16})}});
  module.ownership.calls.push_back(
      CallLifetime{HirNodeId{65},
                   {cleanup(CleanupKind::destroy_if_live,
                            OwnerKind::error_output, 707, TypeId{17})}});

  module.types = std::move(context);
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: oracle FIXTURE [width-error]\n";
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary);
  if (!input) {
    std::cerr << "cannot read fixture: " << argv[1] << "\n";
    return 2;
  }
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  const std::string mode = argc > 2 ? argv[2] : "full";
  try {
    auto snapshot = std::make_shared<SourceSnapshot>(
        FileId{1}, RevisionId{1}, SourceSnapshotId{1}, "fixture.wz", text);
    auto lexed = lex(snapshot);
    if (!lexed.success()) {
      std::cerr << "lex failed\n";
      return 3;
    }
    auto parsed = parse(*lexed.value);
    if (!parsed.success()) {
      std::cerr << "parse failed\n";
      return 3;
    }
    TypeContext context;
    const auto signature = build_types(context);
    HirModule module;
    auto project = std::make_shared<modules::Project>();
    auto resolved = std::make_shared<modules::ResolvedProject>();
    resolved->project = project;
    module.resolved = resolved;
    build_module(*parsed.value, std::move(context), signature, module);

    if (mode == "width-error") {
      module.config.word_bits = 48;
      try {
        static_cast<void>(dump(module));
        std::cerr << "width-error: no exception\n";
        return 5;
      } catch (const std::invalid_argument &error) {
        std::cout << "ERROR: " << error.what() << "\n";
        return 0;
      }
    }
    std::cout << "== types\n" << dump(module.types) << "== module\n"
              << dump(module);
    TypeContext empty;
    std::cout << "== empty-types\ntypes=" << dump(empty).size() << "\n";
    HirModule empty_module;
    std::cout << "== empty-module\n" << dump(empty_module);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "oracle exception: " << error.what() << "\n";
    return 4;
  }
}
