// Python reference-value backend probes.
//
// Two separate claims are checked here and must not be merged:
//
// 1. Real old source that uses noncapturing function values, indirect calls,
//    typed throws, inout logs and function types as generic arguments still
//    emits and runs. The emitted function descriptors are now
//    `_rt.ValueType('function')`.
// 2. Capturing closures, bound method values and Weak are NOT reachable from
//    verified HIR at this revision: the shared verifier rejects every
//    nondefault FunctionValue.captures/bound field and every Weak type. This
//    test forges those fields on a real checked module and requires the
//    published E_IR_INVALID rejection. It never bypasses verification and
//    therefore claims no source capture/bound/Weak pass.
#include "lang/backend/python.hpp"
#include "lang/hir/hir.hpp"
#include "lang/modules/resolve.hpp"
#include "lang/sema/check.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

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

VerifiedHirModule checked(const std::filesystem::path &root,
                          const char *manifest) {
  SourceManager sources;
  auto loaded = modules::load_project({root / manifest, {}, {}}, sources);
  require(loaded.success(), std::string("real project load: ") + manifest);
  loaded.value->backend = "python";
  loaded.value->word_bits = 64;
  auto resolved = modules::resolve_project(
      std::make_shared<const modules::Project>(std::move(*loaded.value)));
  require(resolved.success(), std::string("real project resolve: ") + manifest);
  auto result = sema::check_project(resolved);
  if (!result.success())
    for (const auto &diagnostic : result.diagnostics)
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  require(result.success(), std::string("real project check: ") + manifest);
  return std::move(*result.value);
}

SymbolId entry(const HirModule &m) {
  for (const auto &f : m.functions)
    if (m.resolved->symbols.at(f.symbol.value).name == "main")
      return f.symbol;
  throw std::runtime_error("no main");
}

void old_source(const std::filesystem::path &root, const char *manifest) {
  auto verified = checked(root, manifest);
  auto first = backend::python::emit(verified, entry(verified.module()));
  require(first.success(), std::string("emit old source: ") + manifest);
  const auto code = first.value->code;
  // A function type now has the owning descriptor identity.
  require(contains(code, "_rt.ValueType('function')"),
          std::string("function descriptor missing: ") + manifest);
  // Noncapturing values keep the original immutable Python function and no
  // capture/bound machinery is generated for them.
  require(!contains(code, "_rt.FunctionValue("),
          std::string("old source gained a capture wrapper: ") + manifest);
  require(!contains(code, "_bound"),
          std::string("old source gained a bound adapter: ") + manifest);
  // Stage-A compatibility: the shared classifier still reports Function as
  // non-owning, so old function bindings stay plain names. The emitter's
  // owning branch activates only when the published ValueKind::function is
  // produced by the shared ownership analysis.
  for (const auto &node : verified.module().nodes)
    if (const auto *value = std::get_if<FunctionValue>(&node.data)) {
      require(value->captures.empty() && !value->move_capture && !value->bound,
              "old FunctionValue gained capture metadata");
      const auto ops = classify_value(verified.module(), node.type);
      require(ops.kind == ValueKind::trivial && !ops.needs_drop,
              "shared Function classification changed without ownership work");
    }
  const auto again = backend::python::emit(verified, entry(verified.module()));
  require(again.success() && again.value->code == code,
          std::string("emit is not deterministic: ") + manifest);
}

void stage_boundary(const std::filesystem::path &root) {
  auto verified =
      checked(root, "tests/backend_common/fixtures/function-values/lang.toml");
  const auto &baseline = verified.module();
  const FunctionValue *function = nullptr;
  size_t function_index = 0;
  for (size_t i = 0; i < baseline.nodes.size(); ++i)
    if (const auto *value =
            std::get_if<FunctionValue>(&baseline.nodes[i].data)) {
      function = value;
      function_index = i;
      break;
    }
  require(function != nullptr, "old source has no function value");
  TypeId capture_type{};
  for (uint64_t i = 1; i <= baseline.types.size(); ++i)
    if (baseline.types.find(TypeId{i})->kind == TypeKind::SignedInt &&
        baseline.types.find(TypeId{i})->bits == 32)
      capture_type = TypeId{i};
  require(capture_type.value != 0, "i32 type is missing");
  const auto symbol = baseline.resolved->symbols.at(0).id;
  auto rejects = [&](const HirModule &m, std::string_view message,
                     const char *name) {
    auto result = verify(m);
    if (result.success())
      for (const auto &d : result.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    require(!result.success() && !result.value, name);
    require(!result.diagnostics.empty() &&
                result.diagnostics.front().code == "E_IR_INVALID",
            std::string("wrong stage diagnostic: ") + name);
    require(contains(result.diagnostics.front().message, message),
            std::string("wrong stage reason: ") + name + ": " +
                result.diagnostics.front().message);
  };
  {
    auto m = baseline;
    std::get<FunctionValue>(m.nodes[function_index].data)
        .captures.push_back({symbol, symbol, capture_type, false});
    rejects(m, "captured or bound function value", "captures");
  }
  {
    auto m = baseline;
    std::get<FunctionValue>(m.nodes[function_index].data).move_capture = true;
    rejects(m, "captured or bound function value", "move_capture");
  }
  {
    auto m = baseline;
    std::get<FunctionValue>(m.nodes[function_index].data).bound =
        BoundReceiver{capture_type, ParamMode::ordinary};
    rejects(m, "captured or bound function value", "bound ordinary");
  }
  {
    auto m = baseline;
    std::get<FunctionValue>(m.nodes[function_index].data).bound =
        BoundReceiver{capture_type, ParamMode::move};
    rejects(m, "captured or bound function value", "bound move");
  }
  {
    auto m = baseline;
    m.functions.at(0).captures.push_back({symbol, symbol, capture_type});
    rejects(m, "HirFunction captures", "HirFunction.captures");
  }
  {
    auto m = baseline;
    Type weak;
    weak.kind = TypeKind::Weak;
    weak.type_args = {capture_type};
    auto id = m.types.intern(weak);
    require(id.value != 0, "Weak shape was not internable");
    rejects(m, "Weak type has no verified source construction path", "Weak");
  }
  // The unmodified module still emits; forged capture/bound modules cannot
  // reach the backend because verify() publishes no VerifiedHirModule.
  require(verify(baseline).success(), "baseline was poisoned");
  require(backend::python::emit(verified, entry(baseline)).success(),
          "old source stopped emitting");
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "source root argument");
    const std::filesystem::path root = argv[1];
    old_source(root, "tests/backend_common/fixtures/function-values/lang.toml");
    old_source(root, "tests/cleanup/fixtures/case-08/lang.toml");
    old_source(root, "tests/generics/fixtures/function-argument/lang.toml");
    stage_boundary(root);
    std::cout << "python reference-values backend probe passed:"
              << " old source emits _rt.ValueType('function') and stays"
              << " wrapper/adapter free; captures/move/bound/HirFunction"
              << " captures/Weak still rejected by shared verification\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
