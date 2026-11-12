#include "lang/modules/resolve.hpp"
#include <iostream>
#include <stdexcept>
using namespace lang;
using namespace lang::modules;
namespace {
void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
std::shared_ptr<Project> project(SourceManager &sources,
    const std::vector<std::vector<std::string>> &modules) {
  auto result = std::make_shared<Project>();
  result->entry = ModuleId(0);
  for (size_t m = 0; m < modules.size(); ++m) {
    Module module{ModuleId(m), "module" + std::to_string(m)};
    for (size_t f = 0; f < modules[m].size(); ++f) {
      auto source = sources.update(sources.add_file(std::to_string(m) + "/" +
          std::to_string(f) + ".wz"), modules[m][f]);
      auto tokens = lex(source);
      check(tokens.success(), "test lex failed");
      auto parsed = parse(*tokens.value);
      check(parsed.success(), "test parse failed");
      module.files.push_back(std::make_shared<const AstFile>(std::move(*parsed.value)));
    }
    result->modules.push_back(std::move(module));
  }
  return result;
}
bool has(const PhaseResult<ResolvedProject> &r, const char *code) {
  for (const auto &d : r.diagnostics) if (d.code == code) return true;
  return false;
}
void example(const char *source, const char *error = nullptr) {
  SourceManager manager;
  auto p = project(manager, {{source}});
  auto r = resolve_project(p);
  if (error) check(!r.success() && has(r, error), error);
  else {
    for (const auto &d : r.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    check(r.success(), "valid source rejected");
  }
  check(r.value && r.value->project == p, "project lifetime lost");
  for (const auto &d : r.diagnostics) if (d.primary)
    check(manager.get(d.primary->snapshot)->path == "0/0.wz", "wrong diagnostic file");
}
AstNodeId top_declaration(const AstFile &file, AstKind kind,
                          std::string_view name = {}) {
  for (auto id : file.node(file.root).children) {
    const auto &node = file.node(id);
    if (node.kind != kind) continue;
    size_t p = node.token_begin + (file.tokens.at(node.token_begin).spelling == "pub");
    if (name.empty() || file.tokens.at(p + 1).spelling == name) return id;
  }
  throw std::runtime_error("missing test declaration");
}
SymbolId self_of(const ResolvedProject &resolved, AstNodeId owner) {
  for (auto [ref, symbol] : resolved.self_parameters)
    if (ref.module == ModuleId(0) && ref.file == 0 && ref.node == owner)
      return symbol;
  throw std::runtime_error("missing test Self");
}
void self_occurrences(const ResolvedProject &resolved, const AstFile &file,
                      AstNodeId owner, size_t expected) {
  auto self = self_of(resolved, owner);
  const auto &declaration = file.node(owner);
  size_t found = 0;
  for (const auto &occurrence : resolved.occurrences) {
    if (occurrence.span.snapshot != file.source->id ||
        occurrence.span.begin_byte < declaration.span.begin_byte ||
        occurrence.span.end_byte > declaration.span.end_byte)
      continue;
    if (resolved.symbols.at(occurrence.symbol.value).name != "Self") continue;
    check(occurrence.symbol == self, "Self occurrence bound to another declaration");
    ++found;
  }
  check(found == expected, "missing or duplicate declaration Self occurrence");
}
void self_declarations() {
  SourceManager manager;
  auto p = project(manager, {{"package p; trait Rel[A] {} "
      "trait Edit[A Rel[Self]] { func f(x Self); } "
      "trait View[B Rel[Self]] {} struct Host {} "
      "impl[P Rel[Self]] Rel[P] for Host { func g(x Self) {} }"}});
  auto r = resolve_project(p);
  for (const auto &d : r.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
  check(r.success(), "Self in declaration header bounds rejected");
  const auto &file = *p->modules.at(0).files.at(0);
  auto edit = top_declaration(file, AstKind::Trait, "Edit");
  auto view = top_declaration(file, AstKind::Trait, "View");
  auto impl = top_declaration(file, AstKind::Impl);
  auto edit_self = self_of(*r.value, edit);
  auto view_self = self_of(*r.value, view);
  auto impl_self = self_of(*r.value, impl);
  check(edit_self != view_self && edit_self != impl_self &&
        view_self != impl_self, "different declarations share one Self");
  self_occurrences(*r.value, file, edit, 2);
  self_occurrences(*r.value, file, view, 1);
  self_occurrences(*r.value, file, impl, 2);
}
}
int main() {
  self_declarations();
  example("package p; func f(x i32) i32 { { let x = 2; } return x; }");
  example("package p; func f() { let x = 1; { let x = x + 1; } }");
  example("package p; func f() { let x = x + 1; }", "E_NAME_NOT_FOUND");
  example("package p; func f(x i32) { let x = 2; }", "E_NAME_DUPLICATE");
  example("package p; func f() { let x = missing; }", "E_NAME_NOT_FOUND");
  example("package p; struct T {} func f() { let T = 2; let x T = 3; }", "E_NAME_KIND");
  example("package p; func f() { for let i = 0; i < 2; i += 1 {} let x = i; }", "E_NAME_NOT_FOUND");
  example("package p; func f() { for x in [1, 2] { let y = x; } let z = x; }", "E_NAME_NOT_FOUND");
  example("package p; struct E {} func f() { try {} catch e E { let x = e; } let x = e; }", "E_NAME_NOT_FOUND");
  example("package p; func f() { switch 1 { case x: let y = x; default: let z = x; } }", "E_NAME_NOT_FOUND");
  example("package p; func f() { let _ = 1; let x = _; }", "E_NAME_NOT_FOUND");
  example("package p; const _ = 1; func f() { const _ = 2; }");
  example("package p; const _ = missing;", "E_NAME_NOT_FOUND");
  example("package p; func f() { let x Self = 0; }", "E_NAME_NOT_FOUND");
  example("package p; func f(x Self) {}", "E_NAME_NOT_FOUND");
  example("package p; struct T { x Self; }", "E_NAME_NOT_FOUND");
  example("package p; enum E { V(x Self); }", "E_NAME_NOT_FOUND");
  example("package p; const X Self = 1;", "E_NAME_NOT_FOUND");
  example("package p; trait A { func f(x Self); }");
  example("package p; struct T {} impl T { func f(x Self) {} }");
  example("package p; trait A[T, T] {}", "E_NAME_DUPLICATE");
  example("package p; trait A[Self] {}", "E_NAME_DUPLICATE");
  example("package p; struct T {} pub func f(x T) {}", "E_PRIVATE_TYPE_LEAK");
  example("package p; struct Secret {} pub enum Reply { Value(value Secret); }", "E_PRIVATE_TYPE_LEAK");
  example("package p; func f() { let a = [1]; let i = 0; let x = a[(i)]; }");
  example("package p; const A = B; const B = A;", "E_CONSTANT_CYCLE");
  example("package p; const A = A;", "E_CONSTANT_CYCLE");
  example("package p; trait Hidden {} pub func f[T Hidden](x T) {}", "E_PRIVATE_TYPE_LEAK");
  example("package p; const f = func() { f(); };");
  example("package p; var x = 1;", "E_CAPABILITY_UNAVAILABLE");
  // Capturing a lexical local is resolved as a real cross-function value
  // occurrence; the closure capture itself is checked by Sema/HIR.
  example("package p; func f() { let x = 1; let g = func() i32 { return x; }; }");
  example("package p; func f() { let g = func(x i32) i32 { return x; }; }");
  SourceManager manager;
  auto p = project(manager, {{"package p; func a() { b(); }", "package p; func b() { a(); }"}});
  auto r = resolve_project(p);
  check(r.success() && !r.value->occurrences.empty(), "cross-file recursion failed");
  p = project(manager, {{"package p; import \"dep\" as d; func f() { d.hidden(); }"},
                        {"package q; func hidden() {} pub func visible() {}"}});
  const auto &a = *p->modules[0].files[0];
  auto import = a.node(a.root).children[1];
  p->modules[0].imports.push_back({0, import, ModuleId(1), "d", a.node(import).span});
  r = resolve_project(p);
  check(has(r, "E_PRIVATE_ACCESS"), "private module member allowed");
  p = project(manager, {{"package p; import \"dep\" as d; func f() { (d).hidden(); }"},
                        {"package q; func hidden() {}"}});
  const auto &parenthesized = *p->modules[0].files[0];
  import = parenthesized.node(parenthesized.root).children[1];
  p->modules[0].imports.push_back({0, import, ModuleId(1), "d", parenthesized.node(import).span});
  check(has(resolve_project(p), "E_PRIVATE_ACCESS"), "parentheses bypassed visibility");
  p = project(manager, {{"package p; import \"dep\" as d; func f() { d.visible(); }",
                        "package p; func g() { d.visible(); }"},
                        {"package q; pub func visible() {}"}});
  const auto &b = *p->modules[0].files[0];
  import = b.node(b.root).children[1];
  p->modules[0].imports.push_back({0, import, ModuleId(1), "d", b.node(import).span});
  r = resolve_project(p);
  check(r.diagnostics.size() == 1 && has(r, "E_NAME_NOT_FOUND"), "file alias leaked");
  check(manager.get(r.diagnostics[0].primary->snapshot)->path == "0/1.wz", "alias diagnostic wrong file");
  p = project(manager, {{"package p; func f() { let a = func(x i32) i32 { return x; }; let b = func() { let c = func(y i32) i32 { return y; }; }; }"}});
  r = resolve_project(p);
  check(r.success(), "nested noncapturing literals rejected");
  size_t literals = 0;
  for (const auto &symbol : r.value->symbols) {
    if (symbol.kind != SymbolKind::function || !symbol.name.empty()) continue;
    ++literals;
    check(symbol.id.value < r.value->symbols.size(), "invalid identity");
    check(symbol.definition.has_value() && symbol.declaration.has_value(), "missing literal source");
    const auto &ref = *symbol.definition;
    const auto &literal = p->modules.at(ref.module.value).files.at(ref.file)->node(ref.node);
    check(literal.kind == AstKind::FunctionLiteral && symbol.module == ref.module && !symbol.exported,
          "incorrect literal identity");
    check(symbol.declaration->begin_byte == literal.span.begin_byte &&
          symbol.declaration->end_byte == literal.span.end_byte &&
          symbol.declaration->snapshot == literal.span.snapshot, "incorrect literal span");
    for (const auto &occurrence : r.value->occurrences)
      check(occurrence.symbol != symbol.id, "literal treated as name reference");
  }
  check(literals == 3, "missing or duplicate literal identities");
  example("package p; func f() { let a = func(x i32) { let b = func() i32 { return x; }; }; }");
  std::stop_source stop;
  stop.request_stop();
  r = resolve_project(p, stop.get_token());
  check(r.state == PhaseState::cancelled && r.exit_code() == ExitCode::cancelled, "cancel failed");
  check(resolve_project({}).exit_code() == ExitCode::internal_error, "null project classified as source");
  std::string chain = "package p; ";
  for (size_t i = 0; i < 3000; ++i)
    chain += "const C" + std::to_string(i) + " = C" + std::to_string(i + 1) + "; ";
  chain += "const C3000 = 1;";
  p = project(manager, {{chain}});
  check(resolve_project(p).success(), "long constant graph failed");
  std::cout << "passed: lexical scopes, kinds, exports, recursion, cycles, ownership and cancel\n";
}
