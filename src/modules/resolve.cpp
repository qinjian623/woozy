#include "lang/modules/resolve.hpp"
#include "lang/hir/system_operation.hpp"
#include <algorithm>
#include <map>

namespace lang::modules {
namespace {
struct Scope {
  Scope *parent = nullptr;
  std::map<std::string, SymbolId> names;
};
bool is_type(SymbolKind k) {
  return k == SymbolKind::type || k == SymbolKind::trait ||
         k == SymbolKind::type_parameter || k == SymbolKind::builtin_type;
}
class Resolver {
  ResolvedProject out;
  std::vector<Diagnostic> diagnostics;
  std::stop_token stop;
  Scope prelude;
  std::vector<Scope> packages;
  std::map<std::pair<uint64_t, uint64_t>, SymbolId> declarations;
  std::vector<std::optional<size_t>> owners;
  std::map<uint64_t, std::vector<std::pair<SymbolId, SourceSpan>>> const_edges;
  std::optional<SymbolId> active_const;
  std::optional<size_t> function;
  size_t next_function = 0;
  ModuleId module;
  size_t file = 0;
  const AstFile *ast = nullptr;

  NodeRef ref(AstNodeId id) const { return {module, file, id}; }
  const AstNode &node(AstNodeId id) const { return ast->node(id); }
  const Token &token(size_t p) const { return ast->tokens.at(p); }
  size_t start(const AstNode &n) const {
    return n.token_begin + (token(n.token_begin).spelling == "pub");
  }
  void error(std::string code, std::string message, SourceSpan span,
             std::optional<SourceSpan> previous = {}) {
    Diagnostic d{std::move(code), "resolve", "error", std::move(message), span};
    if (previous) d.related.push_back({*previous, "previous declaration"});
    diagnostics.push_back(std::move(d));
  }
  SymbolId add(Scope &scope, std::string name, SymbolKind kind,
               std::optional<NodeRef> definition = {},
               std::optional<SourceSpan> span = {}, bool exported = false,
               std::optional<ModuleId> target = {}, bool local = false) {
    check_cancelled(stop);
    if (name == "_" && kind != SymbolKind::value && kind != SymbolKind::constant && span)
      error("E_NAME_KIND", "named declaration cannot be _", *span);
    SymbolId id(out.symbols.size());
    if (name != "_") {
      auto [it, inserted] = scope.names.emplace(name, id);
      if (!inserted) {
        if (span)
          error("E_NAME_DUPLICATE", "duplicate declaration: " + name,
                *span, out.symbols[it->second.value].declaration);
        return it->second;
      }
    }
    out.symbols.push_back({id, std::move(name), kind, module, definition, span,
                           exported, target});
    owners.push_back(local ? function : std::nullopt);
    return id;
  }
  std::optional<SymbolId> lookup(Scope &scope, std::string_view name) const {
    for (auto s = &scope; s; s = s->parent) {
      auto it = s->names.find(std::string(name));
      if (it != s->names.end()) return it->second;
    }
    return {};
  }
  void record(AstNodeId id, SymbolId symbol, SourceSpan span) {
    out.occurrences.push_back({ref(id), symbol, span});
    if (active_const && out.symbols[symbol.value].kind == SymbolKind::constant)
      const_edges[active_const->value].push_back({symbol, node(id).span});
  }
  std::optional<SymbolId> name(AstNodeId id, Scope &scope,
                              std::string_view spelling, SourceSpan span,
                              bool require_type = false, bool public_type = false) {
    auto found = lookup(scope, spelling);
    if (!found || spelling == "_") {
      error("E_NAME_NOT_FOUND", "unknown name: " + std::string(spelling), span);
      return {};
    }
    const auto &s = out.symbols[found->value];
    if (require_type && !is_type(s.kind)) {
      error("E_NAME_KIND", "name is not a type: " + std::string(spelling), span,
            s.declaration);
      return {};
    }
    if (public_type && s.module == module && s.definition &&
        (s.kind == SymbolKind::type || s.kind == SymbolKind::trait) && !s.exported)
      error("E_PRIVATE_TYPE_LEAK", "public interface uses private type: " + s.name,
            span, s.declaration);
    record(id, *found, span);
    return found;
  }
  std::optional<SymbolId> member(AstNodeId id, SymbolId base,
                                std::string_view spelling, SourceSpan span,
                                bool require_type = false) {
    const auto target = out.symbols[base.value].target;
    if (!target) {
      if (require_type && !is_type(out.symbols[base.value].kind)) {
        error("E_NAME_KIND", "type qualifier is not a module or a type", span,
              out.symbols[base.value].declaration);
        return {};
      }
      out.deferred_members.push_back(ref(id));
      return {};
    }
    auto it = packages[target->value].names.find(std::string(spelling));
    if (it == packages[target->value].names.end()) {
      error("E_NAME_NOT_FOUND", "module has no declaration: " + std::string(spelling), span);
      return {};
    }
    const auto &s = out.symbols[it->second.value];
    if (*target != module && !s.exported) {
      error("E_PRIVATE_ACCESS", "declaration is private: " + s.name, span, s.declaration);
      return {};
    }
    if (require_type && !is_type(s.kind)) {
      error("E_NAME_KIND", "module declaration is not a type: " + s.name, span,
            s.declaration);
      return {};
    }
    record(id, it->second, span);
    return it->second;
  }
  std::optional<SymbolId> expression(AstNodeId id, Scope &scope) {
    const auto &n = node(id);
    if (n.kind == AstKind::Name)
      return name(id, scope, token(n.token_begin).spelling, n.span);
    if (n.kind == AstKind::Member) {
      auto base = expression(n.children.at(0), scope);
      const auto &t = token(n.token_end - 1);
      if (base) return member(id, *base, t.spelling, t.span);
      out.deferred_members.push_back(ref(id));
      return {};
    }
    if (n.kind == AstKind::Parenthesized)
      return expression(n.children.at(0), scope);
    if (n.kind == AstKind::FunctionLiteral) {
      // Anonymous identities never enter a lexical names table.
      SymbolId identity(out.symbols.size());
      out.symbols.push_back({identity, {}, SymbolKind::function, module,
                             ref(id), n.span, false, {}});
      owners.push_back(std::nullopt);
      function_body(id, scope);
      return {};
    }
    for (auto child : n.children) visit(child, scope);
    return {};
  }
  void type(AstNodeId id, Scope &scope, bool public_type = false,
            bool require_type = true) {
    check_cancelled(stop);
    const auto &n = node(id);
    if (n.kind != AstKind::NamedType) {
      for (auto child : n.children)
        type(child, scope, public_type, require_type || n.kind == AstKind::FunctionType);
      return;
    }
    const auto &first = token(n.token_begin);
    // A qualified type starts with a module or a type; only the final name
    // must be a type. Generic argument nodes are checked separately.
    auto current = name(id, scope, first.spelling, first.span, false, public_type);
    size_t depth = 0;
    bool qualified = false;
    for (size_t p = n.token_begin + 1; p < n.token_end; ++p) {
      auto text = token(p).spelling;
      if (text == "[") ++depth;
      else if (text == "]") --depth;
      else if (text == "." && depth == 0) {
        qualified = true;
        const auto &t = token(++p);
        if (current) current = member(id, *current, t.spelling, t.span, require_type);
      }
    }
    if (require_type && current && !qualified && !is_type(out.symbols[current->value].kind))
      error("E_NAME_KIND", "name is not a type: " + std::string(first.spelling),
            first.span, out.symbols[current->value].declaration);
    for (auto child : n.children) type(child, scope, public_type, require_type);
  }
  void generics(const AstNode &n, Scope &scope, bool exported = false) {
    for (auto child : n.children) {
      const auto &c = node(child);
      if (c.kind != AstKind::GenericParameter) continue;
      const auto &t = token(c.token_begin);
      add(scope, std::string(t.spelling), SymbolKind::type_parameter, ref(child), t.span);
    }
    for (auto child : n.children)
      if (node(child).kind == AstKind::GenericParameter)
        for (auto constraint : node(child).children) type(constraint, scope, exported);
  }
  void function_body(AstNodeId id, Scope &outer, bool exported = false) {
    const auto &n = node(id);
    auto previous_function = function;
    auto previous_const = active_const;
    active_const.reset();
    function = ++next_function;
    Scope scope{&outer, {}};
    generics(n, scope, exported);
    for (auto child : n.children) {
      const auto &c = node(child);
      if (c.kind == AstKind::Parameter) {
        for (auto t : c.children) type(t, scope, exported);
      } else if (c.kind == AstKind::NamedType || c.kind == AstKind::FunctionType ||
                 c.kind == AstKind::ParenthesizedType) type(child, scope, exported);
    }
    for (auto child : n.children) {
      const auto &c = node(child);
      if (c.kind != AstKind::Parameter) continue;
      const auto &t = token(c.token_begin);
      add(scope, std::string(t.spelling), SymbolKind::value, ref(child), t.span,
          false, {}, true);
    }
    for (auto child : n.children)
      if (node(child).kind == AstKind::Block)
        for (auto statement : node(child).children) visit(statement, scope);
    function = previous_function;
    active_const = previous_const;
  }
  void pattern(AstNodeId id, Scope &scope) {
    const auto &n = node(id);
    if (n.kind == AstKind::BindPattern) {
      const auto &t = token(n.token_begin);
      add(scope, std::string(t.spelling), SymbolKind::value, ref(id), t.span,
          false, {}, true);
    } else if (n.kind == AstKind::VariantPattern) {
      const auto &t = token(n.token_begin);
      // The last top-level dot names the variant; preceding dots qualify
      // its enum. Parentheses inside type arguments are not payloads.
      std::vector<size_t> dots;
      size_t depth = 0;
      for (size_t p = n.token_begin; p < n.token_end; ++p) {
        auto text = token(p).spelling;
        if (text == "(" && depth == 0) break;
        if (text == "[") ++depth;
        else if (text == "]") --depth;
        else if (text == "." && depth == 0) dots.push_back(p);
      }
      auto current = name(id, scope, t.spelling, t.span);
      for (size_t i = 0; i + 1 < dots.size(); ++i) {
        const auto &part = token(dots[i] + 1);
        if (current)
          current = member(id, *current, part.spelling, part.span,
                           i + 2 == dots.size());
      }
      if (current && !is_type(out.symbols[current->value].kind))
        error("E_NAME_KIND", "pattern qualifier is not a type", t.span);
      out.deferred_members.push_back(ref(id));
      for (auto child : n.children) {
        if (node(child).kind == AstKind::NamedType ||
            node(child).kind == AstKind::ParenthesizedType ||
            node(child).kind == AstKind::FunctionType) type(child, scope);
        else pattern(child, scope);
      }
    }
  }
  void visit(AstNodeId id, Scope &scope) {
    check_cancelled(stop);
    const auto &n = node(id);
    switch (n.kind) {
    case AstKind::NamedType: case AstKind::ParenthesizedType: case AstKind::FunctionType:
      type(id, scope); return;
    case AstKind::Name: case AstKind::Member: case AstKind::FunctionLiteral:
      expression(id, scope); return;
    case AstKind::BracketItem:
      for (auto child : n.children) {
        if (node(child).kind == AstKind::ParenthesizedType)
          type(child, scope, false, false);
        else visit(child, scope);
      }
      return;
    case AstKind::Block: case AstKind::For: case AstKind::Case: {
      Scope child{&scope, {}};
      for (auto c : n.children) {
        if (node(c).kind == AstKind::BindPattern || node(c).kind == AstKind::VariantPattern ||
            node(c).kind == AstKind::LiteralPattern) pattern(c, child);
        else visit(c, child);
      }
      return;
    }
    case AstKind::ForIn: {
      visit(n.children.at(0), scope);
      Scope child{&scope, {}};
      const auto &t = token(n.token_begin + 1);
      add(child, std::string(t.spelling), SymbolKind::value, ref(id), t.span,
          false, {}, true);
      for (auto c : node(n.children.back()).children) visit(c, child);
      return;
    }
    case AstKind::Catch: {
      type(n.children.at(0), scope);
      Scope child{&scope, {}};
      const auto &t = token(n.token_begin + 1);
      add(child, std::string(t.spelling), SymbolKind::value, ref(id), t.span,
          false, {}, true);
      for (auto c : node(n.children.back()).children) visit(c, child);
      return;
    }
    case AstKind::Binding: {
      size_t p = start(n);
      const auto &t = token(p + 1);
      for (size_t i = 0; i + 1 < n.children.size(); ++i) visit(n.children[i], scope);
      visit(n.children.back(), scope);
      add(scope, std::string(t.spelling), token(p).spelling == "const" ?
            SymbolKind::constant : SymbolKind::value, ref(id), t.span,
          false, {}, true);
      return;
    }
    default:
      for (auto c : n.children) visit(c, scope);
    }
  }
  void collect() {
    packages.resize(out.project->modules.size());
    for (const auto &m : out.project->modules) {
      check_cancelled(stop);
      module = m.id;
      auto &scope = packages.at(module.value);
      scope.parent = &prelude;
      for (size_t i = 0; i < m.foreign_exports.size(); ++i) {
        const auto &e = m.foreign_exports[i];
        auto source = e.c_type
            ? out.project->c_type(*e.c_type).source
            : e.c_callable
                  ? out.project->c_callable(*e.c_callable).source
                  : out.project->foreign_binding(
                        e.declaration ? *e.declaration : e.candidates.at(0)).source;
        auto id = add(scope, e.name, e.kind == ForeignExportKind::type ? SymbolKind::type :
             e.kind == ForeignExportKind::function ? SymbolKind::function : SymbolKind::value,
            {}, source, true);
        out.symbols.at(id.value).foreign = ForeignExportRef{m.id, i};
      }
      for (file = 0; file < m.files.size(); ++file) {
        ast = m.files[file].get();
        for (auto id : node(ast->root).children) {
          const auto &n = node(id);
          if (n.kind == AstKind::Package || n.kind == AstKind::Import ||
              n.kind == AstKind::Impl) continue;
          size_t p = start(n);
          if (n.kind == AstKind::Binding && token(p).spelling != "const") {
            error("E_CAPABILITY_UNAVAILABLE", "top-level let/var is unavailable", n.span);
            continue;
          }
          SymbolKind kind;
          switch (n.kind) {
          case AstKind::Function: kind = SymbolKind::function; break;
          case AstKind::Struct: case AstKind::Enum: kind = SymbolKind::type; break;
          case AstKind::Trait: kind = SymbolKind::trait; break;
          case AstKind::Binding: kind = SymbolKind::constant; break;
          default: continue;
          }
          const auto &t = token(p + 1);
          if (t.spelling == "_" && n.kind != AstKind::Binding) {
            error("E_NAME_KIND", "named top-level declaration cannot be _", t.span);
            continue;
          }
          auto symbol = add(scope, std::string(t.spelling), kind, ref(id), t.span,
                            p != n.token_begin);
          declarations.emplace(std::pair(ast->source->id.value, id.value), symbol);
        }
      }
    }
  }
  void declaration(AstNodeId id, Scope &scope) {
    const auto &n = node(id);
    if (n.kind == AstKind::Package || n.kind == AstKind::Import) return;
    bool exported = token(n.token_begin).spelling == "pub";
    if (n.kind == AstKind::Function) { function_body(id, scope, exported); return; }
    if (n.kind == AstKind::Binding) {
      auto it = declarations.find({ast->source->id.value, id.value});
      if (it == declarations.end()) return;
      active_const = it->second;
      for (auto c : n.children) {
        if (node(c).kind == AstKind::NamedType || node(c).kind == AstKind::FunctionType ||
            node(c).kind == AstKind::ParenthesizedType) type(c, scope, exported);
        else visit(c, scope);
      }
      active_const.reset();
      return;
    }
    Scope inner{&scope, {}};
    // A trait/impl's real Self is visible in its own generic parameter
    // bounds, but nowhere else outside that declaration.
    if (n.kind == AstKind::Trait || n.kind == AstKind::Impl) {
      auto self = add(inner, "Self", SymbolKind::type_parameter);
      out.self_parameters.push_back({ref(id), self});
    }
    generics(n, inner, exported);
    Scope members;
    for (auto child : n.children) {
      const auto &c = node(child);
      if (c.kind == AstKind::GenericParameter) continue;
      if (c.kind == AstKind::Function) {
        const auto &t = token(start(c) + 1);
        add(members, std::string(t.spelling), SymbolKind::function, ref(child),
            t.span, token(c.token_begin).spelling == "pub");
        function_body(child, inner, exported || token(c.token_begin).spelling == "pub");
      } else if (c.kind == AstKind::Field || c.kind == AstKind::Variant) {
        const auto &t = token(start(c));
        add(members, std::string(t.spelling), c.kind == AstKind::Field ?
              SymbolKind::field : SymbolKind::variant, ref(child), t.span);
        if (c.kind == AstKind::Variant) {
          Scope fields;
          for (auto field_id : c.children) {
            const auto &f = node(field_id);
            const auto &ft = token(start(f));
            add(fields, std::string(ft.spelling), SymbolKind::field, ref(field_id), ft.span);
            for (auto t_id : f.children) type(t_id, inner, exported);
          }
        } else for (auto t_id : c.children) type(t_id, inner, exported &&
                      token(c.token_begin).spelling == "pub");
      } else type(child, inner);
    }
  }
  void constants() {
    std::vector<unsigned> color(out.symbols.size());
    std::vector<SymbolId> stack;
    std::vector<size_t> cursors;
    for (const auto &[key, _] : const_edges) {
      if (color[key]) continue;
      stack.push_back(SymbolId(key));
      cursors.push_back(0);
      color[key] = 1;
      while (!stack.empty()) {
        check_cancelled(stop);
        auto id = stack.back();
        auto &edges = const_edges[id.value];
        if (cursors.back() == edges.size()) {
          color[id.value] = 2;
          stack.pop_back();
          cursors.pop_back();
          continue;
        }
        auto [next, span] = edges[cursors.back()++];
        if (!color[next.value]) {
          color[next.value] = 1;
          stack.push_back(next);
          cursors.push_back(0);
          continue;
        }
        if (color[next.value] != 1) continue;
        auto begin = std::find(stack.begin(), stack.end(), next);
        std::string path;
        for (auto it = begin; it != stack.end(); ++it)
          path += out.symbols[it->value].name + " -> ";
        path += out.symbols[next.value].name;
        error("E_CONSTANT_CYCLE", "constant cycle: " + path, span);
        for (auto it = begin; it != stack.end(); ++it)
          if (out.symbols[it->value].declaration)
            diagnostics.back().related.push_back({*out.symbols[it->value].declaration,
                                                   out.symbols[it->value].name});
      }
    }
  }
public:
  Resolver(std::shared_ptr<const Project> project, std::stop_token token)
      : stop(token) { out.project = std::move(project); }
  PhaseResult<ResolvedProject> run() {
    PhaseResult<ResolvedProject> result;
    result.failure = FailureKind::source;
    try {
      check_cancelled(stop);
      if (!out.project) {
        result.failure = FailureKind::internal;
        result.diagnostics.push_back({"E_INTERNAL_ERROR", "resolve", "error", "null project"});
        return result;
      }
      for (const auto *s : {"Unit", "bool", "i8", "i16", "i32", "i64", "u8", "u16",
                           "u32", "u64", "isize", "usize", "f32", "f64", "string", "Vec", "Slice", "Weak", "Result", "Ptr", "PtrMut",
                           "Buffer", "BufferMut",
                           "__woozy_Thread", "__woozy_Mutex", "__woozy_Condvar",
                           "__woozy_AtomicI64", "__woozy_IpcHandle"})
        add(prelude, s, SymbolKind::builtin_type);
      for (const auto *s : {"assert", "panic", "share"}) add(prelude, s, SymbolKind::builtin_function);
      for (const auto &spec : hir::all_system_operations)
        add(prelude, std::string(spec.name), SymbolKind::builtin_function);
      collect();
      for (const auto &m : out.project->modules) {
        module = m.id;
        for (file = 0; file < m.files.size(); ++file) {
          ast = m.files[file].get();
          Scope scope{&packages.at(module.value), {}};
          for (const auto &i : m.imports)
            if (i.file == file && i.alias != "_")
              add(scope, i.alias, SymbolKind::module, ref(i.node), i.span, false, i.target);
          for (auto id : node(ast->root).children) declaration(id, scope);
        }
      }
      constants();
      result.state = diagnostics.empty() ? PhaseState::complete : PhaseState::recovered;
      result.value = std::move(out);
      result.diagnostics = std::move(diagnostics);
    } catch (const Cancelled &) { result.state = PhaseState::cancelled; }
    return result;
  }
};
} // namespace
PhaseResult<ResolvedProject> resolve_project(std::shared_ptr<const Project> project,
                                            std::stop_token stop) {
  return Resolver(std::move(project), stop).run();
}
} // namespace lang::modules
