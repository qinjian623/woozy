// Test-only C++ oracle. Links stage0 libs, parses the same real fixture bytes,
// builds the equivalent HIR records and exercises the accepted C++ verifier.
#include "lang/hir/hir.hpp"
#include "lang/modules/resolve.hpp"
#include "lang/sema/check.hpp"
#include "lang/support/source.hpp"
#include "lang/syntax/parser.hpp"
#include "lang/syntax/token.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace lang;
using namespace lang::hir;

namespace {
struct Built {
  HirModule module;
  bool ok = false;
};

std::string phase(PhaseState state) {
  switch (state) {
  case PhaseState::complete: return "complete";
  case PhaseState::recovered: return "recovered";
  case PhaseState::failed: return "failed";
  case PhaseState::cancelled: return "cancelled";
  }
  return "unknown";
}

std::string first_diag(const PhaseResult<std::monostate> &result) {
  if (result.diagnostics.empty()) return "none";
  return result.diagnostics.front().code + ":" +
         result.diagnostics.front().message;
}

Built build(const AstFile &parsed) {
  Built built;
  size_t function_node = 0, block_node = 0, named_node = 0;
  for (size_t i = 0; i < parsed.nodes.size(); ++i) {
    auto kind = parsed.nodes[i].kind;
    if (kind == AstKind::Function) function_node = i;
    if (kind == AstKind::NamedType) named_node = i;
    if (kind == AstKind::Block) block_node = i;
  }
  TypeContext context;
  Type unit_type;
  unit_type.kind = TypeKind::Unit;
  TypeId unit = context.intern(unit_type);
  Type signature_type;
  signature_type.kind = TypeKind::Function;
  signature_type.result = unit;
  TypeId signature = context.intern(signature_type);

  auto project = std::make_shared<modules::Project>();
  project->package_name = "app";
  project->backend = "native";
  project->word_bits = 64;
  project->entry = ModuleId{0};
  modules::Module mod;
  mod.id = ModuleId{0};
  mod.logical_path = "app";
  mod.identity = "app";
  mod.package_name = "app";
  mod.files.push_back(std::make_shared<const AstFile>(parsed));
  project->modules.push_back(mod);

  modules::NodeRef function_ref{ModuleId{0}, 0, AstNodeId{function_node}};
  modules::NodeRef block_ref{ModuleId{0}, 0, AstNodeId{block_node}};
  modules::NodeRef named_ref{ModuleId{0}, 0, AstNodeId{named_node}};
  auto resolved = std::make_shared<modules::ResolvedProject>();
  resolved->project = project;
  modules::Symbol function_symbol;
  function_symbol.id = SymbolId{0};
  function_symbol.name = "main";
  function_symbol.kind = modules::SymbolKind::function;
  function_symbol.module = ModuleId{0};
  function_symbol.definition = function_ref;
  function_symbol.declaration = parsed.nodes[function_node].span;
  function_symbol.exported = true;
  resolved->symbols.push_back(function_symbol);
  modules::Symbol builtin;
  builtin.id = SymbolId{1};
  builtin.name = "Unit";
  builtin.kind = modules::SymbolKind::builtin_type;
  builtin.module = ModuleId{0};
  builtin.exported = true;
  resolved->symbols.push_back(builtin);
  resolved->occurrences.push_back(modules::Occurrence{
      named_ref, SymbolId{1}, parsed.nodes[named_node].span});

  HirModule module;
  module.resolved = resolved;
  module.identity = ModuleId{0};
  module.types = context;
  module.config = Config{Backend::native, 64};
  HirFunction function;
  function.symbol = SymbolId{0};
  function.signature = signature;
  function.body = HirNodeId{0};
  function.scope = ScopeId{0};
  function.source = Source{function_ref, parsed.nodes[function_node].span};
  module.functions.push_back(function);
  HirNode block;
  block.type = unit;
  block.source = Source{block_ref, parsed.nodes[block_node].span};
  block.scope = ScopeId{0};
  block.data = Block{};
  module.nodes.push_back(block);
  Scope scope;
  scope.function = SymbolId{0};
  scope.source = Source{block_ref, parsed.nodes[block_node].span};
  module.scopes.push_back(scope);
  built.module = std::move(module);
  built.ok = true;
  return built;
}


Built build_full(const AstFile &parsed) {
  Built built;
  auto project = std::make_shared<modules::Project>();
  project->package_name = "app";
  project->backend = "native";
  project->word_bits = 64;
  project->entry = ModuleId{0};
  modules::Module mod;
  mod.id = ModuleId{0};
  mod.logical_path = "app";
  mod.identity = "app";
  mod.package_name = "app";
  mod.files.push_back(std::make_shared<const AstFile>(parsed));
  project->modules.push_back(mod);
  auto resolved = modules::resolve_project(project);
  if (!resolved.value) return built;
  auto checked = sema::check_types(resolved);
  if (!checked.value) return built;
  built.module = std::move(*checked.value);
  built.ok = true;
  return built;
}

int probe_fix_mode(const char *mode, const AstFile &parsed) {
  auto built = build_full(parsed);
  if (!built.ok) {
    std::cout << "PROBE cpp-" << mode << " build-failed\n";
    return 3;
  }
  auto typed = verify_types(built.module);
  std::cout << "PROBE cpp-type-" << mode << " state=" << phase(typed.state)
            << " diags=" << typed.diagnostics.size() << " detail="
            << first_diag(typed) << "\n";
  auto capability = check_backend_capabilities(built.module);
  std::cout << "PROBE cpp-capability-" << mode << " state="
            << phase(capability.state) << " diags="
            << capability.diagnostics.size() << "\n";
  auto verified = verify(built.module, {}, VerifyMode::backend);
  std::cout << "PROBE cpp-verify-" << mode << " state="
            << phase(verified.state) << " has=" << (verified.value ? 1 : 0)
            << " diags=" << verified.diagnostics.size() << "\n";
  if (std::string(mode) == "trait") {
    auto bad = built.module;
    bad.types = built.module.types;
    bad.impls.push_back(bad.impls.at(0));
    auto overlap = verify_types(bad);
    std::cout << "PROBE cpp-trait-overlap state=" << phase(overlap.state)
              << " detail=" << first_diag(overlap) << "\n";
  }
  return (typed.state == PhaseState::complete && typed.diagnostics.empty() &&
          capability.state == PhaseState::complete &&
          capability.diagnostics.empty() && verified.value)
             ? 0
             : 1;
}

void probe(const char *name, const HirModule &module) {
  auto result = verify_types(module);
  std::cout << "PROBE " << name << " state=" << phase(result.state)
            << " diags=" << result.diagnostics.size()
            << " detail=" << first_diag(result) << "\n";
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  std::ifstream input(argv[1], std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(input)),
                   std::istreambuf_iterator<char>());
  try {
    auto snapshot = std::make_shared<SourceSnapshot>(
        FileId{1}, RevisionId{1}, SourceSnapshotId{1}, "fixture.wz", text);
    auto lexed = lex(snapshot);
    auto parsed = parse(*lexed.value);
    if (argc > 2 && std::string(argv[2]) != "unit") {
      return probe_fix_mode(argv[2], *parsed.value);
    }
    auto built = build(*parsed.value);
    if (!built.ok) return 3;
    probe("valid", built.module);
    auto backend = check_backend_capabilities(built.module);
    std::cout << "PROBE backend state=" << phase(backend.state)
              << " diags=" << backend.diagnostics.size() << " detail="
              << (backend.diagnostics.empty()
                      ? "none"
                      : backend.diagnostics.front().code + ":" +
                            backend.diagnostics.front().message)
              << "\n";
    auto verified = verify(built.module, {}, VerifyMode::backend);
    std::cout << "PROBE verify state=" << phase(verified.state)
              << " diags=" << verified.diagnostics.size() << " has="
              << (verified.value ? 1 : 0) << "\n";

    auto bad = built.module;
    bad.types = built.module.types;
    bad.config.word_bits = 48;
    auto width = verify_types(bad);
    std::cout << "PROBE bad-width state=" << phase(width.state)
              << " detail=" << first_diag(width) << "\n";

    bad = built.module;
    bad.types = built.module.types;
    bad.nodes[0].type = TypeId{999};
    auto bad_type = verify_types(bad);
    std::cout << "PROBE bad-type detail=" << first_diag(bad_type) << "\n";

    bad = built.module;
    bad.types = built.module.types;
    bad.nodes[0].children = {HirNodeId{77}};
    auto bad_child = verify_types(bad);
    std::cout << "PROBE bad-child detail=" << first_diag(bad_child) << "\n";

    bad = built.module;
    bad.types = built.module.types;
    bad.functions[0].source.span.end_byte =
        bad.functions[0].source.span.begin_byte - 1;
    auto bad_source = verify_types(bad);
    std::cout << "PROBE bad-source detail=" << first_diag(bad_source) << "\n";

    bad = built.module;
    bad.types = built.module.types;
    bad.nodes[0].data = Unsupported{"probe"};
    auto bad_unsupported = verify_types(bad);
    std::cout << "PROBE bad-unsupported detail="
              << first_diag(bad_unsupported) << "\n";

    bad = built.module;
    bad.types = built.module.types;
    bad.nodes[0].children = {HirNodeId{0}};
    auto bad_cycle = verify_types(bad);
    std::cout << "PROBE bad-cycle detail=" << first_diag(bad_cycle) << "\n";

    bad = built.module;
    bad.types = built.module.types;
    bad.scopes[0].parent = ScopeId{0};
    auto bad_scope = verify_types(bad);
    std::cout << "PROBE bad-scope detail=" << first_diag(bad_scope) << "\n";
    auto bad_verify = verify(bad, {}, VerifyMode::backend);
    std::cout << "PROBE bad-verify has=" << (bad_verify.value ? 1 : 0)
              << " detail="
              << (bad_verify.diagnostics.empty()
                      ? "none"
                      : bad_verify.diagnostics.front().code + ":" +
                            bad_verify.diagnostics.front().message)
              << "\n";
    std::cout << "SUMMARY probes=13 fixture=" << argv[1] << "\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "oracle exception: " << error.what() << "\n";
    return 4;
  }
}
