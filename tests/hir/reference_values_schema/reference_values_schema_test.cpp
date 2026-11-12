// Stage-A probe for the shared reference-values HIR schema.
//
// The compiled header must expose the capture/bound/Weak fields without
// changing any published enum value or old aggregate initializer. HIR
// verification must reject every nondefault field because the creator/lambda
// source and ownership paths are not implemented yet. These are not mock
// language tests: the positive fixture is produced by the real lexer, parser,
// resolver and checker, then each new field is forged on a copy.
#include "lang/hir/hir.hpp"
#include "lang/modules/resolve.hpp"
#include "lang/sema/check.hpp"
#include "lang/syntax/parser.hpp"
#include "lang/syntax/token.hpp"
#include <cstddef>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
using namespace lang;
using namespace lang::hir;

namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
bool contains(const std::string &text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

// Layout and member probes for the shared schema. The sizes/offsets are the
// Linux x86_64 libstdc++ ABI used by the project's Native/64 target; the
// plain-value assertions document the field additions themselves.
static_assert(std::is_aggregate_v<CaptureBinding>);
static_assert(std::is_aggregate_v<CaptureSource>);
static_assert(std::is_aggregate_v<BoundReceiver>);
static_assert(std::is_aggregate_v<FunctionValue>);
static_assert(std::is_aggregate_v<HirFunction>);
static_assert(std::is_aggregate_v<LocalLifetime>);
static_assert(std::is_same_v<decltype(CaptureBinding::origin), SymbolId>);
static_assert(std::is_same_v<decltype(CaptureBinding::local), SymbolId>);
static_assert(std::is_same_v<decltype(CaptureBinding::type), TypeId>);
static_assert(std::is_same_v<decltype(CaptureSource::origin), SymbolId>);
static_assert(std::is_same_v<decltype(CaptureSource::source), SymbolId>);
static_assert(std::is_same_v<decltype(CaptureSource::type), TypeId>);
static_assert(std::is_same_v<decltype(CaptureSource::move), bool>);
static_assert(std::is_same_v<decltype(BoundReceiver::type), TypeId>);
static_assert(std::is_same_v<decltype(BoundReceiver::mode), ParamMode>);
static_assert(std::is_same_v<decltype(FunctionValue::symbol), SymbolId>);
static_assert(std::is_same_v<decltype(FunctionValue::type_args),
                             std::vector<TypeId>>);
static_assert(std::is_same_v<decltype(FunctionValue::captures),
                             std::vector<CaptureSource>>);
static_assert(std::is_same_v<decltype(FunctionValue::move_capture), bool>);
static_assert(std::is_same_v<decltype(FunctionValue::bound),
                             std::optional<BoundReceiver>>);
static_assert(std::is_same_v<decltype(HirFunction::captures),
                             std::vector<CaptureBinding>>);
static_assert(std::is_same_v<decltype(LocalLifetime::read_only_capture), bool>);

static_assert(sizeof(CaptureBinding) == 24);
static_assert(alignof(CaptureBinding) == 8);
static_assert(sizeof(CaptureSource) == 32);
static_assert(alignof(CaptureSource) == 8);
static_assert(sizeof(BoundReceiver) == 16);
static_assert(alignof(BoundReceiver) == 8);
static_assert(sizeof(FunctionValue) == 88);
static_assert(alignof(FunctionValue) == 8);
static_assert(sizeof(HirFunction) == 128);
static_assert(alignof(HirFunction) == 8);
static_assert(sizeof(LocalLifetime) == 40);
static_assert(alignof(LocalLifetime) == 8);
static_assert(std::is_standard_layout_v<FunctionValue>);
static_assert(std::is_standard_layout_v<LocalLifetime>);
static_assert(offsetof(FunctionValue, symbol) == 0);
static_assert(offsetof(FunctionValue, type_args) == 8);
static_assert(offsetof(FunctionValue, captures) == 32);
static_assert(offsetof(FunctionValue, move_capture) == 56);
static_assert(offsetof(FunctionValue, bound) == 64);
static_assert(offsetof(HirFunction, source) == 56);
static_assert(offsetof(HirFunction, captures) == 104);
static_assert(offsetof(LocalLifetime, mode) == 28);
static_assert(offsetof(LocalLifetime, read_only_capture) == 32);

// Appended enum values preserve every published number.
static_assert(static_cast<int>(TypeKind::Weak) ==
              static_cast<int>(TypeKind::Slice) + 1);
static_assert(static_cast<int>(ValueKind::function) ==
              static_cast<int>(ValueKind::slice) + 1);
static_assert(static_cast<int>(ValueKind::weak) ==
              static_cast<int>(ValueKind::function) + 1);

struct Frontend {
  SourceManager sources;
  std::shared_ptr<modules::ResolvedProject> resolved;
  explicit Frontend(std::string_view code) {
    auto source = sources.update(sources.add_file("fixture.wz"),
                                 std::string(code));
    auto tokens = lex(source);
    require(tokens.success(), "fixture lexer failed");
    auto ast = parse(*tokens.value);
    require(ast.success(), "fixture parser failed");
    auto project = std::make_shared<modules::Project>();
    project->backend = "native";
    modules::Module module;
    module.id = ModuleId{0};
    module.logical_path = "app";
    module.files.push_back(
        std::make_shared<const AstFile>(std::move(*ast.value)));
    project->modules.push_back(std::move(module));
    auto names = modules::resolve_project(project);
    require(names.success(), "fixture resolver failed");
    resolved =
        std::make_shared<modules::ResolvedProject>(std::move(*names.value));
  }
};

TypeId find_type(const HirModule &m, TypeKind kind, unsigned bits = 0) {
  for (uint64_t i = 1; i <= m.types.size(); ++i) {
    const auto &t = *m.types.find(TypeId{i});
    if (t.kind == kind && (kind != TypeKind::SignedInt || t.bits == bits))
      return TypeId{i};
  }
  throw std::runtime_error("fixture type is missing");
}

CaptureBinding capture_binding(const HirModule &m) {
  const auto &symbol = m.resolved->symbols.at(0);
  return {symbol.id, symbol.id, find_type(m, TypeKind::SignedInt, 32)};
}

std::optional<HirNodeId> function_value_node(const HirModule &m) {
  for (size_t i = 0; i < m.nodes.size(); ++i)
    if (std::holds_alternative<FunctionValue>(m.nodes[i].data))
      return HirNodeId{i};
  return {};
}

std::optional<HirNodeId> constant_function_node(const HirModule &m) {
  for (size_t i = 0; i < m.nodes.size(); ++i) {
    const auto *literal = std::get_if<ConstantLiteral>(&m.nodes[i].data);
    if (!literal)
      continue;
    const auto *aggregate =
        std::get_if<ConstantAggregate>(&literal->value.data);
    if (!aggregate)
      continue;
    for (const auto &field : aggregate->fields)
      if (std::holds_alternative<FunctionValue>(field.data))
        return HirNodeId{i};
  }
  return {};
}

FunctionValue *nested_constant_function(HirModule &m, HirNodeId node) {
  auto &literal = std::get<ConstantLiteral>(m.nodes[node.value].data);
  auto &aggregate = std::get<ConstantAggregate>(literal.value.data);
  for (auto &field : aggregate.fields)
    if (auto *value = std::get_if<FunctionValue>(&field.data))
      return value;
  throw std::runtime_error("fixture constant function field is missing");
}

void rejects(const HirModule &m, std::string_view message, const char *name) {
  auto result = verify(m);
  require(!result.success() && !result.value && !result.diagnostics.empty(),
          std::string("forged field accepted: ") + name);
  require(result.diagnostics[0].code == "E_IR_INVALID" &&
              result.diagnostics[0].phase == "lowering",
          std::string("wrong rejection category: ") + name);
  require(contains(result.diagnostics[0].message, message),
          std::string("wrong rejection reason: ") + name + ": " +
              result.diagnostics[0].message);
}

void runtime() {
  // The real frontend produces a noncapturing function value, a constant
  // aggregate containing one, and owning locals. It must keep verifying.
  Frontend frontend(R"(package main;
func inc(x i32) i32 { return x + 1; }
struct H { cb func(i32) i32; }
const OP H = H(cb: inc);
func main() i32 {
  let f func(i32) i32 = inc;
  let v Vec[i32] = [1, 2];
  return OP.cb(2) + f(1) + v[0];
}
)");
  auto typed = sema::check_types(frontend.resolved,
                                 Config{Backend::native, 64});
  if (!typed.success())
    for (const auto &diagnostic : typed.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  require(typed.success(), "frontend fixture did not produce HIR");
  const HirModule &baseline = *typed.value;
  auto valid = verify(baseline);
  if (!valid.success())
    for (const auto &diagnostic : valid.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  require(valid.success(), "old noncapturing HIR sample was rejected");

  // Every appended field keeps its default on real old-style HIR.
  for (const auto &function : baseline.functions)
    require(function.captures.empty(), "old HirFunction gained captures");
  for (const auto &local : baseline.ownership.locals)
    require(!local.read_only_capture, "old local became a capture local");
  require(!baseline.ownership.locals.empty(),
          "fixture ownership locals are missing");
  // Default dump must retain the old lifetime line exactly; the new marker is
  // only emitted for a true value.
  require(!contains(dump(baseline), "read_only_capture"),
          "default lifetime dump gained a capture marker");
  auto function_node = function_value_node(baseline);
  require(function_node.has_value(), "fixture function value is missing");
  {
    const auto &value =
        std::get<FunctionValue>(baseline.nodes[function_node->value].data);
    require(value.captures.empty() && !value.move_capture && !value.bound,
            "old FunctionValue gained capture metadata");
    const auto &ops =
        classify_value(baseline, baseline.nodes[function_node->value].type);
    require(ops.kind == ValueKind::trivial && !ops.needs_drop &&
                !ops.pod_inout,
            "stage A changed the existing Function classification");
  }

  // Old aggregate initializers remain valid and mean the same value. The
  // appended vector/optional members have explicit defaults, so the pre-schema
  // arity also stays clean under -Wall -Wextra -Werror.
  FunctionValue legacy_function_value{SymbolId{1}, {TypeId{2}}};
  require(legacy_function_value.captures.empty() &&
              !legacy_function_value.move_capture &&
              !legacy_function_value.bound,
          "two-field FunctionValue initializer is not noncapturing");
  HirFunction legacy_hir_function{SymbolId{1}, TypeId{2}, {},
                                  HirNodeId{0}, ScopeId{0}, Source{}};
  require(legacy_hir_function.captures.empty(),
          "six-field HirFunction initializer is not ordinary");
  LocalLifetime legacy_lifetime{SymbolId{1}, ScopeId{0}, TypeId{2}, 0,
                                ParamMode::ordinary};
  require(!legacy_lifetime.read_only_capture,
          "five-field LocalLifetime initializer is not an ordinary local");

  const auto i32 = find_type(baseline, TypeKind::SignedInt, 32);
  {
    auto m = baseline;
    auto &value = std::get<FunctionValue>(
        m.nodes[function_node->value].data);
    value.captures.push_back({SymbolId{1}, SymbolId{2}, i32, false});
    rejects(m, "captured or bound function value", "FunctionValue.captures");
  }
  {
    auto m = baseline;
    auto &value = std::get<FunctionValue>(
        m.nodes[function_node->value].data);
    value.captures.push_back({SymbolId{1}, SymbolId{2}, i32, true});
    rejects(m, "captured or bound function value",
            "CaptureSource.move");
  }
  {
    auto m = baseline;
    std::get<FunctionValue>(m.nodes[function_node->value].data).move_capture =
        true;
    rejects(m, "captured or bound function value",
            "FunctionValue.move_capture");
  }
  {
    auto m = baseline;
    std::get<FunctionValue>(m.nodes[function_node->value].data).bound =
        BoundReceiver{i32, ParamMode::ordinary};
    rejects(m, "captured or bound function value", "FunctionValue.bound");
  }
  {
    auto m = baseline;
    std::get<FunctionValue>(m.nodes[function_node->value].data).bound =
        BoundReceiver{i32, ParamMode::move};
    rejects(m, "captured or bound function value",
            "BoundReceiver.mode=move");
  }
  {
    auto m = baseline;
    m.functions.at(0).captures.push_back(capture_binding(m));
    rejects(m, "HirFunction captures", "HirFunction.captures");
  }
  {
    auto m = baseline;
    auto node = constant_function_node(m);
    require(node.has_value(), "fixture constant function value is missing");
    auto &constant = *nested_constant_function(m, *node);
    constant.captures.push_back({SymbolId{1}, SymbolId{2}, i32, false});
    rejects(m, "constant function value cannot capture",
            "constant FunctionValue.captures");
  }
  {
    auto m = baseline;
    auto node = constant_function_node(m);
    require(node.has_value(), "fixture constant function value is missing");
    auto &constant = *nested_constant_function(m, *node);
    constant.bound = BoundReceiver{i32, ParamMode::ordinary};
    rejects(m, "constant function value cannot capture",
            "constant FunctionValue.bound");
  }
  {
    auto m = baseline;
    auto node = constant_function_node(m);
    require(node.has_value(), "fixture constant function value is missing");
    nested_constant_function(m, *node)->move_capture = true;
    rejects(m, "constant function value cannot capture",
            "constant FunctionValue.move_capture");
  }
  {
    auto m = baseline;
    require(!m.ownership.locals.empty(),
            "fixture ownership locals are missing");
    m.ownership.locals.at(0).read_only_capture = true;
    require(contains(dump(m), "read_only_capture"),
            "true capture lifetime has no dump marker");
    rejects(m, "read-only capture lifetime", "LocalLifetime.read_only_capture");
  }
  {
    // Weak must be internable as a shape (arity one) for the next stage, but
    // HIR verification must not silently classify it as a trivial value.
    auto m = baseline;
    Type weak;
    weak.kind = TypeKind::Weak;
    weak.type_args = {i32};
    auto id = m.types.intern(weak);
    require(id.value != 0, "Weak TypeId is invalid");
    rejects(m, "Weak type has no verified source construction path",
            "TypeKind::Weak");
    Type bad;
    bad.kind = TypeKind::Weak;
    bool arity0 = false, arity2 = false;
    try {
      (void)m.types.intern(bad);
    } catch (const std::invalid_argument &) {
      arity0 = true;
    }
    bad.type_args = {i32, i32};
    try {
      (void)m.types.intern(bad);
    } catch (const std::invalid_argument &) {
      arity2 = true;
    }
    require(arity0 && arity2, "Weak arity was not validated");
  }

  // Forging the same field on a different copy must not poison the baseline.
  require(verify(baseline).success(), "baseline verification was mutated");
  std::cout << "reference-values schema passed:"
            << " sizes CaptureBinding=" << sizeof(CaptureBinding)
            << " CaptureSource=" << sizeof(CaptureSource)
            << " BoundReceiver=" << sizeof(BoundReceiver)
            << " FunctionValue=" << sizeof(FunctionValue)
            << " HirFunction=" << sizeof(HirFunction)
            << " LocalLifetime=" << sizeof(LocalLifetime)
            << "; old HIR accepted; captures/move/bound/HirFunction.captures/"
               "constant/read_only_capture/Weak rejected\n";
}
} // namespace

int main() {
  try {
    runtime();
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
