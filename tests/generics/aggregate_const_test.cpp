#include "../../src/hir/ownership.hpp"
#include "lang/sema/check.hpp"
#ifdef LANG_AGGREGATE_NATIVE
#include "lang/ir/native.hpp"
#endif
#include <filesystem>
#include <functional>
#include <iostream>
using namespace lang;
void need(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
std::string messages(const auto &r) {
  std::string s;
  for (auto &d : r.diagnostics)
    s += d.code + ": " + d.message + "\n";
  return s;
}
bool invalid_result(const auto &r) {
  return !r.success() && !r.value && r.failure == FailureKind::internal &&
         !r.diagnostics.empty() && r.diagnostics[0].code == "E_IR_INVALID";
}
bool alter(hir::ConstantValue &v,
           const std::function<bool(hir::ConstantValue &)> &f) {
  if (f(v))
    return true;
  if (auto a = std::get_if<hir::ConstantAggregate>(&v.data))
    for (auto &child : a->fields)
      if (alter(child, f))
        return true;
  return false;
}
int main(int argc, char **argv) {
  try {
    need(argc == 2, "source root required");
    SourceManager sm;
    auto loaded =
        modules::load_project({std::filesystem::path(argv[1]) /
                                   "tests/sema/aggregate-constants/lang.toml",
                               {},
                               {}},
                              sm);
    need(loaded.success(), messages(loaded));
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(*loaded.value));
    auto checked = sema::check_project(resolved);
    need(checked.success(), messages(checked));
    const auto original = checked.value->module();
    auto rejected = [&](std::string name,
                        const std::function<bool(hir::HirModule &)> &mutate) {
      auto m = original;
      need(mutate(m), "missing mutation: " + name);
      need(invalid_result(hir::verify_types(m)),
           "direct HIR mutation accepted: " + name);
      auto ownership = hir::detail::produce_ownership(m, {});
      need(ownership.success(),
           "ownership recompute failed: " + name + "\n" + messages(ownership));
      need(invalid_result(hir::verify_types(m)),
           "recomputed HIR mutation accepted: " + name);
      need(invalid_result(hir::verify(m)), "invalid HIR published: " + name);
    };
    auto literal_mutation =
        [&](std::string name,
            const std::function<bool(hir::ConstantValue &)> &change) {
          rejected(name, [&](auto &m) {
            for (auto &n : m.nodes)
              if (auto c = std::get_if<hir::ConstantLiteral>(&n.data);
                  c && alter(c->value, change))
                return true;
            return false;
          });
        };
    literal_mutation("canonical integer changed", [](auto &v) {
      if (auto p = std::get_if<hir::IntegerLiteral>(&v.data)) {
        p->decimal = "123";
        p->bits = 123;
        return true;
      }
      return false;
    });
    literal_mutation("valid UTF8 bytes changed", [](auto &v) {
      if (auto p = std::get_if<hir::StringLiteral>(&v.data)) {
        p->utf8 = "other";
        return true;
      }
      return false;
    });
    SymbolId other;
    for (auto &s : original.resolved->symbols)
      if (s.name == "other" && s.kind == modules::SymbolKind::function)
        other = s.id;
    need(other.value, "other function missing");
    literal_mutation("same-signature target changed", [&](auto &v) {
      if (auto p = std::get_if<hir::FunctionValue>(&v.data)) {
        p->symbol = other;
        p->type_args.clear();
        return true;
      }
      return false;
    });
    literal_mutation("same-shape enum variant changed", [&](auto &v) {
      if (auto p = std::get_if<hir::ConstantAggregate>(&v.data);
          p && p->variant) {
        const auto &t = *original.types.find(v.type);
        for (auto &d : original.nominals)
          if (d.symbol == t.declaration && d.variants.size() == 2 &&
              d.variants[0].payload.size() == 1 &&
              d.variants[1].payload.size() == 1) {
            p->variant = d.variants[1].symbol;
            return true;
          }
      }
      return false;
    });
    rejected("local const initializer changed", [](auto &m) {
      for (auto &n : m.nodes)
        if (auto b = std::get_if<hir::Binding>(&n.data);
            b && m.resolved->symbols.at(b->symbol.value).kind ==
                     modules::SymbolKind::constant) {
          auto &init = m.nodes.at(n.children.at(0).value);
          if (auto p = std::get_if<hir::StructInit>(&init.data))
            for (auto &arg : p->fields)
              if (auto i = std::get_if<hir::IntegerLiteral>(
                      &m.nodes.at(arg.expression.value).data)) {
                i->decimal = "120";
                i->bits = 120;
                return true;
              }
        }
      return false;
    });
    rejected("exported constant leaks private aggregate", [](auto &m) {
      auto resolved = std::make_shared<modules::ResolvedProject>(*m.resolved);
      m.resolved = resolved;
      for (auto &s : resolved->symbols)
        if (s.name == "Origin" && s.kind == modules::SymbolKind::constant) {
          s.exported = true;
          return true;
        }
      return false;
    });
    rejected("same lambda from another enclosing instance", [](auto &m) {
      for (auto &n : m.nodes)
        if (auto value = std::get_if<hir::FunctionValue>(&n.data)) {
          for (const auto &target : m.instances)
            if (target.symbol == value->symbol)
              for (const auto &other : m.instances)
                if (other.origin == target.origin &&
                    other.type_args != target.type_args)
                  for (const auto &function : m.functions)
                    if (function.symbol == other.symbol &&
                        function.signature == n.type) {
                      value->symbol = other.symbol;
                      return true;
                    }
        }
      return false;
    });
#ifdef LANG_AGGREGATE_NATIVE
    auto lowered = ir::lower(*checked.value);
    need(lowered.success(), messages(lowered));
    auto corrupt_ir =
        [&](std::string name,
            const std::function<void(ir::Instruction &)> &change) {
          auto m = lowered.value->module();
          bool found = false;
          for (auto &f : m.functions)
            for (auto &b : f.blocks)
              for (auto &i : b.instructions)
                if (!found && i.result && i.origin &&
                    original.nodes.at(i.origin->value).kind() ==
                        hir::NodeKind::constant_literal) {
                  change(i);
                  found = true;
                }
          need(found, "constant IR root missing");
          auto result = ir::verify(m);
          need(invalid_result(result),
               "IR mutation accepted: " + name + "\n" + messages(result));
        };
    corrupt_ir("origin erased", [](auto &i) { i.origin.reset(); });
    corrupt_ir("source changed", [](auto &i) { ++i.source.begin_byte; });
    corrupt_ir("root value changed", [](auto &i) {
      if (i.inputs.size() > 1)
        std::swap(i.inputs[0], i.inputs[1]);
      else
        i.bits ^= 1;
    });
    auto corrupt_cleanup = [&](std::string name, const auto &change) {
      auto m = lowered.value->module();
      bool found = false;
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          for (auto &i : b.instructions)
            if (!found && i.op == ir::Op::ownership_begin && i.origin &&
                original.nodes.at(i.origin->value).kind() ==
                    hir::NodeKind::constant_literal &&
                original.types.find(original.nodes.at(i.origin->value).type)->kind !=
                    hir::TypeKind::Unit) {
              change(i);
              found = true;
            }
      need(found, "constant full-expression cleanup marker missing");
      const auto result = ir::verify(m);
      need(invalid_result(result),
           "constant cleanup mutation accepted: " + name + "\n" + messages(result));
    };
    corrupt_cleanup("wrong value type", [&](auto &i) {
      i.type = original.nodes.at(i.origin->value).type;
    });
    corrupt_cleanup("source changed", [](auto &i) { ++i.source.begin_byte; });
    corrupt_cleanup("call cleanup on a constant", [](auto &i) { i.bits = 1; });
#endif
    std::cout << "aggregate constants: 7 HIR mutations rejected directly and "
                 "after ownership recompute";
#ifdef LANG_AGGREGATE_NATIVE
    std::cout << "; 6 NativeIR mutations rejected\n";
#else
    std::cout << "; NativeIR not_run (LLVM disabled)\n";
#endif
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
