#include "lang/hir/identity.hpp"
#include "lang/support/digest.hpp"
#include <iostream>
#include <stdexcept>
#include <set>

using namespace lang;
std::vector<std::string> keys(std::string text) {
  SourceManager sources;
  auto source = sources.update(sources.add_file("/identity/test.wz"), std::move(text));
  auto tokens = lex(source);
  if (!tokens.success()) throw std::runtime_error("identity fixture lex failed");
  auto parsed = parse(*tokens.value);
  if (!parsed.success()) throw std::runtime_error("identity fixture parse failed");
  modules::Project project;
  project.backend = "native"; project.word_bits = 64; project.entry = ModuleId{0};
  project.modules.push_back({ModuleId{0}, "app", "source:/identity/猫", "identity",
      {std::make_shared<const AstFile>(std::move(*parsed.value))}, {}, {}});
  auto resolved = modules::resolve_project(std::make_shared<const modules::Project>(project));
  if (!resolved.success()) throw std::runtime_error("identity fixture resolve failed");
  hir::HirModule module;
  auto owned = std::make_shared<modules::ResolvedProject>(*resolved.value);
  module.resolved = owned; module.identity = ModuleId{0}; module.config = {hir::Backend::native, 64};
  std::optional<SymbolId> function, structure;
  for (const auto &symbol : owned->symbols) {
    if (symbol.name == "f") function = symbol.id;
    if (symbol.name == "S") structure = symbol.id;
  }
  if (!function || !structure) throw std::runtime_error("identity fixture symbols missing");
  std::set<std::string> declarations;
  for (const auto &symbol : owned->symbols)
    if (symbol.definition && !declarations.insert(hir::declaration_key(module, symbol.id)).second)
      throw std::runtime_error("duplicate source declaration key");
  const auto base = hir::declaration_key(module, *function);
  hir::Type integer; integer.kind = hir::TypeKind::SignedInt; integer.bits = 32;
  const auto i32 = module.types.intern(integer);
  hir::Type signature; signature.kind = hir::TypeKind::Function;
  signature.parameters = {{hir::ParamMode::move, i32}}; signature.result = i32;
  const auto function_type = module.types.intern(signature);
  hir::Type nominal; nominal.kind = hir::TypeKind::NominalStruct; nominal.declaration = *structure;
  const auto nominal_type = module.types.intern(nominal);
  auto instance = owned->symbols.at(function->value); instance.id = SymbolId{owned->symbols.size()};
  owned->symbols.push_back(instance);
  module.instances.push_back({instance.id, *function, {i32}, {}, module.config});
  return {base, hir::declaration_key(module, instance.id), hir::native_function_name(module, instance.id),
          hir::type_key(module, function_type), hir::type_key(module, nominal_type)};
}
int main(int argc, char **) {
  try {
    const std::string source = "package identity; pub func f[T](x T) T { return x; } pub struct S { pub value i32; }";
    auto original = keys(source);
    if (original != keys("package identity; func other(_ i32, _ i32) i32 { return 12; } " + source.substr(18)))
      throw std::runtime_error("unrelated declaration changes identity");
    if (sha256("") != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
        sha256("abc") != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
      throw std::runtime_error("SHA256 mismatch");
    if (argc > 1) for (const auto &key : original) std::cout << key << '\n';
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
