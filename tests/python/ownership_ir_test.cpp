#include "lang/backend/python.hpp"
#include "lang/sema/check.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace lang;
using namespace lang::hir;
namespace {
void need(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
HirModule input() {
  SourceManager sm;
  auto snapshot = sm.update(sm.add_file("ownership-ir.lang"), R"(
package app;
enum E {Bad(v Vec[i32]);}
func fail(x Vec[i32]) Vec[i32] throws E {throw E.Bad(v: x);}
func keep(x Vec[i32]) Vec[i32] {return x;}
func main() i32 {let a Vec[i32] = [1, 2]; defer {let z = a[0];}
try {let b = fail(a);} catch e E {} let c = keep(a); return c[0] - 1;}
)");
  auto tokens = lex(snapshot);
  need(tokens.success(), "lex");
  auto ast = parse(*tokens.value);
  need(ast.success(), "parse");
  auto project = std::make_shared<modules::Project>();
  project->backend = "python";
  project->word_bits = 64;
  modules::Module module;
  module.id = ModuleId{0};
  module.logical_path = "app";
  module.files.push_back(
      std::make_shared<const AstFile>(std::move(*ast.value)));
  project->modules.push_back(std::move(module));
  auto resolved = modules::resolve_project(project);
  need(resolved.success(), "resolve");
  auto checked = sema::check_types(resolved);
  need(checked.success(), "types");
  return std::move(*checked.value);
}
void bad(const HirModule &original,
         const std::function<void(HirModule &)> &edit) {
  auto m = original;
  edit(m);
  auto types = verify_types(m);
  auto published = verify(m);
  for (const auto *result : {&types}) {
    need(!result->success() && !result->value, "damaged types accepted");
    need(!result->diagnostics.empty() &&
             result->diagnostics[0].code == "E_IR_INVALID",
         "wrong types diagnostic");
  }
  need(!published.success() && !published.value, "damaged HIR published");
  need(!published.diagnostics.empty() &&
           published.diagnostics[0].code == "E_IR_INVALID",
       "wrong publication diagnostic");
}
SymbolId entry(const HirModule &m) {
  for (const auto &f : m.functions)
    if (m.resolved->symbols[f.symbol.value].name == "main")
      return f.symbol;
  throw std::runtime_error("no main");
}
} // namespace
int main() {
  try {
    auto m = input();
    bad(m, [](auto &v) { v.ownership.temporaries.clear(); });
    bad(m, [](auto &v) { v.ownership.uses.clear(); });
    bad(m, [](auto &v) { v.ownership.locals.clear(); });
    bad(m, [](auto &v) { v.ownership.full_expressions.clear(); });
    bad(m, [](auto &v) { v.ownership.calls.clear(); });
    bad(m, [](auto &v) {
      for (auto &n : v.nodes)
        for (auto &e : n.exits)
          e.actions.clear();
    });
    bad(m, [](auto &v) {
      for (auto &n : v.nodes)
        if (n.kind() == NodeKind::vec_init) {
          n.children.clear();
          return;
        }
    });
    bad(m, [](auto &v) {
      for (auto &n : v.nodes)
        for (auto &e : n.exits)
          for (auto &a : e.actions)
            if (a.kind == CleanupKind::destroy_if_live) {
              a.owner.identity += 999999;
              return;
            }
    });
    auto verified = verify(m);
    if (verified.success()) {
      auto before = backend::python::emit(*verified.value, entry(m));
      need(before.success(), "emit accepted own HIR");
      m.ownership = {};
      m.nodes.clear();
      m.functions.clear();
      auto after = backend::python::emit(*verified.value,
                                         entry(verified.value->module()));
      need(after.success() && before.value->code == after.value->code,
           "builder alias mutated frozen HIR");
      std::cout << "8 damaged HIR cases rejected by types and publication; "
                   "owning freeze/emit passed\n";
    } else {
      need(verified.failure == FailureKind::unsupported,
           "unexpected valid HIR failure");
      std::cout << "8 damaged HIR cases rejected by types and publication; "
                   "owning freeze/emit not_run (shared support check)\n";
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
